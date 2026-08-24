#pragma once

#include "OrientationAnalysis/OrientationAnalysis_export.hpp"

#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/Filter/IFilter.hpp"

#include <atomic>

namespace nx::core
{

struct ComputeKernelAvgMisorientationsInputValues;

class ORIENTATIONANALYSIS_EXPORT ComputeKernelAvgMisorientationsDirect
{
public:
  ComputeKernelAvgMisorientationsDirect(DataStructure& dataStructure, const IFilter::MessageHandler& msgHandler, const std::atomic_bool& shouldCancel,
                                        const ComputeKernelAvgMisorientationsInputValues* inputValues);
  ~ComputeKernelAvgMisorientationsDirect() noexcept;

  ComputeKernelAvgMisorientationsDirect(const ComputeKernelAvgMisorientationsDirect&) = delete;
  ComputeKernelAvgMisorientationsDirect(ComputeKernelAvgMisorientationsDirect&&) noexcept = delete;
  ComputeKernelAvgMisorientationsDirect& operator=(const ComputeKernelAvgMisorientationsDirect&) = delete;
  ComputeKernelAvgMisorientationsDirect& operator=(ComputeKernelAvgMisorientationsDirect&&) noexcept = delete;

  Result<> operator()();

private:
  DataStructure& m_DataStructure;
  const ComputeKernelAvgMisorientationsInputValues* m_InputValues = nullptr;
  const std::atomic_bool& m_ShouldCancel;
  const IFilter::MessageHandler& m_MessageHandler;
};

} // namespace nx::core
