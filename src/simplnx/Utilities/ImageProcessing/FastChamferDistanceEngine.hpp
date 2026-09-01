#pragma once

#include "simplnx/Common/Array.hpp"
#include "simplnx/Common/Result.hpp"
#include "simplnx/Common/Types.hpp"
#include "simplnx/DataStructure/AbstractDataStore.hpp"
#include "simplnx/DataStructure/DataStore.hpp"
#include "simplnx/DataStructure/IO/Generic/ITemporaryRecordStore.hpp"
#include "simplnx/Filter/IFilter.hpp"
#include "simplnx/Utilities/DataStoreUtilities.hpp"
#include "simplnx/Utilities/ImageProcessing/WorkingMemory.hpp"
#include "simplnx/Utilities/StringUtilities.hpp"

#include <fmt/format.h>
#include <nonstd/span.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <vector>

namespace nx::core::ImageProcessing
{
namespace detail
{
constexpr usize k_Chamfer2DResidentLimit = 64ULL * 1024ULL * 1024ULL;
constexpr usize k_Chamfer2DFixedStateBytes = 4096;
constexpr usize k_Chamfer2DMaxTileRecords = 5;

struct Chamfer2DBufferPlan
{
  usize coreRows = 0;
  usize coreCols = 0;
  usize residentBytes = 0;
  bool valid = false;
  bool overflow = false;
};

inline bool ChamferCheckedAdd(usize left, usize right, usize& result)
{
  if(right > std::numeric_limits<usize>::max() - left)
  {
    return false;
  }
  result = left + right;
  return true;
}

inline bool ChamferCheckedMultiply(usize left, usize right, usize& result)
{
  if(left != 0 && right > std::numeric_limits<usize>::max() / left)
  {
    return false;
  }
  result = left * right;
  return true;
}

struct FastChamferResidentMemoryAllocation
{
  CacheMemoryBudgetManager::WorkingMemoryReservation reservation;
  usize requiredBytes = 0;

  [[nodiscard]] bool holdsCompleteState() const noexcept
  {
    return requiredBytes > 0 && reservation.sizeBytes() == requiredBytes;
  }
};

inline bool ShouldUseFastChamferResidentState(const SizeVec3& dims)
{
  return dims[2] > 1;
}

inline Result<usize> CalculateFastChamferResidentWorkingMemoryBytes(const SizeVec3& dims)
{
  if(dims[0] == 0 || dims[1] == 0 || dims[2] == 0)
  {
    return {usize{0}};
  }

  usize sliceValues = 0;
  usize volumeValues = 0;
  usize planeWindowValues = 0;
  usize requiredValues = 0;
  usize requiredBytes = 0;
  if(!ChamferCheckedMultiply(dims[0], dims[1], sliceValues) || !ChamferCheckedMultiply(sliceValues, dims[2], volumeValues) || !ChamferCheckedMultiply(sliceValues, usize{2}, planeWindowValues) ||
     !ChamferCheckedAdd(volumeValues, planeWindowValues, requiredValues) || !ChamferCheckedMultiply(requiredValues, sizeof(float32), requiredBytes))
  {
    return MakeErrorResult<usize>(
        -8676, fmt::format("Fast chamfer distance dimensions ({}) overflow while sizing the resident float32 image and two-plane working window.", StringUtilities::formatDimensions3D(dims)));
  }
  return {requiredBytes};
}

inline Result<FastChamferResidentMemoryAllocation> ReserveFastChamferResidentWorkingMemory(const SizeVec3& dims)
{
  auto requiredResult = CalculateFastChamferResidentWorkingMemoryBytes(dims);
  if(requiredResult.invalid())
  {
    return ConvertInvalidResult<FastChamferResidentMemoryAllocation>(std::move(requiredResult));
  }
  auto reservation = ReserveWorkingMemory(requiredResult.value(), requiredResult.value());
  return {FastChamferResidentMemoryAllocation{std::move(reservation), requiredResult.value()}};
}

inline bool Chamfer2DFullWidthPeak(usize columns, usize rows, usize& peak)
{
  usize rowsWithHalo = 0;
  usize cells = 0;
  usize valuesBytes = 0;
  return ChamferCheckedAdd(rows, 1, rowsWithHalo) && ChamferCheckedMultiply(columns, rowsWithHalo, cells) && ChamferCheckedMultiply(cells, sizeof(float32), valuesBytes) &&
         ChamferCheckedAdd(valuesBytes, k_Chamfer2DFixedStateBytes, peak);
}

inline bool Chamfer2DTiledPeak(usize columns, usize& peak)
{
  usize values = 0;
  usize valuesBytes = 0;
  return ChamferCheckedMultiply(columns, k_Chamfer2DMaxTileRecords, values) && ChamferCheckedMultiply(values, sizeof(float32), valuesBytes) &&
         ChamferCheckedAdd(valuesBytes, k_Chamfer2DFixedStateBytes, peak);
}

inline Chamfer2DBufferPlan BuildChamfer2DBufferPlan(usize nx, usize ny, usize residentLimit = k_Chamfer2DResidentLimit)
{
  Chamfer2DBufferPlan plan;
  usize cellCount = 0;
  if(nx == 0 || ny == 0 || residentLimit == 0 || nx > static_cast<usize>(std::numeric_limits<int64>::max()) || ny > static_cast<usize>(std::numeric_limits<int64>::max()) ||
     !ChamferCheckedMultiply(nx, ny, cellCount))
  {
    plan.overflow = true;
    return plan;
  }

  usize minimumPeak = 0;
  if(!Chamfer2DTiledPeak(1, minimumPeak))
  {
    plan.overflow = true;
    return plan;
  }
  if(minimumPeak > residentLimit)
  {
    return plan;
  }

  auto largestFitting = [residentLimit](usize high, const auto& peakFunction) {
    usize low = 1;
    while(low < high)
    {
      const usize middle = low + (high - low + 1) / 2;
      usize candidatePeak = 0;
      if(peakFunction(middle, candidatePeak) && candidatePeak <= residentLimit)
      {
        low = middle;
      }
      else
      {
        high = middle - 1;
      }
    }
    return low;
  };

  usize oneRowPeak = 0;
  if(Chamfer2DFullWidthPeak(nx, 1, oneRowPeak) && oneRowPeak <= residentLimit)
  {
    plan.coreCols = nx;
    plan.coreRows = largestFitting(ny, [nx](usize rows, usize& peak) { return Chamfer2DFullWidthPeak(nx, rows, peak); });
    if(!Chamfer2DFullWidthPeak(nx, plan.coreRows, plan.residentBytes))
    {
      plan.overflow = true;
      return plan;
    }
  }
  else
  {
    plan.coreCols = largestFitting(nx, [](usize columns, usize& peak) { return Chamfer2DTiledPeak(columns, peak); });
    plan.coreRows = 1;
    if(!Chamfer2DTiledPeak(plan.coreCols, plan.residentBytes))
    {
      plan.overflow = true;
      return plan;
    }
  }
  plan.valid = plan.residentBytes <= residentLimit;
  return plan;
}

// One chamfer neighbor: an offset (dx,dy,dz) and its optimized chamfer weight. The weight is selected by
// "diagonality" nt = (|dx|+|dy|+|dz|) - 1: axis=0.92644, face-diagonal=1.34065, cube-diagonal=1.65849 (itk
// FastChamferDistanceImageFilter default weights). These are unitless voxel weights (ITK's chamfer ignores spacing).
struct ChamferNeighbor
{
  int32 dx;
  int32 dy;
  int32 dz;
  float32 w;
};

// The two half-neighborhoods matching ITK's NeighborhoodIterator flat-index split about the center: the FORWARD half
// (flat index > center) is exactly 9*dz + 3*dy + dx > 0, i.e. dz>0, or dz==0 && dy>0, or dz==0 && dy==0 && dx>0; the
// BACKWARD half is the negation. Each has 13 offsets. @p forward selects which half.
// Optimized chamfer weights (itk::FastChamferDistanceImageFilter defaults) indexed by neighbor "diagonality"
// nt = (|dx|+|dy|+|dz|)-1: axis, face-diagonal, cube-diagonal. Single source of truth for both the neighbor table and
// the w0 (axis-weight) propagation guard in the engine below.
inline constexpr std::array<float32, 3> k_ChamferWeights = {0.92644f, 1.34065f, 1.65849f};

inline std::vector<ChamferNeighbor> BuildChamferNeighbors(bool forward)
{
  std::vector<ChamferNeighbor> out;
  out.reserve(13);
  for(int32 dz = -1; dz <= 1; ++dz)
  {
    for(int32 dy = -1; dy <= 1; ++dy)
    {
      for(int32 dx = -1; dx <= 1; ++dx)
      {
        if(dx == 0 && dy == 0 && dz == 0)
        {
          continue;
        }
        const bool isForward = (9 * dz + 3 * dy + dx) > 0;
        if(isForward != forward)
        {
          continue;
        }
        const int32 nt = std::abs(dx) + std::abs(dy) + std::abs(dz) - 1;
        out.push_back({dx, dy, dz, k_ChamferWeights[static_cast<usize>(nt)]});
      }
    }
  }
  return out;
}
} // namespace detail

/**
 * @brief In-place FastChamferDistance (itk::FastChamferDistanceImageFilter full path): propagate a narrow signed
 * band (typically an IsoContourDistance output) into an approximate signed distance map via a two-pass raster chamfer.
 * A forward raster scan (z,y,x ascending) pushes each in-band voxel's value +- the optimized chamfer weight to its
 * FUTURE half-neighborhood (min on the positive side, max on the negative side); a backward scan (descending) pushes
 * to the PAST half. Voxels at or beyond +-@p maxDist are frozen (never propagate); the pushed value itself is not
 * clamped (matching ITK). Out-of-bounds neighbors are never written. All arithmetic is float32.
 *
 * Resident stores use a fixed 2-plane working window. A real-OOC 3-D store reuses that path only after a complete
 * shared working-memory reservation. Otherwise it is copied in bounded bulk transfers to a fixed-record temporary
 * store, propagated there in row/plane blocks, and copied back once. The 2D path uses bounded full-width row blocks
 * when possible and a constant five-record tile window for over-wide images. Optional sign inversion is fused into
 * backward-finalized writes so callers do not need another full-volume pass.
 */
class FastChamferDistance
{
public:
  FastChamferDistance(AbstractDataStore<float32>& store, SizeVec3 dims, float32 maxDist, const std::atomic_bool& shouldCancel, const IFilter::MessageHandler& messageHandler,
                      usize residentLimit2D = detail::k_Chamfer2DResidentLimit, bool negateOutput = false)
  : m_Store(store)
  , m_Dims(dims)
  , m_MaxDist(maxDist)
  , m_ShouldCancel(shouldCancel)
  , m_MessageHandler(messageHandler)
  , m_ResidentLimit2D(residentLimit2D)
  , m_NegateOutput(negateOutput)
  {
  }
  ~FastChamferDistance() = default;
  FastChamferDistance(const FastChamferDistance&) = delete;
  FastChamferDistance(FastChamferDistance&&) noexcept = delete;
  FastChamferDistance& operator=(const FastChamferDistance&) = delete;
  FastChamferDistance& operator=(FastChamferDistance&&) noexcept = delete;

