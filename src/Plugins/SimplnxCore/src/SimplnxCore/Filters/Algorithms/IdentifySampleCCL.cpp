// -----------------------------------------------------------------------------
// IdentifySampleCCL.cpp -- Out-of-core CCL for sample identification
// -----------------------------------------------------------------------------
//
// This file implements the out-of-core optimized variant of the IdentifySample
// algorithm. It replaces the BFS flood-fill approach (see IdentifySampleBFS.cpp)
// with scanline Connected Component Labeling (CCL) and a "replay" technique
// designed to process data in strict Z-slice sequential order, avoiding chunk
// thrashing in OOC storage.
//
// ## Architecture: Generic CCL + Replay
//
// The implementation is built on two generic template functions:
//
// 1. runForwardCCL<T>(store, dims, condition, cancel)
//    Performs a single forward scan through the volume in Z-Y-X order. For each
//    voxel where `condition` returns true, assigns a provisional label by
//    checking three backward neighbors (x-1, y-1, z-1) in a rolling 2-slice
//    label buffer. Records equivalences and sizes in bounded external fixed
//    records. Returns a CCLResult containing that state and the largest root.
//
// 2. replayForwardCCL<T>(store, dims, equivalences, condition, action, cancel)
//    Re-executes the exact same forward scan to re-derive the same provisional
//    labels deterministically. For each labeled voxel, resolves the provisional
//    label to its root through the external equivalence table, then calls
//    `action(data, inSlice, root, x, y, z)` to apply per-voxel logic. If the
//    action modifies the slice data, the slice is written back via copyFromBuffer.
//
// This "run then replay" pattern avoids O(volume) label storage. The trade-off
// is reading each Z-slice twice (once during run, once during replay), but for
// OOC datasets the memory savings are critical -- the volume itself may not fit
// in RAM.
//
// ## Phase Structure
//
// The IdentifySampleCCLFunctor orchestrates up to four phases:
//   Phase 1: runForwardCCL on good voxels -> find largest component
//   Phase 2: replayForwardCCL on good voxels -> mask non-sample voxels
//   Phase 3: runForwardCCL on bad voxels -> find hole components (if FillHoles)
//   Phase 4a: replayForwardCCL on bad voxels -> identify boundary-touching roots
//   Phase 4b: replayForwardCCL on bad voxels -> fill interior holes
//
// See IdentifySampleCCL.hpp for detailed algorithm documentation.
// -----------------------------------------------------------------------------

#include "IdentifySampleCCL.hpp"

#include "IdentifySample.hpp"
#include "IdentifySampleCommon.hpp"

#include "simplnx/DataStructure/DataArray.hpp"
#include "simplnx/DataStructure/Geometry/ImageGeom.hpp"
#include "simplnx/DataStructure/IO/Generic/ITemporaryRecordStore.hpp"
#include "simplnx/Utilities/BoundedRecordPageCache.hpp"
#include "simplnx/Utilities/DataStoreUtilities.hpp"
#include "simplnx/Utilities/ExternalEquivalence.hpp"
#include "simplnx/Utilities/FilterUtilities.hpp"
#include "simplnx/Utilities/InMemoryTemporaryRecordStore.hpp"

#include <limits>
#include <memory>
#include <nonstd/span.hpp>

using namespace nx::core;

