#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace azookey::learning {

inline bool FlushFileToDisk(const std::filesystem::path& path) {
#ifdef _WIN32
  HANDLE file = CreateFileW(path.wstring().c_str(), GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }
  const bool ok = FlushFileBuffers(file) != 0;
  CloseHandle(file);
  return ok;
#else
  int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    return false;
  }
  const bool ok = fsync(fd) == 0;
  close(fd);
  return ok;
#endif
}

inline bool WriteTextFileAtomically(const std::string& path, const std::string& content) {
  const std::filesystem::path target(path);
  std::error_code ec;
  if (!target.parent_path().empty()) {
    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec) return false;
  }

  // Build a temp name that is unique per write across threads and processes.
  // A monotonic clock value alone collides when two writers hit the same coarse
  // tick: they would then open and truncate the *same* temp file concurrently,
  // interleaving their content and racing the final rename. The process id makes
  // the name unique across processes, and a process-local atomic counter makes
  // it unique across every write within this process (covering all threads),
  // independent of clock resolution.
  static std::atomic<std::uint64_t> sequence{0};
  const auto stamp =
      static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
  const std::uint64_t seq = sequence.fetch_add(1, std::memory_order_relaxed);
#ifdef _WIN32
  const unsigned long pid = ::GetCurrentProcessId();
#else
  const unsigned long pid = static_cast<unsigned long>(::getpid());
#endif
  auto temp = target;
  temp += ".tmp." + std::to_string(pid) + "." + std::to_string(stamp) + "." +
          std::to_string(seq);
  {
    std::ofstream ofs(temp, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
      return false;
    }
    ofs << content;
    ofs.flush();
    if (!ofs.good()) {
      std::filesystem::remove(temp, ec);
      return false;
    }
  }
  if (!FlushFileToDisk(temp)) {
    std::filesystem::remove(temp, ec);
    return false;
  }

#ifdef _WIN32
  const bool moved = MoveFileExW(temp.wstring().c_str(), target.wstring().c_str(),
                                 MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
  if (!moved) {
    std::filesystem::remove(temp, ec);
  }
  return moved;
#else
  std::filesystem::rename(temp, target, ec);
  if (ec) {
    std::filesystem::remove(temp, ec);
    return false;
  }
  // Flush the parent directory to ensure the rename metadata is persisted.
  const auto dir = target.parent_path();
  if (!dir.empty()) {
    const int dir_fd = open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (dir_fd >= 0) {
      fsync(dir_fd);
      close(dir_fd);
    }
  }
  return true;
#endif
}

}  // namespace azookey::learning
