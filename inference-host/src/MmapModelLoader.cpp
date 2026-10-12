#include "azookey/host/MmapModelLoader.h"

#include <cerrno>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace azookey::host {

#ifdef _WIN32

bool MmapModelLoader::Load(const std::filesystem::path& path) {
  Unload();
  error_.clear();
  HANDLE file =
      CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    error_ = "open failed: " + std::to_string(GetLastError());
    return false;
  }
  file_ = file;
  LARGE_INTEGER file_size{};
  if (!GetFileSizeEx(file, &file_size)) {
    error_ = "size failed: " + std::to_string(GetLastError());
    Unload();
    return false;
  }
  if (file_size.QuadPart <= 0) {
    error_ = "empty file";
    Unload();
    return false;
  }
  HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
  if (!mapping) {
    error_ = "mapping failed: " + std::to_string(GetLastError());
    Unload();
    return false;
  }
  mapping_ = mapping;
  void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
  if (!view) {
    error_ = "view failed: " + std::to_string(GetLastError());
    Unload();
    return false;
  }
  data_ = static_cast<std::byte*>(view);
  size_ = static_cast<std::size_t>(file_size.QuadPart);
  return true;
}

void MmapModelLoader::Unload() {
  if (data_) UnmapViewOfFile(data_);
  if (mapping_) CloseHandle(static_cast<HANDLE>(mapping_));
  if (file_) CloseHandle(static_cast<HANDLE>(file_));
  data_ = nullptr;
  mapping_ = nullptr;
  file_ = nullptr;
  size_ = 0;
}

#else

bool MmapModelLoader::Load(const std::filesystem::path& path) {
  Unload();
  error_.clear();
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    error_ = "open failed: " + std::to_string(errno);
    return false;
  }
  fd_ = fd;
  struct stat status {};
  if (fstat(fd, &status) != 0) {
    error_ = "size failed: " + std::to_string(errno);
    Unload();
    return false;
  }
  if (status.st_size <= 0) {
    error_ = "empty file";
    Unload();
    return false;
  }
  const auto size = static_cast<std::size_t>(status.st_size);
  void* view = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
  if (view == MAP_FAILED) {
    error_ = "view failed: " + std::to_string(errno);
    Unload();
    return false;
  }
  data_ = static_cast<std::byte*>(view);
  size_ = size;
  return true;
}

void MmapModelLoader::Unload() {
  if (data_) munmap(data_, size_);
  if (fd_ >= 0) close(fd_);
  data_ = nullptr;
  fd_ = -1;
  size_ = 0;
}

#endif

bool PreflightModelMapping(const std::filesystem::path& path) {
  try {
    MmapModelLoader loader;
    return loader.Load(path);
  } catch (...) {
    return false;
  }
}

std::string_view MmapFallbackReasonName(MmapFallbackReason reason) {
  return reason == MmapFallbackReason::Preflight ? "preflight" : "load";
}

}  // namespace azookey::host
