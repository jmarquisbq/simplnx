#include "DBSCANScanline.hpp"

#include "DBSCAN.hpp"

#include "simplnx/Common/Range.hpp"
#include "simplnx/DataStructure/AttributeMatrix.hpp"
#include "simplnx/DataStructure/DataArray.hpp"
#include "simplnx/DataStructure/IO/Generic/IExternalSort.hpp"
#include "simplnx/DataStructure/IO/Generic/ITemporaryRecordStore.hpp"
#include "simplnx/Utilities/AlgorithmDispatch.hpp"
#include "simplnx/Utilities/BoundedRecordPageCache.hpp"
#include "simplnx/Utilities/ClusteringUtilities.hpp"
#include "simplnx/Utilities/DataStoreUtilities.hpp"
#include "simplnx/Utilities/FilterUtilities.hpp"
#include "simplnx/Utilities/MaskCompareUtilities.hpp"

#include <fmt/format.h>

#include <array>
#include <cstring>
#include <limits>
#include <nonstd/span.hpp>
#include <type_traits>

using namespace nx::core;

// =============================================================================
// DBSCANScanline — Out-of-Core (OOC) Algorithm
//
// This file implements the Scanline variant selected by DispatchAlgorithm.
// Genuine OOC arrays use ExternalGDCF: fixed-window input/output transfers,
// external sort streams, temporary fixed-width record stores, bounded page
// caches, and fixed distance tiles. That path has neither per-cell DataStore
// access nor resident point/grid-sized allocations. The older resident GDCF
// below remains only as the forced-Scanline test fallback when every target
// store is in memory.
// =============================================================================

namespace
{
/**
 * @brief Constant mask used when masking is disabled without allocating one value per tuple.
 */
class AllTrueMaskCompare final : public MaskCompareUtilities::MaskCompare
{
public:
  /** @brief Reports both supplied tuples as accepted. */
  bool bothTrue(usize, usize) const override
  {
    return true;
  }
  /** @brief No tuple pair is rejected by the disabled mask. */
  bool bothFalse(usize, usize) const override
  {
    return false;
  }
  /** @brief Reports every tuple as accepted. */
  bool isTrue(usize) const override
  {
    return true;
  }
  /** @brief No-op because this synthetic mask owns no writable storage. */
  void setValue(usize, bool) override
  {
  }
  /** @brief Returns zero because tuple bounds come from the coordinate array. */
  usize getNumberOfTuples() const override
  {
    return 0;
  }
  /** @brief A logical mask has one component. */
  usize getNumberOfComponents() const override
  {
    return 1;
  }
  /** @brief Returns zero because the algorithm does not query an unbounded all-true count. */
  usize countTrueValues() const override
  {
    return 0;
  }
};

constexpr uint64 k_ExternalBatchRecords = 65536;
constexpr uint64 k_ExternalRecordsPerPage = 4096;
constexpr usize k_ExternalCachePages = 8;
constexpr uint64 k_MergeTileRecords = 2048;

/**
 * @brief Fixed-width externally stored state for one occupied spatial grid.
 *
 * Member ranges point into the sorted point stream; parent and cluster fields
 * hold the bounded-cache disjoint-set state used during grid merging.
 */
struct DBSCANGridState
{
  uint64 Grid[3] = {0, 0, 0};
  uint64 FirstMember = 0;
  uint64 MemberCount = 0;
  uint64 Parent = 0;
  int32 ClusterId = 0;
  uint8 IsCore = 0;
};

/** @brief Fixed-width reference used to order core grids without retaining an in-memory index. */
struct DBSCANCoreOrder
{
  uint64 GridId = 0;
};

/** @brief One grid coordinate in an externally stored per-axis lookup table. */
struct DBSCANAxisRecord
{
  uint64 Coordinate = 0;
};

/** @brief Deferred tuple label written externally and sorted back into original tuple order. */
struct DBSCANLabelRecord
{
  uint64 TupleId = 0;
  int32 FeatureId = 0;
};

/** @brief Fixed-width point record sorted by grid coordinates while preserving its original tuple ID. */
template <typename T>
struct DBSCANPointRecord
{
  uint64 Grid[3] = {0, 0, 0};
  uint64 TupleId = 0;
  T Coordinates[3] = {};
};

static_assert(std::is_trivially_copyable_v<DBSCANGridState>);
static_assert(std::is_trivially_copyable_v<DBSCANCoreOrder>);
static_assert(std::is_trivially_copyable_v<DBSCANAxisRecord>);
static_assert(std::is_trivially_copyable_v<DBSCANLabelRecord>);

/** @brief Orders point records by grid Z/Y/X and then tuple ID for deterministic membership ranges. */
template <typename T>
int32 ComparePointRecords(nonstd::span<const std::byte> left, nonstd::span<const std::byte> right)
{
  DBSCANPointRecord<T> lhs = {};
  DBSCANPointRecord<T> rhs = {};
  std::memcpy(&lhs, left.data(), sizeof(lhs));
  std::memcpy(&rhs, right.data(), sizeof(rhs));
  for(usize dimension = 3; dimension-- > 0;)
  {
    if(lhs.Grid[dimension] != rhs.Grid[dimension])
    {
      return lhs.Grid[dimension] < rhs.Grid[dimension] ? -1 : 1;
    }
  }
  if(lhs.TupleId == rhs.TupleId)
  {
    return 0;
  }
  return lhs.TupleId < rhs.TupleId ? -1 : 1;
}

/** @brief Tests whether two sorted point records belong to the same spatial grid. */
template <typename T>
bool SameGrid(const DBSCANPointRecord<T>& left, const DBSCANPointRecord<T>& right)
{
  return left.Grid[0] == right.Grid[0] && left.Grid[1] == right.Grid[1] && left.Grid[2] == right.Grid[2];
}

/** @brief Orders external axis coordinates for bounded neighborhood-range queries. */
int32 CompareAxisRecords(nonstd::span<const std::byte> left, nonstd::span<const std::byte> right)
{
  DBSCANAxisRecord lhs = {};
  DBSCANAxisRecord rhs = {};
  std::memcpy(&lhs, left.data(), sizeof(lhs));
  std::memcpy(&rhs, right.data(), sizeof(rhs));
  if(lhs.Coordinate == rhs.Coordinate)
  {
    return 0;
  }
  return lhs.Coordinate < rhs.Coordinate ? -1 : 1;
}

/** @brief Restores deferred labels to original tuple order before the bulk output pass. */
int32 CompareLabelRecords(nonstd::span<const std::byte> left, nonstd::span<const std::byte> right)
{
  DBSCANLabelRecord lhs = {};
  DBSCANLabelRecord rhs = {};
  std::memcpy(&lhs, left.data(), sizeof(lhs));
  std::memcpy(&rhs, right.data(), sizeof(rhs));
  if(lhs.TupleId == rhs.TupleId)
  {
    return 0;
  }
  return lhs.TupleId < rhs.TupleId ? -1 : 1;
}

/**
 * @brief Executes DBSCAN with externally sorted point membership and bounded record caches.
 *
 * GDCF normally keeps point, grid, neighborhood, and cluster state resident.
 * This implementation instead streams points into a stable external sort,
 * materializes fixed grid/core/axis records through bounded caches, and performs
 * distance tests in fixed tiles. The resulting memory use is independent of the
 * number of input points and occupied grids, apart from configured cache bounds.
 */
template <typename T, typename MaskT>
class ExternalGDCF
{
public:
  static_assert(std::is_trivially_copyable_v<DBSCANPointRecord<T>>);

  /** @brief Borrows input stores and settings for one complete external GDCF execution. */
  ExternalGDCF(const AbstractDataStore<T>& inputStore, const AbstractDataStore<MaskT>* maskStore, const DBSCANInputValues& inputValues, usize dimensions, const std::atomic_bool& shouldCancel)
  : m_InputStore(inputStore)
  , m_MaskStore(maskStore)
  , m_InputValues(inputValues)
  , m_Dimensions(dimensions)
  , m_ShouldCancel(shouldCancel)
  {
  }

  /**
   * @brief Validates inputs, discovers grid origin, externally sorts enabled points, and builds grid/core/axis records.
   * @return A valid result, a no-cluster warning, or the first provider, validation, cancellation, or record-I/O failure.
   */
  Result<> initialize()
  {
    auto& ioCollection = DataStoreUtilities::GetIOCollection();
    if(!ioCollection.hasExternalSortCapability() || !ioCollection.hasTemporaryRecordStoreCapability())
    {
      return MakeErrorResult(-54061, "DBSCAN out-of-core execution requires registered external-sort and temporary-record-store providers.");
    }
    if(m_Dimensions != 2 && m_Dimensions != 3)
    {
      return MakeErrorResult(-54060, "Input components invalid. Only 2 or 3 accepted.");
    }
    const usize tupleCount = m_InputStore.getNumberOfTuples();
    if(m_InputStore.getNumberOfComponents() != m_Dimensions)
    {
      return MakeErrorResult(-54060, "DBSCAN input component count does not match the selected dimensionality.");
    }
    if(m_MaskStore != nullptr && (m_MaskStore->getNumberOfTuples() != tupleCount || m_MaskStore->getNumberOfComponents() != 1))
    {
      return MakeErrorResult(-54062, "DBSCAN mask tuple count does not match the clustering array or the mask is not scalar.");
    }
    if(m_InputValues.MinPoints < 0)
    {
      return MakeErrorResult(-54063, "DBSCAN Minimum Points cannot be negative.");
    }

    std::array<float32, 3> minimum = {std::numeric_limits<float32>::quiet_NaN(), std::numeric_limits<float32>::quiet_NaN(), std::numeric_limits<float32>::quiet_NaN()};
    auto values = std::make_unique<T[]>(k_ExternalBatchRecords * m_Dimensions);
    auto masks = m_MaskStore == nullptr ? nullptr : std::make_unique<MaskT[]>(k_ExternalBatchRecords);
    for(usize offset = 0; offset < tupleCount; offset += k_ExternalBatchRecords)
    {
      if(m_ShouldCancel)
      {
        return {};
      }
      const usize count = std::min<usize>(k_ExternalBatchRecords, tupleCount - offset);
      Result<> result = m_InputStore.copyIntoBuffer(offset * m_Dimensions, nonstd::span<T>(values.get(), count * m_Dimensions));
      if(result.invalid())
      {
        return result;
      }
      if(m_MaskStore != nullptr)
      {
        result = m_MaskStore->copyIntoBuffer(offset, nonstd::span<MaskT>(masks.get(), count));
        if(result.invalid())
        {
          return result;
        }
      }
      for(usize index = 0; index < count; ++index)
      {
        if(m_MaskStore != nullptr && !static_cast<bool>(masks[index]))
        {
          continue;
        }
        for(usize dimension = 0; dimension < m_Dimensions; ++dimension)
        {
          const float32 value = static_cast<float32>(values[index * m_Dimensions + dimension]);
          minimum[dimension] = std::isnan(minimum[dimension]) ? value : std::min(minimum[dimension], value);
        }
      }
    }

    if(std::isnan(minimum[0]))
    {
      return MakeWarningVoidResult(-85640, "No clusters detected - Consider reducing number of required points (`Minimum Points`) or increasing acceptable distance (`Epsilon`).");
    }

    m_SideLength = m_InputValues.Epsilon / std::sqrt(static_cast<float32>(m_Dimensions));
    for(usize dimension = 0; dimension < m_Dimensions; ++dimension)
    {
      m_Origin[dimension] = minimum[dimension] - m_SideLength;
    }

    ExternalSortConfig membershipConfig;
    membershipConfig.recordSize = sizeof(DBSCANPointRecord<T>);
    membershipConfig.maxRecordsPerBatch = k_ExternalBatchRecords;
    membershipConfig.compare = ComparePointRecords<T>;
    auto membershipResult = ioCollection.createExternalSort(membershipConfig);
    if(membershipResult.invalid())
    {
      return ConvertResult(std::move(membershipResult));
    }
    m_Membership = std::move(membershipResult.value());
    if(m_Membership == nullptr)
    {
      return MakeErrorResult(-54064, "DBSCAN external-sort provider returned a null point-membership sorter.");
    }

    auto pointRecords = std::make_unique<DBSCANPointRecord<T>[]>(k_ExternalBatchRecords);
    for(usize offset = 0; offset < tupleCount; offset += k_ExternalBatchRecords)
    {
      if(m_ShouldCancel)
      {
        return {};
      }
      const usize count = std::min<usize>(k_ExternalBatchRecords, tupleCount - offset);
      Result<> result = m_InputStore.copyIntoBuffer(offset * m_Dimensions, nonstd::span<T>(values.get(), count * m_Dimensions));
      if(result.invalid())
      {
        return result;
      }
      if(m_MaskStore != nullptr)
      {
        result = m_MaskStore->copyIntoBuffer(offset, nonstd::span<MaskT>(masks.get(), count));
        if(result.invalid())
        {
          return result;
        }
      }
      uint64 pointCount = 0;
      for(usize index = 0; index < count; ++index)
      {
        if(m_MaskStore != nullptr && !static_cast<bool>(masks[index]))
        {
          continue;
        }
        DBSCANPointRecord<T>& record = pointRecords[static_cast<usize>(pointCount)];
        record = {};
        record.TupleId = static_cast<uint64>(offset + index);
        for(usize dimension = 0; dimension < m_Dimensions; ++dimension)
        {
          const float32 value = static_cast<float32>(values[index * m_Dimensions + dimension]);
          record.Grid[dimension] = static_cast<uint64>(std::floor((value - m_Origin[dimension]) / m_SideLength));
          record.Coordinates[dimension] = values[index * m_Dimensions + dimension];
        }
        ++pointCount;
      }
      if(pointCount > 0)
      {
        auto bytes = nonstd::span<const std::byte>(reinterpret_cast<const std::byte*>(pointRecords.get()), static_cast<usize>(pointCount) * sizeof(DBSCANPointRecord<T>));
        result = m_Membership->append(pointCount, bytes, m_ShouldCancel, {});
        if(result.invalid() || m_ShouldCancel)
        {
          return result;
        }
      }
    }
    Result<> finishResult = m_Membership->finish(m_ShouldCancel, {});
    if(finishResult.invalid() || m_ShouldCancel)
    {
      return finishResult;
    }

    Result<> stateResult = buildGridStates(pointRecords.get());
    if(stateResult.invalid() || m_ShouldCancel)
    {
      return stateResult;
    }
    Result<> axisResult = buildAxisIndexes();
    if(axisResult.invalid() || m_ShouldCancel)
    {
      return axisResult;
    }
    Result<> orderResult = buildCoreOrder();
    if(orderResult.invalid() || m_ShouldCancel)
    {
      return orderResult;
    }
    try
    {
      m_GridStateCache = std::make_unique<BoundedRecordPageCache<DBSCANGridState>>(*m_GridStates, k_ExternalRecordsPerPage, k_ExternalCachePages);
      for(usize dimension = 0; dimension < m_Dimensions; ++dimension)
      {
        m_AxisCaches[dimension] = std::make_unique<BoundedRecordPageCache<DBSCANAxisRecord>>(*m_AxisStores[dimension], k_ExternalRecordsPerPage, k_ExternalCachePages);
      }
      m_LeftMergeBuffer = std::make_unique<DBSCANPointRecord<T>[]>(k_MergeTileRecords);
      m_RightMergeBuffer = std::make_unique<DBSCANPointRecord<T>[]>(k_MergeTileRecords);
      if(m_CoreOrder != nullptr)
      {
        m_CoreOrderCache = std::make_unique<BoundedRecordPageCache<DBSCANCoreOrder>>(*m_CoreOrder, k_ExternalRecordsPerPage, k_ExternalCachePages);
      }
    } catch(const std::bad_alloc&)
    {
      return MakeErrorResult(-54070, "DBSCAN failed to allocate its bounded temporary-record page caches.");
    }
    return {};
  }

