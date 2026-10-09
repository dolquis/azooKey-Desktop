#include "azookey/host/Dispatcher.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "../../learning/tests/TestByteCrypto.h"
#include "IpcTestData.h"
#include "azookey/core/BatchConversionChunker.h"
#include "azookey/core/PlatformPaths.h"
#include "azookey/core/SimpleConverter.h"
#include "azookey/host/InferenceEngine.h"
#include "azookey/host/ModelBenchmark.h"
#include "azookey/host/PunctuationInserter.h"
#include "azookey/host/RequestScheduler.h"
#include "azookey/host/SettingsStore.h"
#include "azookey/ipc/HandshakeToken.h"
#include "azookey/ipc/Json.h"
#include "azookey/ipc/Limits.h"
#include "azookey/ipc/Messages.h"
#include "azookey/ipc/Payloads.h"
#include "azookey/learning/AutoWordStore.h"
#include "azookey/learning/ContextHash.h"
#include "azookey/learning/LearningStore.h"
#include "azookey/learning/TypoCorrectionStore.h"
#include "azookey/learning/UserDictionary.h"
#include "azookey/logging/RuntimeLogger.h"

namespace ipc = azookey::ipc;

TEST(BatchConversionChunkerTest, PreservesSentenceBoundariesAndEveryByte) {
  const std::string input = "にほんご。つぎのぶん！さいご";
  const auto chunks = azookey::core::SplitBatchConversion(input);
  ASSERT_EQ(chunks.size(), 3u);
  EXPECT_EQ(chunks[0], "にほんご。");
  EXPECT_EQ(chunks[1], "つぎのぶん！");
  EXPECT_EQ(chunks[0] + chunks[1] + chunks[2], input);
}

TEST(BatchConversionChunkerTest, HardSplitPreservesUtf8WithoutSentenceBoundaries) {
  const std::string input = "あいうえおかきくけこ";
  const auto chunks = azookey::core::SplitBatchConversion(input, 7);
  std::string joined;
  for (const auto& chunk : chunks) {
    EXPECT_LE(chunk.size(), 7u);
    size_t offset = 0;
    char32_t codepoint{};
    while (offset < chunk.size()) {
      EXPECT_TRUE(azookey::core::DecodeNextUtf8(chunk, offset, codepoint));
    }
    joined += chunk;
  }
  EXPECT_EQ(joined, input);
}

TEST(BatchConversionChunkerTest, RawSplitDoesNotFlushPendingRomaji) {
  for (const auto& input : {std::string("kananakanaka"), std::string(2000, 'n')}) {
    const auto chunks = azookey::core::SplitBatchRomaji(input, 4);
    std::string raw;
    std::string reading;
    for (const auto& chunk : chunks) {
      EXPECT_LE(chunk.raw_romaji.size(), 8u);
      raw += chunk.raw_romaji;
      reading += chunk.reading;
    }
    EXPECT_EQ(raw, input);
    EXPECT_EQ(reading, azookey::core::RomajiKanaConverter::ConvertForCommit(input));
  }
}

TEST(BatchConversionChunkerTest, InputAboveFrameLimitProducesBoundedEnvelopes) {
  const std::string raw(ipc::kMaxJsonInputBytes + 123, 'a');
  size_t raw_size = 0;
  size_t reading_size = 0;
  for (const auto& chunk : azookey::core::SplitBatchRomaji(raw)) {
    ipc::QueryBatchConversionRequest request;
    request.reading = chunk.reading;
    request.raw_romaji = chunk.raw_romaji;
    ipc::Envelope envelope;
    envelope.type = ipc::MessageType::QueryBatchConversion;
    envelope.trace_id = "batch-boundary-test";
    envelope.payload_json = ipc::BuildQueryBatchConversionRequest(request);
    const auto serialized = ipc::Serialize(envelope);
    ASSERT_TRUE(serialized);
    ASSERT_LT(serialized->size(), ipc::kMaxFrameSize);
    raw_size += chunk.raw_romaji.size();
    reading_size += chunk.reading.size();
  }
  EXPECT_EQ(raw_size, raw.size());
  EXPECT_EQ(reading_size, raw.size() * 3);
}

namespace {

constexpr int kProtocolVersion = 1;

std::string TempPath(const char* name) {
  return (std::filesystem::temp_directory_path() / name).string();
}

// CTest runs each case in its own process concurrently, so fixture files carry
// the test name: one case corrupting its M7 file must not reach another case.
std::string PerTestFileName(const char* stem, const char* extension) {
  const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
  return std::string(stem) + "_" + (info ? info->name() : "no_test") + extension;
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
  config.host_generation_id = "dispatcher-test-generation";
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
      : learning_path(PerTestFileName("azookey_dispatcher_test_learning", ".tsv")),
        user_dict_path(PerTestFileName("azookey_dispatcher_test_user", ".json")),
        store(learning_path, &azookey::learning::test::Crypto()),
        user_dict(user_dict_path, &azookey::learning::test::Crypto()),
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

  void EnableEventPrivacy(azookey::host::Dispatcher& target) {
    ipc::HandshakeRequest request;
    request.tip_version = "test";
    request.client_id = "privacy-test";
    request.capabilities = {"secure_flag"};
    target.Dispatch(
        MakeReq(9000, ipc::MessageType::Handshake, ipc::BuildHandshakeRequest(request)));
  }

  std::string learning_path;
  std::string user_dict_path;
  azookey::learning::LearningStore store;
  azookey::learning::UserDictionary user_dict;
  azookey::host::InferenceEngine engine;
  azookey::host::RequestScheduler scheduler;
  azookey::host::Dispatcher dispatcher;
};

TEST_F(DispatcherTest, QueryLatencyUsesConfiguredLogDirectory) {
  const auto directory = std::filesystem::temp_directory_path() /
                         ("azookey_dispatcher_query_log_" + std::to_string(std::random_device{}()));
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() { RemovePathNoThrow(path); }
  } cleanup{directory};
  azookey::logging::RuntimeLoggerOptions options;
  options.enabled = true;
  options.component = "host";
  options.logs_directory = directory;
  azookey::logging::RuntimeLogger logger(options);
  auto config = DefaultDispatcherConfig();
  azookey::host::Dispatcher target(&engine, &scheduler, &user_dict, config, nullptr, nullptr,
                                   &logger);

  auto request = MakeReq(101, ipc::MessageType::QueryCandidates, "{");
  request.trace_id = "018fd2c2-2a3e-7c9a-b8e1-7f3a92d4c5e2";
  const auto response = target.Dispatch(request);
  ASSERT_TRUE(response.has_value());
  request.request_id = 102;
  request.trace_id = "private-prompt-text";
  ASSERT_TRUE(target.Dispatch(request).has_value());
  std::string log_text;
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    if (entry.path().extension() != ".jsonl") continue;
    std::ifstream input(entry.path());
    log_text.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  }
  EXPECT_NE(log_text.find("\"event\":\"query_latency\""), std::string::npos);
  EXPECT_NE(log_text.find("\"trace_id\":\"018fd2c2-2a3e-7c9a-b8e1-7f3a92d4c5e2\""),
            std::string::npos);
  EXPECT_NE(log_text.find("\"trace_id\":\"***redacted***\""), std::string::npos);
  EXPECT_EQ(log_text.find("private-prompt-text"), std::string::npos);
  EXPECT_NE(log_text.find("\"request_id\":101"), std::string::npos);
}

TEST_F(DispatcherTest, HandshakeTokenValuesNeverReachRuntimeLogOrResponses) {
  const auto directory = std::filesystem::temp_directory_path() /
                         ("azookey_dispatcher_token_log_" + std::to_string(std::random_device{}()));
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() { RemovePathNoThrow(path); }
  } cleanup{directory};
  const auto host_token = ipc::GenerateHandshakeToken();
  const auto wrong_token = ipc::GenerateHandshakeToken();
  ASSERT_TRUE(host_token);
  ASSERT_TRUE(wrong_token);
  ASSERT_NE(*host_token, *wrong_token);

  std::string responses;
  {
    azookey::logging::RuntimeLoggerOptions options;
    options.enabled = true;
    options.component = "host";
    options.logs_directory = directory;
    azookey::logging::RuntimeLogger logger(options);
    auto config = DefaultDispatcherConfig();
    config.handshake_token = *host_token;
    azookey::host::Dispatcher target(&engine, &scheduler, &user_dict, config, nullptr, nullptr,
                                     &logger);

    uint64_t next_id = 200;
    // Every handshake is followed by a query so query_latency is written and the
    // scan below is not vacuously true on an empty log.
    const auto handshake_then_query = [&](const std::string& handshake_payload) {
      const auto handshake =
          target.Dispatch(MakeReq(next_id++, ipc::MessageType::Handshake, handshake_payload));
      ASSERT_TRUE(handshake.has_value());
      responses += handshake->payload_json;
      const auto query =
          target.Dispatch(MakeReq(next_id++, ipc::MessageType::QueryCandidates, "{"));
      ASSERT_TRUE(query.has_value());
      responses += query->payload_json;
    };
    const auto handshake_with = [&](const std::string& token) {
      ipc::HandshakeRequest request;
      request.tip_version = "test";
      request.protocol_version = kProtocolVersion;
      request.handshake_token = token;
      return ipc::BuildHandshakeRequest(request);
    };

    handshake_then_query(handshake_with(*wrong_token));
    handshake_then_query(handshake_with(""));
    handshake_then_query("{\"handshake_token\":\"" + *wrong_token + "\",");
    handshake_then_query("{\"handshake_token\":\"" + *host_token + "\",");
    handshake_then_query(handshake_with(*host_token));
  }

  std::string log_text;
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    if (entry.path().extension() != ".jsonl") continue;
    std::ifstream input(entry.path());
    log_text.append(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  }
  EXPECT_NE(log_text.find("\"event\":\"query_latency\""), std::string::npos);
  for (const auto& secret : {*host_token, *wrong_token}) {
    EXPECT_EQ(log_text.find(secret), std::string::npos);
    EXPECT_EQ(responses.find(secret), std::string::npos);
  }
}

TEST_F(DispatcherTest, PendingLimitRejectsQueriesWithoutCompletingExistingRequests) {
  constexpr auto limit = azookey::host::RequestScheduler::kMaxPendingRequestsPerClient;
  for (uint64_t id = 1; id <= limit; ++id) {
    ASSERT_NE(scheduler.TrackCancellation(id), nullptr);
  }
  scheduler.MarkLatest(1);
  ipc::QueryCandidatesRequest query;
  query.reading = "test";
  auto response = dispatcher.Dispatch(
      MakeReq(1, ipc::MessageType::QueryCandidates, ipc::BuildQueryCandidatesRequest(query)));
  ASSERT_TRUE(response.has_value());
  auto parsed = ipc::ParseQueryCandidatesResponse(response->payload_json);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_TRUE(parsed->candidates.empty());
  EXPECT_EQ(scheduler.TrackCancellation(limit + 1), nullptr);

  ipc::QueryBatchConversionRequest batch;
  batch.reading = "test";
  response = dispatcher.Dispatch(MakeReq(limit + 1, ipc::MessageType::QueryBatchConversion,
                                         ipc::BuildQueryBatchConversionRequest(batch)));
  ASSERT_TRUE(response.has_value());
  auto parsed_batch = ipc::ParseQueryBatchConversionResponse(response->payload_json);
  ASSERT_TRUE(parsed_batch.has_value());
  EXPECT_TRUE(parsed_batch->canceled);
  EXPECT_TRUE(scheduler.IsLatest(1));
  EXPECT_EQ(scheduler.TrackCancellation(limit + 1), nullptr);
  const auto predictions =
      dispatcher.Dispatch(MakeReq(limit + 2, ipc::MessageType::QueryPredictions,
                                  ipc::BuildQueryPredictionsRequest({"にほん", "", "word"})));
  ASSERT_TRUE(predictions);
  const auto parsed_predictions = ipc::ParseQueryPredictionsResponse(predictions->payload_json);
  ASSERT_TRUE(parsed_predictions);
  EXPECT_FALSE(parsed_predictions->ok);
  EXPECT_EQ(parsed_predictions->error, "too_many_pending_requests");
  EXPECT_TRUE(scheduler.IsLatest(1));
  scheduler.CompleteRequest(1);
  EXPECT_NE(scheduler.TrackCancellation(limit + 1), nullptr);
}

TEST_F(DispatcherTest, ReverseConvertReturnsKnownReadingAndUnknownSignal) {
  user_dict.Add({"明日", "あした"});
  auto request =
      MakeReq(100, ipc::MessageType::ReverseConvert, ipc::BuildReverseConvertRequest({"明日"}));
  const auto response = dispatcher.Dispatch(request);
  ASSERT_TRUE(response);
  EXPECT_EQ(response->type, ipc::MessageType::ReverseConvert);
  EXPECT_EQ(response->request_id, request.request_id);
  EXPECT_EQ(response->trace_id, request.trace_id);
  const auto parsed = ipc::ParseReverseConvertResponse(response->payload_json);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->reading, "あした");
  EXPECT_DOUBLE_EQ(parsed->confidence, 1.0);

  const auto unknown =
      dispatcher.Dispatch(MakeReq(101, ipc::MessageType::ReverseConvert,
                                  ipc::BuildReverseConvertRequest({"存在しない表層形"})));
  ASSERT_TRUE(unknown);
  const auto parsed_unknown = ipc::ParseReverseConvertResponse(unknown->payload_json);
  ASSERT_TRUE(parsed_unknown);
  EXPECT_TRUE(parsed_unknown->reading.empty());
  EXPECT_DOUBLE_EQ(parsed_unknown->confidence, 0.0);
}

// DEV-1486: releases bundle no static layers, so a model-produced surface is
// reverse-converted from the commit that learned it, and kana is its own reading.
TEST_F(DispatcherTest, ReverseConvertFallsBackToLearnedCommitAndKanaSurface) {
  EnableEventPrivacy(dispatcher);
  const auto reverse = [&](uint64_t id, const std::string& surface) {
    const auto response = dispatcher.Dispatch(
        MakeReq(id, ipc::MessageType::ReverseConvert, ipc::BuildReverseConvertRequest({surface})));
    EXPECT_TRUE(response);
    return response ? ipc::ParseReverseConvertResponse(response->payload_json) : std::nullopt;
  };
  const auto before = reverse(110, "明日");
  ASSERT_TRUE(before);
  EXPECT_TRUE(before->reading.empty());

  ipc::CommitObservationRequest commit;
  commit.secure = false;
  commit.learning_allowed = true;
  commit.reading = "あした";
  commit.chosen = {"明日", "あした", 1.0, "model"};
  commit.timestamp_ms = 1700000000000ULL;
  ASSERT_TRUE(dispatcher.Dispatch(MakeReq(111, ipc::MessageType::CommitObservation,
                                          ipc::BuildCommitObservationRequest(commit))));
  const auto learned = reverse(112, "明日");
  ASSERT_TRUE(learned);
  EXPECT_EQ(learned->reading, "あした");
  EXPECT_DOUBLE_EQ(learned->confidence, 1.0);

  const auto katakana = reverse(113, "アシター");
  ASSERT_TRUE(katakana);
  EXPECT_EQ(katakana->reading, "あしたー");
  const auto hiragana = reverse(114, "あした");
  ASSERT_TRUE(hiragana);
  EXPECT_EQ(hiragana->reading, "あした");
  const auto mixed = reverse(115, "Ashita");
  ASSERT_TRUE(mixed);
  EXPECT_TRUE(mixed->reading.empty());
  EXPECT_DOUBLE_EQ(mixed->confidence, 0.0);
}

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
  EXPECT_EQ(parsed->host_generation_id, "dispatcher-test-generation");
  EXPECT_NE(
      std::find(parsed->capabilities.begin(), parsed->capabilities.end(), "query_live_conversion"),
      parsed->capabilities.end());
  EXPECT_NE(
      std::find(parsed->capabilities.begin(), parsed->capabilities.end(), "query_predictions"),
      parsed->capabilities.end());
  for (const char* capability :
       {"app_profile", "candidate_tag", "list_models", "benchmark_model", "english_candidates",
        "learning_data_management", "commit_correction", "learning_reset", "persona",
        "detect_anomalies"}) {
    EXPECT_NE(std::find(parsed->capabilities.begin(), parsed->capabilities.end(), capability),
              parsed->capabilities.end())
        << capability;
  }

  ipc::HandshakeRequest bad = req;
  bad.protocol_version = 999;
  auto env2 = MakeReq(2, ipc::MessageType::Handshake, ipc::BuildHandshakeRequest(bad));
  auto resp2 = dispatcher.Dispatch(env2);
  ASSERT_TRUE(resp2.has_value());
  auto parsed2 = ipc::ParseHandshakeResponse(resp2->payload_json);
  ASSERT_TRUE(parsed2.has_value());
  EXPECT_FALSE(parsed2->accepted);
}

