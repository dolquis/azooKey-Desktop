#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace azookey::ipc {

inline constexpr int kHandshakeProtocolVersion = 1;

struct HandshakeRequest {
  std::string tip_version;
  int protocol_version{kHandshakeProtocolVersion};
  std::vector<std::string> capabilities;
  std::string client_id;
  std::string handshake_token;
};

struct HandshakeResponse {
  std::string host_version;
  int protocol_version{kHandshakeProtocolVersion};
  bool accepted{false};
  bool model_loaded{false};
  std::string host_generation_id;
  std::vector<std::string> capabilities;
  bool batch_romaji_conversion{false};
  std::string batch_romaji_preview_style{"kana"};
  std::string batch_conversion_mode{"neural"};
  bool batch_auto_punctuation{false};
  bool number_rewriter{false};
  bool katakana_rewriter{false};
  uint32_t max_candidates{9};
  bool symbol_rewriter{false};
  bool emoji_rewriter{false};
  bool emoji_trigger_search{true};
  uint32_t emoji_max_candidates{12};
  uint32_t emoji_trigger_min_query_length{1};
};

struct PingPayload {
  uint64_t nonce{};
  uint64_t t_ms{};
};

struct HealthPayload {
  std::string status;   // "ok" | "degraded" | "error"
  std::string backend;  // "cpu" | "cuda" | "directml"
  bool model_loaded{false};
  std::optional<uint32_t> vram_mb;
  std::optional<std::string> last_error;
};

// State of the neologd_lexicon layer in this Host process
// (auto-word-registration-spec section 15.14). The Host handles the pack only
// at startup, so a consent given after startup stays "not_requested" until
// the Host restarts.
inline constexpr std::string_view kNeologdLayerNotRequested = "not_requested";
inline constexpr std::string_view kNeologdLayerLoading = "loading";
inline constexpr std::string_view kNeologdLayerReady = "ready";
inline constexpr std::string_view kNeologdLayerMissingPack = "missing_pack";
inline constexpr std::string_view kNeologdLayerError = "error";

struct NeologdLayerStatus {
  std::string state{kNeologdLayerNotRequested};
  // Fixed failure class from the neologd_pack_load log; never a path or a
  // server response. Present only for "error".
  std::optional<std::string> reason;
};

struct QueryDiagnosticsPayload {
  bool model_loaded{false};
  std::optional<std::string> loaded_model_path;
  std::string engine;
  std::string backend;
  uint64_t rss_mb{};
  std::optional<std::string> ep;
  std::optional<std::string> ep_state;
  std::optional<std::string> ep_last_error;
  uint64_t learning_entries{};
  uint64_t user_dict_entries{};
  std::string fallback_state;
  std::optional<std::string> last_error;
  // Protocol v1 additive field (DEV-1534). Absent from Hosts that predate it.
  std::optional<NeologdLayerStatus> neologd_layer;
};

struct LoadModelRequest {
  std::string path;
  std::string backend;  // "cpu" | "cuda" | "directml"
  std::optional<int32_t> n_gpu_layers;
};

struct LoadModelResponse {
  bool ok{false};
  std::optional<std::string> error;
};

struct CandidateField {
  std::string surface;
  std::string reading;
  double score{};
  std::string source;
  std::string description;
  // core::CandidateTag value (docs/rich-features-spec.md X-2-3). Omitted on the
  // wire when 0 (None); unknown values are carried through unchanged.
  uint8_t tag{0};
};

// Foreground application identity (docs/app-profile-spec.md section 3.1). The
// Host resolves the profile itself; titles are never sent.
struct AppIdentity {
  std::string process_name;
  std::string window_class;
};

