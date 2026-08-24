#pragma once

#include "simplnx/simplnx_export.hpp"

#include "simplnx/Common/Result.hpp"
#include "simplnx/DataStructure/IArray.hpp"
#include "simplnx/DataStructure/IDataArray.hpp"
#include "simplnx/DataStructure/IDataStore.hpp"
#include "simplnx/DataStructure/INeighborList.hpp"

#include <initializer_list>
#include <utility>
#include <vector>

namespace nx::core
{

/**
 * @brief Checks whether an IDataArray is backed by out-of-core (chunked) storage.
 *
 * Returns true when the array's data store reports StoreType::OutOfCore,
 * indicating that data lives on disk in compressed chunks rather than in a
 * contiguous in-memory buffer.
 *
 * @param array The data array to check
 * @return true if the array uses chunked/OOC storage
 */
inline bool IsOutOfCore(const IDataArray& array)
{
  return array.getIDataStoreRef().getStoreType() == IDataStore::StoreType::OutOfCore;
}

/**
 * @brief Checks whether an array is backed by out-of-core storage.
 *
 * IDataArray instances use their IDataStore residency. NeighborLists use their
 * IListStore residency. Other IArray implementations do not currently expose
 * a storage-residency contract and are conservatively treated as in-memory.
 *
 * @param array The array to check.
 * @return true if the array uses out-of-core storage.
 */
inline bool IsOutOfCore(const IArray& array)
{
  if(const auto* dataArray = dynamic_cast<const IDataArray*>(&array); dataArray != nullptr)
  {
    return IsOutOfCore(*dataArray);
  }
  if(const auto* neighborList = dynamic_cast<const INeighborList*>(&array); neighborList != nullptr)
  {
    const auto* listStore = neighborList->getIListStore();
    return listStore != nullptr && listStore->isOutOfCore();
  }
  return false;
}

/**
 * @brief Owning wrapper for mixed IArray dispatch targets.
 *
 * This wrapper preserves the original IDataArray initializer-list overloads
 * while enabling braced lists that also contain NeighborLists. It owns its
 * pointer list, so a named wrapper can safely outlive its construction
 * expression. The pointed-to arrays remain non-owning targets.
 */
class AlgorithmArrayTargets
{
public:
  /** @brief Copies a braced list of non-owning targets into stable owned pointer storage. */
  AlgorithmArrayTargets(std::initializer_list<const IArray*> arrays)
  : m_Arrays(arrays)
  {
  }

  /** @brief Takes ownership of the pointer vector, not of the pointed-to arrays. */
  explicit AlgorithmArrayTargets(std::vector<const IArray*> arrays)
  : m_Arrays(std::move(arrays))
  {
  }