  Result<> operator()()
  {
    const usize dimX = m_Dims[0];
    const usize dimY = m_Dims[1];
    const usize dimZ = m_Dims[2];
    if(dimX == 0 || dimY == 0 || dimZ == 0)
    {
      return {};
    }
    if(m_ShouldCancel)
    {
      return {};
    }
    if(m_Store.getStoreType() == IDataStore::StoreType::OutOfCore)
    {
      if(dimZ == 1)
      {
        return RunOutOfCore2D(dimX, dimY);
      }

      if(detail::ShouldUseFastChamferResidentState(m_Dims))
      {
        auto allocationResult = detail::ReserveFastChamferResidentWorkingMemory(m_Dims);
        if(allocationResult.invalid())
        {
          return ConvertInvalidResult<void>(std::move(allocationResult));
        }
        auto allocation = std::move(allocationResult.value());
        if(allocation.holdsCompleteState())
        {
          try
          {
            DataStore<float32> residentStore(ShapeType{dimZ, dimY, dimX}, ShapeType{1}, 0.0f);
            nonstd::span<float32> residentValues = residentStore.createSpan();
            if(Result<> result = m_Store.copyIntoBuffer(0, residentValues); result.invalid())
            {
              return result;
            }
            FastChamferDistance residentEngine(residentStore, m_Dims, m_MaxDist, m_ShouldCancel, m_MessageHandler, m_ResidentLimit2D, m_NegateOutput);
            if(Result<> result = residentEngine(); result.invalid())
            {
              return result;
            }
            if(m_ShouldCancel)
            {
              return {};
            }
            return m_Store.copyFromBuffer(0, nonstd::span<const float32>(residentValues.data(), residentValues.size()));
          } catch(const std::bad_alloc&)
          {
            // Release the complete-state reservation before entering the bounded fallback.
          }
        }
      }
      return RunOutOfCore3D(dimX, dimY, dimZ);
    }

    const int64 nX = static_cast<int64>(m_Dims[0]);
    const int64 nY = static_cast<int64>(m_Dims[1]);
    const int64 nZ = static_cast<int64>(m_Dims[2]);
    const usize slice = static_cast<usize>(nX * nY);
    if(slice == 0 || nZ == 0)
    {
      return {};
    }
    const float32 w0 = detail::k_ChamferWeights[0]; // axis weight; single source of truth (see detail::k_ChamferWeights)

    const std::vector<detail::ChamferNeighbor> fwd = detail::BuildChamferNeighbors(/*forward=*/true);
    const std::vector<detail::ChamferNeighbor> bwd = detail::BuildChamferNeighbors(/*forward=*/false);

    std::vector<float32> cur(slice);
    std::vector<float32> other(slice); // next (forward) / prev (backward)

    // ---- Forward pass: z,y,x ascending; push to plane z (dz==0) and plane z+1 (dz==1) ----
    if(Result<> r = m_Store.copyIntoBuffer(0, nonstd::span<float32>(cur.data(), slice)); r.invalid())
    {
      return r;
    }
    if(nZ > 1)
    {
      if(Result<> r = m_Store.copyIntoBuffer(slice, nonstd::span<float32>(other.data(), slice)); r.invalid())
      {
        return r;
      }
    }
    for(int64 z = 0; z < nZ; ++z)
    {
      if(m_ShouldCancel)
      {
        return {};
      }
      const bool hasNextZ = (z + 1) < nZ;
      for(int64 y = 0; y < nY; ++y)
      {
        for(int64 x = 0; x < nX; ++x)
        {
          const float32 c = cur[static_cast<usize>(y * nX + x)];
          if(c >= m_MaxDist || c <= -m_MaxDist)
          {
            continue;
          }
          const bool doPos = c > -w0;
          const bool doNeg = c < w0;
          for(const detail::ChamferNeighbor& nb : fwd)
          {
            const int64 tx = x + nb.dx;
            const int64 ty = y + nb.dy;
            if(tx < 0 || tx >= nX || ty < 0 || ty >= nY)
            {
              continue;
            }
            if(nb.dz == 1 && !hasNextZ)
            {
              continue;
            }
            float32& tgt = (nb.dz == 0) ? cur[static_cast<usize>(ty * nX + tx)] : other[static_cast<usize>(ty * nX + tx)];
            PushChamfer(tgt, c, nb.w, doPos, doNeg);
          }
        }
      }
      if(Result<> r = m_Store.copyFromBuffer(static_cast<usize>(z) * slice, nonstd::span<const float32>(cur.data(), slice)); r.invalid())
      {
        return r;
      }
      if(hasNextZ)
      {
        std::swap(cur, other);
        if((z + 2) < nZ)
        {
          if(Result<> r = m_Store.copyIntoBuffer(static_cast<usize>(z + 2) * slice, nonstd::span<float32>(other.data(), slice)); r.invalid())
          {
            return r;
          }
        }
      }
    }

    // ---- Backward pass: z,y,x descending; push to plane z (dz==0) and plane z-1 (dz==-1) ----
    if(Result<> r = m_Store.copyIntoBuffer(static_cast<usize>(nZ - 1) * slice, nonstd::span<float32>(cur.data(), slice)); r.invalid())
    {
      return r;
    }
    if(nZ > 1)
    {
      if(Result<> r = m_Store.copyIntoBuffer(static_cast<usize>(nZ - 2) * slice, nonstd::span<float32>(other.data(), slice)); r.invalid())
      {
        return r;
      }
    }
    for(int64 z = nZ - 1; z >= 0; --z)
    {
      if(m_ShouldCancel)
      {
        return {};
      }
      const bool hasPrevZ = (z - 1) >= 0;
      for(int64 y = nY - 1; y >= 0; --y)
      {
        for(int64 x = nX - 1; x >= 0; --x)
        {
          const float32 c = cur[static_cast<usize>(y * nX + x)];
          if(c >= m_MaxDist || c <= -m_MaxDist)
          {
            continue;
          }
          const bool doPos = c > -w0;
          const bool doNeg = c < w0;
          for(const detail::ChamferNeighbor& nb : bwd)
          {
            const int64 tx = x + nb.dx;
            const int64 ty = y + nb.dy;
            if(tx < 0 || tx >= nX || ty < 0 || ty >= nY)
            {
              continue;
            }
            if(nb.dz == -1 && !hasPrevZ)
            {
              continue;
            }
            float32& tgt = (nb.dz == 0) ? cur[static_cast<usize>(ty * nX + tx)] : other[static_cast<usize>(ty * nX + tx)];
            PushChamfer(tgt, c, nb.w, doPos, doNeg);
          }
        }
      }
      MaybeNegateValues(nonstd::span<float32>(cur.data(), slice));
      if(Result<> r = m_Store.copyFromBuffer(static_cast<usize>(z) * slice, nonstd::span<const float32>(cur.data(), slice)); r.invalid())
      {
        return r;
      }
      if(hasPrevZ)
      {
        std::swap(cur, other);
        if((z - 2) >= 0)
        {
          if(Result<> r = m_Store.copyIntoBuffer(static_cast<usize>(z - 2) * slice, nonstd::span<float32>(other.data(), slice)); r.invalid())
          {
            return r;
          }
        }
      }
    }
    return {};
  }

private:
  template <class T>
  static std::string DescribeResultError(const Result<T>& result)
  {
    if(result.errors().empty())
    {
      return "provider returned an unspecified error";
    }
    const Error& error = result.errors().front();
    return fmt::format("{} (provider code {})", error.message, error.code);
  }

