#include "ComputeArrayStatistics.hpp"

#include "simplnx/DataStructure/AttributeMatrix.hpp"
#include "simplnx/DataStructure/IDataStore.hpp"
#include "simplnx/DataStructure/IO/Generic/IExternalSort.hpp"
#include "simplnx/Utilities/AlgorithmDispatch.hpp"
#include "simplnx/Utilities/DataArrayUtilities.hpp"
#include "simplnx/Utilities/DataStoreUtilities.hpp"
#include "simplnx/Utilities/FilterUtilities.hpp"
#include "simplnx/Utilities/HistogramUtilities.hpp"
#include "simplnx/Utilities/MaskCompareUtilities.hpp"
#include "simplnx/Utilities/Math/StatisticsCalculations.hpp"
#include "simplnx/Utilities/MessageHelper.hpp"
#include "simplnx/Utilities/ParallelDataAlgorithm.hpp"

#include <nonstd/span.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

using namespace nx::core;

namespace
{
// -----------------------------------------------------------------------------
/**
 * @brief Checks whether all arrays in the set are backed by in-memory DataStores.
 * Used to decide if parallelization is safe: OOC stores are not thread-safe for
 * concurrent random access, so parallelization must be disabled when any array
 * resides out-of-core. Uses IDataStore::getStoreType() for an explicit check
 * rather than the legacy getDataFormat() string comparison.
 */
// -----------------------------------------------------------------------------
bool CheckArraysInMemory(const nx::core::IParallelAlgorithm::AlgorithmArrays& arrays)
{
  if(arrays.empty())
  {
    return true;
  }

  for(const auto* arrayPtr : arrays)
  {
    if(arrayPtr == nullptr)
    {
      continue;
    }

    if(arrayPtr->getIDataStoreRef().getStoreType() == nx::core::IDataStore::StoreType::OutOfCore)
    {
      return false;
    }
  }

  return true;
}

template <typename T>
class StatisticsByFeatureImpl
{
public:
  StatisticsByFeatureImpl(bool length, bool min, bool max, bool mean, bool mode, bool stdDeviation, bool summation, const std::unique_ptr<MaskCompareUtilities::MaskCompare>& mask,
                          const Int32AbstractDataStore& featureIds, const AbstractDataStore<T>& source, BoolArray* featureHasDataArray, UInt64Array* lengthArray, DataArray<T>* minArray,
                          DataArray<T>* maxArray, Float32Array* meanArray, NeighborList<T>* modeArray, Float32Array* stdDevArray, Float32Array* summationArray, const std::atomic_bool& shouldCancel,
                          MessageHelper& messageHelper)
  : m_Length(length)
  , m_Min(min)
  , m_Max(max)
  , m_Mean(mean)
  , m_Mode(mode)
  , m_StdDeviation(stdDeviation)
  , m_Summation(summation)
  , m_Mask(mask)
  , m_FeatureIds(featureIds)
  , m_Source(source)
  , m_FeatureHasDataArray(featureHasDataArray)
  , m_LengthArray(lengthArray)
  , m_MinArray(minArray)
  , m_MaxArray(maxArray)
  , m_MeanArray(meanArray)
  , m_ModeArray(modeArray)
  , m_StdDevArray(stdDevArray)
  , m_SummationArray(summationArray)
  , m_ShouldCancel(shouldCancel)
  , m_MessageHelper(messageHelper)
  {
  }

  void compute(usize start, usize end) const
  {
    ThrottledMessenger throttledMessenger = m_MessageHelper.createThrottledMessenger();

    const usize numTuples = m_FeatureIds.getNumberOfTuples();
    const usize numCurrentFeatures = end - start;

    auto msgHandler = [this](const std::string& msg) { m_MessageHelper.trySendMessage("Preparing features/ensembles for stats calculation " + msg); };
    auto [length, min, max, summation, modalMaps] = HistogramUtilities::concurrent::CalculateFeatureHasDataStats(m_Source, m_FeatureIds, start, end, m_Mask, msgHandler, m_ShouldCancel);
    if(m_ShouldCancel)
    {
      return;
    }

    usize progressCount = 0;
    usize progressIncrement = numCurrentFeatures / 100;

    m_MessageHelper.sendMessage(fmt::format("Calculating statistics for feature range [{}-{}]", start, end));

    std::vector<float32> meanArray;
    if(m_StdDeviation && !m_Mean)
    {
      meanArray.resize(numCurrentFeatures);
    }
    for(usize j = start; j < end; j++)
    {
      if(m_ShouldCancel)
      {
        return;
      }
      const usize localFeatureIndex = j - start;

      m_FeatureHasDataArray->initializeTuple(j, (length[localFeatureIndex] > 0));
      if(m_Length)
      {
        m_LengthArray->initializeTuple(j, length[localFeatureIndex]);
      }
      if(m_Min)
      {
        m_MinArray->initializeTuple(j, min[localFeatureIndex]);
      }
      if(m_Max)
      {
        m_MaxArray->initializeTuple(j, max[localFeatureIndex]);
      }
      if(m_Summation)
      {
        m_SummationArray->initializeTuple(j, summation[localFeatureIndex]);
      }

      float32 meanValue = 0.0f;
      if(length[localFeatureIndex] > 0)
      {
        if constexpr(std::is_same_v<T, bool>)
        {
          meanValue = static_cast<float32>(summation[localFeatureIndex] >= (numTuples - summation[localFeatureIndex]));
        }
        else
        {
          meanValue = summation[localFeatureIndex] / static_cast<float32>(length[localFeatureIndex]);
        }
      }

      if(m_Mean)
      {
        m_MeanArray->initializeTuple(j, meanValue);
      }
      else if(m_StdDeviation)
      {
        meanArray[localFeatureIndex] = meanValue;
      }

      if constexpr(!std::is_same_v<T, bool>)
      {
        if(m_Mode && !modalMaps[localFeatureIndex].empty())
        {
          // Find the maximum occurrence
          auto pr = std::max_element(modalMaps[localFeatureIndex].begin(), modalMaps[localFeatureIndex].end(), [](const auto& x, const auto& y) { return x.second < y.second; });
          int maxCount = pr->second;

          // Store all values that have this maximum occurrence under the proper feature id
          for(const auto& modalPair : modalMaps[localFeatureIndex])
          {
            if(modalPair.second == maxCount)
            {
              m_ModeArray->addEntry(j, modalPair.first);
            }
          }
        }
      }

      progressCount++;
      if(progressCount > progressIncrement)
      {
        throttledMessenger.sendThrottledMessage([&]() {
          progressCount = 0;
          return fmt::format("Calculating statistics for feature [{}-{}] {}/{}", start, end, j, end);
        });
      }
    }

    if(m_StdDeviation)
    {
      // https://www.khanacademy.org/math/statistics-probability/summarizing-quantitative-data/variance-standard-deviation-population/a/calculating-standard-deviation-step-by-step
      m_MessageHelper.sendMessage(fmt::format("Computing StdDev Feature/Ensemble [{}-{}]", start, end));
      // This should probably be done with Kahan Summation instead
      std::vector<float64> sumOfDiffs(numCurrentFeatures, 0.0f);
      progressCount = 0;

      for(usize tupleIndex = 0; tupleIndex < numTuples; tupleIndex++)
      {
        if(m_ShouldCancel)
        {
          return;
        }
        // Is the value in a mask and if so, is that mask TRUE
        if(m_Mask != nullptr && !m_Mask->isTrue(tupleIndex))
        {
          continue;
        }
        // Is the featureId within our range that we care about
        const int32 featureId = m_FeatureIds[tupleIndex];
        if(featureId < start || featureId >= end)
        {
          continue;
        }

        const float32 meanVal = m_Mean ? m_MeanArray->operator[](featureId) : meanArray[featureId - start];
        sumOfDiffs[featureId - start] += static_cast<float64>((m_Source[tupleIndex] - meanVal) * (m_Source[tupleIndex] - meanVal));

        progressCount++;
        if(progressCount > progressIncrement)
        {
          throttledMessenger.sendThrottledMessage([&]() {
            progressCount = 0;
            return fmt::format("StdDev Calculation Feature/Ensemble [{}-{}]: {:.2f}%", start, end, 100.0f * static_cast<float32>(tupleIndex) / static_cast<float32>(numTuples));
          });
        }
      }

      for(usize j = 0; j < numCurrentFeatures; j++)
      {
        // Set the value into the output array
        const uint64 lengthVal = m_Length ? m_LengthArray->operator[](j + start) : length[j];
        if(lengthVal > 0)
        {
          m_StdDevArray->operator[](j + start) = static_cast<float32>(std::sqrt(sumOfDiffs[j] / static_cast<float64>(lengthVal)));
        }
      }
    }

  } // end of compute

  void operator()(const Range& range) const
  {
    compute(range.min(), range.max());
  }

private:
  bool m_Length;
  bool m_Min;
  bool m_Max;
  bool m_Mean;
  bool m_Mode;
  bool m_StdDeviation;
  bool m_Summation;
  const std::unique_ptr<MaskCompareUtilities::MaskCompare>& m_Mask = nullptr;
  const Int32AbstractDataStore& m_FeatureIds;
  const AbstractDataStore<T>& m_Source;
  BoolArray* m_FeatureHasDataArray = nullptr;
  UInt64Array* m_LengthArray = nullptr;
  DataArray<T>* m_MinArray = nullptr;
  DataArray<T>* m_MaxArray = nullptr;
  Float32Array* m_MeanArray = nullptr;
  NeighborList<T>* m_ModeArray = nullptr;
  Float32Array* m_StdDevArray = nullptr;
  Float32Array* m_SummationArray = nullptr;
  const std::atomic_bool& m_ShouldCancel;
  MessageHelper& m_MessageHelper;
};

/// Number of cell/tuple elements read per bulk copyIntoBuffer() call when a range-based (non-"None")
/// feature statistic implementation streams sequentially through a cell-level array. Bounds per-chunk
/// memory to a small, fixed footprint regardless of total cell count -- required so memory use does
/// not scale with the dataset size for out-of-core stores -- while still allowing the whole array to
/// be visited in a single sequential pass. Matches the chunk size used elsewhere in this plugin (e.g.
/// ComputeFeatureCentroids.cpp).
constexpr usize k_ChunkTuples = 65536;

// -----------------------------------------------------------------------------
/**
 * @class StatisticsByFeatureRangeImpl
 * @brief Computes length/min/max/mean/summation/mode/standard-deviation per feature for a contiguous
 * feature-id range [start, start+numFeatures) using a single sequential pass over the cell-level
 * source, feature-id, and (pre-combined mask+range) mask arrays.
 *
 * @section why Why single-pass bucketing replaces the per-feature rescan
 * The previous implementation looped over each FEATURE and, for every feature, rescanned every cell
 * in the array to find that feature's members -- O(numFeatures x numCells) cell-array traversal, plus
 * a second full rescan again for standard deviation. Since each cell's own feature id is available
 * directly from the feature-id array, every cell can instead be routed to its own feature's
 * accumulators in a single scan, reducing the cost to O(numCells) (or O(2 x numCells) when standard
 * deviation is requested, since the variance calculation needs each feature's mean computed first --
 * that mean cannot be known until the first pass completes, so it is inherently a second pass rather
 * than something foldable into the first). Cell-level arrays are read in bounded chunks via
 * copyIntoBuffer so per-chunk memory stays fixed rather than scaling with the total cell count, which
 * out-of-core arrays require. The per-feature accumulators themselves scale with numFeatures, which is
 * feature-scale (typically thousands) rather than cell-scale (millions to billions).
 */
template <typename T>
class StatisticsByFeatureRangeImpl
{
public:
  StatisticsByFeatureRangeImpl(bool length, bool min, bool max, bool mean, bool mode, bool stdDeviation, bool summation, const std::vector<usize>& featureIdToCompactIndex,
                               const std::unique_ptr<MaskCompareUtilities::MaskCompare>& mask, const Int32AbstractDataStore& featureIds, const AbstractDataStore<T>& source, int32 start,
                               usize numFeatures, BoolArray* featureHasDataArray, UInt64Array* lengthArray, DataArray<T>* minArray, DataArray<T>* maxArray, Float32Array* meanArray,
                               NeighborList<T>* modeArray, Float32Array* stdDevArray, Float32Array* summationArray, const std::atomic_bool& shouldCancel)
  : m_Length(length)
  , m_Min(min)
  , m_Max(max)
  , m_Mean(mean)
  , m_Mode(mode)
  , m_StdDeviation(stdDeviation)
  , m_Summation(summation)
  , m_FeatureIdToCompactIndex(featureIdToCompactIndex)
  , m_Mask(mask)
  , m_FeatureIds(featureIds)
  , m_Source(source)
  , m_Start(start)
  , m_NumFeatures(numFeatures)
  , m_FeatureHasDataArray(featureHasDataArray)
  , m_LengthArray(lengthArray)
  , m_MinArray(minArray)
  , m_MaxArray(maxArray)
  , m_MeanArray(meanArray)
  , m_ModeArray(modeArray)
  , m_StdDevArray(stdDevArray)
  , m_SummationArray(summationArray)
  , m_ShouldCancel(shouldCancel)
  {
  }

