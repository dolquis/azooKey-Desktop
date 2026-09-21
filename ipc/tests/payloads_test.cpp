#include "azookey/ipc/Payloads.h"

#include <gtest/gtest.h>

#include <limits>
#include <locale>
#include <string>

#include "azookey/ipc/Json.h"
#include "azookey/ipc/Messages.h"

TEST(RewriterPayload, OptionalFieldsRoundTripAndOldPeersDefaultOff) {
  using namespace azookey::ipc;
  HandshakeResponse handshake;
  handshake.host_version = "test";
  handshake.symbol_rewriter = handshake.emoji_rewriter = true;
  handshake.emoji_max_candidates = 50;
  const auto parsed = ParseHandshakeResponse(BuildHandshakeResponse(handshake));
  ASSERT_TRUE(parsed);
  EXPECT_TRUE(parsed->symbol_rewriter);
  EXPECT_TRUE(parsed->emoji_rewriter);
  EXPECT_EQ(parsed->emoji_max_candidates, 50u);
  const auto old = ParseHandshakeResponse(R"({"host_version":"old"})");
  ASSERT_TRUE(old);
  EXPECT_FALSE(old->symbol_rewriter);
  EXPECT_FALSE(old->emoji_rewriter);
  QueryCandidatesRequest query;
  query.emoji_trigger = "smile";
  EXPECT_EQ(ParseQueryCandidatesRequest(BuildQueryCandidatesRequest(query))->emoji_trigger,
            "smile");
  QueryCandidatesResponse response;
  response.candidates.push_back({"😄", "", 0, "emoji", "笑顔"});
  const auto result = ParseQueryCandidatesResponse(BuildQueryCandidatesResponse(response));
  ASSERT_TRUE(result);
  ASSERT_EQ(result->candidates.size(), 1u);
  EXPECT_EQ(result->candidates[0].description, "笑顔");
}

namespace {

class CommaDecimalPunct : public std::numpunct<char> {
 protected:
  char do_decimal_point() const override { return ','; }
};

class ScopedGlobalLocale {
 public:
  explicit ScopedGlobalLocale(const std::locale& locale) : previous_(std::locale()) {
    std::locale::global(locale);
  }

  ~ScopedGlobalLocale() { std::locale::global(previous_); }

 private:
  std::locale previous_;
};

}  // namespace

TEST(PayloadsTest, JsonEscapeAndRoundTrip) {
  // U+0001 is a control character with no named escape, so it must be emitted
  // as a \uXXXX sequence.
  const std::string src = std::string("「\"日本\\n語\"」\t") + '\x01';
  const auto escaped = azookey::ipc::json::EscapeString(src);
  EXPECT_NE(escaped.find("\\\""), std::string::npos);
  EXPECT_NE(escaped.find("\\n"), std::string::npos);
  EXPECT_NE(escaped.find("\\u0001"), std::string::npos);

  const std::string wrapped = std::string("\"") + escaped + "\"";
  auto v = azookey::ipc::json::Parse(wrapped);
  ASSERT_TRUE(v.has_value());
  ASSERT_TRUE(v->IsString());
  EXPECT_EQ(v->AsString(), src);
}

TEST(PayloadsTest, HostCapabilitiesRemainOptional) {
  azookey::ipc::HandshakeResponse response;
  response.host_version = "test";
  response.capabilities = {"oob_cancel"};
  const auto parsed =
      azookey::ipc::ParseHandshakeResponse(azookey::ipc::BuildHandshakeResponse(response));
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->capabilities, response.capabilities);
  const auto legacy = azookey::ipc::ParseHandshakeResponse(R"({"host_version":"old"})");
  ASSERT_TRUE(legacy);
  EXPECT_TRUE(legacy->capabilities.empty());
}

TEST(PayloadsTest, BatchAiPermissionsFailClosedAndRoundTrip) {
  const auto legacy = azookey::ipc::ParseQueryBatchConversionRequest(R"({"reading":"かな"})");
  ASSERT_TRUE(legacy);
  EXPECT_FALSE(legacy->ai_allowed);
  EXPECT_FALSE(legacy->external_ai_allowed);
  auto request = *legacy;
  request.ai_allowed = request.external_ai_allowed = true;
  const auto parsed = azookey::ipc::ParseQueryBatchConversionRequest(
      azookey::ipc::BuildQueryBatchConversionRequest(request));
  ASSERT_TRUE(parsed);
  EXPECT_TRUE(parsed->ai_allowed);
  EXPECT_TRUE(parsed->external_ai_allowed);
  const auto invalid = azookey::ipc::ParseQueryBatchConversionRequest(
      R"({"reading":"かな","ai_allowed":"true","external_ai_allowed":true})");
  ASSERT_TRUE(invalid);
  EXPECT_FALSE(invalid->ai_allowed);
  EXPECT_FALSE(invalid->external_ai_allowed);
}

