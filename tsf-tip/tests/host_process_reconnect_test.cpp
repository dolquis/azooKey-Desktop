#include <Windows.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "azookey/core/PlatformPaths.h"
#include "azookey/ipc/NamedPipeTransport.h"
#include "azookey/tsf/IpcConnectionState.h"
#include "azookey/tsf/TextService.h"

// DEV-1960 (M42 / M47): the TIP's IPC worker against a real Host process that
// is killed and started again. The in-process tests (ClientReconnectsAfterHostRestart,
// the fake-Host TextService tests) cannot show what only a process boundary
// shows: the pipe breaks without the Host's cooperation, and the next Host is a
// new process that must accept the same handshake token on the same pipe name.
//
// The Host runs on a pipe name, token and data root that belong to this test.
// It never reads or writes the resident Host's pipe, token file or data.

namespace {

using azookey::tsf::IpcConnectionState;

// One budget for the whole test, shared by every wait, so a failure ends with a
// diagnostic before the CTest timeout (120 s, see tsf-tip/CMakeLists.txt) kills
// the process without one. The second Host starts after a connect attempt (0.5 s
// of polling for the pipe) has already failed, so the worker finds it by a retry
// after its backoff (ceiling 3 s): about 3.5 s after the Host listens at most.
// Both are constants of the production code and are not injected; a slow
// (coverage) run only lengthens the Host's startup, which this budget covers
// many times over.
constexpr std::chrono::seconds kBudget{60};

// Shortens how long the worker takes to notice a dead pipe while it is idle: it
// probes the Host once per interval and a failed send ends the connection. The
// reply timeout and the miss threshold stay generous so a slow Host reply is
// never mistaken for the loss this test provokes.
constexpr uint32_t kHealthIntervalMs = 100;
constexpr uint32_t kHealthTimeoutMs = 10000;
constexpr uint32_t kHealthMissThreshold = 1000;

class ScopedEnvironmentValue {
 public:
  // An empty value removes the variable. The Hosts started later inherit the
  // change.
  ScopedEnvironmentValue(const char* name, const std::string& value) : name_(name) {
    char* prior = nullptr;
    size_t length = 0;
    if (_dupenv_s(&prior, &length, name) == 0 && prior) original_ = prior;
    std::free(prior);
    applied_ = _putenv_s(name, value.c_str()) == 0;
  }
  ~ScopedEnvironmentValue() { _putenv_s(name_.c_str(), original_.c_str()); }
  ScopedEnvironmentValue(const ScopedEnvironmentValue&) = delete;
  ScopedEnvironmentValue& operator=(const ScopedEnvironmentValue&) = delete;

  bool applied() const { return applied_; }

 private:
  std::string name_;
  std::string original_;
  bool applied_ = false;
};

// Tail of the Host's redirected stdout/stderr, where the reason it ended is
// written.
std::string ReadLogTail(const std::filesystem::path& path) {
  constexpr std::streamoff kTailBytes = 4096;
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) return "<no log>";
  const std::streamoff size = static_cast<std::streamoff>(input.tellg());
  input.seekg(std::max<std::streamoff>(0, size - kTailBytes));
  std::string text(static_cast<size_t>(std::min(size, kTailBytes)), '\0');
  input.read(text.data(), static_cast<std::streamsize>(text.size()));
  text.resize(static_cast<size_t>(input.gcount()));
  return text;
}

// A child process this test started. The destructor ends it, so a failed
// ASSERT cannot leave a Host running; it never touches any other process.
class HostProcess {
 public:
  HostProcess() = default;
  HostProcess(const HostProcess&) = delete;
  HostProcess& operator=(const HostProcess&) = delete;
  ~HostProcess() {
    Kill();
    Close();
  }

  bool Start(const std::filesystem::path& executable, const std::vector<std::wstring>& args,
             const std::filesystem::path& log_path) {
    Close();
    std::wstring command = L"\"" + executable.wstring() + L"\"";
    for (const auto& arg : args) command += L" \"" + arg + L"\"";
    SECURITY_ATTRIBUTES inheritable{sizeof(inheritable), nullptr, TRUE};
    HANDLE log = CreateFileW(log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &inheritable, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               &inheritable, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE || input == INVALID_HANDLE_VALUE) {
      if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
      if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
      return false;
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    startup.wShowWindow = SW_HIDE;
    startup.hStdInput = input;
    startup.hStdOutput = log;
    startup.hStdError = log;
    PROCESS_INFORMATION info{};
    // A console of its own keeps the Host away from the test runner's console.
    const bool created = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                                        CREATE_NEW_CONSOLE, nullptr, nullptr, &startup, &info);
    CloseHandle(log);
    CloseHandle(input);
    if (!created) return false;
    CloseHandle(info.hThread);
    process_ = info.hProcess;
    return true;
  }