struct QueryCandidatesRequest {
  std::string reading;
  std::string left_context;
  uint32_t max_candidates{10};
  bool live{false};
  bool auto_punctuation{false};
  std::string punctuation_style{"ja"};
  std::string emoji_trigger;
  // Missing or invalid event privacy is denied (protocol v1 additive fields).
  bool secure{true};
  bool learning_allowed{false};
  // Absent from older clients and when the TIP could not identify the app;
  // either way the Host applies the global settings without tag boosts.
  std::optional<AppIdentity> app;
  // M60 (docs/inline-english-candidate-spec.md section 6.2): the romaji as
  // typed, kept apart from reading, and the TIP's inlineEnglishCandidates.
  // Both are omitted on the wire when empty / false.
  std::string raw_romaji;
  bool english_candidates{false};
};

// Offsets count UTF-16 code units in candidates[0].surface. Surface and reading
// are carried separately so learning never slices UTF-8 with those offsets.
struct LiveSegment {
  uint32_t start_char{};
  uint32_t end_char{};
  double score{};
  bool auto_punctuation{false};
  std::string surface;
  std::string reading;
  uint8_t pos{};
  uint8_t head_pos{};
  uint8_t sem{};
  uint8_t head_sem{};
};

struct QueryCandidatesResponse {
  std::vector<CandidateField> candidates;
  std::vector<LiveSegment> segments;
  bool partial{false};
  bool ok{true};
  std::optional<std::string> error;
  // M35 auto_replace: the reading the host substituted for the requested one.
  // Empty means no correction was applied, which is also how an older host that
  // omits the field decodes.
  std::string corrected_reading;
};

// request_id travels in the Envelope, as with QueryCandidates.
struct QueryLiveConversionRequest {
  std::string kana;
  std::string context;
};

struct QueryLiveConversionResponse {
  std::string surface;
  double confidence{};  // Normalized to [0.0, 1.0].
};

// request_id travels in the Envelope. Phase 5 supports only mode="word".
struct QueryPredictionsRequest {
  std::string kana;
  std::string left_side_context;
  std::string mode{"word"};
  std::optional<AppIdentity> app;  // As QueryCandidatesRequest::app.
};

struct QueryPredictionsResponse {
  std::vector<CandidateField> predictions;
  bool ok{true};
  std::optional<std::string> error;
};

struct BatchConversionSegment {
  std::string reading;
  std::vector<CandidateField> candidates;
};

struct QueryBatchConversionRequest {
  std::string reading;
  std::string raw_romaji;
  std::string mode{"neural"};
  bool auto_punctuation{false};
  uint32_t max_candidates{10};
  bool ai_allowed{false};
  bool external_ai_allowed{false};
  std::string ai_backend;  // Empty preserves the host's root setting for older clients.
};

struct QueryBatchConversionResponse {
  std::vector<BatchConversionSegment> segments;
  std::string full_surface;
  bool partial{false};
  bool canceled{false};
  std::optional<std::string> error_class;
};

struct ReverseConvertRequest {
  std::string surface;
};

struct ReverseConvertResponse {
  // Empty reading with confidence 0 means no matching dictionary entry.
  std::string reading;
  double confidence{};
};

struct CancelPayload {
  uint64_t target_request_id{};
};

struct CommitObservationRequest {
  std::string reading;
  CandidateField chosen;
  std::vector<CandidateField> shown;
  std::string left_context;
  uint64_t timestamp_ms{};
  // Idempotency key for at-least-once delivery (DEV-554). Unique per TIP
  // instance and stable across resends of the same commit, so a Host that
  // already applied the observation can ignore the duplicate. Empty means the
  // sender does not support dedupe; privacy authorization is still required.
  std::string observation_id;
  // Missing or invalid event privacy is denied (protocol v1 additive fields).
  bool secure{true};
  bool learning_allowed{false};
  // Foreground app at commit (M54 app rows, DEV-1184). Absent means unknown,
  // which records the global row.
  std::optional<AppIdentity> app;
};

struct CommitObservationResponse {
  bool ok{false};
};

