#include "simplnx/Utilities/ImageProcessing/MaurerDistanceMapEngine.hpp"

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
#include <cstdint>
#include <limits>
#include <thread>
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
class ReadCountingDataStore : public DataStore<T>
{
public:
  using DataStore<T>::DataStore;

  Result<> copyIntoBuffer(usize startIndex, nonstd::span<T> buffer) const override
  {
    ++m_ReadCount;
    return DataStore<T>::copyIntoBuffer(startIndex, buffer);
  }

  [[nodiscard]] usize readCount() const noexcept
  {
    return m_ReadCount;
  }

private:
  mutable usize m_ReadCount = 0;
};

template <class T>
class ExtentCountingDataStore : public DataStore<T>
{
public:
  using DataStore<T>::DataStore;

  Result<> copyFromBuffer(usize startIndex, nonstd::span<const T> buffer) override
  {
    ++m_FlatWriteCount;
    return DataStore<T>::copyFromBuffer(startIndex, buffer);
  }

  void writeExtent(const Extent& extent, nonstd::span<const T> data) override
  {
    ++m_ExtentWriteCount;
    DataStore<T>::writeExtent(extent, data);
  }

  [[nodiscard]] usize flatWriteCount() const noexcept
  {
    return m_FlatWriteCount;
  }

  [[nodiscard]] usize extentWriteCount() const noexcept
  {
    return m_ExtentWriteCount;
  }

private:
  usize m_FlatWriteCount = 0;
  usize m_ExtentWriteCount = 0;
};

// Independent brute-force signed EDT oracle (no image spacing). A feature is an object pixel (input != bg) with at
// least one background OR out-of-bounds neighbor in full connectivity (z clamped when the image is 2D). Distance is
// the exact squared Euclidean distance to the nearest feature; sign follows the ITK rule (inside==insidePos -> +).
std::vector<float32> MaurerOracle(const std::vector<int32>& in, usize dx, usize dy, usize dz, int32 bg, bool insideIsPositive, bool squared)
{
  const int64 nX = static_cast<int64>(dx), nY = static_cast<int64>(dy), nZ = static_cast<int64>(dz);
  const int64 wzLo = (nZ == 1) ? 0 : -1, wzHi = (nZ == 1) ? 0 : 1;
  auto at = [&](int64 x, int64 y, int64 z) { return in[FlatIndex(static_cast<usize>(x), static_cast<usize>(y), static_cast<usize>(z), dx, dy)]; };

  std::vector<std::array<int64, 3>> feats;
  for(int64 z = 0; z < nZ; ++z)
  {
    for(int64 y = 0; y < nY; ++y)
    {
      for(int64 x = 0; x < nX; ++x)
      {
        if(at(x, y, z) == bg)
        {
          continue;
        }
        bool boundary = false;
        for(int64 wz = wzLo; wz <= wzHi && !boundary; ++wz)
        {
          for(int64 wy = -1; wy <= 1 && !boundary; ++wy)
          {
            for(int64 wx = -1; wx <= 1 && !boundary; ++wx)
            {
              if(wx == 0 && wy == 0 && wz == 0)
              {
                continue;
              }
              const int64 ax = x + wx, ay = y + wy, az = z + wz;
              if(ax < 0 || ay < 0 || az < 0 || ax >= nX || ay >= nY || az >= nZ)
              {
                continue; // match ITK's BinaryContour: out-of-bounds neighbors are ignored, not background
              }
              if(at(ax, ay, az) == bg)
              {
                boundary = true;
              }
            }
          }
        }
        if(boundary)
        {
          feats.push_back({x, y, z});
        }
      }
    }
  }

  std::vector<float32> out(dx * dy * dz);
  for(int64 z = 0; z < nZ; ++z)
  {
    for(int64 y = 0; y < nY; ++y)
    {
      for(int64 x = 0; x < nX; ++x)
      {
        int64 bestSq = std::numeric_limits<int64>::max();
        for(const auto& f : feats)
        {
          const int64 ddx = x - f[0], ddy = y - f[1], ddz = z - f[2];
          const int64 sq = ddx * ddx + ddy * ddy + ddz * ddz;
          if(sq < bestSq)
          {
            bestSq = sq;
          }
        }
        const float32 v = squared ? static_cast<float32>(bestSq) : std::sqrt(static_cast<float32>(bestSq));
        const bool inside = at(x, y, z) != bg;
        out[FlatIndex(static_cast<usize>(x), static_cast<usize>(y), static_cast<usize>(z), dx, dy)] = ((inside == insideIsPositive) ? v : -v);
      }
    }
  }
  return out;
}

