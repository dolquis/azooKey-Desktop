#include "azookey/ipc/Payloads.h"

#include <gtest/gtest.h>

#include <limits>
#include <locale>
#include <string>

#include "azookey/ipc/Json.h"
#include "azookey/ipc/Limits.h"
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

TEST(PayloadsTest, ReverseConvertRoundTripAndRejectsInvalidPayloads) {
  using namespace azookey::ipc;
  const ReverseConvertRequest request{"明日"};
  const auto decoded_request = ParseReverseConvertRequest(BuildReverseConvertRequest(request));
  ASSERT_TRUE(decoded_request);
  EXPECT_EQ(decoded_request->surface, request.surface);
  EXPECT_FALSE(ParseReverseConvertRequest(R"({})"));
  EXPECT_FALSE(ParseReverseConvertRequest(R"({"surface":3})"));
  EXPECT_FALSE(ParseReverseConvertRequest(R"({"surface":""})"));

  const ReverseConvertResponse response{"あした", 1.0};
  const auto decoded_response = ParseReverseConvertResponse(BuildReverseConvertResponse(response));
  ASSERT_TRUE(decoded_response);
  EXPECT_EQ(decoded_response->reading, response.reading);
  EXPECT_DOUBLE_EQ(decoded_response->confidence, 1.0);
  const auto unknown = ParseReverseConvertResponse(BuildReverseConvertResponse({}));
  ASSERT_TRUE(unknown);
  EXPECT_TRUE(unknown->reading.empty());
  EXPECT_DOUBLE_EQ(unknown->confidence, 0.0);
  EXPECT_FALSE(ParseReverseConvertResponse(R"({"reading":"あした"})"));
  EXPECT_FALSE(ParseReverseConvertResponse(R"({"reading":"あした","confidence":2})"));
  EXPECT_FALSE(ParseReverseConvertResponse(R"({"reading":"","confidence":1})"));
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
  req.auto_punctuation = true;
  req.punctuation_style = "fullwidth_latin";
  auto json = azookey::ipc::BuildQueryCandidatesRequest(req);
  auto parsed = azookey::ipc::ParseQueryCandidatesRequest(json);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->reading, "にほんご");
  EXPECT_EQ(parsed->max_candidates, 5);
  EXPECT_TRUE(parsed->live);
  EXPECT_TRUE(parsed->auto_punctuation);
  EXPECT_EQ(parsed->punctuation_style, "fullwidth_latin");

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

TEST(PayloadsTest, QueryLiveConversionRoundTrip) {
  using namespace azookey::ipc;
  const QueryLiveConversionRequest request{"きょうは", "昨日は"};
  const auto parsed_request =
      ParseQueryLiveConversionRequest(BuildQueryLiveConversionRequest(request));
  ASSERT_TRUE(parsed_request);
  EXPECT_EQ(parsed_request->kana, request.kana);
  EXPECT_EQ(parsed_request->context, request.context);

  const QueryLiveConversionResponse response{"今日は", 0.875};
  const auto parsed_response =
      ParseQueryLiveConversionResponse(BuildQueryLiveConversionResponse(response));
  ASSERT_TRUE(parsed_response);
  EXPECT_EQ(parsed_response->surface, response.surface);
  EXPECT_DOUBLE_EQ(parsed_response->confidence, response.confidence);
}

// DEV-1442: M59 punctuation travels on QueryLiveConversion; a peer that
// predates it neither sends nor expects the fields.
TEST(PayloadsTest, QueryLiveConversionCarriesPunctuationAndDefaultsOff) {
  using namespace azookey::ipc;
  QueryLiveConversionRequest request{"きょうはいい", ""};
  request.auto_punctuation = true;
  request.punctuation_style = "fullwidth_latin";
  const auto parsed_request =
      ParseQueryLiveConversionRequest(BuildQueryLiveConversionRequest(request));
  ASSERT_TRUE(parsed_request);
  EXPECT_TRUE(parsed_request->auto_punctuation);
  EXPECT_EQ(parsed_request->punctuation_style, "fullwidth_latin");

  const auto legacy = ParseQueryLiveConversionRequest(R"({"kana":"かな","context":""})");
  ASSERT_TRUE(legacy);
  EXPECT_FALSE(legacy->auto_punctuation);
  EXPECT_EQ(legacy->punctuation_style, "ja");

  QueryLiveConversionResponse response{"今日は、", 0.5};
  LiveSegment word;
  word.end_char = 3;
  word.score = 1.0;
  word.surface = "今日は";
  word.reading = "きょうは";
  LiveSegment comma;
  comma.start_char = 3;
  comma.end_char = 4;
  comma.score = 1.0;
  comma.auto_punctuation = true;
  comma.surface = "、";
  response.segments = {word, comma};
  const auto parsed_response =
      ParseQueryLiveConversionResponse(BuildQueryLiveConversionResponse(response));
  ASSERT_TRUE(parsed_response);
  ASSERT_EQ(parsed_response->segments.size(), 2u);
  EXPECT_EQ(parsed_response->segments[0].reading, "きょうは");
  EXPECT_FALSE(parsed_response->segments[0].auto_punctuation);
  EXPECT_TRUE(parsed_response->segments[1].auto_punctuation);
  EXPECT_EQ(parsed_response->segments[1].surface, "、");
  EXPECT_TRUE(parsed_response->segments[1].reading.empty());

  // Without punctuation the response keeps its pre-M59 shape on the wire.
  const auto plain = BuildQueryLiveConversionResponse({"今日は", 0.5});
  EXPECT_FALSE(json::Parse(plain)->Find("segments"));
}

TEST(PayloadsTest, QueryLiveConversionRejectsMalformedPayloads) {
  using namespace azookey::ipc;
  EXPECT_FALSE(ParseQueryLiveConversionRequest("{}").has_value());
  EXPECT_FALSE(ParseQueryLiveConversionRequest(R"({"kana":"かな"})").has_value());
  EXPECT_FALSE(ParseQueryLiveConversionRequest(R"({"kana":123,"context":""})").has_value());
  EXPECT_FALSE(ParseQueryLiveConversionRequest(R"({"kana":"かな","context":null})").has_value());
  EXPECT_FALSE(ParseQueryLiveConversionResponse(R"({"surface":"仮名"})").has_value());
  EXPECT_FALSE(ParseQueryLiveConversionResponse(R"({"confidence":0.5})").has_value());
  EXPECT_FALSE(ParseQueryLiveConversionResponse(R"({"surface":42,"confidence":0.5})").has_value());
  EXPECT_FALSE(
      ParseQueryLiveConversionResponse(R"({"surface":"仮名","confidence":"0.5"})").has_value());
  EXPECT_FALSE(
      ParseQueryLiveConversionResponse(R"({"surface":"仮名","confidence":-0.1})").has_value());
  EXPECT_FALSE(
      ParseQueryLiveConversionResponse(R"({"surface":"仮名","confidence":1.1})").has_value());
  EXPECT_TRUE(ParseQueryLiveConversionResponse(R"({"surface":"","confidence":0})").has_value());
  EXPECT_TRUE(ParseQueryLiveConversionResponse(R"({"surface":"仮名","confidence":1})").has_value());
}

TEST(PayloadsTest, QueryPredictionsRoundTripUsesSpecifiedWireFields) {
  using namespace azookey::ipc;
  const QueryPredictionsRequest request{"にほん", "私は", "word"};
  const auto request_json = BuildQueryPredictionsRequest(request);
  const auto wire = json::Parse(request_json);
  ASSERT_TRUE(wire);
  EXPECT_EQ(wire->GetString("kana"), request.kana);
  EXPECT_EQ(wire->GetString("leftSideContext"), request.left_side_context);
  EXPECT_EQ(wire->GetString("mode"), "word");
  EXPECT_FALSE(wire->GetString("request_id"));
  const auto parsed_request = ParseQueryPredictionsRequest(request_json);
  ASSERT_TRUE(parsed_request);
  EXPECT_EQ(parsed_request->kana, request.kana);
  EXPECT_EQ(parsed_request->left_side_context, request.left_side_context);
  EXPECT_EQ(parsed_request->mode, request.mode);

  QueryPredictionsResponse response;
  response.predictions = {{"日本", "にほん", 1.5, "system", "country"},
                          {"日本語", "にほんご", 0.5, "model", ""}};
  const auto parsed_response =
      ParseQueryPredictionsResponse(BuildQueryPredictionsResponse(response));
  ASSERT_TRUE(parsed_response);
  EXPECT_TRUE(parsed_response->ok);
  ASSERT_EQ(parsed_response->predictions.size(), 2u);
  EXPECT_EQ(parsed_response->predictions[0].surface, "日本");
  EXPECT_EQ(parsed_response->predictions[0].description, "country");
  EXPECT_DOUBLE_EQ(parsed_response->predictions[1].score, 0.5);
}

