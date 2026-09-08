#pragma once

#include "azookey/ipc/Json.h"

namespace azookey::core {
// Permission intersection: request-side detection can only reduce host policy.
struct AiPrivacy {
  bool ai{false};
  bool external{false};
};
inline AiPrivacy ParseAiPrivacy(const ipc::json::Value& settings) {
  if (!settings.IsObject()) return {};
  const auto* privacy = settings.Find("privacy");
  if (!privacy) return {true, true};
  if (!privacy->IsObject()) return {};
  if (privacy->Find("mode") && !privacy->GetString("mode")) return {};
  const auto mode = privacy->GetString("mode").value_or("normal");
  if (mode == "normal") return {true, true};
  if (mode == "private" || mode == "offline") return {true, false};
  if (mode == "custom") {
    const auto* custom = privacy->Find("custom");
    if (!custom) return {true, false};
    if (!custom->IsObject()) return {};
    if ((custom->Find("aiCandidate") && !custom->GetBool("aiCandidate")) ||
        (custom->Find("externalAi") && !custom->GetBool("externalAi")))
      return {};
    const bool ai = custom->GetBool("aiCandidate").value_or(true);
    return {ai, ai && custom->GetBool("externalAi").value_or(false)};
  }
  return {};
}
}  // namespace azookey::core