template <class T>
std::vector<float32> RunInCore(const std::vector<int32>& pattern, usize dx, usize dy, usize dz, int32 bg, bool insideIsPositive, bool squared, bool useSpacing = false,
                               FloatVec3 spacing = FloatVec3{1.0f, 1.0f, 1.0f})
{
  DataStore<T> inStore(ShapeType{dz, dy, dx}, ShapeType{1}, static_cast<T>(0));
  for(usize i = 0; i < pattern.size(); ++i)
  {
    inStore.setValue(i, static_cast<T>(pattern[i]));
  }
  DataStore<float32> outStore(ShapeType{dz, dy, dx}, ShapeType{1}, 0.0f);
  std::atomic_bool shouldCancel{false};
  IFilter::MessageHandler messageHandler{};
  MaurerDistanceInCore<T> engine(inStore, outStore, SizeVec3{dx, dy, dz}, static_cast<T>(bg), insideIsPositive, squared, useSpacing, spacing, shouldCancel, messageHandler);
  const Result<> r = engine();
  REQUIRE(r.valid());
  std::vector<float32> out(pattern.size());
  for(usize i = 0; i < out.size(); ++i)
  {
    out[i] = outStore.getValue(i);
  }
  return out;
}

template <class T>
std::vector<float32> RunSlab(const std::vector<int32>& pattern, usize dx, usize dy, usize dz, int32 bg, bool insideIsPositive, bool squared, bool useSpacing = false,
                             FloatVec3 spacing = FloatVec3{1.0f, 1.0f, 1.0f})
{
  DataStore<T> inStore(ShapeType{dz, dy, dx}, ShapeType{1}, static_cast<T>(0));
  for(usize i = 0; i < pattern.size(); ++i)
  {
    inStore.setValue(i, static_cast<T>(pattern[i]));
  }
  DataStore<float32> outStore(ShapeType{dz, dy, dx}, ShapeType{1}, 0.0f);
  std::atomic_bool shouldCancel{false};
  IFilter::MessageHandler messageHandler{};
  MaurerDistanceSlab<T> engine(inStore, outStore, SizeVec3{dx, dy, dz}, static_cast<T>(bg), insideIsPositive, squared, useSpacing, spacing, shouldCancel, messageHandler);
  const Result<> r = engine();
  REQUIRE(r.valid());
  std::vector<float32> out(pattern.size());
  for(usize i = 0; i < out.size(); ++i)
  {
    out[i] = outStore.getValue(i);
  }
  return out;
}

// A deterministic mixed image: object blobs (value 1) on a background (0), with a couple of isolated object pixels.
std::vector<int32> MakePattern(usize dx, usize dy, usize dz)
{
  std::vector<int32> v(dx * dy * dz, 0);
  for(usize z = 0; z < dz; ++z)
  {
    for(usize y = 0; y < dy; ++y)
    {
      for(usize x = 0; x < dx; ++x)
      {
        // a filled quadrant is object; plus a checker of isolated object pixels elsewhere.
        const bool block = (x >= dx / 2) && (y >= dy / 2);
        const bool speck = ((x + 2 * y + 3 * z) % 5 == 0);
        v[FlatIndex(x, y, z, dx, dy)] = (block || speck) ? 1 : 0;
      }
    }
  }
  return v;
}
} // namespace

TEMPLATE_TEST_CASE("ImageProcessing::MaurerDistanceMapEngine: in-core == brute-force EDT oracle (no spacing)", "[ImageProcessing][MaurerDistanceMapEngine]", uint8, int16, int32)
{
  using T = TestType;
  const bool insideIsPositive = GENERATE(false, true);
  const bool squared = GENERATE(false, true);
  CAPTURE(insideIsPositive, squared);

  auto check = [&](usize dx, usize dy, usize dz) {
    const std::vector<int32> pattern = MakePattern(dx, dy, dz);
    const std::vector<float32> oracle = MaurerOracle(pattern, dx, dy, dz, /*bg=*/0, insideIsPositive, squared);
    const std::vector<float32> got = RunInCore<T>(pattern, dx, dy, dz, /*bg=*/0, insideIsPositive, squared);
    const std::vector<float32> slab = RunSlab<T>(pattern, dx, dy, dz, /*bg=*/0, insideIsPositive, squared);
    REQUIRE(got.size() == oracle.size());
    REQUIRE(slab.size() == oracle.size());
    for(usize i = 0; i < got.size(); ++i)
    {
      INFO("index " << i << " dims " << dx << "x" << dy << "x" << dz);
      REQUIRE(got[i] == oracle[i]);  // in-core == oracle
      REQUIRE(slab[i] == oracle[i]); // OOC slab == oracle
      REQUIRE(slab[i] == got[i]);    // D3 gate: OOC == in-core, byte-identical
    }
  };

  SECTION("3D 5x6x7")
  {
    check(5, 6, 7);
  }
  SECTION("2D 9x8x1")
  {
    check(9, 8, 1);
  }
  SECTION("small 3x3x3")
  {
    check(3, 3, 3);
  }
}

