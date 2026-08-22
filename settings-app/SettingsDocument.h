#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace azookey::settings {

struct EditableSettings {
  bool model_enabled{true};
  std::optional<std::string> model_backend_preference{std::string("auto")};
  std::string hidden_backend_preference;
  std::string model_selected_path;
  std::string log_level{"info"};
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
};

std::optional<std::filesystem::path> DefaultSettingsPath();

SettingsDocumentResult LoadSettingsDocument(
    const std::filesystem::path& path,
    std::chrono::milliseconds lock_timeout = std::chrono::milliseconds(5000));

SettingsSaveResult SaveSettingsDocument(
    const std::filesystem::path& path, const EditableSettings& settings,
    std::chrono::milliseconds lock_timeout = std::chrono::milliseconds(5000));

}  // namespace azookey::settings
