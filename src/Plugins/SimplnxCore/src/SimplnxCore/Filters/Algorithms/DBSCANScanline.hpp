#pragma once

#include "SimplnxCore/SimplnxCore_export.hpp"

#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/Filter/IFilter.hpp"

namespace nx::core
{
struct DBSCANInputValues;

/**
 * @class DBSCANScanline
 * @brief Out-of-core algorithm for grid-based DBSCAN using bounded external records.
 *
 * The DBSCAN algorithm has two major data access phases that benefit from OOC optimization:
 *
 * **Grid construction**: The input array and optional mask are scanned in fixed tuple
 * windows. Selected points are externally sorted into grid membership order, while
 * active-grid state and occupied-axis indexes use temporary fixed-width record stores.
 *
 * **Distance computation**: Grid membership records preserve coordinates in the source
 * primitive type. Pairwise checks read fixed-size record tiles, so even a densely
 * populated grid cell does not require a cell-sized allocation.
 *
 * **Clustering and labeling**: Cluster state uses a bounded page cache over temporary
 * records. Labels are externally restored to tuple order and written in fixed windows.
 *
 * Selected by DispatchAlgorithm when the coordinates, enabled mask, or created
 * FeatureIds target is backed by out-of-core storage.
 *
 * @see DBSCANDirect for the in-core-optimized alternative.
 * @see AlgorithmDispatch.hpp for the dispatch mechanism that selects between them.
 */
class SIMPLNXCORE_EXPORT DBSCANScanline
{
public:
  /**
   * @brief Constructs the out-of-core algorithm with all resources it needs.
   * @param dataStructure The DataStructure containing input/output arrays
   * @param mesgHandler Message handler for progress reporting
   * @param shouldCancel Atomic flag checked periodically to support user cancellation
   * @param inputValues Non-owning pointer to the parameter bundle
   */
  DBSCANScanline(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel, const DBSCANInputValues* inputValues);
  ~DBSCANScanline() noexcept;

  DBSCANScanline(const DBSCANScanline&) = delete;
  DBSCANScanline(DBSCANScanline&&) noexcept = delete;
  DBSCANScanline& operator=(const DBSCANScanline&) = delete;
  DBSCANScanline& operator=(DBSCANScanline&&) noexcept = delete;

  /**
   * @brief Executes the OOC-optimized DBSCAN clustering: grid construction, clustering, labeling.
   * @return Result<> with any errors encountered during execution
   */
  Result<> operator()();

private:
  DataStructure& m_DataStructure;                   ///< Reference to the DataStructure containing all arrays
  const DBSCANInputValues* m_InputValues = nullptr; ///< Non-owning pointer to input parameters
  const std::atomic_bool& m_ShouldCancel;           ///< User cancellation flag
};

} // namespace nx::core
