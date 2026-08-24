#pragma once

#include "SimplnxCore/SimplnxCore_export.hpp"

#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/Filter/IFilter.hpp"

namespace nx::core
{
struct ComputeCoordinateThresholdInputValues;

/**
 * @class ComputeCoordinateThresholdDirect
 * @brief Computes ImageGeom masks directly in contiguous memory.
 * @details Avoids scanline staging and virtual predicate calls so in-memory execution does not pay for out-of-core safeguards.
 */
class SIMPLNXCORE_EXPORT ComputeCoordinateThresholdDirect
{
public:
  ComputeCoordinateThresholdDirect(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel,
                                   const ComputeCoordinateThresholdInputValues* inputValues);
  ~ComputeCoordinateThresholdDirect() noexcept;

  ComputeCoordinateThresholdDirect(const ComputeCoordinateThresholdDirect&) = delete;
  ComputeCoordinateThresholdDirect(ComputeCoordinateThresholdDirect&&) noexcept = delete;
  ComputeCoordinateThresholdDirect& operator=(const ComputeCoordinateThresholdDirect&) = delete;
  ComputeCoordinateThresholdDirect& operator=(ComputeCoordinateThresholdDirect&&) noexcept = delete;

  Result<> operator()();

private:
  DataStructure& m_DataStructure;
  const ComputeCoordinateThresholdInputValues* m_InputValues = nullptr;
  const std::atomic_bool& m_ShouldCancel;
  const IFilter::MessageHandler& m_MessageHandler;
};
} // namespace nx::core
