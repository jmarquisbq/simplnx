#include "ArrayCreationUtilities.hpp"

#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/DataStructure/Geometry/IGeometry.hpp"
#include "simplnx/DataStructure/IO/Generic/IDataStoreFormatResolver.hpp"
#include "simplnx/Utilities/MemoryUtilities.hpp"

using namespace nx::core;

std::string ArrayCreationUtilities::ResolveStorageFormat(const DataStructure& dataStructure, const DataPath& path, DataType numericType, uint64 dataSizeBytes, const std::string& requestedFormat)
{
  // Geometry compatibility is authoritative because unsupported geometry stores do not exist.
  if(!ParentGeometrySupportsOoc(dataStructure, path))
  {
    return "";
  }
  // A compatible geometry permits an explicit format to override automatic policy.
  if(!requestedFormat.empty())
  {
    return requestedFormat;
  }
  // The DataStructure resolver handles the remaining automatic request.
  return dataStructure.formatResolver().resolveFormat(dataStructure, path, numericType, dataSizeBytes);
}

bool ArrayCreationUtilities::ParentGeometrySupportsOoc(const DataStructure& dataStructure, const DataPath& arrayPath)
{
  DataPath parentPath = arrayPath.getParent();
  while(parentPath.getLength() > 0)
  {
    // Missing containers are skipped, so the future leaf object does not need to exist.
    const auto* obj = dataStructure.getData(parentPath);
    if(const auto* geom = dynamic_cast<const IGeometry*>(obj))
    {
      const auto geomType = geom->getGeomType();
      return geomType == IGeometry::Type::Image || geomType == IGeometry::Type::RectGrid;
    }
    parentPath = parentPath.getParent();
  }
  // Objects without a geometry ancestor are structurally eligible for OOC storage.
  return true;
}

bool ArrayCreationUtilities::CheckMemoryRequirement(const DataStructure& dataStructure, uint64 requiredMemory)
{
  static const uint64 k_AvailableMemory = Memory::GetTotalMemory();
  const uint64 memoryUsage = dataStructure.memoryUsage() + requiredMemory;
  return memoryUsage < k_AvailableMemory;
}

bool ArrayCreationUtilities::WouldExceedAvailableMemory(uint64 currentUsageBytes, uint64 requiredMemory, uint64 availableBytes)
{
  // Overflow-safe form of (currentUsageBytes + requiredMemory) > availableBytes.
  if(requiredMemory > availableBytes)
  {
    return true;
  }
  return currentUsageBytes > (availableBytes - requiredMemory);
}
