#pragma once

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>

#include "azookey/learning/PersistenceDiagnostics.h"
#include "azookey/learning/PersistenceRetry.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace azookey::learning {

#ifdef _WIN32
namespace detail {

template <class Operation>
DWORD RetryTransientFileOperation(Operation operation, std::chrono::milliseconds retry_budget,
                                  bool retry_access_denied = false) {
  const auto deadline = std::chrono::steady_clock::now() + retry_budget;
  while (true) {
    const DWORD error = operation();
    if (error == ERROR_SUCCESS) return error;
    const bool transient = error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION ||
                           (retry_access_denied && error == ERROR_ACCESS_DENIED);
    const auto now = std::chrono::steady_clock::now();
    if (!transient || now >= deadline) return error;
    std::this_thread::sleep_for(std::min(
        deadline - now, std::chrono::steady_clock::duration(std::chrono::milliseconds(10))));
  }
}

}  // namespace detail
#endif

inline bool FlushFileToDisk(const std::filesystem::path& path,
                            std::chrono::milliseconds retry_budget = kTransientFileRetryBudget) {
#ifdef _WIN32
  const auto native_path = path.wstring();
  HANDLE file = INVALID_HANDLE_VALUE;
  const DWORD open_error = detail::RetryTransientFileOperation(
      [&] {
        file = CreateFileW(native_path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        return file == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
      },
      retry_budget);
  if (file == INVALID_HANDLE_VALUE) {
    return detail::ReportPersistenceFailure("flush-open",
                                            {static_cast<int>(open_error), std::system_category()});
  }
  const bool ok = FlushFileBuffers(file) != 0;
  const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
  CloseHandle(file);
  return ok || detail::ReportPersistenceFailure("flush",
                                                {static_cast<int>(error), std::system_category()});
#else
  (void)retry_budget;
  int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    return detail::ReportPersistenceFailure("flush-open", {errno, std::generic_category()});
  }
  const bool ok = fsync(fd) == 0;
  const int error = ok ? 0 : errno;
  close(fd);
  return ok || detail::ReportPersistenceFailure("flush", {error, std::generic_category()});
#endif
}

// retry_budget bounds each of the flush-open and replace retries on Windows.
inline bool WriteTextFileAtomically(
    const std::filesystem::path& target, const std::string& content,
    std::chrono::milliseconds retry_budget = kTransientFileRetryBudget) {
  std::error_code ec;
  if (!target.parent_path().empty()) {
    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec) return detail::ReportPersistenceFailure("create-directory", ec);
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
  temp += ".tmp." + std::to_string(pid) + "." + std::to_string(stamp) + "." + std::to_string(seq);
  {
    errno = 0;
    std::ofstream ofs(temp, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
      return detail::ReportPersistenceFailure("temp-open", {errno, std::generic_category()});
    }
    ofs << content;
    ofs.flush();
    if (!ofs.good()) {
      const int error = errno != 0 ? errno : EIO;
      ofs.close();
      std::filesystem::remove(temp, ec);
      return detail::ReportPersistenceFailure("temp-write", {error, std::generic_category()});
    }
  }
  if (!FlushFileToDisk(temp, retry_budget)) {
    std::filesystem::remove(temp, ec);
    return false;
  }

#ifdef _WIN32
  const auto native_temp = temp.wstring();
  const auto native_target = target.wstring();
  // A reader without FILE_SHARE_DELETE can produce ACCESS_DENIED as well as
  // SHARING_VIOLATION. Retry only the completed temp file's replacement; do not
  // repeat encryption or read-modify-write, and retain the caller's file lock.
  const DWORD error = detail::RetryTransientFileOperation(
      [&] {
        return MoveFileExW(native_temp.c_str(), native_target.c_str(),
                           MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
                   ? ERROR_SUCCESS
                   : GetLastError();
      },
      retry_budget, true);
  if (error != ERROR_SUCCESS) {
    std::filesystem::remove(temp, ec);
    return detail::ReportPersistenceFailure("atomic-replace",
                                            {static_cast<int>(error), std::system_category()});
  }
  return true;
#else
  std::filesystem::rename(temp, target, ec);
  if (ec) {
    const auto error = ec;
    std::filesystem::remove(temp, ec);
    return detail::ReportPersistenceFailure("atomic-replace", error);
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
