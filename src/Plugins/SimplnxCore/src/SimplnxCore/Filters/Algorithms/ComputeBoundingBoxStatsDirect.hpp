#pragma once

#include "SimplnxCore/SimplnxCore_export.hpp"

#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/Filter/IFilter.hpp"

namespace nx::core
{
struct ComputeBoundingBoxStatsInputValues;

/**
 * @class ComputeBoundingBoxStatsDirect
 * @brief Computes exact bounding-box statistics with direct in-memory access.
 *
 * This preserves the original parallel implementation because direct DataStore access is fastest
 * for contiguous in-core arrays; out-of-core arrays use ComputeBoundingBoxStatsScanline instead.
 */
class SIMPLNXCORE_EXPORT ComputeBoundingBoxStatsDirect
{
public:
  ComputeBoundingBoxStatsDirect(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel, const ComputeBoundingBoxStatsInputValues* inputValues);
  ~ComputeBoundingBoxStatsDirect() noexcept;

  ComputeBoundingBoxStatsDirect(const ComputeBoundingBoxStatsDirect&) = delete;
  ComputeBoundingBoxStatsDirect(ComputeBoundingBoxStatsDirect&&) noexcept = delete;
  ComputeBoundingBoxStatsDirect& operator=(const ComputeBoundingBoxStatsDirect&) = delete;
  ComputeBoundingBoxStatsDirect& operator=(ComputeBoundingBoxStatsDirect&&) noexcept = delete;

  Result<> operator()();

private:
  DataStructure& m_DataStructure;
  const ComputeBoundingBoxStatsInputValues* m_InputValues = nullptr;
  const std::atomic_bool& m_ShouldCancel;
  const IFilter::MessageHandler& m_MessageHandler;
};
} // namespace nx::core
