#include "SettingsDocument.h"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string_view>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

#include "azookey/ipc/Json.h"
#include "azookey/learning/AtomicFile.h"
#include "azookey/learning/FileLock.h"

namespace azookey::settings {
namespace {

namespace j = azookey::ipc::json;

bool IsStringEnum(const j::Value& value, std::initializer_list<std::string_view> allowed) {
  if (!value.IsString()) return false;
  for (const auto candidate : allowed) {
    if (value.AsString() == candidate) return true;
  }
  return false;
}

bool IsInteger(const j::Value& value, std::optional<double> minimum = std::nullopt) {
  if (!value.IsNumber() || !std::isfinite(value.AsNumber()) ||
      std::floor(value.AsNumber()) != value.AsNumber()) {
    return false;
  }
  return !minimum || value.AsNumber() >= *minimum;
}

j::Object SanitizeModel(const j::Object& input, std::vector<std::string>* warnings) {
  j::Object output;
  for (const auto& [key, value] : input) {
    bool valid = false;
    if (key == "enabled" || key == "autoLoadOnHostStart" || key == "fallbackToSimpleConverter" ||
        key == "benchmarkOnModelChange") {
      valid = value.IsBool();
    } else if (key == "selectedPath") {
      valid = value.IsString();
    } else if (key == "backendPreference") {
      valid = IsStringEnum(value, {"auto", "cpu", "cuda", "vulkan", "winml", "directml", "npu"});
    } else if (key == "epPreference") {
      valid = IsStringEnum(value, {"auto", "npu", "gpu", "cpu"});
    } else if (key == "nGpuLayers") {
      valid = IsInteger(value);
    } else if (key == "benchmarkHistory") {
      valid = value.IsArray();
    }

    if (valid) {
      output.emplace(key, value);
    } else {
      warnings->push_back("model." + key + " was removed because it is unknown or invalid");
    }
  }
  return output;
}

j::Object SanitizeAutoUpdate(const j::Object& input, std::vector<std::string>* warnings) {
  j::Object output;
  for (const auto& [key, value] : input) {
    bool valid = false;
    if (key == "enabled") {
      valid = value.IsBool();
    } else if (key == "channel") {
      valid = IsStringEnum(value, {"stable", "beta"});
    } else if (key == "checkIntervalHours") {
      valid = IsInteger(value, 1.0);
    }
    if (valid) {
      output.emplace(key, value);
    } else {
      warnings->push_back("autoUpdate." + key + " was removed because it is unknown or invalid");
    }
  }
  return output;
}

j::Object SanitizeStringMap(const j::Object& input, std::vector<std::string>* warnings) {
  j::Object output;
  for (const auto& [key, value] : input) {
    if (value.IsString()) {
      output.emplace(key, value);
    } else {
      warnings->push_back("promptPrefixByApp." + key + " was removed because it is invalid");
    }
  }
  return output;
}

j::Object SanitizeRoot(const j::Object& input, std::vector<std::string>* warnings) {
  j::Object output;
  for (const auto& [key, value] : input) {
    bool valid = false;
    if (key == "inputMode") {
      valid = IsStringEnum(value, {"hiragana", "alnum_half", "alnum_full"});
    } else if (key == "liveConversion" || key == "llmMagicConversion" ||
               key == "predictionEnabled" || key == "includeContextInAITransform" ||
               key == "contextReselection" || key == "postCommitLint" ||
               key == "retroactiveRecompute" || key == "sentenceCompletion" ||
               key == "batchRomajiConversion" || key == "batchAutoPunctuation" ||
               key == "numberRewriter") {
      valid = value.IsBool();
    } else if (key == "logLevel") {
      valid = IsStringEnum(value, {"error", "warn", "info", "debug"});
    } else if (key == "inputStyle") {
      valid = IsStringEnum(value, {"default", "custom"});
    } else if (key == "customRomajiTablePath" || key == "openAiApiKey" ||
               key == "openAiApiEndpoint" || key == "openAiModel") {
      valid = value.IsString();
    } else if (key == "aiBackend") {
      valid = IsStringEnum(value, {"none", "openai", "local-zenzai"});
    } else if (key == "backendPreference") {
      valid = IsStringEnum(value, {"auto", "cpu", "cuda", "vulkan", "winml", "directml", "npu"});
    } else if (key == "epPreference") {
      valid = IsStringEnum(value, {"auto", "npu", "gpu", "cpu"});
    } else if (key == "powerProfile") {
      valid = IsStringEnum(value, {"auto", "performance", "battery_saver"});
    } else if (key == "batchRomajiPreviewStyle") {
      valid = IsStringEnum(value, {"kana", "romaji"});
    } else if (key == "batchConversionMode") {
      valid = IsStringEnum(value, {"neural", "ai-cleanup"});
    } else if (key == "model" && value.IsObject()) {
      output.emplace(key, j::Value(SanitizeModel(value.AsObject(), warnings)));
      continue;
    } else if (key == "autoUpdate" && value.IsObject()) {
      output.emplace(key, j::Value(SanitizeAutoUpdate(value.AsObject(), warnings)));
      continue;
    } else if (key == "promptPrefixByApp" && value.IsObject()) {
      output.emplace(key, j::Value(SanitizeStringMap(value.AsObject(), warnings)));
      continue;
    }

    if (valid) {
      output.emplace(key, value);
    } else {
      warnings->push_back(key + " was removed because it is unknown or invalid");
    }
  }
  return output;
}

std::optional<std::string> ReadTextFile(const std::filesystem::path& path, std::string* error) {
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    if (error) *error = "failed to open settings.json";
    return std::nullopt;
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  if (input.bad()) {
    if (error) *error = "failed to read settings.json";
    return std::nullopt;
  }
  return buffer.str();
}

std::optional<std::filesystem::path> QuarantineInvalidFile(const std::filesystem::path& path,
                                                           std::optional<std::string>* error) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) return std::nullopt;

