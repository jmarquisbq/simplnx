#include "RobustAutomaticThreshold.hpp"

#include "simplnx/DataStructure/DataArray.hpp"
#include "simplnx/Utilities/AlgorithmDispatch.hpp"
#include "simplnx/Utilities/FilterUtilities.hpp"

#include <algorithm>
#include <memory>

using namespace nx::core;

namespace
{
/**
 * @brief Computes the weighted threshold and emits the Bool mask with two bounded passes.
 *
 * The first pass preserves the legacy accumulation order while replacing
 * singleton input/gradient reads. The second re-reads only the scalar input and
 * writes complete mask chunks, avoiding a full resident output mask.
 */
struct FindThresholdFunctor
{
  /** @brief Executes both passes for the runtime-selected scalar input type. */
  template <class T>
  Result<> operator()(const IDataArray* inputObject, const Float32AbstractDataStore& gradMag, BoolAbstractDataStore& maskStore, const std::atomic_bool& shouldCancel)
  {
    const auto& inputData = inputObject->template getIDataStoreRefAs<AbstractDataStore<T>>();
    const usize numTuples = inputData.getNumberOfTuples();
    constexpr usize kTuplesPerBatch = 65536;
    auto inputBuffer = std::make_unique<T[]>(kTuplesPerBatch);
    auto gradientBuffer = std::make_unique<float32[]>(kTuplesPerBatch);
    auto maskBuffer = std::make_unique<bool[]>(kTuplesPerBatch);
    float numerator = 0;
    float denominator = 0;

    // Preserve the legacy accumulation order while replacing singleton OOC reads with bounded batches.
    for(usize start = 0; start < numTuples; start += kTuplesPerBatch)
    {
      if(shouldCancel)
      {
        return {};
      }
      const usize count = std::min(kTuplesPerBatch, numTuples - start);
      auto result = inputData.copyIntoBuffer(start, nonstd::span<T>(inputBuffer.get(), count));
      if(result.invalid())
      {
        return result;
      }
      result = gradMag.copyIntoBuffer(start, nonstd::span<float32>(gradientBuffer.get(), count));
      if(result.invalid())
      {
        return result;
      }
      for(usize i = 0; i < count; i++)
      {
        numerator += (inputBuffer[i] * gradientBuffer[i]);
        denominator += gradientBuffer[i];
      }
    }

    float threshold = numerator / denominator;

    for(usize start = 0; start < numTuples; start += kTuplesPerBatch)
    {
      if(shouldCancel)
      {
        return {};
      }
      const usize count = std::min(kTuplesPerBatch, numTuples - start);
      auto result = inputData.copyIntoBuffer(start, nonstd::span<T>(inputBuffer.get(), count));
      if(result.invalid())
      {
        return result;
      }
      for(usize i = 0; i < count; i++)
      {
        maskBuffer[i] = inputBuffer[i] >= threshold;
      }
      result = maskStore.copyFromBuffer(start, nonstd::span<const bool>(maskBuffer.get(), count));
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
RobustAutomaticThreshold::RobustAutomaticThreshold(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel,
                                                   RobustAutomaticThresholdInputValues* inputValues)
: m_DataStructure(dataStructure)
, m_InputValues(inputValues)
, m_ShouldCancel(shouldCancel)
, m_MessageHandler(mesgHandler)
{
}

// -----------------------------------------------------------------------------
RobustAutomaticThreshold::~RobustAutomaticThreshold() noexcept = default;

// -----------------------------------------------------------------------------
Result<> RobustAutomaticThreshold::operator()()
{
  const auto* inputArray = m_DataStructure.getDataAs<IDataArray>(m_InputValues->InputArrayPath);
  const auto* gradientArray = m_DataStructure.getDataAs<Float32Array>(m_InputValues->GradientArrayPath);
  auto* maskArray = m_DataStructure.getDataAs<BoolArray>(m_InputValues->InputArrayPath.replaceName(m_InputValues->CreatedMaskName));
  const auto& gradientStoreRef = gradientArray->getDataStoreRef();
  auto& maskStoreRef = maskArray->getDataStoreRef();

  if(m_ShouldCancel)
  {
    return {};
  }

  const bool usesOutOfCoreStore = AnyOutOfCore({inputArray, gradientArray, maskArray});
  const bool useOutOfCorePath = !ForceInCoreAlgorithm() && (usesOutOfCoreStore || ForceOocAlgorithm());
  RecordAlgorithmPathExecution(useOutOfCorePath ? AlgorithmPath::OutOfCore : AlgorithmPath::InCore, usesOutOfCoreStore);
  return ExecuteDataFunction(FindThresholdFunctor{}, inputArray->getDataType(), inputArray, gradientStoreRef, maskStoreRef, m_ShouldCancel);
}
