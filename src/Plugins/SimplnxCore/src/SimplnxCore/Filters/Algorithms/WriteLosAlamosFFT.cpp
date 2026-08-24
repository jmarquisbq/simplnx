#include "WriteLosAlamosFFT.hpp"

#include "simplnx/Common/Constants.hpp"
#include "simplnx/DataStructure/DataArray.hpp"
#include "simplnx/DataStructure/DataStore.hpp"
#include "simplnx/DataStructure/Geometry/ImageGeom.hpp"
#include "simplnx/Utilities/AlgorithmDispatch.hpp"
#include "simplnx/Utilities/FilterUtilities.hpp"

#include <nonstd/span.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

namespace fs = std::filesystem;
using namespace nx::core;

namespace
{
using ull = unsigned long long int;

/// Limits DataStore transfers and formatted text to a fixed-size working set.
constexpr usize k_ChunkTuples = 65536;
constexpr usize k_EulerComponents = 3;
constexpr usize k_MaxRecordCharacters = 256;

class ChunkedInput
{
public:
  ChunkedInput(const AbstractDataStore<float32>& eulerAngles, const AbstractDataStore<int32>& featureIds, const AbstractDataStore<int32>& cellPhases)
  : m_EulerAngles(eulerAngles)
  , m_FeatureIds(featureIds)
  , m_CellPhases(cellPhases)
  , m_EulerAnglesBuffer(std::make_unique<float32[]>(k_ChunkTuples * k_EulerComponents))
  , m_FeatureIdsBuffer(std::make_unique<int32[]>(k_ChunkTuples))
  , m_CellPhasesBuffer(std::make_unique<int32[]>(k_ChunkTuples))
  {
  }

  Result<> loadChunk(usize tupleOffset, usize tupleCount)
  {
    Result<> result = m_EulerAngles.copyIntoBuffer(tupleOffset * k_EulerComponents, nonstd::span<float32>(m_EulerAnglesBuffer.get(), tupleCount * k_EulerComponents));
    if(result.invalid())
    {
      return result;
    }

    result = m_FeatureIds.copyIntoBuffer(tupleOffset, nonstd::span<int32>(m_FeatureIdsBuffer.get(), tupleCount));
    if(result.invalid())
    {
      return result;
    }

    return m_CellPhases.copyIntoBuffer(tupleOffset, nonstd::span<int32>(m_CellPhasesBuffer.get(), tupleCount));
  }

  const float32* eulerAngles() const
  {
    return m_EulerAnglesBuffer.get();
  }

  const int32* featureIds() const
  {
    return m_FeatureIdsBuffer.get();
  }

