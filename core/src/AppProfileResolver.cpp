#include "azookey/core/AppProfileResolver.h"

#include <algorithm>
#include <cmath>
#include <map>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

namespace azookey::core {
namespace {
namespace j = ipc::json;
void Warn(std::vector<std::string>* warnings, const char* text) {
  if (warnings) warnings->emplace_back(text);
}
bool Enum(const j::Value& value, std::initializer_list<std::string_view> choices) {
  return value.IsString() &&
         std::find(choices.begin(), choices.end(), value.AsString()) != choices.end();
}
std::optional<j::Value> Validate(std::string_view key, const j::Value& value) {
  if (key == "profileName" || key == "promptPrefix") {
    if (value.IsString()) return value;
  } else if (key == "predictionEnabled" || key == "sentenceCompletion" ||
             key == "learningEnabled" || key == "preferTechnicalTerms") {
    if (value.IsBool()) return value;
  } else if (key == "aiBackend") {
    if (Enum(value, {"auto", "local-zenzai", "openai", "none"})) return value;
  } else if (key == "style") {
    if (Enum(value, {"auto", "polite", "casual", "technical"})) return value;
  } else if (key == "privacyMode") {
    if (Enum(value, {"inherit", "normal", "private", "secure"})) return value;
  } else if (key == "bracketPairing") {
    if (Enum(value, {"auto", "on", "off"})) return value;
  }
  return {};
}
const j::Value* ProcessMatch(const j::Object& values, std::string_view name, AppNameEqual equal) {
  const j::Value* selected = nullptr;
  // UTF-8 key ordering is deterministic Unicode scalar ordering for valid JSON.
  // The greatest original key wins, independent of JSON source enumeration.
  for (const auto& [key, value] : values)
    if (equal(key, name)) selected = &value;
  return selected;
}
void WarnCollisions(const j::Object& values, std::vector<std::string>* warnings) {
  if (!warnings) return;
  std::map<std::string, bool> seen;
  std::vector<std::string_view> unicode_names;
  for (const auto& [name, unused] : values) {
    if (std::any_of(name.begin(), name.end(), [](unsigned char ch) { return ch >= 128; })) {
      if (std::any_of(unicode_names.begin(), unicode_names.end(),
                      [&](auto previous) { return EqualAppName(previous, name); }))
        Warn(warnings, "app profile case collision: greatest original key selected");
      unicode_names.push_back(name);
      continue;
    }
    auto key = name;
    for (auto& ch : key)
      if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    if (!seen.emplace(std::move(key), true).second)
      Warn(warnings, "app profile case collision: greatest original key selected");
  }
}
}  // namespace

bool EqualAsciiAppName(std::string_view left, std::string_view right) {
  const auto lower = [](unsigned char ch) {
    return ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch;
  };
  return left.size() == right.size() &&
         std::equal(left.begin(), left.end(), right.begin(),
                    [&](unsigned char a, unsigned char b) { return lower(a) == lower(b); });
}
bool EqualAppName(std::string_view left, std::string_view right) {
#ifdef _WIN32
  const auto ascii = [](std::string_view name) {
    return std::all_of(name.begin(), name.end(), [](unsigned char ch) { return ch < 128; });
  };
  if (!ascii(left) || !ascii(right)) {
    const auto wide = [](std::string_view text) {
      if (text.empty() || text.size() > 32768) return std::wstring{};
      const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                        static_cast<int>(text.size()), nullptr, 0);
      std::wstring result(n, L'\0');
      if (n)
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), result.data(), n);
      return result;
    };
    const auto a = wide(left), b = wide(right);
    return !a.empty() && !b.empty() &&
           CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(),
                                static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
  }
#endif
  return EqualAsciiAppName(left, right);
}

j::Object SanitizeAppProfiles(const j::Object& profiles, std::vector<std::string>* warnings) {
  j::Object output;
  for (const auto& [name, value] : profiles) {
    if (!value.IsObject()) {
      Warn(warnings, "invalid app profile object removed");
      continue;
    }
    j::Object profile;
    for (const auto& [key, field] : value.AsObject()) {
      if (key == "candidateTagBoosts" && field.IsObject()) {
        j::Object boosts;
        for (const auto& [tag, boost] : field.AsObject()) {
          if (!boost.IsNumber() || !std::isfinite(boost.AsNumber())) {
            Warn(warnings, "invalid app profile tag boost removed");
            continue;
          }
          // Preserve unknown names for future tag consumers; the scorer only
          // applies recognized tags. Bound every numeric value at ingestion.
          const auto bounded = std::clamp(boost.AsNumber(), 1.0, 3.0);
          if (bounded != boost.AsNumber()) Warn(warnings, "app profile tag boost clamped");
          boosts.emplace(tag, j::Value(bounded));
        }
        profile.emplace(key, j::Value(std::move(boosts)));
      } else if (const auto valid = Validate(key, field))
        profile.emplace(key, *valid);
      else
        Warn(warnings, "unknown or invalid app profile field removed");
    }
    output.emplace(name, j::Value(std::move(profile)));
  }
  WarnCollisions(output, warnings);
  return output;
}

