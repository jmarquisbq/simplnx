#include "WriteAbaqusHexahedron.hpp"

#include "simplnx/Common/AtomicFile.hpp"
#include "simplnx/DataStructure/DataArray.hpp"
#include "simplnx/DataStructure/DataGroup.hpp"
#include "simplnx/DataStructure/Geometry/ImageGeom.hpp"
#include "simplnx/DataStructure/IO/Generic/IExternalSort.hpp"
#include "simplnx/Utilities/AlgorithmDispatch.hpp"
#include "simplnx/Utilities/DataStoreUtilities.hpp"
#include "simplnx/Utilities/StringUtilities.hpp"

#include <nonstd/span.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <type_traits>

namespace fs = std::filesystem;

using namespace nx::core;

namespace
{
std::string format_duration(std::chrono::milliseconds ms)
{
  using namespace std::chrono;
  auto secs = duration_cast<seconds>(ms);
  ms -= duration_cast<milliseconds>(secs);
  auto mins = duration_cast<minutes>(secs);
  secs -= duration_cast<seconds>(mins);
  auto hour = duration_cast<hours>(mins);
  mins -= duration_cast<minutes>(hour);

  std::stringstream ss;
  ss << hour.count() << " Hours : " << mins.count() << " Minutes : " << secs.count() << " Seconds : " << ms.count() << " Milliseconds";
  return ss.str();
}

std::array<int64, 8> getNodeIds(usize x, usize y, usize z, const usize* pDims)
{
  std::array<int64, 8> nodeId{};

  nodeId[0] = static_cast<int64>(1 + (pDims[0] * pDims[1] * z) + (pDims[0] * y) + x);
  nodeId[1] = static_cast<int64>(1 + (pDims[0] * pDims[1] * z) + (pDims[0] * y) + (x + 1));
  nodeId[2] = static_cast<int64>(1 + (pDims[0] * pDims[1] * z) + (pDims[0] * (y + 1)) + x);
  nodeId[3] = static_cast<int64>(1 + (pDims[0] * pDims[1] * z) + (pDims[0] * (y + 1)) + (x + 1));

  nodeId[4] = static_cast<int64>(1 + (pDims[0] * pDims[1] * (z + 1)) + (pDims[0] * y) + x);
  nodeId[5] = static_cast<int64>(1 + (pDims[0] * pDims[1] * (z + 1)) + (pDims[0] * y) + (x + 1));
  nodeId[6] = static_cast<int64>(1 + (pDims[0] * pDims[1] * (z + 1)) + (pDims[0] * (y + 1)) + x);
  nodeId[7] = static_cast<int64>(1 + (pDims[0] * pDims[1] * (z + 1)) + (pDims[0] * (y + 1)) + (x + 1));

#if 0
  {
    printf("           %lld-------%lld  \n", static_cast<long long int>(nodeId[4]), static_cast<long long int>(nodeId[5]));
    printf("            /|        /|   \n");
    printf("           / |       / |   \n");
    printf("          /  |      /  |   \n");
    printf("       %lld--------%lld  |   \n", static_cast<long long int>(nodeId[6]), static_cast<long long int>(nodeId[7]));
    printf("         |   |      |  |   \n");
    printf("         | %lld------|-%lld  \n", static_cast<long long int>(nodeId[0]), static_cast<long long int>(nodeId[1]));
    printf("         |  /       | /    \n");
    printf("         | /        |/     \n");
    printf("        %lld--------%lld     \n", static_cast<long long int>(nodeId[2]), static_cast<long long int>(nodeId[3]));
#endif
  return nodeId;
}

int32 writeNodes(WriteAbaqusHexahedron* filter, const std::string& fileName, usize* cDims, const float32* origin, const float32* spacing, const std::atomic_bool& shouldCancel, bool writeDummyNode)
{
  usize pDims[3] = {cDims[0] + 1, cDims[1] + 1, cDims[2] + 1};
  usize nodeIndex = 1;
  usize totalPoints = pDims[0] * pDims[1] * pDims[2];
  auto increment = static_cast<usize>(totalPoints * 0.01f);
  if(increment == 0) // check to prevent divide by 0
  {
    increment = 1;
  }

  int32 err = 0;
  FILE* f = fopen(fileName.c_str(), "wb");
  if(nullptr == f)
  {
    return -1;
  }

  auto initialTime = std::chrono::steady_clock::now();
  fprintf(f, "** ----------------------------------------------------------------\n**\n*Node\n");
  for(usize z = 0; z < pDims[2]; z++)
  {
    for(usize y = 0; y < pDims[1]; y++)
    {
      for(usize x = 0; x < pDims[0]; x++)
      {
        float32 xCoord = origin[0] + (x * spacing[0]);
        float32 yCoord = origin[1] + (y * spacing[1]);
        float32 zCoord = origin[2] + (z * spacing[2]);
        fprintf(f, "%llu, %f, %f, %f\n", static_cast<unsigned long long int>(nodeIndex), xCoord, yCoord, zCoord);
        if(nodeIndex % increment == 0)
        {
          auto now = std::chrono::steady_clock::now();
          int64 milliDiff = std::chrono::duration_cast<std::chrono::milliseconds>(now - initialTime).count();
          if(milliDiff > 1000)
          {
            std::string percentage =
                "Writing Nodes (File 1/5) " + StringUtilities::number(static_cast<int32>(static_cast<float32>(nodeIndex) / static_cast<float32>(totalPoints) * 100.0f)) + "% Completed ";
            float32 timeDiff = ((float32)nodeIndex / (float32)(milliDiff));
            int64 estimatedTime = (float32)(totalPoints - nodeIndex) / timeDiff;
            std::string timeRemaining = " || Est. Time Remain: " + format_duration(std::chrono::milliseconds(estimatedTime));
            filter->sendMessage(percentage + timeRemaining);
            initialTime = std::chrono::steady_clock::now();
            if(shouldCancel) // Filter has been cancelled
            {
              fclose(f);
              return 1;
            }
          }
        }
        ++nodeIndex;
      }
    }
  }

  if(writeDummyNode)
  {
    // Write the last node, which is a dummy node used for stress - strain curves.
    fprintf(f, "%llu, %f, %f, %f\n", static_cast<unsigned long long int>(nodeIndex), 0.0f, 0.0f, 0.0f);
  }
  fprintf(f, "**\n** ----------------------------------------------------------------\n**\n");

  // Close the file
  fclose(f);
  return err;
}

int32 writeElems(WriteAbaqusHexahedron* filter, const std::string& fileName, const usize* cDims, usize* pDims, const std::atomic_bool& shouldCancel)
{
  usize totalPoints = cDims[0] * cDims[1] * cDims[2];
  auto increment = static_cast<usize>(totalPoints * 0.01f);
  if(increment == 0) // check to prevent divide by 0
  {
    increment = 1;
  }

  int32 err = 0;
  FILE* f = fopen(fileName.c_str(), "wb");
  if(nullptr == f)
  {
    return -1;
  }

  // Get a typedef to shorten up the printf codes
  using _lli_t_ = long long int;

  auto initialTime = std::chrono::steady_clock::now();
  usize index = 1;
  fprintf(f, "** ----------------------------------------------------------------\n**\n*Element, type=C3D8\n");
  for(usize z = 0; z < cDims[2]; z++)
  {
    for(usize y = 0; y < cDims[1]; y++)
    {
      for(usize x = 0; x < cDims[0]; x++)
      {
        const std::array<int64, 8> nodeId = getNodeIds(x, y, z, pDims);
        fprintf(f, "%llu, %lld, %lld, %lld, %lld, %lld, %lld, %lld, %lld\n", (_lli_t_)index, (_lli_t_)nodeId[5], (_lli_t_)nodeId[1], (_lli_t_)nodeId[0], (_lli_t_)nodeId[4], (_lli_t_)nodeId[7],
                (_lli_t_)nodeId[3], (_lli_t_)nodeId[2], (_lli_t_)nodeId[6]);
        if(index % increment == 0)
        {
          auto now = std::chrono::steady_clock::now();
          int64 milliDiff = std::chrono::duration_cast<std::chrono::milliseconds>(now - initialTime).count();
          if(milliDiff > 1000)
          {
            std::string percentage =
                "Writing Elements (File 2/5) " + StringUtilities::number(static_cast<int32>(static_cast<float32>(index) / static_cast<float32>(totalPoints) * 100.0f)) + "% Completed ";
            float32 timeDiff = ((float32)index / (float32)(milliDiff));
            int64 estimatedTime = (float32)(totalPoints - index) / timeDiff;
            std::string timeRemaining = " || Est. Time Remain: " + format_duration(std::chrono::milliseconds(estimatedTime));
            filter->sendMessage(percentage + timeRemaining);
            initialTime = std::chrono::steady_clock::now();
            if(shouldCancel) // Filter has been cancelled
            {
              fclose(f);
              return 1;
            }
          }
        }
        ++index;
      }
    }
  }

  fprintf(f, "**\n** ----------------------------------------------------------------\n**\n");

  // Close the file
  fclose(f);
  return err;
}

/** @brief Finds the maximum FeatureId in one bounded pass without requiring contiguous storage. */
Result<int32> findMaximumGrainId(const Int32AbstractDataStore& featureIds, const std::atomic_bool& shouldCancel)
{
  const usize totalElements = featureIds.getSize();
  if(totalElements == 0)
  {
    return {int32{0}};
  }

  constexpr usize k_ChunkSize = 65536;
  auto buffer = std::make_unique<int32[]>(std::min(k_ChunkSize, totalElements));
  int32 maximum = std::numeric_limits<int32>::lowest();
  for(usize offset = 0; offset < totalElements; offset += k_ChunkSize)
  {
    if(shouldCancel)
    {
      return {int32{0}};
    }
    const usize count = std::min(k_ChunkSize, totalElements - offset);
    if(Result<> result = featureIds.copyIntoBuffer(offset, nonstd::span<int32>(buffer.get(), count)); result.invalid())
    {
      return ConvertResultTo<int32>(std::move(result), int32{});
    }
    maximum = std::max(maximum, *std::max_element(buffer.get(), buffer.get() + count));
  }
  return {maximum};
}

/**
 * @brief Bucket every FeatureIds element into its owning grain's Abaqus element-id list.
 *
 * The Abaqus ELSET format requires every grain's element ids to be written together as
 * one contiguous list, so *some* grouping by grain is unavoidable. The original
 * implementation obtained that grouping by rescanning the entire FeatureIds array once
 * per grain (`O(grains * cells)`), which on an out-of-core array meant re-issuing chunked
 * HDF5 reads for every single grain -- for a real dataset with thousands of grains this
 * amounted to thousands of full-volume passes.
 *
 * This is the in-core path. It performs the grouping with a single bounded-chunk pass over
 * FeatureIds (`O(cells)` total), pushing each element's flat cell index into its grain's
 * bucket. The OOC path below replaces these cell-sized resident buckets with an external
 * sort and bounded record pages.
 *
 * @param filter Owning filter, used to emit throttled progress messages
 * @param featureIds Cell-level FeatureIds store to read (may be in-memory or OOC-backed)
 * @param maxGrainId Highest grain id present in featureIds (bucket count is maxGrainId + 1)
 * @param shouldCancel Cancellation flag, checked once per chunk
 * @return One bucket per grain id, each holding that grain's flat cell indices in ascending order;
 *         empty if cancelled mid-read
 */
Result<std::vector<std::vector<usize>>> groupElementsByGrain(WriteAbaqusHexahedron* filter, const Int32AbstractDataStore& featureIds, int32 maxGrainId, const std::atomic_bool& shouldCancel)
{
  const usize elsetCount = maxGrainId > 0 ? static_cast<usize>(maxGrainId) + 1 : 0;
  std::vector<std::vector<usize>> elementsByGrain(elsetCount);
  if(elsetCount == 0)
  {
    return {std::move(elementsByGrain)};
  }

  constexpr usize k_ChunkSize = 65536;
  const usize totalElements = featureIds.getSize();
  auto chunkBuffer = std::make_unique<int32[]>(k_ChunkSize);

  auto initialTime = std::chrono::steady_clock::now();
  for(usize offset = 0; offset < totalElements; offset += k_ChunkSize)
  {
    if(shouldCancel) // Filter has been cancelled
    {
      return {std::vector<std::vector<usize>>{}};
    }

    const usize count = std::min(k_ChunkSize, totalElements - offset);
    if(Result<> result = featureIds.copyIntoBuffer(offset, nonstd::span<int32>(chunkBuffer.get(), count)); result.invalid())
    {
      return ConvertResultTo<std::vector<std::vector<usize>>>(std::move(result), {});
    }
    for(usize i = 0; i < count; i++)
    {
      const int32 grainId = chunkBuffer[i];
      if(grainId > 0 && grainId <= maxGrainId)
      {
        elementsByGrain[static_cast<usize>(grainId)].push_back(offset + i);
      }
    }

    const usize elementsProcessed = offset + count;
    auto now = std::chrono::steady_clock::now();
    int64 milliDiff = std::chrono::duration_cast<std::chrono::milliseconds>(now - initialTime).count();
    if(milliDiff > 1000)
    {
      std::string percentage =
          "Writing Element Sets (File 4/5) " + StringUtilities::number(static_cast<int32>(static_cast<float32>(elementsProcessed) / static_cast<float32>(totalElements) * 100.0f)) + "% Grouped ";
      float32 timeDiff = (float32)elementsProcessed / (float32)(milliDiff);
      auto estimatedTime = static_cast<int64>((float32)(totalElements - elementsProcessed) / timeDiff);
      std::string timeRemaining = " || Est. Time Remain: " + format_duration(std::chrono::milliseconds(estimatedTime));
      filter->sendMessage(percentage + timeRemaining);
      initialTime = std::chrono::steady_clock::now();
    }
  }

  return {std::move(elementsByGrain)};
}

/** @brief Appends one Abaqus element ID while enforcing the legacy 16-values-per-line layout. */
void writeElementId(FILE* file, uint64 elementId, usize& elementsOnSet)
{
  if(elementsOnSet != 0)
  {
    fprintf(file, (elementsOnSet % 16) != 0u ? ", " : ",\n");
  }
  fprintf(file, "%llu", static_cast<unsigned long long int>(elementId));
  elementsOnSet++;
}

/**
 * @brief Resident path: groups element IDs in one input pass, then writes each ELSET.
 * This removes the historical O(grains * cells) rescan at the cost of cell-scale RAM.
 */
Result<> writeDirectElsets(WriteAbaqusHexahedron* filter, FILE* file, const Int32AbstractDataStore& featureIds, int32 maxGrainId, const std::atomic_bool& shouldCancel)
{
  auto groupedResult = groupElementsByGrain(filter, featureIds, maxGrainId, shouldCancel);
  if(groupedResult.invalid())
  {
    return ConvertResult(std::move(groupedResult));
  }
  if(shouldCancel)
  {
    return {};
  }
  const auto& elementsByGrain = groupedResult.value();
  for(int32 grainId = 1; grainId <= maxGrainId; ++grainId)
  {
    fprintf(file, "\n*Elset, elset=Grain%d_set\n", grainId);
    usize elementsOnSet = 0;
    for(usize elementId : elementsByGrain[static_cast<usize>(grainId)])
    {
      writeElementId(file, elementId + 1, elementsOnSet);
    }
  }
  return {};
}

/**
 * @brief Exact bounded fallback that rescans FeatureIds once per grain.
 * Used only for forced OOC over resident data when no external sorter exists;
 * genuine OOC execution fails instead of accepting this severe I/O amplification.
 */
Result<> writeRepeatedScanElsets(FILE* file, const Int32AbstractDataStore& featureIds, int32 maxGrainId, const std::atomic_bool& shouldCancel)
{
  constexpr usize k_ChunkSize = 65536;
  const usize totalElements = featureIds.getSize();
  auto featurePage = std::make_unique<int32[]>(std::min(k_ChunkSize, totalElements));
  for(int32 grainId = 1; grainId <= maxGrainId; ++grainId)
  {
    fprintf(file, "\n*Elset, elset=Grain%d_set\n", grainId);
    usize elementsOnSet = 0;
    for(usize offset = 0; offset < totalElements; offset += k_ChunkSize)
    {
      if(shouldCancel)
      {
        return {};
      }
      const usize count = std::min(k_ChunkSize, totalElements - offset);
      if(Result<> result = featureIds.copyIntoBuffer(offset, nonstd::span<int32>(featurePage.get(), count)); result.invalid())
      {
        return result;
      }
      for(usize i = 0; i < count; ++i)
      {
        if(featurePage[i] == grainId)
        {
          writeElementId(file, offset + i + 1, elementsOnSet);
        }
      }
    }
  }
  return {};
}

/**
 * @brief Compact external-sort record keyed by grain then Abaqus element ID.
 * Sorting these records produces contiguous ELSET runs without resident buckets.
 */
struct ElementGrainRecord
{
  int32 grainId = 0;
  uint32 reserved = 0;
  uint64 elementId = 0;
};
static_assert(std::is_trivially_copyable_v<ElementGrainRecord>);

/**
 * @brief OOC path: creates records in bounded FeatureId pages, externally sorts
 * them by grain/element, then streams the ordered runs into ELSET sections.
 */
Result<> writeExternalSortedElsets(FILE* file, const Int32AbstractDataStore& featureIds, int32 maxGrainId, const std::atomic_bool& shouldCancel)
{
  constexpr usize k_ChunkSize = 65536;
  ExternalSortConfig config;
  config.recordSize = sizeof(ElementGrainRecord);
  config.maxRecordsPerBatch = k_ChunkSize;
  config.compare = [](nonstd::span<const std::byte> leftBytes, nonstd::span<const std::byte> rightBytes) {
    ElementGrainRecord left;
    ElementGrainRecord right;
    std::memcpy(&left, leftBytes.data(), sizeof(left));
    std::memcpy(&right, rightBytes.data(), sizeof(right));
    if(left.grainId != right.grainId)
    {
      return left.grainId < right.grainId ? int32{-1} : int32{1};
    }
    if(left.elementId == right.elementId)
    {
      return int32{0};
    }
    return left.elementId < right.elementId ? int32{-1} : int32{1};
  };

  auto sortResult = DataStoreUtilities::GetIOCollection().createExternalSort(config);
  if(sortResult.invalid())
  {
    return ConvertResult(std::move(sortResult));
  }
  std::unique_ptr<IExternalSort> externalSort = std::move(sortResult.value());
  if(externalSort == nullptr)
  {
    return MakeErrorResult(-1119, "The external-sort provider returned a null sorter for Abaqus element sets.");
  }

  const usize totalElements = featureIds.getSize();
  auto featurePage = std::make_unique<int32[]>(std::min(k_ChunkSize, totalElements));
  std::vector<ElementGrainRecord> records;
  records.reserve(std::min(k_ChunkSize, totalElements));
  for(usize offset = 0; offset < totalElements; offset += k_ChunkSize)
  {
    if(shouldCancel)
    {
      return {};
    }
    const usize count = std::min(k_ChunkSize, totalElements - offset);
    if(Result<> result = featureIds.copyIntoBuffer(offset, nonstd::span<int32>(featurePage.get(), count)); result.invalid())
    {
      return result;
    }
    records.clear();
    for(usize i = 0; i < count; ++i)
    {
      if(featurePage[i] > 0 && featurePage[i] <= maxGrainId)
      {
        records.push_back({featurePage[i], 0, offset + i + 1});
      }
    }
    if(records.empty())
    {
      continue;
    }
    auto bytes = nonstd::span<const std::byte>(reinterpret_cast<const std::byte*>(records.data()), records.size() * sizeof(ElementGrainRecord));
    if(Result<> result = externalSort->append(records.size(), bytes, shouldCancel, {}); result.invalid())
    {
      return result;
    }
  }
  if(Result<> result = externalSort->finish(shouldCancel, {}); result.invalid())
  {
    return result;
  }

  int32 currentGrain = 1;
  usize elementsOnSet = 0;
  if(maxGrainId > 0)
  {
    fprintf(file, "\n*Elset, elset=Grain%d_set\n", currentGrain);
  }
  std::vector<ElementGrainRecord> sortedPage(k_ChunkSize);
  for(uint64 offset = 0; offset < externalSort->recordCount(); offset += k_ChunkSize)
  {
    const uint64 count = std::min<uint64>(k_ChunkSize, externalSort->recordCount() - offset);
    auto bytes = nonstd::span<std::byte>(reinterpret_cast<std::byte*>(sortedPage.data()), static_cast<usize>(count) * sizeof(ElementGrainRecord));
    Result<uint64> readResult = externalSort->read(offset, count, bytes, shouldCancel);
    if(readResult.invalid())
    {
      return ConvertResult(std::move(readResult));
    }
    if(readResult.value() != count)
    {
      return MakeErrorResult(-1117, "Abaqus element-set external sort returned a short read.");
    }
    for(uint64 i = 0; i < count; ++i)
    {
      const ElementGrainRecord& record = sortedPage[static_cast<usize>(i)];
      while(currentGrain < record.grainId)
      {
        currentGrain++;
        elementsOnSet = 0;
        fprintf(file, "\n*Elset, elset=Grain%d_set\n", currentGrain);
      }
      writeElementId(file, record.elementId, elementsOnSet);
    }
  }
  while(currentGrain < maxGrainId)
  {
    currentGrain++;
    fprintf(file, "\n*Elset, elset=Grain%d_set\n", currentGrain);
  }
  return {};
}

/**
 * @brief Opens the ELSET file and dispatches resident grouping, external sorting,
 * or the bounded provider-free fallback according to storage residency.
 * @param requireExternalSort True for real disk-backed input, where repeated
 * full-volume scans would be an unacceptable silent regression.
 */
Result<> writeElset(WriteAbaqusHexahedron* filter, const std::string& fileName, size_t totalPoints, const Int32AbstractDataStore& featureIds, int32 maxGrainId, bool useOocAlgorithm,
                    bool requireExternalSort, const std::atomic_bool& shouldCancel)
{
  FILE* file = fopen(fileName.c_str(), "wb");
  if(nullptr == file)
  {
    return MakeErrorResult(-1116, fmt::format("Could not open Abaqus element-set output file '{}'.", fileName));
  }

  fprintf(file, "** ----------------------------------------------------------------\n**\n** The element sets\n");
  fprintf(file, "*Elset, elset=cube, generate\n");
  fprintf(file, "1, %llu, 1\n", static_cast<unsigned long long int>(totalPoints));
  fprintf(file, "**\n** Each Grain is made up of multiple elements\n**");

  Result<> result;
  if(!useOocAlgorithm)
  {
    result = writeDirectElsets(filter, file, featureIds, maxGrainId, shouldCancel);
  }
  else if(DataStoreUtilities::GetIOCollection().hasExternalSortCapability())
  {
    result = writeExternalSortedElsets(file, featureIds, maxGrainId, shouldCancel);
  }
  else if(!requireExternalSort)
  {
    result = writeRepeatedScanElsets(file, featureIds, maxGrainId, shouldCancel);
  }
  else
  {
    result = MakeErrorResult(-1118, "The OOC Abaqus element-set algorithm requires a registered bounded external-sort provider.");
  }

  if(result.valid())
  {
    fprintf(file, "\n**\n** ----------------------------------------------------------------\n**\n");
  }
  fclose(file);
  return result;
}

int32 writeMaster(const std::string& file, const std::string& jobName, const std::string& filePrefix)
{
  int32 err = 0;
  FILE* f = fopen(file.c_str(), "wb");
  if(nullptr == f)
  {
    return -1;
  }

  fprintf(f, "*Heading\n");
  fprintf(f, "%s\n", jobName.c_str());
  fprintf(f, "** Job name : %s\n", jobName.c_str());
  fprintf(f, "*Preprint, echo = NO, model = NO, history = NO, contact = NO\n");
  fprintf(f, "**\n** ----------------------------Geometry----------------------------\n**\n");
  fprintf(f, "*Include, Input = %s\n", (filePrefix + "_nodes.inp").c_str());
  fprintf(f, "*Include, Input = %s\n", (filePrefix + "_elems.inp").c_str());
  fprintf(f, "*Include, Input = %s\n", (filePrefix + "_elset.inp").c_str());
  fprintf(f, "*Include, Input = %s\n", (filePrefix + "_sects.inp").c_str());
  fprintf(f, "**\n** ----------------------------------------------------------------\n**\n");

  // Close the file
  fclose(f);
  return err;
}

int32 writeSects(const std::string& file, int32 maxGrainId, int32 hourglassStiffness)
{
  int32 err = 0;
  FILE* f = fopen(file.c_str(), "wb");
  if(nullptr == f)
  {
    return -1;
  }
  fprintf(f, "** ----------------------------------------------------------------\n**\n** Each section is a separate grain\n");

  // We are now defining the sections, which is for each grain
  int32 grain = 1;
  while(grain <= maxGrainId)
  {
    fprintf(f, "** Section: Grain%d\n", grain);
    fprintf(f, "*Solid Section, elset=Grain%d_set, material=Grain_Mat%d\n", grain, grain);
    fprintf(f, "*Hourglass Stiffness\n%d\n", hourglassStiffness);
    fprintf(f, "** --------------------------------------\n");
    grain++;
  }
  fprintf(f, "**\n** ----------------------------------------------------------------\n**\n");

  // Close the file
  fclose(f);
  return err;
}

void DeleteFiles(const std::vector<Result<AtomicFile>>& fileList)
{
  for(const auto& atomicFile : fileList)
  {
    if(atomicFile.valid())
    {
      atomicFile.value().removeTempFile();
    }
  }
}
} // namespace

// -----------------------------------------------------------------------------
WriteAbaqusHexahedron::WriteAbaqusHexahedron(DataStructure& dataStructure, const IFilter::MessageHandler& mesgHandler, const std::atomic_bool& shouldCancel,
                                             WriteAbaqusHexahedronInputValues* inputValues)
: m_DataStructure(dataStructure)
, m_InputValues(inputValues)
, m_ShouldCancel(shouldCancel)
, m_MessageHandler(mesgHandler)
{
}

// -----------------------------------------------------------------------------
WriteAbaqusHexahedron::~WriteAbaqusHexahedron() noexcept = default;

// -----------------------------------------------------------------------------
const std::atomic_bool& WriteAbaqusHexahedron::getCancel()
{
  return m_ShouldCancel;
}

// -----------------------------------------------------------------------------
void WriteAbaqusHexahedron::sendMessage(const std::string& message)
{
  m_MessageHandler(IFilter::Message::Type::Info, message);
}

// -----------------------------------------------------------------------------
Result<> WriteAbaqusHexahedron::operator()()
{
  auto& imageGeom = m_DataStructure.getDataRefAs<ImageGeom>(m_InputValues->ImageGeometryPath);
  auto& featureIds = m_DataStructure.getDataAs<Int32Array>(m_InputValues->FeatureIdsArrayPath)->getDataStoreRef();
  Vec3<usize> cDims = imageGeom.getDimensions();
  usize pDims[3] = {cDims[0] + 1, cDims[1] + 1, cDims[2] + 1};
  Vec3<float32> origin = imageGeom.getOrigin();
  Vec3<float32> spacing = imageGeom.getSpacing();
  usize totalPoints = imageGeom.getNumberOfCells();

  Result<int32> maxGrainResult = findMaximumGrainId(featureIds, getCancel());
  if(maxGrainResult.invalid())
  {
    return ConvertResult(std::move(maxGrainResult));
  }
  const int32 maxGrainId = maxGrainResult.value();
  if(getCancel())
  {
    return {};
  }
  const bool usesOutOfCoreStore = featureIds.getStoreType() == IDataStore::StoreType::OutOfCore;
  const bool useOocAlgorithm = !ForceInCoreAlgorithm() && (usesOutOfCoreStore || ForceOocAlgorithm());
  RecordAlgorithmPathExecution(useOocAlgorithm ? AlgorithmPath::OutOfCore : AlgorithmPath::InCore, usesOutOfCoreStore);

  // Create file names
  std::vector<Result<AtomicFile>> fileList;
  fileList.push_back(AtomicFile::Create(m_InputValues->OutputPath / fmt::format("{}_nodes.inp", m_InputValues->FilePrefix)));
  fileList.push_back(AtomicFile::Create(m_InputValues->OutputPath / fmt::format("{}_elems.inp", m_InputValues->FilePrefix)));
  fileList.push_back(AtomicFile::Create(m_InputValues->OutputPath / fmt::format("{}_sects.inp", m_InputValues->FilePrefix)));
  fileList.push_back(AtomicFile::Create(m_InputValues->OutputPath / fmt::format("{}_elset.inp", m_InputValues->FilePrefix)));
  fileList.push_back(AtomicFile::Create(m_InputValues->OutputPath / fmt::format("{}.inp", m_InputValues->FilePrefix)));

  for(auto& file : fileList)
  {
    if(file.invalid())
    {
      return ConvertResult(std::move(file));
    }
  }

  int32 err = writeNodes(this, fileList[0].value().tempFilePath().string(), cDims.data(), origin.data(), spacing.data(), getCancel(), m_InputValues->WriteDummyNode); // Nodes file
  if(err < 0)
  {
    return MakeErrorResult(-1113, fmt::format("Error writing output nodes file '{}'", fileList[0].value().tempFilePath().string()));
  }
  if(getCancel()) // Filter has been cancelled
  {
    DeleteFiles(fileList); // delete files
    return {};
  }
  m_MessageHandler(IFilter::Message::Type::Info, "Writing Sections (File 1/5) Complete");

  err = writeElems(this, fileList[1].value().tempFilePath().string(), cDims.data(), pDims, getCancel()); // Elements file
  if(err < 0)
  {
    return MakeErrorResult(-1114, fmt::format("Error writing output elems file '{}'", fileList[1].value().tempFilePath().string()));
  }
  if(getCancel()) // Filter has been cancelled
  {
    DeleteFiles(fileList); // delete files
    return {};
  }
  m_MessageHandler(IFilter::Message::Type::Info, "Writing Sections (File 2/5) Complete");

  err = writeSects(fileList[2].value().tempFilePath().string(), maxGrainId, m_InputValues->HourglassStiffness); // Sections file
  if(err < 0)
  {
    return MakeErrorResult(-1115, fmt::format("Error writing output sects file '{}'", fileList[2].value().tempFilePath().string()));
  }
  if(getCancel()) // Filter has been cancelled
  {
    DeleteFiles(fileList); // delete files
    return {};
  }
  m_MessageHandler(IFilter::Message::Type::Info, "Writing Sections (File 3/5) Complete");

  Result<> elsetResult = writeElset(this, fileList[3].value().tempFilePath().string(), totalPoints, featureIds, maxGrainId, useOocAlgorithm, usesOutOfCoreStore, getCancel()); // Element set file
  if(elsetResult.invalid())
  {
    DeleteFiles(fileList);
    return elsetResult;
  }
  if(getCancel()) // Filter has been cancelled
  {
    DeleteFiles(fileList); // delete files
    return {};
  }
  m_MessageHandler(IFilter::Message::Type::Info, "Writing Sections (File 4/5) Complete");

  err = writeMaster(fileList[4].value().tempFilePath().string(), m_InputValues->JobName, m_InputValues->FilePrefix); // Master file
  if(err < 0)
  {
    return MakeErrorResult(-1117, fmt::format("Error writing output master file '{}'", fileList[4].value().tempFilePath().string()));
  }
  if(getCancel()) // Filter has been cancelled
  {
    DeleteFiles(fileList); // delete files
    return {};
  }
  m_MessageHandler(IFilter::Message::Type::Info, "Writing Sections (File 5/5) Complete");

  for(auto& file : fileList)
  {
    Result<> commitResult = file.value().commit();
    if(commitResult.invalid())
    {
      return commitResult;
    }
  }

  return {};
}