  static nonstd::span<std::byte> AsWritableBytes(nonstd::span<float32> values)
  {
    return {reinterpret_cast<std::byte*>(values.data()), values.size() * sizeof(float32)};
  }

  static nonstd::span<const std::byte> AsBytes(nonstd::span<const float32> values)
  {
    return {reinterpret_cast<const std::byte*>(values.data()), values.size() * sizeof(float32)};
  }

  void MaybeNegateValues(nonstd::span<float32> values) const
  {
    if(!m_NegateOutput)
    {
      return;
    }
    for(float32& value : values)
    {
      value = -value;
    }
  }

  Result<std::unique_ptr<ITemporaryRecordStore>> CreateScratchStore(const TemporaryRecordStoreConfig& config, std::string_view context)
  {
    try
    {
      auto storeResult = DataStoreUtilities::CreateTemporaryRecordStore(config);
      if(storeResult.invalid())
      {
        return MakeErrorResult<std::unique_ptr<ITemporaryRecordStore>>(-8673, fmt::format("Fast chamfer distance failed to create {}: {}", context, DescribeResultError(storeResult)));
      }
      std::unique_ptr<ITemporaryRecordStore> store = std::move(storeResult.value());
      if(store == nullptr)
      {
        return MakeErrorResult<std::unique_ptr<ITemporaryRecordStore>>(-8673, fmt::format("Fast chamfer distance failed to create {}: provider returned a null store", context));
      }
      return {std::move(store)};
    } catch(const std::bad_alloc& exception)
    {
      return MakeErrorResult<std::unique_ptr<ITemporaryRecordStore>>(-8673, fmt::format("Fast chamfer distance failed to create {}: {}", context, exception.what()));
    } catch(const std::exception& exception)
    {
      return MakeErrorResult<std::unique_ptr<ITemporaryRecordStore>>(-8673, fmt::format("Fast chamfer distance failed to create {}: {}", context, exception.what()));
    }
  }