TEST(PayloadsTest, CommitSegmentsRoundTripAndRejectsMalformedSegment) {
  using namespace azookey::ipc;
  CommitSegmentsObservationRequest request;
  CandidateField chosen;
  chosen.surface = "日本";
  chosen.reading = "にほん";
  request.segments.push_back({"にほん", chosen, {chosen}, false});
  chosen.surface = "。";
  chosen.reading.clear();
  request.segments.push_back({"", chosen, {}, true});
  request.left_context = "前";
  request.timestamp_ms = 123;
  request.observation_id = "batch:1";
  const auto parsed =
      ParseCommitSegmentsObservationRequest(BuildCommitSegmentsObservationRequest(request));
  ASSERT_TRUE(parsed);
  ASSERT_EQ(parsed->segments.size(), 2u);
  EXPECT_EQ(parsed->segments[0].chosen.surface, "日本");
  EXPECT_TRUE(parsed->segments[1].is_auto_punctuation);
  EXPECT_EQ(parsed->left_context, "前");
  EXPECT_EQ(parsed->observation_id, "batch:1");
  EXPECT_EQ(parsed->timestamp_ms, 123u);
  EXPECT_FALSE(ParseCommitSegmentsObservationRequest(R"({"segments":[{}]})"));
  EXPECT_FALSE(ParseCommitSegmentsObservationRequest(R"({"segments":[]})"));
  EXPECT_EQ(TypeFromString(TypeToString(MessageType::CommitSegmentsObservation)),
            MessageType::CommitSegmentsObservation);
}

TEST(PayloadsTest, Handshake) {
  azookey::ipc::HandshakeRequest req;
  req.tip_version = "0.1.0";
  req.protocol_version = 1;
  req.capabilities = {"live_conversion", "cancel"};
  req.client_id = "tip-client-123";
  req.handshake_token = "token-123";
  auto json = azookey::ipc::BuildHandshakeRequest(req);
  auto parsed = azookey::ipc::ParseHandshakeRequest(json);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->tip_version, "0.1.0");
  ASSERT_EQ(parsed->capabilities.size(), 2u);
  EXPECT_EQ(parsed->capabilities[0], "live_conversion");
  EXPECT_EQ(parsed->client_id, "tip-client-123");
  EXPECT_EQ(parsed->handshake_token, "token-123");

  auto legacy = azookey::ipc::ParseHandshakeRequest(
      R"({"tip_version":"0.1.0","protocol_version":1,"capabilities":[]})");
  ASSERT_TRUE(legacy.has_value());
  EXPECT_TRUE(legacy->client_id.empty());

  azookey::ipc::HandshakeResponse res;
  res.host_version = "0.1.0";
  res.accepted = true;
  res.model_loaded = false;
  res.host_generation_id = "9c633fc2-6107-4c22-aa71-872135548eee";
  res.batch_romaji_conversion = true;
  res.batch_romaji_preview_style = "romaji";
  res.batch_conversion_mode = "neural";
  res.batch_auto_punctuation = true;
  res.number_rewriter = true;
  res.katakana_rewriter = true;
  res.max_candidates = 32;
  auto json2 = azookey::ipc::BuildHandshakeResponse(res);
  auto parsed2 = azookey::ipc::ParseHandshakeResponse(json2);
  ASSERT_TRUE(parsed2.has_value());
  EXPECT_TRUE(parsed2->accepted);
  EXPECT_FALSE(parsed2->model_loaded);
  EXPECT_EQ(parsed2->host_generation_id, "9c633fc2-6107-4c22-aa71-872135548eee");
  EXPECT_TRUE(parsed2->batch_romaji_conversion);
  EXPECT_EQ(parsed2->batch_romaji_preview_style, "romaji");
  EXPECT_EQ(parsed2->batch_conversion_mode, "neural");
  EXPECT_TRUE(parsed2->batch_auto_punctuation);
  EXPECT_TRUE(parsed2->number_rewriter);
  EXPECT_TRUE(parsed2->katakana_rewriter);
  EXPECT_EQ(parsed2->max_candidates, 32u);

  auto legacy_response = azookey::ipc::ParseHandshakeResponse(
      R"({"host_version":"0.1.0","protocol_version":1,"accepted":true})");
  ASSERT_TRUE(legacy_response.has_value());
  EXPECT_TRUE(legacy_response->host_generation_id.empty());
  EXPECT_FALSE(legacy_response->number_rewriter);
  EXPECT_FALSE(legacy_response->katakana_rewriter);
  EXPECT_EQ(legacy_response->max_candidates, 9u);

  auto out_of_range_response =
      azookey::ipc::ParseHandshakeResponse(R"({"host_version":"0.1.0","max_candidates":33})");
  ASSERT_TRUE(out_of_range_response.has_value());
  EXPECT_EQ(out_of_range_response->max_candidates, 9u);
}

TEST(PayloadsTest, Ping) {
  azookey::ipc::PingPayload p;
  p.nonce = 12345;
  p.t_ms = 1700000000123ULL;
  auto json = azookey::ipc::BuildPing(p);
  auto parsed = azookey::ipc::ParsePing(json);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->nonce, 12345u);
  EXPECT_EQ(parsed->t_ms, 1700000000123ULL);
}

TEST(PayloadsTest, PingPreservesLargeIntegerFields) {
  azookey::ipc::PingPayload p;
  p.nonce = 9007199254740993ULL;
  p.t_ms = std::numeric_limits<uint64_t>::max();

  auto json = azookey::ipc::BuildPing(p);
  EXPECT_NE(json.find("\"nonce\":9007199254740993"), std::string::npos);
  EXPECT_NE(json.find("\"t_ms\":18446744073709551615"), std::string::npos);

  auto parsed = azookey::ipc::ParsePing(json);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->nonce, 9007199254740993ULL);
  EXPECT_EQ(parsed->t_ms, std::numeric_limits<uint64_t>::max());
}