  const int32* cellPhases() const
  {
    return m_CellPhasesBuffer.get();
  }

private:
  const AbstractDataStore<float32>& m_EulerAngles;
  const AbstractDataStore<int32>& m_FeatureIds;
  const AbstractDataStore<int32>& m_CellPhases;
  std::unique_ptr<float32[]> m_EulerAnglesBuffer;
  std::unique_ptr<int32[]> m_FeatureIdsBuffer;
  std::unique_ptr<int32[]> m_CellPhasesBuffer;
};

Result<> WriteChunkedData(std::ofstream& file, const fs::path& outputPath, const SizeVec3& dims, ChunkedInput& input, const std::atomic_bool& shouldCancel)
{
  const usize totalTuples = dims[0] * dims[1] * dims[2];
  std::string textBuffer;
  textBuffer.reserve(k_ChunkTuples * k_MaxRecordCharacters);

  for(usize tupleOffset = 0; tupleOffset < totalTuples; tupleOffset += k_ChunkTuples)
  {
    if(shouldCancel)
    {
      return MakeErrorResult(-1, "Filter cancelled");
    }

    const usize tupleCount = std::min(k_ChunkTuples, totalTuples - tupleOffset);
    Result<> readResult = input.loadChunk(tupleOffset, tupleCount);
    if(readResult.invalid())
    {
      return readResult;
    }

    usize x = tupleOffset % dims[0];
    const usize yzIndex = tupleOffset / dims[0];
    usize y = yzIndex % dims[1];
    usize z = yzIndex / dims[1];

    textBuffer.clear();
    for(usize chunkIndex = 0; chunkIndex < tupleCount; ++chunkIndex)
    {
      const usize eulerIndex = chunkIndex * k_EulerComponents;
      const float32 phi1 = input.eulerAngles()[eulerIndex] * 180.0f * Constants::k_1OverPiF;
      const float32 phi = input.eulerAngles()[eulerIndex + 1] * 180.0f * Constants::k_1OverPiF;
      const float32 phi2 = input.eulerAngles()[eulerIndex + 2] * 180.0f * Constants::k_1OverPiF;

      fmt::format_to(std::back_inserter(textBuffer), "{:.3f} {:.3f} {:.3f} {} {} {} {} {}\n", phi1, phi, phi2, static_cast<ull>(x + 1), static_cast<ull>(y + 1), static_cast<ull>(z + 1),
                     input.featureIds()[chunkIndex], input.cellPhases()[chunkIndex]);

      x++;
      if(x == dims[0])
      {
        x = 0;
        y++;
        if(y == dims[1])
        {
          y = 0;
          z++;
        }
      }
    }

    file.write(textBuffer.data(), static_cast<std::streamsize>(textBuffer.size()));
    if(!file)
    {
      return MakeErrorResult(-73451, fmt::format("Error writing output file at path: {}", outputPath.string()));
    }

    if(shouldCancel)
    {
      return MakeErrorResult(-1, "Filter cancelled");
    }
  }

  return {};
}

Result<> WriteDirectData(std::ofstream& file, const fs::path& outputPath, const SizeVec3& dims, const DataStore<float32>& eulerAnglesStore, const DataStore<int32>& featureIdsStore,
                         const DataStore<int32>& cellPhasesStore, const std::atomic_bool& shouldCancel)
{
  const usize totalTuples = dims[0] * dims[1] * dims[2];
  const float32* eulerAngles = eulerAnglesStore.data();
  const int32* featureIds = featureIdsStore.data();
  const int32* cellPhases = cellPhasesStore.data();

  for(usize tupleOffset = 0; tupleOffset < totalTuples; tupleOffset += k_ChunkTuples)
  {
    if(shouldCancel)
    {
      return MakeErrorResult(-1, "Filter cancelled");
    }

    const usize tupleCount = std::min(k_ChunkTuples, totalTuples - tupleOffset);
    const float32* eulerAnglesChunk = eulerAngles + (tupleOffset * k_EulerComponents);
    const int32* featureIdsChunk = featureIds + tupleOffset;
    const int32* cellPhasesChunk = cellPhases + tupleOffset;

    usize x = tupleOffset % dims[0];
    const usize yzIndex = tupleOffset / dims[0];
    usize y = yzIndex % dims[1];
    usize z = yzIndex / dims[1];

    for(usize chunkIndex = 0; chunkIndex < tupleCount; ++chunkIndex)
    {
      const usize eulerIndex = chunkIndex * k_EulerComponents;
      const float32 phi1 = eulerAnglesChunk[eulerIndex] * 180.0f * Constants::k_1OverPiF;
      const float32 phi = eulerAnglesChunk[eulerIndex + 1] * 180.0f * Constants::k_1OverPiF;
      const float32 phi2 = eulerAnglesChunk[eulerIndex + 2] * 180.0f * Constants::k_1OverPiF;

      file << fmt::format("{:.3f} {:.3f} {:.3f} {} {} {} {} {}\n", phi1, phi, phi2, static_cast<ull>(x + 1), static_cast<ull>(y + 1), static_cast<ull>(z + 1), featureIdsChunk[chunkIndex],
                          cellPhasesChunk[chunkIndex]);

      x++;
      if(x == dims[0])
      {
        x = 0;
        y++;
        if(y == dims[1])
        {
          y = 0;
          z++;
        }
      }
    }

    if(!file)
    {
      return MakeErrorResult(-73451, fmt::format("Error writing output file at path: {}", outputPath.string()));
    }

    if(shouldCancel)
    {
      return MakeErrorResult(-1, "Filter cancelled");
    }
  }

  return {};
}

struct WriterContext
{
  std::ofstream& File;
  const fs::path& OutputPath;
  const SizeVec3& Dims;
  const Float32Array& EulerAngles;
  const Int32Array& FeatureIds;
  const Int32Array& CellPhases;
  const std::atomic_bool& ShouldCancel;
};

class WriteLosAlamosFFTDirect
{
public:
  explicit WriteLosAlamosFFTDirect(WriterContext& context)
  : m_Context(context)
  {
  }

