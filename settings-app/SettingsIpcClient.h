#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

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
  // ImportUserDictionaryText, its preview included, reads and parses a whole file on the Host
  // (user-dictionary-pos-spec section 8).
  uint32_t user_dict_import_timeout_ms{30000};
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
inline constexpr const char* kCapabilityDetectAnomalies = "detect_anomalies";
inline constexpr const char* kCapabilityFetchCatalogModel = "fetch_catalog_model";
inline constexpr const char* kCapabilityCancelCatalogFetch = "cancel_catalog_fetch";
inline constexpr const char* kCapabilityUpdateFetch = "update_fetch";
// user-dictionary-pos-spec section 7.4: the part-of-speech field and the text import.
inline constexpr const char* kCapabilityUserDictPos = "user_dict_pos";
inline constexpr const char* kCapabilityUserDictTextImport = "user_dict_text_import";

// Connects and completes the Handshake only, to learn whether the Host advertises `capability`
// before the user is asked to confirm an operation. Ok means it does; Unsupported means it does
// not.
HostCallStatus ProbeHostCapability(const SettingsIpcOptions& options, const char* capability);
// Connects and completes the Handshake only, and returns every capability the Host advertised,
// so one connection answers for several features. The list is set only when the status is Ok.
HostCallResult<std::vector<std::string>> RequestHostCapabilities(const SettingsIpcOptions& options);

// Each call connects, completes the Handshake, sends one request and waits for its response.
// With include_catalog the Host may hash the catalog's files first, so the call waits as long as
// a benchmark.
HostCallResult<azookey::ipc::ListModelsResponse> RequestListModels(
    const SettingsIpcOptions& options, const azookey::ipc::ListModelsRequest& request);
HostCallResult<azookey::ipc::BenchmarkModelResponse> RequestBenchmarkModel(
    const SettingsIpcOptions& options, const azookey::ipc::BenchmarkModelRequest& request);
// DEV-1821 (model-management-spec section 4.4): starts a catalog fetch on the Host, which
// answers at once; QueryCatalogFetch then reports its progress and result.
HostCallResult<azookey::ipc::CatalogFetchResponse> RequestFetchCatalogModel(
    const SettingsIpcOptions& options, const azookey::ipc::FetchCatalogModelRequest& request);
HostCallResult<azookey::ipc::CatalogFetchResponse> RequestQueryCatalogFetch(
    const SettingsIpcOptions& options);
// DEV-1840: asks the Host to stop its running fetch of request.model_id. The fetch then ends as
// failed / "cancelled", which RequestQueryCatalogFetch reports.
HostCallResult<azookey::ipc::CatalogFetchResponse> RequestCancelCatalogFetch(
    const SettingsIpcOptions& options, const azookey::ipc::CancelCatalogFetchRequest& request);
// DEV-1974 (sideload-packaging-spec section 6.3): starts the Host's download of the available
// update, which answers at once; QueryUpdateFetch then reports its progress and result, and
// CancelUpdateFetch stops it.
HostCallResult<azookey::ipc::UpdateFetchResponse> RequestFetchUpdate(
    const SettingsIpcOptions& options, const azookey::ipc::FetchUpdateRequest& request);
HostCallResult<azookey::ipc::UpdateFetchResponse> RequestQueryUpdateFetch(
    const SettingsIpcOptions& options);
HostCallResult<azookey::ipc::UpdateFetchResponse> RequestCancelUpdateFetch(
    const SettingsIpcOptions& options, const azookey::ipc::CancelUpdateFetchRequest& request);
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
// A word with a part of speech needs `user_dict_pos` in the Handshake of the same connection
// (user-dictionary-pos-spec section 7.1): an older Host would drop it silently, so the call returns
// Unsupported without sending. A noun needs no capability (every Host since M9 answers it), so a
// Host without a user dictionary replies ok=false with kUserDictErrorUnavailable. The edit may
// wait for the cross-process file lock, so the call waits as long as a reset.
HostCallResult<azookey::ipc::AddUserWordResponse> RequestAddUserWord(
    const SettingsIpcOptions& options, const azookey::ipc::AddUserWordRequest& request);
// Imports, or with dry_run previews, another IME's dictionary file (user-dictionary-pos-spec
// section 7.2). Waits user_dict_import_timeout_ms; a Timeout after the request was sent means the
// Host may still finish the import later.
HostCallResult<azookey::ipc::ImportUserDictionaryTextResponse> RequestImportUserDictionaryText(
    const SettingsIpcOptions& options,
    const azookey::ipc::ImportUserDictionaryTextRequest& request);
// Persona ratios (learning-data-management-spec §15); the request payload is an empty object.
HostCallResult<azookey::ipc::QueryPersonaResponse> RequestQueryPersona(
    const SettingsIpcOptions& options);
// Asks the Host's external AI backend for likely mistakes in a pasted text; the call waits as
// long as a benchmark, because the answer comes from a remote service.
HostCallResult<azookey::ipc::DetectAnomaliesResponse> RequestDetectAnomalies(
    const SettingsIpcOptions& options, const azookey::ipc::DetectAnomaliesRequest& request);
// QueryDiagnostics has no capability of its own: a Host that predates a field leaves it out of
// the response, so the caller reads the optional fields.
HostCallResult<azookey::ipc::QueryDiagnosticsPayload> RequestQueryDiagnostics(
    const SettingsIpcOptions& options);

}  // namespace azookey::settings
