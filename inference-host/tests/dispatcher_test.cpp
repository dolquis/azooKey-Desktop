#include <cstdio>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

#include "azookey/core/SimpleConverter.h"
#include "azookey/host/Dispatcher.h"
#include "azookey/host/InferenceEngine.h"
#include "azookey/host/RequestScheduler.h"
#include "azookey/ipc/Messages.h"
#include "azookey/ipc/Payloads.h"
#include "azookey/learning/LearningStore.h"
#include "azookey/learning/UserDictionary.h"

static void Expect(bool cond, const char* msg) {
  if (!cond) throw std::runtime_error(msg);
}

namespace ipc = azookey::ipc;

namespace {

constexpr int kProtocolVersion = 1;

struct Fixture {
  std::string learning_path;
  std::string user_dict_path;
  azookey::learning::LearningStore store;
  azookey::learning::UserDictionary user_dict;
  azookey::host::InferenceEngine engine;
  azookey::host::RequestScheduler scheduler;
  azookey::host::Dispatcher dispatcher;

  Fixture()
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

  ~Fixture() {
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
};

}  // namespace

static void TestHandshake() {
  Fixture f;
  ipc::HandshakeRequest req;
  req.tip_version = "0.1.0";
  req.protocol_version = kProtocolVersion;
  req.capabilities = {"cancel"};
  auto env = f.MakeReq(1, ipc::MessageType::Handshake, ipc::BuildHandshakeRequest(req));
  auto resp = f.dispatcher.Dispatch(env);
  Expect(resp.has_value(), "handshake response present");
  Expect(resp->request_id == 1, "handshake request_id echoed");
  Expect(resp->type == ipc::MessageType::Handshake, "handshake type echoed");
  auto parsed = ipc::ParseHandshakeResponse(resp->payload_json);
  Expect(parsed.has_value() && parsed->accepted, "handshake accepted");

  ipc::HandshakeRequest bad = req;
  bad.protocol_version = 999;
  auto env2 = f.MakeReq(2, ipc::MessageType::Handshake, ipc::BuildHandshakeRequest(bad));
  auto resp2 = f.dispatcher.Dispatch(env2);
  auto parsed2 = ipc::ParseHandshakeResponse(resp2->payload_json);
  Expect(parsed2.has_value() && !parsed2->accepted, "handshake version mismatch rejected");
}

static void TestPing() {
  Fixture f;
  ipc::PingPayload p;
  p.nonce = 0xCAFEBABE;
  auto env = f.MakeReq(10, ipc::MessageType::Ping, ipc::BuildPing(p));
  auto resp = f.dispatcher.Dispatch(env);
  Expect(resp.has_value(), "ping response");
  auto parsed = ipc::ParsePing(resp->payload_json);
  Expect(parsed.has_value() && parsed->nonce == 0xCAFEBABE, "ping nonce echoed");
  Expect(parsed->t_ms > 0, "ping timestamp populated");
}

static void TestQueryCandidates() {
  Fixture f;
  ipc::QueryCandidatesRequest q;
  q.reading = "にほん";
  q.left_context = "";
  q.max_candidates = 10;
  q.live = false;
  auto env = f.MakeReq(20, ipc::MessageType::QueryCandidates, ipc::BuildQueryCandidatesRequest(q));
  auto resp = f.dispatcher.Dispatch(env);
  Expect(resp.has_value(), "query response present");
  auto parsed = ipc::ParseQueryCandidatesResponse(resp->payload_json);
  Expect(parsed.has_value(), "query response parses");
  Expect(!parsed->candidates.empty(), "candidates returned");
  Expect(parsed->candidates.front().surface == "日本",
         "top candidate for にほん is 日本");
}

static void TestQueryCancelBeforeReply() {
  Fixture f;
  // Pre-cancel the request id. Dispatcher must return nullopt
  // (no reply for canceled requests).
  f.scheduler.Cancel(30);

  ipc::QueryCandidatesRequest q;
  q.reading = "わたし";
  auto env = f.MakeReq(30, ipc::MessageType::QueryCandidates, ipc::BuildQueryCandidatesRequest(q));
  auto resp = f.dispatcher.Dispatch(env);
  Expect(!resp.has_value(), "canceled query must not produce a reply");
}

static void TestCancelMessageNoReply() {
  Fixture f;
  ipc::CancelPayload c;
  c.target_request_id = 999;
  auto env = f.MakeReq(40, ipc::MessageType::Cancel, ipc::BuildCancel(c));
  auto resp = f.dispatcher.Dispatch(env);
  Expect(!resp.has_value(), "cancel produces no response");
  // The dispatcher must record the cancel on the scheduler.
  // (RequestScheduler is the source of truth for cancellations.)
}

static void TestCommitObservation() {
  Fixture f;
  ipc::CommitObservationRequest c;
  c.reading = "にほん";
  c.chosen = {"二本", "にほん", 0.4, "fallback"};
  c.shown = {{"日本", "にほん", 1.0, "static"}, c.chosen};
  c.timestamp_ms = 1700000000000ULL;
  auto env = f.MakeReq(50, ipc::MessageType::CommitObservation, ipc::BuildCommitObservationRequest(c));
  auto resp = f.dispatcher.Dispatch(env);
  Expect(resp.has_value(), "commit response present");
  auto parsed = ipc::ParseCommitObservationResponse(resp->payload_json);
  Expect(parsed.has_value() && parsed->ok, "commit ok");
}

static void TestAddRemoveUserWord() {
  Fixture f;
  ipc::AddUserWordRequest add;
  add.word = "azooKey";
  add.ruby = "あずきい";
  add.value = -3.0;
  auto env = f.MakeReq(60, ipc::MessageType::AddUserWord, ipc::BuildAddUserWordRequest(add));
  auto resp = f.dispatcher.Dispatch(env);
  Expect(resp.has_value(), "add response present");
  auto parsed = ipc::ParseAddUserWordResponse(resp->payload_json);
  Expect(parsed.has_value() && parsed->ok, "add ok");

  // Confirm user dict observably contains the entry.
  auto hits = f.user_dict.Lookup("あずきい");
  Expect(hits.size() == 1, "user dict has the added word");

  // Now query: the user word should be present in candidates.
  ipc::QueryCandidatesRequest q;
  q.reading = "あずきい";
  q.max_candidates = 5;
  auto qenv = f.MakeReq(61, ipc::MessageType::QueryCandidates, ipc::BuildQueryCandidatesRequest(q));
  auto qresp = f.dispatcher.Dispatch(qenv);
  auto qparsed = ipc::ParseQueryCandidatesResponse(qresp->payload_json);
  bool found = false;
  for (const auto& c : qparsed->candidates) {
    if (c.surface == "azooKey") found = true;
  }
  Expect(found, "user dict word appears in QueryCandidates");

  // Now remove it.
  ipc::RemoveUserWordRequest rm;
  rm.word = "azooKey";
  rm.ruby = "あずきい";
  auto renv = f.MakeReq(62, ipc::MessageType::RemoveUserWord, ipc::BuildRemoveUserWordRequest(rm));
  auto rresp = f.dispatcher.Dispatch(renv);
  auto rparsed = ipc::ParseRemoveUserWordResponse(rresp->payload_json);
  Expect(rparsed.has_value() && rparsed->ok, "remove ok");
  Expect(f.user_dict.Lookup("あずきい").empty(), "user dict empty after remove");
}

static void TestLoadModelAppliesRequestOptions() {
  Fixture f;
  ipc::LoadModelRequest req;
  req.path = "azookey_dispatcher_missing_zenzai.gguf";
  req.backend = "cuda";
  req.n_gpu_layers = 24;

  auto env = f.MakeReq(65, ipc::MessageType::LoadModel, ipc::BuildLoadModelRequest(req));
  auto resp = f.dispatcher.Dispatch(env);
  Expect(resp.has_value(), "load model response present");
  auto parsed = ipc::ParseLoadModelResponse(resp->payload_json);
  Expect(parsed.has_value(), "load model response parses");
  Expect(!parsed->ok, "missing model should fail");
  Expect(parsed->error.has_value(), "missing model should report an error");
  Expect(f.engine.backend() == azookey::host::BackendKind::Cuda,
         "dispatcher should apply requested backend");
  Expect(f.engine.config().model_path == req.path,
         "dispatcher should apply requested model path");
  Expect(f.engine.config().n_gpu_layers.has_value() &&
             f.engine.config().n_gpu_layers.value() == 24,
         "dispatcher should apply requested n_gpu_layers");

  auto health_env = f.MakeReq(66, ipc::MessageType::Health, "{}");
  auto health_resp = f.dispatcher.Dispatch(health_env);
  auto health = ipc::ParseHealth(health_resp->payload_json);
  Expect(health.has_value(), "health response parses after load failure");
  Expect(health->backend == "cuda", "health should reflect requested backend");
  Expect(!health->model_loaded, "health should report no loaded model");
  Expect(health->last_error.has_value(), "health should expose last load error");
}

static void TestLoadModelRejectsUnsupportedBackend() {
  Fixture f;
  ipc::LoadModelRequest req;
  req.path = "zenzai.gguf";
  req.backend = "directml";

  auto env = f.MakeReq(67, ipc::MessageType::LoadModel, ipc::BuildLoadModelRequest(req));
  auto resp = f.dispatcher.Dispatch(env);
  Expect(resp.has_value(), "unsupported backend response present");
  auto parsed = ipc::ParseLoadModelResponse(resp->payload_json);
  Expect(parsed.has_value(), "unsupported backend response parses");
  Expect(!parsed->ok, "unsupported backend should fail");
  Expect(parsed->error.has_value(), "unsupported backend should explain error");
  Expect(f.engine.backend() == azookey::host::BackendKind::Cpu,
         "unsupported backend should not change engine backend");
}

static void TestHealth() {
  Fixture f;
  auto env = f.MakeReq(70, ipc::MessageType::Health, "{}");
  auto resp = f.dispatcher.Dispatch(env);
  Expect(resp.has_value(), "health response present");
  auto parsed = ipc::ParseHealth(resp->payload_json);
  Expect(parsed.has_value(), "health response parses");
  Expect(parsed->status == "ok", "health status ok");
  Expect(parsed->backend == "cpu" || parsed->backend == "cuda",
         "health backend valid");
}

int main() {
  try {
    TestHandshake();
    TestPing();
    TestQueryCandidates();
    TestQueryCancelBeforeReply();
    TestCancelMessageNoReply();
    TestCommitObservation();
    TestAddRemoveUserWord();
    TestLoadModelAppliesRequestOptions();
    TestLoadModelRejectsUnsupportedBackend();
    TestHealth();
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
  }
}
