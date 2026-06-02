#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "azookey/core/SimpleConverter.h"
#include "azookey/host/Dispatcher.h"
#include "azookey/host/InferenceEngine.h"
#include "azookey/host/RequestScheduler.h"
#include "azookey/ipc/Messages.h"
#include "azookey/ipc/Payloads.h"
#include "azookey/learning/LearningStore.h"
#include "azookey/learning/UserDictionary.h"

namespace ipc = azookey::ipc;

namespace {

constexpr int kProtocolVersion = 1;

std::string TempPath(const char* name) {
  return (std::filesystem::temp_directory_path() / name).string();
}

void WriteMinimalGguf(const std::string& path, uint32_t version = 3) {
  std::ofstream out(path, std::ios::binary);
  out.write("GGUF", 4);
  const unsigned char bytes[4] = {
      static_cast<unsigned char>(version & 0xFF),
      static_cast<unsigned char>((version >> 8) & 0xFF),
      static_cast<unsigned char>((version >> 16) & 0xFF),
      static_cast<unsigned char>((version >> 24) & 0xFF),
  };
  out.write(reinterpret_cast<const char*>(bytes), 4);
}

}  // namespace

class DispatcherTest : public ::testing::Test {
 protected:
  DispatcherTest()
      : learning_path("azookey_dispatcher_test_learning.tsv"),
        user_dict_path("azookey_dispatcher_test_user.json"),
        store(learning_path),
        user_dict(user_dict_path),
        engine(std::make_unique<azookey::core::SimpleConverter>(), &store, {}),
        dispatcher(&engine, &scheduler, &user_dict,
                   {/*host_version=*/"0.1.0", /*protocol_version=*/kProtocolVersion}) {
    std::remove(learning_path.c_str());
    std::remove(user_dict_path.c_str());
    engine.SetUserDictionary(&user_dict);
  }

  ~DispatcherTest() override {
    std::remove(learning_path.c_str());
    std::remove(user_dict_path.c_str());
  }

  ipc::Envelope MakeReq(uint64_t id, ipc::MessageType type, const std::string& payload_json) {
    ipc::Envelope env;
    env.version = 1;
    env.request_id = id;
    env.trace_id = "trace-" + std::to_string(id);
    env.type = type;
    env.payload_json = payload_json;
    return env;
  }

  std::string learning_path;
  std::string user_dict_path;
  azookey::learning::LearningStore store;
  azookey::learning::UserDictionary user_dict;
  azookey::host::InferenceEngine engine;
  azookey::host::RequestScheduler scheduler;
  azookey::host::Dispatcher dispatcher;
};

TEST_F(DispatcherTest, Handshake) {
  ipc::HandshakeRequest req;
  req.tip_version = "0.1.0";
  req.protocol_version = kProtocolVersion;
  req.capabilities = {"cancel"};
  auto env = MakeReq(1, ipc::MessageType::Handshake, ipc::BuildHandshakeRequest(req));
  auto resp = dispatcher.Dispatch(env);
  ASSERT_TRUE(resp.has_value());
  EXPECT_EQ(resp->request_id, 1u);
  EXPECT_EQ(resp->type, ipc::MessageType::Handshake);
  auto parsed = ipc::ParseHandshakeResponse(resp->payload_json);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_TRUE(parsed->accepted);

  ipc::HandshakeRequest bad = req;
  bad.protocol_version = 999;
  auto env2 = MakeReq(2, ipc::MessageType::Handshake, ipc::BuildHandshakeRequest(bad));
  auto resp2 = dispatcher.Dispatch(env2);
  ASSERT_TRUE(resp2.has_value());
  auto parsed2 = ipc::ParseHandshakeResponse(resp2->payload_json);
  ASSERT_TRUE(parsed2.has_value());
  EXPECT_FALSE(parsed2->accepted);
}