TEST(PayloadsTest, QueryPredictionsRejectsMalformedPayloads) {
  using namespace azookey::ipc;
  EXPECT_FALSE(ParseQueryPredictionsRequest("{}").has_value());
  EXPECT_FALSE(ParseQueryPredictionsRequest(R"({"kana":"かな","mode":"word"})").has_value());
  EXPECT_FALSE(
      ParseQueryPredictionsRequest(R"({"kana":3,"leftSideContext":"","mode":"word"})").has_value());
  EXPECT_FALSE(
      ParseQueryPredictionsRequest(R"({"kana":"かな","leftSideContext":null,"mode":"word"})")
          .has_value());
  EXPECT_FALSE(ParseQueryPredictionsRequest(R"({"kana":"かな","leftSideContext":"","mode":42})")
                   .has_value());
  EXPECT_FALSE(ParseQueryPredictionsResponse("{}").has_value());
  EXPECT_FALSE(ParseQueryPredictionsResponse(R"({"predictions":null})").has_value());
  const auto partial = ParseQueryPredictionsResponse(
      R"({"predictions":[{"surface":"日本","reading":"にほん"},{"surface":7},null]})");
  ASSERT_TRUE(partial);
  ASSERT_EQ(partial->predictions.size(), 1u);
  EXPECT_EQ(partial->predictions[0].surface, "日本");
  const auto failed = ParseQueryPredictionsResponse(
      R"({"ok":false,"error":"unsupported_prediction_mode","predictions":[]})");
  ASSERT_TRUE(failed);
  EXPECT_FALSE(failed->ok);
  EXPECT_EQ(failed->error, "unsupported_prediction_mode");
}

TEST(PayloadsTest, QueryCandidatesPunctuationSegmentsAndLegacyDefaults) {
  const auto old_request =
      azookey::ipc::ParseQueryCandidatesRequest(R"({"reading":"かな","live":true})");
  ASSERT_TRUE(old_request);
  EXPECT_FALSE(old_request->auto_punctuation);
  EXPECT_EQ(old_request->punctuation_style, "ja");

  azookey::ipc::QueryCandidatesResponse response;
  response.candidates = {{"今日は晴れです。", "きょうははれです", 0.9, "model"}};
  response.segments = {
      {0, 3, 0.9, false, "今日は", "きょうは", 0, 0, 0, 0},
      {3, 7, 0.8, false, "晴れです", "はれです", 0, 0, 0, 0},
      {7, 8, 0.0, true, "。", "", 11, 11, 0, 0},
  };
  const auto parsed = azookey::ipc::ParseQueryCandidatesResponse(
      azookey::ipc::BuildQueryCandidatesResponse(response));
  ASSERT_TRUE(parsed);
  ASSERT_EQ(parsed->segments.size(), 3u);
  EXPECT_EQ(parsed->segments.back().start_char, 7u);
  EXPECT_TRUE(parsed->segments.back().auto_punctuation);
  EXPECT_TRUE(parsed->segments.back().reading.empty());
  EXPECT_EQ(parsed->segments.back().pos, 11u);
  std::string joined;
  for (const auto& segment : parsed->segments) joined += segment.surface;
  EXPECT_EQ(joined, parsed->candidates.front().surface);

  const auto old_response = azookey::ipc::ParseQueryCandidatesResponse(
      R"({"candidates":[{"surface":"かな","reading":"かな"}]})");
  ASSERT_TRUE(old_response);
  EXPECT_TRUE(old_response->segments.empty());
}

TEST(PayloadsTest, QueryRequestsCarryTheForegroundAppAndOmitItWhenUnknown) {
  using namespace azookey::ipc;
  QueryCandidatesRequest request;
  request.reading = "かな";
  request.app = AppIdentity{"code.exe", "Chrome_WidgetWin_1"};
  const auto wire = json::Parse(BuildQueryCandidatesRequest(request));
  ASSERT_TRUE(wire);
  const auto* app = wire->FindObject("app");
  ASSERT_NE(app, nullptr);
  EXPECT_EQ(json::Value(*app).GetString("process_name"), "code.exe");
  EXPECT_EQ(json::Value(*app).GetString("window_class"), "Chrome_WidgetWin_1");
  const auto parsed = ParseQueryCandidatesRequest(BuildQueryCandidatesRequest(request));
  ASSERT_TRUE(parsed && parsed->app);
  EXPECT_EQ(parsed->app->process_name, "code.exe");
  EXPECT_EQ(parsed->app->window_class, "Chrome_WidgetWin_1");

  request.app.reset();
  EXPECT_FALSE(json::Parse(BuildQueryCandidatesRequest(request))->FindObject("app"));
  // An older TIP never sends app; a malformed one degrades to "unknown app"
  // instead of failing the query.
  for (const char* payload :
       {R"({"reading":"かな"})", R"({"reading":"かな","app":"code.exe"})",
        R"({"reading":"かな","app":{"process_name":7}})", R"({"reading":"かな","app":{}})"}) {
    const auto legacy = ParseQueryCandidatesRequest(payload);
    ASSERT_TRUE(legacy) << payload;
    EXPECT_FALSE(legacy->app.has_value()) << payload;
  }

  QueryPredictionsRequest prediction{"にほん", "", "word", AppIdentity{"outlook.exe", ""}};
  const auto parsed_prediction =
      ParseQueryPredictionsRequest(BuildQueryPredictionsRequest(prediction));
  ASSERT_TRUE(parsed_prediction && parsed_prediction->app);
  EXPECT_EQ(parsed_prediction->app->process_name, "outlook.exe");
  EXPECT_TRUE(parsed_prediction->app->window_class.empty());
  const auto legacy_prediction =
      ParseQueryPredictionsRequest(R"({"kana":"かな","leftSideContext":"","mode":"word"})");
  ASSERT_TRUE(legacy_prediction);
  EXPECT_FALSE(legacy_prediction->app.has_value());
}

TEST(PayloadsTest, CandidateTagRoundTripsAndIsOmittedWhenNone) {
  using namespace azookey::ipc;
  QueryCandidatesResponse response;
  response.candidates = {{"Nihon", "にほん", 1.0, "model", "", 4},
                         {"日本", "にほん", 0.5, "system"}};
  const auto json_text = BuildQueryCandidatesResponse(response);
  const auto wire = json::Parse(json_text);
  ASSERT_TRUE(wire);
  const auto* candidates = wire->GetArray("candidates");
  ASSERT_TRUE(candidates && candidates->size() == 2u);
  EXPECT_EQ((*candidates)[0].GetUInt("tag"), 4u);
  EXPECT_FALSE((*candidates)[1].Find("tag"));

  const auto parsed = ParseQueryCandidatesResponse(json_text);
  ASSERT_TRUE(parsed && parsed->candidates.size() == 2u);
  EXPECT_EQ(parsed->candidates[0].tag, 4u);
  EXPECT_EQ(parsed->candidates[1].tag, 0u);

  // Unknown tags pass through; values beyond uint8 decode as None.
  const auto other = ParseQueryCandidatesResponse(
      R"({"candidates":[{"surface":"a","reading":"a","tag":9},)"
      R"({"surface":"b","reading":"b","tag":256},{"surface":"c","reading":"c","tag":"x"}]})");
  ASSERT_TRUE(other && other->candidates.size() == 3u);
  EXPECT_EQ(other->candidates[0].tag, 9u);
  EXPECT_EQ(other->candidates[1].tag, 0u);
  EXPECT_EQ(other->candidates[2].tag, 0u);
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
  EXPECT_EQ(res_json.find("\"error_class\""), std::string::npos);
  auto res_parsed = azookey::ipc::ParseQueryBatchConversionResponse(res_json);
  ASSERT_TRUE(res_parsed.has_value());
  EXPECT_FALSE(res_parsed->error_class.has_value());
  EXPECT_EQ(res_parsed->full_surface, "日本語");
  ASSERT_EQ(res_parsed->segments.size(), 1u);
  ASSERT_EQ(res_parsed->segments[0].candidates.size(), 1u);
  EXPECT_EQ(res_parsed->segments[0].candidates[0].surface, "日本語");
}

