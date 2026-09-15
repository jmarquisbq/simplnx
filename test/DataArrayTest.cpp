#include "simplnx/DataStructure/DataArray.hpp"
#include "simplnx/Common/Array.hpp"
#include "simplnx/Common/TypesUtility.hpp"
#include "simplnx/Core/Application.hpp"
#include "simplnx/DataStructure/AttributeMatrix.hpp"
#include "simplnx/DataStructure/DataStore.hpp"
#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/DataStructure/EmptyDataStore.hpp"
#include "simplnx/DataStructure/IO/Generic/InMemoryFormatResolver.hpp"
#include "simplnx/UnitTest/UnitTestCommon.hpp"
#include "simplnx/Utilities/ArrayCreationUtilities.hpp"
#include "simplnx/Utilities/DataArrayUtilities.hpp"
#include "simplnx/Utilities/ParallelTaskAlgorithm.hpp"

#include <catch2/catch.hpp>
#include <fmt/ranges.h>

#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

using namespace nx::core;

namespace
{
constexpr StringLiteral k_BuildDir = SIMPLNX_BUILD_DIR;

class FailingResizeDataStore final : public DataStore<int32>
{
public:
  using DataStore<int32>::DataStore;

  /**
   * @brief Returns an injected resize failure for AttributeMatrix contract testing.
   * @param tupleShape Requested tuple dimensions.
   * @return Error -6035 for every request.
   */
  [[nodiscard]] Result<> resizeTuples(const ShapeType& tupleShape) override
  {
    return MakeErrorResult(-6035, fmt::format("Test store resize to shape [{}] failed: injected failure", fmt::join(tupleShape, ", ")));
  }
};

class RecordingFormatResolver final : public IDataStoreFormatResolver
{
public:
  std::string resolveFormat(const DataStructure&, const DataPath& arrayPath, DataType, uint64) const override
  {
    const std::scoped_lock lock(m_Mutex);
    m_RequestedPaths.push_back(arrayPath);
    return {};
  }

  std::vector<DataPath> requestedPaths() const
  {
    const std::scoped_lock lock(m_Mutex);
    return m_RequestedPaths;
  }

private:
  mutable std::mutex m_Mutex;
  mutable std::vector<DataPath> m_RequestedPaths;
};
} // namespace

TEST_CASE("Array")
{
  FloatVec3 vec0(3.0F, 4.0F, 5.0F);

  IntVec3 v0(1, 2, 3);
  IntVec3 v1(0, 0, 0);
  v1.setValues(1, 2, 3);
  REQUIRE(v1[0] == 1);
  REQUIRE(v1[1] == 2);
  REQUIRE(v1[2] == 3);

  auto v1Tuple = v1.toTuple();
  FloatVec3 v1F32 = v1.convertType<float>();

  FloatVec3 v2(-3.0F, -4.0F, -5.0F);
  FloatVec3 cross = vec0.cross(v2);
  REQUIRE(cross[0] == 0);
  REQUIRE(cross[1] == 0);
  REQUIRE(cross[2] == 0);

  float32 dot = v2.dot(vec0);
  REQUIRE(dot == -50.0F);

  float32 mag = vec0.magnitude();
  REQUIRE(mag == std::sqrt(50.0F));
}

TEST_CASE("DataArrayCreation")
{
  nx::core::DataStructure dataStructure;

  using DataStoreType = nx::core::DataStore<int32_t>;
  DataStoreType data_array = DataStoreType(nx::core::ShapeType{0}, nx::core::ShapeType{2}, 0);
  size_t numTuples = data_array.getNumberOfTuples();
  REQUIRE(numTuples == 0);

  UnitTest::CheckArraysInheritTupleDims(dataStructure);
}

TEMPLATE_TEST_CASE("CopyUsingIndexList maps data arrays", "[simplnx][DataArray][CopyUsingIndexList]", bool, int8, uint8, int16, uint16, int32, uint32, int64, uint64, float32, float64)
{
  DataStructure dataStructure;
  auto* sourceArrayPtr = DataArray<TestType>::Create(dataStructure, "Source", std::make_shared<DataStore<TestType>>(ShapeType{3}, ShapeType{2}, TestType{0}));
  auto* destArrayPtr = DataArray<TestType>::Create(dataStructure, "Destination", std::make_shared<DataStore<TestType>>(ShapeType{4}, ShapeType{2}, TestType{1}));
  REQUIRE(sourceArrayPtr != nullptr);
  REQUIRE(destArrayPtr != nullptr);
  const std::array<TestType, 6> sourceValues = {TestType{1}, TestType{0}, TestType{0}, TestType{1}, TestType{1}, TestType{1}};
  for(usize valueIndex = 0; valueIndex < sourceValues.size(); ++valueIndex)
  {
    (*sourceArrayPtr)[valueIndex] = sourceValues[valueIndex];
  }
  const std::vector<int64> indexMap = {2, -1, 0, 2};
  const nonstd::span<const int64> indexSpan(indexMap);
  CopyFromArray::ParallelTaskResult taskResult;
  ParallelTaskAlgorithm runner;
  runner.setParallelizationEnabled(false);
  CopyFromArray::RunParallelCopyUsingIndexList(*destArrayPtr, runner, taskResult, *sourceArrayPtr, indexSpan);
  runner.wait();
  REQUIRE(taskResult.takeResult().valid());
  const std::array<TestType, 8> expectedValues = {TestType{1}, TestType{1}, TestType{0}, TestType{0}, TestType{1}, TestType{0}, TestType{1}, TestType{1}};
  for(usize valueIndex = 0; valueIndex < expectedValues.size(); ++valueIndex)
  {
    REQUIRE((*destArrayPtr)[valueIndex] == expectedValues[valueIndex]);
  }
  UnitTest::CheckArraysInheritTupleDims(dataStructure);
}

TEST_CASE("CopyUsingIndexList maps neighbor lists", "[simplnx][NeighborList][CopyUsingIndexList]")
{
  DataStructure dataStructure;
  auto* sourceListPtr = NeighborList<int32>::Create(dataStructure, "Source", ShapeType{3});
  auto* destListPtr = NeighborList<int32>::Create(dataStructure, "Destination", ShapeType{4});
  REQUIRE(sourceListPtr != nullptr);
  REQUIRE(destListPtr != nullptr);
  sourceListPtr->setList(0, std::vector<int32>{7, 8});
  sourceListPtr->setList(1, std::vector<int32>{9});
  sourceListPtr->setList(2, std::vector<int32>{10, 11, 12});
  for(int32 tupleIndex = 0; tupleIndex < 4; ++tupleIndex)
  {
    destListPtr->setList(tupleIndex, std::vector<int32>{99});
  }
  const std::vector<int64> indexMap = {2, -1, 0, 2};
  const nonstd::span<const int64> indexSpan(indexMap);
  CopyFromArray::ParallelTaskResult taskResult;
  ParallelTaskAlgorithm runner;
  runner.setParallelizationEnabled(false);
  CopyFromArray::RunParallelCopyUsingIndexList(*destListPtr, runner, taskResult, *sourceListPtr, indexSpan);
  runner.wait();
  REQUIRE(taskResult.takeResult().valid());
  REQUIRE(destListPtr->getList(0) == std::vector<int32>{10, 11, 12});
  REQUIRE(destListPtr->getList(1).empty());
  REQUIRE(destListPtr->getList(2) == std::vector<int32>{7, 8});
  REQUIRE(destListPtr->getList(3) == std::vector<int32>{10, 11, 12});
  UnitTest::CheckArraysInheritTupleDims(dataStructure);
}

