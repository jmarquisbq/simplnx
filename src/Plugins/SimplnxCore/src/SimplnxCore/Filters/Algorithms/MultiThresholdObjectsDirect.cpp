#include "MultiThresholdObjectsDirect.hpp"

#include "MultiThresholdObjects.hpp"

#include "simplnx/Common/TypeTraits.hpp"
#include "simplnx/DataStructure/DataArray.hpp"
#include "simplnx/Utilities/ArrayThreshold.hpp"
#include "simplnx/Utilities/FilterUtilities.hpp"

#include <algorithm>

using namespace nx::core;

// =============================================================================
// MultiThresholdObjectsDirect — In-Core Algorithm
//
// This file implements the in-core (Direct) variant of MultiThresholdObjects.
// It is selected by DispatchAlgorithm when all input arrays reside in memory.
//
// ALGORITHM OVERVIEW:
//   For each threshold condition in the user-defined threshold tree:
//   1. Allocate an O(n) temporary result vector initialized to FALSE
//   2. Read each element of the input array via getComponentValue()
//   3. Apply the comparison (< > == !=) to produce TRUE/FALSE per element
//   4. Merge the temporary results into the output mask using AND/OR logic
//
//   Threshold conditions can be nested in ArrayThresholdSets (which recursively
//   apply AND/OR between their children) or be individual ArrayThreshold comparisons.
//
// DATA ACCESS PATTERN:
//   Uses getComponentValue() for per-element random access to input arrays, and
//   operator[] for per-element writes to the output mask and temporary vectors.
//   This is optimal for in-memory data. The O(n) temporary vector is acceptable
//   when data is in memory but would be wasteful for OOC data — see the Scanline
//   variant which uses O(chunkSize) temporaries instead.
// =============================================================================

namespace
{
/** @brief Combines one resident child-result vector into its parent's vector with the requested Boolean union. */
template <typename MaskT>
void MergeThresholdResults(std::vector<MaskT>& current, const std::vector<MaskT>& next, IArrayThreshold::UnionOperator unionOperator, MaskT trueValue, MaskT falseValue,
                           const std::atomic_bool& shouldCancel)
{
  for(usize i = 0; i < current.size(); ++i)
  {
    if((i % 4096) == 0 && shouldCancel)
    {
      return;
    }
    if(unionOperator == IArrayThreshold::UnionOperator::Or)
    {
      current[i] = (current[i] == trueValue || next[i] == trueValue) ? trueValue : falseValue;
    }
    else
    {
      current[i] = (current[i] == trueValue && next[i] == trueValue) ? trueValue : falseValue;
    }
  }
}

/** @brief Dispatches a threshold input type and evaluates one comparison through direct component access. */
template <typename MaskT>
struct DirectComparisonEvaluator
{
  /** @brief Evaluates one leaf threshold for every resident tuple. */
  template <typename InputT>
  void operator()(const ArrayThreshold& threshold, const IDataArray& inputArray, std::vector<MaskT>& output, MaskT trueValue, MaskT falseValue, const std::atomic_bool& shouldCancel)
  {
    const auto& inputStore = inputArray.template getIDataStoreRefAs<AbstractDataStore<InputT>>();
    const InputT comparisonValue = static_cast<InputT>(threshold.getComparisonValue());
    for(usize tupleIndex = 0; tupleIndex < inputStore.getNumberOfTuples(); ++tupleIndex)
    {
      if((tupleIndex % 4096) == 0 && shouldCancel)
      {
        return;
      }
      const InputT value = inputStore.getComponentValue(tupleIndex, threshold.getComponentIndex());
      bool matches = false;
      switch(threshold.getComparisonType())
      {
      case ArrayThreshold::ComparisonType::LessThan:
        matches = value < comparisonValue;
        break;
      case ArrayThreshold::ComparisonType::GreaterThan:
        matches = value > comparisonValue;
        break;
      case ArrayThreshold::ComparisonType::Operator_Equal:
        matches = value == comparisonValue;
        break;
      case ArrayThreshold::ComparisonType::Operator_NotEqual:
        matches = value != comparisonValue;
        break;
      default:
        throw std::runtime_error(fmt::format("MultiThresholdObjects Comparison Operator not understood: '{}'", static_cast<int>(threshold.getComparisonType())));
      }
      output[tupleIndex] = matches ? trueValue : falseValue;
    }
  }
};

/** @brief Recursively evaluates a threshold tree into a cell-count resident mask vector. */
template <typename MaskT>
void EvaluateDirectNode(const IArrayThreshold& node, const DataStructure& dataStructure, usize tupleCount, std::vector<MaskT>& output, MaskT trueValue, MaskT falseValue,
                        const std::atomic_bool& shouldCancel)
{
  if(shouldCancel)
  {
    return;
  }
  output.assign(tupleCount, falseValue);
  if(const auto* threshold = dynamic_cast<const ArrayThreshold*>(&node); threshold != nullptr)
  {
    const auto& inputArray = dataStructure.getDataRefAs<IDataArray>(threshold->getArrayPath());
    ExecuteDataFunction(DirectComparisonEvaluator<MaskT>{}, inputArray.getDataType(), *threshold, inputArray, output, trueValue, falseValue, shouldCancel);
  }
  else if(const auto* thresholdSet = dynamic_cast<const ArrayThresholdSet*>(&node); thresholdSet != nullptr)
  {
    bool hasChild = false;
    for(const auto& child : thresholdSet->getArrayThresholds())
    {
      if(shouldCancel)
      {
        return;
      }
      std::vector<MaskT> childResult;
      EvaluateDirectNode(*child, dataStructure, tupleCount, childResult, trueValue, falseValue, shouldCancel);
      if(shouldCancel)
      {
        return;
      }
      if(!hasChild)
      {
        output = std::move(childResult);
        hasChild = true;
      }
      else
      {
        MergeThresholdResults(output, childResult, child->getUnionOperator(), trueValue, falseValue, shouldCancel);
        if(shouldCancel)
        {
          return;
        }
      }
    }
  }
  if(node.isInverted())
  {
    for(usize i = 0; i < output.size(); ++i)
    {
      if((i % 4096) == 0 && shouldCancel)
      {
        return;
      }
      output[i] = (output[i] == trueValue) ? falseValue : trueValue;
    }
  }
}

/** @brief Dispatches the output mask type, evaluates the complete tree, and writes resident values directly. */
struct DirectEvaluator
{
  /** @brief Executes the complete resident threshold evaluation. */
  template <typename MaskT>
  void operator()(const ArrayThresholdSet& thresholdSet, const DataStructure& dataStructure, IDataArray& outputArray, MaskT trueValue, MaskT falseValue, const std::atomic_bool& shouldCancel)
  {
    std::vector<MaskT> result;
    EvaluateDirectNode(thresholdSet, dataStructure, outputArray.getNumberOfTuples(), result, trueValue, falseValue, shouldCancel);
    if(shouldCancel)
    {
      return;
    }
    auto& outputStore = outputArray.template getIDataStoreRefAs<AbstractDataStore<MaskT>>();
    for(usize i = 0; i < result.size(); ++i)
    {
      if((i % 4096) == 0 && shouldCancel)
      {
        return;
      }
      outputStore[i] = result[i];
    }
  }
};
} // namespace

