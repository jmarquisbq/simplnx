#pragma once

#include "simplnx/Common/Array.hpp"
#include "simplnx/Common/Result.hpp"
#include "simplnx/Common/Types.hpp"
#include "simplnx/DataStructure/AbstractDataStore.hpp"
#include "simplnx/DataStructure/DataStore.hpp"
#include "simplnx/DataStructure/IDataArray.hpp"
#include "simplnx/Filter/IFilter.hpp"
#include "simplnx/Utilities/AlgorithmDispatch.hpp"
#include "simplnx/Utilities/ImageProcessing/WorkingMemory.hpp"
#include "simplnx/Utilities/ParallelDataAlgorithm.hpp"
#include "simplnx/Utilities/StringUtilities.hpp"

#include <fmt/format.h>

#include <nonstd/span.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <type_traits>
#include <vector>

namespace nx::core::ImageProcessing
{
namespace detail
{
constexpr usize k_IsoContour2DResidentLimit = 64ULL * 1024ULL * 1024ULL;
constexpr usize k_IsoContour2DFixedStateBytes = 4096;

// ITK's PixelRealType = NumericTraits<InputPixelType>::RealType: double for every integer type and for float64, float
// for float32. The narrow-band level-set math (and the level-set value itself) is carried in this type; matching it
// per input type is required for bit-exact parity.
template <class T>
using IsoRealType = std::conditional_t<std::is_same_v<T, float32>, float32, float64>;

struct IsoContour2DBufferPlan
{
  usize coreRows = 0;
  usize coreCols = 0;
  usize residentBytes = 0;
  bool valid = false;
  bool overflow = false;
};

inline bool IsoContourCheckedAdd(usize left, usize right, usize& result)
{
  if(right > std::numeric_limits<usize>::max() - left)
  {
    return false;
  }
  result = left + right;
  return true;
}

inline bool IsoContourCheckedMultiply(usize left, usize right, usize& result)
{
  if(left != 0 && right > std::numeric_limits<usize>::max() / left)
  {
    return false;
  }
  result = left * right;
  return true;
}

struct IsoContourResidentMemoryAllocation
{
  CacheMemoryBudgetManager::WorkingMemoryReservation reservation;
  usize requiredBytes = 0;

