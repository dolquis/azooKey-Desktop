#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "azookey/learning/DpapiCrypto.h"
#include "azookey/learning/ImportConflictPolicy.h"
#include "azookey/learning/TypoCorrectionStore.h"
#include "azookey/learning/TypoPattern.h"

namespace azookey::learning {

// docs/typo-correction-learning-spec.md section 12.8: the typo v2 store.
inline constexpr std::string_view kTypoLearningV2FileName = "typo_learning.v2.db";
inline constexpr int kTypoLearningSchemaVersion = 1;
inline constexpr std::string_view kTypoPatternsEscapedTsvHeader =
    "# azookey-typo-patterns-tsv escaped=1";

// Logical paths (section 12.8 "パス"); each is read and written through
// ReadProtectedText / WriteProtectedText, so it is "<path>.enc" on disk.
struct TypoLearningPaths {
  std::filesystem::path v1_store;
  std::filesystem::path v2_store;
  // The migrated v1 store ("v1 の退避").
  std::filesystem::path v1_backup;
};

// The v2 store sits beside the v1 store under a fixed name; the v1 backup is
// the v1 file name with ".v1" before its extension and ".bak" appended
// (typo_corrections.tsv -> typo_corrections.v1.tsv.bak; no extension ->
// <name>.v1.bak).
TypoLearningPaths TypoLearningPathsFor(const std::filesystem::path& v1_store);

// Section 12.8 "保持上限": the earlier of an age and a row count, applied to
// typo_events and typo_reading_pairs right before a save.
struct TypoLearningLimits {
  uint64_t max_age_sec{180ULL * 24 * 60 * 60};
  size_t max_rows{10000};
};

// "store_origin" in typo_settings (section 12.8 "由来の印").
enum class TypoStoreOrigin { None, V1Migrated, Empty, Rebuilt };

enum class TypoLearningMode {
  // The v2 database is in use.
  V2,
  // Section 12.8 "v1 のまま読む": migrating from the v1 store failed, so the
  // v1 store is used under the M35 rules for this run.
  V1Fallback,
  // An empty store in memory that refuses to save (an unreadable store, a
  // newer schema, or a failed migration from the v1 backup).
  Blocked,
};

// Why a load left the store Blocked, or which side file it could not read.
enum class TypoLearningLoadIssue {
  None,
  SqliteUnavailable,
  V2Unreadable,
  V2TooNew,
  V1StoreUnreadable,
  V1BackupUnreadable,
  MigrationFailed,
  CorruptCopyFailed,
};

struct TypoLearningLoadResult {
  TypoLearningMode mode{TypoLearningMode::Blocked};
  TypoLearningLoadIssue issue{TypoLearningLoadIssue::None};
  // Section 12.8 decision 5: the v2 file was corrupt, copied aside, and
  // rebuilt. Reported as the D-010 warning for the rest of the run.
  bool rebuilt_from_corruption{false};
  // "deserialize", "empty", "schema", "integrity" or "read" (no row contents).
  std::string corruption_reason;
  // Pairs moved from the v1 store or the v1 backup during this load.
  bool migrated{false};
  size_t migrated_pairs{0};
  // Migration step 5 did not finish: the v1 store is still in place, and the
  // next load imports it again (decision 7).
  bool v1_store_pending{false};
  // The v1 store or v1 backup could not be read, so forgetting a reading pair
  // fails until it can (the reset of the whole store still works).
  bool v1_side_unreadable{false};
};

struct TypoPatternRecord {
  int64_t id{};
  std::string observed_pattern;
  std::string intended_pattern;
  std::string typo_type;
  // Empty is NULL.
  std::string keyboard_layout;
  double confidence{0.5};
  uint32_t accept_count{};
  uint32_t reject_count{};
  std::optional<uint64_t> last_used_at;
  uint64_t created_at{};
  uint64_t updated_at{};
};

enum class TypoPatternOutcome { Accept, Reject };

// One accepted or rejected correction, as the engine reports it. Patterns must
// already be abstraction tokens (IsValidTypoPattern); the store refuses
// anything else and never sees raw keys.
struct TypoPatternEvent {
  std::string observed_pattern;
  std::string intended_pattern;
  std::string typo_type;
  std::string keyboard_layout;  // empty: NULL
  std::string observed_reading;
  std::string corrected_reading;
  std::string selected_surface;   // empty: NULL
  std::string app_name;           // empty: NULL (the caller omits it when app storage is off)
  std::string left_context_hash;  // empty: NULL; otherwise 8 lowercase hex digits
  TypoPatternOutcome outcome{TypoPatternOutcome::Accept};
  uint64_t now_epoch_sec{};
};

enum class TypoForgetResult { Removed, NotFound, Failed };

// The typo v2 store: an in-memory SQLite database saved whole, encrypted,
// through WriteProtectedText. Like the v1 store it holds no lock; the caller
// serializes every call.
class TypoLearningStore {
 public:
  // `v1_store` is the v1 logical path (typo_corrections.tsv, or --typo-store).
  explicit TypoLearningStore(std::filesystem::path v1_store, const ByteCrypto* crypto = nullptr,
                             TypoLearningLimits limits = {});
  ~TypoLearningStore();
  TypoLearningStore(const TypoLearningStore&) = delete;
  TypoLearningStore& operator=(const TypoLearningStore&) = delete;

