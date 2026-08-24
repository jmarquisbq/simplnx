#pragma once

#include "SimplnxCore/SimplnxCore_export.hpp"

#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/Filter/IFilter.hpp"

namespace nx::core
{
struct ComputeBoundingBoxStatsInputValues;

/**
 * @class ComputeBoundingBoxStatsScanline
 * @brief Computes exact bounding-box statistics with bounded bulk I/O.
 *
 * Contiguous row reads and external merge sorting preserve exact statistics without per-voxel
 * store access or scratch memory proportional to the number of cells.
 */
class SIMPLNXCORE_EXPORT ComputeBoundingBoxStatsScanline
{
public:
  ComputeBoundingBoxStatsScanline(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel,
                                  const ComputeBoundingBoxStatsInputValues* inputValues);
  ~ComputeBoundingBoxStatsScanline() noexcept;

  ComputeBoundingBoxStatsScanline(const ComputeBoundingBoxStatsScanline&) = delete;
  ComputeBoundingBoxStatsScanline(ComputeBoundingBoxStatsScanline&&) noexcept = delete;
  ComputeBoundingBoxStatsScanline& operator=(const ComputeBoundingBoxStatsScanline&) = delete;
  ComputeBoundingBoxStatsScanline& operator=(ComputeBoundingBoxStatsScanline&&) noexcept = delete;

  Result<> operator()();

private:
  DataStructure& m_DataStructure;
  const ComputeBoundingBoxStatsInputValues* m_InputValues = nullptr;
  const std::atomic_bool& m_ShouldCancel;
  const IFilter::MessageHandler& m_MessageHandler;
};
} // namespace nx::core
