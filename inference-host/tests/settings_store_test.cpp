#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "azookey/core/PlatformPaths.h"

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#else
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

#include "azookey/host/SettingsStore.h"
#include "azookey/ipc/Json.h"
#include "azookey/ipc/Payloads.h"
#include "azookey/learning/AtomicFile.h"
#include "azookey/learning/FileLock.h"

namespace {

class ScopedTempDirectory {
 public:
  explicit ScopedTempDirectory(const char* name) {
    const auto base = std::filesystem::temp_directory_path();
#ifdef _WIN32
    const auto process_id = GetCurrentProcessId();
#else
    const auto process_id = getpid();
#endif
    for (int attempt = 0; attempt < 16; ++attempt) {
      path_ = base / (std::string(name) + "_" + std::to_string(process_id) + "_" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                      "_" + std::to_string(next_id_++));
      if (std::filesystem::create_directory(path_)) return;
    }
    throw std::runtime_error("Could not create a unique settings test directory");
  }

  ~ScopedTempDirectory() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  ScopedTempDirectory(const ScopedTempDirectory&) = delete;
  ScopedTempDirectory& operator=(const ScopedTempDirectory&) = delete;

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
  static inline std::atomic<uint64_t> next_id_{0};
};

void WriteText(const std::filesystem::path& path, const std::string& text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << text;
}

// Closes the file before returning, so the test can remove its directory.
std::string ReadText(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

TEST(SettingsStoreTest, BodyLogPolicyDefaultsAndMalformedSettingsFailClosed) {
  struct Case {
    const char* json;
    bool secure;
    bool detailed;
    bool learning;
  };
  const Case cases[] = {
      {"{}", false, false, true},
      {R"({"privacy":{}})", false, false, true},
      {R"({"privacy":false})", true, false, false},
      {R"({"privacy":{"mode":12,"redactLogs":false}})", true, false, false},
      {R"({"privacy":{"mode":"unknown","redactLogs":false}})", true, false, false},
      {R"({"privacy":{"redactLogs":false}})", false, true, true},
      {R"({"privacy":{"mode":"normal","redactLogs":false}})", false, true, true},
      {R"({"privacy":{"mode":"offline","redactLogs":false}})", false, true, true},
      {R"({"privacy":{"mode":"secure","redactLogs":false}})", true, false, false},
      {R"({"privacy":{"mode":"private","redactLogs":false}})", false, false, false},
      {R"({"privacy":{"mode":"normal","redactLogs":"false"}})", false, false, true},
      {R"({"privacy":{"mode":"custom","redactLogs":false}})", false, false, false},
      {R"({"privacy":{"mode":"custom","redactLogs":false,"custom":false}})", false, false, false},
      {R"({"privacy":{"mode":"custom","redactLogs":false,"custom":{"detailedLogging":true}}})",
       false, true, false},
      {R"({"privacy":{"mode":"custom","redactLogs":false,"custom":{"detailedLogging":"true"}}})",
       false, false, false},
      {R"({"privacy":{"mode":"custom","redactLogs":true,"custom":{"detailedLogging":true}}})",
       false, false, false},
      {R"({"privacy":{"mode":"custom","custom":{"learning":true}}})", false, false, true},
      {R"({"privacy":{"mode":"custom","custom":{"learning":"true"}}})", false, false, false},
  };
  ScopedTempDirectory temp("azookey_settings_body_log_policy");
  const auto& dir = temp.path();
  const auto path = dir / "settings.json";
  for (const auto& item : cases) {
    SCOPED_TRACE(item.json);
    WriteText(path, item.json);
    azookey::host::SettingsStore store(path);
    const auto loaded = store.Load();
    EXPECT_EQ(loaded.settings.privacy_policy.secure, item.secure);
    EXPECT_EQ(loaded.settings.privacy_policy.detailed_logging_allowed, item.detailed);
    EXPECT_EQ(loaded.settings.privacy_policy.learning_allowed, item.learning);
  }
  const auto malformed = azookey::core::ParsePrivacyPolicy(azookey::ipc::json::Value{});
  EXPECT_TRUE(malformed.secure);
  EXPECT_FALSE(malformed.detailed_logging_allowed);
  EXPECT_FALSE(malformed.learning_allowed);
}

TEST(SettingsStoreTest, SettingsWrittenAfterLoadReadsOnlyAheadOfTheLoadedFile) {
  ScopedTempDirectory temp("azookey_settings_written_after_load");
  const auto& dir = temp.path();
  const auto path = dir / "settings.json";
  WriteText(path, R"({"maxCandidates":9})");
  azookey::host::SettingsStore store(path);
  store.Load();
  ASSERT_EQ(store.settings().max_candidates, 9);
  // Nothing newer than the load: the Handshake reply answers from memory.
  EXPECT_FALSE(store.SettingsWrittenAfterLoad().has_value());

  const auto stamp = std::filesystem::last_write_time(path);
  WriteText(path, R"({"maxCandidates":11})");
  std::filesystem::last_write_time(path, stamp + std::chrono::seconds(1));
  const auto written = store.SettingsWrittenAfterLoad();
  ASSERT_TRUE(written.has_value());
  EXPECT_EQ(written->max_candidates, 11);
  // Reading ahead of UpdateConfig must not adopt the file as runtime settings:
  // applying a reload stays that message's job, model reload and all.
  EXPECT_EQ(store.settings().max_candidates, 9);

  // An unparsable file is left alone rather than quarantined from this path,
  // and the caller keeps the settings it already had.
  WriteText(path, "{");
  std::filesystem::last_write_time(path, stamp + std::chrono::seconds(2));
  EXPECT_FALSE(store.SettingsWrittenAfterLoad().has_value());
  EXPECT_TRUE(std::filesystem::exists(path));
  EXPECT_FALSE(std::filesystem::exists(dir / "settings.json.invalid"));

  // A file that disappears mid-save is not a reason to fall back to defaults.
  std::filesystem::remove(path);
  EXPECT_FALSE(store.SettingsWrittenAfterLoad().has_value());
  EXPECT_EQ(store.settings().max_candidates, 9);
}

TEST(SettingsStoreTest, SecureAppsNormalizeAndMalformedPrivacyFallsBackToDefaults) {
  ScopedTempDirectory temp("azookey_settings_secure_apps");
  const auto& dir = temp.path();
  const auto path = dir / "settings.json";
  azookey::host::SettingsStore store(path);
  const auto loaded = store.Load();
  EXPECT_TRUE(loaded.settings.secure_apps.empty());
  EXPECT_TRUE(loaded.settings.show_secure_indicator);

  WriteText(path, R"({"privacy":{"secureApps":["KeePassXC.EXE","",3,"vault.exe"]}})");
  const auto parsed = store.Reload();
  // Names are lowercased at this boundary and non-string entries are dropped,
  // so an entry a user typed in the wrong case still matches.
  EXPECT_EQ(parsed.settings.secure_apps, (std::vector<std::string>{"keepassxc.exe", "vault.exe"}));

  // Anything that is not a list of names degrades to "bundled defaults only"
  // rather than to no secure detection, and never stops the host from loading.
  for (const auto* text :
       {R"({})", R"({"privacy":false})", R"({"privacy":{"secureApps":"keepass.exe"}})",
        R"({"privacy":{"secureApps":{}}})", R"({"privacy":{}})"}) {
    WriteText(path, text);
    EXPECT_TRUE(store.Reload().settings.secure_apps.empty()) << text;
  }

  for (const auto* text : {R"({"privacy":{"showSecureIndicator":"yes"}})",
                           R"({"privacy":{"showSecureIndicator":1}})", R"({"privacy":{}})"}) {
    WriteText(path, text);
    EXPECT_TRUE(store.Reload().settings.show_secure_indicator) << text;
  }
  WriteText(path, R"({"privacy":{"showSecureIndicator":false}})");
  EXPECT_FALSE(store.Reload().settings.show_secure_indicator);
}

TEST(SettingsStoreTest, AutoSecureInputOnlyExplicitFalseDisablesAndMalformedReloadRestoresIt) {
  ScopedTempDirectory temp("azookey_settings_auto_secure_input");
  const auto path = temp.path() / "settings.json";
  azookey::host::SettingsStore store(path);
  EXPECT_TRUE(store.Load().settings.auto_secure_input);
  for (const auto* text :
       {R"({})", R"({"privacy":{}})", R"({"privacy":{"autoSecureInput":true}})",
        R"({"privacy":{"autoSecureInput":"false"}})", R"({"privacy":{"autoSecureInput":0}})",
        R"({"privacy":{"autoSecureInput":null}})", R"({"privacy":false})", "{"}) {
    SCOPED_TRACE(text);
    WriteText(path, R"({"privacy":{"autoSecureInput":false}})");
    EXPECT_FALSE(store.Reload().settings.auto_secure_input);
    WriteText(path, text);
    EXPECT_TRUE(store.Reload().settings.auto_secure_input);
    EXPECT_TRUE(store.settings().auto_secure_input);
  }
}

TEST(SettingsStoreTest, CrashConsentRequiresExplicitLocalAndMalformedReloadDisablesIt) {
  ScopedTempDirectory temp("azookey_settings_crash_consent");
  const auto& dir = temp.path();
  const auto path = dir / "settings.json";
  azookey::host::SettingsStore store(path);
  EXPECT_EQ(store.Load().settings.crash_report_consent, "off");
  for (const auto* text :
       {R"({})", R"({"privacy":false})", R"({"privacy":{"crashReportConsent":true}})",
        R"({"privacy":{"crashReportConsent":"unknown"}})",
        R"({"privacy":{"crashReportConsent":"off"}})"}) {
    WriteText(path, text);
    EXPECT_EQ(store.Reload().settings.crash_report_consent, "off") << text;
  }
  WriteText(path,
            R"({"logLevel":"debug","privacy":{"mode":"private","crashReportConsent":"local"}})");
  const auto enabled = store.Reload();
  EXPECT_EQ(enabled.settings.crash_report_consent, "local");
  EXPECT_TRUE(enabled.settings.ai_privacy.ai);
  EXPECT_FALSE(enabled.settings.ai_privacy.external);
  WriteText(path, "{");
  const auto invalid = store.Reload();
  EXPECT_EQ(invalid.status, azookey::host::SettingsLoadStatus::Invalid);
  EXPECT_EQ(invalid.settings.crash_report_consent, "off");
  EXPECT_EQ(store.settings().crash_report_consent, "off");
  EXPECT_EQ(invalid.settings.log_level, "debug");
}

TEST(SettingsStoreTest, AiPrivacyAndTimeoutAreAppliedAndBounded) {
  ScopedTempDirectory temp("azookey_settings_ai_privacy");
  const auto& dir = temp.path();
  const auto path = dir / "settings.json";
  WriteText(path, R"({"openAiTimeoutMs":2500,"privacy":{"mode":"private"}})");
  azookey::host::SettingsStore store(path);
  auto result = store.Load();
  EXPECT_EQ(result.settings.open_ai_timeout_ms, 2500);
  EXPECT_TRUE(result.settings.ai_privacy.ai);
  EXPECT_FALSE(result.settings.ai_privacy.external);
  WriteText(path, R"({"openAiTimeoutMs":999999,"privacy":{"mode":"secure"}})");
  result = store.Reload();
  EXPECT_EQ(result.settings.open_ai_timeout_ms, 120000);
  EXPECT_FALSE(result.settings.ai_privacy.ai);
  WriteText(path, R"({"openAiTimeoutMs":"invalid","privacy":{"mode":false}})");
  result = store.Reload();
  EXPECT_EQ(result.settings.open_ai_timeout_ms, 30000);
  EXPECT_FALSE(result.settings.ai_privacy.external);
}

TEST(SettingsStoreTest, LoadsCommonProfilesAndReloadKeepsPreviousSnapshotImmutable) {
  ScopedTempDirectory temp("azookey_settings_profiles");
  const auto& dir = temp.path();
  const auto path = dir / "settings.json";
  WriteText(path, R"({"predictionEnabled":false,"profilesByApp":{
    "default":{"learningEnabled":false},"code.exe":{"style":"technical","bad":true}}})");
  azookey::host::SettingsStore store(path);
  const auto loaded = store.Load();
  ASSERT_EQ(loaded.status, azookey::host::SettingsLoadStatus::Loaded);
  ASSERT_FALSE(loaded.profile_warnings.empty());
  const azookey::core::ForegroundApp app{"CODE.EXE", "", true};
  const azookey::ipc::json::Value first(loaded.settings.AppProfiles().Resolve(app));
  EXPECT_EQ(first.GetString("style"), "technical");
  EXPECT_EQ(first.GetBool("predictionEnabled"), false);
  EXPECT_EQ(first.GetBool("learningEnabled"), false);
  WriteText(path, R"({"profilesByApp":{"code.exe":{"style":"polite"}}})");
  const auto reloaded = store.Reload();
  EXPECT_EQ(
      azookey::ipc::json::Value(reloaded.settings.AppProfiles().Resolve(app)).GetString("style"),
      "polite");
  EXPECT_EQ(
      azookey::ipc::json::Value(loaded.settings.AppProfiles().Resolve(app)).GetString("style"),
      "technical");
}