  /**
   * @brief Runs the single-pass (two-pass when standard deviation is requested) per-feature statistics
   * calculation for the whole [start, start+numFeatures) range and writes the results directly into
   * the destination arrays.
   * @return An invalid Result if a bulk read from a cell-level array fails.
   */
  Result<> operator()() const
  {
    const usize numTuples = m_Source.getNumberOfTuples();

    // Per-feature accumulators -- O(numFeatures) memory, feature-scale rather than cell-scale. T-typed
    // accumulators use make_unique<T[]> rather than std::vector<T> because T may be `bool`, whose
    // std::vector specialization bit-packs storage and does not support the compound assignment
    // (operator+=) used below.
    std::vector<usize> counts(m_NumFeatures, 0);
    auto minValues = std::make_unique<T[]>(m_NumFeatures);
    auto maxValues = std::make_unique<T[]>(m_NumFeatures);
    auto sums = std::make_unique<T[]>(m_NumFeatures);
    for(usize k = 0; k < m_NumFeatures; k++)
    {
      minValues[k] = std::numeric_limits<T>::max();
      if constexpr(std::is_floating_point_v<T>)
      {
        maxValues[k] = -std::numeric_limits<T>::max();
      }
      else
      {
        maxValues[k] = std::numeric_limits<T>::min();
      }
      sums[k] = static_cast<T>(0);
    }
    std::vector<std::map<T, uint64>> modalMaps(m_Mode ? m_NumFeatures : 0);

    auto featureIdBuf = std::make_unique<int32[]>(k_ChunkTuples);
    auto sourceBuf = std::make_unique<T[]>(k_ChunkTuples);

    // Pass 1: bucket length/min/max/summation/(mode) per feature in a single sweep of the cells.
    for(usize offset = 0; offset < numTuples; offset += k_ChunkTuples)
    {
      if(m_ShouldCancel)
      {
        return {};
      }
      const usize chunkCount = std::min(k_ChunkTuples, numTuples - offset);

      Result<> featureIdResult = m_FeatureIds.copyIntoBuffer(offset, nonstd::span<int32>(featureIdBuf.get(), chunkCount));
      if(featureIdResult.invalid())
      {
        return featureIdResult;
      }
      Result<> sourceResult = m_Source.copyIntoBuffer(offset, nonstd::span<T>(sourceBuf.get(), chunkCount));
      if(sourceResult.invalid())
      {
        return sourceResult;
      }

      for(usize idx = 0; idx < chunkCount; idx++)
      {
        const int32 featureId = featureIdBuf[idx];
        if((m_Mask != nullptr && !m_Mask->isTrue(offset + idx)) || featureId < m_Start || static_cast<int64>(featureId) >= static_cast<int64>(m_Start) + static_cast<int64>(m_NumFeatures))
        {
          continue;
        }
        const usize compactIndex = m_FeatureIdToCompactIndex[static_cast<usize>(static_cast<int64>(featureId) - static_cast<int64>(m_Start))];
        const T val = sourceBuf[idx];

        counts[compactIndex]++;
        if(val < minValues[compactIndex])
        {
          minValues[compactIndex] = val;
        }
        if(val > maxValues[compactIndex])
        {
          maxValues[compactIndex] = val;
        }
        sums[compactIndex] += val;
        if(m_Mode)
        {
          modalMaps[compactIndex][val]++;
        }
      }
    }

    // Per-feature mean is always computed locally (standard deviation below needs it) but only
    // written to the destination array when Mean was actually requested.
    std::vector<float32> meanValues(m_NumFeatures, 0.0f);
    for(usize k = 0; k < m_NumFeatures; k++)
    {
      if(counts[k] == 0)
      {
        continue;
      }
      if constexpr(std::is_same_v<T, bool>)
      {
        meanValues[k] = static_cast<float32>(sums[k] >= (numTuples - sums[k]));
      }
      else
      {
        meanValues[k] = static_cast<float32>(sums[k]) / static_cast<float32>(counts[k]);
      }
    }

    // Pass 2: standard deviation needs each feature's mean already known, so it requires its own
    // second sweep of the cells -- the variance calculation cannot be folded into pass 1.
    std::vector<float64> sumOfDiffs(m_StdDeviation ? m_NumFeatures : 0, 0.0);
    if(m_StdDeviation)
    {
      // https://www.khanacademy.org/math/statistics-probability/summarizing-quantitative-data/variance-standard-deviation-population/a/calculating-standard-deviation-step-by-step
      for(usize offset = 0; offset < numTuples; offset += k_ChunkTuples)
      {
        if(m_ShouldCancel)
        {
          return {};
        }
        const usize chunkCount = std::min(k_ChunkTuples, numTuples - offset);

        Result<> featureIdResult = m_FeatureIds.copyIntoBuffer(offset, nonstd::span<int32>(featureIdBuf.get(), chunkCount));
        if(featureIdResult.invalid())
        {
          return featureIdResult;
        }
        Result<> sourceResult = m_Source.copyIntoBuffer(offset, nonstd::span<T>(sourceBuf.get(), chunkCount));
        if(sourceResult.invalid())
        {
          return sourceResult;
        }

        for(usize idx = 0; idx < chunkCount; idx++)
        {
          const int32 featureId = featureIdBuf[idx];
          if((m_Mask != nullptr && !m_Mask->isTrue(offset + idx)) || featureId < m_Start || static_cast<int64>(featureId) >= static_cast<int64>(m_Start) + static_cast<int64>(m_NumFeatures))
          {
            continue;
          }
          const usize compactIndex = m_FeatureIdToCompactIndex[static_cast<usize>(static_cast<int64>(featureId) - static_cast<int64>(m_Start))];
          const float32 meanVal = meanValues[compactIndex];
          sumOfDiffs[compactIndex] += static_cast<float64>((sourceBuf[idx] - meanVal) * (sourceBuf[idx] - meanVal));
        }
      }
    }

    // Write results. Only features with count > 0 receive computed statistics; empty features keep
    // the sentinel fill values InitializeArrays() applied before this class was invoked, matching the
    // previous per-feature implementation, which likewise skipped writes for empty features.
    for(usize k = 0; k < m_NumFeatures; k++)
    {
      if(m_ShouldCancel)
      {
        return {};
      }
      const bool hasData = counts[k] > 0;
      m_FeatureHasDataArray->initializeTuple(k, hasData);
      if(!hasData)
      {
        continue;
      }

      if(m_Length)
      {
        m_LengthArray->initializeTuple(k, static_cast<uint64>(counts[k]));
      }
      if(m_Min)
      {
        m_MinArray->initializeTuple(k, minValues[k]);
      }
      if(m_Max)
      {
        m_MaxArray->initializeTuple(k, maxValues[k]);
      }
      if(m_Summation)
      {
        m_SummationArray->initializeTuple(k, sums[k]);
      }
      if(m_Mean)
      {
        m_MeanArray->initializeTuple(k, meanValues[k]);
      }
      if constexpr(!std::is_same_v<T, bool>)
      {
        if(m_Mode && !modalMaps[k].empty())
        {
          // Find the maximum occurrence
          auto pr = std::max_element(modalMaps[k].begin(), modalMaps[k].end(), [](const auto& x, const auto& y) { return x.second < y.second; });
          const uint64 maxCount = pr->second;

          // Store all values that have this maximum occurrence under the proper feature id
          for(const auto& modalPair : modalMaps[k])
          {
            if(modalPair.second == maxCount)
            {
              m_ModeArray->addEntry(k, modalPair.first);
            }
          }
        }
      }
      if(m_StdDeviation)
      {
        m_StdDevArray->setValue(k, static_cast<float32>(std::sqrt(sumOfDiffs[k] / static_cast<float64>(counts[k]))));
      }
    }

    return {};
  }

private:
  bool m_Length;
  bool m_Min;
  bool m_Max;
  bool m_Mean;
  bool m_Mode;
  bool m_StdDeviation;
  bool m_Summation;
  const std::vector<usize>& m_FeatureIdToCompactIndex;
  const std::unique_ptr<MaskCompareUtilities::MaskCompare>& m_Mask;
  const Int32AbstractDataStore& m_FeatureIds;
  const AbstractDataStore<T>& m_Source;
  int32 m_Start;
  usize m_NumFeatures;
  BoolArray* m_FeatureHasDataArray = nullptr;
  UInt64Array* m_LengthArray = nullptr;
  DataArray<T>* m_MinArray = nullptr;
  DataArray<T>* m_MaxArray = nullptr;
  Float32Array* m_MeanArray = nullptr;
  NeighborList<T>* m_ModeArray = nullptr;
  Float32Array* m_StdDevArray = nullptr;
  Float32Array* m_SummationArray = nullptr;
  const std::atomic_bool& m_ShouldCancel;
};

template <typename T>
class MedianByFeatureImpl
{
public:
  MedianByFeatureImpl(const std::unique_ptr<MaskCompareUtilities::MaskCompare>& mask, const Int32AbstractDataStore& featureIds, const AbstractDataStore<T>& source, bool findMedian, bool findNumUnique,
                      Float32Array* medianArray, Int32Array* numUniqueValuesArray, DataArray<uint64>* lengthArray, MessageHelper& messageHelper)
  : m_FindMedian(findMedian)
  , m_FindNumUniqueValues(findNumUnique)
  , m_MedianArray(medianArray)
  , m_NumUniqueValuesArray(numUniqueValuesArray)
  , m_Mask(mask)
  , m_FeatureIds(featureIds)
  , m_Source(source)
  , m_LengthArray(lengthArray)
  , m_MessageHelper(messageHelper)
  {
  }

  void compute(usize start, usize end) const
  {
    m_MessageHelper.sendMessage(fmt::format("Starting Median Array Calculation: Feature/Ensemble [{}-{}]", start, end));

    const usize numFeatureSources = end - start;
    // Create the arrays that will collect the values from the arrays. allocate them to the correct size based on the length array
    std::vector<std::vector<T>> featureSources(numFeatureSources);
    for(usize featureSourceIndex = 0; featureSourceIndex < numFeatureSources; featureSourceIndex++)
    {
      if(m_LengthArray != nullptr)
      {
        featureSources[featureSourceIndex].reserve(m_LengthArray->operator[](featureSourceIndex + start));
      }
    }
    const usize numTuples = m_Source.getNumberOfTuples();

    for(usize tupleIndex = 0; tupleIndex < numTuples; tupleIndex++)
    {
      // Is the value in a mask and if so, is that mask TRUE
      if(m_Mask != nullptr && !m_Mask->isTrue(tupleIndex))
      {
        continue;
      }
      // Is the featureId within our range that we care about
      const int32 featureId = m_FeatureIds[tupleIndex];
      if(featureId < start || featureId >= end)
      {
        continue;
      }
      featureSources[featureId - start].push_back(m_Source[tupleIndex]);
    }

    for(usize featureSourceIndex = 0; featureSourceIndex < numFeatureSources; featureSourceIndex++)
    {
      if(m_FindMedian)
      {
        const float32 val = StatisticsCalculations::findMedian(featureSources[featureSourceIndex]);
        m_MedianArray->setValue(featureSourceIndex + start, val);
      }
      if(m_FindNumUniqueValues)
      {
        const auto val = StatisticsCalculations::findNumUniqueValues(featureSources[featureSourceIndex]);
        m_NumUniqueValuesArray->setValue(featureSourceIndex + start, val);
      }
    }
  }

  void operator()(const Range& range) const
  {
    compute(range.min(), range.max());
  }

private:
  bool m_FindMedian;
  bool m_FindNumUniqueValues;
  Float32Array* m_MedianArray;
  Int32Array* m_NumUniqueValuesArray;
  const std::unique_ptr<MaskCompareUtilities::MaskCompare>& m_Mask = nullptr;
  const Int32AbstractDataStore& m_FeatureIds;
  const AbstractDataStore<T>& m_Source;
  const DataArray<uint64>* m_LengthArray = nullptr;
  MessageHelper& m_MessageHelper;
};

// -----------------------------------------------------------------------------
/**
 * @class MedianByFeatureRangeImpl
 * @brief Computes median and/or number-of-unique-values per feature for a contiguous feature-id range
 * using a single sequential pass over the cell-level source, feature-id, and (pre-combined mask+range)
 * mask arrays that buckets each in-range cell's value into its own feature's value list, instead of
 * rescanning every cell once per feature (see StatisticsByFeatureRangeImpl for the general rationale).
 *
 * @note Exact median requires the complete sorted set of values assigned to each feature, so this
 * class necessarily holds all in-range values in per-feature buffers whose combined size is
 * O(numCellsInRange) -- proportional to the number of in-range cells, not to numFeatures. This is
 * inherent to computing an exact (rather than approximate/binned) median and is not eliminated by the
 * single-pass bucketing change; bucketing only reduces the number of full cell-array traversals from
 * O(numFeatures) to O(1).
 */
template <typename T>
class MedianByFeatureRangeImpl
{
public:
  MedianByFeatureRangeImpl(const std::vector<usize>& featureIdToCompactIndex, const std::unique_ptr<MaskCompareUtilities::MaskCompare>& mask, const Int32AbstractDataStore& featureIds,
                           const AbstractDataStore<T>& source, int32 start, usize numFeatures, bool findMedian, bool findNumUnique, Float32Array* medianArray, Int32Array* numUniqueValuesArray,
                           const std::atomic_bool& shouldCancel)
  : m_FindMedian(findMedian)
  , m_FindNumUniqueValues(findNumUnique)
  , m_MedianArray(medianArray)
  , m_NumUniqueValuesArray(numUniqueValuesArray)
  , m_FeatureIdToCompactIndex(featureIdToCompactIndex)
  , m_Mask(mask)
  , m_FeatureIds(featureIds)
  , m_Source(source)
  , m_Start(start)
  , m_NumFeatures(numFeatures)
  , m_ShouldCancel(shouldCancel)
  {
  }

