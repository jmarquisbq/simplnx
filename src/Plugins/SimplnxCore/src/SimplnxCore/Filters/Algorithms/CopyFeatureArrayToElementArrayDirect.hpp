#pragma once

#include "SimplnxCore/SimplnxCore_export.hpp"

#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/Filter/IFilter.hpp"

namespace nx::core
{
struct CopyFeatureArrayToElementArrayInputValues;

/**
 * @class CopyFeatureArrayToElementArrayDirect
 * @brief In-core (direct memory access) algorithm for broadcasting feature data to element data.
 *
 * For every cell, the value of the feature that the cell belongs to is copied into a new
 * cell-level array (created[cell] = selectedFeature[featureIds[cell]]). The work is parallelized
 * across cells with ParallelDataAlgorithm, reading FeatureIds and the source feature array and
 * writing the created array directly through operator[].
 *
 * **When this variant is selected**: DispatchAlgorithm selects this class when the FeatureIds
 * array is backed by contiguous in-memory storage. With in-memory data, operator[] is a simple
 * pointer dereference and parallel per-cell access saturates memory bandwidth, making this the
 * fastest option for in-core data.
 *
 * **Why a separate OOC variant exists**: For chunked/OOC storage this parallel operator[] pattern
 * is both unsafe (the chunk cache is not thread-safe) and slow (per-element chunk-cache lookups).
 * The Scanline variant avoids both by streaming in bounded chunks on a single thread.
 *
 * @see CopyFeatureArrayToElementArrayScanline for the OOC-optimized variant.
 * @see CopyFeatureArrayToElementArray for the dispatcher.
 */
class SIMPLNXCORE_EXPORT CopyFeatureArrayToElementArrayDirect
{
public:
  CopyFeatureArrayToElementArrayDirect(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel,
                                       const CopyFeatureArrayToElementArrayInputValues* inputValues);
  ~CopyFeatureArrayToElementArrayDirect() noexcept;

  CopyFeatureArrayToElementArrayDirect(const CopyFeatureArrayToElementArrayDirect&) = delete;
  CopyFeatureArrayToElementArrayDirect(CopyFeatureArrayToElementArrayDirect&&) noexcept = delete;
  CopyFeatureArrayToElementArrayDirect& operator=(const CopyFeatureArrayToElementArrayDirect&) = delete;
  CopyFeatureArrayToElementArrayDirect& operator=(CopyFeatureArrayToElementArrayDirect&&) noexcept = delete;

  Result<> operator()();

private:
  DataStructure& m_DataStructure;
  const CopyFeatureArrayToElementArrayInputValues* m_InputValues = nullptr;
  const std::atomic_bool& m_ShouldCancel;
  const IFilter::MessageHandler& m_MessageHandler;
};

} // namespace nx::core