TEST_CASE("ImageProcessing::MaurerDistanceMapEngine: single-seed hand values", "[ImageProcessing][MaurerDistanceMapEngine]")
{
  // A single object pixel at the center of a 5x5x1 background field. That pixel is a boundary feature (touches bg on
  // all sides), so distances are the squared distance to (2,2). Inside is only the center; everything else outside.
  constexpr usize DX = 5, DY = 5, DZ = 1;
  std::vector<int32> pattern(DX * DY * DZ, 0);
  pattern[FlatIndex(2, 2, 0, DX, DY)] = 1;
  const std::vector<float32> got = RunInCore<int32>(pattern, DX, DY, DZ, /*bg=*/0, /*insideIsPositive=*/false, /*squared=*/true);
  // Outside pixels are positive squared distance to (2,2); the center (inside) is 0 (it is itself a feature).
  REQUIRE(got[FlatIndex(2, 2, 0, DX, DY)] == 0.0f); // the seed
  REQUIRE(got[FlatIndex(0, 2, 0, DX, DY)] == 4.0f); // dx=2 -> 4
  REQUIRE(got[FlatIndex(0, 0, 0, DX, DY)] == 8.0f); // dx=2,dy=2 -> 8
  REQUIRE(got[FlatIndex(4, 4, 0, DX, DY)] == 8.0f); // symmetric corner
}

// D3 gate WITH non-unit image spacing: the OOC slab must be byte-identical to in-core on the spacing math path
// (the Z-pass build-loop float(i)*float(sp) vs query-loop float(i*sp) distinction). The brute-force oracle is
// unit-spacing only, so this checks slab == in-core directly across types, sqrt/squared, and 3D/2D.
TEMPLATE_TEST_CASE("ImageProcessing::MaurerDistanceMapEngine: slab == in-core with image spacing (D3)", "[ImageProcessing][MaurerDistanceMapEngine]", uint8, int16, int32)
{
  using T = TestType;
  const bool insideIsPositive = GENERATE(false, true);
  const bool squared = GENERATE(false, true);
  const FloatVec3 spacing{2.0f, 1.5f, 3.0f};
  CAPTURE(insideIsPositive, squared);

  auto check = [&](usize dx, usize dy, usize dz) {
    const std::vector<int32> pattern = MakePattern(dx, dy, dz);
    const std::vector<float32> inCore = RunInCore<T>(pattern, dx, dy, dz, /*bg=*/0, insideIsPositive, squared, /*useSpacing=*/true, spacing);
    const std::vector<float32> slab = RunSlab<T>(pattern, dx, dy, dz, /*bg=*/0, insideIsPositive, squared, /*useSpacing=*/true, spacing);
    REQUIRE(inCore.size() == slab.size());
    for(usize i = 0; i < inCore.size(); ++i)
    {
      INFO("index " << i << " dims " << dx << "x" << dy << "x" << dz);
      REQUIRE(slab[i] == inCore[i]);
    }
  };

  SECTION("3D 5x6x7")
  {
    check(5, 6, 7);
  }
  SECTION("2D 9x8x1")
  {
    check(9, 8, 1);
  }
}

TEST_CASE("ImageProcessing::MaurerDistanceMapEngine: 2D planner bounds direct transpose batches", "[ImageProcessing][MaurerDistanceMapEngine]")
{
  const auto benchmark = ImageProcessing::detail::BuildMaurer2DBufferPlan(5888, 5888, sizeof(uint8));
  REQUIRE(benchmark.valid);
  REQUIRE_FALSE(benchmark.spillX);
  REQUIRE_FALSE(benchmark.spillY);
  REQUIRE(benchmark.rowBatchRows > 1000);
  REQUIRE(benchmark.columnBatchCols > 1000);
  REQUIRE(benchmark.residentBytes <= ImageProcessing::detail::k_Maurer2DResidentLimit);

  for(const usize inputBytes : {sizeof(uint8), sizeof(uint64)})
  {
    const auto stress = ImageProcessing::detail::BuildMaurer2DBufferPlan(16385, 1025, inputBytes);
    CAPTURE(inputBytes);
    REQUIRE(stress.valid);
    REQUIRE(stress.rowBatchRows > 0);
    REQUIRE(stress.columnBatchCols > 0);
    REQUIRE(stress.residentBytes <= ImageProcessing::detail::k_Maurer2DResidentLimit);
  }

  const auto oneCell = ImageProcessing::detail::BuildMaurer2DBufferPlan(1, 1, sizeof(uint64));
  REQUIRE(oneCell.valid);
  REQUIRE(oneCell.residentBytes <= ImageProcessing::detail::k_Maurer2DResidentLimit);
}