  [[nodiscard]] bool holdsCompleteState() const noexcept
  {
    return requiredBytes > 0 && reservation.sizeBytes() == requiredBytes;
  }
};

inline bool ShouldUseIsoContourResidentState(const SizeVec3& dims)
{
  return dims[2] > 1;
}

template <class T>
Result<usize> CalculateIsoContourResidentWorkingMemoryBytes(const SizeVec3& dims)
{
  if(dims[0] == 0 || dims[1] == 0 || dims[2] == 0)
  {
    return {usize{0}};
  }

  usize sliceValues = 0;
  usize volumeValues = 0;
  usize bytesPerValue = 0;
  usize requiredBytes = 0;
  if(!IsoContourCheckedMultiply(dims[0], dims[1], sliceValues) || !IsoContourCheckedMultiply(sliceValues, dims[2], volumeValues) || !IsoContourCheckedAdd(sizeof(T), sizeof(float32), bytesPerValue) ||
     !IsoContourCheckedMultiply(volumeValues, bytesPerValue, requiredBytes))
  {
    return MakeErrorResult<usize>(-8626, fmt::format("Iso-contour distance dimensions ({}) and input element size ({} bytes) overflow while sizing the resident input and float32 output state.",
                                                     StringUtilities::formatDimensions3D(dims), sizeof(T)));
  }
  return {requiredBytes};
}

template <class T>
Result<IsoContourResidentMemoryAllocation> ReserveIsoContourResidentWorkingMemory(const SizeVec3& dims)
{
  auto requiredResult = CalculateIsoContourResidentWorkingMemoryBytes<T>(dims);
  if(requiredResult.invalid())
  {
    return ConvertInvalidResult<IsoContourResidentMemoryAllocation>(std::move(requiredResult));
  }
  auto reservation = ReserveWorkingMemory(requiredResult.value(), requiredResult.value());
  return {IsoContourResidentMemoryAllocation{std::move(reservation), requiredResult.value()}};
}

inline bool IsoContour2DFullWidthPeak(usize columns, usize rows, usize inputBytes, usize& peak)
{
  usize inputRows = 0;
  usize inputValues = 0;
  usize outputValues = 0;
  usize input = 0;
  usize output = 0;
  usize total = 0;
  if(!IsoContourCheckedAdd(rows, 4, inputRows) || !IsoContourCheckedMultiply(inputRows, columns, inputValues) || !IsoContourCheckedMultiply(rows, columns, outputValues) ||
     !IsoContourCheckedMultiply(inputValues, inputBytes, input) || !IsoContourCheckedMultiply(outputValues, sizeof(float32), output) || !IsoContourCheckedAdd(input, output, total) ||
     !IsoContourCheckedAdd(total, k_IsoContour2DFixedStateBytes, peak))
  {
    return false;
  }
  return true;
}

inline bool IsoContour2DTiledPeak(usize columns, usize inputBytes, usize& peak)
{
  usize haloColumns = 0;
  usize inputValues = 0;
  usize input = 0;
  usize output = 0;
  usize total = 0;
  if(!IsoContourCheckedAdd(columns, 4, haloColumns) || !IsoContourCheckedMultiply(haloColumns, 5, inputValues) || !IsoContourCheckedMultiply(columns, sizeof(float32), output) ||
     !IsoContourCheckedMultiply(inputValues, inputBytes, input) || !IsoContourCheckedAdd(input, output, total) || !IsoContourCheckedAdd(total, k_IsoContour2DFixedStateBytes, peak))
  {
    return false;
  }
  return true;
}

inline IsoContour2DBufferPlan BuildIsoContour2DBufferPlan(usize nx, usize ny, usize inputBytes, usize residentLimit = k_IsoContour2DResidentLimit)
{
  IsoContour2DBufferPlan plan;
  usize cellCount = 0;
  if(nx == 0 || ny == 0 || inputBytes == 0 || residentLimit == 0 || !IsoContourCheckedMultiply(nx, ny, cellCount))
  {
    plan.overflow = true;
    return plan;
  }

  usize minimumPeak = 0;
  if(!IsoContour2DTiledPeak(1, inputBytes, minimumPeak))
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

  usize oneFullRowPeak = 0;
  if(!IsoContour2DFullWidthPeak(nx, 1, inputBytes, oneFullRowPeak))
  {
    plan.overflow = true;
    return plan;
  }
  if(oneFullRowPeak <= residentLimit)
  {
    plan.coreCols = nx;
    plan.coreRows = largestFitting(ny, [nx, inputBytes](usize rows, usize& candidatePeak) { return IsoContour2DFullWidthPeak(nx, rows, inputBytes, candidatePeak); });
    if(!IsoContour2DFullWidthPeak(plan.coreCols, plan.coreRows, inputBytes, plan.residentBytes))
    {
      plan.overflow = true;
      return plan;
    }
  }
  else
  {
    plan.coreCols = largestFitting(nx, [inputBytes](usize columns, usize& candidatePeak) { return IsoContour2DTiledPeak(columns, inputBytes, candidatePeak); });
    plan.coreRows = 1;
    if(!IsoContour2DTiledPeak(plan.coreCols, inputBytes, plan.residentBytes))
    {
      plan.overflow = true;
      return plan;
    }
  }
  plan.valid = plan.residentBytes <= residentLimit;
  return plan;
}
} // namespace detail

/**
 * @brief ITK-free IsoContourDistance: a narrow-band SIGNED distance around the @p levelSetValue iso-contour of the
 * input. Every voxel is first initialized to +@p farValue (input above the level set), -@p farValue (below), or 0
 * (exactly on it); then a single neighborhood pass writes the sub-pixel gradient-interpolated signed distance for the
 * voxels adjacent to a level-set crossing, keeping the MINIMUM-magnitude value (the nearest crossing). Faithful port
 * of itk::IsoContourDistanceImageFilter's full (non-narrow-band) path, including its ZeroFluxNeumann edge clamping,
 * the grad0(real)/grad1(float32) cast asymmetry, the double-precision magnitude compare, and PixelRealType per type.
 *
 * STORAGE PATHS: exact in-memory stores use a full-span parallel gather with disjoint writes. Real-OOC 3-D stores
 * reuse that gather directly after a complete shared working-memory reservation; otherwise they fall back to a
 * radius-2 rolling Z-window parallel gather (one input plane read and one output plane write per Z step, each output
 * plane computed independently from the window). Real-OOC 2-D stores use bounded row blocks or X tiles. Every path
 * preserves the same interpolation and candidate order.
 */
template <class T>
class IsoContourDistance
{
public:
  using RealT = detail::IsoRealType<T>;

  IsoContourDistance(const AbstractDataStore<T>& inStore, AbstractDataStore<float32>& outStore, SizeVec3 dims, float64 levelSetValue, float64 farValue, FloatVec3 spacing,
                     const std::atomic_bool& shouldCancel, const IFilter::MessageHandler& messageHandler, usize residentLimit2D = detail::k_IsoContour2DResidentLimit)
  : m_In(inStore)
  , m_Out(outStore)
  , m_Dims(dims)
  , m_LevelSet(static_cast<RealT>(levelSetValue))
  , m_FarValue(static_cast<float32>(farValue))
  , m_Spacing(spacing)
  , m_ShouldCancel(shouldCancel)
  , m_MessageHandler(messageHandler)
  , m_ResidentLimit2D(residentLimit2D)
  {
  }
  ~IsoContourDistance() = default;
  IsoContourDistance(const IsoContourDistance&) = delete;
  IsoContourDistance(IsoContourDistance&&) noexcept = delete;
  IsoContourDistance& operator=(const IsoContourDistance&) = delete;
  IsoContourDistance& operator=(IsoContourDistance&&) noexcept = delete;

