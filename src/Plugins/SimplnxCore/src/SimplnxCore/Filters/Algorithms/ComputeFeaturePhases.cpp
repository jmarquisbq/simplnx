#include "ComputeFeaturePhases.hpp"

#include "simplnx/DataStructure/AttributeMatrix.hpp"
#include "simplnx/DataStructure/DataArray.hpp"
#include "simplnx/Utilities/DataArrayUtilities.hpp"

#include <nonstd/span.hpp>

#include <algorithm>
#include <memory>
#include <set>
#include <vector>

using namespace nx::core;

namespace
{
/// Number of tuples to read per bulk I/O call. 64K tuples (256 KB of int32 per
/// buffer) minimizes copyIntoBuffer() round-trips on large datasets while keeping
/// the per-chunk working set bounded regardless of total cell count.
constexpr usize k_ChunkTuples = 65536;
} // namespace

// -----------------------------------------------------------------------------
ComputeFeaturePhases::ComputeFeaturePhases(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel, ComputeFeaturePhasesInputValues* inputValues)
: m_DataStructure(dataStructure)
, m_InputValues(inputValues)
, m_ShouldCancel(shouldCancel)
, m_MessageHandler(mesgHandler)
{
}

// -----------------------------------------------------------------------------
ComputeFeaturePhases::~ComputeFeaturePhases() noexcept = default;

// -----------------------------------------------------------------------------
Result<> ComputeFeaturePhases::operator()()
{
  auto featurePhasesArrayPath = m_InputValues->CellFeaturesAttributeMatrixPath.createChildPath(m_InputValues->FeaturePhasesArrayName);

  const auto& cellPhases = m_DataStructure.getDataAs<Int32Array>(m_InputValues->CellPhasesArrayPath)->getDataStoreRef();
  const auto& featureIdsArray = m_DataStructure.getDataRefAs<Int32Array>(m_InputValues->FeatureIdsPath);
  const auto& featureIds = featureIdsArray.getDataStoreRef();
  auto& featurePhases = m_DataStructure.getDataAs<Int32Array>(featurePhasesArrayPath)->getDataStoreRef();

  // Validate the featurePhases array is the proper size. This also guarantees
  // every FeatureIds value indexes within [0, numFeatures), so the feature-level
  // vectors below can be indexed without per-element bounds checks.
  auto validateNumFeatResult = ValidateFeatureIdsToFeatureAttributeMatrixIndexing(m_DataStructure, m_InputValues->CellFeaturesAttributeMatrixPath, featureIdsArray, false, m_MessageHandler);
  if(validateNumFeatResult.invalid())
  {
    return validateNumFeatResult;
  }

  const usize totalPoints = featureIds.getNumberOfTuples();
  const usize numFeatures = featurePhases.getNumberOfTuples();
  // Feature-level accumulators. These scale with the feature count (small —
  // typically thousands of entries), not the cell count, so allocating them
  // in memory is acceptable even for out-of-core datasets. Plain vectors give
  // O(1) indexed access in the hot per-cell loop. uint8 (not std::vector<bool>)
  // keeps element access a simple byte load/store.
  std::vector<int32> featurePhaseValues(numFeatures, 0);
  std::vector<uint8> featureSeen(numFeatures, 0);
  std::set<int32> warnFeatures;

  // Read FeatureIds and CellPhases chunk-sequentially via copyIntoBuffer().
  // Bulk reads amortize the per-call dispatch (and, for out-of-core stores,
  // the chunk load) over 64K elements instead of paying it per cell. Both
  // arrays are 1-component, so element index == tuple index.
  auto featureIdBuf = std::make_unique<int32[]>(k_ChunkTuples);
  auto cellPhaseBuf = std::make_unique<int32[]>(k_ChunkTuples);
  for(usize offset = 0; offset < totalPoints; offset += k_ChunkTuples)
  {
    if(m_ShouldCancel)
    {
      return {};
    }

    const usize count = std::min(k_ChunkTuples, totalPoints - offset);
    Result<> readFidResult = featureIds.copyIntoBuffer(offset, nonstd::span<int32>(featureIdBuf.get(), count));
    if(readFidResult.invalid())
    {
      return readFidResult;
    }
    Result<> readPhaseResult = cellPhases.copyIntoBuffer(offset, nonstd::span<int32>(cellPhaseBuf.get(), count));
    if(readPhaseResult.invalid())
    {
      return readPhaseResult;
    }

    for(usize i = 0; i < count; i++)
    {
      const int32 gnum = featureIdBuf[i];
      const int32 phase = cellPhaseBuf[i];

      // A feature warns when any of its cells carries a phase different from the
      // first phase seen for that feature. Comparing each cell against the
      // PREVIOUS phase stored for the feature detects exactly the same feature
      // set: over the feature's cell sequence, "some element differs from the
      // first" holds if and only if "some adjacent pair differs". Storing the
      // phase unconditionally afterwards means the feature keeps the LAST phase
      // encountered, which is also the value written to the output below.
      if(featureSeen[gnum] != 0 && featurePhaseValues[gnum] != phase)
      {
        warnFeatures.insert(gnum);
      }
      featurePhaseValues[gnum] = phase;
      featureSeen[gnum] = 1;
    }
  }

  // Single bulk write of the feature-level result. One copyFromBuffer() call
  // replaces numFeatures individual element writes, which matters for
  // out-of-core output stores where each scattered write would dirty a chunk.
  Result<> writeResult = featurePhases.copyFromBuffer(0, nonstd::span<const int32>(featurePhaseValues.data(), numFeatures));
  if(writeResult.invalid())
  {
    return writeResult;
  }

  Result<> result;
  if(!warnFeatures.empty())
  {
    std::string warnStr = "Elements from some features did not all have the same phase ID. The last phase ID copied into each feature will be used. Effected Phase Features: ";
    usize position = 0;
    for(auto value : warnFeatures)
    {
      warnStr.append(std::to_string(value));
      if(++position != warnFeatures.size())
      {
        warnStr.append(", ");
      }
    }
    result.warnings().push_back(Warning{-500, std::move(warnStr)});
  }

  return result;
}
