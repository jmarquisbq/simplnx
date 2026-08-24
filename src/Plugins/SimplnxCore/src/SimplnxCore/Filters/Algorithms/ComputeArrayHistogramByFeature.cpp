#include "ComputeArrayHistogramByFeature.hpp"

#include "SimplnxCore/Filters/ComputeArrayHistogramByFeatureFilter.hpp"
#include "simplnx/DataStructure/DataArray.hpp"
#include "simplnx/DataStructure/DataGroup.hpp"
#include "simplnx/DataStructure/INeighborList.hpp"
#include "simplnx/DataStructure/IO/Generic/IExternalSort.hpp"
#include "simplnx/Utilities/AlgorithmDispatch.hpp"
#include "simplnx/Utilities/DataStoreUtilities.hpp"
#include "simplnx/Utilities/FilterUtilities.hpp"
#include "simplnx/Utilities/HistogramUtilities.hpp"
#include "simplnx/Utilities/MessageHelper.hpp"
#include "simplnx/Utilities/ParallelAlgorithmUtilities.hpp"
#include "simplnx/Utilities/ParallelDataAlgorithm.hpp"
#include "simplnx/Utilities/ParallelTaskAlgorithm.hpp"

#include <nonstd/span.hpp>

#include <algorithm>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <tuple>
#include <type_traits>
#include <vector>

using namespace nx::core;

namespace
{
/**
 * @brief Discovers the output feature count with a single bounded scan of FeatureIds.
 * @return One greater than the largest nonnegative ID, zero on cancellation or
 * when no nonnegative IDs exist, or an error when a page read or size conversion fails.
 */
Result<usize> findFeatureCount(const AbstractDataStore<int32>& featureIdsStore, const std::atomic_bool& shouldCancel)
{
  constexpr usize k_ChunkTuples = 65536;
  const usize numTuples = featureIdsStore.getNumberOfTuples();
  std::vector<int32> buffer(k_ChunkTuples);
  int32 maxFeatureId = -1;
  for(usize offset = 0; offset < numTuples; offset += k_ChunkTuples)
  {
    if(shouldCancel)
    {
      return {usize{0}};
    }
    const usize count = std::min(k_ChunkTuples, numTuples - offset);
    Result<> readResult = featureIdsStore.copyIntoBuffer(offset, nonstd::span<int32>(buffer.data(), count));
    if(readResult.invalid())
    {
      return ConvertInvalidResult<usize>(std::move(readResult));
    }
    for(usize index = 0; index < count; ++index)
    {
      maxFeatureId = std::max(maxFeatureId, buffer[index]);
    }
  }
  if(maxFeatureId < 0)
  {
    return {usize{0}};
  }
  if(static_cast<uint64>(maxFeatureId) >= std::numeric_limits<usize>::max() - 1)
  {
    return MakeErrorResult<usize>(-23802, fmt::format("ComputeArrayHistogramByFeature: largest non-negative FeatureId ({}) cannot be converted to a feature count on this platform.", maxFeatureId));
  }
  return {static_cast<usize>(maxFeatureId) + 1};
}
} // namespace

/**
 * @class GenerateHistogramImpl
 * @brief This class is a pseudo-wrapper for the serial::GenerateHistogram, the reason for this class' existence is to hold/define ownership of objects in each thread
 * @tparam Type this the end type of the function in that the container and data values are of this type
 * @tparam SizeType this is the scalar type of the bin counts container
 */
template <typename Type, std::integral SizeType>
class GenerateFeatureHistogramImpl
{
public:
  /**
   * @function constructor
   * @brief This constructor requires a defined range and creates the object
   * @param inputStore this is the AbstractDataStore holding the data that will be binned
   * @param binRangesStore this is the AbstractDataStore that the ranges will be loaded into.
   * @param rangeMinMax this is assumed to be the inclusive minimum value and exclusive maximum value for the overall histogram bins. FORMAT: [minimum, maximum)
   * @param shouldCancel this is an atomic value that will determine whether execution ends early
   * @param numBins this is the total number of bin ranges being calculated and by extension the indexing value for the ranges
   * @param histogramStore this is the AbstractDataStore that will hold the counts for each bin (variable type sizing)
   * @param overflow this is an atomic counter for the number of values that fall outside the bin range
   */
  GenerateFeatureHistogramImpl(const AbstractDataStore<Type>& inputStore, AbstractDataStore<Type>& binRangesStore, NeighborList<Type>* modalBinRangesList,
                               const AbstractDataStore<int32>& featureIdsStore, float64 histMin, float64 histMax, bool histFullRange, const std::atomic_bool& shouldCancel, const int32 numBins,
                               AbstractDataStore<SizeType>& histogramStore, AbstractDataStore<SizeType>& mostPopulatedStore, const std::unique_ptr<MaskCompareUtilities::MaskCompare>& mask,
                               std::atomic<usize>& overflow, ProgressMessageHelper& progressMessageHelper)
  : m_InputStore(inputStore)
  , m_ShouldCancel(shouldCancel)
  , m_NumBins(numBins)
  , m_BinRangesStore(binRangesStore)
  , m_ModalBinRangesList(modalBinRangesList)
  , m_HistMin(histMin)
  , m_HistMax(histMax)
  , m_HistFullRange(histFullRange)
  , m_HistogramStore(histogramStore)
  , m_MostPopulatedStore(mostPopulatedStore)
  , m_FeatureIdsStore(featureIdsStore)
  , m_Mask(mask)
  , m_Overflow(overflow)
  , m_ProgressMessageHelper(progressMessageHelper)
  {
  }

  GenerateFeatureHistogramImpl(const AbstractDataStore<Type>& inputStore, AbstractDataStore<Type>& binRangesStore, const AbstractDataStore<int32>& featureIdsStore, float64 histMin, float64 histMax,
                               bool histFullRange, const std::atomic_bool& shouldCancel, const int32 numBins, AbstractDataStore<SizeType>& histogramStore,
                               AbstractDataStore<SizeType>& mostPopulatedStore, const std::unique_ptr<MaskCompareUtilities::MaskCompare>& mask, std::atomic<usize>& overflow,
                               ProgressMessageHelper& progressMessageHelper)
  : m_InputStore(inputStore)
  , m_ShouldCancel(shouldCancel)
  , m_NumBins(numBins)
  , m_BinRangesStore(binRangesStore)
  , m_ModalBinRangesList(nullptr)
  , m_HistMin(histMin)
  , m_HistMax(histMax)
  , m_HistFullRange(histFullRange)
  , m_HistogramStore(histogramStore)
  , m_MostPopulatedStore(mostPopulatedStore)
  , m_FeatureIdsStore(featureIdsStore)
  , m_Mask(mask)
  , m_Overflow(overflow)
  , m_ProgressMessageHelper(progressMessageHelper)
  {
  }

  ~GenerateFeatureHistogramImpl() = default;

  /**
   * @function operator()
   * @brief This function serves as the execute method
   */
  void operator()(const Range& range) const
  {
    compute(range.min(), range.max());
  }

