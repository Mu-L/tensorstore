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

#ifndef TENSORSTORE_DRIVER_KVS_BACKED_CHUNK_DRIVER_H_
#define TENSORSTORE_DRIVER_KVS_BACKED_CHUNK_DRIVER_H_

/// \file Framework for implementing TensorStore drivers for storage formats
/// like zarr, n5, and Neuroglancer precomputed backed by an arbitrary key-value
/// store, where there is a key-value store entry for metadata (which may be
/// shared by multiple independent arrays) and one key-value store entry per
/// chunk.

#include <stddef.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "absl/status/status.h"
#include "absl/strings/cord.h"
#include "absl/time/time.h"
#include <nlohmann/json_fwd.hpp>
#include "tensorstore/batch.h"
#include "tensorstore/box.h"
#include "tensorstore/chunk_layout.h"
#include "tensorstore/context.h"
#include "tensorstore/driver/chunk_cache_driver.h"
#include "tensorstore/driver/driver.h"
#include "tensorstore/driver/driver_spec.h"
#include "tensorstore/driver/registry.h"
#include "tensorstore/index.h"
#include "tensorstore/index_space/index_transform.h"
#include "tensorstore/internal/cache/aggregate_writeback_cache.h"
#include "tensorstore/internal/cache/async_cache.h"
#include "tensorstore/internal/cache/async_initialized_cache_mixin.h"
#include "tensorstore/internal/cache/cache.h"
#include "tensorstore/internal/cache/cache_pool_resource.h"
#include "tensorstore/internal/cache/kvs_backed_cache.h"
#include "tensorstore/internal/cache/kvs_backed_chunk_cache.h"
#include "tensorstore/internal/chunk_grid_specification.h"
#include "tensorstore/internal/context_binding.h"
#include "tensorstore/internal/data_copy_concurrency_resource.h"
#include "tensorstore/internal/estimate_heap_usage/estimate_heap_usage.h"
#include "tensorstore/internal/estimate_heap_usage/std_vector.h"  // IWYU pragma: keep
#include "tensorstore/internal/intrusive_ptr.h"
#include "tensorstore/internal/json_binding/bindable.h"
#include "tensorstore/internal/meta/type_traits.h"
#include "tensorstore/internal/open_mode_spec.h"
#include "tensorstore/json_serialization_options.h"
#include "tensorstore/kvstore/driver.h"
#include "tensorstore/kvstore/kvstore.h"
#include "tensorstore/kvstore/spec.h"
#include "tensorstore/open_mode.h"
#include "tensorstore/open_options.h"
#include "tensorstore/rank.h"
#include "tensorstore/serialization/absl_time.h"  // IWYU pragma: keep
#include "tensorstore/serialization/std_optional.h"  // IWYU pragma: keep
#include "tensorstore/staleness_bound.h"
#include "tensorstore/transaction.h"
#include "tensorstore/util/dimension_set.h"
#include "tensorstore/util/executor.h"
#include "tensorstore/util/future.h"
#include "tensorstore/util/garbage_collection/garbage_collection.h"
#include "tensorstore/util/garbage_collection/std_optional.h"  // IWYU pragma: keep
#include "tensorstore/util/result.h"
#include "tensorstore/util/span.h"

namespace tensorstore {
namespace internal_kvs_backed_chunk_driver {

struct FillValueMode {
  bool fill_missing_data_reads;
  bool store_data_equal_to_fill_value;
  static constexpr auto ApplyMembers = [](auto& x, auto f) {
    return f(x.fill_missing_data_reads, x.store_data_equal_to_fill_value);
  };
};

/// Base class for specification representations used by drivers, for use with
/// the driver registry.
///
/// This inherits from `DriverConstraints` as required by the driver registry.
struct KvsDriverSpec : public internal::DriverSpec,
                       public internal::OpenModeSpec {
  kvstore::Spec store;
  Context::Resource<internal::DataCopyConcurrencyResource>
      data_copy_concurrency;
  Context::Resource<internal::CachePoolResource> cache_pool;
  std::optional<Context::Resource<internal::CachePoolResource>>
      metadata_cache_pool;
  StalenessBounds staleness;
  FillValueMode fill_value_mode;

  // Initialize from a URL with the specified base kvstore and
  // optional encoded path.
  void InitializeFromUrl(kvstore::Spec&& base, std::string_view encoded_path);

  static constexpr auto ApplyMembers = [](auto& x, auto f) {
    return f(internal::BaseCast<internal::DriverSpec>(x),
             internal::BaseCast<internal::OpenModeSpec>(x), x.store,
             x.data_copy_concurrency, x.cache_pool, x.metadata_cache_pool,
             x.staleness, x.fill_value_mode);
  };

  kvstore::Spec GetKvstore() const override;

  OpenMode open_mode() const override;

  absl::Status ApplyOptions(SpecOptions&& options) override;
};

/// JSON binder for the base `SpecData` type, must be called by the
/// driver-specific JSON binders.
TENSORSTORE_DECLARE_JSON_BINDER(SpecJsonBinder, KvsDriverSpec,
                                JsonSerializationOptions,
                                JsonSerializationOptions,
                                ::nlohmann::json::object_t);

/// Specifies constraints for atomic metadata updates.
enum AtomicUpdateConstraint {
  /// No additional constraints.  The update function may be called without
  /// any prior attempt to read the existing metadata (in which case it will
  /// be called with `existing_metadata=nullptr`).
  kNone,

