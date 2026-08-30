#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "azookey/ipc/Limits.h"
#include "azookey/ipc/NamedPipeTransport.h"
#include "azookey/ipc/Payloads.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <sddl.h>

#include "../src/NamedPipeTransportInternal.h"

namespace {

int g_immediate_probe_calls = 0;

azookey::ipc::internal::ImmediateOverlappedResult CompletedImmediateProbeForTesting(HANDLE,
                                                                                    OVERLAPPED&) {
  ++g_immediate_probe_calls;
  return {TRUE, ERROR_SUCCESS, 123};
}

azookey::ipc::internal::ImmediateOverlappedResult PartialMessageProbeForTesting(HANDLE,
                                                                                OVERLAPPED&) {
  ++g_immediate_probe_calls;
  return {FALSE, ERROR_MORE_DATA, 7};
}

bool WaitForClientCount(azookey::ipc::NamedPipeServer& server, std::size_t expected,
                        int attempts = 100) {
  for (int i = 0; i < attempts; ++i) {
    if (server.ActiveClientCountForTesting() == expected) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

bool WaitForFlag(const std::atomic<bool>& flag) {
  for (int i = 0; i < 100; ++i) {
    if (flag.load()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

class ScopedEnvironmentVariable {
 public:
  ScopedEnvironmentVariable(const wchar_t* name, const wchar_t* value)
      : name_(name) {
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required > 0) {
      previous_.resize(required);
      const DWORD copied =
          GetEnvironmentVariableW(name, previous_.data(), required);
      if (copied > 0) {
        previous_.resize(copied);
        had_previous_ = true;
      }
    }
    SetEnvironmentVariableW(name_.c_str(), value);
  }

  ~ScopedEnvironmentVariable() {
    SetEnvironmentVariableW(name_.c_str(),
                            had_previous_ ? previous_.c_str() : nullptr);
  }

  ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
  ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) = delete;

 private:
  std::wstring name_;
  std::wstring previous_;
  bool had_previous_{false};
};

// Raw pipe access, so a test can send a frame header without the body that
// NamedPipeClient would always append.
class ScopedPipeHandle {
 public:
  explicit ScopedPipeHandle(HANDLE handle) : handle_(handle) {}
  ~ScopedPipeHandle() { Close(); }

  ScopedPipeHandle(const ScopedPipeHandle&) = delete;
  ScopedPipeHandle& operator=(const ScopedPipeHandle&) = delete;

  bool valid() const { return handle_ != INVALID_HANDLE_VALUE; }
  HANDLE get() const { return handle_; }

  void Close() {
    if (valid()) {
      CloseHandle(handle_);
      handle_ = INVALID_HANDLE_VALUE;
    }
  }

 private:
  HANDLE handle_;
};

HANDLE OpenRawPipeClient(const std::string& pipe_name, int attempts = 200) {
  const std::wstring wide(pipe_name.begin(), pipe_name.end());
  for (int i = 0; i < attempts; ++i) {
    HANDLE pipe = CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                              0, nullptr);
    if (pipe != INVALID_HANDLE_VALUE) {
      return pipe;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return INVALID_HANDLE_VALUE;
}

bool WriteRawPipeSync(HANDLE pipe, const uint8_t* data, DWORD size) {
  DWORD written = 0;
  return WriteFile(pipe, data, size, &written, nullptr) && written == size;
}

#ifndef NDEBUG
HANDLE CreateRawPipeServer(const std::string& pipe_name, DWORD buffer_size = 4096) {
  const std::wstring wide(pipe_name.begin(), pipe_name.end());
  return CreateNamedPipeW(wide.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                          PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1, buffer_size,
                          buffer_size, 0, nullptr);
}

bool ConnectRawPipeServer(HANDLE server, azookey::ipc::NamedPipeClient& client,
                          const std::string& pipe_name) {
  ScopedPipeHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  if (!event.valid()) return false;

  OVERLAPPED overlapped{};
  overlapped.hEvent = event.get();
  const BOOL connected_immediately = ConnectNamedPipe(server, &overlapped);
  const DWORD connect_error = connected_immediately ? ERROR_SUCCESS : GetLastError();
  if (!connected_immediately && connect_error != ERROR_IO_PENDING &&
      connect_error != ERROR_PIPE_CONNECTED) {
    return false;
  }

  if (!client.Connect(pipe_name, 2000)) {
    if (connect_error == ERROR_IO_PENDING) {
      CancelIoEx(server, &overlapped);
      DWORD transferred = 0;
      GetOverlappedResult(server, &overlapped, &transferred, TRUE);
    }
    return false;
  }

  if (connect_error == ERROR_IO_PENDING) {
    if (WaitForSingleObject(event.get(), 2000) != WAIT_OBJECT_0) {
      CancelIoEx(server, &overlapped);
      DWORD transferred = 0;
      GetOverlappedResult(server, &overlapped, &transferred, TRUE);
      return false;
    }
    DWORD transferred = 0;
    return GetOverlappedResult(server, &overlapped, &transferred, FALSE) != FALSE;
  }
  return true;
}

bool WriteRawPipe(HANDLE pipe, const uint8_t* data, DWORD size) {
  ScopedPipeHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  if (!event.valid()) return false;

  OVERLAPPED overlapped{};
  overlapped.hEvent = event.get();
  const BOOL completed = WriteFile(pipe, data, size, nullptr, &overlapped);
  const DWORD error = completed ? ERROR_SUCCESS : GetLastError();
  if (!completed && error != ERROR_IO_PENDING) return false;
  if (!completed && WaitForSingleObject(event.get(), 2000) != WAIT_OBJECT_0) {
    CancelIoEx(pipe, &overlapped);
    DWORD transferred = 0;
    GetOverlappedResult(pipe, &overlapped, &transferred, TRUE);
    return false;
  }
  DWORD transferred = 0;
  return GetOverlappedResult(pipe, &overlapped, &transferred, FALSE) != FALSE &&
         transferred == size;
}

azookey::ipc::Envelope MakePingEnvelope(uint64_t request_id, const std::string& trace_id) {
  azookey::ipc::PingPayload ping;
  ping.nonce = request_id;

  azookey::ipc::Envelope env;
  env.version = 1;
  env.request_id = request_id;
  env.trace_id = trace_id;
  env.type = azookey::ipc::MessageType::Ping;
  env.payload_json = azookey::ipc::BuildPing(ping);
  return env;
}

azookey::ipc::Envelope EchoResponse(const azookey::ipc::Envelope& req) {
  azookey::ipc::Envelope res;
  res.version = req.version;
  res.request_id = req.request_id;
  res.trace_id = req.trace_id;
  res.type = req.type;
  res.payload_json = req.payload_json;
  return res;
}

struct DelayedFrameResult {
  bool setup_ok{false};
  bool writer_ok{false};
  bool connected_after_receive{false};
  std::optional<azookey::ipc::Envelope> response;
};

DelayedFrameResult ReceiveDelayedFrameAcrossPollSlice(const std::string& pipe_name,
                                                      bool use_request_deadline) {
  DelayedFrameResult result;
  ScopedPipeHandle server(CreateRawPipeServer(pipe_name));
  if (!server.valid()) return result;

  azookey::ipc::NamedPipeClient client;
  if (!ConnectRawPipeServer(server.get(), client, pipe_name)) return result;

  auto response_envelope = MakePingEnvelope(700, "delayed-frame");
  response_envelope.payload_json = "{\"blob\":\"" + std::string(128 * 1024, 'x') + "\"}";
  const auto json = azookey::ipc::Serialize(response_envelope);
  if (!json) return result;
  const auto frame = azookey::ipc::EncodeLengthPrefixed(*json);
  if (!frame || frame->size() <= 64 * 1024) return result;

  std::atomic<bool> header_sent{false};
  std::atomic<bool> writer_ok{true};
  std::thread writer([&] {
    if (!WriteRawPipe(server.get(), frame->data(), 4)) {
      writer_ok.store(false);
      return;
    }
    header_sent.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    constexpr DWORD kFirstBodyChunk = 64 * 1024;
    if (!WriteRawPipe(server.get(), frame->data() + 4, kFirstBodyChunk)) {
      writer_ok.store(false);
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto remaining = static_cast<DWORD>(frame->size() - 4 - kFirstBodyChunk);
    if (!WriteRawPipe(server.get(), frame->data() + 4 + kFirstBodyChunk, remaining)) {
      writer_ok.store(false);
    }
  });

  const bool ready = WaitForFlag(header_sent);
  if (ready) {
    const std::optional<std::chrono::steady_clock::time_point> request_deadline =
        use_request_deadline
            ? std::optional<std::chrono::steady_clock::time_point>(
                  std::chrono::steady_clock::now() + std::chrono::milliseconds(1500))
            : std::nullopt;
    result.response = client.ReceiveWithTimeout(50, request_deadline);
    result.connected_after_receive = client.IsConnected();
  }
  client.Disconnect();
  writer.join();
  result.setup_ok = ready;
  result.writer_ok = writer_ok.load();
  return result;
}
#endif  // !NDEBUG

}  // namespace

TEST(NamedPipeTransportTest, ImmediateOverlappedHardErrorDoesNotProbeCompletion) {
  OVERLAPPED overlapped{};
  azookey::ipc::internal::PipeIoResult result;
  result.ok = FALSE;
  result.error = ERROR_BROKEN_PIPE;

  g_immediate_probe_calls = 0;
  azookey::ipc::internal::CaptureImmediateOverlappedTransfer(
      INVALID_HANDLE_VALUE, overlapped, result, CompletedImmediateProbeForTesting);

  EXPECT_EQ(g_immediate_probe_calls, 0);
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error, static_cast<DWORD>(ERROR_BROKEN_PIPE));
  EXPECT_EQ(result.transferred, 0u);
}

TEST(NamedPipeTransportTest, ImmediateOverlappedMoreDataKeepsPartialByteCount) {
  OVERLAPPED overlapped{};
  azookey::ipc::internal::PipeIoResult result;
  result.ok = FALSE;
  result.error = ERROR_MORE_DATA;

  g_immediate_probe_calls = 0;
  azookey::ipc::internal::CaptureImmediateOverlappedTransfer(INVALID_HANDLE_VALUE, overlapped,
                                                             result, PartialMessageProbeForTesting);

  EXPECT_EQ(g_immediate_probe_calls, 1);
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error, static_cast<DWORD>(ERROR_MORE_DATA));
  EXPECT_EQ(result.transferred, 7u);
}

TEST(NamedPipeTransportTest, SecurityDescriptorUsesLogonSidAndScopedRights) {
  const auto sddl = azookey::ipc::internal::BuildPipeSecuritySddl(L"S-1-5-5-123-456", false);
  EXPECT_EQ(sddl, L"D:P(A;;0x12019f;;;S-1-5-5-123-456)");
  EXPECT_EQ(sddl.find(L"GA"), std::wstring::npos);

  const auto restricted = azookey::ipc::internal::BuildPipeSecuritySddl(L"S-1-5-5-123-456", true);
  EXPECT_EQ(restricted, L"D:P(A;;0x12019f;;;S-1-5-5-123-456)(A;;0x12019f;;;WD)");
}

TEST(NamedPipeTransportTest, HandshakeAndPingRoundTrip) {
  const std::string pipe_name =
      "\\\\.\\pipe\\azookey-ipc-test-" + std::to_string(GetCurrentProcessId());

  azookey::ipc::NamedPipeServer server;
  const bool started = server.Start(
      pipe_name, [](const azookey::ipc::Envelope& req) -> std::optional<azookey::ipc::Envelope> {
        azookey::ipc::Envelope res;
        res.version = req.version;
        res.request_id = req.request_id;
        res.trace_id = req.trace_id;
        res.type = req.type;

        if (req.type == azookey::ipc::MessageType::Handshake) {
          auto parsed = azookey::ipc::ParseHandshakeRequest(req.payload_json);
          azookey::ipc::HandshakeResponse payload;
          payload.host_version = "test-host";
          payload.protocol_version = 1;
          payload.accepted = parsed && parsed->protocol_version == 1;
          payload.model_loaded = false;
          res.payload_json = azookey::ipc::BuildHandshakeResponse(payload);
          return res;
        }

        if (req.type == azookey::ipc::MessageType::Ping) {
          auto parsed = azookey::ipc::ParsePing(req.payload_json);
          azookey::ipc::PingPayload payload;
          payload.nonce = parsed ? parsed->nonce : 0;
          payload.t_ms = 123456789;
          res.payload_json = azookey::ipc::BuildPing(payload);
          return res;
        }

        return std::nullopt;
      });
  ASSERT_TRUE(started);

  azookey::ipc::NamedPipeClient client;
  ASSERT_TRUE(client.Connect(pipe_name, 2000));

  azookey::ipc::HandshakeRequest handshake;
  handshake.tip_version = "test-tip";
  handshake.protocol_version = 1;
  handshake.capabilities = {"ping"};

  azookey::ipc::Envelope henv;
  henv.version = 1;
  henv.request_id = 1;
  henv.trace_id = "transport-handshake";
  henv.type = azookey::ipc::MessageType::Handshake;
  henv.payload_json = azookey::ipc::BuildHandshakeRequest(handshake);

  ASSERT_TRUE(client.Send(henv));
  auto hres = client.Receive();
  ASSERT_TRUE(hres.has_value());
  EXPECT_EQ(hres->request_id, 1u);
  auto hpayload = azookey::ipc::ParseHandshakeResponse(hres->payload_json);
  ASSERT_TRUE(hpayload.has_value());
  EXPECT_TRUE(hpayload->accepted);
  EXPECT_EQ(hpayload->host_version, "test-host");

  azookey::ipc::PingPayload ping;
  ping.nonce = 424242;
  ping.t_ms = 1;

  azookey::ipc::Envelope penv;
  penv.version = 1;
  penv.request_id = 2;
  penv.trace_id = "transport-ping";
  penv.type = azookey::ipc::MessageType::Ping;
  penv.payload_json = azookey::ipc::BuildPing(ping);

  ASSERT_TRUE(client.Send(penv));
  auto pres = client.Receive();
  ASSERT_TRUE(pres.has_value());
  EXPECT_EQ(pres->request_id, 2u);
  auto ppayload = azookey::ipc::ParsePing(pres->payload_json);
  ASSERT_TRUE(ppayload.has_value());
  EXPECT_EQ(ppayload->nonce, 424242u);

  client.Disconnect();
  server.Stop();
}

TEST(NamedPipeTransportTest, ReceiveWithTimeoutReturnsWhenServerKeepsConnectionOpenWithoutReply) {
  const std::string pipe_name =
      "\\\\.\\pipe\\azookey-ipc-timeout-test-" + std::to_string(GetCurrentProcessId());

  std::atomic<bool> saw_request{false};
  azookey::ipc::NamedPipeServer server;
  const bool started = server.Start(
      pipe_name,
      [&saw_request](const azookey::ipc::Envelope&) -> std::optional<azookey::ipc::Envelope> {
        saw_request.store(true);
        return std::nullopt;
      });
  ASSERT_TRUE(started);

  azookey::ipc::NamedPipeClient client;
  ASSERT_TRUE(client.Connect(pipe_name, 2000));

  azookey::ipc::PingPayload ping;
  ping.nonce = 42;
  ping.t_ms = 1;

  azookey::ipc::Envelope env;
  env.version = 1;
  env.request_id = 1;
  env.trace_id = "transport-timeout";
  env.type = azookey::ipc::MessageType::Ping;
  env.payload_json = azookey::ipc::BuildPing(ping);

  ASSERT_TRUE(client.Send(env));
  ASSERT_TRUE(WaitForFlag(saw_request));
  const auto start_time = std::chrono::steady_clock::now();
  auto response = client.ReceiveWithTimeout(150);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start_time);

  EXPECT_FALSE(response.has_value());
  EXPECT_LT(elapsed, std::chrono::milliseconds(1000));
  EXPECT_TRUE(client.IsConnected());

  client.Disconnect();
  server.Stop();
}

TEST(NamedPipeTransportTest, StopReturnsWhileAcceptIsPending) {
  const std::string pipe_name =
      "\\\\.\\pipe\\azookey-ipc-stop-pending-test-" + std::to_string(GetCurrentProcessId());

  azookey::ipc::NamedPipeServer server;
  const bool started = server.Start(
      pipe_name, [](const azookey::ipc::Envelope&) -> std::optional<azookey::ipc::Envelope> {
        return std::nullopt;
      });
  ASSERT_TRUE(started);

  const auto start = std::chrono::steady_clock::now();
  server.Stop();
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);

  EXPECT_LT(elapsed, std::chrono::milliseconds(1000));
  EXPECT_FALSE(server.IsRunning());
  EXPECT_EQ(server.ActiveClientCountForTesting(), 0u);
}

TEST(NamedPipeTransportTest, SecondServerCannotClaimExistingPipeName) {
  const std::string pipe_name =
      "\\\\.\\pipe\\azookey-ipc-first-instance-test-" + std::to_string(GetCurrentProcessId());
  auto handler = [](const azookey::ipc::Envelope&) -> std::optional<azookey::ipc::Envelope> {
    return std::nullopt;
  };

  azookey::ipc::NamedPipeServer first;
  azookey::ipc::NamedPipeServer second;
  ASSERT_TRUE(first.Start(pipe_name, handler));
  EXPECT_FALSE(second.Start(pipe_name, handler));
  first.Stop();
}

TEST(NamedPipeTransportTest, MultipleClientsDisconnectAndAreCleanedUp) {
  const std::string pipe_name =
      "\\\\.\\pipe\\azookey-ipc-multi-test-" + std::to_string(GetCurrentProcessId());

  azookey::ipc::NamedPipeServer server;
  const bool started = server.Start(
      pipe_name, [](const azookey::ipc::Envelope& req) -> std::optional<azookey::ipc::Envelope> {
        if (req.type != azookey::ipc::MessageType::Ping) {
          return std::nullopt;
        }

        azookey::ipc::Envelope res;
        res.version = req.version;
        res.request_id = req.request_id;
        res.trace_id = req.trace_id;
        res.type = req.type;

        azookey::ipc::PingPayload payload;
        if (auto parsed = azookey::ipc::ParsePing(req.payload_json)) {
          payload.nonce = parsed->nonce;
        }
        payload.t_ms = 123;
        res.payload_json = azookey::ipc::BuildPing(payload);
        return res;
      });
  ASSERT_TRUE(started);

  std::vector<std::unique_ptr<azookey::ipc::NamedPipeClient>> clients;
  constexpr uint64_t kClientCount = 6;
  for (uint64_t i = 0; i < kClientCount; ++i) {
    SCOPED_TRACE(i);
    auto client = std::make_unique<azookey::ipc::NamedPipeClient>();
    ASSERT_TRUE(client->Connect(pipe_name, 2000));

    azookey::ipc::PingPayload ping;
    ping.nonce = i + 100;
    azookey::ipc::Envelope env;
    env.version = 1;
    env.request_id = i + 1;
    env.trace_id = "multi-client";
    env.type = azookey::ipc::MessageType::Ping;
    env.payload_json = azookey::ipc::BuildPing(ping);

    ASSERT_TRUE(client->Send(env));
    auto response = client->Receive();
    ASSERT_TRUE(response.has_value());
    auto payload = azookey::ipc::ParsePing(response->payload_json);
    ASSERT_TRUE(payload.has_value());
    EXPECT_EQ(payload->nonce, i + 100);
    clients.push_back(std::move(client));
  }

  ASSERT_TRUE(WaitForClientCount(server, kClientCount));
  for (auto& client : clients) {
    client->Disconnect();
  }
  EXPECT_TRUE(WaitForClientCount(server, 0));
  server.Stop();
}

TEST(NamedPipeTransportTest, ConcurrentClientsConnectDuringAcceptChurn) {
  const std::string pipe_name =
      "\\\\.\\pipe\\azookey-ipc-accept-churn-test-" + std::to_string(GetCurrentProcessId());

  azookey::ipc::NamedPipeServer server;
  const bool started = server.Start(
      pipe_name, [](const azookey::ipc::Envelope& req) -> std::optional<azookey::ipc::Envelope> {
        if (req.type != azookey::ipc::MessageType::Ping) {
          return std::nullopt;
        }

        azookey::ipc::Envelope res;
        res.version = req.version;
        res.request_id = req.request_id;
        res.trace_id = req.trace_id;
        res.type = req.type;

        azookey::ipc::PingPayload payload;
        if (auto parsed = azookey::ipc::ParsePing(req.payload_json)) {
          payload.nonce = parsed->nonce;
        }
        payload.t_ms = 456;
        res.payload_json = azookey::ipc::BuildPing(payload);
        return res;
      });
  ASSERT_TRUE(started);

  constexpr uint64_t kClientCount = 12;
  std::atomic<uint64_t> successes{0};
  std::atomic<uint64_t> failures{0};
  std::vector<std::thread> threads;
  threads.reserve(kClientCount);

  for (uint64_t i = 0; i < kClientCount; ++i) {
    threads.emplace_back([&, i] {
      azookey::ipc::NamedPipeClient client;
      if (!client.Connect(pipe_name, 2000)) {
        ++failures;
        return;
      }

      azookey::ipc::PingPayload ping;
      ping.nonce = i + 1000;
      azookey::ipc::Envelope env;
      env.version = 1;
      env.request_id = i + 1;
      env.trace_id = "accept-churn";
      env.type = azookey::ipc::MessageType::Ping;
      env.payload_json = azookey::ipc::BuildPing(ping);

      if (!client.Send(env)) {
        ++failures;
        return;
      }
      auto response = client.Receive();
      auto payload = response ? azookey::ipc::ParsePing(response->payload_json) : std::nullopt;
      if (response && payload && payload->nonce == ping.nonce) {
        ++successes;
      } else {
        ++failures;
      }
      client.Disconnect();
    });
  }

  for (auto& thread : threads) {
    thread.join();
  }

  EXPECT_EQ(successes.load(), kClientCount);
  EXPECT_EQ(failures.load(), 0u);
  EXPECT_TRUE(WaitForClientCount(server, 0, 250));
  server.Stop();
}

TEST(NamedPipeTransportTest, LargeFrameRoundTripExceedsPipeBuffer) {
  const std::string pipe_name =
      "\\\\.\\pipe\\azookey-ipc-large-test-" + std::to_string(GetCurrentProcessId());

  azookey::ipc::NamedPipeServer server;
  const bool started = server.Start(
      pipe_name, [](const azookey::ipc::Envelope& req) -> std::optional<azookey::ipc::Envelope> {
        azookey::ipc::Envelope res;
        res.version = req.version;
        res.request_id = req.request_id;
        res.trace_id = req.trace_id;
        res.type = req.type;
        res.payload_json = req.payload_json;
        return res;
      });
  ASSERT_TRUE(started);

  azookey::ipc::NamedPipeClient client;
  ASSERT_TRUE(client.Connect(pipe_name, 2000));

  constexpr std::size_t kLargePayloadBytes = 128 * 1024;
  static_assert(kLargePayloadBytes < azookey::ipc::kMaxFrameSize);
  const std::string large_payload(kLargePayloadBytes, 'x');

  azookey::ipc::Envelope env;
  env.version = 1;
  env.request_id = 99;
  env.trace_id = "large-frame";
  env.type = azookey::ipc::MessageType::Ping;
  env.payload_json = "{\"blob\":\"" + large_payload + "\"}";
  ASSERT_LT(env.payload_json.size(), azookey::ipc::kMaxFrameSize);

  ASSERT_TRUE(client.Send(env));
  auto response = client.Receive();
  ASSERT_TRUE(response.has_value());
  EXPECT_EQ(response->request_id, env.request_id);
  EXPECT_EQ(response->trace_id, env.trace_id);
  EXPECT_EQ(response->payload_json, env.payload_json);

  client.Disconnect();
  server.Stop();
}

TEST(NamedPipeTransportTest, ServerAcceptsMaximumLengthFrame) {
  const std::string pipe_name =
      "\\\\.\\pipe\\azookey-ipc-max-frame-test-" + std::to_string(GetCurrentProcessId());
  std::atomic<bool> handler_entered{false};
  azookey::ipc::NamedPipeServer server;
  ASSERT_TRUE(server.Start(
      pipe_name,
      [&handler_entered](const azookey::ipc::Envelope&) -> std::optional<azookey::ipc::Envelope> {
        handler_entered.store(true);
        return std::nullopt;
      }));

  ScopedPipeHandle client(OpenRawPipeClient(pipe_name));
  ASSERT_TRUE(client.valid());
  ASSERT_TRUE(WaitForClientCount(server, 1));
  const std::string prefix =
      "{\"version\":1,\"request_id\":1,\"trace_id\":\"max\",\"type\":\"Ping\",\"payload\":{"
      "\"padding\":\"";
  const std::string suffix = "\"}}";
  ASSERT_LT(prefix.size() + suffix.size(), azookey::ipc::kMaxFrameSize);
  const std::string json =
      prefix + std::string(azookey::ipc::kMaxFrameSize - prefix.size() - suffix.size(), 'x') +
      suffix;
  ASSERT_EQ(json.size(), azookey::ipc::kMaxFrameSize);
  const auto frame = azookey::ipc::EncodeLengthPrefixed(json);
  ASSERT_TRUE(frame.has_value());
  ASSERT_TRUE(WriteRawPipeSync(client.get(), frame->data(), static_cast<DWORD>(frame->size())));

  EXPECT_TRUE(WaitForFlag(handler_entered));
  client.Close();
  server.Stop();
}

TEST(NamedPipeTransportTest, ServerRejectsZeroLengthAndOversizedFrames) {
  const auto rejects_header = [](const std::string& pipe_name, uint32_t frame_size) {
    std::atomic<bool> handler_entered{false};
    azookey::ipc::NamedPipeServer server;
    EXPECT_TRUE(server.Start(
        pipe_name,
        [&handler_entered](const azookey::ipc::Envelope&) -> std::optional<azookey::ipc::Envelope> {
          handler_entered.store(true);
          return std::nullopt;
        }));

    ScopedPipeHandle client(OpenRawPipeClient(pipe_name));
    EXPECT_TRUE(client.valid());
    EXPECT_TRUE(WaitForClientCount(server, 1));
    const uint8_t header[4] = {
        static_cast<uint8_t>(frame_size & 0xFF),
        static_cast<uint8_t>((frame_size >> 8) & 0xFF),
        static_cast<uint8_t>((frame_size >> 16) & 0xFF),
        static_cast<uint8_t>((frame_size >> 24) & 0xFF),
    };
    EXPECT_TRUE(WriteRawPipeSync(client.get(), header, static_cast<DWORD>(sizeof(header))));
    EXPECT_TRUE(WaitForClientCount(server, 0));
    EXPECT_FALSE(handler_entered.load());
    client.Close();
    server.Stop();
  };

  const auto pid = std::to_string(GetCurrentProcessId());
  rejects_header("\\\\.\\pipe\\azookey-ipc-zero-frame-test-" + pid, 0);
  rejects_header("\\\\.\\pipe\\azookey-ipc-oversized-frame-test-" + pid,
                 static_cast<uint32_t>(azookey::ipc::kMaxFrameSize + 1));
}

TEST(NamedPipeTransportTest, ClientDisconnectDuringResponseWriteCleansUp) {
  const std::string pipe_name =
      "\\\\.\\pipe\\azookey-ipc-write-close-test-" + std::to_string(GetCurrentProcessId());

  std::atomic<bool> handler_entered{false};
  azookey::ipc::NamedPipeServer server;
  const bool started = server.Start(
      pipe_name,
      [&handler_entered](
          const azookey::ipc::Envelope& req) -> std::optional<azookey::ipc::Envelope> {
        handler_entered.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        azookey::ipc::Envelope res;
        res.version = req.version;
        res.request_id = req.request_id;
        res.trace_id = req.trace_id;
        res.type = req.type;
        res.payload_json = req.payload_json;
        return res;
      });
  ASSERT_TRUE(started);

  azookey::ipc::NamedPipeClient client;
  ASSERT_TRUE(client.Connect(pipe_name, 2000));

  azookey::ipc::PingPayload ping;
  ping.nonce = 777;

  azookey::ipc::Envelope env;
  env.version = 1;
  env.request_id = 100;
  env.trace_id = "write-close";
  env.type = azookey::ipc::MessageType::Ping;
  env.payload_json = azookey::ipc::BuildPing(ping);

  ASSERT_TRUE(client.Send(env));
  ASSERT_TRUE(WaitForFlag(handler_entered));
  client.Disconnect();

  EXPECT_TRUE(WaitForClientCount(server, 0));
  server.Stop();
}

TEST(NamedPipeTransportTest, ClientDisconnectBeforeNextFrameCleansUp) {
  const std::string pipe_name =
      "\\\\.\\pipe\\azookey-ipc-read-close-test-" + std::to_string(GetCurrentProcessId());

  azookey::ipc::NamedPipeServer server;
  const bool started = server.Start(
      pipe_name, [](const azookey::ipc::Envelope& req) -> std::optional<azookey::ipc::Envelope> {
        azookey::ipc::Envelope res;
        res.version = req.version;
        res.request_id = req.request_id;
        res.trace_id = req.trace_id;
        res.type = req.type;
        res.payload_json = req.payload_json;
        return res;
      });
  ASSERT_TRUE(started);

  azookey::ipc::NamedPipeClient client;
  ASSERT_TRUE(client.Connect(pipe_name, 2000));
  ASSERT_TRUE(WaitForClientCount(server, 1));

  client.Disconnect();

  EXPECT_TRUE(WaitForClientCount(server, 0, 250));
  server.Stop();
}

#ifndef NDEBUG
TEST(NamedPipeTransportTest, ClientLimitsPipeServerToIdentificationLevel) {
  const std::string pipe_name =
      "\\\\.\\pipe\\azookey-ipc-sqos-test-" + std::to_string(GetCurrentProcessId());
  const std::wstring wide_pipe_name(pipe_name.begin(), pipe_name.end());
  PSECURITY_DESCRIPTOR raw_descriptor = nullptr;
  ASSERT_TRUE(ConvertStringSecurityDescriptorToSecurityDescriptorW(
      L"D:P(A;;GA;;;WD)", SDDL_REVISION_1, &raw_descriptor, nullptr));
  SECURITY_ATTRIBUTES security_attributes{sizeof(SECURITY_ATTRIBUTES), raw_descriptor, FALSE};
  ScopedPipeHandle server(CreateNamedPipeW(wide_pipe_name.c_str(),
                                           PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                           PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1,
                                           4096, 4096, 0, &security_attributes));
  LocalFree(raw_descriptor);
  ASSERT_TRUE(server.valid());
  ScopedPipeHandle connected_event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  ASSERT_TRUE(connected_event.valid());

  OVERLAPPED overlapped{};
  overlapped.hEvent = connected_event.get();
  const BOOL connected_immediately = ConnectNamedPipe(server.get(), &overlapped);
  const DWORD connect_error = connected_immediately ? ERROR_SUCCESS : GetLastError();
  ASSERT_TRUE(connected_immediately || connect_error == ERROR_IO_PENDING ||
              connect_error == ERROR_PIPE_CONNECTED);

  ScopedPipeHandle client(azookey::ipc::internal::OpenPipeClientHandle(wide_pipe_name));
  ASSERT_TRUE(client.valid());
  if (!connected_immediately && connect_error == ERROR_IO_PENDING) {
    ASSERT_EQ(WaitForSingleObject(connected_event.get(), 2000), WAIT_OBJECT_0);
    DWORD transferred = 0;
    ASSERT_TRUE(GetOverlappedResult(server.get(), &overlapped, &transferred, FALSE));
  }

  SECURITY_IMPERSONATION_LEVEL level = SecurityAnonymous;
  bool queried_level = false;
  const bool impersonated = ImpersonateNamedPipeClient(server.get()) != FALSE;
  if (impersonated) {
    HANDLE raw_token = nullptr;
    if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &raw_token)) {
      ScopedPipeHandle token(raw_token);
      DWORD returned = 0;
      queried_level = GetTokenInformation(token.get(), TokenImpersonationLevel, &level,
                                          sizeof(level), &returned) != FALSE;
    }
  }
  const bool reverted = !impersonated || RevertToSelf() != FALSE;

  EXPECT_TRUE(impersonated);
  EXPECT_TRUE(queried_level);
  EXPECT_EQ(level, SecurityIdentification);
  EXPECT_TRUE(reverted);
  client.Close();
  DisconnectNamedPipe(server.get());
}