  Result<> operator()()
  {
    if(m_Dims[0] == 0 || m_Dims[1] == 0 || m_Dims[2] == 0)
    {
      return {};
    }
    if(m_Dims[0] > static_cast<usize>(std::numeric_limits<int64>::max()) || m_Dims[1] > static_cast<usize>(std::numeric_limits<int64>::max()) ||
       m_Dims[2] > static_cast<usize>(std::numeric_limits<int64>::max()))
    {
      return MakeErrorResult(-8620, fmt::format("Iso-contour distance image dimensions exceed the supported signed coordinate range. Dimensions: {} x {} x {}.", m_Dims[0], m_Dims[1], m_Dims[2]));
    }
    auto checkedMultiply = [](usize left, usize right, usize& product) {
      if(left != 0 && right > std::numeric_limits<usize>::max() / left)
      {
        return false;
      }
      product = left * right;
      return true;
    };
    usize slice = 0;
    usize vol = 0;
    if(!checkedMultiply(m_Dims[0], m_Dims[1], slice) || !checkedMultiply(slice, m_Dims[2], vol))
    {
      return MakeErrorResult(-8621, fmt::format("Iso-contour distance image dimensions overflow the addressable value count. Dimensions: {} x {} x {}.", m_Dims[0], m_Dims[1], m_Dims[2]));
    }
    if(m_In.getSize() != vol)
    {
      return MakeErrorResult(-8622, fmt::format("Iso-contour distance input store size does not match the image dimensions. Actual size: {}; expected size: {}; dimensions: {} x {} x {}.",
                                                m_In.getSize(), vol, m_Dims[0], m_Dims[1], m_Dims[2]));
    }
    if(m_Out.getSize() != vol)
    {
      return MakeErrorResult(-8623, fmt::format("Iso-contour distance output store size does not match the image dimensions. Actual size: {}; expected size: {}; dimensions: {} x {} x {}.",
                                                m_Out.getSize(), vol, m_Dims[0], m_Dims[1], m_Dims[2]));
    }
    if(m_ShouldCancel)
    {
      return {};
    }

    const int64 nX = static_cast<int64>(m_Dims[0]);
    const int64 nY = static_cast<int64>(m_Dims[1]);
    const int64 nZ = static_cast<int64>(m_Dims[2]);

    // Spacing is held as DOUBLE, matching ITK's InputSpacingType = Vector<double> (its m_Spacing is double regardless
    // of PixelRealType). This makes the grad-denominator (`2.0 * sp`) and `val` (`... * sp[n] / norm / diff`)
    // expressions promote to double for float32 input as ITK does; the integer/float64 path (RealT == double) is
    // unchanged. This eliminates the spacing-arithmetic divergence on the float32 path (notably for non-unit spacing).
    // NOTE: a residual <=1 ULP difference vs ITK can still remain on the RealT==float (float32-input) path from other
    // float-vs-double interpolation intermediates and, more fundamentally, from ITK's multithreaded min-magnitude
    // tie-breaking (a std::mutex over competing neighbor writes, so the winner among near-equal-magnitude candidates
    // is not bit-reproducible). Integer and float64 inputs (the only ones the ApproximateSignedDistanceMap composite
    // uses) are bit-exact vs ITK; float32 matches within ~1 ULP.
    const double sp[3] = {static_cast<double>(m_Spacing[0]), static_cast<double>(m_Spacing[1]), static_cast<double>(m_Spacing[2])};
    const float32 negFar = -m_FarValue;

    const bool usesOutOfCoreStore = m_In.getStoreType() == IDataStore::StoreType::OutOfCore || m_Out.getStoreType() == IDataStore::StoreType::OutOfCore;
    const bool useStreamed = !ForceInCoreAlgorithm() && (usesOutOfCoreStore || ForceOocAlgorithm());
    RecordAlgorithmPathExecution(useStreamed ? AlgorithmPath::OutOfCore : AlgorithmPath::InCore, usesOutOfCoreStore);
    if(!useStreamed)
    {
      const auto* inMemoryInputStore = dynamic_cast<const DataStore<T>*>(&m_In);
      auto* inMemoryOutputStore = dynamic_cast<DataStore<float32>*>(&m_Out);
      return RunResident(inMemoryInputStore->createSpan(), inMemoryOutputStore->createSpan(), nX, nY, nZ, slice, vol, sp, negFar);
    }

    if(usesOutOfCoreStore && detail::ShouldUseIsoContourResidentState(m_Dims))
    {
      auto allocationResult = detail::ReserveIsoContourResidentWorkingMemory<T>(m_Dims);
      if(allocationResult.invalid())
      {
        return ConvertInvalidResult<void>(std::move(allocationResult));
      }
      auto allocation = std::move(allocationResult.value());
      if(allocation.holdsCompleteState())
      {
        try
        {
          auto input = std::make_unique<T[]>(vol);
          auto output = std::make_unique<float32[]>(vol);
          if(Result<> result = m_In.copyIntoBuffer(0, nonstd::span<T>(input.get(), vol)); result.invalid())
          {
            return result;
          }
          if(Result<> result = RunResident(nonstd::span<const T>(input.get(), vol), nonstd::span<float32>(output.get(), vol), nX, nY, nZ, slice, vol, sp, negFar); result.invalid())
          {
            return result;
          }
          if(m_ShouldCancel)
          {
            return {};
          }
          return m_Out.copyFromBuffer(0, nonstd::span<const float32>(output.get(), vol));
        } catch(const std::bad_alloc&)
        {
          // Release the complete-state reservation before entering the bounded fallback.
        }
      }
    }

    if(usesOutOfCoreStore && nZ == 1)
    {
      return RunBounded2D(static_cast<usize>(nX), static_cast<usize>(nY), sp, negFar);
    }

    return RunStreamed3D(nX, nY, nZ, slice, sp, negFar);
  }

private:
  /**
   * @brief Real-OOC 3-D fallback for when the resident reservation does not fit: a radius-2 rolling Z window over
   * the input (one new input plane read and one output plane write per Z step) whose compute body is
   * Evaluate3DGather's independent per-cell gather, parallelized over each output plane. Radius 2 because
   * Evaluate3DGather's widest edge (the +Z edge, evaluated at z and z+1) reads gradient samples out to z-1 and z+2.
   * Because every cell derives solely from the window (no cross-plane scatter), the output plane for z needs no
   * carry-over state from neighboring z iterations beyond the window itself, unlike a scatter formulation.
   */
  Result<> RunStreamed3D(int64 nX, int64 nY, int64 nZ, usize slice, const double spacing[3], float32 negativeFar)
  {
    // win[dz + 2] holds the input plane at clamp(z + dz, 0, nZ-1), dz in [-2, +2] (dz == -2 is never read at z == 0
    // but kept for uniform indexing/rolling).
    std::array<std::vector<T>, 5> win;
    for(auto& p : win)
    {
      p.resize(slice);
    }
    std::vector<float32> outPlane(slice);

    auto clampi = [](int64 v, int64 hi) { return v < 0 ? int64{0} : (v > hi ? hi : v); };
    std::array<int64, 5> windowZ = {std::numeric_limits<int64>::min(), std::numeric_limits<int64>::min(), std::numeric_limits<int64>::min(), std::numeric_limits<int64>::min(),
                                    std::numeric_limits<int64>::min()};
    auto loadInputPlane = [&](usize slot, int64 zWanted) -> Result<> {
      const int64 zc = clampi(zWanted, nZ - 1);
      for(usize sourceSlot = 0; sourceSlot < windowZ.size(); ++sourceSlot)
      {
        if(sourceSlot != slot && windowZ[sourceSlot] == zc)
        {
          win[slot] = win[sourceSlot];
          windowZ[slot] = zc;
          return {};
        }
      }
      if(Result<> result = m_In.copyIntoBuffer(static_cast<usize>(zc) * slice, nonstd::span<T>(win[slot].data(), slice)); result.invalid())
      {
        return result;
      }
      windowZ[slot] = zc;
      return {};
    };

    // Prime the window for z = 0: planes clamp(-2..2).
    for(int64 dz = -2; dz <= 2; ++dz)
    {
      if(Result<> r = loadInputPlane(static_cast<usize>(dz + 2), dz); r.invalid())
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

      auto processPlane = [&](const Range& range) {
        for(usize tuple = range.min(); tuple < range.max(); ++tuple)
        {
          if(((tuple - range.min()) & 4095ULL) == 0 && m_ShouldCancel)
          {
            return;
          }
          const int64 y = static_cast<int64>(tuple / static_cast<usize>(nX));
          const int64 x = static_cast<int64>(tuple - static_cast<usize>(y) * static_cast<usize>(nX));
          // win[dz+2] = input plane at clamp(z+dz); nX-1/nY-1 clamp the in-plane (x, y) offsets.
          auto sampleReal = [&](int64 deltaX, int64 deltaY, int64 deltaZ) -> RealT {
            const usize idx = static_cast<usize>(clampi(y + deltaY, nY - 1) * nX + clampi(x + deltaX, nX - 1));
            return static_cast<RealT>(win[static_cast<usize>(deltaZ + 2)][idx]);
          };
          auto sampleFloat = [&](int64 deltaX, int64 deltaY, int64 deltaZ) -> float32 {
            const usize idx = static_cast<usize>(clampi(y + deltaY, nY - 1) * nX + clampi(x + deltaX, nX - 1));
            return static_cast<float32>(win[static_cast<usize>(deltaZ + 2)][idx]);
          };
          outPlane[tuple] = Evaluate3DGather(sampleReal, sampleFloat, x, y, z, nX, nY, nZ, spacing, negativeFar);
        }
      };
      ParallelDataAlgorithm parallelAlgorithm;
      parallelAlgorithm.setRange(0, slice);
      parallelAlgorithm.execute(processPlane);

      if(m_ShouldCancel)
      {
        return {};
      }
      if(Result<> r = m_Out.copyFromBuffer(static_cast<usize>(z) * slice, nonstd::span<const float32>(outPlane.data(), slice)); r.invalid())
      {
        return r;
      }

      if((z + 1) < nZ)
      {
        // roll input window: shift left, load the new leading plane clamp(z+3).
        for(int32 i = 0; i < 4; ++i)
        {
          std::swap(win[static_cast<usize>(i)], win[static_cast<usize>(i + 1)]);
          std::swap(windowZ[static_cast<usize>(i)], windowZ[static_cast<usize>(i + 1)]);
        }
        if(Result<> r = loadInputPlane(4, z + 3); r.invalid())
        {
          return r;
        }
      }
    }
    return {};
  }

