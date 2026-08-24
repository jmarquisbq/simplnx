#pragma once

#include "SimplnxCore/Filters/Algorithms/NearestPointFuseRegularGrids.hpp"

namespace nx::core
{
/**
 * @class NearestPointFuseRegularGridsDirect
 * @brief Resamples resident cell arrays onto a reference image geometry using direct nearest-point access.
 *
 * Arrays are processed concurrently and each output cell computes its source
 * coordinate independently. This is the fastest route for contiguous in-memory
 * stores, where direct tuple access is a pointer operation. The dispatcher avoids
 * this implementation for OOC arrays because its per-cell source reads would cause
 * repeated chunk lookup and disk I/O.
 *
 * The algorithm borrows the DataStructure, cancellation flag, message handler,
 * and input-values bundle for the duration of execution.
 *
 * @see NearestPointFuseRegularGridsScanline for the row-buffered OOC route.
 */
class SIMPLNXCORE_EXPORT NearestPointFuseRegularGridsDirect
{
public:
  /**
   * @brief Creates the direct, parallel resampling implementation.
   * @param dataStructure Data structure containing both image geometries and their cell arrays.
   * @param messageHandler Message callback retained for the common dispatched interface.
   * @param shouldCancel Cancellation flag checked while traversing arrays and reference slices.
   * @param inputValues Non-owning pointer to geometry paths and the out-of-bounds fill value.
   */
  NearestPointFuseRegularGridsDirect(DataStructure& dataStructure, const IFilter::MessageHandler& messageHandler, const std::atomic_bool& shouldCancel,
                                     const NearestPointFuseRegularGridsInputValues* inputValues);
  /**
   * @brief Resamples each numeric sampling-cell array into its corresponding reference-cell array.
   * @return A valid result after all per-array tasks finish, including when cancellation stops traversal early.
   */
  Result<> operator()();

private:
  DataStructure& m_DataStructure;
  const IFilter::MessageHandler& m_MessageHandler;
  const std::atomic_bool& m_ShouldCancel;
  const NearestPointFuseRegularGridsInputValues* m_InputValues = nullptr;
};
} // namespace nx::core
