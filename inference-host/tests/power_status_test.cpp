#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "azookey/host/PowerLanePolicy.h"
#include "azookey/host/PowerStatusWatcher.h"
#include "azookey/host/SettingsStore.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

using azookey::host::InferenceLane;
using azookey::host::kPowerBroadcastResumeAutomatic;
using azookey::host::kPowerBroadcastStatusChange;
using azookey::host::PowerSource;
using azookey::host::PowerStateTracker;
using azookey::host::ResolveInferenceLane;

using Change = std::pair<PowerSource, PowerSource>;

// PBT_APMSUSPEND: a broadcast the tracker does not resample on.
constexpr uintptr_t kPowerBroadcastSuspend = 0x0004;

TEST(PowerLanePolicyTest, BatteryHoldsHeavyInferenceToTheFastLane) {
  EXPECT_EQ(ResolveInferenceLane(PowerSource::Ac, "auto"), InferenceLane::Heavy);
  EXPECT_EQ(ResolveInferenceLane(PowerSource::Battery, "auto"), InferenceLane::Fast);
}

TEST(PowerLanePolicyTest, UnknownPowerSourceCountsAsAc) {
  EXPECT_EQ(ResolveInferenceLane(PowerSource::Unknown, "auto"), InferenceLane::Heavy);
}

TEST(PowerLanePolicyTest, PowerProfileOverridesThePowerSource) {
  EXPECT_EQ(ResolveInferenceLane(PowerSource::Battery, "performance"), InferenceLane::Heavy);
  EXPECT_EQ(ResolveInferenceLane(PowerSource::Ac, "battery_saver"), InferenceLane::Fast);
  EXPECT_EQ(ResolveInferenceLane(PowerSource::Unknown, "battery_saver"), InferenceLane::Fast);
}

TEST(PowerStateTrackerTest, SamplesTheInitialSource) {
  PowerStateTracker tracker([] { return PowerSource::Battery; }, nullptr);
  EXPECT_EQ(tracker.current(), PowerSource::Battery);
}

TEST(PowerStateTrackerTest, ReportsASourceChange) {
  PowerSource source = PowerSource::Ac;
  std::vector<Change> changes;
  PowerStateTracker tracker(
      [&] { return source; },
      [&](PowerSource from, PowerSource to) { changes.emplace_back(from, to); });
  source = PowerSource::Battery;
  EXPECT_TRUE(tracker.OnPowerBroadcast(kPowerBroadcastStatusChange));
  source = PowerSource::Ac;
  EXPECT_TRUE(tracker.OnPowerBroadcast(kPowerBroadcastResumeAutomatic));
  EXPECT_EQ(changes, (std::vector<Change>{{PowerSource::Ac, PowerSource::Battery},
                                          {PowerSource::Battery, PowerSource::Ac}}));
  EXPECT_EQ(tracker.current(), PowerSource::Ac);
}

TEST(PowerStateTrackerTest, BatteryLevelOnlyChangesAreNotReported) {
  int listener_calls = 0;
  PowerStateTracker tracker([] { return PowerSource::Battery; },
                            [&](PowerSource, PowerSource) { ++listener_calls; });
  EXPECT_FALSE(tracker.OnPowerBroadcast(kPowerBroadcastStatusChange));
  EXPECT_EQ(listener_calls, 0);
}

TEST(PowerStateTrackerTest, OtherBroadcastsDoNotResample) {
  int probe_calls = 0;
  PowerStateTracker tracker(
      [&] {
        ++probe_calls;
        return PowerSource::Ac;
      },
      nullptr);
  EXPECT_FALSE(tracker.OnPowerBroadcast(kPowerBroadcastSuspend));
  EXPECT_EQ(probe_calls, 1);
}

TEST(PowerStateTrackerTest, AFailingProbeReadsAsUnknown) {
  bool fail = false;
  std::vector<Change> changes;
  PowerStateTracker tracker(
      [&] {
        if (fail) throw std::runtime_error("power status unavailable");
        return PowerSource::Battery;
      },
      [&](PowerSource from, PowerSource to) { changes.emplace_back(from, to); });
  fail = true;
  EXPECT_TRUE(tracker.OnPowerBroadcast(kPowerBroadcastStatusChange));
  EXPECT_EQ(changes, (std::vector<Change>{{PowerSource::Battery, PowerSource::Unknown}}));
  EXPECT_EQ(ResolveInferenceLane(tracker.current(), "auto"), InferenceLane::Heavy);
}

