// -----------------------------------------------------------------------------
// FillBadDataCCL.cpp -- Out-of-core CCL algorithm for filling bad data
// -----------------------------------------------------------------------------
//
// This file implements the out-of-core optimized variant of the FillBadData
// algorithm. It replaces the BFS flood-fill approach (see FillBadDataBFS.cpp)
// with a four-phase pipeline built on scanline Connected Component Labeling
// (CCL) and externally backed equivalence state, designed to process data in strict Z-slice sequential
// order to avoid chunk thrashing in OOC storage.
//
// ## The Chunk Thrashing Problem
//
// When data is stored in compressed HDF5 chunks (e.g., 64x64x64 voxels per
// chunk), each random access to a voxel may trigger decompression of an entire
// chunk. BFS flood-fill visits neighbors in a wavefront pattern that crosses
// chunk boundaries unpredictably, causing the same chunks to be loaded and
// evicted thousands of times. For a 300x300x300 volume, this can turn a
// ~1-second in-core operation into a multi-hour ordeal.
//
// ## How CCL Avoids Thrashing
//
// The CCL approach processes voxels in a fixed Z-Y-X scan order, reading each
// Z-slice exactly once via bulk copyIntoBuffer/copyFromBuffer calls. Cross-slice
// connectivity is resolved symbolically through temporary records rather than
// a resident Union-Find. A rolling 2-slice
// label buffer provides backward neighbor lookups using O(dimX * dimY) memory.
//
// ## Four-Phase Pipeline
//
// Phase 1: Z-Slice Sequential CCL
//   Scans Z-slices sequentially, assigning provisional labels to bad-data voxels
//   (FeatureId == 0). Uses a 2-slice rolling label buffer for backward neighbor
//   reads. Records equivalences externally. Accumulates per-label voxel counts.
//   Writes provisional labels to the FeatureIds store for Phase 3 to read.
//
// Phase 2: Global Resolution
//   Resolves roots through bounded external-equivalence cache pages. Sizes are
//   accumulated at roots as equivalences are joined.
//
// Phase 3: Region Classification and Relabeling
//   Reads provisional labels from FeatureIds (one Z-slice at a time), resolves
//   each to its root, and classifies by total component size:
//   - Small regions (< threshold): relabeled to -1 for filling in Phase 4
//   - Large regions (>= threshold): relabeled to 0 (optionally new phase)
//
// Phase 4: Iterative Morphological Fill (External-Record Deferred)
//   Each iteration has two passes:
//   - Pass 1 (Vote): 3-slice rolling window scan. For each -1 voxel, majority
//     vote among face neighbors. Write (dest, src) pairs to a temporary record store.
//   - Pass 2 (Apply): Read bounded pair batches back and apply fills via 3-slice buffered bulk I/O.
//   No O(N) memory allocations. Uses O(features) vote counters plus externally backed records.
//   Repeats until no -1 voxels remain.
//
// ## Result Equivalence
//
// This algorithm produces identical results to FillBadDataBFS for the same inputs.
// The four-phase decomposition is purely an optimization of the data access pattern.
//
// -----------------------------------------------------------------------------

#include "FillBadDataCCL.hpp"

#include "FillBadData.hpp"

#include "simplnx/DataStructure/DataArray.hpp"
#include "simplnx/DataStructure/Geometry/ImageGeom.hpp"
#include "simplnx/DataStructure/IO/Generic/ITemporaryRecordStore.hpp"
#include "simplnx/Utilities/AlgorithmDispatch.hpp"
#include "simplnx/Utilities/DataGroupUtilities.hpp"
#include "simplnx/Utilities/DataStoreUtilities.hpp"
#include "simplnx/Utilities/ExternalEquivalence.hpp"
#include "simplnx/Utilities/FilterUtilities.hpp"
#include "simplnx/Utilities/InMemoryTemporaryRecordStore.hpp"

#include <nonstd/span.hpp>

#include <array>
#include <limits>
#include <memory>

using namespace nx::core;

