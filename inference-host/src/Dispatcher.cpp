#include "azookey/host/Dispatcher.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string_view>

#include "azookey/host/PunctuationInserter.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
// Psapi.h requires the Windows SDK types declared by Windows.h.
// clang-format off
#include <Windows.h>
#include <Psapi.h>
// clang-format on
#endif

#include "azookey/core/BatchConversionChunker.h"
#include "azookey/core/CrashReporting.h"
#include "azookey/core/EtwLogger.h"
#include "azookey/ipc/Payloads.h"

namespace azookey::host {

namespace {

core::EtwGuid ClientGuid(std::string_view value) {
  core::EtwGuid result{};
  if (value.size() == 38 && value.front() == '{' && value.back() == '}')
    value = value.substr(1, 36);
  if (value.size() != 36) return result;
  const auto hex = [](char ch) -> int {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
  };
  // GUID uses little endian Data1/Data2/Data3, then eight byte-sized fields.
  constexpr size_t positions[]{6, 4, 2, 0, 11, 9, 16, 14, 19, 21, 24, 26, 28, 30, 32, 34};
  if (value[8] != '-' || value[13] != '-' || value[18] != '-' || value[23] != '-') return result;
  for (size_t index = 0; index < result.size(); ++index) {
    const int high = hex(value[positions[index]]);
    const int low = hex(value[positions[index] + 1]);
    if (high < 0 || low < 0) return {};
    result[index] = static_cast<uint8_t>(high * 16 + low);
  }
  return result;
}

class InferenceTrace {
 public:
  InferenceTrace(uint64_t request, std::string_view client, core::EtwBackend backend,
                 uint64_t length)
      : context_{request, ClientGuid(client)} {
    core::EtwLogger::LogInferenceStart(request, backend, length, context_.client);
  }
  ~InferenceTrace() {
    if (result_ == core::EtwResult::Failed && cancel_ && cancel_->load(std::memory_order_acquire))
      result_ = core::EtwResult::Cancelled;
    core::EtwLogger::LogInferenceEnd(
        context_.request_id, candidates_,
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_)
            .count(),
        result_, context_.client);
  }
  void Finish(core::EtwResult result, uint64_t candidates = 0) {
    result_ = result;
    candidates_ = candidates;
  }
  const InferenceTelemetry* context() const { return &context_; }
  void WatchCancellation(std::shared_ptr<std::atomic<bool>> cancel) { cancel_ = std::move(cancel); }

 private:
  InferenceTelemetry context_;
  std::chrono::steady_clock::time_point start_{std::chrono::steady_clock::now()};
  core::EtwResult result_{core::EtwResult::Failed};
  uint64_t candidates_{};
  std::shared_ptr<std::atomic<bool>> cancel_;
};

uint64_t NowMs() {
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

uint64_t NowSec() {
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
}

uint64_t CurrentRssMb() {
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS counters{};
  counters.cb = sizeof(counters);
  if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
    return static_cast<uint64_t>(counters.WorkingSetSize / (1024ULL * 1024ULL));
  }
#endif
  return 0;
}

ipc::Envelope MakeResponse(const ipc::Envelope& req, std::string payload_json) {
  ipc::Envelope r;
  r.version = req.version;
  r.request_id = req.request_id;
  r.trace_id = req.trace_id;
  r.type = req.type;
  r.payload_json = std::move(payload_json);
  return r;
}

const char* SourceToWire(core::CandidateSource source) {
  if (source == core::CandidateSource::SystemDictionary) return "system";
  if (source == core::CandidateSource::UserDictionary) return "user_dict";
  if (source == core::CandidateSource::Model) return "model";
  if (source == core::CandidateSource::Llm) return "llm";
  if (source == core::CandidateSource::Learning) return "learning";
  if (source == core::CandidateSource::Symbol) return "symbol";
  if (source == core::CandidateSource::Emoji) return "emoji";
  return "heuristic";
}

ipc::CandidateField ToField(const core::Candidate& c) {
  ipc::CandidateField f;
  f.surface = c.surface;
  f.reading = c.reading;
  f.score = c.score;
  f.source = SourceToWire(c.source);
  f.description = c.description;
  return f;
}

std::optional<BackendKind> ParseBackend(const std::string& backend) {
  if (backend.empty() || backend == "cpu") return BackendKind::Cpu;
  if (backend == "cuda") return BackendKind::Cuda;
  if (backend == "vulkan") return BackendKind::Vulkan;
  return std::nullopt;
}

bool ConstantTimeEquals(std::string_view left, std::string_view right) {
  const size_t compared_size = std::max(left.size(), right.size());
  volatile size_t difference = left.size() ^ right.size();
  for (size_t i = 0; i < compared_size; ++i) {
    const auto left_byte = i < left.size() ? static_cast<unsigned char>(left[i]) : 0;
    const auto right_byte = i < right.size() ? static_cast<unsigned char>(right[i]) : 0;
    difference = difference | (left_byte ^ right_byte);
  }
  return difference == 0;
}

class RequestCompletionGuard {
 public:
  RequestCompletionGuard(RequestScheduler* scheduler, std::string client_id, uint64_t request_id)
      : scheduler_(scheduler), client_id_(std::move(client_id)), request_id_(request_id) {}