TEST(NamedPipeTransportTest, ClientHeaderOnlyFrameHonorsWholeCallTimeoutAndDisconnects) {
  const std::string pipe_name =
      "\\\\.\\pipe\\azookey-ipc-client-header-timeout-" + std::to_string(GetCurrentProcessId());
  ScopedPipeHandle server(CreateRawPipeServer(pipe_name));
  ASSERT_TRUE(server.valid());

  azookey::ipc::NamedPipeClient client;
  ASSERT_TRUE(ConnectRawPipeServer(server.get(), client, pipe_name));

  const uint8_t header[4] = {16, 0, 0, 0};
  ASSERT_TRUE(WriteRawPipe(server.get(), header, static_cast<DWORD>(sizeof(header))));

  const auto start = std::chrono::steady_clock::now();
  const auto response = client.ReceiveWithTimeout(150);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);

  EXPECT_FALSE(response.has_value());
  EXPECT_GE(elapsed, std::chrono::milliseconds(100));
  EXPECT_LT(elapsed, std::chrono::milliseconds(1000));
  EXPECT_FALSE(client.IsConnected());
}

TEST(NamedPipeTransportTest, ClientIdleTimeoutPreservesUnreadResponseAndConnection) {
  const std::string pipe_name =
      "\\\\.\\pipe\\azookey-ipc-client-idle-timeout-" + std::to_string(GetCurrentProcessId());

  std::atomic<bool> handler_entered{false};
  azookey::ipc::NamedPipeServer server;
  ASSERT_TRUE(server.Start(pipe_name,
                           [&handler_entered](const azookey::ipc::Envelope& request)
                               -> std::optional<azookey::ipc::Envelope> {
                             handler_entered.store(true);
                             std::this_thread::sleep_for(std::chrono::milliseconds(250));
                             return EchoResponse(request);
                           }));

  azookey::ipc::NamedPipeClient client;
  ASSERT_TRUE(client.Connect(pipe_name, 2000));
  ASSERT_TRUE(client.Send(MakePingEnvelope(701, "idle-timeout")));
  ASSERT_TRUE(WaitForFlag(handler_entered));

  EXPECT_FALSE(client.ReceiveWithTimeout(50).has_value());
  EXPECT_TRUE(client.IsConnected());

  const auto response = client.ReceiveWithTimeout(1000);
  ASSERT_TRUE(response.has_value());
  EXPECT_EQ(response->request_id, 701u);
  EXPECT_TRUE(client.IsConnected());

  client.Disconnect();
  server.Stop();
}

