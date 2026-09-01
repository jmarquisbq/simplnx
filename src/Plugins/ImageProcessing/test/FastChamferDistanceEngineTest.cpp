#include "simplnx/Utilities/ImageProcessing/FastChamferDistanceEngine.hpp"

#include "simplnx/Common/Array.hpp"
#include "simplnx/Common/Types.hpp"
#include "simplnx/DataStructure/DataStore.hpp"
#include "simplnx/Filter/IFilter.hpp"
#include "simplnx/UnitTest/UnitTestCommon.hpp"
#include "simplnx/Utilities/CacheMemoryBudgetManager.hpp"

#include <catch2/catch.hpp>

#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <vector>

using namespace nx::core;
using namespace nx::core::ImageProcessing;

namespace
{
constexpr float32 kW0 = 0.92644f; // axis weight
constexpr float32 kW1 = 1.34065f; // face-diagonal weight
constexpr float32 kW2 = 1.65849f; // cube-diagonal weight

usize FlatIndex(usize x, usize y, usize z, usize dimX, usize dimY)
{
  return (z * dimY + y) * dimX + x;
}

std::vector<float32> RunChamfer(std::vector<float32> field, usize dx, usize dy, usize dz, float32 maxDist, bool negateOutput = false)
{
  DataStore<float32> store(ShapeType{dz, dy, dx}, ShapeType{1}, 0.0f);
  for(usize i = 0; i < field.size(); ++i)
  {
    store.setValue(i, field[i]);
  }
  std::atomic_bool shouldCancel{false};
  IFilter::MessageHandler messageHandler{};
  const Result<> r = ApplyFastChamferDistance(store, SizeVec3{dx, dy, dz}, maxDist, shouldCancel, messageHandler, negateOutput);
  REQUIRE(r.valid());
  std::vector<float32> out(field.size());
  for(usize i = 0; i < out.size(); ++i)
  {
    out[i] = store.getValue(i);
  }
  return out;
}
} // namespace

// A single 0-seed in a large (frozen) positive field: the two-pass chamfer computes the EXACT optimized-chamfer
// distance to the seed. Immediate neighbors are the raw weights; multi-step voxels are the cheapest weighted path.
TEST_CASE("ImageProcessing::FastChamferDistanceEngine: single seed == chamfer metric (3D)", "[ImageProcessing][FastChamferDistanceEngine]")
{
  constexpr usize D = 9;
  constexpr float32 kMax = 100.0f;
  std::vector<float32> field(D * D * D, kMax + 1.0f);
  field[FlatIndex(4, 4, 4, D, D)] = 0.0f;
  const std::vector<float32> out = RunChamfer(field, D, D, D, kMax);

  REQUIRE(out[FlatIndex(4, 4, 4, D, D)] == 0.0f);                             // seed
  REQUIRE(out[FlatIndex(5, 4, 4, D, D)] == Approx(kW0).epsilon(1e-5));        // +x axis
  REQUIRE(out[FlatIndex(3, 4, 4, D, D)] == Approx(kW0).epsilon(1e-5));        // -x axis
  REQUIRE(out[FlatIndex(4, 5, 4, D, D)] == Approx(kW0).epsilon(1e-5));        // +y axis
  REQUIRE(out[FlatIndex(4, 4, 5, D, D)] == Approx(kW0).epsilon(1e-5));        // +z axis
  REQUIRE(out[FlatIndex(5, 5, 4, D, D)] == Approx(kW1).epsilon(1e-5));        // xy face diagonal
  REQUIRE(out[FlatIndex(3, 5, 4, D, D)] == Approx(kW1).epsilon(1e-5));        // mixed-direction face diagonal
  REQUIRE(out[FlatIndex(5, 5, 5, D, D)] == Approx(kW2).epsilon(1e-5));        // xyz cube diagonal
  REQUIRE(out[FlatIndex(6, 4, 4, D, D)] == Approx(2.0f * kW0).epsilon(1e-5)); // two +x axis steps
  REQUIRE(out[FlatIndex(2, 4, 4, D, D)] == Approx(2.0f * kW0).epsilon(1e-5)); // two -x axis steps
}