TEST(PayloadsTest, Health) {
  azookey::ipc::HealthPayload p;
  p.status = "degraded";
  p.backend = "cpu";
  p.model_loaded = false;
  p.vram_mb = 0;
  p.last_error = "no cuda runtime";
  auto json = azookey::ipc::BuildHealth(p);
  auto parsed = azookey::ipc::ParseHealth(json);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->status, "degraded");
  EXPECT_EQ(parsed->backend, "cpu");
  ASSERT_TRUE(parsed->last_error.has_value());
  EXPECT_EQ(*parsed->last_error, "no cuda runtime");
}

TEST(PayloadsTest, LoadModel) {
  azookey::ipc::LoadModelRequest req;
  req.path = "C:\\models\\zenz.gguf";
  req.backend = "cuda";
  req.n_gpu_layers = 32;
  auto json = azookey::ipc::BuildLoadModelRequest(req);
  auto parsed = azookey::ipc::ParseLoadModelRequest(json);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->path, "C:\\models\\zenz.gguf");
  ASSERT_TRUE(parsed->n_gpu_layers.has_value());
  EXPECT_EQ(*parsed->n_gpu_layers, 32);

  azookey::ipc::LoadModelResponse res;
  res.ok = false;
  res.error = "file not found";
  auto json2 = azookey::ipc::BuildLoadModelResponse(res);
  auto parsed2 = azookey::ipc::ParseLoadModelResponse(json2);
  ASSERT_TRUE(parsed2.has_value());
  EXPECT_FALSE(parsed2->ok);
  EXPECT_TRUE(parsed2->error.has_value());
}

TEST(PayloadsTest, QueryCandidates) {
  azookey::ipc::QueryCandidatesRequest req;
  req.reading = "にほんご";
  req.left_context = "私は";
  req.max_candidates = 5;
  req.live = true;
  auto json = azookey::ipc::BuildQueryCandidatesRequest(req);
  auto parsed = azookey::ipc::ParseQueryCandidatesRequest(json);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->reading, "にほんご");
  EXPECT_EQ(parsed->max_candidates, 5);
  EXPECT_TRUE(parsed->live);

  azookey::ipc::QueryCandidatesResponse res;
  res.candidates = {
      {"日本語", "にほんご", 1.0, "static-dict"},
      {"日本五", "にほんご", 0.3, "fallback"},
  };
  res.partial = false;
  auto json2 = azookey::ipc::BuildQueryCandidatesResponse(res);
  auto parsed2 = azookey::ipc::ParseQueryCandidatesResponse(json2);
  ASSERT_TRUE(parsed2.has_value());
  ASSERT_EQ(parsed2->candidates.size(), 2u);
  EXPECT_EQ(parsed2->candidates[0].surface, "日本語");
  EXPECT_EQ(parsed2->candidates[0].score, 1.0);
}

TEST(PayloadsTest, QueryCandidatesResponseDropsMalformedEntries) {
  // A mix of valid and malformed candidate entries must parse successfully,
  // preserving the valid entries in order while silently dropping the malformed
  // ones (non-object, or missing the required surface/reading fields). A single
  // bad entry from the host must not blank out the whole candidate list.
  const std::string json =
      R"({"candidates":[)"
      R"({"surface":"日本語","reading":"にほんご","score":1.0,"source":"static-dict"},)"
      R"({"surface":"欠落","score":0.5},)"   // missing reading -> dropped
      R"("not-an-object",)"                  // non-object -> dropped
      R"({"reading":"のみ","source":"x"},)"  // missing surface -> dropped
      R"({"surface":"日本","reading":"にほん","score":0.7,"source":"fallback"})"
      R"(],"partial":true})";

  auto parsed = azookey::ipc::ParseQueryCandidatesResponse(json);
  ASSERT_TRUE(parsed.has_value());
  ASSERT_EQ(parsed->candidates.size(), 2u);
  EXPECT_EQ(parsed->candidates[0].surface, "日本語");
  EXPECT_EQ(parsed->candidates[1].surface, "日本");
  EXPECT_TRUE(parsed->partial);
}

TEST(PayloadsTest, CandidateScoresIgnoreGlobalCppLocale) {
  ScopedGlobalLocale scoped(std::locale(std::locale::classic(), new CommaDecimalPunct));

  azookey::ipc::QueryCandidatesResponse res;
  res.candidates = {{"日本語", "にほんご", 0.75, "static-dict"}};
  res.partial = false;

  auto json = azookey::ipc::BuildQueryCandidatesResponse(res);
  EXPECT_NE(json.find("\"score\":0.75"), std::string::npos);
  EXPECT_EQ(json.find("0,75"), std::string::npos);

  auto parsed = azookey::ipc::ParseQueryCandidatesResponse(json);
  ASSERT_TRUE(parsed.has_value());
  ASSERT_EQ(parsed->candidates.size(), 1u);
  EXPECT_DOUBLE_EQ(parsed->candidates[0].score, 0.75);
}

TEST(PayloadsTest, Cancel) {
  azookey::ipc::CancelPayload p;
  p.target_request_id = 7777;
  auto json = azookey::ipc::BuildCancel(p);
  auto parsed = azookey::ipc::ParseCancel(json);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->target_request_id, 7777u);
}