TEST_CASE("nx::core::IDataStore getPlannedStoreType", "[simplnx][DataStore]")
{
  // A real in-memory DataStore reports InMemory for both its actual and planned store type.
  DataStore<int32> inMemoryStore(ShapeType{4}, ShapeType{1}, 0);
  REQUIRE(inMemoryStore.getStoreType() == IDataStore::StoreType::InMemory);
  REQUIRE(inMemoryStore.getPlannedStoreType() == IDataStore::StoreType::InMemory);

  // An Empty placeholder with no data format is destined for in-memory storage.
  EmptyDataStore<int32> emptyInMemory(ShapeType{4}, ShapeType{1});
  REQUIRE(emptyInMemory.getStoreType() == IDataStore::StoreType::Empty);
  REQUIRE(emptyInMemory.getPlannedStoreType() == IDataStore::StoreType::InMemory);

  // An Empty placeholder stamped with an out-of-core data format is destined for out-of-core storage.
  EmptyDataStore<int32> emptyOutOfCore(ShapeType{4}, ShapeType{1}, "SomeOutOfCoreFormat");
  REQUIRE(emptyOutOfCore.getStoreType() == IDataStore::StoreType::Empty);
  REQUIRE(emptyOutOfCore.getPlannedStoreType() == IDataStore::StoreType::OutOfCore);
}

TEMPLATE_TEST_CASE("DataArray deepCopy preserves resident initialization", "[simplnx][DataArray][DeepCopyInitialization]", int32, float32, bool)
{
  DataStructure dataStructure;
  dataStructure.setFormatResolver(std::make_shared<InMemoryFormatResolver>());
  const std::optional<TestType> initValue = GENERATE(std::optional<TestType>{TestType{1}}, std::optional<TestType>{});
  CAPTURE(initValue.has_value());

  auto* sourceAMPtr = AttributeMatrix::Create(dataStructure, "Source", {1});
  REQUIRE(sourceAMPtr != nullptr);
  auto sourceStore = std::make_shared<DataStore<TestType>>(ShapeType{1}, ShapeType{2}, initValue);
  sourceStore->setValue(0, TestType{0});
  sourceStore->setValue(1, TestType{1});
  auto* sourceArrayPtr = DataArray<TestType>::Create(dataStructure, "Values", sourceStore, sourceAMPtr->getId());
  REQUIRE(sourceArrayPtr != nullptr);

  auto copy = sourceAMPtr->deepCopy(DataPath({"Copy"}));
  REQUIRE(copy != nullptr);
  AttributeMatrix* copyAMPtr = nullptr;
  DataArray<TestType>* copyArrayPtr = nullptr;
  REQUIRE_NOTHROW(copyAMPtr = &dataStructure.getDataRefAs<AttributeMatrix>(DataPath({"Copy"})));
  REQUIRE_NOTHROW(copyArrayPtr = &dataStructure.getDataRefAs<DataArray<TestType>>(DataPath({"Copy", "Values"})));
  REQUIRE(copyArrayPtr->getIDataStore() != sourceArrayPtr->getIDataStore());
  const auto* copyStorePtr = dynamic_cast<const DataStore<TestType>*>(copyArrayPtr->getIDataStore());
  REQUIRE(copyStorePtr != nullptr);
  CHECK(copyStorePtr->getInitValue() == initValue);

  const Result<> resizeResult = copyAMPtr->resizeTuples({4});
  SIMPLNX_RESULT_REQUIRE_VALID(resizeResult);
  CHECK(copyArrayPtr->getDataStoreRef().getValue(0) == TestType{0});
  CHECK(copyArrayPtr->getDataStoreRef().getValue(1) == TestType{1});
  const TestType expectedGrowthValue = initValue.value_or(GetMudflap<TestType>());
  for(usize valueIndex = 2; valueIndex < 8; ++valueIndex)
  {
    CHECK(copyArrayPtr->getDataStoreRef().getValue(valueIndex) == expectedGrowthValue);
  }

  CHECK(sourceArrayPtr->getNumberOfTuples() == 1);
  CHECK(sourceArrayPtr->getDataStoreRef().getValue(0) == TestType{0});
  CHECK(sourceArrayPtr->getDataStoreRef().getValue(1) == TestType{1});
  UnitTest::CheckArraysInheritTupleDims(dataStructure);
}

TEST_CASE("DataArray deepCopy preserves signed initialization and destination behavior", "[simplnx][DataArray][DeepCopyInitialization]")
{
  DataStructure dataStructure;
  auto resolver = std::make_shared<RecordingFormatResolver>();
  dataStructure.setFormatResolver(resolver);
  const DataPath sourcePath({"Source"});
  const DataPath copyPath({"Copy"});
  auto sourceStore = std::make_shared<DataStore<int32>>(ShapeType{1}, ShapeType{1}, std::optional<int32>{-1});
  sourceStore->setValue(0, 7);
  auto* sourceArrayPtr = DataArray<int32>::Create(dataStructure, sourcePath.getTargetName(), sourceStore);
  REQUIRE(sourceArrayPtr != nullptr);

  auto firstCopy = sourceArrayPtr->deepCopy(copyPath);
  REQUIRE(firstCopy != nullptr);
  DataArray<int32>* copyArrayPtr = nullptr;
  REQUIRE_NOTHROW(copyArrayPtr = &dataStructure.getDataRefAs<DataArray<int32>>(copyPath));
  REQUIRE(copyArrayPtr->getIDataStore() != sourceArrayPtr->getIDataStore());
  const std::vector<DataPath> expectedRequests = {copyPath};
  CHECK(resolver->requestedPaths() == expectedRequests);

  const Result<> resizeResult = copyArrayPtr->resizeTuples({4});
  SIMPLNX_RESULT_REQUIRE_VALID(resizeResult);
  const std::array<int32, 4> expectedValues = {7, -1, -1, -1};
  for(usize valueIndex = 0; valueIndex < expectedValues.size(); ++valueIndex)
  {
    CHECK(copyArrayPtr->getDataStoreRef().getValue(valueIndex) == expectedValues[valueIndex]);
  }
  CHECK(sourceArrayPtr->getNumberOfTuples() == 1);
  CHECK(sourceArrayPtr->getDataStoreRef().getValue(0) == 7);

  auto collision = sourceArrayPtr->deepCopy(copyPath);
  CHECK(collision == nullptr);
  CHECK(dataStructure.getDataAs<DataArray<int32>>(copyPath) == copyArrayPtr);
  CHECK(copyArrayPtr->getNumberOfTuples() == 4);
  for(usize valueIndex = 0; valueIndex < expectedValues.size(); ++valueIndex)
  {
    CHECK(copyArrayPtr->getDataStoreRef().getValue(valueIndex) == expectedValues[valueIndex]);
  }
  CHECK(resolver->requestedPaths() == expectedRequests);
  UnitTest::CheckArraysInheritTupleDims(dataStructure);
}

TEST_CASE("DataArray deepCopy preserves Empty store metadata", "[simplnx][DataArray][DeepCopyInitialization]")
{
  DataStructure dataStructure;
  auto sourceStore = std::make_shared<EmptyDataStore<int32>>(ShapeType{1}, ShapeType{2}, "planned-format");
  auto* sourceArrayPtr = DataArray<int32>::Create(dataStructure, "Source", sourceStore);
  REQUIRE(sourceArrayPtr != nullptr);

  auto copy = sourceArrayPtr->deepCopy(DataPath({"Copy"}));
  REQUIRE(copy != nullptr);
  DataArray<int32>* copyArrayPtr = nullptr;
  REQUIRE_NOTHROW(copyArrayPtr = &dataStructure.getDataRefAs<DataArray<int32>>(DataPath({"Copy"})));
  REQUIRE(copyArrayPtr->getIDataStore() != sourceArrayPtr->getIDataStore());

  const auto* sourceEmptyStorePtr = sourceArrayPtr->getIDataStoreAs<EmptyDataStore<int32>>();
  const auto* copyEmptyStorePtr = copyArrayPtr->getIDataStoreAs<EmptyDataStore<int32>>();
  REQUIRE(sourceEmptyStorePtr != nullptr);
  REQUIRE(copyEmptyStorePtr != nullptr);
  CHECK(copyEmptyStorePtr->getStoreType() == IDataStore::StoreType::Empty);
  CHECK(copyEmptyStorePtr->getPlannedStoreType() == IDataStore::StoreType::OutOfCore);
  CHECK(copyEmptyStorePtr->getTupleShape() == ShapeType{1});
  CHECK(copyEmptyStorePtr->getComponentShape() == ShapeType{2});
  CHECK(copyEmptyStorePtr->dataFormat() == "planned-format");
  CHECK(copyEmptyStorePtr->getTupleShape() == sourceEmptyStorePtr->getTupleShape());
  CHECK(copyEmptyStorePtr->getComponentShape() == sourceEmptyStorePtr->getComponentShape());
  CHECK(copyEmptyStorePtr->dataFormat() == sourceEmptyStorePtr->dataFormat());
  UnitTest::CheckArraysInheritTupleDims(dataStructure);
}