  /// The `update` function is guaranteed to fail if the metadata does not
  /// exist.  It will, however, still be called in such a case to determine
  /// the appropriate error result.
  kRequireExisting,

  /// The `update` function is guaranteed to fail if the metadata already
  /// exists.  It may, however, still be called in such a case to determine the
  /// appropriate error result.  Specifying this option allows unnecessary
  /// re-reads to be avoided.
  kRequireMissing,
};

/// Caches metadata associated with a kvstore-backed chunk driver.  Driver
/// implementations must define a derived type that inherits from this class to
/// perform driver-specific metadata handling.
///
/// There is one entry in the `MetadataCache` for each `DataCache` (but there
/// may be multiple `DataCache` objects associated with a single entry).
///
/// Typically for a given driver, there will be one `MetadataCache` per
/// underlying `kvstore::Driver` with which the driver is used, though there
/// could be more than one if the `MetadataCache` depends on some additional
/// parameters.  Entries within the `MetadataCache` correspond to individual
/// paths within the `kvstore::Driver` and store the decoded metadata
/// representation.
///
/// Implicitly, instances of this class assume a particular `Metadata` type.
class MetadataCache
    : public internal::AggregateWritebackCache<
          MetadataCache,
          internal::KvsBackedCache<MetadataCache, internal::AsyncCache>>,
      public internal::AsyncInitializedCacheMixin {
  using Base = internal::AggregateWritebackCache<
      MetadataCache,
      internal::KvsBackedCache<MetadataCache, internal::AsyncCache>>;

 public:
  using MetadataPtr = std::shared_ptr<const void>;

  /// Constructor parameters
  struct Initializer {
    Context::Resource<internal::DataCopyConcurrencyResource>
        data_copy_concurrency;
    Context::Resource<internal::CachePoolResource> cache_pool;
  };

  explicit MetadataCache(Initializer initializer);

  /// Returns the kvstore key from which to read the encoded metadata for a
  /// given metadata cache entry.
  ///
  /// Typically, this appends a suffix like "/.zarr".
  virtual std::string GetMetadataStorageKey(std::string_view entry_key) = 0;

  /// Decodes metadata read from the kvstore.
  ///
  /// \param entry_key The metadata cache entry key (not the kvstore key) with
  ///     which this metadata is associated.
  /// \param encoded_metadata The encoded metadata read from the kvstore.
  /// \returns On success, non-null pointer to `Metadata` object.
  virtual Result<MetadataPtr> DecodeMetadata(std::string_view entry_key,
                                             absl::Cord encoded_metadata) = 0;

  /// Encodes metadata for storage in the kvstore.
  ///
  /// \param entry_key The metadata cache entry key (not the kvstore key) with
  ///     which this metadata is associated.
  /// \param metadata Non-null pointer to the metadata to encode, of type
  ///     `Metadata`.
  virtual Result<absl::Cord> EncodeMetadata(std::string_view entry_key,
                                            const void* metadata) = 0;

  // The members below are implementation details not relevant to derived class
  // driver implementations.

  /// Function invoked to atomically modify a metadata entry (e.g. to create
  /// or resize an array).
  ///
  /// This function may be called multiple times (even after returning an
  /// error status) due to retries to handle concurrent modifications to the
  /// metadata.
  ///
  /// \param existing_metadata Specifies the existing metadata of type
  ///     `Metadata`, or `nullptr` to indicate no existing metadata.
  /// \returns The new metadata on success, or an error result for the
  /// request.
  using UpdateFunction =
      std::function<Result<MetadataPtr>(const MetadataPtr& existing_metadata)>;

  /// Specifies a request to atomically read-modify-write a metadata entry.
  struct PendingWrite {
    UpdateFunction update;
    AtomicUpdateConstraint update_constraint;
    Promise<void> promise;
  };

  class Entry : public Base::Entry {
   public:
    using OwningCache = MetadataCache;

    MetadataPtr GetMetadata() { return ReadLock<void>(*this).shared_data(); }

    Result<MetadataPtr> GetMetadata(internal::OpenTransactionPtr transaction);

    Future<MetadataPtr> ReadMetadata(internal::OpenTransactionPtr transaction,
                                     AsyncCacheReadRequest request);

    void DoDecode(std::optional<absl::Cord> value,
                  DecodeReceiver receiver) override;
    void DoEncode(EncodeOptions options, std::shared_ptr<const void> data,
                  EncodeReceiver receiver) override;
    std::string GetKeyValueStoreKey() override;

    /// Requests an atomic metadata update.
    ///
    /// \param transaction The transaction to use.
    /// \param update Update function to apply.
    /// \param update_constraint Specifies additional constraints on the atomic
    ///     update.
    /// \param read_time If `transaction` is specified and
    ///     `!transaction.implicit_transaction()` and `read_time` is specified,
    ///     the returned `Future` does not complete until metadata as of the
    ///     specified `*read_time` is available in the cache.
    /// \returns Future that becomes ready when the request has completed
    ///     (either successfully or with an error).  Any error returned from the
    ///     last call to `update` (i.e. the last retry) is returned as the
    ///     Future result.  Additionally, any error that occurs with the
    ///     underlying kvstore is also returned.
    Future<const void> RequestAtomicUpdate(
        const internal::OpenTransactionPtr& transaction, UpdateFunction update,
        AtomicUpdateConstraint update_constraint,
        std::optional<absl::Time> read_time = {});
  };

  class TransactionNode : public Base::TransactionNode {
   public:
    using OwningCache = MetadataCache;
    using MetadataCache::Base::TransactionNode::TransactionNode;

    /// Returns the metadata after applying all requested updates.
    ///
    /// Returns an error if any updates cannot be applied.
    Result<MetadataPtr> GetUpdatedMetadata(MetadataPtr metadata);

    Result<MetadataPtr> GetUpdatedMetadata();

    void DoApply(ApplyOptions options, ApplyReceiver receiver) override;

    void InvalidateReadState() override;

   private:
    friend class Entry;

    /// Base read state on which `updated_metadata_` is based.
    MetadataPtr updated_metadata_base_state_;
    /// Cached result of applying updates to `updated_metadata_base_state_`.
    Result<MetadataPtr> updated_metadata_ = nullptr;
  };

  Entry* DoAllocateEntry() final { return new Entry; }
  size_t DoGetSizeofEntry() final { return sizeof(Entry); }
  TransactionNode* DoAllocateTransactionNode(AsyncCache::Entry& entry) final {
    return new TransactionNode(static_cast<Entry&>(entry));
  }

  kvstore::Driver* base_store() { return base_store_.get(); }

  const Executor& executor() { return data_copy_concurrency_->executor; }

  /// Key-value store from which `kvstore_driver()` was derived.  Used only by
  /// `GetBoundSpecData`.  A driver implementation may apply some type of
  /// adapter to the `kvstore_driver()` in order to retrieve metadata by
  /// overriding the default implementation of
  /// `OpenState::GetMetadataKeyValueStore`.
  kvstore::DriverPtr base_store_;
  Context::Resource<internal::DataCopyConcurrencyResource>
      data_copy_concurrency_;
  Context::Resource<internal::CachePoolResource> metadata_cache_pool_;
};

/// Abstract base class for `Cache` types that are used with
/// `KvsMetadataDriverBase`.
///
/// This is intended to be used as a sibling/cousin base class of
/// `internal::Cache`.
///
/// Like `ChunkCache`, this supports more than one "component", where a
/// component is identified by a `size_t component_index`.
///
/// Derived classes must define various methods for computing bounds and
/// transforms from the metadata, and validating the metadata.
///
/// Implicitly, instances of this class assume a particular `Metadata` type.
class DataCacheBase {
 public:
  using MetadataPtr = MetadataCache::MetadataPtr;

