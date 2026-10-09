#pragma once

#include <array>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "azookey/ipc/Json.h"

namespace azookey::settings {

// One entry of profilesByApp (app-profile-spec section 4). A field that is not set is left out
// of settings.json, so the profile inherits it from the next layer (section 5); the editor never
// fills in a schema default. `prompt_prefix` is set even when empty: an explicit "" clears the
// legacy promptPrefixByApp value (section 6).
struct AppProfile {
  std::optional<std::string> profile_name;
  std::optional<bool> prediction_enabled;
  std::optional<bool> sentence_completion;
  std::optional<bool> learning_enabled;
  std::optional<std::string> ai_backend;
  std::optional<std::string> prompt_prefix;
  std::optional<std::string> style;
  std::optional<bool> prefer_technical_terms;
  // Candidate tag name -> multiplier in [kMinTagBoost, kMaxTagBoost]; empty is left out.
  std::map<std::string, double> candidate_tag_boosts;
  std::optional<std::string> privacy_mode;
  std::optional<std::string> bracket_pairing;

  bool operator==(const AppProfile&) const = default;
};

// Keyed by the name the user gave: an executable name such as "Code.exe", a window class, or
// "default". The case is kept as typed; keys that differ only in case name the same app.
using AppProfiles = std::map<std::string, AppProfile>;

inline constexpr double kMinTagBoost = 1.0;
inline constexpr double kMaxTagBoost = 3.0;
inline constexpr std::string_view kDefaultProfileKey = "default";

// Allowed values of the enum fields (schema, app-profile-spec section 4.1).
inline constexpr std::array<std::string_view, 4> kProfileAiBackends{"auto", "local-zenzai",
                                                                    "openai", "none"};
inline constexpr std::array<std::string_view, 4> kProfileStyles{"auto", "polite", "casual",
                                                                "technical"};
inline constexpr std::array<std::string_view, 4> kProfilePrivacyModes{"inherit", "normal",
                                                                      "private", "secure"};
inline constexpr std::array<std::string_view, 3> kProfileBracketPairings{"auto", "on", "off"};

// Reads the profilesByApp object of a document that was already sanitized; a field of the wrong
// type is skipped.
AppProfiles ParseAppProfiles(const azookey::ipc::json::Object& object);

// The JSON object to write for `profiles`, set fields only.
azookey::ipc::json::Object AppProfilesToJson(const AppProfiles& profiles);

// Why `profiles` cannot be saved, or nothing when they can: an empty or duplicate (ignoring case)
// key, an enum value outside its list, an empty tag name, or a multiplier outside [1.0, 3.0].
std::optional<std::string> ValidateAppProfiles(const AppProfiles& profiles);

// Whether two keys name the same app (case-insensitive, as the resolver compares them).
bool SameProfileKey(std::string_view left, std::string_view right);

}  // namespace azookey::settings
