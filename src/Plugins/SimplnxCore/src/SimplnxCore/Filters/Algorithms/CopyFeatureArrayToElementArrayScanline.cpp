#include "CopyFeatureArrayToElementArrayScanline.hpp"

#include "CopyFeatureArrayToElementArray.hpp"

#include "simplnx/DataStructure/DataArray.hpp"
#include "simplnx/Utilities/DataArrayUtilities.hpp"
#include "simplnx/Utilities/FilterUtilities.hpp"
#include <nonstd/span.hpp>

using namespace nx::core;

namespace
{
// Number of cell tuples processed per bulk-I/O chunk. Chosen to keep the temporary
// featureIds/output buffers small (a few hundred KB) regardless of the total cell count,
// so no allocation ever scales with the size of the volume.
constexpr usize k_ChunkTuples = 65536;

/**
 * @brief Broadcasts one feature-level array through sequential Feature-ID reads and cell-output writes.
 *
 * The feature-scale source is cached once, while Feature IDs and output values
 * use fixed cell chunks. This avoids both random OOC gathers and a cell-count
 * output staging array.
 */
struct CopyFeatureToElementScanlineFunctor
{
  /** @brief Executes the typed feature-cache, chunked gather, and checked output-transfer loop. */
  template <typename T>
  Result<> operator()(const IDataArray* selectedFeatureArray, const Int32AbstractDataStore& featureIdsStore, IDataArray* createdArray, const std::atomic_bool& shouldCancel)
  {
    const auto& selectedFeatureStore = selectedFeatureArray->template getIDataStoreRefAs<AbstractDataStore<T>>();
    auto& createdStore = createdArray->template getIDataStoreRefAs<AbstractDataStore<T>>();

    const usize numComps = selectedFeatureStore.getNumberOfComponents();
    const usize numFeatures = selectedFeatureStore.getNumberOfTuples();
    const usize numCells = featureIdsStore.getNumberOfTuples();

    // Cache the entire feature-level source array into a local buffer with a single bulk read.
    // This is feature-level (numFeatures * numComps), not cell-level, so it is bounded by the
    // number of features and safe for OOC. std::make_unique<T[]> avoids the std::vector<bool>
    // specialization when T == bool.
    auto featureCache = std::make_unique<T[]>(numFeatures * numComps);
    auto featureReadResult = selectedFeatureStore.copyIntoBuffer(0, nonstd::span<T>(featureCache.get(), numFeatures * numComps));
    if(featureReadResult.invalid())
    {
      return featureReadResult;
    }

    // Bounded scratch buffers reused for every chunk.
    auto featureIdsBuffer = std::make_unique<int32[]>(k_ChunkTuples);
    auto outputBuffer = std::make_unique<T[]>(k_ChunkTuples * numComps);

    for(usize chunkStart = 0; chunkStart < numCells; chunkStart += k_ChunkTuples)
    {
      if(shouldCancel)
      {
        return {};
      }

      const usize chunkTupleCount = std::min(k_ChunkTuples, numCells - chunkStart);
      // Sequentially read this chunk of FeatureIds.
      auto featureIdsReadResult = featureIdsStore.copyIntoBuffer(chunkStart, nonstd::span<int32>(featureIdsBuffer.get(), chunkTupleCount));
      if(featureIdsReadResult.invalid())
      {
        return featureIdsReadResult;
      }

      // Gather each cell's feature value from the cached feature array.
      for(usize cellIdx = 0; cellIdx < chunkTupleCount; cellIdx++)
      {
        if((cellIdx & 0xFFFULL) == 0 && shouldCancel)
        {
          return {};
        }
        const usize srcOffset = numComps * static_cast<usize>(featureIdsBuffer[cellIdx]);
        const usize dstOffset = numComps * cellIdx;
        for(usize compIdx = 0; compIdx < numComps; compIdx++)
        {
          outputBuffer[dstOffset + compIdx] = featureCache[srcOffset + compIdx];
        }
      }

      if(shouldCancel)
      {
        return {};
      }

      // Sequentially write this chunk of the created cell array.
      auto outputWriteResult = createdStore.copyFromBuffer(chunkStart * numComps, nonstd::span<const T>(outputBuffer.get(), chunkTupleCount * numComps));
      if(outputWriteResult.invalid())
      {
        return outputWriteResult;
      }
    }

    return {};
  }
};
} // namespace

// -----------------------------------------------------------------------------
CopyFeatureArrayToElementArrayScanline::CopyFeatureArrayToElementArrayScanline(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel,
                                                                               const CopyFeatureArrayToElementArrayInputValues* inputValues)
: m_DataStructure(dataStructure)
, m_InputValues(inputValues)
, m_ShouldCancel(shouldCancel)
, m_MessageHandler(mesgHandler)
{
}

// -----------------------------------------------------------------------------
CopyFeatureArrayToElementArrayScanline::~CopyFeatureArrayToElementArrayScanline() noexcept = default;

// -----------------------------------------------------------------------------
Result<> CopyFeatureArrayToElementArrayScanline::operator()()
{
  if(m_InputValues->SelectedFeatureArrayPaths.empty())
  {
    return {};
  }

  const auto& featureIds = m_DataStructure.getDataRefAs<Int32Array>(m_InputValues->FeatureIdsPath);

  auto validateResult = ValidateFeatureIdsToFeatureAttributeMatrixIndexing(m_DataStructure, m_InputValues->SelectedFeatureArrayPaths[0], featureIds, false, m_MessageHandler, &m_ShouldCancel);
  if(validateResult.invalid())
  {
    return validateResult;
  }

  for(const auto& selectedFeatureArrayPath : m_InputValues->SelectedFeatureArrayPaths)
  {
    if(m_ShouldCancel)
    {
      return {};
    }

    DataPath createdArrayPath = m_InputValues->FeatureIdsPath.replaceName(selectedFeatureArrayPath.getTargetName() + m_InputValues->CreatedArraySuffix);
    const auto* selectedFeatureArray = m_DataStructure.getDataAs<IDataArray>(selectedFeatureArrayPath);

    m_MessageHandler(IFilter::ProgressMessage{IFilter::ProgressMessage::Type::Info, fmt::format("Copying data into target array '{}'...", createdArrayPath.toString())});

    auto result = ExecuteDataFunction(CopyFeatureToElementScanlineFunctor{}, selectedFeatureArray->getDataType(), selectedFeatureArray, featureIds.getDataStoreRef(),
                                      m_DataStructure.getDataAs<IDataArray>(createdArrayPath), m_ShouldCancel);
    if(result.invalid())
    {
      return result;
    }
  }

  return {};
}
