#include "azookey/host/Dispatcher.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "azookey/core/SimpleConverter.h"
#include "azookey/host/InferenceEngine.h"
#include "azookey/host/RequestScheduler.h"
#include "azookey/host/SettingsStore.h"
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

void RemovePathNoThrow(const std::filesystem::path& path) {
  std::error_code ec;
  std::filesystem::remove_all(path, ec);
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

void EnableMockZenzaiCandidatesForTests(azookey::host::ModelLoadOptions& options) {
  options.mock_zenzai_candidates_for_tests = true;
}

azookey::host::DispatcherConfig DefaultDispatcherConfig() {
  azookey::host::DispatcherConfig config;
  config.host_version = "0.1.0";
  config.protocol_version = kProtocolVersion;
  return config;
}

bool ProbeOnlyGgufUnsupportedWithRealLlama() {
#if AZOOKEY_WITH_LLAMA_CPP
  return true;
#else
  return false;
#endif
}

class ThrowingConverter final : public azookey::core::IConverter {
 public:
  std::vector<azookey::core::Candidate> Convert(const std::string&,
                                                const azookey::core::ConversionContext&) override {
    throw std::runtime_error("convert failed");
  }

  std::vector<azookey::core::Candidate> PredictNext(
      const std::string& kana, const azookey::core::ConversionContext& context) override {
    return Convert(kana, context);
  }

  std::vector<azookey::core::Candidate> Correct(
      const std::string& kana, const azookey::core::CorrectionHint&,
      const azookey::core::ConversionContext& context) override {
    return Convert(kana, context);
  }

  void Commit(const azookey::core::Candidate&, const azookey::core::ConversionContext&) override {}
  void Learn(const std::string&, const std::string&) override {}
};
}  // namespace

class DispatcherTest : public ::testing::Test {
 protected:
  DispatcherTest()
      : learning_path("azookey_dispatcher_test_learning.tsv"),
        user_dict_path("azookey_dispatcher_test_user.json"),
        store(learning_path),
        user_dict(user_dict_path),
        engine(std::make_unique<azookey::core::SimpleConverter>(), &store, {}),
        dispatcher(&engine, &scheduler, &user_dict, DefaultDispatcherConfig()) {
    std::remove(learning_path.c_str());
    std::remove(user_dict_path.c_str());
    engine.SetUserDictionary(&user_dict);
  }

