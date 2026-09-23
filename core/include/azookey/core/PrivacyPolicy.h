#pragma once

#include "azookey/ipc/Json.h"

namespace azookey::core {

// Event-local permissions. An absent context permits neither body logging nor learning.
struct PrivacyPolicy {
  bool secure{true};
  bool detailed_logging_allowed{false};
  bool learning_allowed{false};
};

inline PrivacyPolicy ParsePrivacyPolicy(const ipc::json::Value& settings) {
  if (!settings.IsObject()) return {};
  const auto* privacy = settings.Find("privacy");
  if (!privacy) return {false, false, true};
  if (!privacy->IsObject()) return {};
  if (privacy->Find("mode") && !privacy->GetString("mode")) return {};
  const auto mode = privacy->GetString("mode").value_or("normal");
  if (mode == "secure") return {};
  if (mode != "normal" && mode != "private" && mode != "offline" && mode != "custom") return {};
  const bool redact = privacy->GetBool("redactLogs").value_or(true);
  if (mode == "private") return {false, false, false};
  if (mode == "custom") {
    const auto* custom = privacy->Find("custom");
    return {false,
            !redact && custom && custom->IsObject() &&
                custom->GetBool("detailedLogging").value_or(false),
            custom && custom->IsObject() && custom->GetBool("learning").value_or(false)};
  }
  return {false, !redact, true};
}

}  // namespace azookey::core
