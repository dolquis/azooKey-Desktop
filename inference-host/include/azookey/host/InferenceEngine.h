#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "azookey/core/AppProfileResolver.h"
#include "azookey/core/EtwLogger.h"
#include "azookey/core/IConverter.h"
#include "azookey/host/AiBackend.h"
#include "azookey/host/CandidateTags.h"
#include "azookey/host/EnglishCandidates.h"
#include "azookey/host/EnglishDictionaryStore.h"
#include "azookey/host/HealthStateMachine.h"
#include "azookey/host/LearningDataManager.h"
#include "azookey/host/NllScorer.h"
#include "azookey/host/RewriterData.h"
#include "azookey/host/UserLearningScorer.h"
#include "azookey/host/ZenzaiDecodeStats.h"
#include "azookey/ipc/Payloads.h"
#include "azookey/learning/AutoWordStore.h"
#include "azookey/learning/DictionaryStore.h"
#include "azookey/learning/FileLock.h"
#include "azookey/learning/LearningStore.h"
#include "azookey/learning/Persona.h"
#include "azookey/learning/Reranker.h"
#include "azookey/learning/TypoCorrectionStore.h"
#include "azookey/learning/UserDictionary.h"
#include "azookey/logging/RuntimeLogger.h"

namespace azookey::host {

// Removes candidates whose surface holds a Private Use Area character (U+E000..U+F8FF), the
// last filter before a candidate list is returned. The rest keep their order.
void DropPrivateUseCandidates(std::vector<core::Candidate>& candidates);

// For a `reading` that still holds ASCII letters, removes the model candidates whose surface
// mixes kana with ASCII letters (`あっpエ`). Surfaces with only one of the two, kanji with letters,
// and candidates from any other source stay. Applies whether or not English candidates are on.
// english_ranges are the byte ranges of `reading` that M63 English segments occupy
// (docs/mixed-english-auto-conversion-spec.md section 7): their letters do not count, and the
// surface is judged without the first occurrence, in order, of each segment's text.
void DropMixedKanaAsciiModelCandidates(
    std::vector<core::Candidate>& candidates, const std::string& reading,
    const std::vector<std::pair<size_t, size_t>>& english_ranges = {});

// Removes the model candidates whose surface repeats one unit (1 to 4 characters) three times or
// more in a row where `reading` has no such repeat (`New!!!!!!`, `病院🏥🏥🏥`, `製品保証・・・・`,
// `明日明日明日` for あした). Kana are compared with hiragana and katakana folded together. Other
// sources and the order of the rest stay.
void DropRepeatedUnitModelCandidates(std::vector<core::Candidate>& candidates,
                                     const std::string& reading);

// M63-B (docs/mixed-english-auto-conversion-spec.md sections 7, 10 and 12): how a
// reading that holds M63 English segments is converted.
struct MixedConversionOptions {
  // Byte ranges of the reading that the English segments occupy (section 7).
  std::vector<std::pair<size_t, size_t>> english_ranges;
  // Section 10: the conversion ends by the earlier of this and the Zenzai budget.
  std::optional<std::chrono::steady_clock::time_point> deadline;
  // Section 12: false converts the reading as given, without M35.
  bool typo_correction{true};
  // false skips the NLL rerank (the section 8.3 extra conversions).
  bool nll{true};
};

struct InferenceTelemetry {
  uint64_t request_id{};
  core::EtwGuid client{};
  std::string_view trace_id{};
};

enum class BackendKind {
  Cpu,
  Cuda,
  Vulkan,
};

constexpr const char* BackendName(BackendKind backend) {
  switch (backend) {
    case BackendKind::Cpu:
      return "cpu";
    case BackendKind::Cuda:
      return "cuda";
    case BackendKind::Vulkan:
      return "vulkan";
  }
  return "cpu";
}

// M53 dictionary.*; Base remains available independently of these switches.
struct DictionaryLayerConfig {
  bool sudachi_enabled{true};
  bool neologd_enabled{false};
  bool named_entity_enabled{true};
  bool technical_terms_enabled{true};
  bool user_dictionary_enabled{true};
  bool auto_words_enabled{true};
  bool app_specific_dictionary_enabled{true};
};

// M25 power-saving lanes (docs/copilot-pc-backend-spec.md section 6.2). Fast: one Zenzai pass with
// a single beam. Heavy: the full beam and the extra passes on top of it (the NLL rerank).
enum class InferenceLane { Fast, Heavy };

struct EngineConfig {
  DictionaryLayerConfig dictionary;
  RewriterConfig rewriters;
  // M48 profilesByApp, published with the rest of the config so a query reads
  // one consistent snapshot without taking the UpdateConfig lock. Null means
  // no profiles: every app gets the global settings.
  std::shared_ptr<const core::AppProfileResolver> app_profiles;
  // M60 inline English candidates (settings inlineEnglish*).
  EnglishCandidateConfig english;
  NllConfig nll;
  BackendKind backend{BackendKind::Cpu};
  std::string model_path;
  std::optional<int32_t> n_gpu_layers;
  std::optional<int32_t> inference_threads;
  // Resolved by ApplyRuntimeSettingsToEngineConfig from the power source and powerProfile.
  InferenceLane inference_lane{InferenceLane::Heavy};
  uint32_t max_candidates{9};
  uint32_t max_context_length{30};
  bool enable_live_conversion{true};
  bool dynamic_punctuation{false};
  double segment_boundary_confidence{0.5};
  std::string punctuation_rules_path{"%LOCALAPPDATA%\\azooKey\\punctuation-rules.tsv"};
  double learning_alpha{0.8};
  // M54: rank with UserLearningScorer instead of the M7 reranker. Internal
  // switch, off until M52 calibrates the section 13 constants; not a
  // settings.json key.
  bool user_learning_scorer_enabled{false};
  size_t learning_flush_every_n{8};
  uint64_t learning_flush_interval_sec{5};
  size_t learning_max_records{10000};
  double learning_min_weight{0.05};
  // Internal prediction policy from user-learning-enhancement-spec.md section 14.6.
  // These values are runtime configuration, not persisted settings keys.
  size_t prediction_learning_max_entries{3};
  double prediction_learning_min_score{0.05};
  // Default score for user-dictionary entries that lack an explicit value.
  double user_word_default_score{1.5};
  // M35 typo correction; docs/typo-correction-learning-spec.md section 5-2.
  std::string typo_correction_mode{"suggest"};
  uint32_t typo_min_count{learning::kTypoCorrectionDefaultMinCount};
  // M36 mining and trending; docs/auto-word-registration-spec.md section 8.
  bool auto_word_mining_enabled{true};
  bool auto_word_trending_enabled{false};
  uint32_t auto_word_trending_interval_hours{24};
  bool auto_word_auto_register{false};
  uint32_t auto_word_min_count{3};
  // Score for a confirmed auto-word that carries none of its own (every mined
  // word). Below user_word_default_score so an explicit user entry for the same
  // surface wins the merge, and above a typical static-dictionary hit
  // (frequency + base priority 0.20 + exact bonus 0.10). Spec section 6.
  double auto_word_default_score{1.2};
};

struct ModelLoadOptions {
  std::string path;
  BackendKind backend{BackendKind::Cpu};
  std::optional<int32_t> n_gpu_layers;
  // Optional per-load override. Production callers pass the configured runtime value.
  std::optional<int32_t> n_threads;
  // Test-only fixture switch for no-llama builds; production callers leave this false.
  bool mock_zenzai_candidates_for_tests{false};
  // Test-only hook used to prove loading work runs outside state_mutex_.
  std::function<void()> before_probe_for_tests;
  // Test-only fault injection at each backend load attempt.
  std::function<void(BackendKind)> before_load_for_tests;
};

struct ModelLoadResult {
  bool ok{false};
  std::optional<std::string> error;
};

// DEV-1572: a user dictionary edit waits at most this long, from the receipt of
// the request, for the other edits of this Host and for the cross-process file
// lock together. The CLI's response wait (kPersistentCommandResponseTimeoutMs)
// is this plus the save's retry budgets plus one second.
inline constexpr std::chrono::milliseconds kUserDictionaryEditWaitBudget =
    learning::kDefaultFileLockTimeout;

enum class UserDictionaryEditResult {
  Ok,
  // Remove of a word the dictionary does not hold; nothing was written.
  NotFound,
  // No user dictionary is attached.
  Unavailable,
  // The wait budget ran out before the edit could be saved; nothing was written.
  Busy,
  LockFailed,
  LoadFailed,
  SaveFailed,
};

constexpr const char* UserDictionaryEditResultName(UserDictionaryEditResult result) {
  switch (result) {
    case UserDictionaryEditResult::Ok:
      return "ok";
    case UserDictionaryEditResult::NotFound:
      return "not_found";
    case UserDictionaryEditResult::Unavailable:
      return "unavailable";
    case UserDictionaryEditResult::Busy:
      return "busy";
    case UserDictionaryEditResult::LockFailed:
      return "lock_failed";
    case UserDictionaryEditResult::LoadFailed:
      return "load_failed";
    case UserDictionaryEditResult::SaveFailed:
      return "save_failed";
  }
  return "unavailable";
}

// Where the time of one edit went, for the Host's structured log. Durations
// are in milliseconds; a stage the edit did not reach stays 0.
struct UserDictionaryEditReport {
  UserDictionaryEditResult result{UserDictionaryEditResult::Unavailable};
  double writer_wait_ms{};
  double lock_wait_ms{};
  double load_ms{};
  double save_ms{};
  double total_ms{};
  bool load_skipped{false};
};

struct UserDictionaryEditOptions {
  // The wait budget is measured from here; the Dispatcher passes the receipt.
  std::chrono::steady_clock::time_point received{std::chrono::steady_clock::now()};
  std::chrono::milliseconds wait_budget{kUserDictionaryEditWaitBudget};
  UserDictionaryEditReport* report{nullptr};
};

struct EngineHealthSnapshot {
  BackendKind backend{BackendKind::Cpu};
  bool model_loaded{false};
  bool model_preload_in_progress{false};
  std::string model_path;
  std::optional<std::string> last_error;
  size_t learning_entries{};
  size_t user_dict_entries{};
  HealthState health_state{HealthState::Healthy};
};

class InferenceEngine {
 public:
  InferenceEngine(std::unique_ptr<core::IConverter> converter, learning::LearningStore* store,
                  EngineConfig config, logging::RuntimeLogger* runtime_logger = nullptr);
  ~InferenceEngine();