  struct Initializer {
    internal::PinnedCacheEntry<MetadataCache> metadata_cache_entry;
    MetadataPtr metadata;
    Context::Resource<internal::CachePoolResource> cache_pool;
  };

  explicit DataCacheBase(Initializer&& initializer);
  virtual ~DataCacheBase();

  /// Enables support for `internal::CachePtr` despite note inheriting from
  /// `internal::Cache`.  Implementations should be equivalent to
  /// `static_cast<internal::Cache&>(static_cast<Derived&>(*this))`.
  ///
  /// This provides a way to obtain the actual `internal::Cache` type without
  /// relying on virtual inheritance.
  virtual internal::Cache& cache() = 0;

  using Ptr = internal::CachePtr<DataCacheBase>;

  /// Validates that `new_metadata` is compatible with `existing_metadata` for
  /// the purposes of this data cache.
  ///
  /// The `new_metadata` should be considered compatible if chunks encoded using
  /// `existing_metadata` are compatible with chunks encoded using
  /// `new_metadata`.  Changes in the bound of resizable (chunked) dimensions
  /// should generally be considered compatible.
  ///
  /// If this function returns success,
  /// `GetChunkGridSpecification(existing_metadata)` must equal
  /// `GetChunkGridSpecification(new_metadata)`.
  ///
  /// \param existing_metadata Non-null pointer to existing metadata, of type
  ///     `Metadata`.
  /// \param existing_metadata Non-null pointer to new metadata, of type
  ///     `Metadata`.
  /// \returns `absl::Status()` if compatible.
  /// \error `absl::StatusCode::kFailedPrecondition` if not compatible.
  virtual absl::Status ValidateMetadataCompatibility(
      const void* existing_metadata, const void* new_metadata) = 0;

