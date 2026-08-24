#include "SegmentFeatures.hpp"

#include "simplnx/DataStructure/AbstractDataStore.hpp"
#include "simplnx/DataStructure/Geometry/IGridGeometry.hpp"
#include "simplnx/Utilities/AlgorithmDispatch.hpp"
#include "simplnx/Utilities/BoundedRecordPageCache.hpp"
#include "simplnx/Utilities/ClusteringUtilities.hpp"
#include "simplnx/Utilities/DataStoreUtilities.hpp"
#include "simplnx/Utilities/ExternalEquivalence.hpp"
#include "simplnx/Utilities/InMemoryTemporaryRecordStore.hpp"
#include "simplnx/Utilities/MessageHelper.hpp"
#include "simplnx/Utilities/UnionFind.hpp"

#include <algorithm>
#include <limits>
#include <memory>
#include <nonstd/span.hpp>
#include <vector>

using namespace nx::core;

namespace
{
constexpr uint64 k_RecordsPerPage = 4096;
constexpr usize k_MaxCachedPages = 16;

/**
 * @brief Creates fixed-record CCL scratch with a deliberately explicit fallback policy.
 * Genuine OOC input passes false so equivalence tables cannot silently become
 * volume-scale RAM allocations; forced OOC tests may permit the resident provider.
 */
Result<std::unique_ptr<ITemporaryRecordStore>> CreateTemporaryRecordStore(uint64 recordSize, uint64 recordCount, bool allowInMemoryFallback)
{
  TemporaryRecordStoreConfig config;
  config.recordSize = recordSize;
  config.maxRecordsPerBatch = k_RecordsPerPage;
  config.initialRecordCount = recordCount;

  auto result = DataStoreUtilities::GetIOCollection().createTemporaryRecordStore(config);
  if(result.invalid() && allowInMemoryFallback)
  {
    auto fallbackResult = InMemoryTemporaryRecordStore::Create(config);
    if(fallbackResult.invalid())
    {
      return ConvertInvalidResult<std::unique_ptr<ITemporaryRecordStore>>(std::move(fallbackResult));
    }
    result = {std::unique_ptr<ITemporaryRecordStore>(std::move(fallbackResult.value()))};
  }
  if(result.valid() && result.value() == nullptr)
  {
    return MakeErrorResult<std::unique_ptr<ITemporaryRecordStore>>(-87010, "SegmentFeatures temporary-record provider returned a null store.");
  }
  return result;
}

/**
 * @brief Storage-neutral union/find and final-label table for scanline CCL.
 *
 * Resident execution delegates to UnionFind and a vector. OOC execution stores
 * both provisional equivalences and dense final labels behind bounded caches,
 * preventing worst-case label state from scaling resident memory with volume.
 */
class LabelEquivalence
{
public:
  /** @brief Constructs the resident or external backend for labels [0, maximumLabel]. */
  static Result<std::unique_ptr<LabelEquivalence>> Create(bool useExternalStorage, uint64 maximumLabel, bool allowInMemoryFallback)
  {
    try
    {
      auto result = std::unique_ptr<LabelEquivalence>(new LabelEquivalence());
      result->m_UseExternalStorage = useExternalStorage;
      if(!useExternalStorage)
      {
        return {std::move(result)};
      }
      if(maximumLabel == std::numeric_limits<uint64>::max())
      {
        return MakeErrorResult<std::unique_ptr<LabelEquivalence>>(-87011, "SegmentFeatures cannot create external equivalences for this image size.");
      }

      auto equivalenceStoreResult = CreateTemporaryRecordStore(sizeof(ExternalEquivalence::Node), maximumLabel + 1, allowInMemoryFallback);
      if(equivalenceStoreResult.invalid())
      {
        return ConvertInvalidResult<std::unique_ptr<LabelEquivalence>>(std::move(equivalenceStoreResult));
      }
      auto equivalenceResult = ExternalEquivalence::Create(std::move(equivalenceStoreResult.value()), k_RecordsPerPage, k_MaxCachedPages);
      if(equivalenceResult.invalid())
      {
        return ConvertInvalidResult<std::unique_ptr<LabelEquivalence>>(std::move(equivalenceResult));
      }
      result->m_ExternalEquivalence = std::move(equivalenceResult.value());

      auto finalLabelStoreResult = CreateTemporaryRecordStore(sizeof(int32), maximumLabel + 1, allowInMemoryFallback);
      if(finalLabelStoreResult.invalid())
      {
        return ConvertInvalidResult<std::unique_ptr<LabelEquivalence>>(std::move(finalLabelStoreResult));
      }
      result->m_FinalLabelStore = std::move(finalLabelStoreResult.value());
      result->m_FinalLabelCache = std::make_unique<BoundedRecordPageCache<int32>>(*result->m_FinalLabelStore, k_RecordsPerPage, k_MaxCachedPages);
      return {std::move(result)};
    } catch(const std::bad_alloc&)
    {
      return MakeErrorResult<std::unique_ptr<LabelEquivalence>>(-87012, "SegmentFeatures could not allocate its bounded equivalence cache.");
    }
  }