  /**
   * @brief Runs the single-pass median/unique-value bucketing for the whole [start, start+numFeatures)
   * range and writes the results directly into the destination arrays.
   * @return An invalid Result if a bulk read from a cell-level array fails.
   */
  Result<> operator()() const
  {
    const usize numTuples = m_Source.getNumberOfTuples();
    // Per-feature value buffers. Their combined size scales with the number of in-range cells (see
    // class doc comment) -- this is inherent to exact median/unique-value computation, not something
    // this pass introduces.
    std::vector<std::vector<T>> perFeatureValues(m_FindMedian ? m_NumFeatures : 0);
    std::vector<std::set<T>> perFeatureUniqueValues(m_FindNumUniqueValues ? m_NumFeatures : 0);

    auto featureIdBuf = std::make_unique<int32[]>(k_ChunkTuples);
    auto sourceBuf = std::make_unique<T[]>(k_ChunkTuples);

    for(usize offset = 0; offset < numTuples; offset += k_ChunkTuples)
    {
      if(m_ShouldCancel)
      {
        return {};
      }
      const usize chunkCount = std::min(k_ChunkTuples, numTuples - offset);

      Result<> featureIdResult = m_FeatureIds.copyIntoBuffer(offset, nonstd::span<int32>(featureIdBuf.get(), chunkCount));
      if(featureIdResult.invalid())
      {
        return featureIdResult;
      }
      Result<> sourceResult = m_Source.copyIntoBuffer(offset, nonstd::span<T>(sourceBuf.get(), chunkCount));
      if(sourceResult.invalid())
      {
        return sourceResult;
      }

      for(usize idx = 0; idx < chunkCount; idx++)
      {
        const int32 featureId = featureIdBuf[idx];
        if((m_Mask != nullptr && !m_Mask->isTrue(offset + idx)) || featureId < m_Start || static_cast<int64>(featureId) >= static_cast<int64>(m_Start) + static_cast<int64>(m_NumFeatures))
        {
          continue;
        }
        const usize compactIndex = m_FeatureIdToCompactIndex[static_cast<usize>(static_cast<int64>(featureId) - static_cast<int64>(m_Start))];
        if(m_FindMedian)
        {
          perFeatureValues[compactIndex].push_back(sourceBuf[idx]);
        }
        if(m_FindNumUniqueValues)
        {
          perFeatureUniqueValues[compactIndex].emplace(sourceBuf[idx]);
        }
      }
    }

    for(usize k = 0; k < m_NumFeatures; k++)
    {
      if(m_ShouldCancel)
      {
        return {};
      }
      if(m_FindMedian)
      {
        auto& values = perFeatureValues[k];
        if(values.empty())
        {
          m_MedianArray->setValue(k, 0.0f);
        }
        else
        {
          std::sort(values.begin(), values.end());
          if(values.size() % 2 == 1)
          {
            const usize halfElements = static_cast<usize>(std::floor(values.size() / 2.0f));
            m_MedianArray->setValue(k, values[halfElements]);
          }
          else
          {
            const usize idxLow = (values.size() / 2) - 1;
            const usize idxHigh = values.size() / 2;
            m_MedianArray->setValue(k, (values[idxLow] + values[idxHigh]) * 0.5f);
          }
        }
      }
      if(m_FindNumUniqueValues)
      {
        m_NumUniqueValuesArray->setValue(k, static_cast<int32>(perFeatureUniqueValues[k].size()));
      }
    }

    return {};
  }

private:
  bool m_FindMedian;
  bool m_FindNumUniqueValues;
  Float32Array* m_MedianArray;
  Int32Array* m_NumUniqueValuesArray;
  const std::vector<usize>& m_FeatureIdToCompactIndex;
  const std::unique_ptr<MaskCompareUtilities::MaskCompare>& m_Mask;
  const Int32AbstractDataStore& m_FeatureIds;
  const AbstractDataStore<T>& m_Source;
  int32 m_Start;
  usize m_NumFeatures;
  const std::atomic_bool& m_ShouldCancel;
};

// -----------------------------------------------------------------------------
template <class ContainerType, typename T>
Result<> FindStatisticsImpl(const ContainerType& data, std::vector<IArray*>& arrays, const ComputeArrayStatisticsInputValues* inputValues)
{
  if(inputValues->FindLength)
  {
    auto* array0Ptr = dynamic_cast<DataArray<uint64>*>(arrays[0]);
    if(array0Ptr == nullptr)
    {
      return MakeErrorResult(-563501, fmt::format("Could not cast 'Length' array at path '{}' to UInt64Array.", inputValues->LengthArrayName.toString()));
    }
    const auto val = static_cast<uint64>(data.size());
    array0Ptr->initializeTuple(0, val);
  }

  // If we are finding the min or the max (or both) just combine that into a single call
  if(inputValues->FindMin || inputValues->FindMax)
  {
    const std::pair<T, T> minMaxValues = StatisticsCalculations::FindMinMax(data);
    if(inputValues->FindMin)
    {
      auto* array1Ptr = dynamic_cast<DataArray<T>*>(arrays[1]);
      if(array1Ptr == nullptr)
      {
        return MakeErrorResult(-563501, fmt::format("Could not cast 'Minimum' array at path '{}' to the expected type.", inputValues->MinimumArrayName.toString()));
      }
      array1Ptr->initializeTuple(0, minMaxValues.first);
    }
    if(inputValues->FindMax)
    {
      auto* array2Ptr = dynamic_cast<DataArray<T>*>(arrays[2]);
      if(array2Ptr == nullptr)
      {
        return MakeErrorResult(-563501, fmt::format("Could not cast 'Maximum' array at path '{}' to the expected type.", inputValues->MaximumArrayName.toString()));
      }
      array2Ptr->initializeTuple(0, minMaxValues.second);
    }
  }

  // Finding the mean depends on the summation.
  if(inputValues->FindSummation || inputValues->FindMean || inputValues->FindStdDeviation)
  {
    const std::pair<float32, float32> sumMeanValues = StatisticsCalculations::FindSumMean(data);
    if(inputValues->FindSummation)
    {
      auto* array6Ptr = dynamic_cast<Float32Array*>(arrays[7]);
      if(array6Ptr == nullptr)
      {
        return MakeErrorResult(-563501, fmt::format("Could not cast 'Summation' array at path '{}' to Float32Array.", inputValues->SummationArrayName.toString()));
      }
      array6Ptr->initializeTuple(0, sumMeanValues.first);
    }
    if(inputValues->FindMean)
    {
      auto* array3Ptr = dynamic_cast<Float32Array*>(arrays[3]);
      if(array3Ptr == nullptr)
      {
        return MakeErrorResult(-563501, fmt::format("Could not cast 'Mean' array at path '{}' to Float32Array.", inputValues->MeanArrayName.toString()));
      }
      array3Ptr->initializeTuple(0, sumMeanValues.second);
    }
    if(inputValues->FindStdDeviation)
    {
      auto* array5Ptr = dynamic_cast<Float32Array*>(arrays[6]);
      if(array5Ptr == nullptr)
      {
        return MakeErrorResult(-563501, fmt::format("Could not cast 'Standard Deviation' array at path '{}' to Float32Array.", inputValues->StdDeviationArrayName.toString()));
      }
      const float32 val = StatisticsCalculations::FindStdDeviation(data, sumMeanValues);
      array5Ptr->initializeTuple(0, val);
    }
  }

  if(inputValues->FindMedian)
  {
    auto* array4Ptr = dynamic_cast<Float32Array*>(arrays[4]);
    if(array4Ptr == nullptr)
    {
      throw std::invalid_argument(fmt::format("Could not cast 'Median' array at path '{}' to Float32Array.", inputValues->MedianArrayName.toString()));
    }
    const float32 val = StatisticsCalculations::findMedian(data);
    array4Ptr->initializeTuple(0, val);
  }

  if constexpr(!std::is_same_v<T, bool>)
  {
    if(inputValues->FindMode)
    {
      auto* array5Ptr = dynamic_cast<NeighborList<T>*>(arrays[5]);
      if(array5Ptr == nullptr)
      {
        throw std::invalid_argument(fmt::format("Could not cast 'Mode' array at path '{}' to the expected NeighborList type.", inputValues->ModeArrayName.toString()));
      }
      std::vector<T> modes = StatisticsCalculations::findModes(data);
      for(const auto& mode : modes)
      {
        array5Ptr->addEntry(0, mode);
      }
    }
  }

  if(inputValues->FindNumUniqueValues)
  {
    auto* array8Ptr = dynamic_cast<DataArray<int32>*>(arrays[8]);
    if(array8Ptr == nullptr)
    {
      throw std::invalid_argument(fmt::format("Could not cast 'Number of Unique Values' array at path '{}' to Int32Array.", inputValues->NumUniqueValuesName.toString()));
    }
    const auto val = static_cast<int32>(StatisticsCalculations::findNumUniqueValues(data));
    array8Ptr->initializeTuple(0, val);
  }

  return {};
}

// -----------------------------------------------------------------------------
template <typename T>
Result<> InitializeArrays(DataStructure& dataStructure, const ComputeArrayStatisticsInputValues* inputValues)
{
  using InputDataArrayType = DataArray<T>;
  // Need to initialize the output data arrays
  if(inputValues->ComputeByIndex)
  {
    auto* arrayPtr = dataStructure.getDataAs<BoolArray>(inputValues->FeatureHasDataArrayName);
    if(arrayPtr == nullptr)
    {
      return MakeErrorResult(-563502, fmt::format("Could not find or cast 'Feature Has Data' array at path '{}' to BoolArray.", inputValues->FeatureHasDataArrayName.toString()));
    }
    arrayPtr->fill(false);
  }
  if(inputValues->FindLength)
  {
    auto* arrayPtr = dataStructure.getDataAs<UInt64Array>(inputValues->LengthArrayName);
    if(arrayPtr == nullptr)
    {
      return MakeErrorResult(-563503, fmt::format("Could not find or cast 'Length' array at path '{}' to UInt64Array.", inputValues->LengthArrayName.toString()));
    }
    arrayPtr->fill(0ULL);
  }
  if(inputValues->FindMin)
  {
    auto* arrayPtr = dataStructure.getDataAs<InputDataArrayType>(inputValues->MinimumArrayName);
    if(arrayPtr == nullptr)
    {
      return MakeErrorResult(-563504, fmt::format("Could not find or cast 'Minimum' array at path '{}' to the expected type.", inputValues->MinimumArrayName.toString()));
    }
    arrayPtr->fill(static_cast<T>(std::numeric_limits<T>::max()));
  }
  if(inputValues->FindMax)
  {
    auto* arrayPtr = dataStructure.getDataAs<InputDataArrayType>(inputValues->MaximumArrayName);
    if(arrayPtr == nullptr)
    {
      return MakeErrorResult(-563505, fmt::format("Could not find or cast 'Maximum' array at path '{}' to the expected type.", inputValues->MaximumArrayName.toString()));
    }
    arrayPtr->fill(static_cast<T>(std::numeric_limits<T>::min()));
  }
  if(inputValues->FindMean)
  {
    auto* arrayPtr = dataStructure.getDataAs<Float32Array>(inputValues->MeanArrayName);
    if(arrayPtr == nullptr)
    {
      return MakeErrorResult(-563506, fmt::format("Could not find or cast 'Mean' array at path '{}' to Float32Array.", inputValues->MeanArrayName.toString()));
    }
    arrayPtr->fill(0.0F);
  }
  if(inputValues->FindMedian)
  {
    auto* arrayPtr = dataStructure.getDataAs<Float32Array>(inputValues->MedianArrayName);
    if(arrayPtr == nullptr)
    {
      return MakeErrorResult(-563507, fmt::format("Could not find or cast 'Median' array at path '{}' to Float32Array.", inputValues->MedianArrayName.toString()));
    }
    arrayPtr->fill(0.0F);
  }
  if(inputValues->FindStdDeviation)
  {
    auto* arrayPtr = dataStructure.getDataAs<Float32Array>(inputValues->StdDeviationArrayName);
    if(arrayPtr == nullptr)
    {
      return MakeErrorResult(-563509, fmt::format("Could not find or cast 'Standard Deviation' array at path '{}' to Float32Array.", inputValues->StdDeviationArrayName.toString()));
    }
    arrayPtr->fill(0.0F);
  }
  if(inputValues->FindSummation)
  {
    auto* arrayPtr = dataStructure.getDataAs<Float32Array>(inputValues->SummationArrayName);
    if(arrayPtr == nullptr)
    {
      return MakeErrorResult(-563510, fmt::format("Could not find or cast 'Summation' array at path '{}' to Float32Array.", inputValues->SummationArrayName.toString()));
    }
    arrayPtr->fill(0.0F);
  }
  if(inputValues->FindNumUniqueValues)
  {
    auto* arrayPtr = dataStructure.getDataAs<Int32Array>(inputValues->NumUniqueValuesName);
    if(arrayPtr == nullptr)
    {
      return MakeErrorResult(-563513, fmt::format("Could not find or cast 'Number of Unique Values' array at path '{}' to Int32Array.", inputValues->NumUniqueValuesName.toString()));
    }
    arrayPtr->fill(-1);
  }
  // End Initialization

  return {};
}

// -----------------------------------------------------------------------------
struct ComputeArrayStatisticsFunctor
{
  template <typename T>
  Result<> operator()(DataStructure& dataStructure, const IDataArray& inputIDataArray, std::vector<IArray*>& arrays, const ComputeArrayStatisticsInputValues* inputValues)
  {
    std::unique_ptr<MaskCompareUtilities::MaskCompare> maskCompare = nullptr;
    if(inputValues->UseMask)
    {
      try
      {
        maskCompare = MaskCompareUtilities::InstantiateMaskCompare(dataStructure, inputValues->MaskArrayPath);
      } catch(const std::out_of_range& exception)
      {
        // This really should NOT be happening as the path was verified during preflight BUT we may be calling this from
        // somewhere else that is NOT going through the normal nx::core::IFilter API of Preflight and Execute
        const std::string message = fmt::format("Mask Array DataPath does not exist or is not of the correct type (Bool | UInt8) {}", inputValues->MaskArrayPath.toString());
        return MakeErrorResult(-563501, message);
      }
    }

    Result<> initializationResult = InitializeArrays<T>(dataStructure, inputValues);
    if(initializationResult.invalid())
    {
      return initializationResult;
    }

    const auto& inputArray = static_cast<const DataArray<T>&>(inputIDataArray);
    // this level checks whether computing by index or not and preps the calculations accordingly
    if(inputValues->UseMask)
    {
      // This section extracts out the data into a separate storage class. Note that
      // this could get real ugly for an out-of-core DataArray
      const usize numTuples = inputArray.getNumberOfTuples();
      std::vector<T> data;
      data.reserve(numTuples);
      for(usize i = 0; i < numTuples; i++)
      {
        if(maskCompare->isTrue(i))
        {
          data.push_back(inputArray[i]);
        }
      }
      data.shrink_to_fit();
      // compute the statistics for the entire array
      Result<> result = FindStatisticsImpl<std::vector<T>, T>(data, arrays, inputValues);
      if(result.invalid())
      {
        return result;
      }
    }
    else
    {
      // compute the statistics for the entire array
      Result<> result = FindStatisticsImpl<DataArray<T>, T>(inputArray, arrays, inputValues);
      if(result.invalid())
      {
        return result;
      }
    }

    // compute the standardized data based on whether computing by index or not
    if(inputValues->StandardizeData)
    {
      const auto& mean = dataStructure.getDataRefAs<Float32Array>(inputValues->MeanArrayName).getDataStoreRef();
      const auto& std = dataStructure.getDataRefAs<Float32Array>(inputValues->StdDeviationArrayName).getDataStoreRef();
      auto& standardized = dataStructure.getDataRefAs<Float32Array>(inputValues->StandardizedArrayName).getDataStoreRef();
      auto& data = inputArray.getDataStoreRef();

      const usize numTuples = data.getNumberOfTuples();

      for(usize i = 0; i < numTuples; i++)
      {
        if(!inputValues->UseMask || maskCompare->isTrue(i))
        {
          standardized.setValue(i, (static_cast<float32>(data[i]) - mean[0]) / std[0]);
        }
      }
    }
    return {};
  }
};

// -----------------------------------------------------------------------------
struct ComputeArrayStatisticsByFeatureFunctor
{
  template <typename T>
  Result<> operator()(DataStructure& dataStructure, const IDataArray* inputIDataArray, std::vector<IArray*>& arrays, usize numFeatures, const ComputeArrayStatisticsInputValues* inputValues,
                      const std::atomic_bool& shouldCancel, MessageHelper& messageHelper)
  {
    std::unique_ptr<MaskCompareUtilities::MaskCompare> maskCompare = nullptr;
    if(inputValues->UseMask)
    {
      try
      {
        maskCompare = MaskCompareUtilities::InstantiateMaskCompare(dataStructure, inputValues->MaskArrayPath);
      } catch(const std::out_of_range& exception)
      {
        // This really should NOT be happening as the path was verified during preflight BUT we may be calling this from
        // somewhere else that is NOT going through the normal nx::core::IFilter API of Preflight and Execute
        const std::string message = fmt::format("Mask Array DataPath does not exist or is not of the correct type (Bool | UInt8) {}", inputValues->MaskArrayPath.toString());
        return MakeErrorResult(-563508, message);
      }
    }

    Result<> initializationResult = InitializeArrays<T>(dataStructure, inputValues);
    if(initializationResult.invalid())
    {
      return initializationResult;
    }

    // this level preps and preforms the calculations accordingly
    const auto* inputArrayPtr = static_cast<const DataArray<T>*>(inputIDataArray);
    const auto* featureIdsPtr = dataStructure.getDataAs<Int32Array>(inputValues->FeatureIdsArrayPath);
    auto* lengthArrayPtr = dynamic_cast<DataArray<uint64>*>(arrays[0]);
    auto* minArrayPtr = dynamic_cast<DataArray<T>*>(arrays[1]);
    auto* maxArrayPtr = dynamic_cast<DataArray<T>*>(arrays[2]);
    auto* meanArrayPtr = dynamic_cast<Float32Array*>(arrays[3]);
    auto* modeArrayPtr = dynamic_cast<NeighborList<T>*>(arrays[5]);
    auto* stdDevArrayPtr = dynamic_cast<Float32Array*>(arrays[6]);
    auto* summationArrayPtr = dynamic_cast<Float32Array*>(arrays[7]);

    auto* featureHasDataPtr = dynamic_cast<BoolArray*>(arrays[9]);

    IParallelAlgorithm::AlgorithmArrays indexAlgArrays;
    indexAlgArrays.push_back(inputArrayPtr);
    indexAlgArrays.push_back(featureHasDataPtr);
    indexAlgArrays.push_back(lengthArrayPtr);
    indexAlgArrays.push_back(minArrayPtr);
    indexAlgArrays.push_back(maxArrayPtr);
    indexAlgArrays.push_back(meanArrayPtr);
    indexAlgArrays.push_back(stdDevArrayPtr);
    indexAlgArrays.push_back(summationArrayPtr);

    const auto& featureIds = featureIdsPtr->getDataStoreRef();
    auto& data = inputArrayPtr->getDataStoreRef();
    StatisticsByFeatureImpl<T> classToExecute = StatisticsByFeatureImpl<T>(inputValues->FindLength, inputValues->FindMin, inputValues->FindMax, inputValues->FindMean, inputValues->FindMode,
                                                                           inputValues->FindStdDeviation, inputValues->FindSummation, maskCompare, featureIds, data, featureHasDataPtr, lengthArrayPtr,
                                                                           minArrayPtr, maxArrayPtr, meanArrayPtr, modeArrayPtr, stdDevArrayPtr, summationArrayPtr, shouldCancel, messageHelper);
    if(CheckArraysInMemory(indexAlgArrays))
    {
      const tbb::simple_partitioner simplePartitioner;
      const usize grainSize = 500;
      tbb::blocked_range<usize> tbbRange(0, numFeatures, grainSize);
      tbb::parallel_for(tbbRange, std::move(classToExecute), simplePartitioner);
    }
    else
    {
      ParallelDataAlgorithm indexAlg;
      indexAlg.setRange(0, numFeatures);
      indexAlg.requireArraysInMemory(indexAlgArrays);
      indexAlg.execute(std::move(classToExecute));
    }

    if(inputValues->FindMedian || inputValues->FindNumUniqueValues)
    {
      messageHelper.sendMessage("Starting Median Calculation...");

      auto* medianArrayPtr = dynamic_cast<Float32Array*>(arrays[4]);
      auto* numUniqueValuesArrayPtr = dynamic_cast<Int32Array*>(arrays[8]);

      ParallelDataAlgorithm medianDataAlg;
      {
        // Scoped to prevent alg use of ptr array
        IParallelAlgorithm::AlgorithmArrays medianAlgArrays;
        medianAlgArrays.push_back(featureIdsPtr);
        medianAlgArrays.push_back(inputArrayPtr);
        medianAlgArrays.push_back(medianArrayPtr);
        medianAlgArrays.push_back(numUniqueValuesArrayPtr);
        medianAlgArrays.push_back(lengthArrayPtr);

        medianDataAlg.requireArraysInMemory(medianAlgArrays);
      }
      medianDataAlg.setRange(0, numFeatures);
      medianDataAlg.execute(
          MedianByFeatureImpl<T>(maskCompare, featureIds, data, inputValues->FindMedian, inputValues->FindNumUniqueValues, medianArrayPtr, numUniqueValuesArrayPtr, lengthArrayPtr, messageHelper));
    }

    // compute the standardized data
    if(inputValues->StandardizeData)
    {
      const auto& mean = dataStructure.getDataRefAs<Float32Array>(inputValues->MeanArrayName).getDataStoreRef();
      const auto& std = dataStructure.getDataRefAs<Float32Array>(inputValues->StdDeviationArrayName).getDataStoreRef();
      auto& standardized = dataStructure.getDataRefAs<Float32Array>(inputValues->StandardizedArrayName).getDataStoreRef();

      const usize numTuples = data.getNumberOfTuples();
      for(usize i = 0; i < numTuples; i++)
      {
        const int32 featureId = featureIds.at(i);
        if((!inputValues->UseMask || maskCompare->isTrue(i)) && featureId >= 0 && static_cast<usize>(featureId) < numFeatures)
        {
          standardized.setValue(i, (static_cast<float32>(data[i]) - mean[static_cast<usize>(featureId)]) / std[static_cast<usize>(featureId)]);
        }
      }
    }
    return {};
  }