  /// Sets `spec` with the bound spec data associated with the specified
  /// component.
  ///
  /// \param spec[out] Reference to derived SpecData type for the driver.  The
  ///     implementation must `static_cast` this pointer to the appropriate
  ///     derived type.
  /// \param metadata Non-null pointer to metadata of type `Metadata`.
  /// \param component_index The ChunkCache component index.
  virtual absl::Status GetBoundSpecData(KvsDriverSpec& spec,
                                        const void* metadata,
                                        size_t component_index) = 0;

  /// Returns a transform from the "external" index space visible in the `Spec`
  /// to the index space of component `component_index` in the `ChunkCache`.
  ///
  /// For example, the returned transform may apply a translation to account for
  /// an offset specified in the `metadata`.
  ///
  /// An identity transform may be indicated by returning a null transform
  /// (which is what is returned by default if this method is not overridden).
  ///
  /// Any implicit bounds in the domain of the returned transform are inferred
  /// from the `ChunkGridSpecification`.
  ///
  /// \param metadata The metadata.
  /// \param component_index The ChunkCache component index.
  /// \returns The index transform, or a null transform.
  /// \pre `component_index` is less than
  ///     `GetChunkGridSpecification(metadata).component.size()`.
  /// \post The returned transform, if not null, must have an input and output
  ///     rank equal to `component_spec.rank()`, where `component_spec` is
  ///     `GetChunkGridSpecification(metadata).components[component_index]`, and
  ///     must be invertible.
  virtual Result<IndexTransform<>> GetExternalToInternalTransform(
      const void* metadata, size_t component_index);

  virtual void GetComponentBounds(const void* metadata, size_t component_index,
                                  Box<dynamic_rank(kMaxRank)>& bounds,
                                  DimensionSet& implicit_lower_bounds,
                                  DimensionSet& implicit_upper_bounds) = 0;

  /// Returns the kvstore path to include in the spec.
  virtual std::string GetBaseKvstorePath() = 0;

  MetadataCache* metadata_cache() const {
    return &GetOwningCache(*metadata_cache_entry_);
  }

  const Executor& executor() const { return metadata_cache()->executor(); }

  const internal::PinnedCacheEntry<MetadataCache>& metadata_cache_entry()
      const {
    return metadata_cache_entry_;
  }
  const MetadataPtr& initial_metadata() const { return initial_metadata_; }

  const internal::PinnedCacheEntry<MetadataCache> metadata_cache_entry_;
  const MetadataPtr initial_metadata_;
  Context::Resource<internal::CachePoolResource> cache_pool_;
};

/// Abstract base class for `Cache` types that are used with
/// `KvsChunkedDriverBase`.
///
/// Extends `DataCacheBase` with resize and bound computation logic that assumes
/// each grid cell from a `internal::ChunkGridSpecification` corresponds to a
/// kvstore key.
///
/// Implicitly, instances of this class assume a particular `Metadata` type.
class ChunkedDataCacheBase : public DataCacheBase {
 public:
  using Ptr = internal::CachePtr<ChunkedDataCacheBase>;

  using DataCacheBase::DataCacheBase;
  using DataCacheBase::executor;

  /// Returns the grid specification.
  virtual const internal::ChunkGridSpecification& grid() const = 0;

  /// Returns the storage key for the given grid cell.
  virtual std::string GetChunkStorageKey(span<const Index> cell_indices) = 0;

  /// Fills `bounds`, `implicit_lower_bounds`, and `implicit_upper_bounds` with
  /// the current bounds for the chunked dimensions as specified in `metadata`.
  ///
  /// Resizable lower/upper bound should be marked implicit (i.e. a value of
  /// `true` in `implicit_lower_bounds` or `implicit_upper_bounds`).
  ///
  /// \param metadata Non-null pointer to metadata of type `Metadata`.
  /// \param bounds[out] Box of rank equal to `grid_rank`, where
  ///     `grid_rank = GetChunkGridSpecification(metadata).grid_rank()`.
  /// \param implicit_lower_bounds[out] Bit vector of length `bounds.rank()`.
  /// \param implicit_upper_bounds[out] Bit vector of length `bounds.rank()`.
  virtual void GetChunkGridBounds(const void* metadata, MutableBoxView<> bounds,
                                  DimensionSet& implicit_lower_bounds,
                                  DimensionSet& implicit_upper_bounds) = 0;

  /// Computes the bounds for the given component from the specified metadata.
  void GetComponentBounds(const void* metadata, size_t component_index,
                          Box<dynamic_rank(kMaxRank)>& bounds,
                          DimensionSet& implicit_lower_bounds,
                          DimensionSet& implicit_upper_bounds) override;

  /// Returns the chunk layout for the specified component.
  ///
  /// By default, returns a chunk layout computed from `this->grid()`.
  ///
  /// \param metadata Non-null pointer to the metadata of type `Metadata`.
  /// \param component_index The ChunkCache component index.
  virtual Result<ChunkLayout> GetChunkLayoutFromMetadata(
      const void* metadata_ptr, size_t component_index) = 0;

  virtual Result<ChunkLayout> GetChunkLayout(size_t component_index);

