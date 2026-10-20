#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "azookey/host/EnglishCandidates.h"
#include "azookey/host/EnglishDictionaryFiles.h"

namespace azookey::host {

// One (re)build of the dictionary snapshot, for the section 4.4 load log.
struct EnglishDictionaryLoadReport {
  bool ok{false};
  const char* source{"none"};  // "bin" or "tsv".
  size_t entries{0};
  EnglishDictionaryLoadStats stats;
};

// Where an Upsert or Remove ended up (sections 4.6 to 4.8).
enum class EnglishWriteResult {
  Persisted,      // In the overlay and the journal.
  JournalReset,   // In the overlay; an unreadable journal was moved aside for a new one.
  JournalFull,    // In the overlay only: the journal holds kMaxEnglishJournalEntries.
  JournalFailed,  // In the overlay only: the journal could not be written.
  MemoryOnly,     // Not on disk (no lock, no write access): kept until restart.
  Dropped,        // Not kept: there is no base, or memory ran out.
  Cancelled,      // Not written: the caller's still_current turned false.
};
const char* EnglishWriteResultName(EnglishWriteResult result);

struct EnglishWriteOutcome {
  EnglishWriteResult result{EnglishWriteResult::Dropped};
  // true when the op reached the overlay file.
  explicit operator bool() const {
    return result != EnglishWriteResult::MemoryOnly && result != EnglishWriteResult::Dropped &&
           result != EnglishWriteResult::Cancelled;
  }
};

// Where a Retract ended up (section 4.8, DEV-1596).
enum class EnglishRetractResult {
  Retracted,   // Out of the journal, and hidden by an overlay tombstone unless the TSV has it.
  MemoryOnly,  // Hidden only in memory until restart; the journal still has it.
  NotAdded,    // No runtime upsert of the word to take back: nothing changed.
  Failed,      // Not taken back (no lock, or a file could not be read or written).
};
const char* EnglishRetractResultName(EnglishRetractResult result);

// The English dictionary at a configured path (sections 4.5 to 4.8):
// - reads the .bin when it is valid and its mtime equals the TSV's (or there
//   is no TSV), otherwise parses the TSV and recompiles the .bin under the
//   writer lock; with neither, there is no dictionary;
// - replays the overlay that matches the base's content_hash;
// - reads without waiting on writers: before and after each read it samples
//   the base header from the path and the overlay header, and retries when
//   they changed; during a rebuild by another thread it uses the previous
//   snapshot;
// - appends upserts and tombstones to the overlay and compacts it under the
//   writer lock, or keeps them in memory when the files cannot be written;
// - journals the ops it appended, and replays the journal over the TSV when it
//   recompiles the base (section 4.8).
class EnglishDictionaryStore {
 public:
  using LoadListener = std::function<void(const EnglishDictionaryLoadReport&)>;

  explicit EnglishDictionaryStore(const std::filesystem::path& configured,
                                  LoadListener listener = {});

  const EnglishDictionaryPaths& paths() const { return paths_; }

  // Runs compute on a snapshot consistent with the files (nullptr when there
  // is no dictionary) and returns its result.
  template <class Compute>
  auto Read(Compute&& compute) {
    for (int attempt = 1;; ++attempt) {
      const auto before = TakeSample();
      const auto snapshot = SnapshotFor(before);
      auto result = compute(static_cast<const EnglishDictionary*>(snapshot.get()));
      if (attempt >= kMaxReadAttempts || TakeSample() == before) return result;
    }
  }
  std::vector<EnglishDictionaryEntry> Lookup(std::string_view lower_key);