namespace
{
constexpr uint64 k_EquivalenceRecordsPerPage = 4096;
constexpr usize k_EquivalenceMaxPages = 16;
constexpr uint64 k_FillPairBatchRecords = 4096;

/** @brief Fixed-width deferred copy from one face-neighbor source tuple to one bad destination tuple. */
struct FillPair
{
  int64 destination = 0;
  int64 source = 0;
};

/**
 * @brief Creates bounded fixed-record storage for one fill iteration's destination/source pairs.
 *
 * Genuine OOC execution disallows the resident fallback so a dense defect cannot
 * silently allocate one pair per cell in RAM.
 */
Result<std::unique_ptr<ITemporaryRecordStore>> CreateFillPairStore(uint64 maximumPairs, bool allowInMemoryFallback, const std::atomic_bool& shouldCancel)
{
  if(shouldCancel || maximumPairs == std::numeric_limits<uint64>::max())
  {
    return MakeErrorResult<std::unique_ptr<ITemporaryRecordStore>>(-87022, "FillBadData candidate-pair store configuration is cancelled or overflows.");
  }
  TemporaryRecordStoreConfig config;
  config.recordSize = sizeof(FillPair);
  config.maxRecordsPerBatch = k_FillPairBatchRecords;
  config.initialRecordCount = 0;
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
  if(result.invalid())
  {
    return result;
  }
  if(result.value() == nullptr)
  {
    return MakeErrorResult<std::unique_ptr<ITemporaryRecordStore>>(-87023, "FillBadData candidate-pair provider returned a null store.");
  }
  auto resizeResult = result.value()->resize(maximumPairs, shouldCancel);
  if(resizeResult.invalid())
  {
    return ConvertInvalidResult<std::unique_ptr<ITemporaryRecordStore>>(std::move(resizeResult));
  }
  return result;
}

/** @brief Creates externally backed CCL parent/rank/size state for every possible provisional label. */
Result<std::unique_ptr<ExternalEquivalence>> CreateExternalEquivalence(uint64 maximumLabel, bool allowInMemoryFallback)
{
  if(maximumLabel == std::numeric_limits<uint64>::max())
  {
    return MakeErrorResult<std::unique_ptr<ExternalEquivalence>>(-87020, "FillBadData external equivalence label capacity overflows.");
  }
  TemporaryRecordStoreConfig config;
  config.recordSize = sizeof(ExternalEquivalence::Node);
  config.maxRecordsPerBatch = k_EquivalenceRecordsPerPage;
  config.initialRecordCount = maximumLabel + 1;
  auto storeResult = DataStoreUtilities::GetIOCollection().createTemporaryRecordStore(config);
  if(storeResult.invalid() && allowInMemoryFallback)
  {
    auto fallbackResult = InMemoryTemporaryRecordStore::Create(config);
    if(fallbackResult.invalid())
    {
      return ConvertInvalidResult<std::unique_ptr<ExternalEquivalence>>(std::move(fallbackResult));
    }
    storeResult = {std::move(fallbackResult.value())};
  }
  if(storeResult.invalid())
  {
    return ConvertInvalidResult<std::unique_ptr<ExternalEquivalence>>(std::move(storeResult));
  }
  if(storeResult.value() == nullptr)
  {
    return MakeErrorResult<std::unique_ptr<ExternalEquivalence>>(-87021, "FillBadData temporary-record provider returned a null store.");
  }
  return ExternalEquivalence::Create(std::move(storeResult.value()), k_EquivalenceRecordsPerPage, k_EquivalenceMaxPages, 0);
}

/**
 * @brief Applies deferred fill pairs to one typed cell array through a three-slice rolling window.
 *
 * A source is always face-adjacent to its destination, so previous/current/next
 * slices contain every possible copy. Pairs are emitted in Z/Y/X order; the
 * window therefore advances sequentially and each destination slice is flushed
 * once instead of issuing two random OOC tuple accesses per pair.
 */
struct SliceBufferedCopyFunctor
{
  /**
   * @brief Reads deferred pairs in fixed batches and copies every tuple component through the rolling window.
   * @return A valid result or the first pair-store, DataStore, adjacency, overflow, or cancellation error.
   */
  template <typename T>
  Result<> operator()(IDataArray* dataArray, ITemporaryRecordStore& pairStore, uint64 pairCount, usize sliceTuples, int64 sliceStride, int64 dimZ, const std::atomic_bool& shouldCancel) const
  {
    auto& store = dataArray->template getIDataStoreRefAs<AbstractDataStore<T>>();
    const usize numComps = store.getNumberOfComponents();
    if(sliceTuples == 0 || numComps == 0 || sliceTuples > std::numeric_limits<usize>::max() / numComps || sliceTuples * numComps > std::numeric_limits<usize>::max() / 3)
    {
      return MakeErrorResult(-87024, "FillBadData slice-buffer dimensions overflow.");
    }
    const usize sliceValues = sliceTuples * numComps;
    auto values = std::make_unique<T[]>(3 * sliceValues);
    std::array<FillPair, k_FillPairBatchRecords> pairs;
    int64 currentZ = -1;

    for(uint64 pairOffset = 0; pairOffset < pairCount; pairOffset += k_FillPairBatchRecords)
    {
      if(shouldCancel)
      {
        return {};
      }
      const uint64 batchCount = std::min(k_FillPairBatchRecords, pairCount - pairOffset);
      auto bytes = nonstd::span<std::byte>(reinterpret_cast<std::byte*>(pairs.data()), static_cast<usize>(batchCount) * sizeof(FillPair));
      auto readResult = pairStore.read(pairOffset, batchCount, bytes, shouldCancel);
      if(readResult.invalid())
      {
        return ConvertResult(std::move(readResult));
      }
      if(readResult.value() != batchCount)
      {
        return MakeErrorResult(-87025, "FillBadData candidate-pair provider returned a short read.");
      }
      for(uint64 pairIndex = 0; pairIndex < batchCount; pairIndex++)
      {
        const FillPair& pair = pairs[static_cast<usize>(pairIndex)];
        const int64 destinationZ = pair.destination / sliceStride;
        if(destinationZ != currentZ)
        {
          if(currentZ >= 0)
          {
            auto writeResult = store.copyFromBuffer(static_cast<usize>(currentZ) * sliceValues, nonstd::span<const T>(values.get() + sliceValues, sliceValues));
            if(writeResult.invalid())
            {
              return writeResult;
            }
          }
          currentZ = destinationZ;
          if(currentZ > 0)
          {
            auto sliceReadResult = store.copyIntoBuffer(static_cast<usize>(currentZ - 1) * sliceValues, nonstd::span<T>(values.get(), sliceValues));
            if(sliceReadResult.invalid())
            {
              return sliceReadResult;
            }
          }
          auto sliceReadResult = store.copyIntoBuffer(static_cast<usize>(currentZ) * sliceValues, nonstd::span<T>(values.get() + sliceValues, sliceValues));
          if(sliceReadResult.invalid())
          {
            return sliceReadResult;
          }
          if(currentZ + 1 < dimZ)
          {
            sliceReadResult = store.copyIntoBuffer(static_cast<usize>(currentZ + 1) * sliceValues, nonstd::span<T>(values.get() + (2 * sliceValues), sliceValues));
            if(sliceReadResult.invalid())
            {
              return sliceReadResult;
            }
          }
        }
        const usize destinationInSlice = static_cast<usize>(pair.destination - destinationZ * sliceStride);
        const int64 sourceZ = pair.source / sliceStride;
        if(sourceZ < currentZ - 1 || sourceZ > currentZ + 1)
        {
          return MakeErrorResult(-87026, "FillBadData candidate source is not face-adjacent to its destination slice.");
        }
        const usize sourceInSlice = static_cast<usize>(pair.source - sourceZ * sliceStride);
        const usize sourceSlot = sourceZ == currentZ - 1 ? 0 : sourceZ == currentZ ? 1 : 2;
        for(usize component = 0; component < numComps; component++)
        {
          values[sliceValues + destinationInSlice * numComps + component] = values[sourceSlot * sliceValues + sourceInSlice * numComps + component];
        }
      }
    }
    if(currentZ >= 0)
    {
      return store.copyFromBuffer(static_cast<usize>(currentZ) * sliceValues, nonstd::span<const T>(values.get() + sliceValues, sliceValues));
    }
    return {};
  }
};

} // namespace