  /** @brief Returns the number of occupied spatial grids built during initialization. */
  uint64 activeGridCount() const
  {
    return m_ActiveGridCount;
  }

  /** @brief Returns the number of occupied grids that meet the minimum-point core criterion. */
  uint64 coreGridCount() const
  {
    return m_CoreGridCount;
  }

  /**
   * @brief Creates the externally stored processing order for core grids.
   * @return A valid result or the first temporary-record I/O or cancellation failure.
   */
  Result<> prepareCoreOrder()
  {
    if(m_CoreGridCount == 0)
    {
      return MakeWarningVoidResult(-85640, "No clusters detected - Consider reducing number of required points (`Minimum Points`) or increasing acceptable distance (`Epsilon`).");
    }
    if(m_CoreOrderCache == nullptr || m_GridStateCache == nullptr)
    {
      return MakeErrorResult(-54071, "DBSCAN external grid-state or core-order cache was not initialized.");
    }
    Result<> flushResult = flushCaches();
    if(flushResult.invalid())
    {
      return flushResult;
    }

    switch(static_cast<DBSCAN::ParseOrder>(m_InputValues.ParseOrder))
    {
    case DBSCAN::ParseOrder::LowDensityFirst: {
      Result<> sortResult = quickSortCoreOrder(0, m_CoreGridCount - 1);
      if(sortResult.invalid())
      {
        return sortResult;
      }
      break;
    }
    case DBSCAN::ParseOrder::Random:
    case DBSCAN::ParseOrder::SeededRandom: {
      std::mt19937_64 generator(m_InputValues.Seed);
      std::uniform_real_distribution<float64> distribution(0, 1);
      const float64 maximumIndex = static_cast<float64>(m_CoreGridCount - 1);
      for(uint64 index = 1; index < m_CoreGridCount; ++index)
      {
        if(m_ShouldCancel)
        {
          return {};
        }
        const uint64 randomIndex = static_cast<uint64>(std::floor(distribution(generator) * maximumIndex));
        Result<DBSCANCoreOrder> leftResult = m_CoreOrderCache->read(index, m_ShouldCancel);
        if(leftResult.invalid())
        {
          return ConvertResult(std::move(leftResult));
        }
        Result<DBSCANCoreOrder> rightResult = m_CoreOrderCache->read(randomIndex, m_ShouldCancel);
        if(rightResult.invalid())
        {
          return ConvertResult(std::move(rightResult));
        }
        Result<> writeResult = m_CoreOrderCache->write(index, rightResult.value(), m_ShouldCancel);
        if(writeResult.invalid())
        {
          return writeResult;
        }
        writeResult = m_CoreOrderCache->write(randomIndex, leftResult.value(), m_ShouldCancel);
        if(writeResult.invalid())
        {
          return writeResult;
        }
      }
      break;
    }
    default:
      return MakeErrorResult(-54072, fmt::format("DBSCAN parse-order value {} is invalid.", m_InputValues.ParseOrder));
    }
    return flushCaches();
  }

  /**
   * @brief Merges neighboring core grids and assigns deterministic cluster roots.
   * @return A valid result or the first neighborhood, distance-tile, cache, or cancellation failure.
   */
  Result<> cluster()
  {
    Result<> orderResult = prepareCoreOrder();
    if(orderResult.invalid() || !orderResult.warnings().empty() || m_ShouldCancel)
    {
      return orderResult;
    }

    for(uint64 orderIndex = 0; orderIndex < m_CoreGridCount; ++orderIndex)
    {
      if(m_ShouldCancel)
      {
        return {};
      }
      Result<DBSCANCoreOrder> coreOrderResult = m_CoreOrderCache->read(orderIndex, m_ShouldCancel);
      if(coreOrderResult.invalid())
      {
        return ConvertResult(std::move(coreOrderResult));
      }
      const uint64 coreGridId = coreOrderResult.value().GridId;
      Result<GridNeighborhood> neighborsResult = gridNeighborhood(coreGridId);
      if(neighborsResult.invalid())
      {
        return ConvertResult(std::move(neighborsResult));
      }
      if(m_ShouldCancel)
      {
        return {};
      }

      std::array<uint64, 126> clusterGridIds = {};
      usize clusterGridCount = 1;
      clusterGridIds[0] = coreGridId;
      const GridNeighborhood& neighbors = neighborsResult.value();
      for(usize neighborIndex = 0; neighborIndex < neighbors.Count; ++neighborIndex)
      {
        const uint64 neighborGridId = neighbors.GridIds[neighborIndex];
        Result<bool> inferResult = infer(coreGridId, neighborGridId);
        if(inferResult.invalid())
        {
          return ConvertResult(std::move(inferResult));
        }
        if(m_ShouldCancel)
        {
          return {};
        }
        if(inferResult.value())
        {
          continue;
        }
        Result<bool> mergeResult = canMerge(coreGridId, neighborGridId);
        if(mergeResult.invalid())
        {
          return ConvertResult(std::move(mergeResult));
        }
        if(!mergeResult.value())
        {
          continue;
        }
        Result<DBSCANGridState> neighborStateResult = readGridState(neighborGridId);
        if(neighborStateResult.invalid())
        {
          return ConvertResult(std::move(neighborStateResult));
        }
        if(neighborStateResult.value().IsCore == 0 && neighborStateResult.value().Parent == neighborGridId)
        {
          Result<> parentResult = setParent(neighborGridId, coreGridId);
          if(parentResult.invalid())
          {
            return parentResult;
          }
        }
        else
        {
          if(clusterGridCount == clusterGridIds.size())
          {
            return MakeErrorResult(-54079, "DBSCAN core-cluster merge list exceeded its fixed neighbor capacity.");
          }
          clusterGridIds[clusterGridCount++] = neighborGridId;
        }
      }
      Result<> mergeResult = mergeLowestRootCluster(clusterGridIds, clusterGridCount);
      if(mergeResult.invalid())
      {
        return mergeResult;
      }
    }
    Result<> flushResult = flushCaches();
    if(flushResult.invalid() || m_ShouldCancel)
    {
      return flushResult;
    }

    uint64 operations = 0;
    do
    {
      operations = 0;
      for(uint64 gridId = 0; gridId < m_ActiveGridCount; ++gridId)
      {
        if(m_ShouldCancel)
        {
          return {};
        }
        Result<DBSCANGridState> activeStateResult = readGridState(gridId);
        if(activeStateResult.invalid())
        {
          return ConvertResult(std::move(activeStateResult));
        }
        if(activeStateResult.value().IsCore != 0)
        {
          continue;
        }
        Result<GridNeighborhood> neighborsResult = gridNeighborhood(gridId);
        if(neighborsResult.invalid())
        {
          return ConvertResult(std::move(neighborsResult));
        }
        const GridNeighborhood& neighbors = neighborsResult.value();
        if(m_ShouldCancel)
        {
          return {};
        }
        for(usize neighborIndex = 0; neighborIndex < neighbors.Count; ++neighborIndex)
        {
          const uint64 neighborGridId = neighbors.GridIds[neighborIndex];
          Result<bool> inferResult = infer(gridId, neighborGridId);
          if(inferResult.invalid())
          {
            return ConvertResult(std::move(inferResult));
          }
          if(m_ShouldCancel)
          {
            return {};
          }
          if(inferResult.value())
          {
            continue;
          }
          Result<bool> mergeResult = canMerge(gridId, neighborGridId);
          if(mergeResult.invalid())
          {
            return ConvertResult(std::move(mergeResult));
          }
          if(!mergeResult.value())
          {
            continue;
          }

          Result<uint64> activeParentResult = findRoot(gridId);
          if(activeParentResult.invalid())
          {
            return ConvertResult(std::move(activeParentResult));
          }
          Result<uint64> neighborParentResult = findRoot(neighborGridId);
          if(neighborParentResult.invalid())
          {
            return ConvertResult(std::move(neighborParentResult));
          }
          if(m_ShouldCancel)
          {
            return {};
          }
          const uint64 activeParent = activeParentResult.value();
          const uint64 neighborParent = neighborParentResult.value();
          Result<DBSCANGridState> neighborStateResult = readGridState(neighborGridId);
          if(neighborStateResult.invalid())
          {
            return ConvertResult(std::move(neighborStateResult));
          }

          if(activeParent == gridId)
          {
            if(neighborStateResult.value().IsCore == 0 && neighborParent == neighborGridId)
            {
              continue;
            }
            Result<> parentResult = setParent(gridId, neighborParent);
            if(parentResult.invalid())
            {
              return parentResult;
            }
          }
          else if(neighborStateResult.value().IsCore == 0 && neighborParent == neighborGridId)
          {
            Result<> parentResult = setParent(neighborGridId, activeParent);
            if(parentResult.invalid())
            {
              return parentResult;
            }
          }
          else
          {
            Result<DBSCANGridState> activeRootStateResult = readGridState(activeParent);
            if(activeRootStateResult.invalid())
            {
              return ConvertResult(std::move(activeRootStateResult));
            }
            Result<DBSCANGridState> neighborRootStateResult = readGridState(neighborParent);
            if(neighborRootStateResult.invalid())
            {
              return ConvertResult(std::move(neighborRootStateResult));
            }
            Result<> parentResult;
            if(activeRootStateResult.value().ClusterId < neighborRootStateResult.value().ClusterId)
            {
              parentResult = setParent(neighborParent, activeParent);
            }
            else
            {
              parentResult = setParent(activeParent, neighborParent);
            }
            if(parentResult.invalid())
            {
              return parentResult;
            }
          }
          if(operations == std::numeric_limits<uint64>::max())
          {
            return MakeErrorResult(-54083, "DBSCAN non-core merge operation count exceeds uint64.");
          }
          ++operations;
        }
      }
      flushResult = flushCaches();
      if(flushResult.invalid() || m_ShouldCancel)
      {
        return flushResult;
      }
    } while(operations > 0);

    m_FinalClusterCount = 0;
    for(uint64 gridId = 0; gridId < m_ActiveGridCount; ++gridId)
    {
      if(m_ShouldCancel)
      {
        return {};
      }
      Result<DBSCANGridState> stateResult = readGridState(gridId);
      if(stateResult.invalid())
      {
        return ConvertResult(std::move(stateResult));
      }
      DBSCANGridState state = stateResult.value();
      if(state.Parent != gridId)
      {
        continue;
      }
      if(state.IsCore != 0)
      {
        if(m_FinalClusterCount == std::numeric_limits<int32>::max())
        {
          return MakeErrorResult(-54084, "DBSCAN final cluster count exceeds the Int32 label range.");
        }
        state.ClusterId = ++m_FinalClusterCount;
      }
      else
      {
        state.ClusterId = 0;
      }
      Result<> writeResult = writeGridState(gridId, state);
      if(writeResult.invalid())
      {
        return writeResult;
      }
    }
    return flushCaches();
  }

  /** @brief Returns the final number of connected clusters after core-grid merging. */
  int32 finalClusterCount() const
  {
    return m_FinalClusterCount;
  }

