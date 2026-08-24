#include "InterpolatePointCloudToRegularGrid.hpp"

#include "simplnx/DataStructure/DataArray.hpp"
#include "simplnx/DataStructure/Geometry/ImageGeom.hpp"
#include "simplnx/DataStructure/Geometry/VertexGeom.hpp"
#include "simplnx/Utilities/AlgorithmDispatch.hpp"
#include "simplnx/Utilities/BoundedRecordPageCache.hpp"
#include "simplnx/Utilities/DataStoreUtilities.hpp"
#include "simplnx/Utilities/FilterUtilities.hpp"
#include "simplnx/Utilities/InMemoryTemporaryRecordStore.hpp"
#include "simplnx/Utilities/MaskCompareUtilities.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <new>
#include <type_traits>

using namespace nx::core;

namespace
{
/**
 * @brief Per-output-voxel state for interpolated statistics.
 * Welford fields permit stable variance updates without storing every sample.
 */
struct VoxelAccumulator
{
  float64 weightedSum = 0.0;
  float64 weightSum = 0.0;
  uint64 count = 0;
  float64 min = std::numeric_limits<float64>::max();
  float64 max = std::numeric_limits<float64>::lowest();
  float64 welfordMean = 0.0;
  float64 welfordM2 = 0.0;
};
static_assert(std::is_trivially_copyable_v<VoxelAccumulator>);

/** @brief Per-voxel weighted sum used by arrays requested in copy mode. */
struct CopyAccumulator
{
  float64 weightedSum = 0.0;
  float64 weightSum = 0.0;
};
static_assert(std::is_trivially_copyable_v<CopyAccumulator>);

/**
 * @brief Owns one accumulator vector per source array using either resident
 * vectors or temporary record stores with bounded page caches.
 *
 * Genuine OOC output requires an external provider so voxel-scale scratch
 * cannot unexpectedly consume RAM. Forced OOC over resident arrays may use the
 * explicit in-memory provider fallback when no OOC plugin is registered.
 */
template <typename T>
class AccumulatorStorage
{
public:
  /**
   * @brief Allocates and zero-initializes all accumulator vectors.
   * @param arrayCount Number of independent source arrays.
   * @param recordCount Number of output voxels per source array.
   * @param useExternalStorage Selects record stores instead of direct vectors.
   * @param requireExternalStorage Makes provider absence an error for genuine OOC execution.
   */
  static Result<AccumulatorStorage> Create(usize arrayCount, uint64 recordCount, bool useExternalStorage, bool requireExternalStorage, const std::atomic_bool& shouldCancel)
  {
    AccumulatorStorage storage;
    storage.m_RecordCount = recordCount;
    storage.m_RecordsPerPage = std::max<uint64>(1, (1024 * 1024) / sizeof(T));

    if(!useExternalStorage)
    {
      try
      {
        storage.m_Direct.resize(arrayCount);
        for(auto& records : storage.m_Direct)
        {
          records.resize(static_cast<usize>(recordCount));
        }
      } catch(const std::bad_alloc&)
      {
        return MakeErrorResult<AccumulatorStorage>(-34061, "Unable to allocate interpolation accumulators.");
      }
      return {std::move(storage)};
    }

    storage.m_Stores.reserve(arrayCount);
    storage.m_Caches.reserve(arrayCount);
    for(usize arrayIndex = 0; arrayIndex < arrayCount; ++arrayIndex)
    {
      TemporaryRecordStoreConfig config;
      config.recordSize = sizeof(T);
      config.maxRecordsPerBatch = storage.m_RecordsPerPage;
      config.initialRecordCount = recordCount;

      Result<std::unique_ptr<ITemporaryRecordStore>> storeResult = DataStoreUtilities::GetIOCollection().createTemporaryRecordStore(config);
      if(storeResult.invalid() && !requireExternalStorage)
      {
        auto fallbackResult = InMemoryTemporaryRecordStore::Create(config);
        if(fallbackResult.invalid())
        {
          return ConvertInvalidResult<AccumulatorStorage>(std::move(fallbackResult));
        }
        storeResult = {std::unique_ptr<ITemporaryRecordStore>(std::move(fallbackResult.value()))};
      }
      if(storeResult.invalid())
      {
        return ConvertInvalidResult<AccumulatorStorage>(std::move(storeResult));
      }
      std::unique_ptr<ITemporaryRecordStore> store = std::move(storeResult.value());
      if(store == nullptr)
      {
        return MakeErrorResult<AccumulatorStorage>(-34062, "The temporary-record provider returned a null interpolation accumulator store.");
      }

      try
      {
        storage.m_Caches.push_back(std::make_unique<BoundedRecordPageCache<T>>(*store, storage.m_RecordsPerPage, 8));
        storage.m_Stores.push_back(std::move(store));
      } catch(const std::bad_alloc&)
      {
        return MakeErrorResult<AccumulatorStorage>(-34063, "Unable to allocate the bounded interpolation accumulator cache.");
      }
    }
    return {std::move(storage)};
  }