  /// Returns a non-null pointer to a copy of `existing_metadata` with the
  /// specified bounds resized.
  ///
  /// \param existing_metadata Non-null pointer to existing metadata of type
  ///     `Metadata`.
  /// \param new_inclusive_min Specifies the new inclusive lower bounds.  A
  ///     value of `kImplicit` indicates no change.
  /// \param new_exclusive_max Specifies the new exclusive upper bounds, of
  ///     length `grid_rank`.  A value of `kImplicit` indicates no change.
  /// \pre `new_inclusive_min.size()` and `new_exclusive_max.size()` are equal
  ///     to `GetChunkGridSpecification(existing_metadata).grid_rank()`.
  /// \pre `existing_metadata` is compatible with the initial metadata from
  ///     which this cache was constructed, according to
  ///     `ValidateMetadataCompatibility`.
  virtual Result<std::shared_ptr<const void>> GetResizedMetadata(
      const void* existing_metadata, span<const Index> new_inclusive_min,
      span<const Index> new_exclusive_max) = 0;

  virtual Future<const void> DeleteCell(
      span<const Index> grid_cell_indices,
      internal::OpenTransactionPtr transaction) = 0;
};

struct DataCacheInitializer : public ChunkedDataCacheBase::Initializer {
  kvstore::DriverPtr store;
};

/// Combines `KvsBackedChunkCache` with `ChunkedDataCacheBase`.
class DataCache : public internal::KvsBackedChunkCache,
                  public ChunkedDataCacheBase {
 public:
  using DataCacheBase::executor;

  using Initializer = DataCacheInitializer;

  explicit DataCache(Initializer&& initializer,
                     internal::ChunkGridSpecification&& grid);

  const Executor& executor() const final {
    return ChunkedDataCacheBase::executor();
  }

  internal::Cache& cache() final { return *this; }

  const internal::ChunkGridSpecification& grid() const final { return grid_; }

  Future<const void> DeleteCell(span<const Index> grid_cell_indices,
                                internal::OpenTransactionPtr transaction) final;

  internal::ChunkGridSpecification grid_;
};

/// Private data members of `OpenState`.
struct PrivateOpenState {
  internal::OpenTransactionPtr transaction_;
  Batch batch_{no_batch};
  internal::DriverSpec::PtrT<const KvsDriverSpec> spec_;
  ReadWriteMode read_write_mode_;
  std::string metadata_cache_key_;
  /// Pointer to `MetadataCache::Entry`, but upcast to type
  /// `internal::AsyncCache::Entry` to avoid having to define
  /// `MetadataCache` in this header.
  internal::PinnedCacheEntry<MetadataCache> metadata_cache_entry_;
  /// Time at which open request was initiated.
  absl::Time request_time_;
};

/// Abstract driver base class for use with `DataCacheBase`.
class KvsMetadataDriverBase : public internal::Driver {
 public:
  /// Forwards to `ResolveBound` overload below with
  /// `metadata_staleness_bound_`.
  Future<IndexTransform<>> ResolveBounds(ResolveBoundsRequest request) override;

  Future<IndexTransform<>> ResolveBounds(
      ResolveBoundsRequest request, StalenessBound metadata_staleness_bound);

  /// Queries the current metadata, as of `metadata_staleness_bound`.
  Future<MetadataCache::MetadataPtr> ResolveMetadata(
      internal::OpenTransactionPtr transaction,
      absl::Time metadata_staleness_bound);

  Result<IndexTransform<>> GetBoundSpecData(
      internal::OpenTransactionPtr transaction, KvsDriverSpec& spec,
      IndexTransformView<> transform);

  KvStore GetKvstore(const Transaction& transaction) override;

  /// Base class intended for use in implementing
  /// `tensorstore::garbage_collection::GarbageCollection<Derived>`
  /// specializations for `Derived` driver types.
  ///
  /// This handles the base kvstore which was used to open the driver.
  ///
  /// For `Derived` driver (and associated derived `DataCache` and
  /// `MetadataCache`) types with no additional data members that require
  /// garbage collection support, it is sufficient to define an empty
  /// `GarbageCollection` specialization that simply inherits from this type:
  ///
  ///     TENSORSTORE_DEFINE_GARBAGE_COLLECTION_SPECIALIZATION(
  ///         Derived, Derived::GarbageCollection)
  ///
  /// Otherwise, you should define a `Visit` function that visits any additional
  /// data members that require garbage collection support and then calls this
  /// `Visit` function.
  struct GarbageCollectionBase {
    static void Visit(garbage_collection::GarbageCollectionVisitor& visitor,
                      const KvsMetadataDriverBase& value);
  };

  virtual DataCacheBase* cache() const = 0;
  virtual size_t component_index() const = 0;

  const StalenessBound& metadata_staleness_bound() const {
    return metadata_staleness_bound_;
  }

  virtual const StalenessBound& data_staleness_bound() const = 0;

  // Accessors for use by ChunkCacheReadWriteDriverMixin, if the derived class
  // happens to use it.
  bool fill_missing_data_reads() const {
    return fill_value_mode_.fill_missing_data_reads;
  }
  bool store_data_equal_to_fill_value() const {
    return fill_value_mode_.store_data_equal_to_fill_value;
  }