  ~RequestCompletionGuard() { Complete(); }

  void Complete() {
    if (active_) {
      scheduler_->CompleteRequest(client_id_, request_id_);
      active_ = false;
    }
  }

 private:
  RequestScheduler* scheduler_;
  std::string client_id_;
  uint64_t request_id_;
  bool active_{true};
};

}  // namespace

Dispatcher::Dispatcher(InferenceEngine* engine, RequestScheduler* scheduler,
                       learning::UserDictionary* user_dict, DispatcherConfig config,
                       SettingsStore* settings_store, learning::AutoWordStore* auto_word_store)
    : engine_(engine),
      scheduler_(scheduler),
      user_dict_(user_dict),
      settings_store_(settings_store),
      auto_word_store_(auto_word_store),
      config_(std::move(config)) {
  if (!config_.update_config_mutex) {
    config_.update_config_mutex = std::make_shared<std::mutex>();
  }
}

Dispatcher::~Dispatcher() { SetClientId({}); }

std::optional<ipc::Envelope> Dispatcher::Dispatch(const ipc::Envelope& req) {
  if (req.type != ipc::MessageType::Handshake && RequiresAuthenticatedSession()) {
    return HandleUnauthenticated(req);
  }
  switch (req.type) {
    case ipc::MessageType::Handshake:
      return HandleHandshake(req);
    case ipc::MessageType::Ping:
      return HandlePing(req);
    case ipc::MessageType::Health:
      return HandleHealth(req);
    case ipc::MessageType::QueryDiagnostics:
      return HandleQueryDiagnostics(req);
    case ipc::MessageType::LoadModel:
      return HandleLoadModel(req);
    case ipc::MessageType::QueryCandidates:
      return HandleQueryCandidates(req);
    case ipc::MessageType::QueryBatchConversion:
      return HandleQueryBatchConversion(req);
    case ipc::MessageType::Cancel:
      HandleCancel(req);
      return std::nullopt;
    case ipc::MessageType::CommitObservation:
      return HandleCommitObservation(req);
    case ipc::MessageType::CommitSegmentsObservation: {
      auto privacy =
          settings_store_ ? std::optional{settings_store_->LockPrivacyPolicy()} : std::nullopt;
      ipc::CommitObservationResponse response;
      if (auto parsed = ipc::ParseCommitSegmentsObservationRequest(req.payload_json);
          parsed && LearningAllowed(
                        parsed->secure, parsed->learning_allowed,
                        privacy && (privacy->policy.secure || !privacy->policy.learning_allowed))) {
        engine_->CommitSegmentsObservation(*parsed, NowSec());
        response.ok = true;
      }
      return MakeResponse(req, ipc::BuildCommitObservationResponse(response));
    }
    case ipc::MessageType::AddUserWord:
      return HandleAddUserWord(req);
    case ipc::MessageType::RemoveUserWord:
      return HandleRemoveUserWord(req);
    case ipc::MessageType::UpdateConfig:
      return HandleUpdateConfig(req);
    case ipc::MessageType::ObserveTypo:
      // Fire-and-forget (spec section 7): no reply is sent at all.
      HandleObserveTypo(req);
      return std::nullopt;
    case ipc::MessageType::ListNewWordCandidates:
      return HandleListNewWordCandidates(req);
    case ipc::MessageType::ResolveNewWord:
      return HandleResolveNewWord(req);
    default:
      // Preserve the request type in the response envelope so future clients can
      // correlate the protocol error without waiting for a timeout.
      return MakeResponse(req, R"({"ok":false,"error":"unsupported_message_type"})");
  }
}