struct ObservedSegment {
  std::string reading;
  CandidateField chosen;
  std::vector<CandidateField> shown;
  bool is_auto_punctuation{false};
};

struct CommitSegmentsObservationRequest {
  std::vector<ObservedSegment> segments;
  std::string left_context;
  uint64_t timestamp_ms{};
  std::string observation_id;
  // Missing or invalid event privacy is denied (protocol v1 additive fields).
  bool secure{true};
  bool learning_allowed{false};
  // Foreground app at commit (M54 app rows, DEV-1184). Absent means unknown,
  // which records the global row.
  std::optional<AppIdentity> app;
};

std::string BuildCommitSegmentsObservationRequest(const CommitSegmentsObservationRequest& p);
std::optional<CommitSegmentsObservationRequest> ParseCommitSegmentsObservationRequest(
    const std::string& json);

// Correction of a commit (user-learning-enhancement-spec section 4, DEV-1529).
// "undo" is an immediate Backspace right after the commit: the rejected
// surface is penalized and nothing is accepted. "reconvert" replaces the
// rejected surface with selected_surface. Sent only to Hosts that advertise
// the "commit_correction" capability; the answer is CommitObservationResponse.
inline constexpr std::string_view kCorrectionKindUndo = "undo";
inline constexpr std::string_view kCorrectionKindReconvert = "reconvert";

struct CommitCorrectionRequest {
  std::string kind;
  std::string reading;
  std::string rejected_surface;
  // Required for "reconvert" and must differ from rejected_surface; absent
  // for "undo".
  std::optional<std::string> selected_surface;
  std::string left_context;
  uint64_t timestamp_ms{};
  std::string observation_id;
  // Missing or invalid event privacy is denied.
  bool secure{true};
  bool learning_allowed{false};
  std::optional<AppIdentity> app;
};

std::string BuildCommitCorrectionRequest(const CommitCorrectionRequest& p);
std::optional<CommitCorrectionRequest> ParseCommitCorrectionRequest(const std::string& json);

struct AddUserWordRequest {
  std::string word;
  std::string ruby;
  std::optional<int32_t> cid;
  std::optional<int32_t> mid;
  std::optional<double> value;
};

struct AddUserWordResponse {
  bool ok{false};
  std::optional<std::string> generated_id;
};

struct RemoveUserWordRequest {
  std::string word;
  std::string ruby;
};

struct RemoveUserWordResponse {
  bool ok{false};
};

struct UpdateConfigResponse {
  bool ok{false};
  std::optional<std::string> error;
};

// M35. Fire-and-forget: the TIP does not wait for a reply, and the response
// exists so the handler has a shape to build when one is ever needed.
struct ObserveTypoRequest {
  std::string wrong_reading;
  std::string correct_reading;
  uint64_t timestamp_ms{};
  // Missing or invalid event privacy is denied (protocol v1 additive fields).
  bool secure{true};
  bool learning_allowed{false};
};

struct ObserveTypoResponse {
  bool ok{false};
};

// M36-A approval flow.
struct ListNewWordCandidatesRequest {
  // One of "pending", "confirmed", "rejected". An unknown value is rejected by
  // the parser rather than silently listing everything.
  std::string state_filter{"pending"};
  uint32_t max_items{50};
};

struct NewWordField {
  std::string surface;
  std::string reading;
  std::string source;
  std::string state;
  uint32_t count{0};
  uint64_t last_seen_epoch{0};
};

// Error codes carried in the `error` field of the two M36-A responses
// (docs/auto-word-registration-spec.md section 7-1).
inline constexpr std::string_view kNewWordErrorInvalidRequest = "invalid_request";
inline constexpr std::string_view kNewWordErrorStoreUnavailable = "store_unavailable";
inline constexpr std::string_view kNewWordErrorNotAuthenticated = "not_authenticated";
inline constexpr std::string_view kNewWordErrorNotFound = "not_found";
inline constexpr std::string_view kNewWordErrorSaveFailed = "save_failed";

