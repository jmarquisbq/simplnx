#pragma once

#include "SimplnxCore/SimplnxCore_export.hpp"

#include "simplnx/DataStructure/DataPath.hpp"
#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/Filter/IFilter.hpp"

#include <atomic>
#include <string>
#include <vector>

namespace nx::core
{
/**
 * @struct SplitDataArrayByComponentInputValues
 * @brief Defines the source array, output suffix, and ordered components extracted by the algorithm.
 */
struct SIMPLNXCORE_EXPORT SplitDataArrayByComponentInputValues
{
  DataPath InputArrayPath;
  std::string SplitArraysSuffix;
  std::vector<usize> ExtractComponents;
};

/**
 * @class SplitDataArrayByComponent
 * @brief Splits selected components from a multi-component array into scalar arrays.
 *
 * Contiguous in-memory arrays use a parallel direct path. Out-of-core arrays use bounded
 * bulk transfers that read each interleaved input chunk once and stream scalar outputs.
 */
class SIMPLNXCORE_EXPORT SplitDataArrayByComponent
{
public:
  /**
   * @brief Constructs the algorithm with its data structure, messaging, cancellation, and input values.
   * @param dataStructure Data structure containing the input and preflight-created output arrays.
   * @param messageHandler Handler used for progress messages.
   * @param shouldCancel Cancellation flag checked during execution.
   * @param inputValues Values defining the input array and components to extract.
   */
  SplitDataArrayByComponent(DataStructure& dataStructure, const IFilter::MessageHandler& messageHandler, const std::atomic_bool& shouldCancel, SplitDataArrayByComponentInputValues* inputValues);

  /**
   * @brief Destroys the algorithm.
   */
  ~SplitDataArrayByComponent() noexcept;

  SplitDataArrayByComponent(const SplitDataArrayByComponent&) = delete;
  SplitDataArrayByComponent(SplitDataArrayByComponent&&) noexcept = delete;
  SplitDataArrayByComponent& operator=(const SplitDataArrayByComponent&) = delete;
  SplitDataArrayByComponent& operator=(SplitDataArrayByComponent&&) noexcept = delete;

  /**
   * @brief Executes the storage-appropriate split implementation.
   * @return A valid result on success or cancellation, or the first bulk-transfer error.
   */
  Result<> operator()();

  /**
   * @brief Returns the execution cancellation flag.
   * @return The cancellation flag supplied at construction.
   */
  const std::atomic_bool& getCancel();

private:
  DataStructure& m_DataStructure;
  const SplitDataArrayByComponentInputValues* m_InputValues = nullptr;
  const std::atomic_bool& m_ShouldCancel;
  const IFilter::MessageHandler& m_MessageHandler;
};

} // namespace nx::core