TEST_CASE("ImageProcessing::MaurerDistanceMapEngine: 2D planner selects spill lines", "[ImageProcessing][MaurerDistanceMapEngine]")
{
  const auto xPlan = ImageProcessing::detail::BuildMaurer2DBufferPlan(100000000, 1, sizeof(uint8));
  const auto yPlan = ImageProcessing::detail::BuildMaurer2DBufferPlan(1, 100000000, sizeof(uint8));
  REQUIRE(xPlan.valid);
  REQUIRE(yPlan.valid);
  REQUIRE(xPlan.spillX);
  REQUIRE(yPlan.spillY);
  REQUIRE(xPlan.lineBlockValues >= 2);
  REQUIRE(yPlan.lineBlockValues >= 2);
  REQUIRE(xPlan.residentBytes <= ImageProcessing::detail::k_Maurer2DResidentLimit);
  REQUIRE(yPlan.residentBytes <= ImageProcessing::detail::k_Maurer2DResidentLimit);

  constexpr usize kSmallTestLimit = 8192;
  const auto forcedX = ImageProcessing::detail::BuildMaurer2DBufferPlan(100, 8, sizeof(uint8), kSmallTestLimit);
  const auto forcedY = ImageProcessing::detail::BuildMaurer2DBufferPlan(8, 100, sizeof(uint8), kSmallTestLimit);
  REQUIRE(forcedX.valid);
  REQUIRE(forcedX.spillX);
  REQUIRE(forcedX.spillY);
  REQUIRE(forcedX.residentBytes <= kSmallTestLimit);
  REQUIRE(forcedY.valid);
  REQUIRE_FALSE(forcedY.spillX);
  REQUIRE(forcedY.spillY);
  REQUIRE(forcedY.residentBytes <= kSmallTestLimit);
}

TEST_CASE("ImageProcessing::MaurerDistanceMapEngine: 2D planner rejects overflow", "[ImageProcessing][MaurerDistanceMapEngine]")
{
  const auto overflow = ImageProcessing::detail::BuildMaurer2DBufferPlan(std::numeric_limits<usize>::max(), std::numeric_limits<usize>::max(), sizeof(uint8));
  const auto zeroX = ImageProcessing::detail::BuildMaurer2DBufferPlan(0, 1, sizeof(uint8));
  const auto zeroY = ImageProcessing::detail::BuildMaurer2DBufferPlan(1, 0, sizeof(uint8));
  REQUIRE_FALSE(overflow.valid);
  REQUIRE(overflow.overflow);
  REQUIRE_FALSE(zeroX.valid);
  REQUIRE(zeroX.overflow);
  REQUIRE_FALSE(zeroY.valid);
  REQUIRE(zeroY.overflow);
}

TEST_CASE("ImageProcessing::MaurerDistanceMapEngine: working format follows OOC endpoint precedence", "[ImageProcessing][MaurerDistanceMapEngine]")
{
  using StoreType = IDataStore::StoreType;
  REQUIRE(ImageProcessing::detail::SelectDistanceWorkingDataFormat(StoreType::InMemory, "input", StoreType::InMemory, "output") == "input");
  REQUIRE(ImageProcessing::detail::SelectDistanceWorkingDataFormat(StoreType::OutOfCore, "input-ooc", StoreType::OutOfCore, "output-ooc") == "input-ooc");
  REQUIRE(ImageProcessing::detail::SelectDistanceWorkingDataFormat(StoreType::InMemory, "input", StoreType::OutOfCore, "output-ooc") == "output-ooc");
  REQUIRE(ImageProcessing::detail::SelectDistanceWorkingDataFormat(StoreType::OutOfCore, "input-ooc", StoreType::InMemory, "output") == "input-ooc");
  DataStore<uint8> residentStore(ShapeType{1}, ShapeType{1}, 0);
  const IDataStore& abstractResidentStore = residentStore;
  REQUIRE_FALSE(abstractResidentStore.getChunkShape().has_value());
}