TEST_CASE("nx::core::DataArray Copy TupleTest", "[simplnx][DataArray]")
{
  UnitTest::LoadPlugins();

  const std::string k_DataArrayName("DataArray");
  const DataPath k_DataPath({k_DataArrayName});
  const usize k_NumTuples = 5;
  const usize k_NumComponents = 3;

  DataStructure dataStructure;
  ShapeType tupleShape{k_NumTuples};
  ShapeType componentShape{k_NumComponents};
  Result<> result = ArrayCreationUtilities::CreateArray<int32>(dataStructure, tupleShape, componentShape, k_DataPath, IDataAction::Mode::Execute);
  REQUIRE(result.valid() == true);

  auto& dataArray = dataStructure.getDataRefAs<DataArray<int32>>(k_DataPath);

  for(usize i = 0; i < k_NumTuples; i++)
  {
    dataArray.initializeTuple(i, static_cast<int32>(i));
  }

  for(usize tupleIndex = 0; tupleIndex < k_NumTuples; tupleIndex++)
  {
    for(usize componentIndex = 0; componentIndex < k_NumComponents; componentIndex++)
    {
      uint64 index = tupleIndex * 3 + componentIndex;
      REQUIRE(dataArray[index] == static_cast<int32>(tupleIndex));
    }
  }

  dataArray.copyTuple(4, 0);
  REQUIRE(dataArray[0] == 4);
  REQUIRE(dataArray[1] == 4);
  REQUIRE(dataArray[2] == 4);

  dataArray.copyTuple(1, 4);
  REQUIRE(dataArray[12] == 1);
  REQUIRE(dataArray[13] == 1);
  REQUIRE(dataArray[14] == 1);

  UnitTest::CheckArraysInheritTupleDims(dataStructure);
}

TEST_CASE("DataStore Test")
{
  ShapeType tupleShape{5};
  ShapeType componentShape{3};
  DataStore<int32> dataStore(tupleShape, componentShape, 5);

  REQUIRE(dataStore.getSize() == 15);

  for(uint64_t i = 0; i < dataStore.getSize(); i++)
  {
    REQUIRE(dataStore[i] == 5);
  }

  int32 newArrayValues[] = {6, 7, 8};
  dataStore.setTuple(0, newArrayValues);
  for(uint64 i = 0; i < 3; i++)
  {
    REQUIRE(dataStore[i] == newArrayValues[i]);
    REQUIRE(dataStore.getComponentValue(0, i) == newArrayValues[i]);
  }

  std::vector<int32> newValues{1, 2, 3};
  dataStore.setTuple(1, newValues);
  usize offset = dataStore.getNumberOfComponents();
  for(usize i = 0; i < newValues.size(); i++)
  {
    REQUIRE(dataStore[offset + i] == newValues[i]);
    REQUIRE(dataStore.getComponentValue(1, i) == newValues[i]);
  }

  dataStore.setComponent(2, 2, 99);
  REQUIRE(dataStore[8] == 99);
  REQUIRE(dataStore.getComponentValue(2, 2) == 99);
}

