#include "SettingsIpcClient.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "azookey/ipc/HandshakeToken.h"
#include "azookey/ipc/Messages.h"
#include "azookey/ipc/NamedPipeTransport.h"
#include "azookey/ipc/TraceId.h"

namespace azookey::settings {
namespace {

using azookey::ipc::MessageType;

azookey::ipc::Envelope MakeEnvelope(uint64_t request_id, MessageType type, std::string payload,
                                    const std::string& trace_id) {
  azookey::ipc::Envelope envelope;
  envelope.request_id = request_id;
  envelope.trace_id = trace_id;
  envelope.type = type;
  envelope.payload_json = std::move(payload);
  return envelope;
}

SettingsIpcResult Failure(std::string error) {
  SettingsIpcResult result;
  result.error = std::move(error);
  return result;
}

// Which step of opening a session failed, so each caller words its own error.
enum class OpenFailure {
  None,
  NoPipe,
  NoToken,
  NoTraceId,
  NotRunning,
  HandshakeNotSent,
  HandshakeTimeout,
  HandshakeRejected,
};

// One connection to the Host that has completed the Handshake. Requests are numbered in order
// and every request shares the session's trace ID.
class HostSession {
 public:
  OpenFailure Open(const SettingsIpcOptions& options) {
    if (options.pipe_name.empty()) return OpenFailure::NoPipe;
    const auto token = options.handshake_token.empty()
                           ? azookey::ipc::ReadClientHandshakeToken()
                           : std::optional<std::string>(options.handshake_token);
    if (!token) return OpenFailure::NoToken;
    try {
      trace_id_ = azookey::ipc::GenerateTraceId();
    } catch (...) {
      return OpenFailure::NoTraceId;
    }
    if (!client_.Connect(options.pipe_name, options.connect_timeout_ms)) {
      return OpenFailure::NotRunning;
    }

    azookey::ipc::HandshakeRequest handshake;
    handshake.tip_version = "settings-app";
    handshake.protocol_version = azookey::ipc::kHandshakeProtocolVersion;
    handshake.capabilities = {"settings"};
    handshake.client_id = "settings-app";
    handshake.handshake_token = *token;
    const uint64_t request_id = next_request_id_++;
    if (!client_.Send(MakeEnvelope(request_id, MessageType::Handshake,
                                   azookey::ipc::BuildHandshakeRequest(handshake), trace_id_))) {
      return OpenFailure::HandshakeNotSent;
    }
    const auto response = client_.ReceiveWithTimeout(options.response_timeout_ms);
    if (!response || response->request_id != request_id ||
        response->type != MessageType::Handshake) {
      return OpenFailure::HandshakeTimeout;
    }
    const auto payload = azookey::ipc::ParseHandshakeResponse(response->payload_json);
    if (!payload || !payload->accepted) return OpenFailure::HandshakeRejected;
    capabilities_ = payload->capabilities;
    return OpenFailure::None;
  }

  bool HasCapability(const char* capability) const {
    return std::find(capabilities_.begin(), capabilities_.end(), capability) != capabilities_.end();
  }

  // Sends one request and returns the payload of the response with the same ID and type.
  // `sent` is false when the request never left; otherwise a missing result means no usable reply.
  std::optional<std::string> Exchange(MessageType type, std::string payload, uint32_t timeout_ms,
                                      bool* sent) {
    const uint64_t request_id = next_request_id_++;
    *sent = client_.Send(MakeEnvelope(request_id, type, std::move(payload), trace_id_));
    if (!*sent) return std::nullopt;
    const auto response = client_.ReceiveWithTimeout(timeout_ms);
    if (!response || response->request_id != request_id || response->type != type) {
      return std::nullopt;
    }
    return response->payload_json;
  }

