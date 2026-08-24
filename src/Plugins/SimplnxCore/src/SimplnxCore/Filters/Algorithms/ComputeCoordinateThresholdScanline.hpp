#pragma once

#include "SimplnxCore/SimplnxCore_export.hpp"

#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/Filter/IFilter.hpp"

namespace nx::core
{
struct ComputeCoordinateThresholdInputValues;

/**
 * @class ComputeCoordinateThresholdScanline
 * @brief Computes ImageGeom masks with bounded buffers and bulk datastore writes.
 * @details Avoids per-cell disk access and keeps cell-level scratch independent of the image size.
 */
class SIMPLNXCORE_EXPORT ComputeCoordinateThresholdScanline
{
public:
  ComputeCoordinateThresholdScanline(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel,
                                     const ComputeCoordinateThresholdInputValues* inputValues);
  ~ComputeCoordinateThresholdScanline() noexcept;

  ComputeCoordinateThresholdScanline(const ComputeCoordinateThresholdScanline&) = delete;
  ComputeCoordinateThresholdScanline(ComputeCoordinateThresholdScanline&&) noexcept = delete;
  ComputeCoordinateThresholdScanline& operator=(const ComputeCoordinateThresholdScanline&) = delete;
  ComputeCoordinateThresholdScanline& operator=(ComputeCoordinateThresholdScanline&&) noexcept = delete;

  Result<> operator()();

private:
  DataStructure& m_DataStructure;
  const ComputeCoordinateThresholdInputValues* m_InputValues = nullptr;
  const std::atomic_bool& m_ShouldCancel;
  const IFilter::MessageHandler& m_MessageHandler;
};
} // namespace nx::core
