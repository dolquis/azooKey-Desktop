#pragma once

#include <string_view>

#include "azookey/host/SettingsStore.h"

// M25 power-saving lanes (docs/copilot-pc-backend-spec.md section 6.2).
namespace azookey::host {

// Fast: one Zenzai pass. Heavy: the extra passes on top of it (the NLL rerank).
enum class InferenceLane { Fast, Heavy };

// The lane heavy inference may run in. Battery power, or a powerProfile of
// battery_saver, holds it to Fast; an unknown power source counts as AC.
InferenceLane ResolveInferenceLane(PowerSource source, std::string_view power_profile);

std::string_view InferenceLaneName(InferenceLane lane);
std::string_view PowerSourceName(PowerSource source);

}  // namespace azookey::host