TEST_F(DispatcherTest, HandshakeRequiresConfiguredToken) {
  azookey::host::DispatcherConfig config;
  config.host_version = "0.1.0";
  config.protocol_version = kProtocolVersion;
  config.handshake_token = "expected-token";
  azookey::host::Dispatcher token_dispatcher(&engine, &scheduler, &user_dict, config);

  ipc::HandshakeRequest req;
  req.tip_version = "0.1.0";
  req.protocol_version = kProtocolVersion;

  auto missing = token_dispatcher.Dispatch(
      MakeReq(3, ipc::MessageType::Handshake, ipc::BuildHandshakeRequest(req)));
  ASSERT_TRUE(missing.has_value());
  auto missing_payload = ipc::ParseHandshakeResponse(missing->payload_json);
  ASSERT_TRUE(missing_payload.has_value());
  EXPECT_FALSE(missing_payload->accepted);

  req.handshake_token = "wrong-token";
  auto wrong = token_dispatcher.Dispatch(
      MakeReq(4, ipc::MessageType::Handshake, ipc::BuildHandshakeRequest(req)));
  ASSERT_TRUE(wrong.has_value());
  auto wrong_payload = ipc::ParseHandshakeResponse(wrong->payload_json);
  ASSERT_TRUE(wrong_payload.has_value());
  EXPECT_FALSE(wrong_payload->accepted);

  req.handshake_token = "expected-token";
  auto matched = token_dispatcher.Dispatch(
      MakeReq(5, ipc::MessageType::Handshake, ipc::BuildHandshakeRequest(req)));
  ASSERT_TRUE(matched.has_value());
  auto matched_payload = ipc::ParseHandshakeResponse(matched->payload_json);
  ASSERT_TRUE(matched_payload.has_value());
  EXPECT_TRUE(matched_payload->accepted);
}

TEST_F(DispatcherTest, TokenConfiguredDispatcherRejectsMessagesBeforeAcceptedHandshake) {
  azookey::host::DispatcherConfig config;
  config.host_version = "0.1.0";
  config.protocol_version = kProtocolVersion;
  config.handshake_token = "expected-token";
  azookey::host::Dispatcher token_dispatcher(&engine, &scheduler, &user_dict, config);

  ipc::AddUserWordRequest add;
  add.word = "azooKey";
  add.ruby = "あずきい";
  // Unauthenticated requests receive a type-appropriate error response (ok=false)
  // rather than nullopt, so blocking clients do not hang on receive.
  auto add_before_handshake = token_dispatcher.Dispatch(
      MakeReq(6, ipc::MessageType::AddUserWord, ipc::BuildAddUserWordRequest(add)));
  ASSERT_TRUE(add_before_handshake.has_value());
  auto add_before_payload = ipc::ParseAddUserWordResponse(add_before_handshake->payload_json);
  ASSERT_TRUE(add_before_payload.has_value());
  EXPECT_FALSE(add_before_payload->ok);
  EXPECT_TRUE(user_dict.Lookup("あずきい").empty());

  ipc::HandshakeRequest req;
  req.tip_version = "0.1.0";
  req.protocol_version = kProtocolVersion;
  req.handshake_token = "wrong-token";
  auto wrong = token_dispatcher.Dispatch(
      MakeReq(7, ipc::MessageType::Handshake, ipc::BuildHandshakeRequest(req)));
  ASSERT_TRUE(wrong.has_value());
  auto wrong_payload = ipc::ParseHandshakeResponse(wrong->payload_json);
  ASSERT_TRUE(wrong_payload.has_value());
  EXPECT_FALSE(wrong_payload->accepted);
  auto add_after_wrong = token_dispatcher.Dispatch(
      MakeReq(8, ipc::MessageType::AddUserWord, ipc::BuildAddUserWordRequest(add)));
  ASSERT_TRUE(add_after_wrong.has_value());
  EXPECT_FALSE(ipc::ParseAddUserWordResponse(add_after_wrong->payload_json)->ok);

  req.handshake_token = "expected-token";
  auto matched = token_dispatcher.Dispatch(
      MakeReq(9, ipc::MessageType::Handshake, ipc::BuildHandshakeRequest(req)));
  ASSERT_TRUE(matched.has_value());
  auto matched_payload = ipc::ParseHandshakeResponse(matched->payload_json);
  ASSERT_TRUE(matched_payload.has_value());
  EXPECT_TRUE(matched_payload->accepted);

  auto add_after_handshake = token_dispatcher.Dispatch(
      MakeReq(10, ipc::MessageType::AddUserWord, ipc::BuildAddUserWordRequest(add)));
  ASSERT_TRUE(add_after_handshake.has_value());
  auto add_payload = ipc::ParseAddUserWordResponse(add_after_handshake->payload_json);
  ASSERT_TRUE(add_payload.has_value());
  EXPECT_TRUE(add_payload->ok);
  EXPECT_EQ(user_dict.Lookup("あずきい").size(), 1u);
}

