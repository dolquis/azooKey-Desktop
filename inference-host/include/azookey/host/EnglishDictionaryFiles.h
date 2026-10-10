#pragma once

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
// sections 4.5 to 4.7): the compiled base english-words.bin ("AZED"), the
// overlay english-words.delta.bin ("AZEO") and the writer lock
// english-words.lock. All integers are little-endian.

inline constexpr uint32_t kEnglishBaseVersion = 1;
inline constexpr uint32_t kEnglishOverlayVersion = 1;
inline constexpr size_t kEnglishBaseHeaderSize = 32;
inline constexpr size_t kEnglishOverlayHeaderSize = 16;

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
  std::filesystem::path lock;
  // english-words.tsv -> english-words.bin / .delta.bin / .lock (4.5, 4.6, 4.7).
  static EnglishDictionaryPaths From(const std::filesystem::path& configured);
};

// The steps of the section 4.7 write protocols, in the order they happen.
enum class EnglishWriteStep {
  AppendFrameWritten,        // The frame is durable; op_count is not yet raised.
  AppendCountPublished,      // op_count now covers the frame.
  ReinitCountCleared,        // op_count = 0 is durable.
  ReinitTruncated,           // Frames past the header are gone.
  ReinitFingerprintWritten,  // base_fingerprint names the current base.
  CompactBaseReplaced,       // The merged base is live; the overlay is untouched.
};
// Test seam: called after each step; false stops the writer there, as a
// crash at that point would.
using EnglishWriteHook = std::function<bool(EnglishWriteStep)>;

// Section 4.7 (B): the exclusive inter-process writer lock (LockFileEx on
// Windows, flock elsewhere). Released on destruction.
class EnglishDictionaryFileLock {
 public:
  static std::optional<EnglishDictionaryFileLock> Acquire(const std::filesystem::path& path);
  EnglishDictionaryFileLock(EnglishDictionaryFileLock&& other) noexcept;
  EnglishDictionaryFileLock& operator=(EnglishDictionaryFileLock&&) = delete;
  ~EnglishDictionaryFileLock();

 private:
  EnglishDictionaryFileLock() = default;
  intptr_t handle_{-1};
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
// with the new content_hash. A stale overlay is not merged; a base older than
// the TSV is not compacted (false).
bool CompactEnglishDictionary(const EnglishDictionaryPaths& paths,
                              const EnglishWriteHook& hook = {});
// Writes base bytes atomically (temporary file, flush, MoveFileEx). With
// tsv_time (the TSV mtime the bytes reflect), a base mtime older than it is
// raised to it so the next read does not recompile.
bool WriteEnglishBase(const EnglishDictionaryPaths& paths, const std::string& bytes,
                      std::optional<std::filesystem::file_time_type> tsv_time = std::nullopt);

}  // namespace azookey::host