struct ListNewWordCandidatesResponse {
  // false with `error` set when the list could not be produced; an empty
  // `items` with ok=true means there really are no words in that state.
  // Absent on the wire means true (protocol v1 additive field).
  bool ok{true};
  std::optional<std::string> error;
  std::vector<NewWordField> items;
};

struct ResolveNewWordRequest {
  std::string surface;
  std::string reading;
  // "confirm" or "reject"; any other value fails the parse.
  std::string action;
};

struct ResolveNewWordResponse {
  // true when the word is in the requested state after the call and that state
  // is on disk. Re-confirming a confirmed word is ok=true, changed=false.
  bool ok{false};
  // true only when this call moved the word to a new state. Absent means false.
  bool changed{false};
  std::optional<std::string> error;
};

// Builders return the JSON payload string (Envelope.payload_json content).
std::string BuildHandshakeRequest(const HandshakeRequest& p);
std::string BuildHandshakeResponse(const HandshakeResponse& p);
std::string BuildPing(const PingPayload& p);
std::string BuildHealth(const HealthPayload& p);
std::string BuildQueryDiagnostics(const QueryDiagnosticsPayload& p);
std::string BuildLoadModelRequest(const LoadModelRequest& p);
std::string BuildLoadModelResponse(const LoadModelResponse& p);
std::string BuildQueryCandidatesRequest(const QueryCandidatesRequest& p);
std::string BuildQueryCandidatesResponse(const QueryCandidatesResponse& p);
std::string BuildQueryLiveConversionRequest(const QueryLiveConversionRequest& p);
std::string BuildQueryLiveConversionResponse(const QueryLiveConversionResponse& p);
std::string BuildQueryPredictionsRequest(const QueryPredictionsRequest& p);
std::string BuildQueryPredictionsResponse(const QueryPredictionsResponse& p);
std::string BuildQueryBatchConversionRequest(const QueryBatchConversionRequest& p);
std::string BuildQueryBatchConversionResponse(const QueryBatchConversionResponse& p);
std::string BuildReverseConvertRequest(const ReverseConvertRequest& p);
std::string BuildReverseConvertResponse(const ReverseConvertResponse& p);
std::string BuildCancel(const CancelPayload& p);
std::string BuildCommitObservationRequest(const CommitObservationRequest& p);
std::string BuildCommitObservationResponse(const CommitObservationResponse& p);
std::string BuildAddUserWordRequest(const AddUserWordRequest& p);
std::string BuildAddUserWordResponse(const AddUserWordResponse& p);
std::string BuildRemoveUserWordRequest(const RemoveUserWordRequest& p);
std::string BuildRemoveUserWordResponse(const RemoveUserWordResponse& p);
std::string BuildUpdateConfigResponse(const UpdateConfigResponse& p);
std::string BuildObserveTypoRequest(const ObserveTypoRequest& p);
std::string BuildObserveTypoResponse(const ObserveTypoResponse& p);
std::string BuildListNewWordCandidatesRequest(const ListNewWordCandidatesRequest& p);
std::string BuildListNewWordCandidatesResponse(const ListNewWordCandidatesResponse& p);
std::string BuildResolveNewWordRequest(const ResolveNewWordRequest& p);
std::string BuildResolveNewWordResponse(const ResolveNewWordResponse& p);

// Parsers accept the JSON payload string (Envelope.payload_json content).
std::optional<HandshakeRequest> ParseHandshakeRequest(const std::string& json);
std::optional<HandshakeResponse> ParseHandshakeResponse(const std::string& json);
std::optional<PingPayload> ParsePing(const std::string& json);
std::optional<HealthPayload> ParseHealth(const std::string& json);
std::optional<QueryDiagnosticsPayload> ParseQueryDiagnostics(const std::string& json);
std::optional<LoadModelRequest> ParseLoadModelRequest(const std::string& json);
std::optional<LoadModelResponse> ParseLoadModelResponse(const std::string& json);
std::optional<QueryCandidatesRequest> ParseQueryCandidatesRequest(const std::string& json);
std::optional<QueryCandidatesResponse> ParseQueryCandidatesResponse(const std::string& json);
std::optional<QueryLiveConversionRequest> ParseQueryLiveConversionRequest(const std::string& json);
std::optional<QueryLiveConversionResponse> ParseQueryLiveConversionResponse(
    const std::string& json);
