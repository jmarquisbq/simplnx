#pragma once

#include "SimplnxCore/SimplnxCore_export.hpp"

#include "simplnx/DataStructure/DataArray.hpp"
#include "simplnx/DataStructure/DataPath.hpp"
#include "simplnx/DataStructure/DataStructure.hpp"

#include <array>
#include <nonstd/span.hpp>

namespace nx::core
{
namespace detail
{
constexpr usize kTransferPageValues = 65536;
constexpr usize kTransferPageCount = 4;
constexpr usize kTransferFaceRun = 4096;

template <typename T, typename Record, typename SourceIndices>
Result<> TransferBoundedCellTuples(AbstractDataStore<T>& source, AbstractDataStore<T>& destination, usize numComps, nonstd::span<const Record> records, SourceIndices sourceIndices)
{
  if(records.empty())
  {
    return {};
  }
  if(numComps == 0 || numComps > std::numeric_limits<usize>::max() / 2)
  {
    return MakeErrorResult(-62060, "Tuple transfer component count is zero or overflows its two-sided destination shape.");
  }
  const usize valuesPerFace = numComps * 2;
  if(source.getNumberOfComponents() != numComps || destination.getNumberOfComponents() != valuesPerFace)
  {
    return MakeErrorResult(-62066, "Tuple transfer array component shapes are incompatible.");
  }
  const usize tuplesPerPage = kTransferPageValues / numComps;
  const usize facesPerRun = std::min(kTransferFaceRun, std::max(usize{1}, kTransferPageValues / valuesPerFace));
  const usize sourceTuples = source.getNumberOfTuples();
  const usize destinationTuples = destination.getNumberOfTuples();
  bool hasSource = false;
  for(const auto& record : records)
  {
    const auto indices = sourceIndices(record);
    hasSource = indices[0] != std::numeric_limits<usize>::max() || indices[1] != std::numeric_limits<usize>::max();
    if(hasSource)
    {
      break;
    }
  }
  if(!hasSource)
  {
    return {};
  }

  // A tuple may legitimately contain more values than one bounded source page.
  // In that case, stream one face-side/component range at a time instead of
  // rejecting the array or allocating a tuple-sized buffer.
  if(numComps > kTransferPageValues)
  {
    std::unique_ptr<T[]> values;
    try
    {
      values = std::make_unique<T[]>(kTransferPageValues);
    } catch(const std::bad_alloc&)
    {
      return MakeErrorResult(-62061, "Tuple transfer failed to allocate its bounded component buffer.");
    } catch(const std::length_error&)
    {
      return MakeErrorResult(-62061, "Tuple transfer failed to allocate its bounded component buffer.");
    }

    const usize firstFace = records.front().faceIndex;
    if(firstFace > destinationTuples || records.size() > destinationTuples - firstFace)
    {
      return MakeErrorResult(-62065, "Tuple transfer destination face range exceeds its destination array.");
    }
    for(usize localRecord = 0; localRecord < records.size(); localRecord++)
    {
      const auto& record = records[localRecord];
      if(record.faceIndex != firstFace + localRecord || record.faceIndex > std::numeric_limits<usize>::max() / valuesPerFace)
      {
        return MakeErrorResult(-62063, "Tuple transfer records must be contiguous in destination face order.");
      }
      const auto indices = sourceIndices(record);
      const usize destinationFaceOffset = record.faceIndex * valuesPerFace;
      for(usize side = 0; side < 2; side++)
      {
        const usize sourceTuple = indices[side];
        if(sourceTuple != std::numeric_limits<usize>::max() && (sourceTuple >= sourceTuples || sourceTuple > std::numeric_limits<usize>::max() / numComps))
        {
          return MakeErrorResult(-62062, "Tuple transfer source tuple index is outside the source array.");
        }
        const usize sourceTupleOffset = sourceTuple == std::numeric_limits<usize>::max() ? 0 : sourceTuple * numComps;
        if(destinationFaceOffset > std::numeric_limits<usize>::max() - side * numComps)
        {
          return MakeErrorResult(-62065, "Tuple transfer destination face offset overflows usize.");
        }
        const usize destinationSideOffset = destinationFaceOffset + side * numComps;
        for(usize componentStart = 0; componentStart < numComps;)
        {
          const usize componentCount = std::min(kTransferPageValues, numComps - componentStart);
          if(sourceTuple == std::numeric_limits<usize>::max())
          {
            std::fill_n(values.get(), componentCount, T{});
          }
          else
          {
            if(sourceTupleOffset > std::numeric_limits<usize>::max() - componentStart)
            {
              return MakeErrorResult(-62064, "Tuple transfer source component offset overflows usize.");
            }
            auto readResult = source.copyIntoBuffer(sourceTupleOffset + componentStart, nonstd::span<T>(values.get(), componentCount));
            if(readResult.invalid())
            {
              return readResult;
            }
          }
          if(destinationSideOffset > std::numeric_limits<usize>::max() - componentStart)
          {
            return MakeErrorResult(-62065, "Tuple transfer destination component offset overflows usize.");
          }
          auto writeResult = destination.copyFromBuffer(destinationSideOffset + componentStart, nonstd::span<const T>(values.get(), componentCount));
          if(writeResult.invalid())
          {
            return writeResult;
          }
          componentStart += componentCount;
        }
      }
    }
    return {};
  }

  std::array<std::unique_ptr<T[]>, kTransferPageCount> pages;
  std::array<usize, kTransferPageCount> pageStarts;
  pageStarts.fill(std::numeric_limits<usize>::max());
  std::array<usize, kTransferPageCount> pageUse{};
  usize useCounter = 0;
  try
  {
    for(auto& page : pages)
    {
      page = std::make_unique<T[]>(kTransferPageValues);
    }
  } catch(const std::bad_alloc&)
  {
    return MakeErrorResult(-62061, "Tuple transfer failed to allocate its bounded source pages.");
  } catch(const std::length_error&)
  {
    return MakeErrorResult(-62061, "Tuple transfer failed to allocate its bounded source pages.");
  }

  const auto sourceValue = [&](usize tupleIndex, usize component) -> Result<const T*> {
    if(tupleIndex >= sourceTuples)
    {
      return MakeErrorResult<const T*>(-62062, "Tuple transfer source tuple index is outside the source array.");
    }
    const usize pageStart = (tupleIndex / tuplesPerPage) * tuplesPerPage;
    if(pageStart > std::numeric_limits<usize>::max() / numComps)
    {
      return MakeErrorResult<const T*>(-62064, "Tuple transfer source-page offset overflows usize.");
    }
    usize pageIndex = 0;
    while(pageIndex < kTransferPageCount && pageStarts[pageIndex] != pageStart)
    {
      pageIndex++;
    }
    if(pageIndex == kTransferPageCount)
    {
      pageIndex = static_cast<usize>(std::min_element(pageUse.begin(), pageUse.end()) - pageUse.begin());
      const usize pageTuples = std::min(tuplesPerPage, sourceTuples - pageStart);
      auto readResult = source.copyIntoBuffer(pageStart * numComps, nonstd::span<T>(pages[pageIndex].get(), pageTuples * numComps));
      if(readResult.invalid())
      {
        return ConvertInvalidResult<const T*>(std::move(readResult));
      }
      pageStarts[pageIndex] = pageStart;
    }
    pageUse[pageIndex] = ++useCounter;
    return {pages[pageIndex].get() + (tupleIndex - pageStart) * numComps + component};
  };

  for(usize recordStart = 0; recordStart < records.size(); recordStart += facesPerRun)
  {
    const usize recordCount = std::min(facesPerRun, records.size() - recordStart);
    const usize firstFace = records[recordStart].faceIndex;
    if(recordCount > std::numeric_limits<usize>::max() / valuesPerFace || firstFace > std::numeric_limits<usize>::max() / valuesPerFace ||
       firstFace > std::numeric_limits<usize>::max() - recordCount || firstFace + recordCount > destinationTuples)
    {
      return MakeErrorResult(-62065, "Tuple transfer destination face range overflows or exceeds its destination array.");
    }
    try
    {
      auto destinationBuffer = std::make_unique<T[]>(recordCount * valuesPerFace);
      std::fill_n(destinationBuffer.get(), recordCount * valuesPerFace, T{});
      for(usize localRecord = 0; localRecord < recordCount; localRecord++)
      {
        const auto& record = records[recordStart + localRecord];
        if(record.faceIndex != firstFace + localRecord)
        {
          return MakeErrorResult(-62063, "Tuple transfer records must be contiguous in destination face order.");
        }
        const auto indices = sourceIndices(record);
        for(usize side = 0; side < 2; side++)
        {
          if(indices[side] == std::numeric_limits<usize>::max())
          {
            continue;
          }
          for(usize component = 0; component < numComps; component++)
          {
            auto valueResult = sourceValue(indices[side], component);
            if(valueResult.invalid())
            {
              return ConvertResult(std::move(valueResult));
            }
            destinationBuffer[(localRecord * 2 + side) * numComps + component] = *valueResult.value();
          }
        }
      }
      auto writeResult = destination.copyFromBuffer(firstFace * valuesPerFace, nonstd::span<const T>(destinationBuffer.get(), recordCount * valuesPerFace));
      if(writeResult.invalid())
      {
        return writeResult;
      }
    } catch(const std::bad_alloc&)
    {
      return MakeErrorResult(-62061, "Tuple transfer failed to allocate its bounded destination run.");
    } catch(const std::length_error&)
    {
      return MakeErrorResult(-62061, "Tuple transfer failed to allocate its bounded destination run.");
    }
  }
  return {};
}

template <typename T, typename K, typename Record, typename SourceIndices>
Result<> TransferBoundedFeatureTuples(AbstractDataStore<K>& featureIds, AbstractDataStore<T>& featureData, AbstractDataStore<T>& destination, usize numComps, nonstd::span<const Record> records,
                                      SourceIndices sourceIndices)
{
  if(records.empty() || numComps == 0 || numComps > std::numeric_limits<usize>::max() / 2)
  {
    return records.empty() ? Result<>{} : MakeErrorResult(-62070, "Feature tuple transfer component count is zero or overflows its two-sided destination shape.");
  }
  const usize valuesPerFace = numComps * 2;
  if(featureIds.getNumberOfComponents() != 1 || featureData.getNumberOfComponents() != numComps || destination.getNumberOfComponents() != valuesPerFace)
  {
    return MakeErrorResult(-62078, "Feature tuple transfer array component shapes are incompatible.");
  }
  constexpr usize kInvalid = std::numeric_limits<usize>::max();
  bool hasSource = false;
  for(const auto& record : records)
  {
    const auto cells = sourceIndices(record);
    hasSource = cells[0] != kInvalid || cells[1] != kInvalid;
    if(hasSource)
    {
      break;
    }
  }
  if(!hasSource)
  {
    return {};
  }

  if(numComps > kTransferPageValues)
  {
    std::unique_ptr<K[]> idPage;
    std::unique_ptr<T[]> values;
    try
    {
      idPage = std::make_unique<K[]>(kTransferPageValues);
      values = std::make_unique<T[]>(kTransferPageValues);
    } catch(const std::bad_alloc&)
    {
      return MakeErrorResult(-62071, "Feature tuple transfer failed to allocate its bounded component buffers.");
    } catch(const std::length_error&)
    {
      return MakeErrorResult(-62071, "Feature tuple transfer failed to allocate its bounded component buffers.");
    }

    usize idPageStart = kInvalid;
    const auto loadId = [&](usize cell) -> Result<K> {
      if(cell >= featureIds.getNumberOfTuples())
      {
        return MakeErrorResult<K>(-62072, "Feature tuple transfer cell index is outside FeatureIds.");
      }
      const usize pageStart = (cell / kTransferPageValues) * kTransferPageValues;
      if(pageStart != idPageStart)
      {
        auto readResult = featureIds.copyIntoBuffer(pageStart, nonstd::span<K>(idPage.get(), std::min(kTransferPageValues, featureIds.getNumberOfTuples() - pageStart)));
        if(readResult.invalid())
        {
          return ConvertInvalidResult<K>(std::move(readResult));
        }
        idPageStart = pageStart;
      }
      return {idPage[cell - pageStart]};
    };

    const usize firstFace = records.front().faceIndex;
    if(firstFace > destination.getNumberOfTuples() || records.size() > destination.getNumberOfTuples() - firstFace)
    {
      return MakeErrorResult(-62074, "Feature tuple transfer destination range is invalid.");
    }
    for(usize localRecord = 0; localRecord < records.size(); localRecord++)
    {
      const auto& record = records[localRecord];
      if(record.faceIndex != firstFace + localRecord || record.faceIndex > std::numeric_limits<usize>::max() / valuesPerFace)
      {
        return MakeErrorResult(-62075, "Feature tuple transfer records must be contiguous.");
      }
      const auto cells = sourceIndices(record);
      const usize destinationFaceOffset = record.faceIndex * valuesPerFace;
      for(usize side = 0; side < 2; side++)
      {
        usize feature = kInvalid;
        if(cells[side] != kInvalid)
        {
          auto featureIdResult = loadId(cells[side]);
          if(featureIdResult.invalid())
          {
            return ConvertResult(std::move(featureIdResult));
          }
          if constexpr(std::is_signed_v<K>)
          {
            if(featureIdResult.value() < 0)
            {
              return MakeErrorResult(-62076, "Feature tuple transfer encountered a negative feature id.");
            }
          }
          feature = static_cast<usize>(featureIdResult.value());
          if(feature >= featureData.getNumberOfTuples() || feature > std::numeric_limits<usize>::max() / numComps)
          {
            return MakeErrorResult(-62073, "Feature tuple transfer feature id is outside its feature array.");
          }
        }

        const usize sourceTupleOffset = feature == kInvalid ? 0 : feature * numComps;
        if(destinationFaceOffset > std::numeric_limits<usize>::max() - side * numComps)
        {
          return MakeErrorResult(-62074, "Feature tuple transfer destination offset overflows usize.");
        }
        const usize destinationSideOffset = destinationFaceOffset + side * numComps;
        for(usize componentStart = 0; componentStart < numComps;)
        {
          const usize componentCount = std::min(kTransferPageValues, numComps - componentStart);
          if(feature == kInvalid)
          {
            std::fill_n(values.get(), componentCount, T{});
          }
          else
          {
            if(sourceTupleOffset > std::numeric_limits<usize>::max() - componentStart)
            {
              return MakeErrorResult(-62077, "Feature tuple transfer source component offset overflows usize.");
            }
            auto readResult = featureData.copyIntoBuffer(sourceTupleOffset + componentStart, nonstd::span<T>(values.get(), componentCount));
            if(readResult.invalid())
            {
              return readResult;
            }
          }
          if(destinationSideOffset > std::numeric_limits<usize>::max() - componentStart)
          {
            return MakeErrorResult(-62074, "Feature tuple transfer destination component offset overflows usize.");
          }
          auto writeResult = destination.copyFromBuffer(destinationSideOffset + componentStart, nonstd::span<const T>(values.get(), componentCount));
          if(writeResult.invalid())
          {
            return writeResult;
          }
          componentStart += componentCount;
        }
      }
    }
    return {};
  }

  const usize tuplesPerFeaturePage = kTransferPageValues / numComps;
  const usize facesPerRun = std::min(kTransferFaceRun, std::max(usize{1}, kTransferPageValues / valuesPerFace));
  std::array<std::unique_ptr<K[]>, kTransferPageCount> idPages;
  std::array<std::unique_ptr<T[]>, kTransferPageCount> valuePages;
  std::array<usize, kTransferPageCount> idStarts, valueStarts, idUse{}, valueUse{};
  usize idUseCounter = 0;
  usize valueUseCounter = 0;
  idStarts.fill(kInvalid);
  valueStarts.fill(kInvalid);
  try
  {
    for(usize i = 0; i < kTransferPageCount; i++)
    {
      idPages[i] = std::make_unique<K[]>(kTransferPageValues);
      valuePages[i] = std::make_unique<T[]>(kTransferPageValues);
    }
  } catch(const std::bad_alloc&)
  {
    return MakeErrorResult(-62071, "Feature tuple transfer failed to allocate bounded pages.");
  } catch(const std::length_error&)
  {
    return MakeErrorResult(-62071, "Feature tuple transfer failed to allocate bounded pages.");
  }
  const auto loadId = [&](usize cell) -> Result<K> {
    if(cell >= featureIds.getNumberOfTuples())
      return MakeErrorResult<K>(-62072, "Feature tuple transfer cell index is outside FeatureIds.");
    const usize start = (cell / kTransferPageValues) * kTransferPageValues;
    usize p = 0;
    while(p < kTransferPageCount && idStarts[p] != start)
      p++;
    if(p == kTransferPageCount)
    {
      p = static_cast<usize>(std::min_element(idUse.begin(), idUse.end()) - idUse.begin());
      auto r = featureIds.copyIntoBuffer(start, nonstd::span<K>(idPages[p].get(), std::min(kTransferPageValues, featureIds.getNumberOfTuples() - start)));
      if(r.invalid())
        return ConvertInvalidResult<K>(std::move(r));
      idStarts[p] = start;
    }
    idUse[p] = ++idUseCounter;
    return {idPages[p][cell - start]};
  };
  const auto loadValue = [&](usize feature, usize component) -> Result<const T*> {
    if(feature >= featureData.getNumberOfTuples())
      return MakeErrorResult<const T*>(-62073, "Feature tuple transfer feature id is outside its feature array.");
    const usize start = (feature / tuplesPerFeaturePage) * tuplesPerFeaturePage;
    usize p = 0;
    while(p < kTransferPageCount && valueStarts[p] != start)
      p++;
    if(p == kTransferPageCount)
    {
      p = static_cast<usize>(std::min_element(valueUse.begin(), valueUse.end()) - valueUse.begin());
      const usize count = std::min(tuplesPerFeaturePage, featureData.getNumberOfTuples() - start);
      if(start > std::numeric_limits<usize>::max() / numComps || count > std::numeric_limits<usize>::max() / numComps)
      {
        return MakeErrorResult<const T*>(-62077, "Feature tuple transfer page offset overflows usize.");
      }
      auto r = featureData.copyIntoBuffer(start * numComps, nonstd::span<T>(valuePages[p].get(), count * numComps));
      if(r.invalid())
        return ConvertInvalidResult<const T*>(std::move(r));
      valueStarts[p] = start;
    }
    valueUse[p] = ++valueUseCounter;
    return {valuePages[p].get() + (feature - start) * numComps + component};
  };
  for(usize begin = 0; begin < records.size(); begin += facesPerRun)
  {
    const usize count = std::min(facesPerRun, records.size() - begin);
    const usize first = records[begin].faceIndex;
    if(count > std::numeric_limits<usize>::max() / valuesPerFace || first > std::numeric_limits<usize>::max() / valuesPerFace || first > destination.getNumberOfTuples() ||
       count > destination.getNumberOfTuples() - first)
      return MakeErrorResult(-62074, "Feature tuple transfer destination range is invalid.");
    std::unique_ptr<T[]> out;
    try
    {
      out = std::make_unique<T[]>(count * valuesPerFace);
    } catch(const std::bad_alloc&)
    {
      return MakeErrorResult(-62071, "Feature tuple transfer failed to allocate its bounded destination run.");
    } catch(const std::length_error&)
    {
      return MakeErrorResult(-62071, "Feature tuple transfer failed to allocate its bounded destination run.");
    }
    std::fill_n(out.get(), count * valuesPerFace, T{});
    for(usize n = 0; n < count; n++)
    {
      const auto& rec = records[begin + n];
      if(rec.faceIndex != first + n)
        return MakeErrorResult(-62075, "Feature tuple transfer records must be contiguous.");
      const auto cells = sourceIndices(rec);
      for(usize side = 0; side < 2; side++)
        if(cells[side] != kInvalid)
        {
          auto fid = loadId(cells[side]);
          if(fid.invalid())
            return ConvertResult(std::move(fid));
          if constexpr(std::is_signed_v<K>)
            if(fid.value() < 0)
              return MakeErrorResult(-62076, "Feature tuple transfer encountered a negative feature id.");
          const usize feature = static_cast<usize>(fid.value());
          for(usize c = 0; c < numComps; c++)
          {
            auto value = loadValue(feature, c);
            if(value.invalid())
              return ConvertResult(std::move(value));
            out[(n * 2 + side) * numComps + c] = *value.value();
          }
        }
    }
    auto r = destination.copyFromBuffer(first * valuesPerFace, nonstd::span<const T>(out.get(), count * valuesPerFace));
    if(r.invalid())
      return r;
  }
  return {};
}
} // namespace detail

/**
 * @brief Record holding all data needed to perform one QuickSurfaceMesh face transfer.
 *
 * Batching many of these records into a single quickSurfaceTransferBatch() call
 * lets the OOC implementation process ordered face runs through fixed source
 * pages and a fixed destination buffer, avoiding singleton accesses and sparse
 * source-span allocation.
 */
struct QuickSurfaceTransferData
{
  usize faceIndex = 0;    ///< Index of the triangle face in the destination (face-level) array.
  usize firstcIndex = 0;  ///< Cell index on side 0 of the face (used when faceLabel0 != -1).
  usize secondcIndex = 0; ///< Cell index on side 1 of the face (used when faceLabel1 != -1).
  int32 faceLabel0 = 0;   ///< Face label for side 0; -1 indicates exterior (skip copy).
  int32 faceLabel1 = 0;   ///< Face label for side 1; -1 indicates exterior (skip copy).
};

/**
 * @brief Record holding all data needed to perform one SurfaceNets face transfer.
 *
 * Similar to QuickSurfaceTransferData but uses the SurfaceNets convention where
 * each face has two associated NX-array indices (or max sentinel if exterior).
 */
struct SurfaceNetsTransferData
{
  usize faceIndex = 0; ///< Index of the quad face in the destination (face-level) array.
  /// Pair of NX-array cell indices for the two sides of the quad face.
  /// A value of std::numeric_limits<usize>::max() indicates an exterior face (skip).
  std::array<usize, 2> quadNxArrayIndices = {std::numeric_limits<usize>::max(), std::numeric_limits<usize>::max()};
};

/**
 * @brief Abstract base class for transferring tuple data from cell-level DataArrays
 * to face-level DataArrays during surface mesh generation.
 *
 * @section overview Overview
 * When QuickSurfaceMesh or SurfaceNets generates a triangle/quad mesh from a
 * voxelized volume, each face sits between two cells. The face-level output
 * arrays need to store the data from both adjacent cells (stored interleaved:
 * [side0_comp0, side0_comp1, ..., side1_comp0, side1_comp1, ...]).
 *
 * @section ooc_optimization OOC Optimization: Batch Transfer Methods
 * The original per-element transfer methods (quickSurfaceTransfer, surfaceNetsTransfer)
 * use operator[] on the source and destination DataStore references. When the DataStore
 * is backed by OOC chunked storage, each operator[] call may trigger a chunk load/evict
 * cycle, making mesh generation extremely slow on large datasets.
 *
 * The batch methods use fixed source pages and fixed contiguous destination-face
 * runs. Page misses use bulk I/O; sparse source indices never expand a resident
 * range. Callers flush fixed-size face chunks and every I/O failure is propagated.
 */
class SIMPLNXCORE_EXPORT AbstractTupleTransfer
{
public:
  virtual ~AbstractTupleTransfer() = default;

