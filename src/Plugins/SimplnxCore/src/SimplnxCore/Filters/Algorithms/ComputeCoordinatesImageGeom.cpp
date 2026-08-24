#include "ComputeCoordinatesImageGeom.hpp"

#include "simplnx/DataStructure/DataArray.hpp"
#include "simplnx/DataStructure/DataStore.hpp"
#include "simplnx/DataStructure/Geometry/ImageGeom.hpp"
#include "simplnx/Utilities/AlgorithmDispatch.hpp"
#include "simplnx/Utilities/ParallelDataAlgorithm.hpp"

#include <nonstd/span.hpp>

#include <algorithm>
#include <memory>

using namespace nx::core;

namespace
{
constexpr usize k_NumComponents = 3;
constexpr usize k_ChunkTuples = 65536;

template <bool WriteCoordinates, bool WriteIndices>
class GenerateCoordinatesDirectImpl
{
public:
  GenerateCoordinatesDirectImpl(const ImageGeom& imageGeom, float32* coordinates, int32* indices, const std::atomic_bool& shouldCancel)
  : m_XCells(imageGeom.getNumXCells())
  , m_YCells(imageGeom.getNumYCells())
  , m_Spacing(imageGeom.getSpacing())
  , m_Origin(imageGeom.getOrigin())
  , m_Coordinates(coordinates)
  , m_Indices(indices)
  , m_ShouldCancel(shouldCancel)
  {
  }

  void operator()(const Range& range) const
  {
    const usize sliceTuples = m_XCells * m_YCells;
    for(usize zIndex = range.min(); zIndex < range.max(); zIndex++)
    {
      if(m_ShouldCancel)
      {
        return;
      }

      const usize sliceOffset = zIndex * sliceTuples;
      const float32 zCoordinate = zIndex * m_Spacing[2] + m_Origin[2] + (0.5F * m_Spacing[2]);
      for(usize yIndex = 0; yIndex < m_YCells; yIndex++)
      {
        const usize rowOffset = sliceOffset + (yIndex * m_XCells);
        const float32 yCoordinate = yIndex * m_Spacing[1] + m_Origin[1] + (0.5F * m_Spacing[1]);
        float32* coordinates = nullptr;
        int32* indices = nullptr;
        if constexpr(WriteCoordinates)
        {
          coordinates = m_Coordinates + (rowOffset * k_NumComponents);
        }
        if constexpr(WriteIndices)
        {
          indices = m_Indices + (rowOffset * k_NumComponents);
        }

        for(usize xIndex = 0; xIndex < m_XCells; xIndex++)
        {
          if constexpr(WriteCoordinates)
          {
            coordinates[0] = xIndex * m_Spacing[0] + m_Origin[0] + (0.5F * m_Spacing[0]);
            coordinates[1] = yCoordinate;
            coordinates[2] = zCoordinate;
            coordinates += k_NumComponents;
          }
          if constexpr(WriteIndices)
          {
            indices[0] = static_cast<int32>(xIndex);
            indices[1] = static_cast<int32>(yIndex);
            indices[2] = static_cast<int32>(zIndex);
            indices += k_NumComponents;
          }
        }
      }
    }
  }

private:
  usize m_XCells = 0;
  usize m_YCells = 0;
  FloatVec3 m_Spacing = {};
  FloatVec3 m_Origin = {};
  float32* m_Coordinates = nullptr;
  int32* m_Indices = nullptr;
  const std::atomic_bool& m_ShouldCancel;
};

class ComputeCoordinatesImageGeomScanline
{
public:
  ComputeCoordinatesImageGeomScanline(const ImageGeom& imageGeom, Float32Array* coordinates, Int32Array* indices, const std::atomic_bool& shouldCancel)
  : m_ImageGeom(imageGeom)
  , m_Coordinates(coordinates)
  , m_Indices(indices)
  , m_ShouldCancel(shouldCancel)
  {
  }

