#include "azookey/host/HealthStateMachine.h"

#include <algorithm>
#include <ctime>
#include <fstream>
#include <sstream>

#include "azookey/learning/AtomicFile.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

namespace azookey::host {

namespace {

using S = HealthState;
using E = HealthEvent;

constexpr std::array<HealthTransition, kHealthTransitionCount> kTransitions{{
    {S::Healthy, E::HostUnresponsive, S::DegradedSimple},
    {S::Healthy, E::ModelFailed, S::DegradedModel},
    {S::DegradedSimple, E::TransportReconnected, S::RecoveringTransport},
    {S::DegradedModel, E::ModelReloadAccepted, S::RecoveringModel},
    {S::RecoveringTransport, E::HostHealthConfirmed, S::Healthy},
    {S::RecoveringModel, E::ModelLoadConfirmed, S::Healthy},
    {S::RecoveringTransport, E::TransportRecoveryFailed, S::DegradedSimple},
    {S::RecoveringModel, E::ModelRecoveryFailed, S::DegradedModel},
    {S::Healthy, E::CrashLoopDetected, S::SafeMode},
    {S::DegradedSimple, E::CrashLoopDetected, S::SafeMode},
    {S::DegradedModel, E::CrashLoopDetected, S::SafeMode},
    {S::RecoveringTransport, E::CrashLoopDetected, S::SafeMode},
    {S::RecoveringModel, E::CrashLoopDetected, S::SafeMode},
    {S::SafeMode, E::SafeModeCleared, S::Healthy},
}};

constexpr std::string_view kRunHistoryHeader = "azookey-host-run v1";
// The file only ever needs the crashes inside one window; anything past this
// is not a history the Host wrote.
constexpr size_t kMaxRunHistoryBytes = 4096;
constexpr size_t kMaxRecordedCrashes = 32;

}  // namespace

const std::array<HealthTransition, kHealthTransitionCount>& HealthTransitions() {
  return kTransitions;
}

std::optional<HealthState> NextHealthState(HealthState from, HealthEvent event) {
  for (const auto& transition : kTransitions) {
    if (transition.from == from && transition.event == event) return transition.to;
  }
  return std::nullopt;
}

std::string_view HealthStateName(HealthState state) {
  switch (state) {
    case S::Healthy:
      return "healthy";
    case S::DegradedSimple:
      return "degraded_simple";
    case S::DegradedModel:
      return "degraded_model";
    case S::RecoveringTransport:
      return "recovering_transport";
    case S::RecoveringModel:
      return "recovering_model";
    case S::SafeMode:
      return "safe_mode";
  }
  return "unknown";
}

std::string_view HealthEventName(HealthEvent event) {
  switch (event) {
    case E::HostUnresponsive:
      return "host_unresponsive";
    case E::ModelFailed:
      return "model_failed";
    case E::TransportReconnected:
      return "transport_reconnected";
    case E::ModelReloadAccepted:
      return "model_reload_accepted";
    case E::HostHealthConfirmed:
      return "host_health_confirmed";
    case E::ModelLoadConfirmed:
      return "model_load_confirmed";
    case E::TransportRecoveryFailed:
      return "transport_recovery_failed";
    case E::ModelRecoveryFailed:
      return "model_recovery_failed";
    case E::CrashLoopDetected:
      return "crash_loop_detected";
    case E::SafeModeCleared:
      return "safe_mode_cleared";
  }
  return "unknown";
}

std::optional<HealthTransition> HealthStateMachine::Apply(HealthEvent event) {
  const auto next = NextHealthState(state_, event);
  if (!next) return std::nullopt;
  const HealthTransition transition{state_, event, *next};
  state_ = *next;
  return transition;
}

HostStartOutcome RecordHostStart(const HostRunHistory& previous, uint32_t pid,
                                 int64_t now_epoch_ms) {
  HostStartOutcome outcome;
  outcome.previous_run_crashed = previous.running;
  const int64_t window_start = now_epoch_ms - kSafeModeCrashWindow.count();
  for (const int64_t at : previous.crash_epoch_ms) {
    // A time ahead of now is a clock that moved back; it says nothing about
    // how recent the crash was, so it does not count toward the window.
    if (at > window_start && at <= now_epoch_ms) outcome.next.crash_epoch_ms.push_back(at);
  }
  if (outcome.previous_run_crashed) outcome.next.crash_epoch_ms.push_back(now_epoch_ms);
  if (outcome.next.crash_epoch_ms.size() > kMaxRecordedCrashes) {
    outcome.next.crash_epoch_ms.erase(
        outcome.next.crash_epoch_ms.begin(),
        outcome.next.crash_epoch_ms.end() - static_cast<std::ptrdiff_t>(kMaxRecordedCrashes));
  }
  outcome.recent_crashes = outcome.next.crash_epoch_ms.size();
  outcome.crash_loop =
      outcome.previous_run_crashed && outcome.recent_crashes >= kSafeModeCrashThreshold;
  outcome.next.running = true;
  outcome.next.pid = pid;
  return outcome;
}

HostRunHistory RecordHostCleanExit() { return HostRunHistory{}; }

std::optional<HostRunHistory> ReadHostRunHistory(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  std::string content(kMaxRunHistoryBytes + 1, '\0');
  in.read(content.data(), static_cast<std::streamsize>(content.size()));
  content.resize(static_cast<size_t>(in.gcount()));
  if (content.size() > kMaxRunHistoryBytes) return std::nullopt;

  std::istringstream lines(content);
  std::string line;
  if (!std::getline(lines, line) || line != kRunHistoryHeader) return std::nullopt;
  if (!std::getline(lines, line)) return std::nullopt;
  HostRunHistory history;
  std::istringstream status(line);
  std::string word;
  status >> word;
  if (word == "running") {
    history.running = true;
    status >> history.pid;
    if (!status) return std::nullopt;
  } else if (word != "stopped") {
    return std::nullopt;
  }
  while (std::getline(lines, line)) {
    if (line.empty()) continue;
    std::istringstream value(line);
    int64_t at = 0;
    if (!(value >> at)) return std::nullopt;
    history.crash_epoch_ms.push_back(at);
    if (history.crash_epoch_ms.size() > kMaxRecordedCrashes) return std::nullopt;
  }
  return history;
}

bool WriteHostRunHistory(const std::filesystem::path& path, const HostRunHistory& history) {
  std::ostringstream out;
  out << kRunHistoryHeader << '\n';
  if (history.running) {
    out << "running " << history.pid << '\n';
  } else {
    out << "stopped\n";
  }
  for (const int64_t at : history.crash_epoch_ms) out << at << '\n';
  return learning::WriteTextFileAtomically(path, out.str());
}

bool IsOtherProcessAlive(uint32_t pid) {
  if (pid == 0) return false;
#ifdef _WIN32
  if (pid == ::GetCurrentProcessId()) return false;
  HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) return false;
  DWORD exit_code = 0;
  const bool alive = ::GetExitCodeProcess(process, &exit_code) && exit_code == STILL_ACTIVE;
  ::CloseHandle(process);
  return alive;
#else
  if (static_cast<pid_t>(pid) == ::getpid()) return false;
  return ::kill(static_cast<pid_t>(pid), 0) == 0;
#endif
}

uint32_t CurrentProcessId() {
#ifdef _WIN32
  return static_cast<uint32_t>(::GetCurrentProcessId());
#else
  return static_cast<uint32_t>(::getpid());
#endif
}

std::string FormatRfc3339Utc(int64_t epoch_ms) {
  const std::time_t seconds = static_cast<std::time_t>(epoch_ms / 1000);
  std::tm utc{};
#ifdef _WIN32
  if (gmtime_s(&utc, &seconds) != 0) return {};
#else
  if (!gmtime_r(&seconds, &utc)) return {};
#endif
  char buffer[32]{};
  if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc) == 0) return {};
  return buffer;
}

}  // namespace azookey::host
