#pragma once

#include "SimplnxCore/SimplnxCore_export.hpp"

#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/Filter/IFilter.hpp"

namespace nx::core
{
struct ComputeFeatureBoundsInputValues;

/**
 * @class ComputeFeatureBoundsDirect
 * @brief Computes feature bounds using contiguous datastore access and feature-sized
 * index extrema for in-memory image inputs.
 */
class SIMPLNXCORE_EXPORT ComputeFeatureBoundsDirect
{
public:
  ComputeFeatureBoundsDirect(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel, const ComputeFeatureBoundsInputValues* inputValues);
  ~ComputeFeatureBoundsDirect() noexcept;

  ComputeFeatureBoundsDirect(const ComputeFeatureBoundsDirect&) = delete;
  ComputeFeatureBoundsDirect(ComputeFeatureBoundsDirect&&) noexcept = delete;
  ComputeFeatureBoundsDirect& operator=(const ComputeFeatureBoundsDirect&) = delete;
  ComputeFeatureBoundsDirect& operator=(ComputeFeatureBoundsDirect&&) noexcept = delete;

  Result<> operator()();

private:
  DataStructure& m_DataStructure;
  const ComputeFeatureBoundsInputValues* m_InputValues = nullptr;
  const std::atomic_bool& m_ShouldCancel;
  const IFilter::MessageHandler& m_MessageHandler;
};
} // namespace nx::core
