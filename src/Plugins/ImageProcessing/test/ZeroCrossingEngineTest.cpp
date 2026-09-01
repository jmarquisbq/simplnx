#include "simplnx/Utilities/ImageProcessing/ZeroCrossingEngine.hpp"

#include "simplnx/Common/Array.hpp"
#include "simplnx/Common/Types.hpp"
#include "simplnx/DataStructure/DataStore.hpp"
#include "simplnx/Filter/IFilter.hpp"
#include "simplnx/UnitTest/UnitTestCommon.hpp"
#include "simplnx/Utilities/CacheMemoryBudgetManager.hpp"

#include <catch2/catch.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <vector>

using namespace nx::core;
using namespace nx::core::ImageProcessing;
namespace zero_detail = nx::core::ImageProcessing::detail;

namespace
{
usize FlatIndex(usize x, usize y, usize z, usize dimX, usize dimY)
{
  return (z * dimY + y) * dimX + x;
}

template <class T>
class TransferCountingDataStore : public DataStore<T>
{
public:
  using DataStore<T>::DataStore;

  Result<> copyIntoBuffer(usize startIndex, nonstd::span<T> buffer) const override
  {
    m_MaxReadValues = std::max(m_MaxReadValues, buffer.size());
    m_ReadCount++;
    return DataStore<T>::copyIntoBuffer(startIndex, buffer);
  }

  Result<> copyFromBuffer(usize startIndex, nonstd::span<const T> buffer) override
  {
    m_MaxWriteValues = std::max(m_MaxWriteValues, buffer.size());
    m_WrittenValues += buffer.size();
    return DataStore<T>::copyFromBuffer(startIndex, buffer);
  }

  usize maxReadValues() const noexcept
  {
    return m_MaxReadValues;
  }

  usize readCount() const noexcept
  {
    return m_ReadCount;
  }

  usize maxWriteValues() const noexcept
  {
    return m_MaxWriteValues;
  }

  usize writtenValues() const noexcept
  {
    return m_WrittenValues;
  }

private:
  mutable usize m_MaxReadValues = 0;
  mutable usize m_ReadCount = 0;
  usize m_MaxWriteValues = 0;
  usize m_WrittenValues = 0;
};

template <class T>
class OutOfCoreTransferCountingDataStore : public TransferCountingDataStore<T>
{
public:
  using TransferCountingDataStore<T>::TransferCountingDataStore;

  IDataStore::StoreType getStoreType() const override
  {
    return IDataStore::StoreType::OutOfCore;
  }
};

template <class T>
std::vector<uint8> ZeroCrossing2DOracle(const std::vector<T>& field, usize dimX, usize dimY, uint8 foreground, uint8 background)
{
  std::vector<uint8> output(field.size(), background);
  const auto clamp = [](int64 value, int64 upper) { return value < 0 ? int64{0} : (value > upper ? upper : value); };
  const auto absolute = [](T value) -> T { return value < T{} ? static_cast<T>(-value) : value; };
  const int64 maxX = static_cast<int64>(dimX) - 1;
  const int64 maxY = static_cast<int64>(dimY) - 1;
  for(usize y = 0; y < dimY; ++y)
  {
    for(usize x = 0; x < dimX; ++x)
    {
      const T center = field[y * dimX + x];
      const auto valueAt = [&](int64 neighborX, int64 neighborY) { return field[static_cast<usize>(clamp(neighborY, maxY)) * dimX + static_cast<usize>(clamp(neighborX, maxX))]; };
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
      output[y * dimX + x] = hit ? foreground : background;
    }
  }
  return output;
}

// Run the engine on a signed field. fg=1, bg=0. Returns the uint8 marked image.
template <class T>
std::vector<uint8> RunZeroCrossing(const std::vector<T>& field, usize dx, usize dy, usize dz)
{
  DataStore<T> inStore(ShapeType{dz, dy, dx}, ShapeType{1}, T{});
  for(usize i = 0; i < field.size(); ++i)
  {
    inStore.setValue(i, field[i]);
  }
  DataStore<uint8> outStore(ShapeType{dz, dy, dx}, ShapeType{1}, uint8{0});
  std::atomic_bool shouldCancel{false};
  IFilter::MessageHandler messageHandler{};
  const Result<> r = ApplyZeroCrossing<T>(inStore, outStore, SizeVec3{dx, dy, dz}, /*fg=*/1, /*bg=*/0, shouldCancel, messageHandler);
  REQUIRE(r.valid());
  std::vector<uint8> out(field.size());
  for(usize i = 0; i < out.size(); ++i)
  {
    out[i] = outStore.getValue(i);
  }
  return out;
}
} // namespace

