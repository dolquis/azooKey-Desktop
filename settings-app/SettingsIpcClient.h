#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "azookey/ipc/Payloads.h"

namespace azookey::settings {

struct SettingsIpcOptions {
  std::string pipe_name;
  std::string handshake_token;
  uint32_t connect_timeout_ms{1000};
  uint32_t response_timeout_ms{3000};
  // BenchmarkModel runs up to 60 seconds on the Host (model-management-spec section 4.2), and
  // an export or import writes a whole archive, so those wait longer than a settings reload.
  uint32_t long_response_timeout_ms{90000};
};

struct SettingsIpcResult {
  bool ok{false};
  std::optional<std::string> error;
};

SettingsIpcOptions DefaultSettingsIpcOptions();
SettingsIpcResult NotifyHostOfSettingsChange(const SettingsIpcOptions& options);

// Why a request to the Host produced no response payload. Anything the Host answered, including
// its own `ok = false` or `status = "error"`, is `Ok` here and is read from the payload.
enum class HostCallStatus {
  Ok,
  // The pipe name, token or trace ID could not be resolved on this machine.
  Unavailable,
  HostNotRunning,
  HandshakeRejected,
  // The Host did not advertise the capability the request needs (an older Host).
  Unsupported,
  Timeout,
  InvalidResponse,
};

template <typename Response>
struct HostCallResult {
  HostCallStatus status{HostCallStatus::Unavailable};
  std::optional<Response> response;
};

// Capabilities a Host advertises in its Handshake response.
inline constexpr const char* kCapabilityListModels = "list_models";
inline constexpr const char* kCapabilityBenchmarkModel = "benchmark_model";
inline constexpr const char* kCapabilityLearningData = "learning_data_management";
inline constexpr const char* kCapabilityLearningReset = "learning_reset";
inline constexpr const char* kCapabilityPersona = "persona";

// Each call connects, completes the Handshake, sends one request and waits for its response.
HostCallResult<azookey::ipc::ListModelsResponse> RequestListModels(
    const SettingsIpcOptions& options, const azookey::ipc::ListModelsRequest& request);
HostCallResult<azookey::ipc::BenchmarkModelResponse> RequestBenchmarkModel(
    const SettingsIpcOptions& options, const azookey::ipc::BenchmarkModelRequest& request);
HostCallResult<azookey::ipc::ListLearningEntriesResponse> RequestListLearningEntries(
    const SettingsIpcOptions& options, const azookey::ipc::ListLearningEntriesRequest& request);
HostCallResult<azookey::ipc::ForgetLearningEntryResponse> RequestForgetLearningEntry(
    const SettingsIpcOptions& options, const azookey::ipc::ForgetLearningEntryRequest& request);
HostCallResult<azookey::ipc::ExportLearningDataResponse> RequestExportLearningData(
    const SettingsIpcOptions& options, const azookey::ipc::ExportLearningDataRequest& request);
HostCallResult<azookey::ipc::ImportLearningDataResponse> RequestImportLearningData(
    const SettingsIpcOptions& options, const azookey::ipc::ImportLearningDataRequest& request);
HostCallResult<azookey::ipc::ResetLearningStoreResponse> RequestResetLearningStore(
    const SettingsIpcOptions& options, const azookey::ipc::ResetLearningStoreRequest& request);
// Persona ratios (rich-features-spec X-2-7); the request payload is an empty object.
HostCallResult<azookey::ipc::QueryPersonaResponse> RequestQueryPersona(
    const SettingsIpcOptions& options);
// QueryDiagnostics has no capability of its own: a Host that predates a field leaves it out of
// the response, so the caller reads the optional fields.
HostCallResult<azookey::ipc::QueryDiagnosticsPayload> RequestQueryDiagnostics(
    const SettingsIpcOptions& options);

}  // namespace azookey::settings
