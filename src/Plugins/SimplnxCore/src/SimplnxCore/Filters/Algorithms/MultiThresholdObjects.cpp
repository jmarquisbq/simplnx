#include "MultiThresholdObjects.hpp"

#include "MultiThresholdObjectsDirect.hpp"
#include "MultiThresholdObjectsScanline.hpp"

#include "simplnx/DataStructure/DataArray.hpp"
#include "simplnx/Utilities/AlgorithmDispatch.hpp"

using namespace nx::core;

// =============================================================================
// MultiThresholdObjects — Dispatcher
//
// This file contains only the dispatch logic. The actual algorithm implementations
// live in MultiThresholdObjectsDirect.cpp (in-core) and
// MultiThresholdObjectsScanline.cpp (out-of-core).
//
// The dispatch checks every threshold input and the output mask. Valid adaptive
// storage combinations may put any one of these arrays on disk.
// =============================================================================

// -----------------------------------------------------------------------------
MultiThresholdObjects::MultiThresholdObjects(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel,
                                             MultiThresholdObjectsInputValues* inputValues)
: m_DataStructure(dataStructure)
, m_InputValues(inputValues)
, m_ShouldCancel(shouldCancel)
, m_MessageHandler(mesgHandler)
{
}

// -----------------------------------------------------------------------------
MultiThresholdObjects::~MultiThresholdObjects() noexcept = default;

// -----------------------------------------------------------------------------
/**
 * @brief Dispatches to the appropriate algorithm variant based on storage type.
 *
 * Checks every input array referenced by the threshold configuration and the created
 * output mask to determine if OOC storage is in use.
 *
 * Both variants receive identical constructor arguments and produce identical output.
 */
Result<> MultiThresholdObjects::operator()()
{
  auto thresholdsObject = m_InputValues->ArrayThresholdsObject;
  const auto& requiredPaths = thresholdsObject.getRequiredPaths();
  std::vector<const IArray*> targets;
  targets.reserve(requiredPaths.size() + 1);
  for(const auto& path : requiredPaths)
  {
    targets.push_back(m_DataStructure.getDataAs<IDataArray>(path));
  }
  const DataPath maskArrayPath = (*requiredPaths.begin()).replaceName(m_InputValues->OutputDataArrayName);
  targets.push_back(m_DataStructure.getDataAs<IDataArray>(maskArrayPath));
  return DispatchAlgorithm<MultiThresholdObjectsDirect, MultiThresholdObjectsScanline>(AlgorithmArrayTargets(std::move(targets)), m_DataStructure, m_MessageHandler, m_ShouldCancel, m_InputValues);
}