  AbstractTupleTransfer(const AbstractTupleTransfer&) = delete;
  AbstractTupleTransfer(AbstractTupleTransfer&&) noexcept = delete;
  AbstractTupleTransfer& operator=(const AbstractTupleTransfer&) = delete;
  AbstractTupleTransfer& operator=(AbstractTupleTransfer&&) noexcept = delete;

  /**
   * @brief Transfers one tuple from a source cell to a destination face (point sampling).
   *
   * Used by PointSampleTriangleGeom. Copies m_NumComps values from cellRef[firstcIndex...]
   * to faceRef[faceIndex...].
   *
   * @param faceIndex Starting value index in the destination face array.
   * @param firstcIndex Starting value index in the source cell array.
   */
  virtual void pointSampleTransfer(size_t faceIndex, size_t firstcIndex) = 0;

  /**
   * @brief Transfers cell data to both sides of a triangle face (per-element, non-batched).
   *
   * Copies cell data for side 0 and side 1 of a face, checking faceLabels to skip
   * exterior faces (label == -1). The destination layout is interleaved:
   * [face * numComps * 2 + 0..numComps-1] = side 0, [+ numComps..2*numComps-1] = side 1.
   *
   * @note This method uses per-element operator[] access. For OOC data, prefer
   *   accumulating QuickSurfaceTransferData records and calling quickSurfaceTransferBatch().
   *
   * @param faceIndex Index of the face.
   * @param firstcIndex Cell index for side 0 of the face.
   * @param secondcIndex Cell index for side 1 of the face.
   * @param faceLabels FaceLabels array; exterior faces have label -1.
   */
  virtual void quickSurfaceTransfer(size_t faceIndex, size_t firstcIndex, size_t secondcIndex, AbstractDataStore<int32>& faceLabels) = 0;

