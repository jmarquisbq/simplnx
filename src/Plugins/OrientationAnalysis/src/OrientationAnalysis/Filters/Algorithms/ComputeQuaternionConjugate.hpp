#pragma once

#include "OrientationAnalysis/OrientationAnalysis_export.hpp"

#include "simplnx/DataStructure/DataPath.hpp"
#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/Filter/IFilter.hpp"

namespace nx::core
{

/**
 * @brief Holds the paths and options used to compute quaternion conjugates.
 */
struct ORIENTATIONANALYSIS_EXPORT ComputeQuaternionConjugateInputValues
{
  DataPath QuaternionDataArrayPath;
  DataPath OutputDataArrayPath;
  bool DeleteOriginalData;
};

/**
 * @brief Selects the direct or bulk-I/O quaternion conjugation implementation.
 *
 * The dispatcher keeps the existing parallel direct path for RAM-backed arrays and
 * selects the bounded scanline path whenever either quaternion array is out-of-core.
 */
class ORIENTATIONANALYSIS_EXPORT ComputeQuaternionConjugate
{
public:
  ComputeQuaternionConjugate(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel, ComputeQuaternionConjugateInputValues* inputValues);
  ~ComputeQuaternionConjugate() noexcept;

  ComputeQuaternionConjugate(const ComputeQuaternionConjugate&) = delete;
  ComputeQuaternionConjugate(ComputeQuaternionConjugate&&) noexcept = delete;
  ComputeQuaternionConjugate& operator=(const ComputeQuaternionConjugate&) = delete;
  ComputeQuaternionConjugate& operator=(ComputeQuaternionConjugate&&) noexcept = delete;

  /**
   * @brief Computes quaternion conjugates using the storage-appropriate algorithm.
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