// The Host re-applies the runtime settings on a change; the NLL rerank, the
// Heavy lane's extra pass, follows the power source the tracker reports.
TEST(PowerStateTrackerTest, AChangeSwitchesTheLaneTheSettingsApply) {
  using azookey::host::ApplyRuntimeSettingsToEngineConfig;
  using azookey::host::BackendKind;
  using azookey::host::EngineConfig;
  using azookey::host::InferenceThreadEnvironment;

  azookey::host::RuntimeSettings settings;
  settings.nll.enabled = true;
  PowerSource source = PowerSource::Ac;
  EngineConfig applied;
  std::unique_ptr<PowerStateTracker> tracker;
  const auto apply = [&] {
    applied = ApplyRuntimeSettingsToEngineConfig(applied, settings, BackendKind::Cpu, [&] {
      return InferenceThreadEnvironment{tracker->current(), 8};
    });
  };
  tracker = std::make_unique<PowerStateTracker>([&] { return source; },
                                                [&](PowerSource, PowerSource) { apply(); });
  apply();
  EXPECT_TRUE(applied.nll.enabled);
  EXPECT_EQ(applied.inference_threads, 8);

  source = PowerSource::Battery;
  ASSERT_TRUE(tracker->OnPowerBroadcast(kPowerBroadcastStatusChange));
  EXPECT_FALSE(applied.nll.enabled);
  EXPECT_EQ(applied.inference_threads, 2);

  source = PowerSource::Ac;
  ASSERT_TRUE(tracker->OnPowerBroadcast(kPowerBroadcastStatusChange));
  EXPECT_TRUE(applied.nll.enabled);
  EXPECT_EQ(applied.inference_threads, 8);
}

TEST(PowerChangeDebouncerTest, SettlesOnlyAfterTheQuietPeriod) {
  using azookey::host::PowerChangeDebouncer;
  using std::chrono::seconds;
  PowerChangeDebouncer debouncer(seconds(5));
  const PowerChangeDebouncer::Clock::time_point t0{};
  EXPECT_FALSE(debouncer.deadline().has_value());
  EXPECT_FALSE(debouncer.TakeSettled(t0).has_value());

  debouncer.Note(PowerSource::Battery, t0);
  EXPECT_EQ(debouncer.deadline(), t0 + seconds(5));
  EXPECT_FALSE(debouncer.TakeSettled(t0 + seconds(4)).has_value());
  EXPECT_EQ(debouncer.TakeSettled(t0 + seconds(5)), PowerSource::Battery);
  // Each settled source is handed out once.
  EXPECT_FALSE(debouncer.TakeSettled(t0 + seconds(6)).has_value());
  EXPECT_FALSE(debouncer.deadline().has_value());
}

TEST(PowerChangeDebouncerTest, AFlapRestartsTheQuietPeriodAndKeepsTheLatestSource) {
  using azookey::host::PowerChangeDebouncer;
  using std::chrono::seconds;
  PowerChangeDebouncer debouncer(seconds(5));
  const PowerChangeDebouncer::Clock::time_point t0{};
  debouncer.Note(PowerSource::Battery, t0);
  debouncer.Note(PowerSource::Ac, t0 + seconds(3));
  EXPECT_FALSE(debouncer.TakeSettled(t0 + seconds(6)).has_value());
  EXPECT_EQ(debouncer.TakeSettled(t0 + seconds(8)), PowerSource::Ac);
}

#ifdef _WIN32

TEST(PowerStatusWatcherTest, WindowDeliversPowerBroadcastToTheTracker) {
  PowerSource source = PowerSource::Ac;
  std::vector<Change> changes;
  PowerStateTracker tracker(
      [&] { return source; },
      [&](PowerSource from, PowerSource to) { changes.emplace_back(from, to); });
  azookey::host::PowerStatusWatcher watcher(tracker);
  ASSERT_TRUE(watcher.Start());
  const auto window = static_cast<HWND>(watcher.window());
  ASSERT_NE(window, nullptr);
  // The watcher is top-level: a message-only window would miss the broadcast.
  EXPECT_EQ(GetAncestor(window, GA_PARENT), GetDesktopWindow());
  EXPECT_FALSE(IsWindowVisible(window));

  source = PowerSource::Battery;
  // SendMessage returns once the watcher's thread has handled the message.
  EXPECT_EQ(SendMessageW(window, WM_POWERBROADCAST, PBT_APMPOWERSTATUSCHANGE, 0), TRUE);
  EXPECT_EQ(changes, (std::vector<Change>{{PowerSource::Ac, PowerSource::Battery}}));

  watcher.Stop();
  EXPECT_EQ(watcher.window(), nullptr);
  EXPECT_FALSE(IsWindow(window));
  watcher.Stop();
}

TEST(PowerStatusWatcherTest, RestartsAfterStop) {
  PowerStateTracker tracker([] { return PowerSource::Ac; }, nullptr);
  {
    azookey::host::PowerStatusWatcher first(tracker);
    ASSERT_TRUE(first.Start());
  }
  azookey::host::PowerStatusWatcher second(tracker);
  EXPECT_TRUE(second.Start());
}

#else

TEST(PowerStatusWatcherTest, DoesNotStartOutsideWindows) {
  PowerStateTracker tracker([] { return PowerSource::Ac; }, nullptr);
  azookey::host::PowerStatusWatcher watcher(tracker);
  EXPECT_FALSE(watcher.Start());
  EXPECT_EQ(watcher.window(), nullptr);
}

#endif

}  // namespace
