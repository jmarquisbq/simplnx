#pragma once

#include "SimplnxCore/SimplnxCore_export.hpp"

#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/Filter/IFilter.hpp"

namespace nx::core
{
struct ComputeEuclideanDistMapInputValues;

/**
 * @class ComputeEuclideanDistMapScanline
 * @brief Bounded-memory distance-map implementation for out-of-core ImageGeom data.
 *
 * FeatureIds and output arrays are accessed only through full Z-slice bulk transfers.
 * Obstacle-free volumes use exact forward/backward city-block sweeps. Volumes that
 * contain non-positive FeatureIds use an exact layer-synchronous propagation fallback
 * so invalid cells remain non-traversable, matching the direct implementation.
 *
 * Resident scratch is O(X * Y). Euclidean output additionally uses one reusable
 * cell-level nearest-seed DataStore, which follows the active storage policy and is
 * therefore disk-backed when the filter operates out of core.
 */
class SIMPLNXCORE_EXPORT ComputeEuclideanDistMapScanline
{
public:
  ComputeEuclideanDistMapScanline(DataStructure& dataStructure, const IFilter::MessageHandler& messageHandler, const std::atomic_bool& shouldCancel,
                                  const ComputeEuclideanDistMapInputValues* inputValues);
  ~ComputeEuclideanDistMapScanline() noexcept;

  ComputeEuclideanDistMapScanline(const ComputeEuclideanDistMapScanline&) = delete;
  ComputeEuclideanDistMapScanline(ComputeEuclideanDistMapScanline&&) noexcept = delete;
  ComputeEuclideanDistMapScanline& operator=(const ComputeEuclideanDistMapScanline&) = delete;
  ComputeEuclideanDistMapScanline& operator=(ComputeEuclideanDistMapScanline&&) noexcept = delete;

  Result<> operator()();

private:
  DataStructure& m_DataStructure;
  const ComputeEuclideanDistMapInputValues* m_InputValues = nullptr;
  const std::atomic_bool& m_ShouldCancel;
};

} // namespace nx::core