TEST_F(DispatcherTest, Ping) {
  ipc::PingPayload p;
  p.nonce = 0xCAFEBABE;
  auto env = MakeReq(10, ipc::MessageType::Ping, ipc::BuildPing(p));
  auto resp = dispatcher.Dispatch(env);
  ASSERT_TRUE(resp.has_value());
  auto parsed = ipc::ParsePing(resp->payload_json);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->nonce, 0xCAFEBABEu);
  EXPECT_GT(parsed->t_ms, 0u);
}

TEST_F(DispatcherTest, QueryCandidates) {
  ipc::QueryCandidatesRequest q;
  q.reading = "にほん";
  q.left_context = "";
  q.max_candidates = 10;
  q.live = false;
  auto env = MakeReq(20, ipc::MessageType::QueryCandidates, ipc::BuildQueryCandidatesRequest(q));
  auto resp = dispatcher.Dispatch(env);
  ASSERT_TRUE(resp.has_value());
  auto parsed = ipc::ParseQueryCandidatesResponse(resp->payload_json);
  ASSERT_TRUE(parsed.has_value());
  ASSERT_FALSE(parsed->candidates.empty());
  EXPECT_EQ(parsed->candidates.front().surface, "日本");
}

TEST_F(DispatcherTest, QueryCancelBeforeReply) {
  // Pre-cancel the request id. Dispatcher must return nullopt
  // (no reply for canceled requests).
  scheduler.Cancel(30);

  ipc::QueryCandidatesRequest q;
  q.reading = "わたし";
  auto env = MakeReq(30, ipc::MessageType::QueryCandidates, ipc::BuildQueryCandidatesRequest(q));
  auto resp = dispatcher.Dispatch(env);
  EXPECT_FALSE(resp.has_value());
  EXPECT_FALSE(scheduler.IsCanceled(30));
}

TEST_F(DispatcherTest, CancelMessageNoReply) {
  ipc::CancelPayload c;
  c.target_request_id = 999;
  auto env = MakeReq(40, ipc::MessageType::Cancel, ipc::BuildCancel(c));
  auto resp = dispatcher.Dispatch(env);
  EXPECT_FALSE(resp.has_value());
  scheduler.MarkLatest(1000);
  EXPECT_FALSE(scheduler.IsCanceled(999));
}

TEST_F(DispatcherTest, CommitObservation) {
  ipc::CommitObservationRequest c;
  c.reading = "にほん";
  c.chosen = {"二本", "にほん", 0.4, "fallback"};
  c.shown = {{"日本", "にほん", 1.0, "static"}, c.chosen};
  c.timestamp_ms = 1700000000000ULL;
  auto env = MakeReq(50, ipc::MessageType::CommitObservation, ipc::BuildCommitObservationRequest(c));
  auto resp = dispatcher.Dispatch(env);
  ASSERT_TRUE(resp.has_value());
  auto parsed = ipc::ParseCommitObservationResponse(resp->payload_json);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_TRUE(parsed->ok);
}

TEST_F(DispatcherTest, AddRemoveUserWord) {
  ipc::AddUserWordRequest add;
  add.word = "azooKey";
  add.ruby = "あずきい";
  add.value = -3.0;
  auto env = MakeReq(60, ipc::MessageType::AddUserWord, ipc::BuildAddUserWordRequest(add));
  auto resp = dispatcher.Dispatch(env);
  ASSERT_TRUE(resp.has_value());
  auto parsed = ipc::ParseAddUserWordResponse(resp->payload_json);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_TRUE(parsed->ok);

  // Confirm user dict observably contains the entry.
  auto hits = user_dict.Lookup("あずきい");
  EXPECT_EQ(hits.size(), 1u);

  // Now query: the user word should be present in candidates.
  ipc::QueryCandidatesRequest q;
  q.reading = "あずきい";
  q.max_candidates = 5;
  auto qenv = MakeReq(61, ipc::MessageType::QueryCandidates, ipc::BuildQueryCandidatesRequest(q));
  auto qresp = dispatcher.Dispatch(qenv);
  ASSERT_TRUE(qresp.has_value());
  auto qparsed = ipc::ParseQueryCandidatesResponse(qresp->payload_json);
  ASSERT_TRUE(qparsed.has_value());
  bool found = false;
  for (const auto& c : qparsed->candidates) {
    if (c.surface == "azooKey") found = true;
  }
  EXPECT_TRUE(found);

  // Now remove it.
  ipc::RemoveUserWordRequest rm;
  rm.word = "azooKey";
  rm.ruby = "あずきい";
  auto renv = MakeReq(62, ipc::MessageType::RemoveUserWord, ipc::BuildRemoveUserWordRequest(rm));
  auto rresp = dispatcher.Dispatch(renv);
  ASSERT_TRUE(rresp.has_value());
  auto rparsed = ipc::ParseRemoveUserWordResponse(rresp->payload_json);
  ASSERT_TRUE(rparsed.has_value());
  EXPECT_TRUE(rparsed->ok);
  EXPECT_TRUE(user_dict.Lookup("あずきい").empty());
}

