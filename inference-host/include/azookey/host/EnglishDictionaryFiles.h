#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace azookey::host {

// M60 English dictionary files (docs/inline-english-candidate-spec.md
// sections 4.5 to 4.8): the compiled base english-words.bin ("AZED"), the
// overlay english-words.delta.bin ("AZEO"), the added-words journal
// english-words.added.bin ("AZEJ") and the writer lock english-words.lock.
// All integers are little-endian.

inline constexpr uint32_t kEnglishBaseVersion = 1;
inline constexpr uint32_t kEnglishOverlayVersion = 1;
inline constexpr size_t kEnglishBaseHeaderSize = 32;
inline constexpr size_t kEnglishOverlayHeaderSize = 16;
// A base loads only up to this many entries: the 200,000 a TSV may hold plus
// as many added by compaction. Compaction stops before it.
inline constexpr uint32_t kMaxEnglishBaseEntries = 400'000;
// Section 4.8: the journal holds at most this many distinct (key, surface)
// entries, so a TSV of 200,000 plus the journal stays under the base bound.
inline constexpr uint32_t kMaxEnglishJournalEntries = 10'000;
inline constexpr uint32_t kEnglishJournalVersion = 1;

struct EnglishBaseHeader {
  uint32_t version{};
  uint32_t entry_count{};
  uint32_t flags{};
  uint32_t records_offset{};
  uint32_t strings_offset{};
  uint32_t generation{};
  uint32_t content_hash{};
  bool operator==(const EnglishBaseHeader&) const = default;
};

struct EnglishWordRecord {
  std::string key;  // The lowercased surface.
  std::string surface;
  uint32_t frequency{};
  uint8_t flags{};  // EnglishWordFlag bits.
};

// Encodes records into a base image: records sorted by key, then by frequency
// highest first (ties keep the input order). The header carries generation and
// the section 4.6 content_hash.
std::string EncodeEnglishBase(std::vector<EnglishWordRecord> records, uint32_t generation);

// A validated, immutable base: a read-only mapping of the file or owned bytes.
class EnglishBaseImage {
 public:
  // nullptr when the bytes are not a version 1 base of a consistent size.
  static std::shared_ptr<const EnglishBaseImage> FromBytes(std::string bytes);
  // nullptr when the file is missing, unreadable, too large or not a valid
  // base. The file handle is closed once mapped, so a writer can replace the
  // file with MoveFileEx while this mapping stays on the old file (4.7).
  static std::shared_ptr<const EnglishBaseImage> Map(const std::filesystem::path& path);
  ~EnglishBaseImage();
  EnglishBaseImage(const EnglishBaseImage&) = delete;
  EnglishBaseImage& operator=(const EnglishBaseImage&) = delete;

  const EnglishBaseHeader& header() const { return header_; }
  // Binary search on the key; the run under it, frequency highest first.
  std::vector<EnglishWordRecord> Lookup(std::string_view key) const;
  std::vector<EnglishWordRecord> Records() const;

 private:
  EnglishBaseImage() = default;
  bool Validate();
  std::optional<EnglishWordRecord> RecordAt(size_t index) const;
  std::string_view KeyAt(size_t index) const;

  const uint8_t* data_{nullptr};
  size_t size_{0};
  std::string owned_;
  void* view_{nullptr};
  EnglishBaseHeader header_;
};

// Reads the 32-byte header of the file now at path, not of any mapping, so a
// replaced base is noticed (4.7). nullopt when missing or not a valid base.
std::optional<EnglishBaseHeader> ReadEnglishBaseHeader(const std::filesystem::path& path);

enum class EnglishOverlayOpKind : uint8_t { Upsert = 0, Delete = 1 };

struct EnglishOverlayOp {
  EnglishOverlayOpKind kind{EnglishOverlayOpKind::Upsert};
  uint8_t flags{};
  std::string key;
  std::string surface;
  uint32_t frequency{};
};

struct EnglishOverlayHeader {
  uint32_t base_fingerprint{};
  uint32_t op_count{};
  bool operator==(const EnglishOverlayHeader&) const = default;
};

struct EnglishOverlayContents {
  EnglishOverlayHeader header;
  std::vector<EnglishOverlayOp> ops;  // Arrival order; a later op wins.
};

std::optional<EnglishOverlayHeader> ReadEnglishOverlayHeader(const std::filesystem::path& path);
// The first op_count frames. nullopt when missing, not an overlay, or shorter
// than op_count frames (corrupt, or read while a writer reinitializes it).
std::optional<EnglishOverlayContents> ReadEnglishOverlay(const std::filesystem::path& path);

struct EnglishDictionaryPaths {
  std::filesystem::path tsv;  // Empty when the configured path is the .bin itself.
  std::filesystem::path base;
  std::filesystem::path overlay;
  std::filesystem::path journal;
  std::filesystem::path lock;
  // english-words.tsv -> english-words.bin / .delta.bin / .added.bin / .lock
  // (4.5 to 4.8).
  static EnglishDictionaryPaths From(const std::filesystem::path& configured);
};

// The steps of the section 4.7 write protocols, in the order they happen.
enum class EnglishWriteStep {
  AppendFrameWritten,        // The frame is durable; op_count is not yet raised.
  AppendCountPublished,      // op_count now covers the frame.
  ReinitCountCleared,        // op_count = 0 is durable.
  ReinitTruncated,           // Frames past the header are gone.
  ReinitFingerprintWritten,  // base_fingerprint names the current base.
  BaseStamped,               // A new base is written and stamped, not yet renamed.
  CompactBaseReplaced,       // The merged base is live; the overlay is untouched.
  JournalFrameWritten,       // A journal frame is durable; its op_count is not yet raised.
  JournalCountPublished,     // The journal's op_count now covers the frame.
  JournalRewriteWritten,     // A deduplicated journal is written, not yet renamed.
};
// Test seam: called after each step; false stops the writer there, as a
// crash at that point would.
using EnglishWriteHook = std::function<bool(EnglishWriteStep)>;