TEST(SettingsStoreTest, MissingFileUsesSchemaDefaults) {
  ScopedTempDirectory temp("azookey_settings_missing");
  const auto& dir = temp.path();
  const auto path = dir / "settings.json";
  azookey::host::SettingsStore store(path);

  const auto result = store.Load();

  EXPECT_EQ(result.status, azookey::host::SettingsLoadStatus::Missing);
  EXPECT_EQ(result.settings.input_mode, "hiragana");
  EXPECT_FALSE(result.settings.live_conversion);
  EXPECT_FALSE(result.settings.number_rewriter);
  EXPECT_FALSE(result.settings.katakana_rewriter);
  EXPECT_EQ(result.settings.nll, azookey::host::NllConfig{});
  EXPECT_EQ(result.settings.inference_threads, 0);
  EXPECT_EQ(result.settings.max_candidates, 9);
  EXPECT_EQ(result.settings.max_context_length, 10);
  EXPECT_TRUE(result.settings.prediction_enabled);
  EXPECT_TRUE(result.settings.privacy_policy.learning_allowed);
  EXPECT_EQ(result.settings.backend_preference, "auto");
  EXPECT_TRUE(result.settings.model.enabled);
  EXPECT_TRUE(result.settings.model.auto_load_on_host_start);
}

TEST(SettingsStoreTest, NllSettingsClampAndReachEngineConfig) {
  ScopedTempDirectory temp("azookey_settings_nll");
  const auto& dir = temp.path();
  const auto path = dir / "settings.json";
  WriteText(path, R"({"reranker":{"nllRerankEnabled":true,"nllTopK":1e30,
      "nllWeight":-2,"nllBudgetMs":1,"nllFailureThreshold":100}})");
  azookey::host::SettingsStore store(path);
  const auto result = store.Load();
  ASSERT_EQ(result.status, azookey::host::SettingsLoadStatus::Loaded);
  const auto config = azookey::host::ApplyRuntimeSettingsToEngineConfig({}, result.settings);
  EXPECT_TRUE(config.nll.enabled);
  EXPECT_EQ(config.nll.top_k, 16);
  EXPECT_EQ(config.nll.weight, 0.0);
  EXPECT_EQ(config.nll.budget_ms, 5);
  EXPECT_EQ(config.nll.failure_threshold, 10);
  WriteText(path, R"({"reranker":{"nllRerankEnabled":"true","nllTopK":1.5,
      "nllWeight":"bad","nllBudgetMs":null,"nllFailureThreshold":false}})");
  EXPECT_EQ(store.Reload().settings.nll, azookey::host::NllConfig{});
}

TEST(SettingsStoreTest, PartialFileFillsDefaultsAndAppliesEngineConfig) {
  ScopedTempDirectory temp("azookey_settings_partial");
  const auto& dir = temp.path();
  const auto path = dir / "settings.json";
  WriteText(path, R"({
    "liveConversion": true,
    "numberRewriter": true,
    "katakanaRewriter": true,
    "symbolRewriter": true,
    "emojiRewriter": true,
    "emojiTriggerSearch": false,
    "emojiMaxCandidates": 99,
    "emojiTriggerMinQueryLength": 0,
    "symbolDataPath": "C:/data/記号.tsv",
    "emojiDataPath": "C:/data/絵文字.tsv",
    "inferenceThreads": 6,
    "maxCandidates": 12,
    "maxContextLength": 20,
    "predictionEnabled": false,
    "backendPreference": "cuda",
    "model": {
      "selectedPath": "C:/models/zenz-v3.gguf",
      "nGpuLayers": 12
    }
  })");

  azookey::host::SettingsStore store(path);
  const auto result = store.Load();

  EXPECT_EQ(result.status, azookey::host::SettingsLoadStatus::Loaded);
  EXPECT_EQ(result.settings.input_mode, "hiragana");
  EXPECT_TRUE(result.settings.live_conversion);
  EXPECT_TRUE(result.settings.number_rewriter);
  EXPECT_TRUE(result.settings.katakana_rewriter);
  EXPECT_TRUE(result.settings.symbol_rewriter);
  EXPECT_TRUE(result.settings.emoji_rewriter);
  EXPECT_FALSE(result.settings.emoji_trigger_search);
  EXPECT_EQ(result.settings.emoji_max_candidates, 50);
  EXPECT_EQ(result.settings.emoji_trigger_min_query_length, 1);
  EXPECT_EQ(result.settings.inference_threads, 6);
  EXPECT_EQ(result.settings.max_candidates, 12);
  EXPECT_EQ(result.settings.max_context_length, 20);
  EXPECT_FALSE(result.settings.prediction_enabled);
  EXPECT_EQ(result.settings.backend_preference, "cuda");
  EXPECT_EQ(result.settings.model.selected_path, "C:/models/zenz-v3.gguf");
  EXPECT_EQ(result.settings.model.n_gpu_layers, 12);
  EXPECT_TRUE(result.settings.model.auto_load_on_host_start);

  azookey::host::EngineConfig config;
  config = azookey::host::ApplyRuntimeSettingsToEngineConfig(config, result.settings);
  EXPECT_TRUE(config.enable_live_conversion);
  EXPECT_TRUE(config.rewriters.symbol_enabled);
  EXPECT_TRUE(config.rewriters.emoji_enabled);
  EXPECT_FALSE(config.rewriters.trigger_enabled);
  EXPECT_EQ(config.rewriters.symbol_path, azookey::core::Utf8Path("C:/data/記号.tsv"));
  EXPECT_EQ(config.rewriters.emoji_path, azookey::core::Utf8Path("C:/data/絵文字.tsv"));
  EXPECT_EQ(config.backend, azookey::host::BackendKind::Cuda);
  EXPECT_EQ(config.model_path, "C:/models/zenz-v3.gguf");
  ASSERT_TRUE(config.n_gpu_layers.has_value());
  EXPECT_EQ(*config.n_gpu_layers, 12);
  ASSERT_TRUE(config.inference_threads.has_value());
  EXPECT_EQ(*config.inference_threads, 6);
  EXPECT_EQ(config.max_candidates, 12u);
  EXPECT_EQ(config.max_context_length, 20u);
}