  // External, non-owning. May be nullptr (no user dictionary).
  void SetUserDictionary(learning::UserDictionary* dict);
  // External, non-owning; nullptr disables typo correction entirely (M35).
  void SetTypoStore(learning::TypoCorrectionStore* store);
  // External, non-owning; nullptr disables new-word mining entirely (M36-A).
  void SetAutoWordStore(learning::AutoWordStore* store);
  // M60: the English learning channel (UserDataPaths::english_learning_path).
  // nullptr disables English learning; candidates are still offered.
  void SetEnglishLearningStore(learning::LearningStore* store);

  // M60: English candidates for raw_romaji under the current config,
  // dictionary (loaded on first use, reloaded when the file changes) and the
  // English learning channel.
  EnglishCandidates QueryEnglishCandidates(const std::string& raw_romaji, uint64_t now_epoch_sec);
  // M63 (docs/mixed-english-auto-conversion-spec.md section 6.1): the English
  // dictionary's surfaces for lower_key, most frequent first, under the
  // current config. Empty when inlineEnglishDictionary is off or the
  // dictionary is missing or unreadable. Never the learned surfaces.
  std::vector<EnglishDictionaryEntry> LookupEnglishDictionary(std::string_view lower_key);
  // M63-B: runs compute on one snapshot of the same dictionary (nullptr when
  // LookupEnglishDictionary would find nothing), so the mixed lattice does not
  // take the locks and stat the files for each substring. compute may run
  // again when the files change under it, so it must not have side effects
  // beyond its own result; it must not call back into the engine, and the
  // Zenzai conversion runs after it returns.
  void WithEnglishDictionary(const std::function<void(const EnglishDictionary*)>& compute);
  // Records a committed English candidate under its lowercased raw romaji in
  // the English channel only. false for a duplicate observation_id or when no
  // English store is set.
  bool CommitEnglishObservation(const std::string& raw_romaji, const std::string& surface,
                                uint64_t now_epoch_sec, const std::string& observation_id = {});
  // M63-B (mixed-english-auto-conversion-spec sections 8.1 and 11): whether the
  // English channel holds a negative observation of lower_key. Never waits:
  // while a commit saves the channel, it answers as the last lookup or record
  // of the key did.
  bool IsEnglishSuppressed(std::string_view lower_key);
  // Section 11, for a commit that is not English: records a negative
  // observation of each candidate of `shown` that has segments, the English
  // tag and a raw romaji reading without capitals that section 8.1 (ii) makes
  // sure, unless chosen_surface is that word. The caller has checked that
  // learning is allowed. Call it without holding any engine mutex. Returns the
  // number of keys recorded.
  size_t RecordRejectedEnglish(const std::vector<ipc::CandidateField>& shown,
                               const std::string& chosen_surface, uint64_t now_epoch_sec);
  // Test-only: runs while_held with the English channel lock held, as a
  // commit's save does. while_held must not take that lock on this thread.
  void HoldEnglishLockForTests(const std::function<void()>& while_held);
  // DEV-1584 (inline-english-candidate-spec section 4.8): an M36 word that has
  // just become Confirmed enters the English dictionary when inlineEnglishDictionary
  // is on and the surface is ASCII letters only, at least inlineEnglishMinLength
  // long, and not already there. source names the path in the log
  // ("new_word_resolve", "mining", "trending" or "backfill"). It takes
  // state_mutex_ briefly, and the upsert may wait for the dictionary's writer
  // lock; call it without holding any engine mutex.
  // epoch, when set, is EnglishRegistrationEpoch() read before the word's
  // Confirmed state was saved: a reset of the AutoWord store since then
  // cancels the upsert (DEV-1600). true when an upsert was attempted.
  bool RegisterConfirmedEnglishWord(const std::string& surface, std::string_view source,
                                    std::optional<uint64_t> epoch = std::nullopt);
  uint64_t EnglishRegistrationEpoch() const { return english_registration_epoch_.load(); }
  // DEV-1596 (section 4.8): an M36 word that has just left Confirmed for
  // Rejected leaves the English dictionary when a runtime upsert added it
  // (EnglishDictionaryStore::Retract). source names the path in the log
  // ("new_word_resolve", "forget" or "backfill"). Nothing is taken back while
  // another reading of surface is Confirmed. Call it without holding any
  // engine mutex. true when a retract was attempted (not NotAdded).
  bool RetractRejectedEnglishWord(const std::string& surface, std::string_view source);
  // DEV-1597 (section 4.8): registers the Confirmed AutoWord words the English
  // dictionary lacks, such as those confirmed before the wiring or by
  // `newwords confirm --offline`, then retracts the Rejected words whose
  // upsert the journal still holds (a retract that failed midway, DEV-1596).
  // Stops when stop() turns true, or after max_writes upserts and retracts.
  // Returns the number attempted. Meant for a background thread at startup.
  static constexpr size_t kMaxEnglishBackfillWrites = 256;
  size_t BackfillConfirmedEnglishWords(const std::function<bool()>& stop,
                                       size_t max_writes = kMaxEnglishBackfillWrites);
  struct DictionaryLoadResult {
    std::string name;
    bool loaded;
    std::string error;
  };
  std::vector<DictionaryLoadResult> LoadBundledDictionaryLayers(
      const std::filesystem::path& directory);
  bool LoadDictionaryLayer(learning::LayerId layer, const std::filesystem::path& path,
                           bool verify = false);
  void EnableDictionaryLayer(learning::LayerId layer, bool enabled);
  // false on any failure, including a Busy wait budget (DEV-1572); the
  // dictionary on disk and in memory is then unchanged by this call.
  bool AddUserWord(const learning::UserWord& word, const UserDictionaryEditOptions& options = {});
  bool RemoveUserWord(const std::string& word, const std::string& ruby,
                      const UserDictionaryEditOptions& options = {});
  // Test-only: runs when an edit or an import holds user_dict_writer_mutex_
  // and is about to wait for the user dictionary file lock. Set it before
  // any edit starts.
  void SetBeforeUserDictionaryFileLockForTests(std::function<void()> hook);
  // Test-only: runs under state_mutex_ just before each learning store save,
  // with the transient-conflict retry budget that save is given. Set it before
  // any observation is committed.
  void SetBeforeLearningSaveForTests(std::function<void(std::chrono::milliseconds)> hook);