  Result<> ReadScratchRecords(const ITemporaryRecordStore& store, uint64 recordOffset, uint64 recordCount, nonstd::span<float32> values, std::string_view context)
  {
    if(m_ShouldCancel)
    {
      return {};
    }
    try
    {
      auto result = store.read(recordOffset, recordCount, AsWritableBytes(values), m_ShouldCancel);
      if(result.invalid())
      {
        return MakeErrorResult(-8674, fmt::format("Fast chamfer distance scratch read failed for {}: {}", context, DescribeResultError(result)));
      }
      if(result.value() != recordCount)
      {
        return MakeErrorResult(-8674, fmt::format("Fast chamfer distance scratch read failed for {}: returned {} of {} records", context, result.value(), recordCount));
      }
    } catch(const std::exception& exception)
    {
      return MakeErrorResult(-8674, fmt::format("Fast chamfer distance scratch read failed for {}: {}", context, exception.what()));
    }
    return {};
  }

  Result<> WriteScratchRecords(ITemporaryRecordStore& store, uint64 recordOffset, uint64 recordCount, nonstd::span<const float32> values, std::string_view context)
  {
    if(m_ShouldCancel)
    {
      return {};
    }
    try
    {
      Result<> result = store.write(recordOffset, recordCount, AsBytes(values), m_ShouldCancel);
      if(result.invalid())
      {
        return MakeErrorResult(-8674, fmt::format("Fast chamfer distance scratch write failed for {}: {}", context, DescribeResultError(result)));
      }
    } catch(const std::exception& exception)
    {
      return MakeErrorResult(-8674, fmt::format("Fast chamfer distance scratch write failed for {}: {}", context, exception.what()));
    }
    return {};
  }

  Result<> ReadPrimaryValues(usize valueOffset, nonstd::span<float32> values, std::string_view context)
  {
    if(m_ShouldCancel)
    {
      return {};
    }
    try
    {
      Result<> result = m_Store.copyIntoBuffer(valueOffset, values);
      if(result.invalid())
      {
        return MakeErrorResult(-8675, fmt::format("Fast chamfer distance bulk input transfer failed for {}: {}", context, DescribeResultError(result)));
      }
    } catch(const std::exception& exception)
    {
      return MakeErrorResult(-8675, fmt::format("Fast chamfer distance bulk input transfer failed for {}: {}", context, exception.what()));
    }
    return {};
  }

  Result<> WritePrimaryValues(usize valueOffset, nonstd::span<const float32> values, std::string_view context)
  {
    if(m_ShouldCancel)
    {
      return {};
    }
    try
    {
      Result<> result = m_Store.copyFromBuffer(valueOffset, values);
      if(result.invalid())
      {
        return MakeErrorResult(-8675, fmt::format("Fast chamfer distance bulk output transfer failed for {}: {}", context, DescribeResultError(result)));
      }
    } catch(const std::exception& exception)
    {
      return MakeErrorResult(-8675, fmt::format("Fast chamfer distance bulk output transfer failed for {}: {}", context, exception.what()));
    }
    return {};
  }

  void Propagate2DForward(float32* block, usize nx, usize sourceRows, usize loadedRows)
  {
    const float32 w0 = detail::k_ChamferWeights[0];
    const float32 w1 = detail::k_ChamferWeights[1];
    for(usize row = 0; row < sourceRows; ++row)
    {
      float32* current = block + row * nx;
      float32* next = row + 1 < loadedRows ? current + nx : nullptr;
      for(usize x = 0; x < nx; ++x)
      {
        const float32 value = current[x];
        if(value >= m_MaxDist || value <= -m_MaxDist)
        {
          continue;
        }
        const bool doPos = value > -w0;
        const bool doNeg = value < w0;
        if(x + 1 < nx)
        {
          PushChamfer(current[x + 1], value, w0, doPos, doNeg);
        }
        if(next != nullptr)
        {
          if(x > 0)
          {
            PushChamfer(next[x - 1], value, w1, doPos, doNeg);
          }
          PushChamfer(next[x], value, w0, doPos, doNeg);
          if(x + 1 < nx)
          {
            PushChamfer(next[x + 1], value, w1, doPos, doNeg);
          }
        }
      }
    }
  }

  void Propagate2DBackward(float32* block, usize nx, usize firstSourceRow, usize loadedRows)
  {
    const float32 w0 = detail::k_ChamferWeights[0];
    const float32 w1 = detail::k_ChamferWeights[1];
    for(usize row = loadedRows; row-- > firstSourceRow;)
    {
      float32* current = block + row * nx;
      float32* previous = row > 0 ? current - nx : nullptr;
      for(usize x = nx; x-- > 0;)
      {
        const float32 value = current[x];
        if(value >= m_MaxDist || value <= -m_MaxDist)
        {
          continue;
        }
        const bool doPos = value > -w0;
        const bool doNeg = value < w0;
        if(previous != nullptr)
        {
          if(x > 0)
          {
            PushChamfer(previous[x - 1], value, w1, doPos, doNeg);
          }
          PushChamfer(previous[x], value, w0, doPos, doNeg);
          if(x + 1 < nx)
          {
            PushChamfer(previous[x + 1], value, w1, doPos, doNeg);
          }
        }
        if(x > 0)
        {
          PushChamfer(current[x - 1], value, w0, doPos, doNeg);
        }
      }
    }
  }

