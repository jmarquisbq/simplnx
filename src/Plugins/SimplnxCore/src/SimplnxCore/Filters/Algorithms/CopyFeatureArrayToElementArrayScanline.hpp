#pragma once

#include "SimplnxCore/SimplnxCore_export.hpp"

#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/Filter/IFilter.hpp"

namespace nx::core
{
struct CopyFeatureArrayToElementArrayInputValues;

/**
 * @class CopyFeatureArrayToElementArrayScanline
 * @brief Out-of-core (chunk-sequential) algorithm for broadcasting feature data to element data.
 *
 * Produces the same output as CopyFeatureArrayToElementArrayDirect: for every cell, the value of
 * the feature that the cell belongs to is copied into a new cell-level array
 * (created[cell] = selectedFeature[featureIds[cell]]).
 *
 * **When this variant is selected**: DispatchAlgorithm selects this class when the FeatureIds
 * array is backed by chunked/OOC storage (or ForceOocAlgorithm() is set in tests).
 *
 * **Why a separate OOC variant exists**: The Direct variant parallelizes per-cell operator[]
 * access across worker threads. For OOC data that is doubly problematic: (1) DataStore/chunk-cache
 * access is not thread-safe, and (2) each operator[] on a chunked store pays virtual-dispatch and
 * chunk-cache lookup overhead. This variant runs single-threaded and reads FeatureIds / writes the
 * created array in bounded sequential chunks via copyIntoBuffer()/copyFromBuffer(). The (small)
 * feature-level source array is cached once into a local buffer. Memory use is bounded by the chunk
 * size plus the feature count -- never proportional to the cell count.
 *
 * @see CopyFeatureArrayToElementArrayDirect for the in-core variant.
 * @see CopyFeatureArrayToElementArray for the dispatcher.
 */
class SIMPLNXCORE_EXPORT CopyFeatureArrayToElementArrayScanline
{
public:
  CopyFeatureArrayToElementArrayScanline(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel,
                                         const CopyFeatureArrayToElementArrayInputValues* inputValues);
  ~CopyFeatureArrayToElementArrayScanline() noexcept;

  CopyFeatureArrayToElementArrayScanline(const CopyFeatureArrayToElementArrayScanline&) = delete;
  CopyFeatureArrayToElementArrayScanline(CopyFeatureArrayToElementArrayScanline&&) noexcept = delete;
  CopyFeatureArrayToElementArrayScanline& operator=(const CopyFeatureArrayToElementArrayScanline&) = delete;
  CopyFeatureArrayToElementArrayScanline& operator=(CopyFeatureArrayToElementArrayScanline&&) noexcept = delete;

  Result<> operator()();

private:
  DataStructure& m_DataStructure;
  const CopyFeatureArrayToElementArrayInputValues* m_InputValues = nullptr;
  const std::atomic_bool& m_ShouldCancel;
  const IFilter::MessageHandler& m_MessageHandler;
};

} // namespace nx::core
