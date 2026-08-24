#pragma once

#include "SimplnxCore/SimplnxCore_export.hpp"

#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/Filter/IFilter.hpp"

namespace nx::core
{
struct RemoveFlaggedFeaturesInputValues;

/**
 * @class RemoveFlaggedFeaturesScanline
 * @brief Out-of-core (OOC) optimized algorithm for removing/extracting flagged Features
 * using Z-slice rolling-window bulk I/O.
 *
 * **The problem this solves**: The Direct algorithm's "fill removed features" loop scans
 * the full volume once per outer iteration to vote on a replacement Feature for every
 * removed voxel (IdentifyNeighbors), then scans it again to copy the winning neighbor's
 * data into place (FindVoxelArrays). Both passes use operator[] with a 6-face-neighbor
 * stencil whose +/-Z offset is a full Z-slice away. When FeatureIds is stored out-of-core
 * in chunked format, this triggers a chunk load/evict cycle for nearly every voxel access,
 * repeated for every do-while iteration until every removed voxel is resolved.
 *
 * **How the rolling window solves it**: This variant fuses the vote and data-transfer
 * passes into a single Z-slice sweep, entirely with sequential, chunk-aligned bulk I/O:
 *
 *   - FeatureIds votes are read one Z-slice at a time via a 3-slice rolling window
 *     (prevSlice/curSlice/nextSlice), exactly mirroring ComputeBoundaryCellsScanline's
 *     approach. This window is advanced purely in memory (std::swap) plus one disk read
 *     per Z-slice for the newly-needed "ahead" slice; a slice already visited is never
 *     re-read from disk, so every vote sees the FeatureIds state as it was at the start
 *     of the current do-while iteration, never a value written earlier in the same
 *     iteration.
 *   - Because every vote in this algorithm targets the *current* voxel's own slot (never
 *     a neighbor's slot, unlike ErodeDilateBadData's dilate mode), a slice's marks are
 *     fully resolved the instant that slice's XY scan completes. The data-transfer
 *     (commit) for a slice is deferred by exactly one Z-slice -- writing strictly behind
 *     the read frontier -- and reuses the shared SliceBufferedTransferOneZ utility (see
 *     simplnx/Utilities/SliceBufferedTransfer.hpp) to apply the marks to every kept cell
 *     array.
 *
 * **Memory footprint**: the per-slice marks (curMarks/prevMarks) are two
 * `std::vector<int64>` sized to one Z-slice (O(sliceSize), 16 bytes/voxel-column). No
 * allocation in this algorithm scales with the total voxel count. All FeatureIds/
 * companion array reads and writes remain sequential, chunk-aligned bulk I/O.
 *
 * @see RemoveFlaggedFeaturesDirect for the in-core variant.
 * @see RemoveFlaggedFeatures for the dispatcher.
 * @see DispatchAlgorithm for the selection mechanism.
 */
class SIMPLNXCORE_EXPORT RemoveFlaggedFeaturesScanline
{
public:
  RemoveFlaggedFeaturesScanline(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel, const RemoveFlaggedFeaturesInputValues* inputValues);
  ~RemoveFlaggedFeaturesScanline() noexcept;

  RemoveFlaggedFeaturesScanline(const RemoveFlaggedFeaturesScanline&) = delete;
  RemoveFlaggedFeaturesScanline(RemoveFlaggedFeaturesScanline&&) noexcept = delete;
  RemoveFlaggedFeaturesScanline& operator=(const RemoveFlaggedFeaturesScanline&) = delete;
  RemoveFlaggedFeaturesScanline& operator=(RemoveFlaggedFeaturesScanline&&) noexcept = delete;

  /**
   * @brief Executes the OOC-optimized remove/extract flagged features algorithm.
   * @return Result<> indicating success or errors.
   */
  Result<> operator()();

private:
  DataStructure& m_DataStructure;
  const RemoveFlaggedFeaturesInputValues* m_InputValues = nullptr;
  const std::atomic_bool& m_ShouldCancel;
  const IFilter::MessageHandler& m_MessageHandler;
};

} // namespace nx::core