  AccumulatorStorage() = default;
  AccumulatorStorage(AccumulatorStorage&&) noexcept = default;
  AccumulatorStorage& operator=(AccumulatorStorage&&) noexcept = default;
  AccumulatorStorage(const AccumulatorStorage&) = delete;
  AccumulatorStorage& operator=(const AccumulatorStorage&) = delete;

  /** @brief Reads one accumulator through the active direct or cached backend. */
  Result<T> read(usize arrayIndex, uint64 recordIndex, const std::atomic_bool& shouldCancel)
  {
    if(!m_Direct.empty())
    {
      return {m_Direct[arrayIndex][static_cast<usize>(recordIndex)]};
    }
    return m_Caches[arrayIndex]->read(recordIndex, shouldCancel);
  }

  /** @brief Replaces one accumulator and marks its external cache page dirty when needed. */
  Result<> write(usize arrayIndex, uint64 recordIndex, const T& value, const std::atomic_bool& shouldCancel)
  {
    if(!m_Direct.empty())
    {
      m_Direct[arrayIndex][static_cast<usize>(recordIndex)] = value;
      return {};
    }
    return m_Caches[arrayIndex]->write(recordIndex, value, shouldCancel);
  }

  /** @brief Commits every dirty page before sequential output conversion. */
  Result<> flush(const std::atomic_bool& shouldCancel)
  {
    for(auto& cache : m_Caches)
    {
      if(Result<> result = cache->flush(shouldCancel); result.invalid())
      {
        return result;
      }
    }
    return {};
  }

  /** @brief Bulk-reads a contiguous accumulator range without populating the random-access cache. */
  Result<> readPage(usize arrayIndex, uint64 recordOffset, nonstd::span<T> records, const std::atomic_bool& shouldCancel) const
  {
    if(recordOffset + records.size() > m_RecordCount)
    {
      return MakeErrorResult(-34064, "Interpolation accumulator page request exceeds the temporary store.");
    }
    if(!m_Direct.empty())
    {
      std::copy_n(m_Direct[arrayIndex].data() + static_cast<usize>(recordOffset), records.size(), records.data());
      return {};
    }

    auto bytes = nonstd::span<std::byte>(reinterpret_cast<std::byte*>(records.data()), records.size() * sizeof(T));
    Result<uint64> result = m_Stores[arrayIndex]->read(recordOffset, records.size(), bytes, shouldCancel);
    if(result.invalid())
    {
      return ConvertResult(std::move(result));
    }
    if(result.value() != records.size())
    {
      return MakeErrorResult(-34065, "Interpolation accumulator temporary store returned a short page read.");
    }
    return {};
  }

