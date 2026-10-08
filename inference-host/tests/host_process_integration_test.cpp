#include <gtest/gtest.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "azookey/core/PlatformPaths.h"
#include "azookey/ipc/NamedPipeTransport.h"
#include "azookey/ipc/Payloads.h"
#include "azookey/learning/DpapiCrypto.h"
#include "azookey/learning/LearningStore.h"
#include "azookey/learning/UserDictionary.h"

namespace {

constexpr DWORD kProcessTimeoutMs = 15000;
std::atomic<HANDLE> g_control_received{nullptr};

// Returns the last 8 KiB, where the error that ended the process is logged.
std::string ReadLog(const std::filesystem::path& path) {
  constexpr std::streamoff kTailBytes = 8192;
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  const std::streamoff size = input ? static_cast<std::streamoff>(input.tellg()) : 0;
  input.seekg(std::max<std::streamoff>(0, size - kTailBytes));
  std::string text(static_cast<size_t>(std::min(size, kTailBytes)), '\0');
  input.read(text.data(), static_cast<std::streamsize>(text.size()));
  text.resize(static_cast<size_t>(input.gcount()));
  return text;
}

BOOL WINAPI IgnoreTestConsoleControl(DWORD event) {
  if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) return FALSE;
  if (const HANDLE received = g_control_received.load(std::memory_order_acquire)) {
    SetEvent(received);
  }
  return TRUE;
}

class ChildProcess {
 public:
  ChildProcess() = default;
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  ~ChildProcess() {
    if (process_ != nullptr) {
      if (WaitForSingleObject(process_, 0) == WAIT_TIMEOUT) {
        TerminateProcess(process_, 1);  // Only a child created by this test.
        WaitForSingleObject(process_, kProcessTimeoutMs);
      }
      CloseHandle(process_);
    }
    if (thread_ != nullptr) CloseHandle(thread_);
  }

  bool Start(const std::filesystem::path& executable, const std::vector<std::wstring>& args,
             DWORD flags, const std::filesystem::path& log_path = {},
             HANDLE stdin_override = nullptr, HANDLE stdout_override = nullptr) {
    std::wstring command = L"\"" + executable.wstring() + L"\"";
    for (const auto& arg : args) command += L" \"" + arg + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    HANDLE log = nullptr;
    HANDLE input = nullptr;
    if (!log_path.empty()) {
      SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
      log = CreateFileW(log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        &attributes, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (log == INVALID_HANDLE_VALUE || input == INVALID_HANDLE_VALUE) {
        if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
        if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
        return false;
      }
      startup.dwFlags |= STARTF_USESTDHANDLES;
      startup.hStdInput = stdin_override != nullptr ? stdin_override : input;
      startup.hStdOutput = stdout_override != nullptr ? stdout_override : log;
      startup.hStdError = log;
    }
    PROCESS_INFORMATION info{};
    const bool created =
        CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, !log_path.empty(),
                       flags, nullptr, nullptr, &startup, &info);
    if (log != nullptr) CloseHandle(log);
    if (input != nullptr) CloseHandle(input);
    if (!created) {
      return false;
    }
    process_ = info.hProcess;
    thread_ = info.hThread;
    pid_ = info.dwProcessId;
    return true;
  }

  bool Resume() { return thread_ != nullptr && ResumeThread(thread_) != DWORD(-1); }
  DWORD pid() const { return pid_; }
  DWORD Wait() const { return WaitForSingleObject(process_, kProcessTimeoutMs); }
  DWORD ExitCode() const {
    DWORD code = STILL_ACTIVE;
    return GetExitCodeProcess(process_, &code) ? code : STILL_ACTIVE;
  }
  bool Running() const { return WaitForSingleObject(process_, 0) == WAIT_TIMEOUT; }

 private:
  HANDLE process_ = nullptr;
  HANDLE thread_ = nullptr;
  DWORD pid_ = 0;
};

class TestDirectory {
 public:
  TestDirectory() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    root = std::filesystem::temp_directory_path() /
           ("azookey_host_process_" + std::to_string(GetCurrentProcessId()) + "_" +
            std::to_string(stamp));
    std::filesystem::create_directories(root / "data");
  }
  TestDirectory(const TestDirectory&) = delete;
  TestDirectory& operator=(const TestDirectory&) = delete;
  ~TestDirectory() {
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }

  std::filesystem::path root;
};

