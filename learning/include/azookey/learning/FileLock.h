#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <cwctype>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace azookey::learning {

class ScopedFileLock {
 public:
  ScopedFileLock() = default;
  ScopedFileLock(const ScopedFileLock&) = delete;
  ScopedFileLock& operator=(const ScopedFileLock&) = delete;

  ScopedFileLock(ScopedFileLock&& other) noexcept { MoveFrom(other); }

  ScopedFileLock& operator=(ScopedFileLock&& other) noexcept {
    if (this != &other) {
      Release();
      MoveFrom(other);
    }
    return *this;
  }

  ~ScopedFileLock() { Release(); }

  bool owns_lock() const noexcept { return owns_; }

 private:
#ifdef _WIN32
  explicit ScopedFileLock(HANDLE handle, bool owns) : handle_(handle), owns_(owns) {}

  void MoveFrom(ScopedFileLock& other) noexcept {
    handle_ = other.handle_;
    owns_ = other.owns_;
    other.handle_ = nullptr;
    other.owns_ = false;
  }

  void Release() noexcept {
    if (handle_) {
      if (owns_) {
        ::ReleaseMutex(handle_);
      }
      ::CloseHandle(handle_);
    }
    handle_ = nullptr;
    owns_ = false;
  }

  HANDLE handle_{nullptr};
#else
  explicit ScopedFileLock(int fd, bool owns) : fd_(fd), owns_(owns) {}

  void MoveFrom(ScopedFileLock& other) noexcept {
    fd_ = other.fd_;
    owns_ = other.owns_;
    other.fd_ = -1;
    other.owns_ = false;
  }

  void Release() noexcept {
    if (fd_ >= 0) {
      if (owns_) {
        (void)::flock(fd_, LOCK_UN);
      }
      (void)::close(fd_);
    }
    fd_ = -1;
    owns_ = false;
  }

  int fd_{-1};
#endif
  bool owns_{false};

  friend std::optional<ScopedFileLock> AcquireExclusiveFileLockForPath(
      const std::filesystem::path& path, std::chrono::milliseconds timeout);
};

namespace detail {

inline std::filesystem::path AbsoluteLexicalPath(const std::filesystem::path& path) {
  std::error_code ec;
  auto absolute = std::filesystem::absolute(path, ec);
  if (ec) {
    absolute = path;
  }
  return absolute.lexically_normal();
}

inline std::uint64_t Fnv1a64Append(std::uint64_t hash, std::uint32_t code) {
  constexpr std::uint64_t kFnvPrime = 1099511628211ull;
  for (int shift = 0; shift < 32; shift += 8) {
    hash ^= static_cast<unsigned char>((code >> shift) & 0xFFu);
    hash *= kFnvPrime;
  }
  return hash;
}

inline std::string HexString(std::uint64_t value) {
  std::ostringstream oss;
  oss << std::hex << value;
  return oss.str();
}

inline std::uint64_t HashPathForLock(const std::filesystem::path& path) {
  constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
  std::uint64_t hash = kFnvOffset;
#ifdef _WIN32
  const auto normalized = AbsoluteLexicalPath(path).wstring();
  for (wchar_t ch : normalized) {
    hash = Fnv1a64Append(hash, static_cast<std::uint32_t>(std::towlower(ch)));
  }
#else
  const auto normalized = AbsoluteLexicalPath(path).string();
  for (unsigned char ch : normalized) {
    hash = Fnv1a64Append(hash, ch);
  }
#endif
  return hash;
}

#ifdef _WIN32
inline std::wstring MutexNameForPath(const std::filesystem::path& path) {
  const auto hex = HexString(HashPathForLock(path));
  std::wostringstream oss;
  oss << L"Local\\azookey-file-lock-" << std::wstring(hex.begin(), hex.end());
  return oss.str();
}
#else
inline std::filesystem::path LockFilePathFor(const std::filesystem::path& path) {
  std::error_code ec;
  auto root = std::filesystem::temp_directory_path(ec);
  if (ec) {
    root = ".";
  }
  return root / ("azookey-file-lock-" + HexString(HashPathForLock(path)) + ".lock");
}
#endif

}  // namespace detail

inline std::optional<ScopedFileLock> AcquireExclusiveFileLockForPath(
    const std::filesystem::path& path,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
#ifdef _WIN32
  const auto name = detail::MutexNameForPath(path);
  HANDLE mutex = ::CreateMutexW(nullptr, FALSE, name.c_str());
  if (!mutex) {
    return std::nullopt;
  }
  const auto timeout_count = timeout.count();
  const DWORD wait_ms =
      timeout_count < 0
          ? 0
          : static_cast<DWORD>(
                std::min<std::int64_t>(timeout_count, static_cast<std::int64_t>(INFINITE - 1)));
  const DWORD wait_result = ::WaitForSingleObject(mutex, wait_ms);
  if (wait_result == WAIT_OBJECT_0 || wait_result == WAIT_ABANDONED) {
    return ScopedFileLock(mutex, true);
  }
  ::CloseHandle(mutex);
  return std::nullopt;
#else
  const auto lock_path = detail::LockFilePathFor(path);
  std::error_code ec;
  if (!lock_path.parent_path().empty()) {
    std::filesystem::create_directories(lock_path.parent_path(), ec);
    if (ec) return std::nullopt;
  }
  const int fd = ::open(lock_path.c_str(), O_CREAT | O_RDWR, 0600);
  if (fd < 0) {
    return std::nullopt;
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (true) {
    if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
      return ScopedFileLock(fd, true);
    }
    if (errno != EWOULDBLOCK && errno != EAGAIN) {
      (void)::close(fd);
      return std::nullopt;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      (void)::close(fd);
      return std::nullopt;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
#endif
}

}  // namespace azookey::learning
