#pragma once

#include "SimplnxCore/SimplnxCore_export.hpp"

#include "simplnx/DataStructure/DataPath.hpp"
#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/Filter/IFilter.hpp"
#include "simplnx/Parameters/ArrayCreationParameter.hpp"
#include "simplnx/Parameters/ArraySelectionParameter.hpp"

namespace nx::core
{

struct SIMPLNXCORE_EXPORT ComputeDifferencesMapInputValues
{
  ArrayCreationParameter::ValueType DifferenceMapArrayPath;
  ArraySelectionParameter::ValueType FirstInputArrayPath;
  ArraySelectionParameter::ValueType SecondInputArrayPath;
};

/**
 * @class ComputeDifferencesMap
 * @brief Computes the component-wise absolute difference between two arrays.
 *
 * Input and output values are streamed through bounded, component-aligned buffers so
 * out-of-core stores use bulk I/O without allocating memory proportional to the array.
 */

class SIMPLNXCORE_EXPORT ComputeDifferencesMap
{
public:
  ComputeDifferencesMap(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel, ComputeDifferencesMapInputValues* inputValues);
  ~ComputeDifferencesMap() noexcept;

  ComputeDifferencesMap(const ComputeDifferencesMap&) = delete;
  ComputeDifferencesMap(ComputeDifferencesMap&&) noexcept = delete;
  ComputeDifferencesMap& operator=(const ComputeDifferencesMap&) = delete;
  ComputeDifferencesMap& operator=(ComputeDifferencesMap&&) noexcept = delete;

  Result<> operator()();

private:
  DataStructure& m_DataStructure;
  const ComputeDifferencesMapInputValues* m_InputValues = nullptr;
  const std::atomic_bool& m_ShouldCancel;
  const IFilter::MessageHandler& m_MessageHandler;
};

} // namespace nx::core