  /**
   * @brief Transfers cell data to both sides of a quad face for SurfaceNets (per-element).
   *
   * Same interleaved layout as quickSurfaceTransfer. Exterior sides are indicated
   * by quadNxArrayIndices[i] == std::numeric_limits<usize>::max().
   *
   * @note For OOC data, prefer accumulating SurfaceNetsTransferData records and
   *   calling surfaceNetsTransferBatch().
   *
   * @param faceIndex Index of the quad face.
   * @param quadNxArrayIndices Cell indices for the two sides of the quad; max sentinel = exterior.
   */
  virtual void surfaceNetsTransfer(size_t faceIndex, const std::array<usize, 2>& quadNxArrayIndices) = 0;

  /**
   * @brief OOC-optimized batch transfer for QuickSurfaceMesh faces.
   *
   * Processes a span of QuickSurfaceTransferData records in one bulk I/O round-trip.
   * Default implementation is a no-op; subclasses override with typed bulk copy logic.
   *
   * @param records Span of transfer records to process in one batch.
   */
  virtual Result<> quickSurfaceTransferBatch(nonstd::span<const QuickSurfaceTransferData> /*records*/)
  {
    return {};
  }

  /**
   * @brief OOC-optimized batch transfer for SurfaceNets faces.
   *
   * Processes a span of SurfaceNetsTransferData records in one bulk I/O round-trip.
   * Default implementation is a no-op; subclasses override with typed bulk copy logic.
   *
   * @param records Span of transfer records to process in one batch.
   */
  virtual Result<> surfaceNetsTransferBatch(nonstd::span<const SurfaceNetsTransferData> /*records*/)
  {
    return {};
  }

protected:
  AbstractTupleTransfer() = default;

