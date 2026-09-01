#include "simplnx/Utilities/ImageProcessing/IsoContourDistanceEngine.hpp"

#include "simplnx/Common/Array.hpp"
#include "simplnx/Common/Types.hpp"
#include "simplnx/DataStructure/DataStore.hpp"
#include "simplnx/Filter/IFilter.hpp"
#include "simplnx/UnitTest/UnitTestCommon.hpp"
#include "simplnx/Utilities/CacheMemoryBudgetManager.hpp"

#include <catch2/catch.hpp>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace nx::core;
using namespace nx::core::ImageProcessing;

namespace
{
usize FlatIndex(usize x, usize y, usize z, usize dimX, usize dimY)
{
  return (z * dimY + y) * dimX + x;
}

template <class T>
class OocReportingDataStore : public DataStore<T>
{
public:
  using DataStore<T>::DataStore;

  IDataStore::StoreType getStoreType() const override
  {
    return IDataStore::StoreType::OutOfCore;
  }

  Result<> copyIntoBuffer(usize startIndex, nonstd::span<T> buffer) const override
  {
    ++m_ReadCount;
    return DataStore<T>::copyIntoBuffer(startIndex, buffer);
  }

  Result<> copyFromBuffer(usize startIndex, nonstd::span<const T> buffer) override
  {
    ++m_WriteCount;
    return DataStore<T>::copyFromBuffer(startIndex, buffer);
  }

  [[nodiscard]] usize readCount() const noexcept
  {
    return m_ReadCount;
  }

  [[nodiscard]] usize writeCount() const noexcept
  {
    return m_WriteCount;
  }

private:
  mutable usize m_ReadCount = 0;
  usize m_WriteCount = 0;
};

template <class T>
std::vector<float32> RunEngine(const std::vector<T>& field, usize dx, usize dy, usize dz, float64 levelSet, float64 farValue, FloatVec3 spacing)
{
  DataStore<T> inStore(ShapeType{dz, dy, dx}, ShapeType{1}, static_cast<T>(0));
  for(usize i = 0; i < field.size(); ++i)
  {
    inStore.setValue(i, field[i]);
  }
  DataStore<float32> outStore(ShapeType{dz, dy, dx}, ShapeType{1}, 0.0f);
  std::atomic_bool shouldCancel{false};
  IFilter::MessageHandler messageHandler{};
  IsoContourDistance<T> engine(inStore, outStore, SizeVec3{dx, dy, dz}, levelSet, farValue, spacing, shouldCancel, messageHandler);
  const Result<> r = engine();
  REQUIRE(r.valid());
  std::vector<float32> out(field.size());
  for(usize i = 0; i < out.size(); ++i)
  {
    out[i] = outStore.getValue(i);
  }
  return out;
}
} // namespace

// A pure-axis unit-slope ramp f = coord has a single level-set crossing between the two planes bracketing `levelSet`.
// For a unit-slope ramp the sub-pixel formula reduces to the EXACT signed physical distance (coord - levelSet)*spacing
// for those two planes; every other voxel stays at +-farValue. Validates each axis, the sign, and spacing weighting.
TEMPLATE_TEST_CASE("ImageProcessing::IsoContourDistanceEngine: axis ramp == analytic signed distance", "[ImageProcessing][IsoContourDistanceEngine]", uint8, int16, int32, float32, float64)
{
  using T = TestType;
  const int axis = GENERATE(0, 1, 2);
  const bool aniso = GENERATE(false, true);
  const FloatVec3 spacing = aniso ? FloatVec3{2.0f, 1.5f, 3.0f} : FloatVec3{1.0f, 1.0f, 1.0f};
  constexpr float64 kLevel = 2.5;
  constexpr float32 kFar = 10.0f;
  CAPTURE(axis, aniso);

  constexpr usize DX = 6, DY = 5, DZ = 4;
  std::vector<T> field(DX * DY * DZ, T{0});
  for(usize z = 0; z < DZ; ++z)
  {
    for(usize y = 0; y < DY; ++y)
    {
      for(usize x = 0; x < DX; ++x)
      {
        const usize coord = (axis == 0) ? x : (axis == 1) ? y : z;
        field[FlatIndex(x, y, z, DX, DY)] = static_cast<T>(coord);
      }
    }
  }

  const std::vector<float32> out = RunEngine<T>(field, DX, DY, DZ, kLevel, kFar, spacing);
  const float32 sp = spacing[static_cast<usize>(axis)];

  for(usize z = 0; z < DZ; ++z)
  {
    for(usize y = 0; y < DY; ++y)
    {
      for(usize x = 0; x < DX; ++x)
      {
        const int64 coord = static_cast<int64>((axis == 0) ? x : (axis == 1) ? y : z);
        const float32 got = out[FlatIndex(x, y, z, DX, DY)];
        INFO("axis " << axis << " coord " << coord << " aniso " << aniso);
        if(coord == 2 || coord == 3)
        {
          REQUIRE(got == Approx(static_cast<float32>((static_cast<float64>(coord) - kLevel)) * sp).epsilon(1e-5));
        }
        else if(coord < 2)
        {
          REQUIRE(got == -kFar);
        }
        else
        {
          REQUIRE(got == kFar);
        }
      }
    }
  }
}

