#include "NearestPointFuseRegularGridsScanline.hpp"

#include "simplnx/DataStructure/DataArray.hpp"
#include "simplnx/DataStructure/Geometry/ImageGeom.hpp"
#include "simplnx/Utilities/FilterUtilities.hpp"

#include <algorithm>
#include <limits>
#include <memory>
#include <vector>

using namespace nx::core;

namespace
{
constexpr usize k_InvalidIndex = std::numeric_limits<usize>::max();

/**
 * @brief Maps each reference coordinate on one axis to its containing sampling-cell index.
 *
 * Precomputing these O(axis length) maps removes repeated coordinate arithmetic
 * from every array and marks out-of-bounds positions explicitly for fill handling.
 */
std::vector<usize> ComputeAxisIndices(usize referenceDim, float32 referenceOrigin, float32 referenceSpacing, usize sampleDim, float32 sampleOrigin, float32 sampleSpacing)
{
  std::vector<usize> indices(referenceDim, k_InvalidIndex);
  for(usize index = 0; index < referenceDim; index++)
  {
    const float32 coordinate = index * referenceSpacing + referenceOrigin;
    if(coordinate - sampleOrigin < 0)
    {
      continue;
    }
    const usize sampleIndex = static_cast<usize>((coordinate - sampleOrigin) / sampleSpacing);
    if(sampleIndex < sampleDim)
    {
      indices[index] = sampleIndex;
    }
  }
  return indices;
}

/**
 * @brief Resamples one typed array with checked source-row reads and destination-row writes.
 *
 * A row cache is reused across repeated mapped Y/Z coordinates. The X map is then
 * applied entirely in memory, keeping both I/O and scratch bounded to row size.
 */
template <typename T>
Result<> CopyArray(const IDataArray& sourceArray, IDataArray& destinationArray, const ImageGeom& sampleGeom, const ImageGeom& referenceGeom, float64 fillValue, const std::atomic_bool& shouldCancel)
{
  const auto& sourceStore = sourceArray.template getIDataStoreRefAs<AbstractDataStore<T>>();
  auto& destinationStore = destinationArray.template getIDataStoreRefAs<AbstractDataStore<T>>();
  const auto sampleDims = sampleGeom.getDimensions();
  const auto referenceDims = referenceGeom.getDimensions();
  const auto sampleOrigin = sampleGeom.getOrigin();
  const auto referenceOrigin = referenceGeom.getOrigin();
  const auto sampleSpacing = sampleGeom.getSpacing();
  const auto referenceSpacing = referenceGeom.getSpacing();
  const auto xIndices = ComputeAxisIndices(referenceDims[0], referenceOrigin[0], referenceSpacing[0], sampleDims[0], sampleOrigin[0], sampleSpacing[0]);
  const auto yIndices = ComputeAxisIndices(referenceDims[1], referenceOrigin[1], referenceSpacing[1], sampleDims[1], sampleOrigin[1], sampleSpacing[1]);
  const auto zIndices = ComputeAxisIndices(referenceDims[2], referenceOrigin[2], referenceSpacing[2], sampleDims[2], sampleOrigin[2], sampleSpacing[2]);
  const usize components = destinationArray.getNumberOfComponents();
  const usize sourceRowValues = sampleDims[0] * components;
  const usize destinationRowValues = referenceDims[0] * components;
  auto sourceRow = std::make_unique<T[]>(sourceRowValues);
  auto destinationRow = std::make_unique<T[]>(destinationRowValues);
  bool haveSourceRow = false;
  usize cachedY = k_InvalidIndex;
  usize cachedZ = k_InvalidIndex;
  for(usize z = 0; z < referenceDims[2]; z++)
  {
    if(shouldCancel)
    {
      return {};
    }
    for(usize y = 0; y < referenceDims[1]; y++)
    {
      const usize sampleY = yIndices[y];
      const usize sampleZ = zIndices[z];
      if(sampleY != k_InvalidIndex && sampleZ != k_InvalidIndex)
      {
        if(!haveSourceRow || cachedY != sampleY || cachedZ != sampleZ)
        {
          const usize sourceOffset = ((sampleZ * sampleDims[1] * sampleDims[0]) + (sampleY * sampleDims[0])) * components;
          auto result = sourceStore.copyIntoBuffer(sourceOffset, nonstd::span<T>(sourceRow.get(), sourceRowValues));
          if(result.invalid())
          {
            return result;
          }
          haveSourceRow = true;
          cachedY = sampleY;
          cachedZ = sampleZ;
        }
        for(usize x = 0; x < referenceDims[0]; x++)
        {
          T* destinationTuple = destinationRow.get() + x * components;
          if(xIndices[x] == k_InvalidIndex)
          {
            std::fill_n(destinationTuple, components, static_cast<T>(fillValue));
          }
          else
          {
            std::copy_n(sourceRow.get() + xIndices[x] * components, components, destinationTuple);
          }
        }
      }
      else
      {
        std::fill_n(destinationRow.get(), destinationRowValues, static_cast<T>(fillValue));
      }
      const usize destinationOffset = ((z * referenceDims[1] * referenceDims[0]) + (y * referenceDims[0])) * components;
      auto result = destinationStore.copyFromBuffer(destinationOffset, nonstd::span<const T>(destinationRow.get(), destinationRowValues));
      if(result.invalid())
      {
        return result;
      }
    }
  }
  return {};
}

/** @brief Dispatches a runtime DataType to the matching typed row-copy specialization. */
struct CopyArrayFunctor
{
  /** @brief Executes the typed row-buffered copy for one source/destination pair. */
  template <typename T>
  Result<> operator()(const IDataArray& source, IDataArray& destination, const ImageGeom& sampleGeom, const ImageGeom& referenceGeom, float64 fillValue, const std::atomic_bool& shouldCancel) const
  {
    return CopyArray<T>(source, destination, sampleGeom, referenceGeom, fillValue, shouldCancel);
  }
};

} // namespace