  DataPath m_SourceDataPath;      ///< Path to the source (cell-level) DataArray in the DataStructure.
  DataPath m_DestinationDataPath; ///< Path to the destination (face-level) DataArray in the DataStructure.
  size_t m_NumComps = 0;          ///< Number of components per tuple in both source and destination arrays.
};

/**
 * @brief Typed implementation of AbstractTupleTransfer for direct cell-to-face data transfer.
 *
 * Copies data directly from cell-level DataArrays to face-level DataArrays.
 * The source array is indexed by cell index and the destination array stores two
 * sides per face in interleaved layout.
 *
 * @section ooc_batch OOC Batch Methods
 * The quickSurfaceTransferBatch() and surfaceNetsTransferBatch() overrides implement
 * the bounded-page strategy described in AbstractTupleTransfer: source tuples are
 * read through fixed pages while destination faces are emitted in fixed-size runs.
 *
 * @tparam T The element type of both the source cell DataArray and destination face DataArray.
 */
template <typename T>
class TransferTuple : public AbstractTupleTransfer
{
public:
  using DataArrayType = DataArray<T>;
  using DataStoreType = AbstractDataStore<T>;

  /**
   * @brief Constructs a TransferTuple for direct cell-to-face data transfer.
   * @param dataStructure Current DataStructure containing both arrays.
   * @param selectedDataPath Path to the source (cell-level) DataArray.
   * @param createdArrayPath Path to the destination (face-level) DataArray.
   */
  TransferTuple(DataStructure& dataStructure, const DataPath& selectedDataPath, const DataPath& createdArrayPath)
  : m_CellRef(dataStructure.template getDataRefAs<DataArrayType>(selectedDataPath).getDataStoreRef())
  , m_FaceRef(dataStructure.template getDataRefAs<DataArrayType>(createdArrayPath).getDataStoreRef())
  {
    m_SourceDataPath = selectedDataPath;
    m_DestinationDataPath = createdArrayPath;

    IDataArray* cellArrayPtr = dataStructure.template getDataAs<IDataArray>(m_SourceDataPath);
    m_NumComps = cellArrayPtr->getNumberOfComponents();
  }