TEST(PayloadsTest, QueryBatchConversion) {
  azookey::ipc::QueryBatchConversionRequest req;
  req.reading = "にほんご";
  req.raw_romaji = "nihongo";
  req.mode = "neural";
  req.max_candidates = 5;

  auto json = azookey::ipc::BuildQueryBatchConversionRequest(req);
  auto parsed = azookey::ipc::ParseQueryBatchConversionRequest(json);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->reading, "にほんご");
  EXPECT_EQ(parsed->raw_romaji, "nihongo");
  EXPECT_EQ(parsed->mode, "neural");
  EXPECT_EQ(parsed->max_candidates, 5u);

  azookey::ipc::QueryBatchConversionResponse res;
  res.full_surface = "日本語";
  azookey::ipc::BatchConversionSegment segment;
  segment.reading = "にほんご";
  segment.candidates.push_back({"日本語", "にほんご", 1.0, "model"});
  res.segments.push_back(segment);

  auto res_json = azookey::ipc::BuildQueryBatchConversionResponse(res);
  auto res_parsed = azookey::ipc::ParseQueryBatchConversionResponse(res_json);
  ASSERT_TRUE(res_parsed.has_value());
  EXPECT_EQ(res_parsed->full_surface, "日本語");
  ASSERT_EQ(res_parsed->segments.size(), 1u);
  ASSERT_EQ(res_parsed->segments[0].candidates.size(), 1u);
  EXPECT_EQ(res_parsed->segments[0].candidates[0].surface, "日本語");
}

TEST(PayloadsTest, CancelPreservesLargeTargetRequestId) {
  azookey::ipc::CancelPayload p;
  p.target_request_id = std::numeric_limits<uint64_t>::max();

  auto json = azookey::ipc::BuildCancel(p);
  EXPECT_NE(json.find("\"target_request_id\":18446744073709551615"), std::string::npos);

  auto parsed = azookey::ipc::ParseCancel(json);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->target_request_id, std::numeric_limits<uint64_t>::max());
}

TEST(PayloadsTest, CommitObservation) {
  azookey::ipc::CommitObservationRequest req;
  req.reading = "にほんご";
  req.chosen = {"日本語", "にほんご", 1.0, "user"};
  req.shown = {
      {"日本語", "にほんご", 1.0, "static-dict"},
      {"二本後", "にほんご", 0.1, "fallback"},
  };
  req.left_context = "";
  req.timestamp_ms = 1700000000123ULL;
  auto json = azookey::ipc::BuildCommitObservationRequest(req);
  auto parsed = azookey::ipc::ParseCommitObservationRequest(json);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->chosen.surface, "日本語");
  EXPECT_EQ(parsed->shown.size(), 2u);
  EXPECT_EQ(parsed->timestamp_ms, 1700000000123ULL);
}

// DEV-554: observation_id round-trips, and a payload from a TIP that predates
// the field still parses (dedupe simply does not apply to it).
TEST(PayloadsTest, CommitObservationCarriesObservationId) {
  azookey::ipc::CommitObservationRequest req;
  req.reading = "にほんご";
  req.chosen = {"日本語", "にほんご", 1.0, "user"};
  req.left_context = "";
  req.timestamp_ms = 1700000000123ULL;
  req.observation_id = "3F2504E0-4F89-11D3-9A0C-0305E82C3301:7";

  const auto json = azookey::ipc::BuildCommitObservationRequest(req);
  const auto parsed = azookey::ipc::ParseCommitObservationRequest(json);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->observation_id, "3F2504E0-4F89-11D3-9A0C-0305E82C3301:7");
}

TEST(PayloadsTest, CommitObservationWithoutObservationIdParsesAsEmpty) {
  const std::string legacy_json =
      R"({"reading":"にほんご","chosen":{"surface":"日本語","reading":"にほんご",)"
      R"("score":1.0,"source":"user"},"shown":[],"left_context":"","timestamp_ms":1700000000123})";

  const auto parsed = azookey::ipc::ParseCommitObservationRequest(legacy_json);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->reading, "にほんご");
  EXPECT_TRUE(parsed->observation_id.empty());
}

TEST(PayloadsTest, CommitObservationPreservesLargeTimestamp) {
  azookey::ipc::CommitObservationRequest req;
  req.reading = "にほんご";
  req.chosen = {"日本語", "にほんご", 1.0, "user"};
  req.left_context = "";
  req.timestamp_ms = 9007199254740993ULL;

  auto json = azookey::ipc::BuildCommitObservationRequest(req);
  EXPECT_NE(json.find("\"timestamp_ms\":9007199254740993"), std::string::npos);

  auto parsed = azookey::ipc::ParseCommitObservationRequest(json);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->timestamp_ms, 9007199254740993ULL);
}

TEST(PayloadsTest, UserWord) {
  azookey::ipc::AddUserWordRequest add;
  add.word = "azooKey";
  add.ruby = "あずきい";
  add.cid = 1285;
  add.value = -5.0;
  auto json = azookey::ipc::BuildAddUserWordRequest(add);
  auto parsed = azookey::ipc::ParseAddUserWordRequest(json);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->word, "azooKey");
  ASSERT_TRUE(parsed->cid.has_value());
  EXPECT_EQ(*parsed->cid, 1285);
  EXPECT_TRUE(parsed->value.has_value());

  azookey::ipc::RemoveUserWordRequest rm;
  rm.word = "azooKey";
  rm.ruby = "あずきい";
  auto json2 = azookey::ipc::BuildRemoveUserWordRequest(rm);
  auto parsed2 = azookey::ipc::ParseRemoveUserWordRequest(json2);
  ASSERT_TRUE(parsed2.has_value());
  EXPECT_EQ(parsed2->word, "azooKey");
}

