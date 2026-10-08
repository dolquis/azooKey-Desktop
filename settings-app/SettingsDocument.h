#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

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

struct EditableSettings {
  bool model_enabled{true};
  std::optional<std::string> model_backend_preference{std::string("auto")};
  std::string hidden_backend_preference;
  std::string model_selected_path;
  std::string openai_api_key;
  bool openai_api_key_changed{false};
  bool openai_api_key_unavailable{false};
  std::string log_level{"info"};
  std::string crash_report_consent{"off"};
  // Unset keeps whatever dictionary object is on disk.
  std::optional<DictionarySettings> dictionary;
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
