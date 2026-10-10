#include "azookey/host/EnglishDictionaryFiles.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <map>
#include <system_error>
#include <thread>
#include <utility>

#include "azookey/core/DoubleArrayTrie.h"
#include "azookey/learning/AtomicFile.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace azookey::host {

namespace {

namespace fs = std::filesystem;

constexpr char kBaseMagic[4] = {'A', 'Z', 'E', 'D'};
constexpr char kOverlayMagic[4] = {'A', 'Z', 'E', 'O'};
constexpr size_t kRecordSize = 20;
constexpr size_t kFrameHeaderSize = 10;
constexpr uint32_t kSortedFlag = 1;
// A base holds at most 200,000 entries; the bound keeps a hostile file from
// being mapped whole. The overlay stays small through compaction.
constexpr uint64_t kMaxBaseBytes = 256ULL * 1024 * 1024;
constexpr uint64_t kMaxOverlayBytes = 64ULL * 1024 * 1024;
constexpr auto kLockBudget = std::chrono::milliseconds(500);

uint32_t GetU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
         static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
}
uint16_t GetU16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | static_cast<uint16_t>(p[1]) << 8);
}
void PutU32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
void PutU16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}
const uint8_t* Bytes(const std::string& s) { return reinterpret_cast<const uint8_t*>(s.data()); }

// FNV-1a, section 4.6: version, entry_count and generation, then the record
// array and the string pool (everything but the structural header fields).
uint32_t Fnv1a(uint32_t hash, const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    hash ^= p[i];
    hash *= 16777619u;
  }
  return hash;
}
uint32_t ContentHash(const uint8_t* data, size_t size, const EnglishBaseHeader& header) {
  uint8_t fields[12];
  PutU32(fields, header.version);
  PutU32(fields + 4, header.entry_count);
  PutU32(fields + 8, header.generation);
  uint32_t hash = Fnv1a(2166136261u, fields, sizeof(fields));
  hash = Fnv1a(hash, data + header.records_offset, size_t{header.entry_count} * kRecordSize);
  return Fnv1a(hash, data + header.strings_offset, size - header.strings_offset);
}

std::optional<EnglishBaseHeader> ParseBaseHeader(const uint8_t* data, size_t size) {
  if (size < kEnglishBaseHeaderSize || std::memcmp(data, kBaseMagic, 4) != 0) return std::nullopt;
  EnglishBaseHeader h;
  h.version = GetU32(data + 4);
  h.entry_count = GetU32(data + 8);
  h.flags = GetU32(data + 12);
  h.records_offset = GetU32(data + 16);
  h.strings_offset = GetU32(data + 20);
  h.generation = GetU32(data + 24);
  h.content_hash = GetU32(data + 28);
  if (h.version != kEnglishBaseVersion || !(h.flags & kSortedFlag)) return std::nullopt;
  return h;
}

