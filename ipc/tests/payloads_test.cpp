#include "azookey/ipc/Payloads.h"

#include <gtest/gtest.h>

#include <limits>
#include <locale>
#include <string>

#include "azookey/ipc/Json.h"

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