  bool LoadModel();
  AiTransformResult TransformLocal(const AiTransformRequest& request,
                                   const std::atomic<bool>* cancel, AiDeadline deadline);
  bool LoadModel(const ModelLoadOptions& options);
  ModelLoadResult LoadModelWithResult();
  ModelLoadResult LoadModelWithResult(const ModelLoadOptions& options);
  bool StartModelPreload(ModelLoadOptions options);
  void WaitForModelPreload();

  struct CandidatesResult {
    std::vector<core::Candidate> candidates;
    // Non-empty only under typo_correction_mode "auto_replace": the reading the
    // conversion actually ran on, so the TIP can replace its preedit.
    std::string corrected_reading;
    // The Zenzai conversion stopped at its deadline (the budget or
    // MixedConversionOptions::deadline): its candidates are best-so-far or the
    // fallback's (M63-B section 10 leaves such a conversion out).
    bool model_deadline_exceeded{false};
  };

  // QueryCandidates with optional cancel polling, plus the typo-corrected
  // reading (M35). Returns an empty result when *cancel is observed true.
  // cancel may be nullptr.
  CandidatesResult QueryCandidatesEx(const std::string& kana, const std::string& context,
                                     uint64_t now_epoch_sec, const std::atomic<bool>* cancel,
                                     uint32_t max_candidates = 0, bool live = false,
                                     const InferenceTelemetry* telemetry = nullptr,
                                     const TagBoosts* tag_boosts = nullptr,
                                     const MixedConversionOptions* mixed = nullptr);