// Positional reads and writes on a file every process opens with
// FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE (section 4.7).
class RawFile {
 public:
  static std::optional<RawFile> Open(const fs::path& path, bool write) {
    RawFile file;
#ifdef _WIN32
    const HANDLE h =
        CreateFileW(path.c_str(), GENERIC_READ | (write ? GENERIC_WRITE : 0),
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    write ? OPEN_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::nullopt;
    file.handle_ = reinterpret_cast<intptr_t>(h);
#else
    const int fd = open(path.c_str(), write ? (O_RDWR | O_CREAT) : O_RDONLY, 0644);
    if (fd < 0) return std::nullopt;
    file.handle_ = fd;
#endif
    return file;
  }
  RawFile(RawFile&& other) noexcept : handle_(std::exchange(other.handle_, -1)) {}
  RawFile& operator=(RawFile&&) = delete;
  ~RawFile() {
    if (handle_ == -1) return;
#ifdef _WIN32
    CloseHandle(reinterpret_cast<HANDLE>(handle_));
#else
    close(static_cast<int>(handle_));
#endif
  }

  std::optional<uint64_t> Size() const {
#ifdef _WIN32
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(reinterpret_cast<HANDLE>(handle_), &length)) return std::nullopt;
    return static_cast<uint64_t>(length.QuadPart);
#else
    struct stat info {};
    if (fstat(static_cast<int>(handle_), &info) != 0) return std::nullopt;
    return static_cast<uint64_t>(info.st_size);
#endif
  }
  bool ReadAt(uint64_t offset, void* buffer, size_t n) const {
#ifdef _WIN32
    OVERLAPPED at{};
    at.Offset = static_cast<DWORD>(offset);
    at.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD read = 0;
    return ReadFile(reinterpret_cast<HANDLE>(handle_), buffer, static_cast<DWORD>(n), &read, &at) &&
           read == n;
#else
    return pread(static_cast<int>(handle_), buffer, n, static_cast<off_t>(offset)) ==
           static_cast<ssize_t>(n);
#endif
  }
  bool WriteAt(uint64_t offset, const void* buffer, size_t n) {
#ifdef _WIN32
    OVERLAPPED at{};
    at.Offset = static_cast<DWORD>(offset);
    at.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD written = 0;
    return WriteFile(reinterpret_cast<HANDLE>(handle_), buffer, static_cast<DWORD>(n), &written,
                     &at) &&
           written == n;
#else
    return pwrite(static_cast<int>(handle_), buffer, n, static_cast<off_t>(offset)) ==
           static_cast<ssize_t>(n);
#endif
  }
  bool Flush() {
#ifdef _WIN32
    return FlushFileBuffers(reinterpret_cast<HANDLE>(handle_)) != 0;
#else
    return fsync(static_cast<int>(handle_)) == 0;
#endif
  }
  bool Truncate(uint64_t size) {
#ifdef _WIN32
    LARGE_INTEGER at{};
    at.QuadPart = static_cast<LONGLONG>(size);
    const auto h = reinterpret_cast<HANDLE>(handle_);
    return SetFilePointerEx(h, at, nullptr, FILE_BEGIN) && SetEndOfFile(h);
#else
    return ftruncate(static_cast<int>(handle_), static_cast<off_t>(size)) == 0;
#endif
  }
  // Reads [offset, end of file), bounded by limit.
  std::optional<std::string> ReadRest(uint64_t offset, uint64_t limit) const {
    const auto size = Size();
    if (!size || *size < offset || *size - offset > limit) return std::nullopt;
    std::string out(static_cast<size_t>(*size - offset), '\0');
    if (!out.empty() && !ReadAt(offset, out.data(), out.size())) return std::nullopt;
    return out;
  }

 private:
  RawFile() = default;
  intptr_t handle_{-1};
};

std::string EncodeFrame(const EnglishOverlayOp& op) {
  std::string frame(kFrameHeaderSize, '\0');
  auto* p = reinterpret_cast<uint8_t*>(frame.data());
  p[0] = static_cast<uint8_t>(op.kind);
  p[1] = op.flags;
  PutU16(p + 2, static_cast<uint16_t>(op.key.size()));
  PutU16(p + 4, static_cast<uint16_t>(op.surface.size()));
  PutU32(p + 6, op.frequency);
  return frame + op.key + op.surface;
}

// Walks count frames; the bytes they take, or nullopt when they run past the
// data or hold anything but an upsert/delete with UTF-8 strings.
std::optional<size_t> WalkFrames(std::string_view data, uint32_t count,
                                 std::vector<EnglishOverlayOp>* ops) {
  size_t at = 0;
  for (uint32_t i = 0; i < count; ++i) {
    if (data.size() - at < kFrameHeaderSize) return std::nullopt;
    const auto* p = reinterpret_cast<const uint8_t*>(data.data() + at);
    const size_t key_len = GetU16(p + 2);
    const size_t surface_len = GetU16(p + 4);
    if (p[0] > 1 || data.size() - at - kFrameHeaderSize < key_len + surface_len)
      return std::nullopt;
    EnglishOverlayOp op;
    op.kind = static_cast<EnglishOverlayOpKind>(p[0]);
    op.flags = p[1];
    op.frequency = GetU32(p + 6);
    op.key = std::string(data.substr(at + kFrameHeaderSize, key_len));
    op.surface = std::string(data.substr(at + kFrameHeaderSize + key_len, surface_len));
    if (op.key.empty() || !core::IsValidUtf8(op.key) || !core::IsValidUtf8(op.surface))
      return std::nullopt;
    if (ops) ops->push_back(std::move(op));
    at += kFrameHeaderSize + key_len + surface_len;
  }
  return at;
}

std::optional<EnglishOverlayHeader> ParseOverlayHeader(const uint8_t* p) {
  if (std::memcmp(p, kOverlayMagic, 4) != 0 || GetU32(p + 4) != kEnglishOverlayVersion)
    return std::nullopt;
  return EnglishOverlayHeader{GetU32(p + 8), GetU32(p + 12)};
}

