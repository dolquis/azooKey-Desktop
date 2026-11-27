#pragma once

#include <string_view>

namespace azookey::host {

// docs/typo-correction-learning-spec.md section 12.6: the typo correction modes.
enum class TypoCorrectionMode { Off, Suggest, Rank, Aggressive };

// Reads EngineConfig::typo_correction_mode. The v1 auto_replace is aggressive (section 12.6); any
// other value converts as suggest, as the v1 engine did.
TypoCorrectionMode ParseTypoCorrectionMode(std::string_view mode);

}  // namespace azookey::host
