#include <catch2/catch.hpp>

#include "simplnx/UnitTest/UnitTestCommon.hpp"

#include "OrientationAnalysis/Filters/ComputeFeatureNeighborCAxisMisalignmentsFilter.hpp"
#include "OrientationAnalysis/OrientationAnalysis_test_dirs.hpp"

using namespace nx::core;
using namespace nx::core::Constants;

namespace
{
const DataPath k_NeighborListPath = k_CellFeatureDataPath.createChildPath("NeighborList");
const std::string k_CAxisMisalignmentListNameExemplar = "CAxisMisalignmentList";
const std::string k_AvgCAxisMisalignmentsNameExemplar = "AvgCAxisMisalignments";
const std::string k_CAxisMisalignmentListNameComputed = "NX_CAxisMisalignmentList";
const std::string k_AvgCAxisMisalignmentsNameComputed = "NX_AvgCAxisMisalignments";
} // namespace

TEST_CASE("OrientationAnalysis::ComputeFeatureNeighborCAxisMisalignmentsFilter: Valid Filter Execution", "[OrientationAnalysis][ComputeFeatureNeighborCAxisMisalignmentsFilter]")
{
  Application::GetOrCreateInstance()->loadPlugins(unit_test::k_BuildDir.view(), true);

  const nx::core::UnitTest::TestFileSentinel testDataSentinel(nx::core::unit_test::k_CMakeExecutable, nx::core::unit_test::k_TestFilesDir, "caxis_data.tar.gz", "caxis_data");

  // Read Exemplar DREAM3D File Filter
  auto exemplarFilePath = fs::path(fmt::format("{}/caxis_data/7_0_find_caxis_data.dream3d", unit_test::k_TestFilesDir));
  DataStructure dataStructure = UnitTest::LoadDataStructure(exemplarFilePath);

  // Instantiate the filter, a DataStructure object and an Arguments Object
  ComputeFeatureNeighborCAxisMisalignmentsFilter filter;
  Arguments args;

  // Create default Parameters for the filter.
  args.insertOrAssign(ComputeFeatureNeighborCAxisMisalignmentsFilter::k_FindAvgMisals_Key, std::make_any<bool>(true));
  args.insertOrAssign(ComputeFeatureNeighborCAxisMisalignmentsFilter::k_NeighborListArrayPath_Key, std::make_any<DataPath>(k_NeighborListPath));
  args.insertOrAssign(ComputeFeatureNeighborCAxisMisalignmentsFilter::k_AvgQuatsArrayPath_Key, std::make_any<DataPath>(k_CellFeatureDataPath.createChildPath(k_AvgQuats)));
  args.insertOrAssign(ComputeFeatureNeighborCAxisMisalignmentsFilter::k_FeaturePhasesArrayPath_Key, std::make_any<DataPath>(k_CellFeatureDataPath.createChildPath(k_Phases)));
  args.insertOrAssign(ComputeFeatureNeighborCAxisMisalignmentsFilter::k_CrystalStructuresArrayPath_Key, std::make_any<DataPath>(k_CrystalStructuresArrayPath));
  args.insertOrAssign(ComputeFeatureNeighborCAxisMisalignmentsFilter::k_CAxisMisalignmentListArrayName_Key, std::make_any<std::string>(k_CAxisMisalignmentListNameComputed));
  args.insertOrAssign(ComputeFeatureNeighborCAxisMisalignmentsFilter::k_AvgCAxisMisalignmentsArrayName_Key, std::make_any<std::string>(k_AvgCAxisMisalignmentsNameComputed));

  // Preflight the filter and check result
  auto preflightResult = filter.preflight(dataStructure, args);
  SIMPLNX_RESULT_REQUIRE_VALID(preflightResult.outputActions)

  // Execute the filter and check the result
  auto executeResult = filter.execute(dataStructure, args);
  SIMPLNX_RESULT_REQUIRE_VALID(executeResult.result)

  UnitTest::CompareNeighborListFloatArraysWithNans<float32>(dataStructure, k_CellFeatureDataPath.createChildPath(k_CAxisMisalignmentListNameExemplar),
                                                            k_CellFeatureDataPath.createChildPath(k_CAxisMisalignmentListNameComputed), UnitTest::EPSILON, true);
  UnitTest::CompareFloatArraysWithNans<float32>(dataStructure, k_CellFeatureDataPath.createChildPath(k_AvgCAxisMisalignmentsNameExemplar),
                                                k_CellFeatureDataPath.createChildPath(k_AvgCAxisMisalignmentsNameComputed), UnitTest::EPSILON, true);
}