TEST(PayloadsTest, UpdateConfigResponse) {
  azookey::ipc::UpdateConfigResponse ok;
  ok.ok = true;
  auto ok_json = azookey::ipc::BuildUpdateConfigResponse(ok);
  auto ok_parsed = azookey::ipc::ParseUpdateConfigResponse(ok_json);
  ASSERT_TRUE(ok_parsed.has_value());
  EXPECT_TRUE(ok_parsed->ok);
  EXPECT_FALSE(ok_parsed->error.has_value());

  azookey::ipc::UpdateConfigResponse error;
  error.ok = false;
  error.error = "invalid settings.json";
  auto error_json = azookey::ipc::BuildUpdateConfigResponse(error);
  auto error_parsed = azookey::ipc::ParseUpdateConfigResponse(error_json);
  ASSERT_TRUE(error_parsed.has_value());
  EXPECT_FALSE(error_parsed->ok);
  ASSERT_TRUE(error_parsed->error.has_value());
  EXPECT_EQ(*error_parsed->error, "invalid settings.json");
}

TEST(PayloadsTest, QueryDiagnosticsRoundTrips) {
  azookey::ipc::QueryDiagnosticsPayload payload;
  payload.model_loaded = true;
  payload.loaded_model_path = R"(C:\Models\model.gguf)";
  payload.engine = "llama_cpp";
  payload.backend = "cuda";
  payload.rss_mb = 256;
  payload.learning_entries = 10;
  payload.user_dict_entries = 4;
  payload.fallback_state = "healthy";

  const auto parsed =
      azookey::ipc::ParseQueryDiagnostics(azookey::ipc::BuildQueryDiagnostics(payload));

  ASSERT_TRUE(parsed.has_value());
  EXPECT_TRUE(parsed->model_loaded);
  EXPECT_EQ(parsed->loaded_model_path, payload.loaded_model_path);
  EXPECT_EQ(parsed->engine, "llama_cpp");
  EXPECT_EQ(parsed->backend, "cuda");
  EXPECT_EQ(parsed->rss_mb, 256);
  EXPECT_EQ(parsed->learning_entries, 10);
  EXPECT_EQ(parsed->user_dict_entries, 4);
  EXPECT_EQ(parsed->fallback_state, "healthy");
}

TEST(PayloadsTest, QueryDiagnosticsDefaultsMissingCounters) {
  const auto parsed = azookey::ipc::ParseQueryDiagnostics(
      R"({"engine":"mock","backend":"cpu","fallback_state":"degraded_simple"})");

  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->rss_mb, 0);
  EXPECT_EQ(parsed->learning_entries, 0);
  EXPECT_EQ(parsed->user_dict_entries, 0);
}

TEST(PayloadsTest, MalformedRejection) {
  EXPECT_FALSE(azookey::ipc::ParseHandshakeRequest("not json").has_value());
  EXPECT_FALSE(azookey::ipc::ParseQueryCandidatesRequest("{}").has_value());
  EXPECT_FALSE(azookey::ipc::ParseCancel("{}").has_value());
  EXPECT_FALSE(azookey::ipc::ParseQueryDiagnostics(R"({"engine":"mock"})").has_value());
}

TEST(PayloadsTest, QueryErrorIsOptionalAndRoundTrips) {
  const auto old =
      azookey::ipc::ParseQueryCandidatesResponse(R"({"candidates":[],"partial":false})");
  ASSERT_TRUE(old);
  EXPECT_TRUE(old->ok);
  EXPECT_FALSE(old->error);
  azookey::ipc::QueryCandidatesResponse error;
  error.ok = false;
  error.error = "invalid query";
  const auto parsed =
      azookey::ipc::ParseQueryCandidatesResponse(azookey::ipc::BuildQueryCandidatesResponse(error));
  ASSERT_TRUE(parsed);
  EXPECT_FALSE(parsed->ok);
  EXPECT_EQ(parsed->error, error.error);
}

TEST(PayloadsTest, ObserveTypoRequestRoundTrips) {
  azookey::ipc::ObserveTypoRequest request;
  request.wrong_reading = "こんちには";
  request.correct_reading = "こんにちは";
  request.timestamp_ms = 1'780'000'000'000ULL;

  const auto parsed =
      azookey::ipc::ParseObserveTypoRequest(azookey::ipc::BuildObserveTypoRequest(request));
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->wrong_reading, request.wrong_reading);
  EXPECT_EQ(parsed->correct_reading, request.correct_reading);
  EXPECT_EQ(parsed->timestamp_ms, request.timestamp_ms);

  azookey::ipc::ObserveTypoResponse response;
  response.ok = true;
  const auto parsed_response =
      azookey::ipc::ParseObserveTypoResponse(azookey::ipc::BuildObserveTypoResponse(response));
  ASSERT_TRUE(parsed_response);
  EXPECT_TRUE(parsed_response->ok);
}

TEST(PayloadsTest, ObserveTypoRejectsHalfSpecifiedPairs) {
  // Correcting to an empty reading is not a correction; neither is an update
  // that says only what the result should be.
  EXPECT_FALSE(azookey::ipc::ParseObserveTypoRequest(R"({"wrong_reading":"こんちには"})"));
  EXPECT_FALSE(azookey::ipc::ParseObserveTypoRequest(R"({"correct_reading":"こんにちは"})"));
  EXPECT_FALSE(azookey::ipc::ParseObserveTypoRequest("{}"));
  EXPECT_FALSE(azookey::ipc::ParseObserveTypoRequest("not json"));
  // timestamp_ms is optional and defaults to zero.
  const auto parsed = azookey::ipc::ParseObserveTypoRequest(
      R"({"wrong_reading":"こんちには","correct_reading":"こんにちは"})");
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->timestamp_ms, 0u);
}

