#pragma once

#include "simplnx/Common/Array.hpp"
#include "simplnx/Common/Result.hpp"
#include "simplnx/Common/Types.hpp"
#include "simplnx/DataStructure/AbstractDataStore.hpp"
#include "simplnx/DataStructure/DataStore.hpp"
#include "simplnx/Filter/IFilter.hpp"
#include "simplnx/Utilities/AlgorithmDispatch.hpp"
#include "simplnx/Utilities/ImageProcessing/RadiusOneStencil2D.hpp"
#include "simplnx/Utilities/ImageProcessing/WorkingMemory.hpp"
#include "simplnx/Utilities/ParallelDataAlgorithm.hpp"
#include "simplnx/Utilities/StringUtilities.hpp"

#include <fmt/format.h>
#include <nonstd/span.hpp>

#include <array>
#include <atomic>
#include <limits>
#include <new>
#include <optional>
#include <utility>
#include <vector>

namespace nx::core::ImageProcessing
{
namespace detail
{
inline bool ZeroCrossingCheckedMultiply(usize left, usize right, usize& product)
{
  if(left != 0 && right > std::numeric_limits<usize>::max() / left)
  {
    return false;
  }
  product = left * right;
  return true;
}

inline bool ZeroCrossingCheckedAdd(usize left, usize right, usize& sum)
{
  if(right > std::numeric_limits<usize>::max() - left)
  {
    return false;
  }
  sum = left + right;
  return true;
}

struct ZeroCrossingResidentMemoryAllocation
{
  CacheMemoryBudgetManager::WorkingMemoryReservation reservation;
  usize requiredBytes = 0;

  [[nodiscard]] bool holdsCompleteState() const noexcept
  {
    return requiredBytes > 0 && reservation.sizeBytes() == requiredBytes;
  }
};

inline bool ShouldUseZeroCrossingResidentState(const SizeVec3& dims)
{
  return dims[2] > 1;
}

template <class T>
Result<usize> CalculateZeroCrossingResidentWorkingMemoryBytes(const SizeVec3& dims)
{
  if(dims[0] == 0 || dims[1] == 0 || dims[2] == 0)
  {
    return {usize{0}};
  }

  usize sliceValues = 0;
  usize volumeValues = 0;
  usize residentValueBytes = 0;
  usize residentBytes = 0;
  usize rollingInputBytes = 0;
  usize rollingValueBytes = 0;
  usize rollingBytes = 0;
  usize requiredBytes = 0;
  if(!ZeroCrossingCheckedMultiply(dims[0], dims[1], sliceValues) || !ZeroCrossingCheckedMultiply(sliceValues, dims[2], volumeValues) ||
     !ZeroCrossingCheckedAdd(sizeof(T), sizeof(uint8), residentValueBytes) || !ZeroCrossingCheckedMultiply(volumeValues, residentValueBytes, residentBytes) ||
     !ZeroCrossingCheckedMultiply(sizeof(T), usize{3}, rollingInputBytes) || !ZeroCrossingCheckedAdd(rollingInputBytes, sizeof(uint8), rollingValueBytes) ||
     !ZeroCrossingCheckedMultiply(sliceValues, rollingValueBytes, rollingBytes) || !ZeroCrossingCheckedAdd(residentBytes, rollingBytes, requiredBytes))
  {
    return MakeErrorResult<usize>(
        -8761, fmt::format("Zero crossing dimensions ({}) and input element size ({} bytes) overflow while sizing the resident input, uint8 output, and rolling-plane working state.",
                           StringUtilities::formatDimensions3D(dims), sizeof(T)));
  }
  return {requiredBytes};
}

template <class T>
Result<ZeroCrossingResidentMemoryAllocation> ReserveZeroCrossingResidentWorkingMemory(const SizeVec3& dims)
{
  auto requiredResult = CalculateZeroCrossingResidentWorkingMemoryBytes<T>(dims);
  if(requiredResult.invalid())
  {
    return ConvertInvalidResult<ZeroCrossingResidentMemoryAllocation>(std::move(requiredResult));
  }
  auto reservation = ReserveWorkingMemory(requiredResult.value(), requiredResult.value());
  return {ZeroCrossingResidentMemoryAllocation{std::move(reservation), requiredResult.value()}};
}

template <class T>
struct ZeroCrossing2DBlockBody
{
  const T* input;
  uint8* output;
  usize dimX;
  usize dimY;
  usize inputXBegin;
  usize inputYBegin;
  usize inputWidth;
  usize outputXBegin;
  usize outputYBegin;
  usize outputWidth;
  uint8 foreground;
  uint8 background;

