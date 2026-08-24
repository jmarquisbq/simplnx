#pragma once

#include "SimplnxCore/SimplnxCore_export.hpp"

#include "simplnx/DataStructure/DataPath.hpp"
#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/Filter/IFilter.hpp"

namespace nx::core
{

struct SIMPLNXCORE_EXPORT InterpolatePointCloudToRegularGridInputValues
{
  bool useMask;
  uint64 interpolationTechnique;
  DataPath vertexGeomPath;
  DataPath imageGeomPath;
  std::vector<DataPath> interpolatedDataPaths;
  std::vector<DataPath> copyDataPaths;
  std::vector<float32> kernelSize;
  std::vector<float32> sigmas;
  DataPath maskDataPath;

  // Statistics flags
  bool findLength;
  bool findMin;
  bool findMax;
  bool findMean;
  bool findStdDeviation;
  bool findSummation;

  // Output suffix names (appended to source array name)
  std::string lengthSuffix;
  std::string minSuffix;
  std::string maxSuffix;
  std::string meanSuffix;
  std::string stdDeviationSuffix;
  std::string summationSuffix;
};

/**
 * @class InterpolatePointCloudToRegularGrid
 * @brief Accumulates point values onto a regular ImageGeom using a uniform or
 * Gaussian kernel and optionally emits per-voxel statistics.
 *
 * Voxel-scale accumulator arrays use temporary record stores with bounded page
 * caches whenever output dispatch is out-of-core. This avoids multiplying the
 * resident footprint by the number of requested source arrays and statistics.
 */
class SIMPLNXCORE_EXPORT InterpolatePointCloudToRegularGrid
{
public:
  /** @brief Binds filter-owned data, options, progress, and cancellation for synchronous execution. */
  InterpolatePointCloudToRegularGrid(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel,
                                     InterpolatePointCloudToRegularGridInputValues* inputValues);
  ~InterpolatePointCloudToRegularGrid() noexcept;

  InterpolatePointCloudToRegularGrid(const InterpolatePointCloudToRegularGrid&) = delete;
  InterpolatePointCloudToRegularGrid(InterpolatePointCloudToRegularGrid&&) noexcept = delete;
  InterpolatePointCloudToRegularGrid& operator=(const InterpolatePointCloudToRegularGrid&) = delete;
  InterpolatePointCloudToRegularGrid& operator=(InterpolatePointCloudToRegularGrid&&) noexcept = delete;

  /** @brief Builds the kernel, selects accumulator storage, accumulates points, and writes outputs. */
  Result<> operator()();

  /** @brief Returns the filter-owned cancellation flag used by long-running loops. */
  const std::atomic_bool& getCancel();

  static constexpr uint64 k_Uniform = 0;
  static constexpr uint64 k_Gaussian = 1;

private:
  DataStructure& m_DataStructure;
  const InterpolatePointCloudToRegularGridInputValues* m_InputValues = nullptr;
  const std::atomic_bool& m_ShouldCancel;
  const IFilter::MessageHandler& m_MessageHandler;
};

} // namespace nx::core
