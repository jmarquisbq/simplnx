#include "FileIO.hpp"

#include "simplnx/Utilities/Parsing/HDF5/H5Support.hpp"

#include <fmt/format.h>

#include <H5Ppublic.h>

#include <atomic>
#include <functional>
#include <mutex>

namespace
{
// Counts every ReadFile() open so tests can assert that cached preflight paths
// perform zero file opens. Process-wide and atomic because ReadFile() is called
// from preflight worker threads.
std::atomic<nx::core::uint64> s_ReadOpenCount{0};

// Optional test-only hook applied to the file-access property list just before
// a file is opened. Unset in production, so opens use H5P_DEFAULT unchanged.
std::function<void(hid_t)> s_FaplConfigurator;

// Builds the file-access property list for a file open. Returns H5P_DEFAULT
// when no configurator is installed (the production path); otherwise creates a
// fapl, hands it to the configurator, and returns it. The caller must close a
// non-default result with CloseFaplId().
hid_t MakeFaplId()
{
  if(!s_FaplConfigurator)
  {
    return H5P_DEFAULT;
  }
  hid_t faplId = H5Pcreate(H5P_FILE_ACCESS);
  s_FaplConfigurator(faplId);
  return faplId;
}

// Releases a fapl produced by MakeFaplId(). H5P_DEFAULT is a constant, not an
// allocated id, so it is left alone.
void CloseFaplId(hid_t faplId)
{
  if(faplId != H5P_DEFAULT)
  {
    H5Pclose(faplId);
  }
}
} // namespace
namespace nx::core::HDF5
{
FileIO FileIO::ReadFile(const std::filesystem::path& filepath)
{
  s_ReadOpenCount++;
  // Serialize the property-list lifecycle and file open on the process-wide HDF5 lock.
  // The FileIO constructor below touches no HDF5, so it stays outside the lock.
  hid_t fileId = H5I_INVALID_HID;
  {
    std::lock_guard<std::mutex> hdf5Lock(Support::ApiLock());
    hid_t faplId = MakeFaplId();
    fileId = H5Fopen(filepath.string().c_str(), H5F_ACC_RDONLY, faplId);
    CloseFaplId(faplId);
  }
  return FileIO(filepath, fileId);
}

uint64 FileIO::GetReadOpenCount()
{
  return s_ReadOpenCount.load();
}

void FileIO::ResetReadOpenCount()
{
  s_ReadOpenCount.store(0);
}

void FileIO::SetFaplConfigurator(std::function<void(hid_t)> configurator)
{
  std::lock_guard<std::mutex> hdf5Lock(Support::ApiLock());
  s_FaplConfigurator = std::move(configurator);
}

FileIO FileIO::WriteFile(const std::filesystem::path& filepath)
{
  // The filesystem existence check and removal are pure OS calls (no HDF5), so they run
  // outside the lock; only the bare H5Fcreate leaf call is serialized on the ApiLock.
  if(std::filesystem::exists(filepath))
  {
    try
    {
      std::filesystem::remove(filepath);
    } catch(const std::exception& e)
    {
      std::string msg = fmt::format("Failed to remove file at path '{}'. Error: '{}'", filepath.string(), e.what());
      std::cout << msg << std::endl;
    }
  }

  hid_t fileId = H5I_INVALID_HID;
  {
    std::lock_guard<std::mutex> hdf5Lock(Support::ApiLock());
    hid_t faplId = MakeFaplId();
    fileId = H5Fcreate(filepath.string().c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, faplId);
    CloseFaplId(faplId);
  }
  if(fileId > 0)
  {
    return FileIO(filepath, fileId);
  }
  return {};
}

FileIO FileIO::AppendFile(const std::filesystem::path& filepath)
{
  // Self-locks the bare H5Fopen leaf call; the FileIO constructor touches no HDF5.
  hid_t fileId = H5I_INVALID_HID;
  {
    std::lock_guard<std::mutex> hdf5Lock(Support::ApiLock());
    fileId = H5Fopen(filepath.string().c_str(), H5F_ACC_RDWR, H5P_DEFAULT);
  }
  return FileIO(filepath, fileId);
}

FileIO::FileIO(const std::filesystem::path& filepath, hid_t fileId)
: GroupIO()
{
  setFilePath(filepath);
  setId(fileId);
}

FileIO::~FileIO() noexcept
{
  close();
}

hid_t FileIO::open() const
{
  if(isOpen())
  {
    return getId();
  }
  // Self-locks the bare H5Fopen leaf call. getFilePath() touches no HDF5, so the path
  // string is resolved before the lock.
  const std::string pathStr = getFilePath().string();
  hid_t id = H5I_INVALID_HID;
  {
    std::lock_guard<std::mutex> hdf5Lock(Support::ApiLock());
    id = H5Fopen(pathStr.c_str(), H5F_ACC_RDWR, H5P_DEFAULT);
  }
  setId(id);
  return id;
}

void FileIO::close()
{
  // Self-locks the bare H5Fclose leaf call. Invoked by ~FileIO, so destruction of a
  // FileIO on any thread serializes its close against every other HDF5 C call. The id is
  // captured before the lock (isOpen() already guarantees it is open) so nothing
  // lock-taking runs inside the leaf scope.
  if(isOpen())
  {
    const hid_t selfId = getId();
    {
      std::lock_guard<std::mutex> hdf5Lock(Support::ApiLock());
      H5Fclose(selfId);
    }
    setId(0);
  }
}

std::string FileIO::getName() const
{
  if(!isValid())
  {
    return "";
  }
  return getFilePath().filename().string();
}

std::string FileIO::getNamePath() const
{
  return "";
}

std::string FileIO::getObjectPath() const
{
  return "";
}

#if 0
usize FileIO::getNumAttributes() const
{
  auto file = HighFive::File(getFilePath().string(), HighFive::File::ReadOnly);
  return file.getNumberAttributes();
}

std::vector<std::string> FileIO::getAttributeNames() const
{
  auto file = HighFive::File(getFilePath().string(), HighFive::File::ReadOnly);
  return file.listAttributeNames();
}

void FileIO::deleteAttribute(const std::string& name)
{
  auto file = HighFive::File(getFilePath().string(), HighFive::File::ReadWrite);
  file.deleteAttribute(name);
}
#endif

#if 0
bool FileIO::isGroup(const std::string& childName) const
{
  if(!isValid())
  {
    return false;
  }

  if(!m_File->exist(childName))
  {
    return false;
  }
  return m_File->getObjectType(childName) == ObjectType::Group;
}

bool FileIO::isDataset(const std::string& childName) const
{
  if(!isValid())
  {
    return false;
  }

  if(!m_File->exist(childName))
  {
    return false;
  }
  return m_File->getObjectType(childName) == ObjectType::Dataset;
}
#endif

#if 0
Result<GroupIO> FileIO::createGroup(const std::string& childName)
{
  if(!isValid())
  {
    std::string ss = fmt::format("Cannot create Group '{}' as the current HDF5 FileIO is not valid.", childName);
    return MakeErrorResult<GroupIO>(-704, ss);
  }
  hid_t groupId = -1;
  if(isGroup(childName))
  {
    groupId = H5Gopen(getId(), childName.c_str(), H5P_DEFAULT);
  }
  else if (!exists(childName))
  {
    groupId = H5Gcreate(getId(), childName.c_str(), H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
  }
  if(groupId > 0)
  {
    return {GroupIO(*this, childName, groupId)};
  }

  std::string ss = fmt::format("Failed to create HDF5 group '{}' at path: ", childName, getObjectPath());
  return MakeErrorResult<GroupIO>(-722, ss);
}

std::shared_ptr<GroupIO> FileIO::createGroupPtr(const std::string& childName)
{
  if(!isValid())
  {
    return nullptr;
  }
  auto childGroup = m_File->createGroup(childName);
  return std::make_shared<GroupIO>(*this, std::move(childGroup), childName);
}
#endif

#if 0
Result<GroupIO> FileIO::openGroup(const std::string& name) const
{
  if(!isValid())
  {
    std::string ss = fmt::format("Cannot create Group '{}' as the current HDF5 FileIO is not valid.", name);
    return MakeErrorResult<GroupIO>(-804, ss);
  }
  if(!m_File->exist(name))
  {
    std::string ss = fmt::format("Cannot create Group '{}' as a child of that name does not exist.", name);
    return MakeErrorResult<GroupIO>(-805, ss);
  }
  if(m_File->exist(name) && !isGroup(name))
  {
    std::string ss = fmt::format("Cannot create Group '{}' as a child of that name already exists but is not the correct type.", name);
    return MakeErrorResult<GroupIO>(-806, ss);
  }

  auto childGroup = m_File->getGroup(name);
  return {GroupIO(const_cast<FileIO&>(*this), std::move(childGroup), name)};
}
#endif

#if 0
std::shared_ptr<GroupIO> FileIO::openGroupPtr(const std::string& name) const
{
  auto childGroup = m_File->getGroup(name);
  return std::make_shared<GroupIO>(const_cast<FileIO&>(*this), std::move(childGroup), name);
}
#endif
} // namespace nx::core::HDF5