  /**
   * @brief Resolves point/grid membership into labels and bulk-writes them in original tuple order.
   * @param featureIds Destination feature-ID store, which may be disk-backed.
   * @return A valid result or the first external-sort, record-I/O, output-I/O, or cancellation failure.
   */
  Result<> label(AbstractDataStore<int32>& featureIds)
  {
    if(featureIds.getNumberOfComponents() != 1 || featureIds.getNumberOfTuples() != m_InputStore.getNumberOfTuples())
    {
      return MakeErrorResult(-54085, "DBSCAN FeatureIds output shape does not match the clustering input tuple shape.");
    }
    Result<> flushResult = flushCaches();
    if(flushResult.invalid() || m_ShouldCancel)
    {
      return flushResult;
    }

    ExternalSortConfig labelConfig;
    labelConfig.recordSize = sizeof(DBSCANLabelRecord);
    labelConfig.maxRecordsPerBatch = k_ExternalBatchRecords;
    labelConfig.compare = CompareLabelRecords;
    auto labelResult = DataStoreUtilities::GetIOCollection().createExternalSort(labelConfig);
    if(labelResult.invalid())
    {
      return ConvertResult(std::move(labelResult));
    }
    std::unique_ptr<IExternalSort> labels = std::move(labelResult.value());
    if(labels == nullptr)
    {
      return MakeErrorResult(-54086, "DBSCAN external-sort provider returned a null tuple-label sorter.");
    }

    auto points = std::make_unique<DBSCANPointRecord<T>[]>(k_ExternalBatchRecords);
    auto labelRecords = std::make_unique<DBSCANLabelRecord[]>(k_ExternalBatchRecords);
    const uint64 pointCount = m_Membership->recordCount();
    uint64 currentGridId = 0;
    bool hasGrid = false;
    DBSCANPointRecord<T> previousPoint = {};
    int32 currentFeatureId = 0;
    for(uint64 offset = 0; offset < pointCount; offset += k_ExternalBatchRecords)
    {
      if(m_ShouldCancel)
      {
        return {};
      }
      const uint64 count = std::min<uint64>(k_ExternalBatchRecords, pointCount - offset);
      Result<uint64> readResult = readMembership(offset, count, points.get());
      if(readResult.invalid())
      {
        return ConvertResult(std::move(readResult));
      }
      for(uint64 index = 0; index < count; ++index)
      {
        const DBSCANPointRecord<T>& point = points[static_cast<usize>(index)];
        if(!hasGrid || !SameGrid(previousPoint, point))
        {
          if(hasGrid)
          {
            ++currentGridId;
          }
          if(currentGridId >= m_ActiveGridCount)
          {
            return MakeErrorResult(-54087, "DBSCAN point-membership sort contains more grid runs than the grid-state store.");
          }
          Result<DBSCANGridState> stateResult = readGridState(currentGridId);
          if(stateResult.invalid())
          {
            return ConvertResult(std::move(stateResult));
          }
          if(compareGridCoordinates(stateResult.value().Grid, point.Grid) != 0)
          {
            return MakeErrorResult(-54087, fmt::format("DBSCAN point-membership grid run {} does not match its grid-state record.", currentGridId));
          }
          Result<uint64> rootResult = findRoot(currentGridId);
          if(rootResult.invalid())
          {
            return ConvertResult(std::move(rootResult));
          }
          if(m_ShouldCancel)
          {
            return {};
          }
          Result<DBSCANGridState> rootStateResult = readGridState(rootResult.value());
          if(rootStateResult.invalid())
          {
            return ConvertResult(std::move(rootStateResult));
          }
          currentFeatureId = rootStateResult.value().ClusterId;
          hasGrid = true;
        }
        labelRecords[static_cast<usize>(index)] = {point.TupleId, currentFeatureId};
        previousPoint = point;
      }
      auto bytes = nonstd::span<const std::byte>(reinterpret_cast<const std::byte*>(labelRecords.get()), static_cast<usize>(count) * sizeof(DBSCANLabelRecord));
      Result<> appendResult = labels->append(count, bytes, m_ShouldCancel, {});
      if(appendResult.invalid() || m_ShouldCancel)
      {
        return appendResult;
      }
    }
    if(hasGrid && currentGridId + 1 != m_ActiveGridCount)
    {
      return MakeErrorResult(-54087, fmt::format("DBSCAN point-membership sort contains {} grid runs; expected {}.", currentGridId + 1, m_ActiveGridCount));
    }
    Result<> finishResult = labels->finish(m_ShouldCancel, {});
    if(finishResult.invalid() || m_ShouldCancel)
    {
      return finishResult;
    }
    if(labels->recordCount() != pointCount)
    {
      return MakeErrorResult(-54088, fmt::format("DBSCAN tuple-label sort contains {} records; expected {}.", labels->recordCount(), pointCount));
    }

    auto outputBuffer = std::make_unique<int32[]>(k_ExternalBatchRecords);
    const uint64 outputTupleCount = featureIds.getNumberOfTuples();
    uint64 windowStart = 0;
    uint64 windowCount = std::min<uint64>(k_ExternalBatchRecords, outputTupleCount);
    std::fill_n(outputBuffer.get(), static_cast<usize>(windowCount), int32{0});
    std::optional<uint64> previousTuple;
    const auto writeWindow = [&]() -> Result<> {
      if(windowCount == 0)
      {
        return {};
      }
      return featureIds.copyFromBuffer(static_cast<usize>(windowStart), nonstd::span<const int32>(outputBuffer.get(), static_cast<usize>(windowCount)));
    };
    const auto advanceWindow = [&]() -> Result<> {
      Result<> result = writeWindow();
      if(result.invalid())
      {
        return result;
      }
      windowStart += windowCount;
      windowCount = std::min<uint64>(k_ExternalBatchRecords, outputTupleCount - windowStart);
      std::fill_n(outputBuffer.get(), static_cast<usize>(windowCount), int32{0});
      return {};
    };
    for(uint64 offset = 0; offset < pointCount; offset += k_ExternalBatchRecords)
    {
      if(m_ShouldCancel)
      {
        return {};
      }
      const uint64 count = std::min<uint64>(k_ExternalBatchRecords, pointCount - offset);
      auto bytes = nonstd::span<std::byte>(reinterpret_cast<std::byte*>(labelRecords.get()), static_cast<usize>(count) * sizeof(DBSCANLabelRecord));
      Result<uint64> readResult = labels->read(offset, count, bytes, m_ShouldCancel);
      if(readResult.invalid())
      {
        return ConvertResult(std::move(readResult));
      }
      if(readResult.value() != count)
      {
        return MakeErrorResult(-54065, fmt::format("DBSCAN tuple-label sort short read at record {}: requested {}, received {}.", offset, count, readResult.value()));
      }
      for(uint64 index = 0; index < count; ++index)
      {
        const DBSCANLabelRecord& record = labelRecords[static_cast<usize>(index)];
        if(record.TupleId >= featureIds.getNumberOfTuples())
        {
          return MakeErrorResult(-54089, fmt::format("DBSCAN tuple-label sort contains out-of-range tuple {}.", record.TupleId));
        }
        if(previousTuple.has_value() && record.TupleId <= *previousTuple)
        {
          return MakeErrorResult(-54089, fmt::format("DBSCAN tuple-label sort is not strictly increasing at tuple {}.", record.TupleId));
        }
        while(record.TupleId >= windowStart + windowCount)
        {
          Result<> advanceResult = advanceWindow();
          if(advanceResult.invalid())
          {
            return advanceResult;
          }
        }
        outputBuffer[static_cast<usize>(record.TupleId - windowStart)] = record.FeatureId;
        previousTuple = record.TupleId;
      }
    }
    while(windowCount > 0)
    {
      Result<> advanceResult = advanceWindow();
      if(advanceResult.invalid())
      {
        return advanceResult;
      }
    }
    return {};
  }

private:
  /** @brief Lexicographically compares two Z/Y/X grid coordinates for binary searches. */
  static int32 compareGridCoordinates(const uint64* left, const uint64* right)
  {
    for(usize dimension = 3; dimension-- > 0;)
    {
      if(left[dimension] != right[dimension])
      {
        return left[dimension] < right[dimension] ? -1 : 1;
      }
    }
    return 0;
  }

  /**
   * @brief Finds an occupied grid by coordinate through bounded record-cache lookups.
   * @return Its record index, an empty optional when unoccupied, or an I/O failure.
   */
  Result<std::optional<uint64>> findGrid(const uint64* coordinates)
  {
    if(m_ShouldCancel)
    {
      return {std::nullopt};
    }
    if(m_GridStateCache == nullptr)
    {
      return MakeErrorResult<std::optional<uint64>>(-54071, "DBSCAN external grid-state cache was not initialized.");
    }
    uint64 begin = 0;
    uint64 end = m_ActiveGridCount;
    while(begin < end)
    {
      if(m_ShouldCancel)
      {
        return {std::nullopt};
      }
      const uint64 middle = begin + ((end - begin) / 2);
      Result<DBSCANGridState> stateResult = m_GridStateCache->read(middle, m_ShouldCancel);
      if(stateResult.invalid())
      {
        return ConvertInvalidResult<std::optional<uint64>>(std::move(stateResult));
      }
      const int32 comparison = compareGridCoordinates(stateResult.value().Grid, coordinates);
      if(comparison < 0)
      {
        begin = middle + 1;
      }
      else
      {
        end = middle;
      }
    }
    if(begin == m_ActiveGridCount)
    {
      return {std::nullopt};
    }
    Result<DBSCANGridState> stateResult = m_GridStateCache->read(begin, m_ShouldCancel);
    if(stateResult.invalid())
    {
      return ConvertInvalidResult<std::optional<uint64>>(std::move(stateResult));
    }
    return {compareGridCoordinates(stateResult.value().Grid, coordinates) == 0 ? std::optional<uint64>{begin} : std::nullopt};
  }

  /** @brief Half-open range of axis-table records that can lie within epsilon of one coordinate. */
  struct AxisNeighborhood
  {
    std::array<uint64, 5> Coordinates = {};
    usize Count = 0;
  };

  /** @brief Locates the candidate coordinate range for one axis without loading the complete axis index. */
  Result<AxisNeighborhood> axisNeighborhood(usize dimension, uint64 coordinate)
  {
    if(m_ShouldCancel)
    {
      return {AxisNeighborhood{}};
    }
    if(dimension >= m_Dimensions || m_AxisCaches[dimension] == nullptr)
    {
      return MakeErrorResult<AxisNeighborhood>(-54071, "DBSCAN external occupied-axis cache was not initialized.");
    }
    const uint64 axisCount = m_AxisStores[dimension]->recordCount();
    uint64 begin = 0;
    uint64 end = axisCount;
    while(begin < end)
    {
      if(m_ShouldCancel)
      {
        return {AxisNeighborhood{}};
      }
      const uint64 middle = begin + ((end - begin) / 2);
      Result<DBSCANAxisRecord> recordResult = m_AxisCaches[dimension]->read(middle, m_ShouldCancel);
      if(recordResult.invalid())
      {
        return ConvertInvalidResult<AxisNeighborhood>(std::move(recordResult));
      }
      if(recordResult.value().Coordinate < coordinate)
      {
        begin = middle + 1;
      }
      else
      {
        end = middle;
      }
    }
    if(begin >= axisCount)
    {
      return MakeErrorResult<AxisNeighborhood>(-54074, fmt::format("DBSCAN occupied-axis index does not contain coordinate {} for dimension {}.", coordinate, dimension));
    }
    Result<DBSCANAxisRecord> targetResult = m_AxisCaches[dimension]->read(begin, m_ShouldCancel);
    if(targetResult.invalid())
    {
      return ConvertInvalidResult<AxisNeighborhood>(std::move(targetResult));
    }
    if(targetResult.value().Coordinate != coordinate)
    {
      return MakeErrorResult<AxisNeighborhood>(-54074, fmt::format("DBSCAN occupied-axis index does not contain coordinate {} for dimension {}.", coordinate, dimension));
    }

    AxisNeighborhood neighborhood;
    const uint64 first = begin < 2 ? 0 : begin - 2;
    const uint64 last = std::min<uint64>(axisCount, begin + 3);
    for(uint64 index = first; index < last; ++index)
    {
      Result<DBSCANAxisRecord> recordResult = m_AxisCaches[dimension]->read(index, m_ShouldCancel);
      if(recordResult.invalid())
      {
        return ConvertInvalidResult<AxisNeighborhood>(std::move(recordResult));
      }
      neighborhood.Coordinates[neighborhood.Count++] = recordResult.value().Coordinate;
    }
    return {neighborhood};
  }

  /** @brief Fixed-capacity set of occupied neighboring grids relevant to one GDCF merge step. */
  struct GridNeighborhood
  {
    std::array<uint64, 125> GridIds = {};
    usize Count = 0;
  };

  /** @brief Combines the three axis ranges into the occupied neighboring-grid set. */
  Result<GridNeighborhood> gridNeighborhood(uint64 gridId)
  {
    Result<DBSCANGridState> targetResult = readGridState(gridId);
    if(targetResult.invalid())
    {
      return ConvertInvalidResult<GridNeighborhood>(std::move(targetResult));
    }
    std::array<AxisNeighborhood, 3> axes;
    for(usize dimension = 0; dimension < m_Dimensions; ++dimension)
    {
      Result<AxisNeighborhood> axisResult = axisNeighborhood(dimension, targetResult.value().Grid[dimension]);
      if(axisResult.invalid())
      {
        return ConvertInvalidResult<GridNeighborhood>(std::move(axisResult));
      }
      if(m_ShouldCancel)
      {
        return {GridNeighborhood{}};
      }
      axes[dimension] = axisResult.value();
    }
    if(m_Dimensions == 2)
    {
      axes[2].Coordinates[0] = 0;
      axes[2].Count = 1;
    }

    GridNeighborhood neighborhood;
    uint64 coordinates[3] = {};
    for(usize zIndex = 0; zIndex < axes[2].Count; ++zIndex)
    {
      coordinates[2] = axes[2].Coordinates[zIndex];
      for(usize yIndex = 0; yIndex < axes[1].Count; ++yIndex)
      {
        coordinates[1] = axes[1].Coordinates[yIndex];
        for(usize xIndex = 0; xIndex < axes[0].Count; ++xIndex)
        {
          if(m_ShouldCancel)
          {
            return {neighborhood};
          }
          coordinates[0] = axes[0].Coordinates[xIndex];
          Result<std::optional<uint64>> foundResult = findGrid(coordinates);
          if(foundResult.invalid())
          {
            return ConvertInvalidResult<GridNeighborhood>(std::move(foundResult));
          }
          if(foundResult.value().has_value())
          {
            if(neighborhood.Count == neighborhood.GridIds.size())
            {
              return MakeErrorResult<GridNeighborhood>(-54079, "DBSCAN neighbor enumeration exceeded its fixed 5x5x5 capacity.");
            }
            neighborhood.GridIds[neighborhood.Count++] = *foundResult.value();
          }
        }
      }
    }
    std::sort(neighborhood.GridIds.begin(), neighborhood.GridIds.begin() + static_cast<std::ptrdiff_t>(neighborhood.Count));
    return {neighborhood};
  }

  /**
   * @brief Tests point pairs from two grids in fixed tiles until an epsilon-connected pair is found.
   * @return True when the grids connect, false otherwise, or a membership-record I/O failure.
   */
  Result<bool> canMerge(uint64 leftGridId, uint64 rightGridId)
  {
    Result<DBSCANGridState> leftStateResult = readGridState(leftGridId);
    if(leftStateResult.invalid())
    {
      return ConvertInvalidResult<bool>(std::move(leftStateResult));
    }
    Result<DBSCANGridState> rightStateResult = readGridState(rightGridId);
    if(rightStateResult.invalid())
    {
      return ConvertInvalidResult<bool>(std::move(rightStateResult));
    }
    const DBSCANGridState& leftState = leftStateResult.value();
    const DBSCANGridState& rightState = rightStateResult.value();
    const uint64 pointCount = m_Membership->recordCount();
    if(leftState.FirstMember > pointCount || leftState.MemberCount > pointCount - leftState.FirstMember || rightState.FirstMember > pointCount ||
       rightState.MemberCount > pointCount - rightState.FirstMember)
    {
      return MakeErrorResult<bool>(-54080, "DBSCAN grid-state membership range exceeds the point-membership sort.");
    }

    if(m_LeftMergeBuffer == nullptr || m_RightMergeBuffer == nullptr)
    {
      return MakeErrorResult<bool>(-54071, "DBSCAN external merge-tile buffers were not initialized.");
    }
    for(uint64 leftOffset = 0; leftOffset < leftState.MemberCount; leftOffset += k_MergeTileRecords)
    {
      if(m_ShouldCancel)
      {
        return {false};
      }
      const uint64 leftCount = std::min<uint64>(k_MergeTileRecords, leftState.MemberCount - leftOffset);
      Result<uint64> leftReadResult = readMembership(leftState.FirstMember + leftOffset, leftCount, m_LeftMergeBuffer.get());
      if(leftReadResult.invalid())
      {
        return ConvertInvalidResult<bool>(std::move(leftReadResult));
      }
      for(uint64 rightOffset = 0; rightOffset < rightState.MemberCount; rightOffset += k_MergeTileRecords)
      {
        const uint64 rightCount = std::min<uint64>(k_MergeTileRecords, rightState.MemberCount - rightOffset);
        Result<uint64> rightReadResult = readMembership(rightState.FirstMember + rightOffset, rightCount, m_RightMergeBuffer.get());
        if(rightReadResult.invalid())
        {
          return ConvertInvalidResult<bool>(std::move(rightReadResult));
        }
        for(uint64 leftIndex = 0; leftIndex < leftCount; ++leftIndex)
        {
          if(m_ShouldCancel)
          {
            return {false};
          }
          for(uint64 rightIndex = 0; rightIndex < rightCount; ++rightIndex)
          {
            const float64 distance = ClusterUtilities::GetDistance(m_LeftMergeBuffer[static_cast<usize>(leftIndex)].Coordinates, 0, m_RightMergeBuffer[static_cast<usize>(rightIndex)].Coordinates, 0,
                                                                   m_Dimensions, m_InputValues.DistanceMetric);
            if(distance < m_InputValues.Epsilon)
            {
              return {true};
            }
          }
        }
      }
    }
    return {false};
  }