std::optional<QueryPredictionsRequest> ParseQueryPredictionsRequest(const std::string& json);
std::optional<QueryPredictionsResponse> ParseQueryPredictionsResponse(const std::string& json);
std::optional<QueryBatchConversionRequest> ParseQueryBatchConversionRequest(
    const std::string& json);
std::optional<QueryBatchConversionResponse> ParseQueryBatchConversionResponse(
    const std::string& json);
std::optional<ReverseConvertRequest> ParseReverseConvertRequest(const std::string& json);
std::optional<ReverseConvertResponse> ParseReverseConvertResponse(const std::string& json);
std::optional<CancelPayload> ParseCancel(const std::string& json);
std::optional<CommitObservationRequest> ParseCommitObservationRequest(const std::string& json);
std::optional<CommitObservationResponse> ParseCommitObservationResponse(const std::string& json);
std::optional<AddUserWordRequest> ParseAddUserWordRequest(const std::string& json);
std::optional<AddUserWordResponse> ParseAddUserWordResponse(const std::string& json);
std::optional<RemoveUserWordRequest> ParseRemoveUserWordRequest(const std::string& json);
std::optional<RemoveUserWordResponse> ParseRemoveUserWordResponse(const std::string& json);
std::optional<UpdateConfigResponse> ParseUpdateConfigResponse(const std::string& json);
std::optional<ObserveTypoRequest> ParseObserveTypoRequest(const std::string& json);
std::optional<ObserveTypoResponse> ParseObserveTypoResponse(const std::string& json);
std::optional<ListNewWordCandidatesRequest> ParseListNewWordCandidatesRequest(
    const std::string& json);
std::optional<ListNewWordCandidatesResponse> ParseListNewWordCandidatesResponse(
    const std::string& json);
std::optional<ResolveNewWordRequest> ParseResolveNewWordRequest(const std::string& json);
std::optional<ResolveNewWordResponse> ParseResolveNewWordResponse(const std::string& json);

// ---------------------------------------------------------------------------
// M45 model management: ListModels / BenchmarkModel (DEV-1191,
// docs/model-management-spec.md section 4). Later additions append their own
// section below rather than interleaving with this one.

struct ListModelsRequest {
  // Empty means the Host's models directory. Anything else must resolve inside
  // it; "%LOCALAPPDATA%" at the start is expanded.
  std::string directory;
  bool compute_sha256{false};
};

struct ListedModelMetadata {
  std::string model_family;  // Empty when unknown; omitted on the wire.
  std::string quantization;  // Empty when unknown; omitted on the wire.
  uint64_t n_params{};       // 0 when unknown; omitted on the wire.
};

struct ListedModel {
  std::string path;       // UTF-8 absolute path; the directory for onnx_genai.
  std::string file_name;  // File or directory name.
  std::string format;     // "gguf" | "onnx_genai".
  uint64_t size_bytes{};  // File size, or the directory total for onnx_genai.
  bool valid{false};      // Wire also carries gguf_valid for gguf entries.
  ListedModelMetadata metadata;
  std::string sha256;            // Lowercase hex, only when computed.
  std::string last_load_status;  // "success" | "failed" | "not_loaded".
  std::string last_error;        // Fixed validation/load category; omitted when empty.
};

struct ListModelsResponse {
  std::vector<ListedModel> models;
  bool ok{true};
  std::optional<std::string> error;
};