// Init / far-region behavior: a field entirely above (or entirely below) the level set has no crossing, so every
// voxel is exactly +farValue (resp. -farValue); a voxel exactly on the level set is 0. Vary levelSet and farValue.
TEST_CASE("ImageProcessing::IsoContourDistanceEngine: init and far regions", "[ImageProcessing][IsoContourDistanceEngine]")
{
  constexpr usize DX = 5, DY = 4, DZ = 3;
  const float64 levelSet = GENERATE(0.0, 3.0, -1.0);
  const float64 farValue = GENERATE(10.0, 4.0);
  CAPTURE(levelSet, farValue);

  SECTION("all above -> +far")
  {
    std::vector<int32> field(DX * DY * DZ, static_cast<int32>(levelSet) + 5);
    const std::vector<float32> out = RunEngine<int32>(field, DX, DY, DZ, levelSet, farValue, FloatVec3{1.0f, 1.0f, 1.0f});
    for(float32 v : out)
    {
      REQUIRE(v == static_cast<float32>(farValue));
    }
  }
  SECTION("all below -> -far")
  {
    std::vector<int32> field(DX * DY * DZ, static_cast<int32>(levelSet) - 5);
    const std::vector<float32> out = RunEngine<int32>(field, DX, DY, DZ, levelSet, farValue, FloatVec3{1.0f, 1.0f, 1.0f});
    for(float32 v : out)
    {
      REQUIRE(v == -static_cast<float32>(farValue));
    }
  }
  SECTION("exactly on level set -> 0")
  {
    std::vector<float32> field(DX * DY * DZ, static_cast<float32>(levelSet));
    const std::vector<float32> out = RunEngine<float32>(field, DX, DY, DZ, levelSet, farValue, FloatVec3{1.0f, 1.0f, 1.0f});
    for(float32 v : out)
    {
      REQUIRE(v == 0.0f);
    }
  }
}

// Radial field: interior of the contour is negative, exterior positive, and all outputs are finite.
TEST_CASE("ImageProcessing::IsoContourDistanceEngine: radial sign + finiteness", "[ImageProcessing][IsoContourDistanceEngine]")
{
  constexpr usize D = 15;
  const float64 c = 7.0;
  const float64 radius = 4.0;
  std::vector<float32> field(D * D * D);
  for(usize z = 0; z < D; ++z)
  {
    for(usize y = 0; y < D; ++y)
    {
      for(usize x = 0; x < D; ++x)
      {
        const float64 dxc = x - c, dyc = y - c, dzc = z - c;
        field[FlatIndex(x, y, z, D, D)] = static_cast<float32>(std::sqrt(dxc * dxc + dyc * dyc + dzc * dzc));
      }
    }
  }
  const std::vector<float32> out = RunEngine<float32>(field, D, D, D, radius, 100.0, FloatVec3{1.0f, 1.0f, 1.0f});
  for(float32 v : out)
  {
    REQUIRE(std::isfinite(v));
  }
  REQUIRE(out[FlatIndex(7, 7, 7, D, D)] < 0.0f); // center: deep interior
  REQUIRE(out[FlatIndex(0, 0, 0, D, D)] > 0.0f); // corner: far exterior
}

