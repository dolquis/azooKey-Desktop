#include "azookey/host/BackendSelector.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
// The DXCore GUIDs are DEFINE_GUID declarations; define them here instead of
// linking dxcore.lib, because dxcore.dll is loaded at run time. The order
// matters, so include sorting is off for these two.
// clang-format off
#include <initguid.h>
#include <dxcore_interface.h>
// clang-format on
#include <wrl/client.h>
#endif

namespace azookey::host {
namespace {

#ifdef _WIN32
class DxcoreAdapterEnumerator final : public IAdapterEnumerator {
 public:
  std::optional<std::vector<GraphicsAdapterInfo>> Enumerate() override {
    // System32 only, so a dxcore.dll beside the executable cannot be planted.
    HMODULE module = LoadLibraryExW(L"dxcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) return std::nullopt;
    auto result = EnumerateWith(module);
    FreeLibrary(module);
    return result;
  }

 private:
  using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);

  static std::optional<std::vector<GraphicsAdapterInfo>> EnumerateWith(HMODULE module) {
    const auto create = reinterpret_cast<CreateFactoryFn>(
        reinterpret_cast<void*>(GetProcAddress(module, "DXCoreCreateAdapterFactory")));
    if (!create) return std::nullopt;
    Microsoft::WRL::ComPtr<IDXCoreAdapterFactory> factory;
    if (FAILED(create(IID_PPV_ARGS(&factory)))) return std::nullopt;

    std::vector<GraphicsAdapterInfo> adapters;
    if (!AppendAdapters(*factory.Get(), DXCORE_ADAPTER_ATTRIBUTE_D3D12_GRAPHICS, true, adapters)) {
      return std::nullopt;
    }
    // NPUs expose generic ML without graphics; GPUs were listed above.
    (void)AppendAdapters(*factory.Get(), DXCORE_ADAPTER_ATTRIBUTE_D3D12_GENERIC_ML, false,
                         adapters);
    return adapters;
  }

