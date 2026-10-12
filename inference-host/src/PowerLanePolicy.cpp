#include "azookey/host/PowerLanePolicy.h"

#include "azookey/host/BackendSelector.h"

namespace azookey::host {

InferenceLane ResolveInferenceLane(PowerSource source, std::string_view power_profile) {
  return OnBatteryForBackendSelection(source, power_profile) ? InferenceLane::Fast
                                                             : InferenceLane::Heavy;
}

std::string_view InferenceLaneName(InferenceLane lane) {
  return lane == InferenceLane::Fast ? "fast" : "heavy";
}

std::string_view PowerSourceName(PowerSource source) {
  switch (source) {
    case PowerSource::Ac:
      return "ac";
    case PowerSource::Battery:
      return "battery";
    case PowerSource::Unknown:
      break;
  }
  return "unknown";
}

}  // namespace azookey::host