  void operator()(const Range& range) const
  {
    const int64 maxX = static_cast<int64>(dimX) - 1;
    const int64 maxY = static_cast<int64>(dimY) - 1;
    const auto clamp = [](int64 value, int64 upper) { return value < 0 ? int64{0} : (value > upper ? upper : value); };
    const auto absolute = [](T value) -> T { return value < T{} ? static_cast<T>(-value) : value; };
    for(usize tuple = range.min(); tuple < range.max(); ++tuple)
    {
      const usize localX = tuple % outputWidth;
      const usize localY = tuple / outputWidth;
      const usize x = outputXBegin + localX;
      const usize y = outputYBegin + localY;
      const T center = input[(y - inputYBegin) * inputWidth + (x - inputXBegin)];
      const auto valueAt = [&](int64 neighborX, int64 neighborY) {
        const usize clampedX = static_cast<usize>(clamp(neighborX, maxX));
        const usize clampedY = static_cast<usize>(clamp(neighborY, maxY));
        return input[(clampedY - inputYBegin) * inputWidth + (clampedX - inputXBegin)];
      };
      const auto crosses = [&](T neighbor, bool positiveDirection) {
        const bool signChange = ((center < T{}) && (neighbor > T{})) || ((center > T{}) && (neighbor < T{})) || ((center == T{}) && (neighbor != T{})) || ((center != T{}) && (neighbor == T{}));
        if(!signChange)
        {
          return false;
        }
        const T centerMagnitude = absolute(center);
        const T neighborMagnitude = absolute(neighbor);
        return centerMagnitude < neighborMagnitude || (centerMagnitude == neighborMagnitude && positiveDirection);
      };

      const int64 xi = static_cast<int64>(x);
      const int64 yi = static_cast<int64>(y);
      const bool hit = crosses(valueAt(xi - 1, yi), false) || crosses(valueAt(xi, yi - 1), false) || crosses(valueAt(xi + 1, yi), true) || crosses(valueAt(xi, yi + 1), true);
      output[tuple] = hit ? foreground : background;
    }
  }
};
} // namespace detail

// ITK-free port of ZeroCrossingImageFilter: single streamed pass. For each voxel, output = backgroundValue unless it
// forms a zero-crossing with one of its 2*effDim axial face neighbors (x±1,y±1,z±1), in which case foregroundValue.
// ZeroCrossing test (itkZeroCrossingImageFilter.hxx): sign change between center `c` and neighbor `n` (opposite sides
// of zero; exact-zero on one side & nonzero on the other counts) AND the center is the closer-to-zero side --
// |c| < |n|, or |c| == |n| and `n` is the +stride (positive-direction) neighbor (ITK's `i >= ImageDimension`
// tie-break). |·| in T. ZeroFluxNeumann (edge-clamp) boundary. A true-3-D call with a real OOC endpoint first tries
// one budgeted full-volume transfer in each direction and otherwise uses a rolling 3-plane Z window. True 2D uses
// checked fixed-capacity row blocks or overwide X tiles. `messageHandler` is currently unused (progress deferred);
// kept for facade signature parity.
template <class T>
Result<> ApplyZeroCrossing(const AbstractDataStore<T>& inStore, AbstractDataStore<uint8>& outStore, const SizeVec3& dims, uint8 foregroundValue, uint8 backgroundValue,
                           const std::atomic_bool& shouldCancel, const IFilter::MessageHandler& messageHandler, usize target2DBytes = detail::k_RadiusOneStencil2DTargetBytes)
{
  if(dims[2] == 1)
  {
    return detail::ExecuteRadiusOneStencil2D<T, uint8>(
        inStore, outStore, dims, shouldCancel, target2DBytes, /*workerScratchBytes=*/0, /*maximumWorkers=*/0,
        [&](const T* input, uint8* output, usize inputXBegin, usize inputYBegin, usize inputWidth, usize outputXBegin, usize outputYBegin, usize outputWidth) {
          return detail::ZeroCrossing2DBlockBody<T>{.input = input,
                                                    .output = output,
                                                    .dimX = dims[0],
                                                    .dimY = dims[1],
                                                    .inputXBegin = inputXBegin,
                                                    .inputYBegin = inputYBegin,
                                                    .inputWidth = inputWidth,
                                                    .outputXBegin = outputXBegin,
                                                    .outputYBegin = outputYBegin,
                                                    .outputWidth = outputWidth,
                                                    .foreground = foregroundValue,
                                                    .background = backgroundValue};
        });
  }

  const bool usesOutOfCoreStore = inStore.getStoreType() == IDataStore::StoreType::OutOfCore || outStore.getStoreType() == IDataStore::StoreType::OutOfCore;
  if(usesOutOfCoreStore && !ForceInCoreAlgorithm() && detail::ShouldUseZeroCrossingResidentState(dims))
  {
    auto allocationResult = detail::ReserveZeroCrossingResidentWorkingMemory<T>(dims);
    if(allocationResult.invalid())
    {
      return ConvertInvalidResult<void>(std::move(allocationResult));
    }
    auto allocation = std::move(allocationResult.value());
    if(allocation.holdsCompleteState())
    {
      try
      {
        DataStore<T> residentInput(ShapeType{dims[2], dims[1], dims[0]}, ShapeType{1}, std::nullopt);
        DataStore<uint8> residentOutput(ShapeType{dims[2], dims[1], dims[0]}, ShapeType{1}, std::nullopt);
        if(Result<> result = inStore.copyIntoBuffer(0, residentInput.createSpan()); result.invalid())
        {
          return result;
        }
        if(Result<> result = ApplyZeroCrossing(residentInput, residentOutput, dims, foregroundValue, backgroundValue, shouldCancel, messageHandler, target2DBytes); result.invalid())
        {
          return result;
        }
        if(shouldCancel)
        {
          return {};
        }
        const auto outputSpan = residentOutput.createSpan();
        return outStore.copyFromBuffer(0, nonstd::span<const uint8>(outputSpan.data(), outputSpan.size()));
      } catch(const std::bad_alloc&)
      {
        // Release the complete-state reservation before entering the rolling-plane fallback.
      }
    }
  }

  const int64 nX = static_cast<int64>(dims[0]);
  const int64 nY = static_cast<int64>(dims[1]);
  const int64 nZ = static_cast<int64>(dims[2]);
  const usize slice = static_cast<usize>(nX) * static_cast<usize>(nY);
  const uint32 effDim = (nZ > 1) ? 3u : 2u;

  auto clampi = [](int64 v, int64 hi) { return v < 0 ? int64{0} : (v > hi ? hi : v); };
  auto absT = [](T v) -> T { return v < T{} ? static_cast<T>(-v) : v; }; // itk::Math::abs in T (matches ITK; INT_MIN edge is ITK's too)

  // 3-plane rolling input window: win[0]=z-1, win[1]=z, win[2]=z+1 (clamped). For 2D (nZ==1) all three alias plane 0.
  std::array<std::vector<T>, 3> win;
  for(auto& p : win)
  {
    p.resize(slice);
  }
  std::vector<uint8> outPlane(slice);

  auto loadPlane = [&](std::vector<T>& dst, int64 zWanted) -> Result<> {
    const int64 zc = clampi(zWanted, nZ - 1);
    return inStore.copyIntoBuffer(static_cast<usize>(zc) * slice, nonstd::span<T>(dst.data(), slice));
  };

  for(int64 dz = -1; dz <= 1; ++dz)
  {
    if(Result<> r = loadPlane(win[static_cast<usize>(dz + 1)], dz); r.invalid())
    {
      return r;
    }
  }

  for(int64 z = 0; z < nZ; ++z)
  {
    if(shouldCancel)
    {
      return {};
    }
    const auto& pm = win[0]; // z-1
    const auto& p0 = win[1]; // z
    const auto& pp = win[2]; // z+1
    auto zeroCrossingRows = [&](const Range& rowRange) {
      for(usize yu = rowRange.min(); yu < rowRange.max(); ++yu)
      {
        const int64 y = static_cast<int64>(yu);
        for(int64 x = 0; x < nX; ++x)
        {
          const auto at = [&](const std::vector<T>& pl, int64 xx, int64 yy) { return pl[static_cast<usize>(clampi(yy, nY - 1) * nX + clampi(xx, nX - 1))]; };
          const T c = p0[static_cast<usize>(y * nX + x)];
          const T zero = T{};

          // Test one axial neighbor `n`; `positiveDir` = the neighbor is a +stride (ITK i>=d) neighbor (wins |c|==|n| ties).
          auto crosses = [&](T n, bool positiveDir) -> bool {
            const bool signChange = ((c < zero) && (n > zero)) || ((c > zero) && (n < zero)) || ((c == zero) && (n != zero)) || ((c != zero) && (n == zero));
            if(!signChange)
            {
              return false;
            }
            const T ac = absT(c);
            const T an = absT(n);
            return (ac < an) || (ac == an && positiveDir);
          };

          // ITK order: negX,negY(,negZ) [positiveDir=false] then posX,posY(,posZ) [positiveDir=true]. Any hit -> fg.
          bool hit = crosses(at(p0, x - 1, y), false) || crosses(at(p0, x, y - 1), false);
          if(!hit && effDim == 3)
          {
            hit = crosses(at(pm, x, y), false);
          }
          if(!hit)
          {
            hit = crosses(at(p0, x + 1, y), true) || crosses(at(p0, x, y + 1), true);
          }
          if(!hit && effDim == 3)
          {
            hit = crosses(at(pp, x, y), true);
          }
          outPlane[static_cast<usize>(y * nX + x)] = hit ? foregroundValue : backgroundValue;
        }
      }
    };
    ParallelDataAlgorithm parallelAlgorithm;
    parallelAlgorithm.setRange(0, static_cast<usize>(nY));
    parallelAlgorithm.execute(zeroCrossingRows);
    if(Result<> r = outStore.copyFromBuffer(static_cast<usize>(z) * slice, nonstd::span<const uint8>(outPlane.data(), slice)); r.invalid())
    {
      return r;
    }
    if(z + 1 < nZ)
    {
      std::swap(win[0], win[1]);
      std::swap(win[1], win[2]);
      if(Result<> r = loadPlane(win[2], z + 2); r.invalid())
      {
        return r;
      }
    }
  }
  return {};
}
} // namespace nx::core::ImageProcessing