TEST(PayloadsTest, QueryBatchConversionErrorClassRoundTrip) {
  azookey::ipc::QueryBatchConversionResponse response;
  response.error_class = "Network";

  const auto json = azookey::ipc::BuildQueryBatchConversionResponse(response);
  EXPECT_NE(json.find("\"error_class\":\"Network\""), std::string::npos);
  const auto parsed = azookey::ipc::ParseQueryBatchConversionResponse(json);
  ASSERT_TRUE(parsed.has_value());
  ASSERT_TRUE(parsed->error_class.has_value());
  EXPECT_EQ(*parsed->error_class, "Network");
}

TEST(PayloadsTest, QueryBatchConversionErrorClassAcceptsUnknownString) {
  const auto parsed = azookey::ipc::ParseQueryBatchConversionResponse(
      R"({"full_surface":"かな","error_class":"FutureError"})");
  ASSERT_TRUE(parsed.has_value());
  ASSERT_TRUE(parsed->error_class.has_value());
  EXPECT_EQ(*parsed->error_class, "FutureError");
  EXPECT_EQ(parsed->full_surface, "かな");
}

TEST(PayloadsTest, QueryBatchConversionErrorClassIgnoresMalformedValues) {
  for (const auto* json : {
           R"({"full_surface":"かな","error_class":null})",
           R"({"full_surface":"かな","error_class":7})",
           R"({"full_surface":"かな","error_class":false})",
           R"({"full_surface":"かな","error_class":{}})",
       }) {
    const auto parsed = azookey::ipc::ParseQueryBatchConversionResponse(json);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_FALSE(parsed->error_class.has_value());
    EXPECT_EQ(parsed->full_surface, "かな");
  }
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

// DEV-1184 M54 app rows: both commit requests carry the foreground app, and a
// TIP that omits it (or predates the field) decodes as "unknown app".
TEST(PayloadsTest, CommitRequestsCarryTheAppAndDefaultToUnknown) {
  using namespace azookey::ipc;
  CommitObservationRequest single;
  single.reading = "にほんご";
  single.chosen = {"日本語", "にほんご", 1.0, "user"};
  EXPECT_FALSE(json::Parse(BuildCommitObservationRequest(single))->Find("app"));
  const auto single_unknown = ParseCommitObservationRequest(BuildCommitObservationRequest(single));
  ASSERT_TRUE(single_unknown);
  EXPECT_FALSE(single_unknown->app.has_value());
  single.app = AppIdentity{"notepad.exe", "Notepad"};
  const auto single_known = ParseCommitObservationRequest(BuildCommitObservationRequest(single));
  ASSERT_TRUE(single_known && single_known->app);
  EXPECT_EQ(single_known->app->process_name, "notepad.exe");
  EXPECT_EQ(single_known->app->window_class, "Notepad");

  CommitSegmentsObservationRequest batch;
  ObservedSegment segment;
  segment.reading = "にほんご";
  segment.chosen = {"日本語", "にほんご", 1.0, "user"};
  batch.segments.push_back(segment);
  EXPECT_FALSE(json::Parse(BuildCommitSegmentsObservationRequest(batch))->Find("app"));
  const auto batch_unknown =
      ParseCommitSegmentsObservationRequest(BuildCommitSegmentsObservationRequest(batch));
  ASSERT_TRUE(batch_unknown);
  EXPECT_FALSE(batch_unknown->app.has_value());
  batch.app = AppIdentity{"Code.exe", "Chrome_WidgetWin_1"};
  const auto batch_known =
      ParseCommitSegmentsObservationRequest(BuildCommitSegmentsObservationRequest(batch));
  ASSERT_TRUE(batch_known && batch_known->app);
  EXPECT_EQ(batch_known->app->process_name, "Code.exe");
  EXPECT_EQ(batch_known->app->window_class, "Chrome_WidgetWin_1");
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

// DEV-1534: neologd_layer is a protocol v1 additive field, so a Host that
// predates it and an unknown state both decode as "absent".
TEST(PayloadsTest, QueryDiagnosticsCarriesTheNeologdLayerAsAnOptionalField) {
  using namespace azookey::ipc;
  QueryDiagnosticsPayload payload;
  payload.engine = "mock";
  payload.backend = "cpu";
  payload.fallback_state = "healthy";
  EXPECT_FALSE(json::Parse(BuildQueryDiagnostics(payload))->Find("neologd_layer"));
  EXPECT_FALSE(ParseQueryDiagnostics(BuildQueryDiagnostics(payload))->neologd_layer);

  payload.neologd_layer = NeologdLayerStatus{std::string(kNeologdLayerError), "pack size mismatch"};
  const auto error = ParseQueryDiagnostics(BuildQueryDiagnostics(payload));
  ASSERT_TRUE(error && error->neologd_layer);
  EXPECT_EQ(error->neologd_layer->state, kNeologdLayerError);
  EXPECT_EQ(error->neologd_layer->reason, std::optional<std::string>("pack size mismatch"));

  payload.neologd_layer = NeologdLayerStatus{std::string(kNeologdLayerMissingPack), std::nullopt};
  const auto missing = ParseQueryDiagnostics(BuildQueryDiagnostics(payload));
  ASSERT_TRUE(missing && missing->neologd_layer);
  EXPECT_EQ(missing->neologd_layer->state, kNeologdLayerMissingPack);
  EXPECT_FALSE(missing->neologd_layer->reason.has_value());

  const auto unknown =
      ParseQueryDiagnostics(R"({"engine":"mock","backend":"cpu","fallback_state":"healthy",)"
                            R"("neologd_layer":{"state":"downloading"}})");
  ASSERT_TRUE(unknown.has_value());
  EXPECT_FALSE(unknown->neologd_layer.has_value());

  const std::string base = R"({"engine":"mock","backend":"cpu","fallback_state":"healthy",)";
  for (const char* layer : {R"("neologd_layer":"ready"})", R"("neologd_layer":{"state":1}})",
                            R"("neologd_layer":{"reason":"pack size mismatch"}})"}) {
    const auto malformed = ParseQueryDiagnostics(base + layer);
    ASSERT_TRUE(malformed.has_value()) << layer;
    EXPECT_FALSE(malformed->neologd_layer.has_value()) << layer;
  }
  // reason belongs to "error" only.
  const auto ready = ParseQueryDiagnostics(
      base + R"("neologd_layer":{"state":"ready","reason":"pack size mismatch"}})");
  ASSERT_TRUE(ready && ready->neologd_layer);
  EXPECT_FALSE(ready->neologd_layer->reason.has_value());
}

// DEV-1529: CommitCorrection shares the commit privacy and dedupe fields, and
// kind must agree with selected_surface.
TEST(PayloadsTest, CommitCorrectionRoundTripsBothKinds) {
  using namespace azookey::ipc;
  CommitCorrectionRequest undo;
  undo.kind = std::string(kCorrectionKindUndo);
  undo.reading = "かんじ";
  undo.rejected_surface = "幹事";
  undo.left_context = "今日の";
  undo.timestamp_ms = 1700000000000ULL;
  undo.observation_id = "tip-1:3";
  undo.secure = false;
  undo.learning_allowed = true;
  undo.app = AppIdentity{"notepad.exe", "Notepad"};
  EXPECT_FALSE(json::Parse(BuildCommitCorrectionRequest(undo))->Find("selected_surface"));
  const auto parsed_undo = ParseCommitCorrectionRequest(BuildCommitCorrectionRequest(undo));
  ASSERT_TRUE(parsed_undo.has_value());
  EXPECT_EQ(parsed_undo->kind, kCorrectionKindUndo);
  EXPECT_EQ(parsed_undo->reading, "かんじ");
  EXPECT_EQ(parsed_undo->rejected_surface, "幹事");
  EXPECT_FALSE(parsed_undo->selected_surface.has_value());
  EXPECT_EQ(parsed_undo->left_context, "今日の");
  EXPECT_EQ(parsed_undo->timestamp_ms, 1700000000000ULL);
  EXPECT_EQ(parsed_undo->observation_id, "tip-1:3");
  EXPECT_FALSE(parsed_undo->secure);
  EXPECT_TRUE(parsed_undo->learning_allowed);
  ASSERT_TRUE(parsed_undo->app.has_value());
  EXPECT_EQ(parsed_undo->app->process_name, "notepad.exe");

  CommitCorrectionRequest reconvert = undo;
  reconvert.kind = std::string(kCorrectionKindReconvert);
  reconvert.selected_surface = "漢字";
  const auto parsed_reconvert =
      ParseCommitCorrectionRequest(BuildCommitCorrectionRequest(reconvert));
  ASSERT_TRUE(parsed_reconvert.has_value());
  EXPECT_EQ(parsed_reconvert->kind, kCorrectionKindReconvert);
  EXPECT_EQ(parsed_reconvert->selected_surface, std::optional<std::string>("漢字"));
}

TEST(PayloadsTest, CommitCorrectionDeniesPrivacyByDefaultAndRejectsInconsistentKinds) {
  using namespace azookey::ipc;
  const auto minimal = ParseCommitCorrectionRequest(
      R"({"kind":"undo","reading":"かんじ","rejected_surface":"幹事"})");
  ASSERT_TRUE(minimal.has_value());
  EXPECT_TRUE(minimal->secure);
  EXPECT_FALSE(minimal->learning_allowed);
  EXPECT_TRUE(minimal->observation_id.empty());
  EXPECT_FALSE(minimal->app.has_value());

  for (
      const char* json : {
          R"({"kind":"undo","reading":"かんじ","rejected_surface":"幹事","selected_surface":"漢字"})",
          R"({"kind":"reconvert","reading":"かんじ","rejected_surface":"幹事"})",
          R"({"kind":"reconvert","reading":"かんじ","rejected_surface":"幹事","selected_surface":"幹事"})",
          R"({"kind":"reconvert","reading":"かんじ","rejected_surface":"幹事","selected_surface":""})",
          R"({"kind":"replace","reading":"かんじ","rejected_surface":"幹事"})",
          R"({"reading":"かんじ","rejected_surface":"幹事"})",
          R"({"kind":"undo","reading":"","rejected_surface":"幹事"})",
          R"({"kind":"undo","reading":"かんじ","rejected_surface":""})",
          R"({"kind":"undo","reading":"かんじ"})",
      }) {
    EXPECT_FALSE(ParseCommitCorrectionRequest(json).has_value()) << json;
  }
}

TEST(PayloadsTest, ResetLearningStoreAndQueryPersonaRoundTrip) {
  using namespace azookey::ipc;
  ResetLearningStoreRequest reset;
  reset.store = "typo";
  const auto parsed_reset = ParseResetLearningStoreRequest(BuildResetLearningStoreRequest(reset));
  ASSERT_TRUE(parsed_reset.has_value());
  EXPECT_EQ(parsed_reset->store, "typo");
  EXPECT_FALSE(ParseResetLearningStoreRequest("{}").has_value());
  EXPECT_FALSE(ParseResetLearningStoreRequest(R"({"store":""})").has_value());

  ResetLearningStoreResponse failed;
  failed.ok = false;
  failed.error = std::string(kLearningDataErrorSaveFailed);
  const auto parsed_failed =
      ParseResetLearningStoreResponse(BuildResetLearningStoreResponse(failed));
  ASSERT_TRUE(parsed_failed.has_value());
  EXPECT_FALSE(parsed_failed->ok);
  EXPECT_EQ(parsed_failed->error, std::optional<std::string>("save_failed"));

  QueryPersonaResponse persona;
  persona.polite_ratio = 0.5;
  persona.casual_ratio = 0.25;
  persona.technical_ratio = 0.125;
  persona.kaomoji_ratio = 0.0;
  persona.sample_count = 8;
  persona.computed_at_epoch_sec = 1700000000;
  const auto parsed_persona = ParseQueryPersonaResponse(BuildQueryPersonaResponse(persona));
  ASSERT_TRUE(parsed_persona.has_value());
  EXPECT_TRUE(parsed_persona->ok);
  EXPECT_DOUBLE_EQ(parsed_persona->polite_ratio, 0.5);
  EXPECT_DOUBLE_EQ(parsed_persona->casual_ratio, 0.25);
  EXPECT_DOUBLE_EQ(parsed_persona->technical_ratio, 0.125);
  EXPECT_EQ(parsed_persona->sample_count, 8u);
  EXPECT_EQ(parsed_persona->computed_at_epoch_sec, 1700000000u);
  // A ratio outside [0, 1] is not displayed.
  EXPECT_FALSE(ParseQueryPersonaResponse(R"({"ok":true,"polite_ratio":1.5})").has_value());
  EXPECT_FALSE(ParseQueryPersonaResponse(R"({"polite_ratio":0.5})").has_value());
}

// DEV-1532: DetectAnomalies denies privacy by default, bounds its text and
// findings, and drops a finding it cannot place.
TEST(PayloadsTest, DetectAnomaliesRoundTripsAndBoundsItsFields) {
  using namespace azookey::ipc;
  DetectAnomaliesRequest request;
  request.text = "今日は晴れでした。";
  request.max_findings = 7;
  request.secure = false;
  request.learning_allowed = true;
  const auto parsed = ParseDetectAnomaliesRequest(BuildDetectAnomaliesRequest(request));
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->text, request.text);
  EXPECT_EQ(parsed->max_findings, 7u);
  EXPECT_FALSE(parsed->secure);
  EXPECT_TRUE(parsed->learning_allowed);

  const auto defaults = ParseDetectAnomaliesRequest(R"({"text":"あ"})");
  ASSERT_TRUE(defaults.has_value());
  EXPECT_TRUE(defaults->secure);
  EXPECT_FALSE(defaults->learning_allowed);
  EXPECT_EQ(defaults->max_findings, kDefaultAnomalyFindings);
  EXPECT_EQ(ParseDetectAnomaliesRequest(R"({"text":"あ","max_findings":0})")->max_findings, 1u);
  EXPECT_EQ(ParseDetectAnomaliesRequest(R"({"text":"あ","max_findings":999})")->max_findings,
            kMaxAnomalyFindings);
  EXPECT_FALSE(ParseDetectAnomaliesRequest(R"({"text":""})").has_value());
  EXPECT_FALSE(ParseDetectAnomaliesRequest("{}").has_value());
  DetectAnomaliesRequest oversized;
  oversized.text = std::string(kMaxAnomalyTextBytes + 1, 'a');
  EXPECT_FALSE(ParseDetectAnomaliesRequest(BuildDetectAnomaliesRequest(oversized)).has_value());

  DetectAnomaliesResponse response;
  response.findings.push_back({2, 4, "時制", {"晴れです"}, 0.75});
  const auto parsed_response = ParseDetectAnomaliesResponse(BuildDetectAnomaliesResponse(response));
  ASSERT_TRUE(parsed_response.has_value());
  EXPECT_TRUE(parsed_response->ok);
  ASSERT_EQ(parsed_response->findings.size(), 1u);
  EXPECT_EQ(parsed_response->findings[0].start, 2u);
  EXPECT_EQ(parsed_response->findings[0].length, 4u);
  EXPECT_EQ(parsed_response->findings[0].reason, "時制");
  EXPECT_EQ(parsed_response->findings[0].suggestions, std::vector<std::string>{"晴れです"});
  EXPECT_DOUBLE_EQ(parsed_response->findings[0].confidence, 0.75);

  const auto dropped = ParseDetectAnomaliesResponse(
      R"({"ok":true,"findings":[{"start":0,"length":0,"confidence":0.5},)"
      R"({"start":0,"length":1,"confidence":1.5},{"start":1,"length":1,"confidence":0.5}]})");
  ASSERT_TRUE(dropped.has_value());
  ASSERT_EQ(dropped->findings.size(), 1u);
  EXPECT_EQ(dropped->findings[0].start, 1u);

  DetectAnomaliesResponse failed;
  failed.ok = false;
  failed.error = std::string(kAnomalyErrorBlocked);
  const auto parsed_failed = ParseDetectAnomaliesResponse(BuildDetectAnomaliesResponse(failed));
  ASSERT_TRUE(parsed_failed.has_value());
  EXPECT_FALSE(parsed_failed->ok);
  EXPECT_EQ(parsed_failed->error, std::optional<std::string>("blocked"));
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
  EXPECT_TRUE(parsed_response->ok);
  EXPECT_FALSE(parsed_response->error);
}

TEST(PayloadsTest, NewWordResponsesCarryAnErrorChannel) {
  // A failure is distinguishable from "no words in that state".
  azookey::ipc::ListNewWordCandidatesResponse list;
  list.ok = false;
  list.error = std::string(azookey::ipc::kNewWordErrorStoreUnavailable);
  const auto parsed_list = azookey::ipc::ParseListNewWordCandidatesResponse(
      azookey::ipc::BuildListNewWordCandidatesResponse(list));
  ASSERT_TRUE(parsed_list);
  EXPECT_FALSE(parsed_list->ok);
  EXPECT_EQ(parsed_list->error, "store_unavailable");
  EXPECT_TRUE(parsed_list->items.empty());

  azookey::ipc::ResolveNewWordResponse resolve;
  resolve.ok = false;
  resolve.error = std::string(azookey::ipc::kNewWordErrorNotFound);
  const auto parsed_resolve =
      azookey::ipc::ParseResolveNewWordResponse(azookey::ipc::BuildResolveNewWordResponse(resolve));
  ASSERT_TRUE(parsed_resolve);
  EXPECT_FALSE(parsed_resolve->ok);
  EXPECT_FALSE(parsed_resolve->changed);
  EXPECT_EQ(parsed_resolve->error, "not_found");

  // Protocol v1 additive fields: a payload from before the error channel still
  // parses, as a successful list and as an unchanged resolve.
  const auto legacy_list = azookey::ipc::ParseListNewWordCandidatesResponse(R"({"items":[]})");
  ASSERT_TRUE(legacy_list);
  EXPECT_TRUE(legacy_list->ok);
  EXPECT_FALSE(legacy_list->error);
  const auto legacy_resolve = azookey::ipc::ParseResolveNewWordResponse(R"({"ok":true})");
  ASSERT_TRUE(legacy_resolve);
  EXPECT_TRUE(legacy_resolve->ok);
  EXPECT_FALSE(legacy_resolve->changed);
  EXPECT_FALSE(legacy_resolve->error);
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
  response.changed = true;
  const auto parsed_response = azookey::ipc::ParseResolveNewWordResponse(
      azookey::ipc::BuildResolveNewWordResponse(response));
  ASSERT_TRUE(parsed_response);
  EXPECT_TRUE(parsed_response->ok);
  EXPECT_TRUE(parsed_response->changed);
  EXPECT_FALSE(parsed_response->error);
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

TEST(PayloadsTest, ListModelsRoundTripAndStableSchema) {
  using namespace azookey::ipc;
  ListModelsRequest request;
  request.compute_sha256 = true;
  EXPECT_EQ(BuildListModelsRequest(request), R"({"compute_sha256":true})");
  const auto parsed_request =
      ParseListModelsRequest(R"({"directory":"%LOCALAPPDATA%/azooKey/models"})");
  ASSERT_TRUE(parsed_request);
  EXPECT_EQ(parsed_request->directory, "%LOCALAPPDATA%/azooKey/models");
  EXPECT_FALSE(parsed_request->compute_sha256);
  EXPECT_FALSE(ParseListModelsRequest(R"({"directory":7})"));
  EXPECT_FALSE(ParseListModelsRequest("[]"));

  ListModelsResponse response;
  ListedModel gguf;
  gguf.path = "C:/m/zenzai.gguf";
  gguf.file_name = "zenzai.gguf";
  gguf.format = "gguf";
  gguf.size_bytes = 1234;
  gguf.valid = true;
  gguf.metadata.model_family = "gpt2";
  gguf.metadata.quantization = "Q4_K_M";
  gguf.last_load_status = "success";
  ListedModel onnx;
  onnx.path = "C:/m/zenz-onnx";
  onnx.file_name = "zenz-onnx";
  onnx.format = "onnx_genai";
  onnx.last_load_status = "not_loaded";
  onnx.last_error = "missing_tokenizer";
  response.models = {gguf, onnx};
  // Section 9 snapshot: the field set and spelling are the stable schema the
  // settings app and `models list --json` consume.
  const auto json_text = BuildListModelsResponse(response);
  EXPECT_EQ(json_text, R"({"models":[{"file_name":"zenzai.gguf","format":"gguf","gguf_valid":true,)"
                       R"("last_load_status":"success","metadata":{"model_family":"gpt2",)"
                       R"("quantization":"Q4_K_M"},"path":"C:/m/zenzai.gguf","size_bytes":1234,)"
                       R"("valid":true},{"file_name":"zenz-onnx","format":"onnx_genai",)"
                       R"("last_error":"missing_tokenizer","last_load_status":"not_loaded",)"
                       R"("metadata":{},"path":"C:/m/zenz-onnx","size_bytes":0,"valid":false}]})");
  const auto parsed = ParseListModelsResponse(json_text);
  ASSERT_TRUE(parsed && parsed->ok);
  ASSERT_EQ(parsed->models.size(), 2u);
  EXPECT_EQ(parsed->models[0].metadata.quantization, "Q4_K_M");
  EXPECT_TRUE(parsed->models[0].valid);
  EXPECT_EQ(parsed->models[1].last_error, "missing_tokenizer");
  EXPECT_FALSE(parsed->models[1].valid);

  const auto failed = ParseListModelsResponse(
      R"({"ok":false,"error":"directory_outside_models_root","models":[{"path":1},{}]})");
  ASSERT_TRUE(failed);
  EXPECT_FALSE(failed->ok);
  EXPECT_EQ(failed->error, "directory_outside_models_root");
  EXPECT_TRUE(failed->models.empty());
  EXPECT_FALSE(ParseListModelsResponse(R"({"ok":true})"));

  // An empty models directory still round-trips as ok with an empty list.
  EXPECT_EQ(BuildListModelsResponse(ListModelsResponse{}), R"({"models":[]})");
  const auto empty = ParseListModelsResponse(BuildListModelsResponse(ListModelsResponse{}));
  ASSERT_TRUE(empty);
  EXPECT_TRUE(empty->ok);
  EXPECT_TRUE(empty->models.empty());
  EXPECT_FALSE(empty->error.has_value());
}

TEST(PayloadsTest, BenchmarkModelRoundTripAndStableSchema) {
  using namespace azookey::ipc;
  BenchmarkModelRequest request;
  request.path = "C:/m/zenzai.gguf";
  request.backend = "cuda";
  request.cases = {"にほんご", "わたし"};
  request.iterations = 20;
  request.warmup = 2;
  const auto parsed_request = ParseBenchmarkModelRequest(BuildBenchmarkModelRequest(request));
  ASSERT_TRUE(parsed_request);
  EXPECT_EQ(parsed_request->path, request.path);
  EXPECT_EQ(parsed_request->backend, "cuda");
  EXPECT_EQ(parsed_request->cases, request.cases);
  EXPECT_EQ(parsed_request->iterations, 20u);
  EXPECT_EQ(parsed_request->warmup, 2u);
  const auto defaults = ParseBenchmarkModelRequest(R"({"path":"m.gguf"})");
  ASSERT_TRUE(defaults);
  EXPECT_EQ(defaults->backend, "cpu");
  EXPECT_TRUE(defaults->cases.empty());
  EXPECT_EQ(defaults->iterations, 50u);
  EXPECT_EQ(defaults->warmup, 5u);
  EXPECT_FALSE(ParseBenchmarkModelRequest(R"({"path":""})"));
  EXPECT_FALSE(ParseBenchmarkModelRequest(R"({"path":"m","cases":["a",1]})"));
  EXPECT_FALSE(ParseBenchmarkModelRequest(R"({"path":"m","iterations":4294967296})"));

  BenchmarkModelResponse response;
  response.backend = "cpu";
  response.p50_ms = 1.5;
  response.p95_ms = 2.5;
  response.p99_ms = 4;
  response.load_ms = 120;
  response.rss_mb = 512;
  response.status = "success";
  response.iterations_completed = 50;
  const auto json_text = BuildBenchmarkModelResponse(response);
  EXPECT_EQ(json_text, R"({"backend":"cpu","error":null,"iterations_completed":50,"load_ms":120,)"
                       R"("p50_ms":1.5,"p95_ms":2.5,"p99_ms":4,"rss_mb":512,"status":"success",)"
                       R"("vram_mb":null})");
  const auto parsed = ParseBenchmarkModelResponse(json_text);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->status, "success");
  EXPECT_FALSE(parsed->error.has_value());
  EXPECT_FALSE(parsed->vram_mb.has_value());
  EXPECT_DOUBLE_EQ(parsed->p95_ms, 2.5);
  EXPECT_EQ(parsed->iterations_completed, 50u);

  response.status = "timeout";
  response.error = "timeout";
  response.vram_mb = 300;
  const auto timeout = ParseBenchmarkModelResponse(BuildBenchmarkModelResponse(response));
  ASSERT_TRUE(timeout);
  EXPECT_EQ(timeout->error, "timeout");
  EXPECT_EQ(timeout->vram_mb, 300.0);
  EXPECT_FALSE(ParseBenchmarkModelResponse(R"({"backend":"cpu"})"));
}

TEST(PayloadsTest, QueryCandidatesCarriesRawRomajiAndTheEnglishFlag) {
  using namespace azookey::ipc;
  QueryCandidatesRequest request;
  request.reading = "あっぷる";
  EXPECT_FALSE(json::Parse(BuildQueryCandidatesRequest(request))->Find("raw_romaji"));
  EXPECT_FALSE(json::Parse(BuildQueryCandidatesRequest(request))->Find("english_candidates"));
  request.raw_romaji = "Apple";
  request.english_candidates = true;
  const auto parsed = ParseQueryCandidatesRequest(BuildQueryCandidatesRequest(request));
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->raw_romaji, "Apple");
  EXPECT_TRUE(parsed->english_candidates);
  // Section 6.6: an older TIP omits both and gets the defaults.
  const auto legacy = ParseQueryCandidatesRequest(R"({"reading":"あっぷる"})");
  ASSERT_TRUE(legacy);
  EXPECT_TRUE(legacy->raw_romaji.empty());
  EXPECT_FALSE(legacy->english_candidates);

  QueryCandidatesResponse response;
  response.candidates = {{"アップル", "あっぷる", 0.8, "model"},
                         {"apple", "apple", 0.8, "heuristic", "", 4}};
  const auto parsed_response = ParseQueryCandidatesResponse(BuildQueryCandidatesResponse(response));
  ASSERT_TRUE(parsed_response && parsed_response->candidates.size() == 2u);
  EXPECT_EQ(parsed_response->candidates[1].tag, 4u);
  EXPECT_EQ(parsed_response->candidates[1].reading, "apple");
}

