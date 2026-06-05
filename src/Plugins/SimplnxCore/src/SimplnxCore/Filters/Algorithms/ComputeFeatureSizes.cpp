#include "ComputeFeatureSizes.hpp"

#include "ComputeFeatureSizesDirect.hpp"
#include "ComputeFeatureSizesScanline.hpp"

#include "simplnx/DataStructure/DataArray.hpp"
#include "simplnx/Utilities/AlgorithmDispatch.hpp"

using namespace nx::core;

// ----------------------------------------------------------------------------
// ComputeFeatureSizes -- Dispatcher
//
// This file implements the thin dispatch layer for the ComputeFeatureSizes
// algorithm. No algorithm logic lives here; the sole responsibility is to
// inspect the storage type of the FeatureIds array and forward execution to
// either ComputeFeatureSizesDirect (in-core, parallel accumulation) or
// ComputeFeatureSizesScanline (out-of-core, chunked bulk I/O), via the
// DispatchAlgorithm template.
//
// FeatureIds is the critical input: it is a cell-level array with one entry per
// voxel. When stored out-of-core in chunked format, the in-core variant's
// parallel per-element getValue() access is both unsafe (concurrent reads of a
// chunked store) and catastrophically slow (chunk thrashing). The Scanline
// variant avoids both by streaming the array sequentially with copyIntoBuffer().
// ----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
ComputeFeatureSizes::ComputeFeatureSizes(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel, ComputeFeatureSizesInputValues* inputValues)
: m_DataStructure(dataStructure)
, m_InputValues(inputValues)
, m_ShouldCancel(shouldCancel)
, m_MessageHandler(mesgHandler)
{
}

// -----------------------------------------------------------------------------
ComputeFeatureSizes::~ComputeFeatureSizes() noexcept = default;

// -----------------------------------------------------------------------------
Result<> ComputeFeatureSizes::operator()()
{
  auto* featureIdsArray = m_DataStructure.getDataAs<Int32Array>(m_InputValues->FeatureIdsPath);
  return DispatchAlgorithm<ComputeFeatureSizesDirect, ComputeFeatureSizesScanline>({featureIdsArray}, m_DataStructure, m_MessageHandler, m_ShouldCancel, m_InputValues);
}