  // Treat as private:

  StalenessBound metadata_staleness_bound_;

  /// If `OpenMode::assume_metadata` or `OpenMode::assume_cached_metadata` was
  /// specified, set to the assumed metadata.  Otherwise, set to `nullptr`.
  std::shared_ptr<const void> assumed_metadata_;

  /// If `OpenMode::assume_cached_metadata` was specified, set to the time of
  /// the open request.  If `OpenMode::assume_metadata` was specified, set to
  /// `absl::InfiniteFuture()`.  Otherwise, set to `absl::InfinitePast()`.
  absl::Time assumed_metadata_time_ = absl::InfinitePast();

  FillValueMode fill_value_mode_;
};

/// Abstract driver base class for use with `ChunkedDataCacheBase`.
///
/// Defines the `Resize` and `GetChunkLayout` methods.
class KvsChunkedDriverBase : public KvsMetadataDriverBase {
 public:
  virtual ChunkedDataCacheBase* cache() const override = 0;

  /// Returns a chunk layout derived from the metadata.
  Result<ChunkLayout> GetChunkLayout(IndexTransformView<> transform) override;

  Future<IndexTransform<>> Resize(
      internal::Driver::ResizeRequest request) override;
};

using DriverInitializer = internal::ChunkCacheDriverInitializer<DataCacheBase>;

/// Interface by which driver implementations define the open behavior.
///
/// An object of this type is created for each open request.
///
/// Implicitly, instances of this class are associated with a particular
/// `Metadata` type.
///
/// This class handles caching/opening/creating the metadata, but does not
/// integrate a "data cache" (i.e. `DataCacheBase`).  Integration with a "data
/// cache" is added by the derived class `OpenState`.
///
/// Driver implementations should inherit from
/// `RegisteredKvsDriver<Derived>::OpenStateBase`, rather than this class
/// directly.
class MetadataOpenState
    : public internal::AtomicReferenceCount<MetadataOpenState>,
      private PrivateOpenState {
 public:
  using Ptr = internal::IntrusivePtr<MetadataOpenState>;

  struct Initializer {
    internal::DriverSpec::PtrT<const KvsDriverSpec> spec;
    internal::DriverOpenRequest request;
  };

  explicit MetadataOpenState(Initializer initializer);
  virtual ~MetadataOpenState();

  /// Returns the prefix to delete when `OpenMode::delete_existing` is
  /// specified.
  ///
  /// Typically this appends "/" to the key prefix.
  virtual std::string GetPrefixForDeleteExisting() = 0;

  /// Returns the metadata cache entry key to use to find the metadata.
  ///
  /// Typically this is equal to the key prefix.
  virtual std::string GetMetadataCacheEntryKey() = 0;

  /// Returns the constraint that applies to the `Create` method.
  ///
  /// By default, returns `kRequireMissing`.
  virtual AtomicUpdateConstraint GetCreateConstraint();

  struct CreateOptions {
    /// Indicates if `assume_metadata` or `assume_cached_metadata` was
    /// specified. It is guaranteed that `existing_metadata == nullptr` in this
    /// case.
    bool assume_metadata = false;
  };

  /// Returns the metadata with a new array created.
  ///
  /// The behavior must be consistent with the constraint returned by
  /// `GetCreateConstraint()`.
  ///
  /// \param existing_metadata Pointer to the existing metadata of type
  ///     `Metadata`, or `nullptr` if there is no existing metadata.
  /// \param options Additional options.
  /// \error `absl::StatusCode::kAlreadyExists` if create failed because the
  ///     array may already exist.
  virtual Result<std::shared_ptr<const void>> Create(
      const void* existing_metadata, CreateOptions options) = 0;

  /// Returns a unique identifier (for a given value of `typeid(*this)`) of the
  /// state returned by `GetMetadataCache`.
  ///
  /// By default, returns the empty string.
  virtual std::string GetMetadataCacheKey();

  /// Returns a non-null pointer to a `MetadataCache` object associated with the
  /// same `Metadata` type as this object.  If there is an existing metadata
  /// cache in `context()` with the same cache key (as returned by
  /// `GetMetadataCacheKey`), it will be used instead and this method will not
  /// be called.
  virtual std::unique_ptr<MetadataCache> GetMetadataCache(
      MetadataCache::Initializer initializer) = 0;

  /// Returns the `kvstore::Driver` to use for retrieving the metadata.
  ///
  /// Any parameters of the `OpenState` that affect the returned
  /// `kvstore::Driver` must be encoded in the value returned from
  /// `GetMetadataCacheKey()`.
  ///
  /// The default implementation simply returns `base_kv_store`.
  virtual Result<kvstore::DriverPtr> GetMetadataKeyValueStore(
      kvstore::DriverPtr base_kv_store);

  virtual Result<internal::Driver::Handle> CreateDriverHandleFromMetadata(
      std::shared_ptr<const void> metadata) = 0;

  /// Returns a mask specifying whether reading and/or writing is supported.
  ///
  /// By default, returns `ReadWriteMode::read_write`.
  virtual ReadWriteMode GetReadWriteMode(const void* metadata);

  const KvsDriverSpec& spec() const { return *spec_; }

  /// Returns the data copy executor.
  const Executor& executor() const {
    return spec_->data_copy_concurrency->executor;
  }

  const Context::Resource<internal::CachePoolResource>& cache_pool() const {
    return spec_->cache_pool;
  }
  const Context::Resource<internal::CachePoolResource>& metadata_cache_pool()
      const {
    return spec_->metadata_cache_pool ? *spec_->metadata_cache_pool
                                      : spec_->cache_pool;
  }
};

/// Extends `MetadataOpenState` with integration with a "data cache"
/// (i.e. `DataCacheBase`).
class OpenState : public MetadataOpenState {
 public:
  using Ptr = internal::IntrusivePtr<OpenState>;