TEST(SettingsStoreTest, DynamicPunctuationSettingsDefaultAndValidation) {
  ScopedTempDirectory temp("azookey_settings_dynamic_punctuation");
  const auto& dir = temp.path();
  const auto path = dir / "settings.json";
  WriteText(path, R"({"liveConversion":true})");
  azookey::host::SettingsStore store(path);
  auto result = store.Load();
  EXPECT_FALSE(result.settings.dynamic_punctuation);
  EXPECT_EQ(result.settings.dynamic_punctuation_style, "ja");
  EXPECT_EQ(result.settings.dynamic_punctuation_stability, "onPause");
  EXPECT_EQ(result.settings.dynamic_punctuation_idle_ms, 400);
  EXPECT_DOUBLE_EQ(result.settings.segment_boundary_confidence, 0.5);

  WriteText(path, R"({"liveConversion":true,"dynamicPunctuation":true,
    "dynamicPunctuationStyle":"fullwidth_latin","dynamicPunctuationStability":"eager",
    "dynamicPunctuationIdleMs":250,"segmentBoundaryConfidence":0.75,
    "punctuationRulesPath":"C:/rules/custom.tsv"})");
  result = store.Reload();
  EXPECT_TRUE(result.settings.dynamic_punctuation);
  EXPECT_EQ(result.settings.dynamic_punctuation_style, "fullwidth_latin");
  EXPECT_EQ(result.settings.dynamic_punctuation_stability, "eager");
  EXPECT_EQ(result.settings.dynamic_punctuation_idle_ms, 250);
  EXPECT_DOUBLE_EQ(result.settings.segment_boundary_confidence, 0.75);
  EXPECT_EQ(result.settings.punctuation_rules_path, "C:/rules/custom.tsv");
  const auto config = azookey::host::ApplyRuntimeSettingsToEngineConfig({}, result.settings);
  EXPECT_TRUE(config.dynamic_punctuation);
  EXPECT_DOUBLE_EQ(config.segment_boundary_confidence, 0.75);
  EXPECT_EQ(config.punctuation_rules_path, "C:/rules/custom.tsv");

  WriteText(path, R"({"dynamicPunctuation":true,"dynamicPunctuationStyle":"invalid",
    "dynamicPunctuationStability":"invalid","dynamicPunctuationIdleMs":0,
    "segmentBoundaryConfidence":2})");
  result = store.Reload();
  EXPECT_TRUE(result.settings.dynamic_punctuation);
  EXPECT_EQ(result.settings.dynamic_punctuation_style, "ja");
  EXPECT_EQ(result.settings.dynamic_punctuation_stability, "onPause");
  EXPECT_EQ(result.settings.dynamic_punctuation_idle_ms, 400);
  EXPECT_DOUBLE_EQ(result.settings.segment_boundary_confidence, 0.5);
}

TEST(SettingsStoreTest, ModelBlockOverridesRootBackendAndCanDisableModel) {
  ScopedTempDirectory temp("azookey_settings_model_override");
  const auto& dir = temp.path();
  const auto path = dir / "settings.json";
  WriteText(path, R"({
    "backendPreference": "cuda",
    "model": {
      "enabled": false,
      "backendPreference": "cpu",
      "selectedPath": "C:/models/ignored.gguf",
      "autoLoadOnHostStart": false
    }
  })");

  azookey::host::SettingsStore store(path);
  const auto result = store.Load();
  azookey::host::EngineConfig config;
  config.model_path = "C:/models/existing.gguf";
  config = azookey::host::ApplyRuntimeSettingsToEngineConfig(config, result.settings);

  EXPECT_FALSE(result.settings.model.enabled);
  EXPECT_FALSE(result.settings.model.auto_load_on_host_start);
  EXPECT_EQ(config.backend, azookey::host::BackendKind::Cpu);
  EXPECT_TRUE(config.model_path.empty());
  EXPECT_FALSE(config.n_gpu_layers.has_value());
  // M45 section 5.2: both keys present is flagged so the Host can warn.
  EXPECT_TRUE(result.settings.backend_preference_conflict);
}

TEST(SettingsStoreTest, InlineEnglishKeysReachTheEngineConfigAndRejectBadValues) {
  ScopedTempDirectory temp("azookey_settings_inline_english");
  const auto path = temp.path() / "settings.json";
  WriteText(path, R"({
    "inlineEnglishCandidates": true,
    "inlineEnglishCaseVariants": false,
    "fullWidthEnglishCandidate": true,
    "inlineEnglishMinLength": 3,
    "inlineEnglishPromoteThreshold": 0.25,
    "inlineEnglishDictionary": true,
    "inlineEnglishDictionaryPath": "C:/dict/words.tsv"
  })");
  azookey::host::SettingsStore store(path);
  const auto loaded = store.Load();
  EXPECT_TRUE(loaded.settings.inline_english_candidates);
  const auto config = azookey::host::ApplyRuntimeSettingsToEngineConfig(
      azookey::host::EngineConfig{}, loaded.settings);
  EXPECT_FALSE(config.english.case_variants);
  EXPECT_TRUE(config.english.full_width);
  EXPECT_EQ(config.english.min_length, 3u);
  EXPECT_DOUBLE_EQ(config.english.promote_threshold, 0.25);
  EXPECT_TRUE(config.english.dictionary_enabled);
  EXPECT_EQ(config.english.dictionary_path, "C:/dict/words.tsv");

  WriteText(path, R"({"inlineEnglishMinLength": 0, "inlineEnglishPromoteThreshold": 1.5,
                      "inlineEnglishCaseVariants": "yes"})");
  const auto fallback = azookey::host::SettingsStore(path).Load().settings.english;
  EXPECT_EQ(fallback.min_length, 2u);
  EXPECT_DOUBLE_EQ(fallback.promote_threshold, 0.6);
  EXPECT_TRUE(fallback.case_variants);
}

TEST(SettingsStoreTest, BackendPreferenceConflictNeedsBothKeys) {
  ScopedTempDirectory temp("azookey_settings_backend_conflict");
  const auto path = temp.path() / "settings.json";
  for (const char* json :
       {R"({"backendPreference":"cuda"})", R"({"model":{"backendPreference":"cpu"}})",
        R"({"backendPreference":"cuda","model":{"enabled":true}})"}) {
    WriteText(path, json);
    azookey::host::SettingsStore store(path);
    EXPECT_FALSE(store.Load().settings.backend_preference_conflict) << json;
  }
}

TEST(SettingsStoreTest, VulkanPreferenceAndAutoResolveAgainstBuildDefault) {
  azookey::host::RuntimeSettings settings;
  azookey::host::EngineConfig config;
  settings.model.backend_preference = "vulkan";
  auto resolved = azookey::host::ApplyRuntimeSettingsToEngineConfig(
      config, settings, azookey::host::BackendKind::Cpu);
  EXPECT_EQ(resolved.backend, azookey::host::BackendKind::Vulkan);

  settings.model.backend_preference = "auto";
  resolved = azookey::host::ApplyRuntimeSettingsToEngineConfig(config, settings,
                                                               azookey::host::BackendKind::Vulkan);
  EXPECT_EQ(resolved.backend, azookey::host::BackendKind::Vulkan);

  settings.model.backend_preference = "cpu";
  resolved = azookey::host::ApplyRuntimeSettingsToEngineConfig(config, settings,
                                                               azookey::host::BackendKind::Vulkan);
  EXPECT_EQ(resolved.backend, azookey::host::BackendKind::Cpu);
}

TEST(SettingsStoreTest, EmptySelectedPathClearsExistingModelPath) {
  azookey::host::RuntimeSettings settings;
  settings.model.enabled = true;
  settings.model.selected_path.clear();

  azookey::host::EngineConfig config;
  config.model_path = "C:/models/existing.gguf";
  config.n_gpu_layers = 12;

  config = azookey::host::ApplyRuntimeSettingsToEngineConfig(config, settings);

  EXPECT_TRUE(config.model_path.empty());
  EXPECT_FALSE(config.n_gpu_layers.has_value());
}