// Determinism: the same input yields byte-identical output on repeated runs.
TEST_CASE("ImageProcessing::IsoContourDistanceEngine: deterministic", "[ImageProcessing][IsoContourDistanceEngine]")
{
  constexpr usize D = 12;
  std::vector<int16> field(D * D * D);
  for(usize i = 0; i < field.size(); ++i)
  {
    field[i] = static_cast<int16>((i * 7 + 3) % 11);
  }
  const std::vector<float32> a = RunEngine<int16>(field, D, D, D, 5.0, 10.0, FloatVec3{1.0f, 1.0f, 1.0f});
  const std::vector<float32> b = RunEngine<int16>(field, D, D, D, 5.0, 10.0, FloatVec3{1.0f, 1.0f, 1.0f});
  REQUIRE(a.size() == b.size());
  for(usize i = 0; i < a.size(); ++i)
  {
    REQUIRE(a[i] == b[i]);
  }
}

// Streaming bookkeeping: a tall volume that forces many rolling-window refills still reproduces the exact analytic
// z-ramp result (the crossing pair at z=2,3 -> (z-2.5); everything else +-far).
TEST_CASE("ImageProcessing::IsoContourDistanceEngine: tall-volume streaming z-ramp", "[ImageProcessing][IsoContourDistanceEngine]")
{
  constexpr usize DX = 9, DY = 8, DZ = 40;
  constexpr float32 kFar = 50.0f;
  std::vector<int32> field(DX * DY * DZ);
  for(usize z = 0; z < DZ; ++z)
  {
    for(usize y = 0; y < DY; ++y)
    {
      for(usize x = 0; x < DX; ++x)
      {
        field[FlatIndex(x, y, z, DX, DY)] = static_cast<int32>(z);
      }
    }
  }
  const std::vector<float32> out = RunEngine<int32>(field, DX, DY, DZ, 2.5, kFar, FloatVec3{1.0f, 1.0f, 1.0f});
  for(usize z = 0; z < DZ; ++z)
  {
    const float32 got = out[FlatIndex(4, 4, z, DX, DY)];
    INFO("z " << z);
    if(z == 2 || z == 3)
    {
      REQUIRE(got == Approx(static_cast<float32>(z) - 2.5f).epsilon(1e-5));
    }
    else if(z < 2)
    {
      REQUIRE(got == -kFar);
    }
    else
    {
      REQUIRE(got == kFar);
    }
  }
}

TEST_CASE("ImageProcessing::IsoContourDistanceEngine: validates stores and cancellation", "[ImageProcessing][IsoContourDistanceEngine]")
{
  std::atomic_bool shouldCancel{false};
  IFilter::MessageHandler messageHandler{};

  SECTION("rejects mismatched stores")
  {
    DataStore<uint8> shortInput(ShapeType{63}, ShapeType{1}, uint8{0});
    DataStore<uint8> fullInput(ShapeType{64}, ShapeType{1}, uint8{0});
    DataStore<float32> shortOutput(ShapeType{63}, ShapeType{1}, 0.0f);
    DataStore<float32> fullOutput(ShapeType{64}, ShapeType{1}, 0.0f);

    IsoContourDistance<uint8> inputMismatch(shortInput, fullOutput, SizeVec3{4, 4, 4}, 0.0, 10.0, FloatVec3{1.0f, 1.0f, 1.0f}, shouldCancel, messageHandler);
    Result<> result = inputMismatch();
    REQUIRE(result.invalid());
    REQUIRE(result.errors().front().code == -8622);

    IsoContourDistance<uint8> outputMismatch(fullInput, shortOutput, SizeVec3{4, 4, 4}, 0.0, 10.0, FloatVec3{1.0f, 1.0f, 1.0f}, shouldCancel, messageHandler);
    result = outputMismatch();
    REQUIRE(result.invalid());
    REQUIRE(result.errors().front().code == -8623);
  }

  SECTION("pre-cancel preserves output")
  {
    constexpr float32 kPoison = -12345.0f;
    DataStore<uint8> inputStore(ShapeType{4, 4, 4}, ShapeType{1}, uint8{1});
    DataStore<float32> outputStore(ShapeType{4, 4, 4}, ShapeType{1}, kPoison);
    shouldCancel = true;
    IsoContourDistance<uint8> engine(inputStore, outputStore, SizeVec3{4, 4, 4}, 0.0, 10.0, FloatVec3{1.0f, 1.0f, 1.0f}, shouldCancel, messageHandler);
    const Result<> result = engine();
    REQUIRE(result.valid());
    for(const float32 value : outputStore)
    {
      REQUIRE(value == kPoison);
    }
  }
}

