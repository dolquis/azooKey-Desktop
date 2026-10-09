#include "StatusPaneModel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace azookey::settings {

NeologdStatusKind ClassifyNeologdStatus(
    const std::optional<azookey::ipc::NeologdLayerStatus>& layer, bool saved_enabled,
    bool current_enabled) {
  if (!layer) return NeologdStatusKind::Unknown;
  const std::string& state = layer->state;
  if (state == azookey::ipc::kNeologdLayerReady) return NeologdStatusKind::Ready;
  if (state == azookey::ipc::kNeologdLayerLoading) return NeologdStatusKind::Loading;
  if (state == azookey::ipc::kNeologdLayerMissingPack) return NeologdStatusKind::MissingPack;
  if (state == azookey::ipc::kNeologdLayerError) return NeologdStatusKind::Error;
  if (state == azookey::ipc::kNeologdLayerNotRequested) {
    if (current_enabled) {
      return saved_enabled ? NeologdStatusKind::FetchAfterRestart
                           : NeologdStatusKind::FetchAfterSaveAndRestart;
    }
    return NeologdStatusKind::NotEnabled;
  }
  return NeologdStatusKind::UnknownState;
}

std::string NeologdStatusResource(NeologdStatusKind kind) {
  switch (kind) {
    case NeologdStatusKind::Unknown:
      return "NeologdStatus_Unknown";
    case NeologdStatusKind::UnknownState:
      return "NeologdStatus_UnknownState";
    case NeologdStatusKind::NotEnabled:
      return "NeologdStatus_NotEnabled";
    case NeologdStatusKind::FetchAfterRestart:
      return "NeologdStatus_FetchAfterRestart";
    case NeologdStatusKind::FetchAfterSaveAndRestart:
      return "NeologdStatus_FetchAfterSaveAndRestart";
    case NeologdStatusKind::Loading:
      return "NeologdStatus_Loading";
    case NeologdStatusKind::Ready:
      return "NeologdStatus_Ready";
    case NeologdStatusKind::MissingPack:
      return "NeologdStatus_MissingPack";
    case NeologdStatusKind::Error:
      break;
  }
  return "NeologdStatus_Error";
}

std::string FormatRatioPercent(double ratio) {
  if (!std::isfinite(ratio)) return "-";
  const double clamped = std::clamp(ratio, 0.0, 1.0);
  char buffer[32]{};
  std::snprintf(buffer, sizeof(buffer), "%.1f%%", clamped * 100.0);
  return buffer;
}

std::string FormatPersonaTime(uint64_t epoch_seconds) {
  if (epoch_seconds == 0) return {};
  const auto time = static_cast<std::time_t>(epoch_seconds);
  std::tm local{};
  if (localtime_s(&local, &time) != 0) return {};
  char buffer[32]{};
  if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &local) == 0) return {};
  return buffer;
}

bool PersonaHasData(const azookey::ipc::QueryPersonaResponse& persona) {
  return persona.sample_count > 0;
}

}  // namespace azookey::settings