TEST_CASE("ImageProcessing::ZeroCrossingEngine: a 1D sign flip marks the closer-to-zero side", "[ImageProcessing][ZeroCrossingEngine]")
{
  // Row: -2 -1 1 2. Crossing between x=1 (val -1) and x=2 (val 1). |−1|==|1| -> tie -> the POSITIVE-direction neighbor
  // wins, so x=1 (whose +x neighbor is x=2) is marked; x=2's crossing neighbor is x=1 (negative direction) with equal
  // magnitude -> NOT marked by that tie. So only x=1 is a zero crossing.
  const std::vector<int32> field = {-2, -1, 1, 2};
  const std::vector<uint8> out = RunZeroCrossing<int32>(field, 4, 1, 1);
  REQUIRE(out[0] == 0u);
  REQUIRE(out[1] == 1u);
  REQUIRE(out[2] == 0u);
  REQUIRE(out[3] == 0u);
}

TEST_CASE("ImageProcessing::ZeroCrossingEngine: unequal magnitudes mark the smaller-|.| side on both directions", "[ImageProcessing][ZeroCrossingEngine]")
{
  // Row: -1 3. Crossing between them; |−1| < |3| -> x=0 marked (its +x neighbor 3). x=1 (|3|) is not closer -> not marked.
  const std::vector<int32> field = {-1, 3};
  const std::vector<uint8> out = RunZeroCrossing<int32>(field, 2, 1, 1);
  REQUIRE(out[0] == 1u);
  REQUIRE(out[1] == 0u);
}

TEST_CASE("ImageProcessing::ZeroCrossingEngine: exact-zero pixel adjacent to nonzero is a crossing", "[ImageProcessing][ZeroCrossingEngine]")
{
  // Row: 0 5. |0| < |5| -> x=0 (the zero) is marked (exact-zero vs nonzero counts as a sign change).
  const std::vector<int32> field = {0, 5};
  const std::vector<uint8> out = RunZeroCrossing<int32>(field, 2, 1, 1);
  REQUIRE(out[0] == 1u);
  REQUIRE(out[1] == 0u);
}

TEST_CASE("ImageProcessing::ZeroCrossingEngine: no crossing in a monotone-positive field", "[ImageProcessing][ZeroCrossingEngine]")
{
  const std::vector<int32> field = {1, 2, 3, 4};
  const std::vector<uint8> out = RunZeroCrossing<int32>(field, 4, 1, 1);
  for(uint8 v : out)
  {
    REQUIRE(v == 0u);
  }
}

TEST_CASE("ImageProcessing::ZeroCrossingEngine: determinism + tall-Z analytic check (float plane crossing)", "[ImageProcessing][ZeroCrossingEngine]")
{
  const usize dx = 5, dy = 5, dz = 30;
  std::vector<float32> field(dx * dy * dz);
  for(usize z = 0; z < dz; ++z)
  {
    for(usize y = 0; y < dy; ++y)
    {
      for(usize x = 0; x < dx; ++x)
      {
        // signed distance from the plane z==15: crosses zero across the z-window (exercises the rolling z planes).
        field[FlatIndex(x, y, z, dx, dy)] = static_cast<float32>(static_cast<int64>(z) - 15);
      }
    }
  }
  const std::vector<uint8> a = RunZeroCrossing<float32>(field, dx, dy, dz);
  const std::vector<uint8> b = RunZeroCrossing<float32>(field, dx, dy, dz);
  REQUIRE(a == b);
  // field(z) = z - 15, so only z-direction neighbors differ (in-plane neighbors are equal -> no crossing). The single
  // marked plane is z==15 (val 0): its z-1 neighbor (z=14, val -1) and z+1 neighbor (z=16, val 1) each form a sign
  // change with the exact-zero center and |0| < |±1|. z=14 (val -1) is NOT marked -- vs z=15 (0) the center is not the
  // closer side (|-1| > |0|), and vs z=13 (-2) there is no sign change; likewise z=16. So exactly plane z==15 is foreground.
  for(usize z = 0; z < dz; ++z)
  {
    const uint8 expected = (z == 15) ? 1u : 0u;
    for(usize y = 0; y < dy; ++y)
    {
      for(usize x = 0; x < dx; ++x)
      {
        REQUIRE(a[FlatIndex(x, y, z, dx, dy)] == expected);
      }
    }
  }
}