  bool Running() const {
    return process_ != nullptr && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
  }

  // Ends the process the way a crash does: no shutdown handler runs and the
  // pipe breaks from underneath the client. Returns once the process is gone.
  bool Kill() {
    if (process_ == nullptr) return true;
    if (Running()) TerminateProcess(process_, 1);
    return WaitForSingleObject(process_, 15000) == WAIT_OBJECT_0;
  }

 private:
  void Close() {
    if (process_ != nullptr) CloseHandle(process_);
    process_ = nullptr;
  }

  HANDLE process_ = nullptr;
};

// Every state the worker published, in order. The hook runs on the worker
// thread, so the list is the only thing it touches.
class StateLog {
 public:
  static void Record(void* context, IpcConnectionState state) {
    auto* self = static_cast<StateLog*>(context);
    std::lock_guard<std::mutex> lock(self->mutex_);
    self->states_.push_back(state);
  }

  std::vector<IpcConnectionState> Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return states_;
  }

  size_t Count(IpcConnectionState state) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<size_t>(std::count(states_.begin(), states_.end(), state));
  }

 private:
  mutable std::mutex mutex_;
  std::vector<IpcConnectionState> states_;
};

// True when `pattern` appears in `states` in that order, other states allowed
// in between.
bool ContainsInOrder(const std::vector<IpcConnectionState>& states,
                     const std::vector<IpcConnectionState>& pattern) {
  size_t next = 0;
  for (const auto state : states) {
    if (next < pattern.size() && state == pattern[next]) ++next;
  }
  return next == pattern.size();
}

std::string StatesToString(const std::vector<IpcConnectionState>& states) {
  std::string text;
  for (const auto state : states) {
    if (!text.empty()) text += " > ";
    text += azookey::tsf::IpcConnectionStateName(state);
  }
  return text.empty() ? "<none>" : text;
}

class TsfTipHostRestartReconnectTest : public ::testing::Test {
 protected:
  void SetUp() override {
    host_executable_ = azookey::core::Utf8Path(HOST_RECONNECT_TEST_EXE);
    ASSERT_TRUE(std::filesystem::exists(host_executable_)) << host_executable_;
    const auto suffix = std::to_string(GetCurrentProcessId()) + "-" +
                        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    // Unique per run: pipe names are global, and a data root reused across
    // `--repeat` runs would carry the previous run's crash history into the
    // next Host start.
    pipe_name_ = "\\\\.\\pipe\\azookey-tip-host-restart-" + suffix;
    token_ = "host-restart-token-" + suffix;
    root_ = std::filesystem::temp_directory_path() / ("azookey_tip_host_restart_" + suffix);
    std::filesystem::create_directories(root_ / "data");
    started_ = std::chrono::steady_clock::now();
    deadline_ = started_ + kBudget;
  }