  /**
   * @function compute
   * @brief Computes the histogram, bin ranges, most-populated bin, and (optionally) modal bin ranges for every
   * feature in [start, end). This is done in three bounded passes over the feature range rather than one full
   * rescan of the cell arrays per feature: (1) derive each feature's bin edges/increment from the length/min/max
   * stats already gathered by CalculateFeatureHasDataStats, (2) a single chunked pass over every cell that bins
   * it directly into its owning feature's histogram, and (3) a per-feature finalization pass that writes the
   * outputs and computes modal bin ranges. Because every feature's bin edges are known before any cell is read,
   * one pass over the cells suffices to bin all of them - there is no need to revisit the cell arrays once per
   * feature, which is what made this scale as O(features * cells) previously.
   * @param start the inclusive first feature id handled by this call (parallel dispatches split the full
   * feature range across calls)
   * @param end the exclusive last feature id handled by this call
   */
  void compute(usize start, usize end) const
  {
    ProgressMessenger progressMessenger = m_ProgressMessageHelper.createProgressMessenger();

    const usize numTuples = m_FeatureIdsStore.getNumberOfTuples();
    const usize numCurrentFeatures = end - start;

    auto [length, min, max, summation, modalMaps] = HistogramUtilities::concurrent::CalculateFeatureHasDataStats(m_InputStore, m_FeatureIdsStore, start, end, m_Mask, {}, m_ShouldCancel);
    if(m_ShouldCancel)
    {
      return;
    }

    // ---- Pass 1: per-feature bin-edge precompute (feature-level; O(numCurrentFeatures)) ----
    // Bin range, increment, and the "degenerate range" short-circuit only depend on this feature's
    // own [min, max] (or the user-supplied range) and numBins - none of that requires visiting a
    // single cell. Computing it up front for every feature in this chunk lets the cell scan below
    // become a single pass instead of a per-feature rescan.
    std::vector<Type> histMinPerFeature(numCurrentFeatures, static_cast<Type>(0));
    std::vector<float32> incrementPerFeature(numCurrentFeatures, 0.0F);
    std::vector<std::vector<Type>> rangesPerFeature(numCurrentFeatures);
    std::vector<std::vector<uint64>> histogramPerFeature(numCurrentFeatures);

    for(usize localFeatureIndex = 0; localFeatureIndex < numCurrentFeatures; localFeatureIndex++)
    {
      if(m_ShouldCancel)
      {
        return;
      }

      rangesPerFeature[localFeatureIndex] = std::vector<Type>(m_NumBins * 2);
      histogramPerFeature[localFeatureIndex] = std::vector<uint64>(m_NumBins, 0);

      if(length[localFeatureIndex] == 0)
      {
        continue; // no data for this feature: ranges/histogram stay zeroed, matching legacy behavior
      }

      auto histMin = static_cast<Type>(m_HistMin);
      auto histMax = static_cast<Type>(m_HistMax);
      if(m_HistFullRange)
      {
        histMin = min[localFeatureIndex];
        histMax = max[localFeatureIndex] + static_cast<Type>(1.0);
      }

      HistogramUtilities::serial::FillBinRanges(rangesPerFeature[localFeatureIndex], std::make_pair(histMin, histMax), m_NumBins);

      const float32 increment = HistogramUtilities::serial::CalculateIncrement(histMin, histMax, m_NumBins);
      histMinPerFeature[localFeatureIndex] = histMin;
      incrementPerFeature[localFeatureIndex] = increment;

      // A degenerate (near-zero width) range means every value for this feature falls in bin 0.
      // There is nothing a cell scan could add, so this feature is fully resolved without ever
      // reading a cell - the single-pass scan below skips it entirely (see the increment check).
      if(std::fabs(increment) < 1E-10)
      {
        histogramPerFeature[localFeatureIndex][0] = length[localFeatureIndex];
      }
    }

    // ---- Pass 2: single sequential scan over cells, bounded chunks via copyIntoBuffer ----
    // The previous design rescanned the *entire* cell array once per feature (O(features * cells)),
    // which dominates runtime once there are more than a handful of features and drives millions of
    // redundant per-element reads against potentially disk-backed FeatureIds/input arrays. Since
    // every feature's bin edges are already known (Pass 1), each cell can be routed straight to its
    // owning feature's histogram bin in a single O(cells) pass, reading in bounded chunks instead of
    // one element at a time.
    constexpr usize k_ChunkTuples = 65536;
    auto featureIdsBuffer = std::make_unique<int32[]>(k_ChunkTuples);
    auto valueBuffer = std::make_unique<Type[]>(k_ChunkTuples);

    for(usize chunkStart = 0; chunkStart < numTuples; chunkStart += k_ChunkTuples)
    {
      if(m_ShouldCancel)
      {
        return;
      }

      const usize chunkTupleCount = std::min(k_ChunkTuples, numTuples - chunkStart);
      m_FeatureIdsStore.copyIntoBuffer(chunkStart, nonstd::span<int32>(featureIdsBuffer.get(), chunkTupleCount));
      m_InputStore.copyIntoBuffer(chunkStart, nonstd::span<Type>(valueBuffer.get(), chunkTupleCount));

      for(usize cellIdx = 0; cellIdx < chunkTupleCount; cellIdx++)
      {
        const usize globalIdx = chunkStart + cellIdx;
        if(m_Mask != nullptr && !m_Mask->isTrue(globalIdx))
        {
          continue;
        }

        const int32 featureId = featureIdsBuffer[cellIdx];
        if(featureId < static_cast<int32>(start) || featureId >= static_cast<int32>(end))
        {
          continue; // this cell's feature is owned by a different feature-range chunk of this parallel dispatch
        }

        const usize localFeatureIndex = static_cast<usize>(featureId) - start;
        if(length[localFeatureIndex] == 0)
        {
          continue; // defensive: cannot happen given the mask/feature match above already implies length > 0
        }

        const float32 increment = incrementPerFeature[localFeatureIndex];
        if(std::fabs(increment) < 1E-10)
        {
          continue; // degenerate range already fully resolved by the direct-count short-circuit in Pass 1
        }

        // Materialize a concrete Type before calling CalculateBin: when Type == bool, indexing
        // std::vector<bool> yields a proxy reference rather than a plain bool, and CalculateBin's
        // single template parameter requires both the value and the min arguments to deduce to the
        // exact same concrete type.
        const Type histMin = histMinPerFeature[localFeatureIndex];
        const Type value = valueBuffer[cellIdx];
        const auto bin = static_cast<int32>(HistogramUtilities::serial::CalculateBin(value, histMin, increment)); // find bin for this input array value
        if((bin >= 0) && (bin < m_NumBins))                                                                       // make certain bin is in range
        {
          histogramPerFeature[localFeatureIndex][bin]++; // increment histogram element corresponding to this input array value
        }
        else
        {
          m_Overflow++;
        }
      }
    }

    // ---- Pass 3: per-feature finalization - modal bin ranges, output writes, progress ----
    usize progressIncrement = numCurrentFeatures / 100;
    usize progressCount = 0;
    for(usize j = start; j < end; j++)
    {
      if(m_ShouldCancel)
      {
        return;
      }
      const usize localFeatureIndex = j - start;
      const std::vector<uint64>& histogram = histogramPerFeature[localFeatureIndex];
      const std::vector<Type>& ranges = rangesPerFeature[localFeatureIndex];

      if(length[localFeatureIndex] > 0)
      {
        const Type histMin = histMinPerFeature[localFeatureIndex];
        const float32 increment = incrementPerFeature[localFeatureIndex];

        // Bool breaks neighbor lists; if we have made it here we know m_ModalBinRangesList is a nullptr
        if constexpr(!std::is_same_v<Type, bool>)
        {
          if(m_ModalBinRangesList != nullptr)
          {
            if(std::fabs(increment) < 1E-10)
            {
              // The historical serial oracle is the full feature range. Using this worker's
              // TBB [start, end) block made the emitted value partition-dependent.
              m_ModalBinRangesList->addEntry(j, static_cast<Type>(0));
              m_ModalBinRangesList->addEntry(j, static_cast<Type>(m_HistogramStore.getNumberOfTuples()));
            }
            else if(!modalMaps[localFeatureIndex].empty())
            {
              // Find the maximum occurrence
              auto pr = std::max_element(modalMaps[localFeatureIndex].begin(), modalMaps[localFeatureIndex].end(), [](const auto& x, const auto& y) { return x.second < y.second; });
              int maxCount = pr->second;

              // Store all values that have this maximum occurrence under the proper feature id
              for(const auto& modalPair : modalMaps[localFeatureIndex])
              {
                if(modalPair.second == maxCount)
                {
                  const Type mode = modalPair.first;
                  const auto modalBin = HistogramUtilities::serial::CalculateBin(mode, histMin, increment);
                  if((modalBin >= 0) && (modalBin < m_NumBins)) // make certain bin is in range
                  {
                    m_ModalBinRangesList->addEntry(j, ranges[modalBin]);
                    m_ModalBinRangesList->addEntry(j, ranges[modalBin + 1]);
                  }
                }
              }
            }
          }
        }
      } // end of length if

      for(usize k = 0; k < histogram.size(); k++)
      {
        m_HistogramStore.setComponent(j, k, histogram[k]);
      }
      for(usize k = 0; k < ranges.size(); k++)
      {
        m_BinRangesStore.setComponent(j, k, ranges[k]);
      }

      auto maxElementIt = std::max_element(histogram.begin(), histogram.end());
      uint64 index = std::distance(histogram.begin(), maxElementIt);
      m_MostPopulatedStore.setComponent(j, 0, index);
      m_MostPopulatedStore.setComponent(j, 1, histogram[index]);

      progressCount++;
      if(progressCount > progressIncrement)
      {
        progressMessenger.sendProgressMessage(progressCount,
                                              [&](usize currentProgress, usize maxProgress) { return fmt::format("Calculating feature histograms {}/{}", currentProgress, maxProgress); });
        progressCount = 0;
      }
    }

    // Send one at the end so that the progress is communicated properly
    progressMessenger.sendProgressMessage(progressCount);
  }

private:
  const std::atomic_bool& m_ShouldCancel;
  float64 m_HistMin;
  float64 m_HistMax;
  bool m_HistFullRange;
  int32 m_NumBins;
  const std::unique_ptr<MaskCompareUtilities::MaskCompare>& m_Mask;
  const AbstractDataStore<Type>& m_InputStore;
  const AbstractDataStore<int32>& m_FeatureIdsStore;
  AbstractDataStore<SizeType>& m_HistogramStore;
  AbstractDataStore<Type>& m_BinRangesStore;
  AbstractDataStore<uint64>& m_MostPopulatedStore;
  NeighborList<Type>* m_ModalBinRangesList;
  std::atomic<usize>& m_Overflow;
  ProgressMessageHelper& m_ProgressMessageHelper;
};

