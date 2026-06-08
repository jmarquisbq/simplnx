#include "ArrayCreationUtilities.hpp"

#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/DataStructure/Geometry/IGeometry.hpp"
#include "simplnx/DataStructure/IO/Generic/IDataStoreFormatResolver.hpp"
#include "simplnx/Utilities/MemoryUtilities.hpp"

using namespace nx::core;

//-----------------------------------------------------------------------------
std::string ArrayCreationUtilities::ResolveStorageFormat(const DataStructure& dataStructure, const DataPath& path, DataType numericType, uint64 dataSizeBytes, const std::string& requestedFormat)
{
  // (1) An unstructured/poly geometry ancestor forces in-core regardless of preference or override
  //     (OOC stores for those geometry types do not exist). This gate is authoritative.
  if(!ParentGeometrySupportsOoc(dataStructure, path))
  {
    return "";
  }
  // (2) An explicit per-filter override wins over the resolver.
  if(!requestedFormat.empty())
  {
    return requestedFormat;
  }
  // (3) Otherwise let the DataStructure's resolver decide (in-memory default => "").
  return dataStructure.formatResolver().resolveFormat(dataStructure, path, numericType, dataSizeBytes);
}

//-----------------------------------------------------------------------------
bool ArrayCreationUtilities::ParentGeometrySupportsOoc(const DataStructure& dataStructure, const DataPath& arrayPath)
{
  DataPath parentPath = arrayPath.getParent();
  while(parentPath.getLength() > 0)
  {
    // getData returns nullptr for a non-existent intermediate container; dynamic_cast<const IGeometry*>(nullptr)
    // is nullptr, so such levels are skipped — this is why the array leaf itself need not exist.
    const auto* obj = dataStructure.getData(parentPath);
    if(const auto* geom = dynamic_cast<const IGeometry*>(obj))
    {
      const auto geomType = geom->getGeomType();
      return geomType == IGeometry::Type::Image || geomType == IGeometry::Type::RectGrid;
    }
    parentPath = parentPath.getParent();
  }
  return true; // no geometry ancestor -> eligible (e.g., feature/ensemble attribute matrices)
}

//-----------------------------------------------------------------------------
bool ArrayCreationUtilities::CheckMemoryRequirement(const DataStructure& dataStructure, uint64 requiredMemory)
{
  static const uint64 k_AvailableMemory = Memory::GetTotalMemory();
  const uint64 memoryUsage = dataStructure.memoryUsage() + requiredMemory;
  return memoryUsage < k_AvailableMemory;
}