std::optional<ipc::Envelope> Dispatcher::HandleUnauthenticated(const ipc::Envelope& req) {
  // Return a type-appropriate error response so blocking clients do not hang
  // waiting for a reply that would never come if we returned nullopt.
  switch (req.type) {
    case ipc::MessageType::AddUserWord: {
      ipc::AddUserWordResponse r;
      r.ok = false;
      return MakeResponse(req, ipc::BuildAddUserWordResponse(r));
    }
    case ipc::MessageType::RemoveUserWord: {
      ipc::RemoveUserWordResponse r;
      r.ok = false;
      return MakeResponse(req, ipc::BuildRemoveUserWordResponse(r));
    }
    case ipc::MessageType::UpdateConfig: {
      ipc::UpdateConfigResponse r;
      r.ok = false;
      r.error = "not authenticated";
      return MakeResponse(req, ipc::BuildUpdateConfigResponse(r));
    }
    case ipc::MessageType::CommitSegmentsObservation:
    case ipc::MessageType::CommitObservation: {
      ipc::CommitObservationResponse r;
      r.ok = false;
      return MakeResponse(req, ipc::BuildCommitObservationResponse(r));
    }
    case ipc::MessageType::ObserveTypo:
      // Fire-and-forget either way: dropping it silently is the same shape an
      // authenticated session gives, and nothing is recorded.
      return std::nullopt;
    case ipc::MessageType::ListNewWordCandidates: {
      // No items rather than the store's contents: the approval list is built
      // from what the user typed.
      ipc::ListNewWordCandidatesResponse r;
      r.ok = false;
      r.error = std::string(ipc::kNewWordErrorNotAuthenticated);
      return MakeResponse(req, ipc::BuildListNewWordCandidatesResponse(r));
    }
    case ipc::MessageType::ResolveNewWord: {
      ipc::ResolveNewWordResponse r;
      r.ok = false;
      r.error = std::string(ipc::kNewWordErrorNotAuthenticated);
      return MakeResponse(req, ipc::BuildResolveNewWordResponse(r));
    }
    case ipc::MessageType::LoadModel: {
      ipc::LoadModelResponse r;
      r.ok = false;
      r.error = "not authenticated";
      return MakeResponse(req, ipc::BuildLoadModelResponse(r));
    }
    case ipc::MessageType::QueryCandidates: {
      ipc::QueryCandidatesResponse r;
      r.partial = false;
      return MakeResponse(req, ipc::BuildQueryCandidatesResponse(r));
    }
    case ipc::MessageType::QueryBatchConversion: {
      ipc::QueryBatchConversionResponse r;
      if (auto parsed = ipc::ParseQueryBatchConversionRequest(req.payload_json)) {
        r.full_surface = parsed->reading;
      }
      r.partial = false;
      r.canceled = false;
      return MakeResponse(req, ipc::BuildQueryBatchConversionResponse(r));
    }
    case ipc::MessageType::Ping: {
      ipc::PingPayload p;
      p.nonce = 0;
      p.t_ms = 0;
      return MakeResponse(req, ipc::BuildPing(p));
    }
    case ipc::MessageType::Health: {
      ipc::HealthPayload p;
      p.status = "error";
      p.backend = "";
      p.model_loaded = false;
      p.last_error = "not authenticated";
      return MakeResponse(req, ipc::BuildHealth(p));
    }
    case ipc::MessageType::QueryDiagnostics: {
      ipc::QueryDiagnosticsPayload p;
      p.engine = config_.runtime_tier;
      p.backend = "";
      p.fallback_state = "degraded_simple";
      p.last_error = "not authenticated";
      return MakeResponse(req, ipc::BuildQueryDiagnostics(p));
    }
    default:
      // Cancel and unknown types are fire-and-forget; no response needed.
      return std::nullopt;
  }
}

std::optional<ipc::Envelope> Dispatcher::HandleHandshake(const ipc::Envelope& req) {
  client_supports_secure_flag_ = false;
  ipc::HandshakeResponse res;
  res.host_version = config_.host_version;
  res.protocol_version = config_.protocol_version;
  res.host_generation_id = config_.host_generation_id;
  res.capabilities = {"oob_cancel", "commit_segments"};
  if (auto parsed = ipc::ParseHandshakeRequest(req.payload_json)) {
    const bool version_ok = parsed->protocol_version == config_.protocol_version;
    const bool token_ok = config_.handshake_token.empty() ||
                          ConstantTimeEquals(parsed->handshake_token, config_.handshake_token);
    res.accepted = version_ok && token_ok;
    client_supports_secure_flag_ =
        res.accepted && std::find(parsed->capabilities.begin(), parsed->capabilities.end(),
                                  "secure_flag") != parsed->capabilities.end();
    SetClientId(res.accepted ? std::move(parsed->client_id) : std::string());
  } else {
    res.accepted = false;
    SetClientId({});
  }
  authenticated_ = res.accepted;
  res.model_loaded = engine_->model_loaded();
  if (settings_store_) {
    // The settings app writes settings.json before it sends UpdateConfig, so a
    // TIP that re-handshakes on the file change can arrive first and would
    // otherwise be answered with the values it already has (DEV-1143). Read
    // the newer file directly rather than reloading the store: taking
    // update_config_mutex here would park this reply behind the model reload
    // UpdateConfig performs under it, past the TIP's handshake timeout.
    const auto written = settings_store_->SettingsWrittenAfterLoad();
    const auto& settings = written ? *written : settings_store_->settings();
    res.batch_romaji_conversion = settings.batch_romaji_conversion;
    res.batch_romaji_preview_style = settings.batch_romaji_preview_style;
    res.batch_conversion_mode = settings.batch_conversion_mode;
    res.batch_auto_punctuation = settings.batch_auto_punctuation;
    res.number_rewriter = settings.number_rewriter;
    res.katakana_rewriter = settings.katakana_rewriter;
    res.symbol_rewriter = settings.symbol_rewriter;
    res.emoji_rewriter = settings.emoji_rewriter;
    res.emoji_trigger_search = settings.emoji_trigger_search;
    res.emoji_max_candidates = static_cast<uint32_t>(settings.emoji_max_candidates);
    res.emoji_trigger_min_query_length =
        static_cast<uint32_t>(settings.emoji_trigger_min_query_length);
    res.max_candidates = static_cast<uint32_t>(settings.max_candidates);
  }
  return MakeResponse(req, ipc::BuildHandshakeResponse(res));
}