TEST_CASE("DataStore allocation contract", "[simplnx][DataStore]")
{
  SECTION("initialized growth preserves the prefix and initializes the tail")
  {
    DataStore<int32> dataStore(ShapeType{2}, ShapeType{2}, 7);
    const std::array<int32, 4> initialValues = {7, 7, 7, 7};
    for(usize valueIndex = 0; valueIndex < initialValues.size(); ++valueIndex)
    {
      REQUIRE(dataStore[valueIndex] == initialValues[valueIndex]);
    }

    const std::array<int32, 4> prefixValues = {10, 11, 20, 21};
    for(usize valueIndex = 0; valueIndex < prefixValues.size(); ++valueIndex)
    {
      dataStore[valueIndex] = prefixValues[valueIndex];
    }

    const Result<> resizeResult = dataStore.resizeTuples(ShapeType{4});
    REQUIRE(resizeResult.valid());
    REQUIRE(dataStore.getTupleShape() == ShapeType{4});
    REQUIRE(dataStore.getComponentShape() == ShapeType{2});
    REQUIRE(dataStore.getNumberOfTuples() == 4);
    REQUIRE(dataStore.getNumberOfComponents() == 2);
    REQUIRE(dataStore.getSize() == 8);

    const std::array<int32, 8> expectedValues = {10, 11, 20, 21, 7, 7, 7, 7};
    for(usize valueIndex = 0; valueIndex < expectedValues.size(); ++valueIndex)
    {
      REQUIRE(dataStore[valueIndex] == expectedValues[valueIndex]);
    }
  }

  SECTION("zero-sized initialized storage grows to initialized values")
  {
    DataStore<int32> dataStore(ShapeType{0}, ShapeType{2}, 7);
    REQUIRE(dataStore.getTupleShape() == ShapeType{0});
    REQUIRE(dataStore.getComponentShape() == ShapeType{2});
    REQUIRE(dataStore.getNumberOfTuples() == 0);
    REQUIRE(dataStore.getNumberOfComponents() == 2);
    REQUIRE(dataStore.getSize() == 0);

    const Result<> resizeResult = dataStore.resizeTuples(ShapeType{2});
    REQUIRE(resizeResult.valid());
    REQUIRE(dataStore.getTupleShape() == ShapeType{2});
    REQUIRE(dataStore.getComponentShape() == ShapeType{2});
    REQUIRE(dataStore.getNumberOfTuples() == 2);
    REQUIRE(dataStore.getNumberOfComponents() == 2);
    REQUIRE(dataStore.getSize() == 4);

    const std::array<int32, 4> expectedValues = {7, 7, 7, 7};
    for(usize valueIndex = 0; valueIndex < expectedValues.size(); ++valueIndex)
    {
      REQUIRE(dataStore[valueIndex] == expectedValues[valueIndex]);
    }
  }

  SECTION("copy construction preserves values and remembered initialization")
  {
    DataStore<int32> sourceDataStore(ShapeType{2}, ShapeType{2}, 7);
    const std::array<int32, 4> originalValues = {10, 11, 20, 21};
    for(usize valueIndex = 0; valueIndex < originalValues.size(); ++valueIndex)
    {
      sourceDataStore[valueIndex] = originalValues[valueIndex];
    }

    DataStore<int32> copiedDataStore(sourceDataStore);
    REQUIRE(copiedDataStore.getTupleShape() == ShapeType{2});
    REQUIRE(copiedDataStore.getComponentShape() == ShapeType{2});
    REQUIRE(copiedDataStore.getNumberOfTuples() == 2);
    REQUIRE(copiedDataStore.getNumberOfComponents() == 2);
    REQUIRE(copiedDataStore.getSize() == 4);
    REQUIRE(copiedDataStore.data() != sourceDataStore.data());
    for(usize valueIndex = 0; valueIndex < originalValues.size(); ++valueIndex)
    {
      REQUIRE(copiedDataStore[valueIndex] == originalValues[valueIndex]);
    }

    sourceDataStore[0] = 99;
    const std::array<int32, 4> changedSourceValues = {99, 11, 20, 21};
    for(usize valueIndex = 0; valueIndex < originalValues.size(); ++valueIndex)
    {
      REQUIRE(sourceDataStore[valueIndex] == changedSourceValues[valueIndex]);
      REQUIRE(copiedDataStore[valueIndex] == originalValues[valueIndex]);
    }

    const Result<> resizeResult = copiedDataStore.resizeTuples(ShapeType{3});
    REQUIRE(resizeResult.valid());
    REQUIRE(copiedDataStore.getTupleShape() == ShapeType{3});
    REQUIRE(copiedDataStore.getComponentShape() == ShapeType{2});
    REQUIRE(copiedDataStore.getNumberOfTuples() == 3);
    REQUIRE(copiedDataStore.getNumberOfComponents() == 2);
    REQUIRE(copiedDataStore.getSize() == 6);
    const std::array<int32, 6> expectedCopiedValues = {10, 11, 20, 21, 7, 7};
    for(usize valueIndex = 0; valueIndex < expectedCopiedValues.size(); ++valueIndex)
    {
      REQUIRE(copiedDataStore[valueIndex] == expectedCopiedValues[valueIndex]);
    }

    REQUIRE(sourceDataStore.getTupleShape() == ShapeType{2});
    REQUIRE(sourceDataStore.getComponentShape() == ShapeType{2});
    REQUIRE(sourceDataStore.getNumberOfTuples() == 2);
    REQUIRE(sourceDataStore.getNumberOfComponents() == 2);
    REQUIRE(sourceDataStore.getSize() == 4);
    for(usize valueIndex = 0; valueIndex < changedSourceValues.size(); ++valueIndex)
    {
      REQUIRE(sourceDataStore[valueIndex] == changedSourceValues[valueIndex]);
    }
  }

  SECTION("caller-owned storage grows with the integer mudflap value")
  {
    auto buffer = std::make_unique<int32[]>(4);
    const std::array<int32, 4> initialValues = {31, 32, 33, 34};
    for(usize valueIndex = 0; valueIndex < initialValues.size(); ++valueIndex)
    {
      buffer[valueIndex] = initialValues[valueIndex];
    }
    const int32* const bufferPtr = buffer.get();

    DataStore<int32> dataStore(std::move(buffer), ShapeType{2}, ShapeType{2});
    REQUIRE(buffer == nullptr);
    REQUIRE(dataStore.data() == bufferPtr);
    for(usize valueIndex = 0; valueIndex < initialValues.size(); ++valueIndex)
    {
      REQUIRE(dataStore[valueIndex] == initialValues[valueIndex]);
    }

    const Result<> resizeResult = dataStore.resizeTuples(ShapeType{3});
    REQUIRE(resizeResult.valid());
    REQUIRE(dataStore.getTupleShape() == ShapeType{3});
    REQUIRE(dataStore.getComponentShape() == ShapeType{2});
    REQUIRE(dataStore.getNumberOfTuples() == 3);
    REQUIRE(dataStore.getNumberOfComponents() == 2);
    REQUIRE(dataStore.getSize() == 6);
    const std::array<int32, 6> expectedValues = {31, 32, 33, 34, GetMudflap<int32>(), GetMudflap<int32>()};
    for(usize valueIndex = 0; valueIndex < expectedValues.size(); ++valueIndex)
    {
      REQUIRE(dataStore[valueIndex] == expectedValues[valueIndex]);
    }
  }

  SECTION("same-size reshape preserves the buffer and values")
  {
    DataStore<int32> dataStore(ShapeType{4}, ShapeType{1}, 7);
    const std::array<int32, 4> expectedValues = {10, 20, 30, 40};
    for(usize valueIndex = 0; valueIndex < expectedValues.size(); ++valueIndex)
    {
      dataStore[valueIndex] = expectedValues[valueIndex];
    }
    const int32* const originalDataPtr = dataStore.data();

    const Result<> resizeResult = dataStore.resizeTuples(ShapeType{2, 2});
    REQUIRE(resizeResult.valid());
    REQUIRE(dataStore.getTupleShape() == ShapeType{2, 2});
    REQUIRE(dataStore.getComponentShape() == ShapeType{1});
    REQUIRE(dataStore.getNumberOfTuples() == 4);
    REQUIRE(dataStore.getNumberOfComponents() == 1);
    REQUIRE(dataStore.getSize() == 4);
    REQUIRE(dataStore.data() == originalDataPtr);
    for(usize valueIndex = 0; valueIndex < expectedValues.size(); ++valueIndex)
    {
      REQUIRE(dataStore[valueIndex] == expectedValues[valueIndex]);
    }
  }

  SECTION("impossible array length preserves the original state")
  {
    DataStore<int32> dataStore(ShapeType{4}, ShapeType{1}, 7);
    const std::array<int32, 4> expectedValues = {10, 20, 30, 40};
    for(usize valueIndex = 0; valueIndex < expectedValues.size(); ++valueIndex)
    {
      dataStore[valueIndex] = expectedValues[valueIndex];
    }
    const int32* const originalDataPtr = dataStore.data();
    const ShapeType originalTupleShape = dataStore.getTupleShape();
    const ShapeType originalComponentShape = dataStore.getComponentShape();
    const usize originalTupleCount = dataStore.getNumberOfTuples();
    const usize originalComponentCount = dataStore.getNumberOfComponents();
    const usize originalSize = dataStore.getSize();
    // The element count fits usize, but its int32 byte count does not. Array new rejects the invalid length before it requests memory.
    const usize impossibleTuples = (std::numeric_limits<usize>::max)() / sizeof(int32) + 1;

    const Result<> resizeResult = dataStore.resizeTuples(ShapeType{impossibleTuples});
    REQUIRE(resizeResult.invalid());
    REQUIRE_FALSE(resizeResult.errors().empty());
    REQUIRE(resizeResult.errors()[0].code == -6035);
    REQUIRE(dataStore.data() == originalDataPtr);
    REQUIRE(dataStore.getTupleShape() == originalTupleShape);
    REQUIRE(dataStore.getComponentShape() == originalComponentShape);
    REQUIRE(dataStore.getNumberOfTuples() == originalTupleCount);
    REQUIRE(dataStore.getNumberOfComponents() == originalComponentCount);
    REQUIRE(dataStore.getSize() == originalSize);
    for(usize valueIndex = 0; valueIndex < expectedValues.size(); ++valueIndex)
    {
      REQUIRE(dataStore[valueIndex] == expectedValues[valueIndex]);
    }
  }
}

