#pragma once

#include "azookey/ipc/Json.h"

namespace azookey::core {

// Event-local permissions. An absent context must never permit body logging.
struct PrivacyPolicy {
  bool secure{true};
  bool detailed_logging_allowed{false};
};

inline PrivacyPolicy ParsePrivacyPolicy(const ipc::json::Value& settings) {
  if (!settings.IsObject()) return {};
  const auto* privacy = settings.Find("privacy");
  if (!privacy) return {false, false};
  if (!privacy->IsObject()) return {};
  if (privacy->Find("mode") && !privacy->GetString("mode")) return {};
  const auto mode = privacy->GetString("mode").value_or("normal");
  if (mode == "secure") return {};
  if (mode != "normal" && mode != "private" && mode != "offline" && mode != "custom") return {};
  const bool redact = privacy->GetBool("redactLogs").value_or(true);
  if (mode == "private" || redact) return {false, false};
  if (mode == "custom") {
    const auto* custom = privacy->Find("custom");
    return {false,
            custom && custom->IsObject() && custom->GetBool("detailedLogging").value_or(false)};
  }
  return {false, true};
}

}  // namespace azookey::core