TEST_F(DispatcherTest, HandshakeIncludesTipRuntimeSettings) {
  const std::string settings_path = TempPath("azookey_dispatcher_batch_settings.json");
  std::remove(settings_path.c_str());
  {
    std::ofstream out(settings_path);
    ASSERT_TRUE(out.is_open());
    out << "{" << "\"batchRomajiConversion\":true," << "\"batchRomajiPreviewStyle\":\"romaji\","
        << "\"batchConversionMode\":\"neural\"," << "\"batchAutoPunctuation\":true,"
        << "\"numberRewriter\":true," << "\"katakanaRewriter\":true," << "\"maxCandidates\":17"
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
  EXPECT_TRUE(parsed->number_rewriter);
  EXPECT_TRUE(parsed->katakana_rewriter);
  EXPECT_EQ(parsed->max_candidates, 17u);

  std::remove(settings_path.c_str());
}

// DEV-1143: the settings app writes settings.json and only then sends
// UpdateConfig, so a TIP that re-handshakes on the file change must not be
// answered with the values the store loaded before that write. The reply reads
// the file without adopting it, so UpdateConfig still owns the reload.
TEST_F(DispatcherTest, HandshakeAnswersFromSettingsWrittenBeforeUpdateConfig) {
  const std::string settings_path = TempPath("azookey_dispatcher_reread_settings.json");
  std::remove(settings_path.c_str());
  const auto write = [&](const std::string& contents) {
    std::ofstream out(settings_path);
    ASSERT_TRUE(out.is_open());
    out << contents;
  };
  write(R"({"batchConversionMode":"neural","batchAutoPunctuation":false,"maxCandidates":17})");
  azookey::host::SettingsStore settings_store(settings_path);
  settings_store.Load();
  azookey::host::Dispatcher settings_dispatcher(&engine, &scheduler, &user_dict,
                                                DefaultDispatcherConfig(), &settings_store);

  ipc::HandshakeRequest req;
  req.tip_version = "0.1.0";
  req.protocol_version = kProtocolVersion;
  const auto handshake = [&](uint64_t id) {
    auto resp = settings_dispatcher.Dispatch(
        MakeReq(id, ipc::MessageType::Handshake, ipc::BuildHandshakeRequest(req)));
    return resp ? ipc::ParseHandshakeResponse(resp->payload_json) : std::nullopt;
  };

  auto before = handshake(3);
  ASSERT_TRUE(before.has_value());
  EXPECT_EQ(before->batch_conversion_mode, "neural");
  EXPECT_FALSE(before->batch_auto_punctuation);

  const auto loaded_stamp = std::filesystem::last_write_time(settings_path);
  write(R"({"batchConversionMode":"ai-cleanup","batchAutoPunctuation":true,"maxCandidates":21})");
  // Stamped explicitly so the assertion tests the freshness check rather than
  // how finely the filesystem happens to resolve two writes in a row.
  std::filesystem::last_write_time(settings_path, loaded_stamp + std::chrono::seconds(1));
  auto after = handshake(4);
  ASSERT_TRUE(after.has_value());
  EXPECT_EQ(after->batch_conversion_mode, "ai-cleanup");
  EXPECT_TRUE(after->batch_auto_punctuation);
  EXPECT_EQ(after->max_candidates, 21u);
  EXPECT_EQ(settings_store.settings().batch_conversion_mode, "neural");
  EXPECT_EQ(settings_store.settings().max_candidates, 17);

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

TEST_F(DispatcherTest, GeneratedTokenFileAuthenticatesHandshake) {
  const auto path = std::filesystem::path(TempPath("azookey_dispatcher_generated_token"));
  RemovePathNoThrow(path);
  const auto generated = ipc::GenerateHandshakeToken();
  ASSERT_TRUE(generated);
  ASSERT_TRUE(ipc::PublishHandshakeToken(path, *generated));
  const auto read = ipc::ReadHandshakeTokenFile(path);
  ASSERT_TRUE(read);

  auto config = DefaultDispatcherConfig();
  config.handshake_token = *generated;
  azookey::host::Dispatcher token_dispatcher(&engine, &scheduler, &user_dict, config);
  ipc::HandshakeRequest request;
  request.tip_version = "test";
  request.protocol_version = kProtocolVersion;
  request.handshake_token = *read;
  const auto response = token_dispatcher.Dispatch(
      MakeReq(6, ipc::MessageType::Handshake, ipc::BuildHandshakeRequest(request)));
  ASSERT_TRUE(response);
  const auto parsed = ipc::ParseHandshakeResponse(response->payload_json);
  ASSERT_TRUE(parsed);
  EXPECT_TRUE(parsed->accepted);
  RemovePathNoThrow(path);
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

  ipc::CommitCorrectionRequest correction;
  correction.kind = std::string(ipc::kCorrectionKindUndo);
  correction.reading = "かんじ";
  correction.rejected_surface = "幹事";
  correction.secure = false;
  correction.learning_allowed = true;
  const auto correction_before_handshake = token_dispatcher.Dispatch(MakeReq(
      7, ipc::MessageType::CommitCorrection, ipc::BuildCommitCorrectionRequest(correction)));
  ASSERT_TRUE(correction_before_handshake.has_value());
  const auto correction_before_payload =
      ipc::ParseCommitObservationResponse(correction_before_handshake->payload_json);
  ASSERT_TRUE(correction_before_payload.has_value());
  EXPECT_FALSE(correction_before_payload->ok);
  EXPECT_EQ(store.size(), 0u);

  // The approval messages say why they refused instead of looking like an
  // empty store.
  auto list_before_handshake =
      token_dispatcher.Dispatch(MakeReq(82, ipc::MessageType::ListNewWordCandidates, "{}"));
  ASSERT_TRUE(list_before_handshake.has_value());
  auto list_before_payload =
      ipc::ParseListNewWordCandidatesResponse(list_before_handshake->payload_json);
  ASSERT_TRUE(list_before_payload.has_value());
  EXPECT_FALSE(list_before_payload->ok);
  EXPECT_EQ(list_before_payload->error, "not_authenticated");
  EXPECT_TRUE(list_before_payload->items.empty());
  ipc::ResolveNewWordRequest resolve;
  resolve.surface = "azooKey";
  resolve.reading = "あずきー";
  resolve.action = "confirm";
  auto resolve_before_handshake = token_dispatcher.Dispatch(
      MakeReq(83, ipc::MessageType::ResolveNewWord, ipc::BuildResolveNewWordRequest(resolve)));
  ASSERT_TRUE(resolve_before_handshake.has_value());
  auto resolve_before_payload =
      ipc::ParseResolveNewWordResponse(resolve_before_handshake->payload_json);
  ASSERT_TRUE(resolve_before_payload.has_value());
  EXPECT_FALSE(resolve_before_payload->ok);
  EXPECT_EQ(resolve_before_payload->error, "not_authenticated");

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

  const auto predictions_request =
      MakeReq(85, ipc::MessageType::QueryPredictions,
              ipc::BuildQueryPredictionsRequest({"にほん", "", "word"}));
  const auto predictions_before = token_dispatcher.Dispatch(predictions_request);
  ASSERT_TRUE(predictions_before);
  EXPECT_EQ(predictions_before->request_id, predictions_request.request_id);
  const auto predictions_before_payload =
      ipc::ParseQueryPredictionsResponse(predictions_before->payload_json);
  ASSERT_TRUE(predictions_before_payload);
  EXPECT_FALSE(predictions_before_payload->ok);
  EXPECT_EQ(predictions_before_payload->error, "not authenticated");
  EXPECT_TRUE(predictions_before_payload->predictions.empty());
  const auto diagnostics_before =
      token_dispatcher.Dispatch(MakeReq(86, ipc::MessageType::QueryDiagnostics, "{}"));
  ASSERT_TRUE(diagnostics_before.has_value());
  const auto diagnostics_before_payload =
      ipc::ParseQueryDiagnostics(diagnostics_before->payload_json);
  ASSERT_TRUE(diagnostics_before_payload.has_value());
  EXPECT_EQ(diagnostics_before_payload->fallback_state, "healthy");
  EXPECT_EQ(diagnostics_before_payload->last_error, "not authenticated");

  user_dict.Add({"明日", "あした"});
  const auto reverse_request =
      MakeReq(84, ipc::MessageType::ReverseConvert, ipc::BuildReverseConvertRequest({"明日"}));
  const auto reverse_after_wrong = token_dispatcher.Dispatch(reverse_request);
  ASSERT_TRUE(reverse_after_wrong);
  EXPECT_EQ(reverse_after_wrong->type, reverse_request.type);
  EXPECT_EQ(reverse_after_wrong->request_id, reverse_request.request_id);
  EXPECT_EQ(reverse_after_wrong->trace_id, reverse_request.trace_id);
  const auto reverse_before_payload =
      ipc::ParseReverseConvertResponse(reverse_after_wrong->payload_json);
  ASSERT_TRUE(reverse_before_payload);
  EXPECT_TRUE(reverse_before_payload->reading.empty());
  EXPECT_DOUBLE_EQ(reverse_before_payload->confidence, 0.0);

  req.handshake_token = "expected-token";
  auto matched = token_dispatcher.Dispatch(
      MakeReq(9, ipc::MessageType::Handshake, ipc::BuildHandshakeRequest(req)));
  ASSERT_TRUE(matched.has_value());
  auto matched_payload = ipc::ParseHandshakeResponse(matched->payload_json);
  ASSERT_TRUE(matched_payload.has_value());
  EXPECT_TRUE(matched_payload->accepted);

  const auto predictions_after = token_dispatcher.Dispatch(predictions_request);
  ASSERT_TRUE(predictions_after);
  const auto predictions_after_payload =
      ipc::ParseQueryPredictionsResponse(predictions_after->payload_json);
  ASSERT_TRUE(predictions_after_payload);
  EXPECT_TRUE(predictions_after_payload->ok);
  EXPECT_FALSE(predictions_after_payload->predictions.empty());

  auto add_after_handshake = token_dispatcher.Dispatch(
      MakeReq(10, ipc::MessageType::AddUserWord, ipc::BuildAddUserWordRequest(add)));
  ASSERT_TRUE(add_after_handshake.has_value());
  auto add_payload = ipc::ParseAddUserWordResponse(add_after_handshake->payload_json);
  ASSERT_TRUE(add_payload.has_value());
  EXPECT_TRUE(add_payload->ok);
  EXPECT_EQ(user_dict.Lookup("あずきい").size(), 1u);

  // AddUserWord reloads the persisted dictionary and drops the in-memory word
  // inserted before the handshake; restore it for the authenticated check.
  user_dict.Add({"明日", "あした"});
  const auto reverse_after_handshake = token_dispatcher.Dispatch(reverse_request);
  ASSERT_TRUE(reverse_after_handshake);
  const auto reverse_after_payload =
      ipc::ParseReverseConvertResponse(reverse_after_handshake->payload_json);
  ASSERT_TRUE(reverse_after_payload);
  EXPECT_EQ(reverse_after_payload->reading, "あした");
  EXPECT_DOUBLE_EQ(reverse_after_payload->confidence, 1.0);
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

TEST_F(DispatcherTest, SharedEnvelopeFixtureDecodesOnHostSide) {
  const auto fixture = azookey::ipc::test::ReadTextFixture("ping-envelope.json");
  const auto request = ipc::Deserialize(fixture);
  ASSERT_TRUE(request.has_value());

  const auto response = dispatcher.Dispatch(*request);
  ASSERT_TRUE(response.has_value());
  EXPECT_EQ(response->request_id, 42u);
  EXPECT_EQ(response->trace_id, "shared-fixture");
  EXPECT_EQ(response->type, ipc::MessageType::Ping);

  const auto payload = ipc::ParsePing(response->payload_json);
  ASSERT_TRUE(payload.has_value());
  EXPECT_EQ(payload->nonce, 4242u);
  EXPECT_GT(payload->t_ms, 0u);
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

namespace {

// Fixed, score-ordered output so the M48 tests see only the tag boost move it.
class FixedCandidatesConverter final : public azookey::core::IConverter {
 public:
  std::vector<azookey::core::Candidate> Convert(const std::string& kana,
                                                const azookey::core::ConversionContext&) override {
    azookey::core::Candidate technical{"技術", kana, 5.0};
    technical.tag = azookey::core::CandidateTag::Technical;
    return {{"日本", kana, 10.0}, {"二本", kana, 8.0}, {"Nihon", kana, 6.0}, technical};
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

class AppProfileDispatchTest : public ::testing::Test {
 protected:
  AppProfileDispatchTest()
      // Per-test name: CTest runs each TEST_F in its own process, possibly in
      // parallel, so a shared file would be rewritten under another test.
      : settings_path(TempPath(
            ("azookey_dispatcher_app_profile_" +
             std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()) + ".json")
                .c_str())),
        engine(std::make_unique<FixedCandidatesConverter>(), nullptr, {}) {}
  ~AppProfileDispatchTest() override { std::remove(settings_path.c_str()); }

  // Writes settings.json and publishes it through UpdateConfig, as the
  // settings app does.
  void ApplySettings(const std::string& json) {
    {
      std::ofstream out(settings_path, std::ios::binary | std::ios::trunc);
      ASSERT_TRUE(out.is_open());
      out << json;
    }
    settings_store.emplace(settings_path);
    settings_store->Load();
    dispatcher.emplace(&engine, &scheduler, nullptr, DefaultDispatcherConfig(), &*settings_store);
    ipc::Envelope update;
    update.version = 1;
    update.request_id = 1;
    update.type = ipc::MessageType::UpdateConfig;
    update.payload_json = "{}";
    const auto response = dispatcher->Dispatch(update);
    ASSERT_TRUE(response.has_value());
    const auto parsed = ipc::ParseUpdateConfigResponse(response->payload_json);
    ASSERT_TRUE(parsed.has_value());
    ASSERT_TRUE(parsed->ok);
  }

  std::vector<ipc::CandidateField> Query(std::optional<ipc::AppIdentity> app) {
    ipc::QueryCandidatesRequest q;
    q.reading = "にほん";
    q.app = std::move(app);
    ipc::Envelope env;
    env.version = 1;
    env.request_id = ++next_id;
    env.type = ipc::MessageType::QueryCandidates;
    env.payload_json = ipc::BuildQueryCandidatesRequest(q);
    const auto response = dispatcher->Dispatch(env);
    EXPECT_TRUE(response.has_value());
    const auto parsed =
        response ? ipc::ParseQueryCandidatesResponse(response->payload_json) : std::nullopt;
    EXPECT_TRUE(parsed.has_value());
    return parsed ? parsed->candidates : std::vector<ipc::CandidateField>{};
  }

  // Position of surface, or candidates.size() when absent.
  static size_t IndexOf(const std::vector<ipc::CandidateField>& candidates,
                        const std::string& surface) {
    return static_cast<size_t>(
        std::find_if(candidates.begin(), candidates.end(),
                     [&](const ipc::CandidateField& c) { return c.surface == surface; }) -
        candidates.begin());
  }

  static void ExpectGlobalOrder(const std::vector<ipc::CandidateField>& candidates) {
    ASSERT_LT(IndexOf(candidates, "Nihon"), candidates.size());
    EXPECT_LT(IndexOf(candidates, "日本"), IndexOf(candidates, "二本"));
    EXPECT_LT(IndexOf(candidates, "二本"), IndexOf(candidates, "Nihon"));
  }

  std::string settings_path;
  azookey::host::InferenceEngine engine;
  azookey::host::RequestScheduler scheduler;
  std::optional<azookey::host::SettingsStore> settings_store;
  std::optional<azookey::host::Dispatcher> dispatcher;
  uint64_t next_id = 100;
};

constexpr const char* kCodeProfileSettings =
    R"({"profilesByApp":{"code.exe":{"candidateTagBoosts":{"English":3.0}}}})";

}  // namespace

TEST_F(AppProfileDispatchTest, CandidateTagBoostsReorderCandidatesForTheMatchingApp) {
  ApplySettings(kCodeProfileSettings);
  const auto boosted = Query(ipc::AppIdentity{"Code.exe", "Chrome_WidgetWin_1"});
  ASSERT_LT(IndexOf(boosted, "Nihon"), boosted.size());
  // 6.0 x 3.0 overtakes 10.0; the untagged candidates keep their order.
  EXPECT_LT(IndexOf(boosted, "Nihon"), IndexOf(boosted, "日本"));
  EXPECT_LT(IndexOf(boosted, "日本"), IndexOf(boosted, "二本"));
  EXPECT_EQ(boosted[IndexOf(boosted, "Nihon")].tag,
            static_cast<uint8_t>(azookey::core::CandidateTag::English));
}

TEST_F(AppProfileDispatchTest, RequestsWithoutAppKeepTheGlobalOrder) {
  ApplySettings(kCodeProfileSettings);
  ExpectGlobalOrder(Query(std::nullopt));
  ExpectGlobalOrder(Query(ipc::AppIdentity{"notepad.exe", "Notepad"}));
}

TEST_F(AppProfileDispatchTest, TheDefaultProfileDoesNotApplyWithoutAResolvedApp) {
  ApplySettings(R"({"profilesByApp":{"default":{"candidateTagBoosts":{"English":3.0}}}})");
  ExpectGlobalOrder(Query(std::nullopt));
  ExpectGlobalOrder(Query(ipc::AppIdentity{"", "Chrome_WidgetWin_1"}));
  // A resolved app with no profile of its own does get the default profile.
  const auto resolved = Query(ipc::AppIdentity{"notepad.exe", ""});
  EXPECT_LT(IndexOf(resolved, "Nihon"), IndexOf(resolved, "日本"));
}

TEST_F(AppProfileDispatchTest, InvalidProfileValuesFallBackWithoutFailingTheQuery) {
  ApplySettings(
      R"({"profilesByApp":{"code.exe":{"candidateTagBoosts":{"English":"high","Unknown":2.0},)"
      R"("style":"shouting"},"bad.exe":7}})");
  ExpectGlobalOrder(Query(ipc::AppIdentity{"code.exe", ""}));
  ExpectGlobalOrder(Query(ipc::AppIdentity{"bad.exe", ""}));
}

TEST_F(DispatcherTest, QueryCandidatesAddsEnglishCandidatesOnlyWhenAsked) {
  const auto query = [&](uint64_t id, bool english) {
    ipc::QueryCandidatesRequest q;
    q.reading = "にほん";
    q.raw_romaji = "nihon";
    q.english_candidates = english;
    const auto resp = dispatcher.Dispatch(
        MakeReq(id, ipc::MessageType::QueryCandidates, ipc::BuildQueryCandidatesRequest(q)));
    EXPECT_TRUE(resp.has_value());
    const auto parsed = resp ? ipc::ParseQueryCandidatesResponse(resp->payload_json) : std::nullopt;
    EXPECT_TRUE(parsed.has_value());
    return parsed ? parsed->candidates : std::vector<ipc::CandidateField>{};
  };
  constexpr auto kEnglish = static_cast<uint8_t>(azookey::core::CandidateTag::English);
  const auto has_english = [&](const std::vector<ipc::CandidateField>& candidates) {
    return std::any_of(candidates.begin(), candidates.end(),
                       [&](const auto& c) { return c.tag == kEnglish; });
  };
  // Older TIPs (no english_candidates) see exactly what they saw before.
  EXPECT_FALSE(has_english(query(901, false)));

  const auto with = query(902, true);
  ASSERT_FALSE(with.empty());
  EXPECT_EQ(with.front().surface, "日本");  // English never takes the first slot.
  std::vector<std::string> english;
  for (const auto& c : with) {
    if (c.tag != kEnglish) continue;
    english.push_back(c.surface);
    EXPECT_EQ(c.reading, "nihon");
    EXPECT_EQ(c.source, "heuristic");
  }
  EXPECT_EQ(english, (std::vector<std::string>{"nihon", "Nihon", "NIHON"}));

  // inlineEnglishDictionary on but no file: the baseline forms still come.
  auto config = engine.config();
  config.english.dictionary_enabled = true;
  config.english.dictionary_path = TempPath("azookey_dispatcher_missing_english_words.tsv");
  std::remove(config.english.dictionary_path.c_str());
  engine.ApplyConfig(config);
  std::vector<std::string> without_dictionary;
  for (const auto& c : query(903, true)) {
    if (c.tag == kEnglish) without_dictionary.push_back(c.surface);
  }
  EXPECT_EQ(without_dictionary, english);
}

TEST_F(DispatcherTest, EnglishCommitsNeverReachTheKanaLearningStore) {
  azookey::learning::LearningStore english_store(learning_path + ".english",
                                                 &azookey::learning::test::Crypto());
  engine.SetEnglishLearningStore(&english_store);
  EnableEventPrivacy(dispatcher);
  ipc::CommitObservationRequest commit;
  commit.secure = false;
  commit.learning_allowed = true;
  commit.reading = "Nihon";
  commit.chosen = {"Nihon", "Nihon", 0.0, "heuristic", "", 4};
  commit.timestamp_ms = 1700000000000ULL;
  const auto response = dispatcher.Dispatch(MakeReq(911, ipc::MessageType::CommitObservation,
                                                    ipc::BuildCommitObservationRequest(commit)));
  ASSERT_TRUE(response.has_value());
  const auto parsed = ipc::ParseCommitObservationResponse(response->payload_json);
  ASSERT_TRUE(parsed && parsed->ok);
  const auto now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                             std::chrono::system_clock::now().time_since_epoch())
                                             .count());
  EXPECT_GT(english_store.Score("nihon", "Nihon", now), 0.0);
  EXPECT_DOUBLE_EQ(store.Score("Nihon", "Nihon", now), 0.0);
  EXPECT_DOUBLE_EQ(store.Score("nihon", "Nihon", now), 0.0);

  // The English tag on a kana reading (a dictionary iPhone) stays kana learning.
  commit.reading = "あいふぉん";
  commit.chosen = {"iPhone", "あいふぉん", 0.0, "system", "", 4};
  ASSERT_TRUE(dispatcher.Dispatch(MakeReq(912, ipc::MessageType::CommitObservation,
                                          ipc::BuildCommitObservationRequest(commit))));
  EXPECT_GT(store.Score("あいふぉん", "iPhone", now), 0.0);
  EXPECT_DOUBLE_EQ(english_store.Score("あいふぉん", "iPhone", now), 0.0);
  engine.SetEnglishLearningStore(nullptr);
  std::remove((learning_path + ".english").c_str());
}

TEST_F(DispatcherTest, EnglishLearningHonorsPrivacyAndRoutesSegments) {
  azookey::learning::LearningStore english_store(learning_path + ".english2",
                                                 &azookey::learning::test::Crypto());
  engine.SetEnglishLearningStore(&english_store);
  EnableEventPrivacy(dispatcher);
  const auto now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                             std::chrono::system_clock::now().time_since_epoch())
                                             .count());
  // A secure field, or learning not allowed, records nothing in either channel.
  for (const auto& [secure, allowed] : {std::pair{true, true}, std::pair{false, false}}) {
    ipc::CommitObservationRequest commit;
    commit.secure = secure;
    commit.learning_allowed = allowed;
    commit.reading = "secret";
    commit.chosen = {"Secret", "secret", 0.0, "heuristic", "", 4};
    ASSERT_TRUE(dispatcher.Dispatch(MakeReq(921, ipc::MessageType::CommitObservation,
                                            ipc::BuildCommitObservationRequest(commit))));
  }
  EXPECT_EQ(english_store.size(), 0u);

  ipc::CommitSegmentsObservationRequest segments;
  segments.secure = false;
  segments.learning_allowed = true;
  ipc::ObservedSegment kana;
  kana.reading = "きょう";
  kana.chosen = {"今日", "きょう", 0.0, "system"};
  ipc::ObservedSegment english;
  english.reading = "github";
  english.chosen = {"GitHub", "github", 0.0, "heuristic", "", 4};
  segments.segments = {kana, english};
  ASSERT_TRUE(dispatcher.Dispatch(MakeReq(922, ipc::MessageType::CommitSegmentsObservation,
                                          ipc::BuildCommitSegmentsObservationRequest(segments))));
  EXPECT_GT(english_store.Score("github", "GitHub", now), 0.0);
  EXPECT_DOUBLE_EQ(store.Score("github", "GitHub", now), 0.0);
  EXPECT_GT(store.Score("きょう", "今日", now), 0.0);
  engine.SetEnglishLearningStore(nullptr);
  std::remove((learning_path + ".english2").c_str());
}

TEST_F(AppProfileDispatchTest, StyleImpliesABoostForItsTag) {
  ApplySettings(R"({"profilesByApp":{"code.exe":{"style":"technical"}}})");
  const auto plain = Query(ipc::AppIdentity{"notepad.exe", ""});
  EXPECT_LT(IndexOf(plain, "Nihon"), IndexOf(plain, "技術"));
  // 5.0 x 1.5 = 7.5 passes Nihon (6.0) but not 二本 (8.0); English stays put.
  const auto technical = Query(ipc::AppIdentity{"code.exe", ""});
  ASSERT_LT(IndexOf(technical, "技術"), technical.size());
  EXPECT_LT(IndexOf(technical, "技術"), IndexOf(technical, "Nihon"));
  EXPECT_LT(IndexOf(technical, "二本"), IndexOf(technical, "技術"));
  EXPECT_EQ(technical[IndexOf(technical, "技術")].tag,
            static_cast<uint8_t>(azookey::core::CandidateTag::Technical));
}

TEST_F(DispatcherTest, ListModelsScansOnlyTheModelsDirectory) {
  const auto root = std::filesystem::temp_directory_path() / "azookey_dispatcher_list_models";
  RemovePathNoThrow(root);
  std::filesystem::create_directories(root / "models");
  {
    std::ofstream out(root / "models" / "broken.gguf", std::ios::binary);
    out << "NOPE";
  }
  auto config = DefaultDispatcherConfig();
  config.models_dir = root / "models";
  azookey::host::Dispatcher models_dispatcher(&engine, &scheduler, &user_dict, config);

  const auto list = [&](uint64_t id, const ipc::ListModelsRequest& request) {
    const auto response = models_dispatcher.Dispatch(
        MakeReq(id, ipc::MessageType::ListModels, ipc::BuildListModelsRequest(request)));
    EXPECT_TRUE(response.has_value());
    if (response) EXPECT_EQ(response->type, ipc::MessageType::ListModels);
    return response ? ipc::ParseListModelsResponse(response->payload_json) : std::nullopt;
  };
  const auto listed = list(801, {});
  ASSERT_TRUE(listed && listed->ok);
  ASSERT_EQ(listed->models.size(), 1u);
  EXPECT_EQ(listed->models[0].file_name, "broken.gguf");
  EXPECT_FALSE(listed->models[0].valid);
  EXPECT_EQ(listed->models[0].last_error, "magic_mismatch");
  EXPECT_EQ(listed->models[0].last_load_status, "not_loaded");

  ipc::ListModelsRequest outside;
  outside.directory = azookey::core::PathToUtf8(root);
  const auto rejected = list(802, outside);
  ASSERT_TRUE(rejected);
  EXPECT_FALSE(rejected->ok);
  EXPECT_EQ(rejected->error, "directory_outside_models_root");

  // Without a configured models directory the Host refuses rather than guessing.
  const auto unconfigured = dispatcher.Dispatch(MakeReq(803, ipc::MessageType::ListModels, "{}"));
  ASSERT_TRUE(unconfigured.has_value());
  const auto unconfigured_parsed = ipc::ParseListModelsResponse(unconfigured->payload_json);
  ASSERT_TRUE(unconfigured_parsed);
  EXPECT_FALSE(unconfigured_parsed->ok);
  EXPECT_EQ(unconfigured_parsed->error, "models_dir_unavailable");
  RemovePathNoThrow(root);
}

#if !AZOOKEY_WITH_LLAMA_CPP
// The header-only GGUF loads only in the probe-only (no llama.cpp) build.
TEST_F(DispatcherTest, ListModelsReportsTheModelTheLiveEngineLoaded) {
  const auto root = std::filesystem::temp_directory_path() / "azookey_dispatcher_list_loaded";
  RemovePathNoThrow(root);
  std::filesystem::create_directories(root);
  const auto model = root / "live.gguf";
  {
    // magic, v3, one tensor, one key: general.architecture = "gpt2".
    std::string bytes = "GGUF";
    const auto u32 = [&](uint32_t v) {
      for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    };
    const auto u64 = [&](uint64_t v) {
      for (int i = 0; i < 8; ++i) bytes.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    };
    u32(3);
    u64(1);
    u64(1);
    const std::string key = "general.architecture";
    u64(key.size());
    bytes += key;
    u32(8);
    u64(4);
    bytes += "gpt2";
    std::ofstream out(model, std::ios::binary);
    out << bytes;
  }
  azookey::host::ModelLoadOptions load;
  load.path = azookey::core::PathToUtf8(model);
  ASSERT_TRUE(engine.LoadModelWithResult(load).ok);
  auto config = DefaultDispatcherConfig();
  config.models_dir = root;
  azookey::host::Dispatcher models_dispatcher(&engine, &scheduler, &user_dict, config);
  const auto response =
      models_dispatcher.Dispatch(MakeReq(804, ipc::MessageType::ListModels, "{}"));
  ASSERT_TRUE(response.has_value());
  const auto parsed = ipc::ParseListModelsResponse(response->payload_json);
  ASSERT_TRUE(parsed && parsed->models.size() == 1u);
  EXPECT_TRUE(parsed->models[0].valid) << parsed->models[0].last_error;
  EXPECT_EQ(parsed->models[0].last_load_status, "success");
  EXPECT_EQ(parsed->models[0].metadata.model_family, "gpt2");
  RemovePathNoThrow(root);
}
#endif

TEST_F(DispatcherTest, BenchmarkModelRejectsInvalidRequestsWithoutTouchingTheLiveEngine) {
  const bool loaded_before = engine.model_loaded();
  const auto bench = [&](uint64_t id, const std::string& payload) {
    const auto response =
        dispatcher.Dispatch(MakeReq(id, ipc::MessageType::BenchmarkModel, payload));
    EXPECT_TRUE(response.has_value());
    return response ? ipc::ParseBenchmarkModelResponse(response->payload_json) : std::nullopt;
  };
  const auto malformed = bench(811, "{}");
  ASSERT_TRUE(malformed);
  EXPECT_EQ(malformed->status, "error");
  EXPECT_EQ(malformed->error, "invalid_request");
  const auto unconfigured = bench(812, R"({"path":"definitely-missing.gguf"})");
  ASSERT_TRUE(unconfigured);
  EXPECT_EQ(unconfigured->status, "error");
  EXPECT_EQ(unconfigured->error, "models_dir_unavailable");

  const auto root = std::filesystem::temp_directory_path() / "azookey_dispatcher_benchmark";
  RemovePathNoThrow(root);
  std::filesystem::create_directories(root / "models");
  {
    std::ofstream out(root / "models" / "broken.gguf", std::ios::binary);
    out << "NOPE";
    std::ofstream outside(root / "outside.gguf", std::ios::binary);
    outside << "NOPE";
  }
  auto config = DefaultDispatcherConfig();
  config.models_dir = root / "models";
  azookey::host::Dispatcher models_dispatcher(&engine, &scheduler, &user_dict, config);
  const auto bench_in = [&](uint64_t id, const std::filesystem::path& path) {
    ipc::BenchmarkModelRequest request;
    request.path = azookey::core::PathToUtf8(path);
    const auto response = models_dispatcher.Dispatch(
        MakeReq(id, ipc::MessageType::BenchmarkModel, ipc::BuildBenchmarkModelRequest(request)));
    EXPECT_TRUE(response.has_value());
    return response ? ipc::ParseBenchmarkModelResponse(response->payload_json) : std::nullopt;
  };
  // Like ListModels, only paths under the models directory are accepted.
  const auto outside = bench_in(813, root / "outside.gguf");
  ASSERT_TRUE(outside);
  EXPECT_EQ(outside->error, "path_outside_models_root");
  const auto broken = bench_in(814, root / "models" / "broken.gguf");
  ASSERT_TRUE(broken);
  EXPECT_EQ(broken->error, "invalid_model");
  {
    // A second benchmark while one runs is refused rather than queued.
    const auto running = azookey::host::TryAcquireBenchmarkSlot();
    ASSERT_TRUE(running.owns_lock());
    const auto busy = bench_in(815, root / "models" / "broken.gguf");
    ASSERT_TRUE(busy);
    EXPECT_EQ(busy->status, "error");
    EXPECT_EQ(busy->error, "busy");
  }
  EXPECT_EQ(engine.model_loaded(), loaded_before);
  RemovePathNoThrow(root);
}

TEST(DispatcherTraceTest, CorrelatesHostPhasesWithoutLoggingInput) {
  const auto log_dir = std::filesystem::temp_directory_path() / "azookey_host_trace_phase_test";
  RemovePathNoThrow(log_dir);
  azookey::logging::RuntimeLoggerOptions options;
  options.enabled = true;
  options.component = "host";
  options.logs_directory = log_dir;
  azookey::logging::RuntimeLogger logger(options);
  azookey::host::InferenceEngine engine(std::make_unique<azookey::core::SimpleConverter>(), nullptr,
                                        {}, &logger);
  azookey::host::RequestScheduler scheduler;
  azookey::host::Dispatcher dispatcher(&engine, &scheduler, nullptr, DefaultDispatcherConfig(),
                                       nullptr, nullptr, &logger);

  ipc::Envelope request;
  request.version = 1;
  request.request_id = 42;
  request.trace_id = "018fd2c2-2a3e-7c9a-b8e1-7f3a92d4c5e2";
  request.type = ipc::MessageType::QueryCandidates;
  ipc::QueryCandidatesRequest query;
  query.reading = "にほん";
  query.max_candidates = 10;
  request.payload_json = ipc::BuildQueryCandidatesRequest(query);
  const auto response = dispatcher.Dispatch(request);
  ASSERT_TRUE(response);
  EXPECT_EQ(response->trace_id, request.trace_id);

  std::vector<std::string> phases;
  ASSERT_TRUE(std::filesystem::exists(log_dir));
  for (const auto& file : std::filesystem::directory_iterator(log_dir)) {
    if (file.path().extension() != ".jsonl") continue;
    std::ifstream stream(file.path());
    for (std::string line; std::getline(stream, line);) {
      const auto record = ipc::json::Parse(line);
      ASSERT_TRUE(record);
      if (record->GetString("event") != "trace_phase") continue;
      EXPECT_EQ(record->GetString("trace_id"), request.trace_id);
      EXPECT_EQ(record->GetUInt("request_id"), request.request_id);
      ASSERT_TRUE(record->GetNumber("latency_ms"));
      EXPECT_GE(*record->GetNumber("latency_ms"), 0.0);
      EXPECT_EQ(record->GetString("result"), "ok");
      EXPECT_EQ(line.find(query.reading), std::string::npos);
      ASSERT_TRUE(record->GetString("phase"));
      if (record->GetString("phase") == "model_inference")
        EXPECT_EQ(record->GetString("backend"), "cpu");
      phases.push_back(*record->GetString("phase"));
    }
  }
  EXPECT_NE(std::find(phases.begin(), phases.end(), "host_queue_wait"), phases.end());
  EXPECT_NE(std::find(phases.begin(), phases.end(), "model_inference"), phases.end());
  EXPECT_NE(std::find(phases.begin(), phases.end(), "rerank"), phases.end());
  RemovePathNoThrow(log_dir);
}

TEST(DispatcherTraceTest, InvalidClientTraceIdNeverEntersHostPhaseLog) {
  const auto log_dir = std::filesystem::temp_directory_path() / "azookey_host_invalid_trace_test";
  RemovePathNoThrow(log_dir);
  azookey::logging::RuntimeLoggerOptions options;
  options.enabled = true;
  options.component = "host";
  options.logs_directory = log_dir;
  azookey::logging::RuntimeLogger logger(options);
  azookey::host::InferenceEngine engine(std::make_unique<azookey::core::SimpleConverter>(), nullptr,
                                        {}, &logger);
  azookey::host::RequestScheduler scheduler;
  azookey::host::Dispatcher dispatcher(&engine, &scheduler, nullptr, DefaultDispatcherConfig(),
                                       nullptr, nullptr, &logger);

  ipc::QueryCandidatesRequest query;
  query.reading = "にほん";
  query.max_candidates = 10;
  for (const auto& invalid : {std::string("private input\nsecond line"), std::string(1024, 'x'),
                              std::string("018fd2c2-2a3e-4c9a-b8e1-7f3a92d4c5e2")}) {
    ipc::Envelope request;
    request.version = 1;
    request.request_id = 42;
    request.trace_id = invalid;
    request.type = ipc::MessageType::QueryCandidates;
    request.payload_json = ipc::BuildQueryCandidatesRequest(query);
    const auto response = dispatcher.Dispatch(request);
    ASSERT_TRUE(response);
    EXPECT_EQ(response->trace_id, invalid);
  }

  if (std::filesystem::exists(log_dir)) {
    for (const auto& file : std::filesystem::directory_iterator(log_dir)) {
      if (file.path().extension() != ".jsonl") continue;
      std::ifstream stream(file.path());
      for (std::string line; std::getline(stream, line);) {
        EXPECT_EQ(line.find("private input"), std::string::npos);
        EXPECT_EQ(line.find(std::string(1024, 'x')), std::string::npos);
        EXPECT_EQ(line.find("018fd2c2-2a3e-4c9a-b8e1-7f3a92d4c5e2"), std::string::npos);
        const auto record = ipc::json::Parse(line);
        ASSERT_TRUE(record);
        EXPECT_NE(record->GetString("event"), "trace_phase");
      }
    }
  }
  RemovePathNoThrow(log_dir);
}

TEST_F(DispatcherTest, QueryLiveConversionReturnsBestSurface) {
  ipc::QueryLiveConversionRequest query{"にほん", ""};
  const auto request = MakeReq(21, ipc::MessageType::QueryLiveConversion,
                               ipc::BuildQueryLiveConversionRequest(query));
  const auto response = dispatcher.Dispatch(request);
  ASSERT_TRUE(response);
  EXPECT_EQ(response->type, ipc::MessageType::QueryLiveConversion);
  EXPECT_EQ(response->request_id, request.request_id);
  EXPECT_EQ(response->trace_id, request.trace_id);
  const auto parsed = ipc::ParseQueryLiveConversionResponse(response->payload_json);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->surface, "日本");
  EXPECT_GE(parsed->confidence, 0.0);
  EXPECT_LE(parsed->confidence, 1.0);
}

