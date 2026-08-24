#include "WriteVtkStructuredPoints.hpp"

#include "simplnx/Utilities/StringUtilities.hpp"

#include "SimplnxCore/utils/VtkUtilities.hpp"

#include "simplnx/Common/Bit.hpp"
#include "simplnx/DataStructure/DataArray.hpp"
#include "simplnx/DataStructure/DataStore.hpp"
#include "simplnx/DataStructure/Geometry/ImageGeom.hpp"
#include "simplnx/Utilities/AlgorithmDispatch.hpp"
#include "simplnx/Utilities/FilterUtilities.hpp"

#include <nonstd/span.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

using namespace nx::core;
namespace fs = std::filesystem;

namespace
{
constexpr usize k_MaxChunkBytes = usize{16} * usize{1024} * usize{1024};
constexpr usize k_AsciiChunkValues = 65536;
constexpr int32 k_ReadChunkError = -2090;
constexpr int32 k_WriteError = -2091;

template <typename T>
constexpr usize BinaryChunkValueCapacity()
{
  return std::max<usize>(1, k_MaxChunkBytes / sizeof(T));
}

Result<> CheckOutputStream(const std::ofstream& outputStream, const fs::path& outputPath, const DataPath& arrayPath)
{
  if(outputStream.good())
  {
    return {};
  }

  return MakeErrorResult(
      k_WriteError, fmt::format("Failed to write data array '{}' to VTK file '{}'. Check that the destination has sufficient free space and is writable.", arrayPath.toString(), outputPath.string()));
}

template <typename T>
Result<> WriteArrayPreamble(std::ofstream& outputStream, const DataArray<T>& dataArray, const fs::path& outputPath, const DataPath& arrayPath)
{
  const std::string name = StringUtilities::replace(dataArray.getName(), " ", "_");
  outputStream << "SCALARS " << name << " " << ConvertDataTypeToVtkDataType<T>() << " " << dataArray.getNumberOfComponents() << "\n";
  outputStream << "LOOKUP_TABLE default\n";
  return CheckOutputStream(outputStream, outputPath, arrayPath);
}

template <typename T>
Result<> WriteBinaryDirect(std::ofstream& outputStream, const DataStore<T>& dataStore, const fs::path& outputPath, const DataPath& arrayPath, const std::atomic_bool& shouldCancel)
{
  const usize totalValues = dataStore.getSize();
  const usize chunkCapacity = BinaryChunkValueCapacity<T>();
  const T* source = dataStore.data();
  std::unique_ptr<T[]> chunkBuffer;
  if constexpr(endian::native == endian::little && sizeof(T) > 1)
  {
    chunkBuffer = std::make_unique<T[]>(chunkCapacity);
  }

  for(usize offset = 0; offset < totalValues; offset += chunkCapacity)
  {
    if(shouldCancel)
    {
      return {};
    }

    const usize count = std::min(chunkCapacity, totalValues - offset);
    const T* writeBuffer = source + offset;
    if constexpr(endian::native == endian::little && sizeof(T) > 1)
    {
      std::transform(writeBuffer, writeBuffer + count, chunkBuffer.get(), [](T value) { return nx::core::byteswap(value); });
      writeBuffer = chunkBuffer.get();
    }

    outputStream.write(reinterpret_cast<const char*>(writeBuffer), static_cast<std::streamsize>(count * sizeof(T)));
    auto writeResult = CheckOutputStream(outputStream, outputPath, arrayPath);
    if(writeResult.invalid())
    {
      return writeResult;
    }
  }

  outputStream << '\n';
  return CheckOutputStream(outputStream, outputPath, arrayPath);
}

template <typename T>
Result<> ReadChunk(const AbstractDataStore<T>& dataStore, usize offset, nonstd::span<T> chunk, const DataPath& arrayPath)
{
  auto readResult = dataStore.copyIntoBuffer(offset, chunk);
  if(readResult.valid())
  {
    return {};
  }

  const std::string reason = readResult.errors().empty() ? "unknown error" : readResult.errors()[0].message;
  return MakeErrorResult(k_ReadChunkError,
                         fmt::format("Failed to read values [{}, {}) from data array '{}' while writing the VTK file: {}", offset, offset + chunk.size(), arrayPath.toString(), reason));
}

template <typename T>
Result<> WriteBinaryBulk(std::ofstream& outputStream, const AbstractDataStore<T>& dataStore, const fs::path& outputPath, const DataPath& arrayPath, const std::atomic_bool& shouldCancel)
{
  const usize totalValues = dataStore.getSize();
  const usize chunkCapacity = BinaryChunkValueCapacity<T>();
  auto chunkBuffer = std::make_unique<T[]>(chunkCapacity);

  for(usize offset = 0; offset < totalValues; offset += chunkCapacity)
  {
    if(shouldCancel)
    {
      return {};
    }

    const usize count = std::min(chunkCapacity, totalValues - offset);
    auto readResult = ReadChunk(dataStore, offset, nonstd::span<T>(chunkBuffer.get(), count), arrayPath);
    if(readResult.invalid())
    {
      return readResult;
    }

    if constexpr(endian::native == endian::little && sizeof(T) > 1)
    {
      std::transform(chunkBuffer.get(), chunkBuffer.get() + count, chunkBuffer.get(), [](T value) { return nx::core::byteswap(value); });
    }

    outputStream.write(reinterpret_cast<const char*>(chunkBuffer.get()), static_cast<std::streamsize>(count * sizeof(T)));
    auto writeResult = CheckOutputStream(outputStream, outputPath, arrayPath);
    if(writeResult.invalid())
    {
      return writeResult;
    }
  }

  outputStream << '\n';
  return CheckOutputStream(outputStream, outputPath, arrayPath);
}

template <typename T>
void WriteAsciiValues(std::ofstream& outputStream, const T* values, usize count, usize& currentItemCount)
{
  constexpr usize k_ElementsPerLine = 10;
  for(usize index = 0; index < count; index++)
  {
    if constexpr(std::is_same_v<T, int8> || std::is_same_v<T, uint8>)
    {
      outputStream << static_cast<int32>(values[index]);
    }
    else if constexpr(std::is_floating_point_v<T>)
    {
      outputStream << fmt::format("{}", values[index]);
    }
    else
    {
      outputStream << values[index];
    }

    if(currentItemCount < k_ElementsPerLine - 1)
    {
      outputStream << ' ';
      currentItemCount++;
    }
    else
    {
      outputStream << '\n';
      currentItemCount = 0;
    }
  }
}

template <typename T>
Result<> WriteAsciiDirect(std::ofstream& outputStream, const DataStore<T>& dataStore, const fs::path& outputPath, const DataPath& arrayPath, const std::atomic_bool& shouldCancel)
{
  const usize totalValues = dataStore.getSize();
  constexpr usize k_ChunkCapacity = k_AsciiChunkValues;
  const T* source = dataStore.data();
  usize currentItemCount = 0;

  for(usize offset = 0; offset < totalValues; offset += k_ChunkCapacity)
  {
    if(shouldCancel)
    {
      return {};
    }

    const usize count = std::min(k_ChunkCapacity, totalValues - offset);
    WriteAsciiValues(outputStream, source + offset, count, currentItemCount);
    auto writeResult = CheckOutputStream(outputStream, outputPath, arrayPath);
    if(writeResult.invalid())
    {
      return writeResult;
    }
  }

  outputStream << '\n';
  return CheckOutputStream(outputStream, outputPath, arrayPath);
}

template <typename T>
Result<> WriteAsciiBulk(std::ofstream& outputStream, const AbstractDataStore<T>& dataStore, const fs::path& outputPath, const DataPath& arrayPath, const std::atomic_bool& shouldCancel)
{
  const usize totalValues = dataStore.getSize();
  constexpr usize k_ChunkCapacity = k_AsciiChunkValues;
  auto chunkBuffer = std::make_unique<T[]>(k_ChunkCapacity);
  usize currentItemCount = 0;

  for(usize offset = 0; offset < totalValues; offset += k_ChunkCapacity)
  {
    if(shouldCancel)
    {
      return {};
    }

    const usize count = std::min(k_ChunkCapacity, totalValues - offset);
    auto readResult = ReadChunk(dataStore, offset, nonstd::span<T>(chunkBuffer.get(), count), arrayPath);
    if(readResult.invalid())
    {
      return readResult;
    }

    WriteAsciiValues(outputStream, chunkBuffer.get(), count, currentItemCount);
    auto writeResult = CheckOutputStream(outputStream, outputPath, arrayPath);
    if(writeResult.invalid())
    {
      return writeResult;
    }
  }

  outputStream << '\n';
  return CheckOutputStream(outputStream, outputPath, arrayPath);
}

struct WriteVtkDataDirectFunctor
{
  template <typename T>
  Result<> operator()(std::ofstream& outputStream, IDataArray& iDataArray, bool binary, const fs::path& outputPath, const DataPath& arrayPath, const std::atomic_bool& shouldCancel) const
  {
    auto& dataArray = dynamic_cast<DataArray<T>&>(iDataArray);
    auto preambleResult = WriteArrayPreamble(outputStream, dataArray, outputPath, arrayPath);
    if(preambleResult.invalid())
    {
      return preambleResult;
    }

    const auto& dataStore = dataArray.getDataStoreRef();
    const auto* directStore = dynamic_cast<const DataStore<T>*>(&dataStore);
    if(directStore == nullptr)
    {
      return binary ? WriteBinaryBulk(outputStream, dataStore, outputPath, arrayPath, shouldCancel) : WriteAsciiBulk(outputStream, dataStore, outputPath, arrayPath, shouldCancel);
    }

    return binary ? WriteBinaryDirect(outputStream, *directStore, outputPath, arrayPath, shouldCancel) : WriteAsciiDirect(outputStream, *directStore, outputPath, arrayPath, shouldCancel);
  }
};

struct WriteVtkDataScanlineFunctor
{
  template <typename T>
  Result<> operator()(std::ofstream& outputStream, IDataArray& iDataArray, bool binary, const fs::path& outputPath, const DataPath& arrayPath, const std::atomic_bool& shouldCancel) const
  {
    auto& dataArray = dynamic_cast<DataArray<T>&>(iDataArray);
    auto preambleResult = WriteArrayPreamble(outputStream, dataArray, outputPath, arrayPath);
    if(preambleResult.invalid())
    {
      return preambleResult;
    }

    const auto& dataStore = dataArray.getDataStoreRef();
    return binary ? WriteBinaryBulk(outputStream, dataStore, outputPath, arrayPath, shouldCancel) : WriteAsciiBulk(outputStream, dataStore, outputPath, arrayPath, shouldCancel);
  }
};

class WriteVtkDataDirect
{
public:
  WriteVtkDataDirect(std::ofstream& outputStream, IDataArray& dataArray, bool binary, const fs::path& outputPath, const DataPath& arrayPath, const std::atomic_bool& shouldCancel)
  : m_OutputStream(outputStream)
  , m_DataArray(dataArray)
  , m_Binary(binary)
  , m_OutputPath(outputPath)
  , m_ArrayPath(arrayPath)
  , m_ShouldCancel(shouldCancel)
  {
  }

