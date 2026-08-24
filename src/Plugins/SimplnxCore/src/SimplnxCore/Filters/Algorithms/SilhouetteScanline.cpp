#include "SilhouetteScanline.hpp"

#include "Silhouette.hpp"

#include "simplnx/DataStructure/DataArray.hpp"
#include "simplnx/Utilities/ClusteringUtilities.hpp"
#include "simplnx/Utilities/FilterUtilities.hpp"

#include <algorithm>
#include <limits>
#include <memory>
#include <unordered_map>
#include <vector>

using namespace nx::core;

namespace
{
constexpr usize k_TileTuples = 128;

/**
 * @brief Reads an optional Bool or UInt8 mask into a byte tile with a uniform representation.
 *
 * Materializing only the requested range avoids a cell-count synthetic mask and
 * lets the pairwise loop use the same branch for either supported mask type.
 */
Result<> ReadMask(DataStructure& dataStructure, const SilhouetteInputValues& inputValues, usize offset, usize count, std::vector<uint8>& buffer, bool* boolBuffer)
{
  if(!inputValues.UseMask)
  {
    std::fill_n(buffer.data(), count, 1);
    return {};
  }

  const auto& maskArray = dataStructure.getDataRefAs<IDataArray>(inputValues.MaskArrayPath);
  if(maskArray.getDataType() == DataType::boolean)
  {
    auto result = dataStructure.getDataRefAs<BoolArray>(inputValues.MaskArrayPath).getDataStoreRef().copyIntoBuffer(offset, nonstd::span<bool>(boolBuffer, count));
    if(result.invalid())
    {
      return result;
    }
    for(usize i = 0; i < count; i++)
    {
      buffer[i] = boolBuffer[i] ? 1 : 0;
    }
    return result;
  }
  if(maskArray.getDataType() == DataType::uint8)
  {
    return dataStructure.getDataRefAs<UInt8Array>(inputValues.MaskArrayPath).getDataStoreRef().copyIntoBuffer(offset, nonstd::span<uint8>(buffer.data(), count));
  }
  return MakeErrorResult(-54080, fmt::format("Mask Array DataPath does not exist or is not of the correct type (Bool | UInt8) {}", inputValues.MaskArrayPath.toString()));
}

/**
 * @brief Computes exact silhouette scores with bounded outer and inner tuple tiles.
 *
 * The scan is deliberately multi-pass: feature discovery permits sparse IDs,
 * feature counting supplies the exact mean denominators, and the tiled all-pairs
 * pass retains only one outer tile's feature accumulators. This preserves the
 * original distance and tie behavior without resident state proportional to all
 * tuples.
 */
template <typename T>
Result<> ExecuteScanline(DataStructure& dataStructure, const std::atomic_bool& shouldCancel, const SilhouetteInputValues& inputValues)
{
  const auto& inputArray = dataStructure.getDataRefAs<IDataArray>(inputValues.ClusteringArrayPath);
  const auto& inputStore = inputArray.template getIDataStoreRefAs<AbstractDataStore<T>>();
  const auto& featureStore = dataStructure.getDataRefAs<Int32Array>(inputValues.FeatureIdsArrayPath).getDataStoreRef();
  auto& outputStore = dataStructure.getDataRefAs<Float64Array>(inputValues.SilhouetteArrayPath).getDataStoreRef();
  const usize tupleCount = featureStore.getNumberOfTuples();
  const usize componentCount = inputStore.getNumberOfComponents();

  // Phase 1: map sparse positive feature IDs to dense accumulator columns.
  std::unordered_map<int32, usize> denseFeatureIds;
  std::vector<int32> featureBuffer(k_TileTuples);
  std::vector<uint8> maskBuffer(k_TileTuples, 1);
  std::unique_ptr<bool[]> boolMaskBuffer = std::make_unique<bool[]>(k_TileTuples);
  for(usize offset = 0; offset < tupleCount; offset += k_TileTuples)
  {
    if(shouldCancel)
    {
      return {};
    }
    const usize count = std::min(k_TileTuples, tupleCount - offset);
    auto result = featureStore.copyIntoBuffer(offset, nonstd::span<int32>(featureBuffer.data(), count));
    if(result.invalid())
    {
      return result;
    }
    result = ReadMask(dataStructure, inputValues, offset, count, maskBuffer, boolMaskBuffer.get());
    if(result.invalid())
    {
      return result;
    }
    for(usize i = 0; i < count; i++)
    {
      if(featureBuffer[i] < 0)
      {
        return MakeErrorResult(-54081, fmt::format("Feature ID {} at tuple {} is negative", featureBuffer[i], offset + i));
      }
      if(featureBuffer[i] > 0 && !denseFeatureIds.contains(featureBuffer[i]))
      {
        denseFeatureIds.insert({featureBuffer[i], denseFeatureIds.size() + 1});
      }
    }
  }

  const usize clusterCount = denseFeatureIds.size();
  if(clusterCount == std::numeric_limits<usize>::max() || componentCount > std::numeric_limits<usize>::max() / k_TileTuples || clusterCount + 1 > std::numeric_limits<usize>::max() / k_TileTuples)
  {
    return MakeErrorResult(-54083, "Silhouette tile buffer size overflows the platform usize limit");
  }

  // Phase 2: count enabled tuples so every feature distance is normalized exactly once.
  std::vector<float64> featureCounts(clusterCount + 1, 0.0);
  for(usize offset = 0; offset < tupleCount; offset += k_TileTuples)
  {
    if(shouldCancel)
    {
      return {};
    }
    const usize count = std::min(k_TileTuples, tupleCount - offset);
    auto result = featureStore.copyIntoBuffer(offset, nonstd::span<int32>(featureBuffer.data(), count));
    if(result.invalid())
    {
      return result;
    }
    result = ReadMask(dataStructure, inputValues, offset, count, maskBuffer, boolMaskBuffer.get());
    if(result.invalid())
    {
      return result;
    }
    for(usize i = 0; i < count; i++)
    {
      if(maskBuffer[i] != 0)
      {
        const usize cluster = featureBuffer[i] == 0 ? 0 : denseFeatureIds.at(featureBuffer[i]);
        featureCounts[cluster]++;
      }
    }
  }

  // Phase 3: compare each bounded outer tile with every bounded inner tile and flush its scores.
  std::vector<T> outerValues(k_TileTuples * componentCount);
  std::vector<T> innerValues(k_TileTuples * componentCount);
  std::vector<int32> innerFeatures(k_TileTuples);
  std::vector<uint8> innerMask(k_TileTuples, 1);
  std::vector<float64> outputBuffer(k_TileTuples, 0.0);
  std::vector<float64> clusterDistances(k_TileTuples * (clusterCount + 1), 0.0);
  for(usize outerOffset = 0; outerOffset < tupleCount; outerOffset += k_TileTuples)
  {
    if(shouldCancel)
    {
      return {};
    }
    const usize outerCount = std::min(k_TileTuples, tupleCount - outerOffset);
    auto result = inputStore.copyIntoBuffer(outerOffset * componentCount, nonstd::span<T>(outerValues.data(), outerCount * componentCount));
    if(result.invalid())
    {
      return result;
    }
    result = featureStore.copyIntoBuffer(outerOffset, nonstd::span<int32>(featureBuffer.data(), outerCount));
    if(result.invalid())
    {
      return result;
    }
    result = ReadMask(dataStructure, inputValues, outerOffset, outerCount, maskBuffer, boolMaskBuffer.get());
    if(result.invalid())
    {
      return result;
    }

    std::fill(clusterDistances.begin(), clusterDistances.end(), 0.0);
    for(usize innerOffset = 0; innerOffset < tupleCount; innerOffset += k_TileTuples)
    {
      if(shouldCancel)
      {
        return {};
      }
      const usize innerCount = std::min(k_TileTuples, tupleCount - innerOffset);
      result = inputStore.copyIntoBuffer(innerOffset * componentCount, nonstd::span<T>(innerValues.data(), innerCount * componentCount));
      if(result.invalid())
      {
        return result;
      }
      result = featureStore.copyIntoBuffer(innerOffset, nonstd::span<int32>(innerFeatures.data(), innerCount));
      if(result.invalid())
      {
        return result;
      }
      result = ReadMask(dataStructure, inputValues, innerOffset, innerCount, innerMask, boolMaskBuffer.get());
      if(result.invalid())
      {
        return result;
      }

      for(usize outerIndex = 0; outerIndex < outerCount; outerIndex++)
      {
        if(maskBuffer[outerIndex] == 0)
        {
          continue;
        }
        for(usize innerIndex = 0; innerIndex < innerCount; innerIndex++)
        {
          if(innerMask[innerIndex] == 0)
          {
            continue;
          }
          const usize cluster = innerFeatures[innerIndex] == 0 ? 0 : denseFeatureIds.at(innerFeatures[innerIndex]);
          clusterDistances[outerIndex * (clusterCount + 1) + cluster] +=
              ClusterUtilities::GetDistance(outerValues, outerIndex * componentCount, innerValues, innerIndex * componentCount, componentCount, inputValues.DistanceMetric);
        }
      }
    }

    for(usize outerIndex = 0; outerIndex < outerCount; outerIndex++)
    {
      if(maskBuffer[outerIndex] == 0)
      {
        outputBuffer[outerIndex] = 0.0;
        continue;
      }
      const usize ownCluster = featureBuffer[outerIndex] == 0 ? 0 : denseFeatureIds.at(featureBuffer[outerIndex]);
      float64 inClusterDistance = clusterDistances[outerIndex * (clusterCount + 1) + ownCluster];
      if(ownCluster > 0)
      {
        inClusterDistance /= featureCounts[ownCluster];
      }
      float64 outClusterDistance = 0.0;
      float64 minimumDistance = std::numeric_limits<float64>::max();
      for(usize cluster = 1; cluster <= clusterCount; cluster++)
      {
        if(cluster == ownCluster || featureCounts[cluster] == 0.0)
        {
          continue;
        }
        const float64 distance = clusterDistances[outerIndex * (clusterCount + 1) + cluster] / featureCounts[cluster];
        if(distance < minimumDistance)
        {
          minimumDistance = distance;
          outClusterDistance = distance;
        }
      }
      outputBuffer[outerIndex] = (outClusterDistance - inClusterDistance) / std::max(outClusterDistance, inClusterDistance);
    }
    result = outputStore.copyFromBuffer(outerOffset, nonstd::span<const float64>(outputBuffer.data(), outerCount));
    if(result.invalid())
    {
      return result;
    }
  }
  return {};
}

/**
 * @brief Adapts runtime numeric dispatch to the typed ExecuteScanline function.
 *
 * The runner stores references because RunTemplateClass requires a class-template
 * callable; it does not extend the lifetime of any borrowed input.
 */
template <typename T>
class SilhouetteScanlineRunner
{
public:
  /** @brief Captures the borrowed execution context and result slot for typed dispatch. */
  SilhouetteScanlineRunner(DataStructure& dataStructure, const std::atomic_bool& shouldCancel, const SilhouetteInputValues* inputValues, Result<>& result)
  : m_DataStructure(dataStructure)
  , m_ShouldCancel(shouldCancel)
  , m_InputValues(inputValues)
  , m_Result(result)
  {
  }

  /** @brief Runs the selected numeric specialization and stores its checked result. */
  void operator()()
  {
    m_Result = ExecuteScanline<T>(m_DataStructure, m_ShouldCancel, *m_InputValues);
  }

private:
  DataStructure& m_DataStructure;
  const std::atomic_bool& m_ShouldCancel;
  const SilhouetteInputValues* m_InputValues = nullptr;
  Result<>& m_Result;
};
} // namespace

SilhouetteScanline::SilhouetteScanline(DataStructure& dataStructure, const IFilter::MessageHandler&, const std::atomic_bool& shouldCancel, const SilhouetteInputValues* inputValues)
: m_DataStructure(dataStructure)
, m_ShouldCancel(shouldCancel)
, m_InputValues(inputValues)
{
}

SilhouetteScanline::~SilhouetteScanline() noexcept = default;

Result<> SilhouetteScanline::operator()()
{
  const auto& inputArray = m_DataStructure.getDataRefAs<IDataArray>(m_InputValues->ClusteringArrayPath);
  Result<> result;
  RunTemplateClass<SilhouetteScanlineRunner, types::NoBooleanType>(inputArray.getDataType(), m_DataStructure, m_ShouldCancel, m_InputValues, result);
  return result;
}