struct BenchmarkModelRequest {
  std::string path;
  std::string backend{"cpu"};
  std::vector<std::string> cases;  // Readings; empty means the Host's defaults.
  uint32_t iterations{50};
  uint32_t warmup{5};
};

struct BenchmarkModelResponse {
  std::string backend;
  double p50_ms{};
  double p95_ms{};
  double p99_ms{};
  double load_ms{};
  double rss_mb{};
  std::optional<double> vram_mb;  // null when the backend cannot measure it.
  std::string status{"error"};    // "success" | "timeout" | "error".
  uint32_t iterations_completed{};
  std::optional<std::string> error;  // null on success.
};

std::string BuildListModelsRequest(const ListModelsRequest& p);
std::string BuildListModelsResponse(const ListModelsResponse& p);
std::string BuildBenchmarkModelRequest(const BenchmarkModelRequest& p);
std::string BuildBenchmarkModelResponse(const BenchmarkModelResponse& p);
std::optional<ListModelsRequest> ParseListModelsRequest(const std::string& json);
std::optional<ListModelsResponse> ParseListModelsResponse(const std::string& json);
std::optional<BenchmarkModelRequest> ParseBenchmarkModelRequest(const std::string& json);
std::optional<BenchmarkModelResponse> ParseBenchmarkModelResponse(const std::string& json);

// ---------------------------------------------------------------------------
// M49 learning data management: ListLearningEntries / ForgetLearningEntry /
// ExportLearningData / ImportLearningData (DEV-1190,
// docs/learning-data-management-spec.md section 4).

// Error codes carried in the `error` field of the four responses. Archive
// failures use the BackupErrorCode strings ("invalid_path", "decrypt_failed",
// ...) instead.
inline constexpr std::string_view kLearningDataErrorInvalidRequest = "invalid_request";
inline constexpr std::string_view kLearningDataErrorNotAuthenticated = "not_authenticated";
inline constexpr std::string_view kLearningDataErrorStoreUnavailable = "store_unavailable";
inline constexpr std::string_view kLearningDataErrorSaveFailed = "save_failed";
inline constexpr std::string_view kLearningDataErrorUnsupported = "unsupported";

struct ListLearningEntriesRequest {
  // "learning" | "user_dict" | "typo" | "auto_word"; required.
  std::string store;
  std::string query;  // Substring of reading or surface; empty matches all.
  uint32_t limit{100};
  uint32_t offset{0};
};

struct LearningEntryField {
  std::string id;
  std::string channel;  // Learning channel ("kana" / "english"); omitted when empty.
  std::string reading;
  std::string surface;
  double weight{};
  uint64_t last_updated_epoch_sec{};
  std::vector<std::string> tags;
  std::map<std::string, std::string> metadata;
};

struct ListLearningEntriesResponse {
  bool ok{true};
  std::optional<std::string> error;
  uint64_t total{};
  std::vector<LearningEntryField> entries;
};

// Exactly one form: `id` (settings app), or `reading` + `surface` with
// store "learning" (the TIP's Ctrl+Shift+Backspace, legacy-parity-spec 7.2).
struct ForgetLearningEntryRequest {
  std::string store;
  std::string id;
  std::string reading;
  std::string surface;
};

struct ForgetLearningEntryResponse {
  bool ok{true};
  // true when an entry matched and is now forgotten (on disk, for learning).
  bool removed{false};
  std::optional<std::string> error;
};

struct ExportLearningDataRequest {
  std::vector<std::string> stores;
  std::string destination_path;  // UTF-8 absolute path ending in .zip.
  bool encrypt{true};
  bool include_settings{false};  // Not supported yet: true is rejected.
};

struct LearningBackupItemField {
  std::string name;
  std::string file;
  uint64_t count{};
  std::string sha256;
};

