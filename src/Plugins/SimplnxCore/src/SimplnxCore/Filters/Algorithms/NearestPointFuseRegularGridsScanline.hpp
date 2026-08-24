#pragma once

#include "SimplnxCore/Filters/Algorithms/NearestPointFuseRegularGrids.hpp"

namespace nx::core
{
/**
 * @class NearestPointFuseRegularGridsScanline
 * @brief Resamples cell arrays with axis maps and one source/destination row in memory at a time.
 *
 * The implementation precomputes independent X, Y, and Z nearest-source maps,
 * then walks reference rows in storage order. A source row is loaded once and
 * reused while its mapped Y/Z coordinates remain unchanged; each destination row
 * is emitted with one checked bulk write. This changes the OOC cost from random
 * per-cell reads to sequential row transfers while using O(X + Y + Z) mapping
 * state plus two row buffers.
 *
 * The dispatcher selects this route when any participating cell array is
 * disk-backed. Constructor arguments are borrowed and must outlive execution.
 *
 * @see NearestPointFuseRegularGridsDirect for the parallel resident-array route.
 */
class SIMPLNXCORE_EXPORT NearestPointFuseRegularGridsScanline
{
public:
  /**
   * @brief Creates the row-buffered nearest-point resampling implementation.
   * @param dataStructure Data structure containing both image geometries and their cell arrays.
   * @param messageHandler Message callback retained for the common dispatched interface.
   * @param shouldCancel Cancellation flag checked before arrays and reference Z slices.
   * @param inputValues Non-owning pointer to geometry paths and the out-of-bounds fill value.
   */
  NearestPointFuseRegularGridsScanline(DataStructure& dataStructure, const IFilter::MessageHandler& messageHandler, const std::atomic_bool& shouldCancel,
                                       const NearestPointFuseRegularGridsInputValues* inputValues);
  /**
   * @brief Resamples each numeric sampling-cell array using typed row buffers.
   * @return A valid result on success or cancellation; otherwise the first bulk DataStore I/O error.
   */
  Result<> operator()();

private:
  DataStructure& m_DataStructure;
  const IFilter::MessageHandler& m_MessageHandler;
  const std::atomic_bool& m_ShouldCancel;
  const NearestPointFuseRegularGridsInputValues* m_InputValues = nullptr;
};
} // namespace nx::core