/**
 * @class InstantiateHistogramImplFunctor
 * @brief This is a compatibility functor that leverages existing typecasting functions to create the appropriately typed GenerateHistogramImpl() cleanly.
 * Designed for compatibility with the existing parallel execution classes.
 */
struct InstantiateHistogramByFeatureImplFunctor
{
  template <typename T, class... ArgsT>
  auto operator()(INeighborList* modalBinRangesNL, const IDataArray* inputArray, IDataArray* binRangesArray, ArgsT&&... args)
  {
    return GenerateFeatureHistogramImpl(inputArray->template getIDataStoreRefAs<AbstractDataStore<T>>(), binRangesArray->template getIDataStoreRefAs<AbstractDataStore<T>>(),
                                        dynamic_cast<NeighborList<T>*>(modalBinRangesNL), std::forward<ArgsT>(args)...);
  }
  template <typename T, class... ArgsT>
  auto operator()(const IDataArray* inputArray, IDataArray* binRangesArray, ArgsT&&... args)
  {
    return GenerateFeatureHistogramImpl(inputArray->template getIDataStoreRefAs<AbstractDataStore<T>>(), binRangesArray->template getIDataStoreRefAs<AbstractDataStore<T>>(),
                                        std::forward<ArgsT>(args)...);
  }
};