  /**
   * @brief Range-based (IgnoreZero/ShrinkToFit/CustomRange/PaddedCustomRange) feature statistics
   * entry point. Builds a single featureId -> compact-output-index lookup once (feature-scale) and
   * shares it across the stats pass, the median/unique-value pass, and the standardization pass below
   * -- each of which now streams the cell-level arrays in bounded chunks exactly once (twice for
   * standard deviation, which needs the mean first) instead of rescanning all cells once per feature.
   */
  template <typename T>
  Result<> operator()(DataStructure& dataStructure, const IDataArray* inputIDataArray, std::vector<IArray*>& arrays, const std::pair<int32, int32>& range,
                      const ComputeArrayStatisticsInputValues* inputValues, const std::atomic_bool& shouldCancel)
  {
    std::unique_ptr<MaskCompareUtilities::MaskCompare> maskCompare = nullptr;
    if(inputValues->UseMask)
    {
      try
      {
        maskCompare = MaskCompareUtilities::InstantiateMaskCompare(dataStructure, inputValues->MaskArrayPath);
      } catch(const std::out_of_range&)
      {
        return MakeErrorResult(-563508, fmt::format("Mask Array DataPath does not exist or is not of the correct type (Bool | UInt8) {}", inputValues->MaskArrayPath.toString()));
      }
    }

    Result<> initializationResult = InitializeArrays<T>(dataStructure, inputValues);
    if(initializationResult.invalid())
    {
      return initializationResult;
    }

    // this level preps and preforms the calculations accordingly
    const auto* featureIdsMapPtr = dataStructure.getDataAs<Int32Array>(inputValues->FeatureIdMapArrayPath);
    const auto* inputArrayPtr = static_cast<const DataArray<T>*>(inputIDataArray);
    const auto* featureIdsPtr = dataStructure.getDataAs<Int32Array>(inputValues->FeatureIdsArrayPath);
    auto* lengthArrayPtr = dynamic_cast<DataArray<uint64>*>(arrays[0]);
    auto* minArrayPtr = dynamic_cast<DataArray<T>*>(arrays[1]);
    auto* maxArrayPtr = dynamic_cast<DataArray<T>*>(arrays[2]);
    auto* meanArrayPtr = dynamic_cast<Float32Array*>(arrays[3]);
    auto* modeArrayPtr = dynamic_cast<NeighborList<T>*>(arrays[5]);
    auto* stdDevArrayPtr = dynamic_cast<Float32Array*>(arrays[6]);
    auto* summationArrayPtr = dynamic_cast<Float32Array*>(arrays[7]);

    auto* featureHasDataPtr = dynamic_cast<BoolArray*>(arrays[9]);

    const auto& featureIds = featureIdsPtr->getDataStoreRef();
    const auto& featureIdsMapStore = featureIdsMapPtr->getDataStoreRef();
    const auto& data = inputArrayPtr->getDataStoreRef();

    const int32 start = range.first;
    const auto numFeatures = static_cast<usize>(static_cast<int64>(range.second) - static_cast<int64>(range.first) + 1);

    // Cache the feature-id map (feature-scale, small) once and invert it into a dense featureId ->
    // compact-output-index lookup. Replaces the O(numFeatures) std::find previously repeated once per
    // feature inside the stats/median implementations, and once per CELL in the standardization loop
    // below (an O(numCells x numFeatures) cost prior to this change).
    std::vector<int32> featureIdMapCache(numFeatures);
    Result<> mapReadResult = featureIdsMapStore.copyIntoBuffer(0, nonstd::span<int32>(featureIdMapCache.data(), numFeatures));
    if(mapReadResult.invalid())
    {
      return mapReadResult;
    }
    std::vector<usize> featureIdToCompactIndex(numFeatures);
    for(usize k = 0; k < numFeatures; k++)
    {
      featureIdToCompactIndex[static_cast<usize>(static_cast<int64>(featureIdMapCache[k]) - static_cast<int64>(start))] = k;
    }

    StatisticsByFeatureRangeImpl<T> statsImpl(inputValues->FindLength, inputValues->FindMin, inputValues->FindMax, inputValues->FindMean, inputValues->FindMode, inputValues->FindStdDeviation,
                                              inputValues->FindSummation, featureIdToCompactIndex, maskCompare, featureIds, data, start, numFeatures, featureHasDataPtr, lengthArrayPtr, minArrayPtr,
                                              maxArrayPtr, meanArrayPtr, modeArrayPtr, stdDevArrayPtr, summationArrayPtr, shouldCancel);
    Result<> statsResult = statsImpl();
    if(statsResult.invalid())
    {
      return statsResult;
    }

    if(inputValues->FindMedian || inputValues->FindNumUniqueValues)
    {
      auto* medianArrayPtr = dynamic_cast<Float32Array*>(arrays[4]);
      auto* numUniqueValuesArrayPtr = dynamic_cast<Int32Array*>(arrays[8]);

      MedianByFeatureRangeImpl<T> medianImpl(featureIdToCompactIndex, maskCompare, featureIds, data, start, numFeatures, inputValues->FindMedian, inputValues->FindNumUniqueValues, medianArrayPtr,
                                             numUniqueValuesArrayPtr, shouldCancel);
      Result<> medianResult = medianImpl();
      if(medianResult.invalid())
      {
        return medianResult;
      }
    }

    // compute the standardized data based on whether computing by index or not
    if(inputValues->StandardizeData)
    {
      const auto& mean = dataStructure.getDataRefAs<Float32Array>(inputValues->MeanArrayName).getDataStoreRef();
      const auto& stdDevStore = dataStructure.getDataRefAs<Float32Array>(inputValues->StdDeviationArrayName).getDataStoreRef();
      auto& standardized = dataStructure.getDataRefAs<Float32Array>(inputValues->StandardizedArrayName).getDataStoreRef();

      // Cache mean/std (feature-scale) locally, then stream the cell-level arrays once in bounded
      // chunks -- a read-modify-write per chunk so cells outside the feature-id range keep whatever
      // value the standardized array already held (matches the previous per-cell setValue, which only
      // ever touched in-range cells).
      std::vector<float32> meanCache(numFeatures);
      Result<> meanReadResult = mean.copyIntoBuffer(0, nonstd::span<float32>(meanCache.data(), numFeatures));
      if(meanReadResult.invalid())
      {
        return meanReadResult;
      }
      std::vector<float32> stdDevCache(numFeatures);
      Result<> stdDevReadResult = stdDevStore.copyIntoBuffer(0, nonstd::span<float32>(stdDevCache.data(), numFeatures));
      if(stdDevReadResult.invalid())
      {
        return stdDevReadResult;
      }

      const usize numTuples = data.getNumberOfTuples();
      auto featureIdBuf = std::make_unique<int32[]>(k_ChunkTuples);
      auto sourceBuf = std::make_unique<T[]>(k_ChunkTuples);
      auto standardizedBuf = std::make_unique<float32[]>(k_ChunkTuples);

      for(usize offset = 0; offset < numTuples; offset += k_ChunkTuples)
      {
        if(shouldCancel)
        {
          return {};
        }
        const usize chunkCount = std::min(k_ChunkTuples, numTuples - offset);

        Result<> featureIdResult = featureIds.copyIntoBuffer(offset, nonstd::span<int32>(featureIdBuf.get(), chunkCount));
        if(featureIdResult.invalid())
        {
          return featureIdResult;
        }
        Result<> sourceResult = data.copyIntoBuffer(offset, nonstd::span<T>(sourceBuf.get(), chunkCount));
        if(sourceResult.invalid())
        {
          return sourceResult;
        }
        Result<> standardizedReadResult = standardized.copyIntoBuffer(offset, nonstd::span<float32>(standardizedBuf.get(), chunkCount));
        if(standardizedReadResult.invalid())
        {
          return standardizedReadResult;
        }

        for(usize idx = 0; idx < chunkCount; idx++)
        {
          const int32 featureId = featureIdBuf[idx];
          if((maskCompare != nullptr && !maskCompare->isTrue(offset + idx)) || featureId < range.first || featureId > range.second)
          {
            continue;
          }
          const usize compactIndex = featureIdToCompactIndex[static_cast<usize>(static_cast<int64>(featureId) - static_cast<int64>(start))];
          standardizedBuf[idx] = (static_cast<float32>(sourceBuf[idx]) - meanCache[compactIndex]) / stdDevCache[compactIndex];
        }

        Result<> standardizedWriteResult = standardized.copyFromBuffer(offset, nonstd::span<const float32>(standardizedBuf.get(), chunkCount));
        if(standardizedWriteResult.invalid())
        {
          return standardizedWriteResult;
        }
      }
    }
    return {};
  }
};

/**
 * @brief Maps FeatureIds into the compact output tuple range chosen by the filter.
 * IDs outside a custom range return no group and are skipped without allocating
 * a sparse array up to the largest possible ID.
 */
struct StatisticsGroupLayout
{
  bool ComputeByIndex = false;
  bool CreateFeatureIdMap = false;
  int32 FirstFeatureId = 0;
  int32 LastFeatureId = 0;
  usize GroupCount = 1;

