#pragma once

#include "azookey/ipc/Json.h"

namespace azookey::core {
// Permission intersection: request-side detection can only reduce host policy.
struct AiPrivacy {
  bool ai{false};
  bool external{false};
};
// External AI also needs privacy.externalAiConsent in every mode (ai-backend-spec
// section 14.2). A file without the key, an older one included, has no consent.
inline AiPrivacy ParseAiPrivacy(const ipc::json::Value& settings) {
  if (!settings.IsObject()) return {};
  const auto* privacy = settings.Find("privacy");
  if (!privacy) return {true, false};
  if (!privacy->IsObject()) return {};
  if ((privacy->Find("mode") && !privacy->GetString("mode")) ||
      (privacy->Find("externalAiConsent") && !privacy->GetBool("externalAiConsent")))
    return {};
  const bool consent = privacy->GetBool("externalAiConsent").value_or(false);
  const auto mode = privacy->GetString("mode").value_or("normal");
  if (mode == "normal") return {true, consent};
  if (mode == "private" || mode == "offline") return {true, false};
  if (mode == "custom") {
    const auto* custom = privacy->Find("custom");
    if (!custom) return {true, false};
    if (!custom->IsObject()) return {};
    if ((custom->Find("aiCandidate") && !custom->GetBool("aiCandidate")) ||
        (custom->Find("externalAi") && !custom->GetBool("externalAi")))
      return {};
    const bool ai = custom->GetBool("aiCandidate").value_or(true);
    return {ai, consent && ai && custom->GetBool("externalAi").value_or(false)};
  }
  return {};
}
}  // namespace azookey::core
