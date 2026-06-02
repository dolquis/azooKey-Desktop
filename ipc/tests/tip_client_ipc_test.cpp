// Tests the TIP-client IPC flow that mirrors StartDebugIpcProbe in TextService.cpp.
// Verifies: connect → Handshake → Ping roundtrip, and QueryCandidates roundtrip.

#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "azookey/core/SimpleConverter.h"
#include "azookey/host/Dispatcher.h"
#include "azookey/host/InferenceEngine.h"
#include "azookey/host/RequestScheduler.h"
#include "azookey/ipc/NamedPipeTransport.h"
#include "azookey/ipc/Payloads.h"
#include "azookey/learning/LearningStore.h"
#include "azookey/learning/UserDictionary.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace {

std::string TempFilePath(const char* stem) {
  const auto filename =
      std::string(stem) + "-" + std::to_string(GetCurrentProcessId()) + ".tmp";
  return (std::filesystem::temp_directory_path() / filename).string();
}

azookey::ipc::Envelope MakeEnvelope(uint64_t request_id,
                                    azookey::ipc::MessageType type,
                                    std::string payload_json,
                                    std::string trace_id) {
  azookey::ipc::Envelope env;
  env.version = 1;
  env.request_id = request_id;
  env.trace_id = std::move(trace_id);
  env.type = type;
  env.payload_json = std::move(payload_json);
  return env;
}

void ExpectAcceptedHandshake(azookey::ipc::NamedPipeClient& client) {
  azookey::ipc::HandshakeRequest handshake;
  handshake.tip_version = "0.1.0";
  handshake.protocol_version = 1;
  handshake.capabilities = {"ping", "query_candidates"};

  ASSERT_TRUE(client.Send(MakeEnvelope(
      1, azookey::ipc::MessageType::Handshake,
      azookey::ipc::BuildHandshakeRequest(handshake),
      "tip-activate-handshake")));
  auto hres = client.Receive();
  ASSERT_TRUE(hres.has_value());
  EXPECT_EQ(hres->request_id, 1u);
  auto hpayload = azookey::ipc::ParseHandshakeResponse(hres->payload_json);
  ASSERT_TRUE(hpayload.has_value());
  EXPECT_TRUE(hpayload->accepted);
}

}  // namespace

TEST(TipClientIpcTest, ActivationFlowAndQueryRoundTrip) {
  const std::string pipe_name =
      "\\\\.\\pipe\\azookey-tip-client-test-" + std::to_string(GetCurrentProcessId());

  // Mock server that mimics inference-host behaviour needed by TIP activation.
  azookey::ipc::NamedPipeServer server;
  const bool started = server.Start(
      pipe_name,
      [](const azookey::ipc::Envelope& req) -> std::optional<azookey::ipc::Envelope> {
        azookey::ipc::Envelope res;
        res.version = req.version;
        res.request_id = req.request_id;
        res.trace_id = req.trace_id;
        res.type = req.type;

        if (req.type == azookey::ipc::MessageType::Handshake) {
          auto parsed = azookey::ipc::ParseHandshakeRequest(req.payload_json);
          azookey::ipc::HandshakeResponse payload;
          payload.host_version = "mock-host-0.1.0";
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
          payload.t_ms = GetTickCount64();
          res.payload_json = azookey::ipc::BuildPing(payload);
          return res;
        }

        if (req.type == azookey::ipc::MessageType::QueryCandidates) {
          auto parsed = azookey::ipc::ParseQueryCandidatesRequest(req.payload_json);
          azookey::ipc::QueryCandidatesResponse payload;
          if (parsed) {
            azookey::ipc::CandidateField c;
            c.surface = "mock:" + parsed->reading;
            c.reading = parsed->reading;
            c.score = 1.0;
            c.source = "mock";
            payload.candidates = {c};
          }
          payload.partial = false;
          res.payload_json = azookey::ipc::BuildQueryCandidatesResponse(payload);
          return res;
        }

        return std::nullopt;
      });
  ASSERT_TRUE(started);

  // --- TIP activation flow (mirrors StartDebugIpcProbe) ---
  azookey::ipc::NamedPipeClient client;
  ASSERT_TRUE(client.Connect(pipe_name, 2000));

  // Handshake
  azookey::ipc::HandshakeRequest handshake;
  handshake.tip_version = "0.1.0";
  handshake.protocol_version = 1;
  handshake.capabilities = {"ping"};

  azookey::ipc::Envelope henv;
  henv.version = 1;
  henv.request_id = 1;
  henv.trace_id = "tip-activate-handshake";
  henv.type = azookey::ipc::MessageType::Handshake;
  henv.payload_json = azookey::ipc::BuildHandshakeRequest(handshake);

  ASSERT_TRUE(client.Send(henv));
  auto hres = client.Receive();
  ASSERT_TRUE(hres.has_value());
  EXPECT_EQ(hres->request_id, 1u);
  auto hpayload = azookey::ipc::ParseHandshakeResponse(hres->payload_json);
  ASSERT_TRUE(hpayload.has_value());
  EXPECT_TRUE(hpayload->accepted);
  EXPECT_EQ(hpayload->host_version, "mock-host-0.1.0");

  // Ping
  azookey::ipc::PingPayload ping;
  ping.nonce = 999888777;
  ping.t_ms = ping.nonce;

  azookey::ipc::Envelope penv;
  penv.version = 1;
  penv.request_id = 2;
  penv.trace_id = "tip-activate-ping";
  penv.type = azookey::ipc::MessageType::Ping;
  penv.payload_json = azookey::ipc::BuildPing(ping);

  ASSERT_TRUE(client.Send(penv));
  auto pres = client.Receive();
  ASSERT_TRUE(pres.has_value());
  EXPECT_EQ(pres->request_id, 2u);
  auto ppayload = azookey::ipc::ParsePing(pres->payload_json);
  ASSERT_TRUE(ppayload.has_value());
  EXPECT_EQ(ppayload->nonce, ping.nonce);

  // --- QueryCandidates roundtrip (M4 path) ---
  azookey::ipc::QueryCandidatesRequest qreq;
  qreq.reading = "にほんご";
  qreq.left_context = "";
  qreq.max_candidates = 5;
  qreq.live = true;

  azookey::ipc::Envelope qenv;
  qenv.version = 1;
  qenv.request_id = 3;
  qenv.trace_id = "tip-key-query";
  qenv.type = azookey::ipc::MessageType::QueryCandidates;
  qenv.payload_json = azookey::ipc::BuildQueryCandidatesRequest(qreq);

  ASSERT_TRUE(client.Send(qenv));
  auto qres = client.Receive();
  ASSERT_TRUE(qres.has_value());
  EXPECT_EQ(qres->request_id, 3u);
  auto qpayload = azookey::ipc::ParseQueryCandidatesResponse(qres->payload_json);
  ASSERT_TRUE(qpayload.has_value());
  ASSERT_FALSE(qpayload->candidates.empty());
  EXPECT_EQ(qpayload->candidates[0].reading, "にほんご");

  client.Disconnect();
  server.Stop();
}

