#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace azookey::host {

// Minimal ZIP container for learning-data backups. Only the subset written by
// BuildStoreOnlyZip is accepted on read: method 0 (STORE), general-purpose
// flags 0 or bit 11 (UTF-8 names), single disk, no zip64, no encryption, no
// data descriptors. Anything else is rejected rather than interpreted.
struct ZipEntry {
  std::string name;
  std::string data;
};

struct ZipLimits {
  size_t max_entries{16};
  uint64_t max_entry_bytes{32ull * 1024 * 1024};
  uint64_t max_total_bytes{64ull * 1024 * 1024};
};

enum class ZipError {
  None,
  MissingEndOfCentralDirectory,
  MultiDisk,
  BadCentralDirectory,
  BadLocalHeader,
  UnsupportedMethod,
  UnsupportedFlags,
  SizeMismatch,
  HeaderMismatch,
  CrcMismatch,
  BadOffset,
  OverlappingEntries,
  InvalidName,
  DuplicateName,
  TooManyEntries,
  EntryTooLarge,
  TotalTooLarge,
};

// Deterministic: the same entries always produce the same bytes (fixed DOS
// timestamp 1980-01-01 00:00, no extra fields, no comments). The caller must
// keep names valid for ParseStoreOnlyZip, each entry below 4 GiB, and the
// archive below 4 GiB with at most 65535 entries; the writer does not check.
std::string BuildStoreOnlyZip(const std::vector<ZipEntry>& entries);

// Never throws on malformed input. Returns entries in central-directory order.
// Size limits are checked against the central directory before any entry data
// is copied. error (optional) receives the first failure, or None on success.
std::optional<std::vector<ZipEntry>> ParseStoreOnlyZip(std::string_view bytes,
                                                       const ZipLimits& limits, ZipError* error);

}  // namespace azookey::host
