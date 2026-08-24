#pragma once

#include "SimplnxCore/SimplnxCore_export.hpp"

#include "simplnx/Common/Array.hpp"
#include "simplnx/Common/Result.hpp"
#include "simplnx/DataStructure/DataPath.hpp"
#include "simplnx/Filter/IFilter.hpp"

#include <atomic>
#include <optional>
#include <vector>

namespace nx::core
{
class DataStructure;

/**
 * @brief Iteratively fills negative FeatureIds by copying the tuple of the face-neighbor whose
 * non-negative FeatureId has the largest local vote count.
 *
 * The implementation processes one cell array at a time with rolling three-slice input windows.
 * FeatureIds are updated last, so every array observes the same FeatureIds snapshot during an
 * iteration. Resident scratch is O(X*Y) and no allocation scales with the number of cells.
 *
 * @param dataStructure DataStructure containing the cell arrays.
 * @param featureIdsPath Path to the int32 cell FeatureIds array.
 * @param dimensions XYZ dimensions of the owning ImageGeom.
 * @param ignoredArrayPaths Cell arrays that must not be transferred.
 * @param maxFeatureCount Optional upper bound used to validate non-negative FeatureIds.
 * @param messageHandler Filter progress-message handler.
 * @param shouldCancel Filter cancellation flag.
 * @return A valid result on completion or cancellation; error -55567 if a FeatureId is outside
 * maxFeatureCount when that validation is requested, or -55572 if unresolved cells remain and
 * none has a non-negative face neighbor.
 */
SIMPLNXCORE_EXPORT Result<> FillBadVoxels(DataStructure& dataStructure, const DataPath& featureIdsPath, const SizeVec3& dimensions, const std::vector<DataPath>& ignoredArrayPaths,
                                          std::optional<usize> maxFeatureCount, const IFilter::MessageHandler& messageHandler, const std::atomic_bool& shouldCancel);

} // namespace nx::core