  // Section 12.8 "起動時の読み込みと破損時の復旧" decisions 1 to 8, including
  // the migration from the v1 store or the v1 backup. `now_epoch_sec` stamps
  // the .corrupt-<epoch> copy.
  TypoLearningLoadResult Load(uint64_t now_epoch_sec);
  // Applies the retention limits and writes the whole database. False when
  // the store is Blocked, writes are suspended, or the write failed.
  bool Save(uint64_t now_epoch_sec);

  TypoLearningMode mode() const { return mode_; }
  bool save_blocked() const { return mode_ == TypoLearningMode::Blocked; }
  TypoStoreOrigin origin() const;
  const TypoLearningPaths& paths() const { return paths_; }

  // While suspended (secure input), the store neither learns nor writes:
  // observations and pattern events are refused, Save writes nothing, and
  // forgetting or resetting fails without touching any file.
  void SetWritesSuspended(bool suspended) { writes_suspended_ = suspended; }
  bool writes_suspended() const { return writes_suspended_; }

  // Reading pairs (the M35 pair learning kept in v2, section 5-1 / 6).
  bool ObserveReadingPair(const std::string& wrong, const std::string& correct,
                          uint64_t now_epoch_sec);
  std::optional<std::string> LookupReadingPair(const std::string& wrong, uint32_t min_count,
                                               uint64_t now_epoch_sec) const;
  std::vector<TypoCorrectionEntry> ReadingPairs() const;

  // Patterns (sections 12.8 and 12.9). Returns the pattern id, or nullopt when
  // the event is refused (invalid fields, V1Fallback, suspended writes).
  std::optional<int64_t> RecordPatternEvent(const TypoPatternEvent& event);
  std::vector<TypoPatternRecord> Patterns() const;
  // Section 12.9 with the app bonus for `current_app`; nullopt for an unknown id.
  std::optional<double> PatternConfidence(int64_t pattern_id, std::string_view current_app,
                                          bool app_aware_learning, uint64_t now_epoch_sec) const;
  // The app with the most accepts of the pattern; nullopt when none or tied.
  std::optional<std::string> DominantApp(int64_t pattern_id) const;
  size_t EventCount() const;

  // Section 12.8 "学習の削除". A reading pair is also removed from the v1
  // backup (and the v1 store while it remains) first; a pattern is identified
  // by its two patterns (channel "pattern"). Failed leaves the published
  // contents as they were.
  TypoForgetResult ForgetReadingPair(const std::string& wrong, const std::string& correct,
                                     uint64_t now_epoch_sec);
  TypoForgetResult ForgetPattern(const std::string& observed_pattern,
                                 const std::string& intended_pattern, uint64_t now_epoch_sec);
  // Removes every learned row and the v1 files; typo_settings is kept.
  bool ResetAll(uint64_t now_epoch_sec);

  // Backup items (docs/learning-data-management-spec.md section 5): the
  // reading pairs in the v1 TSV format (item typo_corrections) and the
  // patterns in an escaped TSV (item typo_patterns). typo_events is not
  // exported.
  std::string SerializeReadingPairsText() const;
  std::string SerializePatternsText() const;
  // Parses an item typo_patterns; false when any row was malformed (the valid
  // rows are still returned).
  static bool ParsePatternsText(std::string_view text, std::vector<TypoPatternRecord>& patterns);
  // Section 6 of the same spec. Pairs merge as in the v1 store; patterns
  // match on (observed, intended, typo_type, keyboard_layout), Merge adds the
  // counts (saturating) and keeps the later timestamps, and every imported
  // pattern has its confidence recomputed. V1Fallback keeps the pairs only.
  ImportCounts MergeReadingPairs(const TypoCorrectionStore& other, ImportConflictPolicy policy);
  ImportCounts MergePatterns(const std::vector<TypoPatternRecord>& patterns,
                             ImportConflictPolicy policy, uint64_t now_epoch_sec);

 private:
  class Database;

  TypoLearningPaths paths_;
  const ByteCrypto* crypto_;
  TypoLearningLimits limits_;
  TypoLearningMode mode_{TypoLearningMode::Blocked};
  std::unique_ptr<Database> db_;
  // The v1 store, used as the store in V1Fallback.
  std::unique_ptr<TypoCorrectionStore> v1_;
  bool writes_suspended_{false};

  void LoadWithoutV2(uint64_t now, TypoLearningLoadResult& result);
  void RecoverFromCorruption(uint64_t now, const std::string& reason,
                             TypoLearningLoadResult& result);
  void RedoUnverifiedMigration(uint64_t now, TypoLearningLoadResult& result);
  bool MigrateFrom(const TypoCorrectionStore& source, bool from_v1_store, uint64_t now,
                   TypoLearningLoadResult& result);
  bool SettleV1Store(TypoLearningLoadResult& result);
  void EnterBlocked(TypoLearningLoadResult& result, TypoLearningLoadIssue issue);
  void EnterV1Fallback(std::unique_ptr<TypoCorrectionStore> v1, TypoLearningLoadResult& result);
  void EnterV2(TypoLearningLoadResult& result);
  bool StartEmpty(TypoStoreOrigin origin, uint64_t now);
  bool WriteDatabase(bool apply_limits, uint64_t now);
  bool WriteDatabase(Database& db, bool apply_limits, uint64_t now) const;
  std::unique_ptr<Database> CopyDatabase() const;
  bool CommitDeletion(uint64_t now, bool always_save, bool& changed,
                      const std::function<bool(Database&, bool&)>& change);
  bool RemovePairFromV1Side(const std::string& wrong, const std::string& correct, bool& removed);
  bool RemoveV1Files();
};

}  // namespace azookey::learning
