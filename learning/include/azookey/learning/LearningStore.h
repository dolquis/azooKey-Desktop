#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "azookey/learning/DpapiCrypto.h"
#include "azookey/learning/ImportConflictPolicy.h"

namespace azookey::learning {

// Header of the M7 / DEV-7 format (reading, surface, "weight epoch").
inline constexpr std::string_view kLearningStoreEscapedTsvHeader =
    "# azookey-learning-tsv escaped=1";
// Header of the M54 v2 format (user-learning-enhancement-spec section 3.2).
inline constexpr std::string_view kLearningStoreV2Header =
    "# azookey-learning-tsv escaped=1 version=2";
// Weight one commit added before v2 (InferenceEngineConfig::learning_alpha).
// Migration derives commit_count from it so migrated rows keep their effect.
inline constexpr double kLegacyCommitWeight = 0.8;
// Upper bounds applied to values read from a file or merged from a backup, so
// that sums neither wrap nor overflow.
inline constexpr uint64_t kMaxLearningCount = 1'000'000'000;
inline constexpr double kMaxLearningWeight = 1e9;
// user-learning-enhancement-spec section 4: typo pattern confidence steps.
inline constexpr double kTypoAcceptWeight = 0.25;
inline constexpr double kTypoRejectWeight = 0.45;

// user-learning-enhancement-spec section 3.2. None marks a row migrated from
// the M7 format, which recorded no event.
enum class LearningEventType : uint8_t {
  None,
  Commit,
  CorrectionAccept,
  CorrectionReject,
  TypoAccept,
  TypoReject,
};

std::string_view LearningEventTypeName(LearningEventType type);
std::optional<LearningEventType> ParseLearningEventType(std::string_view name);

// The v2 file sits beside the M7 file and never replaces it, so an older Host
// that cannot read v2 still finds its own data (learning.tsv -> learning.v2.tsv).
std::filesystem::path LearningStoreV2PathFor(const std::filesystem::path& legacy_path);

// Number of rows in a learning file text (v2 or M7), or nullopt when any row
// is malformed. For diagnostics, which must not skip rows silently.
std::optional<size_t> CountValidLearningRows(std::string_view text);

// Lowercased basename of a process path or name (spec section 6.1).
std::string NormalizeLearningAppName(std::string_view process_name);

struct LearningRecord {
  // Sum of the per-event weight deltas. user_score is derived from the counts
  // at score time and never stored.
  double weight{};
  uint64_t last_updated_epoch_sec{};
  uint64_t commit_count{};
  uint64_t accept_count{};
  uint64_t reject_count{};
  LearningEventType last_event{LearningEventType::None};
  // "" when unknown (migrated rows), otherwise "0x%08x" (spec section 8.1).
  std::string context_hash;
};

struct LearningEntry {
  std::string reading;
  std::string surface;
  LearningRecord record;
  // "" is the global row (spec section 3.2).
  std::string app_name;
};

// One (reading, surface) pair summed over its per-app rows.
struct LearningAggregate {
  std::string reading;
  std::string surface;
  double weight{};
  uint64_t last_updated_epoch_sec{};
  uint64_t commit_count{};
  uint64_t accept_count{};
  uint64_t reject_count{};
  std::vector<std::string> app_names;
};

struct LearningObservation {
  std::string reading;
  std::string surface;
  std::string app_name;
  LearningEventType event{LearningEventType::Commit};
  std::string context_hash;
};

struct PrefixMatch {
  std::string reading;
  std::string surface;
  double score{};
};

struct PrefixLookupResult {
  std::vector<PrefixMatch> matches;
  size_t visited_readings{};
  size_t scanned_records{};
};

class LearningStore {
 public:
  // `path` is the M7 location (learning.tsv). Saves go to
  // LearningStoreV2PathFor(path); the M7 file is read to migrate it and is
  // rewritten by RemoveFromLegacyFile and removed after a verified v2 save.
  explicit LearningStore(std::filesystem::path path, const ByteCrypto* crypto = nullptr);
  virtual ~LearningStore() = default;