  using MetadataOpenState::MetadataOpenState;

  Result<internal::Driver::Handle> CreateDriverHandleFromMetadata(
      std::shared_ptr<const void> metadata) override;

  /// Returns a newly-allocated object of the appropriate derived `Driver`
  /// class.
  ///
  /// Defined automatically by `RegisteredOpenState`.
  virtual KvsMetadataDriverBase* AllocateDriver(
      DriverInitializer&& initializer) = 0;

  /// Returns a unique identifier (for a given value of `typeid(*this)`) of the
  /// cache returned by `GetDataCache`.
  virtual std::string GetDataCacheKey(const void* metadata) = 0;

  /// Indicates whether the data cache returned by `GetDataCache` should be
  /// associated with the metadata cache. Normally, the data cache should use
  /// the data cache pool, but e.g. in the case of a zarr v3 sharded array, the
  /// outer cache corresponding to the shard grid is associated with the
  /// metadata cache pool, and only the leaf chunk caches are associated with
  /// the data cache.
  virtual bool DataCacheUsesMetadataCachePool(const void* metadata_ptr);

  /// Returns a non-null pointer to a `Cache` object associated with the
  /// same `Metadata` type as this object.  If there is an existing data cache
  /// in `context()` with the same cache key (as returned by `GetDataCacheKey`),
  /// it will be used instead and this method will not be called.
  virtual std::unique_ptr<DataCacheBase> GetDataCache(
      DataCacheInitializer&& initializer) = 0;

  /// Returns the `kvstore::Driver` to use for retrieving the data chunks.
  ///
  /// Any parameters of the `OpenState` that affect the returned
  /// `kvstore::Driver` must be encoded in the value returned from
  /// `GetDataCacheKey(metadata)`.
  ///
  /// The default implementation simply returns `base_kv_store`.
  virtual Result<kvstore::DriverPtr> GetDataKeyValueStore(
      kvstore::DriverPtr base_kv_store, const void* metadata);