TEST_CASE("DataStore caller-owned extent buffers preserve values and validate before writing", "[simplnx][DataStore]")
{
  DataStore<int32> dataStore(ShapeType{2, 3, 4}, ShapeType{2}, 0);
  for(usize tupleIndex = 0; tupleIndex < dataStore.getNumberOfTuples(); ++tupleIndex)
  {
    dataStore[tupleIndex * 2] = static_cast<int32>(tupleIndex * 10);
    dataStore[tupleIndex * 2 + 1] = static_cast<int32>(tupleIndex * 10 + 1);
  }

  const Extent mainExtent({0, 0, 1}, {1, 2, 3}, {1, 2, 1});
  const std::vector<int32> expectedMain = {10, 11, 20, 21, 30, 31, 90, 91, 100, 101, 110, 111, 130, 131, 140, 141, 150, 151, 210, 211, 220, 221, 230, 231};
  std::vector<int32> mainValues(expectedMain.size(), -1);
  REQUIRE(dataStore.readExtentIntoBuffer(mainExtent, nonstd::span<int32>(mainValues.data(), mainValues.size())).valid());
  REQUIRE(mainValues == expectedMain);

  const Extent faceExtent({1, 1, 0}, {1, 2, 3});
  const std::vector<int32> expectedFace = {160, 161, 170, 171, 180, 181, 190, 191, 200, 201, 210, 211, 220, 221, 230, 231};
  std::vector<int32> multiMain(expectedMain.size(), -1);
  std::vector<int32> faceValues(expectedFace.size(), -1);
  std::array<Extent, 2> extents = {mainExtent, faceExtent};
  std::array<nonstd::span<int32>, 2> destinations = {
      nonstd::span<int32>(multiMain.data(), multiMain.size()),
      nonstd::span<int32>(faceValues.data(), faceValues.size()),
  };
  REQUIRE(dataStore.readExtentsIntoBuffers(nonstd::span<const Extent>(extents.data(), extents.size()), nonstd::span<nonstd::span<int32>>(destinations.data(), destinations.size())).valid());
  REQUIRE(multiMain == expectedMain);
  REQUIRE(faceValues == expectedFace);

  std::vector<int32> countMismatchValues(expectedMain.size(), -777);
  std::array<nonstd::span<int32>, 1> countMismatchDestination = {nonstd::span<int32>(countMismatchValues.data(), countMismatchValues.size())};
  Result<> countMismatchResult =
      dataStore.readExtentsIntoBuffers(nonstd::span<const Extent>(extents.data(), extents.size()), nonstd::span<nonstd::span<int32>>(countMismatchDestination.data(), countMismatchDestination.size()));
  REQUIRE(countMismatchResult.invalid());
  REQUIRE(countMismatchResult.errors()[0].code == -6034);
  REQUIRE(countMismatchValues == std::vector<int32>(expectedMain.size(), -777));

  std::vector<int32> shortMain(expectedMain.size() - 1, -777);
  std::vector<int32> untouchedFace(expectedFace.size(), -777);
  std::array<nonstd::span<int32>, 2> sizeMismatchDestinations = {
      nonstd::span<int32>(shortMain.data(), shortMain.size()),
      nonstd::span<int32>(untouchedFace.data(), untouchedFace.size()),
  };
  Result<> sizeMismatchResult =
      dataStore.readExtentsIntoBuffers(nonstd::span<const Extent>(extents.data(), extents.size()), nonstd::span<nonstd::span<int32>>(sizeMismatchDestinations.data(), sizeMismatchDestinations.size()));
  REQUIRE(sizeMismatchResult.invalid());
  REQUIRE(sizeMismatchResult.errors()[0].code == -6034);
  REQUIRE(shortMain == std::vector<int32>(expectedMain.size() - 1, -777));
  REQUIRE(untouchedFace == std::vector<int32>(expectedFace.size(), -777));
}

TEST_CASE("DataStore caller-owned extent buffers support two-dimensional stores", "[simplnx][DataStore]")
{
  DataStore<int32> dataStore(ShapeType{3, 4}, ShapeType{1}, 0);
  for(usize valueIndex = 0; valueIndex < dataStore.getSize(); ++valueIndex)
  {
    dataStore[valueIndex] = static_cast<int32>(valueIndex);
  }

  const Extent extent({0, 1}, {2, 3}, {2, 1});
  std::vector<int32> values(6, -1);
  REQUIRE(dataStore.readExtentIntoBuffer(extent, nonstd::span<int32>(values.data(), values.size())).valid());
  REQUIRE(values == std::vector<int32>{1, 2, 3, 9, 10, 11});
}

TEST_CASE("DataStore::writeExtent rejects unsupported rank instead of silently succeeding", "[DataStore]")
{
  DataStore<int32> store(ShapeType{10, 10}, ShapeType{1}, 0);
  std::vector<int32> data(4, 7);
  Extent extent(std::vector<uint64>{0, 0}, std::vector<uint64>{1, 1}, std::vector<uint64>{1, 1});
  Result<> result = store.writeExtent(extent, data);
  REQUIRE(result.invalid());
  REQUIRE(result.errors()[0].code == -6037);
}

TEST_CASE("AttributeMatrix::resizeTuples reports the failing child", "[AttributeMatrix]")
{
  DataStructure dataStructure;
  auto* attributeMatrix = AttributeMatrix::Create(dataStructure, "AM", ShapeType{4});
  REQUIRE(attributeMatrix != nullptr);
  REQUIRE(DataArray<int32>::CreateWithStore<DataStore<int32>>(dataStructure, "first", ShapeType{4}, ShapeType{1}, attributeMatrix->getId()) != nullptr);
  REQUIRE(DataArray<int32>::CreateWithStore<DataStore<int32>>(dataStructure, "second", ShapeType{4}, ShapeType{1}, attributeMatrix->getId()) != nullptr);

  const auto children = attributeMatrix->findAllChildrenOfType<IArray>();
  REQUIRE(children.size() == 2);
  auto childIterator = children.begin();
  auto* good = dynamic_cast<DataArray<int32>*>(childIterator->get());
  REQUIRE(good != nullptr);
  ++childIterator;
  auto* bad = dynamic_cast<DataArray<int32>*>(childIterator->get());
  REQUIRE(bad != nullptr);
  bad->setDataStore(std::make_shared<FailingResizeDataStore>(ShapeType{4}, ShapeType{1}, 0));
  const std::string badName = bad->getName();

  Result<> result = attributeMatrix->resizeTuples(ShapeType{8});

  REQUIRE(result.invalid());
  REQUIRE(result.errors()[0].message.find(badName) != std::string::npos);
  REQUIRE(good->getNumberOfTuples() == 8);
}

TEST_CASE("Copy DataStore", "DataArray")
{
  ShapeType tupleShape{5};
  ShapeType componentShape{3};
  DataStore<int32> dataStore(tupleShape, componentShape, 5);
  usize size = dataStore.getSize();
  for(usize i = 0; i < size; i++)
  {
    dataStore[i] = i;
  }

  DataStore<int32> dataStore2(tupleShape, componentShape, 5);
  dataStore2.copy(dataStore);

  for(usize i = 0; i < size; i++)
  {
    REQUIRE(dataStore[i] == dataStore2[i]);
  }
}

template <typename T>
void TestDataArrayToFromString(DataStructure& datastructure, const std::string& arrayName, const std::string& minCompareStr, const std::string& maxCompareStr)
{
  constexpr usize index1 = 5;
  constexpr usize index2 = 6;
  constexpr usize index3 = 7;
  constexpr usize index4 = 8;
  constexpr usize index5 = 9;
  constexpr usize index6 = 10;
  const ShapeType tupleShape = {5, 4, 3};
  const ShapeType compShape = {1};
  auto* dataArray = UnitTest::CreateTestDataArray<T>(datastructure, arrayName, tupleShape, compShape);
  constexpr T minVal = std::numeric_limits<T>::min();
  constexpr T maxVal = std::numeric_limits<T>::max();
  dataArray->setValue(index2, minVal);
  dataArray->setValue(index3, maxVal);
  const std::string minStr = dataArray->toString(index2, 0);
  const std::string maxStr = dataArray->toString(index3, 0);
  // std::cout << "DataArray<T>\n\tmin = " << minStr << "\n\tmax = " << maxStr << "\n";
  REQUIRE(minStr == minCompareStr);
  REQUIRE(maxStr == maxCompareStr);
  REQUIRE(dataArray->setValueFromString(index5, 0, minStr));
  REQUIRE(dataArray->setValueFromString(index6, 0, maxStr));
  // std::cout << "DataArray<T>\n\tmin : " << dataArray->getValue(index5) << " == " << minVal << "\n\tmax : " << dataArray->getValue(index6) << " == " << maxVal << "\n";
  REQUIRE(dataArray->getValue(index5) == minVal);
  REQUIRE(dataArray->getValue(index6) == maxVal);
}

