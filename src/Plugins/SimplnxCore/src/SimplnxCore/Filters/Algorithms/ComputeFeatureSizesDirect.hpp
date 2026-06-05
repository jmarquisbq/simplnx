#pragma once

#include "SimplnxCore/SimplnxCore_export.hpp"

#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/Filter/IFilter.hpp"

namespace nx::core
{
struct ComputeFeatureSizesInputValues;

/**
 * @class ComputeFeatureSizesDirect
 * @brief In-core algorithm for computing per-feature volume, equivalent diameter,
 * and voxel count using parallel, thread-local accumulation.
 *
 * This is the in-memory variant. The per-voxel counting (and, for a RectGridGeom,
 * Kahan volume summation) is parallelized across Z-slices using ParallelDataAlgorithm
 * with tbb::combinable thread-local accumulators that are reduced after the parallel
 * region. Each worker reads FeatureIds / element sizes through getValue(), which is a
 * cheap pointer dereference when the DataStore is a contiguous in-memory buffer.
 *
 * **When this variant is selected**: DispatchAlgorithm selects this class when the
 * FeatureIds array is backed by an in-memory DataStore (the common case). It must not
 * be used for out-of-core data: concurrent getValue() calls across worker threads are
 * not safe on chunked stores and would also thrash the chunk cache. The
 * ComputeFeatureSizesScanline variant handles OOC data with sequential bulk I/O.
 *
 * @see ComputeFeatureSizesScanline for the out-of-core variant.
 * @see ComputeFeatureSizes for the dispatcher.
 */
class SIMPLNXCORE_EXPORT ComputeFeatureSizesDirect
{
public:
  ComputeFeatureSizesDirect(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel, const ComputeFeatureSizesInputValues* inputValues);
  ~ComputeFeatureSizesDirect() noexcept;

  ComputeFeatureSizesDirect(const ComputeFeatureSizesDirect&) = delete;
  ComputeFeatureSizesDirect(ComputeFeatureSizesDirect&&) noexcept = delete;
  ComputeFeatureSizesDirect& operator=(const ComputeFeatureSizesDirect&) = delete;
  ComputeFeatureSizesDirect& operator=(ComputeFeatureSizesDirect&&) noexcept = delete;

  /**
   * @brief Executes the feature size computation using parallel in-core accumulation.
   * @return Result<> indicating success or error.
   */
  Result<> operator()();

private:
  DataStructure& m_DataStructure;                                ///< Reference to the DataStructure.
  const ComputeFeatureSizesInputValues* m_InputValues = nullptr; ///< User-configured parameters.
  const std::atomic_bool& m_ShouldCancel;                        ///< Cancellation flag.
  const IFilter::MessageHandler& m_MessageHandler;               ///< Message handler for progress.
};

} // namespace nx::core
