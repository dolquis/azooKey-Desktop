#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "azookey/core/AiPrivacy.h"
#include "azookey/core/AppProfileResolver.h"
#include "azookey/core/PrivacyPolicy.h"
#include "azookey/core/SecureApps.h"
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

// M36 autoWordRegistration.*; docs/auto-word-registration-spec.md section 8.
// trending_enabled and trending_interval_hours are parsed here so the settings
// round-trip is complete, but nothing consumes them until M36-B lands.
struct RuntimeAutoWordSettings {
  bool mining_enabled{true};
  bool trending_enabled{false};
  std::string registration_mode{"confirm"};
  int32_t mining_min_count{3};
  int32_t trending_interval_hours{24};
};

struct RuntimeSettings {
  std::shared_ptr<const core::AppProfileResolver> app_profiles;
  const core::AppProfileResolver& AppProfiles() const {
    static const auto defaults = core::AppProfileResolver::FromSettings(ipc::json::Value{});
    return app_profiles ? *app_profiles : defaults;
  }
  NllConfig nll;
  std::string input_mode{"hiragana"};
  bool live_conversion{false};
  bool llm_magic_conversion{false};
  std::string log_level{"info"};
  std::string crash_report_consent{"off"};
  // M46 privacy.secureApps / showSecureIndicator, parsed here so the host holds
  // the same values the TIP does. Matching against core::kDefaultSecureApps is
  // the TIP's job: the host does not resolve the foreground app (spec 5.1.1),
  // and the indicator is drawn by the TIP's candidate window.
  std::vector<std::string> secure_apps;
  bool show_secure_indicator{true};
  std::string input_style{"default"};
  std::string custom_romaji_table_path{"%LOCALAPPDATA%\\azooKey\\custom-romaji.tsv"};
  bool prediction_enabled{true};
  std::string ai_backend{"none"};
  core::AiPrivacy ai_privacy{true, true};
  core::PrivacyPolicy privacy_policy{false, false};
  int32_t open_ai_timeout_ms{30000};
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
  int32_t inference_threads{0};
  int32_t max_candidates{9};
  int32_t max_context_length{10};
  bool batch_romaji_conversion{false};
  std::string batch_romaji_preview_style{"kana"};
  std::string batch_conversion_mode{"neural"};
  bool batch_auto_punctuation{false};
  bool number_rewriter{false};
  bool katakana_rewriter{false};
  bool symbol_rewriter{false};
  std::string symbol_data_path;
  bool emoji_rewriter{false};
  bool emoji_trigger_search{true};
  int32_t emoji_max_candidates{12};
  int32_t emoji_trigger_min_query_length{1};
  std::string emoji_data_path;
  // M35 typoCorrectionMode / typoMinCount.
  std::string typo_correction_mode{"suggest"};
  int32_t typo_min_count{3};
  RuntimeAutoWordSettings auto_word;
  RuntimeAutoUpdateSettings auto_update;
};

struct SettingsLoadResult {
  RuntimeSettings settings;
  SettingsLoadStatus status{SettingsLoadStatus::Missing};
  std::optional<std::string> error;
  std::optional<std::filesystem::path> quarantined_path;
  std::vector<std::string> profile_warnings;
};

class SettingsStore {
 public:
  explicit SettingsStore(std::filesystem::path settings_path);
  SettingsStore(std::filesystem::path settings_path, std::chrono::milliseconds file_lock_timeout);

  const std::filesystem::path& path() const { return settings_path_; }
  const RuntimeSettings& settings() const { return settings_; }
  const SettingsLoadResult& last_result() const { return last_result_; }

  struct PrivacyGuard {
    std::unique_lock<std::mutex> lock;
    core::PrivacyPolicy policy;
  };
  // Hold through the learning operation. Reload publishes under this same lock,
  // independently of model loading and the caller's config serialization lock.
  PrivacyGuard LockPrivacyPolicy() const;

  SettingsLoadResult Load();
  SettingsLoadResult Reload();
  // The settings as they stand on disk when the file has been written since
  // the last load, or nullopt when the loaded settings are already current or
  // the file cannot be read. Lets the Handshake reply answer from the file the
  // settings app just wrote, ahead of the UpdateConfig it sends next
  // (DEV-1143), without adopting them as the runtime settings: applying a
  // reload stays the caller-serialised job of UpdateConfig.
  std::optional<RuntimeSettings> SettingsWrittenAfterLoad();

 private:
  static constexpr int64_t kNoWriteTime = (std::numeric_limits<int64_t>::min)();

  SettingsLoadResult LoadImpl(bool preserve_current_on_invalid);
  void PublishPrivacyPolicy();

  std::filesystem::path settings_path_;
  std::chrono::milliseconds file_lock_timeout_;
  // Written by LoadImpl, read without the caller's mutex by
  // SettingsWrittenAfterLoad, which must not block behind a model reload.
  std::atomic<int64_t> loaded_write_time_{kNoWriteTime};
  mutable std::mutex privacy_mutex_;
  core::PrivacyPolicy privacy_policy_{false, false};
  RuntimeSettings settings_;
  SettingsLoadResult last_result_;
  std::mutex peeked_mutex_;
  std::optional<RuntimeSettings> peeked_;
  int64_t peeked_write_time_{kNoWriteTime};
};

enum class PowerSource { Unknown, Ac, Battery };

struct InferenceThreadEnvironment {
  PowerSource power_source{PowerSource::Unknown};
  unsigned int hardware_concurrency{0};
};

// Sampled when runtime settings are applied; model reloads reuse the resolved value.
InferenceThreadEnvironment QueryInferenceThreadEnvironment();
using InferenceThreadEnvironmentProvider = std::function<InferenceThreadEnvironment()>;

EngineConfig ApplyRuntimeSettingsToEngineConfig(EngineConfig config,
                                                const RuntimeSettings& settings);
EngineConfig ApplyRuntimeSettingsToEngineConfig(
    EngineConfig config, const RuntimeSettings& settings, BackendKind auto_backend,
    const InferenceThreadEnvironmentProvider& provider = QueryInferenceThreadEnvironment);

}  // namespace azookey::host