  Result<> RunResident(nonstd::span<const T> input, nonstd::span<float32> output, int64 nX, int64 nY, int64 nZ, usize slice, usize volume, const double spacing[3], float32 negativeFar)
  {
    auto clampCoordinate = [](int64 value, int64 high) { return value < 0 ? int64{0} : (value > high ? high : value); };
    auto sampleReal = [&](int64 x, int64 y, int64 z) -> RealT {
      const usize index = static_cast<usize>((clampCoordinate(z, nZ - 1) * nY + clampCoordinate(y, nY - 1)) * nX + clampCoordinate(x, nX - 1));
      return static_cast<RealT>(input[index]);
    };
    auto sampleFloat = [&](int64 x, int64 y, int64 z) -> float32 {
      const usize index = static_cast<usize>((clampCoordinate(z, nZ - 1) * nY + clampCoordinate(y, nY - 1)) * nX + clampCoordinate(x, nX - 1));
      return static_cast<float32>(input[index]);
    };
    const bool is2D = nZ == 1;

    auto processValues = [&](const Range& range) {
      for(usize tuple = range.min(); tuple < range.max(); ++tuple)
      {
        if(((tuple - range.min()) & 4095ULL) == 0 && m_ShouldCancel)
        {
          return;
        }
        const int64 z = is2D ? 0 : static_cast<int64>(tuple / slice);
        const usize planeIndex = is2D ? tuple : tuple - static_cast<usize>(z) * slice;
        const int64 y = static_cast<int64>(planeIndex / m_Dims[0]);
        const int64 x = static_cast<int64>(planeIndex - static_cast<usize>(y) * m_Dims[0]);
        const RealT centerValue = sampleReal(x, y, z);
        float32 best = centerValue > m_LevelSet ? m_FarValue : (centerValue < m_LevelSet ? negativeFar : 0.0f);

        auto evaluateEdge = [&](int64 edgeX, int64 edgeY, int64 edgeZ, int32 axis, bool targetIsFirst) {
          int64 neighborX = edgeX;
          int64 neighborY = edgeY;
          int64 neighborZ = edgeZ;
          if(axis == 0)
          {
            ++neighborX;
          }
          else if(axis == 1)
          {
            ++neighborY;
          }
          else
          {
            ++neighborZ;
          }

          const RealT val0 = sampleReal(edgeX, edgeY, edgeZ) - m_LevelSet;
          const RealT val1 = sampleReal(neighborX, neighborY, neighborZ) - m_LevelSet;
          const bool sign = val0 > RealT{0};
          if(sign == (val1 > RealT{0}))
          {
            return;
          }

          RealT gradient0[3];
          RealT gradient1[3];
          for(int32 gradientAxis = 0; gradientAxis < 3; ++gradientAxis)
          {
            const int64 dx = gradientAxis == 0 ? 1 : 0;
            const int64 dy = gradientAxis == 1 ? 1 : 0;
            const int64 dz = gradientAxis == 2 ? 1 : 0;
            gradient0[gradientAxis] = sampleReal(edgeX + dx, edgeY + dy, edgeZ + dz) - sampleReal(edgeX - dx, edgeY - dy, edgeZ - dz);
            const float32 positive = sampleFloat(neighborX + dx, neighborY + dy, neighborZ + dz);
            const float32 negative = sampleFloat(neighborX - dx, neighborY - dy, neighborZ - dz);
            gradient1[gradientAxis] = static_cast<RealT>(positive - negative);
          }

          const RealT difference = sign ? (val0 - val1) : (val1 - val0);
          if(difference < std::numeric_limits<RealT>::min())
          {
            return;
          }
          RealT gradient[3];
          RealT norm = RealT{0};
          for(int32 gradientAxis = 0; gradientAxis < 3; ++gradientAxis)
          {
            gradient[gradientAxis] = (gradient0[gradientAxis] * RealT{0.5} + gradient1[gradientAxis] * RealT{0.5}) / (RealT{2} * spacing[gradientAxis]);
            norm += gradient[gradientAxis] * gradient[gradientAxis];
          }
          norm = std::sqrt(norm);
          if(norm < std::numeric_limits<RealT>::min())
          {
            return;
          }

          const RealT scale = std::abs(gradient[axis]) * spacing[axis] / norm / difference;
          const float32 candidate = static_cast<float32>((targetIsFirst ? val0 : val1) * scale);
          if(std::abs(static_cast<double>(candidate)) < std::abs(static_cast<double>(best)))
          {
            best = candidate;
          }
        };

        // Match the serial scatter's arrival order for strict min-magnitude ties: prior Z, Y, and X edges,
        // followed by this voxel's positive X, Y, and Z edges.
        if(z > 0)
        {
          evaluateEdge(x, y, z - 1, 2, false);
        }
        if(y > 0)
        {
          evaluateEdge(x, y - 1, z, 1, false);
        }
        if(x > 0)
        {
          evaluateEdge(x - 1, y, z, 0, false);
        }
        if(x + 1 < nX)
        {
          evaluateEdge(x, y, z, 0, true);
        }
        if(y + 1 < nY)
        {
          evaluateEdge(x, y, z, 1, true);
        }
        if(z + 1 < nZ)
        {
          evaluateEdge(x, y, z, 2, true);
        }
        output[tuple] = best;
      }
    };
    ParallelDataAlgorithm parallelAlgorithm;
    parallelAlgorithm.setRange(0, volume);
    parallelAlgorithm.execute(processValues);
    return {};
  }