// DEV-1442 / dynamic-punctuation-spec section 7: the Host inserts punctuation
// into the live conversion surface only when the request asks for it and both
// liveConversion and dynamicPunctuation are on, and flags the inserted mark as
// a segment without a reading. The legacy QueryCandidates live=true carrier
// follows the same rule.
TEST_F(DispatcherTest, QueryLiveConversionInsertsPunctuationOnlyWhenAskedAndEnabled) {
  // Long enough, and ending in "です", for the sentence-final rule.
  const std::string kana = "きょうはいいてんきです";
  const auto live = [&](uint64_t id, bool auto_punctuation) {
    ipc::QueryLiveConversionRequest request{kana, ""};
    request.auto_punctuation = auto_punctuation;
    const auto response = dispatcher.Dispatch(MakeReq(
        id, ipc::MessageType::QueryLiveConversion, ipc::BuildQueryLiveConversionRequest(request)));
    EXPECT_TRUE(response.has_value());
    return response ? ipc::ParseQueryLiveConversionResponse(response->payload_json) : std::nullopt;
  };
  const auto ends_with_period = [](const std::string& text) {
    const std::string period = "。";
    return text.size() > period.size() &&
           text.compare(text.size() - period.size(), period.size(), period) == 0;
  };

  // The setting is off by default: the request flag alone changes nothing.
  const auto off = live(1401, true);
  ASSERT_TRUE(off);
  const auto plain = off->surface;
  EXPECT_FALSE(ends_with_period(plain));
  EXPECT_TRUE(off->segments.empty());

  auto config = engine.config();
  config.dynamic_punctuation = true;
  config.punctuation_rules_path = "";
  engine.ApplyConfig(config);

  const auto on = live(1402, true);
  ASSERT_TRUE(on);
  EXPECT_EQ(on->surface, plain + "。");
  ASSERT_GE(on->segments.size(), 2u);
  EXPECT_FALSE(on->segments.front().auto_punctuation);
  EXPECT_EQ(on->segments.front().reading, kana);
  EXPECT_TRUE(on->segments.back().auto_punctuation);
  EXPECT_EQ(on->segments.back().surface, "。");
  EXPECT_TRUE(on->segments.back().reading.empty());
  std::string joined;
  for (const auto& segment : on->segments) joined += segment.surface;
  EXPECT_EQ(joined, on->surface);

  // onPause typing sends false (section 7.1.1): no punctuation, no segments.
  const auto typing = live(1403, false);
  ASSERT_TRUE(typing);
  EXPECT_EQ(typing->surface, plain);
  EXPECT_TRUE(typing->segments.empty());

  // The legacy carrier, for a Host that does not advertise query_live_conversion.
  ipc::QueryCandidatesRequest legacy;
  legacy.reading = kana;
  legacy.live = true;
  legacy.auto_punctuation = true;
  const auto legacy_response = dispatcher.Dispatch(
      MakeReq(1404, ipc::MessageType::QueryCandidates, ipc::BuildQueryCandidatesRequest(legacy)));
  ASSERT_TRUE(legacy_response);
  const auto legacy_parsed = ipc::ParseQueryCandidatesResponse(legacy_response->payload_json);
  ASSERT_TRUE(legacy_parsed && !legacy_parsed->candidates.empty());
  EXPECT_EQ(legacy_parsed->candidates.front().surface, plain + "。");
  ASSERT_FALSE(legacy_parsed->segments.empty());
  EXPECT_TRUE(legacy_parsed->segments.back().auto_punctuation);

  // liveConversion off disables the feature even with dynamicPunctuation on.
  config.enable_live_conversion = false;
  engine.ApplyConfig(config);
  const auto disabled = live(1405, true);
  ASSERT_TRUE(disabled);
  EXPECT_EQ(disabled->surface, plain);
  EXPECT_TRUE(disabled->segments.empty());
}

TEST_F(DispatcherTest, QueryLiveConversionDoesNotSaturateUserWordScore) {
  azookey::learning::UserWord word;
  word.word = "独自語";
  word.ruby = "どくじご";
  word.value = 1.5;
  ASSERT_TRUE(user_dict.Add(word));
  const auto response =
      dispatcher.Dispatch(MakeReq(23, ipc::MessageType::QueryLiveConversion,
                                  ipc::BuildQueryLiveConversionRequest({word.ruby, ""})));
  ASSERT_TRUE(response);
  const auto parsed = ipc::ParseQueryLiveConversionResponse(response->payload_json);
  ASSERT_TRUE(parsed);
  ASSERT_EQ(parsed->surface, word.word);
  EXPECT_GT(parsed->confidence, 0.5);
  EXPECT_LT(parsed->confidence, 1.0);
}

TEST_F(DispatcherTest, QueryLiveConversionSuppressesPreCanceledReply) {
  scheduler.Cancel(22);
  ipc::QueryLiveConversionRequest query{"わたし", ""};
  const auto response = dispatcher.Dispatch(MakeReq(22, ipc::MessageType::QueryLiveConversion,
                                                    ipc::BuildQueryLiveConversionRequest(query)));
  EXPECT_FALSE(response);
  EXPECT_FALSE(scheduler.IsCanceled(22));
}

TEST_F(DispatcherTest, QueryPredictionsReturnsWordCandidatesAndCorrelatesEnvelope) {
  const ipc::QueryPredictionsRequest query{"にほん", "私は", "word"};
  const auto request =
      MakeReq(24, ipc::MessageType::QueryPredictions, ipc::BuildQueryPredictionsRequest(query));
  const auto response = dispatcher.Dispatch(request);
  ASSERT_TRUE(response);
  EXPECT_EQ(response->type, request.type);
  EXPECT_EQ(response->request_id, request.request_id);
  EXPECT_EQ(response->trace_id, request.trace_id);
  const auto parsed = ipc::ParseQueryPredictionsResponse(response->payload_json);
  ASSERT_TRUE(parsed);
  EXPECT_TRUE(parsed->ok);
  ASSERT_FALSE(parsed->predictions.empty());
  EXPECT_EQ(parsed->predictions.front().surface, "日本");
  EXPECT_EQ(parsed->predictions.front().reading, "にほん");
}

TEST_F(DispatcherTest, QueryPredictionsRejectsUnsupportedModeAndMalformedRequest) {
  ipc::QueryPredictionsRequest query{"にほん", "", "phrase"};
  const auto unsupported = dispatcher.Dispatch(
      MakeReq(25, ipc::MessageType::QueryPredictions, ipc::BuildQueryPredictionsRequest(query)));
  ASSERT_TRUE(unsupported);
  const auto parsed_unsupported = ipc::ParseQueryPredictionsResponse(unsupported->payload_json);
  ASSERT_TRUE(parsed_unsupported);
  EXPECT_FALSE(parsed_unsupported->ok);
  EXPECT_EQ(parsed_unsupported->error, "unsupported_prediction_mode");
  EXPECT_TRUE(parsed_unsupported->predictions.empty());
  query.mode = "sentence";
  const auto sentence = dispatcher.Dispatch(
      MakeReq(28, ipc::MessageType::QueryPredictions, ipc::BuildQueryPredictionsRequest(query)));
  ASSERT_TRUE(sentence);
  const auto parsed_sentence = ipc::ParseQueryPredictionsResponse(sentence->payload_json);
  ASSERT_TRUE(parsed_sentence);
  EXPECT_FALSE(parsed_sentence->ok);
  EXPECT_EQ(parsed_sentence->error, "unsupported_prediction_mode");

  const auto malformed =
      dispatcher.Dispatch(MakeReq(26, ipc::MessageType::QueryPredictions, R"({"kana":"にほん"})"));
  ASSERT_TRUE(malformed);
  const auto parsed_malformed = ipc::ParseQueryPredictionsResponse(malformed->payload_json);
  ASSERT_TRUE(parsed_malformed);
  EXPECT_FALSE(parsed_malformed->ok);
  EXPECT_EQ(parsed_malformed->error, "invalid_request");
}

TEST_F(DispatcherTest, QueryPredictionsSuppressesPreCanceledReply) {
  scheduler.Cancel(27);
  const auto response =
      dispatcher.Dispatch(MakeReq(27, ipc::MessageType::QueryPredictions,
                                  ipc::BuildQueryPredictionsRequest({"にほん", "", "word"})));
  EXPECT_FALSE(response);
  EXPECT_FALSE(scheduler.IsCanceled(27));
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

TEST_F(DispatcherTest, CleanupWithNoBackendFallsBackToNeural) {
  ipc::QueryBatchConversionRequest request;
  request.mode = "ai-cleanup";
  request.ai_allowed = true;
  request.reading = "にほん";
  const auto response = dispatcher.Dispatch(MakeReq(
      825, ipc::MessageType::QueryBatchConversion, ipc::BuildQueryBatchConversionRequest(request)));
  ASSERT_TRUE(response);
  const auto parsed = ipc::ParseQueryBatchConversionResponse(response->payload_json);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->full_surface, "日本");
}