TEST(SettingsStoreTest, AutoBackendCanUseExplicitDefaultBackend) {
  azookey::host::RuntimeSettings settings;
  settings.backend_preference = "auto";

  azookey::host::EngineConfig config;
  config.backend = azookey::host::BackendKind::Cuda;

  auto legacy_fallback = azookey::host::ApplyRuntimeSettingsToEngineConfig(config, settings);
  EXPECT_EQ(legacy_fallback.backend, azookey::host::BackendKind::Cuda);

  auto explicit_default = azookey::host::ApplyRuntimeSettingsToEngineConfig(
      config, settings, azookey::host::BackendKind::Cpu);
  EXPECT_EQ(explicit_default.backend, azookey::host::BackendKind::Cpu);
}

TEST(SettingsStoreTest, NumericInferenceSettingsRejectWrongTypesAndOutOfRangeValues) {
  ScopedTempDirectory temp("azookey_settings_inference_numeric");
  const auto& dir = temp.path();
  const auto path = dir / "settings.json";
  WriteText(path, R"({
    "inferenceThreads": -5,
    "maxCandidates": 100,
    "maxContextLength": -1
  })");

  azookey::host::SettingsStore store(path);
  auto result = store.Load();
  EXPECT_EQ(result.settings.inference_threads, 0);
  EXPECT_EQ(result.settings.max_candidates, 9);
  EXPECT_EQ(result.settings.max_context_length, 10);

  WriteText(path, R"({
    "inferenceThreads": "8",
    "maxCandidates": 3.5,
    "maxContextLength": false
  })");
  result = store.Reload();
  EXPECT_EQ(result.settings.inference_threads, 0);
  EXPECT_EQ(result.settings.max_candidates, 9);
  EXPECT_EQ(result.settings.max_context_length, 10);
}

TEST(SettingsStoreTest, AutomaticInferenceThreadsFollowPowerProfile) {
  using namespace azookey::host;
  for (const auto source : {PowerSource::Ac, PowerSource::Battery, PowerSource::Unknown}) {
    for (const unsigned int cpus : {0u, 1u, 2u, 3u, 4u, 7u, 8u, 16u}) {
      for (const std::string profile : {"auto", "performance", "battery_saver"}) {
        SCOPED_TRACE(profile + ": cpus=" + std::to_string(cpus));
        RuntimeSettings settings;
        settings.power_profile = profile;
        const auto provider = [=] { return InferenceThreadEnvironment{source, cpus}; };
        const auto config =
            ApplyRuntimeSettingsToEngineConfig({}, settings, BackendKind::Cpu, provider);
        const unsigned int target = profile == "performance"         ? 8u
                                    : profile == "battery_saver"     ? 2u
                                    : source == PowerSource::Ac      ? 8u
                                    : source == PowerSource::Battery ? 2u
                                                                     : 4u;
        const auto expected = cpus == 0 ? 1u : cpus < target ? cpus : target;
        EXPECT_EQ(config.inference_threads, static_cast<int32_t>(expected));
      }
    }
  }
}

TEST(SettingsStoreTest, ExplicitInferenceThreadsOverrideEnvironmentWithoutQueryingIt) {
  using namespace azookey::host;
  for (const std::string profile : {"auto", "performance", "battery_saver"}) {
    for (int32_t threads = 1; threads <= 8; ++threads) {
      RuntimeSettings settings;
      settings.power_profile = profile;
      settings.inference_threads = threads;
      const auto config = ApplyRuntimeSettingsToEngineConfig({}, settings, BackendKind::Cpu, [] {
        ADD_FAILURE() << "explicit thread count must not query the environment";
        return InferenceThreadEnvironment{PowerSource::Battery, 1};
      });
      EXPECT_EQ(config.inference_threads, threads);
    }
  }
}

TEST(SettingsStoreTest, ReloadResamplesPowerSourceAndHonorsNewExplicitThreads) {
  using namespace azookey::host;
  ScopedTempDirectory temp("azookey_settings_power_reload");
  const auto& dir = temp.path();
  const auto path = dir / "settings.json";
  WriteText(path, R"({"powerProfile":"auto","inferenceThreads":0})");
  SettingsStore store(path);
  PowerSource source = PowerSource::Ac;
  const auto provider = [&] { return InferenceThreadEnvironment{source, 6}; };
  const auto loaded = store.Load();
  ASSERT_EQ(loaded.status, SettingsLoadStatus::Loaded);
  auto config = ApplyRuntimeSettingsToEngineConfig({}, loaded.settings, BackendKind::Cpu, provider);
  EXPECT_EQ(config.inference_threads, 6);
  source = PowerSource::Battery;
  const auto reloaded = store.Reload();
  ASSERT_EQ(reloaded.status, SettingsLoadStatus::Loaded);
  config =
      ApplyRuntimeSettingsToEngineConfig(config, reloaded.settings, BackendKind::Cpu, provider);
  EXPECT_EQ(config.inference_threads, 2);
  WriteText(path, R"({"powerProfile":"battery_saver","inferenceThreads":8})");
  const auto explicit_settings = store.Reload();
  ASSERT_EQ(explicit_settings.status, SettingsLoadStatus::Loaded);
  config = ApplyRuntimeSettingsToEngineConfig(config, explicit_settings.settings, BackendKind::Cpu,
                                              provider);
  EXPECT_EQ(config.inference_threads, 8);
}

TEST(SettingsStoreTest, InvalidJsonIsQuarantinedAndDefaultsContinue) {
  ScopedTempDirectory temp("azookey_settings_invalid");
  const auto& dir = temp.path();
  const auto path = dir / "settings.json";
  WriteText(path, "{ invalid json");

  azookey::host::SettingsStore store(path);
  const auto result = store.Load();

  EXPECT_EQ(result.status, azookey::host::SettingsLoadStatus::Invalid);
  ASSERT_TRUE(result.error.has_value());
  ASSERT_TRUE(result.quarantined_path.has_value());
  EXPECT_FALSE(std::filesystem::exists(path));
  EXPECT_TRUE(std::filesystem::exists(*result.quarantined_path));
  EXPECT_FALSE(result.settings.live_conversion);
  EXPECT_TRUE(result.settings.prediction_enabled);
}