  for (int i = 0; i < 100; ++i) {
    auto candidate = path;
    candidate += (i == 0) ? ".invalid" : ".invalid." + std::to_string(i);
    if (std::filesystem::exists(candidate, ec)) continue;

    std::filesystem::rename(path, candidate, ec);
    if (!ec) return candidate;
    if (error) *error = "failed to quarantine invalid settings.json: " + ec.message();
    return std::nullopt;
  }
  if (error) *error = "failed to quarantine invalid settings.json: no available suffix";
  return std::nullopt;
}

j::Object ReadAndSanitize(const std::filesystem::path& path, SettingsDocumentStatus* status,
                          std::optional<std::string>* error, std::vector<std::string>* warnings) {
  std::error_code ec;
  const bool exists = std::filesystem::exists(path, ec);
  if (ec) {
    *status = SettingsDocumentStatus::ReadError;
    *error = "failed to inspect settings.json: " + ec.message();
    return {};
  }
  if (!exists) {
    *status = SettingsDocumentStatus::Missing;
    return {};
  }
  std::string read_error;
  const auto content = ReadTextFile(path, &read_error);
  if (!content) {
    *status = SettingsDocumentStatus::ReadError;
    *error = std::move(read_error);
    return {};
  }
  const auto parsed = j::Parse(*content);
  if (!parsed || !parsed->IsObject()) {
    *status = SettingsDocumentStatus::Invalid;
    *error = "invalid settings.json";
    return {};
  }
  *status = SettingsDocumentStatus::Loaded;
  return SanitizeRoot(parsed->AsObject(), warnings);
}

EditableSettings ExtractEditableSettings(const j::Object& root) {
  EditableSettings settings;
  if (const auto it = root.find("logLevel"); it != root.end() && it->second.IsString()) {
    settings.log_level = it->second.AsString();
  }
  const auto model_it = root.find("model");
  if (model_it == root.end() || !model_it->second.IsObject()) return settings;
  const auto& model = model_it->second.AsObject();
  if (const auto it = model.find("enabled"); it != model.end() && it->second.IsBool()) {
    settings.model_enabled = it->second.AsBool();
  }
  if (const auto it = model.find("selectedPath"); it != model.end() && it->second.IsString()) {
    settings.model_selected_path = it->second.AsString();
  }
  if (const auto it = model.find("backendPreference"); it != model.end() && it->second.IsString()) {
    const auto& value = it->second.AsString();
    if (value == "auto" || value == "cpu" || value == "cuda") {
      settings.model_backend_preference = value;
    } else {
      settings.model_backend_preference.reset();
      settings.hidden_backend_preference = value;
    }
  }
  return settings;
}

}  // namespace