// -----------------------------------------------------------------------------
MultiThresholdObjectsDirect::MultiThresholdObjectsDirect(DataStructure& dataStructure, const IFilter::MessageHandler&, const std::atomic_bool& shouldCancel,
                                                         const MultiThresholdObjectsInputValues* inputValues)
: m_DataStructure(dataStructure)
, m_InputValues(inputValues)
, m_ShouldCancel(shouldCancel)
{
}

// -----------------------------------------------------------------------------
MultiThresholdObjectsDirect::~MultiThresholdObjectsDirect() noexcept = default;

// -----------------------------------------------------------------------------
Result<> MultiThresholdObjectsDirect::operator()()
{
  auto thresholdsObject = m_InputValues->ArrayThresholdsObject;
  auto maskArrayName = m_InputValues->OutputDataArrayName;
  auto maskArrayType = m_InputValues->CreatedMaskType;
  auto useCustomTrueValue = m_InputValues->UseCustomTrueValue;
  auto useCustomFalseValue = m_InputValues->UseCustomFalseValue;
  auto customTrueValue = m_InputValues->CustomTrueValue;
  auto customFalseValue = m_InputValues->CustomFalseValue;

  float64 trueValue = useCustomTrueValue ? customTrueValue : 1.0;
  float64 falseValue = useCustomFalseValue ? customFalseValue : 0.0;

  DataPath maskArrayPath = (*thresholdsObject.getRequiredPaths().begin()).replaceName(maskArrayName);
  if(m_ShouldCancel)
  {
    return {};
  }
  ExecuteDataFunction(DirectEvaluator{}, maskArrayType, thresholdsObject, m_DataStructure, m_DataStructure.getDataRefAs<IDataArray>(maskArrayPath), trueValue, falseValue, m_ShouldCancel);

  return {};
}
