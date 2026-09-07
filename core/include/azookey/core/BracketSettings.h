#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "azookey/core/BracketPairing.h"

namespace azookey::core {

enum class BracketPairingTrigger { Immediate, Composition };
enum class BracketInputMode { Hiragana, AlnumHalf, AlnumFull };

struct ForegroundApp {
  std::string process_name;
  std::string window_class;
  bool resolved{false};
};

struct BracketAppPolicy {
  bool allowlist{false};
  std::vector<std::string> apps;
};

using AppNameEqual = bool (*)(std::string_view, std::string_view);
bool EqualAsciiAppName(std::string_view left, std::string_view right);

struct BracketSettings {
  BracketPairingOptions pairing;
  BracketPairingTrigger trigger{BracketPairingTrigger::Immediate};
  BracketInputMode input_mode{BracketInputMode::Hiragana};
  std::string pairs_path;
  std::shared_ptr<const BracketTable> table;
  std::shared_ptr<const BracketAppPolicy> app_policy;

  const BracketTable& Table() const { return table ? *table : BuiltinBracketTable(); }
};

// Parse only the TIP-owned fields of the shared settings document. No file I/O,
// host connection, or mutation of the shared settings file is performed here.
BracketSettings ParseBracketSettings(std::string_view json);
bool BracketPairingEnabledForApp(const BracketSettings& settings, const ForegroundApp& app,
                                 AppNameEqual equal = EqualAsciiAppName);

}  // namespace azookey::core