  ~DispatcherTest() override {
    engine.FlushLearningStore();
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

TEST_F(DispatcherTest, HandshakeIncludesBatchRomajiSettings) {
  const std::string settings_path = TempPath("azookey_dispatcher_batch_settings.json");
  std::remove(settings_path.c_str());
  {
    std::ofstream out(settings_path);
    ASSERT_TRUE(out.is_open());
    out << "{"
        << "\"batchRomajiConversion\":true,"
        << "\"batchRomajiPreviewStyle\":\"romaji\","
        << "\"batchConversionMode\":\"neural\","
        << "\"batchAutoPunctuation\":true"
        << "}";
  }
  azookey::host::SettingsStore settings_store(settings_path);
  settings_store.Load();
  azookey::host::Dispatcher settings_dispatcher(&engine, &scheduler, &user_dict,
                                                DefaultDispatcherConfig(), &settings_store);

  ipc::HandshakeRequest req;
  req.tip_version = "0.1.0";
  req.protocol_version = kProtocolVersion;
  auto resp = settings_dispatcher.Dispatch(
      MakeReq(3, ipc::MessageType::Handshake, ipc::BuildHandshakeRequest(req)));
  ASSERT_TRUE(resp.has_value());
  auto parsed = ipc::ParseHandshakeResponse(resp->payload_json);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_TRUE(parsed->accepted);
  EXPECT_TRUE(parsed->batch_romaji_conversion);
  EXPECT_EQ(parsed->batch_romaji_preview_style, "romaji");
  EXPECT_EQ(parsed->batch_conversion_mode, "neural");
  EXPECT_TRUE(parsed->batch_auto_punctuation);

  std::remove(settings_path.c_str());
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
  ipc::QueryBatchConversionRequest batch;
  batch.reading = "にほん";
  batch.raw_romaji = "nihon";
  auto batch_after_wrong = token_dispatcher.Dispatch(MakeReq(
      81, ipc::MessageType::QueryBatchConversion, ipc::BuildQueryBatchConversionRequest(batch)));
  ASSERT_TRUE(batch_after_wrong.has_value());
  auto batch_payload = ipc::ParseQueryBatchConversionResponse(batch_after_wrong->payload_json);
  ASSERT_TRUE(batch_payload.has_value());
  EXPECT_EQ(batch_payload->full_surface, "にほん");
  EXPECT_FALSE(batch_payload->partial);
  EXPECT_FALSE(batch_payload->canceled);

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

TEST_F(DispatcherTest, QueryBatchConversionReturnsSingleSegment) {
  ipc::QueryBatchConversionRequest q;
  q.reading = "にほん";
  q.raw_romaji = "nihon";
  q.mode = "neural";
  q.max_candidates = 10;
  auto env =
      MakeReq(22, ipc::MessageType::QueryBatchConversion, ipc::BuildQueryBatchConversionRequest(q));
  auto resp = dispatcher.Dispatch(env);
  ASSERT_TRUE(resp.has_value());
  auto parsed = ipc::ParseQueryBatchConversionResponse(resp->payload_json);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_FALSE(parsed->partial);
  EXPECT_FALSE(parsed->canceled);
  ASSERT_EQ(parsed->segments.size(), 1u);
  EXPECT_EQ(parsed->segments.front().reading, "にほん");
  ASSERT_FALSE(parsed->segments.front().candidates.empty());
  EXPECT_EQ(parsed->segments.front().candidates.front().surface, "日本");
  EXPECT_EQ(parsed->full_surface, "日本");
}

TEST_F(DispatcherTest, QueryCandidatesSerializesTsvDictionarySource) {
  const std::string dict_path = TempPath("azookey_dispatcher_tsv_source_fixture.tsv");
  const std::string tsv_learning_path = TempPath("azookey_dispatcher_tsv_source_learning.tsv");
  std::remove(dict_path.c_str());
  std::remove(tsv_learning_path.c_str());
  {
    std::ofstream out(dict_path);
    ASSERT_TRUE(out.is_open());
    out << "かすたむ\tカスタム\t1.0\tgeneral\n";
  }

  auto converter = std::make_unique<azookey::core::SimpleConverter>();
  ASSERT_TRUE(converter->LoadFromTsv(dict_path));
  azookey::learning::LearningStore local_store(tsv_learning_path);
  azookey::host::InferenceEngine local_engine(std::move(converter), &local_store, {});
  azookey::host::RequestScheduler local_scheduler;
  azookey::host::Dispatcher local_dispatcher(&local_engine, &local_scheduler, nullptr,
                                             DefaultDispatcherConfig());

  ipc::QueryCandidatesRequest q;
  q.reading = "かすたむ";
  q.max_candidates = 10;
  auto env = MakeReq(21, ipc::MessageType::QueryCandidates, ipc::BuildQueryCandidatesRequest(q));
  auto resp = local_dispatcher.Dispatch(env);
  ASSERT_TRUE(resp.has_value());
  auto parsed = ipc::ParseQueryCandidatesResponse(resp->payload_json);
  ASSERT_TRUE(parsed.has_value());
  ASSERT_FALSE(parsed->candidates.empty());
  EXPECT_EQ(parsed->candidates.front().surface, "カスタム");
  EXPECT_EQ(parsed->candidates.front().source, "system");

  std::remove(dict_path.c_str());
  std::remove(tsv_learning_path.c_str());
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

TEST_F(DispatcherTest, QueryExceptionCompletesCancellationState) {
  const char* throwing_path = "azookey_dispatcher_throwing_learning.tsv";
  std::remove(throwing_path);
  azookey::learning::LearningStore throwing_store(throwing_path);
  azookey::host::InferenceEngine throwing_engine(std::make_unique<ThrowingConverter>(),
                                                 &throwing_store, {});
  azookey::host::RequestScheduler throwing_scheduler;
  azookey::host::Dispatcher throwing_dispatcher(&throwing_engine, &throwing_scheduler, nullptr,
                                                DefaultDispatcherConfig());

  ipc::QueryCandidatesRequest q;
  q.reading = "わたし";
  auto env = MakeReq(31, ipc::MessageType::QueryCandidates, ipc::BuildQueryCandidatesRequest(q));
  EXPECT_THROW((void)throwing_dispatcher.Dispatch(env), std::runtime_error);

  throwing_scheduler.Cancel(31);
  throwing_scheduler.MarkLatest(32);
  EXPECT_FALSE(throwing_scheduler.IsCanceled(31));

  std::remove(throwing_path);
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
  auto env =
      MakeReq(50, ipc::MessageType::CommitObservation, ipc::BuildCommitObservationRequest(c));
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

TEST_F(DispatcherTest, AddUserWordSaveFailureReturnsFalseAndRollsBack) {
  const auto blocking_parent =
      std::filesystem::path(TempPath("azookey_dispatcher_user_dict_blocker_add"));
  RemovePathNoThrow(blocking_parent);
  {
    std::ofstream blocker(blocking_parent, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(blocker.is_open());
    blocker << "not a directory";
  }

  const auto bad_dict_path = (blocking_parent / "user.json").string();
  azookey::learning::UserDictionary bad_dict(bad_dict_path);
  engine.SetUserDictionary(&bad_dict);
  azookey::host::Dispatcher bad_dispatcher(&engine, &scheduler, &bad_dict,
                                           DefaultDispatcherConfig());

  ipc::AddUserWordRequest add;
  add.word = "azooKey";
  add.ruby = "あずきい";
  add.value = -3.0;
  auto resp = bad_dispatcher.Dispatch(
      MakeReq(63, ipc::MessageType::AddUserWord, ipc::BuildAddUserWordRequest(add)));

  ASSERT_TRUE(resp.has_value());
  auto parsed = ipc::ParseAddUserWordResponse(resp->payload_json);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_FALSE(parsed->ok);
  EXPECT_TRUE(bad_dict.Lookup("あずきい").empty());
  ASSERT_TRUE(engine.last_error().has_value());
  EXPECT_EQ(*engine.last_error(), "failed to save user dictionary");

  engine.SetUserDictionary(&user_dict);
  RemovePathNoThrow(blocking_parent);
}

TEST_F(DispatcherTest, RemoveUserWordSaveFailureReturnsFalseAndRollsBack) {
  const auto blocking_parent =
      std::filesystem::path(TempPath("azookey_dispatcher_user_dict_blocker_remove"));
  RemovePathNoThrow(blocking_parent);
  {
    std::ofstream blocker(blocking_parent, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(blocker.is_open());
    blocker << "not a directory";
  }

  const auto bad_dict_path = (blocking_parent / "user.json").string();
  azookey::learning::UserDictionary bad_dict(bad_dict_path);
  azookey::learning::UserWord existing;
  existing.word = "azooKey";
  existing.ruby = "あずきい";
  existing.value = -3.0;
  bad_dict.Add(existing);
  engine.SetUserDictionary(&bad_dict);
  azookey::host::Dispatcher bad_dispatcher(&engine, &scheduler, &bad_dict,
                                           DefaultDispatcherConfig());

  ipc::RemoveUserWordRequest rm;
  rm.word = "azooKey";
  rm.ruby = "あずきい";
  auto resp = bad_dispatcher.Dispatch(
      MakeReq(64, ipc::MessageType::RemoveUserWord, ipc::BuildRemoveUserWordRequest(rm)));

  ASSERT_TRUE(resp.has_value());
  auto parsed = ipc::ParseRemoveUserWordResponse(resp->payload_json);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_FALSE(parsed->ok);
  auto hits = bad_dict.Lookup("あずきい");
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_EQ(hits.front(), existing);
  ASSERT_TRUE(engine.last_error().has_value());
  EXPECT_EQ(*engine.last_error(), "failed to save user dictionary");

  engine.SetUserDictionary(&user_dict);
  RemovePathNoThrow(blocking_parent);
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
  EXPECT_EQ(health->status, "degraded");
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
  if (ProbeOnlyGgufUnsupportedWithRealLlama()) {
    GTEST_SKIP() << "The minimal GGUF fixture is probe-only; real llama.cpp "
                    "loads require a full model fixture.";
  }

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

TEST_F(DispatcherTest, NoLlamaZenzaiRuntimeStaysFallbackOnly) {
  if (ProbeOnlyGgufUnsupportedWithRealLlama()) {
    GTEST_SKIP() << "The minimal GGUF fixture is probe-only; real llama.cpp "
                    "loads require a full model fixture.";
  }

  const std::string model_path = TempPath("azookey_dispatcher_degraded_zenzai.gguf");
  std::remove(model_path.c_str());
  WriteMinimalGguf(model_path);

  ipc::LoadModelRequest req;
  req.path = model_path;
  req.backend = "cpu";
  auto env = MakeReq(71, ipc::MessageType::LoadModel, ipc::BuildLoadModelRequest(req));
  auto resp = dispatcher.Dispatch(env);
  ASSERT_TRUE(resp.has_value());
  ASSERT_TRUE(ipc::ParseLoadModelResponse(resp->payload_json)->ok);

  ipc::QueryCandidatesRequest fallback_query;
  fallback_query.reading = "にほんご";
  auto fallback_env = MakeReq(72, ipc::MessageType::QueryCandidates,
                              ipc::BuildQueryCandidatesRequest(fallback_query));
  auto fallback_resp = dispatcher.Dispatch(fallback_env);
  ASSERT_TRUE(fallback_resp.has_value());
  auto fallback_candidates = ipc::ParseQueryCandidatesResponse(fallback_resp->payload_json);
  ASSERT_TRUE(fallback_candidates.has_value());
  ASSERT_FALSE(fallback_candidates->candidates.empty());
  EXPECT_EQ(fallback_candidates->candidates.front().surface, "にほんご");

  auto degraded_health_resp = dispatcher.Dispatch(MakeReq(73, ipc::MessageType::Health, "{}"));
  ASSERT_TRUE(degraded_health_resp.has_value());
  auto degraded_health = ipc::ParseHealth(degraded_health_resp->payload_json);
  ASSERT_TRUE(degraded_health.has_value());
  EXPECT_EQ(degraded_health->status, "degraded");
  ASSERT_TRUE(degraded_health->last_error.has_value());
  EXPECT_NE(degraded_health->last_error->find("empty-generation"), std::string::npos);

  std::remove(model_path.c_str());
}

TEST_F(DispatcherTest, QueryCandidatesSerializesStableModelSource) {
  if (ProbeOnlyGgufUnsupportedWithRealLlama()) {
    GTEST_SKIP() << "The minimal GGUF fixture is probe-only; real llama.cpp "
                    "loads require a full model fixture.";
  }

  const std::string model_path = TempPath("azookey_dispatcher_model_source_zenzai.gguf");
  std::remove(model_path.c_str());
  WriteMinimalGguf(model_path);

  azookey::host::ModelLoadOptions options;
  options.path = model_path;
  EnableMockZenzaiCandidatesForTests(options);
  ASSERT_TRUE(engine.LoadModelWithResult(options).ok);

  ipc::QueryCandidatesRequest query;
  query.reading = "にほんご";
  query.max_candidates = 10;
  auto query_env =
      MakeReq(74, ipc::MessageType::QueryCandidates, ipc::BuildQueryCandidatesRequest(query));
  auto query_resp = dispatcher.Dispatch(query_env);
  ASSERT_TRUE(query_resp.has_value());
  auto parsed = ipc::ParseQueryCandidatesResponse(query_resp->payload_json);
  ASSERT_TRUE(parsed.has_value());
  ASSERT_FALSE(parsed->candidates.empty());
  EXPECT_EQ(parsed->candidates.front().surface, "日本語");
  EXPECT_EQ(parsed->candidates.front().source, "model");

  std::remove(model_path.c_str());
}

TEST_F(DispatcherTest, NoLlamaZenzaiFallbackResponseIsParseable) {
  if (ProbeOnlyGgufUnsupportedWithRealLlama()) {
    GTEST_SKIP() << "The minimal GGUF fixture is probe-only; real llama.cpp "
                    "loads require a full model fixture.";
  }

  const std::string model_path = TempPath("azookey_dispatcher_invalid_utf8_zenzai.gguf");
  std::remove(model_path.c_str());
  WriteMinimalGguf(model_path);

  ipc::LoadModelRequest req;
  req.path = model_path;
  req.backend = "cpu";
  auto load_env = MakeReq(76, ipc::MessageType::LoadModel, ipc::BuildLoadModelRequest(req));
  auto load_resp = dispatcher.Dispatch(load_env);
  ASSERT_TRUE(load_resp.has_value());
  ASSERT_TRUE(ipc::ParseLoadModelResponse(load_resp->payload_json)->ok);

  ipc::QueryCandidatesRequest query;
  query.reading = "むこう";
  auto query_env =
      MakeReq(77, ipc::MessageType::QueryCandidates, ipc::BuildQueryCandidatesRequest(query));
  auto query_resp = dispatcher.Dispatch(query_env);
  ASSERT_TRUE(query_resp.has_value());
  auto parsed = ipc::ParseQueryCandidatesResponse(query_resp->payload_json);
  ASSERT_TRUE(parsed.has_value());
  ASSERT_FALSE(parsed->candidates.empty());
  EXPECT_EQ(parsed->candidates.front().surface, "むこう");

  auto health_resp = dispatcher.Dispatch(MakeReq(78, ipc::MessageType::Health, "{}"));
  ASSERT_TRUE(health_resp.has_value());
  auto health = ipc::ParseHealth(health_resp->payload_json);
  ASSERT_TRUE(health.has_value());
  EXPECT_EQ(health->status, "degraded");
  ASSERT_TRUE(health->last_error.has_value());
  EXPECT_NE(health->last_error->find("empty-generation"), std::string::npos);

  std::remove(model_path.c_str());
}

TEST_F(DispatcherTest, LoadModelCudaFallbackKeepsHealthOk) {
  if (ProbeOnlyGgufUnsupportedWithRealLlama()) {
    GTEST_SKIP() << "The minimal GGUF fixture is probe-only; real llama.cpp "
                    "loads require a full model fixture.";
  }

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

TEST_F(DispatcherTest, UpdateConfigWithoutSettingsStoreReturnsError) {
  auto resp = dispatcher.Dispatch(MakeReq(73, ipc::MessageType::UpdateConfig, "{}"));
  ASSERT_TRUE(resp.has_value());
  EXPECT_EQ(resp->type, ipc::MessageType::UpdateConfig);
  auto parsed = ipc::ParseUpdateConfigResponse(resp->payload_json);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_FALSE(parsed->ok);
  ASSERT_TRUE(parsed->error.has_value());
  EXPECT_EQ(*parsed->error, "settings store not configured");
}

TEST_F(DispatcherTest, DispatcherConfigCopiesShareUpdateConfigMutex) {
  azookey::host::DispatcherConfig config;
  const auto shared_mutex = config.update_config_mutex;
  azookey::host::DispatcherConfig copy = config;

  ASSERT_TRUE(shared_mutex);
  EXPECT_EQ(copy.update_config_mutex, shared_mutex);
  EXPECT_NE(azookey::host::DispatcherConfig{}.update_config_mutex, shared_mutex);
}

TEST_F(DispatcherTest, UpdateConfigReloadsSettingsAndAppliesEngineConfig) {
  const auto settings_path = TempPath("azookey_dispatcher_settings.json");
  std::remove(settings_path.c_str());

  {
    std::ofstream out(settings_path, std::ios::binary);
    out << R"({"liveConversion":false,"backendPreference":"cuda"})";
  }
  azookey::host::SettingsStore settings_store(settings_path);
  azookey::host::Dispatcher config_dispatcher(&engine, &scheduler, &user_dict,
                                              DefaultDispatcherConfig(), &settings_store);

  auto resp = config_dispatcher.Dispatch(MakeReq(74, ipc::MessageType::UpdateConfig, "{}"));
  ASSERT_TRUE(resp.has_value());
  auto parsed = ipc::ParseUpdateConfigResponse(resp->payload_json);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_TRUE(parsed->ok);
  EXPECT_FALSE(engine.config().enable_live_conversion);
  EXPECT_EQ(engine.backend(), azookey::host::BackendKind::Cuda);

  {
    std::ofstream out(settings_path, std::ios::binary | std::ios::trunc);
    out << R"({"liveConversion":true,"backendPreference":"auto"})";
  }

  auto second = config_dispatcher.Dispatch(MakeReq(75, ipc::MessageType::UpdateConfig, "{}"));
  ASSERT_TRUE(second.has_value());
  auto second_payload = ipc::ParseUpdateConfigResponse(second->payload_json);
  ASSERT_TRUE(second_payload.has_value());
  EXPECT_TRUE(second_payload->ok);
  EXPECT_TRUE(engine.config().enable_live_conversion);
  EXPECT_EQ(engine.backend(), azookey::host::BackendKind::Cpu);

  std::remove(settings_path.c_str());
}

TEST_F(DispatcherTest, UpdateConfigInvalidSettingsPreservesRuntimeConfig) {
  if (ProbeOnlyGgufUnsupportedWithRealLlama()) {
    GTEST_SKIP() << "The minimal GGUF fixture is probe-only; real llama.cpp "
                    "loads require a full model fixture.";
  }

  const auto settings_path = TempPath("azookey_dispatcher_invalid_reload_settings.json");
  const auto model_path = TempPath("azookey_dispatcher_invalid_reload_zenzai.gguf");
  const auto model_json_path = std::filesystem::path(model_path).generic_string();
  std::remove(settings_path.c_str());
  std::remove(model_path.c_str());
  WriteMinimalGguf(model_path);

  {
    std::ofstream out(settings_path, std::ios::binary);
    out << R"({"model":{"selectedPath":")" << model_json_path << R"("}})";
  }
  azookey::host::SettingsStore settings_store(settings_path);
  azookey::host::Dispatcher config_dispatcher(&engine, &scheduler, &user_dict,
                                              DefaultDispatcherConfig(), &settings_store);

  auto resp = config_dispatcher.Dispatch(MakeReq(76, ipc::MessageType::UpdateConfig, "{}"));
  ASSERT_TRUE(resp.has_value());
  auto parsed = ipc::ParseUpdateConfigResponse(resp->payload_json);
  ASSERT_TRUE(parsed.has_value());
  ASSERT_TRUE(parsed->ok);
  ASSERT_TRUE(engine.model_loaded());
  EXPECT_EQ(engine.config().model_path, model_json_path);

  {
    std::ofstream out(settings_path, std::ios::binary | std::ios::trunc);
    out << "{ invalid json";
  }

  auto invalid = config_dispatcher.Dispatch(MakeReq(77, ipc::MessageType::UpdateConfig, "{}"));
  ASSERT_TRUE(invalid.has_value());
  auto invalid_payload = ipc::ParseUpdateConfigResponse(invalid->payload_json);
  ASSERT_TRUE(invalid_payload.has_value());
  EXPECT_FALSE(invalid_payload->ok);
  ASSERT_TRUE(invalid_payload->error.has_value());
  EXPECT_TRUE(engine.model_loaded());
  EXPECT_EQ(engine.config().model_path, model_json_path);

  std::remove(settings_path.c_str());
  std::remove(model_path.c_str());
}

TEST_F(DispatcherTest, UpdateConfigPreservesCliBackendAndModelOverrides) {
  if (ProbeOnlyGgufUnsupportedWithRealLlama()) {
    GTEST_SKIP() << "The minimal GGUF fixture is probe-only; real llama.cpp "
                    "loads require a full model fixture.";
  }

  const auto settings_path = TempPath("azookey_dispatcher_override_settings.json");
  const auto cli_model_path = TempPath("azookey_dispatcher_override_zenzai.gguf");
  std::remove(settings_path.c_str());
  std::remove(cli_model_path.c_str());
  WriteMinimalGguf(cli_model_path);

  {
    std::ofstream out(settings_path, std::ios::binary);
    out << R"({
      "backendPreference": "cuda",
      "model": {
        "selectedPath": "C:/models/settings-selected.gguf"
      }
    })";
  }

  azookey::host::SettingsStore settings_store(settings_path);
  azookey::host::DispatcherConfig config;
  config.host_version = "0.1.0";
  config.protocol_version = kProtocolVersion;
  config.override_backend = azookey::host::BackendKind::Cpu;
  config.override_model_path = cli_model_path;
  azookey::host::Dispatcher config_dispatcher(&engine, &scheduler, &user_dict, config,
                                              &settings_store);

  auto resp = config_dispatcher.Dispatch(MakeReq(76, ipc::MessageType::UpdateConfig, "{}"));
  ASSERT_TRUE(resp.has_value());
  auto parsed = ipc::ParseUpdateConfigResponse(resp->payload_json);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_TRUE(parsed->ok);
  EXPECT_EQ(engine.backend(), azookey::host::BackendKind::Cpu);
  EXPECT_EQ(engine.config().model_path, cli_model_path);

  std::remove(settings_path.c_str());
  std::remove(cli_model_path.c_str());
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
  auto resp_a =
      conn_a.Dispatch(MakeReq(1, ipc::MessageType::Handshake, ipc::BuildHandshakeRequest(req)));
  ASSERT_TRUE(resp_a.has_value());
  EXPECT_TRUE(ipc::ParseHandshakeResponse(resp_a->payload_json)->accepted);

  // Connection B has NOT performed a handshake; AddUserWord must be rejected
  // (returns ok=false rather than nullopt so the client does not hang).
  ipc::AddUserWordRequest add;
  add.word = "test";
  add.ruby = "てすと";
  auto resp_b =
      conn_b.Dispatch(MakeReq(2, ipc::MessageType::AddUserWord, ipc::BuildAddUserWordRequest(add)));
  ASSERT_TRUE(resp_b.has_value());
  EXPECT_FALSE(ipc::ParseAddUserWordResponse(resp_b->payload_json)->ok);
  EXPECT_TRUE(user_dict.Lookup("てすと").empty());

  // A failed handshake on B must not de-authenticate A.
  ipc::HandshakeRequest bad_req = req;
  bad_req.handshake_token = "wrong";
  conn_b.Dispatch(MakeReq(3, ipc::MessageType::Handshake, ipc::BuildHandshakeRequest(bad_req)));
  auto resp_a2 =
      conn_a.Dispatch(MakeReq(4, ipc::MessageType::AddUserWord, ipc::BuildAddUserWordRequest(add)));
  ASSERT_TRUE(resp_a2.has_value());
  EXPECT_TRUE(ipc::ParseAddUserWordResponse(resp_a2->payload_json)->ok);
}

TEST_F(DispatcherTest, CrossClientCancelUsesHandshakeClientIdNamespace) {
  azookey::host::Dispatcher primary_a(&engine, &scheduler, &user_dict, DefaultDispatcherConfig());
  azookey::host::Dispatcher cancel_a(&engine, &scheduler, &user_dict, DefaultDispatcherConfig());
  azookey::host::Dispatcher primary_b(&engine, &scheduler, &user_dict, DefaultDispatcherConfig());

  auto handshake = [this](azookey::host::Dispatcher& connection, const std::string& client_id,
                          uint64_t request_id) {
    ipc::HandshakeRequest req;
    req.tip_version = "0.1.0";
    req.protocol_version = kProtocolVersion;
    req.client_id = client_id;
    auto response = connection.Dispatch(
        MakeReq(request_id, ipc::MessageType::Handshake, ipc::BuildHandshakeRequest(req)));
    return response && ipc::ParseHandshakeResponse(response->payload_json)->accepted;
  };

  ASSERT_TRUE(handshake(primary_a, "client-a", 1));
  ASSERT_TRUE(handshake(cancel_a, "client-a", 2));
  ASSERT_TRUE(handshake(primary_b, "client-b", 3));

  ipc::CancelPayload cancel;
  cancel.target_request_id = 81;
  EXPECT_FALSE(cancel_a.Dispatch(MakeReq(4, ipc::MessageType::Cancel, ipc::BuildCancel(cancel)))
                   .has_value());

  ipc::QueryCandidatesRequest query;
  query.reading = "わたし";
  const auto query_payload = ipc::BuildQueryCandidatesRequest(query);

  auto response_b =
      primary_b.Dispatch(MakeReq(81, ipc::MessageType::QueryCandidates, query_payload));
  ASSERT_TRUE(response_b.has_value());

  auto response_a =
      primary_a.Dispatch(MakeReq(81, ipc::MessageType::QueryCandidates, query_payload));
  EXPECT_FALSE(response_a.has_value());
}

TEST_F(DispatcherTest, RepeatedHandshakeDoesNotRetainClientStateAfterDisconnect) {
  auto handshake = [this](azookey::host::Dispatcher& connection, uint64_t request_id) {
    ipc::HandshakeRequest req;
    req.tip_version = "0.1.0";
    req.protocol_version = kProtocolVersion;
    req.client_id = "client-a";
    auto response = connection.Dispatch(
        MakeReq(request_id, ipc::MessageType::Handshake, ipc::BuildHandshakeRequest(req)));
    return response && ipc::ParseHandshakeResponse(response->payload_json)->accepted;
  };

  {
    azookey::host::Dispatcher connection(&engine, &scheduler, &user_dict,
                                         DefaultDispatcherConfig());
    ASSERT_TRUE(handshake(connection, 1));
    ASSERT_TRUE(handshake(connection, 2));

    ipc::CancelPayload cancel;
    cancel.target_request_id = 81;
    EXPECT_FALSE(connection.Dispatch(MakeReq(3, ipc::MessageType::Cancel, ipc::BuildCancel(cancel)))
                     .has_value());
  }

  azookey::host::Dispatcher replacement(&engine, &scheduler, &user_dict, DefaultDispatcherConfig());
  ASSERT_TRUE(handshake(replacement, 4));
  ipc::QueryCandidatesRequest query;
  query.reading = "わたし";
  EXPECT_TRUE(replacement
                  .Dispatch(MakeReq(81, ipc::MessageType::QueryCandidates,
                                    ipc::BuildQueryCandidatesRequest(query)))
                  .has_value());
}