  /** @brief Returns the stable non-owning targets used by residency inspection. */
  const std::vector<const IArray*>& arrays() const noexcept
  {
    return m_Arrays;
  }

private:
  std::vector<const IArray*> m_Arrays;
};

/**
 * @brief Checks whether any of the given IDataArrays are backed by out-of-core storage.
 *
 * Filters often operate on multiple input and output arrays. If any of them use
 * chunked storage, the OOC algorithm path should be used to avoid chunk thrashing.
 *
 * @param arrays List of pointers to data arrays to check (nullptrs are skipped)
 * @return true if any non-null array uses chunked/OOC storage
 */
inline bool AnyOutOfCore(std::initializer_list<const IDataArray*> arrays)
{
  for(const auto* array : arrays)
  {
    if(array != nullptr && IsOutOfCore(*array))
    {
      return true;
    }
  }
  return false;
}

/**
 * @brief Checks whether any mixed IArray target is backed by out-of-core storage.
 * @param targets Owning wrapper around non-owning array pointers; nullptrs are skipped.
 * @return true if any target uses out-of-core storage.
 */
inline bool AnyOutOfCore(const AlgorithmArrayTargets& targets)
{
  for(const auto* array : targets.arrays())
  {
    if(array != nullptr && IsOutOfCore(*array))
    {
      return true;
    }
  }
  return false;
}

/**
 * @brief Returns a reference to the global flag that forces DispatchAlgorithm
 *        to always select the out-of-core algorithm, regardless of storage type.
 *
 * This is primarily used in unit tests to exercise the OOC algorithm path
 * even when data is stored in-core. The flag is backed by a function-local
 * static, so it persists for the lifetime of the process.
 *
 * @warning This flag is NOT thread-safe. It should only be set from the main
 *          test thread before any parallel work begins. Use ForceOocAlgorithmGuard
 *          for RAII-safe toggling in tests.
 *
 * @return Reference to the static force flag
 */
SIMPLNX_EXPORT bool& ForceOocAlgorithm();

/**
 * @brief Selects algorithm scenarios compiled into filter unit tests.
 *
 * The CMake SIMPLNX_TEST_ALGORITHM_PATH cache variable supplies this definition:
 *   0 (Both)       - run every explicitly requested in-core and OOC scenario
 *   1 (OocOnly)    - run explicitly requested OOC scenarios
 *   2 (InCoreOnly) - run explicitly requested in-core scenarios
 *
 * Filter tests should use UnitTest::SelectAlgorithmTestScenarios and
 * UnitTest::AlgorithmTestScope. The scope configures algorithm and storage state
 * and verifies which dispatched implementation actually ran.
 */
#ifndef SIMPLNX_TEST_ALGORITHM_PATH
#define SIMPLNX_TEST_ALGORITHM_PATH 0
#endif

/**
 * @brief RAII guard that sets ForceOocAlgorithm() on construction and
 *        restores the previous value on destruction.
 *
 * The guard captures the current value of ForceOocAlgorithm() when constructed,
 * overrides it with the requested value, and restores the original value when
 * the guard goes out of scope. This ensures the global flag is always cleaned
 * up, even if the test throws an exception or fails early.
 *
 * Copy and move operations are deleted to prevent accidental double-restore
 * of the original value, which would corrupt the global flag state.
 *
 * @warning Not thread-safe. The underlying flag is a bare static bool with
 *          no synchronization. In Catch2 tests this is safe because each
 *          TEST_CASE runs on the main thread, but do not use this guard
 *          from worker threads.
 *
 * This is a low-level state-control utility. Filter unit tests should use
 * UnitTest::AlgorithmTestScope so they also control backing storage and prove
 * that the requested implementation executed.
 */
class ForceOocAlgorithmGuard
{
public:
  ForceOocAlgorithmGuard(bool force)
  : m_Original(ForceOocAlgorithm())
  {
    // Override the global flag for the duration of this guard's lifetime
    ForceOocAlgorithm() = force;
  }

  ~ForceOocAlgorithmGuard()
  {
    // Restore the original value so subsequent tests start with a clean state
    ForceOocAlgorithm() = m_Original;
  }