  // M14: one best candidate from the live path (live = true): a loaded model
  // decodes one beam and skips NLL and the typo suggestion; without a model the
  // fallback converter answers. Keeps dictionary and learning ranking.
  std::optional<core::Candidate> QueryLiveConversion(const std::string& kana,
                                                     const std::string& context,
                                                     uint64_t now_epoch_sec,
                                                     const std::atomic<bool>* cancel,
                                                     const InferenceTelemetry* telemetry = nullptr,
                                                     const MixedConversionOptions* mixed = nullptr);

  // Candidates only; equivalent to QueryCandidatesEx(...).candidates.
  std::vector<core::Candidate> QueryCandidates(const std::string& kana, const std::string& context,
                                               uint64_t now_epoch_sec,
                                               const std::atomic<bool>* cancel,
                                               uint32_t max_candidates = 0, bool live = false,
                                               const InferenceTelemetry* telemetry = nullptr);

  // Backwards-compatible overload without cancel support.
  std::vector<core::Candidate> QueryCandidates(const std::string& kana, const std::string& context,
                                               uint64_t now_epoch_sec);

  // Exact dictionary surface lookup. Empty means no known reading.
  std::string ReverseConvert(const std::string& surface, uint64_t now_epoch_sec);

  std::vector<core::Candidate> QueryPredictions(const std::string& kana, const std::string& context,
                                                uint64_t now_epoch_sec,
                                                const std::atomic<bool>* cancel = nullptr,
                                                const InferenceTelemetry* telemetry = nullptr);
  std::vector<core::Candidate> QueryCorrections(const std::string& kana, const std::string& context,
                                                const std::string& rejected_surface,
                                                uint64_t now_epoch_sec);
  // observation_id is the TIP-side idempotency key (DEV-554). A non-empty id
  // that was already applied is ignored so a resend after a pipe drop does not
  // count the same commit twice; an empty id disables dedupe (legacy TIP).
  // Returns false when the observation was dropped as a duplicate.
  // app_name is the foreground process (normalized by the store) and
  // left_context the committed text before it; both feed the M54 app row and
  // context hash and may be empty (global row, empty-context hash).
  bool CommitObservation(const std::string& reading, const std::string& surface,
                         uint64_t now_epoch_sec, const std::string& observation_id = {},
                         const std::string& app_name = {}, const std::string& left_context = {});
  bool CommitSegmentsObservation(const ipc::CommitSegmentsObservationRequest& request,
                                 uint64_t now_epoch_sec);
  // M35: records one observed (mistyped reading -> retyped reading) pair and
  // persists it. Returns false when there is no store, or when the pair fails
  // the accept filters.
  bool ObserveTypo(const std::string& wrong_reading, const std::string& correct_reading,
                   uint64_t now_epoch_sec);
  // M54 correction (user-learning-enhancement-spec section 4, DEV-1529).
  // Without selected_surface it is an immediate Backspace: only
  // correction_reject is recorded and the converter is left alone. With it, a
  // reconversion records correction_accept and correction_reject and commits
  // the selected surface. app_name, left_context and observation_id work as in
  // CommitObservation. Returns false when the call was dropped as a duplicate.
  bool CommitCorrection(const std::string& reading, const std::string& rejected_surface,
                        const std::optional<std::string>& selected_surface, uint64_t now_epoch_sec,
                        const std::string& observation_id = {}, const std::string& app_name = {},
                        const std::string& left_context = {});
  bool FlushLearningStore();

