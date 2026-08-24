#include "ResampleImageGeom.hpp"

#include "simplnx/DataStructure/DataArray.hpp"
#include "simplnx/DataStructure/Geometry/ImageGeom.hpp"
#include "simplnx/DataStructure/StringArray.hpp"
#include "simplnx/Utilities/DataArrayUtilities.hpp"
#include "simplnx/Utilities/MessageHelper.hpp"
#include "simplnx/Utilities/ParallelAlgorithmUtilities.hpp"
#include "simplnx/Utilities/ParallelDataAlgorithm.hpp"
#include "simplnx/Utilities/ParallelTaskAlgorithm.hpp"
#include "simplnx/Utilities/SamplingUtils.hpp"

#include <limits>
#include <memory>

using namespace nx::core;

namespace
{
// Sentinel marking a destination axis position that falls outside the source Image Geometry's bounds
// along that axis (no source cell to copy from - the destination voxel is filled with zero instead).
constexpr usize k_InvalidAxisIndex = std::numeric_limits<usize>::max();

/**
 * @brief Resolves, for every destination Image Geometry coordinate along a single axis, the source
 * Image Geometry cell index that coordinate's min-corner falls into (or k_InvalidAxisIndex if that
 * destination position lies outside the source geometry's bounds along this axis).
 *
 * WHY this is hoisted out of the voxel loop: both geometries are axis-aligned regular grids, so
 * ImageGeom::getIndex()'s bounds check and floor-divide are each fully separable per axis - the source
 * index (or out-of-bounds result) for a given destination x position is the same regardless of the y or
 * z position, and likewise for y and z. This function reproduces that exact per-axis test (same bounds
 * comparison, same floor-divide, same float64 promotion of the underlying float32 origin/spacing) once
 * per axis position - bounded by that axis' destination dimension - instead of re-deriving it for every
 * voxel that shares the position along this axis.
 *
 * @param destDimSize Number of destination coordinates to resolve along this axis
 * @param destOriginComp Destination Image Geometry origin component for this axis
 * @param destSpacingComp Destination Image Geometry spacing component for this axis
 * @param srcDimSize Source Image Geometry dimension for this axis
 * @param srcOriginComp Source Image Geometry origin component for this axis
 * @param srcSpacingComp Source Image Geometry spacing component for this axis
 * @return A vector of length destDimSize mapping each destination axis position to its source axis
 * index, or k_InvalidAxisIndex if the destination position falls outside the source geometry
 */
std::vector<usize> ComputeAxisSrcIndices(usize destDimSize, float64 destOriginComp, float64 destSpacingComp, usize srcDimSize, float64 srcOriginComp, float64 srcSpacingComp)
{
  std::vector<usize> srcIndices(destDimSize, k_InvalidAxisIndex);
  const float64 srcMaxCoord = static_cast<float64>(srcDimSize) * srcSpacingComp + srcOriginComp;
  for(usize i = 0; i < destDimSize; i++)
  {
    const float64 destCoord = static_cast<float64>(i) * destSpacingComp + destOriginComp;
    if(destCoord < srcOriginComp || destCoord > srcMaxCoord)
    {
      continue;
    }
    const auto srcIdx = static_cast<usize>(std::floor((destCoord - srcOriginComp) / srcSpacingComp));
    if(srcIdx < srcDimSize)
    {
      srcIndices[i] = srcIdx;
    }
  }
  return srcIndices;
}

/**
 * @brief Copies one cell-data array from the source Image Geometry to the resampled destination Image
 * Geometry by nearest-source-cell lookup.
 *
 * WHY buffered row I/O instead of per-voxel copyFrom: destination cells sharing the same (y, z) are
 * contiguous along x in the backing store, and (per ComputeAxisSrcIndices) the source cell a given
 * destination x maps to is identical for every row. This lets a whole destination row be assembled in a
 * bounded, reusable buffer - gathering the mapped source values locally - and written out with a single
 * copyFromBuffer call, instead of one CopyData call (a chunk-cache round trip per voxel when either store
 * is out-of-core) for every destination cell. The source row needed for a given (y, z) is itself read once
 * via copyIntoBuffer (and reused across consecutive destination rows mapping to the same source row, which
 * is common when upsampling) rather than re-read per voxel. Both buffers are bounded by an axis dimension,
 * never by the total cell count of either geometry.
 */
template <typename T>
class ResampleImageGeomArrayImpl
{
public:
  ResampleImageGeomArrayImpl(ResampleImageGeom* algorithm, const IDataArray& srcArray, IDataArray& destArray, const ImageGeom& srcImageGeom, const ImageGeom& destImageGeom,
                             const std::atomic_bool& shouldCancel)
  : m_AlgorithmPtr(algorithm)
  , m_SrcArray(srcArray)
  , m_DestArray(destArray)
  , m_SrcImageGeom(srcImageGeom)
  , m_DestImageGeom(destImageGeom)
  , m_ShouldCancel(shouldCancel)
  {
  }

