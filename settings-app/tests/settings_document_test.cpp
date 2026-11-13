#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

#include "SettingsDocument.h"
#include "azookey/ipc/Json.h"
#include "azookey/learning/DpapiCrypto.h"
#include "azookey/learning/FileLock.h"

namespace {

class ScopedTempDirectory {
 public:
  explicit ScopedTempDirectory(const char* name) {
    for (int attempt = 0; attempt < 16; ++attempt) {
      path_ = std::filesystem::temp_directory_path() /
              (std::string(name) + "_" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
               std::to_string(next_id_++));
      if (std::filesystem::create_directory(path_)) return;
    }
    throw std::runtime_error("Could not create a unique settings document test directory");
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

std::filesystem::path TestDir(const char* name) {
  auto path = std::filesystem::temp_directory_path() / name;
  std::filesystem::remove_all(path);
  std::filesystem::create_directories(path);
  return path;
}

void WriteText(const std::filesystem::path& path, const std::string& content) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << content;
}

std::string ReadText(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  std::ostringstream output;
  output << input.rdbuf();
  return output.str();
}

}  // namespace

#ifdef _WIN32
TEST(SettingsDocumentTest, ApiKeyInputIsProtectedAndShownOnlyAsEditablePlaintext) {
  const auto dir = TestDir("azookey_settings_api_key_protect");
  const auto path = dir / "settings.json";
  azookey::settings::EditableSettings settings;
  settings.openai_api_key = "test-api-key";
  settings.openai_api_key_changed = true;
  ASSERT_TRUE(azookey::settings::SaveSettingsDocument(path, settings).ok);
  const auto text = ReadText(path);
  EXPECT_EQ(text.find("test-api-key"), std::string::npos);
  const auto parsed = azookey::ipc::json::Parse(text);
  ASSERT_TRUE(parsed);
  const auto stored = parsed->GetString("openAiApiKey");
  ASSERT_TRUE(stored);
  EXPECT_EQ(stored->rfind("dpapi:", 0), 0u);
  const auto loaded = azookey::settings::LoadSettingsDocument(path);
  EXPECT_EQ(loaded.settings.openai_api_key, "test-api-key");
  EXPECT_FALSE(loaded.settings.openai_api_key_unavailable);
  std::filesystem::remove_all(dir);
}

TEST(SettingsDocumentTest, LegacyApiKeyMigratesAndCorruptCipherIsPreservedUntilReplaced) {
  const auto dir = TestDir("azookey_settings_api_key_migrate");
  const auto path = dir / "settings.json";
  WriteText(path, R"({"openAiApiKey":"legacy-key"})");
  auto loaded = azookey::settings::LoadSettingsDocument(path);
  ASSERT_TRUE(azookey::settings::SaveSettingsDocument(path, loaded.settings).ok);
  EXPECT_EQ(ReadText(path).find("legacy-key"), std::string::npos);

  WriteText(path, R"({"openAiApiKey":"dpapi:bad!"})");
  loaded = azookey::settings::LoadSettingsDocument(path);
  EXPECT_TRUE(loaded.settings.openai_api_key_unavailable);
  ASSERT_TRUE(azookey::settings::SaveSettingsDocument(path, loaded.settings).ok);
  EXPECT_NE(ReadText(path).find("dpapi:bad!"), std::string::npos);
  loaded.settings.openai_api_key_changed = true;
  loaded.settings.openai_api_key = "replacement-key";
  ASSERT_TRUE(azookey::settings::SaveSettingsDocument(path, loaded.settings).ok);
  EXPECT_EQ(azookey::settings::LoadSettingsDocument(path).settings.openai_api_key,
            "replacement-key");
  std::filesystem::remove_all(dir);
}
#endif

TEST(SettingsDocumentTest, BodyLogPolicySurvivesUnrelatedSettingsSave) {
  const auto dir = TestDir("azookey_settings_body_log_preserve");
  const auto path = dir / "settings.json";
  WriteText(
      path,
      R"({"privacy":{"mode":"custom","redactLogs":false,"custom":{"learning":true,"detailedLogging":true,"externalAi":false}}})");
  auto loaded = azookey::settings::LoadSettingsDocument(path);
  EXPECT_TRUE(loaded.warnings.empty());
  loaded.settings.log_level = "warn";
  ASSERT_TRUE(azookey::settings::SaveSettingsDocument(path, loaded.settings).ok);
  const auto parsed = azookey::ipc::json::Parse(ReadText(path));
  ASSERT_TRUE(parsed);
  const auto* privacy = parsed->Find("privacy");
  ASSERT_NE(privacy, nullptr);
  EXPECT_EQ(privacy->GetString("mode"), "custom");
  EXPECT_EQ(privacy->GetBool("redactLogs"), false);
  ASSERT_NE(privacy->Find("custom"), nullptr);
  EXPECT_EQ(privacy->Find("custom")->GetBool("detailedLogging"), true);
  EXPECT_EQ(privacy->Find("custom")->GetBool("learning"), true);
  std::filesystem::remove_all(dir);
}

TEST(SettingsDocumentTest, CustomPredictionSurvivesUnrelatedSettingsSave) {
  const auto dir = TestDir("azookey_settings_custom_prediction_preserve");
  const auto path = dir / "settings.json";
  for (const bool enabled : {false, true}) {
    WriteText(
        path,
        std::string(R"({"privacy":{"mode":"custom","custom":{"learning":false,"prediction":)") +
            (enabled ? "true" : "false") + R"(,"aiCandidate":true}},"predictionEnabled":false})");
    auto loaded = azookey::settings::LoadSettingsDocument(path);
    ASSERT_EQ(loaded.status, azookey::settings::SettingsDocumentStatus::Loaded);
    EXPECT_TRUE(loaded.warnings.empty());
    loaded.settings.log_level = "warn";
    const auto saved = azookey::settings::SaveSettingsDocument(path, loaded.settings);
    ASSERT_TRUE(saved.ok) << saved.error.value_or("");
    EXPECT_TRUE(saved.warnings.empty());

    const auto parsed = azookey::ipc::json::Parse(ReadText(path));
    ASSERT_TRUE(parsed && parsed->IsObject());
    const auto* privacy = parsed->Find("privacy");
    ASSERT_NE(privacy, nullptr);
    EXPECT_EQ(privacy->GetString("mode"), "custom");
    const auto* custom = privacy->Find("custom");
    ASSERT_NE(custom, nullptr);
    EXPECT_EQ(custom->GetBool("prediction"), enabled);
    EXPECT_EQ(custom->GetBool("learning"), false);
    EXPECT_EQ(parsed->GetBool("predictionEnabled"), false);
  }
  std::filesystem::remove_all(dir);
}

TEST(SettingsDocumentTest, AutoSecureInputSurvivesUnrelatedSaveAndInvalidValuesRestrictPrivacy) {
  const auto name = "azookey_settings_auto_secure_input-" +
                    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  const auto dir = TestDir(name.c_str());
  const auto path = dir / "settings.json";
  for (const bool enabled : {false, true}) {
    WriteText(path, std::string(R"({"privacy":{"mode":"normal","autoSecureInput":)") +
                        (enabled ? "true" : "false") + "}}");
    auto loaded = azookey::settings::LoadSettingsDocument(path);
    ASSERT_EQ(loaded.status, azookey::settings::SettingsDocumentStatus::Loaded);
    EXPECT_TRUE(loaded.warnings.empty());
    loaded.settings.log_level = "warn";
    ASSERT_TRUE(azookey::settings::SaveSettingsDocument(path, loaded.settings).ok);
    const auto parsed = azookey::ipc::json::Parse(ReadText(path));
    ASSERT_TRUE(parsed);
    ASSERT_NE(parsed->Find("privacy"), nullptr);
    EXPECT_EQ(parsed->Find("privacy")->GetString("mode"), "normal");
    EXPECT_EQ(parsed->Find("privacy")->GetBool("autoSecureInput"), enabled);
  }
  for (const auto* text :
       {R"({"privacy":{"autoSecureInput":"false"}})", R"({"privacy":{"autoSecureInput":0}})",
        R"({"privacy":{"autoSecureInput":null}})",
        R"({"privacy":{"autoSecureInput":false,"secureUrlPatterns":[]}})"}) {
    SCOPED_TRACE(text);
    WriteText(path, text);
    const auto loaded = azookey::settings::LoadSettingsDocument(path);
    EXPECT_FALSE(loaded.warnings.empty());
    ASSERT_TRUE(azookey::settings::SaveSettingsDocument(path, loaded.settings).ok);
    const auto parsed = azookey::ipc::json::Parse(ReadText(path));
    ASSERT_TRUE(parsed);
    ASSERT_NE(parsed->Find("privacy"), nullptr);
    EXPECT_EQ(parsed->Find("privacy")->GetString("mode"), "secure");
    EXPECT_EQ(parsed->Find("privacy")->Find("autoSecureInput"), nullptr);
  }
  std::filesystem::remove_all(dir);
}

TEST(SettingsDocumentTest, MissingCustomPredictionRemainsAbsentOnUnrelatedSave) {
  const auto dir = TestDir("azookey_settings_custom_prediction_missing");
  const auto path = dir / "settings.json";
  WriteText(path, R"({"privacy":{"mode":"custom","custom":{"learning":false}}})");
  const auto loaded = azookey::settings::LoadSettingsDocument(path);
  ASSERT_EQ(loaded.status, azookey::settings::SettingsDocumentStatus::Loaded);
  EXPECT_TRUE(loaded.warnings.empty());
  const auto saved = azookey::settings::SaveSettingsDocument(path, loaded.settings);
  ASSERT_TRUE(saved.ok) << saved.error.value_or("");
  const auto parsed = azookey::ipc::json::Parse(ReadText(path));
  ASSERT_TRUE(parsed && parsed->IsObject());
  const auto* privacy = parsed->Find("privacy");
  ASSERT_NE(privacy, nullptr);
  const auto* custom = privacy->Find("custom");
  ASSERT_NE(custom, nullptr);
  EXPECT_EQ(custom->Find("prediction"), nullptr);
  EXPECT_EQ(custom->GetBool("learning"), false);
  std::filesystem::remove_all(dir);
}

TEST(SettingsDocumentTest, MalformedBodyLogPolicyRestrictsPrivacyOnSave) {
  const auto dir = TestDir("azookey_settings_body_log_invalid");
  const auto path = dir / "settings.json";
  for (const auto* text : {R"({"privacy":{"redactLogs":"false"}})",
                           R"({"privacy":{"redactLogs":false,"custom":{"detailedLogging":1}}})",
                           R"({"privacy":{"mode":"custom","custom":{"learning":"true"}}})",
                           R"({"privacy":{"mode":"custom","custom":{"prediction":"false"}}})"}) {
    WriteText(path, text);
    const auto loaded = azookey::settings::LoadSettingsDocument(path);
    EXPECT_FALSE(loaded.warnings.empty());
    ASSERT_TRUE(azookey::settings::SaveSettingsDocument(path, loaded.settings).ok);
    const auto parsed = azookey::ipc::json::Parse(ReadText(path));
    ASSERT_TRUE(parsed);
    ASSERT_NE(parsed->Find("privacy"), nullptr);
    EXPECT_EQ(parsed->Find("privacy")->GetString("mode"), "secure");
  }
  std::filesystem::remove_all(dir);
}

TEST(SettingsDocumentTest, CrashConsentRoundTripPreservesAiPrivacyAndDefaultsOff) {
  const auto dir = TestDir("azookey_settings_crash_writeback");
  const auto path = dir / "settings.json";
  EXPECT_EQ(azookey::settings::LoadSettingsDocument(path).settings.crash_report_consent, "off");
  WriteText(path, R"({"privacy":{"mode":"custom","custom":{"aiCandidate":true,"externalAi":false},"crashReportConsent":"local"}})");
  auto loaded = azookey::settings::LoadSettingsDocument(path);
  EXPECT_EQ(loaded.settings.crash_report_consent, "local");
  ASSERT_TRUE(azookey::settings::SaveSettingsDocument(path, loaded.settings).ok);
  auto parsed = azookey::ipc::json::Parse(ReadText(path));
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->Find("privacy")->GetString("crashReportConsent"), "local");
  EXPECT_EQ(parsed->Find("privacy")->GetString("mode"), "custom");
  EXPECT_EQ(parsed->Find("privacy")->Find("custom")->GetBool("externalAi"), false);
  loaded.settings.crash_report_consent = "off";
  ASSERT_TRUE(azookey::settings::SaveSettingsDocument(path, loaded.settings).ok);
  EXPECT_EQ(azookey::settings::LoadSettingsDocument(path).settings.crash_report_consent, "off");
}