  Result<> operator()() const
  {
    if(m_Coordinates != nullptr && m_Indices != nullptr)
    {
      return generate<true, true>();
    }
    if(m_Coordinates != nullptr)
    {
      return generate<true, false>();
    }
    if(m_Indices != nullptr)
    {
      return generate<false, true>();
    }
    return {};
  }

private:
  template <bool WriteCoordinates, bool WriteIndices>
  Result<> generate() const
  {
    const usize xCells = m_ImageGeom.getNumXCells();
    const usize yCells = m_ImageGeom.getNumYCells();
    const usize sliceTuples = xCells * yCells;
    const usize totalTuples = m_ImageGeom.getNumberOfCells();
    const FloatVec3 spacing = m_ImageGeom.getSpacing();
    const FloatVec3 origin = m_ImageGeom.getOrigin();

    std::unique_ptr<float32[]> coordinatesBuffer;
    std::unique_ptr<int32[]> indicesBuffer;
    if constexpr(WriteCoordinates)
    {
      coordinatesBuffer = std::make_unique<float32[]>(k_ChunkTuples * k_NumComponents);
    }
    if constexpr(WriteIndices)
    {
      indicesBuffer = std::make_unique<int32[]>(k_ChunkTuples * k_NumComponents);
    }

    for(usize tupleOffset = 0; tupleOffset < totalTuples; tupleOffset += k_ChunkTuples)
    {
      if(m_ShouldCancel)
      {
        return {};
      }

      const usize tupleCount = std::min(k_ChunkTuples, totalTuples - tupleOffset);
      const usize valueCount = tupleCount * k_NumComponents;
      const usize valueOffset = tupleOffset * k_NumComponents;
      usize zIndex = tupleOffset / sliceTuples;
      const usize sliceIndex = tupleOffset % sliceTuples;
      usize yIndex = sliceIndex / xCells;
      usize xIndex = sliceIndex % xCells;

      for(usize chunkIndex = 0; chunkIndex < tupleCount; chunkIndex++)
      {
        const usize componentOffset = chunkIndex * k_NumComponents;
        if constexpr(WriteCoordinates)
        {
          coordinatesBuffer[componentOffset] = xIndex * spacing[0] + origin[0] + (0.5F * spacing[0]);
          coordinatesBuffer[componentOffset + 1] = yIndex * spacing[1] + origin[1] + (0.5F * spacing[1]);
          coordinatesBuffer[componentOffset + 2] = zIndex * spacing[2] + origin[2] + (0.5F * spacing[2]);
        }
        if constexpr(WriteIndices)
        {
          indicesBuffer[componentOffset] = static_cast<int32>(xIndex);
          indicesBuffer[componentOffset + 1] = static_cast<int32>(yIndex);
          indicesBuffer[componentOffset + 2] = static_cast<int32>(zIndex);
        }

        xIndex++;
        if(xIndex == xCells)
        {
          xIndex = 0;
          yIndex++;
          if(yIndex == yCells)
          {
            yIndex = 0;
            zIndex++;
          }
        }
      }

      if constexpr(WriteCoordinates)
      {
        Result<> writeResult = m_Coordinates->getDataStoreRef().copyFromBuffer(valueOffset, nonstd::span<const float32>(coordinatesBuffer.get(), valueCount));
        if(writeResult.invalid())
        {
          return writeResult;
        }
      }
      if constexpr(WriteIndices)
      {
        Result<> writeResult = m_Indices->getDataStoreRef().copyFromBuffer(valueOffset, nonstd::span<const int32>(indicesBuffer.get(), valueCount));
        if(writeResult.invalid())
        {
          return writeResult;
        }
      }
    }

    return {};
  }

