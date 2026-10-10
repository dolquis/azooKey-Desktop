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

bool BaseNotOlderThan(const fs::path& base, fs::file_time_type tsv_time) {
  std::error_code ec;
  const auto base_time = fs::last_write_time(base, ec);
  return !ec && base_time >= tsv_time;
}

}  // namespace

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
  {
    std::shared_lock lock(mutex_);
    if (snapshot_sample_ == sample) return snapshot_;
  }
  std::unique_lock lock(mutex_);
  if (snapshot_sample_ == sample) return snapshot_;
  try {
    return RebuildLocked(sample);
  } catch (const std::exception&) {
    // An allocation failure on a huge file must not take the query down; the
    // baseline forms still work (section 4.4).
    base_.reset();
    snapshot_.reset();
    snapshot_sample_.reset();
    if (listener_) listener_(EnglishDictionaryLoadReport{});
    return nullptr;
  }
}

std::shared_ptr<const EnglishDictionary> EnglishDictionaryStore::RebuildLocked(const Sample& sample,
                                                                               bool file_locked) {
  EnglishDictionaryLoadReport report;
  bool loaded = false;
  bool consistent = true;
  std::shared_ptr<const EnglishBaseImage> base;
  // Section 4.5 order: a valid .bin no older than the TSV (or without one),
  // else the TSV (recompiling the .bin), else no dictionary.
  if (sample.base && (!sample.tsv || BaseNotOlderThan(paths_.base, sample.tsv->first))) {
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
    base = CompileTsvLocked(report, sample.tsv->first, file_locked);
  }
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
  snapshot_ = snapshot;
  if (consistent) {
    snapshot_sample_ = sample;
  } else {
    snapshot_sample_.reset();
  }
  return snapshot;
}

std::shared_ptr<const EnglishBaseImage> EnglishDictionaryStore::CompileTsvLocked(
    EnglishDictionaryLoadReport& report, fs::file_time_type tsv_time, bool file_locked) {
  const auto text = ReadEnglishTsvFile(paths_.tsv);
  if (!text) return nullptr;
  auto records = ParseEnglishTsv(*text, &report.stats);
  // The .bin is a cache: without the lock or write access the parsed TSV is
  // used from memory (section 4.5).
  const auto lock = file_locked ? std::optional<EnglishDictionaryFileLock>{}
                                : EnglishDictionaryFileLock::Acquire(paths_.lock);
  const bool locked = file_locked || lock.has_value();
  if (locked && ReadEnglishBaseHeader(paths_.base) && BaseNotOlderThan(paths_.base, tsv_time)) {
    // Another host recompiled it while this one waited for the lock.
    if (auto mapped = EnglishBaseImage::Map(paths_.base)) return mapped;
  }
  const auto previous = ReadEnglishBaseHeader(paths_.base);
  auto bytes = EncodeEnglishBase(std::move(records), previous ? previous->generation + 1 : 1);
  std::shared_ptr<const EnglishBaseImage> base;
  // The mtime the parse saw, so a TSV saved again meanwhile stays newer.
  if (locked && WriteEnglishBase(paths_, bytes, tsv_time))
    base = EnglishBaseImage::Map(paths_.base);
  return base ? base : EnglishBaseImage::FromBytes(std::move(bytes));
}

std::vector<EnglishDictionaryEntry> EnglishDictionaryStore::Lookup(std::string_view lower_key) {
  return Read([&](const EnglishDictionary* dictionary) {
    return dictionary ? dictionary->Lookup(lower_key) : std::vector<EnglishDictionaryEntry>{};
  });
}

bool EnglishDictionaryStore::AppendLocked(EnglishOverlayOp op) {
  bool persisted = false;
  try {
    // The base is settled under the writer lock, so a compaction by another
    // host while this one waited is attached to rather than missed.
    const auto lock = EnglishDictionaryFileLock::Acquire(paths_.lock);
    const auto sample = TakeSample();
    if (snapshot_sample_ != sample) RebuildLocked(sample, lock.has_value());
    if (!base_) return false;
    const auto hash = base_->header().content_hash;
    // Only attach to the base on disk; an in-memory base (the .bin could not
    // be written) keeps its ops in memory.
    const auto header = lock ? ReadEnglishBaseHeader(paths_.base) : std::nullopt;
    if (header && header->content_hash == hash) {
      persisted = AppendEnglishOverlayOp(paths_.overlay, hash, op, write_hook_);
      const auto overlay = ReadEnglishOverlayHeader(paths_.overlay);
      // Section 4.6 trigger: the overlay outgrows a tenth of the base.
      if (persisted && overlay &&
          overlay->op_count > std::max<uint32_t>(kMinCompactionOps, header->entry_count / 10)) {
        CompactEnglishDictionary(paths_, write_hook_);
      }
    }
    // Without write access or the lock the op lives in memory until restart (4.7).
    if (!persisted) memory_ops_.push_back(std::move(op));
  } catch (const std::exception&) {
    // An allocation failure on a huge base leaves the files as they were.
    persisted = false;
  }
  snapshot_sample_.reset();
  return persisted;
}

bool EnglishDictionaryStore::Upsert(std::string_view surface, uint32_t frequency, uint8_t flags) {
  if (surface.empty() || surface.size() > 0xFFFF || !core::IsValidUtf8(surface)) return false;
  std::unique_lock lock(mutex_);
  return AppendLocked({EnglishOverlayOpKind::Upsert, flags, EnglishLookupKey(surface),
                       std::string(surface), frequency});
}

bool EnglishDictionaryStore::Remove(std::string_view surface) {
  if (surface.empty() || surface.size() > 0xFFFF || !core::IsValidUtf8(surface)) return false;
  std::unique_lock lock(mutex_);
  return AppendLocked(
      {EnglishOverlayOpKind::Delete, 0, EnglishLookupKey(surface), std::string(surface), 0});
}

bool EnglishDictionaryStore::Compact() {
  std::unique_lock lock(mutex_);
  bool ok = false;
  try {
    const auto file_lock = EnglishDictionaryFileLock::Acquire(paths_.lock);
    ok = file_lock && CompactEnglishDictionary(paths_, write_hook_);
  } catch (const std::exception&) {
    ok = false;  // The old base and overlay stay in place.
  }
  snapshot_sample_.reset();
  return ok;
}

void EnglishDictionaryStore::SetWriteHookForTest(EnglishWriteHook hook) {
  std::unique_lock lock(mutex_);
  write_hook_ = std::move(hook);
}

void EnglishDictionaryStore::SetReadHookForTest(std::function<void()> hook) {
  std::unique_lock lock(mutex_);
  read_hook_ = std::move(hook);
}

}  // namespace azookey::host