  ~TransferTuple() override = default;
  TransferTuple(const TransferTuple&) = delete;
  TransferTuple(TransferTuple&&) noexcept = delete;
  TransferTuple& operator=(const TransferTuple&) = delete;
  TransferTuple& operator=(TransferTuple&&) noexcept = delete;

  /**
   * @brief
   * @param faceIndex
   * @param firstcIndex
   */
  void pointSampleTransfer(size_t faceIndex, size_t firstcIndex) override
  {
    for(size_t i = 0; i < m_NumComps; i++)
    {
      m_FaceRef[faceIndex + i] = m_CellRef[firstcIndex + i];
    }
  }

  /**
   * @brief
   * @param faceIndex
   * @param firstcIndex
   * @param secondcIndex
   * @param faceLabels
   */
  void quickSurfaceTransfer(size_t faceIndex, size_t firstcIndex, size_t secondcIndex, AbstractDataStore<int32>& faceLabels) override
  {
    // Only copy the data if the FaceLabel is NOT -1, indicating that the data is NOT on the exterior
    if(faceLabels[faceIndex * 2] != -1)
    {
      for(size_t i = 0; i < m_NumComps; i++)
      {
        m_FaceRef[faceIndex * m_NumComps * 2 + i] = m_CellRef[firstcIndex * m_NumComps + i];
      }
    }

    if(faceLabels[faceIndex * 2 + 1] != -1)
    {
      for(size_t i = 0; i < m_NumComps; i++)
      {
        size_t index = (faceIndex * m_NumComps * 2) + m_NumComps + i;
        m_FaceRef[index] = m_CellRef[secondcIndex * m_NumComps + i];
      }
    }
  }

