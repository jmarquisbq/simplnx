#include "GroupIO.hpp"

#include "simplnx/Utilities/Parsing/HDF5/H5Support.hpp"
#include "simplnx/Utilities/Parsing/HDF5/IO/DatasetIO.hpp"

#include <H5Dpublic.h>
#include <H5Gpublic.h>
#include <H5Opublic.h>

#include <fmt/format.h>

#include <iostream>
#include <mutex>

namespace nx::core::HDF5
{
IdType getGroupId(IdType parentId, const std::string& groupName)
{
  // Self-locks: the whole probe-then-open/create sequence is bare HDF5 C calls and
  // touches no other lock-taking function, so it is one leaf critical section. HDF5
  // requires only that one thread at a time be inside any C call, which one held lock
  // across these consecutive calls satisfies.
  std::lock_guard<std::mutex> hdf5Lock(Support::ApiLock());

  // Check if group exists
  HDF_ERROR_HANDLER_OFF
  auto status = H5Gget_objinfo(parentId, groupName.c_str(), 0, NULL);
  HDF_ERROR_HANDLER_ON

  if(status == 0) // if group exists...
  {
    return H5Gopen(parentId, groupName.c_str(), H5P_DEFAULT);
  }
  else // if group does not exist...
  {
    return H5Gcreate(parentId, groupName.c_str(), H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
  }
}

GroupIO::GroupIO() = default;

GroupIO::GroupIO(hid_t parentId, const std::string& groupName, hid_t groupId)
: ObjectIO(parentId, groupName)
{
  setId(groupId);
}

GroupIO::~GroupIO() noexcept
{
  close();
}

hid_t GroupIO::open() const
{
  if(isOpen())
  {
    return getId();
  }
  // Resolve the parent id and path (no HDF5 work) before locking the bare H5Gopen leaf.
  const hid_t parentId = getParentId();
  const std::string namePath = getNamePath();
  hid_t id = H5I_INVALID_HID;
  {
    std::lock_guard<std::mutex> hdf5Lock(Support::ApiLock());
    id = H5Gopen(parentId, namePath.c_str(), H5P_DEFAULT);
  }
  setId(id);
  return id;
}

void GroupIO::close()
{
  // Self-locks the bare H5Gclose leaf call. Invoked by ~GroupIO, so destroying a GroupIO
  // on any thread serializes its close against every other HDF5 C call. The id is captured
  // before the lock (isOpen() already guarantees it is open) so nothing lock-taking runs
  // inside the leaf scope.
  if(isOpen())
  {
    const hid_t selfId = getId();
    {
      std::lock_guard<std::mutex> hdf5Lock(Support::ApiLock());
      H5Gclose(selfId);
    }
    setId(0);
  }
}

GroupIO GroupIO::openGroup(const std::string& name) const
{
  // isGroup() self-locks (it routes through getObjectType), so it is resolved BEFORE the
  // leaf lock here; only the bare H5Gopen is wrapped. getId() also self-locks (it lazily
  // opens this group), so it too is resolved outside the lock.
  if(!isGroup(name))
  {
    std::string ss = fmt::format("Could not open Group '{}'. Child object does not exist or object is not a Group", name);
    std::cout << ss << std::endl;
    return {};
  }
  const hid_t selfId = getId();
  hid_t groupId = H5I_INVALID_HID;
  {
    std::lock_guard<std::mutex> hdf5Lock(Support::ApiLock());
    groupId = H5Gopen(selfId, name.c_str(), H5P_DEFAULT);
  }
  if(groupId <= 0)
  {
    std::string ss = fmt::format("Failed to open Group '{}'.", name);
    std::cout << ss << std::endl;
    return {};
  }
  return GroupIO(selfId, name, groupId);
}

DatasetIO GroupIO::openDataset(const std::string& name) const
{
  // isDataset() self-locks (via getObjectType); resolve it before constructing the
  // DatasetIO. The DatasetIO is opened lazily on first use, so no HDF5 work happens here.
  if(!isDataset(name))
  {
    std::string ss = fmt::format("Could not open Dataset '{}'. Child object does not exist or object is not a Dataset", name);
    std::cout << ss << std::endl;
    return {};
  }
  return DatasetIO(getId(), name);
}

usize GroupIO::getNumChildren() const
{
  if(!isValid())
  {
    return 0;
  }

  // getId() self-locks; resolve it before the leaf-locked bare H5Gget_num_objs.
  const hid_t selfId = getId();
  hsize_t numChildren = 0;
  {
    std::lock_guard<std::mutex> hdf5Lock(Support::ApiLock());
    H5Gget_num_objs(selfId, &numChildren);
  }
  return numChildren;
}

std::string GroupIO::getChildNameByIdx(hsize_t idx) const
{
  // getId() self-locks; resolve it before the leaf-locked bare H5Gget_objname_by_idx.
  const hid_t selfId = getId();
  const size_t size = 1024;
  char buffer[size];
  {
    std::lock_guard<std::mutex> hdf5Lock(Support::ApiLock());
    H5Gget_objname_by_idx(selfId, idx, buffer, size);
  }
  return GetNameFromBuffer(buffer);
}

std::vector<std::string> GroupIO::getChildNames() const
{
  if(!isValid())
  {
    return {};
  }

  usize numChildren = getNumChildren();
  std::vector<std::string> names(numChildren);
  for(usize i = 0; i < numChildren; i++)
  {
    names[i] = getChildNameByIdx(i);
  }
  return names;
}

bool GroupIO::isGroup(const std::string& childName) const
{
  return getObjectType(childName) == ObjectType::Group;
}

bool GroupIO::isDataset(const std::string& childName) const
{
  return getObjectType(childName) == ObjectType::Dataset;
}

bool GroupIO::exists(const std::string& childName) const
{
  return getObjectType(childName) != ObjectType::Unknown;
}

ObjectIO::ObjectType GroupIO::getObjectType(const std::string& childName) const
{
  // open() self-locks its own H5Gopen leaf, so it runs BEFORE this method's leaf lock
  // (holding the non-recursive ApiLock across it would self-deadlock). The query and the
  // error-handler toggles around it are then one leaf critical section.
  open();
  if(!isValid())
  {
    return ObjectType::Unknown;
  }

  const hid_t selfId = getId();
  herr_t error = 1;
  H5O_info2_t objectInfo{};
  {
    std::lock_guard<std::mutex> hdf5Lock(Support::ApiLock());
    HDF_ERROR_HANDLER_OFF
    error = H5Oget_info_by_name3(selfId, childName.c_str(), &objectInfo, H5O_INFO_BASIC, H5P_DEFAULT);
    HDF_ERROR_HANDLER_ON
  }
  if(error < 0)
  {
    return ObjectType::Unknown;
  }

  int32 objectType = objectInfo.type;
  switch(objectType)
  {
  case H5O_TYPE_GROUP:
    return ObjectType::Group;
    break;
  case H5O_TYPE_DATASET:
    return ObjectType::Dataset;
    break;
  case H5O_TYPE_NAMED_DATATYPE:
    break;
  default:
    break;
  }

  return ObjectType::Unknown;
}

GroupIO GroupIO::createGroup(const std::string& childName)
{
  if(!isValid())
  {
    std::string ss = fmt::format("Cannot create Group '{}' as the current group is not valid", childName);
    std::cout << ss << std::endl;
    return {};
  }
  // isGroup()/exists() self-lock (via getObjectType), so the type probe is resolved
  // BEFORE this method's leaf lock; getId() self-locks too, so it is resolved first. Only
  // the bare H5Gopen/H5Gcreate is wrapped.
  const bool childIsGroup = isGroup(childName);
  const bool childExists = childIsGroup || exists(childName);
  const hid_t selfId = getId();
  hid_t groupId = -1;
  {
    std::lock_guard<std::mutex> hdf5Lock(Support::ApiLock());
    if(childIsGroup)
    {
      groupId = H5Gopen(selfId, childName.c_str(), H5P_DEFAULT);
    }
    else if(!childExists)
    {
      groupId = H5Gcreate(selfId, childName.c_str(), H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    }
  }
  if(groupId > 0)
  {
    return GroupIO(selfId, childName, groupId);
  }

  std::string ss = fmt::format("Failed to create HDF5 group '{}' at path: ", childName, getObjectPath());
  std::cout << ss << std::endl;
  return {};
}

DatasetIO GroupIO::openDataset(const std::string& childName)
{
  if(!isValid())
  {
    std::string ss = fmt::format("Cannot open Dataset '{}'. Current object is not valid.", childName);
    std::cout << ss << std::endl;
    return {};
  }

  if(isDataset(childName) || !exists(childName))
  {
    return DatasetIO(getId(), childName);
  }

  std::string ss = fmt::format("Failed to open Dataset '{}' at path: ", childName, getObjectPath());
  std::cout << ss << std::endl;
  return {};
}

std::shared_ptr<DatasetIO> GroupIO::openDatasetPtr(const std::string& childName) const
{
  if(!isValid())
  {
    return nullptr;
  }
  if(!isDataset(childName))
  {
    return nullptr;
  }

  return std::make_shared<DatasetIO>(getId(), childName);
}

std::shared_ptr<GroupIO> GroupIO::openGroupPtr(const std::string& name) const
{
  // isGroup() and getId() self-lock; both are resolved before the leaf-locked H5Gopen.
  if(!isGroup(name))
  {
    std::string ss = fmt::format("Could not open Group '{}'. Child object does not exist or object is not a Group", name);
    std::cout << ss << std::endl;
    return nullptr;
  }
  const hid_t selfId = getId();
  hid_t groupId = H5I_INVALID_HID;
  {
    std::lock_guard<std::mutex> hdf5Lock(Support::ApiLock());
    groupId = H5Gopen(selfId, name.c_str(), H5P_DEFAULT);
  }
  if(groupId <= 0)
  {
    std::string ss = fmt::format("Failed to open Group '{}'.", name);
    std::cout << ss << std::endl;
    return nullptr;
  }
  return std::shared_ptr<GroupIO>(new GroupIO(selfId, name, groupId));
}

DatasetIO GroupIO::createDataset(const std::string& childName)
{
  if(!isValid())
  {
    std::string ss = fmt::format("Cannot create Dataset '{}' as the current Group is not valid.", childName);
    std::cout << ss << std::endl;
    return {};
  }

  return DatasetIO(getId(), childName);
}

std::shared_ptr<DatasetIO> GroupIO::createDatasetPtr(const std::string& childName)
{
  if(!isValid())
  {
    return nullptr;
  }

  return std::make_shared<DatasetIO>(getId(), childName);
}

Result<> GroupIO::createLink(const std::string& objectPath)
{
  if(objectPath.empty())
  {
    return MakeErrorResult(-105, "Cannot create link with empty path");
  }

  size_t index = objectPath.find_last_of('/');
  if(index > 0)
  {
    index++;
  }
  std::string objectName = objectPath.substr(index);

  // getId() self-locks (it may lazily open this group); resolve both ids before the
  // leaf-locked bare H5Lcreate_hard.
  const hid_t parentId = getParentId();
  const hid_t selfId = getId();
  herr_t errorCode = 0;
  {
    std::lock_guard<std::mutex> hdf5Lock(Support::ApiLock());
    errorCode = H5Lcreate_hard(parentId, objectPath.c_str(), selfId, objectName.c_str(), H5P_DEFAULT, H5P_DEFAULT);
  }
  if(errorCode < 0)
  {
    return MakeErrorResult(errorCode, fmt::format("Error creating link to path: {}", objectPath));
  }
  return {};
}
// -----------------------------------------------------------------------------
} // namespace nx::core::HDF5
