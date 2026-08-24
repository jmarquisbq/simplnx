#include "RequireMinimumSizeFeatures.hpp"

#include "FillBadVoxels.hpp"

#include "simplnx/DataStructure/DataArray.hpp"
#include "simplnx/DataStructure/Geometry/ImageGeom.hpp"
#include "simplnx/Utilities/DataGroupUtilities.hpp"
#include "simplnx/Utilities/MessageHelper.hpp"

#include <nonstd/span.hpp>

#include <algorithm>
#include <memory>
#include <optional>

using namespace nx::core;

namespace
{
constexpr usize k_ChunkTuples = 65536;
} // namespace

// -----------------------------------------------------------------------------
RequireMinimumSizeFeatures::RequireMinimumSizeFeatures(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel,
                                                       RequireMinimumSizeFeaturesInputValues* inputValues)
: m_DataStructure(dataStructure)
, m_InputValues(inputValues)
, m_ShouldCancel(shouldCancel)
, m_MessageHandler(mesgHandler)
{
}

// -----------------------------------------------------------------------------
RequireMinimumSizeFeatures::~RequireMinimumSizeFeatures() noexcept = default;

// -----------------------------------------------------------------------------
Result<> RequireMinimumSizeFeatures::operator()()
{

  // Input Cell Level Data
  auto& featureIdsStoreRef = m_DataStructure.getDataAs<Int32Array>(m_InputValues->FeatureIdsPath)->getDataStoreRef();

  // Input Feature Level Data
  auto& featureNumCellsStoreRef = m_DataStructure.getDataRefAs<Int32Array>(m_InputValues->FeatureNumCellsPath).getDataStoreRef();

  // Optionally allow applying to a single phase
  auto* featurePhases = m_InputValues->ApplySinglePhase ? m_DataStructure.getDataAs<Int32Array>(m_InputValues->FeaturePhasesPath)->getDataStore() : nullptr;
  if(m_InputValues->ApplySinglePhase && featurePhases != nullptr)
  {
    usize numFeatures = featurePhases->getNumberOfTuples();
    bool unavailablePhase = true;
    for(usize i = 0; i < numFeatures; i++)
    {
      if(featurePhases->getValue(i) == m_InputValues->PhaseNumber)
      {
        unavailablePhase = false;
        break;
      }
    }

    if(unavailablePhase)
    {
      std::string ss = fmt::format("The phase number {} is not available in the supplied Feature phases array with path {}", m_InputValues->PhaseNumber, m_InputValues->FeaturePhasesPath.toString());
      return MakeErrorResult(-5555, ss);
    }
  }

  Error errorReturn = {0, ""};
  std::vector<bool> activeObjects =
      removeSmallFeatures(featureIdsStoreRef, featureNumCellsStoreRef, featurePhases, m_InputValues->PhaseNumber, m_InputValues->ApplySinglePhase, m_InputValues->MinAllowedFeaturesSize, errorReturn);

  if(errorReturn.code < 0)
  {
    return {nonstd::make_unexpected(std::vector<Error>{errorReturn})};
  }
  if(m_ShouldCancel)
  {
    return {};
  }

  auto& imageGeom = m_DataStructure.getDataRefAs<ImageGeom>(m_InputValues->InputImageGeometryPath);
  Result<> assignResult = assignBadVoxels(imageGeom.getDimensions());
  if(assignResult.invalid())
  {
    return assignResult;
  }

  if(m_ShouldCancel)
  {
    return {};
  }
  DataPath cellFeatureGroupPath = m_InputValues->FeatureNumCellsPath.getParent();
  usize currentFeatureCount = featureNumCellsStoreRef.getNumberOfTuples();

  int32 count = 0;
  for(const auto& value : activeObjects)
  {
    if(value)
    {
      count++;
    }
  }
  std::string message = fmt::format("Feature Count Changed: Previous: {} New: {}", currentFeatureCount, count);
  m_MessageHandler(nx::core::IFilter::Message{nx::core::IFilter::Message::Type::Info, message});

  // removeSmallFeatures already renumbered the surviving FeatureIds to their compacted ids (and
  // assignBadVoxels propagated those compacted ids into the filled voxels), so tell
  // RemoveInactiveObjects to skip its full-volume FeatureIds renumber pass and only compact/resize
  // the feature-level arrays.
  nx::core::RemoveInactiveObjects(m_DataStructure, cellFeatureGroupPath, activeObjects, featureIdsStoreRef, currentFeatureCount, m_MessageHandler, m_ShouldCancel, /*cellFeatureIdsRenumbered=*/true);

  return {};
}

