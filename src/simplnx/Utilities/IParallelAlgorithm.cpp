#include "IParallelAlgorithm.hpp"

#include "simplnx/DataStructure/IDataStore.hpp"

namespace
{
// -----------------------------------------------------------------------------
bool CheckStoresInMemory(const nx::core::IParallelAlgorithm::AlgorithmStores& stores)
{
  if(stores.empty())
  {
    return true;
  }

  for(const auto* storePtr : stores)
  {
    if(storePtr == nullptr)
    {
      continue;
    }

    if(storePtr->getStoreType() == nx::core::IDataStore::StoreType::OutOfCore)
    {
      return false;
    }
  }

  return true;
}

// -----------------------------------------------------------------------------
bool CheckArraysInMemory(const nx::core::IParallelAlgorithm::AlgorithmArrays& arrays)
{
  if(arrays.empty())
  {
    return true;
  }

  for(const auto* arrayPtr : arrays)
  {
    if(arrayPtr == nullptr)
    {
      continue;
    }

    if(arrayPtr->getIDataStoreRef().getStoreType() == nx::core::IDataStore::StoreType::OutOfCore)
    {
      return false;
    }
  }

  return true;
}
} // namespace

namespace nx::core
{
// -----------------------------------------------------------------------------
IParallelAlgorithm::IParallelAlgorithm()
{
  // m_RunParallel defaults to true (ifdef SIMPLNX_ENABLE_MULTICORE) or false.
  // Individual filters disable via requireArraysInMemory()/requireStoresInMemory()
  // if they genuinely need in-memory data (e.g., ITK filters).
  // Disk-backed stores serialize their HDF5 access through the process-wide HDF5
  // lock, so TBB parallelism is safe on them too.
}

// -----------------------------------------------------------------------------
IParallelAlgorithm::~IParallelAlgorithm() = default;

// -----------------------------------------------------------------------------
bool IParallelAlgorithm::getParallelizationEnabled() const
{
  return m_RunParallel;
}

// -----------------------------------------------------------------------------
void IParallelAlgorithm::setParallelizationEnabled(bool doParallel)
{
#ifdef SIMPLNX_ENABLE_MULTICORE
  m_RunParallel = doParallel;
#endif
}
// -----------------------------------------------------------------------------
void IParallelAlgorithm::requireArraysInMemory(const AlgorithmArrays& arrays)
{
  setParallelizationEnabled(CheckArraysInMemory(arrays));
}

// -----------------------------------------------------------------------------
void IParallelAlgorithm::requireStoresInMemory(const AlgorithmStores& stores)
{
  setParallelizationEnabled(::CheckStoresInMemory(stores));
}
} // namespace nx::core