bool Step(const EnglishWriteHook& hook, EnglishWriteStep step) { return !hook || hook(step); }

bool Reinit(RawFile& file, uint32_t base_content_hash, const EnglishWriteHook& hook) {
  // Section 4.7: publish the empty state first. Writing the fingerprint first
  // would let a reader replay the old frames against the new base.
  uint8_t zero[4] = {};
  if (!file.WriteAt(12, zero, 4) || !file.Flush()) return false;
  if (!Step(hook, EnglishWriteStep::ReinitCountCleared)) return false;
  if (!file.Truncate(kEnglishOverlayHeaderSize) || !file.Flush()) return false;
  if (!Step(hook, EnglishWriteStep::ReinitTruncated)) return false;
  uint8_t head[12];
  std::memcpy(head, kOverlayMagic, 4);
  PutU32(head + 4, kEnglishOverlayVersion);
  PutU32(head + 8, base_content_hash);
  if (!file.WriteAt(0, head, sizeof(head)) || !file.Flush()) return false;
  return Step(hook, EnglishWriteStep::ReinitFingerprintWritten);
}

std::optional<fs::file_time_type> TsvTime(const EnglishDictionaryPaths& paths) {
  std::error_code ec;
  if (paths.tsv.empty() || !fs::is_regular_file(paths.tsv, ec)) return std::nullopt;
  const auto time = fs::last_write_time(paths.tsv, ec);
  if (ec) return std::nullopt;
  return time;
}

}  // namespace

std::string EncodeEnglishBase(std::vector<EnglishWordRecord> records, uint32_t generation) {
  records.erase(std::remove_if(records.begin(), records.end(),
                               [](const EnglishWordRecord& r) {
                                 return r.key.empty() || r.key.size() > 0xFFFF ||
                                        r.surface.size() > 0xFFFF;
                               }),
                records.end());
  std::stable_sort(records.begin(), records.end(), [](const auto& l, const auto& r) {
    if (l.key != r.key) return l.key < r.key;
    return l.frequency > r.frequency;
  });
  const size_t strings_offset = kEnglishBaseHeaderSize + records.size() * kRecordSize;
  std::string out(strings_offset, '\0');
  for (size_t i = 0; i < records.size(); ++i) {
    const auto& r = records[i];
    const auto key_offset = static_cast<uint32_t>(out.size());
    out += r.key;
    uint32_t surface_offset = key_offset;
    if (r.surface != r.key) {
      surface_offset = static_cast<uint32_t>(out.size());
      out += r.surface;
    }
    auto* p = reinterpret_cast<uint8_t*>(out.data()) + kEnglishBaseHeaderSize + i * kRecordSize;
    PutU32(p, key_offset);
    PutU16(p + 4, static_cast<uint16_t>(r.key.size()));
    PutU16(p + 6, static_cast<uint16_t>(r.surface.size()));
    PutU32(p + 8, surface_offset);
    PutU32(p + 12, r.frequency);
    p[16] = r.flags;
  }
  EnglishBaseHeader header;
  header.version = kEnglishBaseVersion;
  header.entry_count = static_cast<uint32_t>(records.size());
  header.flags = kSortedFlag;
  header.records_offset = static_cast<uint32_t>(kEnglishBaseHeaderSize);
  header.strings_offset = static_cast<uint32_t>(strings_offset);
  header.generation = generation;
  header.content_hash = ContentHash(Bytes(out), out.size(), header);
  auto* p = reinterpret_cast<uint8_t*>(out.data());
  std::memcpy(p, kBaseMagic, 4);
  PutU32(p + 4, header.version);
  PutU32(p + 8, header.entry_count);
  PutU32(p + 12, header.flags);
  PutU32(p + 16, header.records_offset);
  PutU32(p + 20, header.strings_offset);
  PutU32(p + 24, header.generation);
  PutU32(p + 28, header.content_hash);
  return out;
}

std::shared_ptr<const EnglishBaseImage> EnglishBaseImage::FromBytes(std::string bytes) {
  std::shared_ptr<EnglishBaseImage> image(new EnglishBaseImage());
  image->owned_ = std::move(bytes);
  image->data_ = Bytes(image->owned_);
  image->size_ = image->owned_.size();
  if (!image->Validate()) return nullptr;
  return image;
}