class HostProcessTest : public ::testing::Test {
 protected:
  void SetUp() override {
    host_executable_ = azookey::core::Utf8Path(HOST_PROCESS_TEST_EXE);
    ASSERT_TRUE(std::filesystem::exists(host_executable_));
    const auto suffix = std::to_string(GetCurrentProcessId()) + "-" +
                        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    pipe_name_ = "\\\\.\\pipe\\azookey-host-process-" + suffix;
    token_ = "host-process-token-" + suffix;
  }

  void TearDown() override {
    if (HasFailure()) std::cerr << "Host log:\n" << ReadLog(directory_.root / "host.log");
  }

  bool StartHost() {
    const auto args = CommonArgs();
    // The Ctrl+C ignore attribute is inherited, including across a new console.
    if (!SetConsoleCtrlHandler(nullptr, FALSE)) return false;
    if (!host_.Start(host_executable_, args, CREATE_NEW_CONSOLE, directory_.root / "host.log"))
      return false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
    while (std::chrono::steady_clock::now() < deadline) {
      azookey::ipc::NamedPipeClient probe;
      if (probe.Connect(pipe_name_, 100)) return true;
      if (!host_.Running()) return false;
      Sleep(25);
    }
    return false;
  }

  std::vector<std::wstring> CommonArgs() const {
    return {L"--data-root",       directory_.root.wstring(),
            L"--learning",        LearningPath().wstring(),
            L"--user-dict",       UserDictPath().wstring(),
            L"--pipe-name",       azookey::core::Utf8Path(pipe_name_).wstring(),
            L"--handshake-token", azookey::core::Utf8Path(token_).wstring()};
  }

  std::filesystem::path LearningPath() const { return directory_.root / "data" / "learning.tsv"; }
  std::filesystem::path UserDictPath() const { return directory_.root / "data" / "user_dict.json"; }

  bool StopHost(DWORD event) {
    // The Host owns a separate console. Attach only for the signal, so no other
    // CTest or user process receives a console control event.
    const HANDLE received = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (received == nullptr) return false;
    FreeConsole();
    if (!AttachConsole(host_.pid())) {
      CloseHandle(received);
      return false;
    }
    g_control_received.store(received, std::memory_order_release);
    if (!SetConsoleCtrlHandler(IgnoreTestConsoleControl, TRUE)) {
      g_control_received.store(nullptr, std::memory_order_release);
      FreeConsole();
      CloseHandle(received);
      return false;
    }
    const bool sent = GenerateConsoleCtrlEvent(event, 0) != 0;
    const bool delivered =
        sent && WaitForSingleObject(received, kProcessTimeoutMs) == WAIT_OBJECT_0;
    const bool stopped = delivered && host_.Wait() == WAIT_OBJECT_0 && host_.ExitCode() == 0;
    // FreeConsole resets this process's handler table after delivery is known.
    FreeConsole();
    g_control_received.store(nullptr, std::memory_order_release);
    CloseHandle(received);
    return stopped;
  }

  azookey::ipc::Envelope CommitRequest(uint64_t id, const std::string& reading) const {
    azookey::ipc::CommitObservationRequest commit;
    commit.reading = reading;
    commit.chosen = {"chosen-" + reading, reading, 0.5, "fallback"};
    commit.shown = {commit.chosen};
    commit.secure = false;
    commit.learning_allowed = true;
    commit.observation_id = "host-process-" + std::to_string(id);
    commit.timestamp_ms = 1700000000000ULL;
    azookey::ipc::Envelope request;
    request.version = 1;
    request.request_id = id;
    request.trace_id = "host-process";
    request.type = azookey::ipc::MessageType::CommitObservation;
    request.payload_json = azookey::ipc::BuildCommitObservationRequest(commit);
    return request;
  }