  // M49 learning data management (DEV-1190, learning-data-management-spec
  // section 4). Each call takes every store lock it needs at once and
  // persists the stores it changed before it returns.
  enum class ForgetOutcome { NotFound, Forgotten, SaveFailed };
  LearningDataPage ListLearningEntries(LearningDataStore store, const std::string& query,
                                       size_t offset, size_t limit);
  ForgetOutcome ForgetLearningEntry(LearningDataStore store, const std::string& id);
  // The TIP's Ctrl+Shift+Backspace: the kana pair, also removed from the M7 file.
  ForgetOutcome ForgetLearningPair(const std::string& reading, const std::string& surface);
  LearningExportResult ExportLearningData(const std::vector<LearningDataStore>& selected,
                                          const std::filesystem::path& destination, bool encrypt,
                                          const BackupManifest& meta);
  LearningImportResult ImportLearningData(const std::vector<LearningDataStore>& selected,
                                          const std::filesystem::path& source,
                                          learning::ImportConflictPolicy policy);
  // Empties one store on disk and in memory (learning-data-management-spec
  // section 4.6). "learning" covers every learning channel and the M7 file.
  // On SaveFailed the in-memory contents are put back as they were.
  enum class ResetOutcome { Reset, Unavailable, SaveFailed };
  ResetOutcome ResetLearningStore(LearningDataStore store);
  // Serializes the ingest/save transaction with reset and its failure rollback.
  // A failed save retains valid ingested words in memory for the next retry.
  bool IngestTrendingWords(const std::vector<learning::AutoWord>& words, uint64_t now_epoch,
                           bool auto_promote);
  // Cancelled when still_current is false under the same lock, before anything changes,
  // so a reset that follows a cancellation never sees these words afterwards.
  enum class TrendingIngestOutcome { Saved, Cancelled, SaveFailed };
  TrendingIngestOutcome IngestTrendingWords(const std::vector<learning::AutoWord>& words,
                                            uint64_t now_epoch, bool auto_promote,
                                            const std::function<bool()>& still_current);

