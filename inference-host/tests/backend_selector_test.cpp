#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "azookey/host/BackendSelector.h"

namespace {

using azookey::host::BackendKind;
using azookey::host::BackendSelectionReason;
using azookey::host::BackendSystemFacts;
using azookey::host::CompiledBackends;
using azookey::host::GraphicsAdapterInfo;
using azookey::host::SelectAutoBackend;

GraphicsAdapterInfo HardwareGpu() { return {true, true, true}; }
GraphicsAdapterInfo SoftwareGpu() { return {false, true, false}; }
GraphicsAdapterInfo HardwareNpu() { return {true, false, true}; }

CompiledBackends VulkanBuild() { return {false, true}; }

// Every filter passes: a hardware GPU, the loader, AC power, a new OS.
BackendSystemFacts ReadyFacts() {
  BackendSystemFacts facts;
  facts.adapters = std::vector<GraphicsAdapterInfo>{HardwareGpu()};
  facts.vulkan_loader_present = true;
  facts.os_build = 26100;
  return facts;
}

TEST(BackendSelectorTest, VulkanBuildWithHardwareGpuAndLoaderSelectsVulkan) {
  const auto selection = SelectAutoBackend(ReadyFacts(), VulkanBuild());
  EXPECT_EQ(selection.backend, BackendKind::Vulkan);
  EXPECT_EQ(selection.reason, BackendSelectionReason::VulkanCandidate);
  EXPECT_EQ(selection.hardware_gpu_count, 1U);
  EXPECT_TRUE(selection.winml_os_eligible);
}

azookey::host::PowerBackendReloadState AutoWithModel() {
  azookey::host::PowerBackendReloadState state;
  state.auto_requested = true;
  state.model_active = true;
  return state;
}

TEST(BackendSelectorTest, PowerChangeMovesAutoBetweenVulkanAndCpu) {
  using azookey::host::PlanAutoBackendForPower;
  const auto to_battery = PlanAutoBackendForPower(ReadyFacts(), true, VulkanBuild(),
                                                  BackendKind::Vulkan, AutoWithModel());
  EXPECT_EQ(to_battery.selection.backend, BackendKind::Cpu);
  EXPECT_EQ(to_battery.selection.reason, BackendSelectionReason::OnBattery);
  EXPECT_TRUE(to_battery.changed);
  EXPECT_TRUE(to_battery.reload);

  const auto to_ac = PlanAutoBackendForPower(ReadyFacts(), false, VulkanBuild(), BackendKind::Cpu,
                                             AutoWithModel());
  EXPECT_EQ(to_ac.selection.backend, BackendKind::Vulkan);
  EXPECT_TRUE(to_ac.reload);
}

TEST(BackendSelectorTest, PowerChangeReloadsOnlyWhenTheAutoBackendChanges) {
  using azookey::host::PlanAutoBackendForPower;
  const auto same_on_ac = PlanAutoBackendForPower(ReadyFacts(), false, VulkanBuild(),
                                                  BackendKind::Vulkan, AutoWithModel());
  EXPECT_FALSE(same_on_ac.changed);
  EXPECT_FALSE(same_on_ac.reload);
  // Without ggml-vulkan, `auto` is CPU on either power source.
  const auto cpu_build = PlanAutoBackendForPower(ReadyFacts(), false, CompiledBackends{},
                                                 BackendKind::Cpu, AutoWithModel());
  EXPECT_FALSE(cpu_build.changed);
  // A Host started in SafeMode collected no facts, so `auto` stays on CPU.
  const auto no_facts = PlanAutoBackendForPower(BackendSystemFacts{}, false, VulkanBuild(),
                                                BackendKind::Cpu, AutoWithModel());
  EXPECT_EQ(no_facts.selection.backend, BackendKind::Cpu);
  EXPECT_FALSE(no_facts.changed);
}

TEST(BackendSelectorTest, PowerChangeTracksAutoButReloadsOnlyAnAutoRequestWithAModel) {
  using azookey::host::PlanAutoBackendForPower;
  auto explicit_request = AutoWithModel();
  explicit_request.auto_requested = false;
  auto safe_mode = AutoWithModel();
  safe_mode.safe_mode = true;
  auto no_model = AutoWithModel();
  no_model.model_active = false;
  for (const auto& state : {explicit_request, safe_mode, no_model}) {
    const auto plan =
        PlanAutoBackendForPower(ReadyFacts(), true, VulkanBuild(), BackendKind::Vulkan, state);
    // `auto` still follows the power source, so switching back to it later
    // resolves to the backend for the power source at that time.
    EXPECT_EQ(plan.selection.backend, BackendKind::Cpu);
    EXPECT_TRUE(plan.changed);
    EXPECT_FALSE(plan.reload);
  }
}

TEST(BackendSelectorTest, BuildWithoutVulkanAlwaysSelectsCpu) {
  const auto selection = SelectAutoBackend(ReadyFacts(), CompiledBackends{});
  EXPECT_EQ(selection.backend, BackendKind::Cpu);
  EXPECT_EQ(selection.reason, BackendSelectionReason::VulkanNotCompiled);
}

TEST(BackendSelectorTest, UnknownAdaptersSelectCpu) {
  auto facts = ReadyFacts();
  facts.adapters.reset();
  const auto selection = SelectAutoBackend(facts, VulkanBuild());
  EXPECT_EQ(selection.backend, BackendKind::Cpu);
  EXPECT_EQ(selection.reason, BackendSelectionReason::AdapterEnumerationFailed);
}

TEST(BackendSelectorTest, SoftwareAdaptersAndNpusDoNotCountAsGpus) {
  auto facts = ReadyFacts();
  facts.adapters = std::vector<GraphicsAdapterInfo>{SoftwareGpu(), HardwareNpu()};
  const auto selection = SelectAutoBackend(facts, VulkanBuild());
  EXPECT_EQ(selection.backend, BackendKind::Cpu);
  EXPECT_EQ(selection.reason, BackendSelectionReason::NoHardwareGpu);
  EXPECT_EQ(selection.hardware_gpu_count, 0U);
  EXPECT_EQ(selection.hardware_ml_only_count, 1U);
}

TEST(BackendSelectorTest, MissingVulkanLoaderSelectsCpu) {
  auto facts = ReadyFacts();
  facts.vulkan_loader_present = false;
  const auto selection = SelectAutoBackend(facts, VulkanBuild());
  EXPECT_EQ(selection.backend, BackendKind::Cpu);
  EXPECT_EQ(selection.reason, BackendSelectionReason::VulkanLoaderMissing);
}

TEST(BackendSelectorTest, BatteryAvoidsGpu) {
  auto facts = ReadyFacts();
  facts.on_battery = true;
  const auto selection = SelectAutoBackend(facts, VulkanBuild());
  EXPECT_EQ(selection.backend, BackendKind::Cpu);
  EXPECT_EQ(selection.reason, BackendSelectionReason::OnBattery);
}

TEST(BackendSelectorTest, CudaRuntimeIsNeverSelectedWhileCudaIsNotLinked) {
  auto facts = ReadyFacts();
  facts.cuda_runtime_present = true;
  EXPECT_EQ(SelectAutoBackend(facts, CompiledBackends{}).backend, BackendKind::Cpu);
  EXPECT_EQ(SelectAutoBackend(facts, VulkanBuild()).backend, BackendKind::Vulkan);
  // Even a build that links CUDA leaves the choice to Vulkan or CPU.
  EXPECT_EQ(SelectAutoBackend(facts, CompiledBackends{true, true}).backend, BackendKind::Vulkan);
  EXPECT_EQ(SelectAutoBackend(facts, CompiledBackends{true, false}).backend, BackendKind::Cpu);
}

TEST(BackendSelectorTest, EarlierRuleDecidesTheReason) {
  BackendSystemFacts facts;
  facts.on_battery = true;
  // Battery is checked before the adapters, which are unknown here too.
  EXPECT_EQ(SelectAutoBackend(facts, VulkanBuild()).reason, BackendSelectionReason::OnBattery);
  EXPECT_EQ(SelectAutoBackend(facts, CompiledBackends{}).reason,
            BackendSelectionReason::VulkanNotCompiled);
}

TEST(BackendSelectorTest, SafeModeSelectsCpuBeforeAnyOtherRule) {
  azookey::host::BackendSelectionPolicy policy;
  policy.safe_mode = true;
  const auto selection = SelectAutoBackend(ReadyFacts(), VulkanBuild(), policy);
  EXPECT_EQ(selection.backend, BackendKind::Cpu);
  EXPECT_EQ(selection.reason, BackendSelectionReason::SafeMode);
}

TEST(BackendSelectorTest, OldOsIsRecordedButDoesNotBlockVulkanByDefault) {
  auto facts = ReadyFacts();
  facts.os_build = 22631;
  const auto selection = SelectAutoBackend(facts, VulkanBuild());
  EXPECT_EQ(selection.backend, BackendKind::Vulkan);
  EXPECT_FALSE(selection.winml_os_eligible);

  facts.os_build.reset();
  EXPECT_FALSE(SelectAutoBackend(facts, VulkanBuild()).winml_os_eligible);
}

TEST(BackendSelectorTest, OsBuildPolicyKeepsOldOsOnCpu) {
  auto facts = ReadyFacts();
  facts.os_build = 22631;
  azookey::host::BackendSelectionPolicy policy;
  policy.require_winml_os_build = true;
  const auto selection = SelectAutoBackend(facts, VulkanBuild(), policy);
  EXPECT_EQ(selection.backend, BackendKind::Cpu);
  EXPECT_EQ(selection.reason, BackendSelectionReason::OsBuildBelowMinimum);
  facts.os_build = azookey::host::kWinMlMinimumOsBuild;
  EXPECT_EQ(SelectAutoBackend(facts, VulkanBuild(), policy).backend, BackendKind::Vulkan);
}

TEST(BackendSelectorTest, PowerProfileOverridesMeasuredPowerSource) {
  using azookey::host::OnBatteryForBackendSelection;
  using azookey::host::PowerSource;
  EXPECT_TRUE(OnBatteryForBackendSelection(PowerSource::Battery, "auto"));
  EXPECT_FALSE(OnBatteryForBackendSelection(PowerSource::Ac, "auto"));
  EXPECT_FALSE(OnBatteryForBackendSelection(PowerSource::Unknown, "auto"));
  EXPECT_FALSE(OnBatteryForBackendSelection(PowerSource::Battery, "performance"));
  EXPECT_TRUE(OnBatteryForBackendSelection(PowerSource::Ac, "battery_saver"));
}

class FakeAdapters final : public azookey::host::IAdapterEnumerator {
 public:
  std::optional<std::vector<GraphicsAdapterInfo>> result;
  bool throws{false};
  std::optional<std::vector<GraphicsAdapterInfo>> Enumerate() override {
    if (throws) throw std::runtime_error("enumeration failed");
    return result;
  }
};

class FakeLibraries final : public azookey::host::IRuntimeLibraryProbe {
 public:
  std::map<std::string, bool> present;
  bool throws{false};
  bool Exists(std::string_view dll_name) override {
    if (throws) throw std::runtime_error("probe failed");
    const auto it = present.find(std::string(dll_name));
    return it != present.end() && it->second;
  }
};

class FakeOs final : public azookey::host::IOsVersionProbe {
 public:
  std::optional<uint32_t> build;
  bool throws{false};
  std::optional<uint32_t> BuildNumber() override {
    if (throws) throw std::runtime_error("version failed");
    return build;
  }
};

TEST(BackendSelectorTest, CollectsFactsFromProbes) {
  FakeAdapters adapters;
  adapters.result = std::vector<GraphicsAdapterInfo>{HardwareGpu()};
  FakeLibraries libraries;
  libraries.present = {{"vulkan-1.dll", true}, {"cudart64_12.dll", true}};
  FakeOs os;
  os.build = 26200;
  const auto facts = azookey::host::CollectBackendSystemFacts(adapters, libraries, os, true);
  ASSERT_TRUE(facts.adapters.has_value());
  EXPECT_EQ(facts.adapters->size(), 1U);
  EXPECT_TRUE(facts.vulkan_loader_present);
  // CUDA needs both cudart and cublas.
  EXPECT_FALSE(facts.cuda_runtime_present);
  EXPECT_EQ(facts.os_build, 26200U);
  EXPECT_TRUE(facts.on_battery);

  libraries.present["cublas64_12.dll"] = true;
  EXPECT_TRUE(azookey::host::CollectBackendSystemFacts(adapters, libraries, os, false)
                  .cuda_runtime_present);
}

TEST(BackendSelectorTest, ThrowingProbesLeaveFactsUnknownAndSelectCpu) {
  FakeAdapters adapters;
  adapters.throws = true;
  FakeLibraries libraries;
  libraries.throws = true;
  FakeOs os;
  os.throws = true;
  const auto facts = azookey::host::CollectBackendSystemFacts(adapters, libraries, os, false);
  EXPECT_FALSE(facts.adapters.has_value());
  EXPECT_FALSE(facts.vulkan_loader_present);
  EXPECT_FALSE(facts.cuda_runtime_present);
  EXPECT_FALSE(facts.os_build.has_value());
  EXPECT_EQ(SelectAutoBackend(facts, VulkanBuild()).backend, BackendKind::Cpu);
}

// The real probes depend on the machine, so only their survival is asserted.
TEST(BackendSelectorTest, SystemProbesRunWithoutThrowing) {
  const auto adapters = azookey::host::MakeSystemAdapterEnumerator();
  const auto libraries = azookey::host::MakeSystemRuntimeLibraryProbe();
  const auto os = azookey::host::MakeSystemOsVersionProbe();
  const auto facts = azookey::host::CollectBackendSystemFacts(*adapters, *libraries, *os, false);
  const auto selection = SelectAutoBackend(facts, azookey::host::CompiledBackendsForThisBuild());
  EXPECT_NE(selection.backend, BackendKind::Cuda);
#ifdef _WIN32
  EXPECT_TRUE(facts.os_build.has_value());
#endif
}

}  // namespace