  /** @brief Returns the transfer granularity used for bounded output conversion. */
  usize recordsPerPage() const
  {
    return static_cast<usize>(m_RecordsPerPage);
  }

private:
  uint64 m_RecordCount = 0;
  uint64 m_RecordsPerPage = 1;
  std::vector<std::vector<T>> m_Direct;
  std::vector<std::unique_ptr<ITemporaryRecordStore>> m_Stores;
  std::vector<std::unique_ptr<BoundedRecordPageCache<T>>> m_Caches;
};

/**
 * @brief Converts one accumulator vector to its output array in bounded pages.
 * @p converter derives the requested statistic without exposing storage details
 * to the filter-specific output code.
 */
template <typename RecordT, typename OutputT, typename Converter>
Result<> WriteAccumulatorOutput(AbstractDataStore<OutputT>& outputStore, const AccumulatorStorage<RecordT>& records, usize arrayIndex, usize recordCount, Converter&& converter,
                                const std::atomic_bool& shouldCancel)
{
  const usize pageSize = std::min(recordCount, records.recordsPerPage());
  std::vector<RecordT> recordPage(pageSize);
  auto outputPage = std::make_unique<OutputT[]>(pageSize);
  for(usize offset = 0; offset < recordCount; offset += pageSize)
  {
    if(shouldCancel)
    {
      return {};
    }
    const usize count = std::min(pageSize, recordCount - offset);
    if(Result<> result = records.readPage(arrayIndex, offset, nonstd::span<RecordT>(recordPage.data(), count), shouldCancel); result.invalid())
    {
      return result;
    }
    for(usize i = 0; i < count; ++i)
    {
      outputPage[i] = static_cast<OutputT>(converter(recordPage[i]));
    }
    if(Result<> result = outputStore.copyFromBuffer(offset, nonstd::span<const OutputT>(outputPage.get(), count)); result.invalid())
    {
      return result;
    }
  }
  return {};
}

/** @brief Type-dispatch adapter that normalizes a source array to float64 values. */
struct ExtractAsFloat64Functor
{
  /** @brief Returns a resident float64 copy used repeatedly by the vertex/kernel traversal. */
  template <typename T>
  std::vector<float64> operator()(IDataArray* sourceArray)
  {
    auto& store = sourceArray->template getIDataStoreRefAs<AbstractDataStore<T>>();
    usize numTuples = store.getNumberOfTuples();
    std::vector<float64> result(numTuples);
    for(usize i = 0; i < numTuples; i++)
    {
      result[i] = static_cast<float64>(store[i]);
    }
    return result;
  }
};

/** @brief Type-dispatch adapter that writes copy-mode weighted averages. */
struct WriteWeightedAverageFunctor
{
  /** @brief Converts one CopyAccumulator vector into the runtime-typed output store. */
  template <typename T>
  Result<> operator()(IDataArray* outputArray, const AccumulatorStorage<CopyAccumulator>& accumulators, usize arrayIndex, usize numVoxels, const std::atomic_bool& shouldCancel)
  {
    auto& store = outputArray->template getIDataStoreRefAs<AbstractDataStore<T>>();
    return WriteAccumulatorOutput(
        store, accumulators, arrayIndex, numVoxels, [](const CopyAccumulator& accumulator) { return accumulator.weightSum > 0.0 ? accumulator.weightedSum / accumulator.weightSum : 0.0; },
        shouldCancel);
  }
};

/**
 * @brief Precomputes uniform or Gaussian weights for the small 3D kernel.
 * The kernel is reused for every vertex, avoiding repeated exponential work.
 */
void computeKernel(uint64 interpolationTechnique, const std::vector<float32>& sigmas, std::vector<float32>& kernel, const int64 kernelNumVoxels[3])
{
  const auto kDimX = static_cast<usize>(2 * kernelNumVoxels[0] + 1);
  const auto kDimY = static_cast<usize>(2 * kernelNumVoxels[1] + 1);

  for(int64 z = -kernelNumVoxels[2]; z <= kernelNumVoxels[2]; z++)
  {
    for(int64 y = -kernelNumVoxels[1]; y <= kernelNumVoxels[1]; y++)
    {
      for(int64 x = -kernelNumVoxels[0]; x <= kernelNumVoxels[0]; x++)
      {
        const auto kx = static_cast<usize>(x + kernelNumVoxels[0]);
        const auto ky = static_cast<usize>(y + kernelNumVoxels[1]);
        const auto kz = static_cast<usize>(z + kernelNumVoxels[2]);
        const usize idx = kz * kDimY * kDimX + ky * kDimX + kx;

        if(interpolationTechnique == InterpolatePointCloudToRegularGrid::k_Uniform)
        {
          kernel[idx] = 1.0f;
        }
        else if(interpolationTechnique == InterpolatePointCloudToRegularGrid::k_Gaussian)
        {
          kernel[idx] = std::exp(-((static_cast<float32>(x * x) / (2.0f * sigmas[0] * sigmas[0])) + (static_cast<float32>(y * y) / (2.0f * sigmas[1] * sigmas[1])) +
                                   (static_cast<float32>(z * z) / (2.0f * sigmas[2] * sigmas[2]))));
        }
      }
    }
  }
}
} // namespace