TEST(NamedPipeTransportTest, RequestAwareReceiveCompletesFrameAcrossPollSlices) {
  const std::string pipe_name =
      "\\\\.\\pipe\\azookey-ipc-client-request-deadline-" + std::to_string(GetCurrentProcessId());
  const auto result = ReceiveDelayedFrameAcrossPollSlice(pipe_name, true);

  ASSERT_TRUE(result.setup_ok);
  EXPECT_TRUE(result.writer_ok);
  ASSERT_TRUE(result.response.has_value());
  EXPECT_EQ(result.response->request_id, 700u);
  EXPECT_TRUE(result.connected_after_receive);
}

TEST(NamedPipeTransportTest, BatchReceiveWithoutRequestDeadlineIgnoresPollSliceInPhaseB) {
  const std::string pipe_name =
      "\\\\.\\pipe\\azookey-ipc-client-batch-deadline-" + std::to_string(GetCurrentProcessId());
  const auto result = ReceiveDelayedFrameAcrossPollSlice(pipe_name, false);

  ASSERT_TRUE(result.setup_ok);
  EXPECT_TRUE(result.writer_ok);
  ASSERT_TRUE(result.response.has_value());
  EXPECT_EQ(result.response->request_id, 700u);
  EXPECT_TRUE(result.connected_after_receive);
}