  static bool AppendAdapters(IDXCoreAdapterFactory& factory, const GUID& attribute,
                             bool graphics_list, std::vector<GraphicsAdapterInfo>& adapters) {
    Microsoft::WRL::ComPtr<IDXCoreAdapterList> list;
    if (FAILED(factory.CreateAdapterList(1, &attribute, IID_PPV_ARGS(&list)))) return false;
    const uint32_t count = list->GetAdapterCount();
    for (uint32_t i = 0; i < count; ++i) {
      Microsoft::WRL::ComPtr<IDXCoreAdapter> adapter;
      if (FAILED(list->GetAdapter(i, IID_PPV_ARGS(&adapter))) || !adapter->IsValid()) continue;
      GraphicsAdapterInfo info;
      info.supports_graphics =
          adapter->IsAttributeSupported(DXCORE_ADAPTER_ATTRIBUTE_D3D12_GRAPHICS);
      if (!graphics_list && info.supports_graphics) continue;
      info.supports_generic_ml =
          adapter->IsAttributeSupported(DXCORE_ADAPTER_ATTRIBUTE_D3D12_GENERIC_ML);
      bool is_hardware = false;
      if (adapter->IsPropertySupported(DXCoreAdapterProperty::IsHardware) &&
          SUCCEEDED(adapter->GetProperty(DXCoreAdapterProperty::IsHardware, &is_hardware))) {
        info.is_hardware = is_hardware;
      }
      adapters.push_back(info);
    }
    return true;
  }
};

std::optional<std::filesystem::path> ExecutableDirectory() {
  std::wstring buffer(MAX_PATH, L'\0');
  while (true) {
    const DWORD length =
        GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0) return std::nullopt;
    if (length < buffer.size()) {
      buffer.resize(length);
      return std::filesystem::path(buffer).parent_path();
    }
    if (buffer.size() >= 32768) return std::nullopt;
    buffer.resize(buffer.size() * 2);
  }
}

std::optional<std::filesystem::path> System32Directory() {
  wchar_t buffer[MAX_PATH]{};
  const UINT length = GetSystemDirectoryW(buffer, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) return std::nullopt;
  return std::filesystem::path(std::wstring(buffer, length));
}

bool IsRegularFile(const std::filesystem::path& path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

class SystemRuntimeLibraryProbe final : public IRuntimeLibraryProbe {
 public:
  SystemRuntimeLibraryProbe() {
    if (auto system32 = System32Directory()) directories_.push_back(std::move(*system32));
    if (auto exe_dir = ExecutableDirectory()) directories_.push_back(std::move(*exe_dir));
  }

  bool Exists(std::string_view dll_name) override {
    const std::filesystem::path name(std::wstring(dll_name.begin(), dll_name.end()));
    // A Vulkan build imports vulkan-1.dll, so the loader it started with may
    // sit on PATH rather than in either directory below.
    if (GetModuleHandleW(name.c_str())) return true;
    for (const auto& directory : directories_) {
      if (IsRegularFile(directory / name)) return true;
    }
    return false;
  }

 private:
  std::vector<std::filesystem::path> directories_;
};

class SystemOsVersionProbe final : public IOsVersionProbe {
 public:
  std::optional<uint32_t> BuildNumber() override {
    // GetVersionExW reports the manifest's view of the OS, not the real build.
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return std::nullopt;
    const auto rtl_get_version = reinterpret_cast<RtlGetVersionFn>(
        reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")));
    if (!rtl_get_version) return std::nullopt;
    RTL_OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    if (rtl_get_version(&info) != 0) return std::nullopt;
    return static_cast<uint32_t>(info.dwBuildNumber);
  }
};
#else
class DxcoreAdapterEnumerator final : public IAdapterEnumerator {
 public:
  std::optional<std::vector<GraphicsAdapterInfo>> Enumerate() override { return std::nullopt; }
};

class SystemRuntimeLibraryProbe final : public IRuntimeLibraryProbe {
 public:
  bool Exists(std::string_view) override { return false; }
};

class SystemOsVersionProbe final : public IOsVersionProbe {
 public:
  std::optional<uint32_t> BuildNumber() override { return std::nullopt; }
};
#endif

}  // namespace

CompiledBackends CompiledBackendsForThisBuild() {
  CompiledBackends compiled;
#if AZOOKEY_WITH_LLAMA_CPP
  // AZOOKEY_BACKEND=vulkan is the only configuration that links ggml-vulkan.
  compiled.vulkan = std::string_view(AZOOKEY_BACKEND_DEFAULT) == "vulkan";
#endif
  return compiled;
}

BackendSelection SelectAutoBackend(const BackendSystemFacts& facts,
                                   const CompiledBackends& compiled,
                                   const BackendSelectionPolicy& policy) {
  BackendSelection selection;
  selection.winml_os_eligible = facts.os_build && *facts.os_build >= kWinMlMinimumOsBuild;
  if (facts.adapters) {
    for (const auto& adapter : *facts.adapters) {
      if (!adapter.is_hardware) continue;
      if (adapter.supports_graphics) {
        ++selection.hardware_gpu_count;
      } else if (adapter.supports_generic_ml) {
        ++selection.hardware_ml_only_count;
      }
    }
  }

  const auto cpu = [&selection](BackendSelectionReason reason) {
    selection.backend = BackendKind::Cpu;
    selection.reason = reason;
    return selection;
  };
  if (policy.safe_mode) return cpu(BackendSelectionReason::SafeMode);
  if (!compiled.vulkan) return cpu(BackendSelectionReason::VulkanNotCompiled);
  if (policy.require_winml_os_build && !selection.winml_os_eligible) {
    return cpu(BackendSelectionReason::OsBuildBelowMinimum);
  }
  // Spec 4.5: on battery, auto avoids every GPU path.
  if (facts.on_battery) return cpu(BackendSelectionReason::OnBattery);
  if (!facts.adapters) return cpu(BackendSelectionReason::AdapterEnumerationFailed);
  if (selection.hardware_gpu_count == 0) return cpu(BackendSelectionReason::NoHardwareGpu);
  if (!facts.vulkan_loader_present) return cpu(BackendSelectionReason::VulkanLoaderMissing);
  selection.backend = BackendKind::Vulkan;
  selection.reason = BackendSelectionReason::VulkanCandidate;
  return selection;
}

std::optional<BackendSelection> ReselectAutoBackendForPower(
    BackendSystemFacts startup_facts, bool on_battery, const CompiledBackends& compiled,
    const BackendSelectionPolicy& policy, bool auto_requested, BackendKind current) {
  if (!auto_requested || policy.safe_mode) return std::nullopt;
  startup_facts.on_battery = on_battery;
  const auto selection = SelectAutoBackend(startup_facts, compiled, policy);
  if (selection.backend == current) return std::nullopt;
  return selection;
}

BackendSystemFacts CollectBackendSystemFacts(IAdapterEnumerator& adapters,
                                             IRuntimeLibraryProbe& libraries, IOsVersionProbe& os,
                                             bool on_battery) noexcept {
  BackendSystemFacts facts;
  facts.on_battery = on_battery;
  try {
    facts.adapters = adapters.Enumerate();
  } catch (...) {
    facts.adapters.reset();
  }
  try {
    facts.vulkan_loader_present = libraries.Exists("vulkan-1.dll");
  } catch (...) {
    facts.vulkan_loader_present = false;
  }
  try {
    facts.cuda_runtime_present =
        libraries.Exists("cudart64_12.dll") && libraries.Exists("cublas64_12.dll");
  } catch (...) {
    facts.cuda_runtime_present = false;
  }
  try {
    facts.os_build = os.BuildNumber();
  } catch (...) {
    facts.os_build.reset();
  }
  return facts;
}

bool OnBatteryForBackendSelection(PowerSource source, std::string_view power_profile) {
  if (power_profile == "performance") return false;
  if (power_profile == "battery_saver") return true;
  return source == PowerSource::Battery;
}

std::unique_ptr<IAdapterEnumerator> MakeSystemAdapterEnumerator() {
  return std::make_unique<DxcoreAdapterEnumerator>();
}

std::unique_ptr<IRuntimeLibraryProbe> MakeSystemRuntimeLibraryProbe() {
  return std::make_unique<SystemRuntimeLibraryProbe>();
}

std::unique_ptr<IOsVersionProbe> MakeSystemOsVersionProbe() {
  return std::make_unique<SystemOsVersionProbe>();
}

}  // namespace azookey::host
