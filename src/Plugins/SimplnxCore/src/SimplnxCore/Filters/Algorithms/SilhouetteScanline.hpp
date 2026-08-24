#pragma once

#include "SimplnxCore/SimplnxCore_export.hpp"

#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/Filter/IFilter.hpp"

namespace nx::core
{
struct SilhouetteInputValues;

/**
 * @class SilhouetteScanline
 * @brief Computes exact silhouette scores with bounded tiles and checked bulk I/O.
 *
 * The implementation retains the all-pairs mathematics and deterministic tuple
 * order of SilhouetteDirect while replacing per-element DataStore access and
 * cell-count workspaces with fixed tuple tiles. It discovers sparse feature IDs,
 * accumulates one outer tile's distances by feature, and writes each completed
 * output tile before advancing. The memory footprint is therefore bounded by
 * the tile size, component count, and number of features rather than all cells.
 *
 * Silhouette dispatches here when any participating array is out-of-core, or
 * when the OOC route is explicitly forced for verification. The object borrows
 * all constructor arguments and does not own their storage.
 *
 * @see SilhouetteDirect for the resident-array implementation.
 */
class SIMPLNXCORE_EXPORT SilhouetteScanline
{
public:
  /**
   * @brief Creates the bounded, bulk-I/O silhouette implementation.
   * @param dataStructure Data structure containing the clustering input, feature IDs, optional mask, and output.
   * @param messageHandler Accepted for dispatch compatibility; this implementation does not emit progress messages.
   * @param shouldCancel Cancellation flag checked between discovery, counting, and pairwise tiles.
   * @param inputValues Non-owning pointer to the filter arguments. It must remain valid through operator()().
   */
  SilhouetteScanline(DataStructure& dataStructure, const IFilter::MessageHandler& messageHandler, const std::atomic_bool& shouldCancel, const SilhouetteInputValues* inputValues);
  ~SilhouetteScanline() noexcept;

  SilhouetteScanline(const SilhouetteScanline&) = delete;
  SilhouetteScanline(SilhouetteScanline&&) noexcept = delete;
  SilhouetteScanline& operator=(const SilhouetteScanline&) = delete;
  SilhouetteScanline& operator=(SilhouetteScanline&&) noexcept = delete;

  /**
   * @brief Runs feature discovery, feature counting, tiled distance accumulation, and bulk output writes.
   * @return A valid result on success or cancellation; otherwise an I/O, mask-type, feature-ID, or size-overflow error.
   */
  Result<> operator()();

private:
  DataStructure& m_DataStructure;
  const std::atomic_bool& m_ShouldCancel;
  const SilhouetteInputValues* m_InputValues = nullptr;
};
} // namespace nx::core