TEST(NamedPipeTransportTest, RequestDeadlineCutsOffPartialFrameBeforeTransportHardDeadline) {
  const std::string pipe_name =
      "\\\\.\\pipe\\azookey-ipc-client-request-cutoff-" + std::to_string(GetCurrentProcessId());
  ScopedPipeHandle server(CreateRawPipeServer(pipe_name));
  ASSERT_TRUE(server.valid());

  azookey::ipc::NamedPipeClient client;
  ASSERT_TRUE(ConnectRawPipeServer(server.get(), client, pipe_name));
  const uint8_t header[4] = {16, 0, 0, 0};
  ASSERT_TRUE(WriteRawPipe(server.get(), header, static_cast<DWORD>(sizeof(header))));

  const auto start = std::chrono::steady_clock::now();
  const auto request_deadline = start + std::chrono::milliseconds(175);
  const auto response = client.ReceiveWithTimeout(50, request_deadline);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);

  EXPECT_FALSE(response.has_value());
  EXPECT_GE(elapsed, std::chrono::milliseconds(100));
  EXPECT_LT(elapsed, std::chrono::milliseconds(1000));
  EXPECT_FALSE(client.IsConnected());
}

TEST(NamedPipeTransportTest, DisconnectCancelsInFlightClientReceive) {
  const std::string pipe_name =
      "\\\\.\\pipe\\azookey-ipc-client-disconnect-cancel-" + std::to_string(GetCurrentProcessId());
  ScopedPipeHandle server(CreateRawPipeServer(pipe_name));
  ASSERT_TRUE(server.valid());

  azookey::ipc::NamedPipeClient client;
  ASSERT_TRUE(ConnectRawPipeServer(server.get(), client, pipe_name));

  std::atomic<bool> receive_started{false};
  std::optional<azookey::ipc::Envelope> response;
  std::thread receiver([&] {
    receive_started.store(true);
    response = client.Receive();
  });
  const bool started = WaitForFlag(receive_started);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  const auto disconnect_start = std::chrono::steady_clock::now();
  client.Disconnect();
  receiver.join();
  const auto disconnect_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - disconnect_start);

  ASSERT_TRUE(started);
  EXPECT_FALSE(response.has_value());
  EXPECT_LT(disconnect_elapsed, std::chrono::milliseconds(500));
  EXPECT_FALSE(client.IsConnected());
}