TEST(SettingsDocumentTest, InvalidCrashConsentFailsClosedWithoutChangingAiMode) {
  const auto dir = TestDir("azookey_settings_invalid_crash_consent");
  const auto path = dir / "settings.json";
  for (const auto* consent : {"true", "null", "42", "\"unknown\""}) {
    WriteText(path, std::string(R"({"privacy":{"mode":"private","crashReportConsent":)") + consent + "}}");
    auto loaded = azookey::settings::LoadSettingsDocument(path);
    EXPECT_EQ(loaded.settings.crash_report_consent, "off");
    EXPECT_FALSE(loaded.warnings.empty());
    ASSERT_TRUE(azookey::settings::SaveSettingsDocument(path, loaded.settings).ok);
    const auto parsed = azookey::ipc::json::Parse(ReadText(path));
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->Find("privacy")->GetString("mode"), "private");
    EXPECT_EQ(parsed->Find("privacy")->GetString("crashReportConsent"), "off");
  }
  const auto original = ReadText(path);
  auto invalid = azookey::settings::LoadSettingsDocument(path).settings;
  invalid.crash_report_consent = "upload";
  EXPECT_FALSE(azookey::settings::SaveSettingsDocument(path, invalid).ok);
  EXPECT_EQ(ReadText(path), original);
}

TEST(SettingsDocumentTest, PreservesAiPrivacyAndTimeoutAndFailsClosedOnInvalidPrivacy) {
  const auto dir = TestDir("azookey_settings_ai_writeback");
  const auto path = dir / "settings.json";
  WriteText(
      path,
      R"({"privacy":{"mode":"custom","custom":{"aiCandidate":true,"externalAi":false}},"openAiTimeoutMs":45000})");
  const auto loaded = azookey::settings::LoadSettingsDocument(path);
  const auto saved = azookey::settings::SaveSettingsDocument(path, loaded.settings);
  EXPECT_TRUE(saved.ok);
  auto parsed = azookey::ipc::json::Parse(ReadText(path));
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->GetInt("openAiTimeoutMs"), 45000);
  ASSERT_TRUE(parsed->Find("privacy"));
  EXPECT_EQ(parsed->Find("privacy")->GetString("mode"), "custom");
  WriteText(path, R"({"privacy":{"mode":"custom","custom":{"externalAi":"false"}}})");
  const auto invalid = azookey::settings::LoadSettingsDocument(path);
  EXPECT_TRUE(azookey::settings::SaveSettingsDocument(path, invalid.settings).ok);
  parsed = azookey::ipc::json::Parse(ReadText(path));
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->Find("privacy")->GetString("mode"), "secure");
}

TEST(SettingsDocumentTest, PreservesAllCommonProfileFieldsAndSanitizesNestedValues) {
  const auto dir = TestDir("azookey_settings_profile_writeback");
  const auto path = dir / "settings.json";
  WriteText(path, R"({"profilesByApp":{"Code.exe":{
    "profileName":"Code","predictionEnabled":false,"sentenceCompletion":true,
    "learningEnabled":false,"aiBackend":"auto","promptPrefix":"","style":"technical",
    "preferTechnicalTerms":true,"candidateTagBoosts":{"Technical":100},"privacyMode":"secure",
    "bracketPairing":"on","invalid":false},"Bad":false}})");
  const auto loaded = azookey::settings::LoadSettingsDocument(path);
  const auto saved = azookey::settings::SaveSettingsDocument(path, loaded.settings);
  ASSERT_TRUE(saved.ok);
  EXPECT_FALSE(saved.warnings.empty());
  const auto parsed = azookey::ipc::json::Parse(ReadText(path));
  ASSERT_TRUE(parsed);
  const auto* profiles = parsed->FindObject("profilesByApp");
  ASSERT_NE(profiles, nullptr);
  ASSERT_EQ(profiles->size(), 1u);
  const auto& profile = profiles->at("Code.exe");
  EXPECT_EQ(profile.AsObject().size(), 11u);
  EXPECT_EQ(profile.GetString("bracketPairing"), "on");
  EXPECT_EQ(profile.GetString("privacyMode"), "secure");
  EXPECT_EQ(profile.GetString("promptPrefix"), "");
  EXPECT_EQ(profile.GetString("aiBackend"), "auto");
  EXPECT_EQ(profile.FindObject("candidateTagBoosts")->at("Technical").AsNumber(), 3);
  std::filesystem::remove_all(dir);
}

TEST(SettingsDocumentTest, MissingFileUsesM11Defaults) {
  const auto dir = TestDir("azookey_settings_document_missing");
  const auto result = azookey::settings::LoadSettingsDocument(dir / "settings.json");

  EXPECT_EQ(result.status, azookey::settings::SettingsDocumentStatus::Missing);
  EXPECT_TRUE(result.settings.model_enabled);
  ASSERT_TRUE(result.settings.model_backend_preference.has_value());
  EXPECT_EQ(*result.settings.model_backend_preference, "auto");
  EXPECT_TRUE(result.settings.model_selected_path.empty());
  EXPECT_EQ(result.settings.log_level, "info");
  std::filesystem::remove_all(dir);
}

