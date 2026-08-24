#pragma once

#include "SimplnxCore/SimplnxCore_export.hpp"

#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/Filter/IFilter.hpp"

namespace nx::core
{
struct ComputeFeatureBoundsInputValues;

/**
 * @class ComputeFeatureBoundsScanline
 * @brief Computes feature bounds by streaming cell-level Feature Ids through a
 * fixed-size buffer for bounded-memory out-of-core execution.
 */
class SIMPLNXCORE_EXPORT ComputeFeatureBoundsScanline
{
public:
  ComputeFeatureBoundsScanline(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel, const ComputeFeatureBoundsInputValues* inputValues);
  ~ComputeFeatureBoundsScanline() noexcept;

  ComputeFeatureBoundsScanline(const ComputeFeatureBoundsScanline&) = delete;
  ComputeFeatureBoundsScanline(ComputeFeatureBoundsScanline&&) noexcept = delete;
  ComputeFeatureBoundsScanline& operator=(const ComputeFeatureBoundsScanline&) = delete;
  ComputeFeatureBoundsScanline& operator=(ComputeFeatureBoundsScanline&&) noexcept = delete;

  Result<> operator()();

private:
  DataStructure& m_DataStructure;
  const ComputeFeatureBoundsInputValues* m_InputValues = nullptr;
  const std::atomic_bool& m_ShouldCancel;
  const IFilter::MessageHandler& m_MessageHandler;
};
} // namespace nx::core
