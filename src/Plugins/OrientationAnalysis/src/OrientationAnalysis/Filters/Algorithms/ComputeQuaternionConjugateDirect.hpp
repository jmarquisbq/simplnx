#pragma once

#include "OrientationAnalysis/OrientationAnalysis_export.hpp"

#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/Filter/IFilter.hpp"

namespace nx::core
{

struct ComputeQuaternionConjugateInputValues;

/**
 * @brief Parallel random-access implementation for RAM-backed quaternion arrays.
 *
 * This preserves the original implementation so in-core performance and behavior
 * remain unchanged while the dispatcher sends out-of-core arrays to Scanline.
 */
class ORIENTATIONANALYSIS_EXPORT ComputeQuaternionConjugateDirect
{
public:
  ComputeQuaternionConjugateDirect(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel,
                                   const ComputeQuaternionConjugateInputValues* inputValues);
  ~ComputeQuaternionConjugateDirect() noexcept;

  ComputeQuaternionConjugateDirect(const ComputeQuaternionConjugateDirect&) = delete;
  ComputeQuaternionConjugateDirect(ComputeQuaternionConjugateDirect&&) noexcept = delete;
  ComputeQuaternionConjugateDirect& operator=(const ComputeQuaternionConjugateDirect&) = delete;
  ComputeQuaternionConjugateDirect& operator=(ComputeQuaternionConjugateDirect&&) noexcept = delete;

  /**
   * @brief Conjugates all quaternion tuples with the original parallel direct loop.
   */
  Result<> operator()();

  const std::atomic_bool& getCancel();

private:
  DataStructure& m_DataStructure;
  const ComputeQuaternionConjugateInputValues* m_InputValues = nullptr;
  const std::atomic_bool& m_ShouldCancel;
  const IFilter::MessageHandler& m_MessageHandler;
};

} // namespace nx::core