// -------- M49 learning data management (DEV-1190) --------

TEST(PayloadsTest, ListLearningEntriesRequestRoundTripsAndPinsTheWireKeys) {
  using namespace azookey::ipc;
  ListLearningEntriesRequest request;
  request.store = "learning";
  request.query = "nihon";
  request.limit = 50;
  request.offset = 10;
  const auto json_text = BuildListLearningEntriesRequest(request);
  EXPECT_EQ(json_text, R"({"limit":50,"offset":10,"query":"nihon","store":"learning"})");
  const auto parsed = ParseListLearningEntriesRequest(json_text);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->store, "learning");
  EXPECT_EQ(parsed->query, "nihon");
  EXPECT_EQ(parsed->limit, 50u);
  EXPECT_EQ(parsed->offset, 10u);

  const auto defaults = ParseListLearningEntriesRequest(R"({"store":"typo"})");
  ASSERT_TRUE(defaults);
  EXPECT_TRUE(defaults->query.empty());
  EXPECT_EQ(defaults->limit, 100u);
  EXPECT_EQ(defaults->offset, 0u);

  // Section 4.1: the page size is capped at the ceiling rather than rejected.
  const auto capped = ParseListLearningEntriesRequest(R"({"store":"learning","limit":100000})");
  ASSERT_TRUE(capped);
  EXPECT_EQ(capped->limit, kMaxLearningEntries);
}

