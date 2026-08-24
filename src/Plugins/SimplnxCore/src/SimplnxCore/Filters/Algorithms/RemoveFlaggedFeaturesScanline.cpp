#include "RemoveFlaggedFeaturesScanline.hpp"

#include "RemoveFlaggedFeatures.hpp"

#include "SimplnxCore/Filters/ComputeFeatureRectFilter.hpp"
#include "SimplnxCore/Filters/CropImageGeometryFilter.hpp"

#include "simplnx/DataStructure/DataArray.hpp"
#include "simplnx/DataStructure/Geometry/ImageGeom.hpp"
#include "simplnx/Utilities/DataGroupUtilities.hpp"
#include "simplnx/Utilities/FilterUtilities.hpp"
#include "simplnx/Utilities/MaskCompareUtilities.hpp"
#include "simplnx/Utilities/MessageHelper.hpp"
#include "simplnx/Utilities/ParallelTaskAlgorithm.hpp"

#include <nonstd/span.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <stdexcept>

using namespace nx::core;

// ----------------------------------------------------------------------------
// RemoveFlaggedFeaturesScanline -- Out-of-Core Algorithm
//
// Produces the same output as RemoveFlaggedFeaturesDirect. All FeatureIds and
// companion cell-array access is done through sequential, chunk-aligned bulk
// I/O (copyIntoBuffer/copyFromBuffer) rather than per-voxel operator[]/copyTuple, so that out-of-core
// (chunked) storage is served with large sequential reads/writes instead of
// scattered single-element accesses.
//
// See RemoveFlaggedFeaturesScanline.hpp for the rolling-window / deferred-write
// design that keeps the "fill removed features" pass bounded to O(slice) memory.
// ----------------------------------------------------------------------------