  // still_current, when set, is asked once this writer holds the update
  // mutex: false cancels the upsert. ResetRuntimeAdditions takes the same
  // mutex, so an upsert that passes it lands before a reset that follows.
  EnglishWriteOutcome Upsert(std::string_view surface, uint32_t frequency, uint8_t flags = 0,
                             const std::function<bool()>& still_current = {});
  EnglishWriteOutcome Remove(std::string_view surface);
  // Section 4.8 (DEV-1596): takes back a runtime upsert of surface, under the
  // writer lock, only when the latest journal op on it is an upsert (or this
  // host holds one in memory). The overlay gets a tombstone unless the TSV has
  // the word, and the journal loses the entry, so a later TSV that has the
  // word shows it again. A word of the TSV or the shipped .bin that no
  // runtime op added is never hidden. still_current, when set, is asked under
  // the update mutex as for Upsert: false leaves everything (NotAdded).
  EnglishRetractResult Retract(std::string_view surface,
                               const std::function<bool()>& still_current = {});
  // Section 4.8 (DEV-1600): erases the runtime additions, here and in the
  // files (ResetEnglishRuntimeFiles), under the writer lock. Runtime deletes
  // go with a TSV; without one, a delete in the overlay stays. false
  // when the lock, the TSV or a file step fails; calling it again is safe.
  bool ResetRuntimeAdditions();
  bool Compact();

  // Test seams: the write protocol steps, a call between sampling the base
  // header and reading the overlay, and the clock of the writer lock wait.
  void SetWriteHookForTest(EnglishWriteHook hook);
  void SetReadHookForTest(std::function<void()> hook);
  void SetLockClockForTest(EnglishLockClock clock);

 private:
  static constexpr int kMaxReadAttempts = 4;
  // Compaction waits for at least this many ops, or a tenth of the base.
  static constexpr uint32_t kMinCompactionOps = 64;

  struct Sample {
    std::optional<std::pair<std::filesystem::file_time_type, uintmax_t>> tsv;
    std::optional<EnglishBaseHeader> base;
    std::optional<EnglishOverlayHeader> overlay;
    bool operator==(const Sample&) const = default;
  };

  Sample TakeSample() const;
  std::shared_ptr<const EnglishDictionary> SnapshotFor(const Sample& sample);
  // The *Locked members run under update_mutex_. file_locked: the caller
  // already holds the inter-process writer lock.
  std::shared_ptr<const EnglishDictionary> RebuildLocked(const Sample& sample,
                                                         bool file_locked = false);
  // force: compile even when the .bin's mtime matches the TSV.
  std::shared_ptr<const EnglishBaseImage> CompileTsvLocked(EnglishDictionaryLoadReport& report,
                                                           std::filesystem::file_time_type tsv_time,
                                                           bool force, bool file_locked);
  EnglishWriteResult AppendLocked(const EnglishOverlayOp& op);
  // Under update_mutex_ and the writer lock: the base for the current files.
  void SettleBaseLocked(bool file_locked);
  void Publish(std::shared_ptr<const EnglishDictionary> snapshot, std::optional<Sample> sample);
  bool IsCurrent(const Sample& sample) const;
  void Invalidate();

  EnglishDictionaryPaths paths_;
  LoadListener listener_;
  // Section 4.7 (A): update_mutex_ serializes rebuilds and writes, which may
  // parse, wait for the file lock or compact; snapshot_mutex_ only guards the
  // pointer swap, so a reader never waits behind that work.
  std::mutex update_mutex_;
  mutable std::mutex snapshot_mutex_;
  std::shared_ptr<const EnglishDictionary> snapshot_;
  std::optional<Sample> snapshot_sample_;
  bool loaded_once_{false};
  // Under update_mutex_.
  std::shared_ptr<const EnglishBaseImage> base_;
  // base_ was compiled into memory (the .bin could not be written or stamped).
  bool base_in_memory_{false};
  // ...because writing or stamping it failed under the lock: not retried
  // until the TSV changes.
  bool base_write_failed_{false};
  // ...because the journal was there but could not be read (section 4.8): not
  // compiled again for the same TSV until a writer finds the journal readable,
  // or another host's .bin for this TSV appears.
  bool journal_unavailable_{false};
  std::optional<std::pair<std::filesystem::file_time_type, uintmax_t>> compiled_tsv_;
  std::vector<EnglishOverlayOp> memory_ops_;
  EnglishWriteHook write_hook_;
  std::function<void()> read_hook_;
  EnglishLockClock lock_clock_;
};

}  // namespace azookey::host