TEST(PayloadsTest, QueryCandidatesResponseCarriesCorrectedReading) {
  azookey::ipc::QueryCandidatesResponse response;
  response.corrected_reading = "こんにちは";
  const auto parsed = azookey::ipc::ParseQueryCandidatesResponse(
      azookey::ipc::BuildQueryCandidatesResponse(response));
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->corrected_reading, "こんにちは");

  // A host that predates M35 omits the field; that decodes as "no correction"
  // and the field is not written back when empty.
  const auto legacy =
      azookey::ipc::ParseQueryCandidatesResponse(R"({"candidates":[],"partial":false})");
  ASSERT_TRUE(legacy);
  EXPECT_TRUE(legacy->corrected_reading.empty());
  EXPECT_EQ(azookey::ipc::BuildQueryCandidatesResponse(azookey::ipc::QueryCandidatesResponse{})
                .find("corrected_reading"),
            std::string::npos);
}

TEST(PayloadsTest, ListNewWordCandidatesRoundTrips) {
  azookey::ipc::ListNewWordCandidatesRequest request;
  request.state_filter = "confirmed";
  request.max_items = 12;
  const auto parsed_request = azookey::ipc::ParseListNewWordCandidatesRequest(
      azookey::ipc::BuildListNewWordCandidatesRequest(request));
  ASSERT_TRUE(parsed_request);
  EXPECT_EQ(parsed_request->state_filter, "confirmed");
  EXPECT_EQ(parsed_request->max_items, 12u);

  // Defaults apply when the caller sends neither field.
  const auto defaulted = azookey::ipc::ParseListNewWordCandidatesRequest("{}");
  ASSERT_TRUE(defaulted);
  EXPECT_EQ(defaulted->state_filter, "pending");
  EXPECT_EQ(defaulted->max_items, 50u);

  azookey::ipc::ListNewWordCandidatesResponse response;
  azookey::ipc::NewWordField field;
  field.surface = "あずきー";
  field.reading = "あずきー";
  field.source = "mining";
  field.state = "pending";
  field.count = 4;
  field.last_seen_epoch = 1'700'000'000ULL;
  response.items.push_back(field);
  const auto parsed_response = azookey::ipc::ParseListNewWordCandidatesResponse(
      azookey::ipc::BuildListNewWordCandidatesResponse(response));
  ASSERT_TRUE(parsed_response);
  ASSERT_EQ(parsed_response->items.size(), 1u);
  EXPECT_EQ(parsed_response->items[0].surface, field.surface);
  EXPECT_EQ(parsed_response->items[0].source, field.source);
  EXPECT_EQ(parsed_response->items[0].state, field.state);
  EXPECT_EQ(parsed_response->items[0].count, field.count);
  EXPECT_EQ(parsed_response->items[0].last_seen_epoch, field.last_seen_epoch);
}

TEST(PayloadsTest, ListNewWordCandidatesRejectsUnknownFilterAndOutOfRangeLimit) {
  // Widening an unknown filter to "everything" would show the user words they
  // already rejected.
  EXPECT_FALSE(azookey::ipc::ParseListNewWordCandidatesRequest(R"({"state_filter":"all"})"));
  EXPECT_FALSE(azookey::ipc::ParseListNewWordCandidatesRequest(R"({"state_filter":""})"));
  EXPECT_FALSE(azookey::ipc::ParseListNewWordCandidatesRequest(R"({"max_items":0})"));
  EXPECT_FALSE(azookey::ipc::ParseListNewWordCandidatesRequest(R"({"max_items":100000})"));
  EXPECT_FALSE(azookey::ipc::ParseListNewWordCandidatesRequest("not json"));
  // Malformed items are skipped rather than blanking the whole page.
  const auto lenient = azookey::ipc::ParseListNewWordCandidatesResponse(
      R"({"items":[{"surface":"あ"},{"surface":"あずきー","reading":"あずきー"}]})");
  ASSERT_TRUE(lenient);
  ASSERT_EQ(lenient->items.size(), 1u);
  EXPECT_EQ(lenient->items[0].surface, "あずきー");
}

TEST(PayloadsTest, ResolveNewWordRoundTripsAndRejectsUnknownAction) {
  for (const std::string action : {"confirm", "reject"}) {
    azookey::ipc::ResolveNewWordRequest request;
    request.surface = "あずきー";
    request.reading = "あずきー";
    request.action = action;
    const auto parsed =
        azookey::ipc::ParseResolveNewWordRequest(azookey::ipc::BuildResolveNewWordRequest(request));
    ASSERT_TRUE(parsed) << action;
    EXPECT_EQ(parsed->action, action);
    EXPECT_EQ(parsed->surface, request.surface);
    EXPECT_EQ(parsed->reading, request.reading);
  }

  // An unknown action must not fall through to one of the two real outcomes.
  EXPECT_FALSE(azookey::ipc::ParseResolveNewWordRequest(
      R"({"surface":"あずきー","reading":"あずきー","action":"delete"})"));
  EXPECT_FALSE(
      azookey::ipc::ParseResolveNewWordRequest(R"({"surface":"あずきー","reading":"あずきー"})"));
  EXPECT_FALSE(azookey::ipc::ParseResolveNewWordRequest(
      R"({"surface":"","reading":"あずきー","action":"confirm"})"));

  azookey::ipc::ResolveNewWordResponse response;
  response.ok = true;
  const auto parsed_response = azookey::ipc::ParseResolveNewWordResponse(
      azookey::ipc::BuildResolveNewWordResponse(response));
  ASSERT_TRUE(parsed_response);
  EXPECT_TRUE(parsed_response->ok);
}