  /** @brief Reads one grid state through the bounded page cache. */
  Result<DBSCANGridState> readGridState(uint64 gridId)
  {
    if(m_GridStateCache == nullptr)
    {
      return MakeErrorResult<DBSCANGridState>(-54071, "DBSCAN external grid-state cache was not initialized.");
    }
    return m_GridStateCache->read(gridId, m_ShouldCancel);
  }

  /** @brief Updates one grid state through the bounded write-back page cache. */
  Result<> writeGridState(uint64 gridId, const DBSCANGridState& state)
  {
    if(m_GridStateCache == nullptr)
    {
      return MakeErrorResult(-54071, "DBSCAN external grid-state cache was not initialized.");
    }
    return m_GridStateCache->write(gridId, state, m_ShouldCancel);
  }

  /** @brief Resolves a grid's disjoint-set root with bounded path compression. */
  Result<uint64> findRoot(uint64 gridId)
  {
    uint64 current = gridId;
    for(uint64 depth = 0; depth <= m_ActiveGridCount; ++depth)
    {
      if(m_ShouldCancel)
      {
        return {current};
      }
      Result<DBSCANGridState> stateResult = readGridState(current);
      if(stateResult.invalid())
      {
        return ConvertInvalidResult<uint64>(std::move(stateResult));
      }
      const uint64 parent = stateResult.value().Parent;
      if(parent == current)
      {
        return {current};
      }
      if(parent >= m_ActiveGridCount)
      {
        return MakeErrorResult<uint64>(-54081, fmt::format("DBSCAN grid {} has out-of-range parent {}.", current, parent));
      }
      current = parent;
    }
    return MakeErrorResult<uint64>(-54082, "DBSCAN external cluster forest contains a parent cycle.");
  }

  /** @brief Uses existing root relationships to avoid an unnecessary point-distance comparison. */
  Result<bool> infer(uint64 leftGridId, uint64 rightGridId)
  {
    Result<uint64> leftRootResult = findRoot(leftGridId);
    if(leftRootResult.invalid())
    {
      return ConvertInvalidResult<bool>(std::move(leftRootResult));
    }
    if(m_ShouldCancel)
    {
      return {false};
    }
    Result<uint64> rightRootResult = findRoot(rightGridId);
    if(rightRootResult.invalid())
    {
      return ConvertInvalidResult<bool>(std::move(rightRootResult));
    }
    return {leftRootResult.value() == rightRootResult.value()};
  }

  /** @brief Assigns a grid parent while preserving the external disjoint-set invariants. */
  Result<> setParent(uint64 gridId, uint64 parent)
  {
    if(m_ShouldCancel)
    {
      return {};
    }
    Result<DBSCANGridState> stateResult = readGridState(gridId);
    if(stateResult.invalid())
    {
      return ConvertResult(std::move(stateResult));
    }
    stateResult.value().Parent = parent;
    return writeGridState(gridId, stateResult.value());
  }

  /** @brief Merges a bounded neighbor set under its lowest root for deterministic feature numbering. */
  Result<> mergeLowestRootCluster(const std::array<uint64, 126>& gridIds, usize gridCount)
  {
    if(gridCount < 2)
    {
      return {};
    }
    std::array<uint64, 126> roots = {};
    Result<uint64> lowestResult = findRoot(gridIds[0]);
    if(lowestResult.invalid())
    {
      return ConvertResult(std::move(lowestResult));
    }
    if(m_ShouldCancel)
    {
      return {};
    }
    uint64 lowestRoot = lowestResult.value();
    roots[0] = lowestRoot;
    Result<DBSCANGridState> lowestStateResult = readGridState(lowestRoot);
    if(lowestStateResult.invalid())
    {
      return ConvertResult(std::move(lowestStateResult));
    }
    for(usize index = 1; index < gridCount; ++index)
    {
      Result<uint64> rootResult = findRoot(gridIds[index]);
      if(rootResult.invalid())
      {
        return ConvertResult(std::move(rootResult));
      }
      if(m_ShouldCancel)
      {
        return {};
      }
      roots[index] = rootResult.value();
      Result<DBSCANGridState> stateResult = readGridState(rootResult.value());
      if(stateResult.invalid())
      {
        return ConvertResult(std::move(stateResult));
      }
      if(stateResult.value().ClusterId < lowestStateResult.value().ClusterId)
      {
        lowestRoot = rootResult.value();
        lowestStateResult = std::move(stateResult);
      }
    }
    for(usize index = 0; index < gridCount; ++index)
    {
      if(m_ShouldCancel)
      {
        return {};
      }
      if(roots[index] != lowestRoot)
      {
        Result<> result = setParent(roots[index], lowestRoot);
        if(result.invalid())
        {
          return result;
        }
      }
    }
    return {};
  }

  /** @brief Partitions and processes one core-order quick-sort section using record-cache swaps. */
  Result<uint64> processCoreOrderSection(uint64 begin, uint64 end)
  {
    if(m_ShouldCancel)
    {
      return {begin};
    }
    Result<DBSCANCoreOrder> pivotOrderResult = m_CoreOrderCache->read(begin, m_ShouldCancel);
    if(pivotOrderResult.invalid())
    {
      return ConvertInvalidResult<uint64>(std::move(pivotOrderResult));
    }
    Result<DBSCANGridState> pivotStateResult = m_GridStateCache->read(pivotOrderResult.value().GridId, m_ShouldCancel);
    if(pivotStateResult.invalid())
    {
      return ConvertInvalidResult<uint64>(std::move(pivotStateResult));
    }
    const uint64 threshold = pivotStateResult.value().MemberCount;
    uint64 front = begin;
    uint64 back = end;

    while(true)
    {
      if(m_ShouldCancel)
      {
        return {begin};
      }
      while(true)
      {
        if(m_ShouldCancel)
        {
          return {begin};
        }
        Result<DBSCANCoreOrder> orderResult = m_CoreOrderCache->read(front, m_ShouldCancel);
        if(orderResult.invalid())
        {
          return ConvertInvalidResult<uint64>(std::move(orderResult));
        }
        Result<DBSCANGridState> stateResult = m_GridStateCache->read(orderResult.value().GridId, m_ShouldCancel);
        if(stateResult.invalid())
        {
          return ConvertInvalidResult<uint64>(std::move(stateResult));
        }
        if(stateResult.value().MemberCount >= threshold)
        {
          break;
        }
        ++front;
      }
      while(true)
      {
        if(m_ShouldCancel)
        {
          return {begin};
        }
        Result<DBSCANCoreOrder> orderResult = m_CoreOrderCache->read(back, m_ShouldCancel);
        if(orderResult.invalid())
        {
          return ConvertInvalidResult<uint64>(std::move(orderResult));
        }
        Result<DBSCANGridState> stateResult = m_GridStateCache->read(orderResult.value().GridId, m_ShouldCancel);
        if(stateResult.invalid())
        {
          return ConvertInvalidResult<uint64>(std::move(stateResult));
        }
        if(stateResult.value().MemberCount <= threshold)
        {
          break;
        }
        --back;
      }
      if(front >= back)
      {
        return {back};
      }
      Result<DBSCANCoreOrder> frontResult = m_CoreOrderCache->read(front, m_ShouldCancel);
      if(frontResult.invalid())
      {
        return ConvertInvalidResult<uint64>(std::move(frontResult));
      }
      Result<DBSCANCoreOrder> backResult = m_CoreOrderCache->read(back, m_ShouldCancel);
      if(backResult.invalid())
      {
        return ConvertInvalidResult<uint64>(std::move(backResult));
      }
      Result<> writeResult = m_CoreOrderCache->write(front, backResult.value(), m_ShouldCancel);
      if(writeResult.invalid())
      {
        return ConvertResultTo<uint64>(std::move(writeResult), uint64{});
      }
      writeResult = m_CoreOrderCache->write(back, frontResult.value(), m_ShouldCancel);
      if(writeResult.invalid())
      {
        return ConvertResultTo<uint64>(std::move(writeResult), uint64{});
      }
      ++front;
      --back;
    }
  }

  /** @brief Sorts the external core-grid order in place without a core-count resident vector. */
  Result<> quickSortCoreOrder(uint64 begin, uint64 end)
  {
    while(begin < end)
    {
      if(m_ShouldCancel)
      {
        return {};
      }
      Result<uint64> sectionResult = processCoreOrderSection(begin, end);
      if(sectionResult.invalid())
      {
        return ConvertResult(std::move(sectionResult));
      }
      if(m_ShouldCancel)
      {
        return {};
      }
      const uint64 partition = sectionResult.value();
      if(partition < begin || partition >= end)
      {
        return MakeErrorResult(-54073, "DBSCAN external Hoare partition returned an invalid split point.");
      }
      const uint64 leftSize = partition - begin + 1;
      const uint64 rightSize = end - partition;
      if(leftSize < rightSize)
      {
        Result<> result = quickSortCoreOrder(begin, partition);
        if(result.invalid())
        {
          return result;
        }
        begin = partition + 1;
      }
      else
      {
        Result<> result = quickSortCoreOrder(partition + 1, end);
        if(result.invalid())
        {
          return result;
        }
        end = partition;
      }
    }
    return {};
  }

  /** @brief Flushes all dirty grid, core-order, and axis pages before another consumer reads them. */
  Result<> flushCaches()
  {
    if(m_GridStateCache != nullptr)
    {
      Result<> result = m_GridStateCache->flush(m_ShouldCancel);
      if(result.invalid())
      {
        return result;
      }
    }
    if(m_CoreOrderCache != nullptr)
    {
      Result<> result = m_CoreOrderCache->flush(m_ShouldCancel);
      if(result.invalid())
      {
        return result;
      }
    }
    for(usize dimension = 0; dimension < m_Dimensions; ++dimension)
    {
      if(m_AxisCaches[dimension] != nullptr)
      {
        Result<> result = m_AxisCaches[dimension]->flush(m_ShouldCancel);
        if(result.invalid())
        {
          return result;
        }
      }
    }
    return {};
  }

  /** @brief Reads a bounded run from the externally sorted point-membership stream. */
  Result<uint64> readMembership(uint64 offset, uint64 count, DBSCANPointRecord<T>* records) const
  {
    auto bytes = nonstd::span<std::byte>(reinterpret_cast<std::byte*>(records), static_cast<usize>(count) * sizeof(DBSCANPointRecord<T>));
    Result<uint64> result = m_Membership->read(offset, count, bytes, m_ShouldCancel);
    if(result.invalid())
    {
      return result;
    }
    if(result.value() != count)
    {
      return MakeErrorResult<uint64>(-54065, fmt::format("DBSCAN point-membership sort short read at record {}: requested {}, received {}.", offset, count, result.value()));
    }
    return result;
  }

  /** @brief Reduces the sorted membership stream into fixed occupied-grid records. */
  Result<> buildGridStates(DBSCANPointRecord<T>* records)
  {
    const uint64 pointCount = m_Membership->recordCount();
    if(pointCount == 0)
    {
      return MakeWarningVoidResult(-85640, "No clusters detected - Consider reducing number of required points (`Minimum Points`) or increasing acceptable distance (`Epsilon`).");
    }

    DBSCANPointRecord<T> previous = {};
    bool hasPrevious = false;
    for(uint64 offset = 0; offset < pointCount; offset += k_ExternalBatchRecords)
    {
      if(m_ShouldCancel)
      {
        return {};
      }
      const uint64 count = std::min<uint64>(k_ExternalBatchRecords, pointCount - offset);
      Result<uint64> readResult = readMembership(offset, count, records);
      if(readResult.invalid())
      {
        return ConvertResult(std::move(readResult));
      }
      for(uint64 index = 0; index < count; ++index)
      {
        const DBSCANPointRecord<T>& current = records[static_cast<usize>(index)];
        if(!hasPrevious || !SameGrid(previous, current))
        {
          if(m_ActiveGridCount == std::numeric_limits<uint64>::max())
          {
            return MakeErrorResult(-54066, "DBSCAN active-grid count exceeds uint64.");
          }
          ++m_ActiveGridCount;
        }
        previous = current;
        hasPrevious = true;
      }
    }
    if(m_ActiveGridCount > static_cast<uint64>(std::numeric_limits<int32>::max()))
    {
      return MakeErrorResult(-54066, "DBSCAN active-grid count exceeds the Int32 cluster-label range.");
    }

    TemporaryRecordStoreConfig stateConfig;
    stateConfig.recordSize = sizeof(DBSCANGridState);
    stateConfig.maxRecordsPerBatch = k_ExternalBatchRecords;
    stateConfig.initialRecordCount = m_ActiveGridCount;
    auto stateResult = DataStoreUtilities::GetIOCollection().createTemporaryRecordStore(stateConfig);
    if(stateResult.invalid())
    {
      return ConvertResult(std::move(stateResult));
    }
    m_GridStates = std::move(stateResult.value());
    if(m_GridStates == nullptr)
    {
      return MakeErrorResult(-54067, "DBSCAN temporary-record-store provider returned a null grid-state store.");
    }

    auto pointBuffer = std::make_unique<DBSCANPointRecord<T>[]>(k_ExternalBatchRecords);
    auto stateBuffer = std::make_unique<DBSCANGridState[]>(k_ExternalBatchRecords);
    uint64 stateBufferCount = 0;
    uint64 statesWritten = 0;
    DBSCANGridState currentState = {};
    bool hasState = false;
    const auto flushStates = [&]() -> Result<> {
      if(stateBufferCount == 0)
      {
        return {};
      }
      auto bytes = nonstd::span<const std::byte>(reinterpret_cast<const std::byte*>(stateBuffer.get()), static_cast<usize>(stateBufferCount) * sizeof(DBSCANGridState));
      Result<> result = m_GridStates->write(statesWritten, stateBufferCount, bytes, m_ShouldCancel);
      if(result.valid())
      {
        statesWritten += stateBufferCount;
        stateBufferCount = 0;
      }
      return result;
    };
    const auto finishState = [&]() -> Result<> {
      if(!hasState)
      {
        return {};
      }
      currentState.Parent = statesWritten + stateBufferCount;
      currentState.ClusterId = static_cast<int32>(currentState.Parent + 1);
      currentState.IsCore = currentState.MemberCount >= static_cast<uint64>(m_InputValues.MinPoints) ? uint8{1} : uint8{0};
      if(currentState.IsCore != 0)
      {
        ++m_CoreGridCount;
      }
      stateBuffer[static_cast<usize>(stateBufferCount++)] = currentState;
      hasState = false;
      return stateBufferCount == k_ExternalBatchRecords ? flushStates() : Result<>{};
    };

    for(uint64 offset = 0; offset < pointCount; offset += k_ExternalBatchRecords)
    {
      if(m_ShouldCancel)
      {
        return {};
      }
      const uint64 count = std::min<uint64>(k_ExternalBatchRecords, pointCount - offset);
      Result<uint64> readResult = readMembership(offset, count, pointBuffer.get());
      if(readResult.invalid())
      {
        return ConvertResult(std::move(readResult));
      }
      for(uint64 index = 0; index < count; ++index)
      {
        const DBSCANPointRecord<T>& record = pointBuffer[static_cast<usize>(index)];
        if(!hasState || currentState.Grid[0] != record.Grid[0] || currentState.Grid[1] != record.Grid[1] || currentState.Grid[2] != record.Grid[2])
        {
          Result<> finishStateResult = finishState();
          if(finishStateResult.invalid())
          {
            return finishStateResult;
          }
          currentState = {};
          std::copy(std::begin(record.Grid), std::end(record.Grid), std::begin(currentState.Grid));
          currentState.FirstMember = offset + index;
          hasState = true;
        }
        ++currentState.MemberCount;
      }
    }
    Result<> finishStateResult = finishState();
    if(finishStateResult.invalid())
    {
      return finishStateResult;
    }
    Result<> flushResult = flushStates();
    if(flushResult.invalid())
    {
      return flushResult;
    }
    if(statesWritten != m_ActiveGridCount)
    {
      return MakeErrorResult(-54068, fmt::format("DBSCAN built {} grid-state records after counting {} active grids.", statesWritten, m_ActiveGridCount));
    }
    return {};
  }

