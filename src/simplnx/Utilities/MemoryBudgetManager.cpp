#include "simplnx/Utilities/MemoryBudgetManager.hpp"

#include <algorithm>

#include "simplnx/Utilities/MemoryUtilities.hpp"

namespace nx::core
{

namespace
{
constexpr uint64 k_MinBudget = uint64{1} * 1024 * 1024 * 1024;          // 1 GiB
constexpr uint64 k_BudgetReserveBytes = uint64{6} * 1024 * 1024 * 1024; // 6 GiB OS/app headroom
} // namespace

uint64 MemoryBudgetManager::totalSystemRamBytes()
{
  return nx::core::Memory::GetTotalMemory();
}

MemoryBudgetManager::MemoryBudgetManager()
: m_BudgetBytes(defaultBudgetBytes())
{
}

MemoryBudgetManager& MemoryBudgetManager::instance()
{
  static MemoryBudgetManager s_Instance;
  return s_Instance;
}

uint64 MemoryBudgetManager::defaultBudgetBytes()
{
  const uint64 totalRam = totalSystemRamBytes();
  if(totalRam == 0)
  {
    return k_MinBudget;
  }
  // 50% of RAM raised to the 1 GiB floor, then clamped to maxBudgetBytes() so
  // the default never exceeds the cap on low-RAM machines (below 12 GiB the
  // 6 GiB reserve makes the cap smaller than half of RAM). The cap is never
  // below the floor, so the result stays within [1 GiB, cap].
  return std::min(std::max(totalRam / 2, k_MinBudget), maxBudgetBytes());
}

uint64 MemoryBudgetManager::maxBudgetBytes()
{
  const uint64 totalRam = totalSystemRamBytes();
  if(totalRam == 0)
  {
    return k_MinBudget;
  }
  // Reserve a fixed 6 GiB headroom, but never allow more than 95% of RAM.
  // 0.95 * total computed as (total - total/20) to stay in integer math and
  // avoid overflow.
  const uint64 reserved = (totalRam > k_BudgetReserveBytes) ? (totalRam - k_BudgetReserveBytes) : 0;
  const uint64 fraction95 = totalRam - totalRam / 20;
  const uint64 cap = std::min(reserved, fraction95);
  return std::max(cap, k_MinBudget);
}

std::pair<MemoryBudgetManager::AllocationHandle, std::vector<MemoryBudgetManager::AllocationHandle>> MemoryBudgetManager::allocate(const std::string& subsystem, const std::string& key,
                                                                                                                                   uint64 sizeBytes, EvictionCallback onEvict)
{
  std::lock_guard<std::mutex> lock(m_Mutex);

  std::vector<AllocationHandle> evicted = makeRoom(sizeBytes);

  AllocationHandle handle = m_NextHandle++;
  Entry entry;
  entry.subsystem = subsystem;
  entry.key = key;
  entry.sizeBytes = sizeBytes;
  entry.lastAccessed = std::chrono::steady_clock::now();
  entry.onEvict = std::move(onEvict);

  m_Entries.emplace(handle, std::move(entry));
  m_UsedBytes += sizeBytes;

  return {handle, std::move(evicted)};
}

void MemoryBudgetManager::touch(AllocationHandle handle)
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  auto it = m_Entries.find(handle);
  if(it != m_Entries.end())
  {
    it->second.lastAccessed = std::chrono::steady_clock::now();
  }
}

void MemoryBudgetManager::release(AllocationHandle handle)
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  auto it = m_Entries.find(handle);
  if(it != m_Entries.end())
  {
    m_UsedBytes -= it->second.sizeBytes;
    m_Entries.erase(it);
  }
}

bool MemoryBudgetManager::setBudgetBytes(uint64 bytes)
{
  // Clamp the UPPER bound only. maxBudgetBytes() takes no lock, so compute it
  // before acquiring m_Mutex.
  const uint64 maxAllowed = maxBudgetBytes();
  bool clamped = false;
  if(bytes > maxAllowed)
  {
    bytes = maxAllowed;
    clamped = true;
  }

  std::lock_guard<std::mutex> lock(m_Mutex);
  m_BudgetBytes = bytes;
  return clamped;
}

uint64 MemoryBudgetManager::budgetBytes() const
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  return m_BudgetBytes;
}

uint64 MemoryBudgetManager::usedBytes() const
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  return m_UsedBytes;
}

void MemoryBudgetManager::clear()
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  m_Entries.clear();
  m_UsedBytes = 0;
}

std::vector<MemoryBudgetManager::AllocationHandle> MemoryBudgetManager::makeRoom(uint64 needed)
{
  std::vector<AllocationHandle> evicted;

  while(!m_Entries.empty() && m_UsedBytes + needed > m_BudgetBytes)
  {
    // Find entry with oldest lastAccessed using a direct iterator
    auto oldest = m_Entries.end();
    for(auto it = m_Entries.begin(); it != m_Entries.end(); ++it)
    {
      if(oldest == m_Entries.end() || it->second.lastAccessed < oldest->second.lastAccessed)
      {
        oldest = it;
      }
    }

    if(oldest == m_Entries.end())
    {
      break;
    }

    // Invoke eviction callback under mutex (must be non-blocking)
    if(oldest->second.onEvict)
    {
      oldest->second.onEvict();
    }

    m_UsedBytes -= oldest->second.sizeBytes;
    evicted.push_back(oldest->first);
    m_Entries.erase(oldest);
  }

  return evicted;
}

} // namespace nx::core