TEST_CASE("ImageProcessing::IsoContourDistanceEngine: resident state requires a complete dataset-scaled reservation", "[ImageProcessing][IsoContourDistanceEngine][WorkingMemory]")
{
  constexpr usize dimX = 512;
  constexpr usize dimY = 512;
  constexpr usize dimZ = 128;
  constexpr usize valueCount = dimX * dimY * dimZ;
  constexpr uint64 k_MiB = 1024ULL * 1024ULL;
  const SizeVec3 dims{dimX, dimY, dimZ};

  auto uint8Result = ImageProcessing::detail::CalculateIsoContourResidentWorkingMemoryBytes<uint8>(dims);
  SIMPLNX_RESULT_REQUIRE_VALID(uint8Result);
  REQUIRE(uint8Result.value() == valueCount * (sizeof(uint8) + sizeof(float32)));
  auto float64Result = ImageProcessing::detail::CalculateIsoContourResidentWorkingMemoryBytes<float64>(dims);
  SIMPLNX_RESULT_REQUIRE_VALID(float64Result);
  REQUIRE(float64Result.value() == valueCount * (sizeof(float64) + sizeof(float32)));
  SIMPLNX_RESULT_REQUIRE_INVALID(ImageProcessing::detail::CalculateIsoContourResidentWorkingMemoryBytes<uint8>(SizeVec3{std::numeric_limits<usize>::max(), 2, 2}));

  REQUIRE(ImageProcessing::detail::ShouldUseIsoContourResidentState(dims));
  REQUIRE_FALSE(ImageProcessing::detail::ShouldUseIsoContourResidentState(SizeVec3{dimX, dimY, 1}));

  auto& manager = CacheMemoryBudgetManager::instance();
  const uint64 previousBudget = manager.budgetBytes();
  manager.clear();
  manager.setBudgetBytes(512 * k_MiB);
  {
    auto allocationResult = ImageProcessing::detail::ReserveIsoContourResidentWorkingMemory<uint8>(dims);
    SIMPLNX_RESULT_REQUIRE_VALID(allocationResult);
    REQUIRE_FALSE(allocationResult.value().holdsCompleteState());
    REQUIRE(allocationResult.value().reservation.sizeBytes() == 128 * k_MiB);
  }
  REQUIRE(manager.reservedWorkingMemoryBytes() == 0);

  manager.setBudgetBytes(1024 * k_MiB);
  {
    auto allocationResult = ImageProcessing::detail::ReserveIsoContourResidentWorkingMemory<uint8>(dims);
    SIMPLNX_RESULT_REQUIRE_VALID(allocationResult);
    REQUIRE(allocationResult.value().holdsCompleteState());
    REQUIRE(allocationResult.value().reservation.sizeBytes() == uint8Result.value());
  }
  REQUIRE(manager.reservedWorkingMemoryBytes() == 0);
  manager.setBudgetBytes(previousBudget);
}

