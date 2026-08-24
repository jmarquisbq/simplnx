#pragma once

#include "simplnx/Common/Extent.hpp"
#include "simplnx/Common/Types.hpp"

#include <algorithm>
#include <vector>

namespace nx::core::HDF5
{

/**
 * @brief Given a flat index and a shape, returns the N-dimensional position.
 * Uses row-major (C) ordering: the last dimension varies fastest.
 */
inline std::vector<uint64> flatToNd(uint64 flatIndex, const std::vector<uint64>& shape)
{
  std::vector<uint64> position(shape.size());
  for(uint64 d = shape.size(); d > 0; --d)
  {
    uint64 i = d - 1;
    position[i] = flatIndex % shape[i];
    flatIndex /= shape[i];
  }
  return position;
}

/**
 * @brief Given an N-dimensional position and a shape, returns the flat index.
 * Uses row-major (C) ordering: the last dimension varies fastest.
 */
inline uint64 ndToFlat(const std::vector<uint64>& position, const std::vector<uint64>& shape)
{
  uint64 flat = 0;
  uint64 stride = 1;
  for(uint64 d = shape.size(); d > 0; --d)
  {
    uint64 i = d - 1;
    flat += position[i] * stride;
    stride *= shape[i];
  }
  return flat;
}

/**
 * @brief Given a tuple position, returns which chunk it belongs to (N-dimensional chunk index).
 */
inline std::vector<uint64> positionToChunkNd(const std::vector<uint64>& position, const std::vector<uint64>& chunkShape)
{
  std::vector<uint64> chunkNd(position.size());
  for(uint64 d = 0; d < position.size(); ++d)
  {
    chunkNd[d] = position[d] / chunkShape[d];
  }
  return chunkNd;
}

/**
 * @brief Given N-dimensional chunk indices and chunks-per-dimension, returns the flat chunk index.
 */
inline uint64 chunkNdToFlat(const std::vector<uint64>& chunkNd, const std::vector<uint64>& chunksPerDim)
{
  return ndToFlat(chunkNd, chunksPerDim);
}

/**
 * @brief Returns the number of chunks along each dimension.
 * Computed as ceil(tupleShape[d] / chunkShape[d]) for each dimension.
 */
inline std::vector<uint64> getChunksPerDimension(const std::vector<uint64>& tupleShape, const std::vector<uint64>& chunkShape)
{
  std::vector<uint64> chunksPerDim(tupleShape.size());
  for(uint64 d = 0; d < tupleShape.size(); ++d)
  {
    chunksPerDim[d] = (tupleShape[d] + chunkShape[d] - 1) / chunkShape[d];
  }
  return chunksPerDim;
}

/**
 * @brief Returns the total number of chunks for the given tuple shape and chunk shape.
 */
inline uint64 getNumberOfChunks(const std::vector<uint64>& tupleShape, const std::vector<uint64>& chunkShape)
{
  auto chunksPerDim = getChunksPerDimension(tupleShape, chunkShape);
  uint64 total = 1;
  for(uint64 d = 0; d < chunksPerDim.size(); ++d)
  {
    total *= chunksPerDim[d];
  }
  return total;
}

/**
 * @brief Returns the extent (bounds) of a specific chunk.
 * Edge chunks are clamped to the array bounds (may be smaller than chunkShape).
 */
inline Extent getChunkBounds(uint64 flatChunkIndex, const std::vector<uint64>& tupleShape, const std::vector<uint64>& chunkShape)
{
  auto chunksPerDim = getChunksPerDimension(tupleShape, chunkShape);
  auto chunkNd = flatToNd(flatChunkIndex, chunksPerDim);

  std::vector<uint64> minBounds(tupleShape.size());
  std::vector<uint64> maxBounds(tupleShape.size());
  for(uint64 d = 0; d < tupleShape.size(); ++d)
  {
    minBounds[d] = chunkNd[d] * chunkShape[d];
    maxBounds[d] = std::min(minBounds[d] + chunkShape[d] - 1, tupleShape[d] - 1);
  }
  return Extent{std::move(minBounds), std::move(maxBounds)};
}
} // namespace nx::core::HDF5
