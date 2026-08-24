#pragma once

#include "simplnx/Common/Aliases.hpp"
#include "simplnx/Common/Types.hpp"
#include "simplnx/simplnx_export.hpp"

namespace nx::core::HDF5
{

/**
 * @brief Physical chunk byte target shared by every byte-targeted chunk-shape caller.
 * A chunk this size stays inside HDF5's chunk cache, inflates/deflates as one parallel
 * work unit, and keeps the per-chunk index overhead negligible against the payload.
 */
inline constexpr uint64 k_TargetChunkBytes = 1ull << 20; // 1 MiB

/**
 * @brief Below this total dataset size a caller may store the dataset contiguously:
 * the per-chunk index overhead would dominate any compression or streaming benefit.
 * This is a contiguous-vs-chunked decision the caller makes; computeChunkShape itself
 * always returns a shape and does not consult this threshold.
 */
inline constexpr uint64 k_SmallArrayThresholdBytes = 16ull * 1024ull; // 16 KiB

/**
 * @brief Selects how the outermost (slowest-varying) dimension is chunked. The two
 * behaviors are mutually exclusive, so an enum makes the invalid "both/neither" state
 * unrepresentable.
 */
enum class ChunkShapeRegime
{
  /**
   * @brief Write-once output datasets: a greedy outermost-first walk. Any dimension
   * whose interior alone meets the byte target is chunked to 1; the first dimension
   * whose interior fits under the target takes however many of its rows fit, and every
   * dimension inside it keeps full extent. Because the output is written once and need
   * not stay slice-aligned, this bundles multiple whole outer slabs into one chunk when
   * they fit the target (fewer, larger chunks compress better).
   */
  BundleOuterSlabs,

  /**
   * @brief Session read-modify-write datasets: for rank >= 3 the slowest dimension is
   * pinned to 1 so a single slice maps to a bounded set of chunks (slice-aligned RMW and
   * slice reads never over-read a neighbor), and the next dimension is row-banded to the
   * byte target. For rank 1-2 the slowest dimension is row-banded to the byte target.
   */
  PinSlowestDim
};

/**
 * @brief Tuning inputs for computeChunkShape. Defaults match the shared 1 MiB target and
 * the write-once regime; the session store overrides the regime to PinSlowestDim.
 */
struct ChunkShapeOptions
{
  uint64 targetBytes = k_TargetChunkBytes;
  ChunkShapeRegime regime = ChunkShapeRegime::BundleOuterSlabs;
};

/**
 * @brief Computes one byte-targeted row-band chunk shape for a dataset.
 *
 * @p dims is given slowest-varying first. @p numComponents folds the (always-full)
 * trailing component extent into the per-tuple byte unit: a caller passing the full HDF5
 * dataspace dims (tuple dims ++ component dims) uses numComponents = 1, while a caller
 * passing only the tuple-space dims passes product(componentShape) so the component bytes
 * still count toward the target. The returned shape has the same rank as @p dims (empty
 * in yields empty out).
 *
 * The two outer-dimension behaviors are documented on ChunkShapeRegime. This computes only
 * the chunk SHAPE; the contiguous-vs-chunked decision (k_SmallArrayThresholdBytes) stays
 * with the caller.
 *
 * @param dims Dataset dimensions, slowest-varying first.
 * @param numComponents Trailing full-extent component count folded into the per-tuple byte
 *        unit (1 when @p dims already includes the component dimensions).
 * @param elementByteSize sizeof(T) for the dataset element type.
 * @param opts Byte target and outer-dimension regime.
 * @return A chunk shape the same rank as @p dims.
 */
SIMPLNX_EXPORT nx::core::ShapeType computeChunkShape(const nx::core::ShapeType& dims, usize numComponents, usize elementByteSize, const ChunkShapeOptions& opts);

} // namespace nx::core::HDF5