  Result<> RunFullWidth2D(ITemporaryRecordStore& scratch, usize nx, usize ny, usize coreRows)
  {
    const usize capacityRows = std::min(ny, coreRows + 1);
    usize capacityValues = 0;
    if(!detail::ChamferCheckedMultiply(capacityRows, nx, capacityValues))
    {
      return MakeErrorResult(-8672, fmt::format("Fast chamfer distance 2D row-block layout overflows for dimensions {} x {}.", nx, ny));
    }
    std::vector<float32> block(capacityValues);

    bool firstBlock = true;
    for(usize rowBegin = 0; rowBegin < ny; rowBegin += coreRows)
    {
      if(m_ShouldCancel)
      {
        return {};
      }
      const usize sourceRows = std::min(coreRows, ny - rowBegin);
      const usize loadedRows = sourceRows + static_cast<usize>(rowBegin + sourceRows < ny);
      if(firstBlock)
      {
        if(Result<> result = ReadPrimaryValues(rowBegin * nx, nonstd::span<float32>(block.data(), loadedRows * nx), "2D forward input block"); result.invalid())
        {
          return result;
        }
      }
      else
      {
        const usize rowsToRead = loadedRows - 1;
        if(rowsToRead > 0)
        {
          if(Result<> result = ReadPrimaryValues((rowBegin + 1) * nx, nonstd::span<float32>(block.data() + nx, rowsToRead * nx), "2D forward input block"); result.invalid())
          {
            return result;
          }
        }
      }
      Propagate2DForward(block.data(), nx, sourceRows, loadedRows);
      if(Result<> result = WriteScratchRecords(scratch, rowBegin, sourceRows, nonstd::span<const float32>(block.data(), sourceRows * nx), "2D forward output block"); result.invalid())
      {
        return result;
      }
      if(loadedRows > sourceRows)
      {
        std::copy_n(block.data() + sourceRows * nx, nx, block.data());
      }
      firstBlock = false;
    }

    firstBlock = true;
    for(usize rowEnd = ny; rowEnd > 0;)
    {
      if(m_ShouldCancel)
      {
        return {};
      }
      const usize sourceRows = std::min(coreRows, rowEnd);
      const usize sourceBegin = rowEnd - sourceRows;
      const bool hasHalo = sourceBegin > 0;
      const usize loadBegin = hasHalo ? sourceBegin - 1 : sourceBegin;
      const usize loadedRows = sourceRows + static_cast<usize>(hasHalo);
      if(firstBlock)
      {
        if(Result<> result = ReadScratchRecords(scratch, loadBegin, loadedRows, nonstd::span<float32>(block.data(), loadedRows * nx), "2D backward input block"); result.invalid())
        {
          return result;
        }
      }
      else
      {
        std::copy_n(block.data(), nx, block.data() + (loadedRows - 1) * nx);
        const usize rowsToRead = loadedRows - 1;
        if(rowsToRead > 0)
        {
          if(Result<> result = ReadScratchRecords(scratch, loadBegin, rowsToRead, nonstd::span<float32>(block.data(), rowsToRead * nx), "2D backward input block"); result.invalid())
          {
            return result;
          }
        }
      }
      Propagate2DBackward(block.data(), nx, static_cast<usize>(hasHalo), loadedRows);
      const usize sourceOffset = static_cast<usize>(hasHalo) * nx;
      MaybeNegateValues(nonstd::span<float32>(block.data() + sourceOffset, sourceRows * nx));
      if(Result<> result = WritePrimaryValues(sourceBegin * nx, nonstd::span<const float32>(block.data() + sourceOffset, sourceRows * nx), "2D backward output block"); result.invalid())
      {
        return result;
      }
      rowEnd = sourceBegin;
      firstBlock = false;
    }
    return {};
  }

  Result<> ProcessTiled2DPass(ITemporaryRecordStore& scratch, usize nx, usize ny, usize coreCols, usize tileCount, bool forward)
  {
    std::array<std::vector<float32>, detail::k_Chamfer2DMaxTileRecords> tileValues;
    for(auto& values : tileValues)
    {
      values.resize(coreCols);
    }
    std::array<uint64, detail::k_Chamfer2DMaxTileRecords> recordIndices{};
    std::array<bool, detail::k_Chamfer2DMaxTileRecords> dirty{};
    const float32 w0 = detail::k_ChamferWeights[0];
    const float32 w1 = detail::k_ChamferWeights[1];

    const auto processTile = [&](usize row, usize tile) -> Result<> {
      if(m_ShouldCancel)
      {
        return {};
      }
      usize usedRecords = 0;
      dirty.fill(false);
      auto loadRecord = [&](usize targetRow, usize targetTile) -> Result<> {
        const uint64 recordIndex = static_cast<uint64>(targetRow) * static_cast<uint64>(tileCount) + static_cast<uint64>(targetTile);
        for(usize index = 0; index < usedRecords; ++index)
        {
          if(recordIndices[index] == recordIndex)
          {
            return {};
          }
        }
        if(usedRecords >= tileValues.size())
        {
          return MakeErrorResult(-8672, "Fast chamfer distance 2D tile working set exceeded its fixed five-record bound.");
        }
        nonstd::span<float32> values(tileValues[usedRecords].data(), coreCols);
        if(Result<> result = ReadScratchRecords(scratch, recordIndex, 1, values, "2D tile working set"); result.invalid())
        {
          return result;
        }
        recordIndices[usedRecords] = recordIndex;
        ++usedRecords;
        return {};
      };
      auto findRecord = [&](usize targetRow, usize targetTile) {
        const uint64 recordIndex = static_cast<uint64>(targetRow) * static_cast<uint64>(tileCount) + static_cast<uint64>(targetTile);
        for(usize index = 0; index < usedRecords; ++index)
        {
          if(recordIndices[index] == recordIndex)
          {
            return index;
          }
        }
        return usedRecords;
      };

      if(Result<> result = loadRecord(row, tile); result.invalid())
      {
        return result;
      }
      if(forward)
      {
        if(tile + 1 < tileCount)
        {
          if(Result<> result = loadRecord(row, tile + 1); result.invalid())
          {
            return result;
          }
        }
        if(row + 1 < ny)
        {
          if(tile > 0)
          {
            if(Result<> result = loadRecord(row + 1, tile - 1); result.invalid())
            {
              return result;
            }
          }
          if(Result<> result = loadRecord(row + 1, tile); result.invalid())
          {
            return result;
          }
          if(tile + 1 < tileCount)
          {
            if(Result<> result = loadRecord(row + 1, tile + 1); result.invalid())
            {
              return result;
            }
          }
        }
      }
      else
      {
        if(tile > 0)
        {
          if(Result<> result = loadRecord(row, tile - 1); result.invalid())
          {
            return result;
          }
        }
        if(row > 0)
        {
          if(tile > 0)
          {
            if(Result<> result = loadRecord(row - 1, tile - 1); result.invalid())
            {
              return result;
            }
          }
          if(Result<> result = loadRecord(row - 1, tile); result.invalid())
          {
            return result;
          }
          if(tile + 1 < tileCount)
          {
            if(Result<> result = loadRecord(row - 1, tile + 1); result.invalid())
            {
              return result;
            }
          }
        }
      }

      auto pushTarget = [&](usize targetRow, usize targetX, float32 source, float32 weight, bool doPos, bool doNeg) {
        const usize targetTile = targetX / coreCols;
        const usize targetOffset = targetX - targetTile * coreCols;
        const usize recordSlot = findRecord(targetRow, targetTile);
        if(recordSlot == usedRecords)
        {
          return false;
        }
        PushChamfer(tileValues[recordSlot][targetOffset], source, weight, doPos, doNeg);
        dirty[recordSlot] = true;
        return true;
      };

      const usize xBegin = tile * coreCols;
      const usize validColumns = std::min(coreCols, nx - xBegin);
      float32* sourceValues = tileValues[0].data();
      if(forward)
      {
        for(usize localX = 0; localX < validColumns; ++localX)
        {
          const usize x = xBegin + localX;
          const float32 value = sourceValues[localX];
          if(value >= m_MaxDist || value <= -m_MaxDist)
          {
            continue;
          }
          const bool doPos = value > -w0;
          const bool doNeg = value < w0;
          if(x + 1 < nx && !pushTarget(row, x + 1, value, w0, doPos, doNeg))
          {
            return MakeErrorResult(-8672, "Fast chamfer distance 2D forward tile omitted a required target record.");
          }
          if(row + 1 < ny)
          {
            if(x > 0 && !pushTarget(row + 1, x - 1, value, w1, doPos, doNeg))
            {
              return MakeErrorResult(-8672, "Fast chamfer distance 2D forward tile omitted a required target record.");
            }
            if(!pushTarget(row + 1, x, value, w0, doPos, doNeg))
            {
              return MakeErrorResult(-8672, "Fast chamfer distance 2D forward tile omitted a required target record.");
            }
            if(x + 1 < nx && !pushTarget(row + 1, x + 1, value, w1, doPos, doNeg))
            {
              return MakeErrorResult(-8672, "Fast chamfer distance 2D forward tile omitted a required target record.");
            }
          }
        }
      }
      else
      {
        for(usize localX = validColumns; localX-- > 0;)
        {
          const usize x = xBegin + localX;
          const float32 value = sourceValues[localX];
          if(value >= m_MaxDist || value <= -m_MaxDist)
          {
            continue;
          }
          const bool doPos = value > -w0;
          const bool doNeg = value < w0;
          if(row > 0)
          {
            if(x > 0 && !pushTarget(row - 1, x - 1, value, w1, doPos, doNeg))
            {
              return MakeErrorResult(-8672, "Fast chamfer distance 2D backward tile omitted a required target record.");
            }
            if(!pushTarget(row - 1, x, value, w0, doPos, doNeg))
            {
              return MakeErrorResult(-8672, "Fast chamfer distance 2D backward tile omitted a required target record.");
            }
            if(x + 1 < nx && !pushTarget(row - 1, x + 1, value, w1, doPos, doNeg))
            {
              return MakeErrorResult(-8672, "Fast chamfer distance 2D backward tile omitted a required target record.");
            }
          }
          if(x > 0 && !pushTarget(row, x - 1, value, w0, doPos, doNeg))
          {
            return MakeErrorResult(-8672, "Fast chamfer distance 2D backward tile omitted a required target record.");
          }
        }
      }

      for(usize index = 0; index < usedRecords; ++index)
      {
        if(!dirty[index])
        {
          continue;
        }
        const nonstd::span<const float32> values(tileValues[index].data(), coreCols);
        if(Result<> result = WriteScratchRecords(scratch, recordIndices[index], 1, values, forward ? "2D forward tile" : "2D backward tile"); result.invalid())
        {
          return result;
        }
      }
      return {};
    };

    if(forward)
    {
      for(usize row = 0; row < ny; ++row)
      {
        for(usize tile = 0; tile < tileCount; ++tile)
        {
          if(Result<> result = processTile(row, tile); result.invalid())
          {
            return result;
          }
        }
      }
    }
    else
    {
      for(usize row = ny; row-- > 0;)
      {
        for(usize tile = tileCount; tile-- > 0;)
        {
          if(Result<> result = processTile(row, tile); result.invalid())
          {
            return result;
          }
        }
      }
    }
    return {};
  }