TEST(TipClientIpcTest, QueryCandidatesRoundTripThroughHostDispatcher) {
  const std::string pipe_name =
      "\\\\.\\pipe\\azookey-tip-host-roundtrip-test-" +
      std::to_string(GetCurrentProcessId());
  const std::string learning_path = TempFilePath("azookey-tip-host-learning");
  const std::string user_dict_path = TempFilePath("azookey-tip-host-userdict");
  std::remove(learning_path.c_str());
  std::remove(user_dict_path.c_str());

  azookey::learning::LearningStore store(learning_path);
  azookey::learning::UserDictionary user_dict(user_dict_path);
  azookey::host::InferenceEngine engine(
      std::make_unique<azookey::core::SimpleConverter>(), &store, {});
  engine.SetUserDictionary(&user_dict);
  azookey::host::RequestScheduler scheduler;
  azookey::host::Dispatcher dispatcher(
      &engine, &scheduler, &user_dict,
      {/*host_version=*/"0.1.0", /*protocol_version=*/1});

  azookey::ipc::NamedPipeServer server;
  const bool started = server.Start(
      pipe_name,
      [&dispatcher](const azookey::ipc::Envelope& req)
          -> std::optional<azookey::ipc::Envelope> {
        return dispatcher.Dispatch(req);
      });
  ASSERT_TRUE(started);

  azookey::ipc::NamedPipeClient client;
  ASSERT_TRUE(client.Connect(pipe_name, 2000));
  ASSERT_NO_FATAL_FAILURE(ExpectAcceptedHandshake(client));

  azookey::ipc::QueryCandidatesRequest qreq;
  qreq.reading = u8"\u306B\u307B\u3093";
  qreq.left_context = "";
  qreq.max_candidates = 5;
  qreq.live = true;

  ASSERT_TRUE(client.Send(MakeEnvelope(
      2, azookey::ipc::MessageType::QueryCandidates,
      azookey::ipc::BuildQueryCandidatesRequest(qreq), "tip-key-query")));
  auto qres = client.Receive();
  ASSERT_TRUE(qres.has_value());
  EXPECT_EQ(qres->request_id, 2u);
  EXPECT_EQ(qres->type, azookey::ipc::MessageType::QueryCandidates);

  auto qpayload = azookey::ipc::ParseQueryCandidatesResponse(qres->payload_json);
  ASSERT_TRUE(qpayload.has_value());
  ASSERT_FALSE(qpayload->candidates.empty());
  EXPECT_EQ(qpayload->candidates.front().surface, u8"\u65E5\u672C");
  EXPECT_EQ(qpayload->candidates.front().reading, u8"\u306B\u307B\u3093");

  client.Disconnect();
  server.Stop();
  std::remove(learning_path.c_str());
  std::remove(user_dict_path.c_str());
}
#else

TEST(TipClientIpcTest, ActivationFlowAndQueryRoundTrip) {
  GTEST_SKIP() << "TIP-client IPC flow is Windows-only";
}

#endif