  /// Returns the component index within the data cache.
  ///
  /// If the `metadata` is not compatible, returns an error.
  virtual Result<size_t> GetComponentIndex(const void* metadata,
                                           OpenMode open_mode) = 0;
};

/// Attempts to open a TensorStore with a kvstore-backed chunk driver.
///
/// This is intended to be used within the implementation of the open function
/// for drivers based on this framework.
///
/// Creating/opening a kvstore-backed chunked array proceeds as follows:
///
/// 1. Opens the `kvstore::Driver` specified by `open_state.spec().store`.
///
/// 2. If `OpenMode::delete_existing` is specified, deletes all keys starting
///    with `open_state->GetPrefixForDeleteExisting()`.
///
/// 3. If `OpenMode::open` is specified, attempt to read the existing metadata
///    from the metadata cache using an entry key of
///    `open_state->GetMetadataCacheEntryKey()` (the read may be satisfied by
///    the metadata cache).
///
///    - If the read is successful, checks whether the metadata is compatible
///      with the open request by calling `open_state->GetComponentIndex`.
///      If it is compatible, proceeds to step 5.
///
///    - If `OpenMode::create` is specified, and either the existing metadata is
///      not found or `GetComponentIndex` returned an error of
///      `absl::StatusCode::kNotFound`, proceeds to step 4.
///
///    - Otherwise, fails.
///
/// 4. If `OpenMode::create` is specified, attempts to atomically create/update
///    the metadata in `open_state->GetMetadataCacheEntryKey()` by calling
///    `open_state->Create` (possibly more than once if retries are needed as
///    part of the atomic read-modify-write).
///
///    - If the metadata is created/updated successfully, proceeds to step 5.
///
///    - If `open_state->Create` fails with an error of
///      `absl::StatusCode::kAlreadyExists`, then if `OpenMode::open`
///      is specified and the metadata can be read successfully, proceeds to
///      step 5.
///
///    - Otherwise, fails.
///
/// 5. Checks whether the metadata that was read (or retrieved from the cache)
///    is compatible with the open request by calling
///    `open_state->GetComponentIndex`.
///
///    - If it is, either reuses an existing `DataCache` with a cache key that
///      matches `open_state->GetDataCacheKey`, or obtain a new `DataCache` from
///      `open_state->GetDataCache`.
///
///    - Otherwise, fails.
///
/// 6. Calls `data_cache->GetExternalToInternalTransform` to compute the
///    `IndexTransform` to use, where `data_cache` is either the existing or
///    newly created `DataCache`.
///
/// \param open_state Non-null pointer to open state.
Future<internal::Driver::Handle> OpenDriver(MetadataOpenState::Ptr open_state);

/// CRTP base class for kvstore-backed driver implementations.
///
/// This integrates with `OpenState`.
///
/// `Derived` driver implementations should inherit from this class and define
/// the following members:
///
///     using DerivedBase =
///         internal_kvs_backed_chunk_driver::RegisteredKvsDriver<
///             Derived, DerivedSpec, DataCacheType, ParentDriver>;
///     class Derived : public DerivedBase {
///       using Base = DerivedBase;
///      public:
///       // Must inherit the constructors.
///       using Base::Base;
///
///       // Defines the `OpenState` class used to open the driver.  It must
///       // inherit from `Base::OpenStateBase`, inherit its constructors, and
///       // implement all of the required virtual methods of
///       // `internal_kvs_backed_chunk_driver::OpenState` defined above.  The
///       // `spec()` method defined by `Base::OpenState` may be used to access
///       // the bound spec data of type `SpecData`.
///       class OpenState : public Base::OpenStateBase {
///        public:
///         using Base::OpenStateBase::OpenStateBase;
///
///         // ...
///       };
template <typename Derived, typename DerivedSpec, typename DataCacheType,
          typename Parent>
class RegisteredKvsDriver
    : public internal::RegisteredDriver<
          Derived,
          internal::ChunkGridSpecificationDriver<DataCacheType, Parent>> {
 public:
  using Base = internal::RegisteredDriver<
      Derived, internal::ChunkGridSpecificationDriver<DataCacheType, Parent>>;

  using Base::Base;

  using Initializer = DriverInitializer;

  /// CRTP base class for the OpenState associated with kvstore-backed
  /// driver implementations.
  class OpenStateBase : public internal_kvs_backed_chunk_driver::OpenState {
   public:
    static_assert(std::is_base_of_v<KvsMetadataDriverBase, Derived>);
    static_assert(std::is_base_of_v<KvsDriverSpec, DerivedSpec>);

    using internal_kvs_backed_chunk_driver::OpenState::OpenState;

    /// Returns a reference to the bound spec data of type `Derived::SpecData`
    /// used to open the driver.
    ///
    /// This is intended to be called by the derived class to implement the
    /// `OpenState` interface.
    decltype(auto) spec() const {
      return static_cast<const DerivedSpec&>(
          internal_kvs_backed_chunk_driver::OpenState::spec());
    }

    /// Returns a newly allocated object of the `Derived` driver type, as
    /// required by `internal_kvs_backed_chunk_driver::Driver`.
    KvsMetadataDriverBase* AllocateDriver(
        DriverInitializer&& initializer) override {
      return new Derived(std::move(initializer));
    }
  };

  Result<internal::TransformedDriverSpec> GetBoundSpec(
      internal::OpenTransactionPtr transaction,
      IndexTransformView<> transform) override {
    auto driver_spec = KvsDriverSpec::Make<DerivedSpec>();
    driver_spec->context_binding_state_ = ContextBindingState::bound;
    internal::TransformedDriverSpec spec;
    TENSORSTORE_ASSIGN_OR_RETURN(
        spec.transform, static_cast<Derived*>(this)->GetBoundSpecData(
                            std::move(transaction), *driver_spec, transform));
    spec.driver_spec = std::move(driver_spec);
    return spec;
  }

  /// Implements the `Open` method required by `internal::RegisteredDriver` in
  /// terms of `internal_kvs_backed_chunk_driver::OpenDriver`.
  static Future<internal::Driver::Handle> Open(
      const DerivedSpec* spec, internal::DriverOpenRequest request) {
    return internal_kvs_backed_chunk_driver::OpenDriver(
        internal_kvs_backed_chunk_driver::MetadataOpenState::Ptr(
            new typename Derived::OpenState(
                internal_kvs_backed_chunk_driver::MetadataOpenState::
                    Initializer{
                        internal::DriverSpec::PtrT<const DerivedSpec>(spec),
                        std::move(request)})));
  }
};

}  // namespace internal_kvs_backed_chunk_driver

namespace internal {
// `MetadataCache::PendingWrite` stores some dynamically allocated state but it
// is not practical to track the memory usage for the purposes of caching and in
// any case the memory usage should be negligible.
template <>
struct HeapUsageEstimator<
    internal_kvs_backed_chunk_driver::MetadataCache::PendingWrite> {
  constexpr static bool MayUseHeapMemory() { return false; }
};
}  // namespace internal
}  // namespace tensorstore

#endif  // TENSORSTORE_DRIVER_KVS_BACKED_CHUNK_DRIVER_H_