 private:
  azookey::ipc::NamedPipeClient client_;
  std::string trace_id_;
  uint64_t next_request_id_{1};
  std::vector<std::string> capabilities_;
};

HostCallStatus StatusFor(OpenFailure failure) {
  switch (failure) {
    case OpenFailure::None:
      return HostCallStatus::Ok;
    case OpenFailure::NoPipe:
    case OpenFailure::NoToken:
    case OpenFailure::NoTraceId:
      return HostCallStatus::Unavailable;
    case OpenFailure::NotRunning:
      return HostCallStatus::HostNotRunning;
    case OpenFailure::HandshakeNotSent:
    case OpenFailure::HandshakeTimeout:
      return HostCallStatus::Timeout;
    case OpenFailure::HandshakeRejected:
      break;
  }
  return HostCallStatus::HandshakeRejected;
}

template <typename Response, typename Parse>
HostCallResult<Response> Call(const SettingsIpcOptions& options, const char* capability,
                              MessageType type, std::string payload, uint32_t timeout_ms,
                              Parse parse) {
  HostCallResult<Response> result;
  HostSession session;
  if (const auto failure = session.Open(options); failure != OpenFailure::None) {
    result.status = StatusFor(failure);
    return result;
  }
  if (capability != nullptr && !session.HasCapability(capability)) {
    result.status = HostCallStatus::Unsupported;
    return result;
  }
  bool sent = false;
  const auto reply = session.Exchange(type, std::move(payload), timeout_ms, &sent);
  if (!reply) {
    result.status = HostCallStatus::Timeout;
    return result;
  }
  result.response = parse(*reply);
  result.status = result.response ? HostCallStatus::Ok : HostCallStatus::InvalidResponse;
  return result;
}

}  // namespace

SettingsIpcOptions DefaultSettingsIpcOptions() {
  SettingsIpcOptions options;
  options.pipe_name = azookey::ipc::DefaultPipeName();
  return options;
}

SettingsIpcResult NotifyHostOfSettingsChange(const SettingsIpcOptions& options) {
  HostSession session;
  switch (session.Open(options)) {
    case OpenFailure::None:
      break;
    case OpenFailure::NoPipe:
      return Failure("could not resolve the per-user IPC pipe");
    case OpenFailure::NoToken:
      return Failure("settings were saved, but the IPC token is unavailable");
    case OpenFailure::NoTraceId:
      return Failure("settings were saved, but the IPC trace ID is unavailable");
    case OpenFailure::NotRunning:
      return Failure("settings were saved, but the inference host is not running");
    case OpenFailure::HandshakeNotSent:
      return Failure("settings were saved, but the host handshake could not be sent");
    case OpenFailure::HandshakeTimeout:
      return Failure("settings were saved, but the host handshake timed out");
    case OpenFailure::HandshakeRejected:
      return Failure("settings were saved, but the host rejected the handshake");
  }

  bool sent = false;
  const auto reply =
      session.Exchange(MessageType::UpdateConfig, "{}", options.response_timeout_ms, &sent);
  if (!sent) return Failure("settings were saved, but UpdateConfig could not be sent");
  if (!reply) return Failure("settings were saved, but UpdateConfig timed out");
  const auto payload = azookey::ipc::ParseUpdateConfigResponse(*reply);
  if (!payload)
    return Failure("settings were saved, but UpdateConfig returned an invalid response");
  if (!payload->ok) {
    return Failure(payload->error.value_or("settings were saved, but the host rejected them"));
  }

  SettingsIpcResult result;
  result.ok = true;
  return result;
}

HostCallStatus ProbeHostCapability(const SettingsIpcOptions& options, const char* capability) {
  HostSession session;
  if (const auto failure = session.Open(options); failure != OpenFailure::None) {
    return StatusFor(failure);
  }
  return session.HasCapability(capability) ? HostCallStatus::Ok : HostCallStatus::Unsupported;
}

HostCallResult<azookey::ipc::ListModelsResponse> RequestListModels(
    const SettingsIpcOptions& options, const azookey::ipc::ListModelsRequest& request) {
  return Call<azookey::ipc::ListModelsResponse>(
      options, kCapabilityListModels, MessageType::ListModels,
      azookey::ipc::BuildListModelsRequest(request), options.response_timeout_ms,
      azookey::ipc::ParseListModelsResponse);
}

HostCallResult<azookey::ipc::BenchmarkModelResponse> RequestBenchmarkModel(
    const SettingsIpcOptions& options, const azookey::ipc::BenchmarkModelRequest& request) {
  return Call<azookey::ipc::BenchmarkModelResponse>(
      options, kCapabilityBenchmarkModel, MessageType::BenchmarkModel,
      azookey::ipc::BuildBenchmarkModelRequest(request), options.long_response_timeout_ms,
      azookey::ipc::ParseBenchmarkModelResponse);
}

HostCallResult<azookey::ipc::ListLearningEntriesResponse> RequestListLearningEntries(
    const SettingsIpcOptions& options, const azookey::ipc::ListLearningEntriesRequest& request) {
  return Call<azookey::ipc::ListLearningEntriesResponse>(
      options, kCapabilityLearningData, MessageType::ListLearningEntries,
      azookey::ipc::BuildListLearningEntriesRequest(request), options.response_timeout_ms,
      azookey::ipc::ParseListLearningEntriesResponse);
}

HostCallResult<azookey::ipc::ForgetLearningEntryResponse> RequestForgetLearningEntry(
    const SettingsIpcOptions& options, const azookey::ipc::ForgetLearningEntryRequest& request) {
  return Call<azookey::ipc::ForgetLearningEntryResponse>(
      options, kCapabilityLearningData, MessageType::ForgetLearningEntry,
      azookey::ipc::BuildForgetLearningEntryRequest(request), options.response_timeout_ms,
      azookey::ipc::ParseForgetLearningEntryResponse);
}

HostCallResult<azookey::ipc::ExportLearningDataResponse> RequestExportLearningData(
    const SettingsIpcOptions& options, const azookey::ipc::ExportLearningDataRequest& request) {
  return Call<azookey::ipc::ExportLearningDataResponse>(
      options, kCapabilityLearningData, MessageType::ExportLearningData,
      azookey::ipc::BuildExportLearningDataRequest(request), options.long_response_timeout_ms,
      azookey::ipc::ParseExportLearningDataResponse);
}

HostCallResult<azookey::ipc::ImportLearningDataResponse> RequestImportLearningData(
    const SettingsIpcOptions& options, const azookey::ipc::ImportLearningDataRequest& request) {
  return Call<azookey::ipc::ImportLearningDataResponse>(
      options, kCapabilityLearningData, MessageType::ImportLearningData,
      azookey::ipc::BuildImportLearningDataRequest(request), options.long_response_timeout_ms,
      azookey::ipc::ParseImportLearningDataResponse);
}

HostCallResult<azookey::ipc::ResetLearningStoreResponse> RequestResetLearningStore(
    const SettingsIpcOptions& options, const azookey::ipc::ResetLearningStoreRequest& request) {
  return Call<azookey::ipc::ResetLearningStoreResponse>(
      options, kCapabilityLearningReset, MessageType::ResetLearningStore,
      azookey::ipc::BuildResetLearningStoreRequest(request), options.long_response_timeout_ms,
      azookey::ipc::ParseResetLearningStoreResponse);
}

HostCallResult<azookey::ipc::QueryPersonaResponse> RequestQueryPersona(
    const SettingsIpcOptions& options) {
  return Call<azookey::ipc::QueryPersonaResponse>(
      options, kCapabilityPersona, MessageType::QueryPersona, "{}", options.response_timeout_ms,
      azookey::ipc::ParseQueryPersonaResponse);
}

HostCallResult<azookey::ipc::DetectAnomaliesResponse> RequestDetectAnomalies(
    const SettingsIpcOptions& options, const azookey::ipc::DetectAnomaliesRequest& request) {
  return Call<azookey::ipc::DetectAnomaliesResponse>(
      options, kCapabilityDetectAnomalies, MessageType::DetectAnomalies,
      azookey::ipc::BuildDetectAnomaliesRequest(request), options.long_response_timeout_ms,
      azookey::ipc::ParseDetectAnomaliesResponse);
}

HostCallResult<azookey::ipc::QueryDiagnosticsPayload> RequestQueryDiagnostics(
    const SettingsIpcOptions& options) {
  return Call<azookey::ipc::QueryDiagnosticsPayload>(
      options, nullptr, MessageType::QueryDiagnostics, "{}", options.response_timeout_ms,
      azookey::ipc::ParseQueryDiagnostics);
}

}  // namespace azookey::settings