  void TearDown() override {
    if (HasFailure()) {
      std::cerr << "Stage times: " << StagesToString() << "\n"
                << "Worker states: " << StatesToString(states_.Snapshot()) << "\n"
                << "First Host log:\n"
                << ReadLogTail(root_ / "host-1.log") << "\nSecond Host log:\n"
                << ReadLogTail(root_ / "host-2.log") << std::endl;
    }
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  // Same arguments as the HostProcessTest fixture: everything the Host would
  // otherwise take from LocalAppData is given explicitly. --handshake-token in
  // particular keeps the Host from locking and rewriting the real per-user
  // token file.
  std::vector<std::wstring> HostArgs() const {
    return {L"--data-root",       root_.wstring(),
            L"--learning",        (root_ / "data" / "learning.tsv").wstring(),
            L"--user-dict",       (root_ / "data" / "user_dict.json").wstring(),
            L"--pipe-name",       std::filesystem::path(pipe_name_).wstring(),
            L"--handshake-token", std::filesystem::path(token_).wstring()};
  }

  // Polls an observable condition until it holds or the shared budget ends.
  // `must_run`, when given, is a Host that has to stay alive for the condition
  // to ever hold: its death fails the wait at once instead of at the deadline.
  ::testing::AssertionResult WaitUntil(const char* what, const std::function<bool()>& done,
                                       const HostProcess* must_run = nullptr) {
    while (!done()) {
      if (must_run != nullptr && !must_run->Running()) {
        return ::testing::AssertionFailure() << "the Host ended while waiting for: " << what;
      }
      if (std::chrono::steady_clock::now() >= deadline_) {
        return ::testing::AssertionFailure() << "timed out waiting for: " << what;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return ::testing::AssertionSuccess();
  }

  // Milliseconds since the test began at each stage it passed, so a slow run
  // shows which stage was slow.
  void Mark(const char* stage) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started_);
    stages_.emplace_back(stage, elapsed.count());
  }

  std::string StagesToString() const {
    std::string text;
    for (const auto& [stage, ms] : stages_) {
      if (!text.empty()) text += ", ";
      text += stage + "=" + std::to_string(ms) + "ms";
    }
    return text.empty() ? "<none>" : text;
  }

  // The Host accepts a connection on its pipe. Not a handshake: the pipe's
  // existence is all the worker needs to start from a listening Host.
  bool PipeAccepts() const {
    azookey::ipc::NamedPipeClient probe;
    return probe.Connect(pipe_name_, 100);
  }

  std::filesystem::path host_executable_;
  std::filesystem::path root_;
  std::string pipe_name_;
  std::string token_;
  std::chrono::steady_clock::time_point started_;
  std::chrono::steady_clock::time_point deadline_;
  std::vector<std::pair<std::string, long long>> stages_;
  StateLog states_;
};

TEST_F(TsfTipHostRestartReconnectTest, WorkerReconnectsToARestartedHostProcess) {
  // The worker reads the token from this variable, the same value the Host is
  // given on its command line. Without it the worker would fall back to the
  // real per-user token file.
  ScopedEnvironmentValue token("AZOOKEY_IPC_HANDSHAKE_TOKEN", token_);
  ASSERT_TRUE(token.applied());
  // Structured logs are opt-in and go to LocalAppData. Neither the worker nor
  // the Hosts started below may write there.
  ScopedEnvironmentValue logging_off("AZOOKEY_LOG", "");
  ASSERT_TRUE(logging_off.applied());

  HostProcess first;
  HostProcess second;
  ASSERT_TRUE(first.Start(host_executable_, HostArgs(), root_ / "host-1.log"));
  ASSERT_TRUE(
      WaitUntil("the first Host to listen on its pipe", [&] { return PipeAccepts(); }, &first));
  Mark("first_host_listening");

  azookey::tsf::TextService service;
  service.set_ipc_pipe_name_for_test(pipe_name_);
  service.set_ipc_health_timing_for_test(kHealthIntervalMs, kHealthTimeoutMs, kHealthMissThreshold);
  service.set_ipc_state_published_hook_for_test(&StateLog::Record, &states_);
  service.start_ipc_worker_for_test();

  // Before the kill: the worker is connected and the Host is not marked down.
  // The mark follows the published state, so it is part of the wait.
  ASSERT_TRUE(WaitUntil(
      "the worker to reach Ready on the first Host",
      [&] {
        return states_.Count(IpcConnectionState::Ready) >= 1 &&
               service.ipc_connection_state_for_test() == IpcConnectionState::Ready &&
               !service.ipc_host_unavailable_for_test();
      },
      &first));
  Mark("worker_ready");

  // The Host dies. Only once the process is gone is the worker's view of it
  // examined, so a Ready left over from before the kill cannot satisfy the
  // reconnect wait below.
  const size_t disconnected_before_kill = states_.Count(IpcConnectionState::Disconnected);
  ASSERT_TRUE(first.Kill()) << "the first Host did not end";
  Mark("host_killed");
  ASSERT_TRUE(WaitUntil("the worker to notice the lost Host",
                        [&] { return service.ipc_host_unavailable_for_test(); }));
  Mark("loss_noticed");
  EXPECT_NE(service.ipc_connection_state_for_test(), IpcConnectionState::Ready);

  // The new Host starts only after one connect attempt has failed against the
  // missing Host: two more Disconnected than before the kill, the lost
  // connection and that failed attempt. Otherwise the worker could find the
  // Host inside the attempt's 0.5 s window and the backoff retry would go
  // unexercised.
  ASSERT_TRUE(WaitUntil("the worker to fail a connect attempt against the missing Host", [&] {
    return states_.Count(IpcConnectionState::Disconnected) >= disconnected_before_kill + 2;
  }));
  Mark("connect_failed");

  // A new Host process on the same pipe name and token. Nothing tells the
  // worker; it finds the Host again through its own backoff loop.
  ASSERT_TRUE(second.Start(host_executable_, HostArgs(), root_ / "host-2.log"));
  Mark("second_host_started");
  ASSERT_TRUE(WaitUntil(
      "the worker to reach Ready on the restarted Host",
      [&] {
        return states_.Count(IpcConnectionState::Ready) >= 2 &&
               service.ipc_connection_state_for_test() == IpcConnectionState::Ready &&
               !service.ipc_host_unavailable_for_test();
      },
      &second));
  Mark("worker_ready_again");

  service.stop_ipc_worker_for_test();

  const auto states = states_.Snapshot();
  EXPECT_TRUE(ContainsInOrder(states, {IpcConnectionState::Ready, IpcConnectionState::Disconnected,
                                       IpcConnectionState::Ready}))
      << StatesToString(states);
  EXPECT_FALSE(first.Running());
  EXPECT_TRUE(second.Running()) << "the restarted Host ended during the test";
}

}  // namespace
