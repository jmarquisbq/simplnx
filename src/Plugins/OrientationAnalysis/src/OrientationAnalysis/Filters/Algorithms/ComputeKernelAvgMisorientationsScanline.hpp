#pragma once

#include "OrientationAnalysis/OrientationAnalysis_export.hpp"

#include "simplnx/Common/Array.hpp"
#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/Filter/IFilter.hpp"
#include "simplnx/Parameters/VectorParameter.hpp"

namespace nx::core
{

struct ComputeKernelAvgMisorientationsInputValues;

struct ComputeKernelAvgMisorientationsWorkingSet
{
  uint64 CapBytes = 0;
  usize SliceTuples = 0;
  usize WindowSlices = 0;
  uint64 RollingBytes = 0;
  bool UseRollingWindow = false;
  usize BlockTuples = 1;
  usize CacheSlots = 1;
};

ORIENTATIONANALYSIS_EXPORT
Result<ComputeKernelAvgMisorientationsWorkingSet> CreateComputeKernelAvgMisorientationsWorkingSet(const SizeVec3& dimensions, const VectorInt32Parameter::ValueType& kernelSize,
                                                                                                  uint64 cacheBudgetBytes, uint64 cacheUsedBytes, const DataPath& imageGeometryPath);

/**
 * @class ComputeKernelAvgMisorientationsScanline
 * @brief Computes the Kernel Average Misorientation (KAM) for each voxel in
 *        an ImageGeom.
 *
 * For each voxel, the misorientation angle between the voxel and every
 * neighbor within the user-specified kernel is calculated (using
 * crystallographic symmetry operators). The average of these angles is
 * stored as the KAM value. Neighbors are admitted from the same Feature by
 * default, or from any positive Feature ID in the same phase when Use Feature
 * Ids is disabled.
 *
 * CacheMemoryBudgetManager caps the dominant array-buffer payload. When the cap
 * holds the clamped Z window, Feature ID, Cell Phase, and quaternion slices
 * roll through local buffers; workers read only those buffers and write
 * disjoint output ranges. Otherwise, the input-cache and output payload of a fixed
 * tuple-block LRU fit the cap; its fixed metadata and ensemble overhead are
 * independent of volume depth.
 */
class ORIENTATIONANALYSIS_EXPORT ComputeKernelAvgMisorientationsScanline
{
public:
  ComputeKernelAvgMisorientationsScanline(DataStructure& dataStructure, const IFilter::MessageHandler& msgHandler, const std::atomic_bool& shouldCancel,
                                          const ComputeKernelAvgMisorientationsInputValues* inputValues);
  ~ComputeKernelAvgMisorientationsScanline() noexcept;

  ComputeKernelAvgMisorientationsScanline(const ComputeKernelAvgMisorientationsScanline&) = delete;
  ComputeKernelAvgMisorientationsScanline(ComputeKernelAvgMisorientationsScanline&&) noexcept = delete;
  ComputeKernelAvgMisorientationsScanline& operator=(const ComputeKernelAvgMisorientationsScanline&) = delete;
  ComputeKernelAvgMisorientationsScanline& operator=(ComputeKernelAvgMisorientationsScanline&&) noexcept = delete;

  /**
   * @brief Executes the cache-budgeted scanline KAM computation.
   * @return Result<> with any errors encountered during execution.
   */
  Result<> operator()();

private:
  DataStructure& m_DataStructure;
  const ComputeKernelAvgMisorientationsInputValues* m_InputValues = nullptr;
  const std::atomic_bool& m_ShouldCancel;
  const IFilter::MessageHandler& m_MessageHandler;
};

} // namespace nx::core
