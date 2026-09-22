#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <utility>

#include "azookey/host/HealthStateMachine.h"

namespace {

using azookey::host::HealthEvent;
using azookey::host::HealthState;
using azookey::host::HealthStateMachine;
using azookey::host::HostRunHistory;

constexpr std::array kStates{HealthState::Healthy,         HealthState::DegradedSimple,
                             HealthState::DegradedModel,   HealthState::RecoveringTransport,
                             HealthState::RecoveringModel, HealthState::SafeMode};
constexpr std::array kEvents{HealthEvent::HostUnresponsive,        HealthEvent::ModelFailed,
                             HealthEvent::TransportReconnected,    HealthEvent::ModelReloadAccepted,
                             HealthEvent::HostHealthConfirmed,     HealthEvent::ModelLoadConfirmed,
                             HealthEvent::TransportRecoveryFailed, HealthEvent::ModelRecoveryFailed,
                             HealthEvent::CrashLoopDetected,       HealthEvent::SafeModeCleared};

constexpr int64_t kNow = 1'700'000'000'000;

std::filesystem::path TempFile(const char* name) {
  auto path = std::filesystem::temp_directory_path() / name;
  std::filesystem::remove(path);
  return path;
}

// Section 8.5.1, row by row. Kept apart from the implementation's table so a
// row that goes missing or changes target there fails here.
TEST(HealthStateMachineTest, EveryTransitionInTheSpecTableIsTaken) {
  const std::array<std::pair<std::pair<HealthState, HealthEvent>, HealthState>, 14> expected{{
      {{HealthState::Healthy, HealthEvent::HostUnresponsive}, HealthState::DegradedSimple},
      {{HealthState::Healthy, HealthEvent::ModelFailed}, HealthState::DegradedModel},
      {{HealthState::DegradedSimple, HealthEvent::TransportReconnected},
       HealthState::RecoveringTransport},
      {{HealthState::DegradedModel, HealthEvent::ModelReloadAccepted},
       HealthState::RecoveringModel},
      {{HealthState::RecoveringTransport, HealthEvent::HostHealthConfirmed}, HealthState::Healthy},
      {{HealthState::RecoveringModel, HealthEvent::ModelLoadConfirmed}, HealthState::Healthy},
      {{HealthState::RecoveringTransport, HealthEvent::TransportRecoveryFailed},
       HealthState::DegradedSimple},
      {{HealthState::RecoveringModel, HealthEvent::ModelRecoveryFailed},
       HealthState::DegradedModel},
      {{HealthState::Healthy, HealthEvent::CrashLoopDetected}, HealthState::SafeMode},
      {{HealthState::DegradedSimple, HealthEvent::CrashLoopDetected}, HealthState::SafeMode},
      {{HealthState::DegradedModel, HealthEvent::CrashLoopDetected}, HealthState::SafeMode},
      {{HealthState::RecoveringTransport, HealthEvent::CrashLoopDetected}, HealthState::SafeMode},
      {{HealthState::RecoveringModel, HealthEvent::CrashLoopDetected}, HealthState::SafeMode},
      {{HealthState::SafeMode, HealthEvent::SafeModeCleared}, HealthState::Healthy},
  }};
  EXPECT_EQ(azookey::host::HealthTransitions().size(), expected.size());
  for (const auto& [key, to] : expected) {
    SCOPED_TRACE(std::string(azookey::host::HealthStateName(key.first)) + " + " +
                 std::string(azookey::host::HealthEventName(key.second)));
    EXPECT_EQ(azookey::host::NextHealthState(key.first, key.second), to);
    HealthStateMachine machine(key.first);
    const auto transition = machine.Apply(key.second);
    ASSERT_TRUE(transition.has_value());
    EXPECT_EQ(transition->from, key.first);
    EXPECT_EQ(transition->event, key.second);
    EXPECT_EQ(transition->to, to);
    EXPECT_EQ(machine.state(), to);
  }
}

TEST(HealthStateMachineTest, PairsOutsideTheTableAreRejectedWithoutChangingState) {
  size_t accepted = 0;
  for (const auto from : kStates) {
    for (const auto event : kEvents) {
      HealthStateMachine machine(from);
      const auto transition = machine.Apply(event);
      if (transition) {
        ++accepted;
        EXPECT_NE(transition->to, from) << "self-transition";
      } else {
        EXPECT_EQ(machine.state(), from);
        EXPECT_FALSE(azookey::host::NextHealthState(from, event).has_value());
      }
    }
  }
  EXPECT_EQ(accepted, azookey::host::kHealthTransitionCount);
}

// Section 8.5.1: a model reload stays out of Healthy while only the pipe is
// fine, so the degraded-model UI is not taken down by a Ping.
TEST(HealthStateMachineTest, ModelRecoveryIgnoresTransportHealth) {
  HealthStateMachine machine(HealthState::RecoveringModel);
  EXPECT_FALSE(machine.Apply(HealthEvent::HostHealthConfirmed).has_value());
  EXPECT_FALSE(machine.Apply(HealthEvent::TransportReconnected).has_value());
  EXPECT_EQ(machine.state(), HealthState::RecoveringModel);
  EXPECT_TRUE(machine.Apply(HealthEvent::ModelLoadConfirmed).has_value());
  EXPECT_EQ(machine.state(), HealthState::Healthy);

  HealthStateMachine transport(HealthState::RecoveringTransport);
  EXPECT_FALSE(transport.Apply(HealthEvent::ModelLoadConfirmed).has_value());
  EXPECT_EQ(transport.state(), HealthState::RecoveringTransport);
}

TEST(HealthStateMachineTest, SafeModeLeavesOnlyByTheUserClearingIt) {
  HealthStateMachine machine(HealthState::SafeMode);
  for (const auto event : kEvents) {
    if (event == HealthEvent::SafeModeCleared) continue;
    EXPECT_FALSE(machine.Apply(event).has_value()) << azookey::host::HealthEventName(event);
    EXPECT_EQ(machine.state(), HealthState::SafeMode);
  }
  EXPECT_TRUE(machine.Apply(HealthEvent::SafeModeCleared).has_value());
  EXPECT_EQ(machine.state(), HealthState::Healthy);
}

TEST(HealthStateMachineTest, TransportRecoveryCanFailAndRetry) {
  HealthStateMachine machine;
  EXPECT_EQ(machine.state(), HealthState::Healthy);
  ASSERT_TRUE(machine.Apply(HealthEvent::HostUnresponsive));
  ASSERT_TRUE(machine.Apply(HealthEvent::TransportReconnected));
  ASSERT_TRUE(machine.Apply(HealthEvent::TransportRecoveryFailed));
  EXPECT_EQ(machine.state(), HealthState::DegradedSimple);
  ASSERT_TRUE(machine.Apply(HealthEvent::TransportReconnected));
  ASSERT_TRUE(machine.Apply(HealthEvent::HostHealthConfirmed));
  EXPECT_EQ(machine.state(), HealthState::Healthy);
}

TEST(HealthStateMachineTest, NamesAreDistinctLogWords) {
  std::set<std::string> states;
  for (const auto state : kStates) states.emplace(azookey::host::HealthStateName(state));
  EXPECT_EQ(states.size(), kStates.size());
  EXPECT_EQ(azookey::host::HealthStateName(HealthState::SafeMode), "safe_mode");
  std::set<std::string> events;
  for (const auto event : kEvents) events.emplace(azookey::host::HealthEventName(event));
  EXPECT_EQ(events.size(), kEvents.size());
  EXPECT_EQ(events.count("unknown"), 0u);
}

TEST(HostCrashHistoryTest, FirstStartAndCleanRestartsAreNotCrashes) {
  const auto first = azookey::host::RecordHostStart(HostRunHistory{}, 10, kNow);
  EXPECT_FALSE(first.previous_run_crashed);
  EXPECT_EQ(first.recent_crashes, 0u);
  EXPECT_FALSE(first.crash_loop);
  EXPECT_TRUE(first.next.running);
  EXPECT_EQ(first.next.pid, 10u);

  const auto after_clean =
      azookey::host::RecordHostStart(azookey::host::RecordHostCleanExit(), 11, kNow + 1000);
  EXPECT_FALSE(after_clean.previous_run_crashed);
  EXPECT_FALSE(after_clean.crash_loop);
}

TEST(HostCrashHistoryTest, ThreeCrashesInsideSixtySecondsEnterSafeMode) {
  auto history = azookey::host::RecordHostStart(HostRunHistory{}, 1, kNow).next;
  auto outcome = azookey::host::RecordHostStart(history, 2, kNow + 1000);
  EXPECT_TRUE(outcome.previous_run_crashed);
  EXPECT_EQ(outcome.recent_crashes, 1u);
  EXPECT_FALSE(outcome.crash_loop);
  outcome = azookey::host::RecordHostStart(outcome.next, 3, kNow + 2000);
  EXPECT_EQ(outcome.recent_crashes, 2u);
  EXPECT_FALSE(outcome.crash_loop);
  outcome = azookey::host::RecordHostStart(outcome.next, 4, kNow + 3000);
  EXPECT_EQ(outcome.recent_crashes, azookey::host::kSafeModeCrashThreshold);
  EXPECT_TRUE(outcome.crash_loop);
}

TEST(HostCrashHistoryTest, CrashesOutsideTheWindowOrBeforeACleanExitDoNotCount) {
  const int64_t window = azookey::host::kSafeModeCrashWindow.count();
  HostRunHistory spread{true, 1, {kNow - window, kNow - window + 1}};
  // The first falls on the window's edge and drops out; the second stays.
  auto outcome = azookey::host::RecordHostStart(spread, 2, kNow);
  EXPECT_EQ(outcome.recent_crashes, 2u);
  EXPECT_FALSE(outcome.crash_loop);

  // Crashes recorded before a clean exit are not consecutive with later ones.
  auto history = azookey::host::RecordHostCleanExit();
  EXPECT_TRUE(history.crash_epoch_ms.empty());
  history.running = true;
  outcome = azookey::host::RecordHostStart(history, 3, kNow);
  EXPECT_EQ(outcome.recent_crashes, 1u);
}

TEST(HostCrashHistoryTest, TimesAheadOfTheClockAreDropped) {
  HostRunHistory history{true, 1, {kNow + 5000, kNow + 6000}};
  const auto outcome = azookey::host::RecordHostStart(history, 2, kNow);
  EXPECT_EQ(outcome.recent_crashes, 1u);
  EXPECT_FALSE(outcome.crash_loop);
}

TEST(HostCrashHistoryTest, HistoryRoundTripsThroughTheFile) {
  const auto path = TempFile("azookey_host_run_state_roundtrip.txt");
  EXPECT_FALSE(azookey::host::ReadHostRunHistory(path).has_value());

  HostRunHistory running{true, 4242, {kNow, kNow + 10}};
  ASSERT_TRUE(azookey::host::WriteHostRunHistory(path, running));
  const auto read = azookey::host::ReadHostRunHistory(path);
  ASSERT_TRUE(read.has_value());
  EXPECT_TRUE(read->running);
  EXPECT_EQ(read->pid, 4242u);
  EXPECT_EQ(read->crash_epoch_ms, running.crash_epoch_ms);

  ASSERT_TRUE(azookey::host::WriteHostRunHistory(path, azookey::host::RecordHostCleanExit()));
  const auto stopped = azookey::host::ReadHostRunHistory(path);
  ASSERT_TRUE(stopped.has_value());
  EXPECT_FALSE(stopped->running);
  EXPECT_TRUE(stopped->crash_epoch_ms.empty());
  std::filesystem::remove(path);
}

TEST(HostCrashHistoryTest, MalformedHistoryIsTreatedAsFresh) {
  const auto path = TempFile("azookey_host_run_state_malformed.txt");
  for (const char* content :
       {"", "not a history\n", "azookey-host-run v1\n", "azookey-host-run v1\nrunning\n",
        "azookey-host-run v1\nmaybe 1\n", "azookey-host-run v1\nstopped\nsoon\n"}) {
    {
      std::ofstream out(path, std::ios::binary | std::ios::trunc);
      out << content;
    }
    EXPECT_FALSE(azookey::host::ReadHostRunHistory(path).has_value()) << content;
  }
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << "azookey-host-run v1\nstopped\n" << std::string(5000, '1') << "\n";
  }
  EXPECT_FALSE(azookey::host::ReadHostRunHistory(path).has_value());
  std::filesystem::remove(path);
}

TEST(HostCrashHistoryTest, ThisProcessIsNotAnotherLiveHost) {
  EXPECT_FALSE(azookey::host::IsOtherProcessAlive(azookey::host::CurrentProcessId()));
  EXPECT_FALSE(azookey::host::IsOtherProcessAlive(0));
}

TEST(HostCrashHistoryTest, EnteredAtIsRfc3339Utc) {
  EXPECT_EQ(azookey::host::FormatRfc3339Utc(0), "1970-01-01T00:00:00Z");
  EXPECT_EQ(azookey::host::FormatRfc3339Utc(1'700'000'000'999), "2023-11-14T22:13:20Z");
}

}  // namespace