TEST_CASE("ImageProcessing::MaurerDistanceMapEngine: resident state requires a complete dataset-scaled reservation", "[ImageProcessing][MaurerDistanceMapEngine][WorkingMemory]")
{
  constexpr usize dimX = 512;
  constexpr usize dimY = 512;
  constexpr usize dimZ = 128;
  constexpr usize valueCount = dimX * dimY * dimZ;
  constexpr uint64 k_MiB = 1024ULL * 1024ULL;
  constexpr uint64 k_GiB = 1024ULL * k_MiB;
  const SizeVec3 dims{dimX, dimY, dimZ};

  auto requiredResult = ImageProcessing::detail::CalculateMaurerResidentWorkingMemoryBytes<uint8>(dims);
  SIMPLNX_RESULT_REQUIRE_VALID(requiredResult);
  REQUIRE(requiredResult.value() == valueCount * (sizeof(uint8) + sizeof(float32) + sizeof(uint8)));

  auto halfResult = ImageProcessing::detail::CalculateMaurerResidentWorkingMemoryBytes<uint8>(SizeVec3{dimX, dimY, dimZ / 2});
  SIMPLNX_RESULT_REQUIRE_VALID(halfResult);
  REQUIRE(halfResult.value() * 2 == requiredResult.value());
  SIMPLNX_RESULT_REQUIRE_INVALID(ImageProcessing::detail::CalculateMaurerResidentWorkingMemoryBytes<uint8>(SizeVec3{std::numeric_limits<usize>::max(), 2, 2}));

  REQUIRE(ImageProcessing::detail::ShouldUseMaurerResidentState(dims));
  REQUIRE_FALSE(ImageProcessing::detail::ShouldUseMaurerResidentState(SizeVec3{dimX, dimY, 1}));

  auto smallSlabPlan = ImageProcessing::detail::CreateMaurer3DSlabMemoryPlan<uint8>(dims, 16 * k_MiB);
  auto mediumSlabPlan = ImageProcessing::detail::CreateMaurer3DSlabMemoryPlan<uint8>(dims, 128 * k_MiB);
  auto fullSlabPlan = ImageProcessing::detail::CreateMaurer3DSlabMemoryPlan<uint8>(dims, 256 * k_MiB);
  SIMPLNX_RESULT_REQUIRE_VALID(smallSlabPlan);
  SIMPLNX_RESULT_REQUIRE_VALID(mediumSlabPlan);
  SIMPLNX_RESULT_REQUIRE_VALID(fullSlabPlan);
  const usize expectedWorkerCount = std::max<usize>(1, static_cast<usize>(std::thread::hardware_concurrency()));
  REQUIRE(smallSlabPlan.value().workerCount == expectedWorkerCount);
  REQUIRE(mediumSlabPlan.value().workerCount == expectedWorkerCount);
  REQUIRE(fullSlabPlan.value().workerCount == expectedWorkerCount);
  REQUIRE(smallSlabPlan.value().maxYRows > 0);
  REQUIRE(mediumSlabPlan.value().maxYRows > smallSlabPlan.value().maxYRows);
  REQUIRE(fullSlabPlan.value().maxYRows == dimY);
  auto requireMinimumRowsForBatchCount = [dimY](const ImageProcessing::detail::Maurer3DSlabMemoryPlan& plan) {
    const usize batchCount = (dimY + plan.maxYRows - 1) / plan.maxYRows;
    REQUIRE(plan.maxYRows == (dimY + batchCount - 1) / batchCount);
  };
  requireMinimumRowsForBatchCount(smallSlabPlan.value());
  requireMinimumRowsForBatchCount(mediumSlabPlan.value());
  requireMinimumRowsForBatchCount(fullSlabPlan.value());
  REQUIRE(smallSlabPlan.value().residentBytes <= 16 * k_MiB);
  REQUIRE(mediumSlabPlan.value().residentBytes <= 128 * k_MiB);
  REQUIRE(fullSlabPlan.value().residentBytes <= 256 * k_MiB);
  SIMPLNX_RESULT_REQUIRE_INVALID(ImageProcessing::detail::CreateMaurer3DSlabMemoryPlan<uint8>(dims, 1));

  auto& manager = CacheMemoryBudgetManager::instance();
  const uint64 previousBudget = manager.budgetBytes();
  manager.clear();
  manager.setBudgetBytes(512 * k_MiB);
  {
    auto allocationResult = ImageProcessing::detail::ReserveMaurerResidentWorkingMemory<uint8>(dims);
    SIMPLNX_RESULT_REQUIRE_VALID(allocationResult);
    REQUIRE_FALSE(allocationResult.value().holdsCompleteState());
    REQUIRE(allocationResult.value().reservation.sizeBytes() == 128 * k_MiB);
  }
  REQUIRE(manager.reservedWorkingMemoryBytes() == 0);

  manager.setBudgetBytes(k_GiB);
  {
    auto allocationResult = ImageProcessing::detail::ReserveMaurerResidentWorkingMemory<uint8>(dims);
    SIMPLNX_RESULT_REQUIRE_VALID(allocationResult);
    REQUIRE(allocationResult.value().holdsCompleteState());
    REQUIRE(allocationResult.value().reservation.sizeBytes() == requiredResult.value());
  }
  REQUIRE(manager.reservedWorkingMemoryBytes() == 0);
  manager.setBudgetBytes(previousBudget);
}