AppProfileResolver AppProfileResolver::FromSettings(const j::Value& settings,
                                                    std::vector<std::string>* warnings) {
  AppProfileResolver resolver;
  resolver.globals_ = {{"profileName", ""},
                       {"predictionEnabled", true},
                       {"sentenceCompletion", false},
                       {"learningEnabled", true},
                       {"aiBackend", "none"},
                       {"promptPrefix", ""},
                       {"style", "auto"},
                       {"preferTechnicalTerms", false},
                       {"candidateTagBoosts", j::Object{}},
                       {"privacyMode", "inherit"},
                       {"bracketPairing", "auto"}};
  for (auto& [key, value] : resolver.globals_) {
    // The root bracketPairing boolean is a separate master switch. Only
    // profiles may carry the enum, even when a hand-edited root is malformed.
    if (key == "bracketPairing") continue;
    if (const auto* field = settings.Find(key)) {
      if (const auto valid = Validate(key, *field)) value = *valid;
    }
  }
  // "auto" is profile-only. Global aiBackend is always a concrete backend.
  if (resolver.globals_.at("aiBackend").AsString() == "auto")
    resolver.globals_["aiBackend"] = "none";
  if (const auto* profiles = settings.FindObject("profilesByApp"))
    resolver.profiles_ = SanitizeAppProfiles(*profiles, warnings);
  else if (settings.Find("profilesByApp"))
    Warn(warnings, "invalid profilesByApp removed");
  if (const auto* legacy = settings.FindObject("promptPrefixByApp")) {
    for (const auto& [name, value] : *legacy)
      if (value.IsString()) resolver.legacy_.emplace(name, value);
    WarnCollisions(resolver.legacy_, warnings);
  }
  return resolver;
}

j::Object AppProfileResolver::ResolveImpl(const ForegroundApp& app, AppNameEqual equal,
                                          std::optional<std::string_view> field) const {
  j::Object result;
  const auto overlay = [&](const j::Object& values) {
    for (const auto& [key, value] : values) {
      if (field && key != *field) continue;
      if (key == "privacyMode" && value.IsString() && value.AsString() == "inherit") continue;
      result[key] = value;
    }
  };
  overlay(globals_);
  const auto apply = [&](const j::Value* value) {
    if (value && value->IsObject()) overlay(value->AsObject());
  };
  const auto fallback = profiles_.find("default");
  if (fallback != profiles_.end()) apply(&fallback->second);
  if (app.resolved && !app.process_name.empty()) {
    const auto window = profiles_.find(app.window_class);
    if (!app.window_class.empty() && window != profiles_.end()) apply(&window->second);
    if (!field || *field == "promptPrefix") {
      if (const auto* prefix = ProcessMatch(legacy_, app.process_name, equal))
        result["promptPrefix"] = *prefix;
    }
    apply(ProcessMatch(profiles_, app.process_name, equal));
  }
  if (const auto it = result.find("aiBackend");
      it != result.end() && it->second.IsString() && it->second.AsString() == "auto")
    it->second = globals_.at("aiBackend");
  if ((!field || *field == "privacyMode") && !result.contains("privacyMode"))
    result["privacyMode"] = "inherit";
  return result;
}
j::Object AppProfileResolver::Resolve(const ForegroundApp& app, AppNameEqual equal) const {
  return ResolveImpl(app, equal, {});
}
std::optional<j::Value> AppProfileResolver::ResolveField(std::string_view field,
                                                         const ForegroundApp& app,
                                                         AppNameEqual equal) const {
  auto result = ResolveImpl(app, equal, field);
  const auto found = result.find(std::string(field));
  return found == result.end() ? std::nullopt : std::optional<j::Value>(std::move(found->second));
}
}  // namespace azookey::core