TEST(SettingsStoreTest, ReadFailureDoesNotQuarantineFile) {
#ifndef _WIN32
  if (geteuid() == 0) {
    GTEST_SKIP() << "root bypasses file permissions, so chmod(0) cannot simulate a read failure";
  }
#endif
  ScopedTempDirectory temp("azookey_settings_read_failure");
  const auto& dir = temp.path();
  const auto path = dir / "settings.json";
  WriteText(path, R"({"liveConversion":true})");

#ifdef _WIN32
  HANDLE exclusive = CreateFileW(path.wstring().c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(exclusive, INVALID_HANDLE_VALUE);
#else
  ASSERT_EQ(chmod(path.c_str(), 0), 0);
#endif

  azookey::host::SettingsStore store(path, std::chrono::milliseconds(20));
  const auto result = store.Load();

#ifdef _WIN32
  CloseHandle(exclusive);
#else
  ASSERT_EQ(chmod(path.c_str(), S_IRUSR | S_IWUSR), 0);
#endif

  EXPECT_EQ(result.status, azookey::host::SettingsLoadStatus::Invalid);
  EXPECT_FALSE(result.quarantined_path.has_value());
  EXPECT_TRUE(std::filesystem::exists(path));
  EXPECT_FALSE(std::filesystem::exists(path.string() + ".invalid"));
}

TEST(SettingsStoreTest, LockTimeoutLeavesInvalidFileForLaterQuarantine) {
  ScopedTempDirectory temp("azookey_settings_lock_timeout");
  const auto& dir = temp.path();
  const auto path = dir / "settings.json";
  WriteText(path, "{ invalid json");

  std::promise<bool> acquired;
  std::promise<void> release;
  auto release_future = release.get_future().share();
  std::thread holder([&] {
    auto lock =
        azookey::learning::AcquireExclusiveFileLockForPath(path, std::chrono::milliseconds(1000));
    acquired.set_value(lock.has_value());
    if (lock) release_future.wait();
  });
  const bool has_lock = acquired.get_future().get();
  if (!has_lock) {
    release.set_value();
    holder.join();
  }
  ASSERT_TRUE(has_lock);

  azookey::host::SettingsStore store(path, std::chrono::milliseconds(20));
  const auto blocked_result = store.Load();
  EXPECT_EQ(blocked_result.status, azookey::host::SettingsLoadStatus::Invalid);
  EXPECT_FALSE(blocked_result.quarantined_path.has_value());
  EXPECT_TRUE(std::filesystem::exists(path));

  release.set_value();
  holder.join();

  const auto retry_result = store.Load();
  EXPECT_EQ(retry_result.status, azookey::host::SettingsLoadStatus::Invalid);
  EXPECT_TRUE(retry_result.quarantined_path.has_value());
  EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(SettingsStoreTest, SharedLockSerializesAtomicWriterBeforeRead) {
  ScopedTempDirectory temp("azookey_settings_serialized_writer");
  const auto& dir = temp.path();
  const auto path = dir / "settings.json";
  WriteText(path, "{ invalid json");

  std::promise<bool> acquired;
  bool write_succeeded = false;
  std::thread writer([&] {
    auto lock =
        azookey::learning::AcquireExclusiveFileLockForPath(path, std::chrono::milliseconds(1000));
    acquired.set_value(lock.has_value());
    if (!lock) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    write_succeeded = azookey::learning::WriteTextFileAtomically(
        path, R"({"liveConversion":true,"predictionEnabled":true})");
  });
  const bool has_lock = acquired.get_future().get();
  if (!has_lock) writer.join();
  ASSERT_TRUE(has_lock);

  azookey::host::SettingsStore store(path, std::chrono::milliseconds(1000));
  const auto result = store.Load();
  writer.join();

  EXPECT_TRUE(write_succeeded);
  EXPECT_EQ(result.status, azookey::host::SettingsLoadStatus::Loaded);
  EXPECT_TRUE(result.settings.live_conversion);
  EXPECT_FALSE(result.quarantined_path.has_value());
  EXPECT_TRUE(std::filesystem::exists(path));
  EXPECT_FALSE(std::filesystem::exists(path.string() + ".invalid"));
}

TEST(SettingsStoreTest, InvalidReloadKeepsCurrentSettings) {
  ScopedTempDirectory temp("azookey_settings_reload_invalid");
  const auto& dir = temp.path();
  const auto path = dir / "settings.json";
  WriteText(path, R"({"liveConversion":true,"logLevel":"debug",
                      "privacy":{"mode":"normal","redactLogs":false}})");

  azookey::host::SettingsStore store(path);
  ASSERT_EQ(store.Load().status, azookey::host::SettingsLoadStatus::Loaded);
  ASSERT_TRUE(store.settings().live_conversion);
  ASSERT_EQ(store.settings().log_level, "debug");
  ASSERT_FALSE(store.settings().privacy_policy.secure);
  ASSERT_TRUE(store.settings().privacy_policy.detailed_logging_allowed);

  WriteText(path, "{ invalid json");
  const auto result = store.Reload();

  EXPECT_EQ(result.status, azookey::host::SettingsLoadStatus::Invalid);
  EXPECT_TRUE(result.settings.live_conversion);
  EXPECT_EQ(result.settings.log_level, "debug");
  EXPECT_TRUE(store.settings().live_conversion);
  EXPECT_EQ(store.settings().log_level, "debug");
  EXPECT_EQ(store.last_result().settings.log_level, "debug");
  EXPECT_TRUE(result.settings.privacy_policy.secure);
  EXPECT_FALSE(result.settings.privacy_policy.detailed_logging_allowed);
  EXPECT_TRUE(store.settings().privacy_policy.secure);
  EXPECT_FALSE(store.settings().privacy_policy.detailed_logging_allowed);

  WriteText(path, R"({"privacy":{"mode":"normal","redactLogs":false}})");
  const auto restored = store.Reload();
  EXPECT_FALSE(restored.settings.privacy_policy.secure);
  EXPECT_TRUE(restored.settings.privacy_policy.detailed_logging_allowed);
}

TEST(SettingsStoreTest, TypoAndAutoWordKeysReachTheEngineConfig) {
  ScopedTempDirectory temp("azookey_settings_typo_auto_word");
  const auto& dir = temp.path();
  const auto path = dir / "settings.json";
  WriteText(path, R"({
    "typoCorrectionMode": "auto_replace",
    "typoMinCount": 5,
    "autoWordRegistration": {
      "miningEnabled": false,
      "trendingEnabled": true,
      "registrationMode": "auto",
      "miningMinCount": 7,
      "trendingIntervalHours": 6
    }
  })");

  azookey::host::SettingsStore store(path);
  const auto result = store.Load();
  ASSERT_EQ(result.status, azookey::host::SettingsLoadStatus::Loaded);
  EXPECT_EQ(store.settings().typo_correction_mode, "auto_replace");
  EXPECT_EQ(store.settings().typo_min_count, 5);
  EXPECT_FALSE(store.settings().auto_word.mining_enabled);
  EXPECT_TRUE(store.settings().auto_word.trending_enabled);
  EXPECT_EQ(store.settings().auto_word.registration_mode, "auto");
  EXPECT_EQ(store.settings().auto_word.mining_min_count, 7);
  EXPECT_EQ(store.settings().auto_word.trending_interval_hours, 6);

  const auto config = azookey::host::ApplyRuntimeSettingsToEngineConfig(
      azookey::host::EngineConfig{}, store.settings());
  EXPECT_EQ(config.typo_correction_mode, "auto_replace");
  EXPECT_EQ(config.typo_min_count, 5u);
  EXPECT_FALSE(config.auto_word_mining_enabled);
  EXPECT_TRUE(config.auto_word_trending_enabled);
  EXPECT_EQ(config.auto_word_trending_interval_hours, 6u);
  // registrationMode "auto" is what turns on automatic promotion.
  EXPECT_TRUE(config.auto_word_auto_register);
  EXPECT_EQ(config.auto_word_min_count, 7u);
}

TEST(SettingsStoreTest, TypoAndAutoWordDefaultsHoldAndInvalidValuesAreIgnored) {
  ScopedTempDirectory temp("azookey_settings_typo_auto_word_invalid");
  const auto& dir = temp.path();
  const auto path = dir / "settings.json";
  WriteText(path, R"({
    "typoCorrectionMode": "aggressive",
    "typoMinCount": 0,
    "autoWordRegistration": {
      "registrationMode": "whenever",
      "miningMinCount": 9999,
      "trendingIntervalHours": 8761
    }
  })");

  azookey::host::SettingsStore store(path);
  ASSERT_EQ(store.Load().status, azookey::host::SettingsLoadStatus::Loaded);
  // An out-of-range or unknown value falls back to the documented default
  // rather than to an arbitrary one.
  EXPECT_EQ(store.settings().typo_correction_mode, "suggest");
  EXPECT_EQ(store.settings().typo_min_count, 3);
  EXPECT_EQ(store.settings().auto_word.registration_mode, "confirm");
  EXPECT_EQ(store.settings().auto_word.mining_min_count, 3);

  const auto config = azookey::host::ApplyRuntimeSettingsToEngineConfig(
      azookey::host::EngineConfig{}, store.settings());
  // "confirm" is the safe default: nothing is registered without the user.
  EXPECT_FALSE(config.auto_word_auto_register);
  EXPECT_FALSE(config.auto_word_trending_enabled);
  EXPECT_EQ(config.auto_word_trending_interval_hours, 24u);
}

TEST(SettingsStoreTest, OfflineModeSuppressesTrendingWithoutChangingRegistrationPreference) {
  ScopedTempDirectory temp("azookey_settings_offline_trending");
  const auto path = temp.path() / "settings.json";
  WriteText(path, R"({"privacy":{"mode":"offline"},"autoWordRegistration":{
    "trendingEnabled":true,"trendingIntervalHours":6,"registrationMode":"auto"}})");
  azookey::host::SettingsStore store(path);
  ASSERT_EQ(store.Load().status, azookey::host::SettingsLoadStatus::Loaded);
  EXPECT_TRUE(store.settings().offline_mode);
  EXPECT_TRUE(store.settings().auto_word.trending_enabled);
  auto config = azookey::host::ApplyRuntimeSettingsToEngineConfig({}, store.settings());
  EXPECT_FALSE(config.auto_word_trending_enabled);
  EXPECT_EQ(config.auto_word_trending_interval_hours, 6u);
  EXPECT_TRUE(config.auto_word_auto_register);
  EXPECT_TRUE(config.auto_word_mining_enabled);

  // Private mode limits user-data transmission, not public dictionary downloads.
  for (const auto* privacy :
       {R"("privacy":{"mode":"normal"},)", R"("privacy":{"mode":"private"},)", ""}) {
    WriteText(path,
              std::string("{") + privacy + R"("autoWordRegistration":{"trendingEnabled":true}})");
    ASSERT_EQ(store.Reload().status, azookey::host::SettingsLoadStatus::Loaded);
    EXPECT_FALSE(store.settings().offline_mode);
    EXPECT_TRUE(store.settings().auto_word.trending_enabled);
    config = azookey::host::ApplyRuntimeSettingsToEngineConfig(config, store.settings());
    EXPECT_TRUE(config.auto_word_trending_enabled);
  }
}

TEST(SettingsStoreTest, TheShippedSampleMatchesTheParsedDefaults) {
  ScopedTempDirectory temp("azookey_settings_typo_auto_word_empty");
  const auto& dir = temp.path();
  const auto path = dir / "settings.json";
  WriteText(path, "{}");

  azookey::host::SettingsStore store(path);
  ASSERT_EQ(store.Load().status, azookey::host::SettingsLoadStatus::Loaded);
  // These are the values settings/default-settings.sample.json ships.
  EXPECT_FALSE(store.settings().inline_english_candidates);
  EXPECT_TRUE(store.settings().english.case_variants);
  EXPECT_FALSE(store.settings().english.full_width);
  EXPECT_EQ(store.settings().english.min_length, 2u);
  EXPECT_DOUBLE_EQ(store.settings().english.promote_threshold, 0.6);
  EXPECT_FALSE(store.settings().english.dictionary_enabled);
  EXPECT_EQ(store.settings().english.dictionary_path,
            "%LOCALAPPDATA%\\azooKey\\dict\\english-words.tsv");
  EXPECT_EQ(store.settings().typo_correction_mode, "suggest");
  EXPECT_EQ(store.settings().typo_min_count, 3);
  EXPECT_TRUE(store.settings().auto_word.mining_enabled);
  EXPECT_FALSE(store.settings().auto_word.trending_enabled);
  EXPECT_EQ(store.settings().auto_word.registration_mode, "confirm");
  EXPECT_EQ(store.settings().auto_word.mining_min_count, 3);
  EXPECT_EQ(store.settings().auto_word.trending_interval_hours, 24);
  const auto config = azookey::host::ApplyRuntimeSettingsToEngineConfig({}, store.settings());
  EXPECT_FALSE(config.auto_word_trending_enabled);
  EXPECT_EQ(config.auto_word_trending_interval_hours, 24u);
}

