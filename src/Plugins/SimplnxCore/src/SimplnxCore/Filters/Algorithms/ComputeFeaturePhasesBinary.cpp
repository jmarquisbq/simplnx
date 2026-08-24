#include "ComputeFeaturePhasesBinary.hpp"

#include "simplnx/Common/TypesUtility.hpp"
#include "simplnx/DataStructure/DataArray.hpp"
#include "simplnx/Utilities/FilterUtilities.hpp"

#include <nonstd/span.hpp>

#include <algorithm>
#include <memory>
#include <vector>

using namespace nx::core;
namespace
{
/// Limits cell data transfers to five megabytes while keeping the 200^3 benchmark aligned to complete Z-slices.
constexpr usize k_TargetChunkTuples = 1'000'000;

/**
 * @brief Streams feature ids and mask values through bounded buffers and updates a feature-indexed cache.
 *
 * Cell order is deliberately serial because repeated feature ids use last-cell-wins semantics. The output cache
 * grows only to the largest referenced feature id and preserves pre-existing values for feature ids not present in
 * the input. This avoids per-cell DataStore access without allocating cell-sized scratch arrays.
 */
struct ComputeFeaturePhasesBinaryFunctor
{
  template <typename MaskType>
  Result<> operator()(const IDataArray& featureIdsArray, const IDataArray& maskArray, IDataArray& featurePhasesArray, const DataPath& featureIdsPath, const DataPath& featurePhasesPath,
                      const std::atomic_bool& shouldCancel) const
  {
    const auto& featureIdsStore = featureIdsArray.getIDataStoreRefAs<AbstractDataStore<int32>>();
    const auto& maskStore = maskArray.template getIDataStoreRefAs<AbstractDataStore<MaskType>>();
    auto& featurePhasesStore = featurePhasesArray.getIDataStoreRefAs<AbstractDataStore<int32>>();

    const usize numCells = featureIdsStore.getNumberOfTuples();
    if(numCells == 0)
    {
      return {};
    }

    const usize chunkTuples = std::min(k_TargetChunkTuples, numCells);
    auto featureIdsBuffer = std::make_unique<int32[]>(chunkTuples);
    auto maskBuffer = std::make_unique<MaskType[]>(chunkTuples);
    std::vector<int32> featurePhasesCache;

    const usize outputTupleCount = featurePhasesStore.getNumberOfTuples();
    for(usize cellOffset = 0; cellOffset < numCells; cellOffset += chunkTuples)
    {
      if(shouldCancel)
      {
        return {};
      }

      const usize cellCount = std::min(chunkTuples, numCells - cellOffset);
      Result<> result = featureIdsStore.copyIntoBuffer(cellOffset, nonstd::span<int32>(featureIdsBuffer.get(), cellCount));
      if(result.invalid())
      {
        return result;
      }
      result = maskStore.copyIntoBuffer(cellOffset, nonstd::span<MaskType>(maskBuffer.get(), cellCount));
      if(result.invalid())
      {
        return result;
      }

      usize largestFeatureId = 0;
      for(usize index = 0; index < cellCount; index++)
      {
        const int32 featureId = featureIdsBuffer[index];
        if(featureId < 0 || static_cast<usize>(featureId) >= outputTupleCount)
        {
          return MakeErrorResult(-53801, fmt::format("Feature id {} at tuple {} in array '{}' is outside the valid output range [0, {}) for array '{}'.", featureId, cellOffset + index,
                                                     featureIdsPath.toString(), outputTupleCount, featurePhasesPath.toString()));
        }
        largestFeatureId = std::max(largestFeatureId, static_cast<usize>(featureId));
      }

      const usize requiredCacheSize = largestFeatureId + 1;
      if(requiredCacheSize > featurePhasesCache.size())
      {
        const usize previousCacheSize = featurePhasesCache.size();
        featurePhasesCache.resize(requiredCacheSize);
        result = featurePhasesStore.copyIntoBuffer(previousCacheSize, nonstd::span<int32>(featurePhasesCache.data() + previousCacheSize, requiredCacheSize - previousCacheSize));
        if(result.invalid())
        {
          return result;
        }
      }

      for(usize index = 0; index < cellCount; index++)
      {
        const usize featureId = static_cast<usize>(featureIdsBuffer[index]);
        featurePhasesCache[featureId] = maskBuffer[index] != static_cast<MaskType>(0);
      }
    }

    if(shouldCancel)
    {
      return {};
    }

    return featurePhasesStore.copyFromBuffer(0, nonstd::span<const int32>(featurePhasesCache.data(), featurePhasesCache.size()));
  }
};
} // namespace

// -----------------------------------------------------------------------------
ComputeFeaturePhasesBinary::ComputeFeaturePhasesBinary(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel,
                                                       ComputeFeaturePhasesBinaryInputValues* inputValues)
: m_DataStructure(dataStructure)
, m_InputValues(inputValues)
, m_ShouldCancel(shouldCancel)
, m_MessageHandler(mesgHandler)
{
}

// -----------------------------------------------------------------------------
ComputeFeaturePhasesBinary::~ComputeFeaturePhasesBinary() noexcept = default;

// -----------------------------------------------------------------------------
Result<> ComputeFeaturePhasesBinary::operator()()
{
  const auto& featureIdsArray = m_DataStructure.getDataRefAs<IDataArray>(m_InputValues->FeatureIdsArrayPath);
  const auto& maskArray = m_DataStructure.getDataRefAs<IDataArray>(m_InputValues->MaskArrayPath);
  const DataPath featurePhasesPath = m_InputValues->CellDataAttributeMatrixPath.createChildPath(m_InputValues->FeaturePhasesArrayName);
  auto& featurePhasesArray = m_DataStructure.getDataRefAs<IDataArray>(featurePhasesPath);

  if(maskArray.getDataType() != DataType::boolean && maskArray.getDataType() != DataType::uint8)
  {
    return MakeErrorResult(-53800, fmt::format("Mask array '{}' has data type '{}'. The mask must have a boolean or uint8 data type.", m_InputValues->MaskArrayPath.toString(),
                                               DataTypeToString(maskArray.getDataType())));
  }

  if(m_ShouldCancel)
  {
    return {};
  }

  return ExecuteDataFunction(ComputeFeaturePhasesBinaryFunctor{}, maskArray.getDataType(), featureIdsArray, maskArray, featurePhasesArray, m_InputValues->FeatureIdsArrayPath, featurePhasesPath,
                             m_ShouldCancel);
}