std::shared_ptr<const EnglishBaseImage> EnglishBaseImage::Map(const fs::path& path) {
  std::shared_ptr<EnglishBaseImage> image(new EnglishBaseImage());
#ifdef _WIN32
  const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return nullptr;
  LARGE_INTEGER length{};
  HANDLE mapping = nullptr;
  if (GetFileSizeEx(file, &length) &&
      length.QuadPart >= static_cast<LONGLONG>(kEnglishBaseHeaderSize) &&
      static_cast<uint64_t>(length.QuadPart) <= kMaxBaseBytes) {
    mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
  }
  // The view keeps the section alive. An open handle on the base would make
  // a writer's MoveFileEx over it fail even with FILE_SHARE_DELETE.
  CloseHandle(file);
  if (!mapping) return nullptr;
  image->view_ = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
  CloseHandle(mapping);
  if (!image->view_) return nullptr;
  image->size_ = static_cast<size_t>(length.QuadPart);
#else
  const int file = open(path.c_str(), O_RDONLY);
  if (file < 0) return nullptr;
  struct stat info {};
  void* view = MAP_FAILED;
  if (fstat(file, &info) == 0 && info.st_size >= static_cast<off_t>(kEnglishBaseHeaderSize) &&
      static_cast<uint64_t>(info.st_size) <= kMaxBaseBytes) {
    view = mmap(nullptr, static_cast<size_t>(info.st_size), PROT_READ, MAP_PRIVATE, file, 0);
  }
  close(file);
  if (view == MAP_FAILED) return nullptr;
  image->view_ = view;
  image->size_ = static_cast<size_t>(info.st_size);
#endif
  image->data_ = static_cast<const uint8_t*>(image->view_);
  if (!image->Validate()) return nullptr;
  return image;
}

EnglishBaseImage::~EnglishBaseImage() {
  if (!view_) return;
#ifdef _WIN32
  UnmapViewOfFile(view_);
#else
  munmap(view_, size_);
#endif
}

bool EnglishBaseImage::Validate() {
  if (size_ > kMaxBaseBytes) return false;
  const auto header = ParseBaseHeader(data_, size_);
  if (!header || header->records_offset < kEnglishBaseHeaderSize ||
      header->strings_offset > size_ ||
      uint64_t{header->records_offset} + uint64_t{header->entry_count} * kRecordSize > size_) {
    return false;
  }
  header_ = *header;
  return true;
}

std::string_view EnglishBaseImage::KeyAt(size_t index) const {
  const auto* p = data_ + header_.records_offset + index * kRecordSize;
  const uint64_t offset = GetU32(p);
  const uint64_t length = GetU16(p + 4);
  // Out-of-range records read as an empty key and never match.
  if (offset < header_.strings_offset || offset + length > size_) return {};
  return {reinterpret_cast<const char*>(data_ + offset), static_cast<size_t>(length)};
}

std::optional<EnglishWordRecord> EnglishBaseImage::RecordAt(size_t index) const {
  const auto* p = data_ + header_.records_offset + index * kRecordSize;
  const uint64_t offset = GetU32(p + 8);
  const uint64_t length = GetU16(p + 6);
  const auto key = KeyAt(index);
  if (key.empty() || offset < header_.strings_offset || offset + length > size_)
    return std::nullopt;
  EnglishWordRecord record;
  record.key = std::string(key);
  record.surface.assign(reinterpret_cast<const char*>(data_ + offset), static_cast<size_t>(length));
  record.frequency = GetU32(p + 12);
  record.flags = p[16];
  // A surface that is not UTF-8 would make the whole IPC response unparseable.
  if (record.surface.empty() || !core::IsValidUtf8(record.surface)) return std::nullopt;
  return record;
}

std::vector<EnglishWordRecord> EnglishBaseImage::Lookup(std::string_view key) const {
  size_t low = 0;
  size_t high = header_.entry_count;
  while (low < high) {
    const size_t mid = low + (high - low) / 2;
    if (KeyAt(mid) < key) {
      low = mid + 1;
    } else {
      high = mid;
    }
  }
  std::vector<EnglishWordRecord> out;
  for (size_t i = low; i < header_.entry_count && KeyAt(i) == key; ++i) {
    if (auto record = RecordAt(i)) out.push_back(std::move(*record));
  }
  return out;
}

std::vector<EnglishWordRecord> EnglishBaseImage::Records() const {
  std::vector<EnglishWordRecord> out;
  out.reserve(header_.entry_count);
  for (size_t i = 0; i < header_.entry_count; ++i) {
    if (auto record = RecordAt(i)) out.push_back(std::move(*record));
  }
  return out;
}