TEST(SettingsStoreTest, DictionaryLayerDefaultsIgnoreUnknownAndMistypedValues) {
  ScopedTempDirectory temp("azookey_settings_dictionary_defaults");
  const auto path = temp.path() / "settings.json";
  for (const auto* json :
       {"{}", R"({"dictionary":{}})", R"({"dictionary":false})", R"({"dictionary":null})",
        R"({"dictionary":[]})",
        R"({"dictionary":{"sudachiEnabled":"false","neologdEnabled":1,
                "namedEntityEnabled":null,"technicalTermsEnabled":{},
                "userDictionaryEnabled":[],"autoWordsEnabled":"true",
                "appSpecificDictionaryEnabled":0}})",
        R"({"dictionary":{"unknown":false,"baseEnabled":false,"verifyOnLoad":true,
                "categoryBoosts":{"technical":1.2}}})"}) {
    SCOPED_TRACE(json);
    WriteText(path, json);
    azookey::host::SettingsStore store(path);
    ASSERT_EQ(store.Load().status, azookey::host::SettingsLoadStatus::Loaded);
    const auto& dictionary = store.settings().dictionary;
    EXPECT_TRUE(dictionary.sudachi_enabled);
    EXPECT_FALSE(dictionary.neologd_enabled);
    EXPECT_TRUE(dictionary.named_entity_enabled);
    EXPECT_TRUE(dictionary.technical_terms_enabled);
    EXPECT_TRUE(dictionary.user_dictionary_enabled);
    EXPECT_TRUE(dictionary.auto_words_enabled);
    EXPECT_TRUE(dictionary.app_specific_dictionary_enabled);
  }
}

TEST(SettingsStoreTest, DictionaryLayerSwitchesParseApplyAndResetOnReload) {
  ScopedTempDirectory temp("azookey_settings_dictionary_reload");
  const auto path = temp.path() / "settings.json";
  WriteText(path, R"({"dictionary":{
    "sudachiEnabled":false,"neologdEnabled":true,"namedEntityEnabled":false,
    "technicalTermsEnabled":false,"userDictionaryEnabled":false,"autoWordsEnabled":false,
    "appSpecificDictionaryEnabled":false}})");
  azookey::host::SettingsStore store(path);
  ASSERT_EQ(store.Load().status, azookey::host::SettingsLoadStatus::Loaded);
  auto config = azookey::host::ApplyRuntimeSettingsToEngineConfig({}, store.settings());
  EXPECT_FALSE(config.dictionary.sudachi_enabled);
  EXPECT_TRUE(config.dictionary.neologd_enabled);
  EXPECT_FALSE(config.dictionary.named_entity_enabled);
  EXPECT_FALSE(config.dictionary.technical_terms_enabled);
  EXPECT_FALSE(config.dictionary.user_dictionary_enabled);
  EXPECT_FALSE(config.dictionary.auto_words_enabled);
  EXPECT_FALSE(config.dictionary.app_specific_dictionary_enabled);

  WriteText(path, "{}");
  ASSERT_EQ(store.Reload().status, azookey::host::SettingsLoadStatus::Loaded);
  config = azookey::host::ApplyRuntimeSettingsToEngineConfig(config, store.settings());
  EXPECT_TRUE(config.dictionary.sudachi_enabled);
  EXPECT_FALSE(config.dictionary.neologd_enabled);
  EXPECT_TRUE(config.dictionary.named_entity_enabled);
  EXPECT_TRUE(config.dictionary.technical_terms_enabled);
  EXPECT_TRUE(config.dictionary.user_dictionary_enabled);
  EXPECT_TRUE(config.dictionary.auto_words_enabled);
  EXPECT_TRUE(config.dictionary.app_specific_dictionary_enabled);
}

TEST(SettingsStoreTest, PrivacyPublicationWaitsForActiveLearningGuard) {
  using namespace std::chrono_literals;
  ScopedTempDirectory temp("azookey_settings_privacy_publication");
  const auto& dir = temp.path();
  const auto path = dir / "settings.json";
  WriteText(path, R"({"privacy":{"mode":"normal"}})");
  azookey::host::SettingsStore store(path);
  store.Load();
  auto guard = store.LockPrivacyPolicy();
  EXPECT_FALSE(guard.policy.secure);
  WriteText(path, R"({"privacy":{"mode":"secure"}})");
  std::promise<void> started;
  auto entered = started.get_future();
  auto reload = std::async(std::launch::async, [&] {
    started.set_value();
    return store.Reload();
  });
  entered.wait();
  EXPECT_EQ(reload.wait_for(100ms), std::future_status::timeout);
  EXPECT_FALSE(guard.policy.secure);
  guard.lock.unlock();
  const auto result = reload.get();
  EXPECT_TRUE(result.settings.privacy_policy.secure);
  EXPECT_TRUE(store.LockPrivacyPolicy().policy.secure);
  WriteText(path, R"({"privacy":{"mode":"normal"}})");
  store.Reload();
  EXPECT_FALSE(store.LockPrivacyPolicy().policy.secure);
  WriteText(path, "invalid json");
  store.Reload();
  EXPECT_TRUE(store.LockPrivacyPolicy().policy.secure);
}

// M47 section 8.5.3: SafeMode turns AI and learning off over whatever the rest
// of the file asks for, and the engine config follows.
TEST(SettingsStoreTest, SafeModeOverridesModelAiAndLearning) {
  ScopedTempDirectory temp("azookey_settings_safe_mode_overrides");
  const auto& dir = temp.path();
  const auto path = dir / "settings.json";
  const std::string rest =
      R"("aiBackend":"openai","llmMagicConversion":true,"batchConversionMode":"ai-cleanup",)"
      R"("privacy":{"mode":"normal","redactLogs":false,"aiEnabled":true,"externalAiEnabled":true},)"
      R"("reranker":{"nllRerankEnabled":true},"typoCorrectionMode":"auto_replace",)"
      R"("autoWordRegistration":{"miningEnabled":true,"trendingEnabled":true},)"
      R"("model":{"enabled":true,"selectedPath":"C:/models/zenzai.gguf","autoLoadOnHostStart":true})";
  WriteText(path, "{" + rest + "}");
  azookey::host::SettingsStore store(path);
  ASSERT_EQ(store.Load().status, azookey::host::SettingsLoadStatus::Loaded);
  EXPECT_FALSE(store.settings().safe_mode.enabled);
  EXPECT_TRUE(store.settings().model.enabled);
  EXPECT_FALSE(store.settings().privacy_policy.secure);

  WriteText(path, "{" + rest +
                      R"(,"safeMode":{"enabled":true,"enteredAt":"2026-09-22T01:02:03Z",)"
                      R"("lastCrashCount":3}})");
  ASSERT_EQ(store.Reload().status, azookey::host::SettingsLoadStatus::Loaded);
  const auto& settings = store.settings();
  EXPECT_TRUE(settings.safe_mode.enabled);
  EXPECT_EQ(settings.safe_mode.entered_at, "2026-09-22T01:02:03Z");
  EXPECT_EQ(settings.safe_mode.last_crash_count, 3);
  EXPECT_FALSE(settings.model.enabled);
  EXPECT_FALSE(settings.model.auto_load_on_host_start);
  EXPECT_FALSE(settings.nll.enabled);
  EXPECT_FALSE(settings.llm_magic_conversion);
  EXPECT_EQ(settings.ai_backend, "none");
  EXPECT_EQ(settings.batch_conversion_mode, "neural");
  EXPECT_FALSE(settings.ai_privacy.ai);
  EXPECT_FALSE(settings.ai_privacy.external);
  EXPECT_TRUE(settings.privacy_policy.secure);
  EXPECT_TRUE(store.LockPrivacyPolicy().policy.secure);
  EXPECT_EQ(settings.typo_correction_mode, "off");
  EXPECT_FALSE(settings.auto_word.mining_enabled);
  EXPECT_FALSE(settings.auto_word.trending_enabled);

  azookey::host::EngineConfig base;
  base.model_path = "C:/models/zenzai.gguf";
  base.auto_word_trending_enabled = true;
  const auto config = azookey::host::ApplyRuntimeSettingsToEngineConfig(base, settings);
  EXPECT_TRUE(config.model_path.empty());
  EXPECT_FALSE(config.nll.enabled);
  EXPECT_FALSE(config.auto_word_mining_enabled);
  EXPECT_FALSE(config.auto_word_trending_enabled);
  EXPECT_EQ(config.typo_correction_mode, "off");
}