struct ExportLearningDataResponse {
  // "success" or "error"; `error` carries the reason.
  std::string status{"error"};
  std::optional<std::string> error;
  uint64_t file_size_bytes{};
  bool encrypted{true};
  std::vector<LearningBackupItemField> items;
};

struct ImportLearningDataRequest {
  std::string source_path;                   // UTF-8 absolute path ending in .zip.
  std::string conflict_resolution{"merge"};  // "merge" | "overwrite" | "keep_both".
  std::vector<std::string> stores;
};

struct ImportLearningDataResponse {
  std::string status{"error"};
  std::optional<std::string> error;
  // Keyed by archive item name ("learning", "user_dictionary", ...).
  std::map<std::string, uint64_t> imported_counts;
  std::map<std::string, uint64_t> skipped_counts;
  std::map<std::string, uint64_t> conflict_counts;
};

std::string BuildListLearningEntriesRequest(const ListLearningEntriesRequest& p);
std::string BuildListLearningEntriesResponse(const ListLearningEntriesResponse& p);
std::string BuildForgetLearningEntryRequest(const ForgetLearningEntryRequest& p);
std::string BuildForgetLearningEntryResponse(const ForgetLearningEntryResponse& p);
std::string BuildExportLearningDataRequest(const ExportLearningDataRequest& p);
std::string BuildExportLearningDataResponse(const ExportLearningDataResponse& p);
std::string BuildImportLearningDataRequest(const ImportLearningDataRequest& p);
std::string BuildImportLearningDataResponse(const ImportLearningDataResponse& p);
std::optional<ListLearningEntriesRequest> ParseListLearningEntriesRequest(const std::string& json);
std::optional<ListLearningEntriesResponse> ParseListLearningEntriesResponse(
    const std::string& json);
std::optional<ForgetLearningEntryRequest> ParseForgetLearningEntryRequest(const std::string& json);
std::optional<ForgetLearningEntryResponse> ParseForgetLearningEntryResponse(
    const std::string& json);
std::optional<ExportLearningDataRequest> ParseExportLearningDataRequest(const std::string& json);
std::optional<ExportLearningDataResponse> ParseExportLearningDataResponse(const std::string& json);
std::optional<ImportLearningDataRequest> ParseImportLearningDataRequest(const std::string& json);
std::optional<ImportLearningDataResponse> ParseImportLearningDataResponse(const std::string& json);

// Whole-store reset (learning-data-management-spec section 4.6). Capability
// "learning_reset". Errors are the kLearningDataError* codes.
struct ResetLearningStoreRequest {
  // "learning" | "user_dict" | "typo" | "auto_word"; required.
  std::string store;
};

struct ResetLearningStoreResponse {
  bool ok{true};
  std::optional<std::string> error;
};

std::string BuildResetLearningStoreRequest(const ResetLearningStoreRequest& p);
std::string BuildResetLearningStoreResponse(const ResetLearningStoreResponse& p);
std::optional<ResetLearningStoreRequest> ParseResetLearningStoreRequest(const std::string& json);
std::optional<ResetLearningStoreResponse> ParseResetLearningStoreResponse(const std::string& json);

// Persona ratios (rich-features-spec X-2-7). Capability "persona"; the request
// payload is an empty object. Only the four ratios and their basis are sent,
// never a surface. A failure carries kLearningDataErrorNotAuthenticated or
// kLearningDataErrorStoreUnavailable.
struct QueryPersonaResponse {
  bool ok{true};
  std::optional<std::string> error;
  double polite_ratio{};
  double casual_ratio{};
  double technical_ratio{};
  double kaomoji_ratio{};
  // Commits the ratios were computed from; 0 means there is no data yet.
  uint64_t sample_count{};
  uint64_t computed_at_epoch_sec{};
};

std::string BuildQueryPersonaResponse(const QueryPersonaResponse& p);
std::optional<QueryPersonaResponse> ParseQueryPersonaResponse(const std::string& json);

}  // namespace azookey::ipc