TEST_CASE("ImageProcessing::FastChamferDistanceEngine: single seed == chamfer metric (2D)", "[ImageProcessing][FastChamferDistanceEngine]")
{
  constexpr usize D = 9;
  constexpr float32 kMax = 100.0f;
  std::vector<float32> field(D * D, kMax + 1.0f);
  field[FlatIndex(4, 4, 0, D, D)] = 0.0f;
  const std::vector<float32> out = RunChamfer(field, D, D, 1, kMax);

  REQUIRE(out[FlatIndex(4, 4, 0, D, D)] == 0.0f);
  REQUIRE(out[FlatIndex(5, 4, 0, D, D)] == Approx(kW0).epsilon(1e-5));
  REQUIRE(out[FlatIndex(4, 3, 0, D, D)] == Approx(kW0).epsilon(1e-5));
  REQUIRE(out[FlatIndex(5, 5, 0, D, D)] == Approx(kW1).epsilon(1e-5));
  REQUIRE(out[FlatIndex(3, 3, 0, D, D)] == Approx(kW1).epsilon(1e-5));
  REQUIRE(out[FlatIndex(6, 4, 0, D, D)] == Approx(2.0f * kW0).epsilon(1e-5));
}

// A signed step with a zero band at x==C: after the chamfer, the value steps by w0 per voxel away from the band, with
// the correct sign (positive on the +x side, negative on the -x side).
TEST_CASE("ImageProcessing::FastChamferDistanceEngine: signed plane band", "[ImageProcessing][FastChamferDistanceEngine]")
{
  constexpr usize DX = 9, DY = 5, DZ = 1, C = 4;
  constexpr float32 kMax = 100.0f;
  std::vector<float32> field(DX * DY * DZ);
  for(usize y = 0; y < DY; ++y)
  {
    for(usize x = 0; x < DX; ++x)
    {
      field[FlatIndex(x, y, 0, DX, DY)] = (x < C) ? -(kMax + 1.0f) : (x > C) ? (kMax + 1.0f) : 0.0f;
    }
  }
  const std::vector<float32> out = RunChamfer(field, DX, DY, DZ, kMax);
  for(usize y = 0; y < DY; ++y)
  {
    INFO("y=" << y);
    REQUIRE(out[FlatIndex(C, y, 0, DX, DY)] == 0.0f);
    REQUIRE(out[FlatIndex(C + 1, y, 0, DX, DY)] == Approx(kW0).epsilon(1e-5));
    REQUIRE(out[FlatIndex(C + 2, y, 0, DX, DY)] == Approx(2.0f * kW0).epsilon(1e-5));
    REQUIRE(out[FlatIndex(C - 1, y, 0, DX, DY)] == Approx(-kW0).epsilon(1e-5));
    REQUIRE(out[FlatIndex(C - 2, y, 0, DX, DY)] == Approx(-2.0f * kW0).epsilon(1e-5));
  }
}

// Small maxDist freezes the far field: a voxel adjacent to the band gets w0 (< maxDist), but a voxel whose chamfer
// distance exceeds maxDist retains a magnitude >= maxDist (the propagation front stops there).
TEST_CASE("ImageProcessing::FastChamferDistanceEngine: maxDist freeze", "[ImageProcessing][FastChamferDistanceEngine]")
{
  constexpr usize DX = 12, DY = 3, DZ = 1;
  constexpr float32 kMax = 2.0f;
  std::vector<float32> field(DX * DY * DZ, kMax + 1.0f);
  for(usize y = 0; y < DY; ++y)
  {
    field[FlatIndex(0, y, 0, DX, DY)] = 0.0f; // seed band at x==0
  }
  const std::vector<float32> out = RunChamfer(field, DX, DY, DZ, kMax);
  for(usize y = 0; y < DY; ++y)
  {
    REQUIRE(out[FlatIndex(1, y, 0, DX, DY)] == Approx(kW0).epsilon(1e-5)); // within maxDist
    REQUIRE(out[FlatIndex(11, y, 0, DX, DY)] >= kMax);                     // far: frozen at/above maxDist
  }
}

TEST_CASE("ImageProcessing::FastChamferDistanceEngine: deterministic", "[ImageProcessing][FastChamferDistanceEngine]")
{
  constexpr usize D = 10;
  std::vector<float32> field(D * D * D, 51.0f);
  field[FlatIndex(5, 5, 5, D, D)] = 0.0f;
  field[FlatIndex(2, 7, 3, D, D)] = -0.0f;
  const std::vector<float32> a = RunChamfer(field, D, D, D, 50.0f);
  const std::vector<float32> b = RunChamfer(field, D, D, D, 50.0f);
  REQUIRE(a.size() == b.size());
  for(usize i = 0; i < a.size(); ++i)
  {
    REQUIRE(a[i] == b[i]);
  }
}

