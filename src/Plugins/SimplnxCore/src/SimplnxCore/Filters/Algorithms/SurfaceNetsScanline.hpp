#pragma once

#include "SimplnxCore/SimplnxCore_export.hpp"

#include "SimplnxCore/SurfaceNets/MMCellFlag.h"

#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/DataStructure/IO/Generic/ITemporaryRecordStore.hpp"
#include "simplnx/Filter/IFilter.hpp"
#include "simplnx/Utilities/BoundedRecordPageCache.hpp"

#include <array>
#include <limits>
#include <memory>
#include <type_traits>

namespace nx::core
{
struct SurfaceNetsInputValues;

/**
 * @class SurfaceNetsScanline
 * @brief Out-of-core (OOC) optimized algorithm for SurfaceNets.
 *
 * Selected by DispatchAlgorithm when any input array is backed by chunked
 * (OOC) storage. Produces identical output to SurfaceNetsDirect but avoids
 * the O(volume) MMCellMap allocation and per-element FeatureIds access.
 *
 * ## OOC Strategy
 *
 * The key insight is that the Surface Nets cell classification only needs
 * the 8 corner labels of each cell, which span at most 2 adjacent Z-slices.
 * The scanline variant exploits this by:
 *
 *   - **Bulk I/O**: Reading FeatureIds two Z-slices at a time via
 *     copyIntoBuffer() with a rolling ping-pong buffer. Each cell's 8
 *     corner labels are resolved from the two buffered slices.
 *
 *   - **Bounded temporary storage**: Padded cell records live in a fixed-width
 *     temporary record store. Disk-backed OOC execution pages this record
 *     store through a bounded cache; forced scanline tests with in-memory
 *     arrays use the storage-neutral in-memory fallback.
 *
 *   - **Self-contained smoothing**: The relaxation and all face-neighbor
 *     lookups use the bounded padded-cell record cache, without needing the
 *     full MMCellMap or a resident surface map.
 *
 *   - **Buffered output writes**: Triangle connectivity, face labels, vertex
 *     coordinates, and transfer records are streamed in fixed-size chunks.
 *
 * ## Phases
 *
 *   1. **Cell classification** (operator() main loop) -- Iterates padded cells
 *      in Z-slice order, reads 8 corner labels from rolling slice buffers,
 *      computes MMCellFlag for each cell, and writes fixed-width records.
 *
 *   2A. **Smoothing** (optional) -- Iterative relaxation using face-connected
 *       neighbor positions looked up through the record cache. This preserves
 *       MMSurfaceNet::relax() convergence without resident surface staging.
 *
 *   2B. **Vertex transform** -- Converts local cell-relative positions to
 *       world coordinates and assigns node types from MMCellFlag junction counts.
 *
 *   3A. **Triangle counting** -- Iterates surface vertices checking 3 edges
 *       per cell for crossings. Each crossing produces a quad = 2 triangles.
 *
 *   3B-3E. **Triangle generation** -- Second pass writes triangle connectivity,
 *       face labels, and runs TupleTransfer. All output is buffered and flushed
 *       in bulk via copyFromBuffer().
 *
 *   3F. **Winding repair** (optional) -- Same as SurfaceNetsDirect.
 *
 * Memory: bounded Z-slices, record-cache pages, and fixed output chunks.
 *
 * @see SurfaceNetsDirect for the in-core reference implementation
 */
class SIMPLNXCORE_EXPORT SurfaceNetsScanline
{
public:
  /**
   * @brief Constructs the OOC-optimized algorithm.
   * @param dataStructure The DataStructure containing all input/output objects
   * @param mesgHandler Callback for progress and status messages
   * @param shouldCancel Atomic flag checked periodically for user cancellation
   * @param inputValues Pointer to the parameter struct (must outlive this object)
   */
  SurfaceNetsScanline(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel, const SurfaceNetsInputValues* inputValues);
  ~SurfaceNetsScanline() noexcept;

  SurfaceNetsScanline(const SurfaceNetsScanline&) = delete;
  SurfaceNetsScanline(SurfaceNetsScanline&&) noexcept = delete;
  SurfaceNetsScanline& operator=(const SurfaceNetsScanline&) = delete;
  SurfaceNetsScanline& operator=(SurfaceNetsScanline&&) noexcept = delete;

  /**
   * @brief Executes the full OOC Surface Nets pipeline: cell classification,
   * optional smoothing, vertex transformation, triangle generation, and optional
   * winding repair.
   * @return Result<> indicating success or an error from winding repair
   */
  Result<> operator()();

  /**
   * @brief Per-vertex information stored only for surface cells.
   *
   * This struct replaces the full MMCellMap::Cell for surface cells. It stores
   * the padded grid coordinates and the MMCellFlag that encodes which edges
   * and faces of the cell are crossed by the feature boundary.
   */
  struct SurfaceCellRecord
  {
    MMCellFlag Flag;
    uint64 VertexId = std::numeric_limits<uint64>::max();
    int32 Label = 0;
    std::array<float32, 3> LocalPosition = {0.5f, 0.5f, 0.5f};
    int8 NodeType = 0;
  };
  static_assert(std::is_trivially_copyable_v<SurfaceCellRecord>);

private:
  DataStructure& m_DataStructure;                        ///< Reference to the active DataStructure
  const SurfaceNetsInputValues* m_InputValues = nullptr; ///< User parameters and created array paths
  const std::atomic_bool& m_ShouldCancel;                ///< User cancellation flag
  const IFilter::MessageHandler& m_MessageHandler;       ///< Progress message callback

  std::unique_ptr<ITemporaryRecordStore> m_SurfaceCells;
  std::unique_ptr<BoundedRecordPageCache<SurfaceCellRecord>> m_SurfaceCellCache;
};

} // namespace nx::core