  bool Load();
  // Load without migrating a legacy plaintext file or otherwise changing the source files.
  bool LoadReadOnly();
  // retry_budget bounds transient file-conflict retries; zero tries once.
  bool Save(std::chrono::milliseconds retry_budget = kTransientFileRetryBudget) const;
  void Reset();
  bool dirty() const;
  // Number of (reading, surface, app_name) rows.
  size_t size() const;
  std::vector<LearningEntry> All() const;
  // Forgotten pairs (weight and counts all zero) are left out.
  std::vector<LearningAggregate> Aggregates() const;
  // Per-app rows of one pair, keyed by app_name.
  const std::map<std::string, LearningRecord>* Rows(const std::string& reading,
                                                    const std::string& surface) const;
  PrefixLookupResult LookupPrefix(const std::string& reading_prefix, size_t limit, double min_score,
                                  uint64_t now_epoch_sec) const;
  // Returns the reading most strongly learned for `surface`, or empty when no
  // record with a positive decayed weight exists. Scans every record.
  std::string ReverseLookup(const std::string& surface, uint64_t now_epoch_sec) const;

  // A commit of `surface` with no app or context (the M7 entry point).
  void Observe(const std::string& reading, const std::string& surface, double alpha,
               uint64_t now_epoch_sec);
  void ObserveCorrection(const std::string& reading, const std::string& rejected_surface,
                         const std::string& selected_surface, double alpha, uint64_t now_epoch_sec);
  // Applies one section 4 event. `alpha` is the weight of a commit or a
  // correction; typo events use kTypoAcceptWeight / kTypoRejectWeight.
  void ObserveEvent(const LearningObservation& observation, double alpha, uint64_t now_epoch_sec);
  // Zeroes the weight and counts of every app row of the pair, so a later
  // commit starts from scratch; Save leaves zeroed rows out of the file.
  // Returns false when the pair has no row.
  bool Forget(const std::string& reading, const std::string& surface);
  // Removes the M7 rows of the pair from the M7 file, in the M7 format, so an
  // older Host never shows a forgotten pair (learning-data-management-spec
  // section 4.2). Every other line keeps its bytes. True when the file is
  // missing or holds no such row (nothing is written); false when the file
  // cannot be read or WriteProtectedText refuses or fails, leaving it as it was.
  // Never touches the v2 file. `removed_rows`, when given, says whether a row
  // was removed (a pair learned only before v2 has no v2 row to forget).
  bool RemoveFromLegacyFile(const std::string& reading, const std::string& surface,
                            bool* removed_rows = nullptr) const;
  // Removes every M7 row from the M7 file (the reset of the whole store) with
  // the same guarantees as RemoveFromLegacyFile.
  bool ClearLegacyFile() const;
  void Prune(size_t max_records, double min_weight, uint64_t now_epoch_sec);
  // Decayed weight summed over the app rows of the pair.
  virtual double Score(const std::string& reading, const std::string& surface,
                       uint64_t now_epoch_sec) const;

  // The v2 file text, as Save writes it before encryption.
  std::string SerializeText() const;
  // Replaces the contents with rows parsed from `text` (v2 or M7 format).
  // Malformed rows are skipped and make the result false. Does not touch any file.
  bool LoadText(std::string_view text);
  // True after a failed Load: Save refuses to run until the next Load succeeds.
  bool save_blocked() const;
  // Adds every row of `other`; `policy` decides rows that already exist.
  ImportCounts Merge(const LearningStore& other, ImportConflictPolicy policy);

 private:
  bool LoadImpl(bool migrate_plaintext);
  // nullopt removes every row.
  bool RewriteLegacyFile(const std::optional<std::pair<std::string, std::string>>& pair,
                         bool* removed_rows) const;
  bool KeepAsideCopy(const std::filesystem::path& v2_path) const;
  void RemoveVerifiedLegacyFile(std::string_view saved_text,
                                std::chrono::milliseconds lock_timeout) const;

  std::filesystem::path path_;
  const ByteCrypto* crypto_;
  // reading -> surface -> app_name -> record (spec section 14.2, extended by app).
  std::map<std::string, std::map<std::string, std::map<std::string, LearningRecord>>> table_;
  mutable bool dirty_{false};
  bool save_blocked_by_load_failure_{false};
  // Set when Load skipped malformed v2 rows: the next Save first keeps a copy
  // of the file so those rows are not lost silently.
  mutable bool preserve_before_save_{false};
};

}  // namespace azookey::learning