namespace
{
/**
 * @brief Fused, rolling-window equivalent of RemoveFlaggedFeaturesDirect's
 * IdentifyNeighbors + FindVoxelArrays for a single convergence iteration. For every
 * voxel whose FeatureId is <= 0, tallies its (up to 6) face-connected neighbors and,
 * for voxels whose FeatureId is strictly negative (i.e. actually flagged for removal),
 * commits the majority Feature's data into that voxel across every kept cell array.
 *
 * @section why_rolling_window Why a rolling window
 * FeatureIds votes are read one Z-slice at a time via copyIntoBuffer into three buffers
 * (prevSlice/curSlice/nextSlice), exactly as ComputeBoundaryCellsScanline does. This
 * avoids per-voxel operator[] on the (potentially out-of-core) FeatureIds store, which
 * would otherwise trigger a chunk load for nearly every voxel -- especially costly for
 * the +/-Z neighbors, which are a full Z-slice away in the flat index space.
 *
 * @section bounded_marks Bounded per-slice marks
 * Every vote computed here targets the CURRENT voxel's own slot only -- this algorithm
 * never writes a mark into a neighbor's slot (unlike ErodeDilateBadData's dilate mode).
 * That means slice z's marks are fully resolved the instant z's XY scan completes, so
 * only two O(sliceSize) buffers are needed: curMarks (being filled for the slice just
 * scanned) and prevMarks (fully resolved, awaiting commit). The commit for prevMarks is
 * deferred by one Z-slice -- mirroring the sibling rolling-window algorithms
 * (ErodeDilateBadData, FillBadDataCCL) -- purely so the read (vote) side of the
 * algorithm never has to reason about a write that raced ahead of it: by the time
 * prevMarks is committed, every vote that could ever have read that slice's original
 * FeatureIds value has already been computed from the in-memory rolling window, which
 * never re-reads a slice from disk after it has advanced past it. Writes therefore stay
 * strictly behind the slice the read frontier still needs, with memory bounded to two
 * slice-sized buffers regardless of the dataset's total voxel count.
 *
 * @section tie_breaking Tie-breaking fidelity
 * The 6 face neighbors are checked in the SAME order as the Direct algorithm
 * (-Z, -Y, -X, +X, +Y, +Z, matching NeighborUtilities' initializeFaceNeighborInternalIdx
 * for Image3D) using the SAME discovered-features/hit-count bookkeeping, so that ties
 * (multiple candidate Features reaching the same vote count) resolve identically: the
 * first candidate to reach a given count wins and is not displaced by a later candidate
 * that only ties it.
 *
 * @param imageGeom Geometry defining the voxel grid dimensions.
 * @param featureIdsStore FeatureIds data store (read for votes; written via voxelArrays).
 * @param voxelArrays Every kept cell-level array (including FeatureIds itself, unless
 *   the user explicitly ignored it) that must receive the winning neighbor's data.
 * @param shouldCancel Cooperative cancellation flag, checked once per Z-slice.
 * @param messageHelper Used to build a throttled progress messenger.
 * @return true if any voxel with FeatureId <= 0 was found (mirrors the Direct
 *   algorithm's "shouldLoop" result, including background/id==0 voxels that are
 *   never actually filled -- see the class-level Doxygen comment).
 */
struct TransferMarkedSlice
{
  /** @brief Applies one destination slice's source marks to every component of one typed cell array. */
  template <typename T>
  Result<> operator()(IDataArray& dataArray, const std::vector<int64>& marks, usize sliceSize, usize destinationZ, usize dimZ, const std::atomic_bool& shouldCancel) const
  {
    if(shouldCancel)
    {
      return {};
    }
    auto& store = dynamic_cast<DataArray<T>&>(dataArray).getDataStoreRef();
    const usize components = store.getNumberOfComponents();
    if(components == 0 || sliceSize > std::numeric_limits<usize>::max() / components)
    {
      return MakeErrorResult(-45435, "RemoveFlaggedFeatures slice transfer has an invalid component shape.");
    }
    const usize valuesPerSlice = sliceSize * components;
    auto destination = std::make_unique<T[]>(valuesPerSlice);
    auto readResult = store.copyIntoBuffer(destinationZ * valuesPerSlice, nonstd::span<T>(destination.get(), valuesPerSlice));
    if(readResult.invalid())
    {
      return readResult;
    }
    std::array<std::unique_ptr<T[]>, 3> sources;
    std::array<bool, 3> loaded = {false, false, false};
    const auto loadSource = [&](usize slot, usize sourceZ) -> Result<> {
      if(loaded[slot])
      {
        return {};
      }
      if(sourceZ >= dimZ)
      {
        return MakeErrorResult(-45436, "RemoveFlaggedFeatures source slice is outside the image geometry.");
      }
      sources[slot] = std::make_unique<T[]>(valuesPerSlice);
      auto result = store.copyIntoBuffer(sourceZ * valuesPerSlice, nonstd::span<T>(sources[slot].get(), valuesPerSlice));
      if(result.valid())
      {
        loaded[slot] = true;
      }
      return result;
    };

    bool modified = false;
    for(usize tuple = 0; tuple < sliceSize; tuple++)
    {
      const int64 sourceIndex = marks[tuple];
      if(sourceIndex < 0)
      {
        continue;
      }
      const usize sourceTuple = static_cast<usize>(sourceIndex);
      const usize sourceZ = sourceTuple / sliceSize;
      const usize sourceInSlice = sourceTuple % sliceSize;
      const usize sourceSlot = sourceZ < destinationZ ? 0 : (sourceZ > destinationZ ? 2 : 1);
      auto sourceResult = loadSource(sourceSlot, sourceZ);
      if(sourceResult.invalid())
      {
        return sourceResult;
      }
      std::copy_n(sources[sourceSlot].get() + sourceInSlice * components, components, destination.get() + tuple * components);
      modified = true;
    }
    if(shouldCancel || !modified)
    {
      return {};
    }
    return store.copyFromBuffer(destinationZ * valuesPerSlice, nonstd::span<const T>(destination.get(), valuesPerSlice));
  }
};

/** @brief Dispatches a runtime cell-array type to the single-slice buffered transfer. */
Result<> TransferMarkedSliceForArray(IDataArray& dataArray, const std::vector<int64>& marks, usize sliceSize, usize destinationZ, usize dimZ, const std::atomic_bool& shouldCancel)
{
  return ExecuteDataFunction(TransferMarkedSlice{}, dataArray.getDataType(), dataArray, marks, sliceSize, destinationZ, dimZ, shouldCancel);
}

/**
 * @brief Selects and applies majority face-neighbor fills with a three-slice Feature-ID window.
 *
 * Commits lag the read frontier by one slice, ensuring every vote in an iteration
 * observes the same pre-iteration Feature IDs while all companion arrays receive
 * the identical source tuple.
 */
Result<bool> IdentifyAndFillNeighborsScanline(const ImageGeom& imageGeom, Int32AbstractDataStore& featureIdsStore, const std::vector<std::shared_ptr<IDataArray>>& voxelArrays,
                                              const std::atomic_bool& shouldCancel, MessageHelper& messageHelper)
{
  ThrottledMessenger throttledMessenger = messageHelper.createThrottledMessenger();

  const SizeVec3 uDims = imageGeom.getDimensions();
  const int64 dimX = static_cast<int64>(uDims[0]);
  const int64 dimY = static_cast<int64>(uDims[1]);
  const int64 dimZ = static_cast<int64>(uDims[2]);
  const usize sliceSize = static_cast<usize>(dimX) * static_cast<usize>(dimY);
  const usize dimZUnsigned = static_cast<usize>(dimZ);

  // 3-slot rolling window for FeatureIds votes: prevSlice = z-1, curSlice = z, nextSlice = z+1.
  // Populated via in-memory std::swap plus a single disk read per Z-slice (the "ahead"
  // read below); a slice is never re-read from disk once the window has advanced past
  // it, so votes always see the pre-iteration FeatureIds state regardless of what has
  // since been committed to disk for earlier slices.
  std::vector<int32> prevSlice(sliceSize);
  std::vector<int32> curSlice(sliceSize);
  std::vector<int32> nextSlice(sliceSize);

  if(dimZ == 0)
  {
    return {false};
  }
  auto initialRead = featureIdsStore.copyIntoBuffer(0, nonstd::span<int32>(curSlice.data(), sliceSize));
  if(initialRead.invalid())
  {
    return ConvertInvalidResult<bool>(std::move(initialRead));
  }
  if(dimZ > 1)
  {
    auto nextRead = featureIdsStore.copyIntoBuffer(sliceSize, nonstd::span<int32>(nextSlice.data(), sliceSize));
    if(nextRead.invalid())
    {
      return ConvertInvalidResult<bool>(std::move(nextRead));
    }
  }

  // Per-slice destination marks: O(sliceSize) instead of O(n_cells). curMarks[inSlice] is
  // the global flat index of the winning neighbor for the voxel currently being scanned,
  // or -1. prevMarks holds the previous slice's fully-resolved marks, awaiting commit.
  std::vector<int64> curMarks(sliceSize, -1);
  std::vector<int64> prevMarks(sliceSize, -1);

  bool shouldLoop = false;

  auto progressIncrement = dimZ / 100;
  usize progressCounter = 0;

  // Commits one fully-resolved Z-slice of marks across every kept cell-level array via
  // the local typed bulk transfer. It reads a destination slice per array, but only
  // writes it back when at least one voxel in the slice needs filling.
  auto commitSlice = [&](usize z, const std::vector<int64>& marks) -> Result<> {
    for(const auto& voxelArray : voxelArrays)
    {
      auto result = TransferMarkedSliceForArray(*voxelArray, marks, sliceSize, z, dimZUnsigned, shouldCancel);
      if(result.invalid())
      {
        return result;
      }
    }
    return {};
  };

  for(int64 zIdx = 0; zIdx < dimZ; zIdx++)
  {
    if(shouldCancel)
    {
      return {false};
    }

    if(progressCounter > progressIncrement)
    {
      throttledMessenger.sendThrottledMessage([&]() { return fmt::format("Processing Image... {:.2f}%", CalculatePercentComplete(zIdx, dimZ)); });
      progressCounter = 0;
    }
    progressCounter++;

    std::fill(curMarks.begin(), curMarks.end(), -1);

    const int64 kStride = dimX * dimY * zIdx;
    for(int64 yIdx = 0; yIdx < dimY; yIdx++)
    {
      const int64 rowOffset = yIdx * dimX;
      for(int64 xIdx = 0; xIdx < dimX; xIdx++)
      {
        const int64 sliceIndex = rowOffset + xIdx;
        const int64 voxelIndex = kStride + sliceIndex;
        const int32 featureName = curSlice[sliceIndex];
        if(featureName > 0)
        {
          continue;
        }
        shouldLoop = true;

        int32 current = 0;
        int32 most = 0;
        std::array<int32, 6> numHits = {0, 0, 0, 0, 0, 0};
        std::array<int32, 6> discoveredFeatures = {0, 0, 0, 0, 0, 0};
        usize discoveredFeatureCount = 0;

        // Mirrors IdentifyNeighbors' inner discovery loop exactly, but sources the
        // candidate feature value from the rolling-window buffers instead of a
        // per-voxel OOC store read, and only persists the winning global index when
        // featureName is strictly negative (the commit step never copies into
        // background/id==0 voxels, even though they are tallied into shouldLoop above).
        auto considerNeighbor = [&](int32 feature, int64 neighborGlobalIndex) {
          if(feature < 0)
          {
            return;
          }
          for(usize featIndex = 0; featIndex < discoveredFeatureCount; featIndex++)
          {
            if(discoveredFeatures[featIndex] == feature)
            {
              numHits[featIndex]++;
              current = numHits[featIndex];
              if(current > most)
              {
                most = current;
                if(featureName < 0)
                {
                  curMarks[sliceIndex] = neighborGlobalIndex;
                }
              }
              return;
            }
          }
          discoveredFeatures[discoveredFeatureCount] = feature;
          discoveredFeatureCount++;
        };

        // Check the 6 face neighbors in the same order as the Direct algorithm:
        // -Z, -Y, -X, +X, +Y, +Z.
        if(zIdx > 0)
        {
          considerNeighbor(prevSlice[sliceIndex], voxelIndex - dimX * dimY);
        }
        if(yIdx > 0)
        {
          considerNeighbor(curSlice[sliceIndex - dimX], voxelIndex - dimX);
        }
        if(xIdx > 0)
        {
          considerNeighbor(curSlice[sliceIndex - 1], voxelIndex - 1);
        }
        if(xIdx < dimX - 1)
        {
          considerNeighbor(curSlice[sliceIndex + 1], voxelIndex + 1);
        }
        if(yIdx < dimY - 1)
        {
          considerNeighbor(curSlice[sliceIndex + dimX], voxelIndex + dimX);
        }
        if(zIdx < dimZ - 1)
        {
          considerNeighbor(nextSlice[sliceIndex], voxelIndex + dimX * dimY);
        }
      }
    }

    // Commit the previous slice (z-1) now that this slice's scan is done. prevMarks was
    // already fully resolved at the end of the previous iteration (this algorithm never
    // writes into a neighbor's mark slot), so deferring by exactly one slice is enough to
    // mirror the sibling rolling-window algorithms' write-behind-the-read-frontier shape.
    if(zIdx > 0)
    {
      auto commitResult = commitSlice(static_cast<usize>(zIdx - 1), prevMarks);
      if(commitResult.invalid())
      {
        return ConvertInvalidResult<bool>(std::move(commitResult));
      }
    }
    std::swap(prevMarks, curMarks);

    // Rotate the rolling window forward: prevSlice <- curSlice <- nextSlice, then load
    // the next-next Z-slice into the freed buffer (the only disk read this iteration).
    std::swap(prevSlice, curSlice);
    std::swap(curSlice, nextSlice);
    if(zIdx + 2 < dimZ)
    {
      auto readResult = featureIdsStore.copyIntoBuffer(static_cast<usize>(zIdx + 2) * sliceSize, nonstd::span<int32>(nextSlice.data(), sliceSize));
      if(readResult.invalid())
      {
        return ConvertInvalidResult<bool>(std::move(readResult));
      }
    }
  }

  // Flush the final slice's marks: after the loop exits, prevMarks holds slice dimZ-1's
  // fully-resolved marks (from the last rotation) but they have not yet been committed.
  if(dimZ > 0)
  {
    auto commitResult = commitSlice(static_cast<usize>(dimZ - 1), prevMarks);
    if(commitResult.invalid())
    {
      return ConvertInvalidResult<bool>(std::move(commitResult));
    }
  }

  return {shouldLoop};
}

/**
 * @brief Chunked bulk-I/O equivalent of RemoveFlaggedFeaturesDirect's FlagFeatures.
 *
 * Marks inactive Features' voxels for removal (-1 if they will be filled back in later,
 * 0 otherwise), reading and writing FeatureIds in fixed-size chunks via
 * copyIntoBuffer/copyFromBuffer instead of per-voxel operator[]. activeObjects is
 * feature-level (typically thousands of entries at most), so the per-feature isTrue()
 * scan above the chunk loop is not an OOC concern.
 *
 * @param featureIdsStore FeatureIds data store (read and written in place).
 * @param flaggedFeatures Per-feature removal flags.
 * @param fillRemovedFeatures When true, removed voxels are marked -1 (to be filled back
 *   in later); when false, they are marked 0 (permanently removed background).
 * @return Per-feature active flags, or an empty vector if every Feature would be removed.
 */
Result<std::vector<bool>> FlagFeaturesScanline(Int32AbstractDataStore& featureIdsStore, std::unique_ptr<MaskCompareUtilities::MaskCompare>& flaggedFeatures, const bool fillRemovedFeatures,
                                               const std::atomic_bool& shouldCancel)
{
  bool good = false;
  const usize totalPoints = featureIdsStore.getNumberOfTuples();
  const usize totalFeatures = flaggedFeatures->getNumberOfTuples();
  std::vector<bool> activeObjects(totalFeatures, true);
  for(usize i = 1; i < totalFeatures; i++)
  {
    if(!flaggedFeatures->isTrue(i))
    {
      good = true;
    }
    else
    {
      activeObjects[i] = false;
    }
  }
  if(!good)
  {
    return {std::vector<bool>{}};
  }

  constexpr usize k_ChunkSize = 65536;
  const int32 replacementValue = fillRemovedFeatures ? -1 : 0;
  auto chunkBuf = std::make_unique<int32[]>(k_ChunkSize);
  for(usize offset = 0; offset < totalPoints; offset += k_ChunkSize)
  {
    if(shouldCancel)
    {
      return {activeObjects};
    }
    const usize count = std::min(k_ChunkSize, totalPoints - offset);
    auto readResult = featureIdsStore.copyIntoBuffer(offset, nonstd::span<int32>(chunkBuf.get(), count));
    if(readResult.invalid())
    {
      return ConvertInvalidResult<std::vector<bool>>(std::move(readResult));
    }
    bool chunkModified = false;
    for(usize i = 0; i < count; i++)
    {
      const int32 featureId = chunkBuf[i];
      if(featureId >= 0 && static_cast<usize>(featureId) < activeObjects.size() && !activeObjects[featureId])
      {
        chunkBuf[i] = replacementValue;
        chunkModified = true;
      }
    }
    if(chunkModified)
    {
      auto writeResult = featureIdsStore.copyFromBuffer(offset, nonstd::span<const int32>(chunkBuf.get(), count));
      if(writeResult.invalid())
      {
        return ConvertInvalidResult<std::vector<bool>>(std::move(writeResult));
      }
    }
  }
  return {activeObjects};
}

/** @brief Applies the feature-scale renumbering map to Feature IDs through bounded read/modify/write chunks. */
Result<> RenumberFeatureIdsScanline(Int32AbstractDataStore& featureIdsStore, const std::vector<bool>& activeObjects, const std::atomic_bool& shouldCancel)
{
  const FeatureRenumbering renumbering = ComputeFeatureRenumbering(activeObjects);
  if(!renumbering.anyRemoved)
  {
    return {};
  }

  constexpr usize k_ChunkSize = 65536;
  auto values = std::make_unique<int32[]>(k_ChunkSize);
  const usize tupleCount = featureIdsStore.getNumberOfTuples();
  for(usize offset = 0; offset < tupleCount; offset += k_ChunkSize)
  {
    if(shouldCancel)
    {
      return {};
    }
    const usize count = std::min(k_ChunkSize, tupleCount - offset);
    auto readResult = featureIdsStore.copyIntoBuffer(offset, nonstd::span<int32>(values.get(), count));
    if(readResult.invalid())
    {
      return readResult;
    }
    bool modified = false;
    for(usize i = 0; i < count; i++)
    {
      const int32 featureId = values[i];
      if(featureId >= 0 && static_cast<usize>(featureId) < renumbering.newNames.size())
      {
        const int32 remapped = static_cast<int32>(renumbering.newNames[static_cast<usize>(featureId)]);
        modified |= values[i] != remapped;
        values[i] = remapped;
      }
    }
    if(modified)
    {
      auto writeResult = featureIdsStore.copyFromBuffer(offset, nonstd::span<const int32>(values.get(), count));
      if(writeResult.invalid())
      {
        return writeResult;
      }
    }
  }
  return {};
}

/**
 * @brief Runs CropImageGeometryFilter to extract one flagged Feature into its own
 * cropped ImageGeom. Identical orchestration to RemoveFlaggedFeaturesDirect's copy --
 * this delegates entirely to CropImageGeometryFilter and only touches the small,
 * feature-level bounds array, so it is not itself an OOC-sensitive code path.
 */
class RunCropImageGeometryImpl
{
public:
  /** @brief Captures the borrowed crop paths, bounds, data structure, and cancellation state. */
  RunCropImageGeometryImpl(DataStructure& dataStructure, const std::atomic_bool& shouldCancel, const DataPath& imageGeometryPath, const std::vector<uint64>& minVoxelVector,
                           const std::vector<uint64>& maxVoxelVector, const DataPath& createdImgGeomPath)
  : m_DataStructure(dataStructure)
  , m_ShouldCancel(shouldCancel)
  , m_ImageGeometryPath(imageGeometryPath)
  , m_MinVoxelVector(minVoxelVector)
  , m_MaxVoxelVector(maxVoxelVector)
  , m_CreatedImgGeomPath(createdImgGeomPath)
  {
  }