  /** @brief Returns the compact output index for @p featureId, or no value when excluded. */
  std::optional<usize> groupIndex(int32 featureId) const
  {
    if(!ComputeByIndex)
    {
      return usize{0};
    }
    if(GroupCount == 0 || featureId < FirstFeatureId || featureId > LastFeatureId)
    {
      return std::nullopt;
    }
    return static_cast<usize>(static_cast<int64>(featureId) - static_cast<int64>(FirstFeatureId));
  }
};

/** @brief Minimum and maximum IDs discovered by the initial bounded FeatureIds scan. */
struct ObservedFeatureBounds
{
  bool HasValues = false;
  int32 Minimum = 0;
  int32 Maximum = 0;
};

/**
 * @brief Finds the observed FeatureId interval with fixed-size bulk reads.
 * @return Partial/default bounds on cancellation, the first read failure, or
 * the complete bounds after one pass.
 */
Result<ObservedFeatureBounds> discoverFeatureBounds(const Int32AbstractDataStore& featureIds, const std::atomic_bool& shouldCancel)
{
  ObservedFeatureBounds bounds;
  auto featureBuffer = std::make_unique<int32[]>(k_ChunkTuples);
  const usize tupleCount = featureIds.getNumberOfTuples();
  for(usize offset = 0; offset < tupleCount; offset += k_ChunkTuples)
  {
    if(shouldCancel)
    {
      return {bounds};
    }
    const usize count = std::min(k_ChunkTuples, tupleCount - offset);
    Result<> readResult = featureIds.copyIntoBuffer(offset, nonstd::span<int32>(featureBuffer.get(), count));
    if(readResult.invalid())
    {
      return ConvertInvalidResult<ObservedFeatureBounds>(std::move(readResult));
    }
    for(usize index = 0; index < count; ++index)
    {
      const int32 featureId = featureBuffer[index];
      if(!bounds.HasValues)
      {
        bounds.HasValues = true;
        bounds.Minimum = featureId;
        bounds.Maximum = featureId;
      }
      else
      {
        bounds.Minimum = std::min(bounds.Minimum, featureId);
        bounds.Maximum = std::max(bounds.Maximum, featureId);
      }
    }
  }
  return {bounds};
}

/**
 * @brief Combines observed IDs and range controls into a dense output layout.
 * @return An error for an invalid or unrepresentable range; otherwise the exact
 * first ID, last ID, and output tuple count used by both implementations.
 */
Result<StatisticsGroupLayout> resolveGroupLayout(const ComputeArrayStatisticsInputValues& inputValues, const ObservedFeatureBounds& observedBounds)
{
  StatisticsGroupLayout layout;
  layout.ComputeByIndex = inputValues.ComputeByIndex;
  if(!layout.ComputeByIndex)
  {
    return {layout};
  }

  const auto selection = static_cast<ComputeArrayStatistics::FeatureIdRangeControls>(inputValues.RangeType);
  layout.CreateFeatureIdMap = selection != ComputeArrayStatistics::FeatureIdRangeControls::None;
  if(!observedBounds.HasValues)
  {
    if(selection == ComputeArrayStatistics::FeatureIdRangeControls::PaddedCustomRange && inputValues.Range.at(1) != -1)
    {
      layout.FirstFeatureId = inputValues.Range.at(0);
      layout.LastFeatureId = inputValues.Range.at(1);
    }
    else
    {
      layout.GroupCount = 0;
      return {layout};
    }
  }
  else
  {
    layout.FirstFeatureId = observedBounds.Minimum;
    layout.LastFeatureId = observedBounds.Maximum;
    switch(selection)
    {
    case ComputeArrayStatistics::FeatureIdRangeControls::None:
      layout.FirstFeatureId = 0;
      if(layout.LastFeatureId < 0)
      {
        layout.GroupCount = 0;
        return {layout};
      }
      break;
    case ComputeArrayStatistics::FeatureIdRangeControls::IgnoreZero:
      layout.FirstFeatureId = 1;
      break;
    case ComputeArrayStatistics::FeatureIdRangeControls::ShrinkToFit:
      break;
    case ComputeArrayStatistics::FeatureIdRangeControls::CustomRange:
      layout.FirstFeatureId = std::max(layout.FirstFeatureId, inputValues.Range.at(0));
      if(inputValues.Range.at(1) != -1)
      {
        layout.LastFeatureId = std::min(layout.LastFeatureId, inputValues.Range.at(1));
      }
      break;
    case ComputeArrayStatistics::FeatureIdRangeControls::PaddedCustomRange:
      layout.FirstFeatureId = inputValues.Range.at(0);
      if(inputValues.Range.at(1) != -1)
      {
        layout.LastFeatureId = inputValues.Range.at(1);
      }
      break;
    default:
      return MakeErrorResult<StatisticsGroupLayout>(
          -506670, fmt::format("ComputeArrayStatistics: unknown feature ID range control {} for observed FeatureIds [{}, {}].", inputValues.RangeType, observedBounds.Minimum, observedBounds.Maximum));
    }
  }

  if(layout.FirstFeatureId > layout.LastFeatureId)
  {
    return MakeErrorResult<StatisticsGroupLayout>(
        -506671, fmt::format("ComputeArrayStatistics: range minimum ({}) must be less than or equal to range maximum ({}).", layout.FirstFeatureId, layout.LastFeatureId));
  }
  const uint64 groupCount = static_cast<uint64>(static_cast<int64>(layout.LastFeatureId) - static_cast<int64>(layout.FirstFeatureId)) + 1ULL;
  if(groupCount > static_cast<uint64>(std::numeric_limits<int32>::max()))
  {
    return MakeErrorResult<StatisticsGroupLayout>(
        -57300, fmt::format("ComputeArrayStatistics: the requested FeatureId range [{}, {}] contains {} outputs, which exceeds the supported int32 group index range.", layout.FirstFeatureId,
                            layout.LastFeatureId, groupCount));
  }
  if(groupCount > static_cast<uint64>(std::numeric_limits<usize>::max()))
  {
    return MakeErrorResult<StatisticsGroupLayout>(
        -57301, fmt::format("ComputeArrayStatistics: the requested FeatureId range [{}, {}] cannot be represented by the platform size type.", layout.FirstFeatureId, layout.LastFeatureId));
  }
  layout.GroupCount = static_cast<usize>(groupCount);
  return {layout};
}

template <typename T>
constexpr uint64 k_StatisticsRecordSize = sizeof(int32) + sizeof(T) + sizeof(uint64);

/** @brief Serializes (compact group, value, original tuple) for exact external sorting. */
template <typename T>
void encodeStatisticsRecord(nonstd::span<std::byte> bytes, int32 groupId, T value, uint64 originalTupleIndex)
{
  std::memcpy(bytes.data(), &groupId, sizeof(groupId));
  std::memcpy(bytes.data() + sizeof(groupId), &value, sizeof(value));
  std::memcpy(bytes.data() + sizeof(groupId) + sizeof(value), &originalTupleIndex, sizeof(originalTupleIndex));
}

/** @brief Decodes a record created by encodeStatisticsRecord(). */
template <typename T>
void decodeStatisticsRecord(nonstd::span<const std::byte> bytes, int32& groupId, T& value, uint64& originalTupleIndex)
{
  std::memcpy(&groupId, bytes.data(), sizeof(groupId));
  std::memcpy(&value, bytes.data() + sizeof(groupId), sizeof(value));
  std::memcpy(&originalTupleIndex, bytes.data() + sizeof(groupId) + sizeof(value), sizeof(originalTupleIndex));
}

/**
 * @brief Orders scratch records by compact group, value, then input position.
 * The last key makes equal-value ordering deterministic without affecting run counts.
 */
template <typename T>
int32 compareStatisticsRecords(nonstd::span<const std::byte> left, nonstd::span<const std::byte> right)
{
  int32 leftGroup = 0;
  int32 rightGroup = 0;
  T leftValue{};
  T rightValue{};
  uint64 leftIndex = 0;
  uint64 rightIndex = 0;
  decodeStatisticsRecord(left, leftGroup, leftValue, leftIndex);
  decodeStatisticsRecord(right, rightGroup, rightValue, rightIndex);
  if(leftGroup != rightGroup)
  {
    return leftGroup < rightGroup ? -1 : 1;
  }
  if(leftValue < rightValue)
  {
    return -1;
  }
  if(rightValue < leftValue)
  {
    return 1;
  }
  if(leftIndex == rightIndex)
  {
    return 0;
  }
  return leftIndex < rightIndex ? -1 : 1;
}

/** @brief Tests generic numeric equality using the ordering required by the sorter. */
template <typename T>
bool equivalentStatisticsValues(const T& left, const T& right)
{
  return !(left < right) && !(right < left);
}

/**
 * @brief Streams sorted records in bounded pages and reports each equal-value run.
 * @p runFunction receives the compact group, value, occurrence count, and
 * zero-based position of the run within that group.
 */
template <typename T, typename RunFunction>
Result<> scanSortedStatisticsRuns(const IExternalSort& externalSort, usize groupCount, const std::atomic_bool& shouldCancel, RunFunction&& runFunction)
{
  std::vector<std::byte> bytes(k_ChunkTuples * static_cast<usize>(k_StatisticsRecordSize<T>));
  std::optional<int32> currentGroup;
  std::optional<T> currentValue;
  uint64 currentCount = 0;
  uint64 groupPosition = 0;

  const auto flushRun = [&]() -> Result<> {
    if(!currentGroup.has_value())
    {
      return {};
    }
    if(*currentGroup < 0 || static_cast<usize>(*currentGroup) >= groupCount)
    {
      return MakeErrorResult(-57302, fmt::format("ComputeArrayStatistics: external sort returned invalid group index {} for {} output groups.", *currentGroup, groupCount));
    }
    Result<> result = runFunction(static_cast<usize>(*currentGroup), *currentValue, currentCount, groupPosition);
    if(result.invalid())
    {
      return result;
    }
    groupPosition += currentCount;
    return {};
  };

  const uint64 totalRecords = externalSort.recordCount();
  for(uint64 offset = 0; offset < totalRecords; offset += k_ChunkTuples)
  {
    if(shouldCancel)
    {
      return {};
    }
    const uint64 count = std::min<uint64>(k_ChunkTuples, totalRecords - offset);
    Result<uint64> readResult = externalSort.read(offset, count, nonstd::span<std::byte>(bytes.data(), static_cast<usize>(count * k_StatisticsRecordSize<T>)), shouldCancel);
    if(readResult.invalid())
    {
      return ConvertResult(std::move(readResult));
    }
    if(readResult.value() != count)
    {
      return MakeErrorResult(-57303, fmt::format("ComputeArrayStatistics: external sort short read at record {}: requested {} records but received {}.", offset, count, readResult.value()));
    }
    for(uint64 index = 0; index < count; ++index)
    {
      int32 group = 0;
      T value{};
      uint64 originalIndex = 0;
      const usize byteOffset = static_cast<usize>(index * k_StatisticsRecordSize<T>);
      decodeStatisticsRecord(nonstd::span<const std::byte>(bytes.data() + byteOffset, static_cast<usize>(k_StatisticsRecordSize<T>)), group, value, originalIndex);
      if(currentGroup.has_value() && *currentGroup == group && equivalentStatisticsValues(*currentValue, value))
      {
        if(currentCount == std::numeric_limits<uint64>::max())
        {
          return MakeErrorResult(-57304, fmt::format("ComputeArrayStatistics: occurrence count for group {} and a sorted value exceeds uint64.", group));
        }
        ++currentCount;
        continue;
      }
      Result<> flushResult = flushRun();
      if(flushResult.invalid())
      {
        return flushResult;
      }
      if(!currentGroup.has_value() || *currentGroup != group)
      {
        groupPosition = 0;
      }
      currentGroup = group;
      currentValue = value;
      currentCount = 1;
    }
  }
  return flushRun();
}

/**
 * @brief Validates and bulk-writes one feature-scale scalar output.
 * Keeping this write centralized ensures all statistics use the store's
 * contiguous transfer API instead of per-element disk accesses.
 */
template <typename T>
Result<> writeStatisticsOutput(DataArray<T>* outputArray, const T* values, usize count, const DataPath& path)
{
  if(outputArray == nullptr)
  {
    return MakeErrorResult(-57305, fmt::format("ComputeArrayStatistics: required output array '{}' is missing or has the wrong type.", path.toString()));
  }
  if(outputArray->getNumberOfTuples() != count || outputArray->getNumberOfComponents() != 1)
  {
    return MakeErrorResult(-57306, fmt::format("ComputeArrayStatistics: output array '{}' has {} tuples and {} components; expected {} scalar tuples.", path.toString(),
                                               outputArray->getNumberOfTuples(), outputArray->getNumberOfComponents(), count));
  }
  return outputArray->getDataStoreRef().copyFromBuffer(0, nonstd::span<const T>(values, count));
}

/**
 * @brief Bounded implementation for one runtime-selected value and mask type.
 *
 * Feature-scale accumulators remain resident while potentially large cell data
 * is read in fixed pages. Order statistics use a registered external sorter;
 * without one, exact repeated scans trade speed for bounded memory rather than
 * silently changing median, mode, or unique-count results.
 */
template <typename T, typename MaskT>
Result<> generateScanlineStatistics(DataStructure& dataStructure, const IDataArray& inputDataArray, const IDataArray* featureIdsDataArray, const IDataArray* maskDataArray,
                                    const StatisticsGroupLayout& layout, const ComputeArrayStatisticsInputValues& inputValues, const std::atomic_bool& shouldCancel)
{
  const auto& inputStore = inputDataArray.template getIDataStoreRefAs<AbstractDataStore<T>>();
  const auto* featureIdsStore = featureIdsDataArray == nullptr ? nullptr : &featureIdsDataArray->template getIDataStoreRefAs<AbstractDataStore<int32>>();
  const auto* maskStore = maskDataArray == nullptr ? nullptr : &maskDataArray->template getIDataStoreRefAs<AbstractDataStore<MaskT>>();
  const usize tupleCount = inputStore.getNumberOfTuples();
  if((featureIdsStore != nullptr && featureIdsStore->getNumberOfTuples() != tupleCount) || (maskStore != nullptr && maskStore->getNumberOfTuples() != tupleCount))
  {
    return MakeErrorResult(-57307, fmt::format("ComputeArrayStatistics: input '{}' has {} tuples, FeatureIds has {}, and the mask has {}; all enabled cell arrays must have identical tuple counts.",
                                               inputDataArray.getName(), tupleCount, featureIdsStore == nullptr ? 0 : featureIdsStore->getNumberOfTuples(),
                                               maskStore == nullptr ? 0 : maskStore->getNumberOfTuples()));
  }
  if(layout.ComputeByIndex != (featureIdsStore != nullptr))
  {
    return MakeErrorResult(-57308, "ComputeArrayStatistics: the resolved grouping layout and FeatureIds input are inconsistent.");
  }

  const usize groupCount = layout.GroupCount;
  auto* featureHasDataArray = inputValues.ComputeByIndex ? dataStructure.getDataAs<BoolArray>(inputValues.FeatureHasDataArrayName) : nullptr;
  auto* lengthArray = inputValues.FindLength ? dataStructure.getDataAs<UInt64Array>(inputValues.LengthArrayName) : nullptr;
  auto* minimumArray = inputValues.FindMin ? dataStructure.getDataAs<DataArray<T>>(inputValues.MinimumArrayName) : nullptr;
  auto* maximumArray = inputValues.FindMax ? dataStructure.getDataAs<DataArray<T>>(inputValues.MaximumArrayName) : nullptr;
  auto* meanArray = inputValues.FindMean ? dataStructure.getDataAs<Float32Array>(inputValues.MeanArrayName) : nullptr;
  auto* medianArray = inputValues.FindMedian ? dataStructure.getDataAs<Float32Array>(inputValues.MedianArrayName) : nullptr;
  auto* stdDeviationArray = inputValues.FindStdDeviation ? dataStructure.getDataAs<Float32Array>(inputValues.StdDeviationArrayName) : nullptr;
  auto* summationArray = inputValues.FindSummation ? dataStructure.getDataAs<Float32Array>(inputValues.SummationArrayName) : nullptr;
  auto* uniqueValuesArray = inputValues.FindNumUniqueValues ? dataStructure.getDataAs<Int32Array>(inputValues.NumUniqueValuesName) : nullptr;
  auto* standardizedArray = inputValues.StandardizeData ? dataStructure.getDataAs<Float32Array>(inputValues.StandardizedArrayName) : nullptr;
  if(inputValues.ComputeByIndex && featureHasDataArray == nullptr)
  {
    return MakeErrorResult(-57305, fmt::format("ComputeArrayStatistics: required FeatureHasData output '{}' is missing or has the wrong type.", inputValues.FeatureHasDataArrayName.toString()));
  }
  if(inputValues.FindMode)
  {
    if constexpr(std::is_same_v<T, bool>)
    {
      return MakeErrorResult(-57305, "ComputeArrayStatistics: Boolean input does not support Mode output.");
    }
    else if(dataStructure.getDataAs<NeighborList<T>>(inputValues.ModeArrayName) == nullptr)
    {
      return MakeErrorResult(-57305, fmt::format("ComputeArrayStatistics: required Mode output '{}' is missing or has the wrong NeighborList type.", inputValues.ModeArrayName.toString()));
    }
  }
  if(inputValues.StandardizeData && (standardizedArray == nullptr || standardizedArray->getNumberOfTuples() != tupleCount))
  {
    return MakeErrorResult(-57306, fmt::format("ComputeArrayStatistics: standardized output '{}' is missing, has the wrong type, or does not contain {} tuples.",
                                               inputValues.StandardizedArrayName.toString(), tupleCount));
  }

  std::vector<uint64> lengths(groupCount, 0);
  auto minimums = std::make_unique<T[]>(groupCount);
  auto maximums = std::make_unique<T[]>(groupCount);
  std::vector<float32> sums(groupCount, 0.0F);
  std::vector<float32> means(groupCount, 0.0F);
  std::vector<float32> medians(groupCount, 0.0F);
  std::vector<float32> standardDeviations(groupCount, 0.0F);
  std::vector<int32> uniqueCounts(groupCount, 0);
  std::vector<uint64> maximumModeCounts(inputValues.FindMode ? groupCount : 0, 0);
  const bool legacyNoneRange = layout.ComputeByIndex && inputValues.RangeType == to_underlying(ComputeArrayStatistics::FeatureIdRangeControls::None);
  for(usize group = 0; group < groupCount; ++group)
  {
    minimums[group] = std::numeric_limits<T>::max();
    if constexpr(std::is_floating_point_v<T>)
    {
      maximums[group] = legacyNoneRange ? std::numeric_limits<T>::min() : std::numeric_limits<T>::lowest();
    }
    else
    {
      maximums[group] = std::numeric_limits<T>::min();
    }
  }

  using GlobalSumType = std::conditional_t<std::is_integral_v<T>, std::conditional_t<std::is_signed_v<T>, int64, uint64>, float64>;
  GlobalSumType globalSum = 0;

  const bool needsSortedValues = inputValues.FindMedian || inputValues.FindMode || inputValues.FindNumUniqueValues;
  std::unique_ptr<IExternalSort> externalSort;
  std::vector<std::byte> recordBytes;
  if(needsSortedValues && DataStoreUtilities::GetIOCollection().hasExternalSortCapability())
  {
    ExternalSortConfig config;
    config.recordSize = k_StatisticsRecordSize<T>;
    config.maxRecordsPerBatch = k_ChunkTuples;
    config.compare = compareStatisticsRecords<T>;
    Result<std::unique_ptr<IExternalSort>> createResult = DataStoreUtilities::GetIOCollection().createExternalSort(config);
    if(createResult.invalid())
    {
      return ConvertResult(std::move(createResult));
    }
    externalSort = std::move(createResult.value());
    recordBytes.resize(k_ChunkTuples * static_cast<usize>(k_StatisticsRecordSize<T>));
  }

  auto valueBuffer = std::make_unique<T[]>(k_ChunkTuples);
  auto featureBuffer = featureIdsStore == nullptr ? nullptr : std::make_unique<int32[]>(k_ChunkTuples);
  auto maskBuffer = maskStore == nullptr ? nullptr : std::make_unique<MaskT[]>(k_ChunkTuples);
  const auto readChunk = [&](usize offset, usize count) -> Result<> {
    Result<> result = inputStore.copyIntoBuffer(offset, nonstd::span<T>(valueBuffer.get(), count));
    if(result.invalid())
    {
      return result;
    }
    if(featureIdsStore != nullptr)
    {
      result = featureIdsStore->copyIntoBuffer(offset, nonstd::span<int32>(featureBuffer.get(), count));
      if(result.invalid())
      {
        return result;
      }
    }
    if(maskStore != nullptr)
    {
      result = maskStore->copyIntoBuffer(offset, nonstd::span<MaskT>(maskBuffer.get(), count));
    }
    return result;
  };

  // Pass 1 computes count, extrema, and sum while optionally producing compact
  // external-sort records for statistics that depend on value order.
  for(usize offset = 0; offset < tupleCount; offset += k_ChunkTuples)
  {
    if(shouldCancel)
    {
      return {};
    }
    const usize count = std::min(k_ChunkTuples, tupleCount - offset);
    Result<> readResult = readChunk(offset, count);
    if(readResult.invalid())
    {
      return readResult;
    }
    if(shouldCancel)
    {
      return {};
    }
    uint64 recordCount = 0;
    for(usize index = 0; index < count; ++index)
    {
      if(maskStore != nullptr && !static_cast<bool>(maskBuffer[index]))
      {
        continue;
      }
      const std::optional<usize> group = layout.groupIndex(featureIdsStore == nullptr ? 0 : featureBuffer[index]);
      if(!group.has_value())
      {
        continue;
      }
      const T value = valueBuffer[index];
      const usize groupIndex = *group;
      if(lengths[groupIndex] == std::numeric_limits<uint64>::max())
      {
        return MakeErrorResult(-57310, fmt::format("ComputeArrayStatistics: selected-value count for output group {} exceeds uint64.", groupIndex));
      }
      ++lengths[groupIndex];
      if(value < minimums[groupIndex])
      {
        minimums[groupIndex] = value;
      }
      if(value > maximums[groupIndex])
      {
        maximums[groupIndex] = value;
      }
      if(layout.ComputeByIndex)
      {
        sums[groupIndex] = sums[groupIndex] + static_cast<float32>(value);
      }
      else
      {
        globalSum += static_cast<GlobalSumType>(value);
      }
      if(externalSort != nullptr)
      {
        encodeStatisticsRecord<T>(nonstd::span<std::byte>(recordBytes.data() + static_cast<usize>(recordCount * k_StatisticsRecordSize<T>), static_cast<usize>(k_StatisticsRecordSize<T>)),
                                  static_cast<int32>(groupIndex), value, static_cast<uint64>(offset + index));
        ++recordCount;
      }
    }
    if(externalSort != nullptr && recordCount > 0)
    {
      Result<> appendResult = externalSort->append(recordCount, nonstd::span<const std::byte>(recordBytes.data(), static_cast<usize>(recordCount * k_StatisticsRecordSize<T>)), shouldCancel, {});
      if(appendResult.invalid())
      {
        return appendResult;
      }
    }
  }
  if(externalSort != nullptr)
  {
    if(shouldCancel)
    {
      return {};
    }
    Result<> finishResult = externalSort->finish(shouldCancel, {});
    if(finishResult.invalid() || shouldCancel)
    {
      return finishResult;
    }
  }

  if(!layout.ComputeByIndex && groupCount == 1)
  {
    sums[0] = static_cast<float32>(globalSum);
    if(lengths[0] == 0)
    {
      minimums[0] = T{};
      maximums[0] = T{};
    }
  }
  for(usize group = 0; group < groupCount; ++group)
  {
    if(shouldCancel)
    {
      return {};
    }
    if(lengths[group] == 0)
    {
      continue;
    }
    if constexpr(std::is_same_v<T, bool>)
    {
      if(layout.ComputeByIndex)
      {
        means[group] = static_cast<float32>(sums[group] >= (static_cast<float32>(tupleCount) - sums[group]));
      }
      else
      {
        means[group] = sums[group] / static_cast<float32>(lengths[group]);
      }
    }
    else
    {
      means[group] = sums[group] / static_cast<float32>(lengths[group]);
    }
  }

  // Variance requires the completed group means, so it is intentionally a
  // second input pass instead of retaining every selected value in memory.
  if(inputValues.FindStdDeviation)
  {
    std::vector<float64> sumOfDifferences(groupCount, 0.0);
    for(usize offset = 0; offset < tupleCount; offset += k_ChunkTuples)
    {
      if(shouldCancel)
      {
        return {};
      }
      const usize count = std::min(k_ChunkTuples, tupleCount - offset);
      Result<> readResult = readChunk(offset, count);
      if(readResult.invalid())
      {
        return readResult;
      }
      if(shouldCancel)
      {
        return {};
      }
      for(usize index = 0; index < count; ++index)
      {
        if(maskStore != nullptr && !static_cast<bool>(maskBuffer[index]))
        {
          continue;
        }
        const std::optional<usize> group = layout.groupIndex(featureIdsStore == nullptr ? 0 : featureBuffer[index]);
        if(!group.has_value())
        {
          continue;
        }
        const float64 difference = static_cast<float64>(valueBuffer[index] - means[*group]);
        sumOfDifferences[*group] += difference * difference;
      }
    }
    for(usize group = 0; group < groupCount; ++group)
    {
      if(lengths[group] > 0)
      {
        standardDeviations[group] = static_cast<float32>(std::sqrt(sumOfDifferences[group] / static_cast<float64>(lengths[group])));
      }
    }
  }

  auto medianLower = std::make_unique<T[]>(inputValues.FindMedian ? groupCount : 0);
  auto medianUpper = std::make_unique<T[]>(inputValues.FindMedian ? groupCount : 0);
  std::vector<uint8> hasMedianLower(inputValues.FindMedian ? groupCount : 0, 0);
  std::vector<uint8> hasMedianUpper(inputValues.FindMedian ? groupCount : 0, 0);
  const auto consumeSortedRun = [&](usize group, const T& value, uint64 runCount, uint64 runPosition) -> Result<> {
    if(runCount > std::numeric_limits<uint64>::max() - runPosition)
    {
      return MakeErrorResult(-57311, fmt::format("ComputeArrayStatistics: sorted position overflow for output group {}.", group));
    }
    const uint64 runEnd = runPosition + runCount;
    if(inputValues.FindNumUniqueValues)
    {
      if(uniqueCounts[group] == std::numeric_limits<int32>::max())
      {
        return MakeErrorResult(-57312, fmt::format("ComputeArrayStatistics: output group {} contains more than {} unique values, which cannot be represented by the Int32 output.", group,
                                                   std::numeric_limits<int32>::max()));
      }
      ++uniqueCounts[group];
    }
    if(inputValues.FindMode)
    {
      maximumModeCounts[group] = std::max(maximumModeCounts[group], runCount);
    }
    if(inputValues.FindMedian && lengths[group] > 0)
    {
      const uint64 lowerPosition = (lengths[group] - 1) / 2;
      const uint64 upperPosition = lengths[group] / 2;
      if(runPosition <= lowerPosition && lowerPosition < runEnd)
      {
        medianLower[group] = value;
        hasMedianLower[group] = 1;
      }
      if(runPosition <= upperPosition && upperPosition < runEnd)
      {
        medianUpper[group] = value;
        hasMedianUpper[group] = 1;
      }
    }
    return {};
  };

  // Consume equal-value runs to derive unique counts, mode frequencies, and
  // the two exact median positions in one ordered traversal.
  if(needsSortedValues && externalSort != nullptr)
  {
    Result<> sortedResult = scanSortedStatisticsRuns<T>(*externalSort, groupCount, shouldCancel, consumeSortedRun);
    if(sortedResult.invalid() || shouldCancel)
    {
      return sortedResult;
    }
  }
  else if(needsSortedValues)
  {
    // Provider-free fallback repeatedly selects and counts the next value for
    // each group. It is deliberately slower but exact and bounded in memory.
    const auto scanGroup = [&](usize requestedGroup, auto&& valueFunction) -> Result<> {
      for(usize offset = 0; offset < tupleCount; offset += k_ChunkTuples)
      {
        if(shouldCancel)
        {
          return {};
        }
        const usize count = std::min(k_ChunkTuples, tupleCount - offset);
        Result<> readResult = readChunk(offset, count);
        if(readResult.invalid())
        {
          return readResult;
        }
        if(shouldCancel)
        {
          return {};
        }
        for(usize index = 0; index < count; ++index)
        {
          if(maskStore != nullptr && !static_cast<bool>(maskBuffer[index]))
          {
            continue;
          }
          const std::optional<usize> group = layout.groupIndex(featureIdsStore == nullptr ? 0 : featureBuffer[index]);
          if(group.has_value() && *group == requestedGroup)
          {
            valueFunction(valueBuffer[index]);
          }
        }
      }
      return {};
    };

    for(usize group = 0; group < groupCount; ++group)
    {
      if(shouldCancel)
      {
        return {};
      }
      std::optional<T> previous;
      uint64 groupPosition = 0;
      while(!shouldCancel)
      {
        std::optional<T> next;
        Result<> scanResult = scanGroup(group, [&](const T& value) {
          if((!previous.has_value() || *previous < value) && (!next.has_value() || value < *next))
          {
            next = value;
          }
        });
        if(scanResult.invalid() || shouldCancel)
        {
          return scanResult;
        }
        if(!next.has_value())
        {
          break;
        }
        uint64 occurrenceCount = 0;
        bool occurrenceOverflow = false;
        scanResult = scanGroup(group, [&](const T& value) {
          if(equivalentStatisticsValues(value, *next))
          {
            if(occurrenceCount == std::numeric_limits<uint64>::max())
            {
              occurrenceOverflow = true;
            }
            else
            {
              ++occurrenceCount;
            }
          }
        });
        if(scanResult.invalid() || shouldCancel)
        {
          return scanResult;
        }
        if(occurrenceOverflow)
        {
          return MakeErrorResult(-57304, fmt::format("ComputeArrayStatistics: occurrence count for output group {} exceeds uint64.", group));
        }
        Result<> consumeResult = consumeSortedRun(group, *next, occurrenceCount, groupPosition);
        if(consumeResult.invalid())
        {
          return consumeResult;
        }
        groupPosition += occurrenceCount;
        previous = next;
      }
    }
  }

  if(inputValues.FindMedian)
  {
    for(usize group = 0; group < groupCount; ++group)
    {
      if(lengths[group] == 0)
      {
        continue;
      }
      if(hasMedianLower[group] == 0 || hasMedianUpper[group] == 0)
      {
        return MakeErrorResult(-57313, fmt::format("ComputeArrayStatistics: exact median positions were not found for non-empty output group {}.", group));
      }
      if(lengths[group] % 2 == 1)
      {
        medians[group] = static_cast<float32>(medianUpper[group]);
      }
      else
      {
        medians[group] = static_cast<float32>((medianLower[group] + medianUpper[group]) * 0.5F);
      }
    }
  }

  // Mode values need a second ordered traversal after maximum run sizes are
  // known because ties must all be emitted to the NeighborList.
  if constexpr(!std::is_same_v<T, bool>)
  {
    if(inputValues.FindMode)
    {
      auto* modeArray = dataStructure.getDataAs<NeighborList<T>>(inputValues.ModeArrayName);
      if(shouldCancel)
      {
        return {};
      }
      try
      {
        modeArray->clearAllLists();
        modeArray->resizeTuples({groupCount});
      } catch(const std::exception& exception)
      {
        return MakeErrorResult(-57309, fmt::format("ComputeArrayStatistics: could not initialize Mode output '{}': {}", inputValues.ModeArrayName.toString(), exception.what()));
      }
      const auto appendMode = [&](usize group, const T& value) -> Result<> {
        if(shouldCancel)
        {
          return {};
        }
        try
        {
          modeArray->addEntry(static_cast<int32>(group), value);
        } catch(const std::exception& exception)
        {
          return MakeErrorResult(-57314,
                                 fmt::format("ComputeArrayStatistics: could not append a Mode value to output group {} in '{}': {}", group, inputValues.ModeArrayName.toString(), exception.what()));
        }
        return {};
      };
      if(externalSort != nullptr)
      {
        Result<> modeResult = scanSortedStatisticsRuns<T>(*externalSort, groupCount, shouldCancel, [&](usize group, const T& value, uint64 runCount, uint64) -> Result<> {
          if(runCount == maximumModeCounts[group])
          {
            return appendMode(group, value);
          }
          return {};
        });
        if(modeResult.invalid() || shouldCancel)
        {
          return modeResult;
        }
      }
      else
      {
        const auto scanGroup = [&](usize requestedGroup, auto&& valueFunction) -> Result<> {
          for(usize offset = 0; offset < tupleCount; offset += k_ChunkTuples)
          {
            if(shouldCancel)
            {
              return {};
            }
            const usize count = std::min(k_ChunkTuples, tupleCount - offset);
            Result<> readResult = readChunk(offset, count);
            if(readResult.invalid())
            {
              return readResult;
            }
            if(shouldCancel)
            {
              return {};
            }
            for(usize index = 0; index < count; ++index)
            {
              if(maskStore != nullptr && !static_cast<bool>(maskBuffer[index]))
              {
                continue;
              }
              const std::optional<usize> currentGroup = layout.groupIndex(featureIdsStore == nullptr ? 0 : featureBuffer[index]);
              if(currentGroup.has_value() && *currentGroup == requestedGroup)
              {
                valueFunction(valueBuffer[index]);
              }
            }
          }
          return {};
        };
        for(usize group = 0; group < groupCount; ++group)
        {
          std::optional<T> previous;
          while(!shouldCancel)
          {
            std::optional<T> next;
            Result<> scanResult = scanGroup(group, [&](const T& value) {
              if((!previous.has_value() || *previous < value) && (!next.has_value() || value < *next))
              {
                next = value;
              }
            });
            if(scanResult.invalid() || shouldCancel)
            {
              return scanResult;
            }
            if(!next.has_value())
            {
              break;
            }
            uint64 occurrenceCount = 0;
            bool occurrenceOverflow = false;
            scanResult = scanGroup(group, [&](const T& value) {
              if(equivalentStatisticsValues(value, *next))
              {
                if(occurrenceCount == std::numeric_limits<uint64>::max())
                {
                  occurrenceOverflow = true;
                }
                else
                {
                  ++occurrenceCount;
                }
              }
            });
            if(scanResult.invalid() || shouldCancel)
            {
              return scanResult;
            }
            if(occurrenceOverflow)
            {
              return MakeErrorResult(-57304, fmt::format("ComputeArrayStatistics: occurrence count for output group {} exceeds uint64.", group));
            }
            if(occurrenceCount == maximumModeCounts[group])
            {
              Result<> appendResult = appendMode(group, *next);
              if(appendResult.invalid())
              {
                return appendResult;
              }
            }
            previous = next;
          }
        }
      }
    }
  }

  if(shouldCancel)
  {
    return {};
  }
  // Feature-scale outputs are committed in bulk only after every reduction has
  // succeeded, minimizing disk transactions and avoiding partially interleaved writes.
  if(inputValues.ComputeByIndex)
  {
    auto hasData = std::make_unique<bool[]>(groupCount);
    for(usize group = 0; group < groupCount; ++group)
    {
      hasData[group] = lengths[group] > 0;
    }
    Result<> writeResult = writeStatisticsOutput(featureHasDataArray, hasData.get(), groupCount, inputValues.FeatureHasDataArrayName);
    if(writeResult.invalid())
    {
      return writeResult;
    }
  }
  if(shouldCancel)
  {
    return {};
  }
  if(inputValues.FindLength)
  {
    Result<> writeResult = writeStatisticsOutput(lengthArray, lengths.data(), groupCount, inputValues.LengthArrayName);
    if(writeResult.invalid())
    {
      return writeResult;
    }
  }
  if(shouldCancel)
  {
    return {};
  }
  if(inputValues.FindMin)
  {
    Result<> writeResult = writeStatisticsOutput(minimumArray, minimums.get(), groupCount, inputValues.MinimumArrayName);
    if(writeResult.invalid())
    {
      return writeResult;
    }
  }
  if(shouldCancel)
  {
    return {};
  }
  if(inputValues.FindMax)
  {
    Result<> writeResult = writeStatisticsOutput(maximumArray, maximums.get(), groupCount, inputValues.MaximumArrayName);
    if(writeResult.invalid())
    {
      return writeResult;
    }
  }
  if(shouldCancel)
  {
    return {};
  }
  if(inputValues.FindMean)
  {
    Result<> writeResult = writeStatisticsOutput(meanArray, means.data(), groupCount, inputValues.MeanArrayName);
    if(writeResult.invalid())
    {
      return writeResult;
    }
  }
  if(shouldCancel)
  {
    return {};
  }
  if(inputValues.FindMedian)
  {
    Result<> writeResult = writeStatisticsOutput(medianArray, medians.data(), groupCount, inputValues.MedianArrayName);
    if(writeResult.invalid())
    {
      return writeResult;
    }
  }
  if(shouldCancel)
  {
    return {};
  }
  if(inputValues.FindStdDeviation)
  {
    Result<> writeResult = writeStatisticsOutput(stdDeviationArray, standardDeviations.data(), groupCount, inputValues.StdDeviationArrayName);
    if(writeResult.invalid())
    {
      return writeResult;
    }
  }
  if(shouldCancel)
  {
    return {};
  }
  if(inputValues.FindSummation)
  {
    Result<> writeResult = writeStatisticsOutput(summationArray, sums.data(), groupCount, inputValues.SummationArrayName);
    if(writeResult.invalid())
    {
      return writeResult;
    }
  }
  if(shouldCancel)
  {
    return {};
  }
  if(inputValues.FindNumUniqueValues)
  {
    Result<> writeResult = writeStatisticsOutput(uniqueValuesArray, uniqueCounts.data(), groupCount, inputValues.NumUniqueValuesName);
    if(writeResult.invalid())
    {
      return writeResult;
    }
  }

  if(shouldCancel)
  {
    return {};
  }
  // Standardization depends on final mean and deviation values. Stream the
  // source once more and update fixed-size destination pages in place.
  if(inputValues.StandardizeData)
  {
    auto standardizedBuffer = std::make_unique<float32[]>(k_ChunkTuples);
    auto& standardizedStore = standardizedArray->getDataStoreRef();
    for(usize offset = 0; offset < tupleCount; offset += k_ChunkTuples)
    {
      if(shouldCancel)
      {
        return {};
      }
      const usize count = std::min(k_ChunkTuples, tupleCount - offset);
      Result<> readResult = readChunk(offset, count);
      if(readResult.invalid())
      {
        return readResult;
      }
      if(shouldCancel)
      {
        return {};
      }
      readResult = standardizedStore.copyIntoBuffer(offset, nonstd::span<float32>(standardizedBuffer.get(), count));
      if(readResult.invalid())
      {
        return readResult;
      }
      for(usize index = 0; index < count; ++index)
      {
        if(maskStore != nullptr && !static_cast<bool>(maskBuffer[index]))
        {
          continue;
        }
        const std::optional<usize> group = layout.groupIndex(featureIdsStore == nullptr ? 0 : featureBuffer[index]);
        if(!group.has_value())
        {
          continue;
        }
        standardizedBuffer[index] = (static_cast<float32>(valueBuffer[index]) - means[*group]) / standardDeviations[*group];
      }
      Result<> writeResult = standardizedStore.copyFromBuffer(offset, nonstd::span<const float32>(standardizedBuffer.get(), count));
      if(writeResult.invalid())
      {
        return writeResult;
      }
    }
  }
  return {};
}

/** @brief Runtime dispatch adapter for the bounded value and mask types. */
struct StatisticsScanlineFunctor
{
  /** @brief Selects an absent, Boolean, or UInt8 mask and invokes the typed scan. */
  template <typename T>
  Result<> operator()(DataStructure& dataStructure, const IDataArray& inputArray, const IDataArray* featureIdsArray, const IDataArray* maskArray, const StatisticsGroupLayout& layout,
                      const ComputeArrayStatisticsInputValues& inputValues, const std::atomic_bool& shouldCancel) const
  {
    if(maskArray == nullptr)
    {
      return generateScanlineStatistics<T, uint8>(dataStructure, inputArray, featureIdsArray, nullptr, layout, inputValues, shouldCancel);
    }
    if(maskArray->getDataType() == DataType::boolean)
    {
      return generateScanlineStatistics<T, bool>(dataStructure, inputArray, featureIdsArray, maskArray, layout, inputValues, shouldCancel);
    }
    if(maskArray->getDataType() == DataType::uint8)
    {
      return generateScanlineStatistics<T, uint8>(dataStructure, inputArray, featureIdsArray, maskArray, layout, inputValues, shouldCancel);
    }
    return MakeErrorResult(-57315, fmt::format("ComputeArrayStatistics: mask '{}' must have Boolean or UInt8 values.", maskArray->getName()));
  }
};

/** @brief Dispatch wrapper around the original resident statistics callback. */
class ComputeArrayStatisticsDirect
{
public:
  /** @brief Retains a synchronous reference to the dispatcher-owned callback. */
  template <typename... ArgsT>
  explicit ComputeArrayStatisticsDirect(const std::function<Result<>()>& executeDirect, ArgsT&&...)
  : m_ExecuteDirect(executeDirect)
  {
  }

