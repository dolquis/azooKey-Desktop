#include "azookey/host/StoreOnlyZip.h"

#include <algorithm>
#include <array>
#include <set>

namespace azookey::host {
namespace store_only_zip_detail {

constexpr uint32_t kLocalHeaderSignature = 0x04034b50;
constexpr uint32_t kCentralHeaderSignature = 0x02014b50;
constexpr uint32_t kEndOfCentralDirectorySignature = 0x06054b50;
constexpr size_t kLocalHeaderSize = 30;
constexpr size_t kCentralHeaderSize = 46;
constexpr size_t kEndOfCentralDirectorySize = 22;
constexpr size_t kMaxCommentSize = 0xFFFF;
constexpr uint16_t kVersion = 20;  // 2.0: the minimum for STORE entries.
constexpr uint16_t kUtf8NameFlag = 0x0800;
constexpr uint16_t kMethodStore = 0;
constexpr uint16_t kDosTime = 0x0000;  // 00:00:00
constexpr uint16_t kDosDate = 0x0021;  // 1980-01-01

constexpr std::array<uint32_t, 256> MakeCrcTable() {
  std::array<uint32_t, 256> table{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k) c = (c & 1u) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    table[i] = c;
  }
  return table;
}

constexpr std::array<uint32_t, 256> kCrcTable = MakeCrcTable();

uint32_t Crc32(std::string_view data) {
  uint32_t crc = 0xFFFFFFFFu;
  for (char ch : data) {
    crc = kCrcTable[(crc ^ static_cast<uint8_t>(ch)) & 0xFFu] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

void PutU16(std::string& out, uint16_t value) {
  out.push_back(static_cast<char>(value & 0xFFu));
  out.push_back(static_cast<char>((value >> 8) & 0xFFu));
}

void PutU32(std::string& out, uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out.push_back(static_cast<char>((value >> shift) & 0xFFu));
  }
}

// Callers check that offset + 2 / offset + 4 is within bytes.
uint16_t GetU16(std::string_view bytes, size_t offset) {
  return static_cast<uint16_t>(static_cast<uint8_t>(bytes[offset]) |
                               (static_cast<uint8_t>(bytes[offset + 1]) << 8));
}

uint32_t GetU32(std::string_view bytes, size_t offset) {
  return static_cast<uint32_t>(GetU16(bytes, offset)) |
         (static_cast<uint32_t>(GetU16(bytes, offset + 2)) << 16);
}

bool IsSafeEntryName(std::string_view name) {
  if (name.empty() || name.find("..") != std::string_view::npos) return false;
  return std::none_of(name.begin(), name.end(), [](char ch) {
    return ch == '/' || ch == '\\' || ch == ':' || static_cast<uint8_t>(ch) < 0x20;
  });
}

struct CentralEntry {
  uint16_t flags{};
  uint16_t method{};
  uint32_t crc{};
  uint32_t size{};
  uint64_t local_offset{};
  std::string_view name;
};

bool Fail(ZipError* error, ZipError value) {
  if (error) *error = value;
  return false;
}

struct EndOfCentralDirectory {
  uint64_t cd_offset{};
  uint64_t cd_size{};
  size_t entry_count{};
  size_t position{};
};

bool FindEndOfCentralDirectory(std::string_view bytes, EndOfCentralDirectory& eocd,
                               ZipError* error) {
  if (bytes.size() < kEndOfCentralDirectorySize) {
    return Fail(error, ZipError::MissingEndOfCentralDirectory);
  }
  const size_t last = bytes.size() - kEndOfCentralDirectorySize;
  const size_t first = last > kMaxCommentSize ? last - kMaxCommentSize : 0;
  for (size_t pos = last + 1; pos-- > first;) {
    if (GetU32(bytes, pos) != kEndOfCentralDirectorySignature) continue;
    // The comment must end exactly at the end of the file; otherwise this is
    // a signature-like byte sequence inside data or a trailing garbage block.
    if (pos + kEndOfCentralDirectorySize + GetU16(bytes, pos + 20) != bytes.size()) continue;
    const uint16_t disk = GetU16(bytes, pos + 4);
    const uint16_t cd_disk = GetU16(bytes, pos + 6);
    const uint16_t entries_on_disk = GetU16(bytes, pos + 8);
    const uint16_t entries_total = GetU16(bytes, pos + 10);
    if (disk != 0 || cd_disk != 0 || entries_on_disk != entries_total) {
      return Fail(error, ZipError::MultiDisk);
    }
    eocd.entry_count = entries_total;
    eocd.cd_size = GetU32(bytes, pos + 12);
    eocd.cd_offset = GetU32(bytes, pos + 16);
    eocd.position = pos;
    if (eocd.cd_offset + eocd.cd_size > pos) return Fail(error, ZipError::BadOffset);
    return true;
  }
  return Fail(error, ZipError::MissingEndOfCentralDirectory);
}

bool ReadCentralDirectory(std::string_view bytes, const EndOfCentralDirectory& eocd,
                          const ZipLimits& limits, std::vector<CentralEntry>& entries,
                          ZipError* error) {
  if (eocd.entry_count > limits.max_entries) return Fail(error, ZipError::TooManyEntries);
  const size_t cd_end = static_cast<size_t>(eocd.cd_offset + eocd.cd_size);
  size_t pos = static_cast<size_t>(eocd.cd_offset);
  uint64_t total = 0;
  std::set<std::string_view> names;
  for (size_t i = 0; i < eocd.entry_count; ++i) {
    if (cd_end - pos < kCentralHeaderSize || GetU32(bytes, pos) != kCentralHeaderSignature) {
      return Fail(error, ZipError::BadCentralDirectory);
    }
    CentralEntry entry;
    entry.flags = GetU16(bytes, pos + 8);
    entry.method = GetU16(bytes, pos + 10);
    entry.crc = GetU32(bytes, pos + 16);
    const uint32_t compressed = GetU32(bytes, pos + 20);
    entry.size = GetU32(bytes, pos + 24);
    const size_t name_length = GetU16(bytes, pos + 28);
    const size_t extra_length = GetU16(bytes, pos + 30);
    const size_t comment_length = GetU16(bytes, pos + 32);
    const uint16_t start_disk = GetU16(bytes, pos + 34);
    entry.local_offset = GetU32(bytes, pos + 42);
    const size_t variable = name_length + extra_length + comment_length;
    if (cd_end - pos - kCentralHeaderSize < variable) {
      return Fail(error, ZipError::BadCentralDirectory);
    }
    entry.name = bytes.substr(pos + kCentralHeaderSize, name_length);
    pos += kCentralHeaderSize + variable;

    if (start_disk != 0) return Fail(error, ZipError::MultiDisk);
    if ((entry.flags & ~kUtf8NameFlag) != 0) return Fail(error, ZipError::UnsupportedFlags);
    if (entry.method != kMethodStore) return Fail(error, ZipError::UnsupportedMethod);
    if (compressed != entry.size) return Fail(error, ZipError::SizeMismatch);
    if (entry.size > limits.max_entry_bytes) return Fail(error, ZipError::EntryTooLarge);
    total += entry.size;
    if (total > limits.max_total_bytes) return Fail(error, ZipError::TotalTooLarge);
    if (!IsSafeEntryName(entry.name)) return Fail(error, ZipError::InvalidName);
    if (!names.insert(entry.name).second) return Fail(error, ZipError::DuplicateName);
    entries.push_back(entry);
  }
  if (pos != cd_end) return Fail(error, ZipError::BadCentralDirectory);
  return true;
}

// Checks one local header against its central record and returns the data
// offset. data_limit is where the next entry (or the central directory) starts.
bool CheckLocalEntry(std::string_view bytes, const CentralEntry& entry, uint64_t data_limit,
                     size_t& data_offset, ZipError* error) {
  if (entry.local_offset > data_limit || data_limit - entry.local_offset < kLocalHeaderSize) {
    return Fail(error, ZipError::BadOffset);
  }
  const auto pos = static_cast<size_t>(entry.local_offset);
  if (GetU32(bytes, pos) != kLocalHeaderSignature) return Fail(error, ZipError::BadLocalHeader);
  const size_t name_length = GetU16(bytes, pos + 26);
  const size_t extra_length = GetU16(bytes, pos + 28);
  const uint64_t data_start = entry.local_offset + kLocalHeaderSize + name_length + extra_length;
  if (data_start > data_limit || data_limit - data_start < entry.size) {
    return Fail(error, ZipError::OverlappingEntries);
  }
  const std::string_view local_name = bytes.substr(pos + kLocalHeaderSize, name_length);
  if (GetU16(bytes, pos + 6) != entry.flags || GetU16(bytes, pos + 8) != entry.method ||
      GetU32(bytes, pos + 14) != entry.crc || GetU32(bytes, pos + 18) != entry.size ||
      GetU32(bytes, pos + 22) != entry.size || local_name != entry.name) {
    return Fail(error, ZipError::HeaderMismatch);
  }
  data_offset = static_cast<size_t>(data_start);
  if (Crc32(bytes.substr(data_offset, entry.size)) != entry.crc) {
    return Fail(error, ZipError::CrcMismatch);
  }
  return true;
}

}  // namespace store_only_zip_detail

std::string BuildStoreOnlyZip(const std::vector<ZipEntry>& entries) {
  namespace d = store_only_zip_detail;
  std::string out;
  std::string central;
  for (const auto& entry : entries) {
    const auto offset = static_cast<uint32_t>(out.size());
    const uint32_t crc = d::Crc32(entry.data);
    const auto size = static_cast<uint32_t>(entry.data.size());
    const auto name_length = static_cast<uint16_t>(entry.name.size());

    d::PutU32(out, d::kLocalHeaderSignature);
    d::PutU16(out, d::kVersion);
    d::PutU16(out, d::kUtf8NameFlag);
    d::PutU16(out, d::kMethodStore);
    d::PutU16(out, d::kDosTime);
    d::PutU16(out, d::kDosDate);
    d::PutU32(out, crc);
    d::PutU32(out, size);
    d::PutU32(out, size);
    d::PutU16(out, name_length);
    d::PutU16(out, 0);  // extra field length
    out += entry.name;
    out += entry.data;

    d::PutU32(central, d::kCentralHeaderSignature);
    d::PutU16(central, d::kVersion);  // version made by (MS-DOS host)
    d::PutU16(central, d::kVersion);
    d::PutU16(central, d::kUtf8NameFlag);
    d::PutU16(central, d::kMethodStore);
    d::PutU16(central, d::kDosTime);
    d::PutU16(central, d::kDosDate);
    d::PutU32(central, crc);
    d::PutU32(central, size);
    d::PutU32(central, size);
    d::PutU16(central, name_length);
    d::PutU16(central, 0);  // extra field length
    d::PutU16(central, 0);  // comment length
    d::PutU16(central, 0);  // disk number start
    d::PutU16(central, 0);  // internal attributes
    d::PutU32(central, 0);  // external attributes
    d::PutU32(central, offset);
    central += entry.name;
  }
  const auto cd_offset = static_cast<uint32_t>(out.size());
  const auto cd_size = static_cast<uint32_t>(central.size());
  const auto count = static_cast<uint16_t>(entries.size());
  out += central;
  d::PutU32(out, d::kEndOfCentralDirectorySignature);
  d::PutU16(out, 0);  // this disk
  d::PutU16(out, 0);  // central directory disk
  d::PutU16(out, count);
  d::PutU16(out, count);
  d::PutU32(out, cd_size);
  d::PutU32(out, cd_offset);
  d::PutU16(out, 0);  // comment length
  return out;
}

std::optional<std::vector<ZipEntry>> ParseStoreOnlyZip(std::string_view bytes,
                                                       const ZipLimits& limits, ZipError* error) {
  namespace d = store_only_zip_detail;
  if (error) *error = ZipError::None;
  d::EndOfCentralDirectory eocd;
  if (!d::FindEndOfCentralDirectory(bytes, eocd, error)) return std::nullopt;
  std::vector<d::CentralEntry> central;
  if (!d::ReadCentralDirectory(bytes, eocd, limits, central, error)) return std::nullopt;

  // Walk the local entries in file order so that each entry's span must end
  // before the next one starts and before the central directory.
  std::vector<size_t> order(central.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::sort(order.begin(), order.end(),
            [&](size_t a, size_t b) { return central[a].local_offset < central[b].local_offset; });
  std::vector<size_t> data_offsets(central.size());
  for (size_t k = 0; k < order.size(); ++k) {
    const uint64_t limit =
        k + 1 < order.size() ? central[order[k + 1]].local_offset : eocd.cd_offset;
    const auto& entry = central[order[k]];
    if (!d::CheckLocalEntry(bytes, entry, limit, data_offsets[order[k]], error)) {
      return std::nullopt;
    }
  }

  std::vector<ZipEntry> result;
  result.reserve(central.size());
  for (size_t i = 0; i < central.size(); ++i) {
    result.push_back(ZipEntry{std::string(central[i].name),
                              std::string(bytes.substr(data_offsets[i], central[i].size))});
  }
  return result;
}

}  // namespace azookey::host
