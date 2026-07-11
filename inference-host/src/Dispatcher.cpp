#include "azookey/host/Dispatcher.h"

#include <atomic>
#include <chrono>

#include "azookey/ipc/Payloads.h"

namespace azookey::host {

namespace {

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
  return "heuristic";
}

ipc::CandidateField ToField(const core::Candidate& c) {
  ipc::CandidateField f;
  f.surface = c.surface;
  f.reading = c.reading;
  f.score = c.score;
  f.source = SourceToWire(c.source);
  return f;
}

const char* BackendName(BackendKind backend) {
  switch (backend) {
    case BackendKind::Cuda:
      return "cuda";
    case BackendKind::Cpu:
      return "cpu";
  }
  return "cpu";
}

std::optional<BackendKind> ParseBackend(const std::string& backend) {
  if (backend.empty() || backend == "cpu") return BackendKind::Cpu;
  if (backend == "cuda") return BackendKind::Cuda;
  return std::nullopt;
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
                       SettingsStore* settings_store)
    : engine_(engine),
      scheduler_(scheduler),
      user_dict_(user_dict),
      settings_store_(settings_store),
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
    case ipc::MessageType::AddUserWord:
      return HandleAddUserWord(req);
    case ipc::MessageType::RemoveUserWord:
      return HandleRemoveUserWord(req);
    case ipc::MessageType::UpdateConfig:
      return HandleUpdateConfig(req);
    default:
      return std::nullopt;
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
    case ipc::MessageType::CommitObservation: {
      ipc::CommitObservationResponse r;
      r.ok = false;
      return MakeResponse(req, ipc::BuildCommitObservationResponse(r));
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
    default:
      // Cancel and unknown types are fire-and-forget; no response needed.
      return std::nullopt;
  }
}

std::optional<ipc::Envelope> Dispatcher::HandleHandshake(const ipc::Envelope& req) {
  ipc::HandshakeResponse res;
  res.host_version = config_.host_version;
  res.protocol_version = config_.protocol_version;
  if (auto parsed = ipc::ParseHandshakeRequest(req.payload_json)) {
    const bool version_ok = parsed->protocol_version == config_.protocol_version;
    const bool token_ok =
        config_.handshake_token.empty() || parsed->handshake_token == config_.handshake_token;
    res.accepted = version_ok && token_ok;
    SetClientId(res.accepted ? std::move(parsed->client_id) : std::string());
  } else {
    res.accepted = false;
    SetClientId({});
  }
  authenticated_ = res.accepted;
  res.model_loaded = engine_->model_loaded();
  if (settings_store_) {
    const auto& settings = settings_store_->settings();
    res.batch_romaji_conversion = settings.batch_romaji_conversion;
    res.batch_romaji_preview_style = settings.batch_romaji_preview_style;
    res.batch_conversion_mode = settings.batch_conversion_mode;
    res.batch_auto_punctuation = settings.batch_auto_punctuation;
  }
  return MakeResponse(req, ipc::BuildHandshakeResponse(res));
}

bool Dispatcher::RequiresAuthenticatedSession() const {
  return !config_.handshake_token.empty() && !authenticated_;
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

std::optional<ipc::Envelope> Dispatcher::HandleLoadModel(const ipc::Envelope& req) {
  ipc::LoadModelResponse res;
  auto parsed = ipc::ParseLoadModelRequest(req.payload_json);
  if (!parsed) {
    res.ok = false;
    res.error = "invalid LoadModel payload";
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
  if (!parsed) {
    ipc::QueryCandidatesResponse res;
    res.partial = false;
    return MakeResponse(req, ipc::BuildQueryCandidatesResponse(res));
  }
  auto cancel = scheduler_->TrackCancellation(client_id_, req.request_id);
  scheduler_->MarkLatest(client_id_, req.request_id);
  RequestCompletionGuard completion(scheduler_, client_id_, req.request_id);

  auto candidates = engine_->QueryCandidates(parsed->reading, parsed->left_context, NowSec(),
                                             cancel.get(), parsed->max_candidates, parsed->live);

  const bool canceled = cancel->load(std::memory_order_acquire);
  completion.Complete();
  if (canceled) {
    return std::nullopt;  // don't reply to canceled requests
  }

  ipc::QueryCandidatesResponse res;
  for (auto& c : candidates) res.candidates.push_back(ToField(c));
  if (parsed->max_candidates > 0 && res.candidates.size() > parsed->max_candidates) {
    res.candidates.resize(parsed->max_candidates);
  }
  res.partial = false;
  return MakeResponse(req, ipc::BuildQueryCandidatesResponse(res));
}

std::optional<ipc::Envelope> Dispatcher::HandleQueryBatchConversion(const ipc::Envelope& req) {
  auto parsed = ipc::ParseQueryBatchConversionRequest(req.payload_json);
  if (!parsed) {
    ipc::QueryBatchConversionResponse res;
    return MakeResponse(req, ipc::BuildQueryBatchConversionResponse(res));
  }

  auto cancel = scheduler_->TrackCancellation(client_id_, req.request_id);
  scheduler_->MarkLatest(client_id_, req.request_id);
  RequestCompletionGuard completion(scheduler_, client_id_, req.request_id);

  auto candidates = engine_->QueryCandidates(parsed->reading, "", NowSec(), cancel.get(),
                                             parsed->max_candidates, false);

  const bool canceled = cancel->load(std::memory_order_acquire);
  completion.Complete();

  ipc::QueryBatchConversionResponse res;
  res.partial = false;
  res.canceled = canceled;
  if (canceled) {
    return MakeResponse(req, ipc::BuildQueryBatchConversionResponse(res));
  }

  ipc::BatchConversionSegment segment;
  segment.reading = parsed->reading;
  for (auto& c : candidates) segment.candidates.push_back(ToField(c));
  if (parsed->max_candidates > 0 && segment.candidates.size() > parsed->max_candidates) {
    segment.candidates.resize(parsed->max_candidates);
  }
  if (!segment.candidates.empty()) {
    res.full_surface = segment.candidates.front().surface;
  } else {
    res.full_surface = parsed->reading;
  }
  res.segments.push_back(std::move(segment));
  return MakeResponse(req, ipc::BuildQueryBatchConversionResponse(res));
}

void Dispatcher::HandleCancel(const ipc::Envelope& req) {
  if (auto parsed = ipc::ParseCancel(req.payload_json)) {
    scheduler_->Cancel(client_id_, parsed->target_request_id);
  }
}

std::optional<ipc::Envelope> Dispatcher::HandleCommitObservation(const ipc::Envelope& req) {
  ipc::CommitObservationResponse res;
  if (auto parsed = ipc::ParseCommitObservationRequest(req.payload_json)) {
    engine_->CommitObservation(parsed->reading, parsed->chosen.surface, NowSec());
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
  if (config_.override_model_path) {
    next_config.model_path = *config_.override_model_path;
  }
  engine_->ApplyConfig(next_config);
  const auto model_result = engine_->LoadModelWithResult(
      ModelLoadOptions{next_config.model_path, next_config.backend, next_config.n_gpu_layers});

  res.ok = model_result.ok;
  if (load_result.error) {
    res.error = *load_result.error;
  } else if (!model_result.ok) {
    res.error = model_result.error.value_or("model load failed");
  }
  return MakeResponse(req, ipc::BuildUpdateConfigResponse(res));
}

}  // namespace azookey::host
