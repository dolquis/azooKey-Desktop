#include "azookey/host/TypoCorrectionPolicy.h"

namespace azookey::host {

TypoCorrectionMode ParseTypoCorrectionMode(std::string_view mode) {
  if (mode == "off") return TypoCorrectionMode::Off;
  if (mode == "rank") return TypoCorrectionMode::Rank;
  if (mode == "aggressive" || mode == "auto_replace") return TypoCorrectionMode::Aggressive;
  return TypoCorrectionMode::Suggest;
}

}  // namespace azookey::host