  /**
   * @brief
   * @param faceIndex
   * @param quadNxArrayIndices
   */
  void surfaceNetsTransfer(size_t faceIndex, const std::array<usize, 2>& quadNxArrayIndices) override
  {
    // Only copy the data if the quadNxArrayIndices is NOT UINT64_MAX, indicating that the data is NOT on the exterior
    if(quadNxArrayIndices[0] != std::numeric_limits<usize>::max())
    {
      for(size_t i = 0; i < m_NumComps; i++)
      {
        m_FaceRef[faceIndex * m_NumComps * 2 + i] = m_CellRef[quadNxArrayIndices[0] * m_NumComps + i];
      }
    }

    if(quadNxArrayIndices[1] != std::numeric_limits<usize>::max())
    {
      for(size_t i = 0; i < m_NumComps; i++)
      {
        size_t index = (faceIndex * m_NumComps * 2) + m_NumComps + i;
        m_FaceRef[index] = m_CellRef[quadNxArrayIndices[1] * m_NumComps + i];
      }
    }
  }

  /**
   * @brief OOC-optimized batch transfer for QuickSurfaceMesh.
   *
   * Replaces per-element access with bounded source pages and fixed-size
   * destination face runs.
   *
   * All tuple copies happen in-memory between heap-allocated local buffers.
   * Exterior faces (faceLabel == -1) are skipped during the copy phase.
   *
   * @param records Span of QuickSurfaceTransferData records for this batch.
   */
  Result<> quickSurfaceTransferBatch(nonstd::span<const QuickSurfaceTransferData> records) override
  {
    return detail::TransferBoundedCellTuples(m_CellRef, m_FaceRef, m_NumComps, records, [](const QuickSurfaceTransferData& record) {
      return std::array<usize, 2>{record.faceLabel0 == -1 ? std::numeric_limits<usize>::max() : record.firstcIndex, record.faceLabel1 == -1 ? std::numeric_limits<usize>::max() : record.secondcIndex};
    });
  }

