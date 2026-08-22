#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "azookey/host/InferenceEngine.h"

namespace azookey::host {

enum class SettingsLoadStatus {
  Loaded,
  Missing,
  Invalid,
};

struct RuntimeModelSettings {
  bool enabled{true};
  std::string selected_path;
  std::string backend_preference{"auto"};
  std::string ep_preference{"auto"};
  int32_t n_gpu_layers{-1};
  bool auto_load_on_host_start{true};
  bool fallback_to_simple_converter{true};
  bool benchmark_on_model_change{false};
};

struct RuntimeAutoUpdateSettings {
  bool enabled{true};
  std::string channel{"stable"};
  int32_t check_interval_hours{24};
};

struct RuntimeSettings {
  std::string input_mode{"hiragana"};
  bool live_conversion{false};
  bool llm_magic_conversion{false};
  std::string log_level{"info"};
  std::string input_style{"default"};
  std::string custom_romaji_table_path{"%LOCALAPPDATA%\\azooKey\\custom-romaji.tsv"};
  bool prediction_enabled{true};
  std::string ai_backend{"none"};
  std::string open_ai_api_key;
  std::string open_ai_api_endpoint{"https://api.openai.com/v1"};
  std::string open_ai_model{"gpt-4o-mini"};
  bool include_context_in_ai_transform{true};
  bool context_reselection{false};
  bool post_commit_lint{false};
  bool retroactive_recompute{false};
  bool sentence_completion{false};
  std::string backend_preference{"auto"};
  std::string ep_preference{"auto"};
  RuntimeModelSettings model;
  std::string power_profile{"auto"};
  bool batch_romaji_conversion{false};
  std::string batch_romaji_preview_style{"kana"};
  std::string batch_conversion_mode{"neural"};
  bool batch_auto_punctuation{false};
  RuntimeAutoUpdateSettings auto_update;
};

struct SettingsLoadResult {
  RuntimeSettings settings;
  SettingsLoadStatus status{SettingsLoadStatus::Missing};
  std::optional<std::string> error;
  std::optional<std::filesystem::path> quarantined_path;
};

class SettingsStore {
 public:
  explicit SettingsStore(std::filesystem::path settings_path);
  SettingsStore(std::filesystem::path settings_path, std::chrono::milliseconds file_lock_timeout);

  const std::filesystem::path& path() const { return settings_path_; }
  const RuntimeSettings& settings() const { return settings_; }
  const SettingsLoadResult& last_result() const { return last_result_; }

  SettingsLoadResult Load();
  SettingsLoadResult Reload();

 private:
  SettingsLoadResult LoadImpl(bool preserve_current_on_invalid);

  std::filesystem::path settings_path_;
  std::chrono::milliseconds file_lock_timeout_;
  RuntimeSettings settings_;
  SettingsLoadResult last_result_;
};

EngineConfig ApplyRuntimeSettingsToEngineConfig(EngineConfig config,
                                                const RuntimeSettings& settings);
EngineConfig ApplyRuntimeSettingsToEngineConfig(EngineConfig config,
                                                const RuntimeSettings& settings,
                                                BackendKind auto_backend);

}  // namespace azookey::host
