#include "azookey/host/SettingsStore.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>

#include "azookey/core/PlatformPaths.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "azookey/ipc/Json.h"
#include "azookey/learning/AtomicFile.h"
#include "azookey/learning/FileLock.h"

namespace azookey::host {

namespace {

namespace j = ::azookey::ipc::json;

bool IsOneOf(const std::string& value, std::initializer_list<const char*> allowed) {
  for (const char* item : allowed) {
    if (value == item) return true;
  }
  return false;
}

std::string ReadString(const j::Object& object, const char* key, const std::string& fallback) {
  auto it = object.find(key);
  if (it == object.end() || !it->second.IsString()) return fallback;
  return it->second.AsString();
}

std::string ReadEnum(const j::Object& object, const char* key, const std::string& fallback,
                     std::initializer_list<const char*> allowed) {
  const auto value = ReadString(object, key, fallback);
  return IsOneOf(value, allowed) ? value : fallback;
}

bool ReadBool(const j::Object& object, const char* key, bool fallback) {
  auto it = object.find(key);
  if (it == object.end() || !it->second.IsBool()) return fallback;
  return it->second.AsBool();
}

int32_t ReadInt32(const j::Object& object, const char* key, int32_t fallback) {
  auto it = object.find(key);
  if (it == object.end() || !it->second.IsNumber()) return fallback;
  const double value = it->second.AsNumber();
  if (!std::isfinite(value) || std::floor(value) != value ||
      value < static_cast<double>(std::numeric_limits<int32_t>::min()) ||
      value > static_cast<double>(std::numeric_limits<int32_t>::max())) {
    return fallback;
  }
  return static_cast<int32_t>(value);
}

int32_t ReadRangedInt32(const j::Object& object, const char* key, int32_t fallback, int32_t minimum,
                        int32_t maximum) {
  const int32_t value = ReadInt32(object, key, fallback);
  return value < minimum || value > maximum ? fallback : value;
}

int32_t ReadClampedInt32(const j::Object& object, const char* key, int32_t fallback,
                         int32_t minimum, int32_t maximum) {
  const auto item = object.find(key);
  if (item == object.end() || !item->second.IsNumber()) return fallback;
  const double value = item->second.AsNumber();
  if (!std::isfinite(value) || std::floor(value) != value) return fallback;
  return static_cast<int32_t>(
      std::clamp(value, static_cast<double>(minimum), static_cast<double>(maximum)));
}

const j::Object* ReadObject(const j::Object& object, const char* key) {
  auto it = object.find(key);
  if (it == object.end() || !it->second.IsObject()) return nullptr;
  return &it->second.AsObject();
}

std::optional<std::string> ReadFile(const std::filesystem::path& path,
                                    std::optional<std::string>* error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  if (in.bad()) {
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

// Section 8.5.3: SafeMode stops AI, learning and external APIs whatever the
// rest of the file asks for. Overriding the parsed values, rather than checking
// the flag at each use, keeps every reader of RuntimeSettings on the same stop.
void ApplySafeModeOverrides(RuntimeSettings& settings) {
  settings.model.enabled = false;
  settings.model.auto_load_on_host_start = false;
  settings.nll.enabled = false;
  settings.llm_magic_conversion = false;
  settings.ai_backend = "none";
  settings.batch_conversion_mode = "neural";
  settings.ai_privacy = core::AiPrivacy{false, false};
  // Secure is what the Dispatcher checks before any learning write.
  settings.privacy_policy = core::PrivacyPolicy{true, false};
  settings.typo_correction_mode = "off";
  settings.auto_word.mining_enabled = false;
  settings.auto_word.trending_enabled = false;
}

RuntimeSettings ParseRuntimeSettings(const j::Object& object) {
  RuntimeSettings settings;
  if (const auto* reranker = ReadObject(object, "reranker")) {
    settings.nll.enabled = ReadBool(*reranker, "nllRerankEnabled", settings.nll.enabled);
    settings.nll.top_k = ReadClampedInt32(*reranker, "nllTopK", settings.nll.top_k, 1, 16);
    settings.nll.budget_ms =
        ReadClampedInt32(*reranker, "nllBudgetMs", settings.nll.budget_ms, 5, 60);
    settings.nll.failure_threshold =
        ReadClampedInt32(*reranker, "nllFailureThreshold", settings.nll.failure_threshold, 1, 10);
    const auto weight = reranker->find("nllWeight");
    if (weight != reranker->end() && weight->second.IsNumber())
      settings.nll.weight = weight->second.AsNumber();
    settings.nll = ClampNllConfig(settings.nll);
  }
  settings.input_mode =
      ReadEnum(object, "inputMode", settings.input_mode, {"hiragana", "alnum_half", "alnum_full"});
  settings.live_conversion = ReadBool(object, "liveConversion", settings.live_conversion);
  settings.dynamic_punctuation = ReadBool(object, "dynamicPunctuation", settings.dynamic_punctuation);
  settings.dynamic_punctuation_style =
      ReadEnum(object, "dynamicPunctuationStyle", settings.dynamic_punctuation_style,
               {"ja", "fullwidth_latin"});
  settings.dynamic_punctuation_stability =
      ReadEnum(object, "dynamicPunctuationStability", settings.dynamic_punctuation_stability,
               {"onPause", "eager"});
  settings.dynamic_punctuation_idle_ms =
      ReadRangedInt32(object, "dynamicPunctuationIdleMs", settings.dynamic_punctuation_idle_ms,
                      1, (std::numeric_limits<int32_t>::max)());
  if (const auto value = object.find("segmentBoundaryConfidence");
      value != object.end() && value->second.IsNumber() &&
      std::isfinite(value->second.AsNumber()) && value->second.AsNumber() >= 0.0 &&
      value->second.AsNumber() <= 1.0) {
    settings.segment_boundary_confidence = value->second.AsNumber();
  }
  settings.punctuation_rules_path =
      ReadString(object, "punctuationRulesPath", settings.punctuation_rules_path);
  settings.llm_magic_conversion =
      ReadBool(object, "llmMagicConversion", settings.llm_magic_conversion);
  settings.log_level =
      ReadEnum(object, "logLevel", settings.log_level, {"error", "warn", "info", "debug"});
  settings.input_style =
      ReadEnum(object, "inputStyle", settings.input_style, {"default", "custom"});
  settings.custom_romaji_table_path =
      ReadString(object, "customRomajiTablePath", settings.custom_romaji_table_path);
  settings.prediction_enabled = ReadBool(object, "predictionEnabled", settings.prediction_enabled);
  settings.ai_backend =
      ReadEnum(object, "aiBackend", settings.ai_backend, {"none", "openai", "local-zenzai"});
  settings.open_ai_api_key = ReadString(object, "openAiApiKey", settings.open_ai_api_key);
  settings.open_ai_api_endpoint =
      ReadString(object, "openAiApiEndpoint", settings.open_ai_api_endpoint);
  settings.open_ai_model = ReadString(object, "openAiModel", settings.open_ai_model);
  settings.ai_privacy = core::ParseAiPrivacy(ipc::json::Value(object));
  settings.privacy_policy = core::ParsePrivacyPolicy(ipc::json::Value(object));
  settings.secure_apps = core::ParseSecureApps(ipc::json::Value(object));
  if (const auto privacy = object.find("privacy");
      privacy != object.end() && privacy->second.IsObject()) {
    settings.crash_report_consent =
        ReadEnum(privacy->second.AsObject(), "crashReportConsent", "off", {"off", "local"});
    settings.show_secure_indicator =
        ReadBool(privacy->second.AsObject(), "showSecureIndicator", settings.show_secure_indicator);
  }
  settings.open_ai_timeout_ms = static_cast<int32_t>(std::clamp<int64_t>(
      ipc::json::Value(object).GetInt("openAiTimeoutMs").value_or(30000), 1000, 120000));
  settings.include_context_in_ai_transform =
      ReadBool(object, "includeContextInAITransform", settings.include_context_in_ai_transform);
  settings.context_reselection =
      ReadBool(object, "contextReselection", settings.context_reselection);
  settings.post_commit_lint = ReadBool(object, "postCommitLint", settings.post_commit_lint);
  settings.retroactive_recompute =
      ReadBool(object, "retroactiveRecompute", settings.retroactive_recompute);
  settings.sentence_completion =
      ReadBool(object, "sentenceCompletion", settings.sentence_completion);
  settings.backend_preference =
      ReadEnum(object, "backendPreference", settings.backend_preference,
               {"auto", "cpu", "cuda", "vulkan", "winml", "directml", "npu"});
  settings.ep_preference =
      ReadEnum(object, "epPreference", settings.ep_preference, {"auto", "npu", "gpu", "cpu"});
  settings.power_profile = ReadEnum(object, "powerProfile", settings.power_profile,
                                    {"auto", "performance", "battery_saver"});
  settings.inference_threads =
      ReadRangedInt32(object, "inferenceThreads", settings.inference_threads, 0, 8);
  settings.max_candidates =
      ReadRangedInt32(object, "maxCandidates", settings.max_candidates, 1, 32);
  settings.max_context_length =
      ReadRangedInt32(object, "maxContextLength", settings.max_context_length, 0, 30);
  settings.batch_romaji_conversion =
      ReadBool(object, "batchRomajiConversion", settings.batch_romaji_conversion);
  settings.batch_romaji_preview_style = ReadEnum(
      object, "batchRomajiPreviewStyle", settings.batch_romaji_preview_style, {"kana", "romaji"});
  settings.batch_conversion_mode = ReadEnum(
      object, "batchConversionMode", settings.batch_conversion_mode, {"neural", "ai-cleanup"});
  settings.batch_auto_punctuation =
      ReadBool(object, "batchAutoPunctuation", settings.batch_auto_punctuation);
  settings.number_rewriter = ReadBool(object, "numberRewriter", settings.number_rewriter);
  settings.katakana_rewriter = ReadBool(object, "katakanaRewriter", settings.katakana_rewriter);
  settings.symbol_rewriter = ReadBool(object, "symbolRewriter", settings.symbol_rewriter);
  settings.symbol_data_path = ReadString(object, "symbolDataPath", settings.symbol_data_path);
  settings.emoji_rewriter = ReadBool(object, "emojiRewriter", settings.emoji_rewriter);
  settings.emoji_trigger_search =
      ReadBool(object, "emojiTriggerSearch", settings.emoji_trigger_search);
  settings.emoji_max_candidates =
      ReadClampedInt32(object, "emojiMaxCandidates", settings.emoji_max_candidates, 1, 50);
  settings.emoji_trigger_min_query_length = ReadClampedInt32(
      object, "emojiTriggerMinQueryLength", settings.emoji_trigger_min_query_length, 1, 8);
  settings.emoji_data_path = ReadString(object, "emojiDataPath", settings.emoji_data_path);

  if (const auto* model = ReadObject(object, "model")) {
    settings.model.enabled = ReadBool(*model, "enabled", settings.model.enabled);
    settings.model.selected_path = ReadString(*model, "selectedPath", settings.model.selected_path);
    settings.model.backend_preference =
        ReadEnum(*model, "backendPreference", settings.model.backend_preference,
                 {"auto", "cpu", "cuda", "vulkan", "winml", "directml", "npu"});
    settings.model.ep_preference = ReadEnum(*model, "epPreference", settings.model.ep_preference,
                                            {"auto", "npu", "gpu", "cpu"});
    settings.model.n_gpu_layers = ReadInt32(*model, "nGpuLayers", settings.model.n_gpu_layers);
    settings.model.auto_load_on_host_start =
        ReadBool(*model, "autoLoadOnHostStart", settings.model.auto_load_on_host_start);
    settings.model.fallback_to_simple_converter =
        ReadBool(*model, "fallbackToSimpleConverter", settings.model.fallback_to_simple_converter);
    settings.model.benchmark_on_model_change =
        ReadBool(*model, "benchmarkOnModelChange", settings.model.benchmark_on_model_change);
  }

  settings.typo_correction_mode =
      ReadEnum(object, "typoCorrectionMode", settings.typo_correction_mode,
               {"off", "suggest", "auto_replace"});
  settings.typo_min_count =
      ReadRangedInt32(object, "typoMinCount", settings.typo_min_count, 1, 100);

  if (const auto* auto_word = ReadObject(object, "autoWordRegistration")) {
    settings.auto_word.mining_enabled =
        ReadBool(*auto_word, "miningEnabled", settings.auto_word.mining_enabled);
    settings.auto_word.trending_enabled =
        ReadBool(*auto_word, "trendingEnabled", settings.auto_word.trending_enabled);
    settings.auto_word.registration_mode = ReadEnum(
        *auto_word, "registrationMode", settings.auto_word.registration_mode, {"confirm", "auto"});
    settings.auto_word.mining_min_count =
        ReadRangedInt32(*auto_word, "miningMinCount", settings.auto_word.mining_min_count, 1, 100);
    settings.auto_word.trending_interval_hours = ReadRangedInt32(
        *auto_word, "trendingIntervalHours", settings.auto_word.trending_interval_hours, 1, 8760);
  }

  if (const auto* auto_update = ReadObject(object, "autoUpdate")) {
    settings.auto_update.enabled = ReadBool(*auto_update, "enabled", settings.auto_update.enabled);
    settings.auto_update.channel =
        ReadEnum(*auto_update, "channel", settings.auto_update.channel, {"stable", "beta"});
    settings.auto_update.check_interval_hours =
        ReadInt32(*auto_update, "checkIntervalHours", settings.auto_update.check_interval_hours);
  }

  if (const auto* safe_mode = ReadObject(object, "safeMode")) {
    settings.safe_mode.enabled = ReadBool(*safe_mode, "enabled", settings.safe_mode.enabled);
    settings.safe_mode.entered_at =
        ReadString(*safe_mode, "enteredAt", settings.safe_mode.entered_at);
    settings.safe_mode.last_crash_count =
        ReadRangedInt32(*safe_mode, "lastCrashCount", settings.safe_mode.last_crash_count, 0,
                        (std::numeric_limits<int32_t>::max)());
  }
  if (settings.safe_mode.enabled) ApplySafeModeOverrides(settings);

  return settings;
}

BackendKind BackendFromPreference(const std::string& preference, BackendKind fallback) {
  if (preference == "cuda") return BackendKind::Cuda;
  if (preference == "vulkan") return BackendKind::Vulkan;
  if (preference == "cpu" || preference == "winml" || preference == "directml" ||
      preference == "npu") {
    return BackendKind::Cpu;
  }
  return fallback;
}

}  // namespace

SettingsStore::SettingsStore(std::filesystem::path settings_path)
    : SettingsStore(std::move(settings_path), std::chrono::milliseconds(5000)) {}

SettingsStore::SettingsStore(std::filesystem::path settings_path,
                             std::chrono::milliseconds file_lock_timeout)
    : settings_path_(std::move(settings_path)), file_lock_timeout_(file_lock_timeout) {
  last_result_.settings = settings_;
}

SettingsStore::PrivacyGuard SettingsStore::LockPrivacyPolicy() const {
  std::unique_lock lock(privacy_mutex_);
  return {std::move(lock), privacy_policy_};
}

void SettingsStore::PublishPrivacyPolicy() {
  std::lock_guard lock(privacy_mutex_);
  privacy_policy_ = settings_.privacy_policy;
}

SettingsLoadResult SettingsStore::LoadImpl(bool preserve_current_on_invalid) {
  RuntimeSettings defaults;
  SettingsLoadResult result;
  result.settings = defaults;

  auto file_lock =
      azookey::learning::AcquireExclusiveFileLockForPath(settings_path_, file_lock_timeout_);
  const bool may_quarantine = file_lock.has_value();

  // Stamped under the lock and before the read, so a writer cannot slip a
  // revision between the stamp and the content it is supposed to describe.
  std::error_code stamp_error;
  const auto stamp = std::filesystem::last_write_time(settings_path_, stamp_error);
  loaded_write_time_.store(stamp_error ? kNoWriteTime : stamp.time_since_epoch().count(),
                           std::memory_order_release);

  const auto finish_invalid = [&]() -> SettingsLoadResult {
    if (preserve_current_on_invalid) {
      result.settings = settings_;
      // An unreadable consent must never keep crash collection enabled.
      result.settings.crash_report_consent = "off";
      settings_.crash_report_consent = "off";
    } else {
      settings_ = result.settings;
    }
    // Retaining unrelated last-good options must not retain privacy consent.
    result.settings.privacy_policy = {};
    settings_.privacy_policy = {};
    PublishPrivacyPolicy();
    last_result_ = result;
    return last_result_;
  };

  std::error_code ec;
  const bool exists = std::filesystem::exists(settings_path_, ec);
  if (ec) {
    result.status = SettingsLoadStatus::Invalid;
    result.error = "failed to inspect settings.json: " + ec.message();
    return finish_invalid();
  }
  if (!exists) {
    settings_ = result.settings;
    PublishPrivacyPolicy();
    last_result_ = result;
    return last_result_;
  }

  auto read_error = std::optional<std::string>();
  auto content = ReadFile(settings_path_, &read_error);
  if (!content) {
    result.status = SettingsLoadStatus::Invalid;
    result.error = read_error.value_or("failed to read settings.json");
    return finish_invalid();
  }

  auto parsed = j::Parse(*content);
  if (!parsed || !parsed->IsObject()) {
    result.status = SettingsLoadStatus::Invalid;
    result.error = "invalid settings.json";
    if (may_quarantine) {
      result.quarantined_path = QuarantineInvalidFile(settings_path_, &result.error);
    } else {
      result.error = "invalid settings.json; file lock unavailable, so it was not quarantined";
    }
    return finish_invalid();
  }

  result.settings = ParseRuntimeSettings(parsed->AsObject());
  result.settings.app_profiles = std::make_shared<const core::AppProfileResolver>(
      core::AppProfileResolver::FromSettings(*parsed, &result.profile_warnings));
  result.status = SettingsLoadStatus::Loaded;
  settings_ = result.settings;
  PublishPrivacyPolicy();
  last_result_ = result;
  return last_result_;
}

bool SettingsStore::PersistSafeModeEntered(const std::string& entered_at, int32_t crash_count) {
  auto file_lock =
      azookey::learning::AcquireExclusiveFileLockForPath(settings_path_, file_lock_timeout_);
  if (!file_lock) return false;
  j::Object root;
  std::error_code ec;
  const bool exists = std::filesystem::exists(settings_path_, ec);
  if (ec) return false;
  if (exists) {
    auto content = ReadFile(settings_path_, nullptr);
    if (!content) return false;
    auto parsed = j::Parse(*content);
    if (!parsed || !parsed->IsObject()) return false;
    root = parsed->AsObject();
  }
  j::Object safe_mode;
  safe_mode["enabled"] = j::Value(true);
  safe_mode["enteredAt"] = j::Value(entered_at);
  safe_mode["lastCrashCount"] = j::Value(crash_count);
  root["safeMode"] = j::Value(std::move(safe_mode));
  // The same shape settings-app/SettingsDocument.cpp writes.
  std::string serialized = j::Stringify(j::Value(std::move(root)));
  serialized.push_back('\n');
  return azookey::learning::WriteTextFileAtomically(settings_path_, serialized);
}

SettingsLoadResult SettingsStore::Load() { return LoadImpl(false); }

SettingsLoadResult SettingsStore::Reload() { return LoadImpl(true); }

std::optional<RuntimeSettings> SettingsStore::SettingsWrittenAfterLoad() {
  std::error_code ec;
  const auto stamp = std::filesystem::last_write_time(settings_path_, ec);
  // A file that cannot be stat'ed, or still carries the timestamp the loaded
  // settings were parsed from, needs no read. Timestamps only move forward
  // per save, so two saves inside one filesystem tick look like one; the
  // UpdateConfig that follows each save still applies the later one.
  if (ec) return std::nullopt;
  const int64_t stamped = stamp.time_since_epoch().count();
  if (stamped == loaded_write_time_.load(std::memory_order_acquire)) return std::nullopt;

  const std::lock_guard<std::mutex> lock(peeked_mutex_);
  if (peeked_ && peeked_write_time_ == stamped) return peeked_;
  // Deliberately not the load path: this runs while a reply is owed, so it
  // never waits out a writer, never quarantines a file the user may still be
  // editing, and never replaces the runtime settings a failed parse would
  // reset. Any of those outcomes leaves the caller with what it already has.
  constexpr std::chrono::milliseconds kPeekLockTimeout{100};
  auto file_lock =
      azookey::learning::AcquireExclusiveFileLockForPath(settings_path_, kPeekLockTimeout);
  if (!file_lock) return std::nullopt;
  auto content = ReadFile(settings_path_, nullptr);
  if (!content) return std::nullopt;
  auto parsed = j::Parse(*content);
  if (!parsed || !parsed->IsObject()) return std::nullopt;
  RuntimeSettings settings = ParseRuntimeSettings(parsed->AsObject());
  settings.app_profiles = std::make_shared<const core::AppProfileResolver>(
      core::AppProfileResolver::FromSettings(*parsed));
  peeked_write_time_ = stamped;
  peeked_ = std::move(settings);
  return peeked_;
}

InferenceThreadEnvironment QueryInferenceThreadEnvironment() {
  InferenceThreadEnvironment environment;
  environment.hardware_concurrency = std::thread::hardware_concurrency();
#ifdef _WIN32
  SYSTEM_POWER_STATUS status{};
  if (GetSystemPowerStatus(&status)) {
    if (status.ACLineStatus == 1) environment.power_source = PowerSource::Ac;
    if (status.ACLineStatus == 0) environment.power_source = PowerSource::Battery;
  }
#endif
  return environment;
}

EngineConfig ApplyRuntimeSettingsToEngineConfig(EngineConfig config,
                                                const RuntimeSettings& settings) {
  return ApplyRuntimeSettingsToEngineConfig(config, settings, config.backend);
}

EngineConfig ApplyRuntimeSettingsToEngineConfig(
    EngineConfig config, const RuntimeSettings& settings, BackendKind auto_backend,
    const InferenceThreadEnvironmentProvider& provider) {
  config.enable_live_conversion = settings.live_conversion;
  config.dynamic_punctuation = settings.dynamic_punctuation;
  config.segment_boundary_confidence = settings.segment_boundary_confidence;
  config.punctuation_rules_path = settings.punctuation_rules_path;
  config.rewriters.symbol_enabled = settings.symbol_rewriter;
  config.rewriters.emoji_enabled = settings.emoji_rewriter;
  config.rewriters.trigger_enabled = settings.emoji_trigger_search;
  config.rewriters.symbol_path = core::Utf8Path(settings.symbol_data_path);
  config.rewriters.emoji_path = core::Utf8Path(settings.emoji_data_path);
  config.nll = ClampNllConfig(settings.nll);
  if (settings.inference_threads > 0) {
    config.inference_threads = settings.inference_threads;
  } else {
    const auto environment = provider();
    unsigned int profile_threads = 4;
    if (settings.power_profile == "performance") {
      profile_threads = 8;
    } else if (settings.power_profile == "battery_saver") {
      profile_threads = 2;
    } else if (environment.power_source == PowerSource::Ac) {
      profile_threads = 8;
    } else if (environment.power_source == PowerSource::Battery) {
      profile_threads = 2;
    }
    // Keep a concrete positive value for runtime application and model reloads.
    config.inference_threads = static_cast<int32_t>(
        std::min(profile_threads, std::max(1u, environment.hardware_concurrency)));
  }
  config.max_candidates = static_cast<uint32_t>(settings.max_candidates);
  config.max_context_length = static_cast<uint32_t>(settings.max_context_length);
  config.typo_correction_mode = settings.typo_correction_mode;
  config.typo_min_count = static_cast<uint32_t>(settings.typo_min_count);
  config.auto_word_mining_enabled = settings.auto_word.mining_enabled;
  config.auto_word_auto_register = settings.auto_word.registration_mode == "auto";
  config.auto_word_min_count = static_cast<uint32_t>(settings.auto_word.mining_min_count);

  std::string backend_preference = settings.backend_preference;
  if (settings.model.backend_preference != "auto") {
    backend_preference = settings.model.backend_preference;
  }
  config.backend = BackendFromPreference(backend_preference, auto_backend);

  if (!settings.model.enabled) {
    config.model_path.clear();
    config.n_gpu_layers.reset();
    return config;
  }
  config.model_path = settings.model.selected_path;
  if (settings.model.n_gpu_layers >= 0) {
    config.n_gpu_layers = settings.model.n_gpu_layers;
  } else {
    config.n_gpu_layers.reset();
  }
  return config;
}

}  // namespace azookey::host