bool Dispatcher::RequiresAuthenticatedSession() const {
  return !config_.handshake_token.empty() && !authenticated_;
}

bool Dispatcher::LearningAllowed(bool secure, bool learning_allowed,
                                 bool host_learning_blocked) const {
  return client_supports_secure_flag_ && !secure && learning_allowed && !host_learning_blocked;
}

void Dispatcher::SetClientId(std::string client_id) {
  if (client_id == client_id_) return;

  if (!client_id_.empty()) {
    scheduler_->UnregisterClient(client_id_);
  }
  client_id_ = std::move(client_id);
  if (!client_id_.empty()) {
    scheduler_->RegisterClient(client_id_);
  }
}

std::optional<ipc::Envelope> Dispatcher::HandlePing(const ipc::Envelope& req) {
  auto parsed = ipc::ParsePing(req.payload_json);
  ipc::PingPayload res;
  res.nonce = parsed ? parsed->nonce : 0;
  res.t_ms = NowMs();
  return MakeResponse(req, ipc::BuildPing(res));
}

std::optional<ipc::Envelope> Dispatcher::HandleHealth(const ipc::Envelope& req) {
  ipc::HealthPayload p;
  const auto engine_health = engine_->health_snapshot();
  p.backend = BackendName(engine_health.backend);
  p.model_loaded = engine_health.model_loaded;
  p.last_error = engine_health.last_error;
  if (!p.last_error) {
    p.status = "ok";
  } else if (p.model_loaded || !engine_health.model_path.empty()) {
    p.status = "degraded";
  } else {
    p.status = "error";
  }
  return MakeResponse(req, ipc::BuildHealth(p));
}

std::optional<ipc::Envelope> Dispatcher::HandleQueryDiagnostics(const ipc::Envelope& req) {
  const auto engine_health = engine_->health_snapshot();
  ipc::QueryDiagnosticsPayload p;
  const bool effective_model_loaded = engine_health.model_loaded && config_.runtime_tier != "mock";
  p.model_loaded = effective_model_loaded;
  if (effective_model_loaded && !engine_health.model_path.empty()) {
    p.loaded_model_path = engine_health.model_path;
  }
  p.engine = config_.runtime_tier;
  p.backend = BackendName(engine_health.backend);
  p.rss_mb = CurrentRssMb();
  p.learning_entries = static_cast<uint64_t>(engine_health.learning_entries);
  p.user_dict_entries = static_cast<uint64_t>(engine_health.user_dict_entries);
  p.last_error = engine_health.last_error;

  const bool model_enabled = !settings_store_ || settings_store_->settings().model.enabled;
  // Section 12.6 / D-009: SafeMode outranks everything, model.enabled=false
  // included, because it is the one state the user has to act on.
  if (SafeModeEnabled()) {
    p.fallback_state = "safe_mode";
  } else if (!model_enabled) {
    p.fallback_state = "healthy";
  } else if (!effective_model_loaded) {
    p.fallback_state = "degraded_simple";
  } else if (engine_health.last_error) {
    p.fallback_state = "degraded_model";
  } else {
    p.fallback_state = "healthy";
  }
  return MakeResponse(req, ipc::BuildQueryDiagnostics(p));
}

bool Dispatcher::SafeModeEnabled() const {
  // The engine's state, not the settings: UpdateConfig holds its lock through
  // a model reload, and the engine is kept in step with the flag at startup
  // and on every UpdateConfig.
  return engine_->health_state() == HealthState::SafeMode;
}

std::optional<ipc::Envelope> Dispatcher::HandleLoadModel(const ipc::Envelope& req) {
  ipc::LoadModelResponse res;
  auto parsed = ipc::ParseLoadModelRequest(req.payload_json);
  if (!parsed) {
    res.ok = false;
    res.error = "invalid LoadModel payload";
  } else if (SafeModeEnabled()) {
    // Section 8.5.3: no model runs in SafeMode, whoever asks.
    res.ok = false;
    res.error = "safe_mode";
  } else {
    auto backend = ParseBackend(parsed->backend);
    if (!backend) {
      res.ok = false;
      res.error = "unsupported backend: " + parsed->backend;
    } else {
      ModelLoadOptions options;
      options.path = parsed->path;
      options.backend = *backend;
      options.n_gpu_layers = parsed->n_gpu_layers;
      const auto load_result = engine_->LoadModelWithResult(options);
      res.ok = load_result.ok;
      res.error = load_result.error;
      if (!res.ok && !res.error) res.error = "model load failed";
    }
  }
  return MakeResponse(req, ipc::BuildLoadModelResponse(res));
}

