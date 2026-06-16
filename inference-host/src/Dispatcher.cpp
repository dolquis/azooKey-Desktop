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

ipc::CandidateField ToField(const core::Candidate& c) {
  ipc::CandidateField f;
  f.surface = c.surface;
  f.reading = c.reading;
  f.score = c.score;
  f.source = c.debug_info;
  return f;
}

const char* BackendName(BackendKind backend) {
  switch (backend) {
    case BackendKind::Cuda: return "cuda";
    case BackendKind::Cpu: return "cpu";
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
  RequestCompletionGuard(RequestScheduler* scheduler, uint64_t request_id)
      : scheduler_(scheduler), request_id_(request_id) {}

  ~RequestCompletionGuard() {
    Complete();
  }

  void Complete() {
    if (active_) {
      scheduler_->CompleteRequest(request_id_);
      active_ = false;
    }
  }

 private:
  RequestScheduler* scheduler_;
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
      config_(std::move(config)) {}

std::optional<ipc::Envelope> Dispatcher::Dispatch(const ipc::Envelope& req) {
  if (req.type != ipc::MessageType::Handshake && RequiresAuthenticatedSession()) {
    return HandleUnauthenticated(req);
  }
  switch (req.type) {
    case ipc::MessageType::Handshake: return HandleHandshake(req);
    case ipc::MessageType::Ping: return HandlePing(req);
    case ipc::MessageType::Health: return HandleHealth(req);
    case ipc::MessageType::LoadModel: return HandleLoadModel(req);
    case ipc::MessageType::QueryCandidates: return HandleQueryCandidates(req);
    case ipc::MessageType::Cancel: HandleCancel(req); return std::nullopt;
    case ipc::MessageType::CommitObservation: return HandleCommitObservation(req);
    case ipc::MessageType::AddUserWord: return HandleAddUserWord(req);
    case ipc::MessageType::RemoveUserWord: return HandleRemoveUserWord(req);
    case ipc::MessageType::UpdateConfig: return HandleUpdateConfig(req);
    default: return std::nullopt;
  }
}

std::optional<ipc::Envelope> Dispatcher::HandleUnauthenticated(const ipc::Envelope& req) {
  // Return a type-appropriate error response so blocking clients do not hang
  // waiting for a reply that would never come if we returned nullopt.
  switch (req.type) {
    case ipc::MessageType::AddUserWord: {
      ipc::AddUserWordResponse r; r.ok = false;
      return MakeResponse(req, ipc::BuildAddUserWordResponse(r));
    }
    case ipc::MessageType::RemoveUserWord: {
      ipc::RemoveUserWordResponse r; r.ok = false;
      return MakeResponse(req, ipc::BuildRemoveUserWordResponse(r));
    }
    case ipc::MessageType::UpdateConfig: {
      ipc::UpdateConfigResponse r;
      r.ok = false;
      r.error = "not authenticated";
      return MakeResponse(req, ipc::BuildUpdateConfigResponse(r));
    }
    case ipc::MessageType::CommitObservation: {
      ipc::CommitObservationResponse r; r.ok = false;
      return MakeResponse(req, ipc::BuildCommitObservationResponse(r));
    }
    case ipc::MessageType::LoadModel: {
      ipc::LoadModelResponse r; r.ok = false; r.error = "not authenticated";
      return MakeResponse(req, ipc::BuildLoadModelResponse(r));
    }
    case ipc::MessageType::QueryCandidates: {
      ipc::QueryCandidatesResponse r; r.partial = false;
      return MakeResponse(req, ipc::BuildQueryCandidatesResponse(r));
    }
    case ipc::MessageType::Ping: {
      ipc::PingPayload p; p.nonce = 0; p.t_ms = 0;
      return MakeResponse(req, ipc::BuildPing(p));
    }
    case ipc::MessageType::Health: {
      ipc::HealthPayload p;
      p.status = "error"; p.backend = ""; p.model_loaded = false;
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
  } else {
    res.accepted = false;
  }
  authenticated_ = res.accepted;
  res.model_loaded = engine_->model_loaded();
  return MakeResponse(req, ipc::BuildHandshakeResponse(res));
}

bool Dispatcher::RequiresAuthenticatedSession() const {
  return !config_.handshake_token.empty() && !authenticated_;
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
  p.backend = BackendName(engine_->backend());
  p.model_loaded = engine_->model_loaded();
  p.last_error = engine_->last_error();
  if (!p.last_error) {
    p.status = "ok";
  } else if (p.model_loaded) {
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
  auto cancel = scheduler_->TrackCancellation(req.request_id);
  scheduler_->MarkLatest(req.request_id);
  RequestCompletionGuard completion(scheduler_, req.request_id);

  auto candidates = engine_->QueryCandidates(parsed->reading, parsed->left_context,
                                              NowSec(), cancel.get());

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

void Dispatcher::HandleCancel(const ipc::Envelope& req) {
  if (auto parsed = ipc::ParseCancel(req.payload_json)) {
    scheduler_->Cancel(parsed->target_request_id);
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
    user_dict_->Add(w);
    user_dict_->Save();
    res.ok = true;
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
    res.ok = user_dict_->Remove(parsed->word, parsed->ruby);
    if (res.ok) user_dict_->Save();
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

  const auto load_result = settings_store_->Reload();
  auto next_config = ApplyRuntimeSettingsToEngineConfig(engine_->config(), load_result.settings);
  engine_->ApplyConfig(next_config);
  const auto model_result = engine_->LoadModelWithResult(
      ModelLoadOptions{next_config.model_path, next_config.backend, next_config.n_gpu_layers});

  res.ok = load_result.status != SettingsLoadStatus::Invalid && model_result.ok;
  if (load_result.error) {
    res.error = *load_result.error;
  } else if (!model_result.ok) {
    res.error = model_result.error.value_or("model load failed");
  }
  return MakeResponse(req, ipc::BuildUpdateConfigResponse(res));
}

}  // namespace azookey::host