TEST_CASE("ImageProcessing::MaurerDistanceMapEngine: 3D chunk hint follows the bounded Y plan", "[ImageProcessing][MaurerDistanceMapEngine][WorkingMemory]")
{
  const SizeVec3 dims{512, 512, 128};
  constexpr usize k_GrantBytes = 16ULL * 1024ULL * 1024ULL;
  auto planResult = ImageProcessing::detail::CreateMaurer3DSlabMemoryPlan<uint8>(dims, k_GrantBytes);
  SIMPLNX_RESULT_REQUIRE_VALID(planResult);

  auto hintResult = ImageProcessing::detail::CreateMaurer3DChunkHint<uint8>(dims, k_GrantBytes);
  SIMPLNX_RESULT_REQUIRE_VALID(hintResult);
  REQUIRE(hintResult.value() == ShapeType{1, planResult.value().maxYRows, dims[0]});
  REQUIRE(hintResult.value()[0] * hintResult.value()[1] * hintResult.value()[2] * sizeof(float32) > 0);
}

TEST_CASE("ImageProcessing::MaurerDistanceMapEngine: 3D batch alignment preserves a short fallback", "[ImageProcessing][MaurerDistanceMapEngine][WorkingMemory]")
{
  REQUIRE(ImageProcessing::detail::AlignMaurer3DYBatchRows(120, 7) == 119);
  REQUIRE(ImageProcessing::detail::AlignMaurer3DYBatchRows(6, 7) == 6);
  REQUIRE(ImageProcessing::detail::AlignMaurer3DYBatchRows(57, 57) == 57);
}

TEST_CASE("ImageProcessing::MaurerDistanceMapEngine: 3D scratch inherits a valid output chunk hint", "[ImageProcessing][MaurerDistanceMapEngine][WorkingMemory]")
{
  const ShapeType outputChunkShape{1, 7, 32};
  const ShapeType scratchTupleShape{8, 17, 32};
  const auto scratchHint = ImageProcessing::detail::SelectMaurer3DScratchChunkHint(outputChunkShape, scratchTupleShape);
  REQUIRE(scratchHint.has_value());
  REQUIRE(*scratchHint == outputChunkShape);
  REQUIRE_FALSE(ImageProcessing::detail::SelectMaurer3DScratchChunkHint(ShapeType{1, 18, 32}, scratchTupleShape).has_value());
}

TEST_CASE("ImageProcessing::MaurerDistanceMapEngine: bounded 3D reads each input plane once", "[ImageProcessing][MaurerDistanceMapEngine][WorkingMemory]")
{
  constexpr usize dimX = 5;
  constexpr usize dimY = 6;
  constexpr usize dimZ = 7;
  constexpr uint64 k_PartialBudgetBytes = 1024;
  constexpr uint64 k_CompleteBudgetBytes = 8192;
  const SizeVec3 dims{dimX, dimY, dimZ};
  const std::vector<int32> pattern = MakePattern(dimX, dimY, dimZ);
  const std::vector<float32> expected = RunInCore<uint8>(pattern, dimX, dimY, dimZ, 0, false, true);

  auto& manager = CacheMemoryBudgetManager::instance();
  const uint64 previousBudget = manager.budgetBytes();

  auto run = [&](uint64 budgetBytes) {
    manager.clear();
    manager.setBudgetBytes(budgetBytes);
    ReadCountingDataStore<uint8> inputStore(ShapeType{dimZ, dimY, dimX}, ShapeType{1}, uint8{0});
    DataStore<float32> outputStore(ShapeType{dimZ, dimY, dimX}, ShapeType{1}, 0.0f);
    for(usize index = 0; index < pattern.size(); ++index)
    {
      inputStore.setValue(index, static_cast<uint8>(pattern[index]));
    }

    std::atomic_bool shouldCancel{false};
    IFilter::MessageHandler messageHandler{};
    MaurerDistanceWorkingMemory<uint8> engine(inputStore, outputStore, dims, uint8{0}, false, true, false, FloatVec3{1.0f, 1.0f, 1.0f}, shouldCancel, messageHandler);
    SIMPLNX_RESULT_REQUIRE_VALID(engine());

    std::vector<float32> actual(pattern.size());
    SIMPLNX_RESULT_REQUIRE_VALID(outputStore.copyIntoBuffer(0, nonstd::span<float32>(actual.data(), actual.size())));
    REQUIRE(actual == expected);
    REQUIRE(manager.reservedWorkingMemoryBytes() == 0);
    return inputStore.readCount();
  };

  REQUIRE(run(k_PartialBudgetBytes) == dimZ);
  REQUIRE(run(k_CompleteBudgetBytes) == 1);
  manager.setBudgetBytes(previousBudget);
}

