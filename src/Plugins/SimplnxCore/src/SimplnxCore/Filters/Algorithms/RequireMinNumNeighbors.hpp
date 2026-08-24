#pragma once

#include "SimplnxCore/SimplnxCore_export.hpp"

#include "simplnx/Common/Array.hpp"
#include "simplnx/DataStructure/AbstractDataStore.hpp"
#include "simplnx/DataStructure/DataPath.hpp"
#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/Filter/IFilter.hpp"
#include "simplnx/Parameters/MultiArraySelectionParameter.hpp"

namespace nx::core
{

struct SIMPLNXCORE_EXPORT RequireMinNumNeighborsInputValues
{
  bool ApplyToSinglePhase;
  DataPath FeaturePhasesPath;
  uint64 PhaseNumber;
  uint64 MinNumNeighbors;
  DataPath ImageGeomPath;
  DataPath FeatureIdsPath;
  DataPath NumNeighborsPath;
  MultiArraySelectionParameter::ValueType IgnoredVoxelArrayPaths;
};

/**
 * @class RequireMinNumNeighbors
 * @brief Removes features that have fewer than a user-specified number of contiguous neighboring
 * features, then iteratively fills the resulting gaps by majority-voting among face-neighbor
 * feature IDs.
 *
 * @section ooc_optimization Out-of-Core Optimization
 * Two operations were converted to out-of-core-friendly bulk I/O:
 *
 * **removeFeaturesUnderNeighborThreshold()**: The original per-element setValue(-1) loop for marking
 * removed features triggered a chunk operation per voxel. The optimized version reads FeatureIds in
 * 64K-tuple chunks via copyIntoBuffer(), rewrites the buffer in place, and writes back only modified
 * chunks via copyFromBuffer(). The same pass also fuses the feature-id renumber: surviving voxels are
 * remapped to their compacted id (via the shared ComputeFeatureRenumbering mapping) and removed voxels
 * are set to -1. Because this pass already remaps FeatureIds, operator() passes
 * cellFeatureIdsRenumbered=true to RemoveInactiveObjects, eliminating its separate full-volume
 * renumber read+write.
 *
 * **assignBadVoxels()**: The original per-element getValue() voting loop caused chunk thrashing across
 * the whole volume and used a cell-count-wide neighbor-index vector. The optimized shared implementation
 * streams FeatureIds and one target array at a time through rolling three-slice windows, performs only
 * bulk slice I/O, and updates FeatureIds last to preserve synchronous-iteration semantics. Resident
 * scratch is O(X*Y * largest tuple width), with no cell-count-wide mappings or changed-voxel lists.
 */
class SIMPLNXCORE_EXPORT RequireMinNumNeighbors
{
public:
  RequireMinNumNeighbors(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel, RequireMinNumNeighborsInputValues* inputValues);
  ~RequireMinNumNeighbors() noexcept;

  RequireMinNumNeighbors(const RequireMinNumNeighbors&) = delete;
  RequireMinNumNeighbors(RequireMinNumNeighbors&&) noexcept = delete;
  RequireMinNumNeighbors& operator=(const RequireMinNumNeighbors&) = delete;
  RequireMinNumNeighbors& operator=(RequireMinNumNeighbors&&) noexcept = delete;

  Result<> operator()();

private:
  /**
   * @brief Builds the per-feature active flags (a feature is removed when it has fewer than
   * MinNumNeighbors neighbors, honoring the single-phase option) and, in a single chunked bulk-I/O
   * pass, marks removed features' voxels as -1 while renumbering surviving voxels to their compacted
   * feature id. The renumber mapping comes from the shared ComputeFeatureRenumbering helper so it
   * matches the feature-array compaction RemoveInactiveObjects performs.
   * @param featureIds Per-cell Feature ID DataStore (modified in place: marked and renumbered).
   * @param numNeighbors Per-feature neighbor-count array.
   * @param totalPoints Number of cells in the image geometry.
   * @param errorReturn Output: receives error details if every feature would be removed.
   * @return Vector of booleans indicating which features remain active (indexed by original feature id).
   */
  std::vector<bool> removeFeaturesUnderNeighborThreshold(Int32AbstractDataStore& featureIds, const Int32AbstractDataStore& numNeighbors, usize totalPoints, Error& errorReturn);

  /**
   * @brief Iteratively fills voxels belonging to removed features (featureId < 0) by majority-voting
   * among their 6 face-neighbors, then transferring the chosen neighbor's tuple into the bad voxel for
   * every (non-ignored) cell-level array. Uses rolling three-slice buffers and bulk I/O; no allocation
   * scales with the cell count.
   * @param dimensions XYZ dimensions of the ImageGeom.
   * @param totalFeatures Original per-feature count used by the feature-id sanity check.
   * @return Result<> carrying the -55567 error if a feature id greater than or equal to totalFeatures is found.
   */
  Result<> assignBadVoxels(SizeVec3 dimensions, usize totalFeatures);

  DataStructure& m_DataStructure;
  const RequireMinNumNeighborsInputValues* m_InputValues = nullptr;
  const std::atomic_bool& m_ShouldCancel;
  const IFilter::MessageHandler& m_MessageHandler;
};

} // namespace nx::core