namespace
{
// =============================================================================
// runForwardCCL
// =============================================================================
// Generic Z-slice-sequential Connected Component Labeling function that works
// on any boolean condition. It processes the volume one Z-slice at a time using
// copyIntoBuffer for OOC-friendly reads, and a rolling 2-slice label buffer
// instead of storing labels for the entire volume.
//
// How it works:
//   - Scans voxels in Z-slice order (z, y, x innermost). For each voxel where
//     `condition(sliceData, inSlice)` returns true, checks three backward
//     neighbors (x-1, y-1, z-1) for existing labels.
//   - If no labeled neighbor exists, assigns a new provisional label.
//   - If multiple differently-labeled neighbors exist, unites them through the
//     external equivalence table.
//   - Tracks component counts in external records, so the largest root needs no
//     resident per-label state.
//
// The `condition` lambda determines which voxels to label. For example:
//   - `data[inSlice] == true` labels good voxels (sample identification)
//   - `!data[inSlice]` labels bad voxels (hole detection)
//
// Returns a CCLResult containing bounded external equivalences, the next
// available label, and the largest root/size.
// =============================================================================
/**
 * @brief Owns the externally backed equivalence state and summary produced by one forward CCL pass.
 *
 * Only the largest root and size remain resident; the label relationships are
 * replayed from the input during later phases instead of storing one label per cell.
 */
struct CCLResult
{
  std::unique_ptr<ExternalEquivalence> equivalences;
  uint64 nextLabel = 1;
  uint64 largestRoot = 0;
  uint64 largestSize = 0;
};

constexpr uint64 k_RecordsPerPage = 4096;
constexpr usize k_MaxCachedPages = 16;

/**
 * @brief Creates fixed-record scratch through the registered provider, with an explicitly permitted resident fallback.
 *
 * Genuine OOC callers pass false so missing external storage fails closed rather
 * than silently allocating cell-count scratch in RAM.
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
    result = {std::move(fallbackResult.value())};
  }
  if(result.valid() && result.value() == nullptr)
  {
    return MakeErrorResult<std::unique_ptr<ITemporaryRecordStore>>(-45460, "IdentifySample temporary-record provider returned a null store.");
  }
  return result;
}

/** @brief Creates one lazily initialized external-equivalence node for every possible provisional label. */
Result<std::unique_ptr<ExternalEquivalence>> CreateEquivalences(uint64 maximumLabel, bool allowInMemoryFallback)
{
  if(maximumLabel == std::numeric_limits<uint64>::max())
  {
    return MakeErrorResult<std::unique_ptr<ExternalEquivalence>>(-45461, "IdentifySample cannot create external equivalences for this image size.");
  }
  auto storeResult = CreateTemporaryRecordStore(sizeof(ExternalEquivalence::Node), maximumLabel + 1, allowInMemoryFallback);
  if(storeResult.invalid())
  {
    return ConvertInvalidResult<std::unique_ptr<ExternalEquivalence>>(std::move(storeResult));
  }
  return ExternalEquivalence::Create(std::move(storeResult.value()), k_RecordsPerPage, k_MaxCachedPages, 0);
}

/**
 * @brief Externally stored Boolean flags for component roots that touch a domain boundary.
 *
 * Hole filling uses these flags during a later replay, avoiding a label-count
 * resident vector when the number of components is large.
 */
class ExternalRootFlags
{
public:
  /** @brief Creates zero-initialized root-flag storage and its bounded cache. */
  static Result<std::unique_ptr<ExternalRootFlags>> Create(uint64 maximumLabel, bool allowInMemoryFallback)
  {
    auto storeResult = CreateTemporaryRecordStore(sizeof(uint8), maximumLabel + 1, allowInMemoryFallback);
    if(storeResult.invalid())
    {
      return ConvertInvalidResult<std::unique_ptr<ExternalRootFlags>>(std::move(storeResult));
    }
    try
    {
      return {std::unique_ptr<ExternalRootFlags>(new ExternalRootFlags(std::move(storeResult.value())))};
    } catch(const std::bad_alloc&)
    {
      return MakeErrorResult<std::unique_ptr<ExternalRootFlags>>(-45462, "IdentifySample could not allocate bounded boundary-root cache.");
    }
  }

  /** @brief Marks one resolved root as boundary-connected. */
  Result<> mark(uint64 label, const std::atomic_bool& shouldCancel)
  {
    return m_Cache.write(label, uint8{1}, shouldCancel);
  }

  /** @brief Tests one resolved root through the bounded page cache. */
  Result<bool> isMarked(uint64 label, const std::atomic_bool& shouldCancel)
  {
    auto result = m_Cache.read(label, shouldCancel);
    if(result.invalid())
    {
      return ConvertInvalidResult<bool>(std::move(result));
    }
    return {result.value() != 0};
  }

  /** @brief Flushes dirty flag pages before the subsequent replay reads them. */
  Result<> flush(const std::atomic_bool& shouldCancel)
  {
    return m_Cache.flush(shouldCancel);
  }

private:
  /** @brief Takes ownership of the validated flag store and binds its bounded cache. */
  explicit ExternalRootFlags(std::unique_ptr<ITemporaryRecordStore> store)
  : m_Store(std::move(store))
  , m_Cache(*m_Store, k_RecordsPerPage, k_MaxCachedPages)
  {
  }

