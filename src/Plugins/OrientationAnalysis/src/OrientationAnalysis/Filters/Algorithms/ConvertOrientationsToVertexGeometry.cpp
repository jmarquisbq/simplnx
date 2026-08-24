#include "ConvertOrientationsToVertexGeometry.hpp"

#include "simplnx/DataStructure/Geometry/VertexGeom.hpp"
#include "simplnx/DataStructure/IDataArray.hpp"
#include "simplnx/Utilities/FilterUtilities.hpp"

#include <EbsdLib/LaueOps/LaueOps.h>
#include <EbsdLib/Orientation/AxisAngle.hpp>
#include <EbsdLib/Orientation/OrientationFwd.hpp>
#include <EbsdLib/Orientation/Quaternion.hpp>

#include <nonstd/span.hpp>

#include <algorithm>
#include <memory>

using namespace nx::core;

namespace
{
// Bounded chunk size (tuples) used for every bulk read/write in this algorithm. Fixed and
// independent of the total tuple count, so the peak extra memory this algorithm uses is
// O(k_ChunkSize), not O(numTuples).
constexpr usize k_ChunkSize = 4096;

struct CopyDataFunctor
{
  template <typename T>
  void operator()(const IDataArray* srcIArray, IDataArray* destIArray)
  {
    const auto& srcArray = srcIArray->template getIDataStoreRefAs<AbstractDataStore<T>>();
    auto& destArray = destIArray->template getIDataStoreRefAs<AbstractDataStore<T>>();
    destArray.copyFrom(0, srcArray, 0, srcArray.getNumberOfTuples());
  }
};

/**
 * @brief Converts one chunk of orientation tuples (already normalized to float32) into
 * quaternion tuples using EbsdLib's per-representation ::toQuaternion() conversion.
 * Templated on the EbsdLib representation wrapper (e.g. ebsdlib::EulerFType) so the same
 * loop body serves every representation, including Quaternion itself -- Quaternion::toQuaternion()
 * is an identity conversion, so no special case is needed for that representation.
 */
template <class InputFType>
void ConvertChunkToQuaternion(const float32* inBuffer, usize chunkTuples, usize inNumComps, float32* outQuatBuffer)
{
  for(usize t = 0; t < chunkTuples; ++t)
  {
    InputFType inputInstance;
    const usize inOff = t * inNumComps;
    for(usize c = 0; c < inNumComps; ++c)
    {
      inputInstance[c] = inBuffer[inOff + c];
    }
    const ebsdlib::QuaternionFType quat = inputInstance.toQuaternion();
    const usize outOff = t * 4;
    for(usize c = 0; c < 4; ++c)
    {
      outQuatBuffer[outOff + c] = quat[c];
    }
  }
}

/**
 * @brief Dispatches to the EbsdLib representation type matching `inputType` and converts a
 * single chunk of orientation tuples to quaternions. Mirrors the per-representation switch
 * that ConvertOrientations::operator() uses, but scoped to one bounded chunk buffer instead
 * of a full DataStructure array, so no full-size intermediate array is ever created.
 */
void ConvertChunkToQuaternionByType(ebsdlib::orientations::Type inputType, const float32* inBuffer, usize chunkTuples, usize inNumComps, float32* outQuatBuffer)
{
  switch(inputType)
  {
  case ebsdlib::orientations::Type::Euler:
    ConvertChunkToQuaternion<ebsdlib::EulerFType>(inBuffer, chunkTuples, inNumComps, outQuatBuffer);
    break;
  case ebsdlib::orientations::Type::OrientationMatrix:
    ConvertChunkToQuaternion<ebsdlib::OrientationMatrixFType>(inBuffer, chunkTuples, inNumComps, outQuatBuffer);
    break;
  case ebsdlib::orientations::Type::Quaternion:
    ConvertChunkToQuaternion<ebsdlib::QuaternionFType>(inBuffer, chunkTuples, inNumComps, outQuatBuffer);
    break;
  case ebsdlib::orientations::Type::AxisAngle:
    ConvertChunkToQuaternion<ebsdlib::AxisAngleFType>(inBuffer, chunkTuples, inNumComps, outQuatBuffer);
    break;
  case ebsdlib::orientations::Type::Rodrigues:
    ConvertChunkToQuaternion<ebsdlib::RodriguesFType>(inBuffer, chunkTuples, inNumComps, outQuatBuffer);
    break;
  case ebsdlib::orientations::Type::Homochoric:
    ConvertChunkToQuaternion<ebsdlib::HomochoricFType>(inBuffer, chunkTuples, inNumComps, outQuatBuffer);
    break;
  case ebsdlib::orientations::Type::Cubochoric:
    ConvertChunkToQuaternion<ebsdlib::CubochoricFType>(inBuffer, chunkTuples, inNumComps, outQuatBuffer);
    break;
  case ebsdlib::orientations::Type::Stereographic:
    ConvertChunkToQuaternion<ebsdlib::StereographicFType>(inBuffer, chunkTuples, inNumComps, outQuatBuffer);
    break;
  case ebsdlib::orientations::Type::Unknown:
    break;
  }
}
} // namespace