TEST(SettingsDocumentTest, SavePreservesTheInlineEnglishKeys) {
  const auto dir = TestDir("azookey_settings_document_inline_english");
  const auto path = dir / "settings.json";
  WriteText(path, R"({
    "inlineEnglishCandidates": true,
    "mixedEnglishAutoConversion": true,
    "inlineEnglishCaseVariants": false,
    "fullWidthEnglishCandidate": true,
    "inlineEnglishMinLength": 3,
    "inlineEnglishPromoteThreshold": 0.25,
    "inlineEnglishDictionary": true,
    "inlineEnglishDictionaryPath": "C:/dict/words.tsv"
  })");
  azookey::settings::EditableSettings settings;
  ASSERT_TRUE(azookey::settings::SaveSettingsDocument(path, settings).ok);
  auto parsed = azookey::ipc::json::Parse(ReadText(path));
  ASSERT_TRUE(parsed && parsed->IsObject());
  const auto& root = parsed->AsObject();
  ASSERT_TRUE(root.contains("inlineEnglishCandidates"));
  EXPECT_TRUE(root.at("inlineEnglishCandidates").AsBool());
  ASSERT_TRUE(root.contains("mixedEnglishAutoConversion"));
  EXPECT_TRUE(root.at("mixedEnglishAutoConversion").AsBool());
  EXPECT_FALSE(root.at("inlineEnglishCaseVariants").AsBool());
  EXPECT_TRUE(root.at("fullWidthEnglishCandidate").AsBool());
  EXPECT_EQ(root.at("inlineEnglishMinLength").AsNumber(), 3.0);
  EXPECT_EQ(root.at("inlineEnglishPromoteThreshold").AsNumber(), 0.25);
  EXPECT_TRUE(root.at("inlineEnglishDictionary").AsBool());
  EXPECT_EQ(root.at("inlineEnglishDictionaryPath").AsString(), "C:/dict/words.tsv");

  WriteText(path, R"({"inlineEnglishMinLength": 0, "inlineEnglishPromoteThreshold": 2,
                      "inlineEnglishCandidates": "yes", "inlineEnglishDictionaryPath": 7,
                      "mixedEnglishAutoConversion": 1})");
  ASSERT_TRUE(azookey::settings::SaveSettingsDocument(path, settings).ok);
  parsed = azookey::ipc::json::Parse(ReadText(path));
  ASSERT_TRUE(parsed && parsed->IsObject());
  for (const char* key :
       {"inlineEnglishMinLength", "inlineEnglishPromoteThreshold", "inlineEnglishCandidates",
        "inlineEnglishDictionaryPath", "mixedEnglishAutoConversion"}) {
    EXPECT_FALSE(parsed->AsObject().contains(key)) << key;
  }
}

TEST(SettingsDocumentTest, SavePreservesValidHiddenKeysAndDropsInvalidEntries) {
  const auto dir = TestDir("azookey_settings_document_writeback");
  const auto path = dir / "settings.json";
  WriteText(path, R"({
    "liveConversion": true,
    "numberRewriter": true,
    "katakanaRewriter": true,
    "symbolRewriter": true,
    "emojiRewriter": true,
    "emojiTriggerSearch": false,
    "emojiMaxCandidates": 23,
    "emojiTriggerMinQueryLength": 2,
    "symbolDataPath": "C:/data/symbol.tsv",
    "emojiDataPath": "C:/data/emoji.tsv",
    "inferenceThreads": 6,
    "maxCandidates": 12,
    "maxContextLength": 20,
    "aiBackend": "invalid-backend",
    "openAiModel": "local-test-model",
    "backendPreference": "cuda",
    "unknownRoot": 1,
    "reranker": {"nllRerankEnabled": true, "nllTopK": 8, "nllWeight": 0.15,
                 "nllBudgetMs": 20, "nllFailureThreshold": 3, "unknown": true},
    "promptPrefixByApp": {"Code.exe": "code", "bad": false},
    "model": {
      "enabled": true,
      "selectedPath": "old.gguf",
      "backendPreference": "cpu",
      "epPreference": "gpu",
      "unknownModel": true
    }
  })");

  azookey::settings::EditableSettings settings;
  settings.model_enabled = false;
  settings.model_backend_preference = "auto";
  settings.model_selected_path = "C:/models/new.gguf";
  settings.log_level = "debug";
  const auto saved = azookey::settings::SaveSettingsDocument(path, settings);

  ASSERT_TRUE(saved.ok) << saved.error.value_or("");
  const auto parsed = azookey::ipc::json::Parse(ReadText(path));
  ASSERT_TRUE(parsed && parsed->IsObject());
  const auto& root = parsed->AsObject();
  EXPECT_TRUE(root.contains("liveConversion"));
  ASSERT_TRUE(root.contains("numberRewriter"));
  EXPECT_TRUE(root.at("numberRewriter").AsBool());
  EXPECT_TRUE(root.at("katakanaRewriter").AsBool());
  EXPECT_TRUE(root.at("symbolRewriter").AsBool());
  EXPECT_TRUE(root.at("emojiRewriter").AsBool());
  EXPECT_FALSE(root.at("emojiTriggerSearch").AsBool());
  EXPECT_EQ(root.at("emojiMaxCandidates").AsNumber(), 23.0);
  EXPECT_EQ(root.at("emojiTriggerMinQueryLength").AsNumber(), 2.0);
  EXPECT_EQ(root.at("symbolDataPath").AsString(), "C:/data/symbol.tsv");
  EXPECT_EQ(root.at("emojiDataPath").AsString(), "C:/data/emoji.tsv");
  EXPECT_EQ(root.at("inferenceThreads").AsNumber(), 6.0);
  EXPECT_EQ(root.at("maxCandidates").AsNumber(), 12.0);
  EXPECT_EQ(root.at("maxContextLength").AsNumber(), 20.0);
  EXPECT_TRUE(root.contains("openAiModel"));
  EXPECT_FALSE(root.contains("aiBackend"));
  EXPECT_FALSE(root.contains("unknownRoot"));
  ASSERT_TRUE(root.contains("reranker"));
  const auto& reranker = root.at("reranker").AsObject();
  EXPECT_EQ(reranker.size(), 5u);
  EXPECT_TRUE(reranker.at("nllRerankEnabled").AsBool());
  EXPECT_EQ(reranker.at("nllTopK").AsNumber(), 8);
  EXPECT_EQ(reranker.at("nllWeight").AsNumber(), 0.15);
  EXPECT_EQ(reranker.at("nllBudgetMs").AsNumber(), 20);
  EXPECT_EQ(reranker.at("nllFailureThreshold").AsNumber(), 3);
  EXPECT_FALSE(root.contains("backendPreference"));
  ASSERT_TRUE(root.contains("promptPrefixByApp"));
  EXPECT_EQ(root.at("promptPrefixByApp").AsObject().size(), 1u);

  ASSERT_TRUE(root.contains("model"));
  const auto& model = root.at("model").AsObject();
  EXPECT_FALSE(model.at("enabled").AsBool());
  EXPECT_EQ(model.at("selectedPath").AsString(), "C:/models/new.gguf");
  EXPECT_EQ(model.at("backendPreference").AsString(), "auto");
  EXPECT_EQ(model.at("epPreference").AsString(), "gpu");
  EXPECT_FALSE(model.contains("unknownModel"));
  EXPECT_EQ(root.at("logLevel").AsString(), "debug");
  EXPECT_FALSE(saved.warnings.empty());
  std::filesystem::remove_all(dir);
}

TEST(SettingsDocumentTest, SavesModelSettingsWithoutDroppingTipBracketConfiguration) {
  const auto dir = TestDir("azookey_settings_brackets");
  const auto path = dir / "settings.json";
  const std::string json = R"({"bracketPairing":true,"bracketPairingTrigger":"composition",
      "bracketSkipOverClosing":false,"bracketBackspaceDeletesPair":false,
      "bracketPairingInAlnumMode":false,"bracketPairsPath":"pairs.tsv",
      "bracketPairingAppPolicy":"allowlist","bracketPairingApps":["Code.exe"],
      "bracketSymmetricQuotePairing":true,"bracketWrapSelection":true})";
  WriteText(path, json);
  const auto loaded = azookey::settings::LoadSettingsDocument(path);
  const auto saved = azookey::settings::SaveSettingsDocument(path, loaded.settings);
  ASSERT_TRUE(saved.ok);
  EXPECT_TRUE(saved.warnings.empty());
  const auto actual = azookey::ipc::json::Parse(ReadText(path));
  const auto expected = azookey::ipc::json::Parse(json);
  ASSERT_TRUE(actual && expected);
  for (const auto& [key, value] : expected->AsObject()) {
    ASSERT_TRUE(actual->AsObject().contains(key)) << key;
    EXPECT_EQ(azookey::ipc::json::Stringify(actual->AsObject().at(key)),
              azookey::ipc::json::Stringify(value))
        << key;
  }
  std::filesystem::remove_all(dir);
}

