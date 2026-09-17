#pragma once

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "azookey/core/AppProfileResolver.h"
#include "azookey/ipc/Json.h"

namespace azookey::core {

// Bundled defaults for the M46 automatic secure switch. Updated only by a
// repository change shipped in a release: never fetched over the network and
// never driven by telemetry. Keep in sync with
// docs/privacy-and-secure-input-spec.md section 4.1.
inline constexpr std::array<std::string_view, 7> kDefaultSecureApps{
    "keepass.exe",  "keepassxc.exe",          "1password.exe", "bitwarden.exe",
    "lastpass.exe", "credentialuibroker.exe", "lsass.exe",
};

// User additions from privacy.secureApps. The bundled defaults are not repeated
// there, so an invalid document degrades to "defaults only" rather than to no
// secure detection at all. Names are normalized at this boundary so every
// consumer compares lowercase basenames.
inline std::vector<std::string> ParseSecureApps(const ipc::json::Value& settings) {
  std::vector<std::string> apps;
  if (!settings.IsObject()) return apps;
  const auto* privacy = settings.Find("privacy");
  if (!privacy || !privacy->IsObject()) return apps;
  const auto* list = privacy->Find("secureApps");
  if (!list || !list->IsArray()) return apps;
  for (const auto& item : list->AsArray()) {
    if (!item.IsString()) continue;
    auto name = item.AsString();
    if (name.empty()) continue;
    for (auto& ch : name)
      if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    apps.push_back(std::move(name));
  }
  return apps;
}

// Effective list is the union of the bundled defaults and the user additions
// (spec section 4.1); subtracting a bundled default is out of M46 scope.
inline bool IsSecureApp(std::string_view process_name, const std::vector<std::string>& user_apps,
                        AppNameEqual equal = EqualAppName) {
  if (process_name.empty()) return false;
  const auto matches = [&](std::string_view candidate) { return equal(candidate, process_name); };
  return std::any_of(kDefaultSecureApps.begin(), kDefaultSecureApps.end(), matches) ||
         std::any_of(user_apps.begin(), user_apps.end(),
                     [&](const std::string& app) { return matches(app); });
}

}  // namespace azookey::core
