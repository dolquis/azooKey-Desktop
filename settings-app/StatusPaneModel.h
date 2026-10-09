#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "azookey/ipc/Payloads.h"

namespace azookey::settings {

// What the "辞書" pane says about the neologd_lexicon layer (auto-word-registration-spec
// section 15.14). The Host reads the pack only at startup, so a switch turned on after the Host
// started stays `not_requested` until the next start; that is not a failure.
enum class NeologdStatusKind {
  // The Host did not report a state (it predates the field).
  Unknown,
  // Switched off, and the Host is not using the layer.
  NotEnabled,
  // Switched on and saved; the Host fetches the pack when it next starts.
  FetchAfterRestart,
  // Switched on but not saved yet.
  FetchAfterSaveAndRestart,
  Loading,
  Ready,
  MissingPack,
  Error,
};

// `saved_enabled` is the stored dictionary.neologdEnabled; `current_enabled` is the switch as it
// stands in the window.
NeologdStatusKind ClassifyNeologdStatus(
    const std::optional<azookey::ipc::NeologdLayerStatus>& layer, bool saved_enabled,
    bool current_enabled);

// "NeologdStatus_<kind>" string name.
std::string NeologdStatusResource(NeologdStatusKind kind);

// 0..1 as a percentage with one decimal: "45.2%". Values outside the range are clamped.
std::string FormatRatioPercent(double ratio);

// Local time as "YYYY-MM-DD HH:MM"; empty for 0.
std::string FormatPersonaTime(uint64_t epoch_seconds);

// The Persona pane shows "データ不足" instead of the ratios when nothing was counted.
bool PersonaHasData(const azookey::ipc::QueryPersonaResponse& persona);

}  // namespace azookey::settings