// Test seam for the writer lock wait: now and sleep stand in for steady_clock
// and sleep_for. sleep is called only after an attempt found the lock held.
struct EnglishLockClock {
  std::function<std::chrono::steady_clock::time_point()> now;
  std::function<void(std::chrono::steady_clock::duration)> sleep;
};

// Section 4.7 (B): how long a writer that appends or compacts waits for the
// lock, and how often it polls meanwhile.
inline constexpr std::chrono::milliseconds kEnglishLockBudget{500};
inline constexpr std::chrono::milliseconds kEnglishLockPoll{10};

// Section 4.7 (B): the exclusive inter-process writer lock (LockFileEx on
// Windows, flock elsewhere). Released on destruction.
class EnglishDictionaryFileLock {
 public:
  // Polls every kEnglishLockPoll until budget runs out; a zero budget tries
  // once and never sleeps.
  static std::optional<EnglishDictionaryFileLock> Acquire(
      const std::filesystem::path& path, const EnglishLockClock& clock = {},
      std::chrono::milliseconds budget = kEnglishLockBudget);
  EnglishDictionaryFileLock(EnglishDictionaryFileLock&& other) noexcept;
  EnglishDictionaryFileLock& operator=(EnglishDictionaryFileLock&&) = delete;
  ~EnglishDictionaryFileLock();

 private:
  EnglishDictionaryFileLock() = default;
  intptr_t handle_{-1};
};

// Section 4.8: the journal of runtime upserts and deletes that a TSV
// recompile replays. Same frames as the overlay, under a 16-byte header with
// magic "AZEJ" and no base fingerprint.
struct EnglishJournalContents {
  uint32_t op_count{};
  std::vector<EnglishOverlayOp> ops;  // Arrival order; a later op wins.
};
// The first op_count frames. nullopt when missing, not a journal, or shorter
// than op_count frames.
std::optional<EnglishJournalContents> ReadEnglishJournal(const std::filesystem::path& path);

// Section 4.8 merge for a TSV recompile. Per (key, surface), the latest
// journal op decides: a delete hides the TSV record, an upsert adds the entry
// only when the TSV lacks it (the TSV's frequency and flags win). At most
// kMaxEnglishJournalEntries entries are applied.
std::vector<EnglishWordRecord> ApplyEnglishJournal(std::vector<EnglishWordRecord> records,
                                                   const std::vector<EnglishOverlayOp>& ops);

enum class EnglishJournalAppendResult {
  Appended,
  // The journal was unreadable; it was moved to .corrupt.<stamp> and the op
  // starts a new one.
  Reset,
  // The op would add an entry past kMaxEnglishJournalEntries; not written.
  Full,
  Failed,
};

// Writers. Each expects the caller to hold EnglishDictionaryFileLock.
//
// Appends op after checking the overlay against base_content_hash: a missing,
// corrupt or mismatched overlay is reinitialized first, and a torn frame past
// op_count is truncated. Frame, flush, then op_count + 1, flush.
bool AppendEnglishOverlayOp(const std::filesystem::path& overlay, uint32_t base_content_hash,
                            const EnglishOverlayOp& op, const EnglishWriteHook& hook = {});
// op_count = 0 (flushed), truncate, then base_fingerprint (flushed).
bool ReinitEnglishOverlay(const std::filesystem::path& overlay, uint32_t base_content_hash,
                          const EnglishWriteHook& hook = {});
// Merges the base with an overlay that matches it into a new base with
// generation + 1, replaces the base atomically, then reinitializes the overlay
// with the new content_hash. A stale overlay is not merged. false, leaving
// both files, when the base's mtime is not the TSV's (it is about to be
// recompiled), when an overlay for this base cannot be read whole, or when the
// merge would exceed kMaxEnglishBaseEntries.
bool CompactEnglishDictionary(const EnglishDictionaryPaths& paths,
                              const EnglishWriteHook& hook = {});
// Writes base bytes atomically: temporary file, flush, then with tsv_time (the
// TSV mtime the bytes reflect) its mtime set to it and read back, then
// MoveFileEx (rename keeps the mtime). Section 4.5 reads the .bin only while
// its mtime equals the TSV's. false, with the live base untouched, when any
// step fails, including an mtime that cannot be set or reads back rounded.
bool WriteEnglishBase(const EnglishDictionaryPaths& paths, const std::string& bytes,
                      std::optional<std::filesystem::file_time_type> tsv_time = std::nullopt,
                      const EnglishWriteHook& hook = {});
// Appends op to the journal: frame, flush, then op_count + 1, flush, as the
// overlay does; a torn frame past op_count is truncated first. When op_count
// outgrows twice the distinct entries (and kMinEnglishJournalRewriteOps), the
// journal is instead rewritten with the latest op per entry and replaced
// atomically.
EnglishJournalAppendResult AppendEnglishJournalOp(const std::filesystem::path& journal,
                                                  const EnglishOverlayOp& op,
                                                  const EnglishWriteHook& hook = {});
inline constexpr uint32_t kMinEnglishJournalRewriteOps = 256;

}  // namespace azookey::host