  void operator()() const
  {
    const auto& srcDataStore = m_SrcArray.template getIDataStoreRefAs<AbstractDataStore<T>>();
    auto& destDataStore = m_DestArray.template getIDataStoreRefAs<AbstractDataStore<T>>();

    const SizeVec3 srcDims = m_SrcImageGeom.getDimensions();
    const SizeVec3 destDims = m_DestImageGeom.getDimensions();
    const FloatVec3 srcOrigin = m_SrcImageGeom.getOrigin();
    const FloatVec3 srcSpacing = m_SrcImageGeom.getSpacing();
    const FloatVec3 destOrigin = m_DestImageGeom.getOrigin();
    const FloatVec3 destSpacing = m_DestImageGeom.getSpacing();

    // Precompute, once per axis, which source cell (if any) each destination coordinate along that
    // axis maps to - see ComputeAxisSrcIndices for why this only needs to run per-axis, not per-voxel.
    const std::vector<usize> xIndices = ComputeAxisSrcIndices(destDims[0], destOrigin[0], destSpacing[0], srcDims[0], srcOrigin[0], srcSpacing[0]);
    const std::vector<usize> yIndices = ComputeAxisSrcIndices(destDims[1], destOrigin[1], destSpacing[1], srcDims[1], srcOrigin[1], srcSpacing[1]);
    const std::vector<usize> zIndices = ComputeAxisSrcIndices(destDims[2], destOrigin[2], destSpacing[2], srcDims[2], srcOrigin[2], srcSpacing[2]);

    const usize numComponents = m_DestArray.getNumberOfComponents();
    const usize destRowLength = destDims[0] * numComponents;
    const usize srcRowLength = srcDims[0] * numComponents;

    // Reusable row buffers allocated ONCE for the whole array - bounded by an axis dimension, not by
    // the total number of cells in either geometry.
    auto destRowBuffer = std::make_unique<T[]>(destRowLength);
    auto srcRowBuffer = std::make_unique<T[]>(srcRowLength);

    bool haveCachedSrcRow = false;
    usize cachedYIndex = k_InvalidAxisIndex;
    usize cachedZIndex = k_InvalidAxisIndex;

    const usize numVoxels = m_DestImageGeom.getNumberOfCells();
    const usize counterIncrement = numVoxels / 100 == 0 ? 100 : numVoxels / 100;
    usize processedVoxels = 0;
    usize counter = 0;

    for(usize z = 0; z < destDims[2]; z++)
    {
      if(m_ShouldCancel)
      {
        return;
      }

      const usize zIndex = zIndices[z];
      for(usize y = 0; y < destDims[1]; y++)
      {
        const usize yIndex = yIndices[y];
        const bool rowHasSource = (zIndex != k_InvalidAxisIndex) && (yIndex != k_InvalidAxisIndex);

        if(rowHasSource)
        {
          // Bulk-read the source row for this (yIndex, zIndex) once; skip the read if the previous
          // destination row already pulled from the same source row (common when upsampling).
          if(!haveCachedSrcRow || yIndex != cachedYIndex || zIndex != cachedZIndex)
          {
            const usize srcRowStart = ((srcDims[0] * srcDims[1] * zIndex) + (srcDims[0] * yIndex)) * numComponents;
            srcDataStore.copyIntoBuffer(srcRowStart, nonstd::span<T>(srcRowBuffer.get(), srcRowLength));
            cachedYIndex = yIndex;
            cachedZIndex = zIndex;
            haveCachedSrcRow = true;
          }

          // Gather the mapped source values into the destination row buffer - local memory access
          // against the two bounded buffers above, no store access per voxel.
          for(usize x = 0; x < destDims[0]; x++)
          {
            const usize xIndex = xIndices[x];
            T* destTuple = destRowBuffer.get() + (x * numComponents);
            if(xIndex != k_InvalidAxisIndex)
            {
              const T* srcTuple = srcRowBuffer.get() + (xIndex * numComponents);
              std::copy_n(srcTuple, numComponents, destTuple);
            }
            else
            {
              std::fill_n(destTuple, numComponents, static_cast<T>(0));
            }
          }
        }
        else
        {
          // The whole row falls outside the source geometry along y or z - matches the original
          // per-voxel fillTuple(0) fallback for an out-of-bounds source lookup.
          std::fill_n(destRowBuffer.get(), destRowLength, static_cast<T>(0));
        }

        // Bulk-write the fully assembled destination row in a single store access.
        const usize destRowStart = ((z * destDims[1] * destDims[0]) + (y * destDims[0])) * numComponents;
        destDataStore.copyFromBuffer(destRowStart, nonstd::span<const T>(destRowBuffer.get(), destRowLength));

        processedVoxels += destDims[0];
        counter += destDims[0];
        if(counter >= counterIncrement)
        {
          const float progress = static_cast<float>(processedVoxels) / static_cast<float>(numVoxels) * 100.0f;
          m_AlgorithmPtr->sendThreadSafeProgressMessage(fmt::format("Resampling Data Array '{}' {:.0f}% Complete", m_DestArray.getName(), progress));
          counter = 0;
        }
      }
    }
    m_AlgorithmPtr->sendThreadSafeProgressMessage(fmt::format("Resampling Data Array '{}' Complete", m_DestArray.getName()));
  }

private:
  ResampleImageGeom* m_AlgorithmPtr = nullptr;
  const IDataArray& m_SrcArray;
  IDataArray& m_DestArray;
  const ImageGeom& m_SrcImageGeom;
  const ImageGeom& m_DestImageGeom;
  const std::atomic_bool& m_ShouldCancel;
};
} // namespace