template <>
void TestDataArrayToFromString<float32>(DataStructure& datastructure, const std::string& arrayName, const std::string& minCompareStr, const std::string& maxCompareStr)
{
  constexpr usize index1 = 5;
  constexpr usize index2 = 6;
  constexpr usize index3 = 7;
  constexpr usize index4 = 8;
  constexpr usize index5 = 9;
  constexpr usize index6 = 10;
  constexpr usize index7 = 11;
  constexpr usize index8 = 12;
  constexpr usize index9 = 13;
  constexpr usize index10 = 14;
  const ShapeType tupleShape = {5, 4, 3};
  const ShapeType compShape = {1};
  auto* dataArray = UnitTest::CreateTestDataArray<float32>(datastructure, arrayName, tupleShape, compShape);
  constexpr float32 lowestVal = std::numeric_limits<float32>::lowest();
  constexpr float32 minVal = std::numeric_limits<float32>::min();
  constexpr float32 maxVal = std::numeric_limits<float32>::max();
  constexpr float32 infVal = std::numeric_limits<float32>::infinity();
  const float32 nanVal = std::nanf("1");
  dataArray->setValue(index1, lowestVal);
  dataArray->setValue(index2, minVal);
  dataArray->setValue(index3, maxVal);
  dataArray->setValue(index4, infVal);
  dataArray->setValue(index5, nanVal);
  const std::string lowestStr = dataArray->toString(index1, 0);
  const std::string minStr = dataArray->toString(index2, 0);
  const std::string maxStr = dataArray->toString(index3, 0);
  const std::string infStr = dataArray->toString(index4, 0);
  const std::string nanStr = dataArray->toString(index5, 0);
  // std::cout << "DataArray<float32>\n\tlowest = " << lowestStr << "\n\tmin = " << minStr << "\n\tmax = " << maxStr << "\n\tinf = " << infStr << "\n\tnan = " << nanStr << "\n";
  REQUIRE(lowestStr == "-3.4028235e+38");
  REQUIRE(minStr == minCompareStr);
  REQUIRE(maxStr == maxCompareStr);
  REQUIRE(infStr == "inf");
  REQUIRE(nanStr == "nan");
  REQUIRE(dataArray->toString(index2, 0, "{:.8g}") == "1.1754944e-38");
  REQUIRE(dataArray->toString(index3, 0, "{:.6f}") == "340282346638528859811704183484516925440.000000");
  REQUIRE(dataArray->toString(index2, 0, "{:.4e}") == "1.1755e-38");
  REQUIRE(dataArray->setValueFromString(index6, 0, lowestStr));
  REQUIRE(dataArray->setValueFromString(index7, 0, minStr));
  REQUIRE(dataArray->setValueFromString(index8, 0, maxStr));
  REQUIRE(dataArray->setValueFromString(index9, 0, infStr));
  REQUIRE(dataArray->setValueFromString(index10, 0, nanStr));
  // std::cout << "DataArray<float32>\n\tlowest : " << dataArray->getValue(index6) << " == " << lowestVal << "\n\tmin : " << dataArray->getValue(index7) << " == " << minVal
  //<< "\n\tmax : " << dataArray->getValue(index8) << " == " << maxVal << "\n\tinf : " << dataArray->getValue(index9) << " == " << infVal << "\n\tnan : " << dataArray->getValue(index10)
  //<< " == " << nanVal << "\n";
  REQUIRE(dataArray->getValue(index6) == lowestVal);
  REQUIRE(dataArray->getValue(index7) == minVal);
  REQUIRE(dataArray->getValue(index8) == maxVal);
  REQUIRE(dataArray->getValue(index9) == infVal);
  REQUIRE(std::isnan(dataArray->getValue(index10)));
}
template <>
void TestDataArrayToFromString<float64>(DataStructure& datastructure, const std::string& arrayName, const std::string& minCompareStr, const std::string& maxCompareStr)
{
  constexpr usize index1 = 5;
  constexpr usize index2 = 6;
  constexpr usize index3 = 7;
  constexpr usize index4 = 8;
  constexpr usize index5 = 9;
  constexpr usize index6 = 10;
  constexpr usize index7 = 11;
  constexpr usize index8 = 12;
  constexpr usize index9 = 13;
  constexpr usize index10 = 14;
  const ShapeType tupleShape = {5, 4, 3};
  const ShapeType compShape = {1};
  auto* dataArray = UnitTest::CreateTestDataArray<float64>(datastructure, arrayName, tupleShape, compShape);
  constexpr float64 lowestVal = std::numeric_limits<float64>::lowest();
  constexpr float64 minVal = std::numeric_limits<float64>::min();
  constexpr float64 maxVal = std::numeric_limits<float64>::max();
  constexpr float64 infVal = std::numeric_limits<float64>::infinity();
  const float64 nanVal = std::nan("1");
  dataArray->setValue(index1, lowestVal);
  dataArray->setValue(index2, minVal);
  dataArray->setValue(index3, maxVal);
  dataArray->setValue(index4, infVal);
  dataArray->setValue(index5, nanVal);
  const std::string lowestStr = dataArray->toString(index1, 0);
  const std::string minStr = dataArray->toString(index2, 0);
  const std::string maxStr = dataArray->toString(index3, 0);
  const std::string infStr = dataArray->toString(index4, 0);
  const std::string nanStr = dataArray->toString(index5, 0);
  // std::cout << "DataArray<float64>\n\tlowest = " << lowestStr << "\n\tmin = " << minStr << "\n\tmax = " << maxStr << "\n\tinf = " << infStr << "\n\tnan = " << nanStr << "\n";
  REQUIRE(lowestStr == "-1.7976931348623157e+308");
  REQUIRE(minStr == minCompareStr);
  REQUIRE(maxStr == maxCompareStr);
  REQUIRE(infStr == "inf");
  REQUIRE(nanStr == "nan");
  REQUIRE(dataArray->toString(index2, 0, "{:.8g}") == "2.2250739e-308");
  REQUIRE(dataArray->toString(index2, 0, "{:.6f}") == "0.000000");
  REQUIRE(dataArray->toString(index2, 0, "{:.4e}") == "2.2251e-308");
  REQUIRE(dataArray->setValueFromString(index6, 0, lowestStr));
  REQUIRE(dataArray->setValueFromString(index7, 0, minStr));
  REQUIRE(dataArray->setValueFromString(index8, 0, maxStr));
  REQUIRE(dataArray->setValueFromString(index9, 0, infStr));
  REQUIRE(dataArray->setValueFromString(index10, 0, nanStr));
  // std::cout << "DataArray<float64>\n\tlowest : " << dataArray->getValue(index6) << " == " << lowestVal << "\n\tmin : " << dataArray->getValue(index7) << " == " << minVal
  //<< "\n\tmax : " << dataArray->getValue(index8) << " == " << maxVal << "\n\tinf : " << dataArray->getValue(index9) << " == " << infVal << "\n\tnan : " << dataArray->getValue(index10)
  //<< " == " << nanVal << "\n";
  REQUIRE(dataArray->getValue(index6) == lowestVal);
  REQUIRE(dataArray->getValue(index7) == minVal);
  REQUIRE(dataArray->getValue(index8) == maxVal);
  REQUIRE(dataArray->getValue(index9) == infVal);
  REQUIRE(std::isnan(dataArray->getValue(index10)));
}