  /**
   * @brief OOC-optimized batch transfer for SurfaceNets.
   *
   * Same bulk I/O strategy as quickSurfaceTransferBatch but using SurfaceNets
   * conventions: exterior sides are indicated by quadNxArrayIndices[i] == max sentinel.
   *
   * @param records Span of SurfaceNetsTransferData records for this batch.
   */
  Result<> surfaceNetsTransferBatch(nonstd::span<const SurfaceNetsTransferData> records) override
  {
    return detail::TransferBoundedCellTuples(m_CellRef, m_FaceRef, m_NumComps, records, [](const SurfaceNetsTransferData& record) { return record.quadNxArrayIndices; });
  }

private:
  DataStoreType& m_CellRef; ///< Reference to the source (cell-level) DataStore.
  DataStoreType& m_FaceRef; ///< Reference to the destination (face-level) DataStore.
};

/**
 * @brief Typed implementation of AbstractTupleTransfer for feature-level to face-level transfer.
 *
 * Unlike TransferTuple which copies cell data directly, this class performs an indirection
 * through a FeatureIds array: cell index -> featureId -> feature-level data -> face output.
 * This is used when surface mesh faces need feature-level attributes (e.g., average
 * orientations, average C-axis values) rather than raw cell-level data.
 *
 * @section ooc_batch OOC Batch Methods
 * The batch methods add independent, fixed FeatureIds and feature-data page
 * caches to the direct tuple-transfer strategy. Destination face tuples are
 * emitted in fixed contiguous runs, so sparse feature IDs cannot expand a
 * resident feature-sized or cell-sized range.
 *
 * @tparam T The element type of the feature-level source and face-level destination DataArrays.
 * @tparam K The element type of the FeatureIds array (typically int32).
 */
template <typename T, typename K>
class TransferFeatureTuple : public AbstractTupleTransfer
{
public:
  using DataArrayType = DataArray<T>;
  using FeatureIdsArrayType = DataArray<K>;
  using DataStoreType = AbstractDataStore<T>;
  using FeatureIdsStoreType = AbstractDataStore<K>;

  /**
   * @brief Constructs a TransferFeatureTuple for feature-level to face-level transfer.
   * @param dataStructure Current DataStructure containing all arrays.
   * @param selectedDataPath Path to the source (feature-level) DataArray.
   * @param createdArrayPath Path to the destination (face-level) DataArray.
   * @param featureIdsArrayPath Path to the FeatureIds array that maps cell index -> feature ID.
   */
  TransferFeatureTuple(DataStructure& dataStructure, const DataPath& selectedDataPath, const DataPath& createdArrayPath, const DataPath& featureIdsArrayPath)
  : m_FeatureDataRef(dataStructure.template getDataRefAs<DataArrayType>(selectedDataPath).getDataStoreRef())
  , m_FaceRef(dataStructure.template getDataRefAs<DataArrayType>(createdArrayPath).getDataStoreRef())
  , m_FeatureIdsRef(dataStructure.template getDataRefAs<FeatureIdsArrayType>(featureIdsArrayPath).getDataStoreRef())
  {
    m_SourceDataPath = selectedDataPath;
    m_DestinationDataPath = createdArrayPath;

    IDataArray* cellArrayPtr = dataStructure.template getDataAs<IDataArray>(m_SourceDataPath);
    m_NumComps = cellArrayPtr->getNumberOfComponents();
  }

  ~TransferFeatureTuple() override = default;
  TransferFeatureTuple(const TransferFeatureTuple&) = delete;
  TransferFeatureTuple(TransferFeatureTuple&&) noexcept = delete;
  TransferFeatureTuple& operator=(const TransferFeatureTuple&) = delete;
  TransferFeatureTuple& operator=(TransferFeatureTuple&&) noexcept = delete;

  /**
   * @brief
   * @param faceIndex
   * @param firstcIndex
   */
  void pointSampleTransfer(size_t faceIndex, size_t firstcIndex) override
  {
    // FeatureIds is assumed to be an Int32 array with a single component.
    K firstFeatureId = m_FeatureIdsRef[firstcIndex];
    for(size_t i = 0; i < m_NumComps; i++)
    {
      m_FaceRef[faceIndex + i] = m_FeatureDataRef[firstFeatureId + i];
    }
  }

  /**
   * @brief
   * @param faceIndex
   * @param firstcIndex
   * @param secondcIndex
   * @param faceLabels
   */
  void quickSurfaceTransfer(size_t faceIndex, size_t firstcIndex, size_t secondcIndex, AbstractDataStore<int32>& faceLabels) override
  {
    // FeatureIds is assumed to be an Int32 array with a single component.
    // Only copy the data if the FaceLabel is NOT -1, indicating that the data is NOT on the exterior
    if(faceLabels[faceIndex * 2] != -1)
    {
      K firstFeatureId = m_FeatureIdsRef[firstcIndex];
      for(size_t i = 0; i < m_NumComps; i++)
      {
        m_FaceRef[faceIndex * m_NumComps * 2 + i] = m_FeatureDataRef[firstFeatureId * m_NumComps + i];
      }
    }

    if(faceLabels[faceIndex * 2 + 1] != -1)
    {
      K secondFeatureId = m_FeatureIdsRef[secondcIndex];
      for(size_t i = 0; i < m_NumComps; i++)
      {
        size_t index = (faceIndex * m_NumComps * 2) + m_NumComps + i;
        m_FaceRef[index] = m_FeatureDataRef[secondFeatureId * m_NumComps + i];
      }
    }
  }

