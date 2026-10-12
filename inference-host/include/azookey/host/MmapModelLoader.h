#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

// M25 mmap model loading (docs/copilot-pc-backend-spec.md section 5). llama.cpp
// maps the GGUF itself (load_mode); this maps the file first so a file that
// cannot be mapped goes straight to a read load.
namespace azookey::host {

class MmapModelLoader {
 public:
  MmapModelLoader() = default;
  ~MmapModelLoader() { Unload(); }
  MmapModelLoader(const MmapModelLoader&) = delete;
  MmapModelLoader& operator=(const MmapModelLoader&) = delete;

  // Maps the whole file read-only. An empty file cannot be mapped and fails.
  bool Load(const std::filesystem::path& path);
  void Unload();

  const std::byte* Data() const { return data_; }
  std::size_t Size() const { return size_; }
  const std::string& error() const { return error_; }

 private:
#ifdef _WIN32
  void* file_{nullptr};
  void* mapping_{nullptr};
#else
  int fd_{-1};
#endif
  std::byte* data_{nullptr};
  std::size_t size_{0};
  std::string error_;
};

// Maps and unmaps the file; false when the mapping cannot be made.
bool PreflightModelMapping(const std::filesystem::path& path);

enum class MmapFallbackReason { Preflight, Load };

std::string_view MmapFallbackReasonName(MmapFallbackReason reason);

struct MmapLoadReport {
  bool used_mmap{false};
  // Set when the load went without mmap although mmap was wanted.
  std::optional<MmapFallbackReason> fallback;
};

// Runs `attempt(use_mmap)` with mmap first and, when that fails, once more
// without it. A failed preflight skips the mmap attempt. The result's `ok`
// decides; the reason for a failed mmap attempt is not inspected, since
// llama.cpp does not report it apart from other load failures.
template <typename Attempt>
auto LoadWithMmapFallback(bool want_mmap, bool preflight_ok, Attempt&& attempt,
                          MmapLoadReport* report = nullptr) {
  MmapLoadReport local;
  if (!want_mmap) {
    auto result = attempt(false);
    if (report) *report = local;
    return result;
  }
  if (!preflight_ok) {
    local.fallback = MmapFallbackReason::Preflight;
    auto result = attempt(false);
    if (report) *report = local;
    return result;
  }
  auto result = attempt(true);
  if (result.ok) {
    local.used_mmap = true;
    if (report) *report = local;
    return result;
  }
  local.fallback = MmapFallbackReason::Load;
  auto retried = attempt(false);
  if (report) *report = local;
  return retried;
}

}  // namespace azookey::host
