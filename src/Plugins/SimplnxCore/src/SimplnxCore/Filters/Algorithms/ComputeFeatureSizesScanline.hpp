#pragma once

#include "SimplnxCore/SimplnxCore_export.hpp"

#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/Filter/IFilter.hpp"

namespace nx::core
{
struct ComputeFeatureSizesInputValues;

/**
 * @class ComputeFeatureSizesScanline
 * @brief Out-of-core (OOC) optimized algorithm for computing per-feature volume,
 * equivalent diameter, and voxel count using chunked sequential bulk I/O.
 *
 * **The problem this solves**: When the FeatureIds array (and, for a RectGridGeom,
 * the element-sizes array) is stored out-of-core in chunked format, reading it one
 * voxel at a time through operator[]/getValue() triggers a chunk load/evict cycle on
 * nearly every access, which is catastrophically slow on multi-billion-voxel volumes.
 *
 * **The approach**: FeatureIds (and element sizes for RectGrid) are read in fixed-size
 * chunks via copyIntoBuffer() and the per-voxel counting / Kahan volume accumulation
 * runs against the local in-memory buffer. Accumulators are sized to the feature count
 * (small) rather than the voxel count, so peak working memory is bounded by the chunk
 * size, not the dataset size. Because copyIntoBuffer() degrades to a plain std::copy
 * for in-memory DataStores, this variant is also correct (just unnecessary) for in-core
 * data; the dispatcher only selects it when OOC storage is detected.
 *
 * The summation traverses voxels in the same global raster order as the original serial
 * implementation, so its floating-point results are bit-identical to that baseline.
 *
 * @see ComputeFeatureSizesDirect for the in-core (parallel) variant.
 * @see ComputeFeatureSizes for the dispatcher.
 */
class SIMPLNXCORE_EXPORT ComputeFeatureSizesScanline
{
public:
  ComputeFeatureSizesScanline(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel, const ComputeFeatureSizesInputValues* inputValues);
  ~ComputeFeatureSizesScanline() noexcept;

  ComputeFeatureSizesScanline(const ComputeFeatureSizesScanline&) = delete;
  ComputeFeatureSizesScanline(ComputeFeatureSizesScanline&&) noexcept = delete;
  ComputeFeatureSizesScanline& operator=(const ComputeFeatureSizesScanline&) = delete;
  ComputeFeatureSizesScanline& operator=(ComputeFeatureSizesScanline&&) noexcept = delete;

  /**
   * @brief Executes the feature size computation using chunked bulk I/O.
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