TEST_CASE("ImageProcessing::FastChamferDistanceEngine: fused output negation is exact", "[ImageProcessing][FastChamferDistanceEngine]")
{
  for(const SizeVec3 dims : {SizeVec3{7, 6, 5}, SizeVec3{9, 8, 1}})
  {
    const usize valueCount = dims[0] * dims[1] * dims[2];
    std::vector<float32> field(valueCount, 51.0f);
    field[FlatIndex(dims[0] / 2, dims[1] / 2, dims[2] / 2, dims[0], dims[1])] = 0.0f;
    const std::vector<float32> ordinary = RunChamfer(field, dims[0], dims[1], dims[2], 50.0f);
    const std::vector<float32> negated = RunChamfer(field, dims[0], dims[1], dims[2], 50.0f, true);
    REQUIRE(negated.size() == ordinary.size());
    for(usize index = 0; index < ordinary.size(); ++index)
    {
      CAPTURE(dims, index);
      REQUIRE(negated[index] == -ordinary[index]);
    }
  }
}

// Tall volume forcing many streaming-window refills: a single-seed z-column still equals the exact axis chamfer
// metric (n * w0), validating the forward/backward streaming bookkeeping.
TEST_CASE("ImageProcessing::FastChamferDistanceEngine: tall-volume streaming", "[ImageProcessing][FastChamferDistanceEngine]")
{
  constexpr usize DX = 7, DY = 6, DZ = 40;
  constexpr float32 kMax = 200.0f;
  std::vector<float32> field(DX * DY * DZ, kMax + 1.0f);
  field[FlatIndex(3, 3, 20, DX, DY)] = 0.0f;
  const std::vector<float32> out = RunChamfer(field, DX, DY, DZ, kMax);
  for(usize z = 0; z < DZ; ++z)
  {
    const usize dist = (z > 20) ? (z - 20) : (20 - z);
    INFO("z=" << z);
    REQUIRE(out[FlatIndex(3, 3, z, DX, DY)] == Approx(static_cast<float32>(dist) * kW0).epsilon(1e-5));
  }
}

TEST_CASE("ImageProcessing::FastChamferDistanceEngine: resident state requires a complete dataset-scaled reservation", "[ImageProcessing][FastChamferDistanceEngine][WorkingMemory]")
{
  constexpr usize dimX = 512;
  constexpr usize dimY = 512;
  constexpr usize dimZ = 128;
  constexpr usize sliceValues = dimX * dimY;
  constexpr usize valueCount = sliceValues * dimZ;
  constexpr uint64 k_MiB = 1024ULL * 1024ULL;
  const SizeVec3 dims{dimX, dimY, dimZ};

  auto requiredResult = ImageProcessing::detail::CalculateFastChamferResidentWorkingMemoryBytes(dims);
  SIMPLNX_RESULT_REQUIRE_VALID(requiredResult);
  REQUIRE(requiredResult.value() == (valueCount + 2 * sliceValues) * sizeof(float32));
  SIMPLNX_RESULT_REQUIRE_INVALID(ImageProcessing::detail::CalculateFastChamferResidentWorkingMemoryBytes(SizeVec3{std::numeric_limits<usize>::max(), 2, 2}));

  REQUIRE(ImageProcessing::detail::ShouldUseFastChamferResidentState(dims));
  REQUIRE_FALSE(ImageProcessing::detail::ShouldUseFastChamferResidentState(SizeVec3{dimX, dimY, 1}));

  auto& manager = CacheMemoryBudgetManager::instance();
  const uint64 previousBudget = manager.budgetBytes();
  manager.clear();
  manager.setBudgetBytes(256 * k_MiB);
  {
    auto allocationResult = ImageProcessing::detail::ReserveFastChamferResidentWorkingMemory(dims);
    SIMPLNX_RESULT_REQUIRE_VALID(allocationResult);
    REQUIRE_FALSE(allocationResult.value().holdsCompleteState());
    REQUIRE(allocationResult.value().reservation.sizeBytes() == 64 * k_MiB);
  }
  REQUIRE(manager.reservedWorkingMemoryBytes() == 0);

  manager.setBudgetBytes(1024 * k_MiB);
  {
    auto allocationResult = ImageProcessing::detail::ReserveFastChamferResidentWorkingMemory(dims);
    SIMPLNX_RESULT_REQUIRE_VALID(allocationResult);
    REQUIRE(allocationResult.value().holdsCompleteState());
    REQUIRE(allocationResult.value().reservation.sizeBytes() == requiredResult.value());
  }
  REQUIRE(manager.reservedWorkingMemoryBytes() == 0);
  manager.setBudgetBytes(previousBudget);
}