  std::unique_ptr<ITemporaryRecordStore> m_Store;
  BoundedRecordPageCache<uint8> m_Cache;
};

/**
 * @brief Labels a 3D volume in Z/Y/X order with two resident label slices and external equivalences.
 * @return Externally backed label relationships plus the largest component summary.
 */
template <typename T, typename ConditionFn>
Result<CCLResult> runForwardCCL(AbstractDataStore<T>& store, int64 dimX, int64 dimY, int64 dimZ, ConditionFn condition, const std::atomic_bool& shouldCancel)
{
  CCLResult result;
  if(shouldCancel)
  {
    return {std::move(result)};
  }
  const usize sliceSize = static_cast<usize>(dimX * dimY);
  const uint64 totalPoints = static_cast<uint64>(dimX) * static_cast<uint64>(dimY) * static_cast<uint64>(dimZ);
  const bool allowInMemoryFallback = store.getStoreType() != IDataStore::StoreType::OutOfCore;
  auto equivalencesResult = CreateEquivalences(totalPoints, allowInMemoryFallback);
  if(equivalencesResult.invalid())
  {
    return ConvertInvalidResult<CCLResult>(std::move(equivalencesResult));
  }
  result.equivalences = std::move(equivalencesResult.value());

  // Rolling 2-slice buffer: only the current and previous Z-slice labels are
  // kept in memory. The scanline CCL only looks at backward neighbors (x-1,
  // y-1, z-1), so two slices suffice. This gives O(dimX * dimY) memory
  // instead of O(volume).
  std::vector<int64> labelBuffer(2 * sliceSize, 0);
  auto sliceData = std::make_unique<T[]>(sliceSize);

  for(int64 z = 0; z < dimZ; z++)
  {
    if(shouldCancel)
    {
      return {std::move(result)};
    }
    auto readResult = store.copyIntoBuffer(static_cast<usize>(z) * sliceSize, nonstd::span<T>(sliceData.get(), sliceSize));
    if(readResult.invalid())
    {
      return ConvertInvalidResult<CCLResult>(std::move(readResult));
    }

    const usize curOff = (static_cast<usize>(z) % 2) * sliceSize;
    std::fill(labelBuffer.begin() + curOff, labelBuffer.begin() + curOff + sliceSize, 0);
    const usize prevOff = ((static_cast<usize>(z) + 1) % 2) * sliceSize;

    for(int64 y = 0; y < dimY; y++)
    {
      for(int64 x = 0; x < dimX; x++)
      {
        const usize inSlice = static_cast<usize>(y) * static_cast<usize>(dimX) + static_cast<usize>(x);

        if(!condition(sliceData.get(), inSlice))
        {
          continue;
        }

        // Backward neighbor checks from label buffer
        int64 nbrA = 0, nbrB = 0, nbrC = 0;

        if(x > 0)
        {
          nbrA = labelBuffer[curOff + inSlice - 1];
        }
        if(y > 0)
        {
          nbrB = labelBuffer[curOff + inSlice - static_cast<usize>(dimX)];
        }
        if(z > 0)
        {
          nbrC = labelBuffer[prevOff + inSlice];
        }

        int64 minLabel = 0;
        if(nbrA > 0)
        {
          minLabel = nbrA;
        }
        if(nbrB > 0 && (minLabel == 0 || nbrB < minLabel))
        {
          minLabel = nbrB;
        }
        if(nbrC > 0 && (minLabel == 0 || nbrC < minLabel))
        {
          minLabel = nbrC;
        }

        int64 assignedLabel = 0;
        if(minLabel == 0)
        {
          assignedLabel = result.nextLabel++;
        }
        else
        {
          assignedLabel = minLabel;
          if(nbrA > 0 && nbrA != assignedLabel)
          {
            auto uniteResult = result.equivalences->unite(static_cast<uint64>(assignedLabel), static_cast<uint64>(nbrA), shouldCancel);
            if(uniteResult.invalid())
            {
              return ConvertInvalidResult<CCLResult>(std::move(uniteResult));
            }
          }
          if(nbrB > 0 && nbrB != assignedLabel)
          {
            auto uniteResult = result.equivalences->unite(static_cast<uint64>(assignedLabel), static_cast<uint64>(nbrB), shouldCancel);
            if(uniteResult.invalid())
            {
              return ConvertInvalidResult<CCLResult>(std::move(uniteResult));
            }
          }
          if(nbrC > 0 && nbrC != assignedLabel)
          {
            auto uniteResult = result.equivalences->unite(static_cast<uint64>(assignedLabel), static_cast<uint64>(nbrC), shouldCancel);
            if(uniteResult.invalid())
            {
              return ConvertInvalidResult<CCLResult>(std::move(uniteResult));
            }
          }
        }

        labelBuffer[curOff + inSlice] = assignedLabel;
        auto sizeResult = result.equivalences->addSize(static_cast<uint64>(assignedLabel), 1, shouldCancel);
        if(sizeResult.invalid())
        {
          return ConvertInvalidResult<CCLResult>(std::move(sizeResult));
        }
      }
    }
  }

  // ExternalEquivalence carries component sizes in disk-backed records. Scan
  // only roots in ascending label order so equal-size ties retain the original
  // implementation's final (largest label) winner.
  for(uint64 label = 1; label < result.nextLabel; label++)
  {
    auto rootResult = result.equivalences->find(label, shouldCancel);
    if(rootResult.invalid())
    {
      return ConvertInvalidResult<CCLResult>(std::move(rootResult));
    }
    if(rootResult.value() == label)
    {
      auto sizeResult = result.equivalences->componentSize(label, shouldCancel);
      if(sizeResult.invalid())
      {
        return ConvertInvalidResult<CCLResult>(std::move(sizeResult));
      }
      if(sizeResult.value() >= result.largestSize)
      {
        result.largestSize = sizeResult.value();
        result.largestRoot = label;
      }
    }
  }

  auto flushResult = result.equivalences->flush(shouldCancel);
  if(flushResult.invalid())
  {
    return ConvertInvalidResult<CCLResult>(std::move(flushResult));
  }
  return {std::move(result)};
}

// =============================================================================
// replayForwardCCL
// =============================================================================
// Re-derives labels by running the exact same forward CCL scan a second time
// (same Z-slice order, same scanline traversal, same union-find). Since CCL
// label assignment is fully deterministic given the same scan order and
// condition, the re-derived provisional labels match the original ones from
// runForwardCCL exactly. The union-find (already flattened) is then used to
// resolve each provisional label to its root.
//
// The `action` lambda is called for each labeled voxel with its resolved root
// label, the slice data buffer, and the voxel's (x, y, z) coordinates. It
// returns true if the slice data was modified, so the slice can be written back
// via copyFromBuffer. This allows per-voxel decisions (e.g., "mask out if
// root != largestRoot", or "fill if root is an interior hole") without ever
// storing labels for the entire volume.
//
// This is the key OOC trick: by re-computing labels on the fly using only a
// 2-slice rolling buffer, we avoid O(volume) label storage. The trade-off is
// reading the data twice, but for OOC datasets the memory savings are critical.
//
// Note: the union-find unite() calls from the first pass are not repeated here
// because the union-find is already flattened. We only need the label
// assignment logic to re-derive the same provisional labels.
// =============================================================================
/**
 * @brief Re-derives the first pass's provisional labels and applies an action using resolved roots.
 *
 * Replaying trades one sequential read pass for removal of a full-volume label
 * array, which is the central bounded-memory choice in this implementation.
 */
template <typename T, typename ConditionFn, typename ActionFn>
Result<> replayForwardCCL(AbstractDataStore<T>& store, int64 dimX, int64 dimY, int64 dimZ, ExternalEquivalence& equivalences, ConditionFn condition, ActionFn action,
                          const std::atomic_bool& shouldCancel)
{
  const usize sliceSize = static_cast<usize>(dimX * dimY);
  auto sliceData = std::make_unique<T[]>(sliceSize);
  std::vector<int64> labelBuffer(2 * sliceSize, 0);
  int64 nextLabel = 1;

  for(int64 z = 0; z < dimZ; z++)
  {
    if(shouldCancel)
    {
      return {};
    }
    auto readResult = store.copyIntoBuffer(static_cast<usize>(z) * sliceSize, nonstd::span<T>(sliceData.get(), sliceSize));
    if(readResult.invalid())
    {
      return readResult;
    }
    bool modified = false;

    const usize curOff = (static_cast<usize>(z) % 2) * sliceSize;
    std::fill(labelBuffer.begin() + curOff, labelBuffer.begin() + curOff + sliceSize, 0);
    const usize prevOff = ((static_cast<usize>(z) + 1) % 2) * sliceSize;

    for(int64 y = 0; y < dimY; y++)
    {
      for(int64 x = 0; x < dimX; x++)
      {
        const usize inSlice = static_cast<usize>(y) * static_cast<usize>(dimX) + static_cast<usize>(x);

        if(!condition(sliceData.get(), inSlice))
        {
          continue;
        }

        // Re-derive label (same logic, no union-find unites needed since already flattened)
        int64 nbrA = 0, nbrB = 0, nbrC = 0;

        if(x > 0)
        {
          nbrA = labelBuffer[curOff + inSlice - 1];
        }
        if(y > 0)
        {
          nbrB = labelBuffer[curOff + inSlice - static_cast<usize>(dimX)];
        }
        if(z > 0)
        {
          nbrC = labelBuffer[prevOff + inSlice];
        }

        int64 minLabel = 0;
        if(nbrA > 0)
        {
          minLabel = nbrA;
        }
        if(nbrB > 0 && (minLabel == 0 || nbrB < minLabel))
        {
          minLabel = nbrB;
        }
        if(nbrC > 0 && (minLabel == 0 || nbrC < minLabel))
        {
          minLabel = nbrC;
        }

        int64 assignedLabel = 0;
        if(minLabel == 0)
        {
          assignedLabel = nextLabel++;
        }
        else
        {
          assignedLabel = minLabel;
        }

        labelBuffer[curOff + inSlice] = assignedLabel;

        // Apply the action with the re-derived label
        auto rootResult = equivalences.find(static_cast<uint64>(assignedLabel), shouldCancel);
        if(rootResult.invalid())
        {
          return ConvertResult(std::move(rootResult));
        }
        auto actionResult = action(sliceData.get(), inSlice, rootResult.value(), static_cast<usize>(x), static_cast<usize>(y), static_cast<usize>(z));
        if(actionResult.invalid())
        {
          return ConvertResult(std::move(actionResult));
        }
        if(actionResult.value())
        {
          modified = true;
        }
      }
    }

    if(modified)
    {
      auto writeResult = store.copyFromBuffer(static_cast<usize>(z) * sliceSize, nonstd::span<const T>(sliceData.get(), sliceSize));
      if(writeResult.invalid())
      {
        return writeResult;
      }
    }
  }
  return {};
}

/** @brief Performs the same bounded CCL operation on one already buffered 2D plane. */
template <typename T, typename ConditionFn>
Result<CCLResult> runPlaneCCL(T* data, int64 dim1, int64 dim2, ConditionFn condition, bool allowInMemoryFallback, const std::atomic_bool& shouldCancel)
{
  CCLResult result;
  if(shouldCancel)
  {
    return {std::move(result)};
  }
  const uint64 pointCount = static_cast<uint64>(dim1) * static_cast<uint64>(dim2);
  auto equivalencesResult = CreateEquivalences(pointCount, allowInMemoryFallback);
  if(equivalencesResult.invalid())
  {
    return ConvertInvalidResult<CCLResult>(std::move(equivalencesResult));
  }
  result.equivalences = std::move(equivalencesResult.value());
  std::vector<int64> labels(static_cast<usize>(2 * dim1), 0);

  for(int64 row = 0; row < dim2; row++)
  {
    if(shouldCancel)
    {
      return {std::move(result)};
    }
    const usize currentOffset = static_cast<usize>(row % 2) * static_cast<usize>(dim1);
    const usize previousOffset = static_cast<usize>((row + 1) % 2) * static_cast<usize>(dim1);
    std::fill(labels.begin() + currentOffset, labels.begin() + currentOffset + static_cast<usize>(dim1), 0);
    for(int64 column = 0; column < dim1; column++)
    {
      const usize index = static_cast<usize>(row * dim1 + column);
      if(!condition(data[index]))
      {
        continue;
      }
      const int64 left = column > 0 ? labels[currentOffset + static_cast<usize>(column - 1)] : 0;
      const int64 above = row > 0 ? labels[previousOffset + static_cast<usize>(column)] : 0;
      const int64 assigned = left > 0 && above > 0 ? std::min(left, above) : std::max(left, above);
      const int64 label = assigned > 0 ? assigned : static_cast<int64>(result.nextLabel++);
      labels[currentOffset + static_cast<usize>(column)] = label;
      if(left > 0 && left != label)
      {
        auto uniteResult = result.equivalences->unite(static_cast<uint64>(label), static_cast<uint64>(left), shouldCancel);
        if(uniteResult.invalid())
        {
          return ConvertInvalidResult<CCLResult>(std::move(uniteResult));
        }
      }
      if(above > 0 && above != label)
      {
        auto uniteResult = result.equivalences->unite(static_cast<uint64>(label), static_cast<uint64>(above), shouldCancel);
        if(uniteResult.invalid())
        {
          return ConvertInvalidResult<CCLResult>(std::move(uniteResult));
        }
      }
      auto sizeResult = result.equivalences->addSize(static_cast<uint64>(label), 1, shouldCancel);
      if(sizeResult.invalid())
      {
        return ConvertInvalidResult<CCLResult>(std::move(sizeResult));
      }
    }
  }
  for(uint64 label = 1; label < result.nextLabel; label++)
  {
    auto rootResult = result.equivalences->find(label, shouldCancel);
    if(rootResult.invalid())
    {
      return ConvertInvalidResult<CCLResult>(std::move(rootResult));
    }
    if(rootResult.value() == label)
    {
      auto sizeResult = result.equivalences->componentSize(label, shouldCancel);
      if(sizeResult.invalid())
      {
        return ConvertInvalidResult<CCLResult>(std::move(sizeResult));
      }
      if(sizeResult.value() >= result.largestSize)
      {
        result.largestRoot = label;
        result.largestSize = sizeResult.value();
      }
    }
  }
  auto flushResult = result.equivalences->flush(shouldCancel);
  if(flushResult.invalid())
  {
    return ConvertInvalidResult<CCLResult>(std::move(flushResult));
  }
  return {std::move(result)};
}

/** @brief Replays a buffered 2D plane's labels and invokes an action for each selected value. */
template <typename T, typename ConditionFn, typename ActionFn>
Result<> replayPlaneCCL(T* data, int64 dim1, int64 dim2, ExternalEquivalence& equivalences, ConditionFn condition, ActionFn action, const std::atomic_bool& shouldCancel)
{
  std::vector<int64> labels(static_cast<usize>(2 * dim1), 0);
  uint64 nextLabel = 1;
  for(int64 row = 0; row < dim2; row++)
  {
    if(shouldCancel)
    {
      return {};
    }
    const usize currentOffset = static_cast<usize>(row % 2) * static_cast<usize>(dim1);
    const usize previousOffset = static_cast<usize>((row + 1) % 2) * static_cast<usize>(dim1);
    std::fill(labels.begin() + currentOffset, labels.begin() + currentOffset + static_cast<usize>(dim1), 0);
    for(int64 column = 0; column < dim1; column++)
    {
      const usize index = static_cast<usize>(row * dim1 + column);
      if(!condition(data[index]))
      {
        continue;
      }
      const int64 left = column > 0 ? labels[currentOffset + static_cast<usize>(column - 1)] : 0;
      const int64 above = row > 0 ? labels[previousOffset + static_cast<usize>(column)] : 0;
      const int64 label = left > 0 && above > 0 ? std::min(left, above) : std::max(left, above);
      const uint64 assigned = label > 0 ? static_cast<uint64>(label) : nextLabel++;
      labels[currentOffset + static_cast<usize>(column)] = static_cast<int64>(assigned);
      auto rootResult = equivalences.find(assigned, shouldCancel);
      if(rootResult.invalid())
      {
        return ConvertResult(std::move(rootResult));
      }
      auto actionResult = action(data[index], rootResult.value(), column, row);
      if(actionResult.invalid())
      {
        return ConvertResult(std::move(actionResult));
      }
    }
  }
  return {};
}

/** @brief Retains the largest good 2D component and optionally fills non-boundary bad components. */
template <typename T>
Result<> identifyPlane(T* data, int64 dim1, int64 dim2, bool fillHoles, bool allowInMemoryFallback, const std::atomic_bool& shouldCancel)
{
  const auto good = [](T value) { return static_cast<bool>(value); };
  auto goodResult = runPlaneCCL(data, dim1, dim2, good, allowInMemoryFallback, shouldCancel);
  if(goodResult.invalid())
  {
    return ConvertResult(std::move(goodResult));
  }
  if(shouldCancel || goodResult.value().largestRoot == 0)
  {
    return {};
  }
  const uint64 largestRoot = goodResult.value().largestRoot;
  auto removeResult = replayPlaneCCL(
      data, dim1, dim2, *goodResult.value().equivalences, good,
      [largestRoot](T& value, uint64 root, int64, int64) -> Result<> {
        if(root != largestRoot)
        {
          value = static_cast<T>(false);
        }
        return {};
      },
      shouldCancel);
  if(removeResult.invalid() || shouldCancel || !fillHoles)
  {
    return removeResult;
  }
  const auto bad = [](T value) { return !static_cast<bool>(value); };
  auto holesResult = runPlaneCCL(data, dim1, dim2, bad, allowInMemoryFallback, shouldCancel);
  if(holesResult.invalid())
  {
    return ConvertResult(std::move(holesResult));
  }
  auto flagsResult = ExternalRootFlags::Create(holesResult.value().nextLabel, allowInMemoryFallback);
  if(flagsResult.invalid())
  {
    return ConvertResult(std::move(flagsResult));
  }
  auto flags = std::move(flagsResult.value());
  auto boundaryResult = replayPlaneCCL(
      data, dim1, dim2, *holesResult.value().equivalences, bad,
      [&flags, dim1, dim2, &shouldCancel](T&, uint64 root, int64 column, int64 row) -> Result<> {
        if(column == 0 || column == dim1 - 1 || row == 0 || row == dim2 - 1)
        {
          return flags->mark(root, shouldCancel);
        }
        return {};
      },
      shouldCancel);
  if(boundaryResult.invalid())
  {
    return boundaryResult;
  }
  auto flushResult = flags->flush(shouldCancel);
  if(flushResult.invalid())
  {
    return flushResult;
  }
  return replayPlaneCCL(
      data, dim1, dim2, *holesResult.value().equivalences, bad,
      [&flags, &shouldCancel](T& value, uint64 root, int64, int64) -> Result<> {
        auto markedResult = flags->isMarked(root, shouldCancel);
        if(markedResult.invalid())
        {
          return ConvertResult(std::move(markedResult));
        }
        if(!markedResult.value())
        {
          value = static_cast<T>(true);
        }
        return {};
      },
      shouldCancel);
}

/** @brief Dispatches Bool/UInt8 plane extraction, bounded plane CCL, and checked write-back. */
struct IdentifySampleSliceCCLFunctor
{
  /** @brief Processes every plane of the selected orientation with one plane buffer at a time. */
  template <typename T>
  Result<> operator()(const ImageGeom* imageGeom, IDataArray* maskArray, bool fillHoles, IdentifySampleSliceBySliceFunctor::Plane plane, const IFilter::MessageHandler& messageHandler,
                      const std::atomic_bool& shouldCancel) const
  {
    auto& store = maskArray->template getIDataStoreRefAs<AbstractDataStore<T>>();
    const bool allowInMemoryFallback = store.getStoreType() != IDataStore::StoreType::OutOfCore;
    const SizeVec3 dimensions = imageGeom->getDimensions();
    const int64 dimX = static_cast<int64>(dimensions[0]);
    const int64 dimY = static_cast<int64>(dimensions[1]);
    const int64 dimZ = static_cast<int64>(dimensions[2]);
    const usize zSliceSize = static_cast<usize>(dimX * dimY);

    int64 planeDim1 = 0;
    int64 planeDim2 = 0;
    int64 fixedDim = 0;
    if(plane == IdentifySampleSliceBySliceFunctor::Plane::XY)
    {
      planeDim1 = dimX;
      planeDim2 = dimY;
      fixedDim = dimZ;
    }
    else if(plane == IdentifySampleSliceBySliceFunctor::Plane::XZ)
    {
      planeDim1 = dimX;
      planeDim2 = dimZ;
      fixedDim = dimY;
    }
    else
    {
      planeDim1 = dimY;
      planeDim2 = dimZ;
      fixedDim = dimX;
    }
    const usize planeSize = static_cast<usize>(planeDim1 * planeDim2);
    auto planeBuffer = std::make_unique<T[]>(planeSize);
    auto zBuffer = std::make_unique<T[]>(zSliceSize);

    for(int64 fixed = 0; fixed < fixedDim; fixed++)
    {
      if(shouldCancel)
      {
        return {};
      }
      messageHandler(IFilter::Message::Type::Info, fmt::format("Slice {}", fixed));
      if(plane == IdentifySampleSliceBySliceFunctor::Plane::XY)
      {
        auto readResult = store.copyIntoBuffer(static_cast<usize>(fixed) * planeSize, nonstd::span<T>(planeBuffer.get(), planeSize));
        if(readResult.invalid())
        {
          return readResult;
        }
      }
      else if(plane == IdentifySampleSliceBySliceFunctor::Plane::XZ)
      {
        for(int64 z = 0; z < dimZ; z++)
        {
          auto readResult = store.copyIntoBuffer(static_cast<usize>(z * dimX * dimY + fixed * dimX), nonstd::span<T>(planeBuffer.get() + static_cast<usize>(z * dimX), static_cast<usize>(dimX)));
          if(readResult.invalid())
          {
            return readResult;
          }
        }
      }
      else
      {
        for(int64 z = 0; z < dimZ; z++)
        {
          auto readResult = store.copyIntoBuffer(static_cast<usize>(z) * zSliceSize, nonstd::span<T>(zBuffer.get(), zSliceSize));
          if(readResult.invalid())
          {
            return readResult;
          }
          for(int64 y = 0; y < dimY; y++)
          {
            planeBuffer[static_cast<usize>(z * dimY + y)] = zBuffer[static_cast<usize>(y * dimX + fixed)];
          }
        }
      }

      auto identifyResult = identifyPlane(planeBuffer.get(), planeDim1, planeDim2, fillHoles, allowInMemoryFallback, shouldCancel);
      if(identifyResult.invalid())
      {
        return identifyResult;
      }
      if(shouldCancel)
      {
        return {};
      }

      if(plane == IdentifySampleSliceBySliceFunctor::Plane::XY)
      {
        auto writeResult = store.copyFromBuffer(static_cast<usize>(fixed) * planeSize, nonstd::span<const T>(planeBuffer.get(), planeSize));
        if(writeResult.invalid())
        {
          return writeResult;
        }
      }
      else if(plane == IdentifySampleSliceBySliceFunctor::Plane::XZ)
      {
        for(int64 z = 0; z < dimZ; z++)
        {
          auto writeResult =
              store.copyFromBuffer(static_cast<usize>(z * dimX * dimY + fixed * dimX), nonstd::span<const T>(planeBuffer.get() + static_cast<usize>(z * dimX), static_cast<usize>(dimX)));
          if(writeResult.invalid())
          {
            return writeResult;
          }
        }
      }
      else
      {
        for(int64 z = 0; z < dimZ; z++)
        {
          auto readResult = store.copyIntoBuffer(static_cast<usize>(z) * zSliceSize, nonstd::span<T>(zBuffer.get(), zSliceSize));
          if(readResult.invalid())
          {
            return readResult;
          }
          for(int64 y = 0; y < dimY; y++)
          {
            zBuffer[static_cast<usize>(y * dimX + fixed)] = planeBuffer[static_cast<usize>(z * dimY + y)];
          }
          auto writeResult = store.copyFromBuffer(static_cast<usize>(z) * zSliceSize, nonstd::span<const T>(zBuffer.get(), zSliceSize));
          if(writeResult.invalid())
          {
            return writeResult;
          }
        }
      }
    }
    return {};
  }
};

// =============================================================================
// IdentifySampleCCLFunctor
// =============================================================================
// Z-slice-sequential scanline CCL implementation for identifying the largest
// connected component of good voxels in a 3D image geometry, then optionally
// filling interior holes. Processes data one Z-slice at a time using
// copyIntoBuffer/copyFromBuffer for OOC-friendly access, using a 2-slice
// rolling buffer (O(slice) memory) instead of O(volume).
//
// The algorithm has up to four phases:
//
// Phase 1: Forward CCL on good voxels
//   Run runForwardCCL with condition = (goodVoxels[inSlice] == true) to
//   discover all connected components and find the largest one by voxel count.
//
// Phase 2: Replay CCL to mask non-sample voxels
//   Run replayForwardCCL with the same good-voxel condition. For each voxel
//   whose resolved root != largestRoot, set goodVoxels to false (removing
//   satellite regions/noise). No O(volume) label storage is needed -- labels
//   are recomputed on the fly.
//
// Phase 3 (if fillHoles): Forward CCL on bad voxels
//   Run runForwardCCL with condition = (!goodVoxels[inSlice]) to discover all
//   connected components of non-sample space (potential holes + exterior).
//
// Phase 4 (if fillHoles): Replay CCL to identify and fill interior holes
//   First replay: for each bad-voxel component, check if any voxel lies on
//   a domain boundary. Mark boundary-touching roots in a boolean vector.
//   Second replay: for each bad voxel whose root is NOT boundary-touching,
//   set goodVoxels to true (filling the interior hole).
// =============================================================================
/** @brief Dispatches the full-volume bounded CCL and replay pipeline by mask value type. */
struct IdentifySampleCCLFunctor
{
  /** @brief Runs largest-sample retention followed by optional boundary-aware hole filling. */
  template <typename T>
  Result<> operator()(const ImageGeom* imageGeom, IDataArray* goodVoxelsPtr, bool fillHoles, const IFilter::MessageHandler& messageHandler, const std::atomic_bool& shouldCancel)
  {
    auto& goodVoxels = goodVoxelsPtr->template getIDataStoreRefAs<AbstractDataStore<T>>();

    SizeVec3 udims = imageGeom->getDimensions();
    const int64 dimX = static_cast<int64>(udims[0]);
    const int64 dimY = static_cast<int64>(udims[1]);
    const int64 dimZ = static_cast<int64>(udims[2]);

    // --- Phase 1: Forward CCL on good voxels ----------------------------------
    // Discover all connected components of good voxels and find the largest one.
    // The condition lambda selects voxels where goodVoxels[inSlice] is true.
    auto goodCondition = [](const T* data, usize inSlice) -> bool { return static_cast<bool>(data[inSlice]); };
    auto cclResult = runForwardCCL<T>(goodVoxels, dimX, dimY, dimZ, goodCondition, shouldCancel);
    if(cclResult.invalid())
    {
      return ConvertResult(std::move(cclResult));
    }

    if(shouldCancel || cclResult.value().largestRoot == 0)
    {
      return {};
    }

    // --- Phase 2: Replay CCL to mask non-sample voxels ----------------------
    // Re-derive labels using a second forward pass with the same scan order
    // and condition. For each voxel whose resolved root is not the largest
    // component, set goodVoxels to false (removing satellite regions/noise).
    // No O(volume) label storage is needed -- labels are recomputed on the fly.
    const uint64 largestRoot = cclResult.value().largestRoot;
    auto replayGoodResult = replayForwardCCL<T>(
        goodVoxels, dimX, dimY, dimZ, *cclResult.value().equivalences, goodCondition,
        [&largestRoot](T* data, usize inSlice, uint64 root, usize /*x*/, usize /*y*/, usize /*z*/) -> Result<bool> {
          if(root != largestRoot)
          {
            data[inSlice] = static_cast<T>(false);
            return {true};
          }
          return {false};
        },
        shouldCancel);
    if(replayGoodResult.invalid())
    {
      return replayGoodResult;
    }

    if(shouldCancel)
    {
      return {};
    }

    // --- Phase 3: Forward CCL on bad voxels (hole detection) -----------------
    // Only runs if fillHoles is true. Discovers connected components of
    // non-good voxels (the complement of the sample). These include both
    // exterior empty space and interior holes.
    if(fillHoles)
    {
      // Condition selects voxels where goodVoxels[inSlice] is false (bad data)
      auto holeCondition = [](const T* data, usize inSlice) -> bool { return !static_cast<bool>(data[inSlice]); };
      auto holeCCL = runForwardCCL<T>(goodVoxels, dimX, dimY, dimZ, holeCondition, shouldCancel);
      if(holeCCL.invalid())
      {
        return ConvertResult(std::move(holeCCL));
      }

      if(shouldCancel)
      {
        return {};
      }

      // --- Phase 4a: Replay CCL to identify boundary-touching roots ---------
      // Replay the hole CCL to re-derive labels. For each labeled voxel,
      // check if it lies on a domain boundary face. If so, mark its resolved
      // root as boundary-touching. Components that touch the boundary are
      // exterior space (not holes). This avoids O(volume) label storage by
      // re-computing labels on the fly.
      const bool allowInMemoryFallback = goodVoxels.getStoreType() != IDataStore::StoreType::OutOfCore;
      auto boundaryRootsResult = ExternalRootFlags::Create(holeCCL.value().nextLabel, allowInMemoryFallback);
      if(boundaryRootsResult.invalid())
      {
        return ConvertResult(std::move(boundaryRootsResult));
      }
      auto boundaryRoots = std::move(boundaryRootsResult.value());
      auto boundaryResult = replayForwardCCL<T>(
          goodVoxels, dimX, dimY, dimZ, *holeCCL.value().equivalences, holeCondition,
          [&boundaryRoots, dimX, dimY, dimZ, &shouldCancel](T* /*data*/, usize /*inSlice*/, uint64 root, usize x, usize y, usize z) -> Result<bool> {
            if(x == 0 || x == static_cast<usize>(dimX - 1) || y == 0 || y == static_cast<usize>(dimY - 1) || z == 0 || z == static_cast<usize>(dimZ - 1))
            {
              auto markResult = boundaryRoots->mark(root, shouldCancel);
              if(markResult.invalid())
              {
                return ConvertInvalidResult<bool>(std::move(markResult));
              }
            }
            return {false}; // Never modifies data
          },
          shouldCancel);
      if(boundaryResult.invalid())
      {
        return boundaryResult;
      }
      auto flushResult = boundaryRoots->flush(shouldCancel);
      if(flushResult.invalid())
      {
        return flushResult;
      }

      if(shouldCancel)
      {
        return {};
      }

      // --- Phase 4b: Replay CCL again to fill interior holes ----------------
      // A third replay of the same CCL (same condition, same union-find) to
      // apply the fill. For each bad voxel whose root is NOT boundary-touching,
      // it must be an interior hole fully enclosed by the sample -- set it to
      // true. Boundary-touching components are exterior and left as-is.
      auto fillResult = replayForwardCCL<T>(
          goodVoxels, dimX, dimY, dimZ, *holeCCL.value().equivalences, holeCondition,
          [&boundaryRoots, &shouldCancel](T* data, usize inSlice, uint64 root, usize /*x*/, usize /*y*/, usize /*z*/) -> Result<bool> {
            auto markedResult = boundaryRoots->isMarked(root, shouldCancel);
            if(markedResult.invalid())
            {
              return ConvertInvalidResult<bool>(std::move(markedResult));
            }
            if(!markedResult.value())
            {
              data[inSlice] = static_cast<T>(true);
              return {true};
            }
            return {false};
          },
          shouldCancel);
      if(fillResult.invalid())
      {
        return fillResult;
      }
    }
    return {};
  }
};
} // namespace