  template <class SampleReal, class SampleFloat>
  float32 Evaluate2DGather(SampleReal&& sampleReal, SampleFloat&& sampleFloat, int64 x, int64 y, int64 nx, int64 ny, const double spacing[3], float32 negativeFar) const
  {
    const RealT centerValue = sampleReal(0, 0);
    float32 best = centerValue > m_LevelSet ? m_FarValue : (centerValue < m_LevelSet ? negativeFar : 0.0f);
    auto evaluateEdge = [&](int64 edgeX, int64 edgeY, int32 axis, bool targetIsFirst) {
      const int64 neighborX = edgeX + (axis == 0 ? 1 : 0);
      const int64 neighborY = edgeY + (axis == 1 ? 1 : 0);
      const RealT val0 = sampleReal(edgeX, edgeY) - m_LevelSet;
      const RealT val1 = sampleReal(neighborX, neighborY) - m_LevelSet;
      const bool sign = val0 > RealT{0};
      if(sign == (val1 > RealT{0}))
      {
        return;
      }

      RealT grad0[3];
      RealT grad1[3];
      for(int32 gradientAxis = 0; gradientAxis < 3; ++gradientAxis)
      {
        const int64 gradientX = gradientAxis == 0 ? 1 : 0;
        const int64 gradientY = gradientAxis == 1 ? 1 : 0;
        grad0[gradientAxis] = sampleReal(edgeX + gradientX, edgeY + gradientY) - sampleReal(edgeX - gradientX, edgeY - gradientY);
        const float32 positive = sampleFloat(neighborX + gradientX, neighborY + gradientY);
        const float32 negative = sampleFloat(neighborX - gradientX, neighborY - gradientY);
        grad1[gradientAxis] = static_cast<RealT>(positive - negative);
      }

      const RealT difference = sign ? (val0 - val1) : (val1 - val0);
      if(difference < std::numeric_limits<RealT>::min())
      {
        return;
      }
      RealT gradient[3];
      RealT norm = RealT{0};
      for(int32 gradientAxis = 0; gradientAxis < 3; ++gradientAxis)
      {
        gradient[gradientAxis] = (grad0[gradientAxis] * RealT{0.5} + grad1[gradientAxis] * RealT{0.5}) / (RealT{2} * spacing[gradientAxis]);
        norm += gradient[gradientAxis] * gradient[gradientAxis];
      }
      norm = std::sqrt(norm);
      if(norm < std::numeric_limits<RealT>::min())
      {
        return;
      }
      const RealT scale = std::abs(gradient[axis]) * spacing[axis] / norm / difference;
      const RealT candidate = (targetIsFirst ? val0 : val1) * scale;
      UpdateMinimum(best, candidate);
    };

    if(y > 0)
    {
      evaluateEdge(0, -1, 1, false);
    }
    if(x > 0)
    {
      evaluateEdge(-1, 0, 0, false);
    }
    if(x + 1 < nx)
    {
      evaluateEdge(0, 0, 0, true);
    }
    if(y + 1 < ny)
    {
      evaluateEdge(0, 0, 1, true);
    }
    return best;
  }

