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
#include "azookey/core/CustomRomajiLoader.h"
#include "azookey/core/PrivacyPolicy.h"
#include "azookey/tsf/LivePunctuation.h"
#include "azookey/tsf/OpenCloseKey.h"

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
  core::PrivacyPolicy privacy_policy{false, false, true};
  bool prediction_allowed{true};
  // Only an explicit boolean false disables automatic secure detection.
  bool auto_secure_input{true};
  // privacy.showSecureIndicator. Only a display switch, so an absent, malformed
  // or unreadable value keeps the indicator on instead of failing closed.
  bool show_secure_indicator{true};
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
  bool LiveConversionSnapshot() const;
  // punctuationConversion == "commit" (DEV-1638). Absent or malformed: off.
  bool PunctuationConversionSnapshot() const;
  // convertKeysAction == "imeOnOff" (DEV-1736). Absent or any other value: Default.
  ConvertKeysAction ConvertKeysActionSnapshot() const;
  // dynamicPunctuation* (M59). Defaults when absent or malformed: off, "ja", onPause, 400 ms.
  TipDynamicPunctuationSettings DynamicPunctuationSnapshot() const;
  uint32_t MaxContextLengthSnapshot() const;
  bool PredictionEnabledSnapshot() const;
  // inlineEnglishCandidates (M60). An absent or unreadable value is false.
  bool InlineEnglishCandidatesSnapshot() const;
  // mixedEnglishAutoConversion (M63). An absent or unreadable value is false.
  bool MixedEnglishAutoConversionSnapshot() const;
  std::shared_ptr<const core::CustomRomajiTable> RomajiSnapshot() const;

#ifdef AZOOKEY_TSF_TESTING
  void SetForTest(const core::BracketSettings& settings);
  void SetPrivacyForTest(std::string_view contents);
  void SetLiveConversionForTest(bool enabled);
  void SetPunctuationConversionForTest(bool enabled);
  void SetConvertKeysActionForTest(ConvertKeysAction action);
  void SetDynamicPunctuationForTest(const TipDynamicPunctuationSettings& settings);
  void SetMaxContextLengthForTest(uint32_t length);
  void SetPredictionEnabledForTest(bool enabled);
  void SetInlineEnglishCandidatesForTest(bool enabled);
  void SetMixedEnglishAutoConversionForTest(bool enabled);
  void SetRomajiTableForTest(std::shared_ptr<const core::CustomRomajiTable> table);
  bool WaitForEnabledForTest(bool enabled);
  bool WaitForPrivacyForTest(const std::function<bool(const core::PrivacyPolicy&)>& predicate);
  bool WaitForRewritersForTest(const std::function<bool(const TipRewriterSettings&)>& predicate);
  bool WaitForSnapshotForTest(const std::function<bool(const core::BracketSettings&)>& predicate);
  std::array<std::filesystem::path, 2> WatchDirectoriesForTest() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return watch_directories_;
  }
  unsigned WatchNotificationsForTest() const { return watch_notifications_.load(); }
  // Settings-file loads so far, the activation load included.
  unsigned ReloadsForTest() const { return reloads_.load(); }
  // Makes the next `count` directory-watch arms fail, standing in for the
  // transient CreateFileW/ReadDirectoryChangesW failures a TIP cannot provoke.
  static void RefuseWatchArmsForTest(unsigned count);
  // Process-wide count of watch notifications that named an entry below a
  // watched directory (a recursive watch would produce them). Never reset: compare
  // a value taken earlier in the test. Unlike
  // WatchNotificationsForTest it ignores the entries directly in the directory,
  // which NTFS may report late for writes made well before the watch armed.
  static unsigned NestedWatchNotificationsForTest();
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
  bool live_conversion_{false};
  bool punctuation_conversion_{false};
  ConvertKeysAction convert_keys_action_{ConvertKeysAction::Default};
  TipDynamicPunctuationSettings dynamic_punctuation_;
  uint32_t max_context_length_{30};
  bool prediction_enabled_{true};
  bool inline_english_candidates_{false};
  bool mixed_english_auto_conversion_{false};
  std::shared_ptr<const core::CustomRomajiTable> romaji_table_;
  std::filesystem::path path_;
  std::filesystem::path table_path_;
  std::filesystem::path romaji_path_;
  std::atomic<bool> watch_started_{false};
  HANDLE stop_{nullptr};
  HANDLE ready_{nullptr};
  std::thread worker_;
#ifdef AZOOKEY_TSF_TESTING
  std::array<std::filesystem::path, 2> watch_directories_;
  std::atomic<unsigned> watch_notifications_{0};
  std::atomic<unsigned> reloads_{0};
#endif
};

}  // namespace azookey::tsf