TEST_CASE("ImageProcessing::ZeroCrossingEngine: resident state requires a complete dataset-scaled reservation", "[ImageProcessing][ZeroCrossingEngine][WorkingMemory]")
{
  constexpr usize dimX = 512;
  constexpr usize dimY = 512;
  constexpr usize dimZ = 128;
  constexpr usize sliceValues = dimX * dimY;
  constexpr usize valueCount = sliceValues * dimZ;
  constexpr uint64 k_MiB = 1024ULL * 1024ULL;
  const SizeVec3 dims{dimX, dimY, dimZ};

  auto requiredResult = zero_detail::CalculateZeroCrossingResidentWorkingMemoryBytes<float32>(dims);
  SIMPLNX_RESULT_REQUIRE_VALID(requiredResult);
  REQUIRE(requiredResult.value() == valueCount * (sizeof(float32) + sizeof(uint8)) + sliceValues * (3 * sizeof(float32) + sizeof(uint8)));
  REQUIRE(requiredResult.value() == 163 * k_MiB + 256ULL * 1024ULL);
  SIMPLNX_RESULT_REQUIRE_INVALID(zero_detail::CalculateZeroCrossingResidentWorkingMemoryBytes<float64>(SizeVec3{std::numeric_limits<usize>::max(), 2, 2}));

  REQUIRE(zero_detail::ShouldUseZeroCrossingResidentState(dims));
  REQUIRE_FALSE(zero_detail::ShouldUseZeroCrossingResidentState(SizeVec3{dimX, dimY, 1}));

  auto& manager = CacheMemoryBudgetManager::instance();
  const uint64 previousBudget = manager.budgetBytes();
  manager.clear();
  manager.setBudgetBytes(512 * k_MiB);
  {
    auto allocationResult = zero_detail::ReserveZeroCrossingResidentWorkingMemory<float32>(dims);
    SIMPLNX_RESULT_REQUIRE_VALID(allocationResult);
    REQUIRE_FALSE(allocationResult.value().holdsCompleteState());
    REQUIRE(allocationResult.value().reservation.sizeBytes() == 128 * k_MiB);
  }
  REQUIRE(manager.reservedWorkingMemoryBytes() == 0);

  manager.setBudgetBytes(1024 * k_MiB);
  {
    auto allocationResult = zero_detail::ReserveZeroCrossingResidentWorkingMemory<float32>(dims);
    SIMPLNX_RESULT_REQUIRE_VALID(allocationResult);
    REQUIRE(allocationResult.value().holdsCompleteState());
    REQUIRE(allocationResult.value().reservation.sizeBytes() == requiredResult.value());
  }
  REQUIRE(manager.reservedWorkingMemoryBytes() == 0);
  manager.setBudgetBytes(previousBudget);
}