  static void UpdateMinimum(float32& target, RealT candidate)
  {
    if(std::abs(static_cast<double>(candidate)) < std::abs(static_cast<double>(target)))
    {
      target = static_cast<float32>(candidate);
    }
  }

  /**
   * @brief Independent per-cell gather for a single 3-D voxel (x, y, z), using the exact edge set, evaluation
   * order, and tie-breaking as RunResident's kernel: prior Z, Y, and X edges (this voxel as the second/neighbor
   * point of the edge), followed by this voxel's own positive X, Y, and Z edges (this voxel as the first point).
   * Because every candidate write is a strict less-than minimum-magnitude update, the result is independent of the
   * order candidates are combined -- EXCEPT for exact-magnitude ties, where the first candidate written wins; this
   * fixed six-edge sequence is what pins ties to match RunResident (and, transitively, the prior rolling-scatter
   * formulation this gather replaces -- see RunStreamed3D's Doxygen for why the two are equivalent). Samplers take
   * offsets relative to (x, y, z) rather than absolute coordinates, so the same body serves both a full in-core
   * array and RunStreamed3D's radius-2 rolling window -- only the sampler differs per caller.
   */
  template <class SampleReal, class SampleFloat>
  float32 Evaluate3DGather(SampleReal&& sampleReal, SampleFloat&& sampleFloat, int64 x, int64 y, int64 z, int64 nx, int64 ny, int64 nz, const double spacing[3], float32 negativeFar) const
  {
    const RealT centerValue = sampleReal(0, 0, 0);
    float32 best = centerValue > m_LevelSet ? m_FarValue : (centerValue < m_LevelSet ? negativeFar : 0.0f);

    auto evaluateEdge = [&](int64 edgeX, int64 edgeY, int64 edgeZ, int32 axis, bool targetIsFirst) {
      const int64 neighborX = edgeX + (axis == 0 ? 1 : 0);
      const int64 neighborY = edgeY + (axis == 1 ? 1 : 0);
      const int64 neighborZ = edgeZ + (axis == 2 ? 1 : 0);
      const RealT val0 = sampleReal(edgeX, edgeY, edgeZ) - m_LevelSet;
      const RealT val1 = sampleReal(neighborX, neighborY, neighborZ) - m_LevelSet;
      const bool sign = val0 > RealT{0};
      if(sign == (val1 > RealT{0}))
      {
        return;
      }

      RealT gradient0[3];
      RealT gradient1[3];
      for(int32 gradientAxis = 0; gradientAxis < 3; ++gradientAxis)
      {
        const int64 gradientX = gradientAxis == 0 ? 1 : 0;
        const int64 gradientY = gradientAxis == 1 ? 1 : 0;
        const int64 gradientZ = gradientAxis == 2 ? 1 : 0;
        gradient0[gradientAxis] = sampleReal(edgeX + gradientX, edgeY + gradientY, edgeZ + gradientZ) - sampleReal(edgeX - gradientX, edgeY - gradientY, edgeZ - gradientZ);
        const float32 positive = sampleFloat(neighborX + gradientX, neighborY + gradientY, neighborZ + gradientZ);
        const float32 negative = sampleFloat(neighborX - gradientX, neighborY - gradientY, neighborZ - gradientZ);
        gradient1[gradientAxis] = static_cast<RealT>(positive - negative);
      }

      const RealT difference = sign ? (val0 - val1) : (val1 - val0);
      if(difference < std::numeric_limits<RealT>::min())
      {
        return;
      }
      RealT gradient[3];
      RealT norm = RealT{0};
      for(int32 gradientAxis = 0; gradientAxis < 3; ++gradientAxis)
      {
        gradient[gradientAxis] = (gradient0[gradientAxis] * RealT{0.5} + gradient1[gradientAxis] * RealT{0.5}) / (RealT{2} * spacing[gradientAxis]);
        norm += gradient[gradientAxis] * gradient[gradientAxis];
      }
      norm = std::sqrt(norm);
      if(norm < std::numeric_limits<RealT>::min())
      {
        return;
      }
      const RealT scale = std::abs(gradient[axis]) * spacing[axis] / norm / difference;
      const RealT candidate = (targetIsFirst ? val0 : val1) * scale;
      UpdateMinimum(best, candidate);
    };

    if(z > 0)
    {
      evaluateEdge(0, 0, -1, 2, false);
    }
    if(y > 0)
    {
      evaluateEdge(0, -1, 0, 1, false);
    }
    if(x > 0)
    {
      evaluateEdge(-1, 0, 0, 0, false);
    }
    if(x + 1 < nx)
    {
      evaluateEdge(0, 0, 0, 0, true);
    }
    if(y + 1 < ny)
    {
      evaluateEdge(0, 0, 0, 1, true);
    }
    if(z + 1 < nz)
    {
      evaluateEdge(0, 0, 0, 2, true);
    }
    return best;
  }