TEST_CASE("ImageProcessing::IsoContourDistanceEngine: real-OOC selector uses resident state only after a complete grant", "[ImageProcessing][IsoContourDistanceEngine][WorkingMemory]")
{
  constexpr usize dimX = 5;
  constexpr usize dimY = 6;
  constexpr usize dimZ = 7;
  constexpr uint64 k_PartialBudgetBytes = 2048;
  constexpr uint64 k_CompleteBudgetBytes = 8192;
  const SizeVec3 dims{dimX, dimY, dimZ};
  std::vector<uint8> field(dimX * dimY * dimZ);
  for(usize index = 0; index < field.size(); ++index)
  {
    field[index] = static_cast<uint8>((index * 7 + 3) % 11);
  }
  const std::vector<float32> expected = RunEngine<uint8>(field, dimX, dimY, dimZ, 5.0, 10.0, FloatVec3{1.0f, 1.0f, 1.0f});

  auto& manager = CacheMemoryBudgetManager::instance();
  const uint64 previousBudget = manager.budgetBytes();

  auto run = [&](uint64 budgetBytes) {
    manager.clear();
    manager.setBudgetBytes(budgetBytes);
    OocReportingDataStore<uint8> inputStore(ShapeType{dimZ, dimY, dimX}, ShapeType{1}, uint8{0});
    OocReportingDataStore<float32> outputStore(ShapeType{dimZ, dimY, dimX}, ShapeType{1}, 0.0f);
    for(usize index = 0; index < field.size(); ++index)
    {
      inputStore.setValue(index, field[index]);
    }

    std::atomic_bool shouldCancel{false};
    IFilter::MessageHandler messageHandler{};
    IsoContourDistance<uint8> engine(inputStore, outputStore, dims, 5.0, 10.0, FloatVec3{1.0f, 1.0f, 1.0f}, shouldCancel, messageHandler);
    SIMPLNX_RESULT_REQUIRE_VALID(engine());
    std::vector<float32> actual(field.size());
    SIMPLNX_RESULT_REQUIRE_VALID(outputStore.DataStore<float32>::copyIntoBuffer(0, nonstd::span<float32>(actual.data(), actual.size())));
    REQUIRE(actual == expected);
    REQUIRE(manager.reservedWorkingMemoryBytes() == 0);
    return std::array<usize, 2>{inputStore.readCount(), outputStore.writeCount()};
  };

  const auto partialTransfers = run(k_PartialBudgetBytes);
  REQUIRE(partialTransfers[0] > 1);
  REQUIRE(partialTransfers[1] > 1);
  const auto completeTransfers = run(k_CompleteBudgetBytes);
  REQUIRE(completeTransfers[0] == 1);
  REQUIRE(completeTransfers[1] == 1);
  manager.setBudgetBytes(previousBudget);
}

TEST_CASE("ImageProcessing::IsoContourDistanceEngine: streamed 3D path reads each input plane once and writes each output plane once", "[ImageProcessing][IsoContourDistanceEngine][WorkingMemory]")
{
  constexpr usize dimX = 5;
  constexpr usize dimY = 6;
  constexpr usize dimZ = 7;
  const SizeVec3 dims{dimX, dimY, dimZ};
  std::vector<uint8> field(dimX * dimY * dimZ);
  for(usize index = 0; index < field.size(); ++index)
  {
    field[index] = static_cast<uint8>((index * 7 + 3) % 11);
  }
  const auto expected = RunEngine<uint8>(field, dimX, dimY, dimZ, 5.0, 10.0, FloatVec3{1.0f, 1.0f, 1.0f});

  auto& manager = CacheMemoryBudgetManager::instance();
  const uint64 previousBudget = manager.budgetBytes();
  manager.clear();
  manager.setBudgetBytes(2048);
  OocReportingDataStore<uint8> inputStore(ShapeType{dimZ, dimY, dimX}, ShapeType{1}, uint8{0});
  OocReportingDataStore<float32> outputStore(ShapeType{dimZ, dimY, dimX}, ShapeType{1}, 0.0f);
  for(usize index = 0; index < field.size(); ++index)
  {
    inputStore.setValue(index, field[index]);
  }

  std::atomic_bool shouldCancel{false};
  IFilter::MessageHandler messageHandler{};
  IsoContourDistance<uint8> engine(inputStore, outputStore, dims, 5.0, 10.0, FloatVec3{1.0f, 1.0f, 1.0f}, shouldCancel, messageHandler);
  SIMPLNX_RESULT_REQUIRE_VALID(engine());
  std::vector<float32> actual(field.size());
  SIMPLNX_RESULT_REQUIRE_VALID(outputStore.DataStore<float32>::copyIntoBuffer(0, nonstd::span<float32>(actual.data(), actual.size())));
  REQUIRE(actual == expected);
  REQUIRE(inputStore.readCount() == dimZ);
  REQUIRE(outputStore.writeCount() == dimZ);
  REQUIRE(manager.reservedWorkingMemoryBytes() == 0);
  manager.setBudgetBytes(previousBudget);
}