NearestPointFuseRegularGridsScanline::NearestPointFuseRegularGridsScanline(DataStructure& dataStructure, const IFilter::MessageHandler& messageHandler, const std::atomic_bool& shouldCancel,
                                                                           const NearestPointFuseRegularGridsInputValues* inputValues)
: m_DataStructure(dataStructure)
, m_MessageHandler(messageHandler)
, m_ShouldCancel(shouldCancel)
, m_InputValues(inputValues)
{
}

Result<> NearestPointFuseRegularGridsScanline::operator()()
{
  const auto& sampleGeom = m_DataStructure.getDataRefAs<ImageGeom>(m_InputValues->SamplingGeometryPath);
  const auto& referenceGeom = m_DataStructure.getDataRefAs<ImageGeom>(m_InputValues->ReferenceGeometryPath);
  const auto& sampleAM = m_DataStructure.getDataRefAs<AttributeMatrix>(m_InputValues->SamplingCellAttributeMatrixPath);
  for(const auto& source : sampleAM.findAllChildrenOfType<IArray>())
  {
    if(m_ShouldCancel)
    {
      return {};
    }
    if(source->getArrayType() != IArray::ArrayType::DataArray)
    {
      continue;
    }
    auto& destination = m_DataStructure.getDataRefAs<IArray>(m_InputValues->ReferenceCellAttributeMatrixPath.createChildPath(source->getName()));
    const auto& sourceData = dynamic_cast<const IDataArray&>(*source);
    auto& destinationData = dynamic_cast<IDataArray&>(destination);
    auto result = ExecuteDataFunction(CopyArrayFunctor{}, sourceData.getDataType(), sourceData, destinationData, sampleGeom, referenceGeom, m_InputValues->fillValue, m_ShouldCancel);
    if(result.invalid())
    {
      return result;
    }
  }
  return {};
}