  /** @brief Emits one external core-order record for every grid meeting the core criterion. */
  Result<> buildCoreOrder()
  {
    if(m_CoreGridCount == 0)
    {
      return {};
    }
    TemporaryRecordStoreConfig orderConfig;
    orderConfig.recordSize = sizeof(DBSCANCoreOrder);
    orderConfig.maxRecordsPerBatch = k_ExternalBatchRecords;
    orderConfig.initialRecordCount = m_CoreGridCount;
    auto orderResult = DataStoreUtilities::GetIOCollection().createTemporaryRecordStore(orderConfig);
    if(orderResult.invalid())
    {
      return ConvertResult(std::move(orderResult));
    }
    m_CoreOrder = std::move(orderResult.value());
    if(m_CoreOrder == nullptr)
    {
      return MakeErrorResult(-54069, "DBSCAN temporary-record-store provider returned a null core-order store.");
    }

    auto stateBuffer = std::make_unique<DBSCANGridState[]>(k_ExternalBatchRecords);
    auto orderBuffer = std::make_unique<DBSCANCoreOrder[]>(k_ExternalBatchRecords);
    uint64 orderCount = 0;
    uint64 ordersWritten = 0;
    const auto flushOrders = [&]() -> Result<> {
      if(orderCount == 0)
      {
        return {};
      }
      auto bytes = nonstd::span<const std::byte>(reinterpret_cast<const std::byte*>(orderBuffer.get()), static_cast<usize>(orderCount) * sizeof(DBSCANCoreOrder));
      Result<> result = m_CoreOrder->write(ordersWritten, orderCount, bytes, m_ShouldCancel);
      if(result.valid())
      {
        ordersWritten += orderCount;
        orderCount = 0;
      }
      return result;
    };
    for(uint64 offset = 0; offset < m_ActiveGridCount; offset += k_ExternalBatchRecords)
    {
      if(m_ShouldCancel)
      {
        return {};
      }
      const uint64 count = std::min<uint64>(k_ExternalBatchRecords, m_ActiveGridCount - offset);
      auto bytes = nonstd::span<std::byte>(reinterpret_cast<std::byte*>(stateBuffer.get()), static_cast<usize>(count) * sizeof(DBSCANGridState));
      Result<uint64> readResult = m_GridStates->read(offset, count, bytes, m_ShouldCancel);
      if(readResult.invalid())
      {
        return ConvertResult(std::move(readResult));
      }
      if(readResult.value() != count)
      {
        return MakeErrorResult(-54065, fmt::format("DBSCAN grid-state store short read at record {}: requested {}, received {}.", offset, count, readResult.value()));
      }
      for(uint64 index = 0; index < count; ++index)
      {
        if(stateBuffer[static_cast<usize>(index)].IsCore == 0)
        {
          continue;
        }
        orderBuffer[static_cast<usize>(orderCount++)].GridId = offset + index;
        if(orderCount == k_ExternalBatchRecords)
        {
          Result<> flushResult = flushOrders();
          if(flushResult.invalid())
          {
            return flushResult;
          }
        }
      }
    }
    Result<> flushResult = flushOrders();
    if(flushResult.invalid())
    {
      return flushResult;
    }
    if(ordersWritten != m_CoreGridCount)
    {
      return MakeErrorResult(-54068, fmt::format("DBSCAN built {} core-order records after counting {} core grids.", ordersWritten, m_CoreGridCount));
    }
    return {};
  }

  /** @brief Builds compact per-axis coordinate indexes used by neighborhood searches. */
  Result<> buildAxisIndexes()
  {
    std::array<std::unique_ptr<IExternalSort>, 3> axisSorts;
    for(usize dimension = 0; dimension < m_Dimensions; ++dimension)
    {
      ExternalSortConfig sortConfig;
      sortConfig.recordSize = sizeof(DBSCANAxisRecord);
      sortConfig.maxRecordsPerBatch = k_ExternalBatchRecords;
      sortConfig.compare = CompareAxisRecords;
      auto sortResult = DataStoreUtilities::GetIOCollection().createExternalSort(sortConfig);
      if(sortResult.invalid())
      {
        return ConvertResult(std::move(sortResult));
      }
      axisSorts[dimension] = std::move(sortResult.value());
      if(axisSorts[dimension] == nullptr)
      {
        return MakeErrorResult(-54075, fmt::format("DBSCAN external-sort provider returned a null occupied-axis sorter for dimension {}.", dimension));
      }
    }

    auto states = std::make_unique<DBSCANGridState[]>(k_ExternalBatchRecords);
    auto sortedAxisRecords = std::make_unique<DBSCANAxisRecord[]>(k_ExternalBatchRecords);
    std::array<std::unique_ptr<DBSCANAxisRecord[]>, 3> axisRecords;
    for(usize dimension = 0; dimension < m_Dimensions; ++dimension)
    {
      axisRecords[dimension] = std::make_unique<DBSCANAxisRecord[]>(k_ExternalBatchRecords);
    }
    for(uint64 offset = 0; offset < m_ActiveGridCount; offset += k_ExternalBatchRecords)
    {
      if(m_ShouldCancel)
      {
        return {};
      }
      const uint64 count = std::min<uint64>(k_ExternalBatchRecords, m_ActiveGridCount - offset);
      auto stateBytes = nonstd::span<std::byte>(reinterpret_cast<std::byte*>(states.get()), static_cast<usize>(count) * sizeof(DBSCANGridState));
      Result<uint64> readResult = m_GridStates->read(offset, count, stateBytes, m_ShouldCancel);
      if(readResult.invalid())
      {
        return ConvertResult(std::move(readResult));
      }
      if(readResult.value() != count)
      {
        return MakeErrorResult(-54065, fmt::format("DBSCAN grid-state store short read at record {}: requested {}, received {}.", offset, count, readResult.value()));
      }
      for(usize dimension = 0; dimension < m_Dimensions; ++dimension)
      {
        for(uint64 index = 0; index < count; ++index)
        {
          axisRecords[dimension][static_cast<usize>(index)].Coordinate = states[static_cast<usize>(index)].Grid[dimension];
        }
        auto bytes = nonstd::span<const std::byte>(reinterpret_cast<const std::byte*>(axisRecords[dimension].get()), static_cast<usize>(count) * sizeof(DBSCANAxisRecord));
        Result<> appendResult = axisSorts[dimension]->append(count, bytes, m_ShouldCancel, {});
        if(appendResult.invalid() || m_ShouldCancel)
        {
          return appendResult;
        }
      }
    }

    for(usize dimension = 0; dimension < m_Dimensions; ++dimension)
    {
      Result<> finishResult = axisSorts[dimension]->finish(m_ShouldCancel, {});
      if(finishResult.invalid() || m_ShouldCancel)
      {
        return finishResult;
      }
      const uint64 sortedCount = axisSorts[dimension]->recordCount();
      if(sortedCount != m_ActiveGridCount)
      {
        return MakeErrorResult(-54076, fmt::format("DBSCAN occupied-axis sort for dimension {} contains {} records; expected {}.", dimension, sortedCount, m_ActiveGridCount));
      }

      uint64 uniqueCount = 0;
      bool hasPrevious = false;
      uint64 previous = 0;
      for(uint64 offset = 0; offset < sortedCount; offset += k_ExternalBatchRecords)
      {
        if(m_ShouldCancel)
        {
          return {};
        }
        const uint64 count = std::min<uint64>(k_ExternalBatchRecords, sortedCount - offset);
        auto bytes = nonstd::span<std::byte>(reinterpret_cast<std::byte*>(axisRecords[dimension].get()), static_cast<usize>(count) * sizeof(DBSCANAxisRecord));
        Result<uint64> readResult = axisSorts[dimension]->read(offset, count, bytes, m_ShouldCancel);
        if(readResult.invalid())
        {
          return ConvertResult(std::move(readResult));
        }
        if(readResult.value() != count)
        {
          return MakeErrorResult(-54065, fmt::format("DBSCAN occupied-axis sort short read at record {}: requested {}, received {}.", offset, count, readResult.value()));
        }
        for(uint64 index = 0; index < count; ++index)
        {
          const uint64 value = axisRecords[dimension][static_cast<usize>(index)].Coordinate;
          if(!hasPrevious || value != previous)
          {
            ++uniqueCount;
            previous = value;
            hasPrevious = true;
          }
        }
      }

      TemporaryRecordStoreConfig axisConfig;
      axisConfig.recordSize = sizeof(DBSCANAxisRecord);
      axisConfig.maxRecordsPerBatch = k_ExternalBatchRecords;
      axisConfig.initialRecordCount = uniqueCount;
      auto storeResult = DataStoreUtilities::GetIOCollection().createTemporaryRecordStore(axisConfig);
      if(storeResult.invalid())
      {
        return ConvertResult(std::move(storeResult));
      }
      m_AxisStores[dimension] = std::move(storeResult.value());
      if(m_AxisStores[dimension] == nullptr)
      {
        return MakeErrorResult(-54077, fmt::format("DBSCAN temporary-record-store provider returned a null occupied-axis store for dimension {}.", dimension));
      }

      uint64 outputCount = 0;
      uint64 outputOffset = 0;
      hasPrevious = false;
      previous = 0;
      const auto flushAxis = [&]() -> Result<> {
        if(outputCount == 0)
        {
          return {};
        }
        auto bytes = nonstd::span<const std::byte>(reinterpret_cast<const std::byte*>(axisRecords[dimension].get()), static_cast<usize>(outputCount) * sizeof(DBSCANAxisRecord));
        Result<> result = m_AxisStores[dimension]->write(outputOffset, outputCount, bytes, m_ShouldCancel);
        if(result.valid())
        {
          outputOffset += outputCount;
          outputCount = 0;
        }
        return result;
      };
      for(uint64 offset = 0; offset < sortedCount; offset += k_ExternalBatchRecords)
      {
        if(m_ShouldCancel)
        {
          return {};
        }
        const uint64 count = std::min<uint64>(k_ExternalBatchRecords, sortedCount - offset);
        auto bytes = nonstd::span<std::byte>(reinterpret_cast<std::byte*>(sortedAxisRecords.get()), static_cast<usize>(count) * sizeof(DBSCANAxisRecord));
        Result<uint64> readResult = axisSorts[dimension]->read(offset, count, bytes, m_ShouldCancel);
        if(readResult.invalid())
        {
          return ConvertResult(std::move(readResult));
        }
        if(readResult.value() != count)
        {
          return MakeErrorResult(-54065, fmt::format("DBSCAN occupied-axis sort short read at record {}: requested {}, received {}.", offset, count, readResult.value()));
        }
        for(uint64 index = 0; index < count; ++index)
        {
          const uint64 value = sortedAxisRecords[static_cast<usize>(index)].Coordinate;
          if(hasPrevious && value == previous)
          {
            continue;
          }
          axisRecords[dimension][static_cast<usize>(outputCount++)].Coordinate = value;
          previous = value;
          hasPrevious = true;
          if(outputCount == k_ExternalBatchRecords)
          {
            Result<> flushResult = flushAxis();
            if(flushResult.invalid())
            {
              return flushResult;
            }
          }
        }
      }
      Result<> flushResult = flushAxis();
      if(flushResult.invalid())
      {
        return flushResult;
      }
      if(outputOffset != uniqueCount)
      {
        return MakeErrorResult(-54078, fmt::format("DBSCAN occupied-axis store for dimension {} contains {} unique records; expected {}.", dimension, outputOffset, uniqueCount));
      }
    }
    return {};
  }