TEST(SettingsDocumentTest, PreservesValidDynamicPunctuationSettings) {
  const auto dir = TestDir("azookey_settings_dynamic_punctuation_document");
  const auto path = dir / "settings.json";
  const std::string json = R"({"dynamicPunctuation":true,"punctuationConversion":"commit",
    "dynamicPunctuationStyle":"fullwidth_latin","dynamicPunctuationStability":"eager",
    "dynamicPunctuationIdleMs":350,"segmentBoundaryConfidence":0.75,
    "punctuationRulesPath":"C:/rules/custom.tsv"})";
  WriteText(path, json);
  const auto loaded = azookey::settings::LoadSettingsDocument(path);
  const auto saved = azookey::settings::SaveSettingsDocument(path, loaded.settings);
  ASSERT_TRUE(saved.ok);
  const auto actual = azookey::ipc::json::Parse(ReadText(path));
  const auto expected = azookey::ipc::json::Parse(json);
  ASSERT_TRUE(actual && expected);
  for (const auto& [key, value] : expected->AsObject()) {
    ASSERT_TRUE(actual->AsObject().contains(key)) << key;
    EXPECT_EQ(azookey::ipc::json::Stringify(actual->AsObject().at(key)),
              azookey::ipc::json::Stringify(value)) << key;
  }
  std::filesystem::remove_all(dir);
}

TEST(SettingsDocumentTest, UnsupportedValidBackendIsPreservedUntilUserSelectsOne) {
  const auto dir = TestDir("azookey_settings_document_hidden_backend");
  const auto path = dir / "settings.json";
  for (const auto backend : {"cuda", "vulkan", "winml", "directml", "npu"}) {
    SCOPED_TRACE(backend);
    WriteText(path, std::string(R"({"backendPreference":"cpu","model":{"backendPreference":")") +
                        backend + R"("}})");
    const auto loaded = azookey::settings::LoadSettingsDocument(path);
    ASSERT_EQ(loaded.status, azookey::settings::SettingsDocumentStatus::Loaded);
    EXPECT_FALSE(loaded.settings.model_backend_preference.has_value());
    EXPECT_EQ(loaded.settings.hidden_backend_preference, backend);

    auto edited = loaded.settings;
    edited.log_level = "warn";
    ASSERT_TRUE(azookey::settings::SaveSettingsDocument(path, edited).ok);
    const auto parsed = azookey::ipc::json::Parse(ReadText(path));
    ASSERT_TRUE(parsed && parsed->IsObject());
    EXPECT_EQ(parsed->AsObject().at("model").AsObject().at("backendPreference").AsString(),
              backend);
    EXPECT_FALSE(parsed->AsObject().contains("backendPreference"));

    edited.model_backend_preference = "cpu";
    ASSERT_TRUE(azookey::settings::SaveSettingsDocument(path, edited).ok);
    EXPECT_EQ(azookey::settings::LoadSettingsDocument(path).settings.model_backend_preference,
              "cpu");
  }
  std::filesystem::remove_all(dir);
}

TEST(SettingsDocumentTest, VisibleBackendAcceptsOnlyAutoAndCpu) {
  const auto dir = TestDir("azookey_settings_document_visible_backend");
  const auto path = dir / "settings.json";
  azookey::settings::EditableSettings edited;
  for (const auto backend : {"auto", "cpu"}) {
    edited.model_backend_preference = backend;
    ASSERT_TRUE(azookey::settings::SaveSettingsDocument(path, edited).ok);
    const auto loaded = azookey::settings::LoadSettingsDocument(path);
    EXPECT_EQ(loaded.settings.model_backend_preference, backend);
    EXPECT_TRUE(loaded.settings.hidden_backend_preference.empty());
  }
  const auto original = ReadText(path);
  for (const auto backend : {"cuda", "vulkan", "winml", "directml", "npu", "invalid"}) {
    edited.model_backend_preference = backend;
    EXPECT_FALSE(azookey::settings::SaveSettingsDocument(path, edited).ok);
    EXPECT_EQ(ReadText(path), original);
  }
  std::filesystem::remove_all(dir);
}

TEST(SettingsDocumentTest, LockFailureLeavesExistingFileUntouched) {
  const auto dir = TestDir("azookey_settings_document_lock_failure");
  const auto path = dir / "settings.json";
  const std::string original = R"({"logLevel":"info"})";
  WriteText(path, original);

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

  azookey::settings::EditableSettings settings;
  settings.log_level = "debug";
  const auto result =
      azookey::settings::SaveSettingsDocument(path, settings, std::chrono::milliseconds(20));

  release.set_value();
  holder.join();
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(ReadText(path), original);
  std::filesystem::remove_all(dir);
}

#ifdef _WIN32
TEST(SettingsDocumentTest, ReadFailureLeavesExistingFileUntouched) {
  const auto dir = TestDir("azookey_settings_document_read_failure");
  const auto path = dir / "settings.json";
  const std::string original = R"({"logLevel":"info"})";
  WriteText(path, original);

  HANDLE exclusive = CreateFileW(path.wstring().c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(exclusive, INVALID_HANDLE_VALUE);

  azookey::settings::EditableSettings settings;
  settings.log_level = "debug";
  const auto result = azookey::settings::SaveSettingsDocument(path, settings);

  CloseHandle(exclusive);
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(ReadText(path), original);
  std::filesystem::remove_all(dir);
}
#endif

TEST(SettingsDocumentTest, RejectsInvalidEditableEnums) {
  const auto dir = TestDir("azookey_settings_document_invalid_editable_enums");
  const auto path = dir / "settings.json";

  azookey::settings::EditableSettings settings;
  settings.log_level = "verbose";
  EXPECT_FALSE(azookey::settings::SaveSettingsDocument(path, settings).ok);

  settings.log_level = "info";
  settings.model_backend_preference = "winml";
  EXPECT_FALSE(azookey::settings::SaveSettingsDocument(path, settings).ok);
  EXPECT_FALSE(std::filesystem::exists(path));
  std::filesystem::remove_all(dir);
}

TEST(SettingsDocumentTest, InvalidDocumentCanBeRecoveredByAtomicSave) {
  const auto dir = TestDir("azookey_settings_document_invalid");
  const auto path = dir / "settings.json";
  WriteText(path, "{ invalid json");

  azookey::settings::EditableSettings settings;
  settings.model_enabled = false;
  const auto result = azookey::settings::SaveSettingsDocument(path, settings);

  ASSERT_TRUE(result.ok);
  EXPECT_FALSE(result.warnings.empty());
  ASSERT_TRUE(result.quarantined_path.has_value());
  EXPECT_EQ(*result.quarantined_path, std::filesystem::path(path.string() + ".invalid"));
  EXPECT_EQ(ReadText(*result.quarantined_path), "{ invalid json");
  const auto parsed = azookey::ipc::json::Parse(ReadText(path));
  ASSERT_TRUE(parsed && parsed->IsObject());
  EXPECT_FALSE(parsed->AsObject().at("model").AsObject().at("enabled").AsBool());
  std::filesystem::remove_all(dir);
}

TEST(SettingsDocumentTest, InvalidDocumentIsNotOverwrittenWhenQuarantineSuffixesAreExhausted) {
  const auto dir = TestDir("azookey_settings_document_invalid_suffixes");
  const auto path = dir / "settings.json";
  const std::string invalid = "{ invalid json";
  WriteText(path, invalid);
  for (int i = 0; i < 100; ++i) {
    auto candidate = path;
    candidate += (i == 0) ? ".invalid" : ".invalid." + std::to_string(i);
    WriteText(candidate, "reserved");
  }

  azookey::settings::EditableSettings settings;
  settings.model_enabled = false;
  const auto result = azookey::settings::SaveSettingsDocument(path, settings);

  EXPECT_FALSE(result.ok);
  EXPECT_FALSE(result.quarantined_path.has_value());
  EXPECT_EQ(ReadText(path), invalid);
  std::filesystem::remove_all(dir);
}

TEST(SettingsDocumentTest, TypoAndAutoWordKeysSurviveASaveAndInvalidOnesAreDropped) {
  const auto dir = TestDir("azookey_settings_document_typo_auto_word");
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

  azookey::settings::EditableSettings settings;
  const auto saved = azookey::settings::SaveSettingsDocument(path, settings);
  ASSERT_TRUE(saved.ok) << saved.error.value_or("");

  const auto parsed = azookey::ipc::json::Parse(ReadText(path));
  ASSERT_TRUE(parsed && parsed->IsObject());
  const auto& root = parsed->AsObject();
  // The sanitizer drops keys it does not know, so an unlisted key would be
  // silently lost every time the settings app writes the file.
  ASSERT_TRUE(root.contains("typoCorrectionMode"));
  EXPECT_EQ(root.at("typoCorrectionMode").AsString(), "auto_replace");
  ASSERT_TRUE(root.contains("typoMinCount"));
  EXPECT_EQ(root.at("typoMinCount").AsNumber(), 5.0);
  ASSERT_TRUE(root.contains("autoWordRegistration"));
  const auto& auto_word = root.at("autoWordRegistration").AsObject();
  EXPECT_FALSE(auto_word.at("miningEnabled").AsBool());
  EXPECT_TRUE(auto_word.at("trendingEnabled").AsBool());
  EXPECT_EQ(auto_word.at("registrationMode").AsString(), "auto");
  EXPECT_EQ(auto_word.at("miningMinCount").AsNumber(), 7.0);
  EXPECT_EQ(auto_word.at("trendingIntervalHours").AsNumber(), 6.0);
}

TEST(SettingsDocumentTest, InvalidTypoAndAutoWordValuesAreRemoved) {
  const auto dir = TestDir("azookey_settings_document_typo_auto_word_invalid");
  const auto path = dir / "settings.json";
  WriteText(path, R"({
    "typoCorrectionMode": "aggressive",
    "typoMinCount": 0,
    "autoWordRegistration": {
      "registrationMode": "whenever",
      "miningMinCount": 9999,
      "unknownKey": true
    }
  })");

  azookey::settings::EditableSettings settings;
  const auto saved = azookey::settings::SaveSettingsDocument(path, settings);
  ASSERT_TRUE(saved.ok) << saved.error.value_or("");

  const auto parsed = azookey::ipc::json::Parse(ReadText(path));
  ASSERT_TRUE(parsed && parsed->IsObject());
  const auto& root = parsed->AsObject();
  // Removing the key restores the runtime default rather than persisting a
  // value the host would have to reinterpret.
  EXPECT_FALSE(root.contains("typoCorrectionMode"));
  EXPECT_FALSE(root.contains("typoMinCount"));
  ASSERT_TRUE(root.contains("autoWordRegistration"));
  const auto& auto_word = root.at("autoWordRegistration").AsObject();
  EXPECT_FALSE(auto_word.contains("registrationMode"));
  EXPECT_FALSE(auto_word.contains("miningMinCount"));
  EXPECT_FALSE(auto_word.contains("unknownKey"));
}