TEST_CASE("ImageProcessing::MaurerDistanceMapEngine: bounded 3D Z pass transfers each Y batch as one extent", "[ImageProcessing][MaurerDistanceMapEngine][WorkingMemory]")
{
  constexpr usize dimX = 5;
  constexpr usize dimY = 6;
  constexpr usize dimZ = 7;
  const SizeVec3 dims{dimX, dimY, dimZ};
  const std::vector<int32> pattern = MakePattern(dimX, dimY, dimZ);
  const std::vector<float32> expected = RunInCore<uint8>(pattern, dimX, dimY, dimZ, 0, false, true);

  auto& manager = CacheMemoryBudgetManager::instance();
  const uint64 previousBudget = manager.budgetBytes();
  manager.clear();
  manager.setBudgetBytes(1024);

  ReadCountingDataStore<uint8> inputStore(ShapeType{dimZ, dimY, dimX}, ShapeType{1}, uint8{0});
  ExtentCountingDataStore<float32> outputStore(ShapeType{dimZ, dimY, dimX}, ShapeType{1}, -1.0f);
  for(usize index = 0; index < pattern.size(); ++index)
  {
    inputStore.setValue(index, static_cast<uint8>(pattern[index]));
  }

  std::atomic_bool shouldCancel{false};
  IFilter::MessageHandler messageHandler{};
  MaurerDistanceWorkingMemory<uint8> engine(inputStore, outputStore, dims, uint8{0}, false, true, false, FloatVec3{1.0f, 1.0f, 1.0f}, shouldCancel, messageHandler);
  SIMPLNX_RESULT_REQUIRE_VALID(engine());

  std::vector<float32> actual(pattern.size());
  SIMPLNX_RESULT_REQUIRE_VALID(outputStore.copyIntoBuffer(0, nonstd::span<float32>(actual.data(), actual.size())));
  REQUIRE(actual == expected);
  REQUIRE(outputStore.extentWriteCount() == 1);
  REQUIRE(outputStore.flatWriteCount() == 0);
  REQUIRE(manager.reservedWorkingMemoryBytes() == 0);
  manager.setBudgetBytes(previousBudget);
}

TEST_CASE("ImageProcessing::MaurerDistanceMapEngine: encoded-sign line matches explicit inside mask", "[ImageProcessing][MaurerDistanceMapEngine]")
{
  constexpr float32 kMax = std::numeric_limits<float32>::max();
  const std::vector<float32> source = {kMax, 0.0f, kMax, kMax, 0.0f, kMax, 0.0f, kMax, kMax, 0.0f, kMax};
  const std::vector<uint8> inside = {0, 1, 0, 0, 1, 0, 1, 0, 1, 0, 1};

  for(const bool insideIsPositive : {false, true})
  {
    for(const bool useSpacing : {false, true})
    {
      const float32 spacing = useSpacing ? 1.5f : 1.0f;
      std::vector<float32> expected = source;
      std::vector<float32> encoded = source;
      for(usize index = 0; index < encoded.size(); ++index)
      {
        encoded[index] = ImageProcessing::detail::MaurerSignedValue(encoded[index], inside[index] != 0, insideIsPositive);
      }
      std::vector<float32> expectedG(source.size());
      std::vector<float32> expectedH(source.size());
      std::vector<float32> encodedG(source.size());
      std::vector<float32> encodedH(source.size());
      ImageProcessing::detail::Voronoi1D(expected, inside, expected.size(), insideIsPositive, useSpacing, spacing, expectedG, expectedH);
      ImageProcessing::detail::Voronoi1DEncodedSign(encoded, encoded.size(), useSpacing, spacing, encodedG, encodedH);

      REQUIRE(encoded.size() == expected.size());
      for(usize index = 0; index < encoded.size(); ++index)
      {
        REQUIRE(encoded[index] == expected[index]);
        REQUIRE(std::signbit(encoded[index]) == std::signbit(expected[index]));
      }
    }
  }
}

