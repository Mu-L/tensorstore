// Copyright 2020 The TensorStore Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "tensorstore/internal/cache/chunk_cache.h"

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "absl/container/inlined_vector.h"
#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "absl/time/time.h"
#include "riegeli/bytes/cord_reader.h"
#include "riegeli/bytes/cord_writer.h"
#include "tensorstore/array.h"
#include "tensorstore/box.h"
#include "tensorstore/context.h"
#include "tensorstore/contiguous_layout.h"
#include "tensorstore/data_type.h"
#include "tensorstore/driver/chunk.h"
#include "tensorstore/driver/chunk_cache_driver.h"
#include "tensorstore/driver/driver.h"
#include "tensorstore/driver/driver_handle.h"
#include "tensorstore/driver/driver_testutil.h"
#include "tensorstore/index.h"
#include "tensorstore/index_space/dim_expression.h"
#include "tensorstore/index_space/index_transform.h"
#include "tensorstore/index_space/index_transform_builder.h"
#include "tensorstore/index_space/transformed_array.h"
#include "tensorstore/internal/async_write_array.h"
#include "tensorstore/internal/cache/async_cache.h"
#include "tensorstore/internal/cache/cache.h"
#include "tensorstore/internal/cache/kvs_backed_cache.h"
#include "tensorstore/internal/chunk_grid_specification.h"
#include "tensorstore/internal/element_copy_function.h"
#include "tensorstore/internal/elementwise_function.h"
#include "tensorstore/internal/global_initializer.h"
#include "tensorstore/internal/intrusive_ptr.h"
#include "tensorstore/internal/memory.h"
#include "tensorstore/internal/riegeli/array_endian_codec.h"
#include "tensorstore/internal/testing/queue_testutil.h"
#include "tensorstore/internal/thread/thread_pool.h"
#include "tensorstore/kvstore/generation.h"
#include "tensorstore/kvstore/memory/memory_key_value_store.h"
#include "tensorstore/kvstore/mock_kvstore.h"
#include "tensorstore/kvstore/spec.h"
#include "tensorstore/open_mode.h"
#include "tensorstore/progress.h"
#include "tensorstore/read_write_options.h"
#include "tensorstore/staleness_bound.h"
#include "tensorstore/strided_layout.h"
#include "tensorstore/tensorstore.h"
#include "tensorstore/transaction.h"
#include "tensorstore/util/constant_vector.h"
#include "tensorstore/util/endian.h"
#include "tensorstore/util/execution/any_receiver.h"
#include "tensorstore/util/execution/execution.h"
#include "tensorstore/util/executor.h"
#include "tensorstore/util/future.h"
#include "tensorstore/util/garbage_collection/garbage_collection.h"
#include "tensorstore/util/result.h"
#include "tensorstore/util/span.h"
#include "tensorstore/util/status.h"
#include "tensorstore/util/status_testutil.h"

namespace {

namespace kvstore = tensorstore::kvstore;
using ::tensorstore::ArrayView;
using ::tensorstore::Box;
using ::tensorstore::BoxView;
using ::tensorstore::DimensionIndex;
using ::tensorstore::Executor;
using ::tensorstore::Future;
using ::tensorstore::Index;
using ::tensorstore::IndexTransform;
using ::tensorstore::MakeArray;
using ::tensorstore::MakeCopy;
using ::tensorstore::MatchesStatus;
using ::tensorstore::no_transaction;
using ::tensorstore::ReadProgressFunction;
using ::tensorstore::Result;
using ::tensorstore::SharedArray;
using ::tensorstore::span;
using ::tensorstore::StalenessBound;
using ::tensorstore::StorageGeneration;
using ::tensorstore::TensorStore;
using ::tensorstore::Transaction;
using ::tensorstore::WriteProgressFunction;
using ::tensorstore::internal::AsyncCache;
using ::tensorstore::internal::AsyncWriteArray;
using ::tensorstore::internal::CachePool;
using ::tensorstore::internal::CachePtr;
using ::tensorstore::internal::ChunkCache;
using ::tensorstore::internal::ChunkGridSpecification;
using ::tensorstore::internal::ConcreteChunkCache;
using ::tensorstore::internal::ElementCopyFunction;
using ::tensorstore::internal::GetCache;
using ::tensorstore::internal::GetEntryForGridCell;
using ::tensorstore::internal::MakeReadWritePtr;
using ::tensorstore::internal::MockKeyValueStore;
using ::tensorstore::internal::PinnedCacheEntry;
using ::tensorstore::internal::ReadWritePtr;
using ::tensorstore::internal::SimpleElementwiseFunction;
using ::testing::ElementsAre;

/// Decodes component arrays encoded as native-endian C order.
Result<std::shared_ptr<const ChunkCache::ReadData>> DecodeRaw(
    const ChunkGridSpecification& grid, const absl::Cord* value) {
  const auto& component_specs = grid.components;
  std::shared_ptr<ChunkCache::ReadData> read_data;
  if (value) {
    read_data = tensorstore::internal::make_shared_for_overwrite<
        ChunkCache::ReadData[]>(component_specs.size());
    riegeli::CordReader<const absl::Cord*> reader{value};
    for (size_t component_i = 0; component_i < component_specs.size();
         ++component_i) {
      const auto& spec = component_specs[component_i];
      TENSORSTORE_ASSIGN_OR_RETURN(
          read_data.get()[component_i],
          tensorstore::internal::DecodeArrayEndian(
              reader, spec.dtype(), spec.shape(), tensorstore::endian::native,
              tensorstore::c_order));
    }
    if (!reader.VerifyEndAndClose()) return reader.status();
  }
  return std::static_pointer_cast<ChunkCache::ReadData>(std::move(read_data));
}

/// Encodes component arrays as native-endian C order.
template <typename ComponentArrays = std::vector<SharedArray<const void>>>
absl::Cord EncodeRaw(const ChunkGridSpecification& grid,
                     const ComponentArrays& component_arrays) {
  absl::Cord value;
  riegeli::CordWriter<absl::Cord*> writer{&value};
  const auto& component_specs = grid.components;
  for (size_t component_i = 0; component_i < component_specs.size();
       ++component_i) {
    const auto& spec = component_specs[component_i];
    auto& array = component_arrays[component_i];
    ABSL_CHECK(tensorstore::internal::RangesEqual(array.shape(), spec.shape()));
    ABSL_CHECK(array.dtype() == spec.dtype());
    ABSL_CHECK(tensorstore::internal::EncodeArrayEndian(
        array, tensorstore::endian::native, tensorstore::c_order, writer));
  }
  ABSL_CHECK(writer.Close());
  return value;
}

std::string EncodeKey(span<const Index> indices) {
  return absl::StrJoin(indices, ",");
}

class TestCache
    : public tensorstore::internal::KvsBackedCache<TestCache,
                                                   ConcreteChunkCache> {
  using Base =
      tensorstore::internal::KvsBackedCache<TestCache, ConcreteChunkCache>;

 public:
  using Base::Base;

  class Entry : public Base::Entry {
   public:
    using OwningCache = TestCache;
    // DoDecode implementation required by `KvsBackedCache`.
    void DoDecode(std::optional<absl::Cord> value,
                  DecodeReceiver receiver) override {
      GetOwningCache(*this).executor()([this, value = std::move(value),
                                        receiver =
                                            std::move(receiver)]() mutable {
        TENSORSTORE_ASSIGN_OR_RETURN(
            auto read_data,
            DecodeRaw(GetOwningCache(*this).grid(), value ? &*value : nullptr),
            tensorstore::execution::set_error(receiver, _));
        tensorstore::execution::set_value(receiver, std::move(read_data));
      });
    }

    // DoEncode implementation required by `KvsBackedCache`.
    void DoEncode(EncodeOptions options, std::shared_ptr<const ReadData> data,
                  EncodeReceiver receiver) override {
      std::optional<absl::Cord> encoded;
      if (data) {
        encoded = EncodeRaw(GetOwningCache(*this).grid(), data.get());
      }
      tensorstore::execution::set_value(receiver, std::move(encoded));
    }

    std::string GetKeyValueStoreKey() override {
      return EncodeKey(this->cell_indices());
    }
  };

  Entry* DoAllocateEntry() final { return new Entry; }
  size_t DoGetSizeofEntry() final { return sizeof(Entry); }
  TransactionNode* DoAllocateTransactionNode(AsyncCache::Entry& entry) final {
    return new TransactionNode(static_cast<Entry&>(entry));
  }
};

class TestDriver : public tensorstore::internal::ChunkCacheDriver {
 public:
  using ::tensorstore::internal::ChunkCacheDriver::ChunkCacheDriver;
  void GarbageCollectionVisit(
      tensorstore::garbage_collection::GarbageCollectionVisitor& visitor)
      const final {
    // No-op
  }
};

template <typename T>
ElementCopyFunction GetCopyFunction() {
  [[maybe_unused]] const auto copy_func =
      [](const T* source, T* dest, absl::Status* status) { *dest = *source; };
  return SimpleElementwiseFunction<decltype(copy_func), const T, T>();
}

TEST(ChunkGridSpecificationTest, Basic) {
  ChunkGridSpecification grid({ChunkGridSpecification::Component{
      AsyncWriteArray::Spec{SharedArray<const void>(MakeArray<int>({1, 2})),
                            Box<>(1)},
      /*chunk_shape=*/{2}}});
  EXPECT_EQ(1, grid.components[0].rank());
  EXPECT_EQ(1, grid.components[0].chunked_to_cell_dimensions.size());
  EXPECT_EQ(1, grid.chunk_shape.size());

  absl::InlinedVector<Index, 1> origin;
  origin.resize(grid.components[0].rank());

  grid.GetComponentOrigin(0, span<const Index>({0}), origin);
  EXPECT_THAT(origin, testing::ElementsAre(0));
  grid.GetComponentOrigin(0, span<const Index>({1}), origin);
  EXPECT_THAT(origin, testing::ElementsAre(2));
}

TEST(ChunkGridSpecificationTest, MoreComplicated) {
  std::vector<Index> shape = {1, 2, 3, 4};

  // The fill value is an array of int{0} where the byte-strides
  // is 0, so it always references the same element.
  SharedArray<const void> fill_value(
      tensorstore::internal::AllocateAndConstructSharedElements(
          1, tensorstore::value_init, tensorstore::dtype_v<int>),
      tensorstore::StridedLayout<>(
          shape, tensorstore::GetConstantVector<Index, 0, 4>()));

  // The grid has a single component, with fill value as above.
  ChunkGridSpecification grid({ChunkGridSpecification::Component{
      AsyncWriteArray::Spec{fill_value, Box<>(shape)},
      /*chunk_shape=*/shape,
      {3, 2, 1}}});

  EXPECT_EQ(3, grid.chunk_shape.size());
  EXPECT_THAT(grid.chunk_shape, testing::ElementsAre(4, 3, 2));

  EXPECT_EQ(4, grid.components[0].array_spec.overall_fill_value.rank());
  EXPECT_EQ(4, grid.components[0].rank());
  EXPECT_EQ(3, grid.components[0].chunked_to_cell_dimensions.size());
  EXPECT_THAT(grid.components[0].chunked_to_cell_dimensions,
              testing::ElementsAre(3, 2, 1));

  absl::InlinedVector<Index, 4> origin;
  origin.resize(grid.components[0].rank());
  grid.GetComponentOrigin(0, span<const Index>({0, 0, 0}), origin);
  EXPECT_THAT(origin, testing::ElementsAre(0, 0, 0, 0));

  grid.GetComponentOrigin(0, span<const Index>({1, 1, 1}), origin);
  EXPECT_THAT(origin, testing::ElementsAre(0, 2, 3, 4));
  grid.GetComponentOrigin(0, span<const Index>({3, 2, 1}), origin);
  EXPECT_THAT(origin, testing::ElementsAre(0, 2, 6, 12));
}

std::vector<Index> ParseKey(std::string_view key) {
  std::vector<Index> result;
  for (auto s : absl::StrSplit(key, ',')) {
    Index i = 0;
    ABSL_CHECK(absl::SimpleAtoi(s, &i));
    result.push_back(i);
  }
  return result;
}

ReadWritePtr<TestDriver> MakeDriver(CachePtr<ChunkCache> cache,
                                    size_t component_index = 0,
                                    StalenessBound data_staleness = {}) {
  return MakeReadWritePtr<TestDriver>(
      tensorstore::ReadWriteMode::read_write,
      TestDriver::Initializer{std::move(cache), component_index,
                              data_staleness});
}

class ChunkCacheTest : public ::testing::Test {
 public:
  /// Thread pool used by the ChunkCacheDriver to perform read/write operations.
  Executor thread_pool = tensorstore::internal::DetachedThreadPool(1);