  const AbstractDataStore<T>& m_InputStore;
  const AbstractDataStore<MaskT>* m_MaskStore = nullptr;
  const DBSCANInputValues& m_InputValues;
  usize m_Dimensions = 0;
  const std::atomic_bool& m_ShouldCancel;
  float32 m_SideLength = 0.0F;
  std::array<float32, 3> m_Origin = {};
  uint64 m_ActiveGridCount = 0;
  uint64 m_CoreGridCount = 0;
  int32 m_FinalClusterCount = 0;
  std::unique_ptr<IExternalSort> m_Membership;
  std::unique_ptr<ITemporaryRecordStore> m_GridStates;
  std::unique_ptr<ITemporaryRecordStore> m_CoreOrder;
  std::array<std::unique_ptr<ITemporaryRecordStore>, 3> m_AxisStores;
  std::unique_ptr<BoundedRecordPageCache<DBSCANGridState>> m_GridStateCache;
  std::unique_ptr<BoundedRecordPageCache<DBSCANCoreOrder>> m_CoreOrderCache;
  std::array<std::unique_ptr<BoundedRecordPageCache<DBSCANAxisRecord>>, 3> m_AxisCaches;
  std::unique_ptr<DBSCANPointRecord<T>[]> m_LeftMergeBuffer;
  std::unique_ptr<DBSCANPointRecord<T>[]> m_RightMergeBuffer;
};

/**
 * @brief Initializes the potentially disk-backed label output with bounded zero-filled writes.
 *
 * Masked and noise tuples must remain zero, so clearing first lets the later
 * labeling pass write only resolved point labels without a cell-count buffer.
 */
Result<> ZeroFeatureIds(AbstractDataStore<int32>& featureIds, const std::atomic_bool& shouldCancel)
{
  auto zeros = std::make_unique<int32[]>(k_ExternalBatchRecords);
  const usize valueCount = featureIds.getSize();
  for(usize offset = 0; offset < valueCount; offset += k_ExternalBatchRecords)
  {
    if(shouldCancel)
    {
      return {};
    }
    const usize count = std::min<usize>(k_ExternalBatchRecords, valueCount - offset);
    Result<> result = featureIds.copyFromBuffer(offset, nonstd::span<const int32>(zeros.get(), count));
    if(result.invalid())
    {
      return result;
    }
  }
  return {};
}

/** @brief Runs all external GDCF phases for one input and optional-mask type combination. */
template <typename T, typename MaskT>
Result<> RunExternalTyped(const AbstractDataStore<T>& inputStore, const AbstractDataStore<MaskT>* maskStore, AbstractDataStore<int32>& featureIds, const DBSCANInputValues& inputValues,
                          const std::atomic_bool& shouldCancel, int32& finalClusterCount)
{
  ExternalGDCF<T, MaskT> algorithm(inputStore, maskStore, inputValues, inputStore.getNumberOfComponents(), shouldCancel);
  Result<> result = algorithm.initialize();
  if(result.invalid() || !result.warnings().empty() || shouldCancel)
  {
    return result;
  }
  result = algorithm.cluster();
  if(result.invalid() || !result.warnings().empty() || shouldCancel)
  {
    return result;
  }
  result = algorithm.label(featureIds);
  if(result.invalid() || shouldCancel)
  {
    return result;
  }
  finalClusterCount = algorithm.finalClusterCount();
  return result;
}

/** @brief Dispatches runtime input and mask types into the bounded external GDCF implementation. */
struct DBSCANExternalFunctor
{
  /** @brief Clears output labels, selects the real mask type, and executes external clustering. */
  template <typename T>
  Result<> operator()(const DBSCANInputValues* inputValues, const IDataArray& clusterArray, const IDataArray* maskArray, Int32Array& featureIds, const std::atomic_bool& shouldCancel,
                      int32& finalClusterCount) const
  {
    const auto& inputStore = clusterArray.template getIDataStoreRefAs<AbstractDataStore<T>>();
    auto& featureIdsStore = featureIds.getDataStoreRef();
    Result<> zeroResult = ZeroFeatureIds(featureIdsStore, shouldCancel);
    if(zeroResult.invalid() || shouldCancel)
    {
      return zeroResult;
    }
    if(!inputValues->UseMask)
    {
      return RunExternalTyped<T, uint8>(inputStore, nullptr, featureIdsStore, *inputValues, shouldCancel, finalClusterCount);
    }
    if(maskArray == nullptr)
    {
      return MakeErrorResult(-54060, fmt::format("Mask Array DataPath does not exist or is not of the correct type (Bool | UInt8) {}", inputValues->MaskArrayPath.toString()));
    }
    switch(maskArray->getDataType())
    {
    case DataType::boolean:
      return RunExternalTyped<T, bool>(inputStore, &maskArray->template getIDataStoreRefAs<AbstractDataStore<bool>>(), featureIdsStore, *inputValues, shouldCancel, finalClusterCount);
    case DataType::uint8:
      return RunExternalTyped<T, uint8>(inputStore, &maskArray->template getIDataStoreRefAs<AbstractDataStore<uint8>>(), featureIdsStore, *inputValues, shouldCancel, finalClusterCount);
    default:
      return MakeErrorResult(-54060, fmt::format("Mask Array DataPath does not exist or is not of the correct type (Bool | UInt8) {}", inputValues->MaskArrayPath.toString()));
    }
  }
};

// Implementation derived from https://yliu.site/pub/GDCF_PR2019.pdf. This
// resident fallback is used only when Scanline is forced over in-memory stores.

/** @brief Bit-packed incidence table mapping one coordinate position to active grid IDs. */
struct GridBitMap
{
  std::vector<uint8> gridTable = {};
  usize numPositions = 0;
  usize rowLength = 0;
};

/** @brief Allocates zeroed GridBitMap storage with one bit per grid/position pair. */
struct GridBitMapFactory
{
  /** @brief Creates the packed table and records its logical dimensions. */
  static GridBitMap createGridBitMap(usize numGrids, usize numPositons)
  {
    GridBitMap gridBitMap = {};

    usize bitPackSize = numGrids / 8;
    bitPackSize += static_cast<usize>((numGrids % 8 > 0));

    gridBitMap.numPositions = numPositons;
    gridBitMap.rowLength = bitPackSize;
    gridBitMap.gridTable.resize(bitPackSize * numPositons);

    return gridBitMap;
  }
};

/** @brief Common resident grid-to-point membership state for the 2D/3D fallback indices. */
class HyperGridBitMap
{
public:
  std::vector<std::vector<usize>> gridVoxels = {};

protected:
  HyperGridBitMap() = default;
};

/** @brief Three-dimensional resident spatial index used by forced Scanline verification. */
class HyperGridBitMap3D : public HyperGridBitMap
{
public:
  static constexpr float32 Dimensions = 3;

  GridBitMap xTable;
  GridBitMap yTable;
  GridBitMap zTable;

  HyperGridBitMap3D() = delete;

  /** @brief Builds grid membership and X/Y/Z incidence tables with bounded coordinate reads. */
  template <typename T>
  HyperGridBitMap3D(const std::atomic_bool& shouldCancel, const AbstractDataStore<T>& inputArray, float32 epsilon, const std::unique_ptr<MaskCompareUtilities::MaskCompare>& mask)
  : HyperGridBitMap()
  {
    const usize numTuples = inputArray.getNumberOfTuples();
    const usize numComps = inputArray.getNumberOfComponents();
    constexpr usize k_ChunkTuples = 65536;
    auto chunkBuf = std::make_unique<T[]>(k_ChunkTuples * numComps);

    // Load array bounds using chunked bulk I/O
    std::array<float32, 6> bounds = {std::numeric_limits<float32>::quiet_NaN(), std::numeric_limits<float32>::quiet_NaN(), std::numeric_limits<float32>::quiet_NaN(),
                                     std::numeric_limits<float32>::quiet_NaN(), std::numeric_limits<float32>::quiet_NaN(), std::numeric_limits<float32>::quiet_NaN()};
    for(usize startTup = 0; startTup < numTuples; startTup += k_ChunkTuples)
    {
      if(shouldCancel)
      {
        return;
      }
      const usize endTup = std::min(startTup + k_ChunkTuples, numTuples);
      const usize count = endTup - startTup;
      inputArray.copyIntoBuffer(startTup * numComps, nonstd::span<T>(chunkBuf.get(), count * numComps));

      for(usize local = 0; local < count; local++)
      {
        if(!mask->isTrue(startTup + local))
        {
          continue;
        }

        auto xVal = static_cast<float32>(chunkBuf[local * numComps + 0]);
        auto yVal = static_cast<float32>(chunkBuf[local * numComps + 1]);
        auto zVal = static_cast<float32>(chunkBuf[local * numComps + 2]);

        bounds[0] = std::isnan(bounds[0]) ? xVal : std::min(bounds[0], xVal);
        bounds[1] = std::isnan(bounds[1]) ? yVal : std::min(bounds[1], yVal);
        bounds[2] = std::isnan(bounds[2]) ? zVal : std::min(bounds[2], zVal);

        bounds[3] = std::isnan(bounds[3]) ? xVal : std::max(bounds[3], xVal);
        bounds[4] = std::isnan(bounds[4]) ? yVal : std::max(bounds[4], yVal);
        bounds[5] = std::isnan(bounds[5]) ? zVal : std::max(bounds[5], zVal);
      }
    }

    // Grid Info - DO NOT MODIFY - basis for algorithm
    float32 sideLength = epsilon / std::sqrt(Dimensions);
    std::array<float32, 3> spacing = {sideLength, sideLength, sideLength};

    float32 buffer = sideLength;
    std::array<float32, 3> origin = {};
    origin[0] = bounds[0] - buffer;
    origin[1] = bounds[1] - buffer;
    origin[2] = bounds[2] - buffer;

    std::array<usize, 3> dims = {};
    dims[0] = static_cast<usize>(((bounds[3] + buffer) - origin[0]) / spacing[0]) + 2;
    dims[1] = static_cast<usize>(((bounds[4] + buffer) - origin[1]) / spacing[1]) + 2;
    dims[2] = static_cast<usize>(((bounds[5] + buffer) - origin[2]) / spacing[2]) + 2;

    // Fill the BitMap
    {
      std::vector<std::array<usize, 3>> positions = {};
      // Build a set of non-empty grids and temporarily store their positions
      {
        std::vector<bool> grids(std::accumulate(dims.cbegin(), dims.cend(), static_cast<usize>(1), std::multiplies<>()), false);
        // Find num grid cells - chunked bulk I/O pass
        for(usize startTup = 0; startTup < numTuples; startTup += k_ChunkTuples)
        {
          if(shouldCancel)
          {
            return;
          }
          const usize endTup = std::min(startTup + k_ChunkTuples, numTuples);
          const usize count = endTup - startTup;
          inputArray.copyIntoBuffer(startTup * numComps, nonstd::span<T>(chunkBuf.get(), count * numComps));

          for(usize local = 0; local < count; local++)
          {
            const usize tup = startTup + local;
            if(!mask->isTrue(tup))
            {
              continue;
            }

            usize xPos = std::floor((static_cast<float32>(chunkBuf[local * numComps + 0]) - origin[0]) / spacing[0]);
            usize yPos = std::floor((static_cast<float32>(chunkBuf[local * numComps + 1]) - origin[1]) / spacing[1]);
            usize zPos = std::floor((static_cast<float32>(chunkBuf[local * numComps + 2]) - origin[2]) / spacing[2]);

            usize bin = (zPos * dims[1] * dims[0]) + (yPos * dims[0]) + xPos;
            grids[bin] = true;
          }
        }
        usize zSize = dims[1] * dims[0];
        usize ySize = dims[0];
        usize activeGridCount = 0;
        std::vector<usize> gridMap(grids.size());
        for(usize i = 0; i < grids.size(); i++)
        {
          if(grids[i])
          {
            gridMap[i] = activeGridCount;
            activeGridCount++;

            std::array<usize, 3> position = {};
            position[2] = i / zSize;
            usize zRemdr = i % zSize;
            position[1] = zRemdr / ySize;
            position[0] = zRemdr % ySize;
            positions.push_back(position);
          }
        }

        gridVoxels = std::vector<std::vector<usize>>(activeGridCount, std::vector<usize>(0));
        // Fill grid cells - chunked bulk I/O pass
        for(usize startTup = 0; startTup < numTuples; startTup += k_ChunkTuples)
        {
          if(shouldCancel)
          {
            return;
          }
          const usize endTup = std::min(startTup + k_ChunkTuples, numTuples);
          const usize count = endTup - startTup;
          inputArray.copyIntoBuffer(startTup * numComps, nonstd::span<T>(chunkBuf.get(), count * numComps));

          for(usize local = 0; local < count; local++)
          {
            const usize tup = startTup + local;
            if(!mask->isTrue(tup))
            {
              continue;
            }
            usize xPos = std::floor((static_cast<float32>(chunkBuf[local * numComps + 0]) - origin[0]) / spacing[0]);
            usize yPos = std::floor((static_cast<float32>(chunkBuf[local * numComps + 1]) - origin[1]) / spacing[1]);
            usize zPos = std::floor((static_cast<float32>(chunkBuf[local * numComps + 2]) - origin[2]) / spacing[2]);

            usize bin = (zPos * dims[1] * dims[0]) + (yPos * dims[0]) + xPos;
            gridVoxels[gridMap[bin]].push_back(tup);
          }
        }
      } // End of filling non-empty grids and positions vector

      // Pack down memory further
      for(auto& grid : gridVoxels)
      {
        grid.shrink_to_fit();
      }

      std::set<usize> xSet = {};
      std::set<usize> ySet = {};
      std::set<usize> zSet = {};

      for(const auto& position : positions)
      {
        xSet.insert(position[0]);
        ySet.insert(position[1]);
        zSet.insert(position[2]);
      }

      if(shouldCancel)
      {
        return;
      }

      xTable = GridBitMapFactory::createGridBitMap(gridVoxels.size(), xSet.size());
      yTable = GridBitMapFactory::createGridBitMap(gridVoxels.size(), ySet.size());
      zTable = GridBitMapFactory::createGridBitMap(gridVoxels.size(), zSet.size());

      if(shouldCancel)
      {
        return;
      }

      for(usize gridId = 0; gridId < positions.size(); gridId++)
      {
        usize relativeGridBytePos = gridId / 8;
        uint8 bitGridOffset = gridId % 8;

        usize xPos = std::distance(xSet.begin(), xSet.find(positions[gridId][0])) * xTable.rowLength;
        usize yPos = std::distance(ySet.begin(), ySet.find(positions[gridId][1])) * yTable.rowLength;
        usize zPos = std::distance(zSet.begin(), zSet.find(positions[gridId][2])) * zTable.rowLength;

        usize xBytePos = xPos + relativeGridBytePos;
        uint8 xMask = 1;
        xMask <<= bitGridOffset;
        xTable.gridTable[xBytePos] = xMask | xTable.gridTable[xBytePos];

        usize yBytePos = yPos + relativeGridBytePos;
        uint8 yMask = 1;
        yMask <<= bitGridOffset;
        yTable.gridTable[yBytePos] = yMask | yTable.gridTable[yBytePos];

        usize zBytePos = zPos + relativeGridBytePos;
        uint8 zMask = 1;
        zMask <<= bitGridOffset;
        zTable.gridTable[zBytePos] = zMask | zTable.gridTable[zBytePos];
      }
    }
  }
};

/** @brief Two-dimensional resident counterpart of HyperGridBitMap3D. */
class HyperGridBitMap2D : public HyperGridBitMap
{
public:
  static constexpr float32 Dimensions = 2;

  GridBitMap xTable;
  GridBitMap yTable;

  HyperGridBitMap2D() = delete;

