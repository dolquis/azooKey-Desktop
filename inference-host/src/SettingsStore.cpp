#include "azookey/host/SettingsStore.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "azookey/ipc/Json.h"
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

RuntimeSettings ParseRuntimeSettings(const j::Object& object) {
  RuntimeSettings settings;
  settings.input_mode =
      ReadEnum(object, "inputMode", settings.input_mode, {"hiragana", "alnum_half", "alnum_full"});
  settings.live_conversion = ReadBool(object, "liveConversion", settings.live_conversion);
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

  if (const auto* auto_update = ReadObject(object, "autoUpdate")) {
    settings.auto_update.enabled = ReadBool(*auto_update, "enabled", settings.auto_update.enabled);
    settings.auto_update.channel =
        ReadEnum(*auto_update, "channel", settings.auto_update.channel, {"stable", "beta"});
    settings.auto_update.check_interval_hours =
        ReadInt32(*auto_update, "checkIntervalHours", settings.auto_update.check_interval_hours);
  }

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

SettingsLoadResult SettingsStore::LoadImpl(bool preserve_current_on_invalid) {
  RuntimeSettings defaults;
  SettingsLoadResult result;
  result.settings = defaults;

  auto file_lock =
      azookey::learning::AcquireExclusiveFileLockForPath(settings_path_, file_lock_timeout_);
  const bool may_quarantine = file_lock.has_value();

  const auto finish_invalid = [&]() -> SettingsLoadResult {
    if (preserve_current_on_invalid) {
      result.settings = settings_;
    } else {
      settings_ = result.settings;
    }
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
  result.status = SettingsLoadStatus::Loaded;
  settings_ = result.settings;
  last_result_ = result;
  return last_result_;
}

SettingsLoadResult SettingsStore::Load() { return LoadImpl(false); }

SettingsLoadResult SettingsStore::Reload() { return LoadImpl(true); }

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
