#pragma once

#include "SimplnxCore/SimplnxCore_export.hpp"

#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/Filter/IFilter.hpp"

namespace nx::core
{
struct ComputeKMeansInputValues;

/**
 * @class ComputeKMeansScanline
 * @brief Out-of-core algorithm for K-Means clustering using chunked bulk I/O.
 *
 * Uses copyIntoBuffer()/copyFromBuffer() to read input data and write cluster
 * assignments in fixed-size chunks (64K tuples), avoiding random per-element
 * OOC access that would cause chunk thrashing.
 *
 * Key OOC optimizations over the Direct variant:
 *
 * - **Centroid initialization**: Uses copyIntoBuffer()/copyFromBuffer() per-tuple
 *   instead of operator[] to read initial centroid values.
 *
 * - **Cluster assignment (findClusters)**: Caches all centroids in a local vector
 *   (small: k * numComponents), then processes the input array and featureIds
 *   in aligned 64K-tuple chunks via bulk I/O. Each chunk is read once, all
 *   distance computations for that chunk are done in memory, then featureIds
 *   are written back in one bulk operation.
 *
 * - **Centroid recomputation (findMeans)**: Accumulates per-cluster sums and
 *   counts for ALL components in a single chunked pass over the input array,
 *   instead of the Direct algorithm's dims-many independent full-array rescans.
 *   Per-accumulator floating-point sums are unaffected by this restructuring
 *   because each accumulator still receives its increments in the same
 *   increasing-tuple-index order as the Direct variant — only the traversal
 *   interleaving of unrelated accumulators changes, which floating-point
 *   addition is indifferent to.
 *
 * - **Convergence check**: Reads the (tiny) means array via copyIntoBuffer()
 *   instead of operator[] to capture before/after snapshots for the mean-shift
 *   comparison, preserving the Direct algorithm's exact flat-index read.
 *
 * Selected by DispatchAlgorithm when any input array is backed by out-of-core storage.
 *
 * @see ComputeKMeansDirect for the in-core-optimized alternative.
 * @see AlgorithmDispatch.hpp for the dispatch mechanism that selects between them.
 */
class SIMPLNXCORE_EXPORT ComputeKMeansScanline
{
public:
  /**
   * @brief Constructs the out-of-core algorithm with all resources it needs.
   * @param dataStructure The DataStructure containing input/output arrays
   * @param mesgHandler Message handler for progress reporting
   * @param shouldCancel Atomic flag checked periodically to support user cancellation
   * @param inputValues Non-owning pointer to the parameter bundle
   */
  ComputeKMeansScanline(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel, const ComputeKMeansInputValues* inputValues);
  ~ComputeKMeansScanline() noexcept;

  ComputeKMeansScanline(const ComputeKMeansScanline&) = delete;
  ComputeKMeansScanline(ComputeKMeansScanline&&) noexcept = delete;
  ComputeKMeansScanline& operator=(const ComputeKMeansScanline&) = delete;
  ComputeKMeansScanline& operator=(ComputeKMeansScanline&&) noexcept = delete;

  /**
   * @brief Executes the OOC-optimized K-Means clustering.
   * @return Result<> with any errors encountered during execution
   */
  Result<> operator()();

  /**
   * @brief Sends a progress message through the filter's message handler.
   * @param message The progress message text
   */
  void updateProgress(const std::string& message);

  /**
   * @brief Returns a reference to the cancellation flag for checking in inner loops.
   * @return Reference to the atomic bool cancellation flag
   */
  const std::atomic_bool& getCancel();

private:
  DataStructure& m_DataStructure;                          ///< Reference to the DataStructure containing all arrays
  const ComputeKMeansInputValues* m_InputValues = nullptr; ///< Non-owning pointer to input parameters
  const std::atomic_bool& m_ShouldCancel;                  ///< User cancellation flag
  const IFilter::MessageHandler& m_MessageHandler;         ///< Message handler for progress updates
};

} // namespace nx::core