  std::optional<ChunkGridSpecification> grid;

  kvstore::DriverPtr memory_store = tensorstore::GetMemoryKeyValueStore();
  MockKeyValueStore::MockPtr mock_store = MockKeyValueStore::Make();

  std::vector<ChunkCache::ReadData> GetChunk(
      const std::vector<Index>& indices) {
    auto read_result = memory_store->Read(EncodeKey(indices)).value();
    const size_t num_components = grid->components.size();
    std::vector<ChunkCache::ReadData> components(num_components);
    if (auto read_data =
            DecodeRaw(*grid,
                      read_result.has_value() ? &read_result.value : nullptr)
                .value()) {
      for (size_t i = 0; i < num_components; ++i) {
        components[i] = read_data.get()[i];
      }
    }
    return components;
  }

  bool HasChunk(const std::vector<Index>& indices) {
    auto read_result = memory_store->Read(EncodeKey(indices)).value();
    return read_result.has_value();
  }

  void SetChunk(
      const std::vector<Index>& indices,
      std::vector<tensorstore::SharedArrayView<const void>> components) {
    TENSORSTORE_CHECK_OK(
        memory_store->Write(EncodeKey(indices), EncodeRaw(*grid, components)));
  }

  CachePtr<ChunkCache> MakeChunkCache(
      std::string_view cache_identifier = {},
      std::optional<CachePool::StrongPtr> pool = {}) {
    if (!pool) {
      pool = CachePool::Make(CachePool::Limits{10000000});
    }
    return GetCache<TestCache>(pool->get(), cache_identifier, [&] {
      return std::make_unique<TestCache>(mock_store, *grid, thread_pool);
    });
  }

  TensorStore<> GetTensorStore(CachePtr<ChunkCache> cache = {},
                               StalenessBound data_staleness = {},
                               size_t component_index = 0,
                               Transaction transaction = no_transaction) {
    if (!cache) cache = MakeChunkCache();
    return tensorstore::internal::TensorStoreAccess::Construct<TensorStore<>>(
        tensorstore::internal::Driver::Handle{
            MakeDriver(cache, component_index, data_staleness),
            tensorstore::IdentityTransform(
                grid->components[component_index].rank()),
            transaction});
  }
};

template <typename T>
tensorstore::SharedOffsetArray<T> MakeSequentialArray(BoxView<> domain) {
  auto array = tensorstore::AllocateArray<T>(domain);
  T value = T{};
  IterateOverArrays(
      [&](T* ptr) {
        *ptr = value;
        ++value;
      },
      tensorstore::c_order, array);
  return array;
}

ChunkGridSpecification GetSimple1DGrid() {
  // Dimension 0 is chunked with a size of 2.
  return ChunkGridSpecification({ChunkGridSpecification::Component{
      AsyncWriteArray::Spec{MakeSequentialArray<int>(BoxView<>{{0}, {10}}),
                            Box<>(1)},
      /*chunk_shape=*/{2}}});
}

// Tests reading of chunks not present in the data store.
TEST_F(ChunkCacheTest, ReadSingleComponentOneDimensionalFill) {
  // Dimension 0 is chunked with a size of 2.
  grid = GetSimple1DGrid();

  auto cache = MakeChunkCache();

  // Test that chunks that aren't present in store get filled using the fill
  // value.
  {
    auto read_future =
        tensorstore::Read(GetTensorStore(cache, absl::InfinitePast()) |
                          tensorstore::Dims(0).TranslateSizedInterval(3, 3));
    {
      auto r = mock_store->read_requests.pop();
      EXPECT_THAT(ParseKey(r.key), ElementsAre(1));
      EXPECT_EQ(StorageGeneration::Unknown(),
                r.options.generation_conditions.if_not_equal);
      r(memory_store);
    }
    {
      auto r = mock_store->read_requests.pop();
      EXPECT_THAT(ParseKey(r.key), ElementsAre(2));
      EXPECT_EQ(StorageGeneration::Unknown(),
                r.options.generation_conditions.if_not_equal);
      r(memory_store);
    }
    // Index:         0 1 2 3 4 5 ...
    // Fill value:    0 1 2 3 4 5 ...
    // Read region:        [     ]
    EXPECT_THAT(read_future.result(),
                ::testing::Optional(tensorstore::MakeArray({3, 4, 5})));
  }

  // Test reading cached chunks.  The staleness bound of `absl::InfinitePast()`
  // is always satisfied by an existing read response.
  {
    auto read_future =
        tensorstore::Read(GetTensorStore(cache, absl::InfinitePast()) |
                          tensorstore::Dims(0).TranslateSizedInterval(3, 3));
    // Verify that same response as before is received (fill value).
    EXPECT_THAT(read_future.result(),
                ::testing::Optional(tensorstore::MakeArray({3, 4, 5})));
  }

  // Test re-reading chunks.  The staleness bound of `absl::InfiniteFuture()` is
  // never satisfied by an existing read response.
  {
    auto read_future =
        tensorstore::Read(GetTensorStore(cache, absl::InfiniteFuture()) |
                          tensorstore::Dims(0).TranslateSizedInterval(3, 3));
    {
      auto r = mock_store->read_requests.pop();
      EXPECT_THAT(ParseKey(r.key), ElementsAre(1));
      EXPECT_EQ(StorageGeneration::NoValue(),
                r.options.generation_conditions.if_not_equal);
      r(memory_store);
    }
    {
      auto r = mock_store->read_requests.pop();
      EXPECT_THAT(ParseKey(r.key), ElementsAre(2));
      EXPECT_EQ(StorageGeneration::NoValue(),
                r.options.generation_conditions.if_not_equal);
      r(memory_store);
    }
    // Verify that result matches fill value after the new read response.
    EXPECT_THAT(read_future.result(),
                ::testing::Optional(tensorstore::MakeArray({3, 4, 5})));
  }
}

// Tests cancelling a read request.
TEST_F(ChunkCacheTest, CancelRead) {
  // Dimension 0 is chunked with a size of 2.
  grid = GetSimple1DGrid();

  auto cache = MakeChunkCache();

  mock_store->forward_to = memory_store;

  {
    auto read_future =
        tensorstore::Read(GetTensorStore(cache, absl::InfinitePast()) |
                          tensorstore::Dims(0).TranslateSizedInterval(3, 3));
    // Read request is cancelled when `read_future` is destroyed.  At the
    // moment, however, there is no mechanism for propagating cancellation to
    // the implementation of `DoRead` in order to cancel already in-progress
    // reads; however, pending read requests are cancelled.
  }
}

// Special-purpose FlowReceiver used by the `CancelWrite` test defined below,
// passed to `ChunkCache::Write`.
struct CancelWriteReceiver {
  friend void set_starting(CancelWriteReceiver& receiver,
                           tensorstore::AnyCancelReceiver cancel) {
    receiver.cancel = std::move(cancel);
  }
  friend void set_value(CancelWriteReceiver& receiver,
                        tensorstore::internal::WriteChunk chunk,
                        tensorstore::IndexTransform<> cell_transform) {
    EXPECT_FALSE(receiver.set_value_called);
    receiver.set_value_called = true;
    EXPECT_EQ(tensorstore::IndexTransformBuilder<>(1, 1)
                  .input_origin({0})
                  .input_shape({1})
                  .output_single_input_dimension(0, 3, 1, 0)
                  .Finalize()
                  .value(),
              chunk.transform);
    EXPECT_EQ(tensorstore::IndexTransformBuilder<>(1, 1)
                  .input_origin({0})
                  .input_shape({1})
                  .output_single_input_dimension(0, 0)
                  .Finalize()
                  .value(),
              cell_transform);
    receiver.cancel();
  }
  friend void set_done(CancelWriteReceiver& receiver) {}
  friend void set_error(CancelWriteReceiver& receiver, absl::Status status) {}
  friend void set_stopping(CancelWriteReceiver& receiver) {
    receiver.cancel = nullptr;
  }