  /** @brief Runs the established in-memory implementation unchanged. */
  Result<> operator()() const
  {
    return m_ExecuteDirect();
  }

private:
  const std::function<Result<>()>& m_ExecuteDirect;
};

/**
 * @brief Dispatch wrapper for bounded statistics over resident or disk-backed stores.
 * All captured references are non-owning and used synchronously by operator().
 */
class ComputeArrayStatisticsScanline
{
public:
  /** @brief Captures resolved layout, source arrays, outputs, and execution controls. */
  ComputeArrayStatisticsScanline(const std::function<Result<>()>&, DataStructure& dataStructure, const IDataArray& inputArray, const IDataArray* featureIdsArray, const IDataArray* maskArray,
                                 const StatisticsGroupLayout& layout, const ComputeArrayStatisticsInputValues& inputValues, const std::atomic_bool& shouldCancel)
  : m_DataStructure(dataStructure)
  , m_InputArray(inputArray)
  , m_FeatureIdsArray(featureIdsArray)
  , m_MaskArray(maskArray)
  , m_Layout(layout)
  , m_InputValues(inputValues)
  , m_ShouldCancel(shouldCancel)
  {
  }

  /** @brief Dispatches the bounded algorithm on the input array's runtime type. */
  Result<> operator()() const
  {
    return ExecuteDataFunction(StatisticsScanlineFunctor{}, m_InputArray.getDataType(), m_DataStructure, m_InputArray, m_FeatureIdsArray, m_MaskArray, m_Layout, m_InputValues, m_ShouldCancel);
  }

private:
  DataStructure& m_DataStructure;
  const IDataArray& m_InputArray;
  const IDataArray* m_FeatureIdsArray = nullptr;
  const IDataArray* m_MaskArray = nullptr;
  const StatisticsGroupLayout& m_Layout;
  const ComputeArrayStatisticsInputValues& m_InputValues;
  const std::atomic_bool& m_ShouldCancel;
};
} // namespace