std::optional<ipc::Envelope> Dispatcher::HandleQueryCandidates(const ipc::Envelope& req) {
  auto parsed = ipc::ParseQueryCandidatesRequest(req.payload_json);
  InferenceTrace trace(req.request_id, client_id_, core::EtwBackend::Unknown,
                       parsed ? parsed->reading.size() : 0);
  if (!parsed) {
    ipc::QueryCandidatesResponse res;
    res.partial = false;
    return MakeResponse(req, ipc::BuildQueryCandidatesResponse(res));
  }
  auto cancel = scheduler_->TrackCancellation(client_id_, req.request_id);
  if (!cancel) {
    // Capacity rejection is a failure, distinct from a tracked cancellation.
    return MakeResponse(req, ipc::BuildQueryCandidatesResponse(ipc::QueryCandidatesResponse{}));
  }
  scheduler_->MarkLatest(client_id_, req.request_id);
  trace.WatchCancellation(cancel);
  RequestCompletionGuard completion(scheduler_, client_id_, req.request_id);

  const auto engine_config = engine_->config();
  const auto rewriters = engine_config.rewriters;
  std::vector<core::Candidate> candidates;
  std::string corrected_reading;
  if (!parsed->emoji_trigger.empty()) {
    if (!parsed->reading.empty()) {
      static std::atomic_flag warned = ATOMIC_FLAG_INIT;
      if (!warned.test_and_set()) {
        static logging::RuntimeLogger logger(logging::RuntimeLoggerOptionsFromEnvironment("host"));
        logger.Log(logging::RuntimeLogLevel::Warn, "invalid_mixed_emoji_query");
      }
      ipc::QueryCandidatesResponse invalid;
      invalid.ok = false;
      invalid.error = "reading and emoji_trigger are mutually exclusive";
      completion.Complete();
      return MakeResponse(req, ipc::BuildQueryCandidatesResponse(invalid));
    } else if (!parsed->live) {
      candidates =
          engine_->QueryRewriters(rewriters, {}, parsed->emoji_trigger, {}, parsed->max_candidates);
    }
  } else {
    const uint32_t reserve =
        parsed->live ? 0
                     : (rewriters.symbol_enabled ? 4u : 0u) + (rewriters.emoji_enabled ? 4u : 0u);
    const auto ordinary_limit = parsed->max_candidates;
    const size_t merged_limit = ordinary_limit == 0 ? 0 : size_t{ordinary_limit} + reserve;
    auto queried =
        engine_->QueryCandidatesEx(parsed->reading, parsed->left_context, NowSec(), cancel.get(),
                                   ordinary_limit, parsed->live, trace.context());
    candidates = std::move(queried.candidates);
    corrected_reading = std::move(queried.corrected_reading);
    // Under auto_replace the conversion ran on the corrected reading, so the
    // rewriters have to key off the same reading or they would look up the
    // typo the user is no longer being shown.
    const std::string& rewriter_reading =
        corrected_reading.empty() ? parsed->reading : corrected_reading;
    if (!parsed->live && (rewriters.symbol_enabled || rewriters.emoji_enabled))
      candidates = engine_->QueryRewriters(rewriters, rewriter_reading, {}, std::move(candidates),
                                           merged_limit);
  }

  const bool canceled = cancel->load(std::memory_order_acquire);
  completion.Complete();
  if (canceled) {
    trace.Finish(core::EtwResult::Cancelled);
    return std::nullopt;  // don't reply to canceled requests
  }

  ipc::QueryCandidatesResponse res;
  for (auto& c : candidates) res.candidates.push_back(ToField(c));
  if (parsed->live && parsed->auto_punctuation && engine_config.enable_live_conversion &&
      engine_config.dynamic_punctuation && !res.candidates.empty()) {
    ipc::LiveSegment converted;
    converted.surface = res.candidates.front().surface;
    converted.reading = res.candidates.front().reading;
    converted.score = 1.0;
    const auto rules = PunctuationInserter::LoadRules(engine_config.punctuation_rules_path);
    auto inserted = PunctuationInserter::Insert({converted}, rules, parsed->punctuation_style,
                                                engine_config.segment_boundary_confidence);
    res.candidates.front().surface = std::move(inserted.surface);
    res.segments = std::move(inserted.segments);
  }
  if (!parsed->emoji_trigger.empty() && parsed->max_candidates > 0 &&
      res.candidates.size() > parsed->max_candidates) {
    res.candidates.resize(parsed->max_candidates);
  }
  res.partial = false;
  res.corrected_reading = std::move(corrected_reading);
  trace.Finish(core::EtwResult::Success, res.candidates.size());
  return MakeResponse(req, ipc::BuildQueryCandidatesResponse(res));
}