TEST_F(DispatcherTest, CleanupUsesSharedBackendAndPreservesInputOnFailureOrSecureGate) {
  const auto path = TempPath("azookey_cleanup_settings.json");
  {
    std::ofstream file(path);
    file << R"({"aiBackend":"openai","openAiApiKey":"test-only-placeholder"})";
  }
  azookey::host::SettingsStore settings(path);
  settings.Load();
  unsigned calls = 0;
  unsigned status = 200;
  auto transport_error = azookey::host::AiErrorClass::None;
  std::string sent;
  auto config = DefaultDispatcherConfig();
  config.ai_backend = std::make_shared<azookey::host::AiBackend>(
      [&](const auto&, const std::string& body, const auto*, auto) {
        ++calls;
        sent = body;
        return azookey::host::AiHttpResponse{
            status, R"({"choices":[{"message":{"content":"日本。"}}]})", transport_error};
      });
  azookey::host::Dispatcher handler(&engine, &scheduler, &user_dict, config, &settings);
  ipc::QueryBatchConversionRequest request;
  request.mode = "ai-cleanup";
  request.reading = "にほん";
  request.raw_romaji = "nihno";
  request.ai_allowed = request.external_ai_allowed = true;
  const auto query = [&](uint64_t id) {
    const auto response = handler.Dispatch(MakeReq(id, ipc::MessageType::QueryBatchConversion,
                                                   ipc::BuildQueryBatchConversionRequest(request)));
    return response ? ipc::ParseQueryBatchConversionResponse(response->payload_json) : std::nullopt;
  };
  auto result = query(826);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->full_surface, "日本");
  EXPECT_NE(sent.find("nihno"), std::string::npos);
  EXPECT_EQ(result->segments.front().candidates.front().source, "llm");
  request.auto_punctuation = true;
  result = query(827);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->full_surface, "日本。");
  request.ai_backend = "none";
  const auto before_override = calls;
  result = query(831);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->full_surface, "日本");
  EXPECT_EQ(calls, before_override);
  {
    std::ofstream file(path);
    file << R"({"aiBackend":"none","openAiApiKey":"test-only-placeholder"})";
  }
  settings.Reload();
  request.ai_backend = "openai";
  result = query(832);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->full_surface, "日本。");
  EXPECT_EQ(calls, before_override + 1);
  status = 401;
  result = query(828);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->full_surface, "日本");
  ASSERT_TRUE(result->error_class);
  EXPECT_EQ(*result->error_class, "Auth");
  status = 429;
  result = query(833);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->full_surface, "日本");
  ASSERT_TRUE(result->error_class);
  EXPECT_EQ(*result->error_class, "RateLimit");
  status = 0;
  transport_error = azookey::host::AiErrorClass::Timeout;
  result = query(834);
  ASSERT_TRUE(result);
  ASSERT_TRUE(result->error_class);
  EXPECT_EQ(*result->error_class, "Timeout");
  transport_error = azookey::host::AiErrorClass::KeyReentry;
  result = query(835);
  ASSERT_TRUE(result);
  ASSERT_TRUE(result->error_class);
  EXPECT_EQ(*result->error_class, "KeyReentry");
  transport_error = azookey::host::AiErrorClass::None;
  const auto before = calls;
  request.ai_allowed = false;
  result = query(829);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->full_surface, "日本");
  EXPECT_EQ(calls, before);
  request.ai_allowed = true;
  {
    std::ofstream file(path);
    file
        << R"({"aiBackend":"openai","openAiApiKey":"test-only-placeholder","privacy":{"mode":"secure"}})";
  }
  settings.Reload();
  result = query(830);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->full_surface, "日本");
  EXPECT_EQ(result->segments.front().candidates.front().source, "privacy-fallback");
  EXPECT_EQ(calls, before);
  RemovePathNoThrow(path);
}

TEST_F(DispatcherTest, BatchConversionPreservesAllReadingsAcrossHardSplits) {
  ipc::QueryBatchConversionRequest request;
  for (int i = 0; i < 100; ++i) request.reading += "あ";
  auto response = dispatcher.Dispatch(MakeReq(23, ipc::MessageType::QueryBatchConversion,
                                              ipc::BuildQueryBatchConversionRequest(request)));
  ASSERT_TRUE(response);
  auto parsed = ipc::ParseQueryBatchConversionResponse(response->payload_json);
  ASSERT_TRUE(parsed);
  ASSERT_GT(parsed->segments.size(), 1u);
  EXPECT_FALSE(parsed->partial);
  EXPECT_FALSE(parsed->canceled);
  std::string joined_reading;
  std::string joined_surface;
  for (const auto& segment : parsed->segments) {
    EXPECT_LE(segment.reading.size(), 96u);
    ASSERT_FALSE(segment.candidates.empty());
    joined_reading += segment.reading;
    joined_surface += segment.candidates.front().surface;
  }
  EXPECT_EQ(joined_reading, request.reading);
  EXPECT_EQ(joined_surface, parsed->full_surface);
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
  azookey::learning::LearningStore local_store(tsv_learning_path,
                                               &azookey::learning::test::Crypto());
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
  azookey::learning::LearningStore throwing_store(throwing_path,
                                                  &azookey::learning::test::Crypto());
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

TEST_F(DispatcherTest, AuthenticatedUnsupportedMessagesReturnExplicitTypedErrors) {
  for (const auto type : {ipc::MessageType::QueryCorrections, ipc::MessageType::UpdateUserWord,
                          ipc::MessageType::Unknown}) {
    const auto response = dispatcher.Dispatch(MakeReq(32, type, "{}"));
    ASSERT_TRUE(response.has_value()) << ipc::TypeToString(type);
    EXPECT_EQ(response->type, type);
    EXPECT_EQ(response->request_id, 32u);

    const auto payload = ipc::json::Parse(response->payload_json);
    ASSERT_TRUE(payload.has_value());
    EXPECT_EQ(payload->GetBool("ok"), false);
    EXPECT_EQ(payload->GetString("error"), "unsupported_message_type");
  }
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
  EnableEventPrivacy(dispatcher);
  ipc::CommitObservationRequest c;
  c.secure = false;
  c.learning_allowed = true;
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

// DEV-554: a resend after a pipe drop repeats the observation_id. The Dispatcher
// must answer ok=true (so the TIP stops retrying) without counting the commit
// twice in the learning store.
TEST_F(DispatcherTest, CommitObservationResendIsAcknowledgedWithoutDoubleCounting) {
  EnableEventPrivacy(dispatcher);
  ipc::CommitObservationRequest c;
  c.secure = false;
  c.learning_allowed = true;
  c.reading = "にほん";
  c.chosen = {"二本", "にほん", 0.4, "fallback"};
  c.timestamp_ms = 1700000000000ULL;
  c.observation_id = "tip-1:9";
  const auto payload = ipc::BuildCommitObservationRequest(c);

  auto first = dispatcher.Dispatch(MakeReq(51, ipc::MessageType::CommitObservation, payload));
  ASSERT_TRUE(first.has_value());
  const auto first_parsed = ipc::ParseCommitObservationResponse(first->payload_json);
  ASSERT_TRUE(first_parsed.has_value());
  EXPECT_TRUE(first_parsed->ok);
  const size_t entries_after_first = store.size();
  const double score_after_first = store.Score("にほん", "二本", 1700000000ULL);

  // The envelope request_id differs because the TIP restarts its per-connection
  // numbering after a reconnect; only observation_id identifies the commit.
  auto resend = dispatcher.Dispatch(MakeReq(2, ipc::MessageType::CommitObservation, payload));
  ASSERT_TRUE(resend.has_value());
  const auto resend_parsed = ipc::ParseCommitObservationResponse(resend->payload_json);
  ASSERT_TRUE(resend_parsed.has_value());
  EXPECT_TRUE(resend_parsed->ok);
  EXPECT_EQ(store.size(), entries_after_first);
  EXPECT_DOUBLE_EQ(store.Score("にほん", "二本", 1700000000ULL), score_after_first);
}

// DEV-1529: an immediate Backspace penalizes only the rejected surface, on the
// app row and with the context hash, and teaches the converter nothing.
TEST_F(DispatcherTest, CommitCorrectionUndoRecordsOnlyTheRejectOnTheAppRow) {
  EnableEventPrivacy(dispatcher);
  ipc::CommitCorrectionRequest request;
  request.kind = std::string(ipc::kCorrectionKindUndo);
  request.reading = "かんじ";
  request.rejected_surface = "幹事";
  request.left_context = "今日の";
  request.secure = false;
  request.learning_allowed = true;
  request.app = ipc::AppIdentity{"Notepad.exe", "Notepad"};

  const auto response = dispatcher.Dispatch(
      MakeReq(52, ipc::MessageType::CommitCorrection, ipc::BuildCommitCorrectionRequest(request)));
  ASSERT_TRUE(response.has_value());
  EXPECT_EQ(response->type, ipc::MessageType::CommitCorrection);
  const auto parsed = ipc::ParseCommitObservationResponse(response->payload_json);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_TRUE(parsed->ok);

  EXPECT_EQ(store.size(), 1u);
  const auto* rows = store.Rows("かんじ", "幹事");
  ASSERT_NE(rows, nullptr);
  const auto row = rows->find("notepad.exe");
  ASSERT_NE(row, rows->end());
  EXPECT_EQ(row->second.reject_count, 1u);
  EXPECT_EQ(row->second.accept_count, 0u);
  EXPECT_EQ(row->second.last_event, azookey::learning::LearningEventType::CorrectionReject);
  EXPECT_EQ(row->second.context_hash, azookey::learning::ContextHash("今日の"));
}

TEST_F(DispatcherTest, CommitCorrectionReconvertRecordsBothAndDeduplicatesResends) {
  EnableEventPrivacy(dispatcher);
  ipc::CommitCorrectionRequest request;
  request.kind = std::string(ipc::kCorrectionKindReconvert);
  request.reading = "かんじ";
  request.rejected_surface = "幹事";
  request.selected_surface = "漢字";
  request.observation_id = "tip-1:12";
  request.secure = false;
  request.learning_allowed = true;
  const auto payload = ipc::BuildCommitCorrectionRequest(request);

  for (const uint64_t id : {53u, 3u}) {
    const auto response =
        dispatcher.Dispatch(MakeReq(id, ipc::MessageType::CommitCorrection, payload));
    ASSERT_TRUE(response.has_value());
    const auto parsed = ipc::ParseCommitObservationResponse(response->payload_json);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->ok);
  }

  const auto* accepted = store.Rows("かんじ", "漢字");
  const auto* rejected = store.Rows("かんじ", "幹事");
  ASSERT_NE(accepted, nullptr);
  ASSERT_NE(rejected, nullptr);
  ASSERT_EQ(accepted->count(""), 1u);
  ASSERT_EQ(rejected->count(""), 1u);
  EXPECT_EQ(accepted->at("").accept_count, 1u);
  EXPECT_EQ(rejected->at("").reject_count, 1u);
}

TEST_F(DispatcherTest, CommitCorrectionIsDeniedWithoutEventPrivacyOrWithAMismatchedKind) {
  ipc::CommitCorrectionRequest request;
  request.kind = std::string(ipc::kCorrectionKindUndo);
  request.reading = "かんじ";
  request.rejected_surface = "幹事";
  request.secure = false;
  request.learning_allowed = true;
  const auto ok_of = [&](uint64_t id, const std::string& payload) {
    const auto response =
        dispatcher.Dispatch(MakeReq(id, ipc::MessageType::CommitCorrection, payload));
    EXPECT_TRUE(response.has_value());
    const auto parsed =
        response ? ipc::ParseCommitObservationResponse(response->payload_json) : std::nullopt;
    EXPECT_TRUE(parsed.has_value());
    return parsed && parsed->ok;
  };

  // No secure_flag capability in the handshake: learning is refused outright.
  EXPECT_FALSE(ok_of(54, ipc::BuildCommitCorrectionRequest(request)));

  EnableEventPrivacy(dispatcher);
  auto secure = request;
  secure.secure = true;
  EXPECT_FALSE(ok_of(55, ipc::BuildCommitCorrectionRequest(secure)));
  auto not_allowed = request;
  not_allowed.learning_allowed = false;
  EXPECT_FALSE(ok_of(56, ipc::BuildCommitCorrectionRequest(not_allowed)));
  // An undo that names a selected surface would record an accept it did not mean.
  auto mismatched = request;
  mismatched.selected_surface = "漢字";
  EXPECT_FALSE(ok_of(57, ipc::BuildCommitCorrectionRequest(mismatched)));
  EXPECT_EQ(store.size(), 0u);
}

TEST_F(DispatcherTest, CommitSegmentsSkipsAutomaticPunctuationAndDeduplicates) {
  EnableEventPrivacy(dispatcher);
  ipc::CommitSegmentsObservationRequest request;
  request.secure = false;
  request.learning_allowed = true;
  request.observation_id = "segments-test";
  ipc::CandidateField candidate;
  candidate.reading = "にほん";
  candidate.surface = "日本";
  request.segments.push_back({candidate.reading, candidate, {}, false});
  candidate.reading.clear();
  candidate.surface = "。";
  request.segments.push_back({"", candidate, {}, true});
  const auto before = store.size();
  for (uint64_t id : {800u, 801u}) {
    const auto response =
        dispatcher.Dispatch(MakeReq(id, ipc::MessageType::CommitSegmentsObservation,
                                    ipc::BuildCommitSegmentsObservationRequest(request)));
    ASSERT_TRUE(response);
    const auto parsed = ipc::ParseCommitObservationResponse(response->payload_json);
    ASSERT_TRUE(parsed);
    EXPECT_TRUE(parsed->ok);
  }
  EXPECT_EQ(store.size(), before + 1);
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
  azookey::learning::UserDictionary bad_dict(bad_dict_path, &azookey::learning::test::Crypto());
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
  azookey::learning::UserDictionary bad_dict(bad_dict_path, &azookey::learning::test::Crypto());
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
  req.path = "private-candidate-prompt.gguf";
  req.backend = "cuda";
  req.n_gpu_layers = 24;

  auto env = MakeReq(65, ipc::MessageType::LoadModel, ipc::BuildLoadModelRequest(req));
  auto resp = dispatcher.Dispatch(env);
  ASSERT_TRUE(resp.has_value());
  auto parsed = ipc::ParseLoadModelResponse(resp->payload_json);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_FALSE(parsed->ok);
  EXPECT_TRUE(parsed->error.has_value());
  EXPECT_EQ(*parsed->error, "model file probe failed");
  EXPECT_EQ(resp->payload_json.find("private-candidate-prompt"), std::string::npos);
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
  EXPECT_EQ(*health->last_error, "model file probe failed");
  EXPECT_EQ(health_resp->payload_json.find("private-candidate-prompt"), std::string::npos);
  const auto diagnostics =
      dispatcher.Dispatch(MakeReq(67, ipc::MessageType::QueryDiagnostics, "{}"));
  ASSERT_TRUE(diagnostics);
  EXPECT_EQ(diagnostics->payload_json.find("private-candidate-prompt"), std::string::npos);
}

TEST_F(DispatcherTest, LoadModelVulkanFallbackReportsDegradedCpu) {
  if (ProbeOnlyGgufUnsupportedWithRealLlama()) {
    GTEST_SKIP() << "The minimal GGUF fixture only exercises the mock control path.";
  }
  const auto model_path = TempPath("azookey_dispatcher_vulkan_fallback.gguf");
  WriteMinimalGguf(model_path);
  ipc::LoadModelRequest req;
  req.path = model_path;
  req.backend = "vulkan";
  auto response = dispatcher.Dispatch(
      MakeReq(65, ipc::MessageType::LoadModel, ipc::BuildLoadModelRequest(req)));
  ASSERT_TRUE(response);
  auto loaded = ipc::ParseLoadModelResponse(response->payload_json);
  ASSERT_TRUE(loaded);
  EXPECT_TRUE(loaded->ok);
  EXPECT_TRUE(loaded->error);
  auto health_response = dispatcher.Dispatch(MakeReq(66, ipc::MessageType::Health, "{}"));
  ASSERT_TRUE(health_response);
  auto health = ipc::ParseHealth(health_response->payload_json);
  ASSERT_TRUE(health);
  EXPECT_EQ(health->backend, "cpu");
  EXPECT_EQ(health->status, "degraded");
  EXPECT_TRUE(health->model_loaded);
  EXPECT_EQ(health->last_error, loaded->error);
  std::remove(model_path.c_str());
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

  auto diagnostics_resp =
      dispatcher.Dispatch(MakeReq(691, ipc::MessageType::QueryDiagnostics, "{}"));
  ASSERT_TRUE(diagnostics_resp.has_value());
  auto diagnostics = ipc::ParseQueryDiagnostics(diagnostics_resp->payload_json);
  ASSERT_TRUE(diagnostics.has_value());
  EXPECT_EQ(diagnostics->engine, "mock");
  EXPECT_FALSE(diagnostics->model_loaded);
  EXPECT_FALSE(diagnostics->loaded_model_path.has_value());
  EXPECT_EQ(diagnostics->fallback_state, "healthy");

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

TEST_F(DispatcherTest, QueryDiagnosticsReportsRuntimeAndFallbackState) {
  auto env = MakeReq(701, ipc::MessageType::QueryDiagnostics, "{}");
  auto resp = dispatcher.Dispatch(env);
  ASSERT_TRUE(resp.has_value());
  EXPECT_EQ(resp->type, ipc::MessageType::QueryDiagnostics);

  auto parsed = ipc::ParseQueryDiagnostics(resp->payload_json);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->engine, "mock");
  EXPECT_FALSE(parsed->model_loaded);
  EXPECT_FALSE(parsed->loaded_model_path.has_value());
  EXPECT_TRUE(parsed->backend == "cpu" || parsed->backend == "cuda");
  EXPECT_EQ(parsed->learning_entries, 0u);
  EXPECT_EQ(parsed->user_dict_entries, 0u);
  EXPECT_EQ(parsed->fallback_state, "healthy");
  // No layer state was injected, so the field is left out.
  EXPECT_FALSE(parsed->neologd_layer.has_value());
}

TEST_F(DispatcherTest, QueryDiagnosticsReportsTheNeologdLayerState) {
  auto config = DefaultDispatcherConfig();
  config.neologd_layer = std::make_shared<azookey::host::NeologdLayerState>();
  azookey::host::Dispatcher layer_dispatcher(&engine, &scheduler, &user_dict, config);
  const auto layer_of = [&](uint64_t id) {
    const auto response =
        layer_dispatcher.Dispatch(MakeReq(id, ipc::MessageType::QueryDiagnostics, "{}"));
    EXPECT_TRUE(response.has_value());
    const auto parsed =
        response ? ipc::ParseQueryDiagnostics(response->payload_json) : std::nullopt;
    EXPECT_TRUE(parsed.has_value());
    return parsed ? parsed->neologd_layer : std::nullopt;
  };

  auto layer = layer_of(702);
  ASSERT_TRUE(layer.has_value());
  EXPECT_EQ(layer->state, ipc::kNeologdLayerNotRequested);
  EXPECT_FALSE(layer->reason.has_value());

  config.neologd_layer->Set(ipc::kNeologdLayerError, std::string("pack download failed"));
  layer = layer_of(703);
  ASSERT_TRUE(layer.has_value());
  EXPECT_EQ(layer->state, ipc::kNeologdLayerError);
  EXPECT_EQ(layer->reason, std::optional<std::string>("pack download failed"));

  config.neologd_layer->Set(ipc::kNeologdLayerReady);
  layer = layer_of(704);
  ASSERT_TRUE(layer.has_value());
  EXPECT_EQ(layer->state, ipc::kNeologdLayerReady);
  EXPECT_FALSE(layer->reason.has_value());
}

TEST_F(DispatcherTest, QueryDiagnosticsReportsModelFailureAndDisabledModelWithoutChangingState) {
  const auto missing_path = TempPath("azookey_dispatcher_diagnostics_missing.gguf");
  std::remove(missing_path.c_str());
  azookey::host::ModelLoadOptions options;
  options.path = missing_path;
  ASSERT_FALSE(engine.LoadModelWithResult(options).ok);
  ASSERT_EQ(engine.health_state(), azookey::host::HealthState::DegradedModel);

  const auto settings_path = TempPath("azookey_dispatcher_diagnostics_settings.json");
  std::remove(settings_path.c_str());
  {
    std::ofstream out(settings_path, std::ios::binary);
    ASSERT_TRUE(out.is_open());
    out << R"({"model":{"enabled":true}})";
  }
  azookey::host::SettingsStore settings_store(settings_path);
  settings_store.Load();
  azookey::host::Dispatcher config_dispatcher(&engine, &scheduler, &user_dict,
                                              DefaultDispatcherConfig(), &settings_store);
  const auto fallback_state = [&](uint64_t id) {
    const auto response =
        config_dispatcher.Dispatch(MakeReq(id, ipc::MessageType::QueryDiagnostics, "{}"));
    EXPECT_TRUE(response.has_value());
    const auto parsed =
        response ? ipc::ParseQueryDiagnostics(response->payload_json) : std::nullopt;
    EXPECT_TRUE(parsed.has_value());
    return parsed ? parsed->fallback_state : std::string();
  };
  EXPECT_EQ(fallback_state(702), "degraded_model");

  {
    std::ofstream out(settings_path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.is_open());
    out << R"({"model":{"enabled":false}})";
  }
  const auto update =
      config_dispatcher.Dispatch(MakeReq(703, ipc::MessageType::UpdateConfig, "{}"));
  ASSERT_TRUE(update.has_value());
  EXPECT_EQ(engine.health_state(), azookey::host::HealthState::DegradedModel);
  EXPECT_EQ(fallback_state(704), "healthy");
  std::remove(settings_path.c_str());
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

TEST_F(DispatcherTest, UpdateConfigNotifiesTrendingSettingsAndSafeModeButNotInvalidReload) {
  const auto settings_path =
      TempPath(PerTestFileName("azookey_dispatcher_trending_callback", ".json").c_str());
  struct SettingsCleanup {
    std::string path;
    ~SettingsCleanup() { std::remove(path.c_str()); }
  } cleanup{settings_path};
  const auto write_settings = [&](const std::string& json) {
    std::ofstream out(settings_path, std::ios::binary | std::ios::trunc);
    EXPECT_TRUE(out.is_open());
    out << json;
  };
  azookey::host::SettingsStore settings_store(settings_path);
  std::vector<azookey::host::EngineConfig> applied_configs;
  auto config = DefaultDispatcherConfig();
  config.on_config_applied = [&](const azookey::host::EngineConfig& applied) {
    // Publication must happen before the callback can notify its worker.
    EXPECT_EQ(engine.config().auto_word_trending_enabled, applied.auto_word_trending_enabled);
    EXPECT_EQ(engine.config().auto_word_trending_interval_hours,
              applied.auto_word_trending_interval_hours);
    applied_configs.push_back(applied);
  };
  azookey::host::Dispatcher first(&engine, &scheduler, &user_dict, config, &settings_store);
  azookey::host::Dispatcher second(&engine, &scheduler, &user_dict, config, &settings_store);

  write_settings(R"({"autoWordRegistration":{"trendingEnabled":true,
                    "trendingIntervalHours":6,"registrationMode":"auto"}})");
  const auto enabled = first.Dispatch(MakeReq(740, ipc::MessageType::UpdateConfig, "{}"));
  ASSERT_TRUE(enabled);
  const auto enabled_payload = ipc::ParseUpdateConfigResponse(enabled->payload_json);
  ASSERT_TRUE(enabled_payload);
  ASSERT_TRUE(enabled_payload->ok);
  ASSERT_EQ(applied_configs.size(), 1u);
  EXPECT_TRUE(applied_configs.back().auto_word_trending_enabled);
  EXPECT_EQ(applied_configs.back().auto_word_trending_interval_hours, 6u);
  EXPECT_TRUE(applied_configs.back().auto_word_auto_register);

  write_settings(R"({"privacy":{"mode":"offline"},"autoWordRegistration":{
                    "trendingEnabled":true,"trendingIntervalHours":6,"registrationMode":"auto"}})");
  const auto offline = second.Dispatch(MakeReq(743, ipc::MessageType::UpdateConfig, "{}"));
  ASSERT_TRUE(offline);
  const auto offline_payload = ipc::ParseUpdateConfigResponse(offline->payload_json);
  ASSERT_TRUE(offline_payload);
  ASSERT_TRUE(offline_payload->ok);
  ASSERT_EQ(applied_configs.size(), 2u);
  EXPECT_FALSE(applied_configs.back().auto_word_trending_enabled);
  EXPECT_EQ(applied_configs.back().auto_word_trending_interval_hours, 6u);
  EXPECT_TRUE(applied_configs.back().auto_word_auto_register);
  EXPECT_TRUE(settings_store.settings().auto_word.trending_enabled);

  write_settings("{ invalid json");
  const auto invalid_offline = first.Dispatch(MakeReq(744, ipc::MessageType::UpdateConfig, "{}"));
  ASSERT_TRUE(invalid_offline);
  const auto invalid_offline_payload =
      ipc::ParseUpdateConfigResponse(invalid_offline->payload_json);
  ASSERT_TRUE(invalid_offline_payload);
  EXPECT_FALSE(invalid_offline_payload->ok);
  EXPECT_EQ(applied_configs.size(), 2u);
  EXPECT_FALSE(engine.config().auto_word_trending_enabled);

  write_settings(R"({"privacy":{"mode":"normal"},"autoWordRegistration":{
                    "trendingEnabled":true,"trendingIntervalHours":6,"registrationMode":"auto"}})");
  const auto resumed = first.Dispatch(MakeReq(745, ipc::MessageType::UpdateConfig, "{}"));
  ASSERT_TRUE(resumed);
  const auto resumed_payload = ipc::ParseUpdateConfigResponse(resumed->payload_json);
  ASSERT_TRUE(resumed_payload);
  ASSERT_TRUE(resumed_payload->ok);
  ASSERT_EQ(applied_configs.size(), 3u);
  EXPECT_TRUE(applied_configs.back().auto_word_trending_enabled);
  EXPECT_TRUE(settings_store.settings().auto_word.trending_enabled);

  write_settings(R"({"safeMode":{"enabled":true},"autoWordRegistration":{
                    "trendingEnabled":true,"trendingIntervalHours":12}})");
  const auto safe = second.Dispatch(MakeReq(741, ipc::MessageType::UpdateConfig, "{}"));
  ASSERT_TRUE(safe);
  const auto safe_payload = ipc::ParseUpdateConfigResponse(safe->payload_json);
  ASSERT_TRUE(safe_payload);
  ASSERT_TRUE(safe_payload->ok);
  ASSERT_EQ(applied_configs.size(), 4u);
  EXPECT_FALSE(applied_configs.back().auto_word_trending_enabled);
  EXPECT_EQ(applied_configs.back().auto_word_trending_interval_hours, 12u);
  EXPECT_FALSE(applied_configs.back().auto_word_auto_register);
  EXPECT_EQ(engine.health_state(), azookey::host::HealthState::SafeMode);

  write_settings("{ invalid json");
  const auto invalid = first.Dispatch(MakeReq(742, ipc::MessageType::UpdateConfig, "{}"));
  ASSERT_TRUE(invalid);
  const auto invalid_payload = ipc::ParseUpdateConfigResponse(invalid->payload_json);
  ASSERT_TRUE(invalid_payload);
  EXPECT_FALSE(invalid_payload->ok);
  EXPECT_EQ(applied_configs.size(), 4u);
  EXPECT_FALSE(engine.config().auto_word_trending_enabled);
  EXPECT_EQ(engine.config().auto_word_trending_interval_hours, 12u);
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

