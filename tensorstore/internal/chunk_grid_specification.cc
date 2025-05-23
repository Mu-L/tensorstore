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

#include "tensorstore/internal/chunk_grid_specification.h"

#include <assert.h>
#include <stddef.h>

#include <algorithm>
#include <numeric>
#include <utility>
#include <vector>

#include "tensorstore/box.h"
#include "tensorstore/chunk_layout.h"
#include "tensorstore/contiguous_layout.h"
#include "tensorstore/index.h"
#include "tensorstore/index_interval.h"
#include "tensorstore/internal/async_write_array.h"
#include "tensorstore/rank.h"
#include "tensorstore/util/constant_vector.h"
#include "tensorstore/util/dimension_set.h"
#include "tensorstore/util/result.h"
#include "tensorstore/util/span.h"
#include "tensorstore/util/status.h"

namespace tensorstore {
namespace internal {

ChunkGridSpecification::Component::Component(AsyncWriteArray::Spec array_spec,
                                             std::vector<Index> chunk_shape)
    : array_spec(std::move(array_spec)), chunk_shape(std::move(chunk_shape)) {
  assert(this->chunk_shape.size() == this->array_spec.rank());
  chunked_to_cell_dimensions.resize(this->array_spec.rank());
  std::iota(chunked_to_cell_dimensions.begin(),
            chunked_to_cell_dimensions.end(), static_cast<DimensionIndex>(0));
}

ChunkGridSpecification::Component::Component(
    AsyncWriteArray::Spec array_spec, std::vector<Index> chunk_shape,
    std::vector<DimensionIndex> chunked_to_cell_dimensions)
    : array_spec(std::move(array_spec)),
      chunk_shape(std::move(chunk_shape)),
      chunked_to_cell_dimensions(std::move(chunked_to_cell_dimensions)) {
  assert(this->chunk_shape.size() == this->array_spec.rank());
}

ChunkGridSpecification::ChunkGridSpecification(ComponentList components_arg)
    : components(std::move(components_arg)) {
  assert(!components.empty());
  // Extract the chunk shape from the cell shape of the first component.
  chunk_shape.resize(components[0].chunked_to_cell_dimensions.size());
  for (DimensionIndex i = 0;
       i < static_cast<DimensionIndex>(chunk_shape.size()); ++i) {
    chunk_shape[i] =
        components[0].chunk_shape[components[0].chunked_to_cell_dimensions[i]];
  }
  // Verify that the extents of the chunked dimensions are the same for all
  // components.
#if !defined(NDEBUG)
  for (const auto& component : components) {
    assert(component.chunked_to_cell_dimensions.size() == chunk_shape.size());
    DimensionSet seen_dimensions;
    for (DimensionIndex i = 0;
         i < static_cast<DimensionIndex>(chunk_shape.size()); ++i) {
      const DimensionIndex cell_dim = component.chunked_to_cell_dimensions[i];
      assert(!seen_dimensions[cell_dim]);
      seen_dimensions[cell_dim] = true;
      assert(chunk_shape[i] == component.chunk_shape[cell_dim]);
    }
  }
#endif  // !defined(NDEBUG)
}

void ChunkGridSpecification::GetComponentOrigin(
    const size_t component_index, tensorstore::span<const Index> cell_indices,
    tensorstore::span<Index> origin) const {
  assert(rank() == cell_indices.size());
  assert(component_index < components.size());
  const auto& component_spec = components[component_index];
  assert(component_spec.rank() == origin.size());
  std::fill_n(origin.begin(), origin.size(), Index(0));
  for (DimensionIndex chunk_dim_i = 0;
       chunk_dim_i < static_cast<DimensionIndex>(
                         component_spec.chunked_to_cell_dimensions.size());
       ++chunk_dim_i) {
    const DimensionIndex cell_dim_i =
        component_spec.chunked_to_cell_dimensions[chunk_dim_i];
    origin[cell_dim_i] = cell_indices[chunk_dim_i] * chunk_shape[chunk_dim_i];
  }
}

ChunkGridSpecification::CellDomain ChunkGridSpecification::GetCellDomain(
    size_t component_index, tensorstore::span<const Index> cell_indices) const {
  Box<dynamic_rank(kMaxRank)> domain;
  const auto& component_spec = components[component_index];
  const DimensionIndex component_rank = component_spec.rank();
  domain.set_rank(component_rank);
  GetComponentOrigin(component_index, cell_indices, domain.origin());
  std::copy_n(component_spec.chunk_shape.data(), component_rank,
              domain.shape().data());
  return domain;
}

ChunkGridSpecification::CellDomain ChunkGridSpecification::GetValidCellDomain(
    size_t component_index, tensorstore::span<const Index> cell_indices) const {
  auto domain = GetCellDomain(component_index, cell_indices);
  auto& component_spec = components[component_index];
  DimensionIndex rank = component_spec.rank();
  for (DimensionIndex i = 0; i < rank; ++i) {
    domain[i] =
        Intersect(domain[i], component_spec.array_spec.valid_data_bounds[i]);
  }
  return domain;
}

Result<ChunkLayout> GetChunkLayoutFromGrid(
    const ChunkGridSpecification::Component& component_spec) {
  ChunkLayout layout;
  DimensionIndex inner_order[kMaxRank];
  const DimensionIndex rank = component_spec.rank();
  tensorstore::SetPermutation(c_order, tensorstore::span(inner_order, rank));
  TENSORSTORE_RETURN_IF_ERROR(layout.Set(
      ChunkLayout::InnerOrder(tensorstore::span(inner_order, rank))));
  TENSORSTORE_RETURN_IF_ERROR(
      layout.Set(ChunkLayout::GridOrigin(GetConstantVector<Index, 0>(rank))));
  TENSORSTORE_RETURN_IF_ERROR(
      layout.Set(ChunkLayout::WriteChunkShape(component_spec.shape())));
  TENSORSTORE_RETURN_IF_ERROR(layout.Finalize());
  return layout;
}

}  // namespace internal
}  // namespace tensorstore