// -----------------------------------------------------------------------------
ConvertOrientationsToVertexGeometry::ConvertOrientationsToVertexGeometry(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel,
                                                                         ConvertOrientationsToVertexGeometryInputValues* inputValues)
: m_DataStructure(dataStructure)
, m_InputValues(inputValues)
, m_ShouldCancel(shouldCancel)
, m_MessageHandler(mesgHandler)
{
}

// -----------------------------------------------------------------------------
/**
 * @brief Converts a cell-level orientation array (in any of the 8 EbsdLib representations) into
 * quaternions, optionally rotates each quaternion into its Laue-class fundamental zone, then
 * projects it to stereographic (x, y, z) coordinates that become the vertex positions of the
 * output VertexGeom.
 *
 * OOC strategy: the input orientation array and the Cell Phases array both live on the source
 * Image/RectilinearGrid geometry and can therefore be out-of-core. This algorithm streams both
 * arrays in bounded k_ChunkSize-tuple chunks via copyIntoBuffer()/copyFromBuffer(), so the peak
 * extra memory used is O(k_ChunkSize), independent of the total tuple count. Each chunk is cast
 * to float32 (if needed), converted to quaternions, optionally rotated into the fundamental zone,
 * projected to stereographic coordinates, and written directly into the chunk's vertex positions.
 * The Crystal Structures array is ensemble-level (one entry per phase, a handful of entries) and
 * is cached wholesale into a local buffer up front, so the per-tuple fundamental-zone lookup never
 * touches the DataStore. The output VertexGeom's vertex array is always an in-core store (simplnx
 * only supports OOC stores for Image/RectilinearGrid geometries), so bulk-writing into it via
 * copyFromBuffer is at least as fast as per-element writes while keeping one uniform code path.
 */