TEST_CASE("ImageProcessing::IsoContourDistanceEngine: 2D planner bounds row blocks and overwide tiles", "[ImageProcessing][IsoContourDistanceEngine]")
{
  const auto benchmark = ImageProcessing::detail::BuildIsoContour2DBufferPlan(5888, 5888, sizeof(uint8));
  REQUIRE(benchmark.valid);
  REQUIRE(benchmark.coreCols == 5888);
  REQUIRE(benchmark.coreRows > 1);
  REQUIRE(benchmark.residentBytes <= ImageProcessing::detail::k_IsoContour2DResidentLimit);

  for(const usize inputBytes : {sizeof(uint8), sizeof(uint64)})
  {
    const auto stress = ImageProcessing::detail::BuildIsoContour2DBufferPlan(16385, 1025, inputBytes);
    CAPTURE(inputBytes);
    REQUIRE(stress.valid);
    REQUIRE(stress.coreCols == 16385);
    REQUIRE(stress.coreRows > 0);
    REQUIRE(stress.residentBytes <= ImageProcessing::detail::k_IsoContour2DResidentLimit);
  }

  const auto overwide = ImageProcessing::detail::BuildIsoContour2DBufferPlan(100000000, 2, sizeof(uint8));
  REQUIRE(overwide.valid);
  REQUIRE(overwide.coreCols < 100000000);
  REQUIRE(overwide.coreRows == 1);
  REQUIRE(overwide.residentBytes <= ImageProcessing::detail::k_IsoContour2DResidentLimit);

  constexpr usize kSmallLimit = 4995;
  const auto forcedTiled = ImageProcessing::detail::BuildIsoContour2DBufferPlan(100, 8, sizeof(uint8), kSmallLimit);
  REQUIRE(forcedTiled.valid);
  REQUIRE(forcedTiled.coreCols < 100);
  REQUIRE(forcedTiled.coreRows == 1);
  REQUIRE(forcedTiled.residentBytes <= kSmallLimit);

  const auto oneCell = ImageProcessing::detail::BuildIsoContour2DBufferPlan(1, 1, sizeof(uint64));
  REQUIRE(oneCell.valid);
  REQUIRE(oneCell.residentBytes <= ImageProcessing::detail::k_IsoContour2DResidentLimit);
}

TEST_CASE("ImageProcessing::IsoContourDistanceEngine: 2D planner rejects invalid and overflow dimensions", "[ImageProcessing][IsoContourDistanceEngine]")
{
  const auto overflow = ImageProcessing::detail::BuildIsoContour2DBufferPlan(std::numeric_limits<usize>::max(), std::numeric_limits<usize>::max(), sizeof(uint64));
  const auto zeroX = ImageProcessing::detail::BuildIsoContour2DBufferPlan(0, 1, sizeof(uint8));
  const auto zeroY = ImageProcessing::detail::BuildIsoContour2DBufferPlan(1, 0, sizeof(uint8));
  REQUIRE_FALSE(overflow.valid);
  REQUIRE(overflow.overflow);
  REQUIRE_FALSE(zeroX.valid);
  REQUIRE(zeroX.overflow);
  REQUIRE_FALSE(zeroY.valid);
  REQUIRE(zeroY.overflow);
}
