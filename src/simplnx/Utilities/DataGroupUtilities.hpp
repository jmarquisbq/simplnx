#pragma once

#include "simplnx/DataStructure/BaseGroup.hpp"
#include "simplnx/DataStructure/DataArray.hpp"
#include "simplnx/DataStructure/DataPath.hpp"
#include "simplnx/DataStructure/DataStructure.hpp"
#include "simplnx/DataStructure/IDataArray.hpp"
#include "simplnx/Filter/IFilter.hpp"
#include "simplnx/simplnx_export.hpp"

#include <memory>
#include <vector>

namespace nx::core
{
/**
 * @brief Result of ComputeFeatureRenumbering: the compaction mapping applied when inactive features
 * are removed from a feature AttributeMatrix.
 */
struct FeatureRenumbering
{
  std::vector<size_t> newNames; ///< Old feature id -> new compacted id. Feature 0 and inactive features map to 0.
  std::vector<size_t> keepList; ///< Active feature ids (>= 1), in ascending order.
  bool anyRemoved = false;      ///< True if at least one feature with id >= 1 is inactive.
};

/**
 * @brief Computes the feature-id compaction mapping for RemoveInactiveObjects.
 *
 * Active features (id >= 1) are renumbered to consecutive ids starting at 1, preserving order;
 * feature 0 and any inactive feature map to 0. This is the single source of truth for the renumber
 * mapping so callers that renumber cell FeatureIds upstream (and then pass
 * cellFeatureIdsRenumbered=true to RemoveInactiveObjects) stay consistent with the feature-array
 * compaction RemoveInactiveObjects performs.
 *
 * @param activeObjects Per-feature active flag (index 0 is the unused/feature-0 slot).
 * @return FeatureRenumbering holding newNames, keepList, and anyRemoved.
 */
SIMPLNX_EXPORT FeatureRenumbering ComputeFeatureRenumbering(const std::vector<bool>& activeObjects);

/**
 * @brief RemoveInactiveObjects This assumes a single Dimension TupleShape, i.e., a Linear array, (1D)
 *
 * NeighborLists are NO LONGER Removed. That is the responsibility of the filter.
 *
 * @param dataStructure
 * @param featureDataGroupPath
 * @param activeObjects
 * @param cellFeatureIds
 * @param messageHandler
 * @param shouldCancel
 * @param cellFeatureIdsRenumbered When true, the caller has already remapped cellFeatureIds using the
 *        ComputeFeatureRenumbering mapping (e.g. fused into an earlier pass), so the expensive
 *        full-volume renumber of cellFeatureIds is skipped. Feature-array compaction and the
 *        AttributeMatrix resize still run. Defaults to false (RemoveInactiveObjects renumbers).
 * @return
 */
SIMPLNX_EXPORT bool RemoveInactiveObjects(DataStructure& dataStructure, const DataPath& featureDataGroupPath, const std::vector<bool>& activeObjects, Int32AbstractDataStore& cellFeatureIds,
                                          size_t currentFeatureCount, const IFilter::MessageHandler& messageHandler, const std::atomic_bool& shouldCancel, bool cellFeatureIdsRenumbered = false);

/**
 * @brief This function will gather all of the sibling DataArrays to the input DataPath, then filter out all the 'IgnoredDataPaths`
 * @param dataStructure The DataStructure to operate on
 * @param dataArrayPath The DataArrayPath to find all sibling DataArrays to
 * @param ignoredDataPaths Vector of DataPaths that should be remove from the final vector.
 * @return
 */
SIMPLNX_EXPORT std::vector<std::shared_ptr<IDataArray>> GenerateDataArrayList(const DataStructure& dataStructure, const DataPath& dataArrayPath, const std::vector<DataPath>& ignoredDataPaths);

/**
 * @brief This function will return all the DataPaths within a BaseGroup that are of a certain type
 * @param dataStructure The DataStructure to use
 * @param parentGroup The parent group whose children you want to get
 * @param dataObjectType The type of children you want to get
 * @param ignoredDataPaths Vector of DataPaths that should be remove from the final vector.
 * @return std::optional<std::vector<DataPath>>  of child paths that meet the DataObjectType requirement if there no errors during the process.
 */
SIMPLNX_EXPORT std::optional<std::vector<DataPath>> GetAllChildDataPaths(const DataStructure& dataStructure, const DataPath& parentGroup, DataObject::Type dataObjectType,
                                                                         const std::vector<DataPath>& ignoredDataPaths = {});

/**
 * @brief This function will return all the DataPaths within a BaseGroup
 * @param dataStructure The DataStructure to use
 * @param parentGroup The parent group whose children you want to get
 * @return std::optional<std::vector<DataPath>>  of child paths  if there no errors during the process.
 */
SIMPLNX_EXPORT std::optional<std::vector<DataPath>> GetAllChildDataPaths(const DataStructure& dataStructure, const DataPath& parent);

template <typename T>
concept DataObjectDerivedType = std::is_base_of_v<DataObject, T>;

/**
 * @brief This function will return all the DataPaths within a BaseGroup that are of a T type
 * @tparam TargetObjectT the type of DataObject to search for
 * @param dataStructure The DataStructure to use
 * @param parentGroup The parent group whose children you want to get
 * @param ignoredDataPaths Vector of DataPaths that should be removed from the final vector.
 * @return std::optional<std::vector<DataPath>> of child paths that are of T type if there are no errors during the process.
 */
template <DataObjectDerivedType TargetObjectT>
std::optional<std::vector<DataPath>> GetAllChildDataPathsOfType(const DataStructure& dataStructure, const DataPath& parentGroup, const std::vector<DataPath>& ignoredDataPaths = {})
{
  std::vector<DataPath> childDataObjects;
  try
  {
    std::vector<std::string> childrenNames;
    if(parentGroup.empty())
    {
      childrenNames = dataStructure.getDataMap().getNames();
    }
    else
    {
      childrenNames = dataStructure.getDataRefAs<BaseGroup>(parentGroup).getDataMap().getNames();
    }

    for(const auto& childName : childrenNames)
    {
      bool ignore = false;
      DataPath childPath = parentGroup.createChildPath(childName);
      const DataObject* dataObject = dataStructure.getData(childPath);
      for(const auto& ignoredPath : ignoredDataPaths)
      {
        if(childPath == ignoredPath)
        {
          ignore = true;
          break;
        }
      }
      if(!ignore && dynamic_cast<const TargetObjectT*>(dataObject) != nullptr)
      {
        childDataObjects.push_back(childPath);
      }
    }
  } catch(std::exception& e)
  {
    return {};
  }
  return {childDataObjects};
}

/**
 * @brief This function will return all the DataPaths within a BaseGroup that are of an IArray type
 * @param dataStructure The DataStructure to use
 * @param parentGroup The parent group whose children you want to get
 * @param ignoredDataPaths Vector of DataPaths that should be remove from the final vector.
 * @return std::optional<std::vector<DataPath>>  of child paths that are an IArray type if there no errors during the process.
 */
SIMPLNX_EXPORT std::optional<std::vector<DataPath>> GetAllChildArrayDataPaths(const DataStructure& dataStructure, const DataPath& parentGroup, const std::vector<DataPath>& ignoredDataPaths = {});

/**
 * @brief This function will recursively return all the DataPaths within a BaseGroup
 * @param dataStructure The DataStructure to use
 * @param parentGroup The parent group whose children you want to get
 * @param ignoredDataPaths Vector of DataPaths that should be remove from the final vector.
 * @return std::optional<std::vector<DataPath>>  of child paths that are an IArray type if there no errors during the process.
 */
SIMPLNX_EXPORT std::optional<std::vector<DataPath>> GetAllChildDataPathsRecursive(const DataStructure& dataStructure, const DataPath& parentGroup, const std::vector<DataPath>& ignoredDataPaths = {});

/**
 * @brief This function will recursively return all the DataPaths within a BaseGroup that are of a given type
 * @param dataStructure The DataStructure to use
 * @param parentGroup The parent group whose children you want to get
 * @param dataObjectType The type of children you want to get
 * @param ignoredDataPaths Vector of DataPaths that should be remove from the final vector.
 * @return std::optional<std::vector<DataPath>>  of child paths that are an IArray type if there no errors during the process.
 */
SIMPLNX_EXPORT std::optional<std::vector<DataPath>> GetAllChildDataPathsRecursive(const DataStructure& dataStructure, const DataPath& parentGroup, DataObject::Type dataObjectType,
                                                                                  const std::vector<DataPath>& ignoredDataPaths = {});

/**
 * @brief This function will return true if the arrayName is in the list of children of type IDataArray for the given parentGroup
 * @param dataStructure The DataStructure to use
 * @param parentGroup The parent group whose children you want to check against
 * @param arrayName The target name of the array you want to check for in the parentGroup
 * @return bool Whether or not the arrayName exists as a child of the parentGroup
 */
SIMPLNX_EXPORT bool ContainsDataArrayName(const DataStructure& dataStructure, const DataPath& parentGroup, const std::string& arrayName);

} // namespace nx::core