// typo-correction-learning-spec section 12.14: typoCorrection survives a save
// that does not touch it, and its invalid keys are dropped the way the Host
// would ignore them.
TEST(SettingsDocumentTest, TypoCorrectionSurvivesASaveAndInvalidKeysAreDropped) {
  ScopedTempDirectory temp("azookey_settings_typo_correction_keep");
  const auto path = temp.path() / "settings.json";
  WriteText(path, R"({"typoCorrection":{"enabled":false,"mode":"rank",
    "learnPersonalPatterns":false,"keyboardLayout":"auto","maxReadingHypotheses":8,
    "minConfidenceForRanking":0.6,"minConfidenceForTopCandidate":0.8,
    "disableInSecretApps":true}})");
  auto loaded = azookey::settings::LoadSettingsDocument(path);
  EXPECT_TRUE(loaded.warnings.empty());
  loaded.settings.log_level = "warn";
  ASSERT_TRUE(azookey::settings::SaveSettingsDocument(path, loaded.settings).ok);
  auto parsed = azookey::ipc::json::Parse(ReadText(path));
  ASSERT_TRUE(parsed && parsed->IsObject());
  const auto* typo = parsed->Find("typoCorrection");
  ASSERT_TRUE(typo && typo->IsObject());
  EXPECT_EQ(typo->AsObject().size(), 8u);
  EXPECT_EQ(typo->GetString("mode"), "rank");
  EXPECT_EQ(typo->GetBool("enabled"), false);
  EXPECT_EQ(typo->GetNumber("minConfidenceForTopCandidate"), 0.8);
  // The pane shows enabled false as off and writes that, as any non-default
  // value, to the root key, which is what an older Host reads.
  EXPECT_EQ(parsed->GetString("typoCorrectionMode"), "off");

  WriteText(path, R"({"typoCorrection":{"mode":"auto_replace","keyboardLayout":"jis",
    "maxReadingHypotheses":9,"disableInSecretApps":false,"unknownKey":1,
    "minConfidenceForRanking":0.95,"minConfidenceForTopCandidate":0.9,"learnPersonalPatterns":true}})");
  const auto saved = azookey::settings::SaveSettingsDocument(path, {});
  ASSERT_TRUE(saved.ok) << saved.error.value_or("");
  EXPECT_FALSE(saved.warnings.empty());
  parsed = azookey::ipc::json::Parse(ReadText(path));
  ASSERT_TRUE(parsed && parsed->IsObject());
  typo = parsed->Find("typoCorrection");
  ASSERT_TRUE(typo && typo->IsObject());
  // Only the valid key is left; the inverted confidences go as a pair.
  EXPECT_EQ(typo->AsObject().size(), 1u);
  EXPECT_EQ(typo->GetBool("learnPersonalPatterns"), true);
}

// Section 12.14: the "辞書" pane shows the mode the Host uses, and a mode the
// user changes there is written to the root key and to typoCorrection.mode.
TEST(SettingsDocumentTest, ThePaneTypoModeFollowsTypoCorrectionAndAChangeWritesBothKeys) {
  ScopedTempDirectory temp("azookey_settings_typo_correction_sync");
  const auto path = temp.path() / "settings.json";
  const auto shown = [&](const char* json) {
    WriteText(path, json);
    const auto loaded = azookey::settings::LoadSettingsDocument(path);
    const auto it = loaded.settings.values.find("typoCorrectionMode");
    return it == loaded.settings.values.end() ? std::string("<unset>")
                                              : std::get<std::string>(it->second);
  };
  EXPECT_EQ(shown("{}"), "<unset>");
  EXPECT_EQ(shown(R"({"typoCorrectionMode":"auto_replace"})"), "auto_replace");
  EXPECT_EQ(shown(R"({"typoCorrectionMode":"off","typoCorrection":{"mode":"aggressive"}})"),
            "auto_replace");
  EXPECT_EQ(shown(R"({"typoCorrectionMode":"auto_replace","typoCorrection":{"mode":"rank"}})"),
            "suggest");
  EXPECT_EQ(shown(R"({"typoCorrectionMode":"suggest","typoCorrection":{"enabled":false}})"), "off");

  struct Case {
    const char* json;
    const char* chosen;
    const char* root;
    const char* nested;
    std::optional<bool> enabled;
  };
  for (const auto& item : {
           // A save that keeps the mode the file resolves to leaves rank alone.
           Case{R"({"typoCorrection":{"mode":"rank"}})", "suggest", nullptr, "rank", std::nullopt},
           Case{R"({"typoCorrection":{"mode":"rank"}})", "off", "off", "off", std::nullopt},
           Case{R"({"typoCorrectionMode":"suggest"})", "auto_replace", "auto_replace", "aggressive",
                std::nullopt},
           Case{R"({"typoCorrectionMode":"auto_replace","typoCorrection":{"mode":"aggressive"}})",
                "suggest", "suggest", "suggest", std::nullopt},
           // Turning it on clears the enabled false that would keep it off.
           Case{R"({"typoCorrection":{"enabled":false,"mode":"suggest"}})", "suggest", "suggest",
                "suggest", true},
           Case{R"({"typoCorrection":{"enabled":false,"mode":"suggest"}})", "off", "off", "suggest",
                false},
       }) {
    SCOPED_TRACE(std::string(item.json) + " -> " + item.chosen);
    WriteText(path, item.json);
    auto loaded = azookey::settings::LoadSettingsDocument(path);
    loaded.settings.values["typoCorrectionMode"] = std::string(item.chosen);
    const auto saved = azookey::settings::SaveSettingsDocument(path, loaded.settings);
    ASSERT_TRUE(saved.ok) << saved.error.value_or("");
    const auto parsed = azookey::ipc::json::Parse(ReadText(path));
    ASSERT_TRUE(parsed && parsed->IsObject());
    if (item.root) {
      EXPECT_EQ(parsed->GetString("typoCorrectionMode"), item.root);
    } else {
      EXPECT_FALSE(parsed->Find("typoCorrectionMode"));
    }
    const auto* typo = parsed->Find("typoCorrection");
    ASSERT_TRUE(typo && typo->IsObject());
    EXPECT_EQ(typo->GetString("mode"), item.nested);
    EXPECT_EQ(typo->GetBool("enabled"), item.enabled);
  }
}

TEST(SettingsDocumentTest, DictionaryLayerSwitchesSurviveAnUnrelatedSave) {
  ScopedTempDirectory temp("azookey_settings_dictionary_preserve");
  const auto path = temp.path() / "settings.json";
  WriteText(path, R"({"dictionary":{
    "sudachiEnabled":false,"neologdEnabled":true,"namedEntityEnabled":false,
    "technicalTermsEnabled":false,"userDictionaryEnabled":false,"autoWordsEnabled":false,
    "appSpecificDictionaryEnabled":false}})");
  auto loaded = azookey::settings::LoadSettingsDocument(path);
  EXPECT_TRUE(loaded.warnings.empty());
  loaded.settings.log_level = "warn";
  const auto saved = azookey::settings::SaveSettingsDocument(path, loaded.settings);
  ASSERT_TRUE(saved.ok) << saved.error.value_or("");
  EXPECT_TRUE(saved.warnings.empty());
  const auto parsed = azookey::ipc::json::Parse(ReadText(path));
  ASSERT_TRUE(parsed && parsed->IsObject());
  const auto* dictionary = parsed->Find("dictionary");
  ASSERT_TRUE(dictionary && dictionary->IsObject());
  EXPECT_EQ(dictionary->GetBool("sudachiEnabled"), false);
  EXPECT_EQ(dictionary->GetBool("neologdEnabled"), true);
  EXPECT_EQ(dictionary->GetBool("namedEntityEnabled"), false);
  EXPECT_EQ(dictionary->GetBool("technicalTermsEnabled"), false);
  EXPECT_EQ(dictionary->GetBool("userDictionaryEnabled"), false);
  EXPECT_EQ(dictionary->GetBool("autoWordsEnabled"), false);
  EXPECT_EQ(dictionary->GetBool("appSpecificDictionaryEnabled"), false);
}

