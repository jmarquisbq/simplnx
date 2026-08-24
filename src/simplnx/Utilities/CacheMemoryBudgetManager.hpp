#pragma once

#include "simplnx/simplnx_export.hpp"

#include "simplnx/Common/Types.hpp"

#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nx::core
{

/**
 * @brief Unified cache memory budget manager for cache subsystems.
 *
 * Cache subsystems register their allocations with this singleton. When memory
 * pressure exceeds the budget, the manager evicts the globally-oldest entry
 * regardless of which subsystem owns it.
 *
 * @note This budget covers only registered cache entries. It does not cap total
 * application memory, loaded in-memory arrays, temporary working buffers,
 * rendering allocations, or allocator-retained pages.
 *
 * Thread-safe: allocate/touch/release can be called from any thread.
 *
 * Eviction callbacks are invoked under the manager's mutex and MUST be non-blocking
 * (mark data for removal, don't do I/O or VTK operations). Callers receive the list
 * of evicted handles after the mutex is released for post-eviction cleanup.
 */
class SIMPLNX_EXPORT CacheMemoryBudgetManager
{
public:
  using AllocationHandle = uint64;

  /**
   * @brief Callback invoked when an entry is evicted.
   *
   * The callback is invoked under the manager's internal mutex.
   * Callbacks MUST NOT call allocate/touch/release on this manager -- that
   * would deadlock (the mutex is not recursive). Only mark state for removal;
   * do no I/O.
   */
  using EvictionCallback = std::function<void()>;

  /**
   * @brief Non-blocking request for a cache subsystem to free at least the
   * specified number of retained bytes using its own eviction policy.
   *
   * The manager invokes this handler while holding its mutex. The handler must
   * only record the request; it must not take subsystem locks, perform I/O, or
   * call this manager. The subsystem drains later and reconciles accounting by
   * calling release().
   */
  using SubsystemEvictionHandler = std::function<void(uint64 bytesToFree)>;

  /**
   * @brief Registers or replaces a delegated eviction handler for a cache subsystem.
   *
   * Subsystems without a handler retain the per-entry eviction callback path.
   */
  void registerSubsystem(const std::string& subsystem, SubsystemEvictionHandler handler);

  /**
   * @brief Returns the singleton instance.
   */
  static CacheMemoryBudgetManager& instance();

  /**
   * @brief Returns a default budget of 50% of system RAM, raised to a minimum of
   * 1 GiB and clamped to maxBudgetBytes() so the default never exceeds the cap.
   */
  static uint64 defaultBudgetBytes();

  /**
   * @brief Returns the maximum budget this machine should allow, reserving
   * headroom for the OS and the application itself.
   *
   * cap = max( min(totalRAM - 6 GiB, 0.95 * totalRAM), 1 GiB )
   *
   * The 6 GiB reserve binds on machines below ~120 GiB; the 95% fraction binds
   * above that; the 1 GiB floor protects very small machines. Returns the 1 GiB
   * floor if total RAM cannot be determined.
   */
  static uint64 maxBudgetBytes();

  /**
   * @brief Allocates a tracked entry. Evicts oldest entries if needed to stay within budget.
   * @param subsystem Name of the owning subsystem (e.g. "chunk", "stride", "partition")
   * @param key Subsystem-specific key for identification
   * @param sizeBytes Size of the allocation in bytes
   * @param onEvict Callback invoked when this entry is evicted (must be non-blocking)
   * @return Pair of (new handle, list of evicted handles for post-eviction cleanup)
   */
  std::pair<AllocationHandle, std::vector<AllocationHandle>> allocate(const std::string& subsystem, const std::string& key, uint64 sizeBytes, EvictionCallback onEvict);

  /**
   * @brief Updates the last-accessed timestamp of an allocation.
   * @param handle The allocation handle to touch
   */
  void touch(AllocationHandle handle);

  /**
   * @brief Voluntarily releases an allocation.
   * @param handle The allocation handle to release
   */
  void release(AllocationHandle handle);

  /**
   * @brief Sets the cache memory budget in bytes, clamped to maxBudgetBytes().
   *
   * Only the UPPER bound is clamped — a caller may still set a budget smaller
   * than the 1 GiB floor (tests rely on this to exercise eviction).
   * @return true if the requested value exceeded the cap and was reduced.
   */
  bool setBudgetBytes(uint64 bytes);

  /**
   * @brief Returns the current cache memory budget in bytes.
   */
  uint64 budgetBytes() const;

  /**
   * @brief Returns the total bytes currently registered by cache subsystems.
   */
  uint64 usedBytes() const;

  /**
   * @brief Clears all tracked entries and resets used bytes to zero.
   *
   * Intended for tests that share the mutable singleton -- call at the start
   * of each test case so leaked entries from a prior failure do not pollute
   * budget accounting.
   */
  void clear();

private:
  CacheMemoryBudgetManager();
  ~CacheMemoryBudgetManager() = default;

  CacheMemoryBudgetManager(const CacheMemoryBudgetManager&) = delete;
  CacheMemoryBudgetManager& operator=(const CacheMemoryBudgetManager&) = delete;

  struct Entry
  {
    std::string subsystem;
    std::string key;
    uint64 sizeBytes = 0;
    std::chrono::steady_clock::time_point lastAccessed;
    /// Eviction callback. MUST NOT call allocate/touch/release on this
    /// manager -- that would deadlock. Only mark state for removal; do no I/O.
    EvictionCallback onEvict;
  };

  /**
   * @brief Evicts the oldest entries until m_UsedBytes + needed <= m_BudgetBytes.
   * Must be called with m_Mutex held.
   * @param needed Number of bytes needed for a new allocation
   * @return List of evicted handles
   */
  std::vector<AllocationHandle> makeRoom(uint64 needed);

  /**
   * @brief Returns total physical system RAM in bytes (0 if it cannot be read).
   *
   * Delegates directly to the OS via Memory::GetTotalMemory(); it is not cached,
   * so do not call it on hot paths.
   */
  static uint64 totalSystemRamBytes();

  mutable std::mutex m_Mutex;
  std::unordered_map<AllocationHandle, Entry> m_Entries;
  std::unordered_map<std::string, SubsystemEvictionHandler> m_SubsystemHandlers;
  uint64 m_BudgetBytes = 0;
  uint64 m_UsedBytes = 0;
  AllocationHandle m_NextHandle = 1;
};

} // namespace nx::core
