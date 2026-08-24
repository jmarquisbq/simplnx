#pragma once

#include "simplnx/Common/Types.hpp"

#include <cstddef>
#include <limits>
#include <string>

// Platform abstraction for single positional file I/O. POSIX uses pread() with a
// shareable file descriptor. Windows uses a private synchronous handle per read,
// seeks it with SetFilePointerEx(), and calls ReadFile() without an OVERLAPPED struct.
//
// A general-purpose positional (offset-based) file reader, independent of HDF5:
// for callers that read raw bytes at an absolute offset through their own file
// handle (e.g. bypassing the HDF5 library to read chunk bytes in parallel). Kept
// free of any caller-specific or compression-specific detail so it stays broadly
// reusable.
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace nx::core::detail
{
#ifdef _WIN32
using FileHandle = HANDLE;
inline FileHandle invalidFileHandle()
{
  return INVALID_HANDLE_VALUE;
}
inline bool isValidFileHandle(FileHandle h)
{
  return h != INVALID_HANDLE_VALUE;
}
inline FileHandle openFileForRead(const std::string& path)
{
  // FILE_SHARE_READ | FILE_SHARE_WRITE so HDF5's own open handle on the same file
  // does not conflict with our independent read handle.
  return CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}
inline std::ptrdiff_t positionalRead(FileHandle h, void* buf, std::size_t bytes, uint64_t offset)
{
  if(bytes > static_cast<std::size_t>(std::numeric_limits<DWORD>::max()) || offset > static_cast<uint64_t>(std::numeric_limits<LONGLONG>::max()))
  {
    return -1;
  }

  LARGE_INTEGER fileOffset{};
  fileOffset.QuadPart = static_cast<LONGLONG>(offset);
  if(!SetFilePointerEx(h, fileOffset, nullptr, FILE_BEGIN))
  {
    return -1;
  }

  DWORD bytesRead = 0;
  if(!ReadFile(h, buf, static_cast<DWORD>(bytes), &bytesRead, nullptr))
  {
    return -1;
  }
  return static_cast<std::ptrdiff_t>(bytesRead);
}
inline void closeFileHandle(FileHandle h)
{
  CloseHandle(h);
}
#else
using FileHandle = int;
inline FileHandle invalidFileHandle()
{
  return -1;
}
inline bool isValidFileHandle(FileHandle h)
{
  return h >= 0;
}
inline FileHandle openFileForRead(const std::string& path)
{
  return ::open(path.c_str(), O_RDONLY);
}
inline std::ptrdiff_t positionalRead(FileHandle h, void* buf, std::size_t bytes, uint64_t offset)
{
  return ::pread(h, buf, bytes, static_cast<off_t>(offset));
}
inline void closeFileHandle(FileHandle h)
{
  ::close(h);
}
#endif
} // namespace nx::core::detail
