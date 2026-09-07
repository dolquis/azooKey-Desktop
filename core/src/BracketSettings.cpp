#include "azookey/core/BracketSettings.h"

#include <algorithm>

#include "azookey/ipc/Json.h"

namespace azookey::core {

BracketSettings ParseBracketSettings(std::string_view json) {
  BracketSettings settings;
  const auto root = ipc::json::Parse(json);
  if (!root || !root->IsObject()) return settings;
  settings.pairing.enabled = root->GetBool("bracketPairing").value_or(false);
  settings.pairing.symmetric_quote_pairing =
      root->GetBool("bracketSymmetricQuotePairing").value_or(false);
  settings.pairing.wrap_selection = root->GetBool("bracketWrapSelection").value_or(false);
  settings.pairing.skip_over_closing = root->GetBool("bracketSkipOverClosing").value_or(true);
  settings.pairing.backspace_deletes_pair =
      root->GetBool("bracketBackspaceDeletesPair").value_or(true);
  settings.pairing.enabled_in_alnum_mode =
      root->GetBool("bracketPairingInAlnumMode").value_or(true);
  if (root->GetString("bracketPairingTrigger") == "composition") {
    settings.trigger = BracketPairingTrigger::Composition;
  }
  const auto mode = root->GetString("inputMode");
  settings.pairs_path = root->GetString("bracketPairsPath").value_or("");
  if (mode == "alnum_half") settings.input_mode = BracketInputMode::AlnumHalf;
  if (mode == "alnum_full") settings.input_mode = BracketInputMode::AlnumFull;
  auto policy = std::make_shared<BracketAppPolicy>();
  policy->allowlist = root->GetString("bracketPairingAppPolicy") == "allowlist";
  if (const auto* apps = root->GetArray("bracketPairingApps")) {
    for (const auto& app : *apps) {
      if (app.IsString() && !app.AsString().empty()) policy->apps.push_back(app.AsString());
    }
  }
  settings.app_policy = std::move(policy);
  return settings;
}

bool EqualAsciiAppName(std::string_view left, std::string_view right) {
  const auto lower = [](unsigned char ch) {
    return ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch;
  };
  return left.size() == right.size() &&
         std::equal(left.begin(), left.end(), right.begin(),
                    [&](unsigned char a, unsigned char b) { return lower(a) == lower(b); });
}

bool BracketPairingEnabledForApp(const BracketSettings& settings, const ForegroundApp& app,
                                 AppNameEqual equal) {
  if (!settings.pairing.enabled || !app.resolved || app.process_name.empty()) return false;
  bool listed = false;
  if (settings.app_policy) {
    for (const auto& name : settings.app_policy->apps) {
      if (equal(name, app.process_name)) {
        listed = true;
        break;
      }
    }
    if (settings.app_policy->allowlist) return listed;
  }
  if (listed) return false;
  constexpr std::string_view seed[]{"Code.exe",       "devenv.exe",     "idea64.exe",
                                    "pycharm64.exe",  "webstorm64.exe", "phpstorm64.exe",
                                    "clion64.exe",    "rider64.exe",    "goland64.exe",
                                    "rubymine64.exe", "datagrip64.exe", "sublime_text.exe"};
  return std::none_of(std::begin(seed), std::end(seed),
                      [&](const auto name) { return equal(name, app.process_name); });
}

}  // namespace azookey::core
