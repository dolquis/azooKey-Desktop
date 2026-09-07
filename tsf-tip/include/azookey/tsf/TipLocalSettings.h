#pragma once

#include <Windows.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

#include "azookey/core/BracketSettings.h"

namespace azookey::tsf {

struct TipRewriterSettings {
  bool symbol{false};
  bool emoji{false};
  bool trigger{true};
  uint32_t maximum{12};
  uint32_t minimum{1};
};

// Owns a cancellable directory watch. The worker never touches TSF/COM objects.
class TipLocalSettings final {
 public:
  ~TipLocalSettings();
  bool Start(const std::filesystem::path& settings_path) noexcept;
  void Stop() noexcept;
  core::BracketSettings Snapshot() const;
  std::optional<TipRewriterSettings> RewriterSnapshot() const;

#ifdef AZOOKEY_TSF_TESTING
  void SetForTest(const core::BracketSettings& settings);
  bool WaitForEnabledForTest(bool enabled);
  bool WaitForRewritersForTest(const std::function<bool(const TipRewriterSettings&)>& predicate);
  bool WaitForSnapshotForTest(const std::function<bool(const core::BracketSettings&)>& predicate);
  std::array<std::filesystem::path, 2> WatchDirectoriesForTest() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return watch_directories_;
  }
  unsigned WatchNotificationsForTest() const { return watch_notifications_.load(); }
#endif

 private:
  void Reload() noexcept;
  void Watch() noexcept;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  core::BracketSettings settings_;
  std::optional<TipRewriterSettings> rewriters_;
  std::filesystem::path path_;
  std::filesystem::path table_path_;
  std::atomic<bool> watch_started_{false};
  HANDLE stop_{nullptr};
  HANDLE ready_{nullptr};
  std::thread worker_;
#ifdef AZOOKEY_TSF_TESTING
  std::array<std::filesystem::path, 2> watch_directories_;
  std::atomic<unsigned> watch_notifications_{0};
#endif
};

}  // namespace azookey::tsf