TEST(SettingsDocumentTest, DictionarySwitchesAreWrittenOnlyWhenStoredOrChanged) {
  ScopedTempDirectory temp("azookey_settings_dictionary_edit");
  const auto path = temp.path() / "settings.json";
  WriteText(path, R"({"dictionary":{"technicalTermsEnabled":false}})");
  auto loaded = azookey::settings::LoadSettingsDocument(path);
  ASSERT_TRUE(loaded.settings.dictionary);
  EXPECT_FALSE(loaded.settings.dictionary->technical_terms_enabled);
  EXPECT_FALSE(loaded.settings.dictionary->neologd_enabled);
  EXPECT_TRUE(loaded.settings.dictionary->sudachi_enabled);
  loaded.settings.dictionary->neologd_enabled = true;
  loaded.settings.dictionary->auto_words_enabled = false;
  const auto saved = azookey::settings::SaveSettingsDocument(path, loaded.settings);
  ASSERT_TRUE(saved.ok) << saved.error.value_or("");
  const auto parsed = azookey::ipc::json::Parse(ReadText(path));
  ASSERT_TRUE(parsed && parsed->IsObject());
  const auto* dictionary = parsed->Find("dictionary");
  ASSERT_TRUE(dictionary && dictionary->IsObject());
  // Keys left at their schema default and absent from the file keep inheriting the default.
  EXPECT_EQ(dictionary->AsObject().size(), 3u);
  EXPECT_EQ(dictionary->GetBool("neologdEnabled"), true);
  EXPECT_EQ(dictionary->GetBool("technicalTermsEnabled"), false);
  EXPECT_EQ(dictionary->GetBool("autoWordsEnabled"), false);
}

TEST(SettingsDocumentTest, DictionarySwitchesLeftAsLoadedKeepEditsMadeSinceLoading) {
  ScopedTempDirectory temp("azookey_settings_dictionary_external");
  const auto path = temp.path() / "settings.json";
  WriteText(path, R"({"dictionary":{"sudachiEnabled":true}})");
  auto loaded = azookey::settings::LoadSettingsDocument(path);
  ASSERT_TRUE(loaded.settings.dictionary_loaded);
  // Edited by hand while the settings app was open.
  WriteText(path, R"({"dictionary":{"sudachiEnabled":false}})");
  loaded.settings.dictionary->user_dictionary_enabled = false;
  const auto saved = azookey::settings::SaveSettingsDocument(path, loaded.settings);
  ASSERT_TRUE(saved.ok) << saved.error.value_or("");
  const auto parsed = azookey::ipc::json::Parse(ReadText(path));
  ASSERT_TRUE(parsed && parsed->IsObject());
  const auto* dictionary = parsed->Find("dictionary");
  ASSERT_TRUE(dictionary && dictionary->IsObject());
  EXPECT_EQ(dictionary->GetBool("sudachiEnabled"), false);
  EXPECT_EQ(dictionary->GetBool("userDictionaryEnabled"), false);
  EXPECT_EQ(dictionary->AsObject().size(), 2u);
}

TEST(SettingsDocumentTest, GenericValuesAreWrittenOnlyWhenStoredOrChanged) {
  ScopedTempDirectory temp("azookey_settings_generic_values");
  const auto path = temp.path() / "settings.json";
  WriteText(path, R"({"maxCandidates":9,"privacy":{"redactLogs":true,"secureApps":["a.exe"]}})");
  auto loaded = azookey::settings::LoadSettingsDocument(path);
  ASSERT_TRUE(loaded.warnings.empty());
  EXPECT_EQ(std::get<int64_t>(loaded.settings.values.at("maxCandidates")), 9);
  EXPECT_EQ(std::get<std::vector<std::string>>(loaded.settings.values.at("privacy.secureApps")),
            std::vector<std::string>{"a.exe"});
  EXPECT_FALSE(loaded.settings.values.contains("inputMode"));

  loaded.settings.values["inputMode"] = std::string("alnum_half");
  loaded.settings.values["privacy.custom.learning"] = true;
  loaded.settings.values["liveConversion"] = false;
  loaded.settings.values["segmentBoundaryConfidence"] = 0.75;
  const auto saved = azookey::settings::SaveSettingsDocument(path, loaded.settings);
  ASSERT_TRUE(saved.ok) << saved.error.value_or("");

  const auto parsed = azookey::ipc::json::Parse(ReadText(path));
  ASSERT_TRUE(parsed && parsed->IsObject());
  EXPECT_EQ(parsed->GetString("inputMode"), "alnum_half");
  EXPECT_EQ(parsed->GetNumber("maxCandidates"), 9.0);
  EXPECT_EQ(parsed->GetNumber("segmentBoundaryConfidence"), 0.75);
  EXPECT_EQ(parsed->Find("liveConversion"), nullptr);
  const auto* privacy = parsed->Find("privacy");
  ASSERT_TRUE(privacy && privacy->IsObject());
  EXPECT_EQ(privacy->GetBool("redactLogs"), true);
  EXPECT_EQ(privacy->GetString("crashReportConsent"), "off");
  ASSERT_NE(privacy->GetArray("secureApps"), nullptr);
  const auto* custom = privacy->Find("custom");
  ASSERT_TRUE(custom && custom->IsObject());
  EXPECT_EQ(custom->GetBool("learning"), true);
  EXPECT_EQ(custom->AsObject().size(), 1u);
}

TEST(SettingsDocumentTest, InvalidGenericValueIsRejectedWithoutWriting) {
  ScopedTempDirectory temp("azookey_settings_generic_invalid");
  const auto path = temp.path() / "settings.json";
  const std::string original = R"({"maxCandidates":9})";
  WriteText(path, original);
  azookey::settings::EditableSettings settings;
  settings.values["maxCandidates"] = int64_t{33};
  auto saved = azookey::settings::SaveSettingsDocument(path, settings);
  EXPECT_FALSE(saved.ok);
  EXPECT_EQ(saved.invalid_setting, "maxCandidates");
  EXPECT_EQ(ReadText(path), original);

  settings.values = {{"inputMode", std::string("katakana")}};
  saved = azookey::settings::SaveSettingsDocument(path, settings);
  EXPECT_EQ(saved.invalid_setting, "inputMode");
  settings.values = {{"liveConversion", int64_t{1}}};
  saved = azookey::settings::SaveSettingsDocument(path, settings);
  EXPECT_EQ(saved.invalid_setting, "liveConversion");
  settings.values = {{"reranker.nllTopK", int64_t{8}}};
  saved = azookey::settings::SaveSettingsDocument(path, settings);
  EXPECT_EQ(saved.invalid_setting, "reranker.nllTopK");
  EXPECT_EQ(ReadText(path), original);
}

TEST(SettingsDocumentTest, SafeModeIsOnlyClearedOnRequest) {
  ScopedTempDirectory temp("azookey_settings_safe_mode_clear");
  const auto path = temp.path() / "settings.json";
  WriteText(path, R"({"safeMode":{"enabled":true,"enteredAt":"2026-09-22T01:02:03Z",
    "lastCrashCount":3}})");
  auto loaded = azookey::settings::LoadSettingsDocument(path);
  EXPECT_TRUE(loaded.settings.safe_mode_enabled);
  EXPECT_EQ(loaded.settings.safe_mode_entered_at, "2026-09-22T01:02:03Z");
  EXPECT_EQ(loaded.settings.safe_mode_last_crash_count, 3);

  ASSERT_TRUE(azookey::settings::SaveSettingsDocument(path, loaded.settings).ok);
  EXPECT_EQ(azookey::ipc::json::Parse(ReadText(path))->Find("safeMode")->GetBool("enabled"), true);

  loaded.settings.clear_safe_mode = true;
  ASSERT_TRUE(azookey::settings::SaveSettingsDocument(path, loaded.settings).ok);
  const auto parsed = azookey::ipc::json::Parse(ReadText(path));
  const auto* safe_mode = parsed->Find("safeMode");
  ASSERT_TRUE(safe_mode && safe_mode->IsObject());
  EXPECT_EQ(safe_mode->GetBool("enabled"), false);
  EXPECT_EQ(safe_mode->GetString("enteredAt"), "2026-09-22T01:02:03Z");
  EXPECT_EQ(safe_mode->GetNumber("lastCrashCount"), 3.0);
}