TEST_CASE("ImageProcessing::FastChamferDistanceEngine: 2D planner bounds row blocks and overwide tiles", "[ImageProcessing][FastChamferDistanceEngine]")
{
  const auto benchmark = ImageProcessing::detail::BuildChamfer2DBufferPlan(5888, 5888);
  REQUIRE(benchmark.valid);
  REQUIRE(benchmark.coreCols == 5888);
  REQUIRE(benchmark.coreRows > 1);
  REQUIRE(benchmark.residentBytes <= ImageProcessing::detail::k_Chamfer2DResidentLimit);

  const auto stress = ImageProcessing::detail::BuildChamfer2DBufferPlan(16385, 1025);
  REQUIRE(stress.valid);
  REQUIRE(stress.coreCols == 16385);
  REQUIRE(stress.coreRows > 0);
  REQUIRE(stress.residentBytes <= ImageProcessing::detail::k_Chamfer2DResidentLimit);

  for(const auto dimensions : {std::array<usize, 2>{1, 16385}, std::array<usize, 2>{16385, 1}})
  {
    const auto oneDimensional = ImageProcessing::detail::BuildChamfer2DBufferPlan(dimensions[0], dimensions[1]);
    CAPTURE(dimensions[0], dimensions[1]);
    REQUIRE(oneDimensional.valid);
    REQUIRE(oneDimensional.residentBytes <= ImageProcessing::detail::k_Chamfer2DResidentLimit);
  }

  const auto overwide = ImageProcessing::detail::BuildChamfer2DBufferPlan(100000000, 2);
  REQUIRE(overwide.valid);
  REQUIRE(overwide.coreCols < 100000000);
  REQUIRE(overwide.coreRows == 1);
  REQUIRE(overwide.residentBytes <= ImageProcessing::detail::k_Chamfer2DResidentLimit);

  constexpr usize kSmallLimit = 4500;
  const auto forcedTiled = ImageProcessing::detail::BuildChamfer2DBufferPlan(100, 8, kSmallLimit);
  REQUIRE(forcedTiled.valid);
  REQUIRE(forcedTiled.coreCols < 100);
  REQUIRE(forcedTiled.coreRows == 1);
  REQUIRE(forcedTiled.residentBytes <= kSmallLimit);

  constexpr usize kOneColumnTileLimit = ImageProcessing::detail::k_Chamfer2DFixedStateBytes + ImageProcessing::detail::k_Chamfer2DMaxTileRecords * sizeof(float32);
  const auto oneColumnTiles = ImageProcessing::detail::BuildChamfer2DBufferPlan(9, 7, kOneColumnTileLimit);
  REQUIRE(oneColumnTiles.valid);
  REQUIRE(oneColumnTiles.coreCols == 1);
  REQUIRE(oneColumnTiles.residentBytes == kOneColumnTileLimit);

  const auto oneCell = ImageProcessing::detail::BuildChamfer2DBufferPlan(1, 1);
  REQUIRE(oneCell.valid);
  REQUIRE(oneCell.residentBytes <= ImageProcessing::detail::k_Chamfer2DResidentLimit);
}

TEST_CASE("ImageProcessing::FastChamferDistanceEngine: 2D planner rejects invalid and overflow dimensions", "[ImageProcessing][FastChamferDistanceEngine]")
{
  const auto overflow = ImageProcessing::detail::BuildChamfer2DBufferPlan(std::numeric_limits<usize>::max(), std::numeric_limits<usize>::max());
  const auto zeroX = ImageProcessing::detail::BuildChamfer2DBufferPlan(0, 1);
  const auto zeroY = ImageProcessing::detail::BuildChamfer2DBufferPlan(1, 0);
  REQUIRE_FALSE(overflow.valid);
  REQUIRE(overflow.overflow);
  REQUIRE_FALSE(zeroX.valid);
  REQUIRE(zeroX.overflow);
  REQUIRE_FALSE(zeroY.valid);
  REQUIRE(zeroY.overflow);
}
