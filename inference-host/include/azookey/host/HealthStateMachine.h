#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace azookey::host {

// M47 user-visible recovery states (docs/dev-infrastructure-spec.md section 8.5.1).
// Recovering keeps the degradation it came from, because its exits differ by
// origin, so it is two values here: a model reload that answers Ping must still
// stay out of Healthy until the model is loaded.
enum class HealthState : uint8_t {
  Healthy,
  DegradedSimple,
  DegradedModel,
  RecoveringTransport,
  RecoveringModel,
  SafeMode,
};

enum class HealthEvent : uint8_t {
  // The Host stopped answering: pipe lost, or connected but silent past the
  // section 8.5.2 deadline of what it was asked.
  HostUnresponsive,
  // Zenzai failed to load, or an inference ran past its deadline.
  ModelFailed,
  // The pipe is back and the Handshake was accepted.
  TransportReconnected,
  // LoadModel was accepted. Not completed: completion is ModelLoadConfirmed.
  ModelReloadAccepted,
  // A Ping round trip returned inside its deadline.
  HostHealthConfirmed,
  // LoadModel succeeded and a later Health reports model_loaded == true.
  ModelLoadConfirmed,
  // Reconnecting failed, or the deadline was missed again.
  TransportRecoveryFailed,
  // The reload failed or timed out, or Health still reports model_loaded == false.
  ModelRecoveryFailed,
  // Section 8.5.3: kSafeModeCrashThreshold crashes inside kSafeModeCrashWindow.
  CrashLoopDetected,
  // The user turned settings.safeMode.enabled off. SafeMode never clears itself.
  SafeModeCleared,
};

struct HealthTransition {
  HealthState from;
  HealthEvent event;
  HealthState to;
};

inline constexpr size_t kHealthTransitionCount = 14;

// The whole table. Pairs that are not listed are rejected, and no row is a
// self-transition.
const std::array<HealthTransition, kHealthTransitionCount>& HealthTransitions();
std::optional<HealthState> NextHealthState(HealthState from, HealthEvent event);
std::string_view HealthStateName(HealthState state);
std::string_view HealthEventName(HealthEvent event);

class HealthStateMachine {
 public:
  HealthStateMachine() = default;
  // Resumes a state that outlives the process, which is only SafeMode: its
  // flag is persisted and the next start is still in it.
  explicit HealthStateMachine(HealthState initial) : state_(initial) {}

  HealthState state() const { return state_; }
  // The transition taken, or nullopt when the table has no row for the current
  // state and event. A rejected event leaves the state unchanged.
  std::optional<HealthTransition> Apply(HealthEvent event);

 private:
  HealthState state_{HealthState::Healthy};
};

// Section 8.5.3 crash-loop detection. The Host marks itself running when it
// starts and stopped when it exits cleanly; a start that still finds the mark
// means the previous process died without reaching an exit path.
inline constexpr size_t kSafeModeCrashThreshold = 3;
inline constexpr std::chrono::milliseconds kSafeModeCrashWindow{60000};

struct HostRunHistory {
  bool running{false};
  uint32_t pid{0};
  // Detection times of the crashes since the last clean exit, oldest first.
  std::vector<int64_t> crash_epoch_ms;
};

struct HostStartOutcome {
  HostRunHistory next;
  bool previous_run_crashed{false};
  // Crashes inside kSafeModeCrashWindow, the one detected now included.
  size_t recent_crashes{0};
  bool crash_loop{false};
};

// Pure: folds one start into the history. The caller must not record a start
// while the process named in `previous` is still alive, since that is another
// Host holding the pipe and not a crash.
HostStartOutcome RecordHostStart(const HostRunHistory& previous, uint32_t pid,
                                 int64_t now_epoch_ms);
// A clean exit ends the run of consecutive crashes.
HostRunHistory RecordHostCleanExit();

// nullopt when the file is missing or unreadable, which is a fresh history.
std::optional<HostRunHistory> ReadHostRunHistory(const std::filesystem::path& path);
bool WriteHostRunHistory(const std::filesystem::path& path, const HostRunHistory& history);
// Whether `pid` names a live process other than this one.
bool IsOtherProcessAlive(uint32_t pid);
uint32_t CurrentProcessId();

// RFC 3339 in UTC with second precision, for settings.safeMode.enteredAt.
std::string FormatRfc3339Utc(int64_t epoch_ms);

}  // namespace azookey::host