  Result<> operator()()
  {
    const auto* eulerAngles = dynamic_cast<const DataStore<float32>*>(&m_Context.EulerAngles.getDataStoreRef());
    const auto* featureIds = dynamic_cast<const DataStore<int32>*>(&m_Context.FeatureIds.getDataStoreRef());
    const auto* cellPhases = dynamic_cast<const DataStore<int32>*>(&m_Context.CellPhases.getDataStoreRef());
    if(eulerAngles == nullptr || featureIds == nullptr || cellPhases == nullptr)
    {
      ChunkedInput input(m_Context.EulerAngles.getDataStoreRef(), m_Context.FeatureIds.getDataStoreRef(), m_Context.CellPhases.getDataStoreRef());
      return WriteChunkedData(m_Context.File, m_Context.OutputPath, m_Context.Dims, input, m_Context.ShouldCancel);
    }

    return WriteDirectData(m_Context.File, m_Context.OutputPath, m_Context.Dims, *eulerAngles, *featureIds, *cellPhases, m_Context.ShouldCancel);
  }

private:
  WriterContext& m_Context;
};

class WriteLosAlamosFFTScanline
{
public:
  explicit WriteLosAlamosFFTScanline(WriterContext& context)
  : m_Context(context)
  {
  }

  Result<> operator()()
  {
    ChunkedInput input(m_Context.EulerAngles.getDataStoreRef(), m_Context.FeatureIds.getDataStoreRef(), m_Context.CellPhases.getDataStoreRef());
    return WriteChunkedData(m_Context.File, m_Context.OutputPath, m_Context.Dims, input, m_Context.ShouldCancel);
  }

private:
  WriterContext& m_Context;
};
} // namespace

// -----------------------------------------------------------------------------
WriteLosAlamosFFT::WriteLosAlamosFFT(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel, WriteLosAlamosFFTInputValues* inputValues)
: m_DataStructure(dataStructure)
, m_InputValues(inputValues)
, m_ShouldCancel(shouldCancel)
, m_MessageHandler(mesgHandler)
{
}

// -----------------------------------------------------------------------------
WriteLosAlamosFFT::~WriteLosAlamosFFT() noexcept = default;

// -----------------------------------------------------------------------------
const std::atomic_bool& WriteLosAlamosFFT::getCancel()
{
  return m_ShouldCancel;
}

// -----------------------------------------------------------------------------
Result<> WriteLosAlamosFFT::operator()()
{
  // Make sure any directory path is also available as the user may have just typed
  // in a path without actually creating the full path
  Result<> createDirectoriesResult = nx::core::CreateOutputDirectories(m_InputValues->OutputFile.parent_path());
  if(createDirectoriesResult.invalid())
  {
    return createDirectoriesResult;
  }

  std::ofstream file = std::ofstream(m_InputValues->OutputFile, std::ios_base::out | std::ios_base::binary);
  if(!file.is_open())
  {
    return MakeErrorResult(-73450, fmt::format("Error creating and opening output file at path: {}", m_InputValues->OutputFile.string()));
  }

  const auto& imageGeom = m_DataStructure.getDataRefAs<ImageGeom>(m_InputValues->ImageGeomPath);
  const SizeVec3 dims = imageGeom.getDimensions();
  const auto& cellEulerAngles = m_DataStructure.getDataRefAs<Float32Array>(m_InputValues->CellEulerAnglesArrayPath);
  const auto& cellPhases = m_DataStructure.getDataRefAs<Int32Array>(m_InputValues->CellPhasesArrayPath);
  const auto& featureIds = m_DataStructure.getDataRefAs<Int32Array>(m_InputValues->FeatureIdsArrayPath);

  WriterContext context{file, m_InputValues->OutputFile, dims, cellEulerAngles, featureIds, cellPhases, m_ShouldCancel};
  Result<> writeResult = DispatchAlgorithm<WriteLosAlamosFFTDirect, WriteLosAlamosFFTScanline>({&cellEulerAngles, &featureIds, &cellPhases}, context);
  if(writeResult.invalid())
  {
    return writeResult;
  }

  file.flush();
  if(!file)
  {
    return MakeErrorResult(-73452, fmt::format("Error flushing output file at path: {}", m_InputValues->OutputFile.string()));
  }
  file.close();
  if(!file)
  {
    return MakeErrorResult(-73453, fmt::format("Error closing output file at path: {}", m_InputValues->OutputFile.string()));
  }

  return {};
}