  /** @brief Builds grid membership and X/Y incidence tables with bounded coordinate reads. */
  template <typename T>
  HyperGridBitMap2D(const std::atomic_bool& shouldCancel, const AbstractDataStore<T>& inputArray, float32 epsilon, const std::unique_ptr<MaskCompareUtilities::MaskCompare>& mask)
  : HyperGridBitMap()
  {
    const usize numTuples = inputArray.getNumberOfTuples();
    const usize numComps = inputArray.getNumberOfComponents();
    constexpr usize k_ChunkTuples = 65536;
    auto chunkBuf = std::make_unique<T[]>(k_ChunkTuples * numComps);

    // Load array bounds using chunked bulk I/O
    std::array<float32, 4> bounds = {std::numeric_limits<float32>::quiet_NaN(), std::numeric_limits<float32>::quiet_NaN(), std::numeric_limits<float32>::quiet_NaN(),
                                     std::numeric_limits<float32>::quiet_NaN()};
    for(usize startTup = 0; startTup < numTuples; startTup += k_ChunkTuples)
    {
      if(shouldCancel)
      {
        return;
      }
      const usize endTup = std::min(startTup + k_ChunkTuples, numTuples);
      const usize count = endTup - startTup;
      inputArray.copyIntoBuffer(startTup * numComps, nonstd::span<T>(chunkBuf.get(), count * numComps));

      for(usize local = 0; local < count; local++)
      {
        if(!mask->isTrue(startTup + local))
        {
          continue;
        }

        auto xVal = static_cast<float32>(chunkBuf[local * numComps + 0]);
        auto yVal = static_cast<float32>(chunkBuf[local * numComps + 1]);

        bounds[0] = std::isnan(bounds[0]) ? xVal : std::min(bounds[0], xVal);
        bounds[1] = std::isnan(bounds[1]) ? yVal : std::min(bounds[1], yVal);

        bounds[2] = std::isnan(bounds[2]) ? xVal : std::max(bounds[2], xVal);
        bounds[3] = std::isnan(bounds[3]) ? yVal : std::max(bounds[3], yVal);
      }
    }

    // Grid Info - DO NOT MODIFY - basis for algorithm
    float32 sideLength = epsilon / std::sqrt(Dimensions);
    std::array<float32, 2> spacing = {sideLength, sideLength};

    float32 buffer = sideLength;
    std::array<float32, 2> origin = {};
    origin[0] = bounds[0] - buffer;
    origin[1] = bounds[1] - buffer;

    std::array<usize, 2> dims = {};
    dims[0] = static_cast<usize>(((bounds[2] + buffer) - origin[0]) / spacing[0]) + 2;
    dims[1] = static_cast<usize>(((bounds[3] + buffer) - origin[1]) / spacing[1]) + 2;

    // Fill the BitMap
    {
      std::vector<std::array<usize, 2>> positions = {};
      // Build a set of non-empty grids and temporarily store their positions
      {
        std::vector<bool> grids(std::accumulate(dims.cbegin(), dims.cend(), static_cast<usize>(1), std::multiplies<>()), false);
        // Find num grid cells - chunked bulk I/O pass
        for(usize startTup = 0; startTup < numTuples; startTup += k_ChunkTuples)
        {
          if(shouldCancel)
          {
            return;
          }
          const usize endTup = std::min(startTup + k_ChunkTuples, numTuples);
          const usize count = endTup - startTup;
          inputArray.copyIntoBuffer(startTup * numComps, nonstd::span<T>(chunkBuf.get(), count * numComps));

          for(usize local = 0; local < count; local++)
          {
            const usize tup = startTup + local;
            if(!mask->isTrue(tup))
            {
              continue;
            }

            usize xPos = std::floor((static_cast<float32>(chunkBuf[local * numComps + 0]) - origin[0]) / spacing[0]);
            usize yPos = std::floor((static_cast<float32>(chunkBuf[local * numComps + 1]) - origin[1]) / spacing[1]);

            usize bin = (yPos * dims[0]) + xPos;
            grids[bin] = true;
          }
        }

        usize ySize = dims[0];
        usize activeGridCount = 0;
        std::vector<usize> gridMap(grids.size());
        for(usize i = 0; i < grids.size(); i++)
        {
          if(grids[i])
          {
            gridMap[i] = activeGridCount;
            activeGridCount++;

            std::array<usize, 2> position = {};
            position[1] = i / ySize;
            position[0] = i % ySize;
            positions.push_back(position);
          }
        }

        gridVoxels = std::vector<std::vector<usize>>(activeGridCount, std::vector<usize>(0));
        // Fill grid cells - chunked bulk I/O pass
        for(usize startTup = 0; startTup < numTuples; startTup += k_ChunkTuples)
        {
          if(shouldCancel)
          {
            return;
          }
          const usize endTup = std::min(startTup + k_ChunkTuples, numTuples);
          const usize count = endTup - startTup;
          inputArray.copyIntoBuffer(startTup * numComps, nonstd::span<T>(chunkBuf.get(), count * numComps));

          for(usize local = 0; local < count; local++)
          {
            const usize tup = startTup + local;
            if(!mask->isTrue(tup))
            {
              continue;
            }
            usize xPos = std::floor((static_cast<float32>(chunkBuf[local * numComps + 0]) - origin[0]) / spacing[0]);
            usize yPos = std::floor((static_cast<float32>(chunkBuf[local * numComps + 1]) - origin[1]) / spacing[1]);

            usize bin = (yPos * dims[0]) + xPos;
            gridVoxels[gridMap[bin]].push_back(tup);
          }
        }
      } // End of filling non-empty grids and positions vector

      // Pack down memory further
      for(auto& grid : gridVoxels)
      {
        grid.shrink_to_fit();
      }

      std::set<usize> xSet = {};
      std::set<usize> ySet = {};

      for(const auto& position : positions)
      {
        xSet.insert(position[0]);
        ySet.insert(position[1]);
      }

      if(shouldCancel)
      {
        return;
      }

      xTable = GridBitMapFactory::createGridBitMap(gridVoxels.size(), xSet.size());
      yTable = GridBitMapFactory::createGridBitMap(gridVoxels.size(), ySet.size());

      if(shouldCancel)
      {
        return;
      }

      for(usize gridId = 0; gridId < positions.size(); gridId++)
      {
        usize relativeGridBytePos = gridId / 8;
        uint8 bitGridOffset = gridId % 8;

        usize xPos = std::distance(xSet.begin(), xSet.find(positions[gridId][0])) * xTable.rowLength;
        usize yPos = std::distance(ySet.begin(), ySet.find(positions[gridId][1])) * yTable.rowLength;

        usize xBytePos = xPos + relativeGridBytePos;
        uint8 xMask = 1;
        xMask <<= bitGridOffset;
        xTable.gridTable[xBytePos] = xMask | xTable.gridTable[xBytePos];

        usize yBytePos = yPos + relativeGridBytePos;
        uint8 yMask = 1;
        yMask <<= bitGridOffset;
        yTable.gridTable[yBytePos] = yMask | yTable.gridTable[yBytePos];
      }
    }
  }
};

/** @brief Intersects @p outputGridMask with grids present near one axis position. */
void SearchTablePositions(std::vector<uint8>& outputGridMask, usize searchSpace, usize targetPosition, const GridBitMap& selectedTable)
{
  std::vector<uint8> tempGridMask(selectedTable.rowLength, 0);

  usize xStart = (targetPosition < searchSpace) ? 0 : targetPosition - searchSpace;
  usize xEnd = (targetPosition + searchSpace < selectedTable.numPositions) ? targetPosition + searchSpace + 1 : selectedTable.numPositions;

  for(usize pos = xStart; pos < xEnd; pos++)
  {
    for(usize i = 0; i < selectedTable.rowLength; i++)
    {
      tempGridMask[i] = tempGridMask[i] | selectedTable.gridTable[(pos * selectedTable.rowLength) + i];
    }
  }

  for(usize i = 0; i < selectedTable.rowLength; i++)
  {
    outputGridMask[i] = tempGridMask[i] & outputGridMask[i];
  }
}

template <class HGBMT>
concept IsHGBP = std::is_base_of_v<HyperGridBitMap, HGBMT>;

/** @brief Returns occupied grids within the DBSCAN search neighborhood of one grid. */
template <IsHGBP HGBPT>
std::vector<usize> NeighborGridQuery(usize targetGridId, const HGBPT& hyperGridBitMap)
{
  usize searchSpace = std::ceil(std::sqrt(HGBPT::Dimensions));

  std::vector<usize> neighborGridIds = {};

  std::vector<uint8> finalGridMask(hyperGridBitMap.xTable.rowLength, std::numeric_limits<uint8>::max());

  usize relativeGridBytePos = targetGridId / 8;
  uint8 bitGridOffset = targetGridId % 8;

  usize xPos = 0;
  for(usize i = 0; i < hyperGridBitMap.xTable.numPositions; i++)
  {
    usize gridPos = (i * hyperGridBitMap.xTable.rowLength) + relativeGridBytePos;
    uint8 mask = 1;
    mask <<= bitGridOffset;
    uint8 result = hyperGridBitMap.xTable.gridTable[gridPos] & mask;
    if(result > 0)
    {
      xPos = i;
      break;
    }
  }
  SearchTablePositions(finalGridMask, searchSpace, xPos, hyperGridBitMap.xTable);

  usize yPos = 0;
  for(usize i = 0; i < hyperGridBitMap.yTable.numPositions; i++)
  {
    usize gridPos = (i * hyperGridBitMap.yTable.rowLength) + relativeGridBytePos;
    uint8 mask = 1;
    mask <<= bitGridOffset;
    uint8 result = hyperGridBitMap.yTable.gridTable[gridPos] & mask;
    if(result > 0)
    {
      yPos = i;
      break;
    }
  }
  SearchTablePositions(finalGridMask, searchSpace, yPos, hyperGridBitMap.yTable);

  if constexpr(HGBPT::Dimensions == 3)
  {
    usize zPos = 0;
    for(usize i = 0; i < hyperGridBitMap.zTable.numPositions; i++)
    {
      usize gridPos = (i * hyperGridBitMap.zTable.rowLength) + relativeGridBytePos;
      uint8 mask = 1;
      mask <<= bitGridOffset;
      uint8 result = hyperGridBitMap.zTable.gridTable[gridPos] & mask;
      if(result > 0)
      {
        zPos = i;
        break;
      }
    }
    SearchTablePositions(finalGridMask, searchSpace, zPos, hyperGridBitMap.zTable);
  }

  for(usize i = 0; i < finalGridMask.size(); i++)
  {
    if(finalGridMask[i] > 0)
    {
      for(uint8 bit = 0; bit < 8; bit++)
      {
        if((finalGridMask[i] & (1 << bit)) != 0)
        {
          neighborGridIds.push_back((i * 8) + bit);
        }
      }
    }
  }

  return neighborGridIds;
}

/** @brief Union/forest node storing one resident grid's parent and assigned cluster. */
struct ClusterNode
{
  int32 clusterId = 0;
  usize parent = 0;
};

/** @brief Resident union forest used to merge density-connected fallback grids. */
struct ClusterForest
{
  std::vector<ClusterNode> clusterForestNodes = {};

  /** @brief Creates one singleton cluster node per occupied grid. */
  void initialize(usize numGrids)
  {
    clusterForestNodes.resize(numGrids);

    for(usize i = 0; i < clusterForestNodes.size(); i++)
    {
      clusterForestNodes[i].parent = i;
      clusterForestNodes[i].clusterId = static_cast<int32>(i + 1);
    }
  }

  /** @brief Finds the canonical cluster root. */
  usize findClusterRoot(usize gridId)
  {
    if(clusterForestNodes[gridId].parent == gridId)
    {
      return gridId;
    }

    return findClusterRoot(clusterForestNodes[gridId].parent);
  }

  /** @brief Returns whether two grids already resolve to the same cluster. */
  bool infer(usize pGridId, usize qGridId)
  {
    return findClusterRoot(pGridId) == findClusterRoot(qGridId);
  }

  /** @brief Merges all supplied grids under the representative with the lowest cluster ID. */
  void mergeLRC(const std::vector<usize>& gridIds)
  {
    if(gridIds.size() < 2)
    {
      return;
    }

    std::vector<usize> rootClusterIdx = {};

    usize lowestClusterIdx = findClusterRoot(gridIds[0]);
    rootClusterIdx.push_back(lowestClusterIdx);
    for(usize i = 1; i < gridIds.size(); i++)
    {
      usize clusterIndex = findClusterRoot(gridIds[i]);
      rootClusterIdx.push_back(clusterIndex);
      if(clusterForestNodes[clusterIndex].clusterId < clusterForestNodes[lowestClusterIdx].clusterId)
      {
        lowestClusterIdx = clusterIndex;
      }
    }

    for(const usize clusterIdx : rootClusterIdx)
    {
      if(lowestClusterIdx != clusterIdx)
      {
        clusterForestNodes[clusterIdx].parent = clusterForestNodes[lowestClusterIdx].parent;
      }
    }
  }
};

/**
 * @brief Resident fallback for forced Scanline execution on in-memory stores.
 */
template <IsHGBP HGBPT, typename T>
class GDCF
{
public:
  GDCF() = delete;
  /** @brief Builds the selected resident spatial index and borrows clustering settings. */
  GDCF(const std::atomic_bool& shouldCancel, const AbstractDataStore<T>& inputArray, float32 epsilon, const std::unique_ptr<MaskCompareUtilities::MaskCompare>& mask,
       ClusterUtilities::DistanceMetric distMetric)
  : hyperGridBitMap(HGBPT(shouldCancel, inputArray, epsilon, mask))
  , m_Epsilon(epsilon)
  , m_InputDataStore(inputArray)
  , m_DistMetric(distMetric)
  , m_ShouldCancel(shouldCancel)
  {
  }

