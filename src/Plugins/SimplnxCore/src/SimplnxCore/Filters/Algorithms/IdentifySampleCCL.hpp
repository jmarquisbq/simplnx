#pragma once

#include "SimplnxCore/SimplnxCore_export.hpp"

#include "simplnx/DataStructure/DataPath.hpp"
#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/Filter/IFilter.hpp"

namespace nx::core
{

struct IdentifySampleInputValues;

/**
 * @class IdentifySampleCCL
 * @brief Out-of-core optimized algorithm for identifying the largest connected
 * sample region using scanline Connected Component Labeling (CCL) with bounded external equivalences.
 *
 * This is the OOC-optimized implementation of the IdentifySample algorithm. It
 * replaces the BFS flood-fill approach with scanline CCL that processes data in
 * strict Z-slice sequential order, avoiding the random access pattern that causes
 * chunk thrashing when data is stored in compressed HDF5 chunks.
 *
 * ## Why CCL Instead of BFS for Out-of-Core
 *
 * BFS flood-fill visits neighbors in a wavefront pattern, crossing Z-slice (and
 * chunk) boundaries unpredictably. Each getValue() call on an OOC mask array may
 * trigger chunk decompression, and the wavefront pattern causes the same chunks
 * to be loaded and evicted repeatedly. For large volumes (e.g., 500x500x500),
 * this can turn a sub-second in-core operation into a multi-hour ordeal.
 *
 * CCL avoids this by scanning voxels in a fixed Z-Y-X order, only checking
 * backward neighbors (x-1, y-1, z-1). Each Z-slice is read exactly once via
 * a bulk copyIntoBuffer call. Cross-slice connectivity is resolved symbolically
 * through bounded external-equivalence records, rather than resident label
 * tables that could grow with the cell count.
 *
 * ## Algorithm Phases
 *
 * The algorithm has up to four phases (Phases 3-4 only run if FillHoles is true):
 *
 * **Phase 1 -- Forward CCL on good voxels:**
 * Runs runForwardCCL() with condition = (mask[i] == true). Discovers all
 * connected components of good voxels, tracks per-component sizes, and identifies
 * the largest component as "the sample". Uses a rolling 2-slice label buffer
 * (O(dimX * dimY) memory) for backward neighbor lookups.
 *
 * **Phase 2 -- Replay CCL to mask non-sample voxels:**
 * Runs replayForwardCCL() with the same good-voxel condition. This re-executes
 * the exact same forward scan deterministically, re-deriving the same provisional
 * labels on the fly. For each voxel whose resolved root is not the largest
 * component, sets the mask to false (removing satellite regions). This avoids
 * O(volume) label storage by recomputing labels instead of storing them.
 *
 * **Phase 3 -- Forward CCL on bad voxels (hole detection, optional):**
 * Runs runForwardCCL() with condition = (mask[i] == false). Discovers connected
 * components of non-sample space (exterior empty space + interior holes).
 *
 * **Phase 4 -- Replay CCL to fill interior holes (optional):**
 * Two replay passes: (4a) identifies which bad-voxel components touch the domain
 * boundary (these are exterior, not holes); (4b) fills interior holes (components
 * NOT touching any boundary) by setting their mask voxels to true.
 *
 * ## The Replay Trick -- Avoiding O(Volume) Label Storage
 *
 * The key OOC technique in this algorithm is the "replay" approach. Scanline CCL
 * label assignment is fully deterministic given the same scan order and condition.
 * So instead of storing labels for the entire volume (O(volume) memory), we
 * re-run the forward scan to re-derive the same labels on the fly. The already-
 * external equivalence table resolves each re-derived label to its root. This
 * trades an extra data read (reading each Z-slice twice) for massive memory
 * savings -- critical when the volume itself does not fit in RAM.
 *
 * ## Memory Usage
 *
 * - Whole-volume path: O(dimX * dimY) for rolling mask/label slices
 * - Slice-by-slice path: O(plane) mask buffer plus O(row) labels
 * - Component parent, size, and boundary state: bounded page caches over fixed
 *   temporary records, independent of the cell count in RAM
 *
 * No O(volume) memory is allocated at any point.
 *
 * ## Slice-By-Slice Mode
 *
 * When the user enables slice-by-slice processing, this class uses a row-streaming
 * CCL per plane. XY transfers a contiguous plane, XZ transfers contiguous rows,
 * and YZ bulk-transfers Z slices before extracting or updating a column.
 *
 * @see IdentifySampleBFS for the in-core-optimized alternative.
 * @see IdentifySample for the dispatcher that selects between BFS and CCL.
 * @see IdentifySampleCommon.hpp for the in-core slice-by-slice functor.
 * @see AlgorithmDispatch.hpp for the dispatch mechanism.
 */
class SIMPLNXCORE_EXPORT IdentifySampleCCL
{
public:
  /**
   * @brief Constructs the CCL sample identification algorithm with the required context.
   * @param dataStructure The data structure containing the arrays to process.
   * @param mesgHandler Handler for progress and informational messages.
   * @param shouldCancel Cancellation flag checked during execution.
   * @param inputValues Filter parameter values controlling identification behavior.
   */
  IdentifySampleCCL(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel, const IdentifySampleInputValues* inputValues);
  ~IdentifySampleCCL() noexcept;

  IdentifySampleCCL(const IdentifySampleCCL&) = delete;
  IdentifySampleCCL(IdentifySampleCCL&&) noexcept = delete;
  IdentifySampleCCL& operator=(const IdentifySampleCCL&) = delete;
  IdentifySampleCCL& operator=(IdentifySampleCCL&&) noexcept = delete;

  /**
   * @brief Executes the CCL-based algorithm to identify the largest sample region.
   *
   * If slice-by-slice mode is enabled, delegates to IdentifySampleSliceBySliceFunctor.
   * Otherwise, runs the full 3D CCL algorithm (Phases 1-2, plus Phases 3-4 if
   * FillHoles is true). The algorithm modifies the mask array in place, producing
   * results identical to IdentifySampleBFS.
   *
   * @return Result indicating success or an error with a descriptive message.
   */
  Result<> operator()();

private:
  DataStructure& m_DataStructure;                           ///< Reference to the DataStructure containing all arrays
  const IdentifySampleInputValues* m_InputValues = nullptr; ///< Non-owning pointer to filter parameter values
  const std::atomic_bool& m_ShouldCancel;                   ///< Cancellation flag checked between Z-slice iterations
  const IFilter::MessageHandler& m_MessageHandler;          ///< Handler for emitting progress/informational messages
};

} // namespace nx::core
