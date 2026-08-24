#pragma once

#include "SimplnxCore/SimplnxCore_export.hpp"

#include "simplnx/DataStructure/DataArray.hpp"
#include "simplnx/DataStructure/DataPath.hpp"
#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/Filter/IFilter.hpp"
#include "simplnx/Parameters/ChoicesParameter.hpp"
#include "simplnx/Parameters/VectorParameter.hpp"

#include <chrono>
#include <mutex>

namespace nx::core
{

struct SIMPLNXCORE_EXPORT ComputeArrayStatisticsInputValues
{
  ChoicesParameter::ValueType RangeType;
  VectorInt32Parameter::ValueType Range;
  bool FindLength;
  bool FindMin;
  bool FindMax;
  bool FindMean;
  bool FindMedian;
  bool FindMode;
  bool FindStdDeviation;
  bool FindSummation;
  bool UseMask;
  bool ComputeByIndex;
  bool StandardizeData;
  bool FindNumUniqueValues;
  DataPath SelectedArrayPath;
  DataPath FeatureIdsArrayPath;
  DataPath MaskArrayPath;
  DataPath DestinationAttributeMatrix;
  DataPath BinCountsArrayName;
  DataPath BinRangesArrayName;
  DataPath MostPopulatedBinArrayName;
  DataPath ModalBinArrayName;
  DataPath FeatureHasDataArrayName;
  DataPath LengthArrayName;
  DataPath MinimumArrayName;
  DataPath MaximumArrayName;
  DataPath MeanArrayName;
  DataPath MedianArrayName;
  DataPath ModeArrayName;
  DataPath StdDeviationArrayName;
  DataPath SummationArrayName;
  DataPath StandardizedArrayName;
  DataPath NumUniqueValuesName;
  DataPath FeatureIdMapArrayPath;
};

/**
 * @class ComputeArrayStatistics
 * @brief Computes a configurable set of statistical measures (length, min, max,
 * mean, median, mode, standard deviation, summation, unique value count) for a
 * scalar array, optionally grouped by Feature/Ensemble ID.
 *
 * @section ooc_note Out-of-Core Awareness
 * In-memory inputs use the original direct implementation. If any enabled
 * input or output is out-of-core, the algorithm uses bounded bulk reads and
 * writes. Exact median, mode, and unique-value calculations use the registered
 * external-sort capability, with an exact bounded multi-pass fallback when no
 * provider is available.
 */
class SIMPLNXCORE_EXPORT ComputeArrayStatistics
{
public:
  /**
   * @brief Creates an algorithm bound to the filter's DataStructure and options.
   * @param dataStructure Owns all inputs and precreated outputs; it must outlive this object.
   * @param msgHandler Receives phase progress from the bounded implementation.
   * @param shouldCancel Checked between bounded pages and expensive reduction passes.
   * @param inputValues Non-owning pointer to filter options that must outlive this object.
   */
  ComputeArrayStatistics(DataStructure& dataStructure, const IFilter::MessageHandler& msgHandler, const std::atomic_bool& shouldCancel, ComputeArrayStatisticsInputValues* inputValues);
  ~ComputeArrayStatistics() noexcept;

  ComputeArrayStatistics(const ComputeArrayStatistics&) = delete;
  ComputeArrayStatistics(ComputeArrayStatistics&&) noexcept = delete;
  ComputeArrayStatistics& operator=(const ComputeArrayStatistics&) = delete;
  ComputeArrayStatistics& operator=(ComputeArrayStatistics&&) noexcept = delete;

  // sequence dependent DO NOT REORDER
  enum FeatureIdRangeControls : uint8
  {
    None = 0,
    IgnoreZero = 1,
    ShrinkToFit = 2,
    PaddedCustomRange = 3,
    CustomRange = 4
  };

  /**
   * @brief Resolves the requested feature layout and dispatches the resident or
   * bounded algorithm according to the participating stores.
   * @return The first validation, storage, or reduction error; cancellation is
   * reported as a valid early return consistent with filter execution.
   */
  Result<> operator()();

private:
  DataStructure& m_DataStructure;
  const ComputeArrayStatisticsInputValues* m_InputValues = nullptr;
  const std::atomic_bool& m_ShouldCancel;
  const IFilter::MessageHandler& m_MessageHandler;
};

} // namespace nx::core