TEST_CASE("ImageProcessing::MaurerDistanceMapEngine: external envelope matches resident kernel across blocks", "[ImageProcessing][MaurerDistanceMapEngine]")
{
  constexpr float32 kMax = std::numeric_limits<float32>::max();
  const std::vector<std::vector<float32>> sources = {{kMax, 0.0f, kMax, kMax, 0.0f, kMax, 0.0f, kMax, kMax, 0.0f, kMax},
                                                     // The high candidate at index 4 is popped by index 6; the remaining zero-valued candidates span multiple
                                                     // three-value G/H blocks during evaluation.
                                                     {0.0f, kMax, 0.0f, kMax, 100.0f, kMax, 0.0f, kMax, 0.0f, kMax, 0.0f},
                                                     std::vector<float32>(11, kMax)};
  const std::vector<uint8> inside = {0, 1, 0, 0, 1, 0, 1, 0, 1, 0, 1};

  for(const std::vector<float32>& source : sources)
  {
    for(const bool insideIsPositive : {false, true})
    {
      for(const bool useSpacing : {false, true})
      {
        const float32 spacing = useSpacing ? 1.5f : 1.0f;
        CAPTURE(source, insideIsPositive, useSpacing, spacing);
        std::vector<float32> expected = source;
        std::vector<float32> residentG(source.size());
        std::vector<float32> residentH(source.size());
        ImageProcessing::detail::Voronoi1D(expected, inside, expected.size(), insideIsPositive, useSpacing, spacing, residentG, residentH);

        DataStore<float32> sourceStore(ShapeType{source.size()}, ShapeType{1}, 0.0f);
        DataStore<uint8> insideStore(ShapeType{inside.size()}, ShapeType{1}, 0);
        DataStore<float32> gStore(ShapeType{source.size()}, ShapeType{1}, 0.0f);
        DataStore<float32> hStore(ShapeType{source.size()}, ShapeType{1}, 0.0f);
        DataStore<float32> outputStore(ShapeType{source.size()}, ShapeType{1}, 0.0f);
        REQUIRE(sourceStore.copyFromBuffer(0, source).valid());
        REQUIRE(insideStore.copyFromBuffer(0, inside).valid());
        std::atomic_bool shouldCancel{false};
        const Result<> result =
            ImageProcessing::detail::ExternalVoronoi1D(sourceStore, insideStore, 0, source.size(), insideIsPositive, useSpacing, spacing, 3, gStore, hStore, outputStore, shouldCancel);
        REQUIRE(result.valid());
        std::vector<float32> actual(source.size());
        REQUIRE(outputStore.copyIntoBuffer(0, actual).valid());
        REQUIRE(actual == expected);
      }
    }
  }

  DataStore<float32> cancelledSource(ShapeType{sources.front().size()}, ShapeType{1}, 0.0f);
  DataStore<uint8> cancelledInside(ShapeType{inside.size()}, ShapeType{1}, 0);
  DataStore<float32> cancelledG(ShapeType{sources.front().size()}, ShapeType{1}, 0.0f);
  DataStore<float32> cancelledH(ShapeType{sources.front().size()}, ShapeType{1}, 0.0f);
  DataStore<float32> cancelledOutput(ShapeType{sources.front().size()}, ShapeType{1}, -123.0f);
  REQUIRE(cancelledSource.copyFromBuffer(0, sources.front()).valid());
  REQUIRE(cancelledInside.copyFromBuffer(0, inside).valid());
  std::atomic_bool shouldCancel{true};
  REQUIRE(
      ImageProcessing::detail::ExternalVoronoi1D(cancelledSource, cancelledInside, 0, sources.front().size(), false, false, 1.0f, 3, cancelledG, cancelledH, cancelledOutput, shouldCancel).valid());
  std::vector<float32> cancelledValues(sources.front().size());
  REQUIRE(cancelledOutput.copyIntoBuffer(0, cancelledValues).valid());
  REQUIRE(std::all_of(cancelledValues.cbegin(), cancelledValues.cend(), [](float32 value) { return value == -123.0f; }));

  DataStore<float32> midTransferSource(ShapeType{sources[1].size()}, ShapeType{1}, 0.0f);
  DataStore<uint8> midTransferInside(ShapeType{inside.size()}, ShapeType{1}, 0);
  DataStore<float32> midTransferG(ShapeType{sources[1].size()}, ShapeType{1}, 0.0f);
  DataStore<float32> midTransferH(ShapeType{sources[1].size()}, ShapeType{1}, 0.0f);
  DataStore<float32> midTransferOutput(ShapeType{sources[1].size()}, ShapeType{1}, -456.0f);
  REQUIRE(midTransferSource.copyFromBuffer(0, sources[1]).valid());
  REQUIRE(midTransferInside.copyFromBuffer(0, inside).valid());
  std::atomic_bool cancelDuringOutput{false};
  usize outputBlockCount = 0;
  auto cancellingSink = [&](usize blockBegin, nonstd::span<float32> values, nonstd::span<const uint8>) {
    ++outputBlockCount;
    Result<> result = midTransferOutput.copyFromBuffer(blockBegin, nonstd::span<const float32>(values.data(), values.size()));
    cancelDuringOutput.store(true);
    return result;
  };
  REQUIRE(ImageProcessing::detail::ExternalVoronoi1DToSink(midTransferSource, midTransferInside, 0, sources[1].size(), false, false, 1.0f, 3, midTransferG, midTransferH, cancellingSink,
                                                           cancelDuringOutput)
              .valid());
  REQUIRE(outputBlockCount == 1);
  std::vector<float32> partiallyWritten(sources[1].size());
  REQUIRE(midTransferOutput.copyIntoBuffer(0, partiallyWritten).valid());
  REQUIRE(std::none_of(partiallyWritten.cbegin(), partiallyWritten.cbegin() + 3, [](float32 value) { return value == -456.0f; }));
  REQUIRE(std::all_of(partiallyWritten.cbegin() + 3, partiallyWritten.cend(), [](float32 value) { return value == -456.0f; }));
}
