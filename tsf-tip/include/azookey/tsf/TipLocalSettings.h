#pragma once

#include <Windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>

#include "azookey/core/BracketSettings.h"

namespace azookey::tsf {

// Owns a cancellable directory watch. The worker never touches TSF/COM objects.
class TipLocalSettings final {
 public:
  ~TipLocalSettings();
  bool Start(const std::filesystem::path& settings_path) noexcept;
  void Stop() noexcept;
  core::BracketSettings Snapshot() const;

#ifdef AZOOKEY_TSF_TESTING
  void SetForTest(const core::BracketSettings& settings);
  bool WaitForEnabledForTest(bool enabled);
  bool WaitForSnapshotForTest(const std::function<bool(const core::BracketSettings&)>& predicate);
#endif

 private:
  void Reload() noexcept;
  void Watch() noexcept;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  core::BracketSettings settings_;
  std::filesystem::path path_;
  std::filesystem::path table_path_;
  std::atomic<bool> watch_started_{false};
  HANDLE stop_{nullptr};
  HANDLE ready_{nullptr};
  std::thread worker_;
};

}  // namespace azookey::tsf