  ~RunCropImageGeometryImpl() = default;

  /** @brief Preflights and executes the delegated crop using the captured bounds. */
  void operator()() const
  {
    CropImageGeometryFilter filter;

    Arguments args;

    args.insertOrAssign(CropImageGeometryFilter::k_RemoveOriginalGeometry_Key, std::make_any<bool>(false));
    args.insertOrAssign(CropImageGeometryFilter::k_SelectedImageGeometryPath_Key, std::make_any<DataPath>(m_ImageGeometryPath));
    args.insertOrAssign(CropImageGeometryFilter::k_RenumberFeatures_Key, std::make_any<bool>(false));
    args.insertOrAssign(CropImageGeometryFilter::k_UsePhysicalBounds_Key, std::make_any<bool>(false));

    args.insertOrAssign(CropImageGeometryFilter::k_MinVoxel_Key, std::make_any<std::vector<uint64>>(m_MinVoxelVector));
    args.insertOrAssign(CropImageGeometryFilter::k_MaxVoxel_Key, std::make_any<std::vector<uint64>>(m_MaxVoxelVector));
    args.insertOrAssign(CropImageGeometryFilter::k_CreatedImageGeometryPath_Key, std::make_any<DataPath>(m_CreatedImgGeomPath));

    auto preflightResult = filter.preflight(m_DataStructure, args);
    if(preflightResult.outputActions.invalid())
    {
      throw std::runtime_error("Preflight failed when cropping the geometry in extract flagged features!");
    }

    if(m_ShouldCancel)
    {
      return;
    }

    auto executeResult = filter.execute(m_DataStructure, args);
    if(preflightResult.outputActions.invalid())
    {
      throw std::runtime_error("Execute failed when cropping the geometry in extract flagged features!");
    }
  }

private:
  DataStructure& m_DataStructure;
  const std::atomic_bool& m_ShouldCancel;
  const DataPath& m_ImageGeometryPath;
  const std::vector<uint64>& m_MinVoxelVector;
  const std::vector<uint64>& m_MaxVoxelVector;
  const DataPath& m_CreatedImgGeomPath;
};
} // namespace