void Dispatcher::HandleObserveTypo(const ipc::Envelope& req) {
  auto parsed = ipc::ParseObserveTypoRequest(req.payload_json);
  if (!parsed) return;
  auto privacy =
      settings_store_ ? std::optional{settings_store_->LockPrivacyPolicy()} : std::nullopt;
  if (!LearningAllowed(parsed->secure, parsed->learning_allowed,
                       privacy && (privacy->policy.secure || !privacy->policy.learning_allowed)))
    return;
  // The engine applies the accept filters and the "off" gate; a rejected pair
  // is simply not recorded, and there is no reply to carry that outcome.
  engine_->ObserveTypo(parsed->wrong_reading, parsed->correct_reading, NowSec());
}

std::optional<ipc::Envelope> Dispatcher::HandleListNewWordCandidates(const ipc::Envelope& req) {
  ipc::ListNewWordCandidatesResponse res;
  const auto fail = [&](std::string_view error) {
    res.ok = false;
    res.error = std::string(error);
    return MakeResponse(req, ipc::BuildListNewWordCandidatesResponse(res));
  };
  auto parsed = ipc::ParseListNewWordCandidatesRequest(req.payload_json);
  learning::AutoWordState state = learning::AutoWordState::Pending;
  // The parser already restricts state_filter to the three names, so a failed
  // ParseAutoWordState here would be a codec/store mismatch, not user input.
  if (!parsed || !learning::ParseAutoWordState(parsed->state_filter, state)) {
    return fail(ipc::kNewWordErrorInvalidRequest);
  }
  if (!auto_word_store_) return fail(ipc::kNewWordErrorStoreUnavailable);

  auto words = auto_word_store_->ListByState(state);
  // Most recently seen first, so a page of max_items shows the words the user
  // has been typing rather than an arbitrary slice of the map order.
  std::sort(words.begin(), words.end(),
            [](const learning::AutoWord& a, const learning::AutoWord& b) {
              if (a.last_seen_epoch != b.last_seen_epoch)
                return a.last_seen_epoch > b.last_seen_epoch;
              if (a.surface != b.surface) return a.surface < b.surface;
              return a.reading < b.reading;
            });
  if (words.size() > parsed->max_items) words.resize(parsed->max_items);

  res.items.reserve(words.size());
  for (const auto& word : words) {
    ipc::NewWordField field;
    field.surface = word.surface;
    field.reading = word.reading;
    field.source = std::string(learning::AutoWordSourceName(word.source));
    field.state = std::string(learning::AutoWordStateName(word.state));
    field.count = word.count;
    field.last_seen_epoch = word.last_seen_epoch;
    res.items.push_back(std::move(field));
  }
  return MakeResponse(req, ipc::BuildListNewWordCandidatesResponse(res));
}

std::optional<ipc::Envelope> Dispatcher::HandleResolveNewWord(const ipc::Envelope& req) {
  ipc::ResolveNewWordResponse res;
  const auto reply = [&]() { return MakeResponse(req, ipc::BuildResolveNewWordResponse(res)); };
  const auto fail = [&](std::string_view error) {
    res.ok = false;
    res.error = std::string(error);
    return reply();
  };
  auto parsed = ipc::ParseResolveNewWordRequest(req.payload_json);
  if (!parsed) return fail(ipc::kNewWordErrorInvalidRequest);
  if (!auto_word_store_) return fail(ipc::kNewWordErrorStoreUnavailable);

  const auto target = parsed->action == "confirm" ? learning::AutoWordState::Confirmed
                                                  : learning::AutoWordState::Rejected;
  const auto previous = auto_word_store_->SetState(parsed->surface, parsed->reading, target);
  if (!previous) return fail(ipc::kNewWordErrorNotFound);
  // A repeated click is a success that changed nothing, and it does not
  // rewrite the file.
  if (*previous == target) {
    res.ok = true;
    return reply();
  }
  if (!auto_word_store_->Save()) {
    // Put the word back so memory keeps matching the file: otherwise a failed
    // confirm would still inject the word until the host restarts. Only if it
    // is still in our state, so a concurrent resolve from another connection
    // is not undone.
    auto_word_store_->CompareAndSetState(parsed->surface, parsed->reading, target, *previous);
    return fail(ipc::kNewWordErrorSaveFailed);
  }
  res.ok = true;
  res.changed = true;
  return reply();
}