template <typename T>
void TestNeighborListArrayToFromString(DataStructure& datastructure, const std::string& arrayName, const std::string& minCompareStr, const std::string& maxCompareStr)
{
  constexpr usize index1 = 5;
  constexpr usize index2 = 6;
  constexpr usize index3 = 7;
  constexpr usize index4 = 8;
  constexpr usize index5 = 9;
  constexpr usize index6 = 10;
  const ShapeType tupleShape = {5, 4, 3};
  const ShapeType compShape = {1};
  auto* neighborListArray = UnitTest::CreateTestNeighborList<T>(datastructure, arrayName, 10, {});
  constexpr T minVal = std::numeric_limits<T>::min();
  constexpr T maxVal = std::numeric_limits<T>::max();
  neighborListArray->addEntry(index2, minVal);
  neighborListArray->addEntry(index3, maxVal);
  neighborListArray->addEntry(index5, 0);
  neighborListArray->addEntry(index6, 0);
  const std::string minStr = neighborListArray->toString(index2, 0);
  const std::string maxStr = neighborListArray->toString(index3, 0);
  // std::cout << "NeighborListArray<T>\n\tmin = " << minStr << "\n\tmax = " << maxStr << "\n";
  REQUIRE(minStr == minCompareStr);
  REQUIRE(maxStr == maxCompareStr);
  REQUIRE(neighborListArray->setValueFromString(index5, 0, minStr));
  REQUIRE(neighborListArray->setValueFromString(index6, 0, maxStr));
  bool ok;
  // std::cout << "NeighborListArray<T>\n\tmin : " << neighborListArray->getValue(index5, 0, ok) << " == " << minVal << "\n\tmax : " << neighborListArray->getValue(index6, 0, ok) << " == " << maxVal
  //<< "\n";
  REQUIRE(neighborListArray->getValue(index5, 0, ok) == minVal);
  REQUIRE(ok);
  REQUIRE(neighborListArray->getValue(index6, 0, ok) == maxVal);
  REQUIRE(ok);
}

template <>
void TestNeighborListArrayToFromString<float32>(DataStructure& datastructure, const std::string& arrayName, const std::string& minCompareStr, const std::string& maxCompareStr)
{
  constexpr usize index1 = 5;
  constexpr usize index2 = 6;
  constexpr usize index3 = 7;
  constexpr usize index4 = 8;
  constexpr usize index5 = 9;
  constexpr usize index6 = 10;
  constexpr usize index7 = 11;
  constexpr usize index8 = 12;
  constexpr usize index9 = 13;
  constexpr usize index10 = 14;
  const ShapeType tupleShape = {5, 4, 3};
  const ShapeType compShape = {1};
  auto* neighborListFloat32 = UnitTest::CreateTestNeighborList<float32>(datastructure, arrayName, 10, {});
  constexpr float32 lowestVal = std::numeric_limits<float32>::lowest();
  constexpr float32 minVal = std::numeric_limits<float32>::min();
  constexpr float32 maxVal = std::numeric_limits<float32>::max();
  constexpr float32 infVal = std::numeric_limits<float32>::infinity();
  const float32 nanVal = std::nanf("1");
  neighborListFloat32->addEntry(index1, lowestVal);
  neighborListFloat32->addEntry(index2, minVal);
  neighborListFloat32->addEntry(index3, maxVal);
  neighborListFloat32->addEntry(index4, infVal);
  neighborListFloat32->addEntry(index5, nanVal);
  neighborListFloat32->addEntry(index6, 0);
  neighborListFloat32->addEntry(index7, 0);
  neighborListFloat32->addEntry(index8, 0);
  neighborListFloat32->addEntry(index9, 0);
  neighborListFloat32->addEntry(index10, 0);
  const std::string lowestStr = neighborListFloat32->toString(index1, 0);
  const std::string minStr = neighborListFloat32->toString(index2, 0);
  const std::string maxStr = neighborListFloat32->toString(index3, 0);
  const std::string infStr = neighborListFloat32->toString(index4, 0);
  const std::string nanStr = neighborListFloat32->toString(index5, 0);
  // std::cout << "NeighborListArray<float32>\n\tlowest = " << lowestStr << "\n\tmin = " << minStr << "\n\tmax = " << maxStr << "\n\tinf = " << infStr << "\n\tnan = " << nanStr << "\n";
  REQUIRE(lowestStr == "-3.4028235e+38");
  REQUIRE(minStr == minCompareStr);
  REQUIRE(maxStr == maxCompareStr);
  REQUIRE(infStr == "inf");
  REQUIRE(nanStr == "nan");
  REQUIRE(neighborListFloat32->toString(index2, 0, "{:.8g}") == "1.1754944e-38");
  REQUIRE(neighborListFloat32->toString(index3, 0, "{:.6f}") == "340282346638528859811704183484516925440.000000");
  REQUIRE(neighborListFloat32->toString(index2, 0, "{:.4e}") == "1.1755e-38");
  REQUIRE(neighborListFloat32->setValueFromString(index6, 0, lowestStr));
  REQUIRE(neighborListFloat32->setValueFromString(index7, 0, minStr));
  REQUIRE(neighborListFloat32->setValueFromString(index8, 0, maxStr));
  REQUIRE(neighborListFloat32->setValueFromString(index9, 0, infStr));
  REQUIRE(neighborListFloat32->setValueFromString(index10, 0, nanStr));
  // std::cout << "NeighborListArray<float32>\n\tlowest : " << neighborListFloat32->getValue(index6) << " == " << lowestVal << "\n\tmin : " << neighborListFloat32->getValue(index7) << " == " << minVal
  //<< "\n\tmax : " << neighborListFloat32->getValue(index8) << " == " << maxVal << "\n\tinf : " << neighborListFloat32->getValue(index9) << " == " << infVal << "\n\tnan : " <<
  // neighborListFloat32->getValue(index10) << " == " << nanVal << "\n";
  bool ok;
  REQUIRE(neighborListFloat32->getValue(index6, 0, ok) == lowestVal);
  REQUIRE(ok);
  REQUIRE(neighborListFloat32->getValue(index7, 0, ok) == minVal);
  REQUIRE(ok);
  REQUIRE(neighborListFloat32->getValue(index8, 0, ok) == maxVal);
  REQUIRE(ok);
  REQUIRE(neighborListFloat32->getValue(index9, 0, ok) == infVal);
  REQUIRE(ok);
  REQUIRE(std::isnan(neighborListFloat32->getValue(index10, 0, ok)));
  REQUIRE(ok);
}
template <>
void TestNeighborListArrayToFromString<float64>(DataStructure& datastructure, const std::string& arrayName, const std::string& minCompareStr, const std::string& maxCompareStr)
{
  constexpr usize index1 = 5;
  constexpr usize index2 = 6;
  constexpr usize index3 = 7;
  constexpr usize index4 = 8;
  constexpr usize index5 = 9;
  constexpr usize index6 = 10;
  constexpr usize index7 = 11;
  constexpr usize index8 = 12;
  constexpr usize index9 = 13;
  constexpr usize index10 = 14;
  const ShapeType tupleShape = {5, 4, 3};
  const ShapeType compShape = {1};
  auto* neighborListFloat64 = UnitTest::CreateTestNeighborList<float64>(datastructure, arrayName, 10, {});
  constexpr float64 lowestVal = std::numeric_limits<float64>::lowest();
  constexpr float64 minVal = std::numeric_limits<float64>::min();
  constexpr float64 maxVal = std::numeric_limits<float64>::max();
  constexpr float64 infVal = std::numeric_limits<float64>::infinity();
  const float64 nanVal = std::nan("1");
  neighborListFloat64->addEntry(index1, lowestVal);
  neighborListFloat64->addEntry(index2, minVal);
  neighborListFloat64->addEntry(index3, maxVal);
  neighborListFloat64->addEntry(index4, infVal);
  neighborListFloat64->addEntry(index5, nanVal);
  neighborListFloat64->addEntry(index6, 0);
  neighborListFloat64->addEntry(index7, 0);
  neighborListFloat64->addEntry(index8, 0);
  neighborListFloat64->addEntry(index9, 0);
  neighborListFloat64->addEntry(index10, 0);
  const std::string lowestStr = neighborListFloat64->toString(index1, 0);
  const std::string minStr = neighborListFloat64->toString(index2, 0);
  const std::string maxStr = neighborListFloat64->toString(index3, 0);
  const std::string infStr = neighborListFloat64->toString(index4, 0);
  const std::string nanStr = neighborListFloat64->toString(index5, 0);
  // std::cout << "NeighborListArray<float64>\n\tlowest = " << lowestStr << "\n\tmin = " << minStr << "\n\tmax = " << maxStr << "\n\tinf = " << infStr << "\n\tnan = " << nanStr << "\n";
  REQUIRE(lowestStr == "-1.7976931348623157e+308");
  REQUIRE(minStr == minCompareStr);
  REQUIRE(maxStr == maxCompareStr);
  REQUIRE(infStr == "inf");
  REQUIRE(nanStr == "nan");
  REQUIRE(neighborListFloat64->toString(index2, 0, "{:.8g}") == "2.2250739e-308");
  REQUIRE(neighborListFloat64->toString(index2, 0, "{:.6f}") == "0.000000");
  REQUIRE(neighborListFloat64->toString(index2, 0, "{:.4e}") == "2.2251e-308");
  REQUIRE(neighborListFloat64->setValueFromString(index6, 0, lowestStr));
  REQUIRE(neighborListFloat64->setValueFromString(index7, 0, minStr));
  REQUIRE(neighborListFloat64->setValueFromString(index8, 0, maxStr));
  REQUIRE(neighborListFloat64->setValueFromString(index9, 0, infStr));
  REQUIRE(neighborListFloat64->setValueFromString(index10, 0, nanStr));
  // std::cout << "NeighborListArray<float64>\n\tlowest : " << neighborListFloat64->getValue(index6) << " == " << lowestVal << "\n\tmin : " << neighborListFloat64->getValue(index7) << " == " << minVal
  //<< "\n\tmax : " << neighborListFloat64->getValue(index8) << " == " << maxVal << "\n\tinf : " << neighborListFloat64->getValue(index9) << " == " << infVal << "\n\tnan : " <<
  // neighborListFloat64->getValue(index10) << " == " << nanVal << "\n";
  bool ok;
  REQUIRE(neighborListFloat64->getValue(index6, 0, ok) == lowestVal);
  REQUIRE(ok);
  REQUIRE(neighborListFloat64->getValue(index7, 0, ok) == minVal);
  REQUIRE(ok);
  REQUIRE(neighborListFloat64->getValue(index8, 0, ok) == maxVal);
  REQUIRE(ok);
  REQUIRE(neighborListFloat64->getValue(index9, 0, ok) == infVal);
  REQUIRE(ok);
  REQUIRE(std::isnan(neighborListFloat64->getValue(index10, 0, ok)));
  REQUIRE(ok);
}

