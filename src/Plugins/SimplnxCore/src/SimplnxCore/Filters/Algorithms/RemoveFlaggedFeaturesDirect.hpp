#pragma once

#include "SimplnxCore/SimplnxCore_export.hpp"

#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/Filter/IFilter.hpp"

namespace nx::core
{
struct RemoveFlaggedFeaturesInputValues;

/**
 * @class RemoveFlaggedFeaturesDirect
 * @brief In-core (direct memory access) algorithm for removing/extracting flagged Features.
 *
 * This is the original algorithm that reads and writes the FeatureIds array (and any
 * companion cell-level arrays) through operator[] on the DataStore. It repeatedly scans
 * the full volume with a 6-face-neighbor stencil (IdentifyNeighbors) and then propagates
 * the winning neighbor's data into every removed voxel (FindVoxelArrays), looping until no
 * removed voxel remains unresolved.
 *
 * **When this variant is selected**: DispatchAlgorithm selects this class when the
 * FeatureIds array is backed by contiguous in-memory DataStore (i.e., not chunked/OOC).
 * With in-memory data, operator[] is a simple pointer dereference, so the repeated
 * full-volume, random-neighbor-offset scans are inexpensive.
 *
 * **Why a separate OOC variant exists**: When FeatureIds is stored out-of-core in chunked
 * format, every operator[] call may trigger a chunk load from disk, and the +/-Z neighbor
 * offset (a full Z-slice away) makes this especially costly. RemoveFlaggedFeaturesScanline
 * avoids this by reading/writing Z-slices sequentially via copyIntoBuffer/copyFromBuffer.
 *
 * @see RemoveFlaggedFeaturesScanline for the OOC-optimized variant.
 * @see RemoveFlaggedFeatures for the dispatcher.
 */
class SIMPLNXCORE_EXPORT RemoveFlaggedFeaturesDirect
{
public:
  RemoveFlaggedFeaturesDirect(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel, const RemoveFlaggedFeaturesInputValues* inputValues);
  ~RemoveFlaggedFeaturesDirect() noexcept;

  RemoveFlaggedFeaturesDirect(const RemoveFlaggedFeaturesDirect&) = delete;
  RemoveFlaggedFeaturesDirect(RemoveFlaggedFeaturesDirect&&) noexcept = delete;
  RemoveFlaggedFeaturesDirect& operator=(const RemoveFlaggedFeaturesDirect&) = delete;
  RemoveFlaggedFeaturesDirect& operator=(RemoveFlaggedFeaturesDirect&&) noexcept = delete;

  /**
   * @brief Executes the in-core remove/extract flagged features algorithm.
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
