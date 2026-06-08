#pragma once

#include "simplnx/simplnx_export.hpp"

#include "simplnx/Common/Result.hpp"
#include "simplnx/Core/Preferences.hpp"
#include "simplnx/DataStructure/AttributeMatrix.hpp"
#include "simplnx/DataStructure/DataArray.hpp"
#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/DataStructure/EmptyDataStore.hpp"
#include "simplnx/DataStructure/IO/Generic/DataIOCollection.hpp"
#include "simplnx/Filter/Output.hpp"
#include "simplnx/Utilities/DataStoreUtilities.hpp"
#include "simplnx/Utilities/MemoryUtilities.hpp"
#include "simplnx/Utilities/StringInterpretationUtilities.hpp"
#include "simplnx/Utilities/StringUtilities.hpp"

#include <fmt/format.h>

#include <numeric>

namespace nx::core::ArrayCreationUtilities
{
/**
 * @brief Checks whether an in-core allocation of the requested size fits within available system memory.
 * @param dataStructure The DataStructure whose current memory usage is added to the requirement
 * @param requiredMemory Size in bytes of the new in-core allocation being considered
 * @return true if the combined memory requirement fits within available memory, false otherwise
 */
SIMPLNX_EXPORT bool CheckMemoryRequirement(const DataStructure& dataStructure, uint64 requiredMemory);

/**
 * @brief Returns false (=> the array MUST be in-core) iff an unstructured/poly geometry
 * (Vertex/Edge/Triangle/Quad/Tetrahedral/Hexahedral) is an ancestor of @p arrayPath. Returns true
 * for arrays under an Image/RectGrid geometry or with no geometry ancestor. This is the resolver- and
 * format-independent rule that keeps unstructured-geometry arrays in-core (their OOC stores do not
 * exist). It walks the path's ancestor containers, so the array itself need not yet exist.
 *
 * Note: this answers only whether OOC is structurally permitted for the array's location; it is
 * separate from size/preference-based resolution. Passing this check means the array CAN be OOC,
 * not that it WILL be.
 */
SIMPLNX_EXPORT bool ParentGeometrySupportsOoc(const DataStructure& dataStructure, const DataPath& arrayPath);

/**
 * @brief Resolves the storage format for an array/list about to be created, applying the standard order:
 *  (1) unstructured/poly geometry ancestor => "" (in-core, overrides everything);
 *  (2) explicit requestedFormat (non-empty) => use it;
 *  (3) otherwise => the DataStructure's format resolver.
 *
 * Note: the unstructured/poly geometry gate is AUTHORITATIVE — it returns in-core ("") even when
 * requestedFormat is a non-empty explicit format. Arrays under unstructured/poly geometries have no OOC
 * store implementation, so they are always in-core regardless of any explicit per-filter format request.
 *
 * This is the single decision point shared by every array-creation call site (CreateArray here,
 * CreateDataStore/CreateListStore, and CreateNeighborListAction). The resolver is an interface
 * (IDataStoreFormatResolver) so the rule stays OOC-agnostic: the in-memory default always returns "",
 * and an OOC-aware resolver (registered by the OOC plugin) returns a disk-backed format name when its
 * size/preference policy fires.
 *
 * @param dataStructure  The DataStructure that contains (or will contain) the array
 * @param path           The DataPath where the array lives/will be created
 * @param numericType    The element data type
 * @param dataSizeBytes  Total array size in bytes (0 when unknown, e.g. an unpopulated NeighborList)
 * @param requestedFormat An explicit per-filter format override, or "" to defer to the resolver
 * @return A registered format name, or "" for the in-memory default
 */
SIMPLNX_EXPORT std::string ResolveStorageFormat(const DataStructure& dataStructure, const DataPath& path, DataType numericType, uint64 dataSizeBytes, const std::string& requestedFormat);

/**
 * @brief Creates a DataArray with the given properties
 * @tparam T Primitive Type (int, float, ...)
 * @param dataStructure The DataStructure to use
 * @param tupleShape The Tuple Dimensions
 * @param nComp The number of components in the DataArray
 * @param path The DataPath to where the data will be stored.
 * @param mode The mode to assume: PREFLIGHT or EXECUTE. Preflight will NOT allocate any storage. EXECUTE will allocate the memory/storage
 * @return
 */
template <class T>
Result<> CreateArray(DataStructure& dataStructure, const ShapeType& tupleShape, const ShapeType& compShape, const DataPath& path, IDataAction::Mode mode, const std::string& dataFormat = "",
                     std::string fillValue = "")
{
  auto parentPath = path.getParent();

  std::optional<DataObject::IdType> dataObjectId;

  DataObject* parentObjectPtr = nullptr;
  if(parentPath.getLength() != 0)
  {
    parentObjectPtr = dataStructure.getData(parentPath);
    if(parentObjectPtr == nullptr)
    {
      return MakeErrorResult(-260, fmt::format("CreateArray: Parent object '{}' does not exist", parentPath.toString()));
    }

    dataObjectId = parentObjectPtr->getId();
  }

  if(tupleShape.empty())
  {
    return MakeErrorResult(-261, fmt::format("CreateArray: Tuple Shape was empty. Please set the number of tuples."));
  }

  // Validate Number of Components
  if(compShape.empty())
  {
    return MakeErrorResult(-262, fmt::format("CreateArray: Component Shape was empty. Please set the number of components."));
  }
  const usize numComponents = std::accumulate(compShape.cbegin(), compShape.cend(), static_cast<usize>(1), std::multiplies<>());
  if(numComponents == 0 && mode == IDataAction::Mode::Execute)
  {
    return MakeErrorResult(-263, fmt::format("CreateArray: Number of components is ZERO. Please set the number of components."));
  }

  const usize last = path.getLength() - 1;

  std::string name = path[last];

  const usize numTuples = std::accumulate(tupleShape.cbegin(), tupleShape.cend(), static_cast<usize>(1), std::multiplies<>());
  uint64 requiredMemory = numTuples * numComponents * sizeof(T);

  // Resolve the storage format through the shared decision helper: an unstructured-geometry
  // ancestor forces in-core (""), else an explicit per-filter override wins, else the
  // DataStructure's format resolver decides (in-memory default returns "", an OOC-aware
  // resolver may return a disk-backed format name).
  std::string resolvedFormat;
  if(mode == IDataAction::Mode::Execute)
  {
    resolvedFormat = ResolveStorageFormat(dataStructure, path, GetDataType<T>(), requiredMemory, dataFormat);

    // Only check RAM availability for in-core arrays. OOC arrays go to disk
    // and do not consume RAM for their primary storage. "In-core" means either
    // the empty/unset sentinel (resolver defaulted) or the explicit k_InMemoryFormat
    // constant (user forced in-memory).
    const bool isInCore = resolvedFormat.empty() || resolvedFormat == Preferences::k_InMemoryFormat.str();
    if(isInCore && !CheckMemoryRequirement(dataStructure, requiredMemory))
    {
      uint64 totalMemory = requiredMemory + dataStructure.memoryUsage();
      uint64 availableMemory = Memory::GetTotalMemory();
      return MakeErrorResult(-264, fmt::format("Cannot create array '{}': the DataStructure would require {} bytes total, "
                                               "but only {} bytes of RAM are available. Consider enabling out-of-core "
                                               "storage or lowering the size thresholds in Preferences so that large "
                                               "arrays are stored on disk instead of in memory.",
                                               path.toString(), totalMemory, availableMemory));
    }
  }

  // Preflight: never allocate; emit an EmptyDataStore that carries shape metadata only.
  // Execute: hand the already-resolved format directly to the IO collection's typed factory.
  std::shared_ptr<AbstractDataStore<T>> store;
  switch(mode)
  {
  case IDataAction::Mode::Preflight: {
    store = std::make_unique<EmptyDataStore<T>>(tupleShape, compShape, resolvedFormat);
    break;
  }
  case IDataAction::Mode::Execute: {
    // Route through the registered IO managers. The built-in core manager serves the
    // in-memory default ("" / k_InMemoryFormat); any other registered format (e.g. the
    // OOC manager's disk-backed format) is served by its manager when that plugin is loaded.
    store = DataStoreUtilities::GetIOCollection().createDataStoreWithType<T>(resolvedFormat, tupleShape, compShape);
    break;
  }
  default: {
    throw std::runtime_error("Invalid mode");
  }
  }
  if(nullptr == store)
  {
    // No registered IO manager could produce a DataStore<T> for this format.
    // Include the full manager capability list so the user can tell whether
    // the format is a typo, whether the required plugin is missing, or whether
    // the format simply does not support this store type.
    return MakeErrorResult(-265, fmt::format("CreateArray: Unable to create DataStore<T> at '{}' of DataStore format '{}'.\n{}", path.toString(), resolvedFormat,
                                             DataStoreUtilities::GetIOCollection().generateManagerListString()));
  }
  if(!fillValue.empty())
  {
    Result<T> conversionResult = StringInterpretationUtilities::Convert<T>(fillValue);

    if(conversionResult.invalid())
    {
      return ConvertResult(std::move(conversionResult));
    }
    if(mode == IDataAction::Mode::Execute)
    {
      store->fill(conversionResult.value());
      {
        // Only base data store has initialization value
        std::weak_ptr<DataStore<T>> weakDataStorePtr = std::dynamic_pointer_cast<DataStore<T>>(store);
        if(auto dataStorePtr = weakDataStorePtr.lock(); dataStorePtr != nullptr)
        {
          dataStorePtr->setInitValue(conversionResult.value());
        }
      }
    }
  }

  auto dataArray = DataArray<T>::Create(dataStructure, name, store, dataObjectId);
  if(dataArray == nullptr)
  {
    if(dataStructure.getId(path).has_value())
    {
      return MakeErrorResult(-266, fmt::format("CreateArray: Cannot create Data Array at path '{}' because it already exists. Choose a different name.", path.toString()));
    }

    if(parentObjectPtr == nullptr)
    {
      return MakeErrorResult(-267, fmt::format("CreateArray: Parent object '{}' does not exist", parentPath.toString()));
    }
    if(parentObjectPtr->getDataObjectType() == DataObject::Type::AttributeMatrix)
    {
      auto* attrMatrixPtr = dynamic_cast<AttributeMatrix*>(parentObjectPtr);
      std::string amShape = fmt::format("Attribute Matrix Tuple Dims: {}", StringUtilities::formatDimensions(attrMatrixPtr->getShape()));
      std::string arrayShape = fmt::format("Data Array Tuple Shape: {}", StringUtilities::formatDimensions(store->getTupleShape()));
      return MakeErrorResult(-268,
                             fmt::format("CreateArray: Unable to create Data Array '{}' inside Attribute matrix '{}'. Mismatch of tuple dimensions. The created Data Array must have the same tuple "
                                         "dimensions or the same total number of tuples.\n{}\n{}",
                                         name, dataStructure.getDataPathsForId(parentObjectPtr->getId()).front().toString(), amShape, arrayShape));
    }

    return MakeErrorResult(-269, fmt::format("CreateArray: Unable to create DataArray at '{}'", path.toString()));
  }

  return {};
}
} // namespace nx::core::ArrayCreationUtilities
