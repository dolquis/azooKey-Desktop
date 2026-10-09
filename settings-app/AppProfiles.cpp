#include "AppProfiles.h"

#include <algorithm>
#include <cmath>

#include "azookey/core/AppProfileResolver.h"

namespace azookey::settings {
namespace {

namespace j = azookey::ipc::json;

template <typename Allowed>
bool IsOneOf(const std::string& value, const Allowed& allowed) {
  return std::find(allowed.begin(), allowed.end(), value) != allowed.end();
}

void SetString(const j::Object& object, const char* key, std::optional<std::string>* out) {
  if (const auto it = object.find(key); it != object.end() && it->second.IsString()) {
    *out = it->second.AsString();
  }
}

void SetBool(const j::Object& object, const char* key, std::optional<bool>* out) {
  if (const auto it = object.find(key); it != object.end() && it->second.IsBool()) {
    *out = it->second.AsBool();
  }
}

// An enum field read from disk keeps only a value the editor can show.
template <typename Allowed>
void SetEnum(const j::Object& object, const char* key, const Allowed& allowed,
             std::optional<std::string>* out) {
  std::optional<std::string> value;
  SetString(object, key, &value);
  if (value && IsOneOf(*value, allowed)) *out = std::move(value);
}

}  // namespace

bool SameProfileKey(std::string_view left, std::string_view right) {
  return azookey::core::EqualAppName(left, right);
}

AppProfiles ParseAppProfiles(const j::Object& object) {
  AppProfiles profiles;
  for (const auto& [key, value] : object) {
    if (!value.IsObject()) continue;
    const auto& fields = value.AsObject();
    AppProfile profile;
    SetString(fields, "profileName", &profile.profile_name);
    SetBool(fields, "predictionEnabled", &profile.prediction_enabled);
    SetBool(fields, "sentenceCompletion", &profile.sentence_completion);
    SetBool(fields, "learningEnabled", &profile.learning_enabled);
    SetEnum(fields, "aiBackend", kProfileAiBackends, &profile.ai_backend);
    SetString(fields, "promptPrefix", &profile.prompt_prefix);
    SetEnum(fields, "style", kProfileStyles, &profile.style);
    SetBool(fields, "preferTechnicalTerms", &profile.prefer_technical_terms);
    SetEnum(fields, "privacyMode", kProfilePrivacyModes, &profile.privacy_mode);
    SetEnum(fields, "bracketPairing", kProfileBracketPairings, &profile.bracket_pairing);
    if (const auto it = fields.find("candidateTagBoosts");
        it != fields.end() && it->second.IsObject()) {
      for (const auto& [tag, boost] : it->second.AsObject()) {
        if (boost.IsNumber() && std::isfinite(boost.AsNumber())) {
          profile.candidate_tag_boosts.emplace(
              tag, std::clamp(boost.AsNumber(), kMinTagBoost, kMaxTagBoost));
        }
      }
    }
    profiles.emplace(key, std::move(profile));
  }
  return profiles;
}

j::Object AppProfilesToJson(const AppProfiles& profiles) {
  j::Object output;
  for (const auto& [key, profile] : profiles) {
    j::Object fields;
    if (profile.profile_name) fields["profileName"] = j::Value(*profile.profile_name);
    if (profile.prediction_enabled)
      fields["predictionEnabled"] = j::Value(*profile.prediction_enabled);
    if (profile.sentence_completion) {
      fields["sentenceCompletion"] = j::Value(*profile.sentence_completion);
    }
    if (profile.learning_enabled) fields["learningEnabled"] = j::Value(*profile.learning_enabled);
    if (profile.ai_backend) fields["aiBackend"] = j::Value(*profile.ai_backend);
    if (profile.prompt_prefix) fields["promptPrefix"] = j::Value(*profile.prompt_prefix);
    if (profile.style) fields["style"] = j::Value(*profile.style);
    if (profile.prefer_technical_terms) {
      fields["preferTechnicalTerms"] = j::Value(*profile.prefer_technical_terms);
    }
    if (!profile.candidate_tag_boosts.empty()) {
      j::Object boosts;
      for (const auto& [tag, boost] : profile.candidate_tag_boosts) boosts[tag] = j::Value(boost);
      fields["candidateTagBoosts"] = j::Value(std::move(boosts));
    }
    if (profile.privacy_mode) fields["privacyMode"] = j::Value(*profile.privacy_mode);
    if (profile.bracket_pairing) fields["bracketPairing"] = j::Value(*profile.bracket_pairing);
    output.emplace(key, j::Value(std::move(fields)));
  }
  return output;
}

std::optional<std::string> ValidateAppProfiles(const AppProfiles& profiles) {
  for (auto it = profiles.begin(); it != profiles.end(); ++it) {
    const auto& [key, profile] = *it;
    if (key.empty()) return "a profile has no name";
    for (auto other = std::next(it); other != profiles.end(); ++other) {
      if (SameProfileKey(key, other->first)) return "two profiles name the same app";
    }
    if (profile.ai_backend && !IsOneOf(*profile.ai_backend, kProfileAiBackends)) {
      return "aiBackend is not an allowed value";
    }
    if (profile.style && !IsOneOf(*profile.style, kProfileStyles)) {
      return "style is not an allowed value";
    }
    if (profile.privacy_mode && !IsOneOf(*profile.privacy_mode, kProfilePrivacyModes)) {
      return "privacyMode is not an allowed value";
    }
    if (profile.bracket_pairing && !IsOneOf(*profile.bracket_pairing, kProfileBracketPairings)) {
      return "bracketPairing is not an allowed value";
    }
    for (const auto& [tag, boost] : profile.candidate_tag_boosts) {
      if (tag.empty()) return "a candidate tag has no name";
      if (!std::isfinite(boost) || boost < kMinTagBoost || boost > kMaxTagBoost) {
        return "a candidateTagBoosts value is outside 1.0 to 3.0";
      }
    }
  }
  return std::nullopt;
}

}  // namespace azookey::settings