  /** @brief Returns the canonical root of a provisional label. */
  Result<uint64> find(uint64 label, const std::atomic_bool& shouldCancel)
  {
    if(m_UseExternalStorage)
    {
      return m_ExternalEquivalence->find(label, shouldCancel);
    }
    return {static_cast<uint64>(m_DirectEquivalence.find(static_cast<int64>(label)))};
  }

  /** @brief Lazily materializes a label in the selected union/find backend. */
  Result<> initialize(uint64 label, const std::atomic_bool& shouldCancel)
  {
    auto result = find(label, shouldCancel);
    if(result.invalid())
    {
      return ConvertResult(std::move(result));
    }
    return {};
  }

  /** @brief Records that two provisional scanline labels represent one feature. */
  Result<> unite(uint64 left, uint64 right, const std::atomic_bool& shouldCancel)
  {
    if(m_UseExternalStorage)
    {
      return m_ExternalEquivalence->unite(left, right, shouldCancel);
    }
    m_DirectEquivalence.unite(static_cast<int64>(left), static_cast<int64>(right));
    return {};
  }

  /** @brief Allocates/normalizes state needed for the final dense relabeling pass. */
  Result<> prepareFinalLabels(uint64 nextLabel)
  {
    if(!m_UseExternalStorage)
    {
      try
      {
        m_DirectFinalLabels.assign(static_cast<usize>(nextLabel), 0);
      } catch(const std::bad_alloc&)
      {
        return MakeErrorResult(-87013, "SegmentFeatures could not allocate its in-core final-label table.");
      }
      m_DirectEquivalence.flatten();
    }
    return {};
  }

  /**
   * @brief Maps one provisional label's root to a stable dense Int32 feature ID.
   * The mapping is cached so later voxels in the same component avoid another root walk.
   */
  Result<int32> resolveFinalLabel(uint64 label, int32& finalFeatureCount, const std::atomic_bool& shouldCancel)
  {
    auto cachedResult = finalLabel(label, shouldCancel);
    if(cachedResult.invalid())
    {
      return cachedResult;
    }
    if(cachedResult.value() != 0)
    {
      return cachedResult;
    }

    auto rootResult = find(label, shouldCancel);
    if(rootResult.invalid())
    {
      return ConvertInvalidResult<int32>(std::move(rootResult));
    }
    const uint64 root = rootResult.value();
    auto rootFinalResult = finalLabel(root, shouldCancel);
    if(rootFinalResult.invalid())
    {
      return rootFinalResult;
    }
    int32 rootFinal = rootFinalResult.value();
    if(rootFinal == 0)
    {
      if(finalFeatureCount == std::numeric_limits<int32>::max())
      {
        return MakeErrorResult<int32>(-87014, "SegmentFeatures exceeded the Int32 feature-ID capacity.");
      }
      rootFinal = ++finalFeatureCount;
      auto writeRootResult = setFinalLabel(root, rootFinal, shouldCancel);
      if(writeRootResult.invalid())
      {
        return ConvertResultTo<int32>(std::move(writeRootResult), int32{});
      }
    }
    if(root != label)
    {
      auto writeLabelResult = setFinalLabel(label, rootFinal, shouldCancel);
      if(writeLabelResult.invalid())
      {
        return ConvertResultTo<int32>(std::move(writeLabelResult), int32{});
      }
    }
    return {rootFinal};
  }

  /** @brief Commits external equivalence and final-label pages between CCL phases. */
  Result<> flush(const std::atomic_bool& shouldCancel)
  {
    if(!m_UseExternalStorage)
    {
      return {};
    }
    auto equivalenceResult = m_ExternalEquivalence->flush(shouldCancel);
    if(equivalenceResult.invalid())
    {
      return equivalenceResult;
    }
    return m_FinalLabelCache->flush(shouldCancel);
  }

private:
  /** @brief Reads a previously assigned dense label, or zero when unresolved. */
  Result<int32> finalLabel(uint64 label, const std::atomic_bool& shouldCancel)
  {
    if(m_UseExternalStorage)
    {
      return m_FinalLabelCache->read(label, shouldCancel);
    }
    return {m_DirectFinalLabels[static_cast<usize>(label)]};
  }

  /** @brief Caches the dense label for a provisional label or root. */
  Result<> setFinalLabel(uint64 label, int32 finalLabel, const std::atomic_bool& shouldCancel)
  {
    if(m_UseExternalStorage)
    {
      return m_FinalLabelCache->write(label, finalLabel, shouldCancel);
    }
    m_DirectFinalLabels[static_cast<usize>(label)] = finalLabel;
    return {};
  }

  LabelEquivalence() = default;

  bool m_UseExternalStorage = false;
  UnionFind m_DirectEquivalence;
  std::vector<int32> m_DirectFinalLabels;
  std::unique_ptr<ExternalEquivalence> m_ExternalEquivalence;
  std::unique_ptr<ITemporaryRecordStore> m_FinalLabelStore;
  std::unique_ptr<BoundedRecordPageCache<int32>> m_FinalLabelCache;
};
} // namespace

// -----------------------------------------------------------------------------
SegmentFeatures::SegmentFeatures(DataStructure& dataStructure, const std::atomic_bool& shouldCancel, const IFilter::MessageHandler& mesgHandler)
: m_DataStructure(dataStructure)
, m_ShouldCancel(shouldCancel)
, m_MessageHelper(mesgHandler)
{
}

