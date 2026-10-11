#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace azookey::host::test {

// Own only an exclusively created directory; declare this before its stores.
class ScopedTempDirectory {
 public:
  ScopedTempDirectory() {
    const auto base = std::filesystem::absolute(std::filesystem::temp_directory_path());
#ifdef _WIN32
    const auto process_id = _getpid();
#else
    const auto process_id = getpid();
#endif
    for (int attempt = 0; attempt < 16; ++attempt) {
      const auto candidate =
          base / ("azookey_host_fixture_" + std::to_string(process_id) + "_" +
                  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                  "_" + std::to_string(next_id_.fetch_add(1, std::memory_order_relaxed)));
      if (std::filesystem::create_directory(candidate)) {
        path_ = candidate;
        return;
      }
    }
    throw std::runtime_error("Could not create a unique host test directory");
  }

  ~ScopedTempDirectory() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  ScopedTempDirectory(const ScopedTempDirectory&) = delete;
  ScopedTempDirectory& operator=(const ScopedTempDirectory&) = delete;

  const std::filesystem::path& path() const { return path_; }
  std::string File(const char* name) const { return (path_ / name).string(); }

 private:
  std::filesystem::path path_;
  static inline std::atomic<uint64_t> next_id_{0};
};

}  // namespace azookey::host::test