TEST(NamedPipeTransportTest, ClientWriteHardDeadlineCancelsUnreadLargeFrame) {
  ScopedEnvironmentVariable hard_deadline(L"AZOOKEY_TEST_FRAME_HARD_DEADLINE_MS", L"300");
  const std::string pipe_name =
      "\\\\.\\pipe\\azookey-ipc-client-write-deadline-" + std::to_string(GetCurrentProcessId());
  ScopedPipeHandle server(CreateRawPipeServer(pipe_name));
  ASSERT_TRUE(server.valid());

  azookey::ipc::NamedPipeClient client;
  ASSERT_TRUE(ConnectRawPipeServer(server.get(), client, pipe_name));
  auto request = MakePingEnvelope(702, "write-deadline");
  request.payload_json = "{\"blob\":\"" + std::string(512 * 1024, 'x') + "\"}";

  const auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(client.Send(request));
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);

  EXPECT_GE(elapsed, std::chrono::milliseconds(200));
  EXPECT_LT(elapsed, std::chrono::milliseconds(1500));
  EXPECT_FALSE(client.IsConnected());
}

TEST(NamedPipeTransportTest, ZeroByteResponseWriteRetryIsBounded) {
  ScopedEnvironmentVariable force_zero_write(
      L"AZOOKEY_TEST_FORCE_ZERO_BYTE_PIPE_WRITE", nullptr);
  const std::string pipe_name =
      "\\\\.\\pipe\\azookey-ipc-zero-write-test-" + std::to_string(GetCurrentProcessId());

  std::atomic<bool> handler_entered{false};
  azookey::ipc::NamedPipeServer server;
  const bool started =
      server.Start(pipe_name,
                   [&handler_entered](
                       const azookey::ipc::Envelope& req) -> std::optional<azookey::ipc::Envelope> {
                     handler_entered.store(true);
                     SetEnvironmentVariableW(L"AZOOKEY_TEST_FORCE_ZERO_BYTE_PIPE_WRITE", L"1");

                     azookey::ipc::Envelope res;
                     res.version = req.version;
                     res.request_id = req.request_id;
                     res.trace_id = req.trace_id;
                     res.type = req.type;
                     res.payload_json = req.payload_json;
                     return res;
                   });
  ASSERT_TRUE(started);

  azookey::ipc::NamedPipeClient client;
  ASSERT_TRUE(client.Connect(pipe_name, 2000));

  azookey::ipc::PingPayload ping;
  ping.nonce = 888;

  azookey::ipc::Envelope env;
  env.version = 1;
  env.request_id = 101;
  env.trace_id = "zero-write";
  env.type = azookey::ipc::MessageType::Ping;
  env.payload_json = azookey::ipc::BuildPing(ping);

  ASSERT_TRUE(client.Send(env));
  ASSERT_TRUE(WaitForFlag(handler_entered));

  EXPECT_TRUE(WaitForClientCount(server, 0, 250));
  client.Disconnect();
  server.Stop();
}

