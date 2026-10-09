#pragma once

#include <chrono>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "AppProfiles.h"
#include "SettingsFields.h"

namespace azookey::settings {

// The dictionary.* layer switches; defaults match settings/mvp-settings.schema.json.
struct DictionarySettings {
  bool sudachi_enabled{true};
  bool neologd_enabled{false};
  bool named_entity_enabled{true};
  bool technical_terms_enabled{true};
  bool user_dictionary_enabled{true};
  bool auto_words_enabled{true};
  bool app_specific_dictionary_enabled{true};
};

// One entry of model.benchmarkHistory, which the Host writes and the settings app only shows
// (sideload-packaging-spec section 3.6). The Host has not fixed the entry's shape yet, so the
// keys of a BenchmarkModel response are read, plus a model name or path; others are ignored.
struct BenchmarkHistoryEntry {
  std::string model;
  std::string backend;
  std::string status;
  std::optional<double> p50_ms;
  std::optional<double> p95_ms;
  std::optional<double> p99_ms;
};

struct EditableSettings {
  bool model_enabled{true};
  std::optional<std::string> model_backend_preference{std::string("auto")};
  std::string hidden_backend_preference;
  std::string model_selected_path;
  // Shown by the "モデル" pane; never written by a save.
  std::vector<BenchmarkHistoryEntry> benchmark_history;
  std::string openai_api_key;
  bool openai_api_key_changed{false};
  bool openai_api_key_unavailable{false};
  std::string log_level{"info"};
  std::string crash_report_consent{"off"};
  // Unset keeps whatever dictionary object is on disk.
  std::optional<DictionarySettings> dictionary;
  // The switches the UI started from; when set, only switches that differ from it are written.
  std::optional<DictionarySettings> dictionary_loaded;
  // profilesByApp as edited in the "アプリ別" pane (app-profile-spec section 8). Unset keeps the
  // object on disk. The whole object is written, and only when it differs from
  // `profiles_by_app_loaded`, so a save that did not touch profiles leaves them alone.
  std::optional<AppProfiles> profiles_by_app;
  std::optional<AppProfiles> profiles_by_app_loaded;
  // promptPrefixByApp, shown read-only; a save never changes it (app-profile-spec section 6).
  std::map<std::string, std::string> legacy_prompt_prefixes;
  // Fields of the generic panes (GenericSettingFields), keyed by path. Only valid stored values
  // are loaded; an absent path keeps what is on disk when saved.
  SettingValues values;
  // safeMode is written by the Host; the user can only clear it (section 3.6).
  bool safe_mode_enabled{false};
  std::string safe_mode_entered_at;
  int64_t safe_mode_last_crash_count{0};
  bool clear_safe_mode{false};
};

enum class SettingsDocumentStatus {
  Loaded,
  Missing,
  Invalid,
  ReadError,
  LockUnavailable,
};

struct SettingsDocumentResult {
  EditableSettings settings;
  SettingsDocumentStatus status{SettingsDocumentStatus::Missing};
  std::optional<std::string> error;
  std::vector<std::string> warnings;
};

struct SettingsSaveResult {
  bool ok{false};
  std::optional<std::string> error;
  std::vector<std::string> warnings;
  std::optional<std::filesystem::path> quarantined_path;
  // The generic field whose value was rejected, when that is why nothing was written.
  std::optional<std::string> invalid_setting;
  // A requested SafeMode clear was skipped because the Host entered SafeMode again.
  bool safe_mode_reentered{false};
};

std::optional<std::filesystem::path> DefaultSettingsPath();

SettingsDocumentResult LoadSettingsDocument(
    const std::filesystem::path& path,
    std::chrono::milliseconds lock_timeout = std::chrono::milliseconds(5000));

SettingsSaveResult SaveSettingsDocument(
    const std::filesystem::path& path, const EditableSettings& settings,
    std::chrono::milliseconds lock_timeout = std::chrono::milliseconds(5000));

// Keys whose change between two saved states only takes effect after the Host restarts
// (sideload-packaging-spec section 3.6). Turning dictionary.neologdEnabled on is the one
// such change: the Host fetches the pack only at startup.
std::vector<std::string> SettingsRequiringHostRestart(const EditableSettings& before,
                                                      const EditableSettings& after);

}  // namespace azookey::settings
