#include <gtest/gtest.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
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

std::string ReadLog(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  std::string text(8192, '\0');
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
             DWORD flags, const std::filesystem::path& log_path = {}) {
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
      startup.hStdInput = input;
      startup.hStdOutput = log;
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

  bool SendCommit(azookey::ipc::NamedPipeClient* client, uint64_t id,
                  const std::string& reading) const {
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
  const auto encrypted = azookey::learning::EncryptedPathFor(LearningPath());
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
  const auto encrypted = azookey::learning::EncryptedPathFor(LearningPath());
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

}  // namespace