// -----------------------------------------------------------------------------
// FillBadDataCCL Implementation
// -----------------------------------------------------------------------------

FillBadDataCCL::FillBadDataCCL(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel, const FillBadDataInputValues* inputValues)
: m_DataStructure(dataStructure)
, m_InputValues(inputValues)
, m_ShouldCancel(shouldCancel)
, m_MessageHandler(mesgHandler)
{
}

// -----------------------------------------------------------------------------
FillBadDataCCL::~FillBadDataCCL() noexcept = default;

// -----------------------------------------------------------------------------
const std::atomic_bool& FillBadDataCCL::getCancel() const
{
  return m_ShouldCancel;
}

// -----------------------------------------------------------------------------
// PHASE 1: Z-Slice Sequential Connected Component Labeling (CCL)
// -----------------------------------------------------------------------------
//
// This phase performs connected component labeling on bad-data voxels
// (FeatureId == 0) using a Z-slice sequential scanline algorithm. The key
// insight that makes this OOC-friendly is that scanline CCL only needs to
// check three BACKWARD neighbors (x-1, y-1, z-1), all of which have already
// been processed. This means we can process voxels in strict Z-Y-X order,
// reading each Z-slice exactly once with a bulk copyIntoBuffer call.
//
// Data structures:
// - labelBuffer: Rolling 2-slice buffer (2 * dimX * dimY int32 values).
//   Alternates between even/odd Z indices via (z % 2) to store provisional
//   labels for the current and previous Z-slice. This provides O(1) backward
//   neighbor lookups without storing labels for the entire volume.
// - unionFind: Tracks equivalences between provisional labels assigned to
//   different parts of the same connected component. When a voxel has multiple
//   differently-labeled backward neighbors, their labels are united.
// - featureIdsSlice: Temporary buffer for reading/writing one Z-slice of
//   FeatureIds. Provisional labels are written back to the FeatureIds store
//   so that Phase 3 can read them without needing a separate label volume.
//
// Label assignment:
// - Each bad-data voxel with no labeled backward neighbor gets a new
//   provisional label (nextLabel++).
// - Each bad-data voxel with one or more labeled backward neighbors inherits
//   the smallest label. If multiple differently-labeled neighbors exist,
//   they are united in the Union-Find.
// - Good-data voxels (FeatureId != 0) are skipped and retain their original
//   FeatureId values.
//
// Provisional labels start at (maxExistingFeatureId + 1) to avoid collision
// with existing good-feature IDs. This allows Phase 3 to distinguish between
// original feature IDs and CCL-assigned labels using a simple threshold check.
// -----------------------------------------------------------------------------
Result<> FillBadDataCCL::phaseOneCCL(Int32AbstractDataStore& featureIdsStore, ExternalEquivalence& equivalences, int32& nextLabel, const std::array<int64, 3>& dims) const
{
  const usize sliceSize = static_cast<usize>(dims[0]) * static_cast<usize>(dims[1]);

  // Rolling 2-slice buffer for backward neighbor label reads.
  // The scanline CCL algorithm only needs to look at three backward neighbors:
  // x-1 (same slice), y-1 (same slice), and z-1 (previous slice). So we only
  // need the current and immediately previous Z-slice labels in memory. The
  // buffer alternates between even/odd Z indices via (z % 2) indexing.
  // This gives O(dimX * dimY) memory instead of O(volume).
  std::vector<int32> labelBuffer(2 * sliceSize, 0);

  // Temporary buffer for reading/writing featureIds one Z-slice at a time
  std::vector<int32> featureIdsSlice(sliceSize);

  // Process each Z-slice sequentially
  for(usize z = 0; z < static_cast<usize>(dims[2]); z++)
  {
    if(m_ShouldCancel)
    {
      return {};
    }
    auto readResult = featureIdsStore.copyIntoBuffer(z * sliceSize, nonstd::span<int32>(featureIdsSlice.data(), sliceSize));
    if(readResult.invalid())
    {
      return readResult;
    }

    // Clear current slice in rolling label buffer for this z
    const usize curOff = (z % 2) * sliceSize;
    std::fill(labelBuffer.begin() + curOff, labelBuffer.begin() + curOff + sliceSize, 0);
    const usize prevOff = ((z + 1) % 2) * sliceSize;

    for(usize y = 0; y < static_cast<usize>(dims[1]); y++)
    {
      for(usize x = 0; x < static_cast<usize>(dims[0]); x++)
      {
        const usize inSlice = y * static_cast<usize>(dims[0]) + x;

        // Only process bad data voxels (FeatureId == 0)
        if(featureIdsSlice[inSlice] != 0)
        {
          continue;
        }

        // Check backward neighbors using rolling buffer
        int32 assignedLabel = 0;

        if(x > 0)
        {
          int32 neighLabel = labelBuffer[curOff + inSlice - 1];
          if(neighLabel > 0)
          {
            assignedLabel = neighLabel;
          }
        }

        if(y > 0)
        {
          int32 neighLabel = labelBuffer[curOff + inSlice - static_cast<usize>(dims[0])];
          if(neighLabel > 0)
          {
            if(assignedLabel == 0)
            {
              assignedLabel = neighLabel;
            }
            else if(assignedLabel != neighLabel)
            {
              auto uniteResult = equivalences.unite(static_cast<uint64>(assignedLabel), static_cast<uint64>(neighLabel), m_ShouldCancel);
              if(uniteResult.invalid())
                return uniteResult;
            }
          }
        }

        if(z > 0)
        {
          int32 neighLabel = labelBuffer[prevOff + inSlice];
          if(neighLabel > 0)
          {
            if(assignedLabel == 0)
            {
              assignedLabel = neighLabel;
            }
            else if(assignedLabel != neighLabel)
            {
              auto uniteResult = equivalences.unite(static_cast<uint64>(assignedLabel), static_cast<uint64>(neighLabel), m_ShouldCancel);
              if(uniteResult.invalid())
                return uniteResult;
            }
          }
        }

        if(assignedLabel == 0)
        {
          if(nextLabel == std::numeric_limits<int32>::max())
          {
            return MakeErrorResult(-87030, "FillBadData provisional labels exceed the Int32 FeatureIds range.");
          }
          assignedLabel = nextLabel;
          nextLabel++;
        }

        // Write the provisional label to both the rolling buffer (for
        // backward neighbor reads by subsequent voxels) and the featureIds
        // slice buffer (persisted for Phases 2-3 to read back).
        labelBuffer[curOff + inSlice] = assignedLabel;
        featureIdsSlice[inSlice] = assignedLabel;

        // Accumulate region size: each voxel contributes 1 to its label.
        // After Phase 2 flattening, sizes are aggregated to root labels
        // so we can classify regions by total voxel count.
        auto sizeResult = equivalences.addSize(static_cast<uint64>(assignedLabel), 1, m_ShouldCancel);
        if(sizeResult.invalid())
          return sizeResult;
      }
    }

    auto writeResult = featureIdsStore.copyFromBuffer(z * sliceSize, nonstd::span<const int32>(featureIdsSlice.data(), sliceSize));
    if(writeResult.invalid())
    {
      return writeResult;
    }
  }
  return equivalences.flush(m_ShouldCancel);
}