// -----------------------------------------------------------------------------
InterpolatePointCloudToRegularGrid::InterpolatePointCloudToRegularGrid(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel,
                                                                       InterpolatePointCloudToRegularGridInputValues* inputValues)
: m_DataStructure(dataStructure)
, m_InputValues(inputValues)
, m_ShouldCancel(shouldCancel)
, m_MessageHandler(mesgHandler)
{
}

// -----------------------------------------------------------------------------
InterpolatePointCloudToRegularGrid::~InterpolatePointCloudToRegularGrid() noexcept = default;

// -----------------------------------------------------------------------------
const std::atomic_bool& InterpolatePointCloudToRegularGrid::getCancel()
{
  return m_ShouldCancel;
}

// -----------------------------------------------------------------------------
Result<> InterpolatePointCloudToRegularGrid::operator()()
{
  const auto* vertices = m_DataStructure.getDataAs<VertexGeom>(m_InputValues->vertexGeomPath);
  const auto* image = m_DataStructure.getDataAs<ImageGeom>(m_InputValues->imageGeomPath);
  const DataPath interpolatedGroupPath = image->getCellDataPath();
  const SizeVec3 dims = image->getDimensions();
  const FloatVec3 res = image->getSpacing();

  const usize dimX = dims[0];
  const usize dimY = dims[1];
  const usize dimZ = dims[2];
  const usize numVoxels = dimX * dimY * dimZ;

  const usize numVerts = vertices->getNumberOfVertices();

  // Kernel dimensions in voxels
  int64 kernelNumVoxels[3] = {0, 0, 0};
  kernelNumVoxels[0] = static_cast<int64>(std::ceil((m_InputValues->kernelSize[0] / res[0]) * 0.5f));
  kernelNumVoxels[1] = static_cast<int64>(std::ceil((m_InputValues->kernelSize[1] / res[1]) * 0.5f));
  kernelNumVoxels[2] = static_cast<int64>(std::ceil((m_InputValues->kernelSize[2] / res[2]) * 0.5f));

  if(m_InputValues->kernelSize[0] < res[0])
  {
    kernelNumVoxels[0] = 0;
  }
  if(m_InputValues->kernelSize[1] < res[1])
  {
    kernelNumVoxels[1] = 0;
  }
  if(m_InputValues->kernelSize[2] < res[2])
  {
    kernelNumVoxels[2] = 0;
  }

  const auto kDimX = static_cast<usize>(2 * kernelNumVoxels[0] + 1);
  const auto kDimY = static_cast<usize>(2 * kernelNumVoxels[1] + 1);
  const auto kDimZ = static_cast<usize>(2 * kernelNumVoxels[2] + 1);
  const usize totalKernel = kDimX * kDimY * kDimZ;

  // Compute kernel weights
  std::vector<float32> kernel(totalKernel, 0.0f);
  computeKernel(m_InputValues->interpolationTechnique, m_InputValues->sigmas, kernel, kernelNumVoxels);

  // Mask
  std::unique_ptr<MaskCompareUtilities::MaskCompare> maskCompare = nullptr;
  if(m_InputValues->useMask)
  {
    try
    {
      maskCompare = MaskCompareUtilities::InstantiateMaskCompare(m_DataStructure, m_InputValues->maskDataPath);
    } catch(const std::exception& exception)
    {
      // This really should NOT be happening as the path was verified during preflight BUT we may be calling this from
      // somewhere else that is NOT going through the normal nx::core::IFilter API of Preflight and Execute
      std::string message = fmt::format("Mask Array DataPath does not exist or is not of the correct type (Bool | UInt8) {}", m_InputValues->maskDataPath.toString());
      return MakeErrorResult(-34060, message);
    }
  }

  const bool needWelford = m_InputValues->findStdDeviation;

  // Pre-extract interpolated source array data as float64
  std::vector<std::vector<float64>> interpSourceData;
  std::vector<IDataArray*> interpSourceArrays;
  for(const auto& path : m_InputValues->interpolatedDataPaths)
  {
    auto* sourceArray = m_DataStructure.getDataAs<IDataArray>(path);
    if(sourceArray->getDataType() == DataType::boolean)
    {
      continue;
    }
    interpSourceArrays.push_back(sourceArray);
    interpSourceData.push_back(ExecuteDataFunction(ExtractAsFloat64Functor{}, sourceArray->getDataType(), sourceArray));
  }

  // Pre-extract copy source array data as float64
  std::vector<std::vector<float64>> copySourceData;
  std::vector<IDataArray*> copySourceArrays;
  for(const auto& path : m_InputValues->copyDataPaths)
  {
    auto* sourceArray = m_DataStructure.getDataAs<IDataArray>(path);
    if(sourceArray->getDataType() == DataType::boolean)
    {
      continue;
    }
    copySourceArrays.push_back(sourceArray);
    copySourceData.push_back(ExecuteDataFunction(ExtractAsFloat64Functor{}, sourceArray->getDataType(), sourceArray));
  }

  const usize numInterpArrays = interpSourceArrays.size();
  const usize numCopyArrays = copySourceArrays.size();

  bool usesOutOfCoreStore = false;
  const auto observeOutput = [&](const DataPath& path) {
    if(const auto* output = m_DataStructure.getDataAs<IDataArray>(path); output != nullptr && IsOutOfCore(*output))
    {
      usesOutOfCoreStore = true;
    }
  };
  for(const IDataArray* sourceArray : interpSourceArrays)
  {
    const std::string& arrayName = sourceArray->getName();
    observeOutput(interpolatedGroupPath.createChildPath(arrayName));
    if(m_InputValues->findLength)
    {
      observeOutput(interpolatedGroupPath.createChildPath(arrayName + m_InputValues->lengthSuffix));
    }
    if(m_InputValues->findMin)
    {
      observeOutput(interpolatedGroupPath.createChildPath(arrayName + m_InputValues->minSuffix));
    }
    if(m_InputValues->findMax)
    {
      observeOutput(interpolatedGroupPath.createChildPath(arrayName + m_InputValues->maxSuffix));
    }
    if(m_InputValues->findMean)
    {
      observeOutput(interpolatedGroupPath.createChildPath(arrayName + m_InputValues->meanSuffix));
    }
    if(m_InputValues->findStdDeviation)
    {
      observeOutput(interpolatedGroupPath.createChildPath(arrayName + m_InputValues->stdDeviationSuffix));
    }
    if(m_InputValues->findSummation)
    {
      observeOutput(interpolatedGroupPath.createChildPath(arrayName + m_InputValues->summationSuffix));
    }
  }
  for(const IDataArray* sourceArray : copySourceArrays)
  {
    observeOutput(interpolatedGroupPath.createChildPath(sourceArray->getName()));
  }

  const bool useExternalState = !ForceInCoreAlgorithm() && (usesOutOfCoreStore || ForceOocAlgorithm());
  RecordAlgorithmPathExecution(useExternalState ? AlgorithmPath::OutOfCore : AlgorithmPath::InCore, usesOutOfCoreStore);

  auto interpStorageResult = AccumulatorStorage<VoxelAccumulator>::Create(numInterpArrays, numVoxels, useExternalState, usesOutOfCoreStore, m_ShouldCancel);
  if(interpStorageResult.invalid())
  {
    return ConvertResult(std::move(interpStorageResult));
  }
  AccumulatorStorage<VoxelAccumulator> interpAccum = std::move(interpStorageResult.value());

  auto copyStorageResult = AccumulatorStorage<CopyAccumulator>::Create(numCopyArrays, numVoxels, useExternalState, usesOutOfCoreStore, m_ShouldCancel);
  if(copyStorageResult.invalid())
  {
    return ConvertResult(std::move(copyStorageResult));
  }
  AccumulatorStorage<CopyAccumulator> copyAccum = std::move(copyStorageResult.value());

  // Main vertex loop: each accepted point updates only the clipped kernel
  // neighborhood. Voxel accumulators may reside behind bounded external pages.
  const usize progIncrement = numVerts / 100;
  usize prog = 1;

  for(usize i = 0; i < numVerts; i++)
  {
    if(m_ShouldCancel)
    {
      return {};
    }

    if(m_InputValues->useMask && !maskCompare->isTrue(i)) // lazy-evaluation ensures the pointer is never accessed nullptr
    {
      continue;
    }

    const Point3D<float32> coords = vertices->getVertexCoordinate(i);
    const std::optional<usize> optIndex = image->getIndex(coords[0], coords[1], coords[2]);
    if(!optIndex.has_value())
    {
      continue;
    }
    const usize index = optIndex.value();

    const usize curX = index % dimX;
    const usize curY = (index / dimX) % dimY;
    const usize curZ = index / (dimX * dimY);

    // Compute clipped kernel bounds in grid space (symmetric in all 3 dimensions)
    const int64 startX = std::max(static_cast<int64>(0), static_cast<int64>(curX) - kernelNumVoxels[0]);
    const int64 startY = std::max(static_cast<int64>(0), static_cast<int64>(curY) - kernelNumVoxels[1]);
    const int64 startZ = std::max(static_cast<int64>(0), static_cast<int64>(curZ) - kernelNumVoxels[2]);
    const int64 endX = std::min(static_cast<int64>(dimX) - 1, static_cast<int64>(curX) + kernelNumVoxels[0]);
    const int64 endY = std::min(static_cast<int64>(dimY) - 1, static_cast<int64>(curY) + kernelNumVoxels[1]);
    const int64 endZ = std::min(static_cast<int64>(dimZ) - 1, static_cast<int64>(curZ) + kernelNumVoxels[2]);

    // Traverse kernel
    for(int64 gz = startZ; gz <= endZ; gz++)
    {
      for(int64 gy = startY; gy <= endY; gy++)
      {
        for(int64 gx = startX; gx <= endX; gx++)
        {
          // Compute kernel index using 3D offset
          const auto kx = static_cast<usize>(gx - static_cast<int64>(curX) + kernelNumVoxels[0]);
          const auto ky = static_cast<usize>(gy - static_cast<int64>(curY) + kernelNumVoxels[1]);
          const auto kz = static_cast<usize>(gz - static_cast<int64>(curZ) + kernelNumVoxels[2]);
          const usize kernelIdx = kz * kDimY * kDimX + ky * kDimX + kx;

          const float32 weight = kernel[kernelIdx];
          const usize voxelIdx = static_cast<usize>(gz) * dimX * dimY + static_cast<usize>(gy) * dimX + static_cast<usize>(gx);

          // Update interpolated array accumulators (using actual kernel weight)
          if(weight != 0.0f)
          {
            const auto w = static_cast<float64>(weight);
            for(usize a = 0; a < numInterpArrays; a++)
            {
              const float64 sourceVal = interpSourceData[a][i];
              const float64 weightedVal = w * sourceVal;

              Result<VoxelAccumulator> accumulatorResult = interpAccum.read(a, voxelIdx, m_ShouldCancel);
              if(accumulatorResult.invalid())
              {
                return ConvertResult(std::move(accumulatorResult));
              }
              VoxelAccumulator accum = accumulatorResult.value();
              if(accum.count == 0)
              {
                accum.min = weightedVal;
                accum.max = weightedVal;
              }
              accum.count++;
              accum.weightedSum += weightedVal;
              accum.weightSum += w;
              accum.min = std::min(accum.min, weightedVal);
              accum.max = std::max(accum.max, weightedVal);

              if(needWelford)
              {
                const float64 delta = weightedVal - accum.welfordMean;
                accum.welfordMean += delta / static_cast<float64>(accum.count);
                const float64 delta2 = weightedVal - accum.welfordMean;
                accum.welfordM2 += delta * delta2;
              }
              if(Result<> result = interpAccum.write(a, voxelIdx, accum, m_ShouldCancel); result.invalid())
              {
                return result;
              }
            }
          }

          // Update copy array accumulators (always uniform weight = 1.0)
          for(usize a = 0; a < numCopyArrays; a++)
          {
            const float64 sourceVal = copySourceData[a][i];
            Result<CopyAccumulator> accumulatorResult = copyAccum.read(a, voxelIdx, m_ShouldCancel);
            if(accumulatorResult.invalid())
            {
              return ConvertResult(std::move(accumulatorResult));
            }
            CopyAccumulator accumulator = accumulatorResult.value();
            accumulator.weightedSum += sourceVal;
            accumulator.weightSum += 1.0;
            if(Result<> result = copyAccum.write(a, voxelIdx, accumulator, m_ShouldCancel); result.invalid())
            {
              return result;
            }
          }
        }
      }
    }

    if(i > prog)
    {
      const auto progressInt = static_cast<usize>((static_cast<float64>(i) / static_cast<float64>(numVerts)) * 100.0);
      m_MessageHandler(IFilter::Message::Type::Info, fmt::format("Interpolating Point Cloud || {}% Completed", progressInt));
      prog += progIncrement;
    }
  }

  // Finalization pass - write outputs
  m_MessageHandler(IFilter::Message::Type::Info, "Writing interpolated results...");
  if(Result<> result = interpAccum.flush(m_ShouldCancel); result.invalid())
  {
    return result;
  }
  if(Result<> result = copyAccum.flush(m_ShouldCancel); result.invalid())
  {
    return result;
  }

  for(usize a = 0; a < numInterpArrays; a++)
  {
    const std::string& arrayName = interpSourceArrays[a]->getName();

    // Write interpolated (weighted average) output
    auto& interpOutput = m_DataStructure.getDataRefAs<Float64Array>(interpolatedGroupPath.createChildPath(arrayName));
    if(Result<> result = WriteAccumulatorOutput(
           interpOutput.getDataStoreRef(), interpAccum, a, numVoxels,
           [](const VoxelAccumulator& accumulator) { return accumulator.weightSum > 0.0 ? accumulator.weightedSum / accumulator.weightSum : 0.0; }, m_ShouldCancel);
       result.invalid())
    {
      return result;
    }

    // Write statistics arrays
    if(m_InputValues->findLength)
    {
      auto& lengthOutput = m_DataStructure.getDataRefAs<UInt64Array>(interpolatedGroupPath.createChildPath(arrayName + m_InputValues->lengthSuffix));
      if(Result<> result = WriteAccumulatorOutput(
             lengthOutput.getDataStoreRef(), interpAccum, a, numVoxels, [](const VoxelAccumulator& accumulator) { return accumulator.count; }, m_ShouldCancel);
         result.invalid())
      {
        return result;
      }
    }
    if(m_InputValues->findMin)
    {
      auto& minOutput = m_DataStructure.getDataRefAs<Float32Array>(interpolatedGroupPath.createChildPath(arrayName + m_InputValues->minSuffix));
      if(Result<> result = WriteAccumulatorOutput(
             minOutput.getDataStoreRef(), interpAccum, a, numVoxels, [](const VoxelAccumulator& accumulator) { return accumulator.count > 0 ? accumulator.min : 0.0; }, m_ShouldCancel);
         result.invalid())
      {
        return result;
      }
    }
    if(m_InputValues->findMax)
    {
      auto& maxOutput = m_DataStructure.getDataRefAs<Float32Array>(interpolatedGroupPath.createChildPath(arrayName + m_InputValues->maxSuffix));
      if(Result<> result = WriteAccumulatorOutput(
             maxOutput.getDataStoreRef(), interpAccum, a, numVoxels, [](const VoxelAccumulator& accumulator) { return accumulator.count > 0 ? accumulator.max : 0.0; }, m_ShouldCancel);
         result.invalid())
      {
        return result;
      }
    }
    if(m_InputValues->findMean)
    {
      auto& meanOutput = m_DataStructure.getDataRefAs<Float32Array>(interpolatedGroupPath.createChildPath(arrayName + m_InputValues->meanSuffix));
      if(Result<> result = WriteAccumulatorOutput(
             meanOutput.getDataStoreRef(), interpAccum, a, numVoxels,
             [](const VoxelAccumulator& accumulator) { return accumulator.count > 0 ? accumulator.weightedSum / static_cast<float64>(accumulator.count) : 0.0; }, m_ShouldCancel);
         result.invalid())
      {
        return result;
      }
    }
    if(m_InputValues->findStdDeviation)
    {
      auto& stdDevOutput = m_DataStructure.getDataRefAs<Float32Array>(interpolatedGroupPath.createChildPath(arrayName + m_InputValues->stdDeviationSuffix));
      if(Result<> result = WriteAccumulatorOutput(
             stdDevOutput.getDataStoreRef(), interpAccum, a, numVoxels,
             [](const VoxelAccumulator& accumulator) { return accumulator.count > 0 ? std::sqrt(accumulator.welfordM2 / static_cast<float64>(accumulator.count)) : 0.0; }, m_ShouldCancel);
         result.invalid())
      {
        return result;
      }
    }
    if(m_InputValues->findSummation)
    {
      auto& sumOutput = m_DataStructure.getDataRefAs<Float32Array>(interpolatedGroupPath.createChildPath(arrayName + m_InputValues->summationSuffix));
      if(Result<> result = WriteAccumulatorOutput(
             sumOutput.getDataStoreRef(), interpAccum, a, numVoxels, [](const VoxelAccumulator& accumulator) { return accumulator.weightedSum; }, m_ShouldCancel);
         result.invalid())
      {
        return result;
      }
    }
  }

  // Write copy array outputs
  for(usize a = 0; a < numCopyArrays; a++)
  {
    auto* outputArray = m_DataStructure.getDataAs<IDataArray>(interpolatedGroupPath.createChildPath(copySourceArrays[a]->getName()));
    if(Result<> result = ExecuteDataFunction(WriteWeightedAverageFunctor{}, outputArray->getDataType(), outputArray, copyAccum, a, numVoxels, m_ShouldCancel); result.invalid())
    {
      return result;
    }
  }

  return {};
}