std::optional<EnglishBaseHeader> ReadEnglishBaseHeader(const fs::path& path) {
  const auto file = RawFile::Open(path, false);
  uint8_t head[kEnglishBaseHeaderSize];
  if (!file || !file->ReadAt(0, head, sizeof(head))) return std::nullopt;
  return ParseBaseHeader(head, sizeof(head));
}

std::optional<EnglishOverlayHeader> ReadEnglishOverlayHeader(const fs::path& path) {
  const auto file = RawFile::Open(path, false);
  uint8_t head[kEnglishOverlayHeaderSize];
  if (!file || !file->ReadAt(0, head, sizeof(head))) return std::nullopt;
  return ParseOverlayHeader(head);
}

std::optional<EnglishOverlayContents> ReadEnglishOverlay(const fs::path& path) {
  const auto file = RawFile::Open(path, false);
  uint8_t head[kEnglishOverlayHeaderSize];
  if (!file || !file->ReadAt(0, head, sizeof(head))) return std::nullopt;
  const auto header = ParseOverlayHeader(head);
  if (!header) return std::nullopt;
  // op_count first, then only that many frames: a torn frame past it from a
  // crashed writer is never replayed (section 4.7).
  const auto rest = file->ReadRest(kEnglishOverlayHeaderSize, kMaxOverlayBytes);
  if (!rest) return std::nullopt;
  EnglishOverlayContents contents;
  contents.header = *header;
  if (!WalkFrames(*rest, header->op_count, &contents.ops)) return std::nullopt;
  return contents;
}

EnglishDictionaryPaths EnglishDictionaryPaths::From(const fs::path& configured) {
  EnglishDictionaryPaths paths;
  auto extension = configured.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(), [](char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
  });
  paths.base = configured;
  if (extension == ".bin") {
    // A bundled .bin with no TSV source (section 4.5).
  } else {
    paths.tsv = configured;
    paths.base.replace_extension(".bin");
  }
  const auto stem = paths.base.stem();
  paths.overlay = paths.base.parent_path() / stem;
  paths.overlay += ".delta.bin";
  paths.lock = paths.base.parent_path() / stem;
  paths.lock += ".lock";
  return paths;
}