// ---- M35 / M36-A ----

TEST_F(DispatcherTest, ObserveTypoIsFireAndForgetAndUpdatesTheStore) {
  EnableEventPrivacy(dispatcher);
  const auto typo_path =
      (std::filesystem::temp_directory_path() / "azookey_dispatcher_typo.tsv").string();
  std::remove(typo_path.c_str());
  azookey::learning::TypoCorrectionStore typo(typo_path, &azookey::learning::test::Crypto());
  engine.SetTypoStore(&typo);

  ipc::ObserveTypoRequest request;
  request.secure = false;
  request.learning_allowed = true;
  request.wrong_reading = "こんちには";
  request.correct_reading = "こんにちは";
  request.timestamp_ms = 1'700'000'000'000ULL;

  const auto response = dispatcher.Dispatch(
      MakeReq(51, ipc::MessageType::ObserveTypo, ipc::BuildObserveTypoRequest(request)));
  // Spec section 7: no reply at all, so a blocking client must not wait on one.
  EXPECT_FALSE(response.has_value());
  EXPECT_EQ(typo.size(), 1u);

  // A malformed payload is dropped just as silently.
  EXPECT_FALSE(dispatcher.Dispatch(MakeReq(52, ipc::MessageType::ObserveTypo, "{}")).has_value());
  EXPECT_EQ(typo.size(), 1u);

  engine.SetTypoStore(nullptr);
  std::remove(typo_path.c_str());
}

TEST_F(DispatcherTest, QueryCandidatesReportsTheCorrectedReadingUnderAutoReplace) {
  const auto typo_path =
      (std::filesystem::temp_directory_path() / "azookey_dispatcher_typo_auto.tsv").string();
  std::remove(typo_path.c_str());
  azookey::learning::TypoCorrectionStore typo(typo_path, &azookey::learning::test::Crypto());
  engine.SetTypoStore(&typo);
  auto config = engine.config();
  config.typo_correction_mode = "auto_replace";
  config.typo_min_count = 2;
  engine.ApplyConfig(config);

  // The dispatcher stamps the query with the wall clock, and Lookup ignores
  // records older than 180 days, so the observation has to be recent.
  const auto now_epoch_sec =
      static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count());
  typo.Observe("こんちには", "こんにちは", now_epoch_sec);
  typo.Observe("こんちには", "こんにちは", now_epoch_sec);

  ipc::QueryCandidatesRequest query;
  query.reading = "こんちには";
  const auto response = dispatcher.Dispatch(
      MakeReq(53, ipc::MessageType::QueryCandidates, ipc::BuildQueryCandidatesRequest(query)));
  ASSERT_TRUE(response.has_value());
  const auto parsed = ipc::ParseQueryCandidatesResponse(response->payload_json);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->corrected_reading, "こんにちは");

  engine.SetTypoStore(nullptr);
  std::remove(typo_path.c_str());
}