// -----------------------------------------------------------------------------
// PHASE 2: Global Resolution of Equivalences
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// PHASE 3: Region Classification and Relabeling
// -----------------------------------------------------------------------------
//
// Classifies bad data regions as "small" or "large" based on size threshold:
// - Small regions (< minAllowedDefectSize): marked with -1 for filling in Phase 4
// - Large regions (>= minAllowedDefectSize): kept as 0 (or assigned new phase)
// -----------------------------------------------------------------------------
Result<> FillBadDataCCL::phaseThreeRelabeling(Int32AbstractDataStore& featureIdsStore, Int32Array* cellPhasesPtr, int32 startLabel, int32 nextLabel, ExternalEquivalence& equivalences,
                                              usize maxPhase) const
{
  const auto& selectedImageGeom = m_DataStructure.getDataRefAs<ImageGeom>(m_InputValues->inputImageGeometry);
  const SizeVec3 udims = selectedImageGeom.getDimensions();

  // The startLabel boundary is critical: provisional CCL labels were assigned
  // starting at (maxExistingFeatureId + 1) during Phase 1, so labels in the
  // range [1, startLabel) are original good feature IDs that must NOT be
  // touched. Only labels in [startLabel, nextLabel) are CCL-assigned bad-data
  // region labels that need classification and relabeling.
  // Temporary buffer for reading/writing featureIds one Z-slice at a time
  std::vector<int32> sliceData(static_cast<usize>(udims[0]) * static_cast<usize>(udims[1]));
  const usize sliceSize = sliceData.size();

  // Optional cellPhases buffer for bulk read/write (avoids per-element OOC access)
  const bool needPhasesBuffer = m_InputValues->storeAsNewPhase && cellPhasesPtr != nullptr;
  std::vector<int32> phasesSlice;
  Int32AbstractDataStore* cellPhasesStorePtr = nullptr;
  if(needPhasesBuffer)
  {
    phasesSlice.resize(sliceSize);
    cellPhasesStorePtr = &cellPhasesPtr->getDataStoreRef();
  }

  // Read provisional labels from featureIds store (written during Phase 1)
  // and relabel based on region classification.
  // Only voxels with label >= startLabel are provisional CCL labels (bad data).
  // Voxels with label in [1, startLabel) are original good feature IDs — leave them alone.
  for(usize z = 0; z < udims[2]; z++)
  {
    if(m_ShouldCancel)
    {
      return {};
    }
    auto readResult = featureIdsStore.copyIntoBuffer(z * sliceSize, nonstd::span<int32>(sliceData.data(), sliceSize));
    if(readResult.invalid())
      return readResult;

    bool phasesModified = false;
    if(needPhasesBuffer)
    {
      auto phaseReadResult = cellPhasesStorePtr->copyIntoBuffer(z * sliceSize, nonstd::span<int32>(phasesSlice.data(), sliceSize));
      if(phaseReadResult.invalid())
        return phaseReadResult;
    }

    for(usize y = 0; y < udims[1]; y++)
    {
      for(usize x = 0; x < udims[0]; x++)
      {
        const usize inSlice = y * udims[0] + x;

        int32 label = sliceData[inSlice];
        if(label >= startLabel)
        {
          auto rootResult = equivalences.find(static_cast<uint64>(label), m_ShouldCancel);
          if(rootResult.invalid())
            return ConvertResult(std::move(rootResult));
          auto sizeResult = equivalences.componentSize(rootResult.value(), m_ShouldCancel);
          if(sizeResult.invalid())
            return ConvertResult(std::move(sizeResult));

          if(sizeResult.value() < static_cast<uint64>(m_InputValues->minAllowedDefectSizeValue))
          {
            sliceData[inSlice] = -1;
          }
          else
          {
            sliceData[inSlice] = 0;

            if(needPhasesBuffer)
            {
              phasesSlice[inSlice] = static_cast<int32>(maxPhase) + 1;
              phasesModified = true;
            }
          }
        }
      }
    }

    auto writeResult = featureIdsStore.copyFromBuffer(z * sliceSize, nonstd::span<const int32>(sliceData.data(), sliceSize));
    if(writeResult.invalid())
      return writeResult;
    if(phasesModified)
    {
      auto phaseWriteResult = cellPhasesStorePtr->copyFromBuffer(z * sliceSize, nonstd::span<const int32>(phasesSlice.data(), sliceSize));
      if(phaseWriteResult.invalid())
        return phaseWriteResult;
    }
  }
  return {};
}