TEST(PayloadsTest, EventPrivacyDefaultsDenyAndExplicitFlagsRoundTrip) {
  {
    azookey::ipc::QueryCandidatesRequest request;
    request.reading = "kana";
    EXPECT_TRUE(request.secure);
    EXPECT_FALSE(request.learning_allowed);
    for (bool secure : {false, true}) {
      for (bool allowed : {false, true}) {
        request.secure = secure;
        request.learning_allowed = allowed;
        const auto parsed = azookey::ipc::ParseQueryCandidatesRequest(
            azookey::ipc::BuildQueryCandidatesRequest(request));
        ASSERT_TRUE(parsed);
        EXPECT_EQ(parsed->secure, secure);
        EXPECT_EQ(parsed->learning_allowed, allowed);
      }
    }
    {
      const auto parsed = azookey::ipc::ParseQueryCandidatesRequest(R"({"reading":"kana"})");
      ASSERT_TRUE(parsed);
      EXPECT_EQ(parsed->secure, true);
      EXPECT_EQ(parsed->learning_allowed, false);
    }
    {
      const auto parsed =
          azookey::ipc::ParseQueryCandidatesRequest(R"({"reading":"kana","secure":false})");
      ASSERT_TRUE(parsed);
      EXPECT_EQ(parsed->secure, false);
      EXPECT_EQ(parsed->learning_allowed, false);
    }
    {
      const auto parsed = azookey::ipc::ParseQueryCandidatesRequest(
          R"({"reading":"kana","learning_allowed":true})");
      ASSERT_TRUE(parsed);
      EXPECT_EQ(parsed->secure, true);
      EXPECT_EQ(parsed->learning_allowed, true);
    }
    {
      const auto parsed = azookey::ipc::ParseQueryCandidatesRequest(
          R"({"reading":"kana","secure":null,"learning_allowed":"true"})");
      ASSERT_TRUE(parsed);
      EXPECT_EQ(parsed->secure, true);
      EXPECT_EQ(parsed->learning_allowed, false);
    }
    {
      const auto parsed = azookey::ipc::ParseQueryCandidatesRequest(
          R"({"reading":"kana","secure":0,"learning_allowed":1})");
      ASSERT_TRUE(parsed);
      EXPECT_EQ(parsed->secure, true);
      EXPECT_EQ(parsed->learning_allowed, false);
    }
  }
  {
    azookey::ipc::CommitObservationRequest request;
    request.reading = "kana";
    request.chosen.surface = "word";
    EXPECT_TRUE(request.secure);
    EXPECT_FALSE(request.learning_allowed);
    for (bool secure : {false, true}) {
      for (bool allowed : {false, true}) {
        request.secure = secure;
        request.learning_allowed = allowed;
        const auto parsed = azookey::ipc::ParseCommitObservationRequest(
            azookey::ipc::BuildCommitObservationRequest(request));
        ASSERT_TRUE(parsed);
        EXPECT_EQ(parsed->secure, secure);
        EXPECT_EQ(parsed->learning_allowed, allowed);
      }
    }
    {
      const auto parsed = azookey::ipc::ParseCommitObservationRequest(
          R"({"reading":"kana","chosen":{"surface":"word","reading":"kana","score":1,"source":"static"}})");
      ASSERT_TRUE(parsed);
      EXPECT_EQ(parsed->secure, true);
      EXPECT_EQ(parsed->learning_allowed, false);
    }
    {
      const auto parsed = azookey::ipc::ParseCommitObservationRequest(
          R"({"reading":"kana","chosen":{"surface":"word","reading":"kana","score":1,"source":"static"},"secure":false})");
      ASSERT_TRUE(parsed);
      EXPECT_EQ(parsed->secure, false);
      EXPECT_EQ(parsed->learning_allowed, false);
    }
    {
      const auto parsed = azookey::ipc::ParseCommitObservationRequest(
          R"({"reading":"kana","chosen":{"surface":"word","reading":"kana","score":1,"source":"static"},"learning_allowed":true})");
      ASSERT_TRUE(parsed);
      EXPECT_EQ(parsed->secure, true);
      EXPECT_EQ(parsed->learning_allowed, true);
    }
    {
      const auto parsed = azookey::ipc::ParseCommitObservationRequest(
          R"({"reading":"kana","chosen":{"surface":"word","reading":"kana","score":1,"source":"static"},"secure":null,"learning_allowed":"true"})");
      ASSERT_TRUE(parsed);
      EXPECT_EQ(parsed->secure, true);
      EXPECT_EQ(parsed->learning_allowed, false);
    }
    {
      const auto parsed = azookey::ipc::ParseCommitObservationRequest(
          R"({"reading":"kana","chosen":{"surface":"word","reading":"kana","score":1,"source":"static"},"secure":0,"learning_allowed":1})");
      ASSERT_TRUE(parsed);
      EXPECT_EQ(parsed->secure, true);
      EXPECT_EQ(parsed->learning_allowed, false);
    }
  }
  {
    azookey::ipc::CommitSegmentsObservationRequest request;
    request.segments.push_back({"kana", {"word", "kana", 1.0, "static"}, {}, false});
    EXPECT_TRUE(request.secure);
    EXPECT_FALSE(request.learning_allowed);
    for (bool secure : {false, true}) {
      for (bool allowed : {false, true}) {
        request.secure = secure;
        request.learning_allowed = allowed;
        const auto parsed = azookey::ipc::ParseCommitSegmentsObservationRequest(
            azookey::ipc::BuildCommitSegmentsObservationRequest(request));
        ASSERT_TRUE(parsed);
        EXPECT_EQ(parsed->secure, secure);
        EXPECT_EQ(parsed->learning_allowed, allowed);
      }
    }
    {
      const auto parsed = azookey::ipc::ParseCommitSegmentsObservationRequest(
          R"({"segments":[{"reading":"kana","chosen":{"surface":"word","reading":"kana","score":1,"source":"static"}}]})");
      ASSERT_TRUE(parsed);
      EXPECT_EQ(parsed->secure, true);
      EXPECT_EQ(parsed->learning_allowed, false);
    }
    {
      const auto parsed = azookey::ipc::ParseCommitSegmentsObservationRequest(
          R"({"segments":[{"reading":"kana","chosen":{"surface":"word","reading":"kana","score":1,"source":"static"}}],"secure":false})");
      ASSERT_TRUE(parsed);
      EXPECT_EQ(parsed->secure, false);
      EXPECT_EQ(parsed->learning_allowed, false);
    }
    {
      const auto parsed = azookey::ipc::ParseCommitSegmentsObservationRequest(
          R"({"segments":[{"reading":"kana","chosen":{"surface":"word","reading":"kana","score":1,"source":"static"}}],"learning_allowed":true})");
      ASSERT_TRUE(parsed);
      EXPECT_EQ(parsed->secure, true);
      EXPECT_EQ(parsed->learning_allowed, true);
    }
    {
      const auto parsed = azookey::ipc::ParseCommitSegmentsObservationRequest(
          R"({"segments":[{"reading":"kana","chosen":{"surface":"word","reading":"kana","score":1,"source":"static"}}],"secure":null,"learning_allowed":"true"})");
      ASSERT_TRUE(parsed);
      EXPECT_EQ(parsed->secure, true);
      EXPECT_EQ(parsed->learning_allowed, false);
    }
    {
      const auto parsed = azookey::ipc::ParseCommitSegmentsObservationRequest(
          R"({"segments":[{"reading":"kana","chosen":{"surface":"word","reading":"kana","score":1,"source":"static"}}],"secure":0,"learning_allowed":1})");
      ASSERT_TRUE(parsed);
      EXPECT_EQ(parsed->secure, true);
      EXPECT_EQ(parsed->learning_allowed, false);
    }
  }
  {
    azookey::ipc::ObserveTypoRequest request;
    request.wrong_reading = "wrong";
    request.correct_reading = "right";
    EXPECT_TRUE(request.secure);
    EXPECT_FALSE(request.learning_allowed);
    for (bool secure : {false, true}) {
      for (bool allowed : {false, true}) {
        request.secure = secure;
        request.learning_allowed = allowed;
        const auto parsed =
            azookey::ipc::ParseObserveTypoRequest(azookey::ipc::BuildObserveTypoRequest(request));
        ASSERT_TRUE(parsed);
        EXPECT_EQ(parsed->secure, secure);
        EXPECT_EQ(parsed->learning_allowed, allowed);
      }
    }
    {
      const auto parsed = azookey::ipc::ParseObserveTypoRequest(
          R"({"wrong_reading":"wrong","correct_reading":"right"})");
      ASSERT_TRUE(parsed);
      EXPECT_EQ(parsed->secure, true);
      EXPECT_EQ(parsed->learning_allowed, false);
    }
    {
      const auto parsed = azookey::ipc::ParseObserveTypoRequest(
          R"({"wrong_reading":"wrong","correct_reading":"right","secure":false})");
      ASSERT_TRUE(parsed);
      EXPECT_EQ(parsed->secure, false);
      EXPECT_EQ(parsed->learning_allowed, false);
    }
    {
      const auto parsed = azookey::ipc::ParseObserveTypoRequest(
          R"({"wrong_reading":"wrong","correct_reading":"right","learning_allowed":true})");
      ASSERT_TRUE(parsed);
      EXPECT_EQ(parsed->secure, true);
      EXPECT_EQ(parsed->learning_allowed, true);
    }
    {
      const auto parsed = azookey::ipc::ParseObserveTypoRequest(
          R"({"wrong_reading":"wrong","correct_reading":"right","secure":null,"learning_allowed":"true"})");
      ASSERT_TRUE(parsed);
      EXPECT_EQ(parsed->secure, true);
      EXPECT_EQ(parsed->learning_allowed, false);
    }
    {
      const auto parsed = azookey::ipc::ParseObserveTypoRequest(
          R"({"wrong_reading":"wrong","correct_reading":"right","secure":0,"learning_allowed":1})");
      ASSERT_TRUE(parsed);
      EXPECT_EQ(parsed->secure, true);
      EXPECT_EQ(parsed->learning_allowed, false);
    }
  }
}
