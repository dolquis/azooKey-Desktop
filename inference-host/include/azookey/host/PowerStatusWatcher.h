#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#include "azookey/host/SettingsStore.h"

// M25 power status watching (docs/copilot-pc-backend-spec.md section 6.1).
namespace azookey::host {

// WM_POWERBROADCAST events the tracker resamples on. The PBT_* values are
// repeated here so the tracker builds without <windows.h>.
inline constexpr uintptr_t kPowerBroadcastStatusChange = 0x000A;     // PBT_APMPOWERSTATUSCHANGE
inline constexpr uintptr_t kPowerBroadcastResumeAutomatic = 0x0012;  // PBT_APMRESUMEAUTOMATIC

// Unknown when the power status cannot be read; callers treat that as AC.
using PowerSourceProbe = std::function<PowerSource()>;
PowerSource QuerySystemPowerSource();

// Keeps the last sampled power source and reports changes. The battery level
// changes alone also arrive as PBT_APMPOWERSTATUSCHANGE; they do not change
// the source and are not reported.
class PowerStateTracker {
 public:
  using Listener = std::function<void(PowerSource from, PowerSource to)>;

  // Samples the probe once for the initial state.
  PowerStateTracker(PowerSourceProbe probe, Listener listener);

  // Returns true when the event changed the source and the listener ran.
  bool OnPowerBroadcast(uintptr_t event);
  PowerSource current() const { return current_.load(); }

 private:
  PowerSource Sample() const;

  PowerSourceProbe probe_;
  Listener listener_;
  std::atomic<PowerSource> current_;
};

// Holds a power change until it has stood for `quiet`. Docking can flip the
// source several times within a second, and each backend switch reloads the
// model. Not thread-safe; the owner serializes the calls.
class PowerChangeDebouncer {
 public:
  using Clock = std::chrono::steady_clock;

  explicit PowerChangeDebouncer(Clock::duration quiet) : quiet_(quiet) {}

  // Records the latest source and restarts the quiet period.
  void Note(PowerSource source, Clock::time_point now);
  // The latest source once it has stood for the quiet period; returned once.
  std::optional<PowerSource> TakeSettled(Clock::time_point now);
  // When TakeSettled next returns a source; nullopt with nothing pending.
  std::optional<Clock::time_point> deadline() const;

 private:
  Clock::duration quiet_;
  std::optional<PowerSource> pending_;
  Clock::time_point noted_at_{};
};

// Applies power changes on its own thread, off the watcher's window thread, so
// a model load holding the config lock does not stall the window. Changes that
// arrive while one is applied collapse into one more run. Once a source has
// stood for `quiet`, `settle` runs with it (the backend re-resolution).
class PowerChangeWorker {
 public:
  using Apply = std::function<void(PowerSource from, PowerSource to)>;
  using Settle = std::function<void(PowerSource source)>;

  PowerChangeWorker(Apply apply, Settle settle, PowerChangeDebouncer::Clock::duration quiet);
  // Waits for a run in progress, then stops; pending changes are dropped.
  ~PowerChangeWorker();
  PowerChangeWorker(const PowerChangeWorker&) = delete;
  PowerChangeWorker& operator=(const PowerChangeWorker&) = delete;

  void Request(PowerSource from, PowerSource to);

 private:
  void Run();

  Apply apply_;
  Settle settle_;
  // Touched only by the worker thread.
  PowerChangeDebouncer debouncer_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::optional<std::pair<PowerSource, PowerSource>> pending_;
  bool stopping_{false};
  std::thread thread_;
};

// Receives WM_POWERBROADCAST in a hidden top-level window on its own thread.
// A message-only window would not get the broadcast. Outside Windows, Start
// fails and the power source stays the one sampled at startup.
class PowerStatusWatcher {
 public:
  explicit PowerStatusWatcher(PowerStateTracker& tracker);
  ~PowerStatusWatcher();
  PowerStatusWatcher(const PowerStatusWatcher&) = delete;
  PowerStatusWatcher& operator=(const PowerStatusWatcher&) = delete;

  // False when the window cannot be created; nothing is left running then.
  bool Start();
  // Closes the window and joins the thread. Safe to call more than once.
  void Stop();
  // The HWND receiving the broadcast, or null when not running.
  void* window() const { return window_; }

 private:
  void Run(std::function<void(void*)> ready);

  PowerStateTracker& tracker_;
  std::thread thread_;
  void* window_{nullptr};
};

}  // namespace azookey::host