TEST(PayloadsTest, ListLearningEntriesRequestRejectsMissingStoreAndWrongTypes) {
  using namespace azookey::ipc;
  EXPECT_FALSE(ParseListLearningEntriesRequest(R"({})"));
  EXPECT_FALSE(ParseListLearningEntriesRequest(R"({"store":""})"));
  EXPECT_FALSE(ParseListLearningEntriesRequest(R"({"store":1})"));
  EXPECT_FALSE(ParseListLearningEntriesRequest(R"({"store":"learning","query":7})"));
  EXPECT_FALSE(ParseListLearningEntriesRequest(R"({"store":"learning","limit":"100"})"));
  EXPECT_FALSE(ParseListLearningEntriesRequest(R"({"store":"learning","limit":-1})"));
  EXPECT_FALSE(ParseListLearningEntriesRequest(R"({"store":"learning","offset":-1})"));
  EXPECT_FALSE(ParseListLearningEntriesRequest(R"({"store":"learning","offset":true})"));
  EXPECT_FALSE(ParseListLearningEntriesRequest(R"({"store":"learning","offset":4294967296})"));
  EXPECT_FALSE(ParseListLearningEntriesRequest("[]"));
}

TEST(PayloadsTest, ListLearningEntriesResponseRoundTripsEntries) {
  using namespace azookey::ipc;
  ListLearningEntriesResponse response;
  response.total = 1234;
  LearningEntryField kana;
  kana.id = "0123456789abcdef";
  kana.channel = "kana";
  kana.reading = "にほんご";
  kana.surface = "日本語";
  kana.weight = 4.5;
  kana.last_updated_epoch_sec = 1780000000;
  kana.tags = {"notepad.exe"};
  kana.metadata = {{"commit_count", "3"}, {"reject_count", "0"}};
  LearningEntryField word;
  word.id = "fedcba9876543210";
  word.reading = "あずき";
  word.surface = "azooKey";
  response.entries = {kana, word};

  const auto json_text = BuildListLearningEntriesResponse(response);
  const auto wire = json::Parse(json_text);
  ASSERT_TRUE(wire);
  // ok is emitted only on failure, error only when set, channel only when set.
  EXPECT_FALSE(wire->Find("ok"));
  EXPECT_FALSE(wire->Find("error"));
  const auto* entries = wire->GetArray("entries");
  ASSERT_TRUE(entries && entries->size() == 2u);
  EXPECT_TRUE((*entries)[0].Find("channel"));
  EXPECT_FALSE((*entries)[1].Find("channel"));
  for (const auto* key :
       {"id", "reading", "surface", "weight", "last_updated_epoch_sec", "tags", "metadata"}) {
    EXPECT_TRUE((*entries)[0].Find(key)) << key;
  }

  const auto parsed = ParseListLearningEntriesResponse(json_text);
  ASSERT_TRUE(parsed);
  EXPECT_TRUE(parsed->ok);
  EXPECT_FALSE(parsed->error.has_value());
  EXPECT_EQ(parsed->total, 1234u);
  ASSERT_EQ(parsed->entries.size(), 2u);
  EXPECT_EQ(parsed->entries[0].id, "0123456789abcdef");
  EXPECT_EQ(parsed->entries[0].channel, "kana");
  EXPECT_EQ(parsed->entries[0].reading, "にほんご");
  EXPECT_EQ(parsed->entries[0].surface, "日本語");
  EXPECT_DOUBLE_EQ(parsed->entries[0].weight, 4.5);
  EXPECT_EQ(parsed->entries[0].last_updated_epoch_sec, 1780000000u);
  EXPECT_EQ(parsed->entries[0].tags, std::vector<std::string>{"notepad.exe"});
  EXPECT_EQ(parsed->entries[0].metadata.at("commit_count"), "3");
  EXPECT_EQ(parsed->entries[0].metadata.size(), 2u);
  EXPECT_TRUE(parsed->entries[1].channel.empty());
  EXPECT_TRUE(parsed->entries[1].tags.empty());

  ListLearningEntriesResponse failed;
  failed.ok = false;
  failed.error = std::string(kLearningDataErrorStoreUnavailable);
  const auto parsed_failed =
      ParseListLearningEntriesResponse(BuildListLearningEntriesResponse(failed));
  ASSERT_TRUE(parsed_failed);
  EXPECT_FALSE(parsed_failed->ok);
  EXPECT_EQ(parsed_failed->error, "store_unavailable");
  EXPECT_TRUE(parsed_failed->entries.empty());
}

