#include "InitializeData.hpp"

#include "simplnx/Common/TypeTraits.hpp"
#include "simplnx/DataStructure/AbstractDataStore.hpp"
#include "simplnx/DataStructure/IDataArray.hpp"
#include "simplnx/Utilities/AlgorithmDispatch.hpp"
#include "simplnx/Utilities/FilterUtilities.hpp"
#include "simplnx/Utilities/StringInterpretationUtilities.hpp"

#include <nonstd/span.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <memory>
#include <random>
#include <vector>

using namespace nx::core;

namespace
{

// At the current time this code could be simplified with a bool in the incremental template, HOWEVER,
// it was done this way to allow for expansion of operations down the line multiplication, division, etc.
/** @brief Compile-time selection of addition or subtraction for the incremental generator. */
template <bool UseAddition, bool UseSubtraction>
struct IncrementalOptions
{
  static constexpr bool UsingAddition = UseAddition;
  static constexpr bool UsingSubtraction = UseSubtraction;
};

using AdditionT = IncrementalOptions<true, false>;
using SubtractionT = IncrementalOptions<false, true>;

constexpr usize k_InitializationChunkValues = 65536;

/**
 * @brief Generates typed values in tuple/component order and writes fixed chunks to a DataStore.
 *
 * All initialization modes share this sink so none of them allocate or mutate
 * an array-sized resident buffer, including Bool stores.
 */
template <typename T, typename ValueGenerator>
Result<> WriteGeneratedValues(AbstractDataStore<T>& dataStore, ValueGenerator&& generateValue, const std::atomic_bool& shouldCancel)
{
  const usize numComponents = dataStore.getNumberOfComponents();
  const usize numTuples = dataStore.getNumberOfTuples();
  if(numComponents == 0 || numTuples == 0)
  {
    return {};
  }

  const usize tuplesPerChunk = std::max<usize>(1, k_InitializationChunkValues / numComponents);
  auto buffer = std::make_unique<T[]>(tuplesPerChunk * numComponents);
  for(usize tupleOffset = 0; tupleOffset < numTuples; tupleOffset += tuplesPerChunk)
  {
    if(shouldCancel)
    {
      return {};
    }
    const usize tupleCount = std::min(tuplesPerChunk, numTuples - tupleOffset);
    for(usize localTuple = 0; localTuple < tupleCount; localTuple++)
    {
      const usize tupleIndex = tupleOffset + localTuple;
      for(usize component = 0; component < numComponents; component++)
      {
        buffer[localTuple * numComponents + component] = generateValue(tupleIndex, component);
      }
    }
    auto writeResult = dataStore.copyFromBuffer(tupleOffset * numComponents, nonstd::span<const T>(buffer.get(), tupleCount * numComponents));
    if(writeResult.invalid())
    {
      return writeResult;
    }
  }
  return {};
}

/** @brief Parses one fill value per component and repeats it through the bounded generator sink. */
template <typename T>
Result<> ValueFill(AbstractDataStore<T>& dataStore, const std::vector<std::string>& stringValues, const std::atomic_bool& shouldCancel)
{
  const usize numComponents = dataStore.getNumberOfComponents();
  std::vector<T> values;
  values.reserve(numComponents);
  for(const auto& stringValue : stringValues)
  {
    values.push_back(StringInterpretationUtilities::Convert<T>(stringValue).value());
  }
  return WriteGeneratedValues<T>(dataStore, [&values](usize, usize component) { return values[component]; }, shouldCancel);
}

/** @brief Produces the original component-wise incremental sequence without retaining generated tuples. */
template <typename T, class IncrementalOptions = AdditionT>
Result<> IncrementalFill(AbstractDataStore<T>& dataStore, const std::vector<std::string>& startValues, const std::vector<std::string>& stepValues, const std::atomic_bool& shouldCancel)
{
  const usize numComponents = dataStore.getNumberOfComponents();

  std::vector<T> values(numComponents);
  std::vector<T> steps(numComponents);

  for(usize component = 0; component < numComponents; component++)
  {
    values[component] = StringInterpretationUtilities::Convert<T>(startValues[component]).value();
    steps[component] = StringInterpretationUtilities::Convert<T>(stepValues[component]).value();
  }

  if constexpr(std::is_same_v<T, bool>)
  {
    return WriteGeneratedValues<T>(
        dataStore,
        [&values, &steps](usize tupleIndex, usize component) {
          const bool value = values[component];
          if(tupleIndex == 0 && steps[component])
          {
            values[component] = IncrementalOptions::UsingAddition;
          }
          return value;
        },
        shouldCancel);
  }
  else
  {
    return WriteGeneratedValues<T>(
        dataStore,
        [&values, &steps](usize, usize component) {
          const T value = values[component];
          if constexpr(IncrementalOptions::UsingAddition)
          {
            values[component] += steps[component];
          }
          if constexpr(IncrementalOptions::UsingSubtraction)
          {
            values[component] -= steps[component];
          }
          return value;
        },
        shouldCancel);
  }
}

/** @brief Produces seeded random values from component-scale generators and bounded output chunks. */
template <typename T, bool Ranged, class DistributionT>
Result<> RandomFill(std::vector<DistributionT>& distributions, AbstractDataStore<T>& dataStore, const uint64 seed, const bool standardizeSeed, const std::atomic_bool& shouldCancel)
{
  const usize numComponents = dataStore.getNumberOfComponents();

  std::vector<std::mt19937_64> generators(numComponents, std::mt19937_64{});

  for(usize component = 0; component < numComponents; component++)
  {
    generators[component].seed(standardizeSeed ? seed : seed + component);
  }

  return WriteGeneratedValues<T>(
      dataStore,
      [&distributions, &generators](usize, usize component) -> T {
        if constexpr(std::is_floating_point_v<T>)
        {
          if constexpr(Ranged)
          {
            return static_cast<T>(distributions[component](generators[component]));
          }
          else if constexpr(std::is_signed_v<T>)
          {
            return static_cast<T>(distributions[component](generators[component]) * (std::numeric_limits<T>::max() - 1) * (((rand() & 1) == 0) ? 1 : -1));
          }
          else
          {
            return static_cast<T>(distributions[component](generators[component]) * std::numeric_limits<T>::max());
          }
        }
        else
        {
          return static_cast<T>(distributions[component](generators[component]));
        }
      },
      shouldCancel);
}

/** @brief Dispatches the requested incremental operation to its compile-time generator specialization. */
template <typename T, class... ArgsT>
Result<> FillIncForwarder(const StepType& stepType, ArgsT&&... args)
{
  switch(stepType)
  {
  case StepType::Addition: {
    return ::IncrementalFill<T, AdditionT>(std::forward<ArgsT>(args)...);
  }
  case StepType::Subtraction: {
    return ::IncrementalFill<T, SubtractionT>(std::forward<ArgsT>(args)...);
  }
  }
  return MakeErrorResult(-11620, "InitializeData received an invalid incremental operation.");
}

/** @brief Builds type-appropriate component distributions and forwards them to the bounded random generator. */
template <typename T, bool Ranged, class... ArgsT>
Result<> FillRandomForwarder(const std::vector<T>& range, usize numComponents, ArgsT&&... args)
{
  if constexpr(std::is_same_v<T, bool>)
  {
    std::vector<std::uniform_int_distribution<int64>> distributions;
    for(usize component = 0; component < numComponents * 2; component += 2)
    {
      distributions.emplace_back((range.at(component) ? 1 : 0), (range.at(component + 1) ? 1 : 0));
    }
    return ::RandomFill<T, Ranged, std::uniform_int_distribution<int64>>(distributions, std::forward<ArgsT>(args)...);
  }
  else if constexpr(!std::is_floating_point_v<T>)
  {
    std::vector<std::uniform_int_distribution<int64>> distributions;
    for(usize component = 0; component < numComponents * 2; component += 2)
    {
      distributions.emplace_back(range.at(component), range.at(component + 1));
    }
    return ::RandomFill<T, Ranged, std::uniform_int_distribution<int64>>(distributions, std::forward<ArgsT>(args)...);
  }
  else
  {
    std::vector<std::uniform_real_distribution<float64>> distributions;
    for(usize component = 0; component < numComponents * 2; component += 2)
    {
      distributions.emplace_back(Ranged ? static_cast<float64>(range.at(component)) : 0.0, Ranged ? static_cast<float64>(range.at(component + 1)) : 1.0);
    }
    return ::RandomFill<T, Ranged, std::uniform_real_distribution<float64>>(distributions, std::forward<ArgsT>(args)...);
  }
}

/** @brief Expands one user value to every component while preserving explicit per-component lists. */
std::vector<std::string> standardizeMultiComponent(const usize numComps, const std::vector<std::string>& componentValues)
{
  if(componentValues.size() == numComps)
  {
    return {componentValues};
  }
  else
  {
    std::vector<std::string> standardized(numComps);
    for(usize comp = 0; comp < numComps; comp++)
    {
      standardized[comp] = componentValues[0];
    }
    return standardized;
  }
}

/** @brief Dispatches one runtime array type and initialization mode to the matching bounded generator. */
struct FillArrayFunctor
{
  /** @brief Parses mode-specific values and initializes the complete typed target array. */
  template <typename T>
  Result<> operator()(IDataArray& iDataArray, const InitializeDataInputValues& inputValues, const std::atomic_bool& shouldCancel)
  {
    auto& dataStore = iDataArray.template getIDataStoreRefAs<AbstractDataStore<T>>();
    const usize numComp = dataStore.getNumberOfComponents();

    switch(inputValues.initType)
    {
    case InitializeType::FillValue: {
      return ::ValueFill<T>(dataStore, standardizeMultiComponent(numComp, inputValues.stringValues), shouldCancel);
    }
    case InitializeType::Incremental: {
      return ::FillIncForwarder<T>(inputValues.stepType, dataStore, standardizeMultiComponent(numComp, inputValues.startValues), standardizeMultiComponent(numComp, inputValues.stepValues),
                                   shouldCancel);
    }
    case InitializeType::Random: {
      std::vector<T> range;
      if constexpr(!std::is_same_v<T, bool>)
      {
        for(usize comp = 0; comp < numComp; comp++)
        {
          range.push_back(std::numeric_limits<T>::min());
          range.push_back(std::numeric_limits<T>::max());
        }
      }
      if constexpr(std::is_same_v<T, bool>)
      {
        for(usize comp = 0; comp < numComp; comp++)
        {
          range.push_back(false);
          range.push_back(true);
        }
      }
      return ::FillRandomForwarder<T, false>(range, numComp, dataStore, inputValues.seed, inputValues.standardizeSeed, shouldCancel);
    }
    case InitializeType::RangedRandom: {
      auto randBegin = standardizeMultiComponent(numComp, inputValues.randBegin);
      auto randEnd = standardizeMultiComponent(numComp, inputValues.randEnd);

      std::vector<T> range;
      for(usize comp = 0; comp < numComp; comp++)
      {
        Result<T> result = StringInterpretationUtilities::Convert<T>(randBegin[comp]);
        range.push_back(result.value());
        result = StringInterpretationUtilities::Convert<T>(randEnd[comp]);
        range.push_back(result.value());
      }
      return ::FillRandomForwarder<T, true>(range, numComp, dataStore, inputValues.seed, inputValues.standardizeSeed, shouldCancel);
    }
    }
    return MakeErrorResult(-11621, "InitializeData received an invalid initialization type.");
  }
};

int64 CreateCompValFromStr(const std::string& s)
{
  return (StringUtilities::toLower(s) == "true") ? 1 : (StringUtilities::toLower(s) == "false") ? 0 : std::stoll(s);
}
} // namespace