// -----------------------------------------------------------------------------
ComputeArrayStatistics::ComputeArrayStatistics(DataStructure& dataStructure, const IFilter::MessageHandler& msgHandler, const std::atomic_bool& shouldCancel,
                                               ComputeArrayStatisticsInputValues* inputValues)
: m_DataStructure(dataStructure)
, m_InputValues(inputValues)
, m_ShouldCancel(shouldCancel)
, m_MessageHandler(msgHandler)
{
}

// -----------------------------------------------------------------------------
ComputeArrayStatistics::~ComputeArrayStatistics() noexcept = default;

// -----------------------------------------------------------------------------
Result<> ComputeArrayStatistics::operator()()
{
  if(!m_InputValues->FindMin && !m_InputValues->FindMax && !m_InputValues->FindMean && !m_InputValues->FindMedian && !m_InputValues->FindMode && !m_InputValues->FindStdDeviation &&
     !m_InputValues->FindSummation && !m_InputValues->FindLength && !m_InputValues->FindNumUniqueValues)
  {
    return {};
  }
  if(m_ShouldCancel)
  {
    return {};
  }

  const auto* inputArray = m_DataStructure.getDataAs<IDataArray>(m_InputValues->SelectedArrayPath);
  if(inputArray == nullptr)
  {
    return MakeErrorResult(-57316, fmt::format("ComputeArrayStatistics: input array '{}' does not exist.", m_InputValues->SelectedArrayPath.toString()));
  }
  const auto* featureIdsArray = m_InputValues->ComputeByIndex ? m_DataStructure.getDataAs<Int32Array>(m_InputValues->FeatureIdsArrayPath) : nullptr;
  if(m_InputValues->ComputeByIndex && featureIdsArray == nullptr)
  {
    return MakeErrorResult(-57317, fmt::format("ComputeArrayStatistics: FeatureIds array '{}' does not exist or is not Int32.", m_InputValues->FeatureIdsArrayPath.toString()));
  }
  const auto* maskArray = m_InputValues->UseMask ? m_DataStructure.getDataAs<IDataArray>(m_InputValues->MaskArrayPath) : nullptr;
  if(m_InputValues->UseMask && (maskArray == nullptr || (maskArray->getDataType() != DataType::boolean && maskArray->getDataType() != DataType::uint8)))
  {
    return MakeErrorResult(-57315, fmt::format("ComputeArrayStatistics: mask '{}' does not exist or is not Boolean/UInt8.", m_InputValues->MaskArrayPath.toString()));
  }

  ObservedFeatureBounds observedBounds;
  if(featureIdsArray != nullptr)
  {
    Result<ObservedFeatureBounds> boundsResult = discoverFeatureBounds(featureIdsArray->getDataStoreRef(), m_ShouldCancel);
    if(boundsResult.invalid())
    {
      return ConvertResult(std::move(boundsResult));
    }
    if(m_ShouldCancel)
    {
      return {};
    }
    observedBounds = boundsResult.value();
  }
  Result<StatisticsGroupLayout> layoutResult = resolveGroupLayout(*m_InputValues, observedBounds);
  if(layoutResult.invalid())
  {
    return ConvertResult(std::move(layoutResult));
  }
  const StatisticsGroupLayout layout = layoutResult.value();
  if(m_ShouldCancel)
  {
    return {};
  }

  if(layout.ComputeByIndex)
  {
    auto* destination = m_DataStructure.getDataAs<AttributeMatrix>(m_InputValues->DestinationAttributeMatrix);
    if(destination == nullptr)
    {
      return MakeErrorResult(-57318, fmt::format("ComputeArrayStatistics: destination AttributeMatrix '{}' does not exist.", m_InputValues->DestinationAttributeMatrix.toString()));
    }
    try
    {
      destination->resizeTuples({layout.GroupCount});
    } catch(const std::exception& exception)
    {
      return MakeErrorResult(-57319, fmt::format("ComputeArrayStatistics: could not resize destination AttributeMatrix '{}' to {} tuples: {}", m_InputValues->DestinationAttributeMatrix.toString(),
                                                 layout.GroupCount, exception.what()));
    }
    if(m_ShouldCancel)
    {
      return {};
    }
    if(layout.CreateFeatureIdMap)
    {
      auto* featureIdMap = m_DataStructure.getDataAs<Int32Array>(m_InputValues->FeatureIdMapArrayPath);
      if(featureIdMap == nullptr)
      {
        return MakeErrorResult(-57320, fmt::format("ComputeArrayStatistics: FeatureId mapping output '{}' does not exist or is not Int32.", m_InputValues->FeatureIdMapArrayPath.toString()));
      }
      std::vector<int32> mapping(layout.GroupCount);
      for(usize index = 0; index < layout.GroupCount; ++index)
      {
        if(index % k_ChunkTuples == 0 && m_ShouldCancel)
        {
          return {};
        }
        mapping[index] = static_cast<int32>(static_cast<int64>(layout.FirstFeatureId) + static_cast<int64>(index));
      }
      if(m_ShouldCancel)
      {
        return {};
      }
      Result<> mapWriteResult = featureIdMap->getDataStoreRef().copyFromBuffer(0, nonstd::span<const int32>(mapping.data(), mapping.size()));
      if(mapWriteResult.invalid())
      {
        return mapWriteResult;
      }
      if(m_ShouldCancel)
      {
        return {};
      }
    }
  }

  std::vector<IArray*> arrays(10, nullptr);
  arrays[0] = m_InputValues->FindLength ? m_DataStructure.getDataAs<IDataArray>(m_InputValues->LengthArrayName) : nullptr;
  arrays[1] = m_InputValues->FindMin ? m_DataStructure.getDataAs<IDataArray>(m_InputValues->MinimumArrayName) : nullptr;
  arrays[2] = m_InputValues->FindMax ? m_DataStructure.getDataAs<IDataArray>(m_InputValues->MaximumArrayName) : nullptr;
  arrays[3] = m_InputValues->FindMean ? m_DataStructure.getDataAs<IDataArray>(m_InputValues->MeanArrayName) : nullptr;
  arrays[4] = m_InputValues->FindMedian ? m_DataStructure.getDataAs<IDataArray>(m_InputValues->MedianArrayName) : nullptr;
  arrays[5] = m_InputValues->FindMode ? m_DataStructure.getDataAs<INeighborList>(m_InputValues->ModeArrayName) : nullptr;
  arrays[6] = m_InputValues->FindStdDeviation ? m_DataStructure.getDataAs<IDataArray>(m_InputValues->StdDeviationArrayName) : nullptr;
  arrays[7] = m_InputValues->FindSummation ? m_DataStructure.getDataAs<IDataArray>(m_InputValues->SummationArrayName) : nullptr;
  arrays[8] = m_InputValues->FindNumUniqueValues ? m_DataStructure.getDataAs<IDataArray>(m_InputValues->NumUniqueValuesName) : nullptr;
  arrays[9] = m_InputValues->ComputeByIndex ? m_DataStructure.getDataAs<IDataArray>(m_InputValues->FeatureHasDataArrayName) : nullptr;

  MessageHelper messageHelper(m_MessageHandler);
  const std::function<Result<>()> executeDirect = [&]() -> Result<> {
    if(inputArray->getDataType() == DataType::boolean)
    {
      return ExecuteDataFunction(StatisticsScanlineFunctor{}, inputArray->getDataType(), m_DataStructure, *inputArray, featureIdsArray, maskArray, layout, *m_InputValues, m_ShouldCancel);
    }
    if(!m_InputValues->ComputeByIndex)
    {
      if(!m_InputValues->FindMode)
      {
        return ExecuteDataFunctionNoBool(ComputeArrayStatisticsFunctor{}, inputArray->getDataType(), m_DataStructure, *inputArray, arrays, m_InputValues);
      }
      return ExecuteNeighborFunction(ComputeArrayStatisticsFunctor{}, inputArray->getDataType(), m_DataStructure, *inputArray, arrays, m_InputValues);
    }
    if(layout.GroupCount == 0)
    {
      return ExecuteDataFunction(StatisticsScanlineFunctor{}, inputArray->getDataType(), m_DataStructure, *inputArray, featureIdsArray, maskArray, layout, *m_InputValues, m_ShouldCancel);
    }
    if(m_InputValues->RangeType == to_underlying(FeatureIdRangeControls::None))
    {
      if(!m_InputValues->FindMode)
      {
        return ExecuteDataFunctionNoBool(ComputeArrayStatisticsByFeatureFunctor{}, inputArray->getDataType(), m_DataStructure, inputArray, arrays, layout.GroupCount, m_InputValues, m_ShouldCancel,
                                         messageHelper);
      }
      return ExecuteNeighborFunction(ComputeArrayStatisticsByFeatureFunctor{}, inputArray->getDataType(), m_DataStructure, inputArray, arrays, layout.GroupCount, m_InputValues, m_ShouldCancel,
                                     messageHelper);
    }
    if(!m_InputValues->FindMode)
    {
      return ExecuteDataFunctionNoBool(ComputeArrayStatisticsByFeatureFunctor{}, inputArray->getDataType(), m_DataStructure, inputArray, arrays,
                                       std::make_pair(layout.FirstFeatureId, layout.LastFeatureId), m_InputValues, m_ShouldCancel);
    }
    return ExecuteNeighborFunction(ComputeArrayStatisticsByFeatureFunctor{}, inputArray->getDataType(), m_DataStructure, inputArray, arrays,
                                   std::make_pair(layout.FirstFeatureId, layout.LastFeatureId), m_InputValues, m_ShouldCancel);
  };

  std::vector<const IArray*> targets;
  targets.reserve(14);
  targets.push_back(inputArray);
  if(featureIdsArray != nullptr)
  {
    targets.push_back(featureIdsArray);
  }
  if(maskArray != nullptr)
  {
    targets.push_back(maskArray);
  }
  for(IArray* output : arrays)
  {
    if(output != nullptr)
    {
      targets.push_back(output);
    }
  }
  if(layout.CreateFeatureIdMap)
  {
    targets.push_back(m_DataStructure.getDataAs<IDataArray>(m_InputValues->FeatureIdMapArrayPath));
  }
  if(m_InputValues->StandardizeData)
  {
    targets.push_back(m_DataStructure.getDataAs<IDataArray>(m_InputValues->StandardizedArrayName));
  }

  return DispatchAlgorithm<ComputeArrayStatisticsDirect, ComputeArrayStatisticsScanline>(AlgorithmArrayTargets(std::move(targets)), executeDirect, m_DataStructure, *inputArray, featureIdsArray,
                                                                                         maskArray, layout, *m_InputValues, m_ShouldCancel);
}