TEST_F(DispatcherTest, LoadModelAppliesRequestOptions) {
  ipc::LoadModelRequest req;
  req.path = "azookey_dispatcher_missing_zenzai.gguf";
  req.backend = "cuda";
  req.n_gpu_layers = 24;

  auto env = MakeReq(65, ipc::MessageType::LoadModel, ipc::BuildLoadModelRequest(req));
  auto resp = dispatcher.Dispatch(env);
  ASSERT_TRUE(resp.has_value());
  auto parsed = ipc::ParseLoadModelResponse(resp->payload_json);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_FALSE(parsed->ok);
  EXPECT_TRUE(parsed->error.has_value());
  EXPECT_EQ(engine.backend(), azookey::host::BackendKind::Cuda);
  EXPECT_EQ(engine.config().model_path, req.path);
  ASSERT_TRUE(engine.config().n_gpu_layers.has_value());
  EXPECT_EQ(engine.config().n_gpu_layers.value(), 24);

  auto health_env = MakeReq(66, ipc::MessageType::Health, "{}");
  auto health_resp = dispatcher.Dispatch(health_env);
  ASSERT_TRUE(health_resp.has_value());
  auto health = ipc::ParseHealth(health_resp->payload_json);
  ASSERT_TRUE(health.has_value());
  EXPECT_EQ(health->backend, "cuda");
  EXPECT_FALSE(health->model_loaded);
  EXPECT_TRUE(health->last_error.has_value());
}

TEST_F(DispatcherTest, LoadModelRejectsUnsupportedBackend) {
  ipc::LoadModelRequest req;
  req.path = "zenzai.gguf";
  req.backend = "directml";

  auto env = MakeReq(67, ipc::MessageType::LoadModel, ipc::BuildLoadModelRequest(req));
  auto resp = dispatcher.Dispatch(env);
  ASSERT_TRUE(resp.has_value());
  auto parsed = ipc::ParseLoadModelResponse(resp->payload_json);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_FALSE(parsed->ok);
  EXPECT_TRUE(parsed->error.has_value());
  EXPECT_EQ(engine.backend(), azookey::host::BackendKind::Cpu);
}

TEST_F(DispatcherTest, LoadModelValidGgufUpdatesHealthAndHandshake) {
  const std::string model_path = TempPath("azookey_dispatcher_valid_zenzai.gguf");
  std::remove(model_path.c_str());
  WriteMinimalGguf(model_path);

  ipc::LoadModelRequest req;
  req.path = model_path;
  req.backend = "cpu";

  auto env = MakeReq(68, ipc::MessageType::LoadModel, ipc::BuildLoadModelRequest(req));
  auto resp = dispatcher.Dispatch(env);
  ASSERT_TRUE(resp.has_value());
  auto parsed = ipc::ParseLoadModelResponse(resp->payload_json);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_TRUE(parsed->ok);
  EXPECT_FALSE(parsed->error.has_value());

  auto health_env = MakeReq(69, ipc::MessageType::Health, "{}");
  auto health_resp = dispatcher.Dispatch(health_env);
  ASSERT_TRUE(health_resp.has_value());
  auto health = ipc::ParseHealth(health_resp->payload_json);
  ASSERT_TRUE(health.has_value());
  EXPECT_EQ(health->status, "ok");
  EXPECT_EQ(health->backend, "cpu");
  EXPECT_TRUE(health->model_loaded);

  ipc::HandshakeRequest hreq;
  hreq.tip_version = "0.1.0";
  hreq.protocol_version = kProtocolVersion;
  auto henv = MakeReq(70, ipc::MessageType::Handshake, ipc::BuildHandshakeRequest(hreq));
  auto hresp = dispatcher.Dispatch(henv);
  ASSERT_TRUE(hresp.has_value());
  auto handshake = ipc::ParseHandshakeResponse(hresp->payload_json);
  ASSERT_TRUE(handshake.has_value());
  EXPECT_TRUE(handshake->model_loaded);

  std::remove(model_path.c_str());
}