  Result<> operator()()
  {
    return ExecuteDataFunctionNoBool(WriteVtkDataDirectFunctor{}, m_DataArray.getDataType(), m_OutputStream, m_DataArray, m_Binary, m_OutputPath, m_ArrayPath, m_ShouldCancel);
  }

private:
  std::ofstream& m_OutputStream;
  IDataArray& m_DataArray;
  bool m_Binary = false;
  const fs::path& m_OutputPath;
  const DataPath& m_ArrayPath;
  const std::atomic_bool& m_ShouldCancel;
};

// Dispatch keeps in-memory writes on raw contiguous storage while forced and real
// OOC execution uses sequential bulk reads with the same formatting logic.
class WriteVtkDataScanline
{
public:
  WriteVtkDataScanline(std::ofstream& outputStream, IDataArray& dataArray, bool binary, const fs::path& outputPath, const DataPath& arrayPath, const std::atomic_bool& shouldCancel)
  : m_OutputStream(outputStream)
  , m_DataArray(dataArray)
  , m_Binary(binary)
  , m_OutputPath(outputPath)
  , m_ArrayPath(arrayPath)
  , m_ShouldCancel(shouldCancel)
  {
  }

  Result<> operator()()
  {
    return ExecuteDataFunctionNoBool(WriteVtkDataScanlineFunctor{}, m_DataArray.getDataType(), m_OutputStream, m_DataArray, m_Binary, m_OutputPath, m_ArrayPath, m_ShouldCancel);
  }

private:
  std::ofstream& m_OutputStream;
  IDataArray& m_DataArray;
  bool m_Binary = false;
  const fs::path& m_OutputPath;
  const DataPath& m_ArrayPath;
  const std::atomic_bool& m_ShouldCancel;
};
} // namespace

