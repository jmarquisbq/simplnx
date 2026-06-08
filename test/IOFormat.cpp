#include <catch2/catch.hpp>

#include "simplnx/Common/SimplnxConfig.hpp"
#include "simplnx/Core/Application.hpp"
#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/DataStructure/IO/Generic/DataIOCollection.hpp"
#include "simplnx/DataStructure/IO/Generic/IDataIOManager.hpp"
#include "simplnx/UnitTest/UnitTestCommon.hpp"
#include "simplnx/Utilities/DataStoreUtilities.hpp"
#include "simplnx/Utilities/MemoryUtilities.hpp"

using namespace nx::core;

TEST_CASE("Contains HDF5 IO Support", "IOTest")
{
  auto app = Application::GetOrCreateInstance();

  auto& ioCollection = app->getIOCollection();
  auto h5IO = ioCollection.getManager("HDF5");
  REQUIRE(h5IO != nullptr);
}

TEST_CASE("Memory Check", "IOTest")
{
  REQUIRE(Memory::GetTotalMemory() > 0);
  const auto storage = Memory::GetAvailableStorage();
  REQUIRE(storage.total > 0);
  REQUIRE(storage.free > 0);
}

// =============================================================================
// Data Format Preference Tests
//
// These verify the OOC-free build's storage behavior driven by the canonical
// DataStorageMode preference. With no OOC manager registered, the only available
// store is in-memory, so every mode produces an InMemory store; useOocData() still
// reports the user intent (true unless ForceInCore). The OOC-build counterpart
// (where ForceOutOfCore/Adaptive map onto "HDF5-OOC") is covered separately by the
// OOC plugin's own DataFormatPreferenceTest, built only when OOC is compiled in.
// =============================================================================

TEST_CASE("Data Format: ForceInCore keeps useOocData false and stores in memory", "[IOTest][DataFormat]")
{
  auto* prefs = Application::GetOrCreateInstance()->getPreferences();

  const DataStorageMode savedMode = prefs->dataStorageMode();
  prefs->setDataStorageMode(DataStorageMode::ForceInCore);

  // ForceInCore is the only mode for which OOC is "not in use".
  REQUIRE_FALSE(prefs->useOocData());

  // CreateDataStore should produce an InMemory store regardless of size.
  DataStructure ds;
  DataPath dp({"TestArray"});
  auto store = DataStoreUtilities::CreateDataStore<float32>(ds, dp, {100, 100, 100}, {1}, IDataAction::Mode::Execute);
  REQUIRE(store != nullptr);
  REQUIRE(store->getStoreType() == IDataStore::StoreType::InMemory);

  prefs->setDataStorageMode(savedMode);
}

TEST_CASE("Data Format: Adaptive and ForceOutOfCore report useOocData true", "[IOTest][DataFormat]")
{
  auto* prefs = Application::GetOrCreateInstance()->getPreferences();

  const DataStorageMode savedMode = prefs->dataStorageMode();

  // OOC is "in use" for both size-driven and always-out-of-core intents.
  prefs->setDataStorageMode(DataStorageMode::Adaptive);
  REQUIRE(prefs->useOocData());

  prefs->setDataStorageMode(DataStorageMode::ForceOutOfCore);
  REQUIRE(prefs->useOocData());

  // With no OOC manager registered in this build, even ForceOutOfCore can only
  // produce an in-memory store — the resolver has no disk-backed format to return.
  DataStructure ds;
  DataPath dp({"TestArray"});
  auto store = DataStoreUtilities::CreateDataStore<float32>(ds, dp, {100, 100, 100}, {1}, IDataAction::Mode::Execute);
  REQUIRE(store != nullptr);
  REQUIRE(store->getStoreType() == IDataStore::StoreType::InMemory);

  prefs->setDataStorageMode(savedMode);
}

TEST_CASE("Data Format: Cannot register IO manager with reserved InMemory name", "[IOTest][DataFormat]")
{
  // Create a dummy IDataIOManager subclass that returns k_InMemoryFormat
  class ReservedNameManager : public IDataIOManager
  {
  public:
    std::string formatName() const override
    {
      return std::string(Preferences::k_InMemoryFormat);
    }
  };

  auto& ioCollection = Application::GetOrCreateInstance()->getIOCollection();
  auto badManager = std::make_shared<ReservedNameManager>();
  auto addResult = ioCollection.addIOManager(badManager);
  SIMPLNX_RESULT_REQUIRE_INVALID(addResult);
}
