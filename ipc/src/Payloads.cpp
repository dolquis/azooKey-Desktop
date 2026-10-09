#include "azookey/ipc/Payloads.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "azookey/ipc/Json.h"
#include "azookey/ipc/Limits.h"

namespace azookey::ipc {

namespace {

namespace j = ::azookey::ipc::json;

j::Value CandidateToJson(const CandidateField& c) {
  j::Object o;
  o.emplace("surface", j::Value(c.surface));
  o.emplace("reading", j::Value(c.reading));
  o.emplace("score", j::Value(c.score));
  o.emplace("source", j::Value(c.source));
  if (!c.description.empty()) o.emplace("description", j::Value(c.description));
  if (c.tag != 0) o.emplace("tag", j::Value(static_cast<uint64_t>(c.tag)));
  return j::Value(std::move(o));
}

void AppToJson(const std::optional<AppIdentity>& app, j::Object& o) {
  if (!app) return;
  j::Object a;
  a.emplace("process_name", j::Value(app->process_name));
  a.emplace("window_class", j::Value(app->window_class));
  o.emplace("app", j::Value(std::move(a)));
}

// A malformed app object degrades to "unknown app" rather than rejecting the
// query: the profile is an enhancement and the global settings still apply.
std::optional<AppIdentity> AppFromJson(const j::Value& v) {
  const auto* a = v.FindObject("app");
  if (!a) return std::nullopt;
  const j::Value app(*a);
  AppIdentity identity;
  identity.process_name = app.GetString("process_name").value_or(std::string());
  identity.window_class = app.GetString("window_class").value_or(std::string());
  if (identity.process_name.empty() && identity.window_class.empty()) return std::nullopt;
  return identity;
}

std::optional<CandidateField> CandidateFromJson(const j::Value& v) {
  if (!v.IsObject()) return std::nullopt;
  CandidateField c;
  auto surface = v.GetString("surface");
  auto reading = v.GetString("reading");
  auto score = v.GetNumber("score");
  auto source = v.GetString("source");
  if (!surface || !reading) return std::nullopt;
  c.surface = std::move(*surface);
  c.reading = std::move(*reading);
  c.score = score.value_or(0.0);
  c.source = source.value_or(std::string());
  c.description = v.GetString("description").value_or(std::string());
  const auto tag = v.GetUInt("tag").value_or(0);
  c.tag = tag <= 0xFF ? static_cast<uint8_t>(tag) : 0;
  return c;
}

std::optional<BatchConversionSegment> BatchSegmentFromJson(const j::Value& v) {
  if (!v.IsObject()) return std::nullopt;
  BatchConversionSegment segment;
  auto reading = v.GetString("reading");
  if (!reading) return std::nullopt;
  segment.reading = std::move(*reading);
  if (const auto* arr = v.GetArray("candidates")) {
    for (const auto& e : *arr) {
      if (auto c = CandidateFromJson(e)) segment.candidates.push_back(std::move(*c));
    }
  }
  return segment;
}

j::Value BatchSegmentToJson(const BatchConversionSegment& segment) {
  j::Object o;
  o.emplace("reading", j::Value(segment.reading));
  j::Array candidates;
  for (const auto& c : segment.candidates) candidates.push_back(CandidateToJson(c));
  o.emplace("candidates", j::Value(std::move(candidates)));
  return j::Value(std::move(o));
}

std::optional<j::Value> ParseObject(const std::string& s) {
  auto v = j::Parse(s);
  if (!v || !v->IsObject()) return std::nullopt;
  return v;
}

}  // namespace

// -------- Handshake --------

std::string BuildHandshakeRequest(const HandshakeRequest& p) {
  j::Object o;
  o.emplace("tip_version", j::Value(p.tip_version));
  o.emplace("protocol_version", j::Value(p.protocol_version));
  j::Array caps;
  for (const auto& c : p.capabilities) caps.emplace_back(j::Value(c));
  o.emplace("capabilities", j::Value(std::move(caps)));
  if (!p.client_id.empty()) {
    o.emplace("client_id", j::Value(p.client_id));
  }
  if (!p.handshake_token.empty()) {
    o.emplace("handshake_token", j::Value(p.handshake_token));
  }
  return j::Stringify(j::Value(std::move(o)));
}

std::optional<HandshakeRequest> ParseHandshakeRequest(const std::string& json) {
  auto v = ParseObject(json);
  if (!v) return std::nullopt;
  HandshakeRequest p;
  auto tip = v->GetString("tip_version");
  auto pv = v->GetInt("protocol_version");
  if (!tip) return std::nullopt;
  p.tip_version = std::move(*tip);
  p.protocol_version = static_cast<int>(pv.value_or(kHandshakeProtocolVersion));
  if (const auto* caps = v->GetArray("capabilities")) {
    for (const auto& e : *caps) {
      if (e.IsString()) p.capabilities.push_back(e.AsString());
    }
  }
  p.client_id = v->GetString("client_id").value_or(std::string());
  p.handshake_token = v->GetString("handshake_token").value_or(std::string());
  return p;
}

std::string BuildHandshakeResponse(const HandshakeResponse& p) {
  j::Object o;
  o.emplace("host_version", j::Value(p.host_version));
  o.emplace("protocol_version", j::Value(p.protocol_version));
  o.emplace("accepted", j::Value(p.accepted));
  o.emplace("model_loaded", j::Value(p.model_loaded));
  j::Array capabilities;
  for (const auto& capability : p.capabilities) capabilities.emplace_back(capability);
  o.emplace("capabilities", j::Value(std::move(capabilities)));
  if (!p.host_generation_id.empty()) {
    o.emplace("host_generation_id", j::Value(p.host_generation_id));
  }
  o.emplace("batch_romaji_conversion", j::Value(p.batch_romaji_conversion));
  o.emplace("batch_romaji_preview_style", j::Value(p.batch_romaji_preview_style));
  o.emplace("batch_conversion_mode", j::Value(p.batch_conversion_mode));
  o.emplace("batch_auto_punctuation", j::Value(p.batch_auto_punctuation));
  o.emplace("number_rewriter", j::Value(p.number_rewriter));
  o.emplace("katakana_rewriter", j::Value(p.katakana_rewriter));
  o.emplace("symbol_rewriter", j::Value(p.symbol_rewriter));
  o.emplace("emoji_rewriter", j::Value(p.emoji_rewriter));
  o.emplace("emoji_trigger_search", j::Value(p.emoji_trigger_search));
  o.emplace("emoji_max_candidates", j::Value(static_cast<uint64_t>(p.emoji_max_candidates)));
  o.emplace("emoji_trigger_min_query_length",
            j::Value(static_cast<uint64_t>(p.emoji_trigger_min_query_length)));
  o.emplace("max_candidates", j::Value(static_cast<uint64_t>(p.max_candidates)));
  return j::Stringify(j::Value(std::move(o)));
}

std::optional<HandshakeResponse> ParseHandshakeResponse(const std::string& json) {
  auto v = ParseObject(json);
  if (!v) return std::nullopt;
  HandshakeResponse p;
  auto host = v->GetString("host_version");
  if (!host) return std::nullopt;
  p.host_version = std::move(*host);
  p.protocol_version =
      static_cast<int>(v->GetInt("protocol_version").value_or(kHandshakeProtocolVersion));
  p.accepted = v->GetBool("accepted").value_or(false);
  p.model_loaded = v->GetBool("model_loaded").value_or(false);
  p.host_generation_id = v->GetString("host_generation_id").value_or(std::string());
  if (const auto* capabilities = v->GetArray("capabilities")) {
    for (const auto& capability : *capabilities) {
      if (capability.IsString()) p.capabilities.push_back(capability.AsString());
    }
  }
  p.batch_romaji_conversion = v->GetBool("batch_romaji_conversion").value_or(false);
  p.batch_romaji_preview_style =
      v->GetString("batch_romaji_preview_style").value_or(std::string("kana"));
  p.batch_conversion_mode = v->GetString("batch_conversion_mode").value_or(std::string("neural"));
  p.batch_auto_punctuation = v->GetBool("batch_auto_punctuation").value_or(false);
  p.number_rewriter = v->GetBool("number_rewriter").value_or(false);
  p.katakana_rewriter = v->GetBool("katakana_rewriter").value_or(false);
  p.symbol_rewriter = v->GetBool("symbol_rewriter").value_or(false);
  p.emoji_rewriter = v->GetBool("emoji_rewriter").value_or(false);
  p.emoji_trigger_search = v->GetBool("emoji_trigger_search").value_or(true);
  p.emoji_max_candidates = static_cast<uint32_t>(
      std::clamp<int64_t>(v->GetInt("emoji_max_candidates").value_or(12), 1, 50));
  p.emoji_trigger_min_query_length = static_cast<uint32_t>(
      std::clamp<int64_t>(v->GetInt("emoji_trigger_min_query_length").value_or(1), 1, 8));
  if (const auto max_candidates = v->GetUInt("max_candidates");
      max_candidates && *max_candidates >= 1 && *max_candidates <= 32) {
    p.max_candidates = static_cast<uint32_t>(*max_candidates);
  }
  return p;
}

// -------- Ping --------

std::string BuildPing(const PingPayload& p) {
  j::Object o;
  o.emplace("nonce", j::Value(p.nonce));
  o.emplace("t_ms", j::Value(p.t_ms));
  return j::Stringify(j::Value(std::move(o)));
}

std::optional<PingPayload> ParsePing(const std::string& json) {
  auto v = ParseObject(json);
  if (!v) return std::nullopt;
  PingPayload p;
  auto nonce = v->GetUInt("nonce");
  auto t_ms = v->GetUInt("t_ms");
  if (!nonce) return std::nullopt;
  p.nonce = *nonce;
  p.t_ms = t_ms.value_or(0);
  return p;
}

// -------- Health --------

std::string BuildHealth(const HealthPayload& p) {
  j::Object o;
  o.emplace("status", j::Value(p.status));
  o.emplace("backend", j::Value(p.backend));
  o.emplace("model_loaded", j::Value(p.model_loaded));
  if (p.vram_mb) o.emplace("vram_mb", j::Value(static_cast<uint64_t>(*p.vram_mb)));
  if (p.last_error) o.emplace("last_error", j::Value(*p.last_error));
  return j::Stringify(j::Value(std::move(o)));
}

std::optional<HealthPayload> ParseHealth(const std::string& json) {
  auto v = ParseObject(json);
  if (!v) return std::nullopt;
  HealthPayload p;
  auto status = v->GetString("status");
  auto backend = v->GetString("backend");
  if (!status || !backend) return std::nullopt;
  p.status = std::move(*status);
  p.backend = std::move(*backend);
  p.model_loaded = v->GetBool("model_loaded").value_or(false);
  if (auto vram = v->GetUInt("vram_mb")) p.vram_mb = static_cast<uint32_t>(*vram);
  if (auto err = v->GetString("last_error")) p.last_error = std::move(*err);
  return p;
}

std::string BuildQueryDiagnostics(const QueryDiagnosticsPayload& p) {
  j::Object o;
  o.emplace("model_loaded", j::Value(p.model_loaded));
  if (p.loaded_model_path) o.emplace("loaded_model_path", j::Value(*p.loaded_model_path));
  o.emplace("engine", j::Value(p.engine));
  o.emplace("backend", j::Value(p.backend));
  o.emplace("rss_mb", j::Value(p.rss_mb));
  if (p.ep) o.emplace("ep", j::Value(*p.ep));
  if (p.ep_state) o.emplace("ep_state", j::Value(*p.ep_state));
  if (p.ep_last_error) o.emplace("ep_last_error", j::Value(*p.ep_last_error));
  o.emplace("learning_entries", j::Value(p.learning_entries));
  o.emplace("user_dict_entries", j::Value(p.user_dict_entries));
  o.emplace("fallback_state", j::Value(p.fallback_state));
  if (p.last_error) o.emplace("last_error", j::Value(*p.last_error));
  if (p.neologd_layer) {
    j::Object layer;
    layer.emplace("state", j::Value(p.neologd_layer->state));
    if (p.neologd_layer->reason) layer.emplace("reason", j::Value(*p.neologd_layer->reason));
    o.emplace("neologd_layer", j::Value(std::move(layer)));
  }
  return j::Stringify(j::Value(std::move(o)));
}

namespace {

bool IsNeologdLayerState(std::string_view state) {
  return state == kNeologdLayerNotRequested || state == kNeologdLayerLoading ||
         state == kNeologdLayerReady || state == kNeologdLayerMissingPack ||
         state == kNeologdLayerError;
}

// The layer status is informational, so an unknown or malformed object
// degrades to "absent" instead of rejecting the whole diagnostics payload.
std::optional<NeologdLayerStatus> NeologdLayerFromJson(const j::Value& v) {
  const auto* object = v.FindObject("neologd_layer");
  if (!object) return std::nullopt;
  const j::Value layer(*object);
  auto state = layer.GetString("state");
  if (!state || !IsNeologdLayerState(*state)) return std::nullopt;
  NeologdLayerStatus status;
  // reason belongs to "error" only (auto-word-registration-spec section 15.14).
  if (*state == kNeologdLayerError) status.reason = layer.GetString("reason");
  status.state = std::move(*state);
  return status;
}

}  // namespace

std::optional<QueryDiagnosticsPayload> ParseQueryDiagnostics(const std::string& json) {
  auto v = ParseObject(json);
  if (!v) return std::nullopt;
  const auto engine = v->GetString("engine");
  const auto backend = v->GetString("backend");
  const auto fallback_state = v->GetString("fallback_state");
  if (!engine || !backend || !fallback_state) return std::nullopt;
  QueryDiagnosticsPayload p;
  p.model_loaded = v->GetBool("model_loaded").value_or(false);
  p.loaded_model_path = v->GetString("loaded_model_path");
  p.engine = *engine;
  p.backend = *backend;
  p.rss_mb = v->GetUInt("rss_mb").value_or(0);
  p.ep = v->GetString("ep");
  p.ep_state = v->GetString("ep_state");
  p.ep_last_error = v->GetString("ep_last_error");
  p.learning_entries = v->GetUInt("learning_entries").value_or(0);
  p.user_dict_entries = v->GetUInt("user_dict_entries").value_or(0);
  p.fallback_state = *fallback_state;
  p.last_error = v->GetString("last_error");
  p.neologd_layer = NeologdLayerFromJson(*v);
  return p;
}

// -------- LoadModel --------

std::string BuildLoadModelRequest(const LoadModelRequest& p) {
  j::Object o;
  o.emplace("path", j::Value(p.path));
  o.emplace("backend", j::Value(p.backend));
  if (p.n_gpu_layers) o.emplace("n_gpu_layers", j::Value(static_cast<int64_t>(*p.n_gpu_layers)));
  return j::Stringify(j::Value(std::move(o)));
}

std::optional<LoadModelRequest> ParseLoadModelRequest(const std::string& json) {
  auto v = ParseObject(json);
  if (!v) return std::nullopt;
  LoadModelRequest p;
  auto path = v->GetString("path");
  auto backend = v->GetString("backend");
  if (!path || !backend) return std::nullopt;
  p.path = std::move(*path);
  p.backend = std::move(*backend);
  if (auto n = v->GetInt("n_gpu_layers")) p.n_gpu_layers = static_cast<int32_t>(*n);
  return p;
}

std::string BuildLoadModelResponse(const LoadModelResponse& p) {
  j::Object o;
  o.emplace("ok", j::Value(p.ok));
  if (p.error) o.emplace("error", j::Value(*p.error));
  return j::Stringify(j::Value(std::move(o)));
}

std::optional<LoadModelResponse> ParseLoadModelResponse(const std::string& json) {
  auto v = ParseObject(json);
  if (!v) return std::nullopt;
  LoadModelResponse p;
  auto ok = v->GetBool("ok");
  if (!ok) return std::nullopt;
  p.ok = *ok;
  if (auto err = v->GetString("error")) p.error = std::move(*err);
  return p;
}

// -------- QueryCandidates --------

namespace {

j::Value LiveSegmentsToJson(const std::vector<LiveSegment>& segments) {
  j::Array array;
  for (const auto& segment : segments) {
    j::Object item;
    item.emplace("start_char", j::Value(static_cast<uint64_t>(segment.start_char)));
    item.emplace("end_char", j::Value(static_cast<uint64_t>(segment.end_char)));
    item.emplace("score", j::Value(segment.score));
    item.emplace("auto_punctuation", j::Value(segment.auto_punctuation));
    item.emplace("surface", j::Value(segment.surface));
    item.emplace("reading", j::Value(segment.reading));
    item.emplace("pos", j::Value(static_cast<uint64_t>(segment.pos)));
    item.emplace("head_pos", j::Value(static_cast<uint64_t>(segment.head_pos)));
    item.emplace("sem", j::Value(static_cast<uint64_t>(segment.sem)));
    item.emplace("head_sem", j::Value(static_cast<uint64_t>(segment.head_sem)));
    array.emplace_back(std::move(item));
  }
  return j::Value(std::move(array));
}

// Malformed segments are skipped, as malformed candidates are.
std::vector<LiveSegment> LiveSegmentsFromJson(const j::Value& v) {
  std::vector<LiveSegment> result;
  const auto* segments = v.GetArray("segments");
  if (!segments) return result;
  for (const auto& item : *segments) {
    if (!item.IsObject()) continue;
    const auto start = item.GetUInt("start_char");
    const auto end = item.GetUInt("end_char");
    const auto score = item.GetNumber("score");
    const auto surface = item.GetString("surface");
    if (!start || !end || *start > UINT32_MAX || *end > UINT32_MAX || *end < *start || !score ||
        !surface)
      continue;
    LiveSegment segment;
    segment.start_char = static_cast<uint32_t>(*start);
    segment.end_char = static_cast<uint32_t>(*end);
    segment.score = *score;
    segment.auto_punctuation = item.GetBool("auto_punctuation").value_or(false);
    segment.surface = *surface;
    segment.reading = item.GetString("reading").value_or("");
    if (segment.auto_punctuation && !segment.reading.empty()) continue;
    const auto read_byte = [&item](const char* field) -> uint8_t {
      const auto value = item.GetUInt(field).value_or(0);
      return value <= UINT8_MAX ? static_cast<uint8_t>(value) : 0;
    };
    segment.pos = read_byte("pos");
    segment.head_pos = read_byte("head_pos");
    segment.sem = read_byte("sem");
    segment.head_sem = read_byte("head_sem");
    result.push_back(std::move(segment));
  }
  return result;
}

}  // namespace

std::string BuildQueryCandidatesRequest(const QueryCandidatesRequest& p) {
  j::Object o;
  o.emplace("reading", j::Value(p.reading));
  o.emplace("left_context", j::Value(p.left_context));
  o.emplace("max_candidates", j::Value(static_cast<uint64_t>(p.max_candidates)));
  o.emplace("live", j::Value(p.live));
  o.emplace("auto_punctuation", j::Value(p.auto_punctuation));
  o.emplace("punctuation_style", j::Value(p.punctuation_style));
  if (!p.emoji_trigger.empty()) o.emplace("emoji_trigger", j::Value(p.emoji_trigger));
  o.emplace("secure", j::Value(p.secure));
  o.emplace("learning_allowed", j::Value(p.learning_allowed));
  AppToJson(p.app, o);
  if (!p.raw_romaji.empty()) o.emplace("raw_romaji", j::Value(p.raw_romaji));
  if (p.english_candidates) o.emplace("english_candidates", j::Value(true));
  return j::Stringify(j::Value(std::move(o)));
}

std::optional<QueryCandidatesRequest> ParseQueryCandidatesRequest(const std::string& json) {
  auto v = ParseObject(json);
  if (!v) return std::nullopt;
  QueryCandidatesRequest p;
  auto reading = v->GetString("reading");
  if (!reading) return std::nullopt;
  p.reading = std::move(*reading);
  p.left_context = v->GetString("left_context").value_or(std::string());
  if (auto m = v->GetUInt("max_candidates")) p.max_candidates = static_cast<uint32_t>(*m);
  p.live = v->GetBool("live").value_or(false);
  p.auto_punctuation = v->GetBool("auto_punctuation").value_or(false);
  p.punctuation_style = v->GetString("punctuation_style").value_or("ja");
  p.emoji_trigger = v->GetString("emoji_trigger").value_or(std::string());
  p.secure = v->GetBool("secure").value_or(true);
  p.learning_allowed = v->GetBool("learning_allowed").value_or(false);
  p.app = AppFromJson(*v);
  p.raw_romaji = v->GetString("raw_romaji").value_or(std::string());
  p.english_candidates = v->GetBool("english_candidates").value_or(false);
  return p;
}

std::string BuildQueryCandidatesResponse(const QueryCandidatesResponse& p) {
  j::Object o;
  if (!p.ok) o.emplace("ok", j::Value(false));
  if (p.error) o.emplace("error", j::Value(*p.error));
  j::Array arr;
  for (const auto& c : p.candidates) arr.push_back(CandidateToJson(c));
  o.emplace("candidates", j::Value(std::move(arr)));
  o.emplace("partial", j::Value(p.partial));
  if (!p.segments.empty()) o.emplace("segments", LiveSegmentsToJson(p.segments));
  // Omitted when empty so a response to a client that predates M35 keeps its
  // previous shape on the wire.
  if (!p.corrected_reading.empty()) {
    o.emplace("corrected_reading", j::Value(p.corrected_reading));
  }
  return j::Stringify(j::Value(std::move(o)));
}

std::optional<QueryCandidatesResponse> ParseQueryCandidatesResponse(const std::string& json) {
  auto v = ParseObject(json);
  if (!v) return std::nullopt;
  QueryCandidatesResponse p;
  p.ok = v->GetBool("ok").value_or(true);
  p.error = v->GetString("error");
  if (const auto* arr = v->GetArray("candidates")) {
    // Skip malformed entries (non-object, or missing surface/reading) instead of
    // failing the whole response. This lenient decode is the module-wide
    // convention for arrays of optional elements (see BatchSegmentFromJson and
    // the handshake capabilities list): a single bad candidate from the host
    // must not blank out the remaining valid ones, which for an IME would drop
    // the entire candidate list rather than one entry.
    for (const auto& e : *arr) {
      if (auto c = CandidateFromJson(e)) p.candidates.push_back(std::move(*c));
    }
  }
  p.partial = v->GetBool("partial").value_or(false);
  p.segments = LiveSegmentsFromJson(*v);
  // Absent for hosts that predate M35: decodes as "no correction applied".
  p.corrected_reading = v->GetString("corrected_reading").value_or(std::string());
  return p;
}

// -------- QueryLiveConversion --------

std::string BuildQueryLiveConversionRequest(const QueryLiveConversionRequest& p) {
  j::Object o;
  o.emplace("kana", j::Value(p.kana));
  o.emplace("context", j::Value(p.context));
  o.emplace("auto_punctuation", j::Value(p.auto_punctuation));
  o.emplace("punctuation_style", j::Value(p.punctuation_style));
  return j::Stringify(j::Value(std::move(o)));
}

std::optional<QueryLiveConversionRequest> ParseQueryLiveConversionRequest(const std::string& json) {
  auto v = ParseObject(json);
  if (!v) return std::nullopt;
  auto kana = v->GetString("kana");
  auto context = v->GetString("context");
  if (!kana || !context) return std::nullopt;
  QueryLiveConversionRequest p;
  p.kana = std::move(*kana);
  p.context = std::move(*context);
  // Absent for TIPs that predate M59 on this message: no punctuation.
  p.auto_punctuation = v->GetBool("auto_punctuation").value_or(false);
  p.punctuation_style = v->GetString("punctuation_style").value_or("ja");
  return p;
}

std::string BuildQueryLiveConversionResponse(const QueryLiveConversionResponse& p) {
  j::Object o;
  o.emplace("surface", j::Value(p.surface));
  o.emplace("confidence", j::Value(p.confidence));
  if (!p.segments.empty()) o.emplace("segments", LiveSegmentsToJson(p.segments));
  return j::Stringify(j::Value(std::move(o)));
}

std::optional<QueryLiveConversionResponse> ParseQueryLiveConversionResponse(
    const std::string& json) {
  auto v = ParseObject(json);
  if (!v) return std::nullopt;
  auto surface = v->GetString("surface");
  auto confidence = v->GetNumber("confidence");
  if (!surface || !confidence || !std::isfinite(*confidence) || *confidence < 0.0 ||
      *confidence > 1.0)
    return std::nullopt;
  QueryLiveConversionResponse p;
  p.surface = std::move(*surface);
  p.confidence = *confidence;
  p.segments = LiveSegmentsFromJson(*v);
  return p;
}

// -------- QueryPredictions --------

std::string BuildQueryPredictionsRequest(const QueryPredictionsRequest& p) {
  j::Object o;
  o.emplace("kana", j::Value(p.kana));
  o.emplace("leftSideContext", j::Value(p.left_side_context));
  o.emplace("mode", j::Value(p.mode));
  AppToJson(p.app, o);
  return j::Stringify(j::Value(std::move(o)));
}

std::optional<QueryPredictionsRequest> ParseQueryPredictionsRequest(const std::string& json) {
  auto v = ParseObject(json);
  if (!v) return std::nullopt;
  auto kana = v->GetString("kana");
  auto left_side_context = v->GetString("leftSideContext");
  auto mode = v->GetString("mode");
  if (!kana || !left_side_context || !mode) return std::nullopt;
  return QueryPredictionsRequest{std::move(*kana), std::move(*left_side_context), std::move(*mode),
                                 AppFromJson(*v)};
}

std::string BuildQueryPredictionsResponse(const QueryPredictionsResponse& p) {
  j::Object o;
  if (!p.ok) o.emplace("ok", j::Value(false));
  if (p.error) o.emplace("error", j::Value(*p.error));
  j::Array predictions;
  for (const auto& candidate : p.predictions) predictions.push_back(CandidateToJson(candidate));
  o.emplace("predictions", j::Value(std::move(predictions)));
  return j::Stringify(j::Value(std::move(o)));
}

std::optional<QueryPredictionsResponse> ParseQueryPredictionsResponse(const std::string& json) {
  auto v = ParseObject(json);
  if (!v) return std::nullopt;
  const auto* predictions = v->GetArray("predictions");
  if (!predictions) return std::nullopt;
  QueryPredictionsResponse p;
  p.ok = v->GetBool("ok").value_or(true);
  p.error = v->GetString("error");
  for (const auto& entry : *predictions) {
    if (auto candidate = CandidateFromJson(entry)) p.predictions.push_back(std::move(*candidate));
  }
  return p;
}

// -------- ReverseConvert --------

std::string BuildReverseConvertRequest(const ReverseConvertRequest& p) {
  j::Object o;
  o.emplace("surface", j::Value(p.surface));
  return j::Stringify(j::Value(std::move(o)));
}

std::optional<ReverseConvertRequest> ParseReverseConvertRequest(const std::string& json) {
  auto v = ParseObject(json);
  if (!v) return std::nullopt;
  auto surface = v->GetString("surface");
  if (!surface || surface->empty()) return std::nullopt;
  return ReverseConvertRequest{std::move(*surface)};
}

std::string BuildReverseConvertResponse(const ReverseConvertResponse& p) {
  j::Object o;
  o.emplace("reading", j::Value(p.reading));
  o.emplace("confidence", j::Value(p.confidence));
  return j::Stringify(j::Value(std::move(o)));
}

std::optional<ReverseConvertResponse> ParseReverseConvertResponse(const std::string& json) {
  auto v = ParseObject(json);
  if (!v) return std::nullopt;
  auto reading = v->GetString("reading");
  auto confidence = v->GetNumber("confidence");
  if (!reading || !confidence || !std::isfinite(*confidence) || *confidence < 0.0 ||
      *confidence > 1.0 || (reading->empty() != (*confidence == 0.0)))
    return std::nullopt;
  return ReverseConvertResponse{std::move(*reading), *confidence};
}

// -------- QueryBatchConversion --------

std::string BuildQueryBatchConversionRequest(const QueryBatchConversionRequest& p) {
  j::Object o;
  o.emplace("reading", j::Value(p.reading));
  if (!p.raw_romaji.empty()) o.emplace("raw_romaji", j::Value(p.raw_romaji));
  o.emplace("ai_allowed", p.ai_allowed);
  o.emplace("external_ai_allowed", p.external_ai_allowed);
  if (!p.ai_backend.empty()) o.emplace("ai_backend", p.ai_backend);
  o.emplace("mode", j::Value(p.mode));
  o.emplace("auto_punctuation", j::Value(p.auto_punctuation));
  o.emplace("max_candidates", j::Value(static_cast<uint64_t>(p.max_candidates)));
  return j::Stringify(j::Value(std::move(o)));
}

std::optional<QueryBatchConversionRequest> ParseQueryBatchConversionRequest(
    const std::string& json) {
  auto v = ParseObject(json);
  if (!v) return std::nullopt;
  QueryBatchConversionRequest p;
  auto reading = v->GetString("reading");
  if (!reading) return std::nullopt;
  p.reading = std::move(*reading);
  p.raw_romaji = v->GetString("raw_romaji").value_or(std::string());
  p.ai_allowed = v->GetBool("ai_allowed").value_or(false);
  p.external_ai_allowed = p.ai_allowed && v->GetBool("external_ai_allowed").value_or(false);
  p.ai_backend = v->GetString("ai_backend").value_or("");
  if (!p.ai_backend.empty() && p.ai_backend != "none" && p.ai_backend != "openai" &&
      p.ai_backend != "local-zenzai")
    return std::nullopt;
  p.mode = v->GetString("mode").value_or(std::string("neural"));
  p.auto_punctuation = v->GetBool("auto_punctuation").value_or(false);
  if (auto m = v->GetUInt("max_candidates")) p.max_candidates = static_cast<uint32_t>(*m);
  return p;
}

std::string BuildQueryBatchConversionResponse(const QueryBatchConversionResponse& p) {
  j::Object o;
  j::Array segments;
  for (const auto& segment : p.segments) segments.push_back(BatchSegmentToJson(segment));
  o.emplace("segments", j::Value(std::move(segments)));
  o.emplace("full_surface", j::Value(p.full_surface));
  o.emplace("partial", j::Value(p.partial));
  o.emplace("canceled", j::Value(p.canceled));
  if (p.error_class) o.emplace("error_class", j::Value(*p.error_class));
  return j::Stringify(j::Value(std::move(o)));
}

std::optional<QueryBatchConversionResponse> ParseQueryBatchConversionResponse(
    const std::string& json) {
  auto v = ParseObject(json);
  if (!v) return std::nullopt;
  QueryBatchConversionResponse p;
  if (const auto* arr = v->GetArray("segments")) {
    for (const auto& e : *arr) {
      if (auto segment = BatchSegmentFromJson(e)) p.segments.push_back(std::move(*segment));
    }
  }
  p.full_surface = v->GetString("full_surface").value_or(std::string());
  p.partial = v->GetBool("partial").value_or(false);
  p.canceled = v->GetBool("canceled").value_or(false);
  p.error_class = v->GetString("error_class");
  return p;
}

// -------- Cancel --------

std::string BuildCancel(const CancelPayload& p) {
  j::Object o;
  o.emplace("target_request_id", j::Value(p.target_request_id));
  return j::Stringify(j::Value(std::move(o)));
}

std::optional<CancelPayload> ParseCancel(const std::string& json) {
  auto v = ParseObject(json);
  if (!v) return std::nullopt;
  CancelPayload p;
  auto id = v->GetUInt("target_request_id");
  if (!id) return std::nullopt;
  p.target_request_id = *id;
  return p;
}

// -------- CommitObservation --------

std::string BuildCommitObservationRequest(const CommitObservationRequest& p) {
  j::Object o;
  o.emplace("reading", j::Value(p.reading));
  o.emplace("chosen", CandidateToJson(p.chosen));
  j::Array shown;
  for (const auto& c : p.shown) shown.push_back(CandidateToJson(c));
  o.emplace("shown", j::Value(std::move(shown)));
  o.emplace("left_context", j::Value(p.left_context));
  o.emplace("timestamp_ms", j::Value(p.timestamp_ms));
  o.emplace("observation_id", j::Value(p.observation_id));
  o.emplace("secure", j::Value(p.secure));
  o.emplace("learning_allowed", j::Value(p.learning_allowed));
  AppToJson(p.app, o);
  return j::Stringify(j::Value(std::move(o)));
}

std::optional<CommitObservationRequest> ParseCommitObservationRequest(const std::string& json) {
  auto v = ParseObject(json);
  if (!v) return std::nullopt;
  CommitObservationRequest p;
  auto reading = v->GetString("reading");
  if (!reading) return std::nullopt;
  p.reading = std::move(*reading);
  const auto* chosen = v->Find("chosen");
  if (!chosen) return std::nullopt;
  auto cand = CandidateFromJson(*chosen);
  if (!cand) return std::nullopt;
  p.chosen = std::move(*cand);
  if (const auto* shown = v->GetArray("shown")) {
    for (const auto& e : *shown) {
      if (auto c = CandidateFromJson(e)) p.shown.push_back(std::move(*c));
    }
  }
  p.left_context = v->GetString("left_context").value_or(std::string());
  p.timestamp_ms = v->GetUInt("timestamp_ms").value_or(0);
  // Absent for TIPs that predate DEV-554: parse as empty (no dedupe).
  p.observation_id = v->GetString("observation_id").value_or(std::string());
  p.secure = v->GetBool("secure").value_or(true);
  p.learning_allowed = v->GetBool("learning_allowed").value_or(false);
  // Absent for TIPs that predate DEV-1184: unknown app, recorded on the global row.
  p.app = AppFromJson(*v);
  return p;
}

std::string BuildCommitObservationResponse(const CommitObservationResponse& p) {
  j::Object o;
  o.emplace("ok", j::Value(p.ok));
  return j::Stringify(j::Value(std::move(o)));
}

std::string BuildCommitSegmentsObservationRequest(const CommitSegmentsObservationRequest& p) {
  j::Object object;
  j::Array segments;
  for (const auto& segment : p.segments) {
    j::Object item;
    item.emplace("reading", j::Value(segment.reading));
    item.emplace("chosen", CandidateToJson(segment.chosen));
    j::Array shown;
    for (const auto& candidate : segment.shown) shown.push_back(CandidateToJson(candidate));
    item.emplace("shown", j::Value(std::move(shown)));
    item.emplace("is_auto_punctuation", j::Value(segment.is_auto_punctuation));
    segments.emplace_back(std::move(item));
  }
  object.emplace("segments", j::Value(std::move(segments)));
  object.emplace("left_context", j::Value(p.left_context));
  object.emplace("timestamp_ms", j::Value(p.timestamp_ms));
  object.emplace("observation_id", j::Value(p.observation_id));
  object.emplace("secure", j::Value(p.secure));
  object.emplace("learning_allowed", j::Value(p.learning_allowed));
  AppToJson(p.app, object);
  return j::Stringify(j::Value(std::move(object)));
}

std::optional<CommitSegmentsObservationRequest> ParseCommitSegmentsObservationRequest(
    const std::string& json) {
  auto object = ParseObject(json);
  if (!object) return std::nullopt;
  const auto* segments = object->GetArray("segments");
  if (!segments || segments->empty()) return std::nullopt;
  CommitSegmentsObservationRequest request;
  for (const auto& item : *segments) {
    auto reading = item.GetString("reading");
    const auto* chosen = item.Find("chosen");
    if (!reading || !chosen) return std::nullopt;
    auto candidate = CandidateFromJson(*chosen);
    if (!candidate || candidate->surface.empty()) return std::nullopt;
    ObservedSegment segment;
    segment.reading = *reading;
    segment.chosen = std::move(*candidate);
    segment.is_auto_punctuation = item.GetBool("is_auto_punctuation").value_or(false);
    if (segment.reading.empty() && !segment.is_auto_punctuation) return std::nullopt;
    if (const auto* shown = item.GetArray("shown")) {
      for (const auto& value : *shown) {
        auto alternative = CandidateFromJson(value);
        if (!alternative) return std::nullopt;
        segment.shown.push_back(std::move(*alternative));
      }
    }
    request.segments.push_back(std::move(segment));
  }
  request.left_context = object->GetString("left_context").value_or("");
  request.timestamp_ms = object->GetUInt("timestamp_ms").value_or(0);
  request.observation_id = object->GetString("observation_id").value_or("");
  request.secure = object->GetBool("secure").value_or(true);
  request.learning_allowed = object->GetBool("learning_allowed").value_or(false);
  request.app = AppFromJson(*object);
  return request;
}

// -------- CommitCorrection --------

std::string BuildCommitCorrectionRequest(const CommitCorrectionRequest& p) {
  j::Object o;
  o.emplace("kind", j::Value(p.kind));
  o.emplace("reading", j::Value(p.reading));
  o.emplace("rejected_surface", j::Value(p.rejected_surface));
  if (p.selected_surface) o.emplace("selected_surface", j::Value(*p.selected_surface));
  o.emplace("left_context", j::Value(p.left_context));
  o.emplace("timestamp_ms", j::Value(p.timestamp_ms));
  o.emplace("observation_id", j::Value(p.observation_id));
  o.emplace("secure", j::Value(p.secure));
  o.emplace("learning_allowed", j::Value(p.learning_allowed));
  AppToJson(p.app, o);
  return j::Stringify(j::Value(std::move(o)));
}

std::optional<CommitCorrectionRequest> ParseCommitCorrectionRequest(const std::string& json) {
  auto v = ParseObject(json);
  if (!v) return std::nullopt;
  auto kind = v->GetString("kind");
  auto reading = v->GetString("reading");
  auto rejected = v->GetString("rejected_surface");
  if (!kind || !reading || !rejected || reading->empty() || rejected->empty()) return std::nullopt;
  CommitCorrectionRequest p;
  p.selected_surface = v->GetString("selected_surface");
  // The kind must agree with selected_surface, so a sender cannot ask for an
  // accept it did not mean (or lose one it did).
  if (*kind == kCorrectionKindUndo) {
    if (p.selected_surface) return std::nullopt;
  } else if (*kind == kCorrectionKindReconvert) {
    if (!p.selected_surface || p.selected_surface->empty() || *p.selected_surface == *rejected)
      return std::nullopt;
  } else {
    return std::nullopt;
  }
  p.kind = std::move(*kind);
  p.reading = std::move(*reading);
  p.rejected_surface = std::move(*rejected);
  p.left_context = v->GetString("left_context").value_or(std::string());
  p.timestamp_ms = v->GetUInt("timestamp_ms").value_or(0);
  p.observation_id = v->GetString("observation_id").value_or(std::string());
  p.secure = v->GetBool("secure").value_or(true);
  p.learning_allowed = v->GetBool("learning_allowed").value_or(false);
  p.app = AppFromJson(*v);
  return p;
}

std::optional<CommitObservationResponse> ParseCommitObservationResponse(const std::string& json) {
  auto v = ParseObject(json);
  if (!v) return std::nullopt;
  CommitObservationResponse p;
  auto ok = v->GetBool("ok");
  if (!ok) return std::nullopt;
  p.ok = *ok;
  return p;
}

// -------- AddUserWord / RemoveUserWord --------

std::string BuildAddUserWordRequest(const AddUserWordRequest& p) {
  j::Object o;
  o.emplace("word", j::Value(p.word));
  o.emplace("ruby", j::Value(p.ruby));
  if (p.cid) o.emplace("cid", j::Value(static_cast<int64_t>(*p.cid)));
  if (p.mid) o.emplace("mid", j::Value(static_cast<int64_t>(*p.mid)));
  if (p.value) o.emplace("value", j::Value(*p.value));
  return j::Stringify(j::Value(std::move(o)));
}

std::optional<AddUserWordRequest> ParseAddUserWordRequest(const std::string& json) {
  auto v = ParseObject(json);
  if (!v) return std::nullopt;
  AddUserWordRequest p;
  auto word = v->GetString("word");
  auto ruby = v->GetString("ruby");
  if (!word || !ruby) return std::nullopt;
  p.word = std::move(*word);
  p.ruby = std::move(*ruby);
  if (auto cid = v->GetInt("cid")) p.cid = static_cast<int32_t>(*cid);
  if (auto mid = v->GetInt("mid")) p.mid = static_cast<int32_t>(*mid);
  if (auto val = v->GetNumber("value")) p.value = *val;
  return p;
}

std::string BuildAddUserWordResponse(const AddUserWordResponse& p) {
  j::Object o;
  o.emplace("ok", j::Value(p.ok));
  if (p.generated_id) o.emplace("generated_id", j::Value(*p.generated_id));
  return j::Stringify(j::Value(std::move(o)));
}

std::optional<AddUserWordResponse> ParseAddUserWordResponse(const std::string& json) {
  auto v = ParseObject(json);
  if (!v) return std::nullopt;
  AddUserWordResponse p;
  auto ok = v->GetBool("ok");
  if (!ok) return std::nullopt;
  p.ok = *ok;
  if (auto id = v->GetString("generated_id")) p.generated_id = std::move(*id);
  return p;
}

std::string BuildRemoveUserWordRequest(const RemoveUserWordRequest& p) {
  j::Object o;
  o.emplace("word", j::Value(p.word));
  o.emplace("ruby", j::Value(p.ruby));
  return j::Stringify(j::Value(std::move(o)));
}

std::optional<RemoveUserWordRequest> ParseRemoveUserWordRequest(const std::string& json) {
  auto v = ParseObject(json);
  if (!v) return std::nullopt;
  RemoveUserWordRequest p;
  auto word = v->GetString("word");
  auto ruby = v->GetString("ruby");
  if (!word || !ruby) return std::nullopt;
  p.word = std::move(*word);
  p.ruby = std::move(*ruby);
  return p;
}

std::string BuildRemoveUserWordResponse(const RemoveUserWordResponse& p) {
  j::Object o;
  o.emplace("ok", j::Value(p.ok));
  return j::Stringify(j::Value(std::move(o)));
}

std::optional<RemoveUserWordResponse> ParseRemoveUserWordResponse(const std::string& json) {
  auto v = ParseObject(json);
  if (!v) return std::nullopt;
  RemoveUserWordResponse p;
  auto ok = v->GetBool("ok");
  if (!ok) return std::nullopt;
  p.ok = *ok;
  return p;
}

// -------- UpdateConfig --------

std::string BuildUpdateConfigResponse(const UpdateConfigResponse& p) {
  j::Object o;
  o.emplace("ok", j::Value(p.ok));
  if (p.error) o.emplace("error", j::Value(*p.error));
  return j::Stringify(j::Value(std::move(o)));
}

std::optional<UpdateConfigResponse> ParseUpdateConfigResponse(const std::string& json) {
  auto v = ParseObject(json);
  if (!v) return std::nullopt;
  UpdateConfigResponse p;
  auto ok = v->GetBool("ok");
  if (!ok) return std::nullopt;
  p.ok = *ok;
  if (auto error = v->GetString("error")) p.error = std::move(*error);
  return p;
}

// -------- ObserveTypo (M35) --------

std::string BuildObserveTypoRequest(const ObserveTypoRequest& p) {
  j::Object o;
  o.emplace("wrong_reading", j::Value(p.wrong_reading));
  o.emplace("correct_reading", j::Value(p.correct_reading));
  o.emplace("timestamp_ms", j::Value(p.timestamp_ms));
  o.emplace("secure", j::Value(p.secure));
  o.emplace("learning_allowed", j::Value(p.learning_allowed));
  return j::Stringify(j::Value(std::move(o)));
}

std::optional<ObserveTypoRequest> ParseObserveTypoRequest(const std::string& json) {
  auto v = ParseObject(json);
  if (!v) return std::nullopt;
  ObserveTypoRequest p;
  // Both readings are required: a half-specified pair has nothing to learn from
  // and must not be applied as "correct to empty".
  auto wrong = v->GetString("wrong_reading");
  auto correct = v->GetString("correct_reading");
  if (!wrong || !correct) return std::nullopt;
  p.wrong_reading = std::move(*wrong);
  p.correct_reading = std::move(*correct);
  p.timestamp_ms = v->GetUInt("timestamp_ms").value_or(0);
  p.secure = v->GetBool("secure").value_or(true);
  p.learning_allowed = v->GetBool("learning_allowed").value_or(false);
  return p;
}

std::string BuildObserveTypoResponse(const ObserveTypoResponse& p) {
  j::Object o;
  o.emplace("ok", j::Value(p.ok));
  return j::Stringify(j::Value(std::move(o)));
}

std::optional<ObserveTypoResponse> ParseObserveTypoResponse(const std::string& json) {
  auto v = ParseObject(json);
  if (!v) return std::nullopt;
  ObserveTypoResponse p;
  auto ok = v->GetBool("ok");
  if (!ok) return std::nullopt;
  p.ok = *ok;
  return p;
}

// -------- ListNewWordCandidates / ResolveNewWord (M36-A) --------

namespace {

bool IsKnownNewWordState(const std::string& value) {
  return value == "pending" || value == "confirmed" || value == "rejected";
}

j::Value NewWordToJson(const NewWordField& w) {
  j::Object o;
  o.emplace("surface", j::Value(w.surface));
  o.emplace("reading", j::Value(w.reading));
  o.emplace("source", j::Value(w.source));
  o.emplace("state", j::Value(w.state));
  o.emplace("count", j::Value(static_cast<uint64_t>(w.count)));
  o.emplace("last_seen_epoch", j::Value(w.last_seen_epoch));
  return j::Value(std::move(o));
}

std::optional<NewWordField> NewWordFromJson(const j::Value& v) {
  if (!v.IsObject()) return std::nullopt;
  NewWordField w;
  auto surface = v.GetString("surface");
  auto reading = v.GetString("reading");
  if (!surface || !reading) return std::nullopt;
  w.surface = std::move(*surface);
  w.reading = std::move(*reading);
  w.source = v.GetString("source").value_or(std::string());
  w.state = v.GetString("state").value_or(std::string());
  w.count = static_cast<uint32_t>(v.GetUInt("count").value_or(0));
  w.last_seen_epoch = v.GetUInt("last_seen_epoch").value_or(0);
  return w;
}

}  // namespace

std::string BuildListNewWordCandidatesRequest(const ListNewWordCandidatesRequest& p) {
  j::Object o;
  o.emplace("state_filter", j::Value(p.state_filter));
  o.emplace("max_items", j::Value(static_cast<uint64_t>(p.max_items)));
  return j::Stringify(j::Value(std::move(o)));
}

std::optional<ListNewWordCandidatesRequest> ParseListNewWordCandidatesRequest(
    const std::string& json) {
  auto v = ParseObject(json);
  if (!v) return std::nullopt;
  ListNewWordCandidatesRequest p;
  if (auto filter = v->GetString("state_filter")) {
    // An unrecognized filter is rejected rather than widened to "everything":
    // listing rejected words to a caller that asked for pending ones would put
    // words the user turned down back in front of them.
    if (!IsKnownNewWordState(*filter)) return std::nullopt;
    p.state_filter = std::move(*filter);
  }
  if (auto max_items = v->GetUInt("max_items")) {
    if (*max_items == 0 || *max_items > kMaxNewWordCandidates) return std::nullopt;
    p.max_items = static_cast<uint32_t>(*max_items);
  }
  return p;
}

std::string BuildListNewWordCandidatesResponse(const ListNewWordCandidatesResponse& p) {
  j::Object o;
  o.emplace("ok", j::Value(p.ok));
  if (p.error) o.emplace("error", j::Value(*p.error));
  j::Array items;
  for (const auto& w : p.items) items.push_back(NewWordToJson(w));
  o.emplace("items", j::Value(std::move(items)));
  return j::Stringify(j::Value(std::move(o)));
}

std::optional<ListNewWordCandidatesResponse> ParseListNewWordCandidatesResponse(
    const std::string& json) {
  auto v = ParseObject(json);
  if (!v) return std::nullopt;
  ListNewWordCandidatesResponse p;
  // A response without `ok` predates the error channel and was a success.
  p.ok = v->GetBool("ok").value_or(true);
  if (auto error = v->GetString("error")) p.error = std::move(*error);
  if (const auto* arr = v->GetArray("items")) {
    // Malformed entries are skipped, matching the module's lenient decode for
    // arrays of optional elements.
    for (const auto& e : *arr) {
      if (auto w = NewWordFromJson(e)) p.items.push_back(std::move(*w));
    }
  }
  return p;
}

std::string BuildResolveNewWordRequest(const ResolveNewWordRequest& p) {
  j::Object o;
  o.emplace("surface", j::Value(p.surface));
  o.emplace("reading", j::Value(p.reading));
  o.emplace("action", j::Value(p.action));
  return j::Stringify(j::Value(std::move(o)));
}

std::optional<ResolveNewWordRequest> ParseResolveNewWordRequest(const std::string& json) {
  auto v = ParseObject(json);
  if (!v) return std::nullopt;
  ResolveNewWordRequest p;
  auto surface = v->GetString("surface");
  auto reading = v->GetString("reading");
  auto action = v->GetString("action");
  if (!surface || !reading || !action) return std::nullopt;
  if (surface->empty() || reading->empty()) return std::nullopt;
  // An unknown action must not fall through to one of the two real outcomes.
  if (*action != "confirm" && *action != "reject") return std::nullopt;
  p.surface = std::move(*surface);
  p.reading = std::move(*reading);
  p.action = std::move(*action);
  return p;
}

std::string BuildResolveNewWordResponse(const ResolveNewWordResponse& p) {
  j::Object o;
  o.emplace("ok", j::Value(p.ok));
  o.emplace("changed", j::Value(p.changed));
  if (p.error) o.emplace("error", j::Value(*p.error));
  return j::Stringify(j::Value(std::move(o)));
}

std::optional<ResolveNewWordResponse> ParseResolveNewWordResponse(const std::string& json) {
  auto v = ParseObject(json);
  if (!v) return std::nullopt;
  ResolveNewWordResponse p;
  auto ok = v->GetBool("ok");
  if (!ok) return std::nullopt;
  p.ok = *ok;
  p.changed = v->GetBool("changed").value_or(false);
  if (auto error = v->GetString("error")) p.error = std::move(*error);
  return p;
}

// -------- ListModels / BenchmarkModel (DEV-1191) --------

namespace {

j::Value ListedModelToJson(const ListedModel& m) {
  j::Object o;
  o.emplace("path", j::Value(m.path));
  o.emplace("file_name", j::Value(m.file_name));
  o.emplace("format", j::Value(m.format));
  o.emplace("size_bytes", j::Value(m.size_bytes));
  o.emplace("valid", j::Value(m.valid));
  // Section 3.2: gguf_valid stays as the R1 alias of valid.
  if (m.format == "gguf") o.emplace("gguf_valid", j::Value(m.valid));
  j::Object metadata;
  if (!m.metadata.model_family.empty())
    metadata.emplace("model_family", j::Value(m.metadata.model_family));
  if (!m.metadata.quantization.empty())
    metadata.emplace("quantization", j::Value(m.metadata.quantization));
  if (m.metadata.n_params != 0) metadata.emplace("n_params", j::Value(m.metadata.n_params));
  o.emplace("metadata", j::Value(std::move(metadata)));
  if (!m.sha256.empty()) o.emplace("sha256", j::Value(m.sha256));
  o.emplace("last_load_status", j::Value(m.last_load_status));
  if (!m.last_error.empty()) o.emplace("last_error", j::Value(m.last_error));
  return j::Value(std::move(o));
}

std::optional<ListedModel> ListedModelFromJson(const j::Value& v) {
  if (!v.IsObject()) return std::nullopt;
  auto path = v.GetString("path");
  auto format = v.GetString("format");
  if (!path || !format) return std::nullopt;
  ListedModel m;
  m.path = std::move(*path);
  m.format = std::move(*format);
  m.file_name = v.GetString("file_name").value_or(std::string());
  m.size_bytes = v.GetUInt("size_bytes").value_or(0);
  m.valid = v.GetBool("valid").value_or(v.GetBool("gguf_valid").value_or(false));
  if (const auto* metadata = v.FindObject("metadata")) {
    const j::Value md(*metadata);
    m.metadata.model_family = md.GetString("model_family").value_or(std::string());
    m.metadata.quantization = md.GetString("quantization").value_or(std::string());
    m.metadata.n_params = md.GetUInt("n_params").value_or(0);
  }
  m.sha256 = v.GetString("sha256").value_or(std::string());
  m.last_load_status = v.GetString("last_load_status").value_or("not_loaded");
  m.last_error = v.GetString("last_error").value_or(std::string());
  return m;
}

}  // namespace

std::string BuildListModelsRequest(const ListModelsRequest& p) {
  j::Object o;
  if (!p.directory.empty()) o.emplace("directory", j::Value(p.directory));
  o.emplace("compute_sha256", j::Value(p.compute_sha256));
  return j::Stringify(j::Value(std::move(o)));
}

std::optional<ListModelsRequest> ParseListModelsRequest(const std::string& json) {
  auto v = ParseObject(json);
  if (!v) return std::nullopt;
  ListModelsRequest p;
  if (const auto* directory = v->Find("directory")) {
    if (!directory->IsString()) return std::nullopt;
    p.directory = directory->AsString();
  }
  p.compute_sha256 = v->GetBool("compute_sha256").value_or(false);
  return p;
}

std::string BuildListModelsResponse(const ListModelsResponse& p) {
  j::Object o;
  if (!p.ok) o.emplace("ok", j::Value(false));
  if (p.error) o.emplace("error", j::Value(*p.error));
  j::Array models;
  for (const auto& m : p.models) models.push_back(ListedModelToJson(m));
  o.emplace("models", j::Value(std::move(models)));
  return j::Stringify(j::Value(std::move(o)));
}

std::optional<ListModelsResponse> ParseListModelsResponse(const std::string& json) {
  auto v = ParseObject(json);
  if (!v) return std::nullopt;
  const auto* models = v->GetArray("models");
  if (!models) return std::nullopt;
  ListModelsResponse p;
  p.ok = v->GetBool("ok").value_or(true);
  if (auto error = v->GetString("error")) p.error = std::move(*error);
  for (const auto& entry : *models) {
    if (auto m = ListedModelFromJson(entry)) p.models.push_back(std::move(*m));
  }
  return p;
}

std::string BuildBenchmarkModelRequest(const BenchmarkModelRequest& p) {
  j::Object o;
  o.emplace("path", j::Value(p.path));
  o.emplace("backend", j::Value(p.backend));
  j::Array cases;
  for (const auto& c : p.cases) cases.emplace_back(c);
  o.emplace("cases", j::Value(std::move(cases)));
  o.emplace("iterations", j::Value(static_cast<uint64_t>(p.iterations)));
  o.emplace("warmup", j::Value(static_cast<uint64_t>(p.warmup)));
  return j::Stringify(j::Value(std::move(o)));
}

std::optional<BenchmarkModelRequest> ParseBenchmarkModelRequest(const std::string& json) {
  auto v = ParseObject(json);
  if (!v) return std::nullopt;
  auto path = v->GetString("path");
  if (!path || path->empty()) return std::nullopt;
  BenchmarkModelRequest p;
  p.path = std::move(*path);
  p.backend = v->GetString("backend").value_or("cpu");
  if (const auto* cases = v->Find("cases")) {
    if (!cases->IsArray()) return std::nullopt;
    for (const auto& c : cases->AsArray()) {
      if (!c.IsString()) return std::nullopt;
      p.cases.push_back(c.AsString());
    }
  }
  const auto iterations = v->GetUInt("iterations").value_or(p.iterations);
  const auto warmup = v->GetUInt("warmup").value_or(p.warmup);
  if (iterations > UINT32_MAX || warmup > UINT32_MAX) return std::nullopt;
  p.iterations = static_cast<uint32_t>(iterations);
  p.warmup = static_cast<uint32_t>(warmup);
  return p;
}

std::string BuildBenchmarkModelResponse(const BenchmarkModelResponse& p) {
  j::Object o;
  o.emplace("backend", j::Value(p.backend));
  o.emplace("p50_ms", j::Value(p.p50_ms));
  o.emplace("p95_ms", j::Value(p.p95_ms));
  o.emplace("p99_ms", j::Value(p.p99_ms));
  o.emplace("load_ms", j::Value(p.load_ms));
  o.emplace("rss_mb", j::Value(p.rss_mb));
  o.emplace("vram_mb", p.vram_mb ? j::Value(*p.vram_mb) : j::Value(j::Null{}));
  o.emplace("status", j::Value(p.status));
  o.emplace("iterations_completed", j::Value(static_cast<uint64_t>(p.iterations_completed)));
  o.emplace("error", p.error ? j::Value(*p.error) : j::Value(j::Null{}));
  return j::Stringify(j::Value(std::move(o)));
}

std::optional<BenchmarkModelResponse> ParseBenchmarkModelResponse(const std::string& json) {
  auto v = ParseObject(json);
  if (!v) return std::nullopt;
  auto status = v->GetString("status");
  if (!status) return std::nullopt;
  BenchmarkModelResponse p;
  p.status = std::move(*status);
  p.backend = v->GetString("backend").value_or(std::string());
  p.p50_ms = v->GetNumber("p50_ms").value_or(0.0);
  p.p95_ms = v->GetNumber("p95_ms").value_or(0.0);
  p.p99_ms = v->GetNumber("p99_ms").value_or(0.0);
  p.load_ms = v->GetNumber("load_ms").value_or(0.0);
  p.rss_mb = v->GetNumber("rss_mb").value_or(0.0);
  p.vram_mb = v->GetNumber("vram_mb");
  const auto completed = v->GetUInt("iterations_completed").value_or(0);
  p.iterations_completed = completed > UINT32_MAX ? 0 : static_cast<uint32_t>(completed);
  p.error = v->GetString("error");
  return p;
}

// -------- M49 learning data management (DEV-1190) --------

namespace {

// Request fields are strict: a field present with the wrong JSON type rejects
// the request instead of falling back to its default, so a malformed request
// never runs against a store or path the sender did not mean.
bool ReadOptionalString(const j::Value& v, std::string_view key, std::string& out) {
  const auto* field = v.Find(key);
  if (!field) return true;
  if (!field->IsString()) return false;
  out = field->AsString();
  return true;
}

bool ReadOptionalBool(const j::Value& v, std::string_view key, bool& out) {
  const auto* field = v.Find(key);
  if (!field) return true;
  if (!field->IsBool()) return false;
  out = field->AsBool();
  return true;
}

std::optional<std::string> ReadNonEmptyString(const j::Value& v, std::string_view key) {
  auto value = v.GetString(key);
  if (!value || value->empty()) return std::nullopt;
  return value;
}

// `stores`: a non-empty array of non-empty strings, at most
// kMaxLearningDataStores long. Store names are validated by the Host.
std::optional<std::vector<std::string>> ReadStoreList(const j::Value& v) {
  const auto* stores = v.GetArray("stores");
  if (!stores || stores->empty() || stores->size() > kMaxLearningDataStores) return std::nullopt;
  std::vector<std::string> out;
  out.reserve(stores->size());
  for (const auto& store : *stores) {
    if (!store.IsString() || store.AsString().empty()) return std::nullopt;
    out.push_back(store.AsString());
  }
  return out;
}

j::Value StoreListToJson(const std::vector<std::string>& stores) {
  j::Array out;
  for (const auto& store : stores) out.emplace_back(store);
  return j::Value(std::move(out));
}

j::Value LearningEntryToJson(const LearningEntryField& e) {
  j::Object o;
  o.emplace("id", j::Value(e.id));
  if (!e.channel.empty()) o.emplace("channel", j::Value(e.channel));
  o.emplace("reading", j::Value(e.reading));
  o.emplace("surface", j::Value(e.surface));
  o.emplace("weight", j::Value(e.weight));
  o.emplace("last_updated_epoch_sec", j::Value(e.last_updated_epoch_sec));
  j::Array tags;
  for (const auto& tag : e.tags) tags.emplace_back(tag);
  o.emplace("tags", j::Value(std::move(tags)));
  j::Object metadata;
  for (const auto& [key, value] : e.metadata) metadata.emplace(key, j::Value(value));
  o.emplace("metadata", j::Value(std::move(metadata)));
  return j::Value(std::move(o));
}

// An entry without id / reading / surface, or with more tags or metadata keys
// than the caps, is malformed and skipped by the caller. Non-string tags and
// metadata values are dropped individually.
std::optional<LearningEntryField> LearningEntryFromJson(const j::Value& v) {
  if (!v.IsObject()) return std::nullopt;
  auto id = v.GetString("id");
  auto reading = v.GetString("reading");
  auto surface = v.GetString("surface");
  if (!id || !reading || !surface) return std::nullopt;
  LearningEntryField e;
  e.id = std::move(*id);
  e.reading = std::move(*reading);
  e.surface = std::move(*surface);
  e.channel = v.GetString("channel").value_or(std::string());
  e.weight = v.GetNumber("weight").value_or(0.0);
  e.last_updated_epoch_sec = v.GetUInt("last_updated_epoch_sec").value_or(0);
  if (const auto* tags = v.GetArray("tags")) {
    if (tags->size() > kMaxLearningEntryTags) return std::nullopt;
    for (const auto& tag : *tags) {
      if (tag.IsString()) e.tags.push_back(tag.AsString());
    }
  }
  if (const auto* metadata = v.FindObject("metadata")) {
    if (metadata->size() > kMaxLearningEntryMetadata) return std::nullopt;
    for (const auto& entry : *metadata) {
      if (entry.second.IsString()) e.metadata.emplace(entry.first, entry.second.AsString());
    }
  }
  return e;
}

j::Value BackupItemToJson(const LearningBackupItemField& item) {
  j::Object o;
  o.emplace("name", j::Value(item.name));
  o.emplace("file", j::Value(item.file));
  o.emplace("count", j::Value(item.count));
  o.emplace("sha256", j::Value(item.sha256));
  return j::Value(std::move(o));
}

std::optional<LearningBackupItemField> BackupItemFromJson(const j::Value& v) {
  if (!v.IsObject()) return std::nullopt;
  auto name = v.GetString("name");
  if (!name || name->empty()) return std::nullopt;
  LearningBackupItemField item;
  item.name = std::move(*name);
  item.file = v.GetString("file").value_or(std::string());
  item.count = v.GetUInt("count").value_or(0);
  item.sha256 = v.GetString("sha256").value_or(std::string());
  return item;
}

j::Value CountsToJson(const std::map<std::string, uint64_t>& counts) {
  j::Object o;
  for (const auto& [name, count] : counts) o.emplace(name, j::Value(count));
  return j::Value(std::move(o));
}

// Returns false when the map has more keys than kMaxLearningDataStores, which
// rejects the response. Keys whose value is not an unsigned integer are dropped.
bool CountsFromJson(const j::Value& v, std::string_view key, std::map<std::string, uint64_t>& out) {
  const auto* counts = v.FindObject(key);
  if (!counts) return true;
  if (counts->size() > kMaxLearningDataStores) return false;
  const j::Value wrapped(*counts);
  for (const auto& entry : *counts) {
    if (auto count = wrapped.GetUInt(entry.first)) out.emplace(entry.first, *count);
  }
  return true;
}

bool IsKnownConflictResolution(const std::string& value) {
  return value == "merge" || value == "overwrite" || value == "keep_both";
}

}  // namespace

std::string BuildListLearningEntriesRequest(const ListLearningEntriesRequest& p) {
  j::Object o;
  o.emplace("store", j::Value(p.store));
  o.emplace("query", j::Value(p.query));
  o.emplace("limit", j::Value(static_cast<uint64_t>(p.limit)));
  o.emplace("offset", j::Value(static_cast<uint64_t>(p.offset)));
  return j::Stringify(j::Value(std::move(o)));
}

std::optional<ListLearningEntriesRequest> ParseListLearningEntriesRequest(const std::string& json) {
  auto v = ParseObject(json);
  if (!v) return std::nullopt;
  auto store = ReadNonEmptyString(*v, "store");
  if (!store) return std::nullopt;
  ListLearningEntriesRequest p;
  p.store = std::move(*store);
  if (!ReadOptionalString(*v, "query", p.query)) return std::nullopt;
  if (v->Find("limit")) {
    const auto limit = v->GetUInt("limit");
    if (!limit) return std::nullopt;
    // Section 4.1: the page size is capped, not rejected, above the ceiling.
    p.limit = static_cast<uint32_t>(std::min<uint64_t>(*limit, kMaxLearningEntries));
  }
  if (v->Find("offset")) {
    const auto offset = v->GetUInt("offset");
    if (!offset || *offset > UINT32_MAX) return std::nullopt;
    p.offset = static_cast<uint32_t>(*offset);
  }
  return p;
}

std::string BuildListLearningEntriesResponse(const ListLearningEntriesResponse& p) {
  j::Object o;
  if (!p.ok) o.emplace("ok", j::Value(false));
  if (p.error) o.emplace("error", j::Value(*p.error));
  o.emplace("total", j::Value(p.total));
  j::Array entries;
  for (const auto& e : p.entries) entries.push_back(LearningEntryToJson(e));
  o.emplace("entries", j::Value(std::move(entries)));
  return j::Stringify(j::Value(std::move(o)));
}

std::optional<ListLearningEntriesResponse> ParseListLearningEntriesResponse(
    const std::string& json) {
  auto v = ParseObject(json);
  if (!v) return std::nullopt;
  ListLearningEntriesResponse p;
  p.ok = v->GetBool("ok").value_or(true);
  p.error = v->GetString("error");
  p.total = v->GetUInt("total").value_or(0);
  if (const auto* entries = v->GetArray("entries")) {
    if (entries->size() > kMaxLearningEntries) return std::nullopt;
    // Malformed entries are skipped, matching the module's lenient decode for
    // arrays of optional elements.
    for (const auto& entry : *entries) {
      if (auto e = LearningEntryFromJson(entry)) p.entries.push_back(std::move(*e));
    }
  }
  return p;
}

std::string BuildForgetLearningEntryRequest(const ForgetLearningEntryRequest& p) {
  j::Object o;
  o.emplace("store", j::Value(p.store));
  if (!p.id.empty()) o.emplace("id", j::Value(p.id));
  if (!p.reading.empty()) o.emplace("reading", j::Value(p.reading));
  if (!p.surface.empty()) o.emplace("surface", j::Value(p.surface));
  return j::Stringify(j::Value(std::move(o)));
}

std::optional<ForgetLearningEntryRequest> ParseForgetLearningEntryRequest(const std::string& json) {
  auto v = ParseObject(json);
  if (!v) return std::nullopt;
  auto store = ReadNonEmptyString(*v, "store");
  if (!store) return std::nullopt;
  ForgetLearningEntryRequest p;
  p.store = std::move(*store);
  if (!ReadOptionalString(*v, "id", p.id) || !ReadOptionalString(*v, "reading", p.reading) ||
      !ReadOptionalString(*v, "surface", p.surface))
    return std::nullopt;
  // Exactly one form: an id, or a (reading, surface) pair. A request carrying
  // both, neither, or half a pair must not pick an entry by guessing.
  const bool by_id = !p.id.empty();
  const bool by_pair = !p.reading.empty() || !p.surface.empty();
  if (by_id == by_pair) return std::nullopt;
  if (by_pair && (p.reading.empty() || p.surface.empty() || p.store != "learning"))
    return std::nullopt;
  return p;
}

std::string BuildForgetLearningEntryResponse(const ForgetLearningEntryResponse& p) {
  j::Object o;
  if (!p.ok) o.emplace("ok", j::Value(false));
  o.emplace("removed", j::Value(p.removed));
  if (p.error) o.emplace("error", j::Value(*p.error));
  return j::Stringify(j::Value(std::move(o)));
}

std::optional<ForgetLearningEntryResponse> ParseForgetLearningEntryResponse(
    const std::string& json) {
  auto v = ParseObject(json);
  if (!v) return std::nullopt;
  ForgetLearningEntryResponse p;
  p.ok = v->GetBool("ok").value_or(true);
  p.removed = v->GetBool("removed").value_or(false);
  p.error = v->GetString("error");
  return p;
}

std::string BuildExportLearningDataRequest(const ExportLearningDataRequest& p) {
  j::Object o;
  o.emplace("stores", StoreListToJson(p.stores));
  o.emplace("destination_path", j::Value(p.destination_path));
  o.emplace("encrypt", j::Value(p.encrypt));
  o.emplace("include_settings", j::Value(p.include_settings));
  return j::Stringify(j::Value(std::move(o)));
}

std::optional<ExportLearningDataRequest> ParseExportLearningDataRequest(const std::string& json) {
  auto v = ParseObject(json);
  if (!v) return std::nullopt;
  auto stores = ReadStoreList(*v);
  auto destination_path = ReadNonEmptyString(*v, "destination_path");
  if (!stores || !destination_path) return std::nullopt;
  ExportLearningDataRequest p;
  p.stores = std::move(*stores);
  p.destination_path = std::move(*destination_path);
  // include_settings=true is decoded as sent; the Host answers it with
  // kLearningDataErrorUnsupported rather than the codec dropping the request.
  if (!ReadOptionalBool(*v, "encrypt", p.encrypt) ||
      !ReadOptionalBool(*v, "include_settings", p.include_settings))
    return std::nullopt;
  return p;
}

std::string BuildExportLearningDataResponse(const ExportLearningDataResponse& p) {
  j::Object o;
  o.emplace("status", j::Value(p.status));
  if (p.error) o.emplace("error", j::Value(*p.error));
  o.emplace("file_size_bytes", j::Value(p.file_size_bytes));
  o.emplace("encrypted", j::Value(p.encrypted));
  j::Array items;
  for (const auto& item : p.items) items.push_back(BackupItemToJson(item));
  o.emplace("items", j::Value(std::move(items)));
  return j::Stringify(j::Value(std::move(o)));
}

std::optional<ExportLearningDataResponse> ParseExportLearningDataResponse(const std::string& json) {
  auto v = ParseObject(json);
  if (!v) return std::nullopt;
  auto status = v->GetString("status");
  if (!status) return std::nullopt;
  ExportLearningDataResponse p;
  p.status = std::move(*status);
  p.error = v->GetString("error");
  p.file_size_bytes = v->GetUInt("file_size_bytes").value_or(0);
  p.encrypted = v->GetBool("encrypted").value_or(true);
  if (const auto* items = v->GetArray("items")) {
    if (items->size() > kMaxLearningDataStores) return std::nullopt;
    for (const auto& entry : *items) {
      if (auto item = BackupItemFromJson(entry)) p.items.push_back(std::move(*item));
    }
  }
  return p;
}

std::string BuildImportLearningDataRequest(const ImportLearningDataRequest& p) {
  j::Object o;
  o.emplace("source_path", j::Value(p.source_path));
  o.emplace("conflict_resolution", j::Value(p.conflict_resolution));
  o.emplace("stores", StoreListToJson(p.stores));
  return j::Stringify(j::Value(std::move(o)));
}

std::optional<ImportLearningDataRequest> ParseImportLearningDataRequest(const std::string& json) {
  auto v = ParseObject(json);
  if (!v) return std::nullopt;
  auto source_path = ReadNonEmptyString(*v, "source_path");
  auto stores = ReadStoreList(*v);
  if (!source_path || !stores) return std::nullopt;
  ImportLearningDataRequest p;
  p.source_path = std::move(*source_path);
  p.stores = std::move(*stores);
  if (!ReadOptionalString(*v, "conflict_resolution", p.conflict_resolution)) return std::nullopt;
  // An unknown policy must not fall through to one of the three real ones.
  if (!IsKnownConflictResolution(p.conflict_resolution)) return std::nullopt;
  return p;
}

std::string BuildImportLearningDataResponse(const ImportLearningDataResponse& p) {
  j::Object o;
  o.emplace("status", j::Value(p.status));
  if (p.error) o.emplace("error", j::Value(*p.error));
  o.emplace("imported_counts", CountsToJson(p.imported_counts));
  o.emplace("skipped_counts", CountsToJson(p.skipped_counts));
  o.emplace("conflict_counts", CountsToJson(p.conflict_counts));
  return j::Stringify(j::Value(std::move(o)));
}

std::optional<ImportLearningDataResponse> ParseImportLearningDataResponse(const std::string& json) {
  auto v = ParseObject(json);
  if (!v) return std::nullopt;
  auto status = v->GetString("status");
  if (!status) return std::nullopt;
  ImportLearningDataResponse p;
  p.status = std::move(*status);
  p.error = v->GetString("error");
  if (!CountsFromJson(*v, "imported_counts", p.imported_counts) ||
      !CountsFromJson(*v, "skipped_counts", p.skipped_counts) ||
      !CountsFromJson(*v, "conflict_counts", p.conflict_counts))
    return std::nullopt;
  return p;
}

// -------- ResetLearningStore --------

std::string BuildResetLearningStoreRequest(const ResetLearningStoreRequest& p) {
  j::Object o;
  o.emplace("store", j::Value(p.store));
  return j::Stringify(j::Value(std::move(o)));
}

std::optional<ResetLearningStoreRequest> ParseResetLearningStoreRequest(const std::string& json) {
  auto v = ParseObject(json);
  if (!v) return std::nullopt;
  auto store = ReadNonEmptyString(*v, "store");
  if (!store) return std::nullopt;
  ResetLearningStoreRequest p;
  p.store = std::move(*store);
  return p;
}

std::string BuildResetLearningStoreResponse(const ResetLearningStoreResponse& p) {
  j::Object o;
  o.emplace("ok", j::Value(p.ok));
  if (p.error) o.emplace("error", j::Value(*p.error));
  return j::Stringify(j::Value(std::move(o)));
}

std::optional<ResetLearningStoreResponse> ParseResetLearningStoreResponse(const std::string& json) {
  auto v = ParseObject(json);
  if (!v) return std::nullopt;
  auto ok = v->GetBool("ok");
  if (!ok) return std::nullopt;
  ResetLearningStoreResponse p;
  p.ok = *ok;
  p.error = v->GetString("error");
  return p;
}

// -------- QueryPersona --------

std::string BuildQueryPersonaResponse(const QueryPersonaResponse& p) {
  j::Object o;
  o.emplace("ok", j::Value(p.ok));
  if (p.error) o.emplace("error", j::Value(*p.error));
  o.emplace("polite_ratio", j::Value(p.polite_ratio));
  o.emplace("casual_ratio", j::Value(p.casual_ratio));
  o.emplace("technical_ratio", j::Value(p.technical_ratio));
  o.emplace("kaomoji_ratio", j::Value(p.kaomoji_ratio));
  o.emplace("sample_count", j::Value(p.sample_count));
  o.emplace("computed_at_epoch_sec", j::Value(p.computed_at_epoch_sec));
  return j::Stringify(j::Value(std::move(o)));
}

std::optional<QueryPersonaResponse> ParseQueryPersonaResponse(const std::string& json) {
  auto v = ParseObject(json);
  if (!v) return std::nullopt;
  auto ok = v->GetBool("ok");
  if (!ok) return std::nullopt;
  QueryPersonaResponse p;
  p.ok = *ok;
  p.error = v->GetString("error");
  // A ratio outside [0, 1] is not a ratio; reject rather than display it.
  for (auto [key, out] :
       {std::pair{"polite_ratio", &p.polite_ratio}, std::pair{"casual_ratio", &p.casual_ratio},
        std::pair{"technical_ratio", &p.technical_ratio},
        std::pair{"kaomoji_ratio", &p.kaomoji_ratio}}) {
    const double ratio = v->GetNumber(key).value_or(0.0);
    if (!(ratio >= 0.0 && ratio <= 1.0)) return std::nullopt;
    *out = ratio;
  }
  p.sample_count = v->GetUInt("sample_count").value_or(0);
  p.computed_at_epoch_sec = v->GetUInt("computed_at_epoch_sec").value_or(0);
  return p;
}

// -------- DetectAnomalies --------

std::string BuildDetectAnomaliesRequest(const DetectAnomaliesRequest& p) {
  j::Object o;
  o.emplace("text", j::Value(p.text));
  o.emplace("max_findings", j::Value(static_cast<uint64_t>(p.max_findings)));
  o.emplace("secure", j::Value(p.secure));
  o.emplace("learning_allowed", j::Value(p.learning_allowed));
  return j::Stringify(j::Value(std::move(o)));
}

std::optional<DetectAnomaliesRequest> ParseDetectAnomaliesRequest(const std::string& json) {
  auto v = ParseObject(json);
  if (!v) return std::nullopt;
  auto text = v->GetString("text");
  if (!text || text->empty() || text->size() > kMaxAnomalyTextBytes) return std::nullopt;
  DetectAnomaliesRequest p;
  p.text = std::move(*text);
  const auto max_findings = v->GetUInt("max_findings").value_or(kDefaultAnomalyFindings);
  p.max_findings = static_cast<uint32_t>(
      std::clamp<uint64_t>(max_findings, 1, static_cast<uint64_t>(kMaxAnomalyFindings)));
  p.secure = v->GetBool("secure").value_or(true);
  p.learning_allowed = v->GetBool("learning_allowed").value_or(false);
  return p;
}

std::string BuildDetectAnomaliesResponse(const DetectAnomaliesResponse& p) {
  j::Object o;
  o.emplace("ok", j::Value(p.ok));
  if (p.error) o.emplace("error", j::Value(*p.error));
  j::Array findings;
  for (const auto& finding : p.findings) {
    j::Array suggestions;
    for (const auto& suggestion : finding.suggestions) suggestions.emplace_back(suggestion);
    j::Object item;
    item.emplace("start", j::Value(static_cast<uint64_t>(finding.start)));
    item.emplace("length", j::Value(static_cast<uint64_t>(finding.length)));
    item.emplace("reason", j::Value(finding.reason));
    item.emplace("suggestions", j::Value(std::move(suggestions)));
    item.emplace("confidence", j::Value(finding.confidence));
    findings.emplace_back(std::move(item));
  }
  o.emplace("findings", j::Value(std::move(findings)));
  return j::Stringify(j::Value(std::move(o)));
}

std::optional<DetectAnomaliesResponse> ParseDetectAnomaliesResponse(const std::string& json) {
  auto v = ParseObject(json);
  if (!v) return std::nullopt;
  auto ok = v->GetBool("ok");
  if (!ok) return std::nullopt;
  DetectAnomaliesResponse p;
  p.ok = *ok;
  p.error = v->GetString("error");
  const auto* findings = v->GetArray("findings");
  if (!findings) return p;
  if (findings->size() > kMaxAnomalyFindings) return std::nullopt;
  for (const auto& item : *findings) {
    const auto start = item.GetUInt("start");
    const auto length = item.GetUInt("length");
    const auto confidence = item.GetNumber("confidence");
    // A finding that cannot be placed or rated is dropped, as malformed
    // candidates are.
    if (!item.IsObject() || !start || !length || *length == 0 || *start > UINT32_MAX ||
        *length > UINT32_MAX - *start || !confidence || !(*confidence >= 0.0 && *confidence <= 1.0))
      continue;
    AnomalyFindingField finding;
    finding.start = static_cast<uint32_t>(*start);
    finding.length = static_cast<uint32_t>(*length);
    finding.reason = item.GetString("reason").value_or(std::string());
    finding.confidence = *confidence;
    if (const auto* suggestions = item.GetArray("suggestions")) {
      for (const auto& suggestion : *suggestions) {
        if (finding.suggestions.size() >= kMaxAnomalySuggestions) break;
        if (suggestion.IsString()) finding.suggestions.push_back(suggestion.AsString());
      }
    }
    p.findings.push_back(std::move(finding));
  }
  return p;
}

}  // namespace azookey::ipc