namespace nx
{
namespace core
{
std::string CreateCompValsStr(const std::vector<int64>& componentValues, usize numComps)
{
  const usize compValueVisibilityThresholdCount = 10;
  const usize startEndEllipseValueCount = compValueVisibilityThresholdCount / 2;

  std::stringstream updatedValStrm;
  auto cValueTokens = componentValues;
  if(cValueTokens.size() == 1)
  {
    cValueTokens = std::vector<int64>(numComps, cValueTokens[0]);
  }

  if(numComps <= compValueVisibilityThresholdCount)
  {
    auto initFillTokensStr = fmt::format("{}", fmt::join(cValueTokens, ","));
    updatedValStrm << fmt::format("|{}|", initFillTokensStr, numComps);
  }
  else
  {
    auto initFillTokensBeginStr = fmt::format("{}", fmt::join(cValueTokens.begin(), cValueTokens.begin() + startEndEllipseValueCount, ","));
    auto initFillTokensEndStr = fmt::format("{}", fmt::join(cValueTokens.end() - startEndEllipseValueCount, cValueTokens.end(), ","));
    updatedValStrm << fmt::format("|{} ... {}|", initFillTokensBeginStr, initFillTokensEndStr, numComps);
  }

  return updatedValStrm.str();
}

std::string CreateCompValsStr(const std::vector<std::string>& componentValuesStrs, usize numComps)
{
  std::vector<int64> componentValues;
  componentValues.reserve(componentValues.size());
  std::transform(componentValuesStrs.begin(), componentValuesStrs.end(), std::back_inserter(componentValues), CreateCompValFromStr);
  return CreateCompValsStr(componentValues, numComps);
}

void CreateFillPreflightVals(const std::string& initFillValueStr, usize numComps, std::vector<IFilter::PreflightValue>& preflightUpdatedValues)
{
  if(numComps <= 1)
  {
    return;
  }

  std::stringstream updatedValStrm;

  auto initFillTokens = StringUtilities::split(initFillValueStr, std::vector<char>{';'}, false);
  if(initFillTokens.size() == 1)
  {
    updatedValStrm << "Each tuple will contain the same values for all components: ";
  }
  else
  {
    updatedValStrm << "Each tuple will contain different values for all components: ";
  }

  updatedValStrm << CreateCompValsStr(initFillTokens, numComps);

  preflightUpdatedValues.push_back({"Tuple Details", updatedValStrm.str()});
};

void CreateIncrementalPreflightVals(const std::string& initFillValueStr, usize stepOperation, const std::string& stepValueStr, usize numTuples, usize numComps,
                                    std::vector<IFilter::PreflightValue>& preflightUpdatedValues)
{
  std::stringstream ss;

  auto initFillTokens = StringUtilities::split(initFillValueStr, std::vector<char>{';'}, false);
  auto stepValueTokens = StringUtilities::split(stepValueStr, ";", false);

  if(numComps > 1)
  {
    if(initFillTokens.size() == 1)
    {
      ss << "The first tuple will contain the same values for all components: ";
    }
    else
    {
      ss << "The first tuple will contain different values for all components: ";
    }

    ss << CreateCompValsStr(initFillTokens, numComps);

    if(stepOperation == StepType::Addition)
    {
      ss << fmt::format("\nThe components in each tuple will increment by the following: {}.", CreateCompValsStr(stepValueTokens, numComps));
    }
    else
    {
      ss << fmt::format("\nThe components in each tuple will decrement by the following: {}.", CreateCompValsStr(stepValueTokens, numComps));
    }
  }
  else if(stepOperation == StepType::Addition)
  {
    ss << fmt::format("\nThe single component tuples will increment by {}.", stepValueTokens[0]);
  }
  else
  {

    ss << fmt::format("\nThe single component tuples will decrement by {}.", stepValueTokens[0]);
  }

  std::vector<int64> initFillValues;
  initFillValues.reserve(initFillTokens.size());
  std::transform(initFillTokens.begin(), initFillTokens.end(), std::back_inserter(initFillValues), [](const std::string& s) -> int64 { return std::stoll(s); });
  std::vector<int64> stepValues;
  stepValues.reserve(stepValueTokens.size());
  std::transform(stepValueTokens.begin(), stepValueTokens.end(), std::back_inserter(stepValues), [](const std::string& s) -> int64 { return std::stoll(s); });

  ss << "\n\nTuples Preview:\n";
  const usize maxIterations = 3;
  usize actualIterations = std::min(numTuples, maxIterations);
  for(usize i = 0; i < actualIterations; ++i)
  {
    ss << fmt::format("{}\n", CreateCompValsStr(initFillValues, numComps));
    std::transform(initFillValues.begin(), initFillValues.end(), stepValues.begin(), initFillValues.begin(),
                   [stepOperation](int64 a, int64 b) { return (stepOperation == StepType::Addition) ? (a + b) : (a - b); });
  }
  if(numTuples > maxIterations)
  {
    ss << "...";
  }

  std::vector<usize> zeroIdx;
  for(usize i = 0; i < stepValueTokens.size(); i++)
  {
    if(stepValueTokens[i] == "0")
    {
      zeroIdx.push_back(i);
    }
  }
  if(!zeroIdx.empty())
  {

    ss << "\n\nWarning: Component(s) at index(es) " << fmt::format("[{}]", fmt::join(zeroIdx, ","))
       << " have a ZERO value for the step value.  The values at these component indexes will be unchanged from the starting value.";
  }

  preflightUpdatedValues.push_back({"Tuple Details", ss.str()});
}

void CreateRandomPreflightVals(bool standardizeSeed, InitializeType initType, const std::string& initStartRange, const std::string& initEndRange, usize numTuples, usize numComps,
                               std::vector<IFilter::PreflightValue>& preflightUpdatedValues)
{
  std::stringstream ss;

  if(numComps == 1)
  {
    if(initType == InitializeType::Random)
    {
      ss << fmt::format("The 1 component in each of the {} tuples will be filled with random values.", numTuples);
    }
    else if(initType == InitializeType::RangedRandom)
    {
      ss << fmt::format("The 1 component in each of the {} tuples will be filled with random values ranging from {} to {}.", numTuples, std::stoll(initStartRange), std::stoll(initEndRange));
    }

    if(standardizeSeed)
    {
      ss << "\n\nYou chose to standardize the seed for each component, but the array that will be created has a single component so it will not alter the randomization scheme.";
    }
  }
  else
  {
    if(initType == InitializeType::Random)
    {
      ss << fmt::format("All {} components in each of the {} tuples will be filled with random values.", numComps, numTuples);
    }
    else if(initType == InitializeType::RangedRandom)
    {
      ss << fmt::format("All {} components in each of the {} tuples will be filled with random values ranging from these starting values:", numComps, numTuples);
      auto startRangeTokens = StringUtilities::split(initStartRange, ";", false);
      ss << "\n" << CreateCompValsStr(startRangeTokens, numComps);
      ss << "\nto these ending values:";
      auto endRangeTokens = StringUtilities::split(initEndRange, ";", false);
      ss << "\n" << CreateCompValsStr(endRangeTokens, numComps);
    }

    if(standardizeSeed)
    {
      ss << "\n\nThis will generate THE SAME random value for all components in a given tuple, based on one seed.";
      ss << "\nFor example: |1,1,1| |9,9,9| |4,4,4| ...";
    }
    else
    {
      ss << "\n\nThis will generate DIFFERENT random values for each component in a given tuple, based on multiple seeds that are all modified versions of the original seed.";
      ss << "\nFor example: |1,9,5| |7,1,6| |2,12,7| ...";
    }
  }

  preflightUpdatedValues.push_back({"Tuple Details", ss.str()});
}
} // namespace core
} // namespace nx