/**
 * @brief Iteratively fills voxels belonging to removed features (featureId < 0)
 * by majority-voting among their 6 face-neighbors.
 *
 * The shared OOC implementation processes one cell array at a time with rolling three-slice
 * FeatureIds and target-array windows. It recomputes neighbor choices while streaming each array,
 * then updates FeatureIds last so all arrays observe the same iteration snapshot. Resident scratch
 * is O(X*Y) and no allocation scales with the cell count.
 */
Result<> RequireMinimumSizeFeatures::assignBadVoxels(SizeVec3 dimensions)
{
  return FillBadVoxels(m_DataStructure, m_InputValues->FeatureIdsPath, dimensions, {}, std::nullopt, m_MessageHandler, m_ShouldCancel);
}

// -----------------------------------------------------------------------------
std::vector<bool> RequireMinimumSizeFeatures::removeSmallFeatures(Int32AbstractDataStore& featureIdsStoreRef, const Int32AbstractDataStore& featureNumCellsStoreRef,
                                                                  const Int32AbstractDataStore* featurePhases, int32 phaseNumber, bool applyToSinglePhase, int64 minAllowedFeatureSize,
                                                                  Error& errorReturn)
{
  MessageHelper messageHelper(m_MessageHandler);
  messageHelper.sendMessage(fmt::format("Removing small features...."));

  usize totalPoints = featureIdsStoreRef.getNumberOfTuples();

  bool good = false;

  usize totalFeatures = featureNumCellsStoreRef.getNumberOfTuples();

  std::vector<bool> activeObjects(totalFeatures, true);

  for(usize i = 1; i < totalFeatures; i++)
  {
    if(m_ShouldCancel)
    {
      return {};
    }
    if(!applyToSinglePhase)
    {
      if(featureNumCellsStoreRef.getValue(i) >= minAllowedFeatureSize)
      {
        good = true;
      }
      else
      {
        activeObjects[i] = false;
      }
    }
    else
    {
      if(featureNumCellsStoreRef.getValue(i) >= minAllowedFeatureSize || featurePhases->getValue(i) != phaseNumber)
      {
        good = true;
      }
      else
      {
        activeObjects[i] = false;
      }
    }
  }
  if(!good)
  {
    errorReturn = Error{-1, "The minimum size is larger than the largest Feature.  All Features would be removed"};
    return activeObjects;
  }

  // Compaction mapping for the features that survive. Computed with the shared helper so it matches
  // exactly what RemoveInactiveObjects uses to compact the feature arrays.
  const FeatureRenumbering renumbering = ComputeFeatureRenumbering(activeObjects);
  const std::vector<size_t>& newNames = renumbering.newNames;

  // Single chunked bulk-I/O pass over FeatureIds that BOTH marks removed features' voxels as -1 AND
  // renumbers surviving voxels to their compacted id. Folding the renumber in here (this pass
  // already reads and writes FeatureIds) lets operator() pass cellFeatureIdsRenumbered=true to
  // RemoveInactiveObjects, eliminating its separate full-volume renumber read+write pass. The bad
  // (-1) voxels are filled by assignBadVoxels using neighbor values that are now already compacted,
  // so the final FeatureIds are identical to marking-then-renumbering. Only modified chunks are
  // written back.
  auto featureIdBuf = std::make_unique<int32[]>(k_ChunkTuples);
  for(usize offset = 0; offset < totalPoints; offset += k_ChunkTuples)
  {
    if(m_ShouldCancel)
    {
      return {};
    }
    const usize count = std::min(k_ChunkTuples, totalPoints - offset);
    featureIdsStoreRef.copyIntoBuffer(offset, nonstd::span<int32>(featureIdBuf.get(), count));

    bool modified = false;
    for(usize i = 0; i < count; i++)
    {
      const int32 oldId = featureIdBuf[i];
      const int32 newId = (oldId >= 0 && activeObjects[oldId]) ? static_cast<int32>(newNames[oldId]) : -1;
      if(newId != oldId)
      {
        featureIdBuf[i] = newId;
        modified = true;
      }
    }
    if(modified)
    {
      featureIdsStoreRef.copyFromBuffer(offset, nonstd::span<const int32>(featureIdBuf.get(), count));
    }
  }
  return activeObjects;
}