TEST(PayloadsTest, ListLearningEntriesResponseSkipsMalformedEntriesAndBoundsSizes) {
  using namespace azookey::ipc;
  // Entries missing id / reading / surface are skipped; non-string tags and
  // metadata values are dropped one by one.
  const auto lenient = ParseListLearningEntriesResponse(
      R"({"total":3,"entries":[{"reading":"a","surface":"b"},7,)"
      R"({"id":"x","reading":"a","surface":"b","tags":["t",1],"metadata":{"k":"v","n":2}}]})");
  ASSERT_TRUE(lenient);
  ASSERT_EQ(lenient->entries.size(), 1u);
  EXPECT_EQ(lenient->entries[0].tags, std::vector<std::string>{"t"});
  EXPECT_EQ(lenient->entries[0].metadata.size(), 1u);

  // More entries than one page may hold rejects the whole response.
  ListLearningEntriesResponse oversized;
  LearningEntryField entry;
  entry.id = "x";
  entry.reading = "a";
  entry.surface = "b";
  oversized.entries.assign(kMaxLearningEntries + 1, entry);
  EXPECT_FALSE(ParseListLearningEntriesResponse(BuildListLearningEntriesResponse(oversized)));
  oversized.entries.resize(kMaxLearningEntries);
  const auto full_page =
      ParseListLearningEntriesResponse(BuildListLearningEntriesResponse(oversized));
  ASSERT_TRUE(full_page);
  EXPECT_EQ(full_page->entries.size(), kMaxLearningEntries);

  // An entry over the tag or metadata cap is malformed and skipped.
  ListLearningEntriesResponse heavy;
  LearningEntryField many_tags = entry;
  many_tags.tags.assign(kMaxLearningEntryTags + 1, "t");
  LearningEntryField many_keys = entry;
  for (std::size_t i = 0; i <= kMaxLearningEntryMetadata; ++i) {
    many_keys.metadata.emplace("k" + std::to_string(i), "v");
  }
  heavy.entries = {many_tags, many_keys, entry};
  const auto parsed_heavy =
      ParseListLearningEntriesResponse(BuildListLearningEntriesResponse(heavy));
  ASSERT_TRUE(parsed_heavy);
  EXPECT_EQ(parsed_heavy->entries.size(), 1u);
}