// -----------------------------------------------------------------------------
RemoveFlaggedFeaturesScanline::RemoveFlaggedFeaturesScanline(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel,
                                                             const RemoveFlaggedFeaturesInputValues* inputValues)
: m_DataStructure(dataStructure)
, m_InputValues(inputValues)
, m_ShouldCancel(shouldCancel)
, m_MessageHandler(mesgHandler)
{
}

// -----------------------------------------------------------------------------
RemoveFlaggedFeaturesScanline::~RemoveFlaggedFeaturesScanline() noexcept = default;

// -----------------------------------------------------------------------------
Result<> RemoveFlaggedFeaturesScanline::operator()()
{
  auto& featureIds = m_DataStructure.getDataAs<Int32Array>(m_InputValues->FeatureIdsArrayPath)->getDataStoreRef();
  auto& imageGeom = m_DataStructure.getDataRefAs<ImageGeom>(m_InputValues->ImageGeometryPath);
  auto function = static_cast<Functionality>(m_InputValues->ExtractFeatures);

  std::unique_ptr<MaskCompareUtilities::MaskCompare> flaggedFeatures = nullptr;
  try
  {
    flaggedFeatures = MaskCompareUtilities::InstantiateMaskCompare(m_DataStructure, m_InputValues->FlaggedFeaturesArrayPath);
  } catch(const std::out_of_range& exception)
  {
    // This really should NOT be happening as the path was verified during preflight BUT we may be calling this from
    // somewhere else that is NOT going through the normal nx::core::IFilter API of Preflight and Execute
    std::string message = fmt::format("Mask Array DataPath does not exist or is not of the correct type (Bool | UInt8) {}", m_InputValues->FlaggedFeaturesArrayPath.toString());
    return MakeErrorResult(-53900, message);
  }

  if(m_ShouldCancel)
  {
    return {};
  }

  MessageHelper messageHelper(m_MessageHandler);

  // Valid values Functionality::Extract and Functionality::ExtractThenRemove
  if(function != Functionality::Remove)
  {
    m_MessageHandler(IFilter::ProgressMessage{IFilter::Message::Type::Info, fmt::format("Beginning Feature Extraction")});

    {
      ComputeFeatureRectFilter filter;
      Arguments args;

      args.insert(ComputeFeatureRectFilter::k_FeatureIdsArrayPath_Key, std::make_any<DataPath>(m_InputValues->FeatureIdsArrayPath));
      args.insert(ComputeFeatureRectFilter::k_FeatureDataAttributeMatrixPath_Key, std::make_any<DataPath>(m_InputValues->TempBoundsPath.getParent()));
      args.insert(ComputeFeatureRectFilter::k_FeatureRectArrayName_Key, std::make_any<std::string>(m_InputValues->TempBoundsPath.getTargetName()));

      auto preflightResult = filter.preflight(m_DataStructure, args);
      if(preflightResult.outputActions.invalid())
      {
        throw std::runtime_error("Preflight failed when cropping the geometry in extract flagged features!");
      }

      if(m_ShouldCancel)
      {
        return {};
      }

      auto executeResult = filter.execute(m_DataStructure, args);
      if(preflightResult.outputActions.invalid())
      {
        throw std::runtime_error("Execute failed when cropping the geometry in extract flagged features!");
      }
    }

    auto bounds = m_DataStructure.getDataRefAs<UInt32Array>(m_InputValues->TempBoundsPath);

    if(m_ShouldCancel)
    {
      return {};
    }

    ParallelTaskAlgorithm taskRunner;
    // This has to be run in serial for the time being because adding to the dataStructure is not thread-safe
    taskRunner.setParallelizationEnabled(false);

    usize maxTuple = flaggedFeatures->getNumberOfTuples();
    std::string paddingWidth = std::to_string(std::to_string(maxTuple).size());
    for(usize i = 1; i < maxTuple; i++)
    {
      if(m_ShouldCancel)
      {
        return {};
      }

      if(!flaggedFeatures->isTrue(i))
      {
        continue;
      }

      usize index = 6 * i;
      std::vector<uint64> minVoxels = {static_cast<uint64>(bounds[index]), static_cast<uint64>(bounds[index + 1]), static_cast<uint64>(bounds[index + 2])};
      std::vector<uint64> maxVoxels = {static_cast<uint64>(bounds[index + 3]), static_cast<uint64>(bounds[index + 4]), static_cast<uint64>(bounds[index + 5])};

      DataPath createdImgGeomPath({fmt::format(fmt::runtime("{}-{:0" + paddingWidth + "d}"), m_InputValues->CreatedImageGeometryPrefix, i)});

      m_MessageHandler(IFilter::ProgressMessage{IFilter::Message::Type::Info, fmt::format("Now Extracting Feature {}", i)});
      taskRunner.execute(RunCropImageGeometryImpl(m_DataStructure, m_ShouldCancel, m_InputValues->ImageGeometryPath, minVoxels, maxVoxels, createdImgGeomPath));
    }
    taskRunner.wait();

    m_MessageHandler(IFilter::ProgressMessage{IFilter::Message::Type::Info, fmt::format("All Features Successfully Extracted")});
  }

  if(m_ShouldCancel)
  {
    return {};
  }

  // Valid values Functionality::Remove and Functionality::ExtractThenRemove
  if(function != Functionality::Extract)
  {
    m_MessageHandler(IFilter::ProgressMessage{IFilter::Message::Type::Info, fmt::format("Beginning Feature Removal")});

    auto activeObjectsResult = FlagFeaturesScanline(featureIds, flaggedFeatures, m_InputValues->FillRemovedFeatures, m_ShouldCancel);
    if(activeObjectsResult.invalid())
    {
      return ConvertResult(std::move(activeObjectsResult));
    }
    std::vector<bool> activeObjects = std::move(activeObjectsResult.value());
    if(activeObjects.empty())
    {
      return MakeErrorResult(-45433, "All Features were flagged and would all be removed. The filter has quit.");
    }

    if(m_ShouldCancel)
    {
      return {};
    }

    if(m_InputValues->FillRemovedFeatures)
    {
      bool shouldLoop;
      usize count = 0;
      do
      {
        count++;
        m_MessageHandler(IFilter::ProgressMessage{IFilter::Message::Type::Info, fmt::format("Entering iteration number {}...", count)});

        // Every kept cell-level array (including FeatureIds itself, unless the user
        // explicitly ignored it) that must receive the winning neighbor's data.
        // Gathered once per convergence iteration -- mirrors the Direct algorithm's
        // per-iteration regeneration and is not itself an OOC concern (feature-level
        // list, not cell-level data).
        std::vector<std::shared_ptr<IDataArray>> voxelArrays = GenerateDataArrayList(m_DataStructure, m_InputValues->FeatureIdsArrayPath, m_InputValues->IgnoredDataArrayPaths);
        auto fillResult = IdentifyAndFillNeighborsScanline(imageGeom, featureIds, voxelArrays, m_ShouldCancel, messageHelper);
        if(fillResult.invalid())
        {
          return ConvertResult(std::move(fillResult));
        }
        shouldLoop = fillResult.value();

        if(m_ShouldCancel)
        {
          return {};
        }
      } while(shouldLoop);
    }

    if(m_ShouldCancel)
    {
      return {};
    }

    m_MessageHandler(IFilter::ProgressMessage{IFilter::Message::Type::Info, fmt::format("Stripping excess inactive objects from model...")});
    DataPath featureGroupPath = m_InputValues->FlaggedFeaturesArrayPath.getParent();
    auto renumberResult = RenumberFeatureIdsScanline(featureIds, activeObjects, m_ShouldCancel);
    if(renumberResult.invalid())
    {
      return renumberResult;
    }
    if(m_ShouldCancel)
    {
      return {};
    }
    if(!RemoveInactiveObjects(m_DataStructure, featureGroupPath, activeObjects, featureIds, flaggedFeatures->getNumberOfTuples(), m_MessageHandler, m_ShouldCancel,
                              /*cellFeatureIdsRenumbered=*/true))
    {
      return MakeErrorResult(-45434, fmt::format("Failed to remove inactive objects from feature group at path '{}'.", featureGroupPath.toString()));
    }
  }

  return {};
}
