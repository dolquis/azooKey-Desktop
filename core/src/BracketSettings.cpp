#include "azookey/core/BracketSettings.h"

#include "azookey/ipc/Json.h"

namespace azookey::core {

BracketSettings ParseBracketSettings(std::string_view json) {
  BracketSettings settings;
  const auto root = ipc::json::Parse(json);
  if (!root || !root->IsObject()) return settings;
  settings.pairing.enabled = root->GetBool("bracketPairing").value_or(false);
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
  return settings;
}

}  // namespace azookey::core