TEST(PayloadsTest, ForgetLearningEntryRoundTripsBothForms) {
  using namespace azookey::ipc;
  ForgetLearningEntryRequest by_id;
  by_id.store = "user_dict";
  by_id.id = "0123456789abcdef";
  EXPECT_EQ(BuildForgetLearningEntryRequest(by_id),
            R"({"id":"0123456789abcdef","store":"user_dict"})");
  const auto parsed_id = ParseForgetLearningEntryRequest(BuildForgetLearningEntryRequest(by_id));
  ASSERT_TRUE(parsed_id);
  EXPECT_EQ(parsed_id->store, "user_dict");
  EXPECT_EQ(parsed_id->id, "0123456789abcdef");
  EXPECT_TRUE(parsed_id->reading.empty());
  EXPECT_TRUE(parsed_id->surface.empty());

  ForgetLearningEntryRequest by_pair;
  by_pair.store = "learning";
  by_pair.reading = "にほんご";
  by_pair.surface = "日本語";
  const auto parsed_pair =
      ParseForgetLearningEntryRequest(BuildForgetLearningEntryRequest(by_pair));
  ASSERT_TRUE(parsed_pair);
  EXPECT_TRUE(parsed_pair->id.empty());
  EXPECT_EQ(parsed_pair->reading, "にほんご");
  EXPECT_EQ(parsed_pair->surface, "日本語");

  ForgetLearningEntryResponse response;
  response.removed = true;
  EXPECT_EQ(BuildForgetLearningEntryResponse(response), R"({"removed":true})");
  const auto parsed = ParseForgetLearningEntryResponse(BuildForgetLearningEntryResponse(response));
  ASSERT_TRUE(parsed);
  EXPECT_TRUE(parsed->ok);
  EXPECT_TRUE(parsed->removed);
  EXPECT_FALSE(parsed->error.has_value());

  ForgetLearningEntryResponse failed;
  failed.ok = false;
  failed.error = std::string(kLearningDataErrorSaveFailed);
  const auto parsed_failed =
      ParseForgetLearningEntryResponse(BuildForgetLearningEntryResponse(failed));
  ASSERT_TRUE(parsed_failed);
  EXPECT_FALSE(parsed_failed->ok);
  EXPECT_FALSE(parsed_failed->removed);
  EXPECT_EQ(parsed_failed->error, "save_failed");
  EXPECT_FALSE(ParseForgetLearningEntryResponse("[]"));
}

TEST(PayloadsTest, ForgetLearningEntryRejectsAmbiguousOrMalformedForms) {
  using namespace azookey::ipc;
  // store is required and non-empty.
  EXPECT_FALSE(ParseForgetLearningEntryRequest(R"({"id":"x"})"));
  EXPECT_FALSE(ParseForgetLearningEntryRequest(R"({"store":"","id":"x"})"));
  EXPECT_FALSE(ParseForgetLearningEntryRequest(R"({"store":1,"id":"x"})"));
  // Neither form, or both forms at once.
  EXPECT_FALSE(ParseForgetLearningEntryRequest(R"({"store":"learning"})"));
  EXPECT_FALSE(ParseForgetLearningEntryRequest(R"({"store":"learning","id":""})"));
  EXPECT_FALSE(ParseForgetLearningEntryRequest(
      R"({"store":"learning","id":"x","reading":"a","surface":"b"})"));
  EXPECT_FALSE(ParseForgetLearningEntryRequest(R"({"store":"learning","id":"x","reading":"a"})"));
  // Half a pair.
  EXPECT_FALSE(ParseForgetLearningEntryRequest(R"({"store":"learning","reading":"a"})"));
  EXPECT_FALSE(ParseForgetLearningEntryRequest(R"({"store":"learning","surface":"b"})"));
  // The pair form is only for the learning store.
  EXPECT_FALSE(
      ParseForgetLearningEntryRequest(R"({"store":"user_dict","reading":"a","surface":"b"})"));
  // Wrong types.
  EXPECT_FALSE(ParseForgetLearningEntryRequest(R"({"store":"learning","id":7})"));
  EXPECT_FALSE(
      ParseForgetLearningEntryRequest(R"({"store":"learning","reading":1,"surface":"b"})"));
  EXPECT_FALSE(
      ParseForgetLearningEntryRequest(R"({"store":"learning","reading":"a","surface":null})"));
}

