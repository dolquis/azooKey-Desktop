#include "SettingsFields.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace azookey::settings {
namespace {

using Kind = SettingKind;
using Pane = SettingsPane;
using Strings = std::vector<std::string>;

constexpr double kInt32Max = 2147483647.0;

SettingField Bool(std::string_view path, Pane pane, std::string_view section, bool value,
                  std::optional<SettingCondition> when = std::nullopt, bool described = false) {
  return {path, Kind::Bool, pane, section, value, std::nullopt, std::nullopt, {}, when, described};
}

SettingField Integer(std::string_view path, Pane pane, std::string_view section, int64_t value,
                     double minimum, std::optional<double> maximum,
                     std::optional<SettingCondition> when = std::nullopt, bool described = false) {
  return {path, Kind::Integer, pane, section, value, minimum, maximum, {}, when, described};
}

SettingField Number(std::string_view path, Pane pane, std::string_view section, double value,
                    std::optional<SettingCondition> when = std::nullopt) {
  return {path, Kind::Number, pane, section, value, 0.0, 1.0, {}, when, false};
}

SettingField Enum(std::string_view path, Pane pane, std::string_view section,
                  std::string_view value, std::vector<std::string_view> options,
                  std::optional<SettingCondition> when = std::nullopt, bool described = false) {
  return {path,         Kind::Enum,         pane, section,  std::string(value), std::nullopt,
          std::nullopt, std::move(options), when, described};
}

SettingField Text(std::string_view path, Pane pane, std::string_view section,
                  std::string_view value, std::optional<SettingCondition> when = std::nullopt,
                  bool described = false) {
  return {path,         Kind::String, pane, section, std::string(value),
          std::nullopt, std::nullopt, {},   when,    described};
}

SettingField List(std::string_view path, Pane pane, std::string_view section,
                  std::optional<SettingCondition> when = std::nullopt) {
  return {path,         Kind::StringList, pane, section, Strings{},
          std::nullopt, std::nullopt,     {},   when,    true};
}

std::vector<SettingField> BuildFields() {
  // Conditions follow the feature specs: a field is disabled where the spec says it has no effect.
  const SettingCondition update{"autoUpdate.enabled", "true"};
  const SettingCondition custom_romaji{"inputStyle", "custom"};
  const SettingCondition punctuation{"dynamicPunctuation", "true"};
  const SettingCondition batch{"batchRomajiConversion", "true"};
  const SettingCondition ai_cleanup{"batchConversionMode", "ai-cleanup"};
  const SettingCondition english_dictionary{"inlineEnglishDictionary", "true"};
  const SettingCondition symbols{"symbolRewriter", "true"};
  const SettingCondition emoji{"emojiRewriter", "true"};
  const SettingCondition brackets{"bracketPairing", "true"};
  const SettingCondition mining{"autoWordRegistration.miningEnabled", "true"};
  const SettingCondition trending{"autoWordRegistration.trendingEnabled", "true"};
  const SettingCondition openai{"aiBackend", "openai"};
  const SettingCondition custom_privacy{"privacy.mode", "custom"};

  return {
      Bool("autoUpdate.enabled", Pane::General, "AutoUpdate", true),
      Enum("autoUpdate.channel", Pane::General, "AutoUpdate", "stable", {"stable", "beta"}, update),
      Integer("autoUpdate.checkIntervalHours", Pane::General, "AutoUpdate", 24, 1.0, kInt32Max,
              update),

      Enum("inputMode", Pane::Input, "InputMethod", "hiragana",
           {"hiragana", "alnum_half", "alnum_full"}),
      Enum("inputStyle", Pane::Input, "InputMethod", "default", {"default", "custom"}),
      Text("customRomajiTablePath", Pane::Input, "InputMethod",
           "%LOCALAPPDATA%\\azooKey\\custom-romaji.tsv", custom_romaji),
      Bool("liveConversion", Pane::Input, "InputMethod", false),
      Bool("predictionEnabled", Pane::Input, "InputMethod", true),

      Bool("dynamicPunctuation", Pane::Input, "Punctuation", false, std::nullopt, true),
      Enum("dynamicPunctuationStyle", Pane::Input, "Punctuation", "ja", {"ja", "fullwidth_latin"},
           punctuation),
      Enum("dynamicPunctuationStability", Pane::Input, "Punctuation", "onPause",
           {"onPause", "eager"}, punctuation),
      Integer("dynamicPunctuationIdleMs", Pane::Input, "Punctuation", 400, 1.0, kInt32Max,
              punctuation, true),
      Number("segmentBoundaryConfidence", Pane::Input, "Punctuation", 0.5, punctuation),
      Text("punctuationRulesPath", Pane::Input, "Punctuation",
           "%LOCALAPPDATA%\\azooKey\\punctuation-rules.tsv", punctuation),

      Bool("batchRomajiConversion", Pane::Input, "Batch", false),
      Enum("batchRomajiPreviewStyle", Pane::Input, "Batch", "kana", {"kana", "romaji"}, batch),
      Enum("batchConversionMode", Pane::Input, "Batch", "neural", {"neural", "ai-cleanup"}, batch,
           true),
      Bool("batchAutoPunctuation", Pane::Input, "Batch", false, ai_cleanup),

      Bool("inlineEnglishCandidates", Pane::Input, "English", false),
      Bool("inlineEnglishCaseVariants", Pane::Input, "English", true),
      Bool("fullWidthEnglishCandidate", Pane::Input, "English", false),
      Integer("inlineEnglishMinLength", Pane::Input, "English", 2, 1.0, kInt32Max),
      Number("inlineEnglishPromoteThreshold", Pane::Input, "English", 0.6),
      Bool("inlineEnglishDictionary", Pane::Input, "English", false),
      Text("inlineEnglishDictionaryPath", Pane::Input, "English",
           "%LOCALAPPDATA%\\azooKey\\dict\\english-words.tsv", english_dictionary),

      Bool("numberRewriter", Pane::Input, "Rewriters", false),
      Bool("katakanaRewriter", Pane::Input, "Rewriters", false),
      Bool("symbolRewriter", Pane::Input, "Rewriters", false),
      Text("symbolDataPath", Pane::Input, "Rewriters", "", symbols, true),
      Bool("emojiRewriter", Pane::Input, "Rewriters", false),
      Bool("emojiTriggerSearch", Pane::Input, "Rewriters", true, emoji, true),
      Integer("emojiMaxCandidates", Pane::Input, "Rewriters", 12, 1.0, 50.0, emoji),
      Integer("emojiTriggerMinQueryLength", Pane::Input, "Rewriters", 1, 1.0, 8.0, emoji),
      Text("emojiDataPath", Pane::Input, "Rewriters", "", emoji, true),

      Bool("bracketPairing", Pane::Input, "Brackets", false),
      Enum("bracketPairingTrigger", Pane::Input, "Brackets", "immediate",
           {"immediate", "composition"}, brackets),
      Bool("bracketSkipOverClosing", Pane::Input, "Brackets", true, brackets),
      Bool("bracketBackspaceDeletesPair", Pane::Input, "Brackets", true, brackets),
      Bool("bracketPairingInAlnumMode", Pane::Input, "Brackets", true, brackets),
      Bool("bracketSymmetricQuotePairing", Pane::Input, "Brackets", false, brackets),
      Bool("bracketWrapSelection", Pane::Input, "Brackets", false, brackets),
      Enum("bracketPairingAppPolicy", Pane::Input, "Brackets", "denylist",
           {"denylist", "allowlist"}, brackets, true),
      List("bracketPairingApps", Pane::Input, "Brackets", brackets),
      Text("bracketPairsPath", Pane::Input, "Brackets", "", brackets, true),

      Bool("autoWordRegistration.miningEnabled", Pane::Dictionary, "AutoWords", true),
      Integer("autoWordRegistration.miningMinCount", Pane::Dictionary, "AutoWords", 3, 1.0, 100.0,
              mining),
      Bool("autoWordRegistration.trendingEnabled", Pane::Dictionary, "AutoWords", false),
      Integer("autoWordRegistration.trendingIntervalHours", Pane::Dictionary, "AutoWords", 24, 1.0,
              8760.0, trending),
      Enum("autoWordRegistration.registrationMode", Pane::Dictionary, "AutoWords", "confirm",
           {"confirm", "auto"}),
      Enum("typoCorrectionMode", Pane::Dictionary, "Typo", "suggest",
           {"off", "suggest", "auto_replace"}),
      Integer("typoMinCount", Pane::Dictionary, "Typo", 3, 1.0, 100.0),

      Enum("aiBackend", Pane::Ai, "AiBackend", "none", {"none", "openai", "local-zenzai"}),
      Bool("llmMagicConversion", Pane::Ai, "AiBackend", false),
      Bool("includeContextInAITransform", Pane::Ai, "AiBackend", true),
      Text("openAiApiEndpoint", Pane::Ai, "OpenAi", "https://api.openai.com/v1", openai, true),
      Text("openAiModel", Pane::Ai, "OpenAi", "gpt-4o-mini", openai),
      Integer("openAiTimeoutMs", Pane::Ai, "OpenAi", 30000, 1000.0, 120000.0, openai),

      Enum("privacy.mode", Pane::Privacy, "PrivacyMode", "normal",
           {"normal", "private", "secure", "offline", "custom"}, std::nullopt, true),
      Bool("privacy.custom.learning", Pane::Privacy, "PrivacyMode", false, custom_privacy),
      Bool("privacy.custom.prediction", Pane::Privacy, "PrivacyMode", true, custom_privacy),
      Bool("privacy.custom.aiCandidate", Pane::Privacy, "PrivacyMode", true, custom_privacy),
      Bool("privacy.custom.externalAi", Pane::Privacy, "PrivacyMode", false, custom_privacy, true),
      Bool("privacy.custom.detailedLogging", Pane::Privacy, "PrivacyMode", false, custom_privacy,
           true),
      Bool("privacy.redactLogs", Pane::Privacy, "PrivacyMode", true),
      Bool("privacy.autoSecureInput", Pane::Privacy, "SecureInput", true, std::nullopt, true),
      List("privacy.secureApps", Pane::Privacy, "SecureInput"),
      Bool("privacy.showSecureIndicator", Pane::Privacy, "SecureInput", true),

      Enum("powerProfile", Pane::Advanced, "Performance", "auto",
           {"auto", "performance", "battery_saver"}),
      Integer("inferenceThreads", Pane::Advanced, "Performance", 0, 0.0, 8.0, std::nullopt, true),
      Integer("maxCandidates", Pane::Advanced, "Performance", 9, 1.0, 32.0),
      Integer("maxContextLength", Pane::Advanced, "Performance", 10, 0.0, 30.0, std::nullopt, true),
      Bool("contextReselection", Pane::Advanced, "Experimental", false),
      Bool("postCommitLint", Pane::Advanced, "Experimental", false),
      Bool("retroactiveRecompute", Pane::Advanced, "Experimental", false),
      Bool("sentenceCompletion", Pane::Advanced, "Experimental", false),
  };
}

bool IsWholeNumber(double value) { return std::isfinite(value) && std::floor(value) == value; }

bool InRange(const SettingField& field, double value) {
  return (!field.minimum || value >= *field.minimum) && (!field.maximum || value <= *field.maximum);
}

std::string ResourceSuffix(std::string_view text) {
  std::string suffix(text);
  std::replace_if(
      suffix.begin(), suffix.end(),
      [](char ch) {
        return !((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'));
      },
      '_');
  return suffix;
}

}  // namespace

const std::vector<SettingField>& GenericSettingFields() {
  static const std::vector<SettingField> fields = BuildFields();
  return fields;
}

const std::vector<std::string_view>& DedicatedSettingPaths() {
  static const std::vector<std::string_view> paths = {
      "model.enabled",
      "model.selectedPath",
      "model.backendPreference",
      "logLevel",
      "openAiApiKey",
      "privacy.crashReportConsent",
      "dictionary.sudachiEnabled",
      "dictionary.neologdEnabled",
      "dictionary.namedEntityEnabled",
      "dictionary.technicalTermsEnabled",
      "dictionary.userDictionaryEnabled",
      "dictionary.autoWordsEnabled",
      "dictionary.appSpecificDictionaryEnabled",
      "safeMode.enabled",
  };
  return paths;
}

const std::vector<UneditedSetting>& UneditedSettings() {
  static const std::vector<UneditedSetting> settings = {
      {"profilesByApp", "per-app profile editor (DEV-1535)"},
      {"promptPrefixByApp", "legacy; new edits go to profilesByApp (app-profile-spec section 6)"},
      {"backendPreference", "root tier is not bound to the UI (section 3.6)"},
      {"epPreference", "root tier is not bound to the UI (section 3.6)"},
      {"model.epPreference", "exposed once a build links the backend (section 3.7)"},
      {"model.nGpuLayers", "model pane (DEV-1530)"},
      {"model.autoLoadOnHostStart", "model pane (DEV-1530)"},
      {"model.fallbackToSimpleConverter", "model pane (DEV-1530)"},
      {"model.benchmarkOnModelChange", "model pane (DEV-1530)"},
      {"model.benchmarkHistory", "written by the Host"},
      {"reranker.nllRerankEnabled", "no dedicated UI (neural-reranker-spec B9)"},
      {"reranker.nllTopK", "no dedicated UI (neural-reranker-spec B9)"},
      {"reranker.nllWeight", "no dedicated UI (neural-reranker-spec B9)"},
      {"reranker.nllBudgetMs", "no dedicated UI (neural-reranker-spec B9)"},
      {"reranker.nllFailureThreshold", "no dedicated UI (neural-reranker-spec B9)"},
      {"safeMode.enteredAt", "written by the Host; shown only"},
      {"safeMode.lastCrashCount", "written by the Host; shown only"},
  };
  return settings;
}

const SettingField* FindSettingField(std::string_view path) {
  const auto& fields = GenericSettingFields();
  const auto it = std::find_if(fields.begin(), fields.end(),
                               [path](const SettingField& field) { return field.path == path; });
  return it == fields.end() ? nullptr : &*it;
}

SettingValue SettingValueOrDefault(const SettingField& field, const SettingValues& values) {
  const auto it = values.find(field.path);
  if (it != values.end() && !ValidateSettingValue(field, it->second)) return it->second;
  return field.default_value;
}

std::optional<std::string> ValidateSettingValue(const SettingField& field,
                                                const SettingValue& value) {
  switch (field.kind) {
    case Kind::Bool:
      if (std::holds_alternative<bool>(value)) return std::nullopt;
      break;
    case Kind::Integer:
      if (const auto* number = std::get_if<int64_t>(&value)) {
        if (InRange(field, static_cast<double>(*number))) return std::nullopt;
        return "out of range";
      }
      break;
    case Kind::Number:
      if (const auto* number = std::get_if<double>(&value)) {
        if (std::isfinite(*number) && InRange(field, *number)) return std::nullopt;
        return "out of range";
      }
      break;
    case Kind::Enum:
      if (const auto* text = std::get_if<std::string>(&value)) {
        if (std::find(field.options.begin(), field.options.end(), *text) != field.options.end()) {
          return std::nullopt;
        }
        return "not one of the allowed values";
      }
      break;
    case Kind::String:
      if (std::holds_alternative<std::string>(value)) return std::nullopt;
      break;
    case Kind::StringList:
      if (std::holds_alternative<Strings>(value)) return std::nullopt;
      break;
  }
  return "wrong type";
}

bool IsSettingFieldActive(const SettingField& field, const SettingValues& values) {
  if (!field.active_when) return true;
  const auto* condition = FindSettingField(field.active_when->path);
  if (!condition) return true;
  const auto value = SettingValueOrDefault(*condition, values);
  if (const auto* flag = std::get_if<bool>(&value)) {
    return (*flag ? "true" : "false") == field.active_when->equals;
  }
  if (const auto* text = std::get_if<std::string>(&value)) {
    return *text == field.active_when->equals;
  }
  return true;
}

std::vector<std::string> ParseSettingList(std::string_view text) {
  std::vector<std::string> items;
  size_t start = 0;
  while (start <= text.size()) {
    const auto end = text.find_first_of("\r\n", start);
    auto line =
        text.substr(start, end == std::string_view::npos ? text.size() - start : end - start);
    const auto first = line.find_first_not_of(" \t");
    if (first != std::string_view::npos) {
      line = line.substr(first, line.find_last_not_of(" \t") - first + 1);
      items.emplace_back(line);
    }
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return items;
}

std::string FormatSettingList(const std::vector<std::string>& items) {
  std::string text;
  for (const auto& item : items) {
    if (!text.empty()) text.push_back('\r');
    text += item;
  }
  return text;
}

std::string SettingLabelResource(std::string_view path) {
  return "Setting_" + ResourceSuffix(path);
}

std::string SettingDescriptionResource(std::string_view path) {
  return "SettingNote_" + ResourceSuffix(path);
}

std::string SettingOptionResource(std::string_view path, std::string_view option) {
  return "SettingOption_" + ResourceSuffix(path) + "_" + ResourceSuffix(option);
}

std::string SettingSectionResource(std::string_view section) {
  return "SettingSection_" + ResourceSuffix(section);
}

}  // namespace azookey::settings