  /**
   * @brief
   * @param faceIndex
   * @param quadNxArrayIndices
   */
  void surfaceNetsTransfer(size_t faceIndex, const std::array<usize, 2>& quadNxArrayIndices) override
  {
    // FeatureIds is assumed to be an Int32 array with a single component.
    // Only copy the data if the quadNxArrayIndices is NOT UINT64_MAX, indicating that the data is NOT on the exterior
    if(quadNxArrayIndices[0] != std::numeric_limits<usize>::max())
    {
      usize firstcIndex = quadNxArrayIndices[0];
      K firstFeatureId = m_FeatureIdsRef[firstcIndex];
      for(size_t i = 0; i < m_NumComps; i++)
      {
        m_FaceRef[faceIndex * m_NumComps * 2 + i] = m_FeatureDataRef[firstFeatureId * m_NumComps + i];
      }
    }

    if(quadNxArrayIndices[1] != std::numeric_limits<usize>::max())
    {
      usize secondcIndex = quadNxArrayIndices[1];
      K secondFeatureId = m_FeatureIdsRef[secondcIndex];
      for(size_t i = 0; i < m_NumComps; i++)
      {
        size_t index = (faceIndex * m_NumComps * 2) + m_NumComps + i;
        m_FaceRef[index] = m_FeatureDataRef[secondFeatureId * m_NumComps + i];
      }
    }
  }

  /**
   * @brief OOC-optimized batch transfer for QuickSurfaceMesh (feature-level variant).
   *
   * Unlike the TransferTuple version, this performs a two-level indirection
   * through independent fixed FeatureIds and feature-data page caches. The
   * resulting face tuples are written in bounded contiguous runs.
   *
   * @param records Span of QuickSurfaceTransferData records for this batch.
   */
  Result<> quickSurfaceTransferBatch(nonstd::span<const QuickSurfaceTransferData> records) override
  {
    return detail::TransferBoundedFeatureTuples<T, K>(m_FeatureIdsRef, m_FeatureDataRef, m_FaceRef, m_NumComps, records, [](const QuickSurfaceTransferData& record) {
      constexpr usize kInvalid = std::numeric_limits<usize>::max();
      return std::array<usize, 2>{record.faceLabel0 == -1 ? kInvalid : record.firstcIndex, record.faceLabel1 == -1 ? kInvalid : record.secondcIndex};
    });
  }

  /**
   * @brief OOC-optimized batch transfer for SurfaceNets (feature-level variant).
   *
   * Same two-level indirection as quickSurfaceTransferBatch (cell -> featureId -> feature data)
   * but using SurfaceNets conventions for exterior-face detection.
   *
   * @param records Span of SurfaceNetsTransferData records for this batch.
   */
  Result<> surfaceNetsTransferBatch(nonstd::span<const SurfaceNetsTransferData> records) override
  {
    return detail::TransferBoundedFeatureTuples<T, K>(m_FeatureIdsRef, m_FeatureDataRef, m_FaceRef, m_NumComps, records,
                                                      [](const SurfaceNetsTransferData& record) { return record.quadNxArrayIndices; });
  }

private:
  DataStoreType& m_FeatureDataRef;      ///< Reference to the source (feature-level) DataStore.
  DataStoreType& m_FaceRef;             ///< Reference to the destination (face-level) DataStore.
  FeatureIdsStoreType& m_FeatureIdsRef; ///< Reference to the FeatureIds DataStore (cell index -> feature ID).
};

/**
 * @brief Factory function that creates a type-appropriate TransferTuple instance and appends it
 * to the provided vector of tuple transfer functions.
 *
 * Inspects the DataType of the selected DataArray and instantiates TransferTuple<T> with
 * the matching type. This is the primary way callers set up direct cell-to-face transfers.
 *
 * @param dataStructure Current DataStructure.
 * @param selectedDataPath Path to the source (cell-level) DataArray.
 * @param createdDataPath Path to the destination (face-level) DataArray.
 * @param tupleTransferFunctions Vector to append the new instance to.
 */
SIMPLNXCORE_EXPORT void AddTupleTransferInstance(DataStructure& dataStructure, const DataPath& selectedDataPath, const DataPath& createdDataPath,
                                                 std::vector<std::shared_ptr<AbstractTupleTransfer>>& tupleTransferFunctions);

/**
 * @brief Factory function that creates a type-appropriate TransferFeatureTuple instance and
 * appends it to the provided vector of tuple transfer functions.
 *
 * Inspects the DataType of the selected DataArray and the FeatureIds array, then
 * instantiates TransferFeatureTuple<T,K> with the matching types. This sets up
 * the two-level indirection path: cell -> featureId -> feature data -> face output.
 *
 * @param dataStructure Current DataStructure.
 * @param selectedDataPath Path to the source (feature-level) DataArray.
 * @param createdDataPath Path to the destination (face-level) DataArray.
 * @param featureIdsArrayPath Path to the FeatureIds array (cell index -> feature ID).
 * @param tupleTransferFunctions Vector to append the new instance to.
 */
SIMPLNXCORE_EXPORT void AddFeatureTupleTransferInstance(DataStructure& dataStructure, const DataPath& selectedDataPath, const DataPath& createdDataPath, const DataPath& featureIdsArrayPath,
                                                        std::vector<std::shared_ptr<AbstractTupleTransfer>>& tupleTransferFunctions);

} // namespace nx::core
