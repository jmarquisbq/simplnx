#pragma once

#include "simplnx/Common/Types.hpp"
#include "simplnx/simplnx_export.hpp"

#include <string>

namespace nx::core
{
class DataPath;
class DataStructure;

/**
 * @brief Policy interface that decides which storage format a DataArray/NeighborList should use.
 *
 * A resolver is consulted at array-creation and import time (see ArrayCreationUtilities::CreateArray
 * and the OOC import-finalize path) only AFTER the core "unstructured geometry => in-core" gate
 * (ParentGeometrySupportsOoc) and only when no explicit per-filter format was given. It returns a
 * registered DataIOCollection format name (e.g. "HDF5-OOC") or "" for the in-memory default.
 *
 * Implementations MUST be const and thread-safe: a single shared resolver instance may be consulted
 * concurrently by multiple DataStructures (e.g. one window running a pipeline while another visualizes
 * a dragged-in file). They carry no mutable state.
 */
class SIMPLNX_EXPORT IDataStoreFormatResolver
{
public:
  virtual ~IDataStoreFormatResolver() noexcept;

  /**
   * @brief Decide the storage format for an array about to be created/imported.
   * @param dataStructure The DataStructure that contains (or will contain) the array
   * @param arrayPath The DataPath where the array lives/will be created
   * @param numericType The element data type
   * @param dataSizeBytes Total array size in bytes (0 when unknown, e.g. an unpopulated NeighborList)
   * @return A registered format name, or "" for the in-memory default
   */
  virtual std::string resolveFormat(const DataStructure& dataStructure, const DataPath& arrayPath, DataType numericType, uint64 dataSizeBytes) const = 0;

protected:
  IDataStoreFormatResolver() = default;
  IDataStoreFormatResolver(const IDataStoreFormatResolver&) = default;
  IDataStoreFormatResolver(IDataStoreFormatResolver&&) noexcept = default;
  IDataStoreFormatResolver& operator=(const IDataStoreFormatResolver&) = default;
  IDataStoreFormatResolver& operator=(IDataStoreFormatResolver&&) noexcept = default;
};
} // namespace nx::core
