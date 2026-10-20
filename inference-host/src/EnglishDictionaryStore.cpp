#include "azookey/host/EnglishDictionaryStore.h"

#include <algorithm>
#include <exception>
#include <mutex>
#include <string>
#include <system_error>

#include "azookey/core/DoubleArrayTrie.h"

namespace azookey::host {

namespace {

namespace fs = std::filesystem;

// Section 4.5: a .bin carries the mtime of the TSV it was compiled from, so
// any other TSV mtime (an edit, or a restored copy with an older timestamp)
// means the .bin no longer reflects it.
bool BaseMatchesTsv(const fs::path& base, fs::file_time_type tsv_time) {
  std::error_code ec;
  const auto base_time = fs::last_write_time(base, ec);
  return !ec && base_time == tsv_time;
}

bool SameEntry(const EnglishOverlayOp& a, const EnglishOverlayOp& b) {
  return a.key == b.key && a.surface == b.surface;
}

}  // namespace

const char* EnglishWriteResultName(EnglishWriteResult result) {
  switch (result) {
    case EnglishWriteResult::Persisted:
      return "persisted";
    case EnglishWriteResult::JournalReset:
      return "journal_reset";
    case EnglishWriteResult::JournalFull:
      return "journal_full";
    case EnglishWriteResult::JournalFailed:
      return "journal_failed";
    case EnglishWriteResult::MemoryOnly:
      return "memory_only";
    case EnglishWriteResult::Dropped:
      return "dropped";
    case EnglishWriteResult::Cancelled:
      return "cancelled";
  }
  return "dropped";
}

const char* EnglishRetractResultName(EnglishRetractResult result) {
  switch (result) {
    case EnglishRetractResult::Retracted:
      return "retracted";
    case EnglishRetractResult::MemoryOnly:
      return "memory_only";
    case EnglishRetractResult::NotAdded:
      return "not_added";
    case EnglishRetractResult::Failed:
      return "failed";
  }
  return "failed";
}

EnglishDictionaryStore::EnglishDictionaryStore(const fs::path& configured, LoadListener listener)
    : paths_(EnglishDictionaryPaths::From(configured)), listener_(std::move(listener)) {}

EnglishDictionaryStore::Sample EnglishDictionaryStore::TakeSample() const {
  Sample sample;
  if (!paths_.tsv.empty()) {
    std::error_code ec;
    if (fs::is_regular_file(paths_.tsv, ec)) {
      const auto time = fs::last_write_time(paths_.tsv, ec);
      // Size too: a copy that keeps the timestamp (robocopy, restore) still reloads.
      const auto size = ec ? uintmax_t{0} : fs::file_size(paths_.tsv, ec);
      if (!ec) sample.tsv = std::make_pair(time, size);
    }
  }
  // From the path, not from the mapping: a replaced base is a new file (4.7).
  sample.base = ReadEnglishBaseHeader(paths_.base);
  if (read_hook_) read_hook_();
  sample.overlay = ReadEnglishOverlayHeader(paths_.overlay);
  return sample;
}

std::shared_ptr<const EnglishDictionary> EnglishDictionaryStore::SnapshotFor(const Sample& sample) {
  std::shared_ptr<const EnglishDictionary> previous;
  bool has_previous = false;
  {
    std::lock_guard lock(snapshot_mutex_);
    if (snapshot_sample_ == sample) return snapshot_;
    previous = snapshot_;
    has_previous = loaded_once_;
  }
  // Section 4.7 (A): while a writer or another reader rebuilds (TSV parse,
  // file lock wait, compaction), a reader keeps the previous snapshot instead
  // of waiting. Only the very first load waits.
  std::unique_lock update(update_mutex_, std::try_to_lock);
  if (!update.owns_lock()) {
    if (has_previous) return previous;
    update.lock();
  }
  {
    std::lock_guard lock(snapshot_mutex_);
    if (snapshot_sample_ == sample) return snapshot_;
  }
  try {
    return RebuildLocked(sample);
  } catch (const std::exception&) {
    // An allocation failure on a huge file must not take the query down; the
    // baseline forms still work (section 4.4).
    base_.reset();
    Publish(nullptr, std::nullopt);
    if (listener_) listener_(EnglishDictionaryLoadReport{});
    return nullptr;
  }
}

void EnglishDictionaryStore::Publish(std::shared_ptr<const EnglishDictionary> snapshot,
                                     std::optional<Sample> sample) {
  std::lock_guard lock(snapshot_mutex_);
  snapshot_ = std::move(snapshot);
  snapshot_sample_ = std::move(sample);
  loaded_once_ = true;
}

bool EnglishDictionaryStore::IsCurrent(const Sample& sample) const {
  std::lock_guard lock(snapshot_mutex_);
  return snapshot_sample_ == sample;
}

void EnglishDictionaryStore::Invalidate() {
  std::lock_guard lock(snapshot_mutex_);
  snapshot_sample_.reset();
}

std::shared_ptr<const EnglishDictionary> EnglishDictionaryStore::RebuildLocked(const Sample& sample,
                                                                               bool file_locked) {
  EnglishDictionaryLoadReport report;
  bool loaded = false;
  bool consistent = true;
  std::shared_ptr<const EnglishBaseImage> base;
  // Within this process a TSV whose size changed under the same mtime is
  // compiled again too.
  const bool tsv_changed = sample.tsv && compiled_tsv_ && *compiled_tsv_ != *sample.tsv;
  // This TSV is already compiled into memory because writing or stamping its
  // .bin failed under the lock. A retry would rewrite the .bin, change the
  // sample and fail again on every read, so it waits for a TSV change (4.5).
  const bool bin_matches = sample.base && !tsv_changed &&
                           (!sample.tsv || BaseMatchesTsv(paths_.base, sample.tsv->first));
  if (base_ && base_write_failed_ && sample.tsv && compiled_tsv_ == sample.tsv) {
    base = base_;
  } else if (base_ && journal_unavailable_ && !bin_matches && sample.tsv &&
             compiled_tsv_ == sample.tsv) {
    // Section 4.8: compiled into memory because the journal could not be
    // read. Compiling the same TSV again on every read would fail the same
    // way; a writer that finds the journal readable compiles it again.
    base = base_;
  } else if (bin_matches) {
    // Section 4.5 order: a valid .bin compiled from this TSV (or without one),
    // else the TSV (recompiling the .bin), else no dictionary.
    base_in_memory_ = false;
    base_write_failed_ = false;
    journal_unavailable_ = false;
    if (base_ && base_->header() == *sample.base) {
      base = base_;
    } else {
      base = EnglishBaseImage::Map(paths_.base);
      loaded = true;
      report.source = "bin";
      // Replaced again since the sample; the read retries.
      if (base && base->header() != *sample.base) consistent = false;
    }
  }
  if (!base && sample.tsv) {
    report = {};
    report.source = "tsv";
    loaded = true;
    base = CompileTsvLocked(report, sample.tsv->first, tsv_changed, file_locked);
  }
  compiled_tsv_ = sample.tsv;
  std::vector<EnglishOverlayOp> ops;
  if (base) {
    // An overlay for another base is stale and read as empty (section 4.6).
    auto overlay = ReadEnglishOverlay(paths_.overlay);
    if (overlay && overlay->header.base_fingerprint == base->header().content_hash) {
      ops = std::move(overlay->ops);
    }
    // A valid header whose frames did not read may be a writer mid-way; use
    // the snapshot this once but read the overlay again next time.
    if (!overlay && sample.overlay) consistent = false;
    ops.insert(ops.end(), memory_ops_.begin(), memory_ops_.end());
  }
  auto snapshot = base ? std::make_shared<const EnglishDictionary>(base, ops) : nullptr;
  if (loaded && listener_) {
    report.ok = base != nullptr;
    report.entries = snapshot ? snapshot->size() : 0;
    listener_(report);
  }
  base_ = base;
  Publish(snapshot, consistent ? std::optional<Sample>(sample) : std::nullopt);
  return snapshot;
}

std::shared_ptr<const EnglishBaseImage> EnglishDictionaryStore::CompileTsvLocked(
    EnglishDictionaryLoadReport& report, fs::file_time_type tsv_time, bool force,
    bool file_locked) {
  base_in_memory_ = false;
  base_write_failed_ = false;
  journal_unavailable_ = false;
  const auto text = ReadEnglishTsvFile(paths_.tsv);
  if (!text) return nullptr;
  auto records = ParseEnglishTsv(*text, &report.stats);
  // The .bin is a cache: without the lock or write access (a read-only TSV
  // folder) the parsed TSV is used from memory (section 4.5). The lock is
  // tried once, never waited for: this runs for a reader, or for a writer
  // that has already spent its wait (section 4.7 (A)).
  const auto lock = file_locked ? std::optional<EnglishDictionaryFileLock>{}
                                : EnglishDictionaryFileLock::Acquire(paths_.lock, lock_clock_,
                                                                     std::chrono::milliseconds{0});
  const bool locked = file_locked || lock.has_value();
  if (locked && !force && ReadEnglishBaseHeader(paths_.base) &&
      BaseMatchesTsv(paths_.base, tsv_time)) {
    // Another host recompiled it while this one waited for the lock.
    if (auto mapped = EnglishBaseImage::Map(paths_.base)) return mapped;
  }
  // Section 4.8: runtime additions and deletes outlive the TSV edit. Read
  // after the lock, so an op another host journaled before releasing it is in
  // the base this one writes. A missing or corrupt journal adds nothing (the
  // next writer moves a corrupt one aside). One that cannot be read now must
  // not be taken for missing: the base is then served from memory only, not
  // stamped, and the next writer that holds the lock compiles it again.
  auto journal = LoadEnglishJournal(paths_.journal);
  if (journal.state == EnglishJournalState::Ok) {
    records = ApplyEnglishJournal(std::move(records), journal.contents.ops);
  }
  const bool journal_unavailable = journal.state == EnglishJournalState::Unavailable;
  journal_unavailable_ = journal_unavailable;
  const auto previous = ReadEnglishBaseHeader(paths_.base);
  auto bytes = EncodeEnglishBase(std::move(records), previous ? previous->generation + 1 : 1);
  std::shared_ptr<const EnglishBaseImage> base;
  // Stamped with the mtime the parse saw, so a TSV saved again meanwhile no
  // longer matches it. A .bin whose stamp did not hold is not used: this
  // TSV is served from memory instead.
  if (locked && !journal_unavailable) {
    if (WriteEnglishBase(paths_, bytes, tsv_time, write_hook_)) {
      base = EnglishBaseImage::Map(paths_.base);
    } else {
      base_write_failed_ = true;
    }
  }
  if (base) return base;
  base_in_memory_ = true;
  return EnglishBaseImage::FromBytes(std::move(bytes));
}

std::vector<EnglishDictionaryEntry> EnglishDictionaryStore::Lookup(std::string_view lower_key) {
  return Read([&](const EnglishDictionary* dictionary) {
    return dictionary ? dictionary->Lookup(lower_key) : std::vector<EnglishDictionaryEntry>{};
  });
}

void EnglishDictionaryStore::SettleBaseLocked(bool file_locked) {
  // A base held in memory only because the lock was busy when it was
  // compiled gets its .bin now that this writer holds the lock; the sample
  // alone would not show that anything changed.
  const auto sample = TakeSample();
  // A base held in memory because the journal could not be read is compiled
  // again only once the journal reads (section 4.8); until then each write
  // would recompile the whole TSV under the lock for nothing.
  if (file_locked && base_in_memory_ && journal_unavailable_ &&
      LoadEnglishJournal(paths_.journal).state != EnglishJournalState::Unavailable) {
    journal_unavailable_ = false;
  }
  if (!IsCurrent(sample) ||
      (file_locked && base_in_memory_ && !base_write_failed_ && !journal_unavailable_))
    RebuildLocked(sample, file_locked);
}

EnglishWriteResult EnglishDictionaryStore::AppendLocked(const EnglishOverlayOp& op) {
  bool persisted = false;
  auto result = EnglishWriteResult::MemoryOnly;
  try {
    // The base is settled under the writer lock, so a compaction by another
    // host while this one waited is attached to rather than missed.
    const auto lock = EnglishDictionaryFileLock::Acquire(paths_.lock, lock_clock_);
    SettleBaseLocked(lock.has_value());
    if (!base_) return EnglishWriteResult::Dropped;
    const auto hash = base_->header().content_hash;
    // Only attach to the base on disk; an in-memory base (the .bin could not
    // be written or stamped) keeps its ops in memory.
    const auto header =
        lock && !base_in_memory_ ? ReadEnglishBaseHeader(paths_.base) : std::nullopt;
    if (header && header->content_hash == hash) {
      persisted = AppendEnglishOverlayOp(paths_.overlay, hash, op, write_hook_);
    }
    if (persisted) {
      // Section 4.8: after the overlay, under the same lock. A crash between
      // the two loses the op only at the next TSV recompile.
      switch (AppendEnglishJournalOp(paths_.journal, op, write_hook_)) {
        case EnglishJournalAppendResult::Appended:
          result = EnglishWriteResult::Persisted;
          break;
        case EnglishJournalAppendResult::Reset:
          result = EnglishWriteResult::JournalReset;
          break;
        case EnglishJournalAppendResult::Full:
          result = EnglishWriteResult::JournalFull;
          break;
        case EnglishJournalAppendResult::Failed:
          result = EnglishWriteResult::JournalFailed;
          break;
      }
    }
    // Memory ops replay after the file ops, so an older memory op on the same
    // entry would override this newer persisted one (section 4.6: the later
    // op wins). Drop it; a new memory op replaces older ones the same way.
    // Settled before the compaction below, which may throw.
    memory_ops_.erase(std::remove_if(memory_ops_.begin(), memory_ops_.end(),
                                     [&](const auto& m) { return SameEntry(m, op); }),
                      memory_ops_.end());
    // Without write access or the lock the op lives in memory until restart (4.7).
    if (!persisted) memory_ops_.push_back(op);
    const auto overlay = persisted ? ReadEnglishOverlayHeader(paths_.overlay) : std::nullopt;
    // Section 4.6 trigger: the overlay outgrows a tenth of the base.
    if (overlay &&
        overlay->op_count > std::max<uint32_t>(kMinCompactionOps, header->entry_count / 10)) {
      CompactEnglishDictionary(paths_, write_hook_);
    }
  } catch (const std::exception&) {
    // An allocation failure on a huge base leaves the files as they were. An
    // op not yet persisted is kept in memory when that is still possible.
    if (!persisted) {
      try {
        memory_ops_.push_back(op);
      } catch (const std::exception&) {
        result = EnglishWriteResult::Dropped;
      }
    } else if (result == EnglishWriteResult::MemoryOnly) {
      // In the overlay, but the journal append threw before it finished.
      result = EnglishWriteResult::JournalFailed;
    }
  }
  Invalidate();
  return result;
}

EnglishWriteOutcome EnglishDictionaryStore::Upsert(std::string_view surface, uint32_t frequency,
                                                   uint8_t flags,
                                                   const std::function<bool()>& still_current) {
  if (surface.empty() || surface.size() > 0xFFFF || !core::IsValidUtf8(surface)) return {};
  std::lock_guard lock(update_mutex_);
  if (still_current && !still_current()) return {EnglishWriteResult::Cancelled};
  return {AppendLocked({EnglishOverlayOpKind::Upsert, flags, EnglishLookupKey(surface),
                        std::string(surface), frequency})};
}

EnglishRetractResult EnglishDictionaryStore::Retract(std::string_view surface,
                                                    const std::function<bool()>& still_current) {
  using Result = EnglishRetractResult;
  if (surface.empty() || surface.size() > 0xFFFF || !core::IsValidUtf8(surface))
    return Result::NotAdded;
  std::lock_guard lock(update_mutex_);
  if (still_current && !still_current()) return Result::NotAdded;
  const EnglishOverlayOp op{EnglishOverlayOpKind::Delete, 0, EnglishLookupKey(surface),
                            std::string(surface), 0};
  auto result = Result::Failed;
  try {
    // An upsert this host could not persist lives only in memory_ops_; it is
    // dropped here whatever the files say.
    const auto memory_op = LatestEnglishJournalOp(memory_ops_, op.key, op.surface);
    const bool in_memory = memory_op && memory_op->kind == EnglishOverlayOpKind::Upsert;
    if (in_memory) {
      memory_ops_.erase(std::remove_if(memory_ops_.begin(), memory_ops_.end(),
                                       [&](const auto& m) { return SameEntry(m, op); }),
                        memory_ops_.end());
      result = Result::Retracted;
    }
    // The journal decides what a runtime op added, and only a writer holding
    // the lock reads it consistently with the overlay.
    const auto file_lock = EnglishDictionaryFileLock::Acquire(paths_.lock, lock_clock_);
    if (!file_lock) {
      Invalidate();
      return in_memory ? Result::MemoryOnly : Result::Failed;
    }
    SettleBaseLocked(true);
    const auto journal = LoadEnglishJournal(paths_.journal);
    if (journal.state == EnglishJournalState::Unavailable) {
      Invalidate();
      return Result::Failed;
    }
    const auto latest = journal.state == EnglishJournalState::Ok
                            ? LatestEnglishJournalOp(journal.contents.ops, op.key, op.surface)
                            : std::nullopt;
    if (!latest || latest->kind != EnglishOverlayOpKind::Upsert) {
      Invalidate();
      return in_memory ? result : Result::NotAdded;
    }
    // A TSV that has the word keeps it: the journal loses the entry, and the
    // base compiled from the TSV already shows the TSV's own record.
    bool in_tsv = false;
    std::error_code ec;
    if (!paths_.tsv.empty() && fs::is_regular_file(paths_.tsv, ec)) {
      const auto text = ReadEnglishTsvFile(paths_.tsv);
      if (!text) {
        Invalidate();
        return Result::Failed;
      }
      const auto records = ParseEnglishTsv(*text);
      in_tsv = std::any_of(records.begin(), records.end(), [&](const auto& r) {
        return r.key == op.key && r.surface == op.surface;
      });
    }
    bool hidden_on_disk = in_tsv;
    if (!in_tsv) {
      // The tombstone goes first: should the journal rewrite below fail, the
      // journal still names the word, and a later reset still erases it.
      const auto header =
          base_ && !base_in_memory_ ? ReadEnglishBaseHeader(paths_.base) : std::nullopt;
      if (header && header->content_hash == base_->header().content_hash) {
        hidden_on_disk =
            AppendEnglishOverlayOp(paths_.overlay, header->content_hash, op, write_hook_);
      }
      if (!hidden_on_disk) {
        // Hidden here until restart all the same (section 4.7). The journal
        // keeps the entry: it is then the only record that a runtime upsert
        // added the word, which a reset or the next TSV recompile needs.
        memory_ops_.push_back(op);
        Invalidate();
        return Result::MemoryOnly;
      }
    }
    const auto retracted =
        RetractEnglishJournalEntry(paths_.journal, op.key, op.surface, write_hook_);
    result = retracted == EnglishJournalRetractResult::Failed ? Result::Failed : Result::Retracted;
  } catch (const std::exception&) {
    result = Result::Failed;
  }
  Invalidate();
  return result;
}

bool EnglishDictionaryStore::ResetRuntimeAdditions() {
  std::lock_guard lock(update_mutex_);
  bool ok = false;
  try {
    // This host's own unpersisted ops go whatever the files do.
    memory_ops_.clear();
    const auto file_lock = EnglishDictionaryFileLock::Acquire(paths_.lock, lock_clock_);
    if (file_lock) {
      std::optional<std::vector<EnglishWordRecord>> tsv_records;
      std::optional<fs::file_time_type> tsv_time;
      std::error_code ec;
      bool tsv_ok = true;
      if (!paths_.tsv.empty() && fs::is_regular_file(paths_.tsv, ec)) {
        // The mtime before the parse, as section 4.5 stamps it: a TSV saved
        // again meanwhile no longer matches the new base.
        const auto time = fs::last_write_time(paths_.tsv, ec);
        const auto text = ec ? std::nullopt : ReadEnglishTsvFile(paths_.tsv);
        tsv_ok = text.has_value();
        if (tsv_ok) {
          tsv_records = ParseEnglishTsv(*text);
          tsv_time = time;
        }
      }
      ok = tsv_ok && ResetEnglishRuntimeFiles(paths_, tsv_records, tsv_time, write_hook_);
    }
  } catch (const std::exception&) {
    ok = false;
  }
  // The next read maps what the files now hold; nothing compiled in memory
  // from the old journal is reused.
  base_.reset();
  base_in_memory_ = false;
  base_write_failed_ = false;
  journal_unavailable_ = false;
  compiled_tsv_.reset();
  Invalidate();
  return ok;
}

EnglishWriteOutcome EnglishDictionaryStore::Remove(std::string_view surface) {
  if (surface.empty() || surface.size() > 0xFFFF || !core::IsValidUtf8(surface)) return {};
  std::lock_guard lock(update_mutex_);
  return {AppendLocked(
      {EnglishOverlayOpKind::Delete, 0, EnglishLookupKey(surface), std::string(surface), 0})};
}

bool EnglishDictionaryStore::Compact() {
  std::lock_guard lock(update_mutex_);
  bool ok = false;
  try {
    const auto file_lock = EnglishDictionaryFileLock::Acquire(paths_.lock, lock_clock_);
    ok = file_lock && CompactEnglishDictionary(paths_, write_hook_);
  } catch (const std::exception&) {
    ok = false;  // The old base and overlay stay in place.
  }
  Invalidate();
  return ok;
}

void EnglishDictionaryStore::SetWriteHookForTest(EnglishWriteHook hook) {
  std::lock_guard lock(update_mutex_);
  write_hook_ = std::move(hook);
}

void EnglishDictionaryStore::SetReadHookForTest(std::function<void()> hook) {
  std::lock_guard lock(update_mutex_);
  read_hook_ = std::move(hook);
}

void EnglishDictionaryStore::SetLockClockForTest(EnglishLockClock clock) {
  std::lock_guard lock(update_mutex_);
  lock_clock_ = std::move(clock);
}

}  // namespace azookey::host