TEST(SettingsDocumentTest, SafeModeEnteredAgainAfterLoadingIsNotCleared) {
  ScopedTempDirectory temp("azookey_settings_safe_mode_reentry");
  const auto path = temp.path() / "settings.json";
  WriteText(path, R"({"safeMode":{"enabled":true,"enteredAt":"2026-09-22T01:02:03Z"}})");
  auto loaded = azookey::settings::LoadSettingsDocument(path);
  // The Host enters SafeMode again before the user saves the clear.
  WriteText(path, R"({"safeMode":{"enabled":true,"enteredAt":"2026-09-23T04:05:06Z"}})");
  loaded.settings.clear_safe_mode = true;
  const auto saved = azookey::settings::SaveSettingsDocument(path, loaded.settings);
  ASSERT_TRUE(saved.ok) << saved.error.value_or("");
  EXPECT_TRUE(saved.safe_mode_reentered);
  const auto parsed = azookey::ipc::json::Parse(ReadText(path));
  EXPECT_EQ(parsed->Find("safeMode")->GetBool("enabled"), true);
}

TEST(SettingsDocumentTest, MissingDictionaryLoadsSchemaDefaults) {
  ScopedTempDirectory temp("azookey_settings_dictionary_missing");
  const auto loaded = azookey::settings::LoadSettingsDocument(temp.path() / "settings.json");
  ASSERT_TRUE(loaded.settings.dictionary);
  EXPECT_FALSE(loaded.settings.dictionary->neologd_enabled);
  EXPECT_TRUE(loaded.settings.dictionary->sudachi_enabled);
  EXPECT_TRUE(loaded.settings.dictionary->named_entity_enabled);
  EXPECT_TRUE(loaded.settings.dictionary->technical_terms_enabled);
  EXPECT_TRUE(loaded.settings.dictionary->user_dictionary_enabled);
  EXPECT_TRUE(loaded.settings.dictionary->auto_words_enabled);
  EXPECT_TRUE(loaded.settings.dictionary->app_specific_dictionary_enabled);
}

TEST(SettingsDocumentTest, OnlyTurningNeologdOnRequiresAHostRestart) {
  azookey::settings::EditableSettings off;
  off.dictionary = azookey::settings::DictionarySettings{};
  auto on = off;
  on.dictionary->neologd_enabled = true;
  auto other = off;
  other.dictionary->sudachi_enabled = false;
  other.log_level = "debug";
  other.model_enabled = false;

  EXPECT_EQ(azookey::settings::SettingsRequiringHostRestart(off, on),
            std::vector<std::string>{"dictionary.neologdEnabled"});
  EXPECT_TRUE(azookey::settings::SettingsRequiringHostRestart(on, off).empty());
  EXPECT_TRUE(azookey::settings::SettingsRequiringHostRestart(on, on).empty());
  EXPECT_TRUE(azookey::settings::SettingsRequiringHostRestart(off, other).empty());
}

TEST(SettingsDocumentTest, ChangedSettingPathsIsEmptyForTheControlsAsLoaded) {
  ScopedTempDirectory temp("azookey_settings_changed_paths");
  const auto path = temp.path() / "settings.json";
  // An invalid generic value and a backend this UI cannot select are shown without counting.
  WriteText(path, R"({"maxCandidates":"many","model":{"backendPreference":"cuda"},
    "dictionary":{"sudachiEnabled":false}})");
  const auto loaded = azookey::settings::LoadSettingsDocument(path);
  ASSERT_FALSE(loaded.settings.model_backend_preference.has_value());

  // What the controls hand to a save when nothing was touched: every generic field at its shown
  // value, the dictionary switches, and the profiles as loaded.
  auto edited = loaded.settings;
  edited.dictionary = loaded.settings.dictionary.value_or(azookey::settings::DictionarySettings{});
  for (const auto& field : azookey::settings::GenericSettingFields()) {
    edited.values[std::string(field.path)] =
        azookey::settings::SettingValueOrDefault(field, loaded.settings.values);
  }
  EXPECT_TRUE(azookey::settings::ChangedSettingPaths(edited, loaded.settings).empty());

  const auto missing = azookey::settings::LoadSettingsDocument(temp.path() / "absent.json");
  auto untouched = missing.settings;
  untouched.dictionary = azookey::settings::DictionarySettings{};
  EXPECT_TRUE(azookey::settings::ChangedSettingPaths(untouched, missing.settings).empty());
}

TEST(SettingsDocumentTest, ChangedSettingPathsCountsEachKeyASaveWouldWrite) {
  azookey::settings::EditableSettings loaded;
  loaded.dictionary = azookey::settings::DictionarySettings{};
  loaded.values = {{"maxCandidates", int64_t{12}}};
  loaded.profiles_by_app = azookey::settings::AppProfiles{};

  auto edited = loaded;
  edited.model_enabled = false;
  edited.model_backend_preference = "cpu";
  edited.model_selected_path = "C:\\models\\a.gguf";
  edited.openai_api_key_changed = true;
  edited.log_level = "debug";
  edited.crash_report_consent = "local";
  edited.dictionary->sudachi_enabled = false;
  edited.dictionary->neologd_enabled = true;
  edited.clear_safe_mode = true;
  edited.values["maxCandidates"] = int64_t{9};
  edited.values["liveConversion"] = true;
  // Still the default, so not a change.
  edited.values["inputMode"] = std::string("hiragana");

  const auto paths = azookey::settings::ChangedSettingPaths(edited, loaded);
  EXPECT_EQ(paths, (std::vector<std::string>{
                       "model.enabled", "model.backendPreference", "model.selectedPath",
                       "openAiApiKey", "logLevel", "privacy.crashReportConsent",
                       "dictionary.sudachiEnabled", "dictionary.neologdEnabled", "safeMode.enabled",
                       "liveConversion", "maxCandidates"}));

  // Changing a value back is no longer a change.
  auto reverted = loaded;
  reverted.model_backend_preference = loaded.model_backend_preference;
  EXPECT_TRUE(azookey::settings::ChangedSettingPaths(reverted, loaded).empty());
}

TEST(SettingsDocumentTest, SaveBarAllowsARepairingSaveWithoutChanges) {
  ScopedTempDirectory temp("azookey_settings_save_bar");
  const auto path = temp.path() / "settings.json";
  using azookey::settings::SaveRepairsLoadedDocument;

  EXPECT_FALSE(SaveRepairsLoadedDocument(azookey::settings::LoadSettingsDocument(path)));
  WriteText(path, R"({"logLevel":"debug"})");
  EXPECT_FALSE(SaveRepairsLoadedDocument(azookey::settings::LoadSettingsDocument(path)));
  // The load message promises that a save removes the invalid entry or rebuilds the file.
  WriteText(path, R"({"maxCandidates":"many"})");
  EXPECT_TRUE(SaveRepairsLoadedDocument(azookey::settings::LoadSettingsDocument(path)));
  WriteText(path, "{not json");
  EXPECT_TRUE(SaveRepairsLoadedDocument(azookey::settings::LoadSettingsDocument(path)));
  // A key that cannot be decrypted is kept by a save, so a save would repair nothing.
  azookey::settings::SettingsDocumentResult undecryptable;
  undecryptable.status = azookey::settings::SettingsDocumentStatus::Loaded;
  undecryptable.settings.openai_api_key_unavailable = true;
  undecryptable.warnings.emplace_back("openAiApiKey could not be decrypted");
  EXPECT_FALSE(SaveRepairsLoadedDocument(undecryptable));
  undecryptable.warnings.emplace_back("maxCandidates: wrong type");
  EXPECT_TRUE(SaveRepairsLoadedDocument(undecryptable));
  azookey::settings::SettingsDocumentResult unreadable;
  unreadable.status = azookey::settings::SettingsDocumentStatus::ReadError;
  unreadable.warnings.emplace_back("ignored");
  EXPECT_FALSE(SaveRepairsLoadedDocument(unreadable));

  using azookey::settings::SaveBarAllowsSave;
  EXPECT_FALSE(SaveBarAllowsSave(true, false, 0, false));
  EXPECT_TRUE(SaveBarAllowsSave(true, false, 2, false));
  EXPECT_TRUE(SaveBarAllowsSave(true, false, 0, true));
  EXPECT_FALSE(SaveBarAllowsSave(true, true, 2, false));
  // A file that could not be read is never overwritten.
  EXPECT_FALSE(SaveBarAllowsSave(false, false, 2, true));
}

TEST(SettingsDocumentTest, OnlyPanesWithKeysToSaveShowTheSaveBar) {
  using azookey::settings::PaneShowsSaveBar;
  for (const auto* pane : {L"input", L"candidates", L"dictionary", L"ai-conversion", L"ai-service",
                           L"privacy", L"profiles", L"advanced", L"version"}) {
    EXPECT_TRUE(PaneShowsSaveBar(pane));
  }
  // The "データとツール" panes.
  for (const auto* pane : {L"learning", L"proofread"}) {
    EXPECT_FALSE(PaneShowsSaveBar(pane));
  }
}

