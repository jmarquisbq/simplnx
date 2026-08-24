#include "RemoveFlaggedFeatures.hpp"

#include "RemoveFlaggedFeaturesDirect.hpp"
#include "RemoveFlaggedFeaturesScanline.hpp"

#include "simplnx/DataStructure/DataArray.hpp"
#include "simplnx/Utilities/AlgorithmDispatch.hpp"
#include "simplnx/Utilities/DataGroupUtilities.hpp"

using namespace nx::core;

// ----------------------------------------------------------------------------
// RemoveFlaggedFeatures -- Dispatcher
//
// This file implements the thin dispatch layer for the RemoveFlaggedFeatures
// algorithm. No algorithm logic lives here; the sole responsibility is to
// inspect the storage type of the FeatureIds array and forward execution to
// either RemoveFlaggedFeaturesDirect (in-core) or RemoveFlaggedFeaturesScanline
// (out-of-core), via the DispatchAlgorithm template.
//
// FeatureIds is the critical input because it is a cell-level array with one
// entry per voxel, read and written repeatedly by the "fill removed features"
// loop. When stored out-of-core in chunked format, the 6-face-neighbor stencil
// used to pick a replacement value (especially the +/-Z neighbors, a full
// Z-slice away) triggers chunk load/evict cycles on every voxel. The Scanline
// variant avoids this by reading/writing entire Z-slices sequentially.
// ----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
RemoveFlaggedFeatures::RemoveFlaggedFeatures(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel,
                                             RemoveFlaggedFeaturesInputValues* inputValues)
: m_DataStructure(dataStructure)
, m_InputValues(inputValues)
, m_ShouldCancel(shouldCancel)
, m_MessageHandler(mesgHandler)
{
}

// -----------------------------------------------------------------------------
RemoveFlaggedFeatures::~RemoveFlaggedFeatures() noexcept = default;

// -----------------------------------------------------------------------------
const std::atomic_bool& RemoveFlaggedFeatures::getCancel()
{
  return m_ShouldCancel;
}

// -----------------------------------------------------------------------------
/**
 * @brief Inspects the FeatureIds array's storage type and dispatches to the
 * appropriate algorithm variant.
 *
 * The dispatch decision is made by DispatchAlgorithm, which checks:
 *   1. ForceInCoreAlgorithm() -- test override, always selects Direct
 *   2. AnyOutOfCore({featureIdsArray}) -- runtime detection of chunked storage
 *   3. ForceOocAlgorithm() -- test override, forces Scanline
 *   4. Default -- selects Direct (in-core)
 */
Result<> RemoveFlaggedFeatures::operator()()
{
  std::vector<const IArray*> targets;
  const auto append = [&targets](const IDataArray* array) {
    if(array != nullptr)
    {
      targets.push_back(array);
    }
  };

  append(m_DataStructure.getDataAs<IDataArray>(m_InputValues->FeatureIdsArrayPath));
  if(static_cast<Functionality>(m_InputValues->ExtractFeatures) != Functionality::Extract && m_InputValues->FillRemovedFeatures)
  {
    for(const auto& array : GenerateDataArrayList(m_DataStructure, m_InputValues->FeatureIdsArrayPath, m_InputValues->IgnoredDataArrayPaths))
    {
      append(array.get());
    }
  }
  return DispatchAlgorithm<RemoveFlaggedFeaturesDirect, RemoveFlaggedFeaturesScanline>(AlgorithmArrayTargets(std::move(targets)), m_DataStructure, m_MessageHandler, m_ShouldCancel, m_InputValues);
}