// -----------------------------------------------------------------------------
IdentifySampleCCL::IdentifySampleCCL(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel, const IdentifySampleInputValues* inputValues)
: m_DataStructure(dataStructure)
, m_InputValues(inputValues)
, m_ShouldCancel(shouldCancel)
, m_MessageHandler(mesgHandler)
{
}

// -----------------------------------------------------------------------------
IdentifySampleCCL::~IdentifySampleCCL() noexcept = default;

// -----------------------------------------------------------------------------
Result<> IdentifySampleCCL::operator()()
{
  auto* inputData = m_DataStructure.getDataAs<IDataArray>(m_InputValues->MaskArrayPath);
  const auto* imageGeom = m_DataStructure.getDataAs<ImageGeom>(m_InputValues->InputImageGeometryPath);

  if(m_InputValues->SliceBySlice)
  {
    return ExecuteDataFunction(IdentifySampleSliceCCLFunctor{}, inputData->getDataType(), imageGeom, inputData, m_InputValues->FillHoles,
                               static_cast<IdentifySampleSliceBySliceFunctor::Plane>(m_InputValues->SliceBySlicePlaneIndex), m_MessageHandler, m_ShouldCancel);
  }
  else
  {
    return ExecuteDataFunction(IdentifySampleCCLFunctor{}, inputData->getDataType(), imageGeom, inputData, m_InputValues->FillHoles, m_MessageHandler, m_ShouldCancel);
  }

  return {};
}
