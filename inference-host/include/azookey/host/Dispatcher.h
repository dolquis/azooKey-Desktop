#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "azookey/host/InferenceEngine.h"
#include "azookey/host/RequestScheduler.h"
#include "azookey/host/SettingsStore.h"
#include "azookey/ipc/Messages.h"
#include "azookey/learning/UserDictionary.h"

namespace azookey::host {

struct DispatcherConfig {
  std::string host_version{"0.1.0"};
  int protocol_version{1};
  std::string host_generation_id;
  std::string handshake_token;
  BackendKind default_backend{BackendKind::Cpu};
  std::optional<BackendKind> override_backend;
  std::optional<std::string> override_model_path;
  std::string runtime_tier{"mock"};
  // Shared by per-connection Dispatcher copies so config reload/apply is serialized.
  std::shared_ptr<std::mutex> update_config_mutex{std::make_shared<std::mutex>()};
};

// Envelope-level request handler. Transport-agnostic: drives the same code
// path whether messages arrive from stdin (current) or a Named Pipe (M1).
//
// Dispatch() returns std::nullopt for fire-and-forget messages (Cancel) and
// for queries that were canceled mid-flight; otherwise it returns the
// response Envelope to send back to the caller.
class Dispatcher {
 public:
  Dispatcher(InferenceEngine* engine, RequestScheduler* scheduler,
             learning::UserDictionary* user_dict, DispatcherConfig config = {},
             SettingsStore* settings_store = nullptr);
  ~Dispatcher();

  std::optional<ipc::Envelope> Dispatch(const ipc::Envelope& request);

  const DispatcherConfig& config() const { return config_; }

 private:
  std::optional<ipc::Envelope> HandleUnauthenticated(const ipc::Envelope& req);
  std::optional<ipc::Envelope> HandleHandshake(const ipc::Envelope& req);
  std::optional<ipc::Envelope> HandlePing(const ipc::Envelope& req);
  std::optional<ipc::Envelope> HandleHealth(const ipc::Envelope& req);
  std::optional<ipc::Envelope> HandleQueryDiagnostics(const ipc::Envelope& req);
  std::optional<ipc::Envelope> HandleLoadModel(const ipc::Envelope& req);
  std::optional<ipc::Envelope> HandleQueryCandidates(const ipc::Envelope& req);
  std::optional<ipc::Envelope> HandleQueryBatchConversion(const ipc::Envelope& req);
  void HandleCancel(const ipc::Envelope& req);
  std::optional<ipc::Envelope> HandleCommitObservation(const ipc::Envelope& req);
  std::optional<ipc::Envelope> HandleAddUserWord(const ipc::Envelope& req);
  std::optional<ipc::Envelope> HandleRemoveUserWord(const ipc::Envelope& req);
  std::optional<ipc::Envelope> HandleUpdateConfig(const ipc::Envelope& req);
  bool RequiresAuthenticatedSession() const;
  void SetClientId(std::string client_id);

  InferenceEngine* engine_;
  RequestScheduler* scheduler_;
  learning::UserDictionary* user_dict_;
  SettingsStore* settings_store_;
  DispatcherConfig config_;
  bool authenticated_{false};
  std::string client_id_;
};

}  // namespace azookey::host
