#pragma once

#include "simplnx/simplnx_export.hpp"

#include "simplnx/DataStructure/AbstractDataStore.hpp"
#include "simplnx/DataStructure/EmptyDataStore.hpp"
#include "simplnx/DataStructure/EmptyListStore.hpp"
#include "simplnx/DataStructure/IO/Generic/DataIOCollection.hpp"
#include "simplnx/Filter/Output.hpp"

#include <fmt/format.h>

namespace nx::core
{
class DataStructure;

namespace ArrayCreationUtilities
{
/**
 * @brief Resolves the storage format for an array/list about to be created.
 *
 * Forward-declared here (the definition lives in ArrayCreationUtilities) so the store-creation
 * templates below can route their format decision through the single shared resolver without
 * pulling in ArrayCreationUtilities.hpp, which would create an include cycle (ArrayCreationUtilities.hpp
 * already includes this header).
 */
SIMPLNX_EXPORT std::string ResolveStorageFormat(const DataStructure& dataStructure, const DataPath& path, DataType numericType, uint64 dataSizeBytes, const std::string& requestedFormat);
} // namespace ArrayCreationUtilities
} // namespace nx::core

namespace nx::core::DataStoreUtilities
{
/**
 * @brief Returns a non-owning reference to the application's DataIOCollection.
 *
 * The DataIOCollection is owned by the Application singleton and lives for the
 * entire process lifetime. Callers receive a reference, not a shared_ptr, to
 * make the non-ownership relationship explicit and prevent accidental lifetime
 * extension.
 *
 * @return Reference to the Application's DataIOCollection.
 */
SIMPLNX_EXPORT DataIOCollection& GetIOCollection();

template <class T>
uint64 CalculateDataSize(const ShapeType& tupleShape, const ShapeType& componentShape)
{
  uint64 numValues = std::accumulate(tupleShape.begin(), tupleShape.end(), 1ULL, std::multiplies<>());
  uint64 numComponents = std::accumulate(componentShape.begin(), componentShape.end(), 1ULL, std::multiplies<>());
  return numValues * numComponents * sizeof(T);
}

/**
 * @brief Creates a DataStore whose format is resolved through the IOCollection's
 * registered format resolver.
 *
 * This is the standard way to allocate a DataStore that will live inside a
 * DataStructure. The format is resolved through
 * ArrayCreationUtilities::ResolveStorageFormat: the unstructured-geometry gate
 * first (topology arrays of unstructured/poly geometries stay in-core), then any
 * explicit format, then the DataStructure's format resolver (which, in an
 * out-of-core build, maps the DataStorageMode preference --
 * Adaptive/ForceInCore/ForceOutOfCore -- and the array's size onto a concrete
 * format). Callers get out-of-core-backed storage when an out-of-core resolver
 * routes the array to that format.
 *
 * In Preflight mode, returns an EmptyDataStore that records shape metadata
 * without allocating any storage. In Execute mode, calls the resolver and
 * forwards to createDataStoreWithType() to allocate the real backing store.
 *
 * For DataStores that are NOT going to live inside a DataStructure (e.g.,
 * scratch buffers, raw HDF5 reader output), construct the in-memory
 * DataStore class directly via std::make_shared<DataStore<T>>(...) — the
 * resolver has nothing meaningful to resolve against without a DataStructure
 * and a DataPath.
 *
 * @tparam T Primitive type (int8, float32, uint64, etc.)
 * @param dataStructure The DataStructure the array will live in (needed for format resolution)
 * @param arrayPath The DataPath where the array will be inserted (needed for parent-walk resolution)
 * @param tupleShape The tuple dimensions (e.g., {100, 200, 300} for a 3D volume)
 * @param componentShape The component dimensions (e.g., {3} for a 3-component vector)
 * @param mode PREFLIGHT returns an EmptyDataStore; EXECUTE allocates real storage
 * @return Shared pointer to the created AbstractDataStore
 */
template <class T>
std::shared_ptr<AbstractDataStore<T>> CreateDataStore(const DataStructure& dataStructure, const DataPath& arrayPath, const ShapeType& tupleShape, const ShapeType& componentShape,
                                                      IDataAction::Mode mode = IDataAction::Mode::Execute)
{
  switch(mode)
  {
  case IDataAction::Mode::Preflight: {
    return std::make_unique<EmptyDataStore<T>>(tupleShape, componentShape, std::string{});
  }
  case IDataAction::Mode::Execute: {
    // Resolve the backing format through the single shared decision helper (unstructured
    // geometry forces in-core "", else the DataStructure's resolver decides). No per-filter
    // override is available at this call site, so an empty requestedFormat is passed.
    const uint64 requiredBytes = CalculateDataSize<T>(tupleShape, componentShape);
    const std::string resolvedFormat = ArrayCreationUtilities::ResolveStorageFormat(dataStructure, arrayPath, GetDataType<T>(), requiredBytes, "");
    // Route through the registered IO managers: the built-in core manager serves the in-memory
    // default (""), any other registered format is served by its manager when that plugin is loaded.
    return GetIOCollection().createDataStoreWithType<T>(resolvedFormat, tupleShape, componentShape);
  }
  default: {
    throw std::runtime_error("Invalid mode");
  }
  }
}

/**
 * @brief Creates a ListStore whose format is resolved through the IOCollection's
 * registered format resolver.
 *
 * This is the standard way to allocate a ListStore (the backing store for
 * NeighborList) that will live inside a DataStructure. Like CreateDataStore,
 * the format is resolved through ArrayCreationUtilities::ResolveStorageFormat:
 * the unstructured-geometry gate first, then any explicit format, then the
 * DataStructure's format resolver (which, in an out-of-core build, maps the
 * DataStorageMode preference -- Adaptive/ForceInCore/ForceOutOfCore -- and the
 * array's size onto a concrete format). Callers get out-of-core-backed storage
 * when an out-of-core resolver routes the array to that format.
 *
 * In Preflight mode, returns an EmptyListStore that records shape metadata
 * without allocating any storage. In Execute mode, calls the resolver and
 * forwards to createListStoreWithType() to allocate the real backing store.
 *
 * Sizing note: NeighborList is variable-length (each tuple is a list whose
 * size isn't known at creation time). We pass numTuples * sizeof(T) as a
 * lower-bound estimate to the resolver. The unstructured-geometry gate and the
 * storage-mode choice dominate the decision for NeighborLists in practice; the
 * size threshold only matters in Adaptive mode, where a NeighborList below the
 * cutoff stays in-core.
 *
 * For ListStores that are NOT going to live inside a DataStructure (e.g.,
 * raw HDF5 reader output), construct the in-memory ListStore class directly
 * via std::make_shared<ListStore<T>>(tupleShape).
 *
 * @tparam T Primitive type of the list elements
 * @param dataStructure The DataStructure the list will live in (needed for format resolution)
 * @param arrayPath The DataPath where the list will be inserted (needed for parent-walk resolution)
 * @param tupleShape The tuple dimensions
 * @param mode PREFLIGHT returns an EmptyListStore; EXECUTE allocates real storage
 * @param dataFormat An explicit per-filter format override, or "" to defer to the format resolver.
 *                   Threaded through from CreateNeighborListAction so a filter can force a specific
 *                   store format; empty means "Automatic" (let the resolver decide).
 * @return Shared pointer to the created AbstractListStore
 */
template <class T>
std::shared_ptr<AbstractListStore<T>> CreateListStore(const DataStructure& dataStructure, const DataPath& arrayPath, const ShapeType& tupleShape, IDataAction::Mode mode = IDataAction::Mode::Execute,
                                                      const std::string& dataFormat = "")
{
  switch(mode)
  {
  case IDataAction::Mode::Preflight: {
    return std::make_unique<EmptyListStore<T>>(tupleShape);
  }
  case IDataAction::Mode::Execute: {
    // Resolve the backing format through the single shared decision helper. NeighborList is
    // variable-length, so we pass numTuples * sizeof(T) as a lower-bound size estimate; the
    // geometry-walk and user-preference checks dominate the resolver's decision for NeighborLists
    // in practice. An explicit per-filter override (dataFormat) wins over the resolver.
    const uint64 numTuples = std::accumulate(tupleShape.begin(), tupleShape.end(), 1ULL, std::multiplies<>());
    const uint64 estimatedBytes = numTuples * sizeof(T);
    const std::string resolvedFormat = ArrayCreationUtilities::ResolveStorageFormat(dataStructure, arrayPath, GetDataType<T>(), estimatedBytes, dataFormat);
    // Route through the registered IO managers (see CreateDataStore for rationale).
    return GetIOCollection().createListStoreWithType<T>(resolvedFormat, tupleShape);
  }
  default: {
    throw std::runtime_error("Invalid mode");
  }
  }
}

template <typename T>
std::shared_ptr<AbstractDataStore<T>> ConvertDataStore(const AbstractDataStore<T>& dataStore, const std::string& dataFormat)
{
  if(dataStore.getDataFormat() == dataFormat)
  {
    return nullptr;
  }

  // The caller supplies the explicit target format directly (there is no DataStructure/DataPath
  // here to resolve against). Route through the registered IO managers; the manager owning
  // @p dataFormat builds the store.
  std::shared_ptr<AbstractDataStore<T>> newStore = GetIOCollection().createDataStoreWithType<T>(dataFormat, dataStore.getTupleShape(), dataStore.getComponentShape());
  if(newStore == nullptr)
  {
    return nullptr;
  }

  newStore->copy(dataStore);
  return newStore;
}
} // namespace nx::core::DataStoreUtilities