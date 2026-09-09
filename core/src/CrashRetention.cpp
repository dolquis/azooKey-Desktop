#include "azookey/core/CrashRetention.h"

#include <algorithm>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace azookey::core {
namespace {

bool IsManagedName(std::string_view name) {
  constexpr std::string_view host = "azookey-host-";
  constexpr std::string_view settings = "azookey-settings-";
  if (name.starts_with(host))
    name.remove_prefix(host.size());
  else if (name.starts_with(settings))
    name.remove_prefix(settings.size());
  else
    return false;
  // yyyyMMddTHHmmssZ-<decimal pid>.dmp
  if (name.size() < 21 || name[8] != 'T' || name[15] != 'Z' || name[16] != '-' ||
      !name.ends_with(".dmp"))
    return false;
  for (std::size_t i = 0; i < name.size() - 4; ++i) {
    if (i == 8 || i == 15 || i == 16) continue;
    if (name[i] < '0' || name[i] > '9') return false;
  }
  return name.size() > 21;
}

bool IsLink(const std::filesystem::path& path, std::error_code& ec) {
  const auto status = std::filesystem::symlink_status(path, ec);
  if (ec || std::filesystem::is_symlink(status)) return true;
#ifdef _WIN32
  const auto attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    ec = std::error_code(static_cast<int>(GetLastError()), std::system_category());
    return true;
  }
  return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
  return false;
#endif
}

}  // namespace

CrashRetentionResult PruneCrashDumps(const std::filesystem::path& directory,
                                     CrashRetentionLimits limits,
                                     std::filesystem::file_time_type now) noexcept {
  CrashRetentionResult result;
  try {
    std::error_code ec;
    if (!std::filesystem::exists(directory, ec)) {
      result.failed = static_cast<bool>(ec);
      return result;
    }
    if (IsLink(directory, ec)) {
      result.failed = true;
      return result;
    }
    struct Dump {
      std::filesystem::path path;
      std::filesystem::file_time_type time;
      std::uintmax_t size;
    };
    std::vector<Dump> dumps;
    std::filesystem::directory_iterator it(directory, ec), end;
    while (!ec && it != end) {
      const auto path = it->path();
      const auto filename = path.filename().u8string();
      const std::string_view name(reinterpret_cast<const char*>(filename.data()), filename.size());
      if (IsManagedName(name) && !IsLink(path, ec) &&
          std::filesystem::is_regular_file(it->symlink_status(ec))) {
        const auto size = it->file_size(ec);
        if (ec) break;
        const auto time = it->last_write_time(ec);
        if (ec) break;
        dumps.push_back({path, time, size});
      }
      if (!ec) it.increment(ec);
    }
    if (ec) {
      result.failed = true;
      return result;  // Never prune a partially enumerated collection.
    }
    std::sort(dumps.begin(), dumps.end(), [](const Dump& a, const Dump& b) {
      return a.time != b.time ? a.time > b.time : a.path < b.path;
    });
    std::size_t kept = 0;
    std::uintmax_t bytes = 0;
    bool cutoff = false;
    for (const auto& dump : dumps) {
      if (!cutoff && now - dump.time <= limits.age && kept < limits.count &&
          dump.size <= limits.bytes - bytes) {
        ++kept;
        bytes += dump.size;
        continue;
      }
      cutoff = true;
      // Recheck before removal so a replaced entry cannot redirect cleanup.
      if (IsLink(dump.path, ec) ||
          !std::filesystem::is_regular_file(std::filesystem::symlink_status(dump.path, ec))) {
        result.failed = true;
        ec.clear();
        continue;
      }
      if (std::filesystem::remove(dump.path, ec)) ++result.removed;
      if (ec) result.failed = true;
      ec.clear();
    }
  } catch (...) {
    result.failed = true;
  }
  return result;
}

}  // namespace azookey::core