  Result<> RunBounded2DFullWidth(usize nx, usize ny, usize coreRows, const double spacing[3], float32 negativeFar)
  {
    std::vector<T> inputBlock((coreRows + 4) * nx);
    std::vector<float32> outputBlock(coreRows * nx);

    for(usize yBegin = 0; yBegin < ny; yBegin += coreRows)
    {
      if(m_ShouldCancel)
      {
        return {};
      }
      const usize rowCount = std::min(coreRows, ny - yBegin);
      const usize yEnd = yBegin + rowCount;
      const usize loadBegin = yBegin > 1 ? yBegin - 2 : 0;
      const usize loadEnd = std::min(ny, yEnd + 2);
      const usize loadRows = loadEnd - loadBegin;
      if(Result<> result = m_In.copyIntoBuffer(loadBegin * nx, nonstd::span<T>(inputBlock.data(), loadRows * nx)); result.invalid())
      {
        return result;
      }

      const usize coreCellCount = rowCount * nx;
      auto processBlock = [&](const Range& range) {
        for(usize localIndex = range.min(); localIndex < range.max(); ++localIndex)
        {
          const usize localRow = localIndex / nx;
          const int64 globalY = static_cast<int64>(yBegin + localRow);
          const int64 x = static_cast<int64>(localIndex - localRow * nx);
          auto sampleReal = [&](int64 deltaX, int64 deltaY) -> RealT {
            const int64 sampleX = std::clamp<int64>(x + deltaX, 0, static_cast<int64>(nx) - 1);
            const int64 sampleY = std::clamp<int64>(globalY + deltaY, 0, static_cast<int64>(ny) - 1);
            return static_cast<RealT>(inputBlock[(static_cast<usize>(sampleY) - loadBegin) * nx + static_cast<usize>(sampleX)]);
          };
          auto sampleFloat = [&](int64 deltaX, int64 deltaY) -> float32 {
            const int64 sampleX = std::clamp<int64>(x + deltaX, 0, static_cast<int64>(nx) - 1);
            const int64 sampleY = std::clamp<int64>(globalY + deltaY, 0, static_cast<int64>(ny) - 1);
            return static_cast<float32>(inputBlock[(static_cast<usize>(sampleY) - loadBegin) * nx + static_cast<usize>(sampleX)]);
          };
          outputBlock[localIndex] = Evaluate2DGather(sampleReal, sampleFloat, x, globalY, static_cast<int64>(nx), static_cast<int64>(ny), spacing, negativeFar);
        }
      };
      ParallelDataAlgorithm parallelAlgorithm;
      parallelAlgorithm.setRange(0, coreCellCount);
      parallelAlgorithm.execute(processBlock);

      if(Result<> result = m_Out.copyFromBuffer(yBegin * nx, nonstd::span<const float32>(outputBlock.data(), coreCellCount)); result.invalid())
      {
        return result;
      }
    }
    return {};
  }