TEST(PayloadsTest, ExportLearningDataRoundTripsAndAppliesDefaults) {
  using namespace azookey::ipc;
  ExportLearningDataRequest request;
  request.stores = {"learning", "user_dict"};
  request.destination_path = "C:/Users/me/Desktop/azookey-backup.zip";
  request.encrypt = false;
  request.include_settings = true;
  const auto json_text = BuildExportLearningDataRequest(request);
  EXPECT_EQ(json_text,
            R"({"destination_path":"C:/Users/me/Desktop/azookey-backup.zip","encrypt":false,)"
            R"("include_settings":true,"stores":["learning","user_dict"]})");
  const auto parsed = ParseExportLearningDataRequest(json_text);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->stores, (std::vector<std::string>{"learning", "user_dict"}));
  EXPECT_EQ(parsed->destination_path, "C:/Users/me/Desktop/azookey-backup.zip");
  EXPECT_FALSE(parsed->encrypt);
  // Decoded as sent; the Host is the one that answers it with "unsupported".
  EXPECT_TRUE(parsed->include_settings);

  const auto defaults =
      ParseExportLearningDataRequest(R"({"stores":["typo"],"destination_path":"C:/b.zip"})");
  ASSERT_TRUE(defaults);
  EXPECT_TRUE(defaults->encrypt);
  EXPECT_FALSE(defaults->include_settings);

  ExportLearningDataResponse response;
  response.status = "success";
  response.file_size_bytes = 12345;
  response.encrypted = true;
  response.items = {{"learning", "learning.tsv.enc", 1234, "ab12"},
                    {"user_dictionary", "user_dict.json.enc", 42, "cd34"}};
  const auto parsed_response =
      ParseExportLearningDataResponse(BuildExportLearningDataResponse(response));
  ASSERT_TRUE(parsed_response);
  EXPECT_EQ(parsed_response->status, "success");
  EXPECT_FALSE(parsed_response->error.has_value());
  EXPECT_EQ(parsed_response->file_size_bytes, 12345u);
  EXPECT_TRUE(parsed_response->encrypted);
  ASSERT_EQ(parsed_response->items.size(), 2u);
  EXPECT_EQ(parsed_response->items[1].name, "user_dictionary");
  EXPECT_EQ(parsed_response->items[1].file, "user_dict.json.enc");
  EXPECT_EQ(parsed_response->items[1].count, 42u);
  EXPECT_EQ(parsed_response->items[1].sha256, "cd34");

  ExportLearningDataResponse failed;
  failed.error = "invalid_path";
  const auto parsed_failed =
      ParseExportLearningDataResponse(BuildExportLearningDataResponse(failed));
  ASSERT_TRUE(parsed_failed);
  EXPECT_EQ(parsed_failed->status, "error");
  EXPECT_EQ(parsed_failed->error, "invalid_path");
  EXPECT_TRUE(parsed_failed->items.empty());
}

TEST(PayloadsTest, ExportLearningDataRejectsMalformedRequestsAndResponses) {
  using namespace azookey::ipc;
  const std::string path = R"("destination_path":"C:/b.zip")";
  EXPECT_FALSE(ParseExportLearningDataRequest("{" + path + "}"));
  EXPECT_FALSE(ParseExportLearningDataRequest(R"({"stores":[],)" + path + "}"));
  EXPECT_FALSE(ParseExportLearningDataRequest(R"({"stores":"learning",)" + path + "}"));
  EXPECT_FALSE(ParseExportLearningDataRequest(R"({"stores":["learning",1],)" + path + "}"));
  EXPECT_FALSE(ParseExportLearningDataRequest(R"({"stores":[""],)" + path + "}"));
  EXPECT_FALSE(ParseExportLearningDataRequest(R"({"stores":["learning"]})"));
  EXPECT_FALSE(ParseExportLearningDataRequest(R"({"stores":["learning"],"destination_path":""})"));
  EXPECT_FALSE(
      ParseExportLearningDataRequest(R"({"stores":["learning"],"encrypt":"no",)" + path + "}"));
  EXPECT_FALSE(ParseExportLearningDataRequest(R"({"stores":["learning"],"include_settings":1,)" +
                                              path + "}"));
  ExportLearningDataRequest too_many;
  too_many.stores.assign(kMaxLearningDataStores + 1, "learning");
  too_many.destination_path = "C:/b.zip";
  EXPECT_FALSE(ParseExportLearningDataRequest(BuildExportLearningDataRequest(too_many)));

  // status is required; more items than the cap reject the response.
  EXPECT_FALSE(ParseExportLearningDataResponse(R"({"file_size_bytes":1})"));
  ExportLearningDataResponse oversized;
  oversized.status = "success";
  oversized.items.assign(kMaxLearningDataStores + 1, {"learning", "f", 1, "h"});
  EXPECT_FALSE(ParseExportLearningDataResponse(BuildExportLearningDataResponse(oversized)));
}

TEST(PayloadsTest, ImportLearningDataRoundTripsAndAppliesDefaults) {
  using namespace azookey::ipc;
  ImportLearningDataRequest request;
  request.source_path = "C:/b.zip";
  request.conflict_resolution = "keep_both";
  request.stores = {"learning", "user_dict"};
  const auto json_text = BuildImportLearningDataRequest(request);
  EXPECT_EQ(json_text, R"({"conflict_resolution":"keep_both","source_path":"C:/b.zip",)"
                       R"("stores":["learning","user_dict"]})");
  const auto parsed = ParseImportLearningDataRequest(json_text);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->source_path, "C:/b.zip");
  EXPECT_EQ(parsed->conflict_resolution, "keep_both");
  EXPECT_EQ(parsed->stores, (std::vector<std::string>{"learning", "user_dict"}));

  const auto defaults =
      ParseImportLearningDataRequest(R"({"source_path":"C:/b.zip","stores":["learning"]})");
  ASSERT_TRUE(defaults);
  EXPECT_EQ(defaults->conflict_resolution, "merge");
  EXPECT_TRUE(ParseImportLearningDataRequest(
      R"({"source_path":"C:/b.zip","stores":["learning"],"conflict_resolution":"overwrite"})"));

  ImportLearningDataResponse response;
  response.status = "success";
  response.imported_counts = {{"learning", 1234}, {"user_dictionary", 42}};
  response.skipped_counts = {{"learning", 5}};
  EXPECT_EQ(BuildImportLearningDataResponse(response),
            R"({"conflict_counts":{},"imported_counts":{"learning":1234,"user_dictionary":42},)"
            R"("skipped_counts":{"learning":5},"status":"success"})");
  const auto parsed_response =
      ParseImportLearningDataResponse(BuildImportLearningDataResponse(response));
  ASSERT_TRUE(parsed_response);
  EXPECT_EQ(parsed_response->status, "success");
  EXPECT_FALSE(parsed_response->error.has_value());
  EXPECT_EQ(parsed_response->imported_counts, response.imported_counts);
  EXPECT_EQ(parsed_response->skipped_counts, response.skipped_counts);
  EXPECT_TRUE(parsed_response->conflict_counts.empty());

  ImportLearningDataResponse failed;
  failed.error = "decrypt_failed";
  const auto parsed_failed =
      ParseImportLearningDataResponse(BuildImportLearningDataResponse(failed));
  ASSERT_TRUE(parsed_failed);
  EXPECT_EQ(parsed_failed->status, "error");
  EXPECT_EQ(parsed_failed->error, "decrypt_failed");
}

TEST(PayloadsTest, ImportLearningDataRejectsMalformedRequestsAndResponses) {
  using namespace azookey::ipc;
  EXPECT_FALSE(ParseImportLearningDataRequest(R"({"stores":["learning"]})"));
  EXPECT_FALSE(ParseImportLearningDataRequest(R"({"source_path":"","stores":["learning"]})"));
  EXPECT_FALSE(ParseImportLearningDataRequest(R"({"source_path":1,"stores":["learning"]})"));
  EXPECT_FALSE(ParseImportLearningDataRequest(R"({"source_path":"C:/b.zip"})"));
  EXPECT_FALSE(ParseImportLearningDataRequest(R"({"source_path":"C:/b.zip","stores":[]})"));
  EXPECT_FALSE(ParseImportLearningDataRequest(R"({"source_path":"C:/b.zip","stores":[1]})"));
  EXPECT_FALSE(ParseImportLearningDataRequest(
      R"({"source_path":"C:/b.zip","stores":["learning"],"conflict_resolution":"replace"})"));
  EXPECT_FALSE(ParseImportLearningDataRequest(
      R"({"source_path":"C:/b.zip","stores":["learning"],"conflict_resolution":""})"));
  EXPECT_FALSE(ParseImportLearningDataRequest(
      R"({"source_path":"C:/b.zip","stores":["learning"],"conflict_resolution":1})"));

  // status is required; non-integer counts are dropped; an oversized map rejects.
  EXPECT_FALSE(ParseImportLearningDataResponse(R"({"imported_counts":{}})"));
  const auto lenient = ParseImportLearningDataResponse(
      R"({"status":"success","imported_counts":{"learning":3,"typo":-1,"x":"7"}})");
  ASSERT_TRUE(lenient);
  EXPECT_EQ(lenient->imported_counts, (std::map<std::string, uint64_t>{{"learning", 3}}));
  ImportLearningDataResponse oversized;
  oversized.status = "success";
  for (std::size_t i = 0; i <= kMaxLearningDataStores; ++i) {
    oversized.conflict_counts.emplace("s" + std::to_string(i), 1);
  }
  EXPECT_FALSE(ParseImportLearningDataResponse(BuildImportLearningDataResponse(oversized)));
}
