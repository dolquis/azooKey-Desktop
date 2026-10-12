#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "azookey/host/InferenceEngine.h"
#include "azookey/host/SettingsStore.h"

// M24 BackendSelector: resolves the `auto` backend preference
// (docs/copilot-pc-backend-spec.md section 4.4). The probes only filter;
// the chosen backend is still tried by InferenceEngine::LoadModelWithResult,
// which falls back to CPU when the attempt fails.
namespace azookey::host {

// Windows 11 24H2. R2 (Windows ML) stays off below this build (spec 4.3).
inline constexpr uint32_t kWinMlMinimumOsBuild = 26100;

struct GraphicsAdapterInfo {
  // False for WARP and Microsoft Basic Render Driver.
  bool is_hardware{false};
  bool supports_graphics{false};
  bool supports_generic_ml{false};
};

class IAdapterEnumerator {
 public:
  virtual ~IAdapterEnumerator() = default;
  // nullopt when the adapters cannot be enumerated (no DXCore, a failed call).
  virtual std::optional<std::vector<GraphicsAdapterInfo>> Enumerate() = 0;
};

class IRuntimeLibraryProbe {
 public:
  virtual ~IRuntimeLibraryProbe() = default;
  // Presence only: the library is not loaded, so presence does not mean it works.
  virtual bool Exists(std::string_view dll_name) = 0;
};

class IOsVersionProbe {
 public:
  virtual ~IOsVersionProbe() = default;
  virtual std::optional<uint32_t> BuildNumber() = 0;
};

// Backends this executable can actually attempt.
struct CompiledBackends {
  bool cuda{false};
  bool vulkan{false};
};

CompiledBackends CompiledBackendsForThisBuild();

struct BackendSystemFacts {
  std::optional<std::vector<GraphicsAdapterInfo>> adapters;
  bool vulkan_loader_present{false};
  bool cuda_runtime_present{false};
  std::optional<uint32_t> os_build;
  bool on_battery{false};
};

struct BackendSelectionPolicy {
  // When set, an OS below kWinMlMinimumOsBuild keeps `auto` on CPU.
  bool require_winml_os_build{false};
};

enum class BackendSelectionReason {
  VulkanCandidate,
  VulkanNotCompiled,
  OsBuildBelowMinimum,
  OnBattery,
  AdapterEnumerationFailed,
  NoHardwareGpu,
  VulkanLoaderMissing,
};

constexpr const char* BackendSelectionReasonName(BackendSelectionReason reason) {
  switch (reason) {
    case BackendSelectionReason::VulkanCandidate:
      return "vulkan_candidate";
    case BackendSelectionReason::VulkanNotCompiled:
      return "vulkan_not_compiled";
    case BackendSelectionReason::OsBuildBelowMinimum:
      return "os_build_below_minimum";
    case BackendSelectionReason::OnBattery:
      return "on_battery";
    case BackendSelectionReason::AdapterEnumerationFailed:
      return "adapter_enumeration_failed";
    case BackendSelectionReason::NoHardwareGpu:
      return "no_hardware_gpu";
    case BackendSelectionReason::VulkanLoaderMissing:
      return "vulkan_loader_missing";
  }
  return "vulkan_not_compiled";
}

struct BackendSelection {
  BackendKind backend{BackendKind::Cpu};
  BackendSelectionReason reason{BackendSelectionReason::VulkanNotCompiled};
  size_t hardware_gpu_count{};
  size_t hardware_ml_only_count{};
  // The OS is new enough for R2. Recorded only while R2 is on hold.
  bool winml_os_eligible{false};
};

// Pure: the same facts always select the same backend. CUDA is never chosen
// here, because no build links ggml-cuda; an explicit request still reaches
// LoadModelWithResult and its CPU fallback.
BackendSelection SelectAutoBackend(const BackendSystemFacts& facts,
                                   const CompiledBackends& compiled,
                                   const BackendSelectionPolicy& policy = {});

// A probe that throws or fails leaves its fact unknown or false; never throws.
BackendSystemFacts CollectBackendSystemFacts(IAdapterEnumerator& adapters,
                                             IRuntimeLibraryProbe& libraries, IOsVersionProbe& os,
                                             bool on_battery) noexcept;

// powerProfile overrides the measured power source (spec 6.3).
bool OnBatteryForBackendSelection(PowerSource source, std::string_view power_profile);

// DXCore loaded at run time from System32; nullopt where it is absent.
std::unique_ptr<IAdapterEnumerator> MakeSystemAdapterEnumerator();
// Checks System32 and the executable directory only.
std::unique_ptr<IRuntimeLibraryProbe> MakeSystemRuntimeLibraryProbe();
std::unique_ptr<IOsVersionProbe> MakeSystemOsVersionProbe();

}  // namespace azookey::host