  Result<> RunTiled2D(ITemporaryRecordStore& scratch, usize nx, usize ny, usize coreCols, usize tileCount)
  {
    {
      std::vector<float32> record(coreCols, 0.0f);
      for(usize row = 0; row < ny; ++row)
      {
        for(usize tile = 0; tile < tileCount; ++tile)
        {
          if(m_ShouldCancel)
          {
            return {};
          }
          const usize xBegin = tile * coreCols;
          const usize validColumns = std::min(coreCols, nx - xBegin);
          std::fill(record.begin(), record.end(), 0.0f);
          if(Result<> result = ReadPrimaryValues(row * nx + xBegin, nonstd::span<float32>(record.data(), validColumns), "2D tiled scratch initialization"); result.invalid())
          {
            return result;
          }
          const uint64 recordIndex = static_cast<uint64>(row) * static_cast<uint64>(tileCount) + static_cast<uint64>(tile);
          if(Result<> result = WriteScratchRecords(scratch, recordIndex, 1, nonstd::span<const float32>(record.data(), coreCols), "2D tiled scratch initialization"); result.invalid())
          {
            return result;
          }
        }
      }
    }

    if(Result<> result = ProcessTiled2DPass(scratch, nx, ny, coreCols, tileCount, true); result.invalid())
    {
      return result;
    }
    if(Result<> result = ProcessTiled2DPass(scratch, nx, ny, coreCols, tileCount, false); result.invalid())
    {
      return result;
    }

    {
      std::vector<float32> record(coreCols);
      for(usize row = 0; row < ny; ++row)
      {
        for(usize tile = 0; tile < tileCount; ++tile)
        {
          if(m_ShouldCancel)
          {
            return {};
          }
          const uint64 recordIndex = static_cast<uint64>(row) * static_cast<uint64>(tileCount) + static_cast<uint64>(tile);
          if(Result<> result = ReadScratchRecords(scratch, recordIndex, 1, nonstd::span<float32>(record.data(), coreCols), "2D tiled final output"); result.invalid())
          {
            return result;
          }
          const usize xBegin = tile * coreCols;
          const usize validColumns = std::min(coreCols, nx - xBegin);
          MaybeNegateValues(nonstd::span<float32>(record.data(), validColumns));
          if(Result<> result = WritePrimaryValues(row * nx + xBegin, nonstd::span<const float32>(record.data(), validColumns), "2D tiled final output"); result.invalid())
          {
            return result;
          }
        }
      }
    }
    return {};
  }