// -----------------------------------------------------------------------------
WriteVtkStructuredPoints::WriteVtkStructuredPoints(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel,
                                                   WriteVtkStructuredPointsInputValues* inputValues)
: m_DataStructure(dataStructure)
, m_InputValues(inputValues)
, m_ShouldCancel(shouldCancel)
, m_MessageHandler(mesgHandler)
{
}

// -----------------------------------------------------------------------------
WriteVtkStructuredPoints::~WriteVtkStructuredPoints() noexcept = default;

// -----------------------------------------------------------------------------
const std::atomic_bool& WriteVtkStructuredPoints::getCancel()
{
  return m_ShouldCancel;
}

// -----------------------------------------------------------------------------
Result<> WriteVtkStructuredPoints::operator()()
{
  const auto& imageGeom = m_DataStructure.getDataRefAs<ImageGeom>(m_InputValues->ImageGeometryPath);
  const SizeVec3 dims = imageGeom.getDimensions();
  const FloatVec3 spacing = imageGeom.getSpacing();
  const FloatVec3 origin = imageGeom.getOrigin();

  const fs::path vtkOutPath = m_InputValues->OutputFile;
  std::ofstream outStrm(vtkOutPath.string(), std::ios_base::out | std::ios_base::binary | std::ios_base::trunc);
  if(!outStrm.is_open())
  {
    return MakeErrorResult(-66667, fmt::format("Output file could not be opened for writing: '{}'. Check that the parent directory exists and is writable.", m_InputValues->OutputFile.string()));
  }
  outStrm << "# vtk DataFile Version 3.0\n";
  outStrm << "vtk output\n";
  outStrm << (m_InputValues->WriteBinaryFile ? "BINARY\n" : "ASCII\n");
  outStrm << "DATASET STRUCTURED_POINTS\n";
  outStrm << fmt::format("DIMENSIONS {} {} {}\n", dims[0] + 1, dims[1] + 1, dims[2] + 1);
  outStrm << fmt::format("SPACING {} {} {}\n", spacing[0], spacing[1], spacing[2]);
  outStrm << fmt::format("ORIGIN {} {} {}\n", origin[0], origin[1], origin[2]);
  outStrm << fmt::format("CELL_DATA {}\n", dims[0] * dims[1] * dims[2]);
  if(!outStrm.good())
  {
    return MakeErrorResult(k_WriteError, fmt::format("Failed to write the VTK header to file '{}'. Check that the destination has sufficient free space and is writable.", vtkOutPath.string()));
  }

  Result<> result;
  for(const auto& arrayPath : m_InputValues->SelectedDataArrayPaths)
  {
    m_MessageHandler({IFilter::Message::Type::Info, fmt::format("Writing {}", arrayPath.toString())});
    auto& dataArray = m_DataStructure.getDataRefAs<IDataArray>(arrayPath);
    auto writeResult = DispatchAlgorithm<WriteVtkDataDirect, WriteVtkDataScanline>({&dataArray}, outStrm, dataArray, m_InputValues->WriteBinaryFile, vtkOutPath, arrayPath, m_ShouldCancel);
    result = MergeResults(result, std::move(writeResult));
  }

  return result;
}