  ForceOocAlgorithmGuard(const ForceOocAlgorithmGuard&) = delete;
  ForceOocAlgorithmGuard(ForceOocAlgorithmGuard&&) = delete;
  ForceOocAlgorithmGuard& operator=(const ForceOocAlgorithmGuard&) = delete;
  ForceOocAlgorithmGuard& operator=(ForceOocAlgorithmGuard&&) = delete;

private:
  bool m_Original = false;
};

/**
 * @brief Returns a reference to the global flag that forces DispatchAlgorithm
 *        to always select the in-core algorithm, overriding storage-type detection.
 *
 * This is primarily used by low-level dispatch-state tests. Ordinary filter
 * tests should use UnitTest::AlgorithmTestScope.
 * The flag is backed by a function-local static, so it persists for the lifetime
 * of the process.
 *
 * ForceInCoreAlgorithm() takes the highest precedence in DispatchAlgorithm:
 * when set to true, neither AnyOutOfCore() nor ForceOocAlgorithm() can
 * override it. The supported filter-test scenarios intentionally do not pair
 * the in-core algorithm with out-of-core stores.
 *
 * @warning Not thread-safe. See ForceOocAlgorithm() for details.
 *
 * @return Reference to the static force flag
 */
SIMPLNX_EXPORT bool& ForceInCoreAlgorithm();

/**
 * @brief RAII guard that unconditionally sets ForceInCoreAlgorithm() to true
 *        on construction and restores the previous value on destruction.
 *
 * Unlike ForceOocAlgorithmGuard, this guard always forces in-core mode and
 * does not accept a boolean parameter. This is intentional: forcing in-core
 * is an override that should only be applied deliberately in tests that need
 * to verify in-core behavior in an OOC-enabled build.
 *
 * Copy and move operations are deleted to prevent accidental double-restore.
 *
 * @warning Not thread-safe. See ForceOocAlgorithmGuard for details.
 *
 * Usage in tests:
 * @code
 *   const nx::core::ForceInCoreAlgorithmGuard guard;
 * @endcode
 */
class ForceInCoreAlgorithmGuard
{
public:
  ForceInCoreAlgorithmGuard()
  : m_Original(ForceInCoreAlgorithm())
  {
    // Unconditionally force in-core dispatch for the guard's lifetime
    ForceInCoreAlgorithm() = true;
  }

  ~ForceInCoreAlgorithmGuard()
  {
    // Restore the original value so subsequent tests start with a clean state
    ForceInCoreAlgorithm() = m_Original;
  }