  Result<> RunOutOfCore2D(usize nx, usize ny)
  {
    const detail::Chamfer2DBufferPlan plan = detail::BuildChamfer2DBufferPlan(nx, ny, m_ResidentLimit2D);
    if(plan.overflow)
    {
      return MakeErrorResult(-8670, fmt::format("Fast chamfer distance 2D buffer plan cannot represent dimensions {} x {} due to arithmetic overflow.", nx, ny));
    }
    if(!plan.valid)
    {
      return MakeErrorResult(-8672, fmt::format("Fast chamfer distance 2D buffer plan exceeds the {}-byte resident limit for dimensions {} x {}.", m_ResidentLimit2D, nx, ny));
    }
    usize cellCount = 0;
    if(!detail::ChamferCheckedMultiply(nx, ny, cellCount))
    {
      return MakeErrorResult(-8670, fmt::format("Fast chamfer distance image dimensions overflow the addressable value count: {} x {} x 1.", nx, ny));
    }
    if(m_Store.getSize() != cellCount)
    {
      return MakeErrorResult(-8671, fmt::format("Fast chamfer distance store size ({}) does not match the expected image size ({}) for dimensions {} x {} x 1.", m_Store.getSize(), cellCount, nx, ny));
    }

    const usize tileCount = 1 + (nx - 1) / plan.coreCols;
    usize recordSize = 0;
    usize recordCount = 0;
    if(!detail::ChamferCheckedMultiply(plan.coreCols, sizeof(float32), recordSize) || !detail::ChamferCheckedMultiply(ny, tileCount, recordCount))
    {
      return MakeErrorResult(-8670, fmt::format("Fast chamfer distance 2D scratch layout cannot represent dimensions {} x {} due to arithmetic overflow.", nx, ny));
    }
    TemporaryRecordStoreConfig config;
    config.recordSize = static_cast<uint64>(recordSize);
    config.maxRecordsPerBatch = static_cast<uint64>(plan.coreCols == nx ? std::min(ny, plan.coreRows + 1) : 1);
    config.initialRecordCount = static_cast<uint64>(recordCount);
    auto scratchResult = CreateScratchStore(config, "bounded 2D fixed-record scratch store");
    if(scratchResult.invalid())
    {
      return ConvertResult(std::move(scratchResult));
    }
    std::unique_ptr<ITemporaryRecordStore> scratch = std::move(scratchResult.value());
    try
    {
      if(plan.coreCols == nx)
      {
        return RunFullWidth2D(*scratch, nx, ny, plan.coreRows);
      }
      return RunTiled2D(*scratch, nx, ny, plan.coreCols, tileCount);
    } catch(const std::bad_alloc& exception)
    {
      return MakeErrorResult(-8672, fmt::format("Fast chamfer distance could not allocate its bounded 2D working set: {}", exception.what()));
    }
  }

  void Propagate3DForward(float32* block, usize nx, usize ny, usize sourcePlanes, usize loadedPlanes, const std::vector<detail::ChamferNeighbor>& neighbors)
  {
    const usize slice = nx * ny;
    const int64 signedX = static_cast<int64>(nx);
    const int64 signedY = static_cast<int64>(ny);
    const float32 w0 = detail::k_ChamferWeights[0];
    for(usize plane = 0; plane < sourcePlanes; ++plane)
    {
      float32* current = block + plane * slice;
      float32* next = plane + 1 < loadedPlanes ? current + slice : nullptr;
      for(int64 y = 0; y < signedY; ++y)
      {
        for(int64 x = 0; x < signedX; ++x)
        {
          const usize sourceIndex = static_cast<usize>(y) * nx + static_cast<usize>(x);
          const float32 value = current[sourceIndex];
          if(value >= m_MaxDist || value <= -m_MaxDist)
          {
            continue;
          }
          const bool doPos = value > -w0;
          const bool doNeg = value < w0;
          for(const detail::ChamferNeighbor& neighbor : neighbors)
          {
            const int64 targetX = x + neighbor.dx;
            const int64 targetY = y + neighbor.dy;
            if(targetX < 0 || targetX >= signedX || targetY < 0 || targetY >= signedY || (neighbor.dz == 1 && next == nullptr))
            {
              continue;
            }
            const usize targetIndex = static_cast<usize>(targetY) * nx + static_cast<usize>(targetX);
            float32& target = neighbor.dz == 0 ? current[targetIndex] : next[targetIndex];
            PushChamfer(target, value, neighbor.w, doPos, doNeg);
          }
        }
      }
    }
  }

  void Propagate3DBackward(float32* block, usize nx, usize ny, usize firstSourcePlane, usize loadedPlanes, const std::vector<detail::ChamferNeighbor>& neighbors)
  {
    const usize slice = nx * ny;
    const int64 signedX = static_cast<int64>(nx);
    const int64 signedY = static_cast<int64>(ny);
    const float32 w0 = detail::k_ChamferWeights[0];
    for(usize plane = loadedPlanes; plane-- > firstSourcePlane;)
    {
      float32* current = block + plane * slice;
      float32* previous = plane > 0 ? current - slice : nullptr;
      for(int64 y = signedY; y-- > 0;)
      {
        for(int64 x = signedX; x-- > 0;)
        {
          const usize sourceIndex = static_cast<usize>(y) * nx + static_cast<usize>(x);
          const float32 value = current[sourceIndex];
          if(value >= m_MaxDist || value <= -m_MaxDist)
          {
            continue;
          }
          const bool doPos = value > -w0;
          const bool doNeg = value < w0;
          for(const detail::ChamferNeighbor& neighbor : neighbors)
          {
            const int64 targetX = x + neighbor.dx;
            const int64 targetY = y + neighbor.dy;
            if(targetX < 0 || targetX >= signedX || targetY < 0 || targetY >= signedY || (neighbor.dz == -1 && previous == nullptr))
            {
              continue;
            }
            const usize targetIndex = static_cast<usize>(targetY) * nx + static_cast<usize>(targetX);
            float32& target = neighbor.dz == 0 ? current[targetIndex] : previous[targetIndex];
            PushChamfer(target, value, neighbor.w, doPos, doNeg);
          }
        }
      }
    }
  }

