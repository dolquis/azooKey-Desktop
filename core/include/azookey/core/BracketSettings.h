#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "azookey/core/AppProfileResolver.h"
#include "azookey/core/BracketPairing.h"
#include "azookey/core/SecureApps.h"

namespace azookey::core {

enum class BracketPairingTrigger { Immediate, Composition };
enum class BracketInputMode { Hiragana, AlnumHalf, AlnumFull };

struct BracketAppPolicy {
  bool allowlist{false};
  std::vector<std::string> apps;
};

struct BracketSettings {
  BracketPairingOptions pairing;
  BracketPairingTrigger trigger{BracketPairingTrigger::Immediate};
  BracketInputMode input_mode{BracketInputMode::Hiragana};
  std::string pairs_path;
  std::shared_ptr<const BracketTable> table;
  std::shared_ptr<const BracketAppPolicy> app_policy;
  std::shared_ptr<const AppProfileResolver> profiles;
  // M46 privacy.secureApps user additions, lowercase-normalized. Matched
  // against the bundled kDefaultSecureApps union by core::IsSecureApp.
  std::shared_ptr<const std::vector<std::string>> secure_apps;

  const BracketTable& Table() const { return table ? *table : BuiltinBracketTable(); }
};

// Parse only the TIP-owned fields of the shared settings document. No file I/O,
// host connection, or mutation of the shared settings file is performed here.
BracketSettings ParseBracketSettings(std::string_view json);
bool BracketPairingEnabledForApp(const BracketSettings& settings, const ForegroundApp& app,
                                 AppNameEqual equal = EqualAppName);

}  // namespace azookey::core