TEST(NamedPipeTransportTest, StalledFrameDeadlineDropsOnlyTheStalledClient) {
  // Shortened so the test does not sit out the production budget. Kept at
  // 500ms rather than tens of ms: the override is process-wide, so a value
  // small enough to race a loaded CI runner would also trip the healthy
  // client's own frames.
  ScopedEnvironmentVariable frame_deadline(L"AZOOKEY_TEST_FRAME_HARD_DEADLINE_MS", L"500");

  const std::string pipe_name =
      "\\\\.\\pipe\\azookey-ipc-stall-test-" + std::to_string(GetCurrentProcessId());

  azookey::ipc::NamedPipeServer server;
  const bool started = server.Start(
      pipe_name, [](const azookey::ipc::Envelope& req) -> std::optional<azookey::ipc::Envelope> {
        return EchoResponse(req);
      });
  ASSERT_TRUE(started);

  // Announce a 16-byte body and then go silent, holding the connection open.
  ScopedPipeHandle stalled(OpenRawPipeClient(pipe_name));
  ASSERT_TRUE(stalled.valid());
  const uint8_t header[4] = {16, 0, 0, 0};
  DWORD written = 0;
  ASSERT_TRUE(
      WriteFile(stalled.get(), header, static_cast<DWORD>(sizeof(header)), &written, nullptr));
  ASSERT_EQ(written, static_cast<DWORD>(sizeof(header)));
  ASSERT_TRUE(WaitForClientCount(server, 1));

  // A second client is served normally while the first one is stalled.
  azookey::ipc::NamedPipeClient client;
  ASSERT_TRUE(client.Connect(pipe_name, 2000));
  ASSERT_TRUE(client.Send(MakePingEnvelope(7, "stall")));
  const auto reply = client.ReceiveWithTimeout(2000);
  ASSERT_TRUE(reply.has_value());
  EXPECT_EQ(reply->request_id, 7u);

  // The stalled connection is reaped on its own; the healthy one is untouched.
  EXPECT_TRUE(WaitForClientCount(server, 1, 500));
  EXPECT_TRUE(client.IsConnected());

  client.Disconnect();
  EXPECT_TRUE(WaitForClientCount(server, 0, 500));
  server.Stop();
}

