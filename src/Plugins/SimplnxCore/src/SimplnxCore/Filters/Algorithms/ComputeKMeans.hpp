#pragma once

#include "SimplnxCore/SimplnxCore_export.hpp"

#include "simplnx/DataStructure/DataPath.hpp"
#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/Filter/IFilter.hpp"
#include "simplnx/Parameters/ArrayCreationParameter.hpp"
#include "simplnx/Parameters/ArraySelectionParameter.hpp"
#include "simplnx/Parameters/ChoicesParameter.hpp"
#include "simplnx/Parameters/NumberParameter.hpp"
#include "simplnx/Utilities/ClusteringUtilities.hpp"

namespace nx::core
{

/**
 * @struct ComputeKMeansInputValues
 * @brief Input parameter bundle for the ComputeKMeans algorithm.
 *
 * Aggregates all DataPaths and configuration values needed by both the in-core
 * (Direct) and out-of-core (Scanline) variants of K-Means clustering.
 */
struct SIMPLNXCORE_EXPORT ComputeKMeansInputValues
{
  uint64 InitClusters;                             ///< Number of clusters (k) to partition the data into
  ClusterUtilities::DistanceMetric DistanceMetric; ///< Distance metric used for cluster assignment
  bool UseMask = false;                            ///< Whether MaskArrayPath is active
  DataPath ClusteringArrayPath;                    ///< Input array containing the data to be clustered (any numeric type)
  DataPath MaskArrayPath;                          ///< Input Bool/UInt8 mask array; masked-out elements are excluded from clustering
  DataPath FeatureIdsArrayPath;                    ///< Output Int32 array storing per-element cluster assignments
  DataPath MeansArrayPath;                         ///< Output array storing the mean (centroid) for each cluster
  uint64 Seed;                                     ///< Random seed for reproducible initial centroid selection
};

/**
 * @class ComputeKMeans
 * @brief Dispatcher algorithm for K-Means clustering.
 *
 * K-Means is a partitioning clustering algorithm that assigns each data point to the
 * nearest centroid (the arithmetic mean of its cluster's members), then iteratively
 * recomputes centroids and reassigns points until the centroids stop moving (convergence).
 *
 * This class acts as a thin dispatcher that selects between two concrete implementations:
 *
 * - **ComputeKMeansDirect** (in-core): Uses per-element operator[] access for distance
 *   computation, cluster assignment, and centroid accumulation. Optimal when all arrays
 *   reside in memory.
 *
 * - **ComputeKMeansScanline** (out-of-core / OOC): Uses chunked copyIntoBuffer() /
 *   copyFromBuffer() bulk I/O to read input data and write cluster assignments in
 *   fixed-size chunks (64K tuples), avoiding per-element OOC access on each convergence
 *   iteration.
 *
 * The dispatch decision is made by DispatchAlgorithm<Direct, Scanline>() in
 * AlgorithmDispatch.hpp, which checks whether any input IDataArray uses OOC storage.
 *
 * **Why two variants exist**: Each convergence iteration performs two full passes over
 * the input array — findClusters (nearest-centroid assignment) and findMeans (per-cluster
 * sum/count accumulation, further multiplied by the number of components since the
 * in-core algorithm rescans the array once per component). When data is stored
 * out-of-core, per-element operator[] access on each of these passes triggers a chunk
 * load/evict cycle. The Scanline variant streams the input array in bounded chunks for
 * both passes, converting random per-element access into sequential bulk reads.
 *
 * @see ComputeKMeansDirect
 * @see ComputeKMeansScanline
 * @see AlgorithmDispatch.hpp
 */
class SIMPLNXCORE_EXPORT ComputeKMeans
{
public:
  /**
   * @brief Constructs the dispatcher with all resources needed by either algorithm variant.
   * @param dataStructure The DataStructure containing input/output arrays
   * @param mesgHandler Message handler for progress reporting
   * @param shouldCancel Atomic flag checked periodically to support user cancellation
   * @param inputValues Non-owning pointer to the parameter bundle
   */
  ComputeKMeans(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel, ComputeKMeansInputValues* inputValues);
  ~ComputeKMeans() noexcept;

  ComputeKMeans(const ComputeKMeans&) = delete;
  ComputeKMeans(ComputeKMeans&&) noexcept = delete;
  ComputeKMeans& operator=(const ComputeKMeans&) = delete;
  ComputeKMeans& operator=(ComputeKMeans&&) noexcept = delete;

  /**
   * @brief Dispatches to the Direct or Scanline algorithm based on storage type.
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