  bool SendCommit(azookey::ipc::NamedPipeClient* client, uint64_t id,
                  const std::string& reading) const {
    const auto request = CommitRequest(id, reading);
    if (!client->Send(request)) return false;
    const auto response = client->ReceiveWithTimeout(2000);
    if (!response) return false;
    const auto parsed = azookey::ipc::ParseCommitObservationResponse(response->payload_json);
    return parsed && parsed->ok;
  }

  bool ConnectLearningClient(azookey::ipc::NamedPipeClient* client) const {
    if (!client->Connect(pipe_name_, 1000)) return false;
    azookey::ipc::HandshakeRequest handshake;
    handshake.tip_version = "host-process-test";
    handshake.client_id = "host-process-test";
    handshake.capabilities = {"secure_flag"};
    handshake.handshake_token = token_;
    azookey::ipc::Envelope request;
    request.version = 1;
    request.request_id = 1;
    request.trace_id = "host-process";
    request.type = azookey::ipc::MessageType::Handshake;
    request.payload_json = azookey::ipc::BuildHandshakeRequest(handshake);
    if (!client->Send(request)) return false;
    const auto response = client->ReceiveWithTimeout(2000);
    if (!response) return false;
    const auto parsed = azookey::ipc::ParseHandshakeResponse(response->payload_json);
    return parsed && parsed->accepted;
  }

  bool ContainsLearning(const std::string& reading) const {
    azookey::learning::LearningStore store(LearningPath());
    if (!store.Load()) {
      std::cerr << "learning load failed: " << LearningPath() << std::endl;
      return false;
    }
    for (const auto& entry : store.All()) {
      if (entry.reading == reading && entry.surface == "chosen-" + reading) return true;
    }
    std::cerr << "learning missing " << reading << ", entries=" << store.size() << std::endl;
    if (!host_.Running()) {
      std::ifstream log(directory_.root / "host.log");
      std::cerr << log.rdbuf();
    }
    return false;
  }

