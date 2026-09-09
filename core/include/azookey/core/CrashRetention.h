#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace azookey::core {

struct CrashRetentionLimits {
  std::size_t count{5};
  std::uintmax_t bytes{50 * 1024 * 1024};
  std::chrono::hours age{24 * 30};
};

struct CrashRetentionResult {
  std::size_t removed{};
  bool failed{};
};

// Only regular azookey-{host,settings}-<UTC>-<pid>.dmp files are managed.
// Never follows symlinks, recurses, or creates the directory.
CrashRetentionResult PruneCrashDumps(
    const std::filesystem::path& directory, CrashRetentionLimits limits = {},
    std::filesystem::file_time_type now = std::filesystem::file_time_type::clock::now()) noexcept;

}  // namespace azookey::core
