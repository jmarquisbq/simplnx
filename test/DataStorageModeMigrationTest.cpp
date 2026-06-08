#include "simplnx/Core/Preferences.hpp"

#include <catch2/catch.hpp>

using namespace nx::core;

TEST_CASE("DataStorageMode migrates from legacy keys", "[Core][Preferences]")
{
  Preferences prefs;
  prefs.setValue(Preferences::k_ForceOocData_Key, true);
  REQUIRE(prefs.dataStorageMode() == DataStorageMode::ForceOutOfCore);

  Preferences p2;
  p2.setValue(Preferences::k_ForceOocData_Key, false);
  p2.setValue(Preferences::k_PreferredLargeDataFormat_Key, std::string(Preferences::k_InMemoryFormat));
  REQUIRE(p2.dataStorageMode() == DataStorageMode::ForceInCore);

  Preferences p3;
  p3.setValue(Preferences::k_ForceOocData_Key, false);
  p3.setValue(Preferences::k_PreferredLargeDataFormat_Key, std::string("HDF5-OOC"));
  REQUIRE(p3.dataStorageMode() == DataStorageMode::Adaptive);

  // Fresh prefs with no legacy user values default to Adaptive.
  Preferences p4;
  REQUIRE(p4.dataStorageMode() == DataStorageMode::Adaptive);

  // The canonical key round-trips through set/get.
  Preferences p5;
  p5.setDataStorageMode(DataStorageMode::ForceOutOfCore);
  REQUIRE(p5.dataStorageMode() == DataStorageMode::ForceOutOfCore);

  // An explicit canonical mode takes precedence over any legacy keys present.
  Preferences p6;
  p6.setValue(Preferences::k_ForceOocData_Key, false); // legacy would imply in-core
  p6.setValue(Preferences::k_PreferredLargeDataFormat_Key, std::string(Preferences::k_InMemoryFormat));
  p6.setDataStorageMode(DataStorageMode::ForceOutOfCore);
  REQUIRE(p6.dataStorageMode() == DataStorageMode::ForceOutOfCore);
}