TEST_CASE("ImageProcessing::ZeroCrossingEngine: real-OOC selector uses resident state only after a complete grant", "[ImageProcessing][ZeroCrossingEngine][WorkingMemory]")
{
  constexpr usize dimX = 5;
  constexpr usize dimY = 6;
  constexpr usize dimZ = 7;
  constexpr uint64 k_PartialBudgetBytes = 4096;
  constexpr uint64 k_CompleteBudgetBytes = 8192;
  const SizeVec3 dims{dimX, dimY, dimZ};
  std::vector<float32> field(dimX * dimY * dimZ);
  for(usize index = 0; index < field.size(); ++index)
  {
    field[index] = static_cast<float32>(static_cast<int32>((index * 19 + 3) % 127) - 63);
  }
  const std::vector<uint8> expected = RunZeroCrossing<float32>(field, dimX, dimY, dimZ);

  auto& manager = CacheMemoryBudgetManager::instance();
  const uint64 previousBudget = manager.budgetBytes();

  auto run = [&](uint64 budgetBytes) {
    manager.clear();
    manager.setBudgetBytes(budgetBytes);
    OutOfCoreTransferCountingDataStore<float32> inputStore(ShapeType{dimZ, dimY, dimX}, ShapeType{1}, 0.0f);
    OutOfCoreTransferCountingDataStore<uint8> outputStore(ShapeType{dimZ, dimY, dimX}, ShapeType{1}, uint8{255});
    for(usize index = 0; index < field.size(); ++index)
    {
      inputStore.setValue(index, field[index]);
    }

    std::atomic_bool shouldCancel{false};
    IFilter::MessageHandler messageHandler{};
    SIMPLNX_RESULT_REQUIRE_VALID(ApplyZeroCrossing(inputStore, outputStore, dims, uint8{1}, uint8{0}, shouldCancel, messageHandler));
    for(usize index = 0; index < expected.size(); ++index)
    {
      REQUIRE(outputStore.getValue(index) == expected[index]);
    }
    REQUIRE(manager.reservedWorkingMemoryBytes() == 0);
    return std::array<usize, 3>{inputStore.readCount(), inputStore.maxReadValues(), outputStore.maxWriteValues()};
  };

  const auto partialTransfers = run(k_PartialBudgetBytes);
  REQUIRE(partialTransfers[0] > 1);
  REQUIRE(partialTransfers[1] == dimX * dimY);
  REQUIRE(partialTransfers[2] == dimX * dimY);
  const auto completeTransfers = run(k_CompleteBudgetBytes);
  REQUIRE(completeTransfers[0] == 1);
  REQUIRE(completeTransfers[1] == field.size());
  REQUIRE(completeTransfers[2] == field.size());
  manager.setBudgetBytes(previousBudget);
}

TEST_CASE("ImageProcessing::ZeroCrossingEngine: bounded 2D blocks and tiles preserve axial order", "[ImageProcessing][ZeroCrossingEngine]")
{
  constexpr usize dimX = 9;
  constexpr usize dimY = 7;
  constexpr usize dimZ = 1;
  constexpr usize totalValues = dimX * dimY;
  constexpr uint8 foreground = 7;
  constexpr uint8 background = 3;
  std::vector<int32> input(totalValues);
  for(usize y = 0; y < dimY; ++y)
  {
    for(usize x = 0; x < dimX; ++x)
    {
      int32 value = static_cast<int32>((x * 5 + y * 3) % 11) - 5;
      if((x + y) % 6 == 0)
      {
        value = 0;
      }
      input[y * dimX + x] = value;
    }
  }
  const std::vector<uint8> expected = ZeroCrossing2DOracle(input, dimX, dimY, foreground, background);

  struct TransferCase
  {
    const char* label;
    usize targetBytes;
    usize maximumReadValues;
    usize maximumWriteValues;
  };
  const std::array<TransferCase, 2> transferCases = {{{"full-width row blocks", 162, 36, 18}, {"overwide X tiles", 50, 4, 2}}};

  for(const TransferCase& transferCase : transferCases)
  {
    DYNAMIC_SECTION(transferCase.label)
    {
      TransferCountingDataStore<int32> inputStore(ShapeType{dimZ, dimY, dimX}, ShapeType{1}, int32{0});
      TransferCountingDataStore<uint8> outputStore(ShapeType{dimZ, dimY, dimX}, ShapeType{1}, background);
      SIMPLNX_RESULT_REQUIRE_VALID(inputStore.copyFromBuffer(0, nonstd::span<const int32>(input.data(), input.size())));

      std::atomic_bool shouldCancel{false};
      IFilter::MessageHandler messageHandler{};
      SIMPLNX_RESULT_REQUIRE_VALID(ApplyZeroCrossing<int32>(inputStore, outputStore, SizeVec3{dimX, dimY, dimZ}, foreground, background, shouldCancel, messageHandler, transferCase.targetBytes));

      std::vector<uint8> actual(totalValues);
      SIMPLNX_RESULT_REQUIRE_VALID(outputStore.copyIntoBuffer(0, nonstd::span<uint8>(actual.data(), actual.size())));
      REQUIRE(actual == expected);
      CAPTURE(inputStore.maxReadValues(), outputStore.maxWriteValues(), outputStore.writtenValues());
      REQUIRE(inputStore.maxReadValues() == transferCase.maximumReadValues);
      REQUIRE(outputStore.maxWriteValues() == transferCase.maximumWriteValues);
      REQUIRE(outputStore.writtenValues() == totalValues);
    }
  }
}
