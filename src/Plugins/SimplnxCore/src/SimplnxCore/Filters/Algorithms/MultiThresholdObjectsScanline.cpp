#include "MultiThresholdObjectsScanline.hpp"

#include "MultiThresholdObjects.hpp"

#include "simplnx/Common/TypeTraits.hpp"
#include "simplnx/DataStructure/DataArray.hpp"
#include "simplnx/Utilities/ArrayThreshold.hpp"
#include "simplnx/Utilities/FilterUtilities.hpp"

#include <nonstd/span.hpp>

#include <algorithm>
#include <memory>

using namespace nx::core;

// =============================================================================
// MultiThresholdObjectsScanline — Out-of-Core (OOC) Algorithm
//
// This file implements the out-of-core (Scanline) variant of MultiThresholdObjects.
// It is selected by DispatchAlgorithm when any input array uses chunked on-disk
// storage (e.g., ZarrStore / HDF5 chunked store).
//
// PROBLEM:
//   The Direct variant uses getComponentValue() for per-element input reads and
//   operator[] for per-element output writes, plus allocates an O(n) temporary
//   result vector per threshold condition. When data is stored out-of-core:
//   - Each getComponentValue() call may load an entire chunk from disk
//   - The O(n) temporary vector wastes memory when only a chunk is needed
//   - operator[] writes to the output mask may also trigger chunk load/evict
//
// SOLUTION — CHUNKED PROCESSING:
//   Evaluate the complete threshold tree for one bounded tuple chunk:
//   1. Read each threshold input via copyIntoBuffer()
//   2. Recursively merge child results in chunk-local buffers
//   3. Write the completed result once via copyFromBuffer()
//
// MEMORY SAVINGS:
//   Peak memory is bounded by the chunk size, tree depth, and input component width,
//   instead of the total tuple count.
//
// IMPLEMENTATION NOTE:
//   Temporary buffers use std::unique_ptr<T[]> instead of std::vector<T> to avoid
//   the std::vector<bool> specialization, which would prevent direct memory access
//   needed for copyIntoBuffer/copyFromBuffer spans.
// =============================================================================

