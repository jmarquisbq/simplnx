#pragma once

#include "SimplnxCore/SimplnxCore_export.hpp"

#include "simplnx/DataStructure/DataPath.hpp"
#include "simplnx/Filter/IFilter.hpp"
#include "simplnx/Parameters/ArraySelectionParameter.hpp"
#include "simplnx/Parameters/BoolParameter.hpp"
#include "simplnx/Parameters/GeometrySelectionParameter.hpp"
#include "simplnx/Parameters/NumberParameter.hpp"

namespace nx::core
{

/**
 * @struct RequireMinimumSizeFeaturesInputValues
 * @brief Holds all user-configured parameters for the RequireMinimumSizeFeatures algorithm.
 */
struct SIMPLNXCORE_EXPORT RequireMinimumSizeFeaturesInputValues
{
  BoolParameter::ValueType ApplySinglePhase;                    ///< If true, only remove small features in one phase.
  ArraySelectionParameter::ValueType FeatureIdsPath;            ///< Per-cell Feature ID array (int32).
  ArraySelectionParameter::ValueType FeaturePhasesPath;         ///< Per-feature phase array (for single-phase mode).
  GeometrySelectionParameter::ValueType InputImageGeometryPath; ///< Input ImageGeom.
  Int64Parameter::ValueType MinAllowedFeaturesSize;             ///< Minimum voxel count threshold.
  ArraySelectionParameter::ValueType FeatureNumCellsPath;       ///< Per-feature voxel count array.
  Int32Parameter::ValueType PhaseNumber;                        ///< Phase to filter (when ApplySinglePhase is true).
};

/**
 * @class RequireMinimumSizeFeatures
 * @brief Removes features with fewer voxels than a user-specified minimum threshold,
 * then iteratively fills the resulting gaps by voting among face-neighbor feature IDs.
 *
 * @section ooc_optimization Out-of-Core Optimization
 * Two operations were optimized:
 *
 * **removeSmallFeatures()**: The original per-element setValue(-1) loop for marking
 * removed features caused a chunk operation per voxel. The optimized version reads
 * FeatureIds in 64K-tuple chunks via copyIntoBuffer(), modifies the buffer in-place,
 * and writes back only modified chunks via copyFromBuffer().
 *
 * **assignBadVoxels()**: The original per-element getValue() voting loop caused chunk
 * thrashing across the entire volume. The optimized shared implementation streams
 * FeatureIds and one target array at a time through rolling three-slice windows, performs
 * only bulk slice I/O, and updates FeatureIds last to preserve synchronous-iteration
 * semantics. Resident scratch is O(X*Y * largest tuple width); there are no cell-count-wide
 * mappings, changed-voxel lists, or transfer slabs for multiple arrays at once.
 */
class SIMPLNXCORE_EXPORT RequireMinimumSizeFeatures
{
public:
  RequireMinimumSizeFeatures(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel, RequireMinimumSizeFeaturesInputValues* inputValues);
  ~RequireMinimumSizeFeatures() noexcept;

  RequireMinimumSizeFeatures(const RequireMinimumSizeFeatures&) = delete;
  RequireMinimumSizeFeatures(RequireMinimumSizeFeatures&&) noexcept = delete;
  RequireMinimumSizeFeatures& operator=(const RequireMinimumSizeFeatures&) = delete;
  RequireMinimumSizeFeatures& operator=(RequireMinimumSizeFeatures&&) noexcept = delete;

  /**
   * @brief Executes the minimum-size filter: removes small features, then fills gaps.
   * @return Result<> indicating success or error.
   */
  Result<> operator()();

protected:
  /**
   * @brief Iteratively fills voxels belonging to removed features (featureId < 0)
   * by voting among their 6 face-neighbors. Uses the shared rolling-slice bulk-I/O path.
   * @param dimensions XYZ dimensions of the ImageGeom.
   * @return Result<> indicating success or an I/O error.
   */
  Result<> assignBadVoxels(SizeVec3 dimensions);

  /**
   * @brief Marks features below the minimum size as inactive and sets their voxels'
   * Feature IDs to -1 using chunked bulk I/O.
   * @param featureIdsStoreRef Per-cell Feature ID DataStore (modified in-place).
   * @param featureNumCellsStoreRef Per-feature voxel count array.
   * @param featurePhases Per-feature phase array (may be nullptr).
   * @param phaseNumber Target phase number (when applyToSinglePhase is true).
   * @param applyToSinglePhase If true, only remove features in the specified phase.
   * @param minAllowedFeatureSize Minimum voxel count threshold.
   * @param errorReturn Output: receives error details if all features would be removed.
   * @return Vector of booleans indicating which features remain active.
   */
  std::vector<bool> removeSmallFeatures(Int32AbstractDataStore& featureIdsStoreRef, const Int32AbstractDataStore& featureNumCellsStoreRef, const Int32AbstractDataStore* featurePhases,
                                        int32_t phaseNumber, bool applyToSinglePhase, int64 minAllowedFeatureSize, Error& errorReturn);

private:
  DataStructure& m_DataStructure;                                       ///< Reference to the DataStructure.
  const RequireMinimumSizeFeaturesInputValues* m_InputValues = nullptr; ///< User-configured parameters.
  const std::atomic_bool& m_ShouldCancel;                               ///< Cancellation flag.
  const IFilter::MessageHandler& m_MessageHandler;                      ///< Message handler for progress.
};

} // namespace nx::core