std::optional<std::filesystem::path> DefaultSettingsPath() {
#ifdef _WIN32
  const DWORD size = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
  if (size == 0) return std::nullopt;
  std::wstring value(size, L'\0');
  const DWORD written = GetEnvironmentVariableW(L"LOCALAPPDATA", value.data(), size);
  if (written == 0 || written >= size) return std::nullopt;
  value.resize(written);
  return std::filesystem::path(value) / L"azooKey" / L"config" / L"settings.json";
#else
  const char* value = std::getenv("LOCALAPPDATA");
  if (!value || *value == '\0') return std::nullopt;
  return std::filesystem::path(value) / "azooKey" / "config" / "settings.json";
#endif
}

SettingsDocumentResult LoadSettingsDocument(const std::filesystem::path& path,
                                            std::chrono::milliseconds lock_timeout) {
  SettingsDocumentResult result;
  auto lock = azookey::learning::AcquireExclusiveFileLockForPath(path, lock_timeout);
  if (!lock) {
    result.status = SettingsDocumentStatus::LockUnavailable;
    result.error = "settings.json is busy; try again";
    return result;
  }
  const auto root = ReadAndSanitize(path, &result.status, &result.error, &result.warnings);
  result.settings = ExtractEditableSettings(root);
  return result;
}

SettingsSaveResult SaveSettingsDocument(const std::filesystem::path& path,
                                        const EditableSettings& settings,
                                        std::chrono::milliseconds lock_timeout) {
  SettingsSaveResult result;
  if (settings.model_backend_preference && *settings.model_backend_preference != "auto" &&
      *settings.model_backend_preference != "cpu" && *settings.model_backend_preference != "cuda") {
    result.error = "invalid model backend preference";
    return result;
  }
  if (settings.log_level != "error" && settings.log_level != "warn" &&
      settings.log_level != "info" && settings.log_level != "debug") {
    result.error = "invalid log level";
    return result;
  }
  auto lock = azookey::learning::AcquireExclusiveFileLockForPath(path, lock_timeout);
  if (!lock) {
    result.error = "settings.json is busy; no changes were written";
    return result;
  }

  SettingsDocumentStatus read_status = SettingsDocumentStatus::Missing;
  std::optional<std::string> read_error;
  auto root = ReadAndSanitize(path, &read_status, &read_error, &result.warnings);
  if (read_status == SettingsDocumentStatus::ReadError) {
    result.error = read_error.value_or("failed to read settings.json");
    return result;
  }
  if (read_status == SettingsDocumentStatus::Invalid) {
    result.quarantined_path = QuarantineInvalidFile(path, &result.error);
    if (!result.quarantined_path) return result;
    result.warnings.push_back("invalid settings.json was quarantined before save");
  }

  j::Object model;
  if (const auto it = root.find("model"); it != root.end() && it->second.IsObject()) {
    model = it->second.AsObject();
  }
  model["enabled"] = j::Value(settings.model_enabled);
  model["selectedPath"] = j::Value(settings.model_selected_path);
  if (settings.model_backend_preference) {
    model["backendPreference"] = j::Value(*settings.model_backend_preference);
  }
  root["model"] = j::Value(std::move(model));
  root["logLevel"] = j::Value(settings.log_level);
  root.erase("backendPreference");

  std::string serialized = j::Stringify(j::Value(std::move(root)));
  serialized.push_back('\n');
  if (!azookey::learning::WriteTextFileAtomically(path, serialized)) {
    result.error = "failed to atomically replace settings.json";
    return result;
  }
  result.ok = true;
  return result;
}

}  // namespace azookey::settings