TEST(SettingsStoreTest, SafeModeDefaultsOffAndIgnoresMistypedValues) {
  ScopedTempDirectory temp("azookey_settings_safe_mode_defaults");
  const auto& dir = temp.path();
  const auto path = dir / "settings.json";
  WriteText(path, R"({"safeMode":{"enabled":"yes","enteredAt":7,"lastCrashCount":-1}})");
  azookey::host::SettingsStore store(path);
  ASSERT_EQ(store.Load().status, azookey::host::SettingsLoadStatus::Loaded);
  EXPECT_FALSE(store.settings().safe_mode.enabled);
  EXPECT_TRUE(store.settings().safe_mode.entered_at.empty());
  EXPECT_EQ(store.settings().safe_mode.last_crash_count, 0);
  EXPECT_TRUE(store.settings().model.enabled);
}

TEST(SettingsStoreTest, PersistSafeModeKeepsOtherKeysAndLoadsBackEnabled) {
  ScopedTempDirectory temp("azookey_settings_safe_mode_persist");
  const auto& dir = temp.path();
  const auto path = dir / "settings.json";
  WriteText(path, R"({"maxCandidates":5,"model":{"selectedPath":"a.gguf"},"unknownKey":[1,2]})");
  azookey::host::SettingsStore store(path);
  ASSERT_TRUE(store.PersistSafeModeEntered("2026-09-22T01:02:03Z", 3));
  ASSERT_EQ(store.Load().status, azookey::host::SettingsLoadStatus::Loaded);
  EXPECT_TRUE(store.settings().safe_mode.enabled);
  EXPECT_EQ(store.settings().safe_mode.entered_at, "2026-09-22T01:02:03Z");
  EXPECT_EQ(store.settings().safe_mode.last_crash_count, 3);
  EXPECT_EQ(store.settings().max_candidates, 5);
  EXPECT_EQ(store.settings().model.selected_path, "a.gguf");

  EXPECT_NE(ReadText(path).find(R"("unknownKey":[1,2])"), std::string::npos) << ReadText(path);
}

TEST(SettingsStoreTest, PersistSafeModeCreatesAMissingFile) {
  ScopedTempDirectory temp("azookey_settings_safe_mode_missing");
  const auto& dir = temp.path();
  const auto path = dir / "config" / "settings.json";
  std::filesystem::create_directories(path.parent_path());
  azookey::host::SettingsStore store(path);
  ASSERT_TRUE(store.PersistSafeModeEntered("2026-09-22T01:02:03Z", 4));
  ASSERT_EQ(store.Load().status, azookey::host::SettingsLoadStatus::Loaded);
  EXPECT_TRUE(store.settings().safe_mode.enabled);
  EXPECT_EQ(store.settings().safe_mode.last_crash_count, 4);
}

TEST(SettingsStoreTest, PersistSafeModeLeavesAnUnparsableFileAlone) {
  ScopedTempDirectory temp("azookey_settings_safe_mode_invalid");
  const auto& dir = temp.path();
  const auto path = dir / "settings.json";
  WriteText(path, "{ not json");
  azookey::host::SettingsStore store(path);
  EXPECT_FALSE(store.PersistSafeModeEntered("2026-09-22T01:02:03Z", 3));
  EXPECT_EQ(ReadText(path), "{ not json");
}

TEST(SettingsStoreTest, BenchmarkHistoryRetainsSevenResultsAndLatestDiskSettings) {
  ScopedTempDirectory temp("azookey_benchmark_history");
  const auto settings_path = temp.path() / "settings.json";
  const auto model_path = azookey::core::PathToUtf8(temp.path() / "model.gguf");
  WriteText(settings_path, R"({"maxCandidates":5,"model":{"selectedPath":"old.gguf"}})");
  azookey::host::SettingsStore store(settings_path);
  ASSERT_EQ(store.Load().status, azookey::host::SettingsLoadStatus::Loaded);
  // A different writer changed the file after Load; never merge into the snapshot.
  WriteText(settings_path, R"({"maxCandidates":9,"safeMode":{"enabled":true},
      "unknownRoot":[1],"model":{"selectedPath":"new.gguf","unknownModel":true,
      "benchmarkHistory":[{"model":"legacy","status":"success"},false]}})");
  azookey::ipc::BenchmarkModelResponse response;
  response.backend = "cpu";
  response.p50_ms = 1;
  response.p95_ms = 2;
  response.p99_ms = 3;
  response.load_ms = 4;
  response.rss_mb = 5;
  response.vram_mb = 6;
  for (uint32_t i = 0; i < 9; ++i) {
    response.status = i % 3 == 0 ? "success" : i % 3 == 1 ? "timeout" : "error";
    response.error =
        response.status == "error" ? std::optional<std::string>{"load_failed"} : std::nullopt;
    response.iterations_completed = i;
    ASSERT_TRUE(store.PersistBenchmarkResult(model_path, "2026-10-10T10:00:00Z", response));
  }
  const auto document = azookey::ipc::json::Parse(ReadText(settings_path));
  ASSERT_TRUE(document);
  EXPECT_EQ(document->GetInt("maxCandidates"), 9);
  EXPECT_TRUE(document->Find("unknownRoot"));
  ASSERT_TRUE(document->FindObject("safeMode"));
  EXPECT_TRUE(document->FindObject("safeMode")->at("enabled").AsBool());
  const auto* model = document->Find("model");
  ASSERT_TRUE(model);
  EXPECT_EQ(model->GetString("selectedPath"), "new.gguf");
  EXPECT_EQ(model->GetBool("unknownModel"), true);
  const auto* history = model->GetArray("benchmarkHistory");
  ASSERT_TRUE(history);
  ASSERT_EQ(history->size(), 7u);
  for (size_t i = 0; i < history->size(); ++i) {
    EXPECT_EQ((*history)[i].GetUInt("iterations_completed"), i + 2);
    EXPECT_EQ((*history)[i].GetString("path"), model_path);
    EXPECT_EQ((*history)[i].GetString("completedAt"), "2026-10-10T10:00:00Z");
  }
  const auto& last = history->back();
  EXPECT_EQ(last.GetString("status"), "error");
  EXPECT_EQ(last.GetString("error"), "load_failed");
  EXPECT_EQ(last.GetNumber("p50_ms"), 1);
  EXPECT_EQ(last.GetNumber("p95_ms"), 2);
  EXPECT_EQ(last.GetNumber("p99_ms"), 3);
  EXPECT_EQ(last.GetNumber("load_ms"), 4);
  EXPECT_EQ(last.GetNumber("rss_mb"), 5);
  EXPECT_EQ(last.GetNumber("vram_mb"), 6);
  EXPECT_EQ(store.settings().max_candidates, 5);  // No runtime reload.
}

TEST(SettingsStoreTest, BenchmarkHistoryCreatesMissingFileAndPreservesNullMetrics) {
  ScopedTempDirectory temp("azookey_benchmark_missing");
  const auto path = temp.path() / "config" / "settings.json";
  azookey::host::SettingsStore store(path);
  azookey::ipc::BenchmarkModelResponse response;
  response.backend = "cpu";
  response.status = "success";
  ASSERT_TRUE(store.PersistBenchmarkResult(azookey::core::PathToUtf8(temp.path() / "model.gguf"),
                                           "2026-10-10T10:00:00Z", response));
  const auto document = azookey::ipc::json::Parse(ReadText(path));
  ASSERT_TRUE(document && document->Find("model"));
  const auto* history = document->Find("model")->GetArray("benchmarkHistory");
  ASSERT_TRUE(history);
  ASSERT_EQ(history->size(), 1u);
  EXPECT_TRUE(history->front().Find("vram_mb")->IsNull());
  EXPECT_TRUE(history->front().Find("error")->IsNull());
}

TEST(SettingsStoreTest, BenchmarkHistoryLeavesUnreadableAndInvalidDocumentsAlone) {
  ScopedTempDirectory temp("azookey_benchmark_invalid");
  const auto path = temp.path() / "settings.json";
  const auto model_path = azookey::core::PathToUtf8(temp.path() / "model.gguf");
  azookey::host::SettingsStore store(path);
  azookey::ipc::BenchmarkModelResponse response;
  response.backend = "cpu";
  response.status = "success";
  for (const auto* text : {"{ not json", "[]", R"({"model":false})"}) {
    WriteText(path, text);
    EXPECT_FALSE(store.PersistBenchmarkResult(model_path, "2026-10-10T10:00:00Z", response));
    EXPECT_EQ(ReadText(path), text);
    EXPECT_FALSE(std::filesystem::exists(path.string() + ".invalid"));
  }
  std::filesystem::remove(path);
  std::filesystem::create_directory(path);
  EXPECT_FALSE(store.PersistBenchmarkResult(model_path, "2026-10-10T10:00:00Z", response));
  EXPECT_TRUE(std::filesystem::is_directory(path));
}