  // rich-features-spec X-2-7: the persona of the kana learning store, computed
  // by the learning flush worker at startup and every kPersonaRefreshInterval.
  struct PersonaSnapshot {
    learning::Persona persona;
    uint64_t computed_at_epoch_sec{};
  };
  static constexpr std::chrono::hours kPersonaRefreshInterval{24};
  // nullopt without a learning store. Computes now when the worker has not yet.
  std::optional<PersonaSnapshot> CurrentPersona();
  // Recomputes immediately (tests, and the worker).
  void RefreshPersona();
  // Encryption of backup archive entries; DPAPI unless a test injects a key.
  void SetBackupCrypto(const learning::ByteCrypto* crypto);

  void ApplyConfig(const EngineConfig& config);
  // Uses only the data-index lock, never the model conversion lock.
  std::vector<core::Candidate> QueryRewriters(const RewriterConfig& config,
                                              const std::string& reading,
                                              const std::string& trigger,
                                              std::vector<core::Candidate> ordinary,
                                              size_t max_candidates);

  BackendKind backend() const;
  EngineConfig config() const;
  EngineHealthSnapshot health_snapshot() const;
  bool model_loaded() const;
  bool model_preload_in_progress() const;
  std::optional<std::string> last_error() const;
  std::optional<std::string> effective_last_error() const;
  std::optional<ZenzaiDecodeStats> last_zenzai_decode_stats() const;
  // Threads the loaded Zenzai model decodes with; nullopt without a loaded model.
  std::optional<int32_t> model_thread_count() const;

  // M47 section 8.5.1. The engine drives the model rows itself from its load
  // outcomes; the Host drives SafeMode. The transport rows describe what the
  // TIP sees and are not driven here.
  std::optional<HealthTransition> ApplyHealthEvent(HealthEvent event);
  // Startup only: resumes SafeMode persisted by an earlier process.
  void RestoreHealthState(HealthState state);
  HealthState health_state() const;