TEST(SettingsDocumentTest, UnknownAndMistypedDictionaryLayerValuesAreRemoved) {
  ScopedTempDirectory temp("azookey_settings_dictionary_invalid");
  const auto path = temp.path() / "settings.json";
  WriteText(path, R"({"dictionary":{
    "sudachiEnabled":"false","neologdEnabled":1,"namedEntityEnabled":null,
    "technicalTermsEnabled":{},"userDictionaryEnabled":[],"autoWordsEnabled":"true",
    "appSpecificDictionaryEnabled":0,"baseEnabled":false,"unknown":true,
    "verifyOnLoad":true,"categoryBoosts":{"technical":1.2}}})");
  const auto saved =
      azookey::settings::SaveSettingsDocument(path, azookey::settings::EditableSettings{});
  ASSERT_TRUE(saved.ok) << saved.error.value_or("");
  EXPECT_EQ(saved.warnings.size(), 11u);
  const auto parsed = azookey::ipc::json::Parse(ReadText(path));
  ASSERT_TRUE(parsed && parsed->IsObject());
  const auto* dictionary = parsed->Find("dictionary");
  ASSERT_TRUE(dictionary && dictionary->IsObject());
  EXPECT_TRUE(dictionary->AsObject().empty());

  WriteText(path, R"({"dictionary":false})");
  const auto wrong_object =
      azookey::settings::SaveSettingsDocument(path, azookey::settings::EditableSettings{});
  ASSERT_TRUE(wrong_object.ok);
  EXPECT_EQ(wrong_object.warnings.size(), 1u);
  const auto restored = azookey::ipc::json::Parse(ReadText(path));
  ASSERT_TRUE(restored);
  EXPECT_EQ(restored->Find("dictionary"), nullptr);
}

// M47: the Host records SafeMode here and only the user clears it, so an
// unrelated save must carry it through unchanged.
TEST(SettingsDocumentTest, SafeModeSurvivesASaveAndInvalidFieldsAreDropped) {
  const auto dir = TestDir("azookey_settings_document_safe_mode");
  const auto path = dir / "settings.json";
  WriteText(path, R"({
    "safeMode": {
      "enabled": true,
      "enteredAt": "2026-09-22T01:02:03Z",
      "lastCrashCount": 3,
      "unknownKey": 1
    }
  })");

  azookey::settings::EditableSettings settings;
  const auto saved = azookey::settings::SaveSettingsDocument(path, settings);
  ASSERT_TRUE(saved.ok) << saved.error.value_or("");

  const auto parsed = azookey::ipc::json::Parse(ReadText(path));
  ASSERT_TRUE(parsed && parsed->IsObject());
  const auto& root = parsed->AsObject();
  ASSERT_TRUE(root.contains("safeMode"));
  const auto& safe_mode = root.at("safeMode").AsObject();
  EXPECT_TRUE(safe_mode.at("enabled").AsBool());
  EXPECT_EQ(safe_mode.at("enteredAt").AsString(), "2026-09-22T01:02:03Z");
  EXPECT_EQ(safe_mode.at("lastCrashCount").AsNumber(), 3.0);
  EXPECT_FALSE(safe_mode.contains("unknownKey"));
}

TEST(SettingsDocumentTest, BenchmarkHistoryIsShownAndSurvivesASave) {
  const auto dir = TestDir("azookey_settings_benchmark_history");
  const auto path = dir / "settings.json";
  WriteText(path, R"({
    "model": {
      "selectedPath": "C:/models/zenz.gguf",
      "benchmarkHistory": [
        {"path": "C:/models/zenz.gguf", "backend": "cpu", "status": "success",
         "p50_ms": 18.5, "p95_ms": 41.0, "p99_ms": 78.4, "extra": 1},
        "not an object",
        {"backend": "cpu", "status": "timeout"}
      ]
    }
  })");

  auto loaded = azookey::settings::LoadSettingsDocument(path);

  ASSERT_EQ(loaded.settings.benchmark_history.size(), 2u);
  const auto& first = loaded.settings.benchmark_history[0];
  EXPECT_EQ(first.path, "C:/models/zenz.gguf");
  EXPECT_EQ(first.backend, "cpu");
  EXPECT_EQ(first.status, "success");
  ASSERT_TRUE(first.p50_ms);
  EXPECT_DOUBLE_EQ(*first.p50_ms, 18.5);
  EXPECT_FALSE(loaded.settings.benchmark_history[1].p50_ms);

  loaded.settings.model_selected_path = "C:/models/other.gguf";
  ASSERT_TRUE(azookey::settings::SaveSettingsDocument(path, loaded.settings).ok);
  const auto parsed = azookey::ipc::json::Parse(ReadText(path));
  ASSERT_TRUE(parsed && parsed->IsObject());
  const auto& model = parsed->AsObject().at("model").AsObject();
  EXPECT_EQ(model.at("selectedPath").AsString(), "C:/models/other.gguf");
  ASSERT_TRUE(model.at("benchmarkHistory").IsArray());
  EXPECT_EQ(model.at("benchmarkHistory").AsArray().size(), 3u);
  std::filesystem::remove_all(dir);
}

TEST(SettingsDocumentTest, ClearingTheModelWritesAnEmptySelectedPath) {
  // sideload-packaging-spec section 3.7: an empty selectedPath is "no model chosen yet", the
  // schema default; clearing the choice in the "AI 変換" pane saves it that way.
  const auto dir = TestDir("azookey_settings_clear_model");
  const auto path = dir / "settings.json";
  WriteText(path, R"({"model": {"enabled": true, "selectedPath": "C:/models/zenz.gguf"}})");

  auto loaded = azookey::settings::LoadSettingsDocument(path);
  ASSERT_EQ(loaded.settings.model_selected_path, "C:/models/zenz.gguf");
  auto edited = loaded.settings;
  edited.model_selected_path.clear();
  EXPECT_EQ(azookey::settings::ChangedSettingPaths(edited, loaded.settings),
            (std::vector<std::string>{"model.selectedPath"}));

  ASSERT_TRUE(azookey::settings::SaveSettingsDocument(path, edited).ok);
  const auto parsed = azookey::ipc::json::Parse(ReadText(path));
  ASSERT_TRUE(parsed && parsed->IsObject());
  const auto& model = parsed->AsObject().at("model").AsObject();
  ASSERT_TRUE(model.contains("selectedPath"));
  EXPECT_EQ(model.at("selectedPath").AsString(), "");
  EXPECT_TRUE(model.at("enabled").AsBool());
  EXPECT_TRUE(azookey::settings::LoadSettingsDocument(path).settings.model_selected_path.empty());
  std::filesystem::remove_all(dir);
}

TEST(SettingsDocumentTest, CanonicalBenchmarkHistoryIsReadAndConcurrentAppendSurvivesSave) {
  const auto dir = TestDir("azookey_settings_canonical_benchmark_history");
  const auto path = dir / "settings.json";
  WriteText(path, R"({"model":{"benchmarkHistory":[{
    "path":"C:/models/zenz.gguf","completedAt":"2026-10-10T10:00:00Z",
    "backend":"cpu","status":"timeout","p50_ms":1,"p95_ms":2,"p99_ms":3,
    "load_ms":4,"rss_mb":5,"vram_mb":null,"iterations_completed":6,"error":null
  }]}})");
  auto loaded = azookey::settings::LoadSettingsDocument(path);
  ASSERT_EQ(loaded.settings.benchmark_history.size(), 1u);
  const auto& entry = loaded.settings.benchmark_history.front();
  EXPECT_EQ(entry.path, "C:/models/zenz.gguf");
  EXPECT_EQ(entry.completed_at, "2026-10-10T10:00:00Z");
  EXPECT_EQ(entry.load_ms, 4);
  EXPECT_EQ(entry.rss_mb, 5);
  EXPECT_FALSE(entry.vram_mb);
  EXPECT_EQ(entry.iterations_completed, 6u);
  EXPECT_FALSE(entry.error);
  // Host appended after this UI loaded. Saving a stale UI snapshot must retain
  // the latest history on disk rather than the UI's earlier copy.
  WriteText(path, R"({"model":{"benchmarkHistory":[{
    "path":"C:/models/other.gguf","completedAt":"2026-10-10T10:01:00Z",
    "backend":"vulkan","status":"error","p50_ms":0,"p95_ms":0,"p99_ms":0,
    "load_ms":2,"rss_mb":5,"vram_mb":7,"iterations_completed":0,"error":"load_failed"
  }]}})");
  loaded.settings.model_selected_path = "C:/models/selected.gguf";
  ASSERT_TRUE(azookey::settings::SaveSettingsDocument(path, loaded.settings).ok);
  const auto saved = azookey::settings::LoadSettingsDocument(path);
  ASSERT_EQ(saved.settings.benchmark_history.size(), 1u);
  const auto& latest = saved.settings.benchmark_history.front();
  EXPECT_EQ(latest.path, "C:/models/other.gguf");
  EXPECT_EQ(latest.completed_at, "2026-10-10T10:01:00Z");
  EXPECT_EQ(latest.vram_mb, 7);
  EXPECT_EQ(latest.error, "load_failed");
  EXPECT_EQ(saved.settings.model_selected_path, "C:/models/selected.gguf");
  std::filesystem::remove_all(dir);
}