  ForceInCoreAlgorithmGuard(const ForceInCoreAlgorithmGuard&) = delete;
  ForceInCoreAlgorithmGuard(ForceInCoreAlgorithmGuard&&) = delete;
  ForceInCoreAlgorithmGuard& operator=(const ForceInCoreAlgorithmGuard&) = delete;
  ForceInCoreAlgorithmGuard& operator=(ForceInCoreAlgorithmGuard&&) = delete;

private:
  bool m_Original = false;
};

/**
 * @enum AlgorithmPath
 * @brief Identifies an implementation selected by a dispatched algorithm.
 */
enum class AlgorithmPath : uint8
{
  InCore,
  OutOfCore
};

/**
 * @struct AlgorithmPathExecutionCounts
 * @brief Stores the number of times each dispatched implementation was entered.
 */
struct AlgorithmPathExecutionCounts
{
  uint64 InCore = 0;
  uint64 OutOfCore = 0;
  uint64 InCoreOnInMemoryStore = 0;
  uint64 InCoreOnOutOfCoreStore = 0;
  uint64 OutOfCoreOnInMemoryStore = 0;
  uint64 OutOfCoreOnOutOfCoreStore = 0;
};

/**
 * @brief Records the algorithm and backing-store combination that is about to execute.
 * @param path The selected implementation path.
 * @param usesOutOfCoreStore Whether the selector observed an out-of-core backing store.
 */
SIMPLNX_EXPORT void RecordAlgorithmPathExecution(AlgorithmPath path, bool usesOutOfCoreStore);

/**
 * @brief Resets all dispatched algorithm and backing-store execution counters to zero.
 */
SIMPLNX_EXPORT void ResetAlgorithmPathExecutionCounts();

/**
 * @brief Replaces all dispatched algorithm and backing-store execution counters.
 * @param counts The counter values to restore.
 *
 * This is intended for unit-test state restoration after a target-only
 * execution witness. Production algorithms should call
 * RecordAlgorithmPathExecution() instead.
 */
SIMPLNX_EXPORT void SetAlgorithmPathExecutionCounts(const AlgorithmPathExecutionCounts& counts);

/**
 * @brief Returns a snapshot of the dispatched-implementation execution counters.
 * @return The current aggregate algorithm counts and exact algorithm/store combination counts.
 */
SIMPLNX_EXPORT AlgorithmPathExecutionCounts GetAlgorithmPathExecutionCounts();

/**
 * @brief Dispatches between two algorithm classes based on whether any of the
 *        given arrays use out-of-core (chunked) storage, or if the global
 *        ForceOocAlgorithm() flag is set.
 *
 * Some algorithms that perform well on in-memory data (e.g. BFS flood fill with
 * random access) become extremely slow when data is stored in disk-backed chunks,
 * because each random access may trigger a chunk load/evict cycle. In these cases,
 * a different algorithm (e.g. scanline CCL with sequential chunk access) can be
 * orders of magnitude faster for OOC data.
 *
 * Selection logic (evaluated in order):
 *   1. ForceInCoreAlgorithm() == true  -> always use InCoreAlgo
 *   2. AnyOutOfCore(arrays) == true    -> use OocAlgo
 *   3. ForceOocAlgorithm() == true     -> use OocAlgo
 *   4. Otherwise                       -> use InCoreAlgo
 *
 * @tparam InCoreAlgo Algorithm class optimized for in-memory data
 * @tparam OocAlgo Algorithm class optimized for out-of-core (chunked) data
 * @tparam ArgsT Constructor argument types (must be identical for both algorithms)
 * @param arrays The arrays used to detect storage type (OOC if any is OOC)
 * @param args Constructor arguments forwarded to the selected algorithm
 * @return Result<> from the selected algorithm's operator()()
 */
template <typename InCoreAlgo, typename OocAlgo, typename... ArgsT>
Result<> DispatchAlgorithm(std::initializer_list<const IDataArray*> arrays, ArgsT&&... args)
{
  const bool usesOutOfCoreStore = AnyOutOfCore(arrays);
  const bool useOutOfCoreAlgorithm = !ForceInCoreAlgorithm() && (usesOutOfCoreStore || ForceOocAlgorithm());
  RecordAlgorithmPathExecution(useOutOfCoreAlgorithm ? AlgorithmPath::OutOfCore : AlgorithmPath::InCore, usesOutOfCoreStore);

  if(useOutOfCoreAlgorithm)
  {
    return OocAlgo(std::forward<ArgsT>(args)...)();
  }
  return InCoreAlgo(std::forward<ArgsT>(args)...)();
}

/**
 * @brief Dispatches between algorithms for mixed IArray targets, including NeighborLists.
 *
 * Braced mixed targets select this overload through AlgorithmArrayTargets;
 * existing IDataArray-only and empty braced calls retain the legacy overload.
 */
template <typename InCoreAlgo, typename OocAlgo, typename... ArgsT>
Result<> DispatchAlgorithm(const AlgorithmArrayTargets& targets, ArgsT&&... args)
{
  // Selection priority (highest to lowest):
  //   1. ForceInCoreAlgorithm == true  -> InCoreAlgo  (test override, wins over everything)
  //   2. AnyOutOfCore(arrays) == true  -> OocAlgo     (real OOC data detected at runtime)
  //   3. ForceOocAlgorithm == true     -> OocAlgo     (test override for exercising OOC path)
  //   4. Default                       -> InCoreAlgo  (all data is in-memory)
  const bool usesOutOfCoreStore = AnyOutOfCore(targets);
  const bool useOutOfCoreAlgorithm = !ForceInCoreAlgorithm() && (usesOutOfCoreStore || ForceOocAlgorithm());
  RecordAlgorithmPathExecution(useOutOfCoreAlgorithm ? AlgorithmPath::OutOfCore : AlgorithmPath::InCore, usesOutOfCoreStore);

  if(useOutOfCoreAlgorithm)
  {
    // Construct the OOC algorithm with the forwarded args and invoke operator()()
    return OocAlgo(std::forward<ArgsT>(args)...)();
  }
  else
  {
    // Construct the in-core algorithm with the forwarded args and invoke operator()()
    return InCoreAlgo(std::forward<ArgsT>(args)...)();
  }
}

} // namespace nx::core