 private:
  CandidatesResult QueryCandidatesExImpl(const std::string& kana, const std::string& context,
                                         uint64_t now_epoch_sec, const std::atomic<bool>* cancel,
                                         uint32_t max_candidates, bool live,
                                         const InferenceTelemetry* telemetry,
                                         const TagBoosts* tag_boosts = nullptr,
                                         const MixedConversionOptions* mixed = nullptr);
  RewriterData rewriter_data_;
  // M36-A section 4-3: shape and dictionary-membership filters that decide
  // whether a committed (reading, surface) pair is an unknown word worth mining.
  // Holds state_mutex_ for the dictionary lookups.
  bool IsMiningCandidateLocked(const std::string& reading, const std::string& surface) const;
  void NoteLearningMutationLocked(uint64_t now_epoch_sec);
  bool NoteObservationIdLocked(const std::string& observation_id);
  // The dictionary store for the path; it reloads when its files change.
  std::shared_ptr<EnglishDictionaryStore> EnglishDictionaryFor(const std::string& utf8_path);
  // DEV-1600: the AutoWord reset's erasure of the English dictionary's
  // runtime additions. true when there is nothing to erase.
  bool ResetEnglishRuntimeAdditions();
  // Saves a dirty English store; logs a failure. Holds english_mutex_.
  bool SaveEnglishStoreLocked(std::chrono::milliseconds retry_budget);
  // M63-B section 11: the keys of `shown` a non-English commit of
  // chosen_surface records as negative observations. Looks the dictionary up;
  // call it before taking any engine mutex.
  std::vector<std::string> RejectedEnglishKeys(const std::vector<ipc::CandidateField>& shown,
                                               std::string_view chosen_surface);
  // Writes the negative observations of `rejected` and lifts those of
  // `released`, then bounds the store. Holds english_mutex_.
  // Drops what IsEnglishSuppressed answers under contention; called wherever
  // the English store may lose a negative observation (prune, forget, reset,
  // import). Takes the cache lock only.
  void ClearEnglishSuppressedCache();
  void ApplyEnglishNegativesLocked(const std::vector<std::string>& rejected,
                                   const std::vector<std::string>& released, uint64_t now_epoch_sec,
                                   size_t max_records, double min_weight);
  std::vector<core::Candidate> ApplyRerankerOrRaw(const std::string& kana,
                                                  std::vector<core::Candidate> candidates,
                                                  uint64_t now_epoch_sec,
                                                  const std::atomic<bool>* cancel = nullptr);
  void ApplyConfigLocked(const EngineConfig& config);
  void MirrorModelRuntimeErrorLocked(const std::shared_ptr<core::IConverter>& converter);
  std::optional<HealthTransition> ApplyHealthEventLocked(HealthEvent event);
  void NoteModelLoadFailedLocked();
  // Takes user_dict_writer_mutex_ itself; the caller holds no engine mutex.
  bool EditUserDictionary(const std::function<bool(learning::UserDictionary&)>& edit,
                          const UserDictionaryEditOptions& options);
  // The learning data view over every store; callers hold all store locks.
  LearningDataStores LearningDataStoresLocked();
  // Persists what a learning data operation may have changed. Holds every
  // store lock; false when any store failed to save.
  bool SaveLearningDataLocked();
  using LearningSnapshots =
      std::vector<std::pair<learning::LearningStore*, learning::LearningStore>>;
  LearningSnapshots SnapshotLearningStoresLocked();
  bool SaveLearningSnapshots(LearningSnapshots& snapshots);
  void PublishLearningSnapshotsLocked(LearningSnapshots& snapshots);
  ForgetOutcome ForgetPairSnapshot(learning::LearningStore& store, const std::string& reading,
                                   const std::string& surface);
  bool ShouldFlushLearningStoreLocked(uint64_t now_epoch_sec) const;
  // Flushes reached while queries wait on state_mutex_ pass zero: they try once
  // and keep the store dirty. Shutdown and explicit flushes retry conflicts.
  bool FlushLearningStoreLocked(std::chrono::milliseconds retry_budget);
  void RecordLearningSaveFailureLocked();
  void RecordUserDictionaryFailureLocked(const char* error);
  void LearningFlushWorker();
  ResetOutcome ResetLearningChannels();
  void RefreshPersonaLocked();
  void RefreshDictionaryLocked();
  void ApplyDictionaryConfigLocked();

