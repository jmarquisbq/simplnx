#include "simplnx/Utilities/Parsing/HDF5/ChunkShapePolicy.hpp"

#include <algorithm>

namespace
{
/**
 * @brief Returns how many "rows" of a dataset fit @p targetBytes, clamped to [1, maxRows].
 * A row is the full extent of every dimension from @p firstRowDim onward times the per-tuple
 * byte unit @p unitBytes (element size folded with the trailing component count) — rows are
 * never split across chunks.
 *
 * @param dims Dataset dimensions, slowest-varying first.
 * @param firstRowDim Index at which a row begins; dims [firstRowDim, dims.size()) run full.
 * @param unitBytes Bytes per tuple (elementByteSize * numComponents).
 * @param targetBytes Desired physical chunk size in bytes.
 * @param maxRows Upper clamp on the returned row count (the subdivided axis extent).
 * @return Row count in [1, maxRows].
 */
nx::core::usize computeRowsForByteTarget(const nx::core::ShapeType& dims, nx::core::usize firstRowDim, nx::core::usize unitBytes, nx::core::usize targetBytes, nx::core::usize maxRows)
{
  nx::core::usize rowBytes = unitBytes;
  for(nx::core::usize i = firstRowDim; i < dims.size(); ++i)
  {
    rowBytes *= dims[i];
  }
  nx::core::usize rows = (rowBytes == 0) ? 1 : targetBytes / rowBytes;
  rows = std::max<nx::core::usize>(rows, 1);
  return std::min(rows, maxRows);
}
} // namespace

namespace nx::core::HDF5
{

ShapeType computeChunkShape(const ShapeType& dims, usize numComponents, usize elementByteSize, const ChunkShapeOptions& opts)
{
  if(dims.empty())
  {
    return {};
  }

  // Bytes occupied by one tuple: the element size times the always-full trailing component
  // extent. Folding numComponents here lets a caller pass tuple-only dims and still have the
  // component bytes counted toward the target.
  const usize unitBytes = elementByteSize * numComponents;

  if(opts.regime == ChunkShapeRegime::PinSlowestDim)
  {
    // rank >= 3: pin the slowest dimension to 1 so one slice maps to a bounded chunk set,
    // and row-band the next dimension to the byte target. rank 1-2: row-band the slowest
    // dimension to the byte target. Dimensions interior to the banded one keep full extent.
    ShapeType chunk(dims);
    if(dims.size() >= 3)
    {
      chunk[0] = 1;
      chunk[1] = computeRowsForByteTarget(dims, /*firstRowDim=*/2, unitBytes, opts.targetBytes, dims[1]);
    }
    else
    {
      chunk[0] = computeRowsForByteTarget(dims, /*firstRowDim=*/1, unitBytes, opts.targetBytes, dims[0]);
    }
    return chunk;
  }

  // BundleOuterSlabs: greedy outermost-first walk. suffixBytes[i] is the byte cost of one
  // index step of dimension i, i.e. unitBytes * product(dims[i+1..]), computed innermost-first.
  ShapeType chunk(dims);
  ShapeType suffixBytes(dims.size());
  usize inner = unitBytes;
  for(usize i = dims.size(); i-- > 0;)
  {
    suffixBytes[i] = inner;
    inner *= dims[i];
  }
  // Any dimension whose interior alone meets the target is chunked to 1 (the target is
  // reachable further in). The first dimension whose interior fits under the target takes
  // however many of its rows fit, clamped to its extent, and every dimension inside it keeps
  // full extent. Bundling whole outer slabs falls out of this naturally: when an outer slab
  // is smaller than the target, multiple of them are taken into one chunk.
  for(usize i = 0; i < dims.size(); ++i)
  {
    if(suffixBytes[i] >= opts.targetBytes)
    {
      chunk[i] = 1;
      continue;
    }
    usize rows = (suffixBytes[i] == 0) ? 1 : opts.targetBytes / suffixBytes[i];
    rows = std::min<usize>(std::max<usize>(rows, 1), dims[i]);
    chunk[i] = rows;
    break;
  }
  return chunk;
}

} // namespace nx::core::HDF5