std::optional<ipc::Envelope> Dispatcher::HandleQueryBatchConversion(const ipc::Envelope& req) {
  auto parsed = ipc::ParseQueryBatchConversionRequest(req.payload_json);
  InferenceTrace trace(
      req.request_id, client_id_,
      parsed && parsed->mode == "ai-cleanup" ? core::EtwBackend::Ai : core::EtwBackend::Unknown,
      parsed ? parsed->reading.size() : 0);
  if (!parsed) {
    static logging::RuntimeLogger logger(logging::RuntimeLoggerOptionsFromEnvironment("host"));
    logger.Log(logging::RuntimeLogLevel::Error, "invalid_batch_request");
    ipc::QueryBatchConversionResponse res;
    return MakeResponse(req, ipc::BuildQueryBatchConversionResponse(res));
  }

  auto cancel = scheduler_->TrackCancellation(client_id_, req.request_id);
  if (!cancel) {
    // Preserve the wire response while recording the capacity rejection accurately.
    ipc::QueryBatchConversionResponse res;
    res.canceled = true;
    return MakeResponse(req, ipc::BuildQueryBatchConversionResponse(res));
  }
  scheduler_->MarkLatest(client_id_, req.request_id);
  trace.WatchCancellation(cancel);
  RequestCompletionGuard completion(scheduler_, client_id_, req.request_id);

  ipc::QueryBatchConversionResponse res;
  bool suppress_learning = false;
  if (parsed->mode == "ai-cleanup") {
    AiBackendOptions options;
    core::AiPrivacy privacy{true, true};
    {
      std::lock_guard lock(*config_.update_config_mutex);
      if (settings_store_) {
        const auto& settings = settings_store_->settings();
        options.backend = settings.ai_backend;
        options.endpoint = settings.open_ai_api_endpoint;
        options.api_key = settings.open_ai_api_key;
        options.model = settings.open_ai_model;
        options.timeout_ms = settings.open_ai_timeout_ms;
        options.include_context = settings.include_context_in_ai_transform;
        privacy = settings.ai_privacy;
      }
    }
    AiTransformRequest transform;
    if (!parsed->ai_backend.empty()) options.backend = parsed->ai_backend;
    transform.task = AiTask::Cleanup;
    if (parsed->raw_romaji.empty()) options.backend = "none";
    transform.text = parsed->reading;
    transform.raw_romaji = parsed->raw_romaji;
    transform.auto_punctuation = parsed->auto_punctuation;
    transform.ai_allowed = privacy.ai && parsed->ai_allowed;
    suppress_learning = !transform.ai_allowed;
    transform.external_allowed = privacy.external && parsed->external_ai_allowed;
    const auto phase_start = std::chrono::steady_clock::now();
    AiTransformResult result;
    try {
      result = config_.ai_backend->Transform(
          transform, options, cancel.get(),
          [this](const AiTransformRequest& local, const std::atomic<bool>* flag,
                 AiDeadline deadline) { return engine_->TransformLocal(local, flag, deadline); });
    } catch (...) {
      core::EtwLogger::LogInferencePhase(
          req.request_id, core::EtwPhase::AiTransform, core::EtwBackend::Ai,
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - phase_start)
              .count(),
          cancel->load(std::memory_order_acquire) ? core::EtwResult::Cancelled
                                                  : core::EtwResult::Failed,
          trace.context()->client);
      throw;
    }
    const auto phase_result =
        result.ok                                      ? core::EtwResult::Success
        : result.error_class == AiErrorClass::Canceled ? core::EtwResult::Cancelled
        : result.error_class == AiErrorClass::Timeout  ? core::EtwResult::Timeout
                                                       : core::EtwResult::Failed;
    core::EtwLogger::LogInferencePhase(
        req.request_id, core::EtwPhase::AiTransform, core::EtwBackend::Ai,
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - phase_start)
            .count(),
        phase_result, trace.context()->client);
    if (!result.ok) {
      static logging::RuntimeLogger logger(logging::RuntimeLoggerOptionsFromEnvironment("host"));
      logger.Log(logging::RuntimeLogLevel::Error, "ai_cleanup_fallback",
                 {{"error_class", static_cast<uint64_t>(result.error_class)}});
    }
    if (result.ok) {
      ipc::CandidateField candidate;
      candidate.reading = parsed->reading;
      candidate.surface = result.result;
      candidate.source = "llm";
      res.full_surface = candidate.surface;
      res.segments.push_back({parsed->reading, {std::move(candidate)}});
    }
  }
  if (res.segments.empty()) {
    for (const auto& reading : core::SplitBatchConversion(parsed->reading)) {
      if (cancel->load(std::memory_order_acquire)) break;
      auto candidates = engine_->QueryCandidates(reading, "", NowSec(), cancel.get(),
                                                 parsed->max_candidates, false, trace.context());
      ipc::BatchConversionSegment segment;
      segment.reading = reading;
      for (auto& candidate : candidates) segment.candidates.push_back(ToField(candidate));
      if (parsed->max_candidates > 0 && segment.candidates.size() > parsed->max_candidates) {
        segment.candidates.resize(parsed->max_candidates);
      }
      if (segment.candidates.empty()) {
        ipc::CandidateField fallback;
        fallback.reading = reading;
        fallback.surface = reading;
        fallback.source = "fallback";
        segment.candidates.push_back(std::move(fallback));
      }
      // Preserve secure/unknown-input learning suppression even when host and
      // TIP settings snapshots differ. The surface still comes from neural.
      if (suppress_learning)
        for (auto& candidate : segment.candidates) candidate.source = "privacy-fallback";
      res.full_surface += segment.candidates.front().surface;
      res.segments.push_back(std::move(segment));
    }
  }
  if (cancel->load(std::memory_order_acquire)) {
    res = {};
    res.canceled = true;
  }
  completion.Complete();
  uint64_t candidate_count = 0;
  for (const auto& segment : res.segments) candidate_count += segment.candidates.size();
  trace.Finish(res.canceled ? core::EtwResult::Cancelled : core::EtwResult::Success,
               candidate_count);
  return MakeResponse(req, ipc::BuildQueryBatchConversionResponse(res));
}