namespace
{
/**
 * @brief Chunk size for OOC processing. Each iteration reads/writes this many
 * tuples via bulk I/O. 64K tuples balances between minimizing I/O calls and
 * keeping per-chunk memory small.
 */
constexpr usize k_ChunkSize = 65536;

/**
 * @brief Applies a single comparison operator to a chunk of input data, writing
 * trueValue/falseValue into the chunk-sized output buffer.
 *
 * @tparam CompT Comparison functor (std::less<>, std::greater<>, etc.)
 * @tparam InputT The input array element type
 * @tparam MaskT The output mask element type
 */
template <class CompT, class InputT, class MaskT>
void filterChunkWithComparison(const InputT* inputBuffer, usize numComponents, usize componentIndex, usize chunkTuples, MaskT trueValue, MaskT falseValue, MaskT* outputBuffer, InputT comparisonValue)
{
  for(usize i = 0; i < chunkTuples; ++i)
  {
    InputT inputValue = inputBuffer[i * numComponents + componentIndex];
    outputBuffer[i] = CompT{}(inputValue, comparisonValue) ? trueValue : falseValue;
  }
}

/**
 * @brief Dispatches the comparison based on the ComparisonType enum.
 */
template <class InputT, class MaskT>
void filterChunk(ArrayThreshold::ComparisonType compOperator, const InputT* inputBuffer, usize numComponents, usize componentIndex, usize chunkTuples, MaskT trueValue, MaskT falseValue,
                 MaskT* outputBuffer, InputT comparisonValue)
{
  switch(compOperator)
  {
  case ArrayThreshold::ComparisonType::LessThan:
    filterChunkWithComparison<std::less<>, InputT, MaskT>(inputBuffer, numComponents, componentIndex, chunkTuples, trueValue, falseValue, outputBuffer, comparisonValue);
    break;
  case ArrayThreshold::ComparisonType::GreaterThan:
    filterChunkWithComparison<std::greater<>, InputT, MaskT>(inputBuffer, numComponents, componentIndex, chunkTuples, trueValue, falseValue, outputBuffer, comparisonValue);
    break;
  case ArrayThreshold::ComparisonType::Operator_Equal:
    filterChunkWithComparison<std::equal_to<>, InputT, MaskT>(inputBuffer, numComponents, componentIndex, chunkTuples, trueValue, falseValue, outputBuffer, comparisonValue);
    break;
  case ArrayThreshold::ComparisonType::Operator_NotEqual:
    filterChunkWithComparison<std::not_equal_to<>, InputT, MaskT>(inputBuffer, numComponents, componentIndex, chunkTuples, trueValue, falseValue, outputBuffer, comparisonValue);
    break;
  default: {
    std::string errorMessage = fmt::format("MultiThresholdObjects Comparison Operator not understood: '{}'", static_cast<int>(compOperator));
    throw std::runtime_error(errorMessage);
  }
  }
}

/**
 * @brief Merges a chunk of new threshold results into the current output chunk.
 */
template <typename MaskT>
void insertThresholdChunk(usize chunkTuples, MaskT* currentBuffer, IArrayThreshold::UnionOperator unionOperator, MaskT* newBuffer, bool inverse, MaskT trueValue, MaskT falseValue,
                          const std::atomic_bool& shouldCancel)
{
  for(usize i = 0; i < chunkTuples; i++)
  {
    if((i % 4096) == 0 && shouldCancel)
    {
      return;
    }
    if(inverse)
    {
      newBuffer[i] = (newBuffer[i] == trueValue) ? falseValue : trueValue;
    }

    if(IArrayThreshold::UnionOperator::Or == unionOperator)
    {
      currentBuffer[i] = (currentBuffer[i] == trueValue || newBuffer[i] == trueValue) ? trueValue : falseValue;
    }
    else if(currentBuffer[i] == falseValue || newBuffer[i] == falseValue)
    {
      currentBuffer[i] = falseValue;
    }
  }
}

/**
 * @brief Functor that reads a chunk of the input array via copyIntoBuffer and
 * applies the threshold comparison to produce chunk-sized output.
 */
struct ChunkedThresholdHelper
{
  template <typename InputT, typename MaskT>
  Result<> operator()(const IDataArray& iDataArray, ArrayThreshold::ComparisonType compOperator, ArrayThreshold::ComparisonValue compValue, usize componentIndex, usize chunkStartTuple,
                      usize chunkTuples, MaskT trueValue, MaskT falseValue, MaskT* tempBuffer)
  {
    const auto& inputStore = iDataArray.template getIDataStoreRefAs<AbstractDataStore<InputT>>();
    usize numComponents = inputStore.getNumberOfComponents();

    // Read input chunk (flat elements = tuples * components)
    // Use unique_ptr instead of vector to avoid std::vector<bool> specialization
    usize flatStart = chunkStartTuple * numComponents;
    usize flatCount = chunkTuples * numComponents;
    auto inputBuffer = std::make_unique<InputT[]>(flatCount);
    Result<> readResult = inputStore.copyIntoBuffer(flatStart, nonstd::span<InputT>(inputBuffer.get(), flatCount));
    if(readResult.invalid())
    {
      return readResult;
    }

    InputT comparisonValueTyped = static_cast<InputT>(compValue);
    filterChunk<InputT, MaskT>(compOperator, inputBuffer.get(), numComponents, componentIndex, chunkTuples, trueValue, falseValue, tempBuffer, comparisonValueTyped);
    return {};
  }
};

/**
 * @brief Recursively evaluates one threshold-tree node for only the current tuple chunk.
 *
 * Child buffers are released as recursion unwinds, so peak scratch depends on
 * chunk size and tree depth rather than the total output tuple count.
 */
template <typename MaskT>
Result<> EvaluateScanlineNode(const IArrayThreshold& node, const DataStructure& dataStructure, usize chunkStart, usize chunkTuples, MaskT trueValue, MaskT falseValue, MaskT* output,
                              const std::atomic_bool& shouldCancel)
{
  if(shouldCancel)
  {
    return {};
  }
  if(const auto* threshold = dynamic_cast<const ArrayThreshold*>(&node); threshold != nullptr)
  {
    const auto& inputArray = dataStructure.getDataRefAs<IDataArray>(threshold->getArrayPath());
    Result<> result = ExecuteDataFunction(ChunkedThresholdHelper{}, inputArray.getDataType(), inputArray, threshold->getComparisonType(), threshold->getComparisonValue(),
                                          threshold->getComponentIndex(), chunkStart, chunkTuples, trueValue, falseValue, output);
    if(result.invalid())
    {
      return result;
    }
  }
  else if(const auto* thresholdSet = dynamic_cast<const ArrayThresholdSet*>(&node); thresholdSet != nullptr)
  {
    std::fill_n(output, chunkTuples, falseValue);
    bool hasChild = false;
    for(const auto& child : thresholdSet->getArrayThresholds())
    {
      if(shouldCancel)
      {
        return {};
      }
      auto childOutput = std::make_unique<MaskT[]>(chunkTuples);
      Result<> result = EvaluateScanlineNode(*child, dataStructure, chunkStart, chunkTuples, trueValue, falseValue, childOutput.get(), shouldCancel);
      if(result.invalid())
      {
        return result;
      }
      if(!hasChild)
      {
        std::copy_n(childOutput.get(), chunkTuples, output);
        hasChild = true;
      }
      else
      {
        insertThresholdChunk(chunkTuples, output, child->getUnionOperator(), childOutput.get(), false, trueValue, falseValue, shouldCancel);
      }
    }
  }
  if(node.isInverted())
  {
    for(usize i = 0; i < chunkTuples; ++i)
    {
      if((i % 4096) == 0 && shouldCancel)
      {
        return {};
      }
      output[i] = (output[i] == trueValue) ? falseValue : trueValue;
    }
  }
  return {};
}

/** @brief Dispatches the output mask type and writes one fully evaluated chunk at a time. */
struct ScanlineEvaluator
{
  /** @brief Evaluates the complete threshold tree per bounded chunk and performs one checked output write. */
  template <typename MaskT>
  Result<> operator()(const ArrayThresholdSet& thresholdSet, const DataStructure& dataStructure, IDataArray& outputArray, MaskT trueValue, MaskT falseValue, const std::atomic_bool& shouldCancel)
  {
    auto& outputStore = outputArray.template getIDataStoreRefAs<AbstractDataStore<MaskT>>();
    for(usize chunkStart = 0; chunkStart < outputStore.getNumberOfTuples(); chunkStart += k_ChunkSize)
    {
      if(shouldCancel)
      {
        return {};
      }
      const usize chunkTuples = std::min(k_ChunkSize, outputStore.getNumberOfTuples() - chunkStart);
      auto outputBuffer = std::make_unique<MaskT[]>(chunkTuples);
      Result<> result = EvaluateScanlineNode(thresholdSet, dataStructure, chunkStart, chunkTuples, trueValue, falseValue, outputBuffer.get(), shouldCancel);
      if(result.invalid())
      {
        return result;
      }
      if(shouldCancel)
      {
        return {};
      }
      result = outputStore.copyFromBuffer(chunkStart, nonstd::span<const MaskT>(outputBuffer.get(), chunkTuples));
      if(result.invalid())
      {
        return result;
      }
    }
    return {};
  }
};
} // namespace

// -----------------------------------------------------------------------------
MultiThresholdObjectsScanline::MultiThresholdObjectsScanline(DataStructure& dataStructure, const IFilter::MessageHandler&, const std::atomic_bool& shouldCancel,
                                                             const MultiThresholdObjectsInputValues* inputValues)
: m_DataStructure(dataStructure)
, m_InputValues(inputValues)
, m_ShouldCancel(shouldCancel)
{
}

// -----------------------------------------------------------------------------
MultiThresholdObjectsScanline::~MultiThresholdObjectsScanline() noexcept = default;

// -----------------------------------------------------------------------------
Result<> MultiThresholdObjectsScanline::operator()()
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
  auto& maskArray = m_DataStructure.getDataRefAs<IDataArray>(maskArrayPath);

  return ExecuteDataFunction(ScanlineEvaluator{}, maskArrayType, thresholdsObject, m_DataStructure, maskArray, trueValue, falseValue, m_ShouldCancel);
}
