#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "azookey/ipc/Limits.h"
#include "azookey/ipc/NamedPipeTransport.h"
#include "azookey/ipc/Payloads.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace {

bool WaitForClientCount(azookey::ipc::NamedPipeServer& server, std::size_t expected) {
  for (int i = 0; i < 100; ++i) {
    if (server.ActiveClientCountForTesting() == expected) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

}  // namespace

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

#else

TEST(NamedPipeTransportTest, HandshakeAndPingRoundTrip) {
  GTEST_SKIP() << "NamedPipeTransport is Windows-only";
}

#endif