TEST(NamedPipeTransportTest, SoftFrameDeadlineIsCountedWithoutDroppingTheFrame) {
  ScopedEnvironmentVariable soft_deadline(L"AZOOKEY_TEST_FRAME_SOFT_DEADLINE_MS", L"1");
  ScopedEnvironmentVariable hard_deadline(L"AZOOKEY_TEST_FRAME_HARD_DEADLINE_MS", L"10000");

  const std::string pipe_name =
      "\\\\.\\pipe\\azookey-ipc-soft-deadline-test-" + std::to_string(GetCurrentProcessId());

  std::atomic<bool> handler_entered{false};
  azookey::ipc::NamedPipeServer server;
  const bool started =
      server.Start(pipe_name,
                   [&handler_entered](
                       const azookey::ipc::Envelope& req) -> std::optional<azookey::ipc::Envelope> {
                     handler_entered.store(true);
                     return EchoResponse(req);
                   });
  ASSERT_TRUE(started);

  const auto json = azookey::ipc::Serialize(MakePingEnvelope(9, "soft-deadline"));
  ASSERT_TRUE(json.has_value());
  const auto frame = azookey::ipc::EncodeLengthPrefixed(*json);
  ASSERT_TRUE(frame.has_value());
  ASSERT_GT(frame->size(), 4u);

  ScopedPipeHandle slow(OpenRawPipeClient(pipe_name));
  ASSERT_TRUE(slow.valid());

  // Header, a pause well past the soft threshold and well short of the hard
  // one, then the body.
  DWORD written = 0;
  ASSERT_TRUE(WriteFile(slow.get(), frame->data(), 4, &written, nullptr));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  ASSERT_TRUE(WriteFile(slow.get(), frame->data() + 4, static_cast<DWORD>(frame->size() - 4),
                        &written, nullptr));

  // Exceeding the soft deadline records the slowness but still delivers.
  ASSERT_TRUE(WaitForFlag(handler_entered));
  EXPECT_GE(server.SoftDeadlineExceededCount(), 1u);

  slow.Close();
  EXPECT_TRUE(WaitForClientCount(server, 0, 500));
  server.Stop();
}
#endif

#else

TEST(NamedPipeTransportTest, HandshakeAndPingRoundTrip) {
  GTEST_SKIP() << "NamedPipeTransport is Windows-only";
}

#endif