  /** @brief Forms core clusters, merges density-connected grids, and assigns border grids. */
  Result<> cluster(usize minPoints, DBSCAN::ParseOrder parseOrder, std::mt19937_64::result_type seed = std::mt19937_64::default_seed)
  {
    std::vector<usize> coreGridIds = {};
    for(usize i = 0; i < hyperGridBitMap.gridVoxels.size(); i++)
    {
      if(hyperGridBitMap.gridVoxels[i].size() >= minPoints)
      {
        coreGridIds.push_back(i);
      }
    }
    if(coreGridIds.empty())
    {
      return MakeWarningVoidResult(-85640, "No clusters detected - Consider reducing number of required points (`Minimum Points`) or increasing acceptable distance (`Epsilon`).");
    }

    if(m_ShouldCancel)
    {
      return {};
    }

    switch(parseOrder)
    {
    case DBSCAN::ParseOrder::LowDensityFirst: {
      QuickSortGrids(coreGridIds, 0, coreGridIds.size() - 1);
      break;
    }
    case DBSCAN::ParseOrder::Random: {
      std::mt19937_64 gen(seed);
      std::uniform_real_distribution<float64> dist(0, 1);

      auto maxIdx = static_cast<float64>(coreGridIds.size() - 1);

      for(usize i = 1; i < coreGridIds.size(); i++)
      {
        auto r = static_cast<usize>(std::floor(dist(gen) * maxIdx));
        std::swap(coreGridIds[i], coreGridIds[r]);
      }

      break;
    }
    case DBSCAN::SeededRandom: {
      std::mt19937_64 gen(seed);
      std::uniform_real_distribution<float64> dist(0, 1);

      auto maxIdx = static_cast<float64>(coreGridIds.size() - 1);

      for(usize i = 1; i < coreGridIds.size(); i++)
      {
        auto r = static_cast<usize>(std::floor(dist(gen) * maxIdx));
        std::swap(coreGridIds[i], coreGridIds[r]);
      }

      break;
    }
    }

    if(m_ShouldCancel)
    {
      return {};
    }

    clusterForest.initialize(hyperGridBitMap.gridVoxels.size());
    for(usize i = 0; i < coreGridIds.size(); i++)
    {
      if(m_ShouldCancel)
      {
        return {};
      }

      std::vector<usize> neighborGrids = NeighborGridQuery(coreGridIds[i], hyperGridBitMap);

      std::vector<usize> cluster = {};
      cluster.push_back(coreGridIds[i]);
      for(const usize gridId : neighborGrids)
      {
        if(clusterForest.infer(coreGridIds[i], gridId))
        {
          continue;
        }

        if(canMerge(coreGridIds[i], gridId))
        {
          if(hyperGridBitMap.gridVoxels[gridId].size() < minPoints && clusterForest.clusterForestNodes[gridId].parent == gridId)
          {
            clusterForest.clusterForestNodes[gridId].parent = coreGridIds[i];
          }
          else
          {
            cluster.push_back(gridId);
          }
        }
      }

      clusterForest.mergeLRC(cluster);
    }

    // Now determine if non-core grids are close enough to a cluster to be border else noise
    usize operations = 0;
    do
    {
      operations = 0;
      for(usize i = 0; i < hyperGridBitMap.gridVoxels.size(); i++)
      {
        if(m_ShouldCancel)
        {
          return {};
        }

        if(hyperGridBitMap.gridVoxels[i].size() < minPoints)
        {
          std::vector<usize> neighborGrids = NeighborGridQuery(i, hyperGridBitMap);

          for(const usize gridId : neighborGrids)
          {
            if(clusterForest.infer(i, gridId))
            {
              continue;
            }

            if(canMerge(i, gridId))
            {
              usize activeParent = clusterForest.findClusterRoot(i);
              usize neighborGridParent = clusterForest.findClusterRoot(gridId);
              if(activeParent == i)
              {
                if(hyperGridBitMap.gridVoxels[gridId].size() < minPoints && neighborGridParent == gridId)
                {
                  continue;
                }
                clusterForest.clusterForestNodes[i].parent = neighborGridParent;
              }
              else
              {
                if(hyperGridBitMap.gridVoxels[gridId].size() < minPoints && neighborGridParent == gridId)
                {
                  clusterForest.clusterForestNodes[gridId].parent = activeParent;
                }
                else
                {
                  if(clusterForest.clusterForestNodes[activeParent].clusterId < clusterForest.clusterForestNodes[neighborGridParent].clusterId)
                  {
                    clusterForest.clusterForestNodes[neighborGridParent].parent = activeParent;
                  }
                  else
                  {
                    clusterForest.clusterForestNodes[activeParent].parent = neighborGridParent;
                  }
                }
              }
              operations++;
            }
          }
        }
      }
    } while(operations > 0);

    std::vector<usize> clusters = {};
    for(usize i = 0; i < clusterForest.clusterForestNodes.size(); i++)
    {
      if(clusterForest.clusterForestNodes[i].parent == i)
      {
        if(hyperGridBitMap.gridVoxels[i].size() >= minPoints)
        {
          clusters.push_back(i);
        }
        else
        {
          clusterForest.clusterForestNodes[i].clusterId = 0;
        }
      }
    }

    for(usize i = 0; i < clusters.size(); i++)
    {
      clusterForest.clusterForestNodes[clusters[i]].clusterId = static_cast<int32>(i + 1);
    }

    return {};
  }

  /** @brief Writes each point's resolved cluster ID, leaving unassigned points at zero. */
  Result<> label(AbstractDataStore<int32>& fIdsDataStore)
  {
    if(clusterForest.clusterForestNodes.empty())
    {
      return MakeWarningVoidResult(-85640, "No clusters detected - Consider reducing number of required points (`Minimum Points`) or increasing acceptable distance (`Epsilon`).");
    }

    fIdsDataStore.fill(0);
    for(usize gridIdx = 0; gridIdx < hyperGridBitMap.gridVoxels.size(); gridIdx++)
    {
      if(m_ShouldCancel)
      {
        return {};
      }

      int32 featureId = clusterForest.clusterForestNodes[clusterForest.findClusterRoot(gridIdx)].clusterId;
      for(usize pointIdx : hyperGridBitMap.gridVoxels[gridIdx])
      {
        fIdsDataStore.setValue(pointIdx, featureId);
      }
    }

    return {};
  }

private:
  HGBPT hyperGridBitMap;

  ClusterForest clusterForest = {};

  float32 m_Epsilon = 0.0f;
  const AbstractDataStore<T>& m_InputDataStore;
  ClusterUtilities::DistanceMetric m_DistMetric;
  const std::atomic_bool& m_ShouldCancel;

  /**
   * @brief Reads coordinate data for all points in a resident-fallback grid cell.
   *
   * This method gathers each member once through the storage-neutral bulk API
   * and assembles a contiguous buffer that the pairwise loop can reuse.
   *
   * Memory cost is O(gridCellSize * dims) per call, which is typically very small
   * (grid cells contain a handful of points in practice). The returned buffer is
   * used for all pairwise distance computations in canMerge, so the data is read
   * once and reused for every comparison.
   *
   * @param gridId Index of the grid cell whose member coordinates to read
   * @return Contiguous float32 buffer with coordinates for all grid cell members
   */
  std::vector<float32> readGridCellCoords(usize gridId) const
  {
    const auto& indices = hyperGridBitMap.gridVoxels[gridId];
    const usize dims = static_cast<usize>(HGBPT::Dimensions);
    std::vector<float32> coords(indices.size() * dims);
    auto tupleBuf = std::make_unique<T[]>(dims);
    for(usize i = 0; i < indices.size(); i++)
    {
      m_InputDataStore.copyIntoBuffer(indices[i] * dims, nonstd::span<T>(tupleBuf.get(), dims));
      for(usize d = 0; d < dims; d++)
      {
        coords[i * dims + d] = static_cast<float32>(tupleBuf[d]);
      }
    }
    return coords;
  }

  /** @brief Partitions one density-sort range using Hoare's method. */
  usize ProcessSection(std::vector<usize>& sorted, usize begin, usize end) const
  {
    const usize threshold = hyperGridBitMap.gridVoxels[sorted[begin]].size();

    usize front = begin;
    usize back = end;

    while(true)
    {
      while(hyperGridBitMap.gridVoxels[sorted[front]].size() < threshold)
      {
        front++;
      }

      while(hyperGridBitMap.gridVoxels[sorted[back]].size() > threshold)
      {
        back--;
      }

      if(front >= back)
      {
        return back;
      }

      std::swap(sorted[front], sorted[back]);
      front++;
      back--;
    }
  }

  /** @brief Sorts core grid IDs by resident population for LowDensityFirst traversal. */
  void QuickSortGrids(std::vector<usize>& sorted, usize begin, usize end) const
  {
    if(begin >= end)
    {
      return;
    }

    usize next = ProcessSection(sorted, begin, end);

    QuickSortGrids(sorted, begin, next);
    QuickSortGrids(sorted, next + 1, end);
  }

  /** @brief Tests all locally buffered point pairs until an epsilon-connected pair is found. */
  bool canMerge(usize pGridId, usize qGridId)
  {
    const usize dims = static_cast<usize>(HGBPT::Dimensions);
    auto pCoords = readGridCellCoords(pGridId);
    auto qCoords = readGridCellCoords(qGridId);

    for(usize p = 0; p < hyperGridBitMap.gridVoxels[pGridId].size(); p++)
    {
      for(usize q = 0; q < hyperGridBitMap.gridVoxels[qGridId].size(); q++)
      {
        float64 dist = ClusterUtilities::GetDistance(pCoords, dims * p, qCoords, dims * q, dims, m_DistMetric);
        if(dist < m_Epsilon)
        {
          return true;
        }
      }
    }
    return false;
  }
};

/** @brief Constructs one dimensional fallback specialization and runs clustering plus labeling. */
template <class AlgorithmT, typename T>
Result<> RunAlgorithm(const DBSCANInputValues* inputValues, const AbstractDataStore<T>& inputArray, const std::unique_ptr<MaskCompareUtilities::MaskCompare>& mask, Int32Array& featureIds,
                      const std::atomic_bool& shouldCancel)
{
  AlgorithmT algorithm = AlgorithmT(shouldCancel, inputArray, inputValues->Epsilon, mask, inputValues->DistanceMetric);

  if(shouldCancel)
  {
    return {};
  }

  Result<> result = algorithm.cluster(inputValues->MinPoints, static_cast<DBSCAN::ParseOrder>(inputValues->ParseOrder), inputValues->Seed);
  if(result.invalid() || !result.warnings().empty())
  {
    return result;
  }

  if(shouldCancel)
  {
    return {};
  }

  return algorithm.label(featureIds.getDataStoreRef());
}

/**
 * @brief Dispatches the resident forced-Scanline fallback by input type and dimensionality.
 *
 * This fallback is used only when tests force the Scanline class while all stores
 * are resident; genuine OOC execution always uses DBSCANExternalFunctor.
 */
struct DBSCANScanlineFunctor
{
  /** @brief Selects the two- or three-dimensional resident GDCF specialization. */
  template <typename T>
  Result<> operator()(const DBSCANInputValues* inputValues, const IDataArray& clusterArray, const std::unique_ptr<MaskCompareUtilities::MaskCompare>& mask, Int32Array& featureIds,
                      const std::atomic_bool& shouldCancel)
  {
    const auto& inputArray = dynamic_cast<const DataArray<T>&>(clusterArray).getDataStoreRef();
    if(inputArray.getNumberOfComponents() == 2)
    {
      return RunAlgorithm<GDCF<HyperGridBitMap2D, T>, T>(inputValues, inputArray, mask, featureIds, shouldCancel);
    }
    else if(inputArray.getNumberOfComponents() == 3)
    {
      return RunAlgorithm<GDCF<HyperGridBitMap3D, T>, T>(inputValues, inputArray, mask, featureIds, shouldCancel);
    }
    else
    {
      return MakeErrorResult(-54060, "Input components invalid. Only 2 or 3 accepted.");
    }
  }
};
} // namespace

// -----------------------------------------------------------------------------
DBSCANScanline::DBSCANScanline(DataStructure& dataStructure, const IFilter::MessageHandler&, const std::atomic_bool& shouldCancel, const DBSCANInputValues* inputValues)
: m_DataStructure(dataStructure)
, m_InputValues(inputValues)
, m_ShouldCancel(shouldCancel)
{
}

// -----------------------------------------------------------------------------
DBSCANScanline::~DBSCANScanline() noexcept = default;

// -----------------------------------------------------------------------------
/**
 * @brief OOC DBSCAN execution using bounded external records and bulk I/O.
 */
Result<> DBSCANScanline::operator()()
{
  auto& clusteringArray = m_DataStructure.getDataRefAs<IDataArray>(m_InputValues->ClusteringArrayPath);
  auto& featureIds = m_DataStructure.getDataRefAs<Int32Array>(m_InputValues->FeatureIdsArrayPath);
  const IDataArray* maskArray = m_InputValues->UseMask ? m_DataStructure.getDataAs<IDataArray>(m_InputValues->MaskArrayPath) : nullptr;
  const bool actualOutOfCore = IsOutOfCore(clusteringArray) || IsOutOfCore(featureIds) || (maskArray != nullptr && IsOutOfCore(*maskArray));

  Result<> result;
  int32 maxCluster = 0;
  if(actualOutOfCore)
  {
    result = ExecuteDataFunction(DBSCANExternalFunctor{}, clusteringArray.getDataType(), m_InputValues, clusteringArray, maskArray, featureIds, m_ShouldCancel, maxCluster);
    if(result.invalid() || m_ShouldCancel)
    {
      return result;
    }
  }
  else
  {
    std::unique_ptr<MaskCompareUtilities::MaskCompare> maskCompare;
    if(m_InputValues->UseMask)
    {
      try
      {
        maskCompare = MaskCompareUtilities::InstantiateMaskCompare(m_DataStructure, m_InputValues->MaskArrayPath);
      } catch(const std::out_of_range& exception)
      {
        std::string message = fmt::format("Mask Array DataPath does not exist or is not of the correct type (Bool | UInt8) {}", m_InputValues->MaskArrayPath.toString());
        return MakeErrorResult(-54060, message);
      }
    }
    else
    {
      maskCompare = std::make_unique<AllTrueMaskCompare>();
    }

    result = ExecuteDataFunction(DBSCANScanlineFunctor{}, clusteringArray.getDataType(), m_InputValues, clusteringArray, maskCompare, featureIds, m_ShouldCancel);
    if(result.invalid())
    {
      return result;
    }

    if(m_ShouldCancel)
    {
      return {};
    }

    auto& featureIdsDataStore = featureIds.getDataStoreRef();
    const usize totalSize = featureIdsDataStore.getSize();
    constexpr usize k_ChunkSize = 1000000;
    auto maxBuffer = std::make_unique<int32[]>(std::min<usize>(totalSize, k_ChunkSize));
    for(usize start = 0; start < totalSize; start += k_ChunkSize)
    {
      const usize count = std::min(k_ChunkSize, totalSize - start);
      Result<> readResult = featureIdsDataStore.copyIntoBuffer(start, nonstd::span<int32>(maxBuffer.get(), count));
      if(readResult.invalid())
      {
        return readResult;
      }
      for(usize index = 0; index < count; ++index)
      {
        maxCluster = std::max(maxCluster, maxBuffer[index]);
      }
    }
  }

  auto* featureAttributeMatrix = m_DataStructure.getDataAs<AttributeMatrix>(m_InputValues->FeatureAM);
  if(featureAttributeMatrix == nullptr)
  {
    return MakeErrorResult(-54090, fmt::format("DBSCAN Feature Attribute Matrix '{}' is missing.", m_InputValues->FeatureAM.toString()));
  }
  if(maxCluster == std::numeric_limits<int32>::max())
  {
    return MakeErrorResult(-54084, "DBSCAN Feature Attribute Matrix tuple count exceeds the platform size range.");
  }
  featureAttributeMatrix->resizeTuples(ShapeType{static_cast<usize>(maxCluster) + 1});

  return result;
}