TEST_F(DispatcherTest, ListAndResolveNewWordDriveTheApprovalFlow) {
  const auto auto_word_path =
      (std::filesystem::temp_directory_path() / "azookey_dispatcher_auto_words.tsv").string();
  std::remove(auto_word_path.c_str());
  azookey::learning::AutoWordStore auto_words(auto_word_path, &azookey::learning::test::Crypto());
  auto_words.Observe("azooKey", "あずきー", 1'700'000'000ULL, 3, false);
  auto_words.Observe("azooKey社", "あずきーしゃ", 1'700'000'100ULL, 3, false);

  azookey::host::Dispatcher approval(&engine, &scheduler, &user_dict, DefaultDispatcherConfig(),
                                     nullptr, &auto_words);

  ipc::ListNewWordCandidatesRequest list;
  list.state_filter = "pending";
  auto response = approval.Dispatch(MakeReq(60, ipc::MessageType::ListNewWordCandidates,
                                            ipc::BuildListNewWordCandidatesRequest(list)));
  ASSERT_TRUE(response.has_value());
  auto listed = ipc::ParseListNewWordCandidatesResponse(response->payload_json);
  ASSERT_TRUE(listed);
  EXPECT_TRUE(listed->ok);
  ASSERT_EQ(listed->items.size(), 2u);
  // Most recently seen first.
  EXPECT_EQ(listed->items[0].surface, "azooKey社");
  EXPECT_EQ(listed->items[0].state, "pending");
  EXPECT_EQ(listed->items[0].source, "mining");

  // max_items pages the response.
  list.max_items = 1;
  response = approval.Dispatch(MakeReq(61, ipc::MessageType::ListNewWordCandidates,
                                       ipc::BuildListNewWordCandidatesRequest(list)));
  ASSERT_TRUE(response.has_value());
  listed = ipc::ParseListNewWordCandidatesResponse(response->payload_json);
  ASSERT_TRUE(listed);
  EXPECT_EQ(listed->items.size(), 1u);

  ipc::ResolveNewWordRequest resolve;
  resolve.surface = "azooKey";
  resolve.reading = "あずきー";
  resolve.action = "confirm";
  response = approval.Dispatch(
      MakeReq(62, ipc::MessageType::ResolveNewWord, ipc::BuildResolveNewWordRequest(resolve)));
  ASSERT_TRUE(response.has_value());
  auto resolved = ipc::ParseResolveNewWordResponse(response->payload_json);
  ASSERT_TRUE(resolved);
  EXPECT_TRUE(resolved->ok);
  EXPECT_TRUE(resolved->changed);
  EXPECT_EQ(auto_words.LookupConfirmed("あずきー").size(), 1u);

  // Confirming it again is a success that changes nothing, not a failure the
  // UI would have to explain.
  response = approval.Dispatch(
      MakeReq(65, ipc::MessageType::ResolveNewWord, ipc::BuildResolveNewWordRequest(resolve)));
  ASSERT_TRUE(response.has_value());
  resolved = ipc::ParseResolveNewWordResponse(response->payload_json);
  ASSERT_TRUE(resolved);
  EXPECT_TRUE(resolved->ok);
  EXPECT_FALSE(resolved->changed);
  EXPECT_FALSE(resolved->error);

  resolve.surface = "azooKey社";
  resolve.reading = "あずきーしゃ";
  resolve.action = "reject";
  response = approval.Dispatch(
      MakeReq(63, ipc::MessageType::ResolveNewWord, ipc::BuildResolveNewWordRequest(resolve)));
  ASSERT_TRUE(response.has_value());
  resolved = ipc::ParseResolveNewWordResponse(response->payload_json);
  ASSERT_TRUE(resolved);
  EXPECT_TRUE(resolved->ok);
  EXPECT_EQ(auto_words.ListByState(azookey::learning::AutoWordState::Rejected).size(), 1u);

  // The decision reached disk, so it survives a restart.
  azookey::learning::AutoWordStore reloaded(auto_word_path, &azookey::learning::test::Crypto());
  ASSERT_TRUE(reloaded.Load());
  EXPECT_EQ(reloaded.LookupConfirmed("あずきー").size(), 1u);
  EXPECT_EQ(reloaded.ListByState(azookey::learning::AutoWordState::Rejected).size(), 1u);

  // An unknown key resolves to nothing rather than reporting success.
  resolve.surface = "しらないご";
  resolve.reading = "しらないご";
  resolve.action = "confirm";
  response = approval.Dispatch(
      MakeReq(64, ipc::MessageType::ResolveNewWord, ipc::BuildResolveNewWordRequest(resolve)));
  ASSERT_TRUE(response.has_value());
  resolved = ipc::ParseResolveNewWordResponse(response->payload_json);
  ASSERT_TRUE(resolved);
  EXPECT_FALSE(resolved->ok);
  EXPECT_EQ(resolved->error, "not_found");

  // A malformed request is reported as such, on both messages.
  response = approval.Dispatch(MakeReq(66, ipc::MessageType::ResolveNewWord, R"({"action":"x"})"));
  ASSERT_TRUE(response.has_value());
  resolved = ipc::ParseResolveNewWordResponse(response->payload_json);
  ASSERT_TRUE(resolved);
  EXPECT_FALSE(resolved->ok);
  EXPECT_EQ(resolved->error, "invalid_request");
  response = approval.Dispatch(
      MakeReq(67, ipc::MessageType::ListNewWordCandidates, R"({"state_filter":"all"})"));
  ASSERT_TRUE(response.has_value());
  listed = ipc::ParseListNewWordCandidatesResponse(response->payload_json);
  ASSERT_TRUE(listed);
  EXPECT_FALSE(listed->ok);
  EXPECT_EQ(listed->error, "invalid_request");

  // Both words are resolved, so pending is genuinely empty: that is ok=true.
  list.state_filter = "pending";
  list.max_items = 50;
  response = approval.Dispatch(MakeReq(68, ipc::MessageType::ListNewWordCandidates,
                                       ipc::BuildListNewWordCandidatesRequest(list)));
  ASSERT_TRUE(response.has_value());
  listed = ipc::ParseListNewWordCandidatesResponse(response->payload_json);
  ASSERT_TRUE(listed);
  EXPECT_TRUE(listed->ok);
  EXPECT_TRUE(listed->items.empty());

  std::remove(auto_word_path.c_str());
}

TEST_F(DispatcherTest, ResolveNewWordRollsBackWhenSaveFails) {
  // The store's parent "directory" is a regular file, so every Save() fails.
  const auto blocker = std::filesystem::temp_directory_path() / "azookey_dispatcher_save_blocker";
  std::filesystem::remove_all(blocker);
  // The temporary stream closes the file at the end of the statement.
  std::ofstream(blocker) << "not a directory";
  azookey::learning::AutoWordStore auto_words(blocker / "auto_words.tsv",
                                              &azookey::learning::test::Crypto());
  auto_words.Observe("azooKey", "あずきー", 1'700'000'000ULL, 3, false);
  azookey::host::Dispatcher approval(&engine, &scheduler, &user_dict, DefaultDispatcherConfig(),
                                     nullptr, &auto_words);

  ipc::ResolveNewWordRequest resolve;
  resolve.surface = "azooKey";
  resolve.reading = "あずきー";
  resolve.action = "confirm";
  auto response = approval.Dispatch(
      MakeReq(69, ipc::MessageType::ResolveNewWord, ipc::BuildResolveNewWordRequest(resolve)));
  ASSERT_TRUE(response.has_value());
  const auto resolved = ipc::ParseResolveNewWordResponse(response->payload_json);
  ASSERT_TRUE(resolved);
  EXPECT_FALSE(resolved->ok);
  EXPECT_FALSE(resolved->changed);
  EXPECT_EQ(resolved->error, "save_failed");
  // Memory still matches the file: the word is pending and is not injected.
  EXPECT_TRUE(auto_words.LookupConfirmed("あずきー").empty());
  EXPECT_EQ(auto_words.ListByState(azookey::learning::AutoWordState::Pending).size(), 1u);

  std::filesystem::remove_all(blocker);
}

TEST_F(DispatcherTest, ConfirmedNewWordIsInjectedOnlyAfterApproval) {
  const auto auto_word_path =
      (std::filesystem::temp_directory_path() / "azookey_dispatcher_auto_words_inject.tsv")
          .string();
  std::remove(auto_word_path.c_str());
  azookey::learning::AutoWordStore auto_words(auto_word_path, &azookey::learning::test::Crypto());
  // registrationMode=confirm: the threshold is reached but auto_promote is off.
  for (uint64_t i = 0; i < 3; ++i) {
    auto_words.Observe("阿頭季", "あずき", 1'700'000'000ULL + i, 3, false);
  }
  engine.SetAutoWordStore(&auto_words);
  azookey::host::Dispatcher approval(&engine, &scheduler, &user_dict, DefaultDispatcherConfig(),
                                     nullptr, &auto_words);

  const auto has_auto_word = [&]() {
    const auto candidates = engine.QueryCandidates("あずき", "", 1'700'000'100ULL);
    return std::any_of(candidates.begin(), candidates.end(), [](const auto& c) {
      return c.surface == "阿頭季" && c.debug_info.find("auto-word") != std::string::npos;
    });
  };
  EXPECT_FALSE(has_auto_word());

  ipc::ResolveNewWordRequest resolve;
  resolve.surface = "阿頭季";
  resolve.reading = "あずき";
  resolve.action = "confirm";
  auto response = approval.Dispatch(
      MakeReq(73, ipc::MessageType::ResolveNewWord, ipc::BuildResolveNewWordRequest(resolve)));
  ASSERT_TRUE(response.has_value());
  const auto resolved = ipc::ParseResolveNewWordResponse(response->payload_json);
  ASSERT_TRUE(resolved);
  EXPECT_TRUE(resolved->ok);
  EXPECT_TRUE(has_auto_word());

  engine.SetAutoWordStore(nullptr);
  std::remove(auto_word_path.c_str());
}

TEST_F(DispatcherTest, ApprovalMessagesWithoutAStoreAnswerInsteadOfCrashing) {
  // The default fixture dispatcher has no AutoWordStore.
  auto response = dispatcher.Dispatch(MakeReq(70, ipc::MessageType::ListNewWordCandidates, "{}"));
  ASSERT_TRUE(response.has_value());
  const auto listed = ipc::ParseListNewWordCandidatesResponse(response->payload_json);
  ASSERT_TRUE(listed);
  EXPECT_TRUE(listed->items.empty());
  // Distinguishable from a store that genuinely has no pending words.
  EXPECT_FALSE(listed->ok);
  EXPECT_EQ(listed->error, "store_unavailable");

  ipc::ResolveNewWordRequest resolve;
  resolve.surface = "azooKey";
  resolve.reading = "あずきー";
  resolve.action = "confirm";
  response = dispatcher.Dispatch(
      MakeReq(71, ipc::MessageType::ResolveNewWord, ipc::BuildResolveNewWordRequest(resolve)));
  ASSERT_TRUE(response.has_value());
  const auto resolved = ipc::ParseResolveNewWordResponse(response->payload_json);
  ASSERT_TRUE(resolved);
  EXPECT_FALSE(resolved->ok);
  EXPECT_EQ(resolved->error, "store_unavailable");

  // ObserveTypo stays fire-and-forget with no store behind it.
  EXPECT_FALSE(
      dispatcher
          .Dispatch(MakeReq(72, ipc::MessageType::ObserveTypo,
                            R"({"wrong_reading":"こんちには","correct_reading":"こんにちは"})"))
          .has_value());
}

TEST_F(DispatcherTest, LearningEventsDenyUnknownAndDoNotConsumeObservationIds) {
  EnableEventPrivacy(dispatcher);
  const auto typo_path = TempPath("azookey_event_privacy_typo.tsv");
  std::remove(typo_path.c_str());
  azookey::learning::TypoCorrectionStore typo(typo_path, &azookey::learning::test::Crypto());
  engine.SetTypoStore(&typo);
  ipc::QueryCandidatesRequest query;
  query.reading = "normal";
  query.secure = false;
  query.learning_allowed = true;
  dispatcher.Dispatch(
      MakeReq(9100, ipc::MessageType::QueryCandidates, ipc::BuildQueryCandidatesRequest(query)));
  for (auto type : {ipc::MessageType::CommitObservation,
                    ipc::MessageType::CommitSegmentsObservation, ipc::MessageType::ObserveTypo}) {
    SCOPED_TRACE(static_cast<int>(type));
    const std::string id = "privacy-denied-" + std::to_string(static_cast<int>(type));
    const std::string reading = "privacy" + id;
    std::string base;
    if (type == ipc::MessageType::CommitObservation)
      base =
          R"({"reading":")" + reading +
          R"(","chosen":{"surface":"word","reading":"kana","score":1,"source":"static"},"observation_id":")" +
          id + "\"";
    else if (type == ipc::MessageType::CommitSegmentsObservation)
      base =
          R"({"segments":[{"reading":")" + reading +
          R"(","chosen":{"surface":"word","reading":"kana","score":1,"source":"static"}}],"observation_id":")" +
          id + "\"";
    else
      base = R"({"wrong_reading":"こんちには","correct_reading":"こんにちは")";
    const auto before = store.size();
    for (const auto* flags :
         {"", R"(,"secure":false)", R"(,"learning_allowed":true)",
          R"(,"secure":true,"learning_allowed":true)", R"(,"secure":true,"learning_allowed":false)",
          R"(,"secure":false,"learning_allowed":false)",
          R"(,"secure":null,"learning_allowed":"true")"}) {
      const auto response = dispatcher.Dispatch(MakeReq(9101, type, base + flags + "}"));
      if (type == ipc::MessageType::ObserveTypo) {
        EXPECT_FALSE(response);
      } else {
        ASSERT_TRUE(response);
        const auto parsed = ipc::ParseCommitObservationResponse(response->payload_json);
        ASSERT_TRUE(parsed);
        EXPECT_FALSE(parsed->ok);
      }
      EXPECT_EQ(store.size(), before);
      EXPECT_EQ(typo.size(), 0u);
    }
    const auto allowed = dispatcher.Dispatch(
        MakeReq(9102, type, base + R"(,"secure":false,"learning_allowed":true})"));
    if (type == ipc::MessageType::ObserveTypo) {
      EXPECT_FALSE(allowed);
      EXPECT_EQ(typo.size(), 1u);
    } else {
      ASSERT_TRUE(allowed);
      EXPECT_TRUE(ipc::ParseCommitObservationResponse(allowed->payload_json)->ok);
      EXPECT_EQ(store.size(), before + 1);
    }
  }
  engine.SetTypoStore(nullptr);
  std::remove(typo_path.c_str());
}

TEST_F(DispatcherTest, SecureFlagCapabilityIsConnectionLocalAndResetOnEveryHandshake) {
  ipc::CommitObservationRequest commit;
  commit.reading = "かな";
  commit.chosen = {"仮名", "かな", 1.0, "static"};
  commit.secure = false;
  commit.learning_allowed = true;
  auto allowed = [&](azookey::host::Dispatcher& target) {
    auto response = target.Dispatch(MakeReq(9200, ipc::MessageType::CommitObservation,
                                            ipc::BuildCommitObservationRequest(commit)));
    EXPECT_TRUE(response);
    return response && ipc::ParseCommitObservationResponse(response->payload_json)->ok;
  };
  EXPECT_FALSE(allowed(dispatcher));
  EnableEventPrivacy(dispatcher);
  EXPECT_TRUE(allowed(dispatcher));
  azookey::host::Dispatcher other(&engine, &scheduler, &user_dict, DefaultDispatcherConfig());
  EXPECT_FALSE(allowed(other));
  ipc::HandshakeRequest refresh;
  refresh.tip_version = "test";
  refresh.client_id = "privacy-test";
  dispatcher.Dispatch(
      MakeReq(9201, ipc::MessageType::Handshake, ipc::BuildHandshakeRequest(refresh)));
  EXPECT_FALSE(allowed(dispatcher));
  EnableEventPrivacy(dispatcher);
  dispatcher.Dispatch(MakeReq(9202, ipc::MessageType::Handshake, "{}"));
  EXPECT_FALSE(allowed(dispatcher));
  EnableEventPrivacy(dispatcher);
  refresh.capabilities = {"secure_flag"};
  refresh.protocol_version = 999;
  dispatcher.Dispatch(
      MakeReq(9203, ipc::MessageType::Handshake, ipc::BuildHandshakeRequest(refresh)));
  EXPECT_FALSE(allowed(dispatcher));
  EnableEventPrivacy(dispatcher);
  EXPECT_TRUE(allowed(dispatcher));
}

TEST_F(DispatcherTest, HostPrivacySettingsRejectAllLearningAndMining) {
  const auto settings_path = TempPath("azookey_learning_privacy_settings.json");
  const auto mining_path = TempPath("azookey_learning_privacy_mining.tsv");
  const auto typo_path = TempPath("azookey_learning_privacy_typo.tsv");
  for (const auto* policy : {
           R"({"privacy":{"mode":"secure"}})",
           R"({"privacy":{"mode":"private"}})",
           R"({"privacy":{"mode":"custom"}})",
           R"({"privacy":{"mode":"custom","custom":{"learning":false}}})",
           R"({"privacy":{"mode":"custom","custom":{"learning":"true"}}})",
       }) {
    SCOPED_TRACE(policy);
    std::remove(mining_path.c_str());
    std::remove(typo_path.c_str());
    {
      std::ofstream out(settings_path);
      out << policy;
    }
    azookey::host::SettingsStore settings(settings_path);
    settings.Load();
    azookey::learning::AutoWordStore mining(mining_path, &azookey::learning::test::Crypto());
    azookey::learning::TypoCorrectionStore typo(typo_path, &azookey::learning::test::Crypto());
    engine.SetAutoWordStore(&mining);
    engine.SetTypoStore(&typo);
    azookey::host::Dispatcher target(&engine, &scheduler, &user_dict, DefaultDispatcherConfig(),
                                     &settings, &mining);
    EnableEventPrivacy(target);
    ipc::CommitObservationRequest commit;
    commit.reading = "あずきーしゃ";
    commit.chosen = {"azooKey社", commit.reading, 1.0, "static"};
    commit.secure = false;
    commit.learning_allowed = true;
    ipc::CommitSegmentsObservationRequest segments;
    segments.segments.push_back({commit.reading, commit.chosen, {}, false});
    segments.secure = false;
    segments.learning_allowed = true;
    ipc::ObserveTypoRequest observation;
    observation.wrong_reading = "こんちには";
    observation.correct_reading = "こんにちは";
    observation.secure = false;
    observation.learning_allowed = true;
    const auto before = store.size();
    ipc::CommitCorrectionRequest correction;
    correction.kind = std::string(ipc::kCorrectionKindReconvert);
    correction.reading = commit.reading;
    correction.rejected_surface = "あずきー社";
    correction.selected_surface = commit.chosen.surface;
    correction.secure = false;
    correction.learning_allowed = true;
    for (auto type :
         {ipc::MessageType::CommitObservation, ipc::MessageType::CommitSegmentsObservation,
          ipc::MessageType::CommitCorrection}) {
      const auto payload = type == ipc::MessageType::CommitObservation
                               ? ipc::BuildCommitObservationRequest(commit)
                           : type == ipc::MessageType::CommitCorrection
                               ? ipc::BuildCommitCorrectionRequest(correction)
                               : ipc::BuildCommitSegmentsObservationRequest(segments);
      const auto response = target.Dispatch(MakeReq(9300, type, payload));
      ASSERT_TRUE(response);
      const auto parsed = ipc::ParseCommitObservationResponse(response->payload_json);
      ASSERT_TRUE(parsed);
      EXPECT_FALSE(parsed->ok);
    }
    EXPECT_FALSE(target.Dispatch(
        MakeReq(9301, ipc::MessageType::ObserveTypo, ipc::BuildObserveTypoRequest(observation))));
    EXPECT_EQ(store.size(), before);
    EXPECT_TRUE(mining.ListByState(azookey::learning::AutoWordState::Pending).empty());
    EXPECT_EQ(typo.size(), 0u);
    EXPECT_FALSE(std::filesystem::exists(mining_path));
    EXPECT_FALSE(std::filesystem::exists(typo_path));
    engine.SetAutoWordStore(nullptr);
    engine.SetTypoStore(nullptr);
    std::remove(settings_path.c_str());
  }
}

TEST_F(DispatcherTest, HostCustomLearningOptInRestoresObservations) {
  const auto settings_path = TempPath("azookey_custom_learning_settings.json");
  {
    std::ofstream out(settings_path);
    out << R"({"privacy":{"mode":"custom","custom":{"learning":true}}})";
  }
  azookey::host::SettingsStore settings(settings_path);
  settings.Load();
  azookey::host::Dispatcher target(&engine, &scheduler, &user_dict, DefaultDispatcherConfig(),
                                   &settings);
  EnableEventPrivacy(target);
  ipc::CommitObservationRequest commit;
  commit.reading = "かな";
  commit.chosen = {"仮名", "かな", 1.0, "static"};
  commit.secure = false;
  commit.learning_allowed = true;
  const auto before = store.size();
  const auto response = target.Dispatch(MakeReq(9350, ipc::MessageType::CommitObservation,
                                                ipc::BuildCommitObservationRequest(commit)));
  ASSERT_TRUE(response);
  const auto parsed = ipc::ParseCommitObservationResponse(response->payload_json);
  ASSERT_TRUE(parsed);
  EXPECT_TRUE(parsed->ok);
  EXPECT_GT(store.size(), before);
  std::remove(settings_path.c_str());
}

TEST_F(DispatcherTest, LearningPrivacyDoesNotWaitForUpdateConfigModelLoad) {
  using namespace std::chrono_literals;
  const auto path = TempPath("azookey_privacy_model_reload.json");
  for (bool secure : {false, true}) {
    SCOPED_TRACE(secure);
    // Start with the opposite policy so publication is observable without
    // reading the non-thread-safe general settings object.
    {
      std::ofstream out(path);
      out << (secure ? R"({"privacy":{"mode":"normal"}})" : R"({"privacy":{"mode":"secure"}})");
    }
    azookey::host::SettingsStore settings(path);
    settings.Load();
    const auto config = DefaultDispatcherConfig();
    azookey::host::Dispatcher updater(&engine, &scheduler, &user_dict, config, &settings);
    azookey::host::Dispatcher observer(&engine, &scheduler, &user_dict, config, &settings);
    EnableEventPrivacy(observer);
    {
      std::ofstream out(path);
      out << (secure ? R"({"privacy":{"mode":"secure"}})" : R"({"privacy":{"mode":"normal"}})");
    }
    std::promise<void> loading;
    std::promise<void> release;
    auto entered = loading.get_future();
    auto released = release.get_future().share();
    azookey::host::ModelLoadOptions options;
    options.path = "missing-privacy-test-model.gguf";
    options.before_probe_for_tests = [&] {
      loading.set_value();
      released.wait();
    };
    auto model =
        std::async(std::launch::async, [&] { return engine.LoadModelWithResult(options); });
    if (entered.wait_for(2s) != std::future_status::ready) {
      release.set_value();
      model.get();
      FAIL() << "model load did not reach the test barrier";
    }
    auto update = std::async(std::launch::async, [&] {
      return updater.Dispatch(MakeReq(9400, ipc::MessageType::UpdateConfig, "{}"));
    });
    bool published = false;
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
      {
        const auto privacy = settings.LockPrivacyPolicy();
        published = privacy.policy.secure == secure;
      }
      if (published) break;
      std::this_thread::yield();
    }
    auto learning = std::async(std::launch::async, [&] {
      ipc::CommitObservationRequest commit;
      commit.reading = "かな";
      commit.chosen = {"仮名", "かな", 1.0, "static"};
      commit.secure = false;
      commit.learning_allowed = true;
      ipc::CommitSegmentsObservationRequest segments;
      segments.segments.push_back({commit.reading, commit.chosen, {}, false});
      segments.secure = false;
      segments.learning_allowed = true;
      for (const auto type :
           {ipc::MessageType::CommitObservation, ipc::MessageType::CommitSegmentsObservation}) {
        const auto response =
            observer.Dispatch(MakeReq(9401, type,
                                      type == ipc::MessageType::CommitObservation
                                          ? ipc::BuildCommitObservationRequest(commit)
                                          : ipc::BuildCommitSegmentsObservationRequest(segments)));
        if (!response) return false;
        const auto parsed = ipc::ParseCommitObservationResponse(response->payload_json);
        if (!parsed || parsed->ok == secure) return false;
      }
      ipc::ObserveTypoRequest typo;
      typo.wrong_reading = "こんちには";
      typo.correct_reading = "こんにちは";
      typo.secure = false;
      typo.learning_allowed = true;
      return !observer
                  .Dispatch(MakeReq(9402, ipc::MessageType::ObserveTypo,
                                    ipc::BuildObserveTypoRequest(typo)))
                  .has_value();
    });
    const bool completed = learning.wait_for(1s) == std::future_status::ready;
    const bool update_waiting = update.wait_for(0s) == std::future_status::timeout;
    // Always unblock both async tasks before any fatal test assertion.
    release.set_value();
    model.get();
    update.get();
    const bool correct = learning.get();
    EXPECT_TRUE(published);
    EXPECT_TRUE(update_waiting);
    EXPECT_TRUE(completed) << "learning waited behind model loading";
    EXPECT_TRUE(correct);
  }
  std::remove(path.c_str());
}

namespace {

// Blocks inside Convert until the request's cancel flag is raised, the way a
// long Zenzai decode polls its abort callback.
class CancelObservingConverter final : public azookey::core::IConverter {
 public:
  std::vector<azookey::core::Candidate> Convert(
      const std::string& kana, const azookey::core::ConversionContext& context) override {
    entered_.store(true);
    const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < give_up) {
      if (context.cancel && context.cancel->load()) {
        saw_cancel_.store(true);
        return {};
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return {azookey::core::Candidate{kana, kana, 1.0, azookey::core::CandidateSource::Heuristic,
                                     "not-canceled"}};
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

  const std::atomic<bool>& entered() const { return entered_; }
  const std::atomic<bool>& saw_cancel() const { return saw_cancel_; }

 private:
  std::atomic<bool> entered_{false};
  std::atomic<bool> saw_cancel_{false};
};

class BlockingPredictionConverter final : public azookey::core::IConverter {
 public:
  explicit BlockingPredictionConverter(std::shared_future<void> release)
      : release_(std::move(release)) {}

  std::future<void> Entered() { return entered_.get_future(); }

  std::vector<azookey::core::Candidate> Convert(const std::string&,
                                                const azookey::core::ConversionContext&) override {
    return {};
  }
  std::vector<azookey::core::Candidate> PredictNext(
      const std::string& kana, const azookey::core::ConversionContext&) override {
    entered_.set_value();
    release_.wait();
    return {{kana, kana, 1.0, azookey::core::CandidateSource::Heuristic, "test"}};
  }
  std::vector<azookey::core::Candidate> Correct(const std::string&,
                                                const azookey::core::CorrectionHint&,
                                                const azookey::core::ConversionContext&) override {
    return {};
  }
  void Commit(const azookey::core::Candidate&, const azookey::core::ConversionContext&) override {}
  void Learn(const std::string&, const std::string&) override {}

 private:
  std::promise<void> entered_;
  std::shared_future<void> release_;
};

}  // namespace

TEST_F(DispatcherTest, QueryPredictionsDropsStaleResultAfterNewerRequest) {
  std::promise<void> release;
  auto converter = std::make_unique<BlockingPredictionConverter>(release.get_future().share());
  auto entered = converter->Entered();
  azookey::host::InferenceEngine local_engine(std::move(converter), &store, {});
  azookey::host::RequestScheduler local_scheduler;
  azookey::host::Dispatcher local_dispatcher(&local_engine, &local_scheduler, &user_dict,
                                             DefaultDispatcherConfig());
  const auto request = MakeReq(300, ipc::MessageType::QueryPredictions,
                               ipc::BuildQueryPredictionsRequest({"にほん", "", "word"}));
  auto pending = std::async(std::launch::async, [&] { return local_dispatcher.Dispatch(request); });
  const bool started = entered.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
  if (started) local_scheduler.MarkLatest(301);
  release.set_value();
  const auto response = pending.get();
  ASSERT_TRUE(started);
  EXPECT_FALSE(response);
  EXPECT_FALSE(local_scheduler.IsCanceled(300));
}

// M47 section 8.5.2: an out-of-band Cancel stops the conversion itself, not
// only the reply, so a converter that runs past the TIP's deadline does not
// hold the next request back.
TEST_F(DispatcherTest, OutOfBandCancelReachesTheRunningConverter) {
  const std::string path = TempPath("azookey_dispatcher_cancel_reaches_converter.tsv");
  std::remove(path.c_str());
  azookey::learning::LearningStore cancel_store(path, &azookey::learning::test::Crypto());
  auto converter = std::make_unique<CancelObservingConverter>();
  auto* observed = converter.get();
  azookey::host::InferenceEngine cancel_engine(std::move(converter), &cancel_store, {});
  azookey::host::RequestScheduler cancel_scheduler;
  azookey::host::Dispatcher primary(&cancel_engine, &cancel_scheduler, nullptr,
                                    DefaultDispatcherConfig());
  azookey::host::Dispatcher control(&cancel_engine, &cancel_scheduler, nullptr,
                                    DefaultDispatcherConfig());
  auto handshake = [this](azookey::host::Dispatcher& connection, uint64_t request_id) {
    ipc::HandshakeRequest req;
    req.tip_version = "0.1.0";
    req.protocol_version = kProtocolVersion;
    req.client_id = "cancel-client";
    auto response = connection.Dispatch(
        MakeReq(request_id, ipc::MessageType::Handshake, ipc::BuildHandshakeRequest(req)));
    return response && ipc::ParseHandshakeResponse(response->payload_json)->accepted;
  };
  ASSERT_TRUE(handshake(primary, 1));
  ASSERT_TRUE(handshake(control, 2));

  ipc::QueryCandidatesRequest query;
  query.reading = "わたし";
  auto pending = std::async(std::launch::async, [&] {
    return primary.Dispatch(
        MakeReq(90, ipc::MessageType::QueryCandidates, ipc::BuildQueryCandidatesRequest(query)));
  });
  const auto entered_by = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!observed->entered().load() && std::chrono::steady_clock::now() < entered_by) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_TRUE(observed->entered().load());

  const auto canceled_at = std::chrono::steady_clock::now();
  ipc::CancelPayload cancel;
  cancel.target_request_id = 90;
  EXPECT_FALSE(
      control.Dispatch(MakeReq(3, ipc::MessageType::Cancel, ipc::BuildCancel(cancel))).has_value());
  ASSERT_EQ(pending.wait_for(std::chrono::seconds(3)), std::future_status::ready);
  EXPECT_LT(std::chrono::steady_clock::now() - canceled_at, std::chrono::seconds(3));
  EXPECT_FALSE(pending.get().has_value());
  EXPECT_TRUE(observed->saw_cancel().load());