  Result<> RunOutOfCore3D(usize nx, usize ny, usize nz)
  {
    if(nx > static_cast<usize>(std::numeric_limits<int64>::max()) || ny > static_cast<usize>(std::numeric_limits<int64>::max()) || nz > static_cast<usize>(std::numeric_limits<int64>::max()))
    {
      return MakeErrorResult(-8670, fmt::format("Fast chamfer distance dimensions exceed the supported signed coordinate range: {} x {} x {}.", nx, ny, nz));
    }
    usize slice = 0;
    usize volume = 0;
    usize planeBytes = 0;
    if(!detail::ChamferCheckedMultiply(nx, ny, slice) || !detail::ChamferCheckedMultiply(slice, nz, volume) || !detail::ChamferCheckedMultiply(slice, sizeof(float32), planeBytes))
    {
      return MakeErrorResult(-8670, fmt::format("Fast chamfer distance image dimensions overflow the addressable value count: {} x {} x {}.", nx, ny, nz));
    }
    if(m_Store.getSize() != volume)
    {
      return MakeErrorResult(-8671,
                             fmt::format("Fast chamfer distance store size ({}) does not match the expected image size ({}) for dimensions {} x {} x {}.", m_Store.getSize(), volume, nx, ny, nz));
    }

    constexpr usize kMaxCorePlanes = 16;
    const usize availableBytes = detail::k_Chamfer2DResidentLimit - detail::k_Chamfer2DFixedStateBytes;
    const usize planesFit = planeBytes == 0 ? 0 : availableBytes / planeBytes;
    usize corePlanes = planesFit > 1 ? planesFit - 1 : 1;
    corePlanes = std::min({corePlanes, kMaxCorePlanes, nz});
    const usize capacityPlanes = corePlanes + static_cast<usize>(corePlanes < nz);
    usize capacityValues = 0;
    if(!detail::ChamferCheckedMultiply(capacityPlanes, slice, capacityValues))
    {
      return MakeErrorResult(-8670, fmt::format("Fast chamfer distance 3D plane-block layout overflows for dimensions {} x {} x {}.", nx, ny, nz));
    }

    TemporaryRecordStoreConfig config;
    config.recordSize = static_cast<uint64>(planeBytes);
    config.maxRecordsPerBatch = static_cast<uint64>(capacityPlanes);
    config.initialRecordCount = static_cast<uint64>(nz);
    auto scratchResult = CreateScratchStore(config, "bounded 3D fixed-record scratch store");
    if(scratchResult.invalid())
    {
      return ConvertResult(std::move(scratchResult));
    }
    std::unique_ptr<ITemporaryRecordStore> scratch = std::move(scratchResult.value());

    try
    {
      std::vector<float32> block(capacityValues);
      const std::vector<detail::ChamferNeighbor> forwardNeighbors = detail::BuildChamferNeighbors(true);
      bool firstBlock = true;
      for(usize planeBegin = 0; planeBegin < nz; planeBegin += corePlanes)
      {
        if(m_ShouldCancel)
        {
          return {};
        }
        const usize sourcePlanes = std::min(corePlanes, nz - planeBegin);
        const usize loadedPlanes = sourcePlanes + static_cast<usize>(planeBegin + sourcePlanes < nz);
        if(firstBlock)
        {
          if(Result<> result = ReadPrimaryValues(planeBegin * slice, nonstd::span<float32>(block.data(), loadedPlanes * slice), "3D forward input block"); result.invalid())
          {
            return result;
          }
        }
        else
        {
          const usize planesToRead = loadedPlanes - 1;
          if(planesToRead > 0)
          {
            if(Result<> result = ReadPrimaryValues((planeBegin + 1) * slice, nonstd::span<float32>(block.data() + slice, planesToRead * slice), "3D forward input block"); result.invalid())
            {
              return result;
            }
          }
        }
        Propagate3DForward(block.data(), nx, ny, sourcePlanes, loadedPlanes, forwardNeighbors);
        if(Result<> result = WriteScratchRecords(*scratch, planeBegin, sourcePlanes, nonstd::span<const float32>(block.data(), sourcePlanes * slice), "3D forward output block"); result.invalid())
        {
          return result;
        }
        if(loadedPlanes > sourcePlanes)
        {
          std::copy_n(block.data() + sourcePlanes * slice, slice, block.data());
        }
        firstBlock = false;
      }

      const std::vector<detail::ChamferNeighbor> backwardNeighbors = detail::BuildChamferNeighbors(false);
      firstBlock = true;
      for(usize planeEnd = nz; planeEnd > 0;)
      {
        if(m_ShouldCancel)
        {
          return {};
        }
        const usize sourcePlanes = std::min(corePlanes, planeEnd);
        const usize sourceBegin = planeEnd - sourcePlanes;
        const bool hasHalo = sourceBegin > 0;
        const usize loadBegin = hasHalo ? sourceBegin - 1 : sourceBegin;
        const usize loadedPlanes = sourcePlanes + static_cast<usize>(hasHalo);
        if(firstBlock)
        {
          if(Result<> result = ReadScratchRecords(*scratch, loadBegin, loadedPlanes, nonstd::span<float32>(block.data(), loadedPlanes * slice), "3D backward input block"); result.invalid())
          {
            return result;
          }
        }
        else
        {
          std::copy_n(block.data(), slice, block.data() + (loadedPlanes - 1) * slice);
          const usize planesToRead = loadedPlanes - 1;
          if(planesToRead > 0)
          {
            if(Result<> result = ReadScratchRecords(*scratch, loadBegin, planesToRead, nonstd::span<float32>(block.data(), planesToRead * slice), "3D backward input block"); result.invalid())
            {
              return result;
            }
          }
        }
        Propagate3DBackward(block.data(), nx, ny, static_cast<usize>(hasHalo), loadedPlanes, backwardNeighbors);
        const usize sourceOffset = static_cast<usize>(hasHalo) * slice;
        MaybeNegateValues(nonstd::span<float32>(block.data() + sourceOffset, sourcePlanes * slice));
        if(Result<> result = WritePrimaryValues(sourceBegin * slice, nonstd::span<const float32>(block.data() + sourceOffset, sourcePlanes * slice), "3D backward output block"); result.invalid())
        {
          return result;
        }
        planeEnd = sourceBegin;
        firstBlock = false;
      }
    } catch(const std::bad_alloc& exception)
    {
      return MakeErrorResult(-8672, fmt::format("Fast chamfer distance could not allocate its bounded 3D plane working set: {}", exception.what()));
    }
    return {};
  }

  // itk push: if c > -w0 and c + w < neighbor -> neighbor = c + w (min, positive side); if c < w0 and c - w > neighbor
  // -> neighbor = c - w (max, negative side). No clamp on the written value.
  static void PushChamfer(float32& neighbor, float32 c, float32 w, bool doPos, bool doNeg)
  {
    if(doPos)
    {
      const float32 cand = c + w;
      if(cand < neighbor)
      {
        neighbor = cand;
      }
    }
    if(doNeg)
    {
      const float32 cand = c - w;
      if(cand > neighbor)
      {
        neighbor = cand;
      }
    }
  }

  AbstractDataStore<float32>& m_Store;
  SizeVec3 m_Dims;
  float32 m_MaxDist;
  const std::atomic_bool& m_ShouldCancel;
  const IFilter::MessageHandler& m_MessageHandler;
  usize m_ResidentLimit2D;
  bool m_NegateOutput = false;
};

/**
 * @brief In-place two-pass chamfer propagation of a narrow signed band to an approximate signed distance map, clamped
 * to +-@p maxDist. Resident and OOC paths preserve the same raster arithmetic and update ordering. When @p
 * negateOutput is true, sign inversion is fused into the final backward writes.
 */
inline Result<> ApplyFastChamferDistance(AbstractDataStore<float32>& store, const SizeVec3& dims, float32 maxDist, const std::atomic_bool& shouldCancel, const IFilter::MessageHandler& messageHandler,
                                         bool negateOutput = false)
{
  FastChamferDistance engine(store, dims, maxDist, shouldCancel, messageHandler, detail::k_Chamfer2DResidentLimit, negateOutput);
  return engine();
}
} // namespace nx::core::ImageProcessing