// -----------------------------------------------------------------------------
SegmentFeatures::~SegmentFeatures() = default;

// =============================================================================
// Connected Component Labeling (CCL) Segmentation
// =============================================================================
//
// This method segments voxels into features using a scanline-based
// connected-component labeling algorithm. It processes voxels in strict Z-Y-X
// scanline order, which yields sequential data-store access patterns and keeps
// the working set bounded to a rolling two-slice window regardless of volume
// size.
//
// The algorithm has three phases:
//
// Phase 1 (Forward CCL):
//   Scan voxels in Z-Y-X order. For each valid voxel, examine only its
//   "backward" neighbors — those already visited earlier in scanline order.
//   If a backward neighbor has a label and is similar (per areNeighborsSimilar),
//   adopt that label. If multiple distinct labels are found among backward
//   neighbors, unite them in a Union-Find structure. If no backward neighbor
//   matches, assign a fresh provisional label. Labels are written to both an
//   rolling two-slice buffer (for fast neighbor lookups) and to the featureIds
//   store (for persistence).
//
// Phase 1b (Periodic boundary merge):
//   If periodic boundaries are enabled, Phase 1 cannot detect connections
//   that wrap around the volume (the wrapped neighbor has a higher linear
//   index and hasn't been visited yet). This phase reads back provisional
//   labels and unites similar voxels on opposite boundary faces.
//
// Phase 2 (Resolution + Relabeling):
//   Resolve each provisional label through either the in-core Union-Find table
//   or the bounded external equivalence store, then map roots to contiguous
//   final feature IDs. The final-label map is likewise resident for Direct and
//   disk-backed/page-cached for OOC. Write final IDs back one slice at a time.
// =============================================================================
Result<> SegmentFeatures::executeCCL(IGridGeometry* gridGeom, AbstractDataStore<int32>& featureIdsStore, bool usesOutOfCoreInput)
{
  const SizeVec3 udims = gridGeom->getDimensions();
  // getDimensions() returns [X, Y, Z]
  const int64 dimX = static_cast<int64>(udims[0]);
  const int64 dimY = static_cast<int64>(udims[1]);
  const int64 dimZ = static_cast<int64>(udims[2]);
  const usize totalVoxels = static_cast<usize>(dimX) * static_cast<usize>(dimY) * static_cast<usize>(dimZ);

  const int64 sliceStride = dimX * dimY;

  const bool useFaceOnly = (m_NeighborScheme == NeighborScheme::Face);
  bool hasNonContiguousFeature = false;

  const bool usesOutOfCoreStore = usesOutOfCoreInput || featureIdsStore.getStoreType() == IDataStore::StoreType::OutOfCore;
  const bool useExternalEquivalence = !ForceInCoreAlgorithm() && (usesOutOfCoreStore || ForceOocAlgorithm());
  RecordAlgorithmPathExecution(useExternalEquivalence ? AlgorithmPath::OutOfCore : AlgorithmPath::InCore, usesOutOfCoreStore);

  auto equivalenceResult = LabelEquivalence::Create(useExternalEquivalence, static_cast<uint64>(totalVoxels), !usesOutOfCoreStore);
  if(equivalenceResult.invalid())
  {
    return ConvertResult(std::move(equivalenceResult));
  }
  auto equivalences = std::move(equivalenceResult.value());
  int32 nextLabel = 1; // Provisional labels start at 1

  // Rolling 2-slice buffer for backward neighbor label lookups.
  //
  // Why 2 slices is sufficient:
  //   In Z-Y-X scanline order, a voxel at (ix, iy, iz) has backward neighbors
  //   only in the current Z-slice (iz) or the immediately previous Z-slice
  //   (iz-1). No backward neighbor can ever be in Z-slice (iz-2) or earlier,
  //   because all 13 backward neighbor offsets have dz in {-1, 0}. Therefore,
  //   keeping just 2 slices in memory — the current and the previous — is
  //   enough for all backward neighbor label reads.
  //
  // This design uses O(dimX * dimY) memory instead of O(dimX * dimY * dimZ),
  // enabling processing of datasets much larger than available RAM.
  //
  // Buffer layout: Z-slice (iz % 2) occupies indices
  //   [sliceOffset .. sliceOffset + sliceStride), where
  //   sliceOffset = (iz % 2) * sliceSize.
  const usize sliceSize = static_cast<usize>(sliceStride);
  std::vector<int32> labelBuffer(2 * sliceSize, 0);

  // =========================================================================
  // Phase 1: Forward CCL - assign provisional labels using backward neighbors
  // =========================================================================
  // Slice buffer for batch-writing featureIds to the data store.
  // Phase 1 only writes to featureIdsStore (never reads), so we accumulate
  // writes per Z-slice and flush once via copyFromBuffer at slice end.
  std::vector<int32> featureIdsSlice(sliceSize, 0);

  for(int64 iz = 0; iz < dimZ; iz++)
  {
    if(m_ShouldCancel)
    {
      return {};
    }

    auto prepareResult = prepareForSlice(iz, dimX, dimY, dimZ);
    if(prepareResult.invalid())
    {
      return prepareResult;
    }

    // Zero the featureIds slice buffer for this Z-slice (prevents stale
    // labels from the previous slice being written back for invalid voxels)
    std::fill(featureIdsSlice.begin(), featureIdsSlice.end(), 0);

    // Clear the current slice's portion of the rolling buffer
    const usize currentSliceOffset = static_cast<usize>(iz % 2) * sliceSize;
    std::fill(labelBuffer.begin() + currentSliceOffset, labelBuffer.begin() + currentSliceOffset + sliceSize, 0);

    for(int64 iy = 0; iy < dimY; iy++)
    {
      for(int64 ix = 0; ix < dimX; ix++)
      {
        const int64 index = iz * sliceStride + iy * dimX + ix;
        const usize bufIdx = currentSliceOffset + static_cast<usize>(iy * dimX + ix);

        // Skip voxels that are not valid
        if(!isValidVoxel(index))
        {
          continue;
        }

        // Check backward neighbors for existing labels.
        // "Backward" neighbors are those with a smaller linear index — i.e.,
        // already processed earlier in Z-Y-X scanline order. In 3D, these are
        // neighbors with dz < 0, or dz == 0 && dy < 0, or dz == 0 && dy == 0
        // && dx < 0. Forward neighbors (higher linear index) are not yet
        // labeled and cannot be consulted.
        //
        // Neighbor labels are read from the rolling buffer (direct memory
        // access, O(1)) rather than from the OOC featureIds store, avoiding
        // chunk loads for every neighbor lookup.
        int32 assignedLabel = 0;
        const usize prevSliceOffset = static_cast<usize>((iz + 1) % 2) * sliceSize;

        if(useFaceOnly)
        {
          // Face connectivity: exactly 3 backward neighbors exist:
          //   -X (dx=-1): one column to the left in the same row/slice
          //   -Y (dy=-1): one row earlier in the same slice
          //   -Z (dz=-1): same (x,y) position in the previous slice
          // The 3 forward neighbors (+X, +Y, +Z) have not been labeled yet
          // and are skipped.

          // Check -X neighbor (same Z-slice, same buffer region)
          if(ix > 0)
          {
            const int64 neighIdx = index - 1;
            int32 neighLabel = labelBuffer[bufIdx - 1];
            if(neighLabel > 0 && areNeighborsSimilar(index, neighIdx))
            {
              if(assignedLabel == 0)
              {
                assignedLabel = neighLabel;
              }
              else if(assignedLabel != neighLabel)
              {
                auto uniteResult = equivalences->unite(static_cast<uint64>(assignedLabel), static_cast<uint64>(neighLabel), m_ShouldCancel);
                if(uniteResult.invalid())
                {
                  return uniteResult;
                }
              }
            }
          }
          // Check -Y neighbor (same Z-slice, same buffer region)
          if(iy > 0)
          {
            const int64 neighIdx = index - dimX;
            int32 neighLabel = labelBuffer[currentSliceOffset + static_cast<usize>((iy - 1) * dimX + ix)];
            if(neighLabel > 0 && areNeighborsSimilar(index, neighIdx))
            {
              if(assignedLabel == 0)
              {
                assignedLabel = neighLabel;
              }
              else if(assignedLabel != neighLabel)
              {
                auto uniteResult = equivalences->unite(static_cast<uint64>(assignedLabel), static_cast<uint64>(neighLabel), m_ShouldCancel);
                if(uniteResult.invalid())
                {
                  return uniteResult;
                }
              }
            }
          }
          // Check -Z neighbor (previous Z-slice, other buffer region)
          if(iz > 0)
          {
            const int64 neighIdx = index - sliceStride;
            int32 neighLabel = labelBuffer[prevSliceOffset + static_cast<usize>(iy * dimX + ix)];
            if(neighLabel > 0 && areNeighborsSimilar(index, neighIdx))
            {
              if(assignedLabel == 0)
              {
                assignedLabel = neighLabel;
              }
              else if(assignedLabel != neighLabel)
              {
                auto uniteResult = equivalences->unite(static_cast<uint64>(assignedLabel), static_cast<uint64>(neighLabel), m_ShouldCancel);
                if(uniteResult.invalid())
                {
                  return uniteResult;
                }
              }
            }
          }
        }
        else
        {
          // FaceEdgeVertex connectivity: 13 backward neighbors out of 26 total.
          //
          // A 3x3x3 neighborhood has 26 neighbors (excluding self). Exactly
          // half (13) have a smaller linear index in Z-Y-X order and are thus
          // "backward." These are enumerated by iterating:
          //   dz in {-1, 0}:
          //     dz=-1: all 9 neighbors in the previous Z-slice (any dx, dy)
          //     dz= 0: only neighbors with dy < 0 (3 neighbors), or
          //            dy == 0 && dx == -1 (1 neighbor) => 4 total
          //   Total: 9 + 4 = 13 backward neighbors
          //
          // The loop bounds below encode this enumeration efficiently:
          //   - dz ranges [-1, 0]
          //   - dy ranges [-1, +1] when dz<0, or [-1, 0] when dz==0
          //   - dx ranges [-1, +1] when dz<0 or dy<0, or [-1, -1] when dz==0 && dy==0
          for(int64 dz = -1; dz <= 0; ++dz)
          {
            const int64 nz = iz + dz;
            if(nz < 0 || nz >= dimZ)
            {
              continue;
            }

            const usize neighSliceOffset = (dz < 0) ? prevSliceOffset : currentSliceOffset;

            const int64 dyStart = -1;
            const int64 dyEnd = (dz < 0) ? 1 : 0;

            for(int64 dy = dyStart; dy <= dyEnd; ++dy)
            {
              const int64 ny = iy + dy;
              if(ny < 0 || ny >= dimY)
              {
                continue;
              }

              int64 dxStart;
              int64 dxEnd;
              if(dz < 0)
              {
                dxStart = -1;
                dxEnd = 1;
              }
              else if(dy < 0)
              {
                dxStart = -1;
                dxEnd = 1;
              }
              else
              {
                dxStart = -1;
                dxEnd = -1;
              }

              for(int64 dx = dxStart; dx <= dxEnd; ++dx)
              {
                const int64 nx = ix + dx;
                if(nx < 0 || nx >= dimX)
                {
                  continue;
                }
                if(dx == 0 && dy == 0 && dz == 0)
                {
                  continue;
                }

                const int64 neighIdx = nz * sliceStride + ny * dimX + nx;
                int32 neighLabel = labelBuffer[neighSliceOffset + static_cast<usize>(ny * dimX + nx)];
                if(neighLabel > 0 && areNeighborsSimilar(index, neighIdx))
                {
                  if(assignedLabel == 0)
                  {
                    assignedLabel = neighLabel;
                  }
                  else if(assignedLabel != neighLabel)
                  {
                    auto uniteResult = equivalences->unite(static_cast<uint64>(assignedLabel), static_cast<uint64>(neighLabel), m_ShouldCancel);
                    if(uniteResult.invalid())
                    {
                      return uniteResult;
                    }
                  }
                }
              }
            }
          }
        }

        // If no matching backward neighbor, assign new provisional label
        if(assignedLabel == 0)
        {
          if(nextLabel == std::numeric_limits<int32>::max())
          {
            return MakeErrorResult(-87014, "SegmentFeatures exceeded the Int32 provisional-label capacity.");
          }
          assignedLabel = nextLabel++;
          auto initializeResult = equivalences->initialize(static_cast<uint64>(assignedLabel), m_ShouldCancel);
          if(initializeResult.invalid())
          {
            return initializeResult;
          }
        }

        // Write label to rolling buffer (for neighbor reads) and slice buffer (for batch write)
        labelBuffer[bufIdx] = assignedLabel;
        const usize inSlice = static_cast<usize>(iy * dimX + ix);
        featureIdsSlice[inSlice] = assignedLabel;
      }
    }

    // Batch-write this Z-slice's featureIds to the data store
    auto writeResult = featureIdsStore.copyFromBuffer(static_cast<usize>(iz) * sliceSize, nonstd::span<const int32>(featureIdsSlice.data(), sliceSize));
    if(writeResult.invalid())
    {
      return writeResult;
    }
  }

  if(m_ShouldCancel)
  {
    return {};
  }

  // =========================================================================
  // Phase 1b: Periodic boundary merge (O(slice) memory)
  // =========================================================================
  // The forward CCL pass cannot detect connections that wrap around periodic
  // boundaries because the wrapped neighbor has a higher linear index and
  // has not been processed yet when the boundary voxel is visited. This
  // phase reads back provisional labels from featureIdsStore slice by slice
  // and unites labels of similar voxels on opposite boundary faces.
  //
  // Memory strategy: instead of reading the entire volume into memory, we
  // read featureIds one or two Z-slices at a time and call prepareForSlice()
  // so areNeighborsSimilar() reads from the subclass's fast slice buffers.
  // This keeps memory at O(dimX * dimY) rather than O(dimX * dimY * dimZ).
  if(m_IsPeriodic)
  {
    // Reusable slice-sized buffers for reading featureIds from the store
    std::vector<int32> featureIdsSliceCur(sliceSize, 0);

    if(useFaceOnly)
    {
      // Face connectivity: each axis is handled independently because face
      // neighbors only connect along a single axis. For each axis, we
      // iterate over the 2D face and compare each voxel at the low boundary
      // (e.g. ix=0) with its counterpart at the high boundary (e.g.
      // ix=dimX-1). These are the wrapped neighbor pairs that Phase 1 could
      // not process because the wrapped neighbor had not yet been labeled.

      // X-axis: unite voxels at ix=0 with ix=dimX-1
      // Both voxels are in the same Z-slice, so one slice suffices.
      if(dimX > 1)
      {
        for(int64 iz = 0; iz < dimZ; iz++)
        {
          auto readResult = featureIdsStore.copyIntoBuffer(static_cast<usize>(iz) * sliceSize, nonstd::span<int32>(featureIdsSliceCur.data(), sliceSize));
          if(readResult.invalid())
          {
            return readResult;
          }
          auto prepareResult = prepareForSlice(iz, dimX, dimY, dimZ);
          if(prepareResult.invalid())
          {
            return prepareResult;
          }

          for(int64 iy = 0; iy < dimY; iy++)
          {
            const int64 idxA = iz * sliceStride + iy * dimX;
            const int64 idxB = iz * sliceStride + iy * dimX + (dimX - 1);
            const int32 labelA = featureIdsSliceCur[static_cast<usize>(iy * dimX)];
            const int32 labelB = featureIdsSliceCur[static_cast<usize>(iy * dimX + (dimX - 1))];
            if(labelA > 0 && labelB > 0 && areNeighborsSimilar(idxA, idxB))
            {
              auto uniteResult = equivalences->unite(static_cast<uint64>(labelA), static_cast<uint64>(labelB), m_ShouldCancel);
              if(uniteResult.invalid())
              {
                return uniteResult;
              }
              hasNonContiguousFeature = true;
            }
          }
        }
      }

      // Y-axis: unite voxels at iy=0 with iy=dimY-1
      // Both voxels are in the same Z-slice, so one slice suffices.
      if(dimY > 1)
      {
        for(int64 iz = 0; iz < dimZ; iz++)
        {
          auto readResult = featureIdsStore.copyIntoBuffer(static_cast<usize>(iz) * sliceSize, nonstd::span<int32>(featureIdsSliceCur.data(), sliceSize));
          if(readResult.invalid())
          {
            return readResult;
          }
          auto prepareResult = prepareForSlice(iz, dimX, dimY, dimZ);
          if(prepareResult.invalid())
          {
            return prepareResult;
          }

          for(int64 ix = 0; ix < dimX; ix++)
          {
            const int64 idxA = iz * sliceStride + ix;
            const int64 idxB = iz * sliceStride + (dimY - 1) * dimX + ix;
            const int32 labelA = featureIdsSliceCur[static_cast<usize>(ix)];
            const int32 labelB = featureIdsSliceCur[static_cast<usize>((dimY - 1) * dimX + ix)];
            if(labelA > 0 && labelB > 0 && areNeighborsSimilar(idxA, idxB))
            {
              auto uniteResult = equivalences->unite(static_cast<uint64>(labelA), static_cast<uint64>(labelB), m_ShouldCancel);
              if(uniteResult.invalid())
              {
                return uniteResult;
              }
              hasNonContiguousFeature = true;
            }
          }
        }
      }

      // Z-axis: unite voxels at iz=0 with iz=dimZ-1
      // These are in different Z-slices, so we read both into separate
      // buffers. We call prepareForSlice for both so the 2-slot rolling
      // buffer holds both slices for areNeighborsSimilar().
      if(dimZ > 1)
      {
        std::vector<int32> featureIdsSliceOther(sliceSize, 0);
        auto firstReadResult = featureIdsStore.copyIntoBuffer(0, nonstd::span<int32>(featureIdsSliceCur.data(), sliceSize));
        if(firstReadResult.invalid())
        {
          return firstReadResult;
        }
        auto lastReadResult = featureIdsStore.copyIntoBuffer(static_cast<usize>(dimZ - 1) * sliceSize, nonstd::span<int32>(featureIdsSliceOther.data(), sliceSize));
        if(lastReadResult.invalid())
        {
          return lastReadResult;
        }

        // Load both slices into the subclass's 2-slot rolling buffer so
        // areNeighborsSimilar() can compare voxels across these two slices.
        auto firstPrepareResult = prepareForSlice(0, dimX, dimY, dimZ);
        if(firstPrepareResult.invalid())
        {
          return firstPrepareResult;
        }
        auto lastPrepareResult = prepareForSlice(dimZ - 1, dimX, dimY, dimZ);
        if(lastPrepareResult.invalid())
        {
          return lastPrepareResult;
        }

        for(int64 iy = 0; iy < dimY; iy++)
        {
          for(int64 ix = 0; ix < dimX; ix++)
          {
            const usize inSlice = static_cast<usize>(iy * dimX + ix);
            const int64 idxA = iy * dimX + ix;
            const int64 idxB = (dimZ - 1) * sliceStride + iy * dimX + ix;
            const int32 labelA = featureIdsSliceCur[inSlice];
            const int32 labelB = featureIdsSliceOther[inSlice];
            if(labelA > 0 && labelB > 0 && areNeighborsSimilar(idxA, idxB))
            {
              auto uniteResult = equivalences->unite(static_cast<uint64>(labelA), static_cast<uint64>(labelB), m_ShouldCancel);
              if(uniteResult.invalid())
              {
                return uniteResult;
              }
              hasNonContiguousFeature = true;
            }
          }
        }
      }
    }
    else
    {
      // FaceEdgeVertex connectivity: check all 26-neighbor pairs that wrap
      // across periodic boundaries. Unlike face-only mode, edge and vertex
      // neighbors can wrap across two or even three axes simultaneously
      // (e.g. a corner voxel's diagonal neighbor wraps in X, Y, and Z).
      //
      // Memory strategy: iterate Z-slices one at a time. For each slice,
      // read its featureIds into featureIdsSliceCur. Only Z-boundary slices
      // (iz=0 and iz=dimZ-1) can have neighbors that wrap in Z; all other
      // slices only have X/Y wrapping where both voxels are in the same
      // Z-slice. For Z-boundary slices, we also read the wrapped partner
      // slice (dimZ-1 or 0) into featureIdsSliceWrapped.
      //
      // The subclass's prepareForSlice 2-slot rolling buffer naturally
      // holds both the current slice and the wrapped partner, so
      // areNeighborsSimilar() works correctly.
      //
      // Pre-read featureIds for slices 0 and dimZ-1 into persistent buffers
      // so they are available when either Z-boundary slice is processed.
      std::vector<int32> featureIdsSlice0(sliceSize, 0);
      std::vector<int32> featureIdsSliceLast(sliceSize, 0);
      auto firstReadResult = featureIdsStore.copyIntoBuffer(0, nonstd::span<int32>(featureIdsSlice0.data(), sliceSize));
      if(firstReadResult.invalid())
      {
        return firstReadResult;
      }
      if(dimZ > 1)
      {
        auto lastReadResult = featureIdsStore.copyIntoBuffer(static_cast<usize>(dimZ - 1) * sliceSize, nonstd::span<int32>(featureIdsSliceLast.data(), sliceSize));
        if(lastReadResult.invalid())
        {
          return lastReadResult;
        }
      }

      for(int64 iz = 0; iz < dimZ; iz++)
      {
        if(m_ShouldCancel)
        {
          return {};
        }

        // Use the pre-read buffer for slices 0 and dimZ-1; read fresh for others
        if(iz == 0)
        {
          std::copy(featureIdsSlice0.begin(), featureIdsSlice0.end(), featureIdsSliceCur.begin());
        }
        else if(iz == dimZ - 1)
        {
          std::copy(featureIdsSliceLast.begin(), featureIdsSliceLast.end(), featureIdsSliceCur.begin());
        }
        else
        {
          auto readResult = featureIdsStore.copyIntoBuffer(static_cast<usize>(iz) * sliceSize, nonstd::span<int32>(featureIdsSliceCur.data(), sliceSize));
          if(readResult.invalid())
          {
            return readResult;
          }
        }

        // Load input data for the current slice
        auto prepareResult = prepareForSlice(iz, dimX, dimY, dimZ);
        if(prepareResult.invalid())
        {
          return prepareResult;
        }

        // Determine if this is a Z-boundary slice and identify the wrapped
        // partner. Only iz=0 can wrap to dimZ-1, and only iz=dimZ-1 can
        // wrap to 0. Interior slices have no Z-wrapped neighbors.
        int64 wrappedPartnerZ = -1; // sentinel: no Z-wrapped partner
        const int32* wrappedSlicePtr = nullptr;
        if(iz == 0 && dimZ > 1)
        {
          wrappedPartnerZ = dimZ - 1;
          wrappedSlicePtr = featureIdsSliceLast.data();
          // Load the wrapped partner into the 2-slot buffer so
          // areNeighborsSimilar() can access both slices
          auto wrappedPrepareResult = prepareForSlice(wrappedPartnerZ, dimX, dimY, dimZ);
          if(wrappedPrepareResult.invalid())
          {
            return wrappedPrepareResult;
          }
        }
        else if(iz == dimZ - 1 && dimZ > 1)
        {
          wrappedPartnerZ = 0;
          wrappedSlicePtr = featureIdsSlice0.data();
          auto wrappedPrepareResult = prepareForSlice(wrappedPartnerZ, dimX, dimY, dimZ);
          if(wrappedPrepareResult.invalid())
          {
            return wrappedPrepareResult;
          }
        }

        for(int64 iy = 0; iy < dimY; iy++)
        {
          for(int64 ix = 0; ix < dimX; ix++)
          {
            // Only boundary voxels can have neighbors that wrap around
            const bool onBoundary = (ix == 0 || ix == dimX - 1 || iy == 0 || iy == dimY - 1 || iz == 0 || iz == dimZ - 1);
            if(!onBoundary)
            {
              continue;
            }

            const usize inSlice = static_cast<usize>(iy * dimX + ix);
            const int64 index = iz * sliceStride + iy * dimX + ix;
            const int32 labelCurrent = featureIdsSliceCur[inSlice];
            if(labelCurrent <= 0)
            {
              continue;
            }

            for(int64 dz = -1; dz <= 1; ++dz)
            {
              int64 nz = iz + dz;
              bool wrappedZ = false;
              if(nz < 0)
              {
                nz += dimZ;
                wrappedZ = true;
              }
              else if(nz >= dimZ)
              {
                nz -= dimZ;
                wrappedZ = true;
              }

              for(int64 dy = -1; dy <= 1; ++dy)
              {
                int64 ny = iy + dy;
                bool wrappedY = false;
                if(ny < 0)
                {
                  ny += dimY;
                  wrappedY = true;
                }
                else if(ny >= dimY)
                {
                  ny -= dimY;
                  wrappedY = true;
                }

                for(int64 dx = -1; dx <= 1; ++dx)
                {
                  if(dx == 0 && dy == 0 && dz == 0)
                  {
                    continue;
                  }

                  int64 nx = ix + dx;
                  bool wrappedX = false;
                  if(nx < 0)
                  {
                    nx += dimX;
                    wrappedX = true;
                  }
                  else if(nx >= dimX)
                  {
                    nx -= dimX;
                    wrappedX = true;
                  }

                  // Only process pairs that actually wrap around at least one
                  // axis. Non-wrapped pairs were already handled in Phase 1.
                  if(!wrappedX && !wrappedY && !wrappedZ)
                  {
                    continue;
                  }

                  const int64 neighIdx = nz * sliceStride + ny * dimX + nx;
                  // Deduplication: only process the pair where neighIdx > index.
                  // This ensures each (voxelA, voxelB) pair is united exactly
                  // once, since unite() is symmetric.
                  if(neighIdx <= index)
                  {
                    continue;
                  }

                  // Look up the neighbor's label from the appropriate slice buffer.
                  // If the neighbor is in the same Z-slice, use featureIdsSliceCur.
                  // If the neighbor is in the wrapped partner Z-slice, use wrappedSlicePtr.
                  int32 labelNeigh = 0;
                  if(nz == iz)
                  {
                    labelNeigh = featureIdsSliceCur[static_cast<usize>(ny * dimX + nx)];
                  }
                  else if(nz == wrappedPartnerZ && wrappedSlicePtr != nullptr)
                  {
                    labelNeigh = wrappedSlicePtr[static_cast<usize>(ny * dimX + nx)];
                  }

                  if(labelNeigh > 0 && areNeighborsSimilar(index, neighIdx))
                  {
                    auto uniteResult = equivalences->unite(static_cast<uint64>(labelCurrent), static_cast<uint64>(labelNeigh), m_ShouldCancel);
                    if(uniteResult.invalid())
                    {
                      return uniteResult;
                    }
                    hasNonContiguousFeature = true;
                  }
                }
              }
            }
          }
        }
      }
    }
  }

  if(hasNonContiguousFeature)
  {
    m_MessageHelper.sendMessage("Non-contiguous Features were found: at least one Feature wraps across a periodic boundary.");
  }

  if(m_ShouldCancel)
  {
    return {};
  }

  // =========================================================================
  // Phase 2: Resolution + Relabeling (combined single pass)
  // =========================================================================
  //
  // After Phase 1/1b, every valid voxel has a provisional label and the
  // equivalence state knows which provisional labels belong to the same
  // connected component. This phase scans voxels slice-by-slice, resolves the
  // root and final ID through LabelEquivalence, and writes each completed slice
  // back through copyFromBuffer. Direct keeps these tables resident; OOC keeps
  // them in bounded page caches over temporary-record storage.
  //
  // Combining discovery and relabeling into a single pass halves the number
  // of OOC accesses compared to doing them separately. The slice-sequential
  // iteration order ensures optimal I/O patterns.
  //
  // Because the scan is in linear (Z-Y-X) order, final feature IDs are
  // assigned in the order their first voxel appears in the volume.
  // =========================================================================
  auto prepareFinalLabelsResult = equivalences->prepareFinalLabels(static_cast<uint64>(nextLabel));
  if(prepareFinalLabelsResult.invalid())
  {
    return prepareFinalLabelsResult;
  }
  int32 finalFeatureCount = 0;

  std::vector<int32> sliceData(sliceSize);

  for(int64 iz = 0; iz < dimZ; iz++)
  {
    if(m_ShouldCancel)
    {
      return {};
    }

    auto readResult = featureIdsStore.copyIntoBuffer(static_cast<usize>(iz) * sliceSize, nonstd::span<int32>(sliceData.data(), sliceSize));
    if(readResult.invalid())
    {
      return readResult;
    }

    for(int64 iy = 0; iy < dimY; iy++)
    {
      for(int64 ix = 0; ix < dimX; ix++)
      {
        const usize inSlice = static_cast<usize>(iy * dimX + ix);
        int32 label = sliceData[inSlice];
        if(label > 0)
        {
          auto finalLabelResult = equivalences->resolveFinalLabel(static_cast<uint64>(label), finalFeatureCount, m_ShouldCancel);
          if(finalLabelResult.invalid())
          {
            return ConvertResult(std::move(finalLabelResult));
          }
          sliceData[inSlice] = finalLabelResult.value();
        }
      }
    }

    auto writeResult = featureIdsStore.copyFromBuffer(static_cast<usize>(iz) * sliceSize, nonstd::span<const int32>(sliceData.data(), sliceSize));
    if(writeResult.invalid())
    {
      return writeResult;
    }
  }

  auto flushResult = equivalences->flush(m_ShouldCancel);
  if(flushResult.invalid())
  {
    return flushResult;
  }

  m_FoundFeatures = finalFeatureCount;
  m_MessageHelper.sendMessage(fmt::format("Total Features Found: {}", m_FoundFeatures));

  return {};
}

// -----------------------------------------------------------------------------
Result<> SegmentFeatures::prepareForSlice(int64 /*iz*/, int64 /*dimX*/, int64 /*dimY*/, int64 /*dimZ*/)
{
  return {};
}

// -----------------------------------------------------------------------------
bool SegmentFeatures::isValidVoxel(int64 point) const
{
  return true;
}

// -----------------------------------------------------------------------------
bool SegmentFeatures::areNeighborsSimilar(int64 point1, int64 point2) const
{
  return false;
}

// -----------------------------------------------------------------------------
void SegmentFeatures::randomizeFeatureIds(nx::core::Int32Array* featureIds, uint64 totalFeatures)
{
  m_MessageHelper.sendMessage("Randomizing Feature Ids");
  ClusterUtilities::RandomizeFeatureIds(featureIds->getDataStoreRef(), totalFeatures);
}
