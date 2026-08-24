#pragma once

#include "SimplnxCore/SimplnxCore_export.hpp"

#include "simplnx/DataStructure/DataPath.hpp"
#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/Filter/IFilter.hpp"
#include "simplnx/Parameters/ArraySelectionParameter.hpp"
#include "simplnx/Parameters/BoolParameter.hpp"
#include "simplnx/Parameters/StringParameter.hpp"

namespace nx::core
{

/**
 * @brief Runtime values used by ConditionalSetValue.
 */
struct SIMPLNXCORE_EXPORT ConditionalSetValueInputValues
{
  ArraySelectionParameter::ValueType ConditionalArrayPath;
  BoolParameter::ValueType InvertMask;
  StringParameter::ValueType RemoveValue;
  StringParameter::ValueType ReplaceValue;
  ArraySelectionParameter::ValueType SelectedArrayPath;
  BoolParameter::ValueType UseConditional;
};

/**
 * @class ConditionalSetValue
 * @brief Replaces selected array values using either value comparison or a conditional mask.
 *
 * In-memory arrays use contiguous pointers to avoid virtual per-value access and staging-copy overhead.
 * If the target or conditional array is out-of-core, the algorithm dispatches to a bounded streaming
 * path that type-dispatches both arrays and performs sequential bulk transfers instead of per-cell OOC I/O.
 */
class SIMPLNXCORE_EXPORT ConditionalSetValue
{
public:
  /**
   * @brief Constructs the algorithm with its data, message, cancellation, and parameter inputs.
   */
  ConditionalSetValue(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel, ConditionalSetValueInputValues* inputValues);
  ~ConditionalSetValue() noexcept;

  ConditionalSetValue(const ConditionalSetValue&) = delete;
  ConditionalSetValue(ConditionalSetValue&&) noexcept = delete;
  ConditionalSetValue& operator=(const ConditionalSetValue&) = delete;
  ConditionalSetValue& operator=(ConditionalSetValue&&) noexcept = delete;

  /**
   * @brief Executes the direct or bounded streaming replacement path.
   * @return A valid result on success or cancellation, otherwise the datastore or conversion error.
   */
  Result<> operator()();

private:
  DataStructure& m_DataStructure;
  const ConditionalSetValueInputValues* m_InputValues = nullptr;
  const std::atomic_bool& m_ShouldCancel;
  const IFilter::MessageHandler& m_MessageHandler;
};

} // namespace nx::core