void Dispatcher::HandleCancel(const ipc::Envelope& req) {
  if (auto parsed = ipc::ParseCancel(req.payload_json)) {
    scheduler_->Cancel(client_id_, parsed->target_request_id);
  }
}

std::optional<ipc::Envelope> Dispatcher::HandleCommitObservation(const ipc::Envelope& req) {
  auto privacy =
      settings_store_ ? std::optional{settings_store_->LockPrivacyPolicy()} : std::nullopt;
  ipc::CommitObservationResponse res;
  if (auto parsed = ipc::ParseCommitObservationRequest(req.payload_json);
      parsed &&
      LearningAllowed(parsed->secure, parsed->learning_allowed,
                      privacy && (privacy->policy.secure || !privacy->policy.learning_allowed))) {
    // A duplicate resend is answered ok=true: the observation is already
    // recorded, so the TIP must stop retrying it (DEV-554).
    engine_->CommitObservation(parsed->reading, parsed->chosen.surface, NowSec(),
                               parsed->observation_id);
    res.ok = true;
  } else {
    res.ok = false;
  }
  return MakeResponse(req, ipc::BuildCommitObservationResponse(res));
}

std::optional<ipc::Envelope> Dispatcher::HandleAddUserWord(const ipc::Envelope& req) {
  ipc::AddUserWordResponse res;
  if (!user_dict_) {
    res.ok = false;
    return MakeResponse(req, ipc::BuildAddUserWordResponse(res));
  }
  if (auto parsed = ipc::ParseAddUserWordRequest(req.payload_json)) {
    learning::UserWord w;
    w.word = parsed->word;
    w.ruby = parsed->ruby;
    w.cid = parsed->cid;
    w.mid = parsed->mid;
    w.value = parsed->value;
    res.ok = engine_->AddUserWord(w);
  }
  return MakeResponse(req, ipc::BuildAddUserWordResponse(res));
}

std::optional<ipc::Envelope> Dispatcher::HandleRemoveUserWord(const ipc::Envelope& req) {
  ipc::RemoveUserWordResponse res;
  if (!user_dict_) {
    res.ok = false;
    return MakeResponse(req, ipc::BuildRemoveUserWordResponse(res));
  }
  if (auto parsed = ipc::ParseRemoveUserWordRequest(req.payload_json)) {
    res.ok = engine_->RemoveUserWord(parsed->word, parsed->ruby);
  }
  return MakeResponse(req, ipc::BuildRemoveUserWordResponse(res));
}

std::optional<ipc::Envelope> Dispatcher::HandleUpdateConfig(const ipc::Envelope& req) {
  ipc::UpdateConfigResponse res;
  if (!settings_store_) {
    res.ok = false;
    res.error = "settings store not configured";
    return MakeResponse(req, ipc::BuildUpdateConfigResponse(res));
  }

  std::lock_guard<std::mutex> lock(*config_.update_config_mutex);
  const auto load_result = settings_store_->Reload();
  core::CrashReporting::SetConsent(load_result.settings.crash_report_consent == "local"
                                       ? core::CrashConsent::Local
                                       : core::CrashConsent::Off);
  if (load_result.status == SettingsLoadStatus::Invalid) {
    res.ok = false;
    res.error = load_result.error.value_or("invalid settings.json");
    return MakeResponse(req, ipc::BuildUpdateConfigResponse(res));
  }

  auto next_config = ApplyRuntimeSettingsToEngineConfig(engine_->config(), load_result.settings,
                                                        config_.default_backend);
  if (config_.override_backend) {
    next_config.backend = *config_.override_backend;
  }
  const bool safe_mode = load_result.settings.safe_mode.enabled;
  if (config_.override_model_path && !safe_mode) {
    next_config.model_path = *config_.override_model_path;
  }
  // SafeMode ends only here, when the user has turned the flag off (section
  // 8.5.1); a flag that appears on disk is followed as it stands.
  if (!safe_mode && engine_->health_state() == HealthState::SafeMode) {
    (void)engine_->ApplyHealthEvent(HealthEvent::SafeModeCleared);
  } else if (safe_mode && engine_->health_state() != HealthState::SafeMode) {
    engine_->RestoreHealthState(HealthState::SafeMode);
  }
  engine_->ApplyConfig(next_config);
  const auto model_result = engine_->LoadModelWithResult(
      ModelLoadOptions{next_config.model_path, next_config.backend, next_config.n_gpu_layers,
                       next_config.inference_threads});

  res.ok = model_result.ok;
  if (load_result.error) {
    res.error = *load_result.error;
  } else if (!model_result.ok) {
    res.error = model_result.error.value_or("model load failed");
  }
  return MakeResponse(req, ipc::BuildUpdateConfigResponse(res));
}

}  // namespace azookey::host
