#include "ReadBinaryCTNorthstar.hpp"

#include "simplnx/Common/ScopeGuard.hpp"
#include "simplnx/DataStructure/DataArray.hpp"
#include "simplnx/DataStructure/Geometry/ImageGeom.hpp"
#include "simplnx/Utilities/MessageHelper.hpp"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <memory>

#include <nonstd/span.hpp>

#if !defined(_MSC_VER)
#include <sys/types.h>
#endif

using namespace nx::core;

namespace
{
bool SeekToOffset(FILE* file, uint64 offset)
{
#if defined(_MSC_VER)
  using FileOffset = int64;
#else
  using FileOffset = off_t;
#endif

  if(offset > static_cast<uint64>(std::numeric_limits<FileOffset>::max()))
  {
    return false;
  }

#if defined(_MSC_VER)
  return ::_fseeki64(file, static_cast<FileOffset>(offset), SEEK_SET) == 0;
#else
  return ::fseeko(file, static_cast<FileOffset>(offset), SEEK_SET) == 0;
#endif
}

// -----------------------------------------------------------------------------
Result<> SanityCheckFileSizeVersusAllocatedSize(size_t allocatedBytes, size_t fileSize)
{
  if(fileSize < allocatedBytes)
  {
    return MakeErrorResult(-4000, fmt::format("File size ({} bytes) is less than allocated size ({} bytes).", fileSize, allocatedBytes));
  }

  // File Size and Allocated Size are equal, so we  are good to go
  return {};
}

// -----------------------------------------------------------------------------
Result<> ReadBinaryCTFiles(DataStructure& dataStructure, const IFilter::MessageHandler& messageHandler, const std::atomic_bool& shouldCancel, const ReadBinaryCTNorthstarInputValues* inputValues)
{
  auto& geom = dataStructure.getDataRefAs<ImageGeom>(inputValues->ImageGeometryPath);
  geom.setUnits(static_cast<IGeometry::LengthUnit>(inputValues->LengthUnit));

  auto& density = dataStructure.getDataAs<Float32Array>(inputValues->DensityArrayPath)->getDataStoreRef();
  const usize deltaX = inputValues->EndVoxelCoord[0] - inputValues->StartVoxelCoord[0] + 1;

  // Preserve the initial sentinel for incomplete imports without invoking per-element OOC store access.
  const usize sliceSize = inputValues->ImportedGeometryDims[0] * inputValues->ImportedGeometryDims[1];
  auto initializationBuffer = std::make_unique<float32[]>(sliceSize);
  std::fill_n(initializationBuffer.get(), sliceSize, static_cast<float32>(0xABCDEF));
  for(usize offset = 0; offset < density.getSize(); offset += sliceSize)
  {
    const usize count = std::min(sliceSize, density.getSize() - offset);
    const Result<> initializeResult = density.copyFromBuffer(offset, nonstd::span<const float32>(initializationBuffer.get(), count));
    if(initializeResult.invalid())
    {
      return initializeResult;
    }
  }

  usize zShift = 0;
  // int32 fileIndex = 1;

  MessageHelper messageHelper(messageHandler);
  auto throttledMessenger = messageHelper.createThrottledMessenger();

  for(const auto& dataFileInput : inputValues->DataFilePaths)
  {
    fs::path dataFilePath = inputValues->InputHeaderFile.parent_path() / dataFileInput.first;
    const usize fileSize = fs::file_size(dataFilePath);
    // allocated bytes should be the x * y dims * number of slices in the current data file....not necessarily the size of the whole density array
    usize allocatedBytes = inputValues->OriginalGeometryDims[0] * inputValues->OriginalGeometryDims[1] * dataFileInput.second * sizeof(float32);

    Result<> result = SanityCheckFileSizeVersusAllocatedSize(allocatedBytes, fileSize);
    if(result.invalid())
    {
      return MakeErrorResult(-38705, fmt::format("The size of file '{}' on the file system ({} bytes) is less than the stated size in the binary CT header. ({} bytes).", dataFilePath.string(),
                                                 fileSize, allocatedBytes));
    }

    FILE* f = fopen(dataFilePath.string().c_str(), "rb");
    if(nullptr == f)
    {
      return MakeErrorResult(-38706, fmt::format("Error opening binary input file: {}.", dataFilePath.string()));
    }

    auto fileGuard = MakeScopeGuard([f]() noexcept { fclose(f); });

    usize fileZSlice = 0;

    // Keep one row in memory so each destination write is a contiguous bulk transfer.
    std::vector<float32> buffer(deltaX);

    for(usize z = zShift; z < (zShift + dataFileInput.second); z++)
    {
      if(shouldCancel)
      {
        return {};
      }

      if(inputValues->ImportSubvolume && (z < inputValues->StartVoxelCoord[2] || z > inputValues->EndVoxelCoord[2]))
      {
        fileZSlice++;
        continue;
      }
      throttledMessenger.sendThrottledMessage([&]() { return fmt::format("Importing Data || Data File: {} || Importing Slice {}", dataFileInput.first.string(), z); });
      for(usize y = 0; y < inputValues->OriginalGeometryDims[1]; y++)
      {
        if(inputValues->ImportSubvolume && (y < inputValues->StartVoxelCoord[1] || y > inputValues->EndVoxelCoord[1]))
        {
          continue;
        }

        const uint64 fpOffset = ((static_cast<uint64>(inputValues->OriginalGeometryDims[1]) * inputValues->OriginalGeometryDims[0] * fileZSlice) +
                                 (static_cast<uint64>(inputValues->OriginalGeometryDims[0]) * y) + static_cast<uint64>(inputValues->StartVoxelCoord[0])) *
                                sizeof(float32);
        if(!SeekToOffset(f, fpOffset))
        {
          return MakeErrorResult(-38707, fmt::format("Could not seek to position {} in file '{}'.", fpOffset, dataFileInput.first.string()));
        }

        usize index = (inputValues->ImportedGeometryDims[0] * inputValues->ImportedGeometryDims[1] * (z - inputValues->StartVoxelCoord[2])) +
                      (inputValues->ImportedGeometryDims[0] * (y - inputValues->StartVoxelCoord[1])) + (0);
        if(fread(buffer.data(), sizeof(float32), deltaX, f) != deltaX)
        {
          return MakeErrorResult(-38708, fmt::format("Error reading file at position {} in file '{}'.", fpOffset, dataFileInput.first.string()));
        }

        const Result<> copyResult = density.copyFromBuffer(index, nonstd::span<const float32>(buffer.data(), deltaX));
        if(copyResult.invalid())
        {
          return copyResult;
        }
      }
      fileZSlice++;
    }
    zShift += dataFileInput.second;
    // fileIndex++;

    if(shouldCancel)
    {
      break;
    }
  }

  return {};
}
} // namespace

// -----------------------------------------------------------------------------
ReadBinaryCTNorthstar::ReadBinaryCTNorthstar(DataStructure& dataStructure, const IFilter::MessageHandler& messageHandler, const std::atomic_bool& shouldCancel,
                                             ReadBinaryCTNorthstarInputValues* inputValues)
: m_DataStructure(dataStructure)
, m_InputValues(inputValues)
, m_ShouldCancel(shouldCancel)
, m_MessageHandler(messageHandler)
{
}

// -----------------------------------------------------------------------------
ReadBinaryCTNorthstar::~ReadBinaryCTNorthstar() noexcept = default;

// -----------------------------------------------------------------------------
const std::atomic_bool& ReadBinaryCTNorthstar::getCancel()
{
  return m_ShouldCancel;
}

// -----------------------------------------------------------------------------
Result<> ReadBinaryCTNorthstar::operator()()
{
  Result<> result = ReadBinaryCTFiles(m_DataStructure, m_MessageHandler, m_ShouldCancel, m_InputValues);
  if(result.invalid())
  {
    return result;
  }

  return {};
}
