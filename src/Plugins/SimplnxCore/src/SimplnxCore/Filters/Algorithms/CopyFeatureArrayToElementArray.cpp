#include "CopyFeatureArrayToElementArray.hpp"

#include "CopyFeatureArrayToElementArrayDirect.hpp"
#include "CopyFeatureArrayToElementArrayScanline.hpp"

#include "simplnx/DataStructure/DataArray.hpp"
#include "simplnx/Utilities/AlgorithmDispatch.hpp"

using namespace nx::core;

// ----------------------------------------------------------------------------
// CopyFeatureArrayToElementArray -- Dispatcher
//
// This file implements the thin dispatch layer for the CopyFeatureArrayToElementArray
// algorithm. No algorithm logic lives here; the sole responsibility is to inspect the
// storage type of every cell-level input and output and forward execution to either
// CopyFeatureArrayToElementArrayDirect (in-core) or CopyFeatureArrayToElementArrayScanline
// (out-of-core), via the DispatchAlgorithm template.
//
// A mixed store is valid: FeatureIds, any selected feature source, or any created cell
// output may be out-of-core. When any target is
// backed by chunked/OOC storage, the Direct variant's parallel per-element operator[]
// access is both unsafe (the chunk cache is not thread-safe) and slow. The Scanline
// variant streams FeatureIds in and the created arrays out using sequential bulk I/O.
// ----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
CopyFeatureArrayToElementArray::CopyFeatureArrayToElementArray(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel,
                                                               const CopyFeatureArrayToElementArrayInputValues* inputValues)
: m_DataStructure(dataStructure)
, m_InputValues(inputValues)
, m_ShouldCancel(shouldCancel)
, m_MessageHandler(mesgHandler)
{
}

// -----------------------------------------------------------------------------
CopyFeatureArrayToElementArray::~CopyFeatureArrayToElementArray() noexcept = default;

// -----------------------------------------------------------------------------
Result<> CopyFeatureArrayToElementArray::operator()()
{
  if(m_InputValues->SelectedFeatureArrayPaths.empty())
  {
    return {};
  }

  std::vector<const IArray*> targets;
  targets.reserve(1 + (2 * m_InputValues->SelectedFeatureArrayPaths.size()));
  targets.push_back(m_DataStructure.getDataAs<Int32Array>(m_InputValues->FeatureIdsPath));
  for(const auto& selectedFeatureArrayPath : m_InputValues->SelectedFeatureArrayPaths)
  {
    targets.push_back(m_DataStructure.getDataAs<IDataArray>(selectedFeatureArrayPath));
    const DataPath createdArrayPath = m_InputValues->FeatureIdsPath.replaceName(selectedFeatureArrayPath.getTargetName() + m_InputValues->CreatedArraySuffix);
    targets.push_back(m_DataStructure.getDataAs<IDataArray>(createdArrayPath));
  }
  return DispatchAlgorithm<CopyFeatureArrayToElementArrayDirect, CopyFeatureArrayToElementArrayScanline>(AlgorithmArrayTargets{std::move(targets)}, m_DataStructure, m_MessageHandler, m_ShouldCancel,
                                                                                                         m_InputValues);
}
