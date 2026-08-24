#include "ComputeKMeans.hpp"

#include "ComputeKMeansDirect.hpp"
#include "ComputeKMeansScanline.hpp"

#include "simplnx/DataStructure/DataArray.hpp"
#include "simplnx/Utilities/AlgorithmDispatch.hpp"

using namespace nx::core;

// =============================================================================
// ComputeKMeans — Dispatcher
//
// This file contains only the dispatch logic. The actual algorithm implementations
// live in ComputeKMeansDirect.cpp (in-core) and ComputeKMeansScanline.cpp
// (out-of-core).
//
// The dispatch checks both the ClusteringArray and FeatureIds array storage types:
// if either uses chunked on-disk storage (OOC), the Scanline variant is selected
// to avoid chunk thrashing during the iterative distance and mean computations.
// =============================================================================

// -----------------------------------------------------------------------------
ComputeKMeans::ComputeKMeans(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel, ComputeKMeansInputValues* inputValues)
: m_DataStructure(dataStructure)
, m_InputValues(inputValues)
, m_ShouldCancel(shouldCancel)
, m_MessageHandler(mesgHandler)
{
}

// -----------------------------------------------------------------------------
ComputeKMeans::~ComputeKMeans() noexcept = default;

// -----------------------------------------------------------------------------
void ComputeKMeans::updateProgress(const std::string& message)
{
  m_MessageHandler(IFilter::Message::Type::Info, message);
}

// -----------------------------------------------------------------------------
const std::atomic_bool& ComputeKMeans::getCancel()
{
  return m_ShouldCancel;
}

// -----------------------------------------------------------------------------
/**
 * @brief Dispatches to the appropriate algorithm variant based on storage type.
 *
 * Uses DispatchAlgorithm<Direct, Scanline>() to check whether the ClusteringArray
 * or FeatureIds array is backed by out-of-core (chunked) storage. If so, the
 * Scanline variant is used; otherwise, the Direct variant is selected.
 *
 * Both variants receive identical constructor arguments, use identical RNG seeding
 * and convergence math, and produce identical output.
 */
Result<> ComputeKMeans::operator()()
{
  // Include every Scanline input/output; an OOC mask or means array must not
  // accidentally route execution to Direct's element-wise implementation.
  auto* clusteringArray = m_DataStructure.getDataAs<IDataArray>(m_InputValues->ClusteringArrayPath);
  auto* featureIdsArray = m_DataStructure.getDataAs<Int32Array>(m_InputValues->FeatureIdsArrayPath);
  auto* meansArray = m_DataStructure.getDataAs<IDataArray>(m_InputValues->MeansArrayPath);
  std::vector<const IArray*> targets = {clusteringArray, featureIdsArray, meansArray};
  if(m_InputValues->UseMask)
  {
    targets.push_back(m_DataStructure.getDataAs<IDataArray>(m_InputValues->MaskArrayPath));
  }
  return DispatchAlgorithm<ComputeKMeansDirect, ComputeKMeansScanline>(AlgorithmArrayTargets(std::move(targets)), m_DataStructure, m_MessageHandler, m_ShouldCancel, m_InputValues);
}
