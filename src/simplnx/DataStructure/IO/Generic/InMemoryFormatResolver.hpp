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
  std::string resolveFormat(const DataStructure& /*dataStructure*/, const DataPath& /*arrayPath*/, DataType /*numericType*/, uint64 /*dataSizeBytes*/) const override
  {
    return {};
  }
};
} // namespace nx::core