  cancel_engine.FlushLearningStore();
  std::remove(path.c_str());
}

TEST_F(DispatcherTest, OutOfBandCancelReachesPredictionConverter) {
  const auto log_dir =
      std::filesystem::temp_directory_path() /
      ("azookey_prediction_cancel_trace_" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  azookey::logging::RuntimeLoggerOptions options;
  options.enabled = true;
  options.component = "host";
  options.logs_directory = log_dir;
  azookey::logging::RuntimeLogger logger(options);
  auto converter = std::make_unique<CancelObservingConverter>();
  auto* observed = converter.get();
  azookey::host::InferenceEngine local_engine(std::move(converter), &store, {}, &logger);
  azookey::host::RequestScheduler local_scheduler;
  azookey::host::Dispatcher primary(&local_engine, &local_scheduler, &user_dict,
                                    DefaultDispatcherConfig(), nullptr, nullptr, &logger);
  azookey::host::Dispatcher control(&local_engine, &local_scheduler, &user_dict,
                                    DefaultDispatcherConfig());
  auto handshake = [this](azookey::host::Dispatcher& connection, uint64_t request_id) {
    ipc::HandshakeRequest req;
    req.tip_version = "0.1.0";
    req.protocol_version = kProtocolVersion;
    req.client_id = "prediction-cancel-client";
    const auto response = connection.Dispatch(
        MakeReq(request_id, ipc::MessageType::Handshake, ipc::BuildHandshakeRequest(req)));
    return response && ipc::ParseHandshakeResponse(response->payload_json)->accepted;
  };
  ASSERT_TRUE(handshake(primary, 1));
  ASSERT_TRUE(handshake(control, 2));

  auto prediction = MakeReq(91, ipc::MessageType::QueryPredictions,
                            ipc::BuildQueryPredictionsRequest({"わたし", "", "word"}));
  prediction.trace_id = "018fd2c2-2a3e-7c9a-b8e1-7f3a92d4c5e2";
  auto pending = std::async(std::launch::async, [&] { return primary.Dispatch(prediction); });
  const auto entered_by = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!observed->entered().load() && std::chrono::steady_clock::now() < entered_by) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_TRUE(observed->entered().load());
  ipc::CancelPayload cancel;
  cancel.target_request_id = 91;
  EXPECT_FALSE(
      control.Dispatch(MakeReq(3, ipc::MessageType::Cancel, ipc::BuildCancel(cancel))).has_value());
  EXPECT_EQ(pending.wait_for(std::chrono::seconds(3)), std::future_status::ready);
  EXPECT_FALSE(pending.get().has_value());
  EXPECT_TRUE(observed->saw_cancel().load());
  bool canceled_inference_phase = false;
  ASSERT_TRUE(std::filesystem::exists(log_dir));
  for (const auto& file : std::filesystem::directory_iterator(log_dir)) {
    if (file.path().extension() != ".jsonl") continue;
    std::ifstream stream(file.path());
    for (std::string line; std::getline(stream, line);) {
      const auto record = ipc::json::Parse(line);
      if (!record || record->GetString("event") != "trace_phase") continue;
      canceled_inference_phase |= record->GetString("trace_id") == prediction.trace_id &&
                                  record->GetString("phase") == "model_inference" &&
                                  record->GetString("result") == "cancelled";
    }
  }
  EXPECT_TRUE(canceled_inference_phase);
  RemovePathNoThrow(log_dir);
}

// M47 section 8.5.3 / 12.6: SafeMode is reported first, refuses model loads,
// and ends when the user turns the flag off.
TEST_F(DispatcherTest, SafeModeIsReportedRefusesModelLoadsAndClearsOnUpdateConfig) {
  const auto settings_path = TempPath("azookey_dispatcher_safe_mode_settings.json");
  std::remove(settings_path.c_str());
  {
    std::ofstream out(settings_path, std::ios::binary);
    out << R"({"model":{"enabled":false},"safeMode":{"enabled":true,"lastCrashCount":3}})";
  }
  azookey::host::SettingsStore settings_store(settings_path);
  azookey::host::Dispatcher config_dispatcher(&engine, &scheduler, &user_dict,
                                              DefaultDispatcherConfig(), &settings_store);
  auto update = config_dispatcher.Dispatch(MakeReq(80, ipc::MessageType::UpdateConfig, "{}"));
  ASSERT_TRUE(update.has_value());
  EXPECT_EQ(engine.health_state(), azookey::host::HealthState::SafeMode);

  const auto fallback_state = [&](uint64_t id) {
    auto response =
        config_dispatcher.Dispatch(MakeReq(id, ipc::MessageType::QueryDiagnostics, "{}"));
    EXPECT_TRUE(response.has_value());
    const auto parsed = ipc::ParseQueryDiagnostics(response->payload_json);
    EXPECT_TRUE(parsed.has_value());
    return parsed ? parsed->fallback_state : std::string();
  };
  // model.enabled=false would otherwise read as healthy (D-009).
  EXPECT_EQ(fallback_state(81), "safe_mode");

  const auto model_path = TempPath("azookey_dispatcher_safe_mode_model.gguf");
  WriteMinimalGguf(model_path);
  ipc::LoadModelRequest load;
  load.path = model_path;
  load.backend = "cpu";
  auto loaded = config_dispatcher.Dispatch(
      MakeReq(82, ipc::MessageType::LoadModel, ipc::BuildLoadModelRequest(load)));
  ASSERT_TRUE(loaded.has_value());
  const auto load_result = ipc::ParseLoadModelResponse(loaded->payload_json);
  ASSERT_TRUE(load_result.has_value());
  EXPECT_FALSE(load_result->ok);
  EXPECT_EQ(load_result->error, "safe_mode");
  EXPECT_FALSE(engine.model_loaded());

  {
    std::ofstream out(settings_path, std::ios::binary | std::ios::trunc);
    out << R"({"model":{"enabled":false},"safeMode":{"enabled":false,"lastCrashCount":3}})";
  }
  update = config_dispatcher.Dispatch(MakeReq(83, ipc::MessageType::UpdateConfig, "{}"));
  ASSERT_TRUE(update.has_value());
  EXPECT_EQ(engine.health_state(), azookey::host::HealthState::Healthy);
  EXPECT_EQ(fallback_state(84), "healthy");

  std::remove(model_path.c_str());
  std::remove(settings_path.c_str());
}

TEST_F(DispatcherTest, LearningDataListForgetExportImportRoundTrip) {
  const auto now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                             std::chrono::system_clock::now().time_since_epoch())
                                             .count());
  EnableEventPrivacy(dispatcher);
  azookey::learning::test::TestByteCrypto backup_crypto;
  engine.SetBackupCrypto(&backup_crypto);
  ipc::CommitObservationRequest commit;
  commit.secure = false;
  commit.learning_allowed = true;
  commit.reading = "にほん";
  commit.chosen = {"日本", "にほん", 1.0, "model"};
  commit.left_context = "ここは";
  commit.app = ipc::AppIdentity{"Code.exe", "Chrome_WidgetWin_1"};
  ASSERT_TRUE(dispatcher.Dispatch(MakeReq(700, ipc::MessageType::CommitObservation,
                                          ipc::BuildCommitObservationRequest(commit))));
  // The commit carries its app into the M54 app row.
  const auto* rows = store.Rows("にほん", "日本");
  ASSERT_NE(rows, nullptr);
  ASSERT_EQ(rows->count("code.exe"), 1u);
  EXPECT_EQ(rows->at("code.exe").context_hash, azookey::learning::ContextHash("ここは"));

  ipc::ListLearningEntriesRequest list;
  list.store = "learning";
  auto response = dispatcher.Dispatch(MakeReq(701, ipc::MessageType::ListLearningEntries,
                                              ipc::BuildListLearningEntriesRequest(list)));
  ASSERT_TRUE(response);
  auto listed = ipc::ParseListLearningEntriesResponse(response->payload_json);
  ASSERT_TRUE(listed && listed->ok);
  ASSERT_EQ(listed->total, 1u);
  EXPECT_EQ(listed->entries[0].channel, "kana");
  EXPECT_EQ(listed->entries[0].tags, std::vector<std::string>{"code.exe"});

  const auto dir = std::filesystem::temp_directory_path() / "azookey_dispatcher_learning_backup";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  ipc::ExportLearningDataRequest export_request;
  export_request.stores = {"learning", "user_dict"};
  export_request.destination_path = azookey::core::PathToUtf8(dir / "backup.zip");
  response = dispatcher.Dispatch(MakeReq(702, ipc::MessageType::ExportLearningData,
                                         ipc::BuildExportLearningDataRequest(export_request)));
  ASSERT_TRUE(response);
  const auto exported = ipc::ParseExportLearningDataResponse(response->payload_json);
  ASSERT_TRUE(exported);
  EXPECT_EQ(exported->status, "success") << exported->error.value_or("");
  EXPECT_GT(exported->file_size_bytes, 0u);

  ipc::ForgetLearningEntryRequest forget;
  forget.store = "learning";
  forget.id = listed->entries[0].id;
  response = dispatcher.Dispatch(MakeReq(703, ipc::MessageType::ForgetLearningEntry,
                                         ipc::BuildForgetLearningEntryRequest(forget)));
  ASSERT_TRUE(response);
  auto forgot = ipc::ParseForgetLearningEntryResponse(response->payload_json);
  ASSERT_TRUE(forgot && forgot->ok);
  EXPECT_TRUE(forgot->removed);
  EXPECT_DOUBLE_EQ(store.Score("にほん", "日本", now), 0.0);

  ipc::ImportLearningDataRequest import_request;
  import_request.source_path = export_request.destination_path;
  import_request.stores = {"learning"};
  response = dispatcher.Dispatch(MakeReq(704, ipc::MessageType::ImportLearningData,
                                         ipc::BuildImportLearningDataRequest(import_request)));
  ASSERT_TRUE(response);
  const auto imported = ipc::ParseImportLearningDataResponse(response->payload_json);
  ASSERT_TRUE(imported);
  EXPECT_EQ(imported->status, "success") << imported->error.value_or("");
  EXPECT_EQ(imported->imported_counts.at("learning"), 1u);
  EXPECT_GT(store.Score("にほん", "日本", now), 0.0);

  // The TIP form names the pair instead of an id.
  forget = {};
  forget.store = "learning";
  forget.reading = "にほん";
  forget.surface = "日本";
  response = dispatcher.Dispatch(MakeReq(705, ipc::MessageType::ForgetLearningEntry,
                                         ipc::BuildForgetLearningEntryRequest(forget)));
  forgot = ipc::ParseForgetLearningEntryResponse(response->payload_json);
  ASSERT_TRUE(forgot && forgot->ok);
  EXPECT_TRUE(forgot->removed);
  std::filesystem::remove_all(dir);
}

TEST_F(DispatcherTest, LearningDataRequestsRejectBadInput) {
  ipc::ListLearningEntriesRequest list;
  list.store = "everything";
  auto response = dispatcher.Dispatch(MakeReq(710, ipc::MessageType::ListLearningEntries,
                                              ipc::BuildListLearningEntriesRequest(list)));
  auto listed = ipc::ParseListLearningEntriesResponse(response->payload_json);
  ASSERT_TRUE(listed);
  EXPECT_FALSE(listed->ok);
  EXPECT_EQ(listed->error, "invalid_request");

  ipc::ExportLearningDataRequest export_request;
  export_request.stores = {"learning"};
  export_request.destination_path = "relative.zip";
  response = dispatcher.Dispatch(MakeReq(711, ipc::MessageType::ExportLearningData,
                                         ipc::BuildExportLearningDataRequest(export_request)));
  auto exported = ipc::ParseExportLearningDataResponse(response->payload_json);
  ASSERT_TRUE(exported);
  EXPECT_EQ(exported->status, "error");
  EXPECT_EQ(exported->error, "invalid_path");

  export_request.include_settings = true;
  response = dispatcher.Dispatch(MakeReq(712, ipc::MessageType::ExportLearningData,
                                         ipc::BuildExportLearningDataRequest(export_request)));
  exported = ipc::ParseExportLearningDataResponse(response->payload_json);
  ASSERT_TRUE(exported);
  EXPECT_EQ(exported->error, "unsupported");

  ipc::ForgetLearningEntryRequest forget;
  forget.store = "learning";
  forget.id = "0000000000000000";
  response = dispatcher.Dispatch(MakeReq(713, ipc::MessageType::ForgetLearningEntry,
                                         ipc::BuildForgetLearningEntryRequest(forget)));
  auto forgot = ipc::ParseForgetLearningEntryResponse(response->payload_json);
  ASSERT_TRUE(forgot && forgot->ok);
  EXPECT_FALSE(forgot->removed);
}

TEST_F(DispatcherTest, LearningDataRequestsNeedAnAuthenticatedSession) {
  auto config = DefaultDispatcherConfig();
  config.handshake_token = "expected-token";
  azookey::host::Dispatcher token_dispatcher(&engine, &scheduler, &user_dict, config);
  const auto list = token_dispatcher.Dispatch(
      MakeReq(720, ipc::MessageType::ListLearningEntries, R"({"store":"learning"})"));
  ASSERT_TRUE(list);
  const auto listed = ipc::ParseListLearningEntriesResponse(list->payload_json);
  ASSERT_TRUE(listed);
  EXPECT_FALSE(listed->ok);
  EXPECT_EQ(listed->error, "not_authenticated");
  for (const auto type :
       {ipc::MessageType::ForgetLearningEntry, ipc::MessageType::ExportLearningData,
        ipc::MessageType::ImportLearningData, ipc::MessageType::ResetLearningStore,
        ipc::MessageType::QueryPersona, ipc::MessageType::DetectAnomalies}) {
    const auto reply = token_dispatcher.Dispatch(MakeReq(721, type, "{}"));
    ASSERT_TRUE(reply);
    EXPECT_NE(reply->payload_json.find("not_authenticated"), std::string::npos);
  }
}

// learning-data-management-spec section 4.6: the reset empties the store on
// disk, so a fresh load finds nothing, and an unknown store is refused.
TEST_F(DispatcherTest, ResetLearningStoreEmptiesTheStoreAndItsFile) {
  EnableEventPrivacy(dispatcher);
  ipc::CommitObservationRequest commit;
  commit.secure = false;
  commit.learning_allowed = true;
  commit.reading = "にほん";
  commit.chosen = {"日本", "にほん", 1.0, "model"};
  ASSERT_TRUE(dispatcher.Dispatch(MakeReq(730, ipc::MessageType::CommitObservation,
                                          ipc::BuildCommitObservationRequest(commit))));
  ASSERT_TRUE(engine.FlushLearningStore());
  ASSERT_EQ(store.size(), 1u);

  const auto reset_of = [&](uint64_t id, const std::string& name) {
    ipc::ResetLearningStoreRequest request;
    request.store = name;
    const auto response = dispatcher.Dispatch(MakeReq(
        id, ipc::MessageType::ResetLearningStore, ipc::BuildResetLearningStoreRequest(request)));
    EXPECT_TRUE(response.has_value());
    return response ? ipc::ParseResetLearningStoreResponse(response->payload_json) : std::nullopt;
  };
  const auto reset = reset_of(731, "learning");
  ASSERT_TRUE(reset.has_value());
  EXPECT_TRUE(reset->ok);
  EXPECT_FALSE(reset->error.has_value());
  EXPECT_EQ(store.size(), 0u);
  azookey::learning::LearningStore reloaded(learning_path, &azookey::learning::test::Crypto());
  ASSERT_TRUE(reloaded.Load());
  EXPECT_EQ(reloaded.size(), 0u);

  ipc::AddUserWordRequest add;
  add.word = "azooKey";
  add.ruby = "あずきー";
  ASSERT_TRUE(dispatcher.Dispatch(
      MakeReq(732, ipc::MessageType::AddUserWord, ipc::BuildAddUserWordRequest(add))));
  ASSERT_FALSE(user_dict.Lookup("あずきー").empty());
  const auto user_dict_reset = reset_of(733, "user_dict");
  ASSERT_TRUE(user_dict_reset.has_value());
  EXPECT_TRUE(user_dict_reset->ok);
  EXPECT_TRUE(user_dict.Lookup("あずきー").empty());

  // The fixture engine has no typo store.
  const auto typo_reset = reset_of(734, "typo");
  ASSERT_TRUE(typo_reset.has_value());
  EXPECT_FALSE(typo_reset->ok);
  EXPECT_EQ(typo_reset->error, std::optional<std::string>("store_unavailable"));

  const auto unknown = reset_of(735, "everything");
  ASSERT_TRUE(unknown.has_value());
  EXPECT_FALSE(unknown->ok);
  EXPECT_EQ(unknown->error, std::optional<std::string>("invalid_request"));
}

// rich-features-spec X-2-7: QueryPersona returns the cached ratios, and only
// the ratios.
TEST_F(DispatcherTest, QueryPersonaReturnsTheRatiosOfTheLearningStore) {
  EnableEventPrivacy(dispatcher);
  for (const auto& [reading, surface] :
       {std::pair{"します", "します"}, std::pair{"だよ", "だよ"}, std::pair{"ゆーざー", "user_id"},
        std::pair{"にほん", "日本"}}) {
    ipc::CommitObservationRequest commit;
    commit.secure = false;
    commit.learning_allowed = true;
    commit.reading = reading;
    commit.chosen = {surface, reading, 1.0, "model"};
    ASSERT_TRUE(dispatcher.Dispatch(MakeReq(740, ipc::MessageType::CommitObservation,
                                            ipc::BuildCommitObservationRequest(commit))));
  }
  engine.RefreshPersona();

  const auto response = dispatcher.Dispatch(MakeReq(741, ipc::MessageType::QueryPersona, "{}"));
  ASSERT_TRUE(response.has_value());
  EXPECT_EQ(response->type, ipc::MessageType::QueryPersona);
  const auto persona = ipc::ParseQueryPersonaResponse(response->payload_json);
  ASSERT_TRUE(persona.has_value());
  EXPECT_TRUE(persona->ok);
  EXPECT_EQ(persona->sample_count, 4u);
  EXPECT_DOUBLE_EQ(persona->polite_ratio, 0.25);
  EXPECT_DOUBLE_EQ(persona->casual_ratio, 0.25);
  EXPECT_DOUBLE_EQ(persona->technical_ratio, 0.25);
  EXPECT_DOUBLE_EQ(persona->kaomoji_ratio, 0.0);
  EXPECT_GT(persona->computed_at_epoch_sec, 0u);
  EXPECT_EQ(response->payload_json.find("user_id"), std::string::npos);

  // The cache is what the 24-hour worker refreshes; a reset refreshes it too.
  ipc::ResetLearningStoreRequest reset;
  reset.store = "learning";
  ASSERT_TRUE(dispatcher.Dispatch(MakeReq(742, ipc::MessageType::ResetLearningStore,
                                          ipc::BuildResetLearningStoreRequest(reset))));
  const auto after = dispatcher.Dispatch(MakeReq(743, ipc::MessageType::QueryPersona, "{}"));
  ASSERT_TRUE(after.has_value());
  const auto emptied = ipc::ParseQueryPersonaResponse(after->payload_json);
  ASSERT_TRUE(emptied.has_value());
  EXPECT_EQ(emptied->sample_count, 0u);
}