TEST(SettingsStoreTest, BenchmarkHistoryRejectsInvalidNewEntriesWithoutWriting) {
  ScopedTempDirectory temp("azookey_benchmark_entry_invalid");
  const auto path = temp.path() / "settings.json";
  const auto model_path = azookey::core::PathToUtf8(temp.path() / "model.gguf");
  WriteText(path, "{}");
  azookey::host::SettingsStore store(path);
  azookey::ipc::BenchmarkModelResponse response;
  response.backend = "cpu";
  response.status = "success";
  EXPECT_FALSE(store.PersistBenchmarkResult("relative.gguf", "2026-10-10T10:00:00Z", response));
  for (const auto* timestamp :
       {"0000-10-10T10:00:00Z", "2026-02-29T10:00:00Z", "2026-02-30T10:00:00Z",
        "2026-10-10T24:00:00Z", "2026-10-10T10:60:00Z", "2026-10-10T10:00:60Z",
        "2026-10-10T10:00:00+00:00"}) {
    EXPECT_FALSE(store.PersistBenchmarkResult(model_path, timestamp, response)) << timestamp;
  }
  response.p50_ms = -1;
  EXPECT_FALSE(store.PersistBenchmarkResult(model_path, "2026-10-10T10:00:00Z", response));
  response.p50_ms = 0;
  response.status = "unknown";
  EXPECT_FALSE(store.PersistBenchmarkResult(model_path, "2026-10-10T10:00:00Z", response));
  EXPECT_EQ(ReadText(path), "{}");
  response.status = "success";
  EXPECT_TRUE(store.PersistBenchmarkResult(model_path, "2024-02-29T23:59:59Z", response));
}

TEST(SettingsStoreTest, BenchmarkHistoryExcludesAdmissionRejectionsButRecordsExecutedErrors) {
  ScopedTempDirectory temp("azookey_benchmark_admission");
  const auto path = temp.path() / "settings.json";
  const auto model_path = azookey::core::PathToUtf8(temp.path() / "model.gguf");
  azookey::host::SettingsStore store(path);
  azookey::ipc::BenchmarkModelResponse response;
  response.backend = "cpu";
  for (const auto* error : {"invalid_request", "unsupported_backend", "invalid_model", "busy"}) {
    response.error = error;
    EXPECT_FALSE(store.PersistBenchmarkResult(model_path, "2026-10-10T10:00:00Z", response));
    EXPECT_FALSE(std::filesystem::exists(path));
  }
  for (const auto* error : {"load_failed", "benchmark_failed"}) {
    response.error = error;
    ASSERT_TRUE(store.PersistBenchmarkResult(model_path, "2026-10-10T10:00:00Z", response));
  }
  const auto original = ReadText(path);
  for (const auto* error : {"invalid_request", "unsupported_backend", "invalid_model", "busy"}) {
    response.error = error;
    EXPECT_FALSE(store.PersistBenchmarkResult(model_path, "2026-10-10T10:00:00Z", response));
    EXPECT_EQ(ReadText(path), original);
  }
  const auto document = azookey::ipc::json::Parse(original);
  ASSERT_TRUE(document && document->Find("model"));
  const auto* history = document->Find("model")->GetArray("benchmarkHistory");
  ASSERT_TRUE(history);
  ASSERT_EQ(history->size(), 2u);
  EXPECT_EQ(history->front().GetString("error"), "load_failed");
  EXPECT_EQ(history->back().GetString("error"), "benchmark_failed");
}

TEST(SettingsStoreTest, BenchmarkHistoryLockTimeoutLeavesOriginalFileAlone) {
  ScopedTempDirectory temp("azookey_benchmark_locked");
  const auto path = temp.path() / "settings.json";
  WriteText(path, "{}");
  std::promise<bool> acquired;
  std::promise<void> release;
  auto released = release.get_future();
  std::thread holder([&] {
    const auto lock = azookey::learning::AcquireExclusiveFileLockForPath(path);
    acquired.set_value(lock.has_value());
    released.wait();
  });
  const bool locked = acquired.get_future().get();
  azookey::host::SettingsStore store(path, std::chrono::milliseconds(0));
  azookey::ipc::BenchmarkModelResponse response;
  response.backend = "cpu";
  response.status = "success";
  const bool saved = store.PersistBenchmarkResult(azookey::core::PathToUtf8(temp.path() / "model"),
                                                  "2026-10-10T10:00:00Z", response);
  release.set_value();
  holder.join();
  ASSERT_TRUE(locked);
  EXPECT_FALSE(saved);
  EXPECT_EQ(ReadText(path), "{}");
}

TEST(SettingsStoreTest, BenchmarkHistoryConcurrentWritersRetainBothResults) {
  ScopedTempDirectory temp("azookey_benchmark_concurrent");
  const auto path = temp.path() / "settings.json";
  const auto model_path = azookey::core::PathToUtf8(temp.path() / "model.gguf");
  azookey::host::SettingsStore first(path);
  azookey::host::SettingsStore second(path);
  std::promise<void> start;
  const auto ready = start.get_future().share();
  const auto write = [&](azookey::host::SettingsStore& store, const char* backend) {
    ready.wait();
    azookey::ipc::BenchmarkModelResponse response;
    response.backend = backend;
    response.status = "success";
    return store.PersistBenchmarkResult(model_path, "2026-10-10T10:00:00Z", response);
  };
  auto a = std::async(std::launch::async, [&] { return write(first, "cpu"); });
  auto b = std::async(std::launch::async, [&] { return write(second, "vulkan"); });
  start.set_value();
  EXPECT_TRUE(a.get());
  EXPECT_TRUE(b.get());
  const auto document = azookey::ipc::json::Parse(ReadText(path));
  ASSERT_TRUE(document && document->Find("model"));
  const auto* history = document->Find("model")->GetArray("benchmarkHistory");
  ASSERT_TRUE(history);
  ASSERT_EQ(history->size(), 2u);
  EXPECT_NE(history->front().GetString("backend"), history->back().GetString("backend"));
}

TEST(SettingsStoreTest, BenchmarkHistoryDropsMalformedCanonicalRowsOnAppend) {
  ScopedTempDirectory temp("azookey_benchmark_bad_rows");
  const auto path = temp.path() / "settings.json";
  const auto model_path = azookey::core::PathToUtf8(temp.path() / "model.gguf");
  azookey::host::SettingsStore store(path);
  azookey::ipc::BenchmarkModelResponse response;
  response.backend = "cpu";
  response.status = "success";
  ASSERT_TRUE(store.PersistBenchmarkResult(model_path, "2026-10-10T10:00:00Z", response));
  const auto parsed = azookey::ipc::json::Parse(ReadText(path));
  ASSERT_TRUE(parsed && parsed->Find("model"));
  const auto* history = parsed->Find("model")->GetArray("benchmarkHistory");
  ASSERT_TRUE(history && !history->empty());
  const auto canonical = history->front().AsObject();
  azookey::ipc::json::Array rows{azookey::ipc::json::Value(canonical)};
  for (const auto& [key, value] : azookey::ipc::json::Object{{"completedAt", "not-a-date"},
                                                             {"status", "unknown"},
                                                             {"p95_ms", -1},
                                                             {"vram_mb", "invalid"},
                                                             {"iterations_completed", 1.5},
                                                             {"error", false},
                                                             {"extra", true}}) {
    auto malformed = canonical;
    malformed[key] = value;
    rows.emplace_back(std::move(malformed));
  }
  auto missing = canonical;
  missing.erase("load_ms");
  rows.emplace_back(std::move(missing));
  auto model = parsed->Find("model")->AsObject();
  model["benchmarkHistory"] = azookey::ipc::json::Value(std::move(rows));
  auto root = parsed->AsObject();
  root["model"] = azookey::ipc::json::Value(std::move(model));
  WriteText(path, azookey::ipc::json::Stringify(azookey::ipc::json::Value(std::move(root))));
  ASSERT_TRUE(store.PersistBenchmarkResult(model_path, "2026-10-10T10:01:00Z", response));
  const auto saved = azookey::ipc::json::Parse(ReadText(path));
  ASSERT_TRUE(saved && saved->Find("model"));
  const auto* kept = saved->Find("model")->GetArray("benchmarkHistory");
  ASSERT_TRUE(kept);
  ASSERT_EQ(kept->size(), 2u);
  EXPECT_EQ(kept->front().GetString("completedAt"), "2026-10-10T10:00:00Z");
  EXPECT_EQ(kept->back().GetString("completedAt"), "2026-10-10T10:01:00Z");
}

#ifdef _WIN32
TEST(SettingsStoreTest, BenchmarkHistoryReplacementFailurePreservesOriginal) {
  ScopedTempDirectory temp("azookey_benchmark_replace_failure");
  const auto path = temp.path() / "settings.json";
  WriteText(path, "{}");
  // Reads remain possible, but replacement needs FILE_SHARE_DELETE.
  const HANDLE held = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(held, INVALID_HANDLE_VALUE);
  azookey::host::SettingsStore store(path);
  azookey::ipc::BenchmarkModelResponse response;
  response.backend = "cpu";
  response.status = "success";
  const bool saved = store.PersistBenchmarkResult(azookey::core::PathToUtf8(temp.path() / "model"),
                                                  "2026-10-10T10:00:00Z", response);
  CloseHandle(held);
  EXPECT_FALSE(saved);
  EXPECT_EQ(ReadText(path), "{}");
  for (const auto& file : std::filesystem::directory_iterator(temp.path())) {
    EXPECT_EQ(file.path(), path);  // No orphaned atomic-write temporary file.
  }
}
#endif