  void CheckBlockedStdioWrite(std::optional<DWORD> control_event) {
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
    HANDLE input_read = nullptr;
    HANDLE input_write = nullptr;
    HANDLE output_read = nullptr;
    HANDLE output_write = nullptr;
    ASSERT_TRUE(CreatePipe(&input_read, &input_write, &attributes, 128 * 1024));
    std::unique_ptr<void, decltype(&CloseHandle)> input_reader(input_read, &CloseHandle);
    std::unique_ptr<void, decltype(&CloseHandle)> input_writer(input_write, &CloseHandle);
    ASSERT_TRUE(CreatePipe(&output_read, &output_write, &attributes, 4096));
    std::unique_ptr<void, decltype(&CloseHandle)> output_reader(output_read, &CloseHandle);
    std::unique_ptr<void, decltype(&CloseHandle)> output_writer(output_write, &CloseHandle);
    ASSERT_TRUE(SetHandleInformation(input_write, HANDLE_FLAG_INHERIT, 0));
    ASSERT_TRUE(SetHandleInformation(output_read, HANDLE_FLAG_INHERIT, 0));
    DWORD output_capacity = 0;
    DWORD input_capacity = 0;
    ASSERT_TRUE(GetNamedPipeInfo(output_write, nullptr, &output_capacity, nullptr, nullptr));
    ASSERT_TRUE(GetNamedPipeInfo(input_write, nullptr, &input_capacity, nullptr, nullptr));
    ASSERT_TRUE(SetConsoleCtrlHandler(nullptr, FALSE));
    const std::vector<std::wstring> args = {L"--data-root", directory_.root.wstring(),
                                            L"--learning",  LearningPath().wstring(),
                                            L"--user-dict", UserDictPath().wstring()};
    const auto shutdown_host = azookey::core::Utf8Path(HOST_SHUTDOWN_TEST_EXE);
    ASSERT_TRUE(std::filesystem::exists(shutdown_host));
    const bool started = host_.Start(shutdown_host, args, CREATE_NEW_CONSOLE,
                                     directory_.root / "host.log", input_read, output_write);
    input_reader.reset();
    output_writer.reset();
    ASSERT_TRUE(started);

    const auto send = [&](const azookey::ipc::Envelope& request) {
      const auto serialized = azookey::ipc::Serialize(request);
      if (!serialized) return false;
      const auto line = *serialized + '\n';
      // The entire request fits in stdin's buffer even if Host has not read it.
      if (line.size() > input_capacity) return false;
      DWORD written = 0;
      return WriteFile(input_write, line.data(), static_cast<DWORD>(line.size()), &written,
                       nullptr) &&
             written == line.size();
    };
    const auto read_response = [&]() -> std::optional<azookey::ipc::Envelope> {
      std::string line;
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
      while (std::chrono::steady_clock::now() < deadline && host_.Running()) {
        DWORD available = 0;
        if (!PeekNamedPipe(output_read, nullptr, 0, nullptr, &available, nullptr)) return {};
        if (available == 0) {
          Sleep(10);
          continue;
        }
        char ch = 0;
        DWORD read = 0;
        if (!ReadFile(output_read, &ch, 1, &read, nullptr) || read != 1) return {};
        if (ch == '\n') return azookey::ipc::Deserialize(line);
        line += ch;
      }
      return {};
    };
    azookey::ipc::HandshakeRequest handshake;
    handshake.tip_version = "host-process-test";
    handshake.client_id = "host-process-test";
    handshake.capabilities = {"secure_flag"};
    azookey::ipc::Envelope handshake_request;
    handshake_request.request_id = 1;
    handshake_request.trace_id = "host-process";
    handshake_request.type = azookey::ipc::MessageType::Handshake;
    handshake_request.payload_json = azookey::ipc::BuildHandshakeRequest(handshake);
    ASSERT_TRUE(send(handshake_request));
    const auto handshake_response = read_response();
    ASSERT_TRUE(handshake_response);
    const auto accepted = azookey::ipc::ParseHandshakeResponse(handshake_response->payload_json);
    ASSERT_TRUE(accepted && accepted->accepted);

    // The first observation is saved immediately, even with a long interval.
    const auto first_observation_at = std::chrono::steady_clock::now();
    ASSERT_TRUE(send(CommitRequest(2, "first-stdio")));
    const auto first_response = read_response();
    ASSERT_TRUE(first_response);
    const auto first_commit =
        azookey::ipc::ParseCommitObservationResponse(first_response->payload_json);
    ASSERT_TRUE(first_commit && first_commit->ok);
    const auto encrypted = azookey::learning::EncryptedPathFor(
        azookey::learning::LearningStoreV2PathFor(LearningPath()));
    ASSERT_TRUE(std::filesystem::exists(encrypted));
    ASSERT_TRUE(ContainsLearning("first-stdio"));
    const auto first_write = std::filesystem::last_write_time(encrypted);
    const std::string reading = "pending-stdio";
    auto pending = CommitRequest(3, reading);
    // The response echoes trace_id. Its serialized frame cannot fit in stdout,
    // so seeing a response prefix proves Dispatch completed before we stop.
    pending.trace_id.assign(64 * 1024, 'x');
    ASSERT_GT(pending.trace_id.size(), output_capacity);
    ASSERT_TRUE(send(pending));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
    DWORD available = 0;
    do {
      ASSERT_TRUE(PeekNamedPipe(output_read, nullptr, 0, nullptr, &available, nullptr));
      ASSERT_TRUE(host_.Running()) << "Host exited before writing the response";
      ASSERT_LT(std::chrono::steady_clock::now(), deadline) << "Host did not write stdio";
      if (available == 0) Sleep(10);
    } while (available == 0);
    ASSERT_LT(available, pending.trace_id.size());
    ASSERT_EQ(std::filesystem::last_write_time(encrypted), first_write);
    ASSERT_FALSE(ContainsLearning(reading));
    // Only one observation is dirty (below the count threshold). Both the
    // periodic worker and mutation-triggered interval flush use this deadline.
    const auto flush_interval = std::chrono::seconds(AZOOKEY_HOST_PROCESS_TEST_FLUSH_INTERVAL_SEC);
    ASSERT_LT(std::chrono::steady_clock::now() - first_observation_at + std::chrono::seconds(30),
              flush_interval);
    if (control_event) {
      // Keep both stdin and unread stdout open through shutdown; EOF or a broken
      // pipe must not be responsible for releasing the blocked Host.
      ASSERT_TRUE(StopHost(*control_event));
    } else {
      // Break stdout while stdin remains open: the failed response write must
      // leave the loop without requiring EOF or a console control event.
      output_reader.reset();
      ASSERT_EQ(host_.Wait(), WAIT_OBJECT_0);
      ASSERT_EQ(host_.ExitCode(), 0U);
    }
    ASSERT_LT(std::chrono::steady_clock::now() - first_observation_at, flush_interval);
    EXPECT_TRUE(ContainsLearning(reading));
    EXPECT_EQ(ReadLog(directory_.root / "host.log").find("failed to save learning store"),
              std::string::npos);
  }