namespace {
std::optional<ipc::Envelope> SendLearningData(azookey::host::Dispatcher& target, uint64_t id,
                                              ipc::MessageType type, const std::string& payload) {
  ipc::Envelope env;
  env.version = 1;
  env.request_id = id;
  env.trace_id = "trace-" + std::to_string(id);
  env.type = type;
  env.payload_json = payload;
  return target.Dispatch(env);
}

std::filesystem::path FreshBackupDirectory(const char* name) {
  const auto dir = std::filesystem::temp_directory_path() / name;
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  return dir;
}

void RemoveLearningFiles(const std::filesystem::path& path) {
  std::error_code ec;
  for (const auto& base : {path, azookey::learning::LearningStoreV2PathFor(path)}) {
    std::filesystem::remove(base, ec);
    std::filesystem::remove(azookey::learning::EncryptedPathFor(base), ec);
    auto backup = base;
    backup += ".bak";
    std::filesystem::remove(backup, ec);
  }
}

std::string ReadAllBytes(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void WriteAllBytes(const std::filesystem::path& path, const std::string& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << bytes;
}
}  // namespace

// learning-data-management-spec section 4.6 step 1: a store whose file could
// not be read cannot be emptied on disk, so the reset refuses before changing
// anything rather than answering ok over data that would come back.
TEST_F(DispatcherTest, ResetLearningStoreRefusesAStoreWhoseFileCouldNotBeRead) {
  const auto v2_file = azookey::learning::EncryptedPathFor(
      azookey::learning::LearningStoreV2PathFor(std::filesystem::path(learning_path)));
  WriteAllBytes(v2_file, "not a protected blob");
  ASSERT_FALSE(store.Load());
  ASSERT_TRUE(store.save_blocked());

  ipc::ResetLearningStoreRequest request;
  request.store = "learning";
  const auto response = dispatcher.Dispatch(MakeReq(750, ipc::MessageType::ResetLearningStore,
                                                    ipc::BuildResetLearningStoreRequest(request)));
  ASSERT_TRUE(response.has_value());
  const auto parsed = ipc::ParseResetLearningStoreResponse(response->payload_json);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_FALSE(parsed->ok);
  EXPECT_EQ(parsed->error, std::optional<std::string>("save_failed"));
  EXPECT_EQ(ReadAllBytes(v2_file), "not a protected blob");
  RemoveLearningFiles(learning_path);
}

// Steps 2 to 4: the v2 file is emptied first; when the M7 file then cannot be
// rewritten, the snapshot goes back into memory and onto the v2 file.
TEST_F(DispatcherTest, ResetLearningStoreRestoresTheStoreWhenTheM7FileCannotBeRewritten) {
  EnableEventPrivacy(dispatcher);
  ipc::CommitObservationRequest commit;
  commit.secure = false;
  commit.learning_allowed = true;
  commit.reading = "にほん";
  commit.chosen = {"日本", "にほん", 1.0, "model"};
  ASSERT_TRUE(dispatcher.Dispatch(MakeReq(751, ipc::MessageType::CommitObservation,
                                          ipc::BuildCommitObservationRequest(commit))));
  ASSERT_TRUE(engine.FlushLearningStore());
  const auto m7_file = azookey::learning::EncryptedPathFor(std::filesystem::path(learning_path));
  WriteAllBytes(m7_file, "not a protected blob");

  ipc::ResetLearningStoreRequest request;
  request.store = "learning";
  const auto response = dispatcher.Dispatch(MakeReq(752, ipc::MessageType::ResetLearningStore,
                                                    ipc::BuildResetLearningStoreRequest(request)));
  ASSERT_TRUE(response.has_value());
  const auto parsed = ipc::ParseResetLearningStoreResponse(response->payload_json);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_FALSE(parsed->ok);
  EXPECT_EQ(parsed->error, std::optional<std::string>("save_failed"));
  EXPECT_EQ(store.size(), 1u);
  EXPECT_EQ(ReadAllBytes(m7_file), "not a protected blob");
  azookey::learning::LearningStore reloaded(learning_path, &azookey::learning::test::Crypto());
  ASSERT_TRUE(reloaded.Load());
  EXPECT_EQ(reloaded.size(), 1u);
  RemoveLearningFiles(learning_path);
}

TEST_F(DispatcherTest, ResetLearningStoreEmptiesTheTypoAndAutoWordStores) {
  const auto typo_path = TempPath("azookey_dispatcher_reset_typo.tsv");
  const auto mining_path = TempPath("azookey_dispatcher_reset_mining.tsv");
  azookey::learning::TypoCorrectionStore typo(typo_path, &azookey::learning::test::Crypto());
  azookey::learning::AutoWordStore mining(mining_path, &azookey::learning::test::Crypto());
  ASSERT_TRUE(typo.Observe("こんちには", "こんにちは", 1700000000));
  mining.Observe("azooKey社", "あずきーしゃ", 1700000000, 3, false);
  ASSERT_EQ(typo.size(), 1u);
  ASSERT_FALSE(mining.All().empty());
  engine.SetTypoStore(&typo);
  engine.SetAutoWordStore(&mining);

  for (const auto* name : {"typo", "auto_word"}) {
    ipc::ResetLearningStoreRequest request;
    request.store = name;
    const auto response = dispatcher.Dispatch(MakeReq(
        753, ipc::MessageType::ResetLearningStore, ipc::BuildResetLearningStoreRequest(request)));
    ASSERT_TRUE(response.has_value()) << name;
    const auto parsed = ipc::ParseResetLearningStoreResponse(response->payload_json);
    ASSERT_TRUE(parsed.has_value()) << name;
    EXPECT_TRUE(parsed->ok) << name;
  }
  EXPECT_EQ(typo.size(), 0u);
  EXPECT_TRUE(mining.All().empty());
  azookey::learning::TypoCorrectionStore typo_reloaded(typo_path,
                                                       &azookey::learning::test::Crypto());
  ASSERT_TRUE(typo_reloaded.Load());
  EXPECT_EQ(typo_reloaded.size(), 0u);

  engine.SetTypoStore(nullptr);
  engine.SetAutoWordStore(nullptr);
  std::error_code ec;
  for (const auto& path : {typo_path, mining_path}) {
    std::filesystem::remove(path, ec);
    std::filesystem::remove(azookey::learning::EncryptedPathFor(path), ec);
  }
}

TEST_F(DispatcherTest, ResetLearningStoreRefusesUnreadableTypoAndAutoWordFiles) {
  const auto typo_path = TempPath("azookey_dispatcher_reset_blocked_typo.tsv");
  const auto mining_path = TempPath("azookey_dispatcher_reset_blocked_mining.tsv");
  for (const auto& path : {typo_path, mining_path}) {
    WriteAllBytes(azookey::learning::EncryptedPathFor(path), "not a protected blob");
  }
  azookey::learning::TypoCorrectionStore typo(typo_path, &azookey::learning::test::Crypto());
  azookey::learning::AutoWordStore mining(mining_path, &azookey::learning::test::Crypto());
  ASSERT_FALSE(typo.Load());
  ASSERT_FALSE(mining.Load());
  ASSERT_TRUE(typo.save_blocked());
  ASSERT_TRUE(mining.save_blocked());
  engine.SetTypoStore(&typo);
  engine.SetAutoWordStore(&mining);

  for (const auto* name : {"typo", "auto_word"}) {
    ipc::ResetLearningStoreRequest request;
    request.store = name;
    const auto response = dispatcher.Dispatch(MakeReq(
        754, ipc::MessageType::ResetLearningStore, ipc::BuildResetLearningStoreRequest(request)));
    ASSERT_TRUE(response.has_value()) << name;
    const auto parsed = ipc::ParseResetLearningStoreResponse(response->payload_json);
    ASSERT_TRUE(parsed.has_value()) << name;
    EXPECT_FALSE(parsed->ok) << name;
    EXPECT_EQ(parsed->error, std::optional<std::string>("save_failed")) << name;
  }
  for (const auto& path : {typo_path, mining_path}) {
    EXPECT_EQ(ReadAllBytes(azookey::learning::EncryptedPathFor(path)), "not a protected blob");
  }

  engine.SetTypoStore(nullptr);
  engine.SetAutoWordStore(nullptr);
  std::error_code ec;
  for (const auto& path : {typo_path, mining_path}) {
    std::filesystem::remove(azookey::learning::EncryptedPathFor(path), ec);
  }
}

// The persona follows a forget of the kana channel, as it follows a reset.
TEST_F(DispatcherTest, ForgettingALearningPairRefreshesThePersona) {
  EnableEventPrivacy(dispatcher);
  ipc::CommitObservationRequest commit;
  commit.secure = false;
  commit.learning_allowed = true;
  commit.reading = "します";
  commit.chosen = {"します", "します", 1.0, "model"};
  ASSERT_TRUE(dispatcher.Dispatch(MakeReq(755, ipc::MessageType::CommitObservation,
                                          ipc::BuildCommitObservationRequest(commit))));
  engine.RefreshPersona();
  ASSERT_EQ(engine.CurrentPersona()->persona.sample_count, 1u);

  ipc::ForgetLearningEntryRequest forget;
  forget.store = "learning";
  forget.reading = "します";
  forget.surface = "します";
  ASSERT_TRUE(dispatcher.Dispatch(MakeReq(756, ipc::MessageType::ForgetLearningEntry,
                                          ipc::BuildForgetLearningEntryRequest(forget))));
  EXPECT_EQ(engine.CurrentPersona()->persona.sample_count, 0u);
  RemoveLearningFiles(learning_path);
}

// Without a settings store there is no AI consent at all.
TEST_F(DispatcherTest, DetectAnomaliesWithoutSettingsSendsNothing) {
  unsigned calls = 0;
  auto config = DefaultDispatcherConfig();
  config.ai_backend = std::make_shared<azookey::host::AiBackend>(
      [&](const auto&, const std::string&, const auto*, auto) {
        ++calls;
        return azookey::host::AiHttpResponse{};
      });
  azookey::host::Dispatcher handler(&engine, &scheduler, &user_dict, config);
  ipc::DetectAnomaliesRequest request;
  request.text = "今日は晴れでした。";
  request.secure = false;
  request.learning_allowed = true;
  const auto response = handler.Dispatch(
      MakeReq(770, ipc::MessageType::DetectAnomalies, ipc::BuildDetectAnomaliesRequest(request)));
  ASSERT_TRUE(response.has_value());
  EXPECT_EQ(ipc::ParseDetectAnomaliesResponse(response->payload_json)->error,
            std::optional<std::string>("unsupported"));
  EXPECT_EQ(calls, 0u);
}

// rich-features-spec X-3-6 (DEV-1532): the remote AI backend finds the spans,
// the Host places them; privacy and a missing backend are refused explicitly.
TEST_F(DispatcherTest, DetectAnomaliesPlacesTheBackendFindingsAndHonorsPrivacy) {
  const auto settings_path = TempPath("azookey_detect_anomalies_settings.json");
  {
    std::ofstream file(settings_path);
    file << R"({"aiBackend":"openai","openAiApiKey":"test-only-placeholder"})";
  }
  azookey::host::SettingsStore settings(settings_path);
  settings.Load();
  std::string sent;
  std::string content = R"({"result":"[]"})";
  auto config = DefaultDispatcherConfig();
  config.ai_backend = std::make_shared<azookey::host::AiBackend>(
      [&](const auto&, const std::string& body, const auto*, auto) {
        sent = body;
        ipc::json::Object message{{"content", content}};
        ipc::json::Object choice{{"message", std::move(message)}, {"finish_reason", "stop"}};
        ipc::json::Array choices;
        choices.emplace_back(std::move(choice));
        return azookey::host::AiHttpResponse{
            200, ipc::json::Stringify(ipc::json::Object{{"choices", std::move(choices)}})};
      });
  azookey::host::Dispatcher handler(&engine, &scheduler, &user_dict, config, &settings);
  const auto detect = [&](uint64_t id, const ipc::DetectAnomaliesRequest& request) {
    const auto response = handler.Dispatch(
        MakeReq(id, ipc::MessageType::DetectAnomalies, ipc::BuildDetectAnomaliesRequest(request)));
    EXPECT_TRUE(response.has_value());
    return response ? ipc::ParseDetectAnomaliesResponse(response->payload_json) : std::nullopt;
  };
  ipc::DetectAnomaliesRequest request;
  request.text = "今日は晴れでした。";
  request.secure = false;
  request.learning_allowed = true;
  ipc::json::Array findings;
  findings.emplace_back(ipc::json::Object{{"quote", "晴れでした"},
                                          {"reason", "時制が合わない"},
                                          {"suggestions", ipc::json::Array{"晴れです"}},
                                          {"confidence", 0.9}});
  content = ipc::json::Stringify(
      ipc::json::Object{{"result", ipc::json::Stringify(ipc::json::Value(std::move(findings)))}});

  const auto found = detect(760, request);
  ASSERT_TRUE(found.has_value());
  EXPECT_TRUE(found->ok) << found->error.value_or("");
  ASSERT_EQ(found->findings.size(), 1u);
  EXPECT_EQ(found->findings[0].start, 3u);
  EXPECT_EQ(found->findings[0].length, 5u);
  EXPECT_EQ(found->findings[0].suggestions, std::vector<std::string>{"晴れです"});
  EXPECT_NE(sent.find("今日は晴れでした"), std::string::npos);

  // A result that is not a findings array is a backend failure, not "none".
  content = R"({"result":"looks fine"})";
  const auto garbled = detect(761, request);
  ASSERT_TRUE(garbled.has_value());
  EXPECT_FALSE(garbled->ok);
  EXPECT_EQ(garbled->error, std::optional<std::string>("backend_failed"));

  // Text from a secure or learning-disallowed context is never sent.
  sent.clear();
  auto secure = request;
  secure.secure = true;
  const auto blocked = detect(762, secure);
  ASSERT_TRUE(blocked.has_value());
  EXPECT_EQ(blocked->error, std::optional<std::string>("blocked"));
  auto not_allowed = request;
  not_allowed.learning_allowed = false;
  EXPECT_EQ(detect(763, not_allowed)->error, std::optional<std::string>("blocked"));
  EXPECT_TRUE(sent.empty());

  // Without a configured backend the answer is "unsupported".
  {
    std::ofstream file(settings_path, std::ios::trunc);
    file << R"({"aiBackend":"none"})";
  }
  settings.Reload();
  const auto unsupported = detect(764, request);
  ASSERT_TRUE(unsupported.has_value());
  EXPECT_EQ(unsupported->error, std::optional<std::string>("unsupported"));
  EXPECT_TRUE(sent.empty());

  const auto invalid = handler.Dispatch(MakeReq(765, ipc::MessageType::DetectAnomalies, "{}"));
  ASSERT_TRUE(invalid.has_value());
  EXPECT_EQ(ipc::ParseDetectAnomaliesResponse(invalid->payload_json)->error,
            std::optional<std::string>("invalid_request"));

  // An openai backend without an API key is a setting to make, not a failure.
  {
    std::ofstream file(settings_path, std::ios::trunc);
    file << R"({"aiBackend":"openai"})";
  }
  settings.Reload();
  EXPECT_EQ(detect(766, request)->error, std::optional<std::string>("unsupported"));
  EXPECT_TRUE(sent.empty());

  // The Host's own privacy setting blocks the text before any backend sees it.
  {
    std::ofstream file(settings_path, std::ios::trunc);
    file << R"({"aiBackend":"openai","openAiApiKey":"test-only-placeholder",)"
            R"("privacy":{"mode":"private"}})";
  }
  settings.Reload();
  EXPECT_EQ(detect(767, request)->error, std::optional<std::string>("blocked"));
  EXPECT_TRUE(sent.empty());

  // Without AI consent (ai off, or external off with openai and a key), nothing
  // is sent and the answer is "unsupported".
  for (
      const auto* privacy :
      {R"("privacy":{"mode":"custom","custom":{"learning":true,"aiCandidate":false}})",
       R"("privacy":{"mode":"custom","custom":{"learning":true,"aiCandidate":true,"externalAi":false}})"}) {
    {
      std::ofstream file(settings_path, std::ios::trunc);
      file << R"({"aiBackend":"openai","openAiApiKey":"test-only-placeholder",)" << privacy << "}";
    }
    settings.Reload();
    const auto refused = detect(769, request);
    ASSERT_TRUE(refused.has_value()) << privacy;
    EXPECT_EQ(refused->error, std::optional<std::string>("unsupported")) << privacy;
    EXPECT_TRUE(sent.empty()) << privacy;
  }

  // A request canceled before it runs gets no reply.
  {
    std::ofstream file(settings_path, std::ios::trunc);
    file << R"({"aiBackend":"openai","openAiApiKey":"test-only-placeholder"})";
  }
  settings.Reload();
  scheduler.Cancel(768);
  EXPECT_FALSE(handler.Dispatch(
      MakeReq(768, ipc::MessageType::DetectAnomalies, ipc::BuildDetectAnomaliesRequest(request))));
  std::remove(settings_path.c_str());
}

TEST_F(DispatcherTest, LearningDataListsAndForgetsTheEnglishChannel) {
  const std::filesystem::path english_path = "azookey_dispatcher_test_english.tsv";
  RemoveLearningFiles(english_path);
  azookey::learning::LearningStore english(english_path, &azookey::learning::test::Crypto());
  english.Observe("apple", "Apple Inc.", 0.8, 1'700'000'000);
  engine.SetEnglishLearningStore(&english);

  ipc::ListLearningEntriesRequest list;
  list.store = "learning";
  auto reply = SendLearningData(dispatcher, 730, ipc::MessageType::ListLearningEntries,
                                ipc::BuildListLearningEntriesRequest(list));
  const auto listed = ipc::ParseListLearningEntriesResponse(reply->payload_json);
  ASSERT_TRUE(listed && listed->ok);
  ASSERT_EQ(listed->total, 1u);
  EXPECT_EQ(listed->entries[0].channel, "english");

  ipc::ForgetLearningEntryRequest forget;
  forget.store = "learning";
  forget.id = listed->entries[0].id;
  reply = SendLearningData(dispatcher, 731, ipc::MessageType::ForgetLearningEntry,
                           ipc::BuildForgetLearningEntryRequest(forget));
  const auto forgot = ipc::ParseForgetLearningEntryResponse(reply->payload_json);
  ASSERT_TRUE(forgot && forgot->ok);
  EXPECT_TRUE(forgot->removed);
  EXPECT_DOUBLE_EQ(english.Score("apple", "Apple Inc.", 1'700'000'000), 0.0);
  engine.SetEnglishLearningStore(nullptr);
  RemoveLearningFiles(english_path);

  // The pair form addresses the kana channel only.
  forget = {};
  forget.store = "typo";
  forget.reading = "あ";
  forget.surface = "亜";
  reply = SendLearningData(dispatcher, 732, ipc::MessageType::ForgetLearningEntry,
                           ipc::BuildForgetLearningEntryRequest(forget));
  const auto rejected = ipc::ParseForgetLearningEntryResponse(reply->payload_json);
  ASSERT_TRUE(rejected);
  EXPECT_FALSE(rejected->ok);
}

TEST_F(DispatcherTest, LearningDataExportImportRestoresContentUnderEachPolicy) {
  azookey::learning::test::TestByteCrypto backup_crypto;
  engine.SetBackupCrypto(&backup_crypto);
  constexpr uint64_t kWhen = 1'700'000'000;
  store.Observe("にほん", "日本", 0.8, kWhen);
  store.Observe("にほん", "日本", 0.8, kWhen);
  const auto original = store.Rows("にほん", "日本")->at("");

  const auto dir = FreshBackupDirectory("azookey_dispatcher_backup_policies");
  ipc::ExportLearningDataRequest export_request;
  export_request.stores = {"learning"};
  export_request.destination_path = azookey::core::PathToUtf8(dir / "backup.zip");
  auto reply = SendLearningData(dispatcher, 740, ipc::MessageType::ExportLearningData,
                                ipc::BuildExportLearningDataRequest(export_request));
  const auto exported = ipc::ParseExportLearningDataResponse(reply->payload_json);
  ASSERT_TRUE(exported);
  ASSERT_EQ(exported->status, "success") << exported->error.value_or("");

  const auto import_with = [&](const char* policy, uint64_t id) {
    ipc::ImportLearningDataRequest request;
    request.source_path = export_request.destination_path;
    request.stores = {"learning"};
    request.conflict_resolution = policy;
    const auto response = SendLearningData(dispatcher, id, ipc::MessageType::ImportLearningData,
                                           ipc::BuildImportLearningDataRequest(request));
    return ipc::ParseImportLearningDataResponse(response->payload_json);
  };

  // Round trip: forget, import, and every column comes back.
  store.Forget("にほん", "日本");
  auto imported = import_with("merge", 741);
  ASSERT_TRUE(imported);
  ASSERT_EQ(imported->status, "success") << imported->error.value_or("");
  const auto restored = store.Rows("にほん", "日本")->at("");
  EXPECT_DOUBLE_EQ(restored.weight, original.weight);
  EXPECT_EQ(restored.commit_count, original.commit_count);
  EXPECT_EQ(restored.last_updated_epoch_sec, original.last_updated_epoch_sec);

  // merge adds, overwrite replaces, keep_both keeps the local row.
  imported = import_with("merge", 742);
  ASSERT_TRUE(imported);
  EXPECT_EQ(imported->conflict_counts.at("learning"), 1u);
  EXPECT_DOUBLE_EQ(store.Rows("にほん", "日本")->at("").weight, original.weight * 2);
  imported = import_with("overwrite", 743);
  ASSERT_TRUE(imported);
  EXPECT_DOUBLE_EQ(store.Rows("にほん", "日本")->at("").weight, original.weight);
  store.Observe("にほん", "日本", 0.8, kWhen);
  imported = import_with("keep_both", 744);
  ASSERT_TRUE(imported);
  EXPECT_EQ(imported->skipped_counts.at("learning"), 1u);
  EXPECT_DOUBLE_EQ(store.Rows("にほん", "日本")->at("").weight, original.weight + 0.8);
  std::filesystem::remove_all(dir);
}

TEST_F(DispatcherTest, LearningDataImportRejectsBadArchivesWithoutCrashing) {
  const auto dir = FreshBackupDirectory("azookey_dispatcher_bad_backups");
  const auto import_from = [&](const std::filesystem::path& path, uint64_t id) {
    ipc::ImportLearningDataRequest request;
    request.source_path = azookey::core::PathToUtf8(path);
    request.stores = {"learning"};
    const auto response = SendLearningData(dispatcher, id, ipc::MessageType::ImportLearningData,
                                           ipc::BuildImportLearningDataRequest(request));
    return ipc::ParseImportLearningDataResponse(response->payload_json);
  };
  {
    std::ofstream out(dir / "garbage.zip", std::ios::binary);
    out << "PK not really a zip";
  }
  auto result = import_from(dir / "garbage.zip", 750);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->status, "error");
  EXPECT_EQ(result->error, "not_archive");
  result = import_from(dir / "missing.zip", 751);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->error, "source_missing");
  result = import_from(dir / ".." / "escape.zip", 752);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->error, "invalid_path");
  EXPECT_EQ(store.size(), 0u);
  std::filesystem::remove_all(dir);
}

TEST_F(DispatcherTest, ForgettingReachesPairsOnlyInTheLegacyFile) {
  RemoveLearningFiles(learning_path);
  // Learned before v2: the pair is only in the M7 file.
  ASSERT_TRUE(azookey::learning::WriteProtectedText(learning_path, "にほん\t日本\t1 100\n",
                                                    azookey::learning::test::Crypto()));
  ipc::ForgetLearningEntryRequest forget;
  forget.store = "learning";
  forget.reading = "にほん";
  forget.surface = "日本";
  auto reply = SendLearningData(dispatcher, 760, ipc::MessageType::ForgetLearningEntry,
                                ipc::BuildForgetLearningEntryRequest(forget));
  auto forgot = ipc::ParseForgetLearningEntryResponse(reply->payload_json);
  ASSERT_TRUE(forgot && forgot->ok);
  EXPECT_TRUE(forgot->removed);
  std::string legacy;
  ASSERT_EQ(azookey::learning::ReadProtectedText(learning_path, azookey::learning::test::Crypto(),
                                                 legacy),
            azookey::learning::ProtectedFileSource::Encrypted);
  EXPECT_EQ(legacy.find("日本"), std::string::npos);

  // An unreadable M7 file fails the forget instead of claiming success.
  {
    std::ofstream out(azookey::learning::EncryptedPathFor(learning_path),
                      std::ios::binary | std::ios::trunc);
    out << "undecipherable";
  }
  store.Observe("にほん", "日本", 0.8, 1'700'000'000);
  reply = SendLearningData(dispatcher, 761, ipc::MessageType::ForgetLearningEntry,
                           ipc::BuildForgetLearningEntryRequest(forget));
  forgot = ipc::ParseForgetLearningEntryResponse(reply->payload_json);
  ASSERT_TRUE(forgot);
  EXPECT_FALSE(forgot->ok);
  EXPECT_EQ(forgot->error, "save_failed");
  RemoveLearningFiles(learning_path);
}