TEST_CASE("IArray ToFromString")
{
  DataStructure datastructure;

  SECTION("DataArray<int8>")
  {
    TestDataArrayToFromString<int8>(datastructure, "Int8Array", "-128", "127");
  }
  SECTION("DataArray<uint8>")
  {
    TestDataArrayToFromString<uint8>(datastructure, "UInt8Array", "0", "255");
  }
  SECTION("DataArray<int16>")
  {
    TestDataArrayToFromString<int16>(datastructure, "Int16Array", "-32768", "32767");
  }
  SECTION("DataArray<uint16>")
  {
    TestDataArrayToFromString<uint16>(datastructure, "UInt16Array", "0", "65535");
  }
  SECTION("DataArray<int32>")
  {
    TestDataArrayToFromString<int32>(datastructure, "Int32Array", "-2147483648", "2147483647");
  }
  SECTION("DataArray<uint32>")
  {
    TestDataArrayToFromString<uint32>(datastructure, "UInt32Array", "0", "4294967295");
  }
  SECTION("DataArray<int64>")
  {
    TestDataArrayToFromString<int64>(datastructure, "Int64Array", "-9223372036854775808", "9223372036854775807");
  }
  SECTION("DataArray<uint64>")
  {
    TestDataArrayToFromString<uint64>(datastructure, "UInt64Array", "0", "18446744073709551615");
  }
  SECTION("DataArray<float32>")
  {
    TestDataArrayToFromString<float32>(datastructure, "Float32Array", "1.1754944e-38", "3.4028235e+38");
  }
  SECTION("DataArray<float64>")
  {
    TestDataArrayToFromString<float64>(datastructure, "Float64Array", "2.2250738585072014e-308", "1.7976931348623157e+308");
  }
  SECTION("DataArray<bool>")
  {
    TestDataArrayToFromString<bool>(datastructure, "BoolArray", "false", "true");
  }

  SECTION("NeighborList<int8>")
  {
    TestNeighborListArrayToFromString<int8>(datastructure, "Int8NeighborListArray", "-128", "127");
  }
  SECTION("NeighborList<uint8>")
  {
    TestNeighborListArrayToFromString<uint8>(datastructure, "UInt8NeighborListArray", "0", "255");
  }
  SECTION("NeighborList<int16>")
  {
    TestNeighborListArrayToFromString<int16>(datastructure, "Int16NeighborListArray", "-32768", "32767");
  }
  SECTION("NeighborList<uint16>")
  {
    TestNeighborListArrayToFromString<uint16>(datastructure, "UInt16NeighborListArray", "0", "65535");
  }
  SECTION("NeighborList<int32>")
  {
    TestNeighborListArrayToFromString<int32>(datastructure, "Int32NeighborListArray", "-2147483648", "2147483647");
  }
  SECTION("NeighborList<uint32>")
  {
    TestNeighborListArrayToFromString<uint32>(datastructure, "UInt32NeighborListArray", "0", "4294967295");
  }
  SECTION("NeighborList<int64>")
  {
    TestNeighborListArrayToFromString<int64>(datastructure, "Int64NeighborListArray", "-9223372036854775808", "9223372036854775807");
  }
  SECTION("NeighborList<uint64>")
  {
    TestNeighborListArrayToFromString<uint64>(datastructure, "UInt64NeighborListArray", "0", "18446744073709551615");
  }
  SECTION("NeighborList<float32>")
  {
    TestNeighborListArrayToFromString<float32>(datastructure, "Float32NeighborListArray", "1.1754944e-38", "3.4028235e+38");
  }
  SECTION("NeighborList<float64>")
  {
    TestNeighborListArrayToFromString<float64>(datastructure, "Float64NeighborListArray", "2.2250738585072014e-308", "1.7976931348623157e+308");
  }

  SECTION("StringArray")
  {
    constexpr usize index1 = 5;
    constexpr usize index2 = 6;
    constexpr usize index3 = 7;
    constexpr usize index4 = 8;
    constexpr usize index5 = 9;
    constexpr usize index6 = 10;
    const ShapeType tupleShape = {5, 4, 3};
    std::vector<std::string> values(60, "This is a string");
    StringArray* stringArray = StringArray::CreateWithValues(datastructure, "StringArray", tupleShape, values, {});
    REQUIRE(stringArray->setValueFromString(index1, 0, "AllLetters"));
    REQUIRE(stringArray->setValueFromString(index2, 0, "MixedLetters&Numbers"));
    REQUIRE(stringArray->setValueFromString(index3, 0, "234523"));
    REQUIRE(stringArray->setValueFromString(index4, 0, "45.78"));
    REQUIRE(stringArray->setValueFromString(index5, 0, "inf"));
    REQUIRE(stringArray->setValueFromString(index6, 0, "nan"));
    REQUIRE(stringArray->toString(index1, 0) == "AllLetters");
    REQUIRE(stringArray->toString(index2, 0) == "MixedLetters&Numbers");
    REQUIRE(stringArray->toString(index3, 0) == "234523");
    REQUIRE(stringArray->toString(index4, 0) == "45.78");
    REQUIRE(stringArray->toString(index5, 0) == "inf");
    REQUIRE(stringArray->toString(index6, 0) == "nan");
  }
}
