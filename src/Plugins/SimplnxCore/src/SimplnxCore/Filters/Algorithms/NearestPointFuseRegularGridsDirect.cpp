#include "NearestPointFuseRegularGridsDirect.hpp"

#include "simplnx/DataStructure/DataArray.hpp"
#include "simplnx/DataStructure/Geometry/ImageGeom.hpp"
#include "simplnx/Utilities/FilterUtilities.hpp"
#include "simplnx/Utilities/ParallelAlgorithmUtilities.hpp"
#include "simplnx/Utilities/ParallelTaskAlgorithm.hpp"

#include <algorithm>

using namespace nx::core;

namespace
{
/**
 * @brief Resamples one resident typed array through direct nearest-cell indexing.
 *
 * This path recomputes coordinates per destination cell so independent arrays can
 * run concurrently. That tradeoff is favorable for resident stores and avoided by
 * the Scanline path when source reads may reach disk.
 */
template <class ArrayT, class T>
void CopyData(const ArrayT& inputArray, ArrayT& destArray, const ImageGeom& sampleImageGeom, const ImageGeom& refImageGeom, const std::atomic_bool& shouldCancel, const T& fillValue)
{
  const auto sampleRes = sampleImageGeom.getSpacing();
  const auto refRes = refImageGeom.getSpacing();
  const auto refOrigin = refImageGeom.getOrigin();
  const auto sampleOrigin = sampleImageGeom.getOrigin();
  const auto refDims = refImageGeom.getDimensions();
  const auto sampleDims = sampleImageGeom.getDimensions();
  const usize numComps = destArray.getNumberOfComponents();
  for(usize z = 0; z < refDims[2]; z++)
  {
    if(shouldCancel)
    {
      return;
    }
    for(usize y = 0; y < refDims[1]; y++)
    {
      for(usize x = 0; x < refDims[0]; x++)
      {
        const usize destIndex = ((z * refDims[0] * refDims[1]) + (y * refDims[0]) + x) * numComps;
        const float32 xCoord = x * refRes[0] + refOrigin[0];
        const float32 yCoord = y * refRes[1] + refOrigin[1];
        const float32 zCoord = z * refRes[2] + refOrigin[2];
        if(xCoord - sampleOrigin[0] < 0 || yCoord - sampleOrigin[1] < 0 || zCoord - sampleOrigin[2] < 0)
        {
          std::fill(destArray.begin() + destIndex, destArray.begin() + destIndex + numComps, fillValue);
          continue;
        }
        const usize sampleX = static_cast<usize>((xCoord - sampleOrigin[0]) / sampleRes[0]);
        const usize sampleY = static_cast<usize>((yCoord - sampleOrigin[1]) / sampleRes[1]);
        const usize sampleZ = static_cast<usize>((zCoord - sampleOrigin[2]) / sampleRes[2]);
        if(sampleX >= sampleDims[0] || sampleY >= sampleDims[1] || sampleZ >= sampleDims[2])
        {
          std::fill(destArray.begin() + destIndex, destArray.begin() + destIndex + numComps, fillValue);
          continue;
        }
        const usize sourceIndex = ((sampleZ * sampleDims[0] * sampleDims[1]) + (sampleY * sampleDims[0]) + sampleX) * inputArray.getNumberOfComponents();
        for(usize component = 0; component < numComps; component++)
        {
          destArray[destIndex + component] = inputArray.at(sourceIndex + component);
        }
      }
    }
  }
}

/** @brief Adapts runtime array-type dispatch to a task that resamples one resident array. */
template <typename T>
class CopyArrayImpl
{
public:
  /** @brief Captures borrowed source, destination, geometry, fill, and cancellation state for the task. */
  CopyArrayImpl(const IArray& source, IArray& destination, const ImageGeom& sampleGeom, const ImageGeom& referenceGeom, float64 fillValue, const std::atomic_bool& shouldCancel)
  : m_Source(source)
  , m_Destination(destination)
  , m_SampleGeom(sampleGeom)
  , m_ReferenceGeom(referenceGeom)
  , m_FillValue(fillValue)
  , m_ShouldCancel(shouldCancel)
  {
  }

  /** @brief Casts the arrays to the selected value type and executes direct resampling. */
  void operator()() const
  {
    using ArrayT = DataArray<T>;
    CopyData(dynamic_cast<const ArrayT&>(m_Source), dynamic_cast<ArrayT&>(m_Destination), m_SampleGeom, m_ReferenceGeom, m_ShouldCancel, static_cast<T>(m_FillValue));
  }

private:
  const IArray& m_Source;
  IArray& m_Destination;
  const ImageGeom& m_SampleGeom;
  const ImageGeom& m_ReferenceGeom;
  float64 m_FillValue = 0.0;
  const std::atomic_bool& m_ShouldCancel;
};
} // namespace

NearestPointFuseRegularGridsDirect::NearestPointFuseRegularGridsDirect(DataStructure& dataStructure, const IFilter::MessageHandler& messageHandler, const std::atomic_bool& shouldCancel,
                                                                       const NearestPointFuseRegularGridsInputValues* inputValues)
: m_DataStructure(dataStructure)
, m_MessageHandler(messageHandler)
, m_ShouldCancel(shouldCancel)
, m_InputValues(inputValues)
{
}

Result<> NearestPointFuseRegularGridsDirect::operator()()
{
  const auto& sampleGeom = m_DataStructure.getDataRefAs<ImageGeom>(m_InputValues->SamplingGeometryPath);
  const auto& referenceGeom = m_DataStructure.getDataRefAs<ImageGeom>(m_InputValues->ReferenceGeometryPath);
  const auto& sampleAM = m_DataStructure.getDataRefAs<AttributeMatrix>(m_InputValues->SamplingCellAttributeMatrixPath);
  // Each resident array is independent, so array-level tasks expose parallelism without synchronizing stores.
  ParallelTaskAlgorithm taskRunner;
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
    auto& sourceData = dynamic_cast<const IDataArray&>(*source);
    ExecuteParallelFunction<CopyArrayImpl>(sourceData.getDataType(), taskRunner, *source, destination, sampleGeom, referenceGeom, m_InputValues->fillValue, m_ShouldCancel);
  }
  taskRunner.wait();
  return {};
}