  TestDirectory directory_;
  ChildProcess host_;
  std::filesystem::path host_executable_;
  std::string pipe_name_;
  std::string token_;
};

TEST_F(HostProcessTest, ConcurrentIpcAndOfflineUserDictAddsPreserveAllEntries) {
  ASSERT_TRUE(StartHost()) << "Host failed to listen (exit=" << host_.ExitCode() << ")";
  constexpr int kPairs = 8;
  std::vector<std::unique_ptr<ChildProcess>> children;
  for (int i = 0; i < kPairs; ++i) {
    for (const bool offline : {false, true}) {
      auto args = CommonArgs();
      const std::string stem = offline ? "offline-" : "ipc-";
      const auto reading = stem + std::to_string(i);
      args.insert(args.end(), {L"userdict", L"add"});
      if (offline) args.push_back(L"--offline");
      args.insert(args.end(), {L"--reading", azookey::core::Utf8Path(reading).wstring(),
                               L"--surface", azookey::core::Utf8Path(reading).wstring()});
      auto child = std::make_unique<ChildProcess>();
      const auto log = directory_.root / ("cli-" + std::to_string(children.size()) + ".log");
      ASSERT_TRUE(child->Start(host_executable_, args, CREATE_NO_WINDOW | CREATE_SUSPENDED, log));
      children.push_back(std::move(child));
    }
  }
  for (const auto& child : children) ASSERT_TRUE(child->Resume());
  for (size_t i = 0; i < children.size(); ++i) {
    const auto& child = children[i];
    const auto log = directory_.root / ("cli-" + std::to_string(i) + ".log");
    const char* route = i % 2 == 0 ? "ipc" : "offline";
    ASSERT_EQ(child->Wait(), WAIT_OBJECT_0)
        << "CLI timed out: index=" << i << " via=" << route << " pid=" << child->pid() << '\n'
        << ReadLog(log);
    EXPECT_EQ(child->ExitCode(), 0U)
        << "CLI index=" << i << " via=" << route << " pid=" << child->pid() << '\n'
        << ReadLog(log);
  }
  azookey::learning::UserDictionary dictionary(UserDictPath());
  ASSERT_TRUE(dictionary.Load());
  EXPECT_EQ(dictionary.Size(), 2U * kPairs);
  for (int i = 0; i < kPairs; ++i) {
    for (const std::string stem : {"ipc-", "offline-"}) {
      const auto reading = stem + std::to_string(i);
      const auto matches = dictionary.Lookup(reading);
      ASSERT_EQ(matches.size(), 1U) << reading;
      EXPECT_EQ(matches.front().word, reading);
    }
  }
  EXPECT_TRUE(StopHost(CTRL_BREAK_EVENT));
}

TEST_F(HostProcessTest, CtrlCFlushesPendingLearning) {
  // Simulate a CTest launcher that passes the inheritable Ctrl+C ignore flag.
  ASSERT_TRUE(SetConsoleCtrlHandler(nullptr, TRUE));
  ASSERT_TRUE(StartHost()) << "Host failed to listen (exit=" << host_.ExitCode() << ")";
  azookey::ipc::NamedPipeClient client;
  ASSERT_TRUE(ConnectLearningClient(&client));
  ASSERT_TRUE(SendCommit(&client, 2, "first-c"));
  const auto encrypted = azookey::learning::EncryptedPathFor(
      azookey::learning::LearningStoreV2PathFor(LearningPath()));
  ASSERT_TRUE(std::filesystem::exists(encrypted));
  const auto first_write = std::filesystem::last_write_time(encrypted);
  Sleep(250);
  ASSERT_TRUE(SendCommit(&client, 3, "pending-c"));
  ASSERT_EQ(std::filesystem::last_write_time(encrypted), first_write)
      << "observation flushed before control event";
  client.Disconnect();
  ASSERT_TRUE(StopHost(CTRL_C_EVENT));
  EXPECT_TRUE(ContainsLearning("pending-c"));
}

