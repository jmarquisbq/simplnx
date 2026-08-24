#pragma once

#include "H5Support.hpp"

#include "simplnx/DataStructure/IDataStore.hpp"

#include <algorithm>
#include <atomic>
#include <limits>

namespace nx::core::HDF5
{
namespace Support
{
template <typename T>
Result<> FillDataStore(DataArray<T>& dataArray, const DataPath& dataArrayPath, const nx::core::HDF5::DatasetIO& datasetReader, const std::optional<std::vector<hsize_t>>& start = std::nullopt,
                       const std::optional<std::vector<hsize_t>>& count = std::nullopt)
{
  try
  {
    using StoreType = DataStore<T>;
    StoreType& dataStore = dataArray.template getIDataStoreRefAs<StoreType>();
    auto dataSpan = dataStore.createSpan();
    Result<> result;
    if(start.has_value() && count.has_value())
    {
      std::vector<uint64> startVec(start->begin(), start->end());
      std::vector<uint64> countVec(count->begin(), count->end());
      result = datasetReader.readIntoSpan<T>(dataSpan, startVec, countVec);
    }
    else
    {
      result = datasetReader.readIntoSpan<T>(dataSpan);
    }
    if(result.invalid())
    {
      return MakeErrorResult(-21002,
                             fmt::format("Error reading dataset '{}' with '{}' total elements into data store for data array '{}' with '{}' total elements ('{}' tuples and '{}' components):\n\n{}",
                                         dataArrayPath.getTargetName(), datasetReader.getNumElements(), dataArrayPath.toString(), dataArray.getSize(), dataArray.getNumberOfTuples(),
                                         dataArray.getNumberOfComponents(), result.errors()[0].message));
    }
  } catch(const std::exception& e)
  {
    return MakeErrorResult(-21003, e.what());
  }

  return {};
}

template <typename T>
Result<> FillOocDataStore(DataArray<T>& dataArray, const DataPath& dataArrayPath, const nx::core::HDF5::DatasetIO& datasetReader, const std::optional<std::vector<hsize_t>>& start = std::nullopt,
                          const std::optional<std::vector<hsize_t>>& count = std::nullopt, const std::atomic_bool* shouldCancel = nullptr)
{
  auto& absDataStore = dataArray.getDataStoreRef();

  // Streaming path: read HDF5 dataset in row-batches using hyperslab reads
  // to avoid allocating a buffer proportional to the full dataset size.
  // When start/count are provided, the read is restricted to that sub-region.
  auto dims = datasetReader.getDimensions();
  if(dims.empty())
  {
    return MakeErrorResult(-21005, fmt::format("Error reading dataset '{}': unable to get dimensions.", dataArrayPath.getTargetName()));
  }

  // Compute effective start/count for the read region
  const usize rank = dims.size();
  std::vector<uint64> effStart(rank, 0);
  std::vector<uint64> effCount(rank);
  for(usize d = 0; d < rank; d++)
  {
    effCount[d] = dims[d];
  }
  if(start.has_value())
  {
    for(usize d = 0; d < std::min(rank, start.value().size()); d++)
    {
      if(start.value()[d] > dims[d])
      {
        return MakeErrorResult(-21006, fmt::format("Error reading dataset '{}': HDF5 start is outside the dataset dimensions.", dataArrayPath.getTargetName()));
      }
      effStart[d] = start.value()[d];
      effCount[d] = dims[d] - start.value()[d];
    }
  }
  if(count.has_value())
  {
    for(usize d = 0; d < std::min(rank, count.value().size()); d++)
    {
      if(count.value()[d] > dims[d] - effStart[d])
      {
        return MakeErrorResult(-21006, fmt::format("Error reading dataset '{}': HDF5 count exceeds the dataset dimensions.", dataArrayPath.getTargetName()));
      }
      effCount[d] = count.value()[d];
    }
  }

  // Walk a C-order hyperslab decomposition.  Batching solely along dimension 0
  // would require a complete row to fit in memory, which is not true for
  // vector-valued 3-D datasets.  Instead, select the first dimension whose
  // trailing extent fits in the fixed transfer buffer and iterate all preceding
  // dimensions one at a time.  Each selected hyperslab remains a contiguous
  // run in both the source dataset and the flattened destination DataArray.
  constexpr usize k_TargetBatchElements = 65536;
  usize expectedElements = 1;
  for(const uint64 dimensionSize : effCount)
  {
    if(dimensionSize == 0 || dimensionSize > std::numeric_limits<usize>::max() || expectedElements > std::numeric_limits<usize>::max() / static_cast<usize>(dimensionSize))
    {
      return MakeErrorResult(-21006, fmt::format("Error reading dataset '{}': invalid or overflowing HDF5 dimensions.", dataArrayPath.getTargetName()));
    }
    expectedElements *= static_cast<usize>(dimensionSize);
  }
  if(expectedElements != absDataStore.getSize())
  {
    return MakeErrorResult(-21009, fmt::format("Error reading dataset '{}': requested HDF5 region has '{}' elements but destination data array '{}' has '{}'.", dataArrayPath.getTargetName(),
                                               expectedElements, dataArrayPath.toString(), absDataStore.getSize()));
  }

  usize trailingElements = 1;
  usize batchDimension = 0;
  for(usize d = rank - 1; d > 0; d--)
  {
    if(trailingElements > k_TargetBatchElements / effCount[d])
    {
      batchDimension = d;
      break;
    }
    trailingElements *= static_cast<usize>(effCount[d]);
    batchDimension = d - 1;
  }

  const usize batchRows = std::max(static_cast<usize>(1), k_TargetBatchElements / trailingElements);
  usize outerCount = 1;
  for(usize d = 0; d < batchDimension; d++)
  {
    if(outerCount > std::numeric_limits<usize>::max() / effCount[d])
    {
      return MakeErrorResult(-21006, fmt::format("Error reading dataset '{}': HDF5 dimensions overflow the transfer iterator.", dataArrayPath.getTargetName()));
    }
    outerCount *= effCount[d];
  }

  std::vector<T> buf(k_TargetBatchElements);
  std::vector<uint64> hStart(rank);
  std::vector<uint64> hCount(rank, 1);
  usize flatOffset = 0;
  for(usize outer = 0; outer < outerCount; outer++)
  {
    if(shouldCancel != nullptr && *shouldCancel)
    {
      return {};
    }
    usize coordinate = outer;
    for(usize d = batchDimension; d-- > 0;)
    {
      hStart[d] = effStart[d] + (coordinate % effCount[d]);
      coordinate /= effCount[d];
    }
    for(usize d = batchDimension + 1; d < rank; d++)
    {
      hStart[d] = effStart[d];
      hCount[d] = effCount[d];
    }

    for(usize batchStart = 0; batchStart < effCount[batchDimension]; batchStart += batchRows)
    {
      if(shouldCancel != nullptr && *shouldCancel)
      {
        return {};
      }
      const usize rowCount = std::min(batchRows, static_cast<usize>(effCount[batchDimension] - batchStart));
      const usize batchElements = rowCount * trailingElements;
      hStart[batchDimension] = effStart[batchDimension] + batchStart;
      hCount[batchDimension] = rowCount;

      nonstd::span<T> batchSpan(buf.data(), batchElements);
      auto result = datasetReader.readIntoSpan<T>(batchSpan, hStart, hCount);
      if(result.invalid())
      {
        return MakeErrorResult(
            -21003, fmt::format("Error reading dataset '{}' into data store for data array '{}':\n\n{}", dataArrayPath.getTargetName(), dataArrayPath.toString(), result.errors()[0].message));
      }

      if(flatOffset > absDataStore.getSize() || batchElements > absDataStore.getSize() - flatOffset)
      {
        return MakeErrorResult(-21007, fmt::format("Error reading dataset '{}': destination bulk-write range exceeds data array '{}'.", dataArrayPath.getTargetName(), dataArrayPath.toString()));
      }
      auto writeResult = absDataStore.copyFromBuffer(flatOffset, nonstd::span<const T>(buf.data(), batchElements));
      if(writeResult.invalid())
      {
        return writeResult;
      }
      flatOffset += batchElements;
    }
  }

  if(flatOffset != expectedElements)
  {
    return MakeErrorResult(-21008, fmt::format("Error reading dataset '{}': bounded transfer did not cover the requested HDF5 region.", dataArrayPath.getTargetName()));
  }

  return {};
}

template <typename T>
Result<> FillDataArray(DataStructure& dataStructure, const DataPath& dataArrayPath, const nx::core::HDF5::DatasetIO& datasetReader, const std::optional<std::vector<hsize_t>>& start = std::nullopt,
                       const std::optional<std::vector<hsize_t>>& count = std::nullopt, const std::atomic_bool* shouldCancel = nullptr)
{
  auto& dataArray = dataStructure.getDataRefAs<DataArray<T>>(dataArrayPath);
  if(dataArray.getIDataStoreRef().getStoreType() != IDataStore::StoreType::OutOfCore)
  {
    return FillDataStore(dataArray, dataArrayPath, datasetReader, start, count);
  }
  else
  {
    return FillOocDataStore(dataArray, dataArrayPath, datasetReader, start, count, shouldCancel);
  }
}
} // namespace Support
} // namespace nx::core::HDF5