// -----------------------------------------------------------------------------
ResampleImageGeom::ResampleImageGeom(DataStructure& dataStructure, const IFilter::MessageHandler& msgHandler, const std::atomic_bool& shouldCancel, ResampleImageGeomInputValues* inputValues)
: m_DataStructure(dataStructure)
, m_InputValues(inputValues)
, m_ShouldCancel(shouldCancel)
, m_MessageHandler(msgHandler)
{
}

// -----------------------------------------------------------------------------
ResampleImageGeom::~ResampleImageGeom() noexcept = default;

// -----------------------------------------------------------------------------
const std::atomic_bool& ResampleImageGeom::getCancel()
{
  return m_ShouldCancel;
}

// -----------------------------------------------------------------------------
Result<> ResampleImageGeom::operator()()
{
  MessageHelper messageHelper(m_MessageHandler);
  ThrottledMessenger throttledMessenger = messageHelper.createThrottledMessenger();
  m_ThrottledMessengerPtr = &throttledMessenger;

  const auto& selectedImageGeom = m_DataStructure.getDataRefAs<ImageGeom>(m_InputValues->SelectedImageGeometryPath);

  auto& destImageGeom = m_DataStructure.getDataRefAs<ImageGeom>(m_InputValues->CreatedImageGeometryPath);
  const auto& srcCellDataAM = selectedImageGeom.getCellDataRef();
  auto& destCellDataAM = destImageGeom.getCellDataRef();

  usize arrayIndex = 0;
  usize totalArrays = srcCellDataAM.getSize();

  ParallelTaskAlgorithm taskRunner;
  taskRunner.setParallelizationEnabled(true);

  for(const auto& [dataId, oldDataObject] : srcCellDataAM)
  {
    if(m_ShouldCancel)
    {
      return {};
    }

    arrayIndex++;
    const auto& oldDataArray = dynamic_cast<const IDataArray&>(*oldDataObject);
    const std::string srcName = oldDataArray.getName();
    auto& newDataArray = dynamic_cast<IDataArray&>(destCellDataAM.at(srcName));
    m_MessageHandler(fmt::format("Resampling Data Array: '{}' ({}/{})", srcName, arrayIndex, totalArrays));

    ExecuteParallelFunction<ResampleImageGeomArrayImpl>(oldDataArray.getDataType(), taskRunner, this, oldDataArray, newDataArray, selectedImageGeom, destImageGeom, m_ShouldCancel);
  }

  taskRunner.wait(); // This will spill over if the number of geometries to process does not divide evenly by the number of threads.

  if(m_ShouldCancel)
  {
    return {};
  }

  // Careful with this next section. We purposefully copy in the original dataStructure arrays
  // into the destination feature attribute matrix so that we have somewhere to start.
  // During the renumbering phase is when those copied arrays will get potentially resized
  // to their proper number of tuples.
  DataPath cellFeatureAMPath = m_InputValues->CellFeatureAttributeMatrix;
  auto destImagePath = m_InputValues->CreatedImageGeometryPath;
  DataPath featureIdsArrayPath = m_InputValues->FeatureIdsArrayPath;

  if(m_InputValues->RenumberFeatures)
  {
    const auto& featureIds = m_DataStructure.getDataRefAs<Int32Array>(featureIdsArrayPath);
    auto validateNumFeatResult = ValidateFeatureIdsToFeatureAttributeMatrixIndexing(m_DataStructure, cellFeatureAMPath, featureIds, false, m_MessageHandler);
    if(validateNumFeatResult.invalid())
    {
      return validateNumFeatResult;
    }

    std::vector<DataPath> sourceFeatureDataPaths;
    auto childPathsResult = GetAllChildArrayDataPaths(m_DataStructure, cellFeatureAMPath);
    if(childPathsResult.has_value())
    {
      sourceFeatureDataPaths = childPathsResult.value();
    }
    std::vector<DataPath> destFeatureDataPaths = sourceFeatureDataPaths;
    DataPath destCellFeatureAMPath = destImagePath.createChildPath(cellFeatureAMPath.getTargetName());

    for(auto& dataPath : destFeatureDataPaths)
    {
      dataPath = destCellFeatureAMPath.createChildPath(dataPath.getTargetName());
    }

    // Loop over all the DataPaths and do a deep copy on each DataArray|StringArray
    // so that the updating of the Feature level data can happen. We do a bit of
    // under-the-covers where we actually remove the existing array that preflight
    // created, so we can use the convenience of the DataArray.deepCopy() function.
    for(size_t index = 0; index < sourceFeatureDataPaths.size(); index++)
    {
      DataObject* dataObject = m_DataStructure.getData(sourceFeatureDataPaths[index]);
      if(dataObject->getDataObjectType() == DataObject::Type::DataArray)
      {
        auto result = DeepCopy<IDataArray>(m_DataStructure, sourceFeatureDataPaths[index], destFeatureDataPaths[index]);
        if(result.invalid())
        {
          return result;
        }
      }
      else if(dataObject->getDataObjectType() == DataObject::Type::StringArray)
      {
        auto result = DeepCopy<StringArray>(m_DataStructure, sourceFeatureDataPaths[index], destFeatureDataPaths[index]);
        if(result.invalid())
        {
          return result;
        }
      }
    }

    // NOW DO THE ACTUAL RENUMBERING and updating.
    DataPath destFeatureIdsPath = destImagePath.createChildPath(srcCellDataAM.getName()).createChildPath(featureIdsArrayPath.getTargetName());
    return Sampling::RenumberFeatures(m_DataStructure, destImagePath, destCellFeatureAMPath, featureIdsArrayPath, destFeatureIdsPath, m_MessageHandler, m_ShouldCancel);
  }

  return {};
}

void ResampleImageGeom::sendThreadSafeProgressMessage(const std::string& message)
{
  std::lock_guard<std::mutex> guard(m_ProgressMessage_Mutex);
  if(nullptr != m_ThrottledMessengerPtr)
  {
    m_ThrottledMessengerPtr->sendThrottledMessage([&]() { return message; });
  }
}
