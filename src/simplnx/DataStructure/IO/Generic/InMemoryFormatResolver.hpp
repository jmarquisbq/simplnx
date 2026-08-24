#pragma once

#include "simplnx/DataStructure/IO/Generic/IDataStoreFormatResolver.hpp"

namespace nx::core
{
/**
 * @brief The default store-format policy: every array is in-memory.
 *
 * Installed as the process-wide default resolver by libsimplnx when no other resolver is set, and used
 * as the standalone in-core build's only policy. Header-only and trivial.
 */
class InMemoryFormatResolver : public IDataStoreFormatResolver
{
public:
  /**
   * @brief Selects the default in-memory store for every array.
   *
   * The empty format name is the storage-neutral signal consumed by the
   * array-creation utilities to use their normal in-memory implementation.
   * This resolver deliberately ignores array size: applications that want to
   * place sufficiently large arrays out of core install a different policy.
   *
   * @return An empty format name, selecting the default in-memory store.
   */
  std::string resolveFormat(const DataStructure& /*dataStructure*/, const DataPath& /*arrayPath*/, DataType /*numericType*/, uint64 /*dataSizeBytes*/) const override
  {
    return {};
  }
};
} // namespace nx::core