namespace
{
constexpr usize k_HistogramChunkTuples = 65536;

/** @brief Multiplies two allocation dimensions without wrapping usize. */
bool checkedMultiply(usize lhs, usize rhs, usize& product)
{
  if(lhs != 0 && rhs > std::numeric_limits<usize>::max() / lhs)
  {
    return false;
  }
  product = lhs * rhs;
  return true;
}

template <typename T>
constexpr uint64 k_ModalRecordSize = sizeof(int32) + sizeof(T) + sizeof(uint64);

/**
 * @brief Serializes a modal candidate as (feature ID, value, original tuple).
 * The tuple index supplies a deterministic final ordering for equal values.
 */
template <typename T>
void encodeModalRecord(nonstd::span<std::byte> bytes, int32 featureId, T value, uint64 originalTupleIndex)
{
  std::memcpy(bytes.data(), &featureId, sizeof(featureId));
  std::memcpy(bytes.data() + sizeof(featureId), &value, sizeof(value));
  std::memcpy(bytes.data() + sizeof(featureId) + sizeof(value), &originalTupleIndex, sizeof(originalTupleIndex));
}

/** @brief Reconstructs a modal candidate written by encodeModalRecord(). */
template <typename T>
void decodeModalRecord(nonstd::span<const std::byte> bytes, int32& featureId, T& value, uint64& originalTupleIndex)
{
  std::memcpy(&featureId, bytes.data(), sizeof(featureId));
  std::memcpy(&value, bytes.data() + sizeof(featureId), sizeof(value));
  std::memcpy(&originalTupleIndex, bytes.data() + sizeof(featureId) + sizeof(value), sizeof(originalTupleIndex));
}

/**
 * @brief Orders modal records by feature, value, then original tuple index so
 * equal values become contiguous while sorting remains deterministic.
 */
template <typename T>
int32 compareModalRecords(nonstd::span<const std::byte> left, nonstd::span<const std::byte> right)
{
  int32 leftFeature = 0;
  int32 rightFeature = 0;
  T leftValue{};
  T rightValue{};
  uint64 leftIndex = 0;
  uint64 rightIndex = 0;
  decodeModalRecord(left, leftFeature, leftValue, leftIndex);
  decodeModalRecord(right, rightFeature, rightValue, rightIndex);
  if(leftFeature != rightFeature)
  {
    return leftFeature < rightFeature ? -1 : 1;
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

/** @brief Tests value equality using only operator<, including for generic numeric types. */
template <typename T>
bool equivalentModalValues(const T& left, const T& right)
{
  return !(left < right) && !(right < left);
}

/** @brief Applies the histogram's half-open [minimum, maximum) range rule. */
template <typename T>
bool isInHistogramRange(const T& value, const T& minimum, const T& maximum)
{
  return !(value < minimum) && value < maximum;
}

/**
 * @brief Calculates a bin index without overflowing signed integral subtraction.
 * The floating-point path is used only when value - minimum is not representable in T.
 */
template <typename T>
int32 calculateSafeBin(const T& value, const T& minimum, float32 increment)
{
  if constexpr(std::is_signed_v<T> && std::is_integral_v<T>)
  {
    if(value > 0 && minimum < 0 && value > std::numeric_limits<T>::max() + minimum)
    {
      return static_cast<int32>(std::floor((static_cast<float32>(value) - static_cast<float32>(minimum)) / increment));
    }
  }
  return static_cast<int32>(HistogramUtilities::serial::CalculateBin(value, minimum, increment));
}

/**
 * @brief Appends the bin-range pair associated with one modal value.
 * Values outside the configured histogram range are deliberately omitted to
 * preserve the direct algorithm's modal NeighborList behavior.
 */
template <typename T>
void appendModalRange(NeighborList<T>& modalBinRanges, usize feature, const T& value, const T* ranges, usize bins, const T& minimum, const T& maximum, float32 increment)
{
  if(!isInHistogramRange(value, minimum, maximum))
  {
    return;
  }
  const int32 bin = calculateSafeBin(value, minimum, increment);
  if(bin >= 0 && static_cast<usize>(bin) < bins)
  {
    // Preserve the historical Direct-path modal oracle, which indexes the feature's flat pair
    // buffer by bin rather than by the pair's first component.
    const usize rangeOffset = feature * bins * 2 + static_cast<usize>(bin);
    modalBinRanges.addEntry(static_cast<int32>(feature), ranges[rangeOffset]);
    modalBinRanges.addEntry(static_cast<int32>(feature), ranges[rangeOffset + 1]);
  }
}

/**
 * @brief Streams an externally sorted record set and reports each contiguous
 * (feature, value) run to @p groupFunction.
 *
 * Reading in fixed record pages bounds memory while the external sorter keeps
 * exact modal grouping from requiring the full input in RAM.
 */
template <typename T, typename GroupFunction>
Result<> scanSortedModalRecords(const IExternalSort& externalSort, const std::atomic_bool& shouldCancel, GroupFunction&& groupFunction)
{
  constexpr uint64 k_RecordsPerRead = k_HistogramChunkTuples;
  std::vector<std::byte> bytes(static_cast<usize>(k_RecordsPerRead * k_ModalRecordSize<T>));
  std::optional<int32> currentFeature;
  std::optional<T> currentValue;
  uint64 currentCount = 0;
  const uint64 totalRecords = externalSort.recordCount();
  for(uint64 offset = 0; offset < totalRecords; offset += k_RecordsPerRead)
  {
    if(shouldCancel)
    {
      return {};
    }
    const uint64 count = std::min(k_RecordsPerRead, totalRecords - offset);
    Result<uint64> readResult = externalSort.read(offset, count, nonstd::span<std::byte>(bytes.data(), static_cast<usize>(count * k_ModalRecordSize<T>)), shouldCancel);
    if(readResult.invalid())
    {
      return ConvertResult(std::move(readResult));
    }
    if(readResult.value() != count)
    {
      return MakeErrorResult(
          -23811, fmt::format("ComputeArrayHistogramByFeature: external modal sort short read at record offset {}: requested {} records but received {}.", offset, count, readResult.value()));
    }
    for(uint64 index = 0; index < count; ++index)
    {
      int32 feature = 0;
      T value{};
      uint64 originalIndex = 0;
      const usize byteOffset = static_cast<usize>(index * k_ModalRecordSize<T>);
      decodeModalRecord(nonstd::span<const std::byte>(bytes.data() + byteOffset, k_ModalRecordSize<T>), feature, value, originalIndex);
      if(currentFeature.has_value() && *currentFeature == feature && equivalentModalValues(*currentValue, value))
      {
        if(currentCount == std::numeric_limits<uint64>::max())
        {
          return MakeErrorResult(-23812, fmt::format("ComputeArrayHistogramByFeature: modal-value count for FeatureId {} and value {} exceeds uint64.", *currentFeature, *currentValue));
        }
        ++currentCount;
      }
      else
      {
        if(currentFeature.has_value())
        {
          groupFunction(*currentFeature, *currentValue, currentCount);
        }
        currentFeature = feature;
        currentValue = value;
        currentCount = 1;
      }
    }
  }
  if(currentFeature.has_value())
  {
    groupFunction(*currentFeature, *currentValue, currentCount);
  }
  return {};
}

/**
 * @brief Exact provider-free modal fallback for one feature.
 *
 * Repeated bounded scans select the next distinct value and count it. This can
 * be slower than external sorting, but it preserves exact results without an
 * input-sized scratch allocation when no external-sort provider is registered.
 */
template <typename T, typename MaskT, typename GroupFunction>
Result<> scanFallbackModalGroups(const AbstractDataStore<T>& inputStore, const AbstractDataStore<int32>& featureIdsStore, const AbstractDataStore<MaskT>* maskStore, usize tupleCount, int32 featureId,
                                 const std::atomic_bool& shouldCancel, GroupFunction&& groupFunction)
{
  auto values = std::make_unique<T[]>(k_HistogramChunkTuples);
  std::vector<int32> features(k_HistogramChunkTuples);
  auto masks = maskStore == nullptr ? nullptr : std::make_unique<MaskT[]>(k_HistogramChunkTuples);
  const auto scan = [&](auto&& valueFunction) -> Result<> {
    for(usize offset = 0; offset < tupleCount; offset += k_HistogramChunkTuples)
    {
      if(shouldCancel)
      {
        return {};
      }
      const usize count = std::min(k_HistogramChunkTuples, tupleCount - offset);
      Result<> result = featureIdsStore.copyIntoBuffer(offset, nonstd::span<int32>(features.data(), count));
      if(result.invalid())
      {
        return result;
      }
      result = inputStore.copyIntoBuffer(offset, nonstd::span<T>(values.get(), count));
      if(result.invalid())
      {
        return result;
      }
      if(maskStore != nullptr)
      {
        result = maskStore->copyIntoBuffer(offset, nonstd::span<MaskT>(masks.get(), count));
        if(result.invalid())
        {
          return result;
        }
      }
      for(usize index = 0; index < count; ++index)
      {
        if(features[index] == featureId && (maskStore == nullptr || static_cast<bool>(masks[index])))
        {
          valueFunction(values[index]);
        }
      }
    }
    return {};
  };

  std::optional<T> previous;
  while(!shouldCancel)
  {
    std::optional<T> next;
    Result<> result = scan([&](const T& value) {
      if((!previous.has_value() || *previous < value) && (!next.has_value() || value < *next))
      {
        next = value;
      }
    });
    if(result.invalid() || shouldCancel)
    {
      return result;
    }
    if(!next.has_value())
    {
      break;
    }
    uint64 count = 0;
    bool countOverflow = false;
    result = scan([&](const T& value) {
      if(equivalentModalValues(value, *next))
      {
        if(count == std::numeric_limits<uint64>::max())
        {
          countOverflow = true;
          return;
        }
        ++count;
      }
    });
    if(result.invalid() || shouldCancel)
    {
      return result;
    }
    if(countOverflow)
    {
      return MakeErrorResult(-23812, fmt::format("ComputeArrayHistogramByFeature: modal-value count for FeatureId {} and value {} exceeds uint64.", featureId, *next));
    }
    groupFunction(*next, count);
    previous = next;
  }
  return {};
}

/**
 * @brief OOC implementation for one type-dispatched input array.
 *
 * The algorithm separates discovery, bin construction, counting, modal
 * reduction, and output writes. Each cell pass uses fixed-size pages. Modal
 * values use external sorting when available and the exact bounded fallback
 * otherwise; neither path requires a scratch array proportional to all tuples.
 */
template <typename T, typename MaskT>
Result<> generateScanlineHistogram(const IDataArray& inputArray, IDataArray& binRangesArray, const AbstractDataStore<int32>& featureIdsStore, const AbstractDataStore<MaskT>* maskStore,
                                   DataArray<uint64>& countsArray, DataArray<uint64>& mostPopulatedArray, INeighborList* modalBinRanges, const ComputeArrayHistogramByFeatureInputValues& inputValues,
                                   usize numFeatures, std::atomic<usize>& overflow, const std::atomic_bool& shouldCancel)
{
  if constexpr(std::is_same_v<T, bool>)
  {
    if(modalBinRanges != nullptr)
    {
      return MakeErrorResult(-23813,
                             fmt::format("ComputeArrayHistogramByFeature: Boolean input array '{}' cannot produce modal NeighborList output '{}'.", inputArray.getName(), modalBinRanges->getName()));
    }
  }
  else if(modalBinRanges != nullptr && dynamic_cast<NeighborList<T>*>(modalBinRanges) == nullptr)
  {
    return MakeErrorResult(-23813, fmt::format("ComputeArrayHistogramByFeature: modal NeighborList '{}' is incompatible with input array '{}' and cannot store its modal bin ranges.",
                                               modalBinRanges->getName(), inputArray.getName()));
  }
  const int32 numBins = inputValues.NumberOfBins;
  if(numBins <= 0)
  {
    return MakeErrorResult(-23803, fmt::format("ComputeArrayHistogramByFeature: NumberOfBins ({}) must be greater than zero for input array '{}'.", numBins, inputArray.getName()));
  }
  const usize bins = static_cast<usize>(numBins);
  usize featureBins = 0;
  usize rangeValues = 0;
  if(!checkedMultiply(numFeatures, bins, featureBins) || !checkedMultiply(featureBins, 2, rangeValues))
  {
    return MakeErrorResult(
        -23804, fmt::format("ComputeArrayHistogramByFeature: output shape for input array '{}' overflows the platform size type ({} features, {} bins).", inputArray.getName(), numFeatures, bins));
  }

  binRangesArray.resizeTuples({numFeatures});
  countsArray.resizeTuples({numFeatures});
  mostPopulatedArray.resizeTuples({numFeatures});

  const auto& inputStore = inputArray.template getIDataStoreRefAs<AbstractDataStore<T>>();
  auto& binRangesStore = binRangesArray.template getIDataStoreRefAs<AbstractDataStore<T>>();
  auto& countsStore = countsArray.getDataStoreRef();
  auto& mostPopulatedStore = mostPopulatedArray.getDataStoreRef();
  const usize tupleCount = featureIdsStore.getNumberOfTuples();
  if(inputStore.getNumberOfTuples() != tupleCount || (maskStore != nullptr && maskStore->getNumberOfTuples() != tupleCount))
  {
    return MakeErrorResult(-23805, fmt::format("ComputeArrayHistogramByFeature: input array '{}' has {} tuples, FeatureIds has {} tuples, and mask {} has {} tuples. These tuple counts must match.",
                                               inputArray.getName(), inputStore.getNumberOfTuples(), tupleCount, maskStore == nullptr ? "is disabled" : "array",
                                               maskStore == nullptr ? 0 : maskStore->getNumberOfTuples()));
  }

  std::vector<uint64> lengths(numFeatures, 0);
  auto minimums = std::make_unique<T[]>(numFeatures);
  auto maximums = std::make_unique<T[]>(numFeatures);
  std::vector<uint64> counts(featureBins, 0);
  auto ranges = std::make_unique<T[]>(rangeValues);
  std::fill_n(ranges.get(), rangeValues, T{});
  auto valueBuffer = std::make_unique<T[]>(k_HistogramChunkTuples);
  std::vector<int32> featureBuffer(k_HistogramChunkTuples);
  auto maskBuffer = maskStore == nullptr ? nullptr : std::make_unique<MaskT[]>(k_HistogramChunkTuples);

  std::unique_ptr<IExternalSort> externalSort;
  std::vector<std::byte> modalRecordBytes;
  if constexpr(!std::is_same_v<T, bool>)
  {
    if(modalBinRanges != nullptr && DataStoreUtilities::GetIOCollection().hasExternalSortCapability())
    {
      ExternalSortConfig config;
      config.recordSize = k_ModalRecordSize<T>;
      config.maxRecordsPerBatch = k_HistogramChunkTuples;
      config.compare = compareModalRecords<T>;
      Result<std::unique_ptr<IExternalSort>> sortResult = DataStoreUtilities::GetIOCollection().createExternalSort(config);
      if(sortResult.invalid())
      {
        return ConvertResult(std::move(sortResult));
      }
      externalSort = std::move(sortResult.value());
      modalRecordBytes.resize(k_HistogramChunkTuples * k_ModalRecordSize<T>);
    }
  }

  const auto readChunk = [&](usize offset, usize count) -> Result<> {
    auto result = featureIdsStore.copyIntoBuffer(offset, nonstd::span<int32>(featureBuffer.data(), count));
    if(result.invalid())
    {
      return result;
    }
    result = inputStore.copyIntoBuffer(offset, nonstd::span<T>(valueBuffer.get(), count));
    if(result.invalid())
    {
      return result;
    }
    if(maskStore != nullptr)
    {
      result = maskStore->copyIntoBuffer(offset, nonstd::span<MaskT>(maskBuffer.get(), count));
    }
    return result;
  };

  // Pass 1 discovers each feature's sample count and extrema. Bin ranges cannot
  // be finalized until these values are known for the entire input.
  for(usize offset = 0; offset < tupleCount; offset += k_HistogramChunkTuples)
  {
    if(shouldCancel)
    {
      return {};
    }
    const usize count = std::min(k_HistogramChunkTuples, tupleCount - offset);
    auto readResult = readChunk(offset, count);
    if(readResult.invalid())
    {
      return readResult;
    }
    for(usize index = 0; index < count; ++index)
    {
      const int32 featureId = featureBuffer[index];
      if(featureId < 0 || (maskStore != nullptr && !static_cast<bool>(maskBuffer[index])))
      {
        continue;
      }
      const usize feature = static_cast<usize>(featureId);
      if(feature >= numFeatures)
      {
        return MakeErrorResult(-23806, fmt::format("ComputeArrayHistogramByFeature: FeatureId {} at tuple {} in input array '{}' exceeds the discovered feature count {}.", featureId, offset + index,
                                                   inputArray.getName(), numFeatures));
      }
      const T value = valueBuffer[index];
      if(lengths[feature] == 0)
      {
        minimums[feature] = value;
        maximums[feature] = value;
      }
      else
      {
        minimums[feature] = std::min(static_cast<T>(minimums[feature]), value);
        maximums[feature] = std::max(static_cast<T>(maximums[feature]), value);
      }
      if(lengths[feature] == std::numeric_limits<uint64>::max())
      {
        return MakeErrorResult(-23807, fmt::format("ComputeArrayHistogramByFeature: sample count for FeatureId {} in input array '{}' exceeds uint64.", feature, inputArray.getName()));
      }
      ++lengths[feature];
    }
  }

  // Build per-feature bin geometry once, before the counting pass. Feature-scale
  // state stays resident because it is independent of the potentially huge cell count.
  auto histogramMinimums = std::make_unique<T[]>(numFeatures);
  std::vector<float32> increments(numFeatures, 0.0F);
  for(usize feature = 0; feature < numFeatures; ++feature)
  {
    if(shouldCancel)
    {
      return {};
    }
    if(lengths[feature] == 0)
    {
      continue;
    }
    T histMin = static_cast<T>(inputValues.MinRange);
    T histMax = static_cast<T>(inputValues.MaxRange);
    if(!inputValues.UserDefinedRange)
    {
      histMin = minimums[feature];
      if constexpr(std::is_integral_v<T> && !std::is_same_v<T, bool>)
      {
        if(maximums[feature] == std::numeric_limits<T>::max())
        {
          return MakeErrorResult(-23808,
                                 fmt::format("ComputeArrayHistogramByFeature: full-range maximum {} for FeatureId {} in input array '{}' cannot be incremented without overflowing the input type.",
                                             maximums[feature], feature, inputArray.getName()));
        }
      }
      histMax = maximums[feature] + static_cast<T>(1);
    }
    if constexpr(std::is_signed_v<T> && std::is_integral_v<T>)
    {
      if(histMax < histMin || (histMin < 0 && histMax > std::numeric_limits<T>::max() + histMin))
      {
        return MakeErrorResult(-23810, fmt::format("ComputeArrayHistogramByFeature: histogram range [{}, {}) for FeatureId {} in input array '{}' cannot be represented without signed overflow.",
                                                   histMin, histMax, feature, inputArray.getName()));
      }
    }
    if constexpr(std::is_unsigned_v<T> && !std::is_same_v<T, bool>)
    {
      if(histMax < histMin)
      {
        return MakeErrorResult(-23810, fmt::format("ComputeArrayHistogramByFeature: histogram maximum {} is smaller than minimum {} for FeatureId {} in input array '{}'.", histMax, histMin, feature,
                                                   inputArray.getName()));
      }
    }
    const float32 increment = HistogramUtilities::serial::CalculateIncrement(histMin, histMax, numBins);
    histogramMinimums[feature] = histMin;
    increments[feature] = increment;
    auto range = nonstd::span<T>(ranges.get() + feature * bins * 2, bins * 2);
    HistogramUtilities::serial::FillBinRanges(range, std::make_pair(histMin, histMax), numBins, increment);
    if(std::fabs(increment) < 1.0E-10F)
    {
      counts[feature * bins] = lengths[feature];
    }
  }

  // Pass 2 routes every accepted cell directly to its feature/bin and, when
  // requested, appends a compact modal record to the external sorter.
  for(usize offset = 0; offset < tupleCount; offset += k_HistogramChunkTuples)
  {
    if(shouldCancel)
    {
      return {};
    }
    const usize count = std::min(k_HistogramChunkTuples, tupleCount - offset);
    auto readResult = readChunk(offset, count);
    if(readResult.invalid())
    {
      return readResult;
    }
    for(usize index = 0; index < count; ++index)
    {
      const int32 featureId = featureBuffer[index];
      if(featureId < 0 || (maskStore != nullptr && !static_cast<bool>(maskBuffer[index])))
      {
        continue;
      }
      const usize feature = static_cast<usize>(featureId);
      const float32 increment = increments[feature];
      const T& value = valueBuffer[index];
      if constexpr(!std::is_same_v<T, bool>)
      {
        if(externalSort != nullptr && modalBinRanges != nullptr)
        {
          encodeModalRecord<T>(nonstd::span<std::byte>(modalRecordBytes.data() + index * k_ModalRecordSize<T>, k_ModalRecordSize<T>), featureId, value, static_cast<uint64>(offset + index));
        }
      }
      if(std::fabs(increment) < 1.0E-10F)
      {
        continue;
      }
      if(!isInHistogramRange(value, histogramMinimums[feature], inputValues.UserDefinedRange ? static_cast<T>(inputValues.MaxRange) : static_cast<T>(maximums[feature] + static_cast<T>(1))))
      {
        ++overflow;
        continue;
      }
      const auto bin = calculateSafeBin(value, histogramMinimums[feature], increment);
      if(bin >= 0 && bin < numBins)
      {
        uint64& binCount = counts[feature * bins + static_cast<usize>(bin)];
        if(binCount == std::numeric_limits<uint64>::max())
        {
          return MakeErrorResult(-23809,
                                 fmt::format("ComputeArrayHistogramByFeature: histogram count for FeatureId {}, bin {}, in input array '{}' exceeds uint64.", feature, bin, inputArray.getName()));
        }
        ++binCount;
      }
      else
      {
        ++overflow;
      }
    }
    if(externalSort != nullptr)
    {
      uint64 recordCount = 0;
      for(usize index = 0; index < count; ++index)
      {
        const int32 featureId = featureBuffer[index];
        if(featureId >= 0 && (maskStore == nullptr || static_cast<bool>(maskBuffer[index])))
        {
          const usize sourceOffset = index * k_ModalRecordSize<T>;
          const usize destinationOffset = static_cast<usize>(recordCount) * k_ModalRecordSize<T>;
          if(sourceOffset != destinationOffset)
          {
            std::memcpy(modalRecordBytes.data() + destinationOffset, modalRecordBytes.data() + sourceOffset, k_ModalRecordSize<T>);
          }
          ++recordCount;
        }
      }
      if(recordCount > 0)
      {
        Result<> appendResult = externalSort->append(recordCount, nonstd::span<const std::byte>(modalRecordBytes.data(), static_cast<usize>(recordCount * k_ModalRecordSize<T>)), shouldCancel, {});
        if(appendResult.invalid())
        {
          return appendResult;
        }
      }
    }
  }

  // Finish all sort runs before scanning them; IExternalSort does not expose a
  // globally ordered read view until finish() succeeds.
  if(externalSort != nullptr)
  {
    Result<> finishResult = externalSort->finish(shouldCancel, {});
    if(finishResult.invalid() || shouldCancel)
    {
      return finishResult;
    }
  }

  std::vector<uint64> mostPopulated(numFeatures * 2, 0);
  for(usize feature = 0; feature < numFeatures; ++feature)
  {
    const auto first = counts.begin() + static_cast<ptrdiff_t>(feature * bins);
    const auto highest = std::max_element(first, first + static_cast<ptrdiff_t>(bins));
    mostPopulated[feature * 2] = static_cast<uint64>(std::distance(first, highest));
    mostPopulated[feature * 2 + 1] = *highest;
  }

  // Modal reduction is a separate phase because it needs globally grouped
  // values. The histogram counts themselves are already complete at this point.
  if constexpr(!std::is_same_v<T, bool>)
  {
    if(modalBinRanges != nullptr)
    {
      auto& typedModalBinRanges = *dynamic_cast<NeighborList<T>*>(modalBinRanges);
      typedModalBinRanges.resizeTuples({numFeatures});
      for(usize feature = 0; feature < numFeatures; ++feature)
      {
        if(lengths[feature] > 0 && std::fabs(increments[feature]) < 1.0E-10F)
        {
          // The Direct algorithm historically writes its parallel feature-range start/end
          // indices for a degenerate increment. Its sequential range is [0, numFeatures).
          typedModalBinRanges.addEntry(static_cast<int32>(feature), static_cast<T>(0));
          typedModalBinRanges.addEntry(static_cast<int32>(feature), static_cast<T>(numFeatures));
        }
      }
      if(externalSort != nullptr)
      {
        std::vector<uint64> maxCounts(numFeatures, 0);
        Result<> scanResult = scanSortedModalRecords<T>(*externalSort, shouldCancel, [&](int32 featureId, T, uint64 count) {
          if(featureId >= 0 && static_cast<usize>(featureId) < numFeatures)
          {
            maxCounts[static_cast<usize>(featureId)] = std::max(maxCounts[static_cast<usize>(featureId)], count);
          }
        });
        if(scanResult.invalid() || shouldCancel)
        {
          return scanResult;
        }
        scanResult = scanSortedModalRecords<T>(*externalSort, shouldCancel, [&](int32 featureId, T value, uint64 count) {
          const usize feature = static_cast<usize>(featureId);
          if(featureId >= 0 && feature < numFeatures && std::fabs(increments[feature]) >= 1.0E-10F && count == maxCounts[feature])
          {
            const T maximum = inputValues.UserDefinedRange ? static_cast<T>(inputValues.MaxRange) : static_cast<T>(maximums[feature] + static_cast<T>(1));
            appendModalRange(typedModalBinRanges, feature, value, ranges.get(), bins, histogramMinimums[feature], maximum, increments[feature]);
          }
        });
        if(scanResult.invalid() || shouldCancel)
        {
          return scanResult;
        }
      }
      else
      {
        for(usize feature = 0; feature < numFeatures; ++feature)
        {
          if(lengths[feature] == 0 || std::fabs(increments[feature]) < 1.0E-10F)
          {
            continue;
          }
          uint64 maxCount = 0;
          Result<> scanResult =
              scanFallbackModalGroups(inputStore, featureIdsStore, maskStore, tupleCount, static_cast<int32>(feature), shouldCancel, [&](T, uint64 count) { maxCount = std::max(maxCount, count); });
          if(scanResult.invalid() || shouldCancel)
          {
            return scanResult;
          }
          scanResult = scanFallbackModalGroups(inputStore, featureIdsStore, maskStore, tupleCount, static_cast<int32>(feature), shouldCancel, [&](T value, uint64 count) {
            if(count == maxCount)
            {
              const T maximum = inputValues.UserDefinedRange ? static_cast<T>(inputValues.MaxRange) : static_cast<T>(maximums[feature] + static_cast<T>(1));
              appendModalRange(typedModalBinRanges, feature, value, ranges.get(), bins, histogramMinimums[feature], maximum, increments[feature]);
            }
          });
          if(scanResult.invalid() || shouldCancel)
          {
            return scanResult;
          }
        }
      }
    }
  }
  // Commit feature-scale outputs in bulk to avoid one disk transaction per bin.
  auto writeResult = binRangesStore.copyFromBuffer(0, nonstd::span<const T>(ranges.get(), rangeValues));
  if(writeResult.invalid())
  {
    return writeResult;
  }
  writeResult = countsStore.copyFromBuffer(0, nonstd::span<const uint64>(counts.data(), counts.size()));
  if(writeResult.invalid())
  {
    return writeResult;
  }
  return mostPopulatedStore.copyFromBuffer(0, nonstd::span<const uint64>(mostPopulated.data(), mostPopulated.size()));
}

/** @brief Dispatches the OOC histogram implementation across input and mask types. */
struct HistogramScanlineFunctor
{
  /** @brief Selects Boolean, UInt8, or absent mask storage and runs the typed scan. */
  template <typename T>
  Result<> operator()(const IDataArray& inputArray, IDataArray& binRangesArray, const AbstractDataStore<int32>& featureIdsStore, const IDataArray* maskArray, DataArray<uint64>& countsArray,
                      DataArray<uint64>& mostPopulatedArray, INeighborList* modalBinRanges, const ComputeArrayHistogramByFeatureInputValues& inputValues, usize numFeatures,
                      std::atomic<usize>& overflow, const std::atomic_bool& shouldCancel) const
  {
    if(maskArray == nullptr)
    {
      return generateScanlineHistogram<T, uint8>(inputArray, binRangesArray, featureIdsStore, nullptr, countsArray, mostPopulatedArray, modalBinRanges, inputValues, numFeatures, overflow,
                                                 shouldCancel);
    }
    if(maskArray->getDataType() == DataType::boolean)
    {
      return generateScanlineHistogram<T, bool>(inputArray, binRangesArray, featureIdsStore, &maskArray->template getIDataStoreRefAs<AbstractDataStore<bool>>(), countsArray, mostPopulatedArray,
                                                modalBinRanges, inputValues, numFeatures, overflow, shouldCancel);
    }
    return generateScanlineHistogram<T, uint8>(inputArray, binRangesArray, featureIdsStore, &maskArray->template getIDataStoreRefAs<AbstractDataStore<uint8>>(), countsArray, mostPopulatedArray,
                                               modalBinRanges, inputValues, numFeatures, overflow, shouldCancel);
  }
};

/** @brief Dispatch wrapper that preserves the original resident implementation. */
class ComputeArrayHistogramByFeatureDirect
{
public:
  /** @brief Retains the dispatcher-owned direct callback for the duration of the call. */
  template <typename... ArgsT>
  explicit ComputeArrayHistogramByFeatureDirect(const std::function<Result<>()>& executeDirect, ArgsT&&...)
  : m_ExecuteDirect(executeDirect)
  {
  }

  /** @brief Executes the original in-memory algorithm. */
  Result<> operator()() const
  {
    return m_ExecuteDirect();
  }

private:
  const std::function<Result<>()>& m_ExecuteDirect;
};

/**
 * @brief Dispatch wrapper for the bounded, store-neutral histogram implementation.
 * References remain owned by ComputeArrayHistogramByFeature and are used synchronously.
 */
class ComputeArrayHistogramByFeatureScanline
{
public:
  /** @brief Captures the input/output stores and options needed by the typed scan. */
  ComputeArrayHistogramByFeatureScanline(const std::function<Result<>()>&, const IDataArray& inputArray, IDataArray& binRangesArray, const AbstractDataStore<int32>& featureIdsStore,
                                         const IDataArray* maskArray, DataArray<uint64>& countsArray, DataArray<uint64>& mostPopulatedArray, INeighborList* modalBinRanges,
                                         const ComputeArrayHistogramByFeatureInputValues& inputValues, usize numFeatures, std::atomic<usize>& overflow, const std::atomic_bool& shouldCancel)
  : m_InputArray(inputArray)
  , m_BinRangesArray(binRangesArray)
  , m_FeatureIdsStore(featureIdsStore)
  , m_MaskArray(maskArray)
  , m_CountsArray(countsArray)
  , m_MostPopulatedArray(mostPopulatedArray)
  , m_ModalBinRanges(modalBinRanges)
  , m_InputValues(inputValues)
  , m_NumFeatures(numFeatures)
  , m_Overflow(overflow)
  , m_ShouldCancel(shouldCancel)
  {
  }

  /** @brief Runtime-dispatches the bounded implementation on the input value type. */
  Result<> operator()() const
  {
    return ExecuteDataFunction(HistogramScanlineFunctor{}, m_InputArray.getDataType(), m_InputArray, m_BinRangesArray, m_FeatureIdsStore, m_MaskArray, m_CountsArray, m_MostPopulatedArray,
                               m_ModalBinRanges, m_InputValues, m_NumFeatures, m_Overflow, m_ShouldCancel);
  }

private:
  const IDataArray& m_InputArray;
  IDataArray& m_BinRangesArray;
  const AbstractDataStore<int32>& m_FeatureIdsStore;
  const IDataArray* m_MaskArray = nullptr;
  DataArray<uint64>& m_CountsArray;
  DataArray<uint64>& m_MostPopulatedArray;
  INeighborList* m_ModalBinRanges = nullptr;
  const ComputeArrayHistogramByFeatureInputValues& m_InputValues;
  usize m_NumFeatures = 0;
  std::atomic<usize>& m_Overflow;
  const std::atomic_bool& m_ShouldCancel;
};
} // namespace

// -----------------------------------------------------------------------------
ComputeArrayHistogramByFeature::ComputeArrayHistogramByFeature(DataStructure& dataStructure, const IFilter::MessageHandler& msgHandler, const std::atomic_bool& shouldCancel,
                                                               ComputeArrayHistogramByFeatureInputValues* inputValues)
: m_DataStructure(dataStructure)
, m_InputValues(inputValues)
, m_ShouldCancel(shouldCancel)
, m_MessageHandler(msgHandler)
{
}

// -----------------------------------------------------------------------------
ComputeArrayHistogramByFeature::~ComputeArrayHistogramByFeature() noexcept = default;

// -----------------------------------------------------------------------------
Result<> ComputeArrayHistogramByFeature::operator()()
{
  const int32 numBins = m_InputValues->NumberOfBins;
  const std::vector<DataPath> selectedArrayPaths = m_InputValues->SelectedArrayPaths;
  std::atomic<usize> overflow = 0;

  const auto& featureIdsArray = m_DataStructure.getDataRefAs<Int32Array>(m_InputValues->FeatureIdsArrayPath);
  const auto& featureIdsStore = featureIdsArray.getDataStoreRef();

  Result<usize> featureCountResult = findFeatureCount(featureIdsStore, m_ShouldCancel);
  if(featureCountResult.invalid())
  {
    return ConvertResult(std::move(featureCountResult));
  }
  if(m_ShouldCancel)
  {
    return {};
  }
  const usize numFeatures = featureCountResult.value();

  MessageHelper messageHelper(m_MessageHandler);

  for(int32 i = 0; i < selectedArrayPaths.size(); i++)
  {
    if(m_ShouldCancel)
    {
      return {};
    }

    const auto* inputData = m_DataStructure.getDataAs<IDataArray>(selectedArrayPaths[i]);
    const IDataArray* maskArray = m_InputValues->UseMask ? m_DataStructure.getDataAs<IDataArray>(m_InputValues->MaskArrayPath) : nullptr;
    auto* binRanges = m_DataStructure.getDataAs<IDataArray>(m_InputValues->CreatedBinRangeDataPaths.at(i));
    auto* countsArray = m_DataStructure.getDataAs<DataArray<uint64>>(m_InputValues->CreatedHistogramCountsDataPaths.at(i));
    auto* mostPopulatedArray = m_DataStructure.getDataAs<DataArray<uint64>>(m_InputValues->CreatedBinMostPopulatedDataPaths.at(i));
    auto& counts = countsArray->getDataStoreRef();
    auto& mostPopulated = mostPopulatedArray->getDataStoreRef();
    INeighborList* modalBinRanges = nullptr;
    if(m_InputValues->CreatedBinModalRangesDataPaths.has_value())
    {
      modalBinRanges = m_DataStructure.getDataAs<INeighborList>(m_InputValues->CreatedBinModalRangesDataPaths->at(i));
    }

    const std::function<Result<>()> executeDirect = [&]() -> Result<> {
      binRanges->resizeTuples({numFeatures});
      counts.resizeTuples({numFeatures});
      mostPopulated.resizeTuples({numFeatures});

      std::unique_ptr<MaskCompareUtilities::MaskCompare> mask = nullptr;
      if(m_InputValues->UseMask)
      {
        mask = MaskCompareUtilities::InstantiateMaskCompare(m_DataStructure, m_InputValues->MaskArrayPath);
      }

      ParallelDataAlgorithm dataAlg;
      dataAlg.setRange(0, numFeatures);
      const bool histFullRange = !m_InputValues->UserDefinedRange;
      ProgressMessageHelper progressMessageHelper = messageHelper.createProgressMessageHelper();
      progressMessageHelper.setMaxProgresss(numFeatures);

      if(m_InputValues->CreatedBinModalRangesDataPaths.has_value())
      {
        modalBinRanges->resizeTuples({numFeatures});
        ExecuteParallelFunctor<InstantiateHistogramByFeatureImplFunctor, NoBooleanType>(InstantiateHistogramByFeatureImplFunctor{}, inputData->getDataType(), dataAlg, modalBinRanges, inputData,
                                                                                        binRanges, featureIdsStore, m_InputValues->MinRange, m_InputValues->MaxRange, histFullRange, m_ShouldCancel,
                                                                                        numBins, counts, mostPopulated, mask, overflow, progressMessageHelper);
      }
      else
      {
        ExecuteParallelFunctor(InstantiateHistogramByFeatureImplFunctor{}, inputData->getDataType(), dataAlg, inputData, binRanges, featureIdsStore, m_InputValues->MinRange, m_InputValues->MaxRange,
                               histFullRange, m_ShouldCancel, numBins, counts, mostPopulated, mask, overflow, progressMessageHelper);
      }
      return {};
    };

    Result<> executeResult;
    if(maskArray != nullptr && modalBinRanges != nullptr)
    {
      executeResult = DispatchAlgorithm<ComputeArrayHistogramByFeatureDirect, ComputeArrayHistogramByFeatureScanline>(
          AlgorithmArrayTargets{inputData, &featureIdsArray, maskArray, binRanges, countsArray, mostPopulatedArray, modalBinRanges}, executeDirect, *inputData, *binRanges, featureIdsStore, maskArray,
          *countsArray, *mostPopulatedArray, modalBinRanges, *m_InputValues, numFeatures, overflow, m_ShouldCancel);
    }
    else if(maskArray != nullptr)
    {
      executeResult = DispatchAlgorithm<ComputeArrayHistogramByFeatureDirect, ComputeArrayHistogramByFeatureScanline>(
          AlgorithmArrayTargets{inputData, &featureIdsArray, maskArray, binRanges, countsArray, mostPopulatedArray}, executeDirect, *inputData, *binRanges, featureIdsStore, maskArray, *countsArray,
          *mostPopulatedArray, nullptr, *m_InputValues, numFeatures, overflow, m_ShouldCancel);
    }
    else if(modalBinRanges != nullptr)
    {
      executeResult = DispatchAlgorithm<ComputeArrayHistogramByFeatureDirect, ComputeArrayHistogramByFeatureScanline>(
          AlgorithmArrayTargets{inputData, &featureIdsArray, binRanges, countsArray, mostPopulatedArray, modalBinRanges}, executeDirect, *inputData, *binRanges, featureIdsStore, nullptr, *countsArray,
          *mostPopulatedArray, modalBinRanges, *m_InputValues, numFeatures, overflow, m_ShouldCancel);
    }
    else
    {
      executeResult = DispatchAlgorithm<ComputeArrayHistogramByFeatureDirect, ComputeArrayHistogramByFeatureScanline>(
          AlgorithmArrayTargets{inputData, &featureIdsArray, binRanges, countsArray, mostPopulatedArray}, executeDirect, *inputData, *binRanges, featureIdsStore, nullptr, *countsArray,
          *mostPopulatedArray, nullptr, *m_InputValues, numFeatures, overflow, m_ShouldCancel);
    }
    if(executeResult.invalid())
    {
      return executeResult;
    }

    messageHelper.sendMessage(fmt::format("Calculated {} feature histograms!", numFeatures));

    if(overflow > 0)
    {
      messageHelper.sendMessage(fmt::format("{} values not categorized into bin for array {}", overflow.load(), inputData->getName()));
    }
  }

  return {};
}