  std::shared_ptr<core::IConverter> fallback_converter_;
  std::shared_ptr<core::IConverter> model_converter_;
  std::shared_ptr<core::IConverter> active_converter_;
  learning::LearningStore* store_;
  learning::Reranker reranker_;
  learning::UserDictionary* user_dict_{nullptr};
  learning::TypoCorrectionStore* typo_store_{nullptr};
  learning::AutoWordStore* auto_word_store_{nullptr};
  const learning::ByteCrypto* backup_crypto_{nullptr};
  // Guards the English store and the English dictionary cache. Only the
  // learning data operations hold it together with state_mutex_ (through one
  // std::scoped_lock); the conversion path takes it alone.
  mutable std::mutex english_mutex_;
  learning::LearningStore* english_store_{nullptr};
  // M63-B: the keys IsEnglishSuppressed last found suppressed, for the
  // keystrokes that come while a commit holds english_mutex_. Its own lock,
  // taken after english_mutex_ or alone.
  std::mutex english_suppressed_cache_mutex_;
  std::unordered_set<std::string> english_suppressed_cache_;
  // The dictionary cache has its own lock so a reload never blocks learning.
  std::mutex english_dictionary_mutex_;
  std::shared_ptr<EnglishDictionaryStore> english_dictionary_;
  std::filesystem::path english_dictionary_path_;
  // DEV-1600: raised by each successful AutoWord reset; an English upsert for
  // a word confirmed under an older value is cancelled.
  std::atomic<uint64_t> english_registration_epoch_{0};
  learning::DictionaryStore dictionaries_;
  const learning::UserDictionary* indexed_user_dict_{nullptr};
  uint64_t indexed_user_revision_{};
  bool user_dictionary_enabled_{true};
  // Serializes TypoCorrectionStore, which keeps no lock of its own. The query
  // path holds it on its own, so the store's disk write does not stall queries
  // that take state_mutex_ several times per request; only the learning data
  // operations take it together with state_mutex_ (one std::scoped_lock).
  mutable std::mutex typo_store_mutex_;
  mutable std::mutex state_mutex_;
  mutable std::mutex converter_call_mutex_;
  // Serializes commits with erasure across disk writes and the reset's final
  // state/converter reacquisition. Take before either engine mutex; queries do
  // not take it, and disk I/O never needs converter_call_mutex_.
  std::mutex learning_history_mutex_;
  // Serializes every in-process taker of the user dictionary file lock, so
  // edits of this Host are not lost (DEV-1572). Lock order:
  // learning_history_mutex_ (not taken by AddUserWord / RemoveUserWord), then
  // this, then the file lock, then state_mutex_. Timed, so an edit can give
  // up within its wait budget instead of queueing without bound.
  std::timed_mutex user_dict_writer_mutex_;
  std::function<void()> before_user_dictionary_file_lock_for_tests_;
  std::function<void(std::chrono::milliseconds)> before_learning_save_for_tests_;
  // Written with state_mutex_ and english_mutex_; either protects reads.
  // Background/explicit flushes leave the live snapshot alone during erasure.
  bool learning_erasure_in_progress_{false};
  std::mutex model_load_mutex_;
  std::mutex model_preload_thread_mutex_;
  EngineConfig config_;
  uint64_t nll_config_revision_{};
  logging::RuntimeLogger* runtime_logger_;  // Non-owning; must outlive the engine.
  // Written under state_mutex_; the handshake reads it without the lock.
  std::atomic<bool> model_loaded_{false};
  bool model_preload_in_progress_{false};
  HealthStateMachine health_;
  std::optional<std::string> last_error_;
  std::optional<std::string> model_runtime_error_;
  size_t unsaved_observations_{0};
  std::optional<uint64_t> first_unsaved_observation_epoch_sec_;
  std::optional<uint64_t> last_unsaved_observation_epoch_sec_;
  std::optional<std::chrono::steady_clock::time_point> first_unsaved_observation_steady_;
  std::optional<std::chrono::steady_clock::time_point> last_successful_learning_save_steady_;
  // Bounded ring of recently applied observation ids (DEV-554). Depth covers the
  // resend backlogs of every TIP instance the transport admits at once, so an id
  // is never evicted while its own TIP can still retry it. Not persisted: a Host that crashes after
  // Save() but before the ACK sees the resend as new, which double-counts one commit's weight (see
  // docs/learning-data-management-spec.md, at-least-once section).
  std::deque<std::string> applied_observation_ids_;
  std::unordered_set<std::string> applied_observation_id_set_;
  std::optional<PersonaSnapshot> persona_;
  std::optional<std::chrono::steady_clock::time_point> persona_computed_steady_;
  std::condition_variable learning_flush_cv_;
  std::thread learning_flush_thread_;
  std::thread model_preload_thread_;
  bool learning_flush_stop_{false};
};

}  // namespace azookey::host