  bool set_value_called = false;
  tensorstore::AnyCancelReceiver cancel;
};

// Tests cancelling a write request.
TEST_F(ChunkCacheTest, CancelWrite) {
  // Dimension 0 is chunked with a size of 2.
  grid = GetSimple1DGrid();

  CancelWriteReceiver receiver;
  auto cache = MakeChunkCache();
  cache->Write(
      ChunkCache::WriteRequest{
          {/*transaction=*/{},
           (tensorstore::IdentityTransform(1) |
            tensorstore::Dims(0).TranslateSizedInterval(3, 3))
               .value()},
          /*.component_index=*/0},
      std::ref(receiver));
  EXPECT_TRUE(receiver.set_value_called);
}

// Tests that the implementation of `ChunkCacheDriver::dtype` returns the
// data type of the associated component array.
TEST_F(ChunkCacheTest, DriverDataType) {
  grid = ChunkGridSpecification({
      ChunkGridSpecification::Component{
          AsyncWriteArray::Spec{SharedArray<const void>(MakeArray<int>({1, 2})),
                                Box<>(1)},
          /*chunk_shape=*/{2}},
      ChunkGridSpecification::Component{
          AsyncWriteArray::Spec{
              SharedArray<const void>(MakeArray<float>({{1, 2}, {3, 4}})),
              Box<>(2)},
          /*chunk_shape=*/{2, 2},
          {1}},
  });

  auto cache = MakeChunkCache();

  EXPECT_EQ(tensorstore::dtype_v<int>, MakeDriver(cache, 0)->dtype());

  EXPECT_EQ(tensorstore::dtype_v<float>, MakeDriver(cache, 1)->dtype());
}

// Tests reading of existing data.
TEST_F(ChunkCacheTest, ReadSingleComponentOneDimensionalExisting) {
  // Dimension 0 is chunked with a size of 2.
  grid = GetSimple1DGrid();

  // Initialize chunk 1 in the `memory_store`.
  SetChunk({1}, {MakeArray<int>({42, 43})});

  auto cache = MakeChunkCache();

  // Test reading from an existing chunk (read request partially overlaps
  // existing data).
  {
    // Read chunk 1, position [1], and chunk 2, positions [0] and [1].
    auto read_future =
        tensorstore::Read(GetTensorStore(cache, absl::InfinitePast()) |
                          tensorstore::Dims(0).TranslateSizedInterval(3, 3));
    {
      auto r = mock_store->read_requests.pop();
      EXPECT_THAT(ParseKey(r.key), ElementsAre(1));
      r(memory_store);
    }
    {
      auto r = mock_store->read_requests.pop();
      EXPECT_THAT(ParseKey(r.key), ElementsAre(2));
      r(memory_store);
    }
    EXPECT_THAT(read_future.result(),
                ::testing::Optional(tensorstore::MakeArray({43, 4, 5})));
  }

  // Initialize chunk 2 in the data store.  The cache does not yet reflect this
  // chunk.
  SetChunk({2}, {MakeArray<int>({44, 45})});

  // Test reading cached chunks (verifies that the previously cached cells were
  // not evicted).
  {
    auto read_future =
        tensorstore::Read(GetTensorStore(cache, absl::InfinitePast()) |
                          tensorstore::Dims(0).TranslateSizedInterval(3, 3));
    // Read is satisfied without issuing any new read requests.
    // Read result corresponds to:
    // [0]: chunk 1, position [1]
    // [1]: chunk 2, position [0] (fill value)
    // [2]: chunk 2, position [1] (fill value)
    EXPECT_THAT(read_future.result(),
                ::testing::Optional(tensorstore::MakeArray({43, 4, 5})));
  }

  // Test re-reading chunks by using a staleness bound of
  // `absl::InfiniteFuture()`, which is never satisfied by an existing read
  // response.  This causes the cache to re-read chunk 2, which has been
  // updated.
  {
    auto read_future =
        tensorstore::Read(GetTensorStore(cache, absl::InfiniteFuture()) |
                          tensorstore::Dims(0).TranslateSizedInterval(3, 3));
    {
      auto r = mock_store->read_requests.pop();
      EXPECT_THAT(ParseKey(r.key), ElementsAre(1));
      r(memory_store);
    }
    {
      auto r = mock_store->read_requests.pop();
      EXPECT_THAT(ParseKey(r.key), ElementsAre(2));
      r(memory_store);
    }
    EXPECT_THAT(read_future.result(),
                ::testing::Optional(tensorstore::MakeArray({43, 44, 45})));
  }
}

// Test reading the fill value from a two-dimensional chunk cache.
TEST_F(ChunkCacheTest, TwoDimensional) {
  grid = ChunkGridSpecification({ChunkGridSpecification::Component{
      AsyncWriteArray::Spec{
          MakeSequentialArray<int>(BoxView<>({0, 0}, {10, 100})), Box<>(2)},
      /*chunk_shape=*/{2, 3},
      // Transpose the grid dimensions relative to the cell dimensions to test
      // that grid vs. cell indices are correctly handled.
      {1, 0}}});
  auto cache = MakeChunkCache();
  auto read_future = tensorstore::Read(
      GetTensorStore(cache, absl::InfinitePast()) |
      // Read box starting at {1, 5} of shape {6, 5}.
      tensorstore::Dims(0, 1).TranslateSizedInterval({1, 5}, {6, 5}));
  for (auto cell_indices : std::vector<std::vector<Index>>{{1, 0},
                                                           {1, 1},
                                                           {1, 2},
                                                           {1, 3},
                                                           {2, 0},
                                                           {2, 1},
                                                           {2, 2},
                                                           {2, 3},
                                                           {3, 0},
                                                           {3, 1},
                                                           {3, 2},
                                                           {3, 3}}) {
    auto r = mock_store->read_requests.pop();
    EXPECT_THAT(ParseKey(r.key), ::testing::ElementsAreArray(cell_indices));
    r(memory_store);
  }
  EXPECT_THAT(read_future.result(), ::testing::Optional(MakeArray<int>({
                                        {105, 106, 107, 108, 109},
                                        {205, 206, 207, 208, 209},
                                        {305, 306, 307, 308, 309},
                                        {405, 406, 407, 408, 409},
                                        {505, 506, 507, 508, 509},
                                        {605, 606, 607, 608, 609},
                                    })));
}

TEST_F(ChunkCacheTest, ReadRequestErrorBasic) {
  // Dimension 0 is chunked with a size of 2.
  grid = GetSimple1DGrid();

  auto cache = MakeChunkCache();

  // Test a read request failing (e.g. because the request to the underlying
  // storage system failed).
  {
    auto read_future =
        tensorstore::Read(GetTensorStore(cache, absl::InfinitePast()) |
                          tensorstore::Dims(0).TranslateSizedInterval(3, 3));
    {
      auto r = mock_store->read_requests.pop();
      EXPECT_THAT(ParseKey(r.key), ElementsAre(1));
      r(memory_store);
    }
    {
      auto r = mock_store->read_requests.pop();
      EXPECT_THAT(ParseKey(r.key), ElementsAre(2));
      r.promise.SetResult(absl::UnknownError("Test read error"));
    }
    EXPECT_THAT(read_future.result(),
                MatchesStatus(absl::StatusCode::kUnknown,
                              "Error reading .*: Test read error"));
  }

  // Test that the error is not cached.
  {
    auto read_future =
        tensorstore::Read(GetTensorStore(cache, absl::InfinitePast()) |
                          tensorstore::Dims(0).TranslateSizedInterval(3, 3));
    {
      auto r = mock_store->read_requests.pop();
      EXPECT_THAT(ParseKey(r.key), ElementsAre(2));
      r.promise.SetResult(absl::UnknownError("Test read error 2"));
    }
    EXPECT_THAT(read_future.result(),
                MatchesStatus(absl::StatusCode::kUnknown,
                              "Error reading .*: Test read error 2"));
  }

  // Test that the request is repeated if we require a later timestamp.
  {
    auto read_future =
        tensorstore::Read(GetTensorStore(cache, absl::InfiniteFuture()) |
                          tensorstore::Dims(0).TranslateSizedInterval(3, 3));
    {
      auto r = mock_store->read_requests.pop();
      EXPECT_THAT(ParseKey(r.key), ElementsAre(1));
      r(memory_store);
    }
    {
      auto r = mock_store->read_requests.pop();
      EXPECT_THAT(ParseKey(r.key), ElementsAre(2));
      r(memory_store);
    }
    EXPECT_THAT(read_future.result(),
                ::testing::Optional(tensorstore::MakeArray({3, 4, 5})));
  }
}

TEST_F(ChunkCacheTest, WriteSingleComponentOneDimensional) {
  // Dimension 0 is chunked with a size of 2.
  grid = GetSimple1DGrid();
  auto cache = MakeChunkCache();

  // Read chunk 3 into the cache.
  {
    auto read_future =
        tensorstore::Read(GetTensorStore(cache, absl::InfinitePast()) |
                          tensorstore::Dims(0).TranslateSizedInterval(6, 2));
    {
      auto r = mock_store->read_requests.pop();
      EXPECT_THAT(ParseKey(r.key), ElementsAre(3));
      r(memory_store);
    }
    EXPECT_THAT(read_future.result(),
                ::testing::Optional(tensorstore::MakeArray({6, 7})));
  }

  // Write chunk 1: [1]=13
  // Write chunk 2: [0]=14, [1]=15
  // Write chunk 3: [0]=16
  auto write_future =
      tensorstore::Write(MakeArray<int>({13, 14, 15, 16}),
                         GetTensorStore(cache) |
                             tensorstore::Dims(0).TranslateSizedInterval(3, 4));

  // Test that writeback issues a read request for chunk 1, and writeback
  // requests for chunks 2, 3, and 1.
  write_future.Force();
  {
    auto r = mock_store->read_requests.pop();
    EXPECT_THAT(ParseKey(r.key), ElementsAre(1));
    EXPECT_EQ(StorageGeneration::Unknown(),
              r.options.generation_conditions.if_equal);
    r(memory_store);
  }
  std::vector<std::pair<std::vector<Index>, StorageGeneration>> write_requests;
  for (size_t i = 0; i < 3; ++i) {
    auto r = mock_store->write_requests.pop();
    write_requests.emplace_back(ParseKey(r.key),
                                r.options.generation_conditions.if_equal);
    r(memory_store);
  }
  EXPECT_THAT(
      write_requests,
      ::testing::UnorderedElementsAre(
          ::testing::Pair(ElementsAre(2), StorageGeneration::Unknown()),
          ::testing::Pair(ElementsAre(3), StorageGeneration::NoValue()),
          ::testing::Pair(ElementsAre(1), StorageGeneration::NoValue())));
  EXPECT_THAT(GetChunk({1}), ElementsAre(MakeArray<int>({2, 13})));
  EXPECT_THAT(GetChunk({2}), ElementsAre(MakeArray<int>({14, 15})));
  EXPECT_THAT(GetChunk({3}), ElementsAre(MakeArray<int>({16, 7})));
  TENSORSTORE_EXPECT_OK(write_future);
}

TEST_F(ChunkCacheTest, WriteSingleComponentOneDimensionalCacheDisabled) {
  // Dimension 0 is chunked with a size of 2.
  grid = GetSimple1DGrid();
  auto cache = MakeChunkCache("", CachePool::StrongPtr{});

  // Test that reading chunk 3 returns fill value.
  {
    auto read_future =
        tensorstore::Read(GetTensorStore(cache, absl::InfinitePast()) |
                          tensorstore::Dims(0).TranslateSizedInterval(6, 2));
    {
      auto r = mock_store->read_requests.pop();
      EXPECT_THAT(ParseKey(r.key), ElementsAre(3));
      r(memory_store);
    }
    EXPECT_THAT(read_future.result(),
                ::testing::Optional(tensorstore::MakeArray({6, 7})));
  }

  // Write chunk 1: [1]=13
  // Write chunk 2: [0]=14, [1]=15
  // Write chunk 3: [0]=16
  auto write_future =
      tensorstore::Write(MakeArray<int>({13, 14, 15, 16}),
                         GetTensorStore(cache) |
                             tensorstore::Dims(0).TranslateSizedInterval(3, 4));

  write_future.Force();
  // Test that writeback issues a read request for chunks 1 and 3, and writeback
  // requests for chunks 2, 3, and 1.
  {
    std::vector<std::vector<Index>> read_requests;
    for (size_t i = 0; i < 2; ++i) {
      auto r = mock_store->read_requests.pop();
      read_requests.emplace_back(ParseKey(r.key));
      EXPECT_EQ(StorageGeneration::Unknown(),
                r.options.generation_conditions.if_equal);
      r(memory_store);
    }
    EXPECT_THAT(read_requests, ::testing::UnorderedElementsAre(ElementsAre(1),
                                                               ElementsAre(3)));
  }
  {
    std::vector<std::pair<std::vector<Index>, StorageGeneration>>
        write_requests;
    for (size_t i = 0; i < 3; ++i) {
      auto r = mock_store->write_requests.pop();
      write_requests.emplace_back(ParseKey(r.key),
                                  r.options.generation_conditions.if_equal);
      r(memory_store);
    }
    EXPECT_THAT(
        write_requests,
        ::testing::UnorderedElementsAre(
            ::testing::Pair(ElementsAre(2), StorageGeneration::Unknown()),
            ::testing::Pair(ElementsAre(3), StorageGeneration::NoValue()),
            ::testing::Pair(ElementsAre(1), StorageGeneration::NoValue())));
  }
  EXPECT_THAT(GetChunk({1}), ElementsAre(MakeArray<int>({2, 13})));
  EXPECT_THAT(GetChunk({2}), ElementsAre(MakeArray<int>({14, 15})));
  EXPECT_THAT(GetChunk({3}), ElementsAre(MakeArray<int>({16, 7})));
  TENSORSTORE_EXPECT_OK(write_future);
}

TEST_F(ChunkCacheTest,
       WriteSingleComponentOneDimensionalWithTransactionCacheDisabled) {
  // Dimension 0 is chunked with a size of 2.
  grid = GetSimple1DGrid();
  auto cache = MakeChunkCache("", CachePool::StrongPtr{});

  Transaction transaction(tensorstore::isolated);

  // Write chunk 1: [1]=13
  // Write chunk 2: [0]=14, [1]=15
  // Write chunk 3: [0]=16
  auto write_future = tensorstore::Write(
      MakeArray<int>({13, 14, 15, 16}),
      GetTensorStore(cache, /*data_staleness=*/{}, /*component_index=*/0,
                     /*transaction=*/transaction) |
          tensorstore::Dims(0).TranslateSizedInterval(3, 4));
  TENSORSTORE_EXPECT_OK(write_future.result());

  // Tests that the prior write is visible to a transaction read.
  {
    auto read_future = tensorstore::Read(
        GetTensorStore(cache, /*data_staleness=*/absl::InfinitePast(),
                       /*component_index=*/0, /*transaction=*/transaction) |
        tensorstore::Dims(0).TranslateSizedInterval(6, 2));
    {
      auto r = mock_store->read_requests.pop();
      EXPECT_THAT(ParseKey(r.key), ElementsAre(3));
      r(memory_store);
    }
    EXPECT_THAT(read_future.result(),
                ::testing::Optional(tensorstore::MakeArray({16, 7})));
  }

  auto commit_future = transaction.CommitAsync();

  // Test that writeback issues a read request for chunks 1 and 3, and writeback
  // requests for chunks 2, 3, and 1.
  {
    std::vector<std::vector<Index>> read_requests;
    for (size_t i = 0; i < 1; ++i) {
      auto r = mock_store->read_requests.pop();
      read_requests.emplace_back(ParseKey(r.key));
      EXPECT_EQ(StorageGeneration::Unknown(),
                r.options.generation_conditions.if_equal);
      r(memory_store);
    }
    EXPECT_THAT(read_requests, ::testing::UnorderedElementsAre(ElementsAre(1)));
  }
  {
    std::vector<std::pair<std::vector<Index>, StorageGeneration>>
        write_requests;
    for (size_t i = 0; i < 3; ++i) {
      auto r = mock_store->write_requests.pop();
      write_requests.emplace_back(ParseKey(r.key),
                                  r.options.generation_conditions.if_equal);
      r(memory_store);
    }
    EXPECT_THAT(
        write_requests,
        ::testing::UnorderedElementsAre(
            ::testing::Pair(ElementsAre(2), StorageGeneration::Unknown()),
            ::testing::Pair(ElementsAre(3), StorageGeneration::NoValue()),
            ::testing::Pair(ElementsAre(1), StorageGeneration::NoValue())));
  }
  EXPECT_THAT(GetChunk({1}), ElementsAre(MakeArray<int>({2, 13})));
  EXPECT_THAT(GetChunk({2}), ElementsAre(MakeArray<int>({14, 15})));
  EXPECT_THAT(GetChunk({3}), ElementsAre(MakeArray<int>({16, 7})));
  TENSORSTORE_EXPECT_OK(commit_future);
}

TEST_F(ChunkCacheTest, WriteAfterReadWithTransactionCacheDisabled) {
  // Dimension 0 is chunked with a size of 2.
  grid = GetSimple1DGrid();
  auto cache = MakeChunkCache("", CachePool::StrongPtr{});

  Transaction transaction(tensorstore::isolated);

  // Read fill value.
  {
    auto read_future = tensorstore::Read(
        GetTensorStore(cache, /*data_staleness=*/absl::InfinitePast(),
                       /*component_index=*/0, /*transaction=*/transaction) |
        tensorstore::Dims(0).TranslateSizedInterval(0, 2));
    {
      auto r = mock_store->read_requests.pop();
      EXPECT_THAT(ParseKey(r.key), ElementsAre(0));
      r(memory_store);
    }
    EXPECT_THAT(read_future.result(),
                ::testing::Optional(tensorstore::MakeArray({0, 1})));
  }

  auto write_future = tensorstore::Write(
      MakeArray<int>({13}),
      GetTensorStore(cache, /*data_staleness=*/{}, /*component_index=*/0,
                     /*transaction=*/transaction) |
          tensorstore::Dims(0).TranslateSizedInterval(0, 1));
  TENSORSTORE_EXPECT_OK(write_future.result());

  auto commit_future = transaction.CommitAsync();

  // Test that writeback issues a writeback request for chunk 0.
  {
    std::vector<std::pair<std::vector<Index>, StorageGeneration>>
        write_requests;
    for (size_t i = 0; i < 1; ++i) {
      auto r = mock_store->write_requests.pop();
      write_requests.emplace_back(ParseKey(r.key),
                                  r.options.generation_conditions.if_equal);
      r(memory_store);
    }
    EXPECT_THAT(write_requests,
                ::testing::UnorderedElementsAre(::testing::Pair(
                    ElementsAre(0), StorageGeneration::NoValue())));
  }
  EXPECT_THAT(GetChunk({0}), ElementsAre(MakeArray<int>({13, 1})));
  TENSORSTORE_EXPECT_OK(commit_future);
}

// Tests that overwriting a non-present chunk with the fill value results in the
// chunk remaining deleted.
TEST_F(ChunkCacheTest, OverwriteMissingWithFillValue) {
  // Dimension 0 is chunked with a size of 2.
  grid = GetSimple1DGrid();
  auto cache = MakeChunkCache();
  auto cell_entry = GetEntryForGridCell(*cache, span<const Index>({1}));
  // Overwrite chunk 1: [0]=2, [1]=3 (matches fill value)
  auto write_future =
      tensorstore::Write(MakeArray<int>({2, 3}),
                         GetTensorStore(cache) |
                             tensorstore::Dims(0).TranslateSizedInterval(2, 2));
  write_future.Force();
  {
    auto r = mock_store->write_requests.pop();
    EXPECT_THAT(ParseKey(r.key), ElementsAre(1));
    EXPECT_EQ(StorageGeneration::Unknown(),
              r.options.generation_conditions.if_equal);
    r(memory_store);
  }
  EXPECT_FALSE(HasChunk({1}));
  TENSORSTORE_EXPECT_OK(write_future);
}

// Tests that overwriting an existing chunk with the fill value results in the
// chunk being deleted.
TEST_F(ChunkCacheTest, OverwriteExistingWithFillValue) {
  // Dimension 0 is chunked with a size of 2.
  grid = GetSimple1DGrid();
  auto cache = MakeChunkCache();
  auto cell_entry = GetEntryForGridCell(*cache, span<const Index>({1}));
  // Write initial value to chunk 1: [0]=3, [1]=4
  {
    auto write_future = tensorstore::Write(
        MakeArray<int>({3, 4}),
        GetTensorStore(cache) |
            tensorstore::Dims(0).TranslateSizedInterval(2, 2));
    write_future.Force();
    {
      auto r = mock_store->write_requests.pop();
      EXPECT_THAT(ParseKey(r.key), ElementsAre(1));
      EXPECT_EQ(StorageGeneration::Unknown(),
                r.options.generation_conditions.if_equal);
      r(memory_store);
    }
    TENSORSTORE_EXPECT_OK(write_future);
    EXPECT_TRUE(HasChunk({1}));
  }

  // Overwrite chunk 1 with fill value: [0]=2, [1]=3
  {
    auto write_future = tensorstore::Write(
        MakeArray<int>({2, 3}),
        GetTensorStore(cache) |
            tensorstore::Dims(0).TranslateSizedInterval(2, 2));
    write_future.Force();
    {
      auto r = mock_store->write_requests.pop();
      EXPECT_THAT(ParseKey(r.key), ElementsAre(1));
      EXPECT_EQ(StorageGeneration::Unknown(),
                r.options.generation_conditions.if_equal);
      r(memory_store);
    }
    EXPECT_FALSE(HasChunk({1}));
    TENSORSTORE_EXPECT_OK(write_future);
  }
}

// Tests that fill value comparison is based on "identical" equality.
TEST_F(ChunkCacheTest, FillValueIdenticallyEqual) {
  // Dimension 0 is chunked with a size of 2.
  grid = ChunkGridSpecification({ChunkGridSpecification::Component{
      AsyncWriteArray::Spec{
          SharedArray<const void>(MakeArray<float>({NAN, -0.0, NAN, -0.0})),
          Box<>(1)},
      /*chunk_shape=*/{2}}});
  auto cache = MakeChunkCache();
  auto cell_entry = GetEntryForGridCell(*cache, span<const Index>({1}));
  // Write initial value to chunk 1: [0]=NAN, [1]=+0.0
  {
    auto write_future = tensorstore::Write(
        MakeArray<float>({NAN, +0.0}),
        GetTensorStore(cache) |
            tensorstore::Dims(0).TranslateSizedInterval(2, 2));
    write_future.Force();
    {
      auto r = mock_store->write_requests.pop();
      EXPECT_THAT(ParseKey(r.key), ElementsAre(1));
      EXPECT_EQ(StorageGeneration::Unknown(),
                r.options.generation_conditions.if_equal);
      r(memory_store);
    }
    TENSORSTORE_EXPECT_OK(write_future);
    EXPECT_TRUE(HasChunk({1}));
  }

  // Overwrite chunk 1 with fill value: [0]=NAN, [1]=-0.0
  {
    auto write_future = tensorstore::Write(
        MakeArray<float>({NAN, -0.0}),
        GetTensorStore(cache) |
            tensorstore::Dims(0).TranslateSizedInterval(2, 2));
    write_future.Force();
    {
      auto r = mock_store->write_requests.pop();
      EXPECT_THAT(ParseKey(r.key), ElementsAre(1));
      EXPECT_EQ(StorageGeneration::Unknown(),
                r.options.generation_conditions.if_equal);
      r(memory_store);
    }
    EXPECT_FALSE(HasChunk({1}));
    TENSORSTORE_EXPECT_OK(write_future);
  }
}

// Tests that deleting a chunk that was previously written results in the chunk
// being deleted.
TEST_F(ChunkCacheTest, DeleteAfterNormalWriteback) {
  // Dimension 0 is chunked with a size of 2.
  grid = GetSimple1DGrid();
  auto cache = MakeChunkCache();
  auto cell_entry = GetEntryForGridCell(*cache, span<const Index>({1}));
  // Write initial value to chunk 1: [0]=3, [1]=4
  {
    auto write_future = tensorstore::Write(
        MakeArray<int>({3, 4}),
        GetTensorStore(cache) |
            tensorstore::Dims(0).TranslateSizedInterval(2, 2));
    write_future.Force();
    {
      auto r = mock_store->write_requests.pop();
      EXPECT_THAT(ParseKey(r.key), ElementsAre(1));
      EXPECT_EQ(StorageGeneration::Unknown(),
                r.options.generation_conditions.if_equal);
      r(memory_store);
    }
    TENSORSTORE_EXPECT_OK(write_future);
    EXPECT_TRUE(HasChunk({1}));
  }

  // Perform delete.
  auto write_future = cell_entry->Delete({});
  write_future.Force();
  {
    auto r = mock_store->write_requests.pop();
    EXPECT_THAT(ParseKey(r.key), ElementsAre(1));
    EXPECT_EQ(StorageGeneration::Unknown(),
              r.options.generation_conditions.if_equal);
    r(memory_store);
  }
  EXPECT_FALSE(HasChunk({1}));
  TENSORSTORE_EXPECT_OK(write_future);
}

// Tests that a partial write works correctly after a delete that has been
// written back.
TEST_F(ChunkCacheTest, PartialWriteAfterWrittenBackDelete) {
  // Dimension 0 is chunked with a size of 2.
  grid = GetSimple1DGrid();
  auto cache = MakeChunkCache();
  auto cell_entry = GetEntryForGridCell(*cache, span<const Index>({1}));
  // Cell initially has unknown data because no reads have been performed.
  EXPECT_EQ(
      nullptr,
      ChunkCache::GetReadComponent(
          AsyncCache::ReadLock<ChunkCache::ReadData>(*cell_entry).data(), 0)
          .data());
  auto write_future = cell_entry->Delete({});
  write_future.Force();
  {
    auto r = mock_store->write_requests.pop();
    EXPECT_THAT(ParseKey(r.key), ElementsAre(1));
    EXPECT_EQ(StorageGeneration::Unknown(),
              r.options.generation_conditions.if_equal);
    r(memory_store);
  }
  EXPECT_FALSE(HasChunk({1}));
  TENSORSTORE_EXPECT_OK(write_future);

  // Issue partial write: chunk 1, position [0]=42
  {
    auto write_future = tensorstore::Write(
        MakeArray<int>({42}),
        GetTensorStore(cache) |
            tensorstore::Dims(0).TranslateSizedInterval(2, 1));
    write_future.Force();
    {
      auto r = mock_store->write_requests.pop();
      EXPECT_THAT(ParseKey(r.key), ElementsAre(1));
      EXPECT_EQ(StorageGeneration::NoValue(),
                r.options.generation_conditions.if_equal);
      // WritebackSnapshot fills in the unmasked data values with the fill
      // value.
      r(memory_store);
    }
    TENSORSTORE_EXPECT_OK(write_future);
    EXPECT_THAT(GetChunk({1}), ElementsAre(MakeArray({42, 3})));
  }

  // Read back value from cache.
  {
    auto read_future =
        tensorstore::Read(GetTensorStore(cache, absl::InfinitePast()) |
                          tensorstore::Dims(0).TranslateSizedInterval(2, 2));
    // Read result corresponds to:
    // [0]: chunk 1, position [0]
    // [1]: chunk 1, position [1] (fill value)
    EXPECT_THAT(read_future.result(),
                ::testing::Optional(MakeArray<int>({42, 3})));
  }
}

// Tests calling Delete on a chunk while a read is pending.
TEST_F(ChunkCacheTest, DeleteWithPendingRead) {
  // Dimension 0 is chunked with a size of 2.
  grid = GetSimple1DGrid();
  auto cache = MakeChunkCache();
  auto cell_entry = GetEntryForGridCell(*cache, span<const Index>({1}));
  // Read chunk 1, position [0] and [1]
  auto read_future =
      tensorstore::Read(GetTensorStore(cache, absl::InfinitePast()) |
                        tensorstore::Dims(0).TranslateSizedInterval(2, 2));
  // Wait for the read request to be received.
  auto r = mock_store->read_requests.pop();

  // Perform delete.  This fully overwrites chunk 1.
  auto write_future = cell_entry->Delete({});

  // Complete the read request.
  EXPECT_THAT(ParseKey(r.key), ElementsAre(1));
  EXPECT_EQ(StorageGeneration::Unknown(),
            r.options.generation_conditions.if_not_equal);
  r(memory_store);

  // Verify that read result matches fill value.
  EXPECT_THAT(read_future.result(),
              ::testing::Optional(MakeArray<int>({2, 3})));

  {
    auto r = mock_store->write_requests.pop();
    EXPECT_THAT(ParseKey(r.key), ElementsAre(1));
    r(memory_store);
  }
  TENSORSTORE_EXPECT_OK(write_future);
}

TEST_F(ChunkCacheTest, WriteToMaskedArrayError) {
  // Dimension 0 is chunked with a size of 2.
  // Dimension 1 has a size of 2 and is not chunked.
  // Fill value is: {{1,2}, {3,4}}
  grid = ChunkGridSpecification({ChunkGridSpecification::Component{
      AsyncWriteArray::Spec{
          MakeSequentialArray<int>(BoxView<>{{0, 0}, {10, 10}}), Box<>(2)},
      /*chunk_shape=*/{2, 2},
      {0}}});

  auto cache = MakeChunkCache();
  auto cell_entry = GetEntryForGridCell(*cache, span<const Index>({1}));
  auto write_future = tensorstore::Write(
      MakeArray<int>({5, 6}),
      GetTensorStore(cache)
          // Specify out-of-bounds index of 2 for dimension 1.
          | tensorstore::Dims(1)
                .IndexArraySlice(MakeArray<Index>({2, 2}))
                .MoveToBack()
          // Select single index of dimension 0.
          | tensorstore::Dims(0).IndexSlice(2));
  EXPECT_THAT(write_future.result(),
              MatchesStatus(absl::StatusCode::kOutOfRange));

  // Verify read of same chunk after failed write returns fill value.
  auto read_future = tensorstore::Read(
      GetTensorStore(cache, absl::InfinitePast()) |
      tensorstore::Dims(0, 1).TranslateSizedInterval({2, 0}, {2, 2}));
  // Handle the read request.
  {
    auto r = mock_store->read_requests.pop();
    EXPECT_THAT(ParseKey(r.key), ElementsAre(1));
    EXPECT_EQ(StorageGeneration::Unknown(),
              r.options.generation_conditions.if_not_equal);
    r(memory_store);
  }
  EXPECT_THAT(read_future.result(),
              ::testing::Optional(MakeArray<int>({{20, 21}, {30, 31}})));
}

// Tests writeback where the store was modified after the read and before the
// writeback.
TEST_F(ChunkCacheTest, WriteGenerationMismatch) {
  // Dimension 0 is chunked with a size of 2.
  grid = GetSimple1DGrid();

  auto cache = MakeChunkCache();

  auto write_future =
      tensorstore::Write(MakeArray<int>({3}),
                         GetTensorStore(cache) |
                             tensorstore::Dims(0).TranslateSizedInterval(3, 1));

  write_future.Force();
  // Initialize chunk 1 in the store.
  SetChunk({1}, {MakeArray({5, 6})});

  // Handle the read request for chunk 1, which copies the generation 1 data
  // into the cache.
  {
    auto r = mock_store->read_requests.pop();
    EXPECT_THAT(ParseKey(r.key), ElementsAre(1));
    EXPECT_EQ(StorageGeneration::Unknown(),
              r.options.generation_conditions.if_not_equal);
    r(memory_store);
  }

  // Modify chunk 1 in the store.
  SetChunk({1}, {MakeArray({7, 8})});

  // Handle the writeback request for chunk 1, which fails due to a generation
  // mismatch.
  {
    auto r = mock_store->write_requests.pop();
    EXPECT_THAT(ParseKey(r.key), ElementsAre(1));
    EXPECT_NE(StorageGeneration::Unknown(),
              r.options.generation_conditions.if_equal);
    r(memory_store);
  }
  // Handle the re-issued read request for chunk 1.
  {
    auto r = mock_store->read_requests.pop();
    EXPECT_THAT(ParseKey(r.key), ElementsAre(1));
    r(memory_store);
  }
  // Handle the re-issued writeback request for chunk 1.
  {
    auto r = mock_store->write_requests.pop();
    EXPECT_THAT(ParseKey(r.key), ElementsAre(1));
    EXPECT_NE(StorageGeneration::Unknown(),
              r.options.generation_conditions.if_equal);
    r(memory_store);
  }
  TENSORSTORE_EXPECT_OK(write_future);
  EXPECT_THAT(GetChunk({1}), ElementsAre(MakeArray({7, 3})));
}

TEST_F(ChunkCacheTest, ModifyDuringWriteback) {
  // Dimension 0 is chunked with a size of 4.
  grid = ChunkGridSpecification({ChunkGridSpecification::Component{
      AsyncWriteArray::Spec{
          SharedArray<const void>(MakeArray<int>({1, 2, 3, 4})), Box<>(1)},
      /*chunk_shape=*/{4}}});
  auto cache = MakeChunkCache();
  // Partial write to chunk 0: [1]=5, [3]=6
  auto write_future = tensorstore::Write(
      MakeArray<int>({5, 6}),
      GetTensorStore(cache) |
          tensorstore::Dims(0).IndexArraySlice(MakeArray<Index>({1, 3})));

  write_future.Force();
  // Handle the read request for chunk 0 (the chunk was only partially
  // overwritten).
  {
    auto r = mock_store->read_requests.pop();
    EXPECT_THAT(ParseKey(r.key), ElementsAre(0));
    EXPECT_EQ(StorageGeneration::Unknown(),
              r.options.generation_conditions.if_not_equal);
    r(memory_store);
  }

  Future<const void> write_future2;
  // Handle the writeback request for chunk 1.
  {
    auto r = mock_store->write_requests.pop();
    EXPECT_THAT(ParseKey(r.key), ElementsAre(0));
    EXPECT_EQ(StorageGeneration::NoValue(),
              r.options.generation_conditions.if_equal);
    // While the writeback is in progress, write to chunk 0 again: [2]=7
    write_future2 =
        tensorstore::Write(
            MakeArray<int>({7}),
            GetTensorStore(cache) |
                tensorstore::Dims(0).IndexArraySlice(MakeArray<Index>({2})))
            .commit_future;
    r(memory_store);
  }

  TENSORSTORE_EXPECT_OK(write_future);

  // Verify that the writeback didn't include the second write.
  EXPECT_THAT(GetChunk({0}), ElementsAre(MakeArray({1, 5, 3, 6})));

  // Modify the chunk in the `memory_store`.
  SetChunk({0}, {MakeArray({10, 11, 12, 13})});

  write_future2.Force();
  // Handle the writeback request for chunk 0 (will fail due to generation
  // mismatch).
  {
    auto r = mock_store->write_requests.pop();
    EXPECT_THAT(ParseKey(r.key), ElementsAre(0));
    EXPECT_NE(StorageGeneration::Unknown(),
              r.options.generation_conditions.if_equal);
    r(memory_store);
  }
  // Handle the re-issued read request due to the generation mismatch.
  {
    auto r = mock_store->read_requests.pop();
    EXPECT_THAT(ParseKey(r.key), ElementsAre(0));
    r(memory_store);
  }
  // Handle the re-issued writeback request for chunk 0.
  {
    auto r = mock_store->write_requests.pop();
    EXPECT_THAT(ParseKey(r.key), ElementsAre(0));
    EXPECT_NE(StorageGeneration::Unknown(),
              r.options.generation_conditions.if_equal);
    r(memory_store);
  }
  TENSORSTORE_EXPECT_OK(write_future2);

  // Verify that the writeback only modified the single element touched by the
  // second write.
  EXPECT_THAT(GetChunk({0}), ElementsAre(MakeArray({10, 11, 7, 13})));
}

// Tests that fully overwriting a partial chunk (as determined by the
// `component_bounds`) results in an unconditional writeback.
TEST_F(ChunkCacheTest, FullyOverwritePartialChunk) {
  // Dimension 0 is chunked with a size of 4, but the component bounds are
  // `[1, 6)`, meaning chunk 0 has a valid range of `[1, 4)` and chunk 1 has a
  // valid range of `[4, 6)`.
  grid = ChunkGridSpecification({ChunkGridSpecification::Component{
      AsyncWriteArray::Spec{MakeSequentialArray<int>(BoxView<>{{0}, {10}}),
                            Box<>({1}, {5})},
      /*chunk_shape=*/{4}}});
  auto cache = MakeChunkCache();

  // Fully overwrite chunk 0
  {
    auto write_future = tensorstore::Write(
        MakeArray<int>({11, 12, 13}),
        GetTensorStore(cache) | tensorstore::Dims(0).HalfOpenInterval(1, 4));
    write_future.Force();
    {
      auto r = mock_store->write_requests.pop();
      EXPECT_THAT(ParseKey(r.key), ElementsAre(0));
      EXPECT_EQ(StorageGeneration::Unknown(),
                r.options.generation_conditions.if_equal);
      r(memory_store);
      EXPECT_THAT(GetChunk({0}), ElementsAre(MakeArray({0, 11, 12, 13})));
    }
    TENSORSTORE_ASSERT_OK(write_future);
  }

  // Fully overwrite chunk 1
  {
    auto write_future = tensorstore::Write(
        MakeArray<int>({14, 15}),
        GetTensorStore(cache) | tensorstore::Dims(0).HalfOpenInterval(4, 6));
    write_future.Force();
    {
      auto r = mock_store->write_requests.pop();
      EXPECT_THAT(ParseKey(r.key), ElementsAre(1));
      EXPECT_EQ(StorageGeneration::Unknown(),
                r.options.generation_conditions.if_equal);
      r(memory_store);
      EXPECT_THAT(GetChunk({1}), ElementsAre(MakeArray({14, 15, 6, 7})));
    }
    TENSORSTORE_ASSERT_OK(write_future);
  }
}

TEST_F(ChunkCacheTest, WritebackError) {
  // Dimension 0 is chunked with a size of 2.
  grid = GetSimple1DGrid();
  auto cache = MakeChunkCache();

  auto write_future =
      tensorstore::Write(
          MakeArray<int>({3, 4}),
          GetTensorStore(cache) | tensorstore::Dims(0).SizedInterval(0, 2))
          .commit_future;
  write_future.Force();

  {
    auto r = mock_store->write_requests.pop();
    EXPECT_THAT(ParseKey(r.key), ElementsAre(0));
    EXPECT_EQ(StorageGeneration::Unknown(),
              r.options.generation_conditions.if_equal);
    r.promise.SetResult(absl::UnknownError("Writeback error"));
  }

  EXPECT_THAT(write_future.result(),
              MatchesStatus(absl::StatusCode::kUnknown,
                            "Error writing .*: Writeback error"));
}

class ChunkCacheTransactionalTest : public ChunkCacheTest,
                                    public ::testing::WithParamInterface<bool> {
 protected:
  bool UseTransaction() const { return GetParam(); }
};

INSTANTIATE_TEST_SUITE_P(Instantiation, ChunkCacheTransactionalTest,
                         ::testing::Bool());

// Tests that copying from a chunk to a different chunk in the same
// `ChunkCache`, when the source chunk has not previously been modified, does
// not deadlock.
//
// There isn't really any reason why this would potentially deadlock, but it
// serves as a sanity check.
TEST_P(ChunkCacheTransactionalTest, SelfCopyDifferentChunksNoExistingData) {
  // Dimension 0 is chunked with a size of 2.
  grid = GetSimple1DGrid();
  auto cache = MakeChunkCache();
  Transaction transaction{no_transaction};
  if (UseTransaction()) {
    transaction = Transaction(tensorstore::isolated);
  }

  // Copies the fill value of `0` from position 0 to position 3.
  auto write_future =
      tensorstore::Copy(GetTensorStore(cache, {}, 0, transaction) |
                            tensorstore::Dims(0).SizedInterval(0, 1),
                        GetTensorStore(cache, {}, 0, transaction) |
                            tensorstore::Dims(0).SizedInterval(3, 1));
  {
    auto r = mock_store->read_requests.pop();
    EXPECT_THAT(ParseKey(r.key), ElementsAre(0));
    r(memory_store);
  }
  write_future.Force();
  if (UseTransaction()) {
    transaction.CommitAsync().IgnoreFuture();
  }
  {
    auto r = mock_store->read_requests.pop();
    EXPECT_THAT(ParseKey(r.key), ElementsAre(1));
    r(memory_store);
  }

  {
    auto r = mock_store->write_requests.pop();
    EXPECT_THAT(ParseKey(r.key), ElementsAre(1));
    r(memory_store);
  }
  TENSORSTORE_EXPECT_OK(write_future);
  if (UseTransaction()) {
    TENSORSTORE_EXPECT_OK(transaction.future());
  }

  EXPECT_THAT(GetChunk({1}), ElementsAre(MakeArray<int>({2, 0})));
}

// Tests that copying from a chunk to a different chunk in the same
// `ChunkCache`, when the source chunk *has* previously been modified, does not
// deadlock.
//
// There isn't really any reason why this would potentially deadlock, but it
// serves as a sanity check.
TEST_P(ChunkCacheTransactionalTest, SelfCopyDifferentChunksWithExistingData) {
  // Dimension 0 is chunked with a size of 2.
  grid = GetSimple1DGrid();

  auto cache = MakeChunkCache();

  Transaction transaction{no_transaction};
  if (UseTransaction()) {
    transaction = Transaction(tensorstore::isolated);
  }

  // Writes 42 to position 0 and 43 to position 1.
  auto write_future1 =
      tensorstore::Write(MakeArray<int>({42, 43}),
                         GetTensorStore(cache, {}, 0, transaction) |
                             tensorstore::Dims(0).TranslateSizedInterval(0, 2));

  if (!UseTransaction()) {
    auto r = mock_store->write_requests.pop();
    EXPECT_THAT(ParseKey(r.key), ElementsAre(0));
    r(memory_store);
    TENSORSTORE_EXPECT_OK(write_future1);
  } else {
    // Ensure write completes before Copy to ensure consistent behavior.
    TENSORSTORE_EXPECT_OK(write_future1.copy_future);
  }

  auto write_future2 =
      tensorstore::Copy(GetTensorStore(cache, {}, 0, transaction) |
                            tensorstore::Dims(0).SizedInterval(0, 1),
                        GetTensorStore(cache, {}, 0, transaction) |
                            tensorstore::Dims(0).SizedInterval(3, 1));
  if (!UseTransaction()) {
    // For a non-transactional write, the committed data is requested.
    auto r = mock_store->read_requests.pop();
    EXPECT_THAT(ParseKey(r.key), ElementsAre(0));
    r(memory_store);
  }

  // Ensure that the copying step has completed to ensure consistent behavior.
  TENSORSTORE_EXPECT_OK(write_future2.copy_future);
  write_future1.Force();
  write_future2.Force();
  if (UseTransaction()) {
    transaction.CommitAsync().IgnoreFuture();
  }
  {
    auto r = mock_store->read_requests.pop();
    EXPECT_THAT(ParseKey(r.key), ElementsAre(1));
    r(memory_store);
  }

  if (UseTransaction()) {
    // Both chunks will be written back.
    for (size_t i = 0; i < 2; ++i) {
      auto r = mock_store->write_requests.pop();
      r(memory_store);
    }
  } else {
    auto r = mock_store->write_requests.pop();
    EXPECT_THAT(ParseKey(r.key), ElementsAre(1));
    r(memory_store);
  }

  TENSORSTORE_EXPECT_OK(write_future1);
  TENSORSTORE_EXPECT_OK(write_future2);
  if (UseTransaction()) {
    TENSORSTORE_EXPECT_OK(transaction.future());
  }

  EXPECT_THAT(GetChunk({0}), ElementsAre(MakeArray<int>({42, 43})));
  EXPECT_THAT(GetChunk({1}), ElementsAre(MakeArray<int>({2, 42})));
}

// Tests that copying from a chunk to itself, when the chunk has not previously
// been modified, does not deadlock.
//
// There isn't really any reason why this would potentially deadlock, but it
// serves as a sanity check.
TEST_P(ChunkCacheTransactionalTest, SelfCopySameChunkNoExistingData) {
  // Dimension 0 is chunked with a size of 2.
  grid = GetSimple1DGrid();

  auto cache = MakeChunkCache();

  Transaction transaction{no_transaction};
  if (UseTransaction()) {
    transaction = Transaction(tensorstore::isolated);
  }

  // Copies the fill value of `0` from position 0 to position 1.
  auto write_future =
      tensorstore::Copy(GetTensorStore(cache, {}, 0, transaction) |
                            tensorstore::Dims(0).SizedInterval(0, 1),
                        GetTensorStore(cache, {}, 0, transaction) |
                            tensorstore::Dims(0).SizedInterval(1, 1));
  {
    auto r = mock_store->read_requests.pop();
    EXPECT_THAT(ParseKey(r.key), ElementsAre(0));
    r(memory_store);
  }
  write_future.Force();
  if (UseTransaction()) {
    transaction.CommitAsync().IgnoreFuture();
  }
  {
    auto r = mock_store->write_requests.pop();
    EXPECT_THAT(ParseKey(r.key), ElementsAre(0));
    r(memory_store);
  }
  TENSORSTORE_EXPECT_OK(write_future);
  if (UseTransaction()) {
    TENSORSTORE_EXPECT_OK(transaction.future());
  }

  EXPECT_THAT(GetChunk({0}), ElementsAre(MakeArray<int>({0, 0})));
}

// Tests that copying from a chunk to itself, when the chunk *has* previously
// been modified, does not deadlock.
//
// In the case of `UseTransaction() == true`, the copy operation involves
// reading and writing from the same chunk, and relies on `LockCollection` to
// avoid deadlock.
TEST_P(ChunkCacheTransactionalTest, SelfCopySameChunkWithExistingData) {
  // Dimension 0 is chunked with a size of 2.
  grid = GetSimple1DGrid();
  auto cache = MakeChunkCache();

  Transaction transaction{no_transaction};
  if (UseTransaction()) {
    transaction = Transaction(tensorstore::isolated);
  }

  auto write_future1 =
      tensorstore::Write(MakeArray<int>({42, 43}),
                         GetTensorStore(cache, {}, 0, transaction) |
                             tensorstore::Dims(0).TranslateSizedInterval(0, 2));
  // Ensure write completes before Copy to ensure consistent behavior.
  TENSORSTORE_EXPECT_OK(write_future1.copy_future);

  // In non-transactional case, the writes are not coalesced.
  if (!UseTransaction()) {
    write_future1.Force();
    auto r = mock_store->write_requests.pop();
    EXPECT_THAT(ParseKey(r.key), ElementsAre(0));
    r(memory_store);
    TENSORSTORE_EXPECT_OK(write_future1);
  }

  auto write_future2 =
      tensorstore::Copy(GetTensorStore(cache, {}, 0, transaction) |
                            tensorstore::Dims(0).SizedInterval(0, 1),
                        GetTensorStore(cache, {}, 0, transaction) |
                            tensorstore::Dims(0).SizedInterval(1, 1));
  if (!UseTransaction()) {
    // For a non-transactional write, the fully-overwritten source chunk is
    // ignored, and the committed data is requested.
    auto r = mock_store->read_requests.pop();
    EXPECT_THAT(ParseKey(r.key), ElementsAre(0));
    r(memory_store);
  }
  // Ensure that the copying step has completed to ensure consistent behavior.
  TENSORSTORE_EXPECT_OK(write_future2.copy_future);

  write_future1.Force();
  write_future2.Force();
  if (UseTransaction()) {
    transaction.CommitAsync().IgnoreFuture();
  }
  // Regardless of `UseTransaction()`, both writes are coalesced.
  {
    auto r = mock_store->write_requests.pop();
    EXPECT_THAT(ParseKey(r.key), ElementsAre(0));
    r(memory_store);
  }

  TENSORSTORE_EXPECT_OK(write_future1);
  TENSORSTORE_EXPECT_OK(write_future2);
  if (UseTransaction()) {
    TENSORSTORE_EXPECT_OK(transaction.future());
  }

  EXPECT_THAT(GetChunk({0}), ElementsAre(MakeArray<int>({42, 42})));
}

// As above, tests that copying from a chunk to itself, when the chunk *has*
// previously been modified, does not deadlock.  Additionally, in this case, the
// read and write chunks use separate caches (connected to the same
// KeyValueStore), in order to test the interaction of `AsyncCache::Revoke` with
// the chunk locking behavior.
//
// TODO(jbms): Add variant of this test that writes to a "transformed"
// TensorStore that locks multiple chunks for writing, once such a transform
// adapter exists.
TEST_F(ChunkCacheTest, SelfCopySameChunkSeparateCachesWithExistingData) {
  // Dimension 0 is chunked with a size of 2.
  grid = GetSimple1DGrid();
  auto cache1 = MakeChunkCache();
  auto cache2 = MakeChunkCache();

  Transaction transaction(tensorstore::isolated);

  auto write_future1 =
      tensorstore::Write(MakeArray<int>({42, 43}),
                         GetTensorStore(cache1, {}, 0, transaction) |
                             tensorstore::Dims(0).TranslateSizedInterval(0, 2));
  // Ensure write completes before Copy to ensure consistent behavior.
  TENSORSTORE_EXPECT_OK(write_future1.copy_future);
  auto write_future2 =
      tensorstore::Copy(GetTensorStore(cache1, {}, 0, transaction) |
                            tensorstore::Dims(0).SizedInterval(0, 1),
                        GetTensorStore(cache2, {}, 0, transaction) |
                            tensorstore::Dims(0).SizedInterval(1, 1));
  // Ensure that the copying step has completed to ensure consistent behavior.
  TENSORSTORE_EXPECT_OK(write_future2.copy_future);

  write_future1.Force();
  write_future2.Force();
  transaction.CommitAsync().IgnoreFuture();
  {
    auto r = mock_store->write_requests.pop();
    EXPECT_THAT(ParseKey(r.key), ElementsAre(0));
    r(memory_store);
  }

  TENSORSTORE_EXPECT_OK(write_future1);
  TENSORSTORE_EXPECT_OK(write_future2);
  TENSORSTORE_EXPECT_OK(transaction.future());

  EXPECT_THAT(GetChunk({0}), ElementsAre(MakeArray<int>({42, 42})));
}

TEST_F(ChunkCacheTest, SeparateCachesTransactionalReadThenWrite) {
  // Dimension 0 is chunked with a size of 2.
  grid = GetSimple1DGrid();
  auto cache1 = MakeChunkCache();
  auto cache2 = MakeChunkCache();

  Transaction transaction(tensorstore::isolated);

  auto read_future =
      tensorstore::Read(GetTensorStore(cache1, {}, 0, transaction) |
                        tensorstore::Dims(0).TranslateSizedInterval(0, 2));
  {
    auto r = mock_store->read_requests.pop();
    EXPECT_THAT(ParseKey(r.key), ElementsAre(0));
    r(memory_store);
  }

  EXPECT_THAT(read_future.result(),
              ::testing::Optional(MakeArray<int>({0, 1})));

  TENSORSTORE_ASSERT_OK(tensorstore::Write(
      MakeArray<int>({42, 43}),
      GetTensorStore(cache2, {}, 0, transaction) |
          tensorstore::Dims(0).TranslateSizedInterval(0, 2)));

  transaction.CommitAsync().IgnoreFuture();

  {
    auto r = mock_store->write_requests.pop();
    EXPECT_THAT(ParseKey(r.key), ElementsAre(0));
    r(memory_store);
  }

  TENSORSTORE_EXPECT_OK(transaction.future());

  EXPECT_THAT(GetChunk({0}), ElementsAre(MakeArray<int>({42, 43})));
}

TEST_F(ChunkCacheTest, SeparateCachesTransactionalWriteThenRead) {
  // Dimension 0 is chunked with a size of 2.
  grid = GetSimple1DGrid();
  auto cache1 = MakeChunkCache();
  auto cache2 = MakeChunkCache();

  Transaction transaction(tensorstore::isolated);

  TENSORSTORE_ASSERT_OK(tensorstore::Write(
      MakeArray<int>({42, 43}),
      GetTensorStore(cache1, {}, 0, transaction) |
          tensorstore::Dims(0).TranslateSizedInterval(0, 2)));

  EXPECT_THAT(
      tensorstore::Read(GetTensorStore(cache2, {}, 0, transaction) |
                        tensorstore::Dims(0).TranslateSizedInterval(0, 2))
          .result(),
      ::testing::Optional(MakeArray<int>({42, 43})));

  transaction.CommitAsync().IgnoreFuture();

  {
    auto r = mock_store->write_requests.pop();
    EXPECT_THAT(ParseKey(r.key), ElementsAre(0));
    r(memory_store);
  }

  TENSORSTORE_EXPECT_OK(transaction.future());

  EXPECT_THAT(GetChunk({0}), ElementsAre(MakeArray<int>({42, 43})));
}

TEST_F(ChunkCacheTest, SeparateCachesReadIfNotEqualAbort) {
  // Dimension 0 is chunked with a size of 2.
  grid = GetSimple1DGrid();
  auto cache1 = MakeChunkCache();
  auto cache2 = MakeChunkCache();

  Transaction transaction(tensorstore::isolated);

  // Read to populate entry in cache1.
  {
    auto read_future =
        tensorstore::Read(GetTensorStore(cache1, {}, 0, transaction) |
                          tensorstore::Dims(0).TranslateSizedInterval(0, 2));
    {
      auto r = mock_store->read_requests.pop();
      EXPECT_THAT(ParseKey(r.key), ElementsAre(0));
      EXPECT_EQ(StorageGeneration::Unknown(),
                r.options.generation_conditions.if_not_equal);
      r(memory_store);
    }
    EXPECT_THAT(read_future.result(),
                ::testing::Optional(MakeArray<int>({0, 1})));
  }

  // Read to populate read state of transaction node in cache2.
  {
    auto read_future =
        tensorstore::Read(GetTensorStore(cache2, {}, 0, transaction) |
                          tensorstore::Dims(0).TranslateSizedInterval(0, 2));
    {
      auto r = mock_store->read_requests.pop();
      EXPECT_THAT(ParseKey(r.key), ElementsAre(0));
      EXPECT_EQ(StorageGeneration::NoValue(),
                r.options.generation_conditions.if_not_equal);
      r(memory_store);
    }
    EXPECT_THAT(read_future.result(),
                ::testing::Optional(MakeArray<int>({0, 1})));
  }

  // Re-read transaction node in cache2.
  {
    auto read_future =
        tensorstore::Read(GetTensorStore(cache2, {}, 0, transaction) |
                          tensorstore::Dims(0).TranslateSizedInterval(0, 2));
    {
      auto r = mock_store->read_requests.pop();
      EXPECT_THAT(ParseKey(r.key), ElementsAre(0));
      EXPECT_EQ(StorageGeneration::NoValue(),
                r.options.generation_conditions.if_not_equal);
      r(memory_store);
    }
    EXPECT_THAT(read_future.result(),
                ::testing::Optional(MakeArray<int>({0, 1})));
  }
}

TENSORSTORE_GLOBAL_INITIALIZER {
  tensorstore::internal::TensorStoreRepeatableReadTestOptions options;
  options.test_suite_name = "RepeatableReadTest";
  options.fill_value = MakeArray<int>({1, 2});
  options.value1 = MakeArray<int>({3, 4});
  options.value2 = MakeArray<int>({5, 6});
  options.value3 = MakeArray<int>({7, 8});
  options.key = EncodeKey({{0}});

  auto grid = std::make_shared<ChunkGridSpecification>(
      ChunkGridSpecification({ChunkGridSpecification::Component{
          AsyncWriteArray::Spec{SharedArray<const void>(options.fill_value),
                                Box<>(1)},
          /*chunk_shape=*/{2}}}));

  options.encode_value =
      [=](SharedArray<const void> value) -> Result<std::optional<absl::Cord>> {
    return EncodeRaw(*grid, {value});
  };
  options.make_tensorstore =
      [=](const tensorstore::Context& context) -> Result<TensorStore<>> {
    TENSORSTORE_ASSIGN_OR_RETURN(
        auto mock_key_value_store_resource,
        context
            .GetResource<tensorstore::internal::MockKeyValueStoreResource>());
    auto thread_pool = tensorstore::internal::DetachedThreadPool(1);
    const auto& mock_store = *mock_key_value_store_resource;
    auto pool = CachePool::Make(CachePool::Limits{});
    auto cache = GetCache<TestCache>(pool.get(), "", [&] {
      return std::make_unique<TestCache>(mock_store, *grid, thread_pool);
    });
    return tensorstore::internal::TensorStoreAccess::Construct<TensorStore<>>(
               tensorstore::internal::Driver::Handle{
                   MakeDriver(cache), tensorstore::IdentityTransform(1)}) |
           tensorstore::Dims(0).TranslateSizedInterval(0, 2);
  };
  tensorstore::internal::RegisterTensorStoreRepeatableReadTest(options);
}

TEST_F(ChunkCacheTest, CanReferenceSourceDataIndefinitely) {
  // Dimension 0 is chunked with a size of 64.  Need chunk to be at least 256
  // bytes due to riegeli size limits for non copying cords.
  grid = ChunkGridSpecification({ChunkGridSpecification::Component{
      AsyncWriteArray::Spec{
          SharedArray<const void>(tensorstore::AllocateArray<int32_t>(
              {64}, tensorstore::c_order, tensorstore::value_init)),
          Box<>(1)},
      /*chunk_shape=*/{64}}});
  const Index size = 64;
  auto index_array = tensorstore::AllocateArray<int64_t>({size});
  for (Index i = 0; i < size; ++i) {
    index_array(i) = i;
  }
  for (bool use_index_array_for_store : {false, true}) {
    SCOPED_TRACE(absl::StrFormat("use_index_array_for_store=%d",
                                 use_index_array_for_store));
    for (bool use_index_array_for_source : {false, true}) {
      SCOPED_TRACE(absl::StrFormat("use_index_array_for_source=%d",
                                   use_index_array_for_source));
      for (bool reference_source_data : {false, true}) {
        SCOPED_TRACE(
            absl::StrFormat("reference_source_data=%d", reference_source_data));
        auto cache = MakeChunkCache();
        auto store = GetTensorStore(cache);
        auto full_chunk = tensorstore::AllocateArray<int32_t>({size});
        std::fill_n(full_chunk.data(), full_chunk.num_elements(), 42);
        tensorstore::TransformedSharedArray<const void> source_array =
            full_chunk;
        if (use_index_array_for_source) {
          TENSORSTORE_ASSERT_OK_AND_ASSIGN(
              source_array,
              source_array |
                  tensorstore::Dims(0).OuterIndexArraySlice(index_array));
        }
        TENSORSTORE_ASSERT_OK_AND_ASSIGN(
            store, store | tensorstore::Dims(0).SizedInterval(0, size));
        if (use_index_array_for_store) {
          TENSORSTORE_ASSERT_OK_AND_ASSIGN(
              store,
              store | tensorstore::Dims(0).OuterIndexArraySlice(index_array));
        }
        auto write_future = tensorstore::Write(
            source_array, store,
            (reference_source_data
                 ? tensorstore::can_reference_source_data_indefinitely
                 : tensorstore::cannot_reference_source_data));
        const bool zero_copy = reference_source_data &&
                               !use_index_array_for_store &&
                               !use_index_array_for_source;
        {
          auto r = mock_store->write_requests.pop();
          EXPECT_THAT(ParseKey(r.key), ElementsAre(0));
          ASSERT_TRUE(r.value);
          EXPECT_EQ(*r.value, EncodeRaw(*grid, {full_chunk}));
          auto flat = r.value->TryFlat();
          ASSERT_TRUE(flat);
          EXPECT_EQ(zero_copy, (flat->data() == reinterpret_cast<const char*>(
                                                    full_chunk.data())));
          r(memory_store);
        }
        TENSORSTORE_EXPECT_OK(write_future);
      }
    }
  }
}

}  // namespace