// -----------------------------------------------------------------------------
// PHASE 4: Iterative Morphological Fill (External-Record Deferred)
// -----------------------------------------------------------------------------
//
// Uses a temporary record store to avoid O(N) memory allocations. Each iteration:
//   Pass 1 (Vote): Scan voxels using a 3-slice rolling window. For each -1 voxel,
//     find the best positive-featureId neighbor via majority vote. Write (dest, src)
//     pairs to external records. featureIds is read-only during this pass.
//   Pass 2 (Apply): Read bounded record batches back. Copy all cell data array
//     components from src to dest. Update featureIds last.
// -----------------------------------------------------------------------------
Result<> FillBadDataCCL::phaseFourIterativeFill(Int32AbstractDataStore& featureIdsStore, const std::array<int64, 3>& dims, usize numFeatures, bool allowInMemoryFallback) const
{
  const auto& selectedImageGeom = m_DataStructure.getDataRefAs<ImageGeom>(m_InputValues->inputImageGeometry);

  // Feature vote counter: O(features) not O(voxels)
  std::vector<int32> featureNumber(numFeatures + 1, 0);

  // Get cell arrays that need updating during filling
  std::optional<std::vector<DataPath>> allChildArrays = GetAllChildDataPaths(m_DataStructure, selectedImageGeom.getCellDataPath(), DataObject::Type::DataArray, m_InputValues->ignoredDataArrayPaths);
  std::vector<DataPath> voxelArrayNames;
  if(allChildArrays.has_value())
  {
    voxelArrayNames = allChildArrays.value();
  }

  auto pairStoreResult = CreateFillPairStore(featureIdsStore.getNumberOfTuples(), allowInMemoryFallback, m_ShouldCancel);
  if(pairStoreResult.invalid())
  {
    return ConvertResult(std::move(pairStoreResult));
  }
  auto& pairStore = *pairStoreResult.value();
  std::array<FillPair, k_FillPairBatchRecords> pairBatch;

  usize count = 1;
  usize iteration = 0;
  usize pairsWritten = 0;

  const usize sliceSize = static_cast<usize>(dims[0]) * static_cast<usize>(dims[1]);

  while(count != 0)
  {
    iteration++;
    count = 0;

    pairsWritten = 0;
    usize batchCount = 0;

    // Pass 1 (Vote): Z-slice scan using a 3-slice rolling window, writing
    // (dest, src) pairs to temporary records. featureIds is read-only during this
    // pass — two-pass semantics are automatic.
    std::vector<int32> prevSlice(sliceSize, 0);
    std::vector<int32> curSlice(sliceSize);
    std::vector<int32> nextSlice(sliceSize, 0);

    auto sliceReadResult = featureIdsStore.copyIntoBuffer(0, nonstd::span<int32>(curSlice.data(), sliceSize));
    if(sliceReadResult.invalid())
    {
      return sliceReadResult;
    }
    if(dims[2] > 1)
    {
      sliceReadResult = featureIdsStore.copyIntoBuffer(sliceSize, nonstd::span<int32>(nextSlice.data(), sliceSize));
      if(sliceReadResult.invalid())
      {
        return sliceReadResult;
      }
    }

    for(int64 z = 0; z < dims[2]; z++)
    {
      if(m_ShouldCancel)
      {
        return {};
      }

      for(int64 y = 0; y < dims[1]; y++)
      {
        for(int64 x = 0; x < dims[0]; x++)
        {
          const usize inSlice = static_cast<usize>(y) * static_cast<usize>(dims[0]) + static_cast<usize>(x);
          int32 featureName = curSlice[inSlice];

          if(featureName < 0)
          {
            count++;
            int32 most = 0;
            int64 bestNeighbor = -1;

            // Check 6 face neighbors using the legacy ordering so ties select
            // the same source tuple as the in-core algorithm.
            // -Z neighbor
            if(z > 0)
            {
              int32 feature = prevSlice[inSlice];
              if(feature > 0)
              {
                featureNumber[feature]++;
                if(featureNumber[feature] > most)
                {
                  most = featureNumber[feature];
                  bestNeighbor = (z - 1) * dims[0] * dims[1] + y * dims[0] + x;
                }
              }
            }
            // -Y neighbor
            if(y > 0)
            {
              int32 feature = curSlice[inSlice - static_cast<usize>(dims[0])];
              if(feature > 0)
              {
                featureNumber[feature]++;
                if(featureNumber[feature] > most)
                {
                  most = featureNumber[feature];
                  bestNeighbor = z * dims[0] * dims[1] + (y - 1) * dims[0] + x;
                }
              }
            }
            // -X neighbor
            if(x > 0)
            {
              int32 feature = curSlice[inSlice - 1];
              if(feature > 0)
              {
                featureNumber[feature]++;
                if(featureNumber[feature] > most)
                {
                  most = featureNumber[feature];
                  bestNeighbor = z * dims[0] * dims[1] + y * dims[0] + (x - 1);
                }
              }
            }
            // +X neighbor
            if(x < dims[0] - 1)
            {
              int32 feature = curSlice[inSlice + 1];
              if(feature > 0)
              {
                featureNumber[feature]++;
                if(featureNumber[feature] > most)
                {
                  most = featureNumber[feature];
                  bestNeighbor = z * dims[0] * dims[1] + y * dims[0] + (x + 1);
                }
              }
            }
            // +Y neighbor
            if(y < dims[1] - 1)
            {
              int32 feature = curSlice[inSlice + static_cast<usize>(dims[0])];
              if(feature > 0)
              {
                featureNumber[feature]++;
                if(featureNumber[feature] > most)
                {
                  most = featureNumber[feature];
                  bestNeighbor = z * dims[0] * dims[1] + (y + 1) * dims[0] + x;
                }
              }
            }
            // +Z neighbor
            if(z < dims[2] - 1)
            {
              int32 feature = nextSlice[inSlice];
              if(feature > 0)
              {
                featureNumber[feature]++;
                if(featureNumber[feature] > most)
                {
                  most = featureNumber[feature];
                  bestNeighbor = (z + 1) * dims[0] * dims[1] + y * dims[0] + x;
                }
              }
            }

            // Reset vote counters by re-visiting only the neighbors that
            // were actually incremented above. This sets featureNumber[feature]
            // back to 0 for each neighbor's feature, avoiding the need to zero
            // the entire featureNumber vector (which would be O(numFeatures)
            // per voxel). Since at most 6 neighbors are visited, this reset
            // is O(1) per voxel.
            if(x > 0)
            {
              int32 f = curSlice[inSlice - 1];
              if(f > 0)
              {
                featureNumber[f] = 0;
              }
            }
            if(x < dims[0] - 1)
            {
              int32 f = curSlice[inSlice + 1];
              if(f > 0)
              {
                featureNumber[f] = 0;
              }
            }
            if(y > 0)
            {
              int32 f = curSlice[inSlice - static_cast<usize>(dims[0])];
              if(f > 0)
              {
                featureNumber[f] = 0;
              }
            }
            if(y < dims[1] - 1)
            {
              int32 f = curSlice[inSlice + static_cast<usize>(dims[0])];
              if(f > 0)
              {
                featureNumber[f] = 0;
              }
            }
            if(z > 0)
            {
              int32 f = prevSlice[inSlice];
              if(f > 0)
              {
                featureNumber[f] = 0;
              }
            }
            if(z < dims[2] - 1)
            {
              int32 f = nextSlice[inSlice];
              if(f > 0)
              {
                featureNumber[f] = 0;
              }
            }

            // Write (dest, src) pair to the external record store if a valid neighbor was found
            if(bestNeighbor >= 0)
            {
              pairBatch[batchCount++] = {z * dims[0] * dims[1] + y * dims[0] + x, bestNeighbor};
              if(batchCount == pairBatch.size())
              {
                auto writeResult =
                    pairStore.write(pairsWritten, batchCount, nonstd::span<const std::byte>(reinterpret_cast<const std::byte*>(pairBatch.data()), batchCount * sizeof(FillPair)), m_ShouldCancel);
                if(writeResult.invalid())
                {
                  return writeResult;
                }
                pairsWritten += batchCount;
                batchCount = 0;
              }
            }
          }
        }
      }

      // Shift 3-slice window forward
      std::swap(prevSlice, curSlice);
      std::swap(curSlice, nextSlice);
      if(z + 2 < dims[2])
      {
        sliceReadResult = featureIdsStore.copyIntoBuffer(static_cast<usize>(z + 2) * sliceSize, nonstd::span<int32>(nextSlice.data(), sliceSize));
        if(sliceReadResult.invalid())
        {
          return sliceReadResult;
        }
      }
    }

    if(batchCount != 0)
    {
      auto writeResult = pairStore.write(pairsWritten, batchCount, nonstd::span<const std::byte>(reinterpret_cast<const std::byte*>(pairBatch.data()), batchCount * sizeof(FillPair)), m_ShouldCancel);
      if(writeResult.invalid())
      {
        return writeResult;
      }
      pairsWritten += batchCount;
    }

    if(count == 0)
    {
      break;
    }

    if(pairsWritten == 0)
    {
      m_MessageHandler({IFilter::Message::Type::Warning,
                        fmt::format("{} bad-data voxel(s) could not be filled because they have no adjacent good-data neighbor. Stopping after {} iteration(s).", count, iteration)});
      break;
    }

    // Pass 2 (Apply): replay the bounded temporary-record batches separately
    // for FeatureIds and each sibling cell array.  Every replay sees the exact
    // original Z-Y-X candidate order, while keeping only three slices and one
    // 4,096-record batch resident.
    const int64 sliceStride = dims[0] * dims[1];
    const usize sliceTuples = sliceSize;
    auto featureIdsResult = ExecuteDataFunction(SliceBufferedCopyFunctor{}, m_DataStructure.getDataAs<IDataArray>(m_InputValues->featureIdsArrayPath)->getDataType(),
                                                m_DataStructure.getDataAs<IDataArray>(m_InputValues->featureIdsArrayPath), pairStore, pairsWritten, sliceTuples, sliceStride, dims[2], m_ShouldCancel);
    if(featureIdsResult.invalid())
    {
      return featureIdsResult;
    }

    // Apply non-featureIds cell array fills, one array at a time
    for(const auto& cellArrayPath : voxelArrayNames)
    {
      if(cellArrayPath == m_InputValues->featureIdsArrayPath)
      {
        continue;
      }
      auto* cellArray = m_DataStructure.getDataAs<IDataArray>(cellArrayPath);
      if(m_ShouldCancel)
      {
        return {};
      }
      auto fillResult = ExecuteDataFunction(SliceBufferedCopyFunctor{}, cellArray->getDataType(), cellArray, pairStore, pairsWritten, sliceTuples, sliceStride, dims[2], m_ShouldCancel);
      if(fillResult.invalid())
      {
        return fillResult;
      }
    }
    featureIdsStore.flush();
  }

  m_MessageHandler({IFilter::Message::Type::Info, fmt::format("  Completed in {} iteration{}", iteration, iteration == 1 ? "" : "s")});
  return {};
}