// -----------------------------------------------------------------------------
InitializeData::InitializeData(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel, InitializeDataInputValues* inputValues)
: m_DataStructure(dataStructure)
, m_InputValues(inputValues)
, m_ShouldCancel(shouldCancel)
, m_MessageHandler(mesgHandler)
{
}

// -----------------------------------------------------------------------------
InitializeData::~InitializeData() noexcept = default;

// -----------------------------------------------------------------------------
const std::atomic_bool& InitializeData::getCancel()
{
  return m_ShouldCancel;
}

// -----------------------------------------------------------------------------
Result<> InitializeData::operator()()
{
  auto& iDataArray = m_DataStructure.getDataRefAs<IDataArray>(m_InputValues->InputArrayPath);
  const AlgorithmArrayTargets targets({&iDataArray});
  const bool usesOutOfCoreStore = AnyOutOfCore(targets);
  const bool useOutOfCorePath = !ForceInCoreAlgorithm() && (usesOutOfCoreStore || ForceOocAlgorithm());
  RecordAlgorithmPathExecution(useOutOfCorePath ? AlgorithmPath::OutOfCore : AlgorithmPath::InCore, usesOutOfCoreStore);
  return ExecuteDataFunction(::FillArrayFunctor{}, iDataArray.getDataType(), iDataArray, *m_InputValues, m_ShouldCancel);
}