TEST_F(HostProcessTest, CtrlBreakFlushesPendingLearning) {
  ASSERT_TRUE(StartHost()) << "Host failed to listen (exit=" << host_.ExitCode() << ")";
  azookey::ipc::NamedPipeClient client;
  ASSERT_TRUE(ConnectLearningClient(&client));
  ASSERT_TRUE(SendCommit(&client, 2, "first-break"));
  const auto encrypted = azookey::learning::EncryptedPathFor(
      azookey::learning::LearningStoreV2PathFor(LearningPath()));
  ASSERT_TRUE(std::filesystem::exists(encrypted));
  const auto first_write = std::filesystem::last_write_time(encrypted);
  Sleep(250);
  ASSERT_TRUE(SendCommit(&client, 3, "pending-break"));
  ASSERT_EQ(std::filesystem::last_write_time(encrypted), first_write)
      << "observation flushed before control event";
  client.Disconnect();
  ASSERT_TRUE(StopHost(CTRL_BREAK_EVENT));
  EXPECT_TRUE(ContainsLearning("pending-break"));
}

TEST_F(HostProcessTest, CtrlBreakUnblocksStdioRead) {
  // Keep stdin open so only the control event can end the Host's blocking read.
  SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
  HANDLE read_end = nullptr;
  HANDLE write_end = nullptr;
  ASSERT_TRUE(CreatePipe(&read_end, &write_end, &attributes, 0));
  std::unique_ptr<void, decltype(&CloseHandle)> reader(read_end, &CloseHandle);
  std::unique_ptr<void, decltype(&CloseHandle)> writer(write_end, &CloseHandle);
  ASSERT_TRUE(SetHandleInformation(write_end, HANDLE_FLAG_INHERIT, 0));
  ASSERT_TRUE(SetConsoleCtrlHandler(nullptr, FALSE));
  const std::vector<std::wstring> args = {L"--data-root", directory_.root.wstring(),
                                          L"--learning",  LearningPath().wstring(),
                                          L"--user-dict", UserDictPath().wstring()};
  const auto log = directory_.root / "host.log";
  const bool started = host_.Start(host_executable_, args, CREATE_NEW_CONSOLE, log, read_end);
  reader.reset();
  ASSERT_TRUE(started);
  // The Host reports a malformed line only from the stdio loop, after the
  // console control handler is registered.
  constexpr char kMalformedLine[] = "not-an-envelope\n";
  DWORD written = 0;
  ASSERT_TRUE(WriteFile(write_end, kMalformedLine, sizeof(kMalformedLine) - 1, &written, nullptr));
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
  while (ReadLog(log).find("warn: failed to parse envelope") == std::string::npos) {
    ASSERT_TRUE(host_.Running()) << "Host exited before reading stdio";
    ASSERT_LT(std::chrono::steady_clock::now(), deadline) << "Host did not read stdio";
    Sleep(25);
  }
  // Let main return to the blocking read before the control event.
  Sleep(250);
  EXPECT_TRUE(StopHost(CTRL_BREAK_EVENT));
}

TEST_F(HostProcessTest, CtrlCUnblocksStdioWriteAndFlushesPendingLearning) {
  CheckBlockedStdioWrite(CTRL_C_EVENT);
}

TEST_F(HostProcessTest, CtrlBreakUnblocksStdioWriteAndFlushesPendingLearning) {
  CheckBlockedStdioWrite(CTRL_BREAK_EVENT);
}

TEST_F(HostProcessTest, BrokenStdoutFlushesPendingLearning) {
  CheckBlockedStdioWrite(std::nullopt);
}

}  // namespace