TEST_CASE("OrientationAnalysis::ComputeFeatureNeighborCAxisMisalignmentsFilter: InValid Filter Execution")
{
  Application::GetOrCreateInstance()->loadPlugins(unit_test::k_BuildDir.view(), true);

  const nx::core::UnitTest::TestFileSentinel testDataSentinel(nx::core::unit_test::k_CMakeExecutable, nx::core::unit_test::k_TestFilesDir, "caxis_data.tar.gz", "caxis_data");

  // Read Exemplar DREAM3D File Filter
  auto exemplarFilePath = fs::path(fmt::format("{}/caxis_data/7_0_find_caxis_data.dream3d", unit_test::k_TestFilesDir));
  DataStructure dataStructure = UnitTest::LoadDataStructure(exemplarFilePath);

  // Instantiate the filter, a DataStructure object and an Arguments Object
  ComputeFeatureNeighborCAxisMisalignmentsFilter filter;
  Arguments args;

  args.insertOrAssign(ComputeFeatureNeighborCAxisMisalignmentsFilter::k_FindAvgMisals_Key, std::make_any<bool>(true));
  args.insertOrAssign(ComputeFeatureNeighborCAxisMisalignmentsFilter::k_NeighborListArrayPath_Key, std::make_any<DataPath>(k_NeighborListPath));
  args.insertOrAssign(ComputeFeatureNeighborCAxisMisalignmentsFilter::k_AvgQuatsArrayPath_Key, std::make_any<DataPath>(k_CellFeatureDataPath.createChildPath(k_AvgQuats)));
  args.insertOrAssign(ComputeFeatureNeighborCAxisMisalignmentsFilter::k_CrystalStructuresArrayPath_Key, std::make_any<DataPath>(k_CrystalStructuresArrayPath));
  args.insertOrAssign(ComputeFeatureNeighborCAxisMisalignmentsFilter::k_CAxisMisalignmentListArrayName_Key, std::make_any<std::string>(k_CAxisMisalignmentListNameComputed));
  args.insertOrAssign(ComputeFeatureNeighborCAxisMisalignmentsFilter::k_AvgCAxisMisalignmentsArrayName_Key, std::make_any<std::string>(k_AvgCAxisMisalignmentsNameComputed));

  SECTION("Invalid Crystal Structure Type")
  {
    // Invalid crystal structure type : should fail in execute
    args.insertOrAssign(ComputeFeatureNeighborCAxisMisalignmentsFilter::k_FeaturePhasesArrayPath_Key, std::make_any<DataPath>(k_CellFeatureDataPath.createChildPath(k_Phases)));

    auto& crystalStructs = dataStructure.getDataRefAs<UInt32Array>(k_CrystalStructuresArrayPath);
    crystalStructs[1] = 1;

    // Preflight the filter and check result
    auto preflightResult = filter.preflight(dataStructure, args);
    SIMPLNX_RESULT_REQUIRE_VALID(preflightResult.outputActions)
  }
  SECTION("Mismatching Input Array Tuples")
  {
    args.insertOrAssign(ComputeFeatureNeighborCAxisMisalignmentsFilter::k_FeaturePhasesArrayPath_Key, std::make_any<DataPath>(k_CellAttributeMatrix.createChildPath(k_Phases)));

    // Preflight the filter and check result
    auto preflightResult = filter.preflight(dataStructure, args);
    SIMPLNX_RESULT_REQUIRE_INVALID(preflightResult.outputActions)
  }

  // Execute the filter and check the result
  auto executeResult = filter.execute(dataStructure, args);
  SIMPLNX_RESULT_REQUIRE_INVALID(executeResult.result)
}