TEST_F(DispatcherTest, LoadModelCudaFallbackKeepsHealthOk) {
  const std::string model_path = TempPath("azookey_dispatcher_cuda_fallback_zenzai.gguf");
  std::remove(model_path.c_str());
  WriteMinimalGguf(model_path);

  ipc::LoadModelRequest req;
  req.path = model_path;
  req.backend = "cuda";

  auto env = MakeReq(71, ipc::MessageType::LoadModel, ipc::BuildLoadModelRequest(req));
  auto resp = dispatcher.Dispatch(env);
  ASSERT_TRUE(resp.has_value());
  auto parsed = ipc::ParseLoadModelResponse(resp->payload_json);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_TRUE(parsed->ok);
  EXPECT_TRUE(parsed->error.has_value());

  auto health_env = MakeReq(72, ipc::MessageType::Health, "{}");
  auto health_resp = dispatcher.Dispatch(health_env);
  ASSERT_TRUE(health_resp.has_value());
  auto health = ipc::ParseHealth(health_resp->payload_json);
  ASSERT_TRUE(health.has_value());
  EXPECT_EQ(health->status, "ok");
  EXPECT_EQ(health->backend, "cpu");
  EXPECT_TRUE(health->model_loaded);
  EXPECT_FALSE(health->last_error.has_value());

  std::remove(model_path.c_str());
}

TEST_F(DispatcherTest, Health) {
  auto env = MakeReq(70, ipc::MessageType::Health, "{}");
  auto resp = dispatcher.Dispatch(env);
  ASSERT_TRUE(resp.has_value());
  auto parsed = ipc::ParseHealth(resp->payload_json);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->status, "ok");
  EXPECT_TRUE(parsed->backend == "cpu" || parsed->backend == "cuda");
}

// Verify that a token-configured Dispatcher isolates auth state per instance.
// Connection A authenticates; a separate Dispatcher (simulating connection B)
// must NOT inherit A's authenticated state.
TEST_F(DispatcherTest, CrossClientAuthIsolation) {
  azookey::host::DispatcherConfig config;
  config.host_version = "0.1.0";
  config.protocol_version = kProtocolVersion;
  config.handshake_token = "secret";

  azookey::host::Dispatcher conn_a(&engine, &scheduler, &user_dict, config);
  azookey::host::Dispatcher conn_b(&engine, &scheduler, &user_dict, config);

  // Connection A authenticates successfully.
  ipc::HandshakeRequest req;
  req.tip_version = "0.1.0";
  req.protocol_version = kProtocolVersion;
  req.handshake_token = "secret";
  auto resp_a = conn_a.Dispatch(MakeReq(1, ipc::MessageType::Handshake, ipc::BuildHandshakeRequest(req)));
  ASSERT_TRUE(resp_a.has_value());
  EXPECT_TRUE(ipc::ParseHandshakeResponse(resp_a->payload_json)->accepted);

  // Connection B has NOT performed a handshake; AddUserWord must be rejected
  // (returns ok=false rather than nullopt so the client does not hang).
  ipc::AddUserWordRequest add;
  add.word = "test";
  add.ruby = "てすと";
  auto resp_b = conn_b.Dispatch(
      MakeReq(2, ipc::MessageType::AddUserWord, ipc::BuildAddUserWordRequest(add)));
  ASSERT_TRUE(resp_b.has_value());
  EXPECT_FALSE(ipc::ParseAddUserWordResponse(resp_b->payload_json)->ok);
  EXPECT_TRUE(user_dict.Lookup("てすと").empty());

  // A failed handshake on B must not de-authenticate A.
  ipc::HandshakeRequest bad_req = req;
  bad_req.handshake_token = "wrong";
  conn_b.Dispatch(MakeReq(3, ipc::MessageType::Handshake, ipc::BuildHandshakeRequest(bad_req)));
  auto resp_a2 = conn_a.Dispatch(
      MakeReq(4, ipc::MessageType::AddUserWord, ipc::BuildAddUserWordRequest(add)));
  ASSERT_TRUE(resp_a2.has_value());
  EXPECT_TRUE(ipc::ParseAddUserWordResponse(resp_a2->payload_json)->ok);
}
