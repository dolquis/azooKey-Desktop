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

#include "azookey/core/AiPrivacy.h"
#include "azookey/core/BracketSettings.h"
#include "azookey/core/PrivacyPolicy.h"

namespace azookey::tsf {

struct TipRewriterSettings {
  bool symbol{false};
  bool emoji{false};
  bool trigger{true};
  uint32_t maximum{12};
  uint32_t minimum{1};
};
struct TipAiSettings {
  core::AiPrivacy privacy;
  core::PrivacyPolicy privacy_policy{false, false};
  std::string backend{"none"};
  int timeout_ms{30000};
};

// Owns a cancellable directory watch. The worker never touches TSF/COM objects.
class TipLocalSettings final {
 public:
  ~TipLocalSettings();
  // Runs on the watcher thread after every reload. Set it before Start and
  // keep it free of TSF/COM work; it exists so an already-connected TIP can
  // refresh the options it only receives in the Host handshake (DEV-1143).
  void SetOnChanged(std::function<void()> callback) { changed_callback_ = std::move(callback); }
  bool Start(const std::filesystem::path& settings_path) noexcept;
  void Stop() noexcept;
  core::BracketSettings Snapshot() const;
  std::optional<TipRewriterSettings> RewriterSnapshot() const;
  TipAiSettings AiSnapshot() const;

#ifdef AZOOKEY_TSF_TESTING
  void SetForTest(const core::BracketSettings& settings);
  void SetPrivacyForTest(std::string_view contents);
  bool WaitForEnabledForTest(bool enabled);
  bool WaitForPrivacyForTest(const std::function<bool(const core::PrivacyPolicy&)>& predicate);
  bool WaitForRewritersForTest(const std::function<bool(const TipRewriterSettings&)>& predicate);
  bool WaitForSnapshotForTest(const std::function<bool(const core::BracketSettings&)>& predicate);
  std::array<std::filesystem::path, 2> WatchDirectoriesForTest() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return watch_directories_;
  }
  unsigned WatchNotificationsForTest() const { return watch_notifications_.load(); }
  // Makes the next `count` directory-watch arms fail, standing in for the
  // transient CreateFileW/ReadDirectoryChangesW failures a TIP cannot provoke.
  static void RefuseWatchArmsForTest(unsigned count);
#endif

 private:
  void Reload() noexcept;
  void Watch() noexcept;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::function<void()> changed_callback_;
  // Last bytes read from the settings file; empty until the first load, so the
  // activation load itself is not reported as a change. Touched only by
  // Reload, which runs on the activating thread until the worker starts and on
  // the worker afterwards, never both at once.
  std::optional<std::string> last_contents_;
  core::BracketSettings settings_;
  std::optional<TipRewriterSettings> rewriters_;
  TipAiSettings ai_;
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