Result<> ConvertOrientationsToVertexGeometry::operator()()
{
  if(m_ShouldCancel)
  {
    return {};
  }

  auto* inputArrayF32 = m_DataStructure.getDataAs<Float32Array>(m_InputValues->InputOrientationArrayPath);
  auto* inputArrayF64 = (inputArrayF32 != nullptr) ? nullptr : m_DataStructure.getDataAs<Float64Array>(m_InputValues->InputOrientationArrayPath);

  const usize numTuples = (inputArrayF32 != nullptr) ? inputArrayF32->getNumberOfTuples() : inputArrayF64->getNumberOfTuples();
  const usize inNumComps = (inputArrayF32 != nullptr) ? inputArrayF32->getNumberOfComponents() : inputArrayF64->getNumberOfComponents();

  auto* phasesArray = m_DataStructure.getDataAs<Int32Array>(m_InputValues->CellPhasesArrayPath);
  auto* crystalStructuresArray = m_DataStructure.getDataAs<UInt32Array>(m_InputValues->CrystalStructuresArrayPath);
  auto& outputVertexGeom = m_DataStructure.getDataRefAs<VertexGeom>(m_InputValues->OutputVertexGeometryPath);
  Float32Array& vertices = outputVertexGeom.getVerticesRef();
  auto& verticesStoreRef = vertices.getDataStoreRef();

  // Ensemble-level cache (indexed by phase, typically only a handful of entries): bulk-read once
  // so the per-tuple fundamental-zone lookup below never goes through DataStore virtual dispatch.
  std::vector<uint32> crystalStructuresCache;
  if(m_InputValues->ConvertToFundamentalZone)
  {
    const usize numCrystalStructures = crystalStructuresArray->getNumberOfTuples();
    crystalStructuresCache.resize(numCrystalStructures);
    crystalStructuresArray->getDataStoreRef().copyIntoBuffer(0, nonstd::span<uint32>(crystalStructuresCache.data(), numCrystalStructures));
  }
  const std::vector<ebsdlib::LaueOps::Pointer> ops = ebsdlib::LaueOps::GetAllOrientationOps();

  // Bounded chunk buffers, reused every iteration -- none of these scale with numTuples.
  auto inBuffer = std::make_unique<float32[]>(k_ChunkSize * inNumComps);
  auto quatBuffer = std::make_unique<float32[]>(k_ChunkSize * 4);
  auto outVertBuffer = std::make_unique<float32[]>(k_ChunkSize * 3);
  std::unique_ptr<float64[]> f64Buffer;
  if(inputArrayF64 != nullptr)
  {
    f64Buffer = std::make_unique<float64[]>(k_ChunkSize * inNumComps);
  }
  std::unique_ptr<int32[]> phasesBuffer;
  if(m_InputValues->ConvertToFundamentalZone)
  {
    phasesBuffer = std::make_unique<int32[]>(k_ChunkSize);
  }

  usize tupleIdx = 0;
  while(tupleIdx < numTuples)
  {
    // Poll for cancellation once per chunk (not per tuple) so a large out-of-core conversion
    // can be interrupted without paying a per-tuple branch cost.
    if(m_ShouldCancel)
    {
      return {};
    }

    const usize chunkTuples = std::min(k_ChunkSize, numTuples - tupleIdx);
    const usize inElemCount = chunkTuples * inNumComps;

    if(inputArrayF32 != nullptr)
    {
      inputArrayF32->getDataStoreRef().copyIntoBuffer(tupleIdx * inNumComps, nonstd::span<float32>(inBuffer.get(), inElemCount));
    }
    else
    {
      inputArrayF64->getDataStoreRef().copyIntoBuffer(tupleIdx * inNumComps, nonstd::span<float64>(f64Buffer.get(), inElemCount));
      std::transform(f64Buffer.get(), f64Buffer.get() + inElemCount, inBuffer.get(), [](float64 value) { return static_cast<float32>(value); });
    }

    ConvertChunkToQuaternionByType(m_InputValues->InputOrientationType, inBuffer.get(), chunkTuples, inNumComps, quatBuffer.get());

    if(m_InputValues->ConvertToFundamentalZone)
    {
      phasesArray->getDataStoreRef().copyIntoBuffer(tupleIdx, nonstd::span<int32>(phasesBuffer.get(), chunkTuples));
    }

    for(usize t = 0; t < chunkTuples; ++t)
    {
      const usize quatOff = t * 4;
      ebsdlib::QuatD quat(quatBuffer[quatOff + 0], quatBuffer[quatOff + 1], quatBuffer[quatOff + 2], quatBuffer[quatOff + 3]);
      if(m_InputValues->ConvertToFundamentalZone)
      {
        const int32 currentPhaseId = phasesBuffer[t];
        const uint32 laueClass = crystalStructuresCache[currentPhaseId];
        quat = (laueClass < ops.size()) ? ops[laueClass]->getFZQuat(quat) : ebsdlib::QuatD(0, 0, 0, 1);
      }

      const ebsdlib::StereographicDType st = ebsdlib::QuaternionDType(quat.getPositiveOrientation()).toStereographic();
      const usize outOff = t * 3;
      outVertBuffer[outOff + 0] = static_cast<float32>(st[0]);
      outVertBuffer[outOff + 1] = static_cast<float32>(st[1]);
      outVertBuffer[outOff + 2] = static_cast<float32>(st[2]);
    }

    verticesStoreRef.copyFromBuffer(tupleIdx * 3, nonstd::span<const float32>(outVertBuffer.get(), chunkTuples * 3));

    tupleIdx += chunkTuples;
  }

  // Copy over the DataArrays to the new Vertex Geometry. This already uses a bulk copyFrom()
  // (see CopyDataFunctor above), so it is not part of the chunked streaming above.
  ShapeType verticesTupleShape = vertices.getTupleShape();
  DataPath vertexAttrMatrixPath = m_InputValues->OutputVertexGeometryPath.createChildPath(m_InputValues->OutputVertexAttrMatrixName);
  for(const auto& sourceDataPath : m_InputValues->DataPathCopySources)
  {
    auto* sourceDataArrayPtr = m_DataStructure.getDataAs<IDataArray>(sourceDataPath);
    DataPath destinationDataPath = vertexAttrMatrixPath.createChildPath(sourceDataArrayPtr->getName());
    auto* destinationDataArrayPtr = m_DataStructure.getDataAs<IDataArray>(destinationDataPath);
    ExecuteDataFunction(CopyDataFunctor{}, sourceDataArrayPtr->getDataType(), sourceDataArrayPtr, destinationDataArrayPtr);
    // This does not resize anything (at least it had better not), but is
    // a round-about way to set the Tuple Shape on the destination array
    destinationDataArrayPtr->resizeTuples(verticesTupleShape);
  }

  return {};
}
