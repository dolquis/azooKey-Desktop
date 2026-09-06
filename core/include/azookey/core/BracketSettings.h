#pragma once

#include <string_view>

#include "azookey/core/BracketPairing.h"

namespace azookey::core {

enum class BracketPairingTrigger { Immediate, Composition };
enum class BracketInputMode { Hiragana, AlnumHalf, AlnumFull };

struct BracketSettings {
  BracketPairingOptions pairing;
  BracketPairingTrigger trigger{BracketPairingTrigger::Immediate};
  BracketInputMode input_mode{BracketInputMode::Hiragana};
};

// Parse only the TIP-owned fields of the shared settings document. No file I/O,
// host connection, or mutation of the shared settings file is performed here.
BracketSettings ParseBracketSettings(std::string_view json);

}  // namespace azookey::core
