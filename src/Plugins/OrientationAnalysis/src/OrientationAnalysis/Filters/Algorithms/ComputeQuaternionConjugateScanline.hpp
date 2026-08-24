#pragma once

#include "OrientationAnalysis/OrientationAnalysis_export.hpp"

#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/Filter/IFilter.hpp"

namespace nx::core
{

struct ComputeQuaternionConjugateInputValues;

/**
 * @brief Bulk-I/O implementation for out-of-core quaternion arrays.
 *
 * It streams fixed-size tuple chunks so disk-backed arrays never incur per-value
 * store access or memory use proportional to the total number of tuples.
 */
class ORIENTATIONANALYSIS_EXPORT ComputeQuaternionConjugateScanline
{
public:
  ComputeQuaternionConjugateScanline(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel,
                                     const ComputeQuaternionConjugateInputValues* inputValues);
  ~ComputeQuaternionConjugateScanline() noexcept;

  ComputeQuaternionConjugateScanline(const ComputeQuaternionConjugateScanline&) = delete;
  ComputeQuaternionConjugateScanline(ComputeQuaternionConjugateScanline&&) noexcept = delete;
  ComputeQuaternionConjugateScanline& operator=(const ComputeQuaternionConjugateScanline&) = delete;
  ComputeQuaternionConjugateScanline& operator=(ComputeQuaternionConjugateScanline&&) noexcept = delete;

  /**
   * @brief Streams input quaternion chunks and writes their conjugates.
   */
  Result<> operator()();

private:
  DataStructure& m_DataStructure;
  const ComputeQuaternionConjugateInputValues* m_InputValues = nullptr;
  const std::atomic_bool& m_ShouldCancel;
  const IFilter::MessageHandler& m_MessageHandler;
};

} // namespace nx::core
