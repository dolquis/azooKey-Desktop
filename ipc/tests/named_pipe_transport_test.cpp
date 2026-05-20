#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "azookey/ipc/NamedPipeTransport.h"
#include "azookey/ipc/Payloads.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

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

#else

TEST(NamedPipeTransportTest, HandshakeAndPingRoundTrip) {
  GTEST_SKIP() << "NamedPipeTransport is Windows-only";
}

#endif