std::optional<EnglishDictionaryFileLock> EnglishDictionaryFileLock::Acquire(const fs::path& path) {
  EnglishDictionaryFileLock lock;
  const auto deadline = std::chrono::steady_clock::now() + kLockBudget;
#ifdef _WIN32
  const HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return std::nullopt;
  lock.handle_ = reinterpret_cast<intptr_t>(h);
  while (true) {
    OVERLAPPED at{};
    if (LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &at))
      return lock;
    if (GetLastError() != ERROR_LOCK_VIOLATION || std::chrono::steady_clock::now() >= deadline)
      return std::nullopt;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
#else
  const int fd = open(path.c_str(), O_RDWR | O_CREAT, 0644);
  if (fd < 0) return std::nullopt;
  lock.handle_ = fd;
  while (true) {
    if (flock(fd, LOCK_EX | LOCK_NB) == 0) return lock;
    if (errno != EWOULDBLOCK || std::chrono::steady_clock::now() >= deadline) return std::nullopt;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
#endif
}

EnglishDictionaryFileLock::EnglishDictionaryFileLock(EnglishDictionaryFileLock&& other) noexcept
    : handle_(std::exchange(other.handle_, -1)) {}

EnglishDictionaryFileLock::~EnglishDictionaryFileLock() {
  if (handle_ == -1) return;
#ifdef _WIN32
  // Closing the handle releases the byte-range lock.
  CloseHandle(reinterpret_cast<HANDLE>(handle_));
#else
  close(static_cast<int>(handle_));
#endif
}

bool AppendEnglishOverlayOp(const fs::path& overlay, uint32_t base_content_hash,
                            const EnglishOverlayOp& op, const EnglishWriteHook& hook) {
  if (op.key.empty() || op.key.size() > 0xFFFF || op.surface.size() > 0xFFFF) return false;
  auto file = RawFile::Open(overlay, true);
  if (!file) return false;
  // Section 4.7 attach check: a missing, corrupt or stale overlay is
  // reinitialized for the current base before anything is appended to it.
  std::optional<size_t> end;
  uint32_t op_count = 0;
  uint8_t head[kEnglishOverlayHeaderSize];
  if (file->ReadAt(0, head, sizeof(head))) {
    const auto header = ParseOverlayHeader(head);
    if (header && header->base_fingerprint == base_content_hash) {
      if (const auto rest = file->ReadRest(kEnglishOverlayHeaderSize, kMaxOverlayBytes)) {
        if (const auto walked = WalkFrames(*rest, header->op_count, nullptr)) {
          end = kEnglishOverlayHeaderSize + *walked;
          op_count = header->op_count;
        }
      }
    }
  }
  if (!end) {
    if (!Reinit(*file, base_content_hash, hook)) return false;
    end = kEnglishOverlayHeaderSize;
  } else {
    // Drop a torn frame a crashed writer left past op_count.
    const auto size = file->Size();
    if (!size || (*size > *end && (!file->Truncate(*end) || !file->Flush()))) return false;
  }
  const auto frame = EncodeFrame(op);
  if (!file->WriteAt(*end, frame.data(), frame.size()) || !file->Flush()) return false;
  if (!Step(hook, EnglishWriteStep::AppendFrameWritten)) return false;
  uint8_t count[4];
  PutU32(count, op_count + 1);
  if (!file->WriteAt(12, count, 4) || !file->Flush()) return false;
  return Step(hook, EnglishWriteStep::AppendCountPublished);
}

bool ReinitEnglishOverlay(const fs::path& overlay, uint32_t base_content_hash,
                          const EnglishWriteHook& hook) {
  auto file = RawFile::Open(overlay, true);
  return file && Reinit(*file, base_content_hash, hook);
}

bool WriteEnglishBase(const EnglishDictionaryPaths& paths, const std::string& bytes,
                      std::optional<fs::file_time_type> tsv_time) {
  if (!learning::WriteTextFileAtomically(paths.base, bytes)) return false;
  // A TSV saved again after tsv_time stays newer, so its edit is still read.
  std::error_code ec;
  const auto base_time = fs::last_write_time(paths.base, ec);
  if (tsv_time && !ec && base_time < *tsv_time) fs::last_write_time(paths.base, *tsv_time, ec);
  return true;
}

bool CompactEnglishDictionary(const EnglishDictionaryPaths& paths, const EnglishWriteHook& hook) {
  // A base older than the TSV is about to be recompiled from it; compacting it
  // would give the stale content a newer mtime and hide the TSV edit.
  const auto tsv_time = TsvTime(paths);
  std::error_code ec;
  if (tsv_time && (fs::last_write_time(paths.base, ec) < *tsv_time || ec)) return false;
  const auto base = EnglishBaseImage::Map(paths.base);
  if (!base) return false;
  auto records = base->Records();
  const auto overlay = ReadEnglishOverlay(paths.overlay);
  // A stale overlay belongs to another base and must not be merged (4.7).
  if (overlay && overlay->header.base_fingerprint == base->header().content_hash) {
    std::map<std::pair<std::string, std::string>, size_t> index;
    for (size_t i = 0; i < records.size(); ++i) index[{records[i].key, records[i].surface}] = i;
    std::vector<bool> erased(records.size(), false);
    for (const auto& op : overlay->ops) {
      const auto found = index.find({op.key, op.surface});
      if (op.kind == EnglishOverlayOpKind::Delete) {
        if (found != index.end()) erased[found->second] = true;
        continue;
      }
      if (op.surface.empty()) continue;
      EnglishWordRecord record{op.key, op.surface, op.frequency, op.flags};
      if (found != index.end()) {
        records[found->second] = std::move(record);
        erased[found->second] = false;
      } else {
        index[{op.key, op.surface}] = records.size();
        records.push_back(std::move(record));
        erased.push_back(false);
      }
    }
    std::vector<EnglishWordRecord> kept;
    kept.reserve(records.size());
    for (size_t i = 0; i < records.size(); ++i) {
      if (!erased[i]) kept.push_back(std::move(records[i]));
    }
    records = std::move(kept);
  }
  // The new generation and content_hash travel inside the new base, so they
  // are durable before the rename makes it live.
  const auto bytes = EncodeEnglishBase(std::move(records), base->header().generation + 1);
  if (!WriteEnglishBase(paths, bytes, tsv_time)) return false;
  if (!Step(hook, EnglishWriteStep::CompactBaseReplaced)) return false;
  // Only now, with the new base durable, the overlay is emptied for it.
  return ReinitEnglishOverlay(paths.overlay, GetU32(Bytes(bytes) + 28), hook);
}

}  // namespace azookey::host