  const ImageGeom& m_ImageGeom;
  Float32Array* m_Coordinates = nullptr;
  Int32Array* m_Indices = nullptr;
  const std::atomic_bool& m_ShouldCancel;
};

class ComputeCoordinatesImageGeomDirect
{
public:
  ComputeCoordinatesImageGeomDirect(const ImageGeom& imageGeom, Float32Array* coordinates, Int32Array* indices, const std::atomic_bool& shouldCancel)
  : m_ImageGeom(imageGeom)
  , m_Coordinates(coordinates)
  , m_Indices(indices)
  , m_ShouldCancel(shouldCancel)
  {
  }

  Result<> operator()() const
  {
    auto* coordinatesStore = m_Coordinates == nullptr ? nullptr : dynamic_cast<Float32DataStore*>(&m_Coordinates->getDataStoreRef());
    auto* indicesStore = m_Indices == nullptr ? nullptr : dynamic_cast<Int32DataStore*>(&m_Indices->getDataStoreRef());
    if((m_Coordinates != nullptr && coordinatesStore == nullptr) || (m_Indices != nullptr && indicesStore == nullptr))
    {
      // A forced direct path can still receive noncontiguous stores; retain bounded behavior in that case.
      return ComputeCoordinatesImageGeomScanline(m_ImageGeom, m_Coordinates, m_Indices, m_ShouldCancel)();
    }

    // Workers own disjoint complete Z ranges, so both contiguous outputs can be written without synchronization.
    ParallelDataAlgorithm parallelAlgorithm;
    parallelAlgorithm.setRange(0, m_ImageGeom.getNumZCells());
    if(coordinatesStore != nullptr && indicesStore != nullptr)
    {
      parallelAlgorithm.execute(GenerateCoordinatesDirectImpl<true, true>(m_ImageGeom, coordinatesStore->data(), indicesStore->data(), m_ShouldCancel));
    }
    else if(coordinatesStore != nullptr)
    {
      parallelAlgorithm.execute(GenerateCoordinatesDirectImpl<true, false>(m_ImageGeom, coordinatesStore->data(), nullptr, m_ShouldCancel));
    }
    else if(indicesStore != nullptr)
    {
      parallelAlgorithm.execute(GenerateCoordinatesDirectImpl<false, true>(m_ImageGeom, nullptr, indicesStore->data(), m_ShouldCancel));
    }
    return {};
  }

private:
  const ImageGeom& m_ImageGeom;
  Float32Array* m_Coordinates = nullptr;
  Int32Array* m_Indices = nullptr;
  const std::atomic_bool& m_ShouldCancel;
};
} // namespace

// -----------------------------------------------------------------------------
ComputeCoordinatesImageGeom::ComputeCoordinatesImageGeom(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel,
                                                         ComputeCoordinatesImageGeomInputValues* inputValues)
: m_DataStructure(dataStructure)
, m_InputValues(inputValues)
, m_ShouldCancel(shouldCancel)
, m_MessageHandler(mesgHandler)
{
}

// -----------------------------------------------------------------------------
ComputeCoordinatesImageGeom::~ComputeCoordinatesImageGeom() noexcept = default;

// -----------------------------------------------------------------------------
Result<> ComputeCoordinatesImageGeom::operator()()
{
  auto& imageGeom = m_DataStructure.getDataRefAs<ImageGeom>(m_InputValues->ImageGeomPath);
  const bool writeCoordinates = m_InputValues->CoordinateOption != to_underlying(OutputType::Index);
  const bool writeIndices = m_InputValues->CoordinateOption != to_underlying(OutputType::Physical);

  Float32Array* coordinates = nullptr;
  Int32Array* indices = nullptr;
  if(writeCoordinates)
  {
    coordinates = &m_DataStructure.getDataRefAs<Float32Array>(m_InputValues->CoordArrayPath);
  }
  if(writeIndices)
  {
    indices = &m_DataStructure.getDataRefAs<Int32Array>(m_InputValues->IndexArrayPath);
  }

  return DispatchAlgorithm<ComputeCoordinatesImageGeomDirect, ComputeCoordinatesImageGeomScanline>({coordinates, indices}, imageGeom, coordinates, indices, m_ShouldCancel);
}