// -----------------------------------------------------------------------------
// Main Algorithm Entry Point -- Orchestrates Phases 1-4
// -----------------------------------------------------------------------------
// This method performs the initial setup (finding max feature ID and max phase
// via chunked bulk scans), initializes external equivalence state, and then calls each
// phase method sequentially. The chunked scans use a Z-slice-sized buffer
// (dimX * dimY) to read feature IDs and phases in bulk, avoiding per-element
// OOC access during the setup phase.
// -----------------------------------------------------------------------------
Result<> FillBadDataCCL::operator()()
{
  auto& featureIdsStore = m_DataStructure.getDataAs<Int32Array>(m_InputValues->featureIdsArrayPath)->getDataStoreRef();
  const auto& selectedImageGeom = m_DataStructure.getDataRefAs<ImageGeom>(m_InputValues->inputImageGeometry);
  const SizeVec3 udims = selectedImageGeom.getDimensions();

  std::array<int64, 3> dims = {
      static_cast<int64>(udims[0]),
      static_cast<int64>(udims[1]),
      static_cast<int64>(udims[2]),
  };

  const usize totalPoints = featureIdsStore.getNumberOfTuples();
  if(totalPoints == 0)
  {
    return MakeErrorResult(-87027, "FillBadData requires at least one image cell.");
  }

  // Get cell phases array if we need to assign large regions to a new phase
  Int32Array* cellPhasesPtr = nullptr;
  usize maxPhase = 0;

  if(m_InputValues->storeAsNewPhase)
  {
    cellPhasesPtr = m_DataStructure.getDataAs<Int32Array>(m_InputValues->cellPhasesArrayPath);
  }

  // Chunked scan: find max feature ID and optionally max phase using bulk reads
  usize numFeatures = 0;
  constexpr usize k_ScanBatchSize = 65536;
  std::vector<int32> scanBuffer(k_ScanBatchSize);
  for(usize offset = 0; offset < totalPoints; offset += k_ScanBatchSize)
  {
    const usize batchSize = std::min(k_ScanBatchSize, totalPoints - offset);
    auto readResult = featureIdsStore.copyIntoBuffer(offset, nonstd::span<int32>(scanBuffer.data(), batchSize));
    if(readResult.invalid())
    {
      return readResult;
    }
    for(usize i = 0; i < batchSize; i++)
    {
      if(scanBuffer[i] > 0 && static_cast<usize>(scanBuffer[i]) > numFeatures)
      {
        numFeatures = static_cast<usize>(scanBuffer[i]);
      }
    }
  }
  // Separate chunked scan for cellPhases if needed
  if(cellPhasesPtr != nullptr)
  {
    auto& cellPhasesStore = cellPhasesPtr->getDataStoreRef();
    for(usize offset = 0; offset < totalPoints; offset += k_ScanBatchSize)
    {
      const usize batchSize = std::min(k_ScanBatchSize, totalPoints - offset);
      auto readResult = cellPhasesStore.copyIntoBuffer(offset, nonstd::span<int32>(scanBuffer.data(), batchSize));
      if(readResult.invalid())
      {
        return readResult;
      }
      for(usize i = 0; i < batchSize; i++)
      {
        if(static_cast<usize>(scanBuffer[i]) > maxPhase)
        {
          maxPhase = scanBuffer[i];
        }
      }
    }
  }

  // Initialize data structures for connected component labeling.
  // Start provisional labels AFTER the max existing feature ID to avoid collisions.
  // Existing feature IDs are in [1, numFeatures], so provisional labels start at numFeatures+1.
  if(numFeatures >= static_cast<usize>(std::numeric_limits<int32>::max()))
  {
    return MakeErrorResult(-87028, "FillBadData cannot allocate a provisional label beyond the Int32 FeatureIds range.");
  }
  const int32 startLabel = static_cast<int32>(numFeatures) + 1;
  int32 nextLabel = startLabel;
  if(totalPoints > std::numeric_limits<usize>::max() - static_cast<usize>(startLabel))
  {
    return MakeErrorResult(-87029, "FillBadData external equivalence capacity overflows the platform index type.");
  }
  bool usesOutOfCoreStore = featureIdsStore.getStoreType() == IDataStore::StoreType::OutOfCore;
  if(cellPhasesPtr != nullptr)
  {
    usesOutOfCoreStore = usesOutOfCoreStore || cellPhasesPtr->getDataStoreRef().getStoreType() == IDataStore::StoreType::OutOfCore;
  }
  const auto childPaths = GetAllChildDataPaths(m_DataStructure, selectedImageGeom.getCellDataPath(), DataObject::Type::DataArray, m_InputValues->ignoredDataArrayPaths);
  if(childPaths.has_value())
  {
    for(const DataPath& path : childPaths.value())
    {
      const auto* array = m_DataStructure.getDataAs<IDataArray>(path);
      usesOutOfCoreStore = usesOutOfCoreStore || (array != nullptr && IsOutOfCore(*array));
    }
  }
  const bool allowInMemoryFallback = !usesOutOfCoreStore;
  auto equivalencesResult = CreateExternalEquivalence(totalPoints + static_cast<usize>(startLabel), allowInMemoryFallback);
  if(equivalencesResult.invalid())
  {
    return ConvertResult(std::move(equivalencesResult));
  }

  // Phase 1: Z-Slice Sequential Connected Component Labeling
  // Uses a 2-slice rolling buffer (O(slice) memory) for backward neighbor reads.
  // Writes provisional labels to featureIds store for Phases 2-3.
  m_MessageHandler({IFilter::Message::Type::Info, "Phase 1/4: Labeling connected components..."});
  auto phaseOneResult = phaseOneCCL(featureIdsStore, *equivalencesResult.value(), nextLabel, dims);
  if(phaseOneResult.invalid())
  {
    return phaseOneResult;
  }

  // Phase 3: Relabeling based on region size classification
  // Reads provisional labels from featureIds store (written during Phase 1)
  m_MessageHandler({IFilter::Message::Type::Info, "Phase 3/4: Classifying region sizes..."});
  auto phaseThreeResult = phaseThreeRelabeling(featureIdsStore, cellPhasesPtr, startLabel, nextLabel, *equivalencesResult.value(), maxPhase);
  if(phaseThreeResult.invalid())
  {
    return phaseThreeResult;
  }

  // Phase 4: Iterative morphological fill
  m_MessageHandler({IFilter::Message::Type::Info, "Phase 4/4: Filling small defects..."});
  auto result = phaseFourIterativeFill(featureIdsStore, dims, numFeatures, allowInMemoryFallback);
  return result;
}