  Result<> RunBounded2DTiled(usize nx, usize ny, usize coreCols, const double spacing[3], float32 negativeFar)
  {
    const usize inputStride = coreCols + 4;
    std::vector<T> inputWindow(5 * inputStride);
    std::vector<float32> output(coreCols);
    for(usize y = 0; y < ny; ++y)
    {
      for(usize xBegin = 0; xBegin < nx; xBegin += coreCols)
      {
        if(m_ShouldCancel)
        {
          return {};
        }
        const usize columnCount = std::min(coreCols, nx - xBegin);
        const usize xEnd = xBegin + columnCount;
        const usize haloBegin = xBegin > 2 ? xBegin - 2 : 0;
        const usize haloEnd = std::min(nx, xEnd + 2);
        const usize haloCount = haloEnd - haloBegin;
        for(int64 rowOffset = -2; rowOffset <= 2; ++rowOffset)
        {
          const usize sampleY = static_cast<usize>(std::clamp<int64>(static_cast<int64>(y) + rowOffset, 0, static_cast<int64>(ny) - 1));
          if(Result<> result = m_In.copyIntoBuffer(sampleY * nx + haloBegin, nonstd::span<T>(inputWindow.data() + static_cast<usize>(rowOffset + 2) * inputStride, haloCount)); result.invalid())
          {
            return result;
          }
        }

        auto processColumns = [&](const Range& range) {
          for(usize localX = range.min(); localX < range.max(); ++localX)
          {
            const int64 globalX = static_cast<int64>(xBegin + localX);
            auto sampleReal = [&](int64 deltaX, int64 deltaY) -> RealT {
              const usize sampleX = static_cast<usize>(std::clamp<int64>(globalX + deltaX, 0, static_cast<int64>(nx) - 1));
              const usize inputRow = static_cast<usize>(deltaY + 2);
              return static_cast<RealT>(inputWindow[inputRow * inputStride + sampleX - haloBegin]);
            };
            auto sampleFloat = [&](int64 deltaX, int64 deltaY) -> float32 {
              const usize sampleX = static_cast<usize>(std::clamp<int64>(globalX + deltaX, 0, static_cast<int64>(nx) - 1));
              const usize inputRow = static_cast<usize>(deltaY + 2);
              return static_cast<float32>(inputWindow[inputRow * inputStride + sampleX - haloBegin]);
            };
            output[localX] = Evaluate2DGather(sampleReal, sampleFloat, globalX, static_cast<int64>(y), static_cast<int64>(nx), static_cast<int64>(ny), spacing, negativeFar);
          }
        };
        ParallelDataAlgorithm parallelAlgorithm;
        parallelAlgorithm.setRange(0, columnCount);
        parallelAlgorithm.execute(processColumns);

        if(Result<> result = m_Out.copyFromBuffer(y * nx + xBegin, nonstd::span<const float32>(output.data(), columnCount)); result.invalid())
        {
          return result;
        }
      }
    }
    return {};
  }

  Result<> RunBounded2D(usize nx, usize ny, const double spacing[3], float32 negativeFar)
  {
    const detail::IsoContour2DBufferPlan plan = detail::BuildIsoContour2DBufferPlan(nx, ny, sizeof(T), m_ResidentLimit2D);
    if(plan.overflow)
    {
      return MakeErrorResult(-8624, fmt::format("Iso-contour distance 2D buffer plan cannot represent dimensions {} x {} due to arithmetic overflow.", nx, ny));
    }
    if(!plan.valid)
    {
      return MakeErrorResult(
          -8625, fmt::format("Iso-contour distance 2D buffer plan exceeds the {}-byte resident limit for dimensions {} x {} and {}-byte input values.", m_ResidentLimit2D, nx, ny, sizeof(T)));
    }
    if(plan.coreCols == nx)
    {
      usize blockRows = plan.coreRows;
      const std::optional<ShapeType> outputChunkShape = m_Out.getChunkShape();
      if(outputChunkShape.has_value() && outputChunkShape->size() >= 3 && (*outputChunkShape)[0] == 1 && (*outputChunkShape)[1] > 0 && (*outputChunkShape)[1] <= blockRows &&
         (*outputChunkShape)[2] == nx)
      {
        blockRows = (blockRows / (*outputChunkShape)[1]) * (*outputChunkShape)[1];
      }
      return RunBounded2DFullWidth(nx, ny, blockRows, spacing, negativeFar);
    }
    return RunBounded2DTiled(nx, ny, plan.coreCols, spacing, negativeFar);
  }

  const AbstractDataStore<T>& m_In;
  AbstractDataStore<float32>& m_Out;
  SizeVec3 m_Dims;
  RealT m_LevelSet;
  float32 m_FarValue;
  FloatVec3 m_Spacing;
  const std::atomic_bool& m_ShouldCancel;
  const IFilter::MessageHandler& m_MessageHandler;
  usize m_ResidentLimit2D = detail::k_IsoContour2DResidentLimit;
};

/**
 * @brief IsoContour narrow-band signed distance. Exact in-memory stores use a direct parallel gather; real-OOC 2-D
 * stores use a bounded parallel gather; real-OOC 3-D stores use a rolling-window parallel gather. Output is fixed
 * float32.
 */
template <class T>
Result<> ApplyIsoContourDistance(const AbstractDataStore<T>& inStore, AbstractDataStore<float32>& outStore, const SizeVec3& dims, float64 levelSetValue, float64 farValue, FloatVec3 spacing,
                                 const std::atomic_bool& shouldCancel, const IFilter::MessageHandler& messageHandler)
{
  IsoContourDistance<T> engine(inStore, outStore, dims, levelSetValue, farValue, spacing, shouldCancel, messageHandler);
  return engine();
}
} // namespace nx::core::ImageProcessing
