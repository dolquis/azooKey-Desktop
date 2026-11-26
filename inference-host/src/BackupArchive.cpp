#include "azookey/host/BackupArchive.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <set>
#include <system_error>
#include <utility>

#include "azookey/host/StoreOnlyZip.h"
#include "azookey/ipc/Json.h"
#include "azookey/learning/AtomicFile.h"
#include "azookey/learning/Sha256.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#endif

namespace azookey::host {
namespace backup_archive_detail {

namespace fs = std::filesystem;
namespace json = ipc::json;

constexpr int kManifestVersion = 1;
constexpr std::string_view kManifestFile = "manifest.json";
constexpr std::string_view kMethodDpapi = "dpapi-user-scope";
constexpr std::string_view kMethodNone = "none";
constexpr uint64_t kMaxArchiveBytes = 64ull * 1024 * 1024;

ZipLimits ArchiveLimits() {
  ZipLimits limits;
  limits.max_entries = 16;
  limits.max_entry_bytes = 32ull * 1024 * 1024;
  limits.max_total_bytes = kMaxArchiveBytes;
  return limits;
}

bool Fail(BackupError* error, BackupError value) {
  if (error) *error = value;
  return false;
}

// Windows maps these names to devices in every directory, extension or not
// ("NUL.zip" is the null device).
bool IsReservedDeviceName(const fs::path& filename) {
  std::string base;
  for (const auto ch : filename.native()) {
    if (ch == '.') break;
    if (ch > 0x7F) return false;
    base.push_back(static_cast<char>(ch >= 'a' && ch <= 'z' ? ch - 'a' + 'A' : ch));
  }
  while (!base.empty() && base.back() == ' ') base.pop_back();
  for (const char* reserved : {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"}) {
    if (base == reserved) return true;
  }
  return base.size() == 4 && (base.starts_with("COM") || base.starts_with("LPT")) &&
         base[3] >= '0' && base[3] <= '9';
}

// Absolute, no "." or ".." component, no UNC or device namespace, and no
// component with a ':' or a reserved device name. Only the string is looked
// at; OpenSourceFile and CheckDestination check where it leads.
bool IsAcceptableSourcePath(const fs::path& path) {
  if (!path.is_absolute() || !path.has_filename()) return false;
  const auto& native = path.native();
  const auto is_separator = [](fs::path::value_type ch) { return ch == '\\' || ch == '/'; };
#ifdef _WIN32
  // Only "X:\..." (or "X:/...") is accepted. UNC (\\host\share), the device namespaces (\\?\,
  // \\.\, //?/) and the NT namespace (\??\UNC\host\share, \??\C:\) all have another root name;
  // Win32 hands the NT form to the kernel unconverted, so \??\UNC\... would reach a network
  // share. The settings app has no reason to name any of them.
  const auto is_ascii_letter = [](fs::path::value_type ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
  };
  if (native.size() < 3 || !is_ascii_letter(native[0]) || native[1] != ':' ||
      !is_separator(native[2])) {
    return false;
  }
#else
  // Two leading separators mean UNC or a device namespace.
  if (native.size() >= 2 && is_separator(native[0]) && is_separator(native[1])) return false;
#endif
  for (const auto& part : path.relative_path()) {
    if (part == "." || part == "..") return false;
    // A ':' would address an NTFS alternate data stream, also of a directory.
    if (part.native().find(fs::path::value_type{':'}) != fs::path::string_type::npos) {
      return false;
    }
    if (IsReservedDeviceName(part)) return false;
  }
  return true;
}

// IsAcceptableSourcePath, and a case-insensitive ".zip" extension.
bool IsAcceptableArchivePath(const fs::path& path) {
  if (!IsAcceptableSourcePath(path)) return false;
  // extension() returns a temporary; keep a copy of its string.
  const auto extension = path.extension().native();
  constexpr std::string_view kZip = ".zip";
  if (extension.size() != kZip.size()) return false;
  for (size_t i = 0; i < kZip.size(); ++i) {
    auto ch = extension[i];
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<fs::path::value_type>(ch - 'A' + 'a');
    if (ch != static_cast<fs::path::value_type>(kZip[i])) return false;
  }
  return true;
}

#ifdef _WIN32
// Closes a kernel handle on every way out.
class FileHandle {
 public:
  explicit FileHandle(HANDLE handle) : handle_(handle) {}
  ~FileHandle() {
    if (valid()) CloseHandle(handle_);
  }
  FileHandle(const FileHandle&) = delete;
  FileHandle& operator=(const FileHandle&) = delete;
  bool valid() const { return handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr; }
  HANDLE get() const { return handle_; }
  void reset(HANDLE handle) {
    if (valid()) CloseHandle(handle_);
    handle_ = handle;
  }
  HANDLE release() { return std::exchange(handle_, INVALID_HANDLE_VALUE); }

 private:
  HANDLE handle_;
};

// Opens an existing file or directory. If the path leads to a named pipe, its
// server may only identify the Host, not impersonate it. CreateFileW reads the
// SECURITY_SQOS_PRESENT bit as FILE_FLAG_OPEN_NO_RECALL too, which would keep a
// cloud file from being recalled to be read; CreateFile2 takes the two apart.
HANDLE OpenExisting(const fs::path& path, DWORD access, DWORD share, DWORD flags) {
  CREATEFILE2_EXTENDED_PARAMETERS params{};
  params.dwSize = sizeof(params);
  params.dwFileFlags = flags;
  params.dwSecurityQosFlags = SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION;
  return CreateFile2(path.c_str(), access, share, OPEN_EXISTING, &params);
}

// A failure to open a named file. A network location whose name or share is
// not found, or whose connection was lost, is no local drive
// (learning-data-management-spec section 5). The other errors
// std::filesystem reports as a missing file (MSVC's __std_is_file_not_found)
// are a missing file; any other failure to open is no missing file.
BackupError OpenFailure(DWORD error) {
  switch (error) {
    case ERROR_BAD_NETPATH:
    case ERROR_BAD_NET_NAME:
    case ERROR_NETNAME_DELETED:
      return BackupError::InvalidPath;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME:
    case ERROR_BAD_PATHNAME:
    case ERROR_DIRECTORY:
      return BackupError::SourceMissing;
    default:
      return BackupError::Io;
  }
}

// The final path of an open file or directory, every link and junction
// resolved, in the "\\?\X:\..." form; "\\?\UNC\..." for a network file.
std::optional<std::wstring> FinalPathOf(HANDLE handle) {
  std::wstring buffer(MAX_PATH, L'\0');
  for (int attempt = 0; attempt < 2; ++attempt) {
    const DWORD length =
        GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()),
                                  FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (length == 0) return std::nullopt;
    if (length < buffer.size()) {
      buffer.resize(length);
      return buffer;
    }
    buffer.assign(length, L'\0');  // Too small: length is the size it needs.
  }
  return std::nullopt;
}

// A file on a local disk whose final path has a drive letter.
bool IsLocalDiskHandle(HANDLE handle) {
  if (GetFileType(handle) != FILE_TYPE_DISK) return false;
  const auto final_path = FinalPathOf(handle);
  return final_path && IsLocalDriveFinalPath(*final_path);
}

// The file a path names, opened for reading (learning-data-management-spec
// section 5). A link as the last component (a reparse point that names
// another file: a symbolic link, a junction, a WSL symbolic link) is refused,
// not followed; the directories before it are followed, and the file they
// lead to must be on a local drive. Other reparse points (a cloud file, a
// deduplicated or compressed file) are files and are read.
BackupError OpenSourceFile(const fs::path& path, FileHandle& out) {
  // FILE_FLAG_OPEN_REPARSE_POINT opens a link itself, to look at it.
  FileHandle link(OpenExisting(path, FILE_READ_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS));
  if (!link.valid()) return OpenFailure(GetLastError());
  FILE_ATTRIBUTE_TAG_INFO tag{};
  if (!GetFileInformationByHandleEx(link.get(), FileAttributeTagInfo, &tag, sizeof(tag))) {
    return BackupError::Io;
  }
  if ((tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return BackupError::InvalidPath;
  if ((tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 &&
      IsReparseTagNameSurrogate(tag.ReparseTag)) {
    return BackupError::InvalidPath;
  }
  // Opened again to read through the reparse points that are files.
  FileHandle file(OpenExisting(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               FILE_FLAG_SEQUENTIAL_SCAN));
  if (!file.valid()) return OpenFailure(GetLastError());
  // The same file as the one looked at: a link put in its place in between
  // is not followed.
  BY_HANDLE_FILE_INFORMATION looked{};
  BY_HANDLE_FILE_INFORMATION opened{};
  if (!GetFileInformationByHandle(link.get(), &looked) ||
      !GetFileInformationByHandle(file.get(), &opened)) {
    return BackupError::Io;
  }
  if (looked.dwVolumeSerialNumber != opened.dwVolumeSerialNumber ||
      looked.nFileIndexHigh != opened.nFileIndexHigh ||
      looked.nFileIndexLow != opened.nFileIndexLow) {
    return BackupError::InvalidPath;
  }
  if (!IsLocalDiskHandle(file.get())) return BackupError::InvalidPath;
  out.reset(file.release());
  return BackupError::None;
}
#endif

// Reads a file of `size` bytes with read_some, which returns how many bytes it
// read, 0 at the end, or nullopt on a failure. One byte more than the size is
// asked for: a file that changed while it was read is refused.
template <typename ReadSome>
BackupError ReadKnownSize(uint64_t size, size_t max_bytes, std::string& out, ReadSome read_some) {
  if (size > max_bytes) return BackupError::TooLarge;
  const auto expected = static_cast<size_t>(size);
  out.assign(expected + 1, '\0');
  size_t filled = 0;
  while (filled < out.size()) {
    const std::optional<size_t> read = read_some(out.data() + filled, out.size() - filled);
    if (!read) {
      learning::SecureErase(out);
      return BackupError::Io;
    }
    if (*read == 0) break;
    filled += *read;
  }
  if (filled != expected) {
    learning::SecureErase(out);
    return filled > max_bytes ? BackupError::TooLarge : BackupError::Io;
  }
  out.resize(filled);
  return BackupError::None;
}

// IsAcceptableSourcePath, then the file is read from the handle its checks
// were made on, so what was checked is what is read. A file over max_bytes is
// refused before it is read.
BackupError ReadSourceFile(const fs::path& path, size_t max_bytes, std::string& out) {
  out.clear();
  if (!IsAcceptableSourcePath(path)) return BackupError::InvalidPath;
#ifdef _WIN32
  FileHandle file(INVALID_HANDLE_VALUE);
  const BackupError opened = OpenSourceFile(path, file);
  if (opened != BackupError::None) return opened;
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file.get(), &size) || size.QuadPart < 0) return BackupError::Io;
  return ReadKnownSize(static_cast<uint64_t>(size.QuadPart), max_bytes, out,
                       [&](char* data, size_t length) -> std::optional<size_t> {
                         DWORD read = 0;
                         const auto chunk = static_cast<DWORD>(std::min<size_t>(length, 1u << 30));
                         if (!ReadFile(file.get(), data, chunk, &read, nullptr)) {
                           return std::nullopt;
                         }
                         return read;
                       });
#else
  // O_NOFOLLOW refuses a link as the last component; O_NONBLOCK keeps a FIFO
  // from blocking the open, and fstat then refuses it.
  const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0) {
    if (errno == ELOOP) return BackupError::InvalidPath;
    if (errno == ENOENT || errno == ENOTDIR) return BackupError::SourceMissing;
    return BackupError::Io;
  }
  struct CloseOnExit {
    int fd;
    ~CloseOnExit() { close(fd); }
  } close_fd{fd};
  struct stat info {};
  if (fstat(fd, &info) != 0) return BackupError::Io;
  // Never read a directory, a FIFO or a device.
  if (!S_ISREG(info.st_mode)) return BackupError::InvalidPath;
  if (info.st_size < 0) return BackupError::Io;
  return ReadKnownSize(static_cast<uint64_t>(info.st_size), max_bytes, out,
                       [&](char* data, size_t length) -> std::optional<size_t> {
                         for (;;) {
                           const ssize_t read_bytes = read(fd, data, length);
                           if (read_bytes >= 0) return static_cast<size_t>(read_bytes);
                           if (errno != EINTR) return std::nullopt;
                         }
                       });
#endif
}

// [a-z0-9_.]+ without "..", so every token is also a safe ZIP entry name.
bool IsBackupToken(std::string_view token) {
  if (token.empty() || token.find("..") != std::string_view::npos) return false;
  for (char ch : token) {
    const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '.';
    if (!ok) return false;
  }
  return true;
}

bool IsLowerHexSha256(std::string_view text) {
  if (text.size() != 64) return false;
  for (char ch : text) {
    if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return false;
  }
  return true;
}

std::string SerializeManifest(const BackupManifest& manifest) {
  json::Array items;
  for (const auto& item : manifest.items) {
    json::Object entry;
    entry["name"] = item.name;
    entry["file"] = item.file;
    entry["count"] = item.count;
    entry["sha256"] = item.sha256;
    items.emplace_back(std::move(entry));
  }
  json::Object root;
  root["version"] = manifest.version;
  root["created_at"] = manifest.created_at;
  root["app_version"] = manifest.app_version;
  root["host_version"] = manifest.host_version;
  root["encrypted"] = manifest.encrypted;
  root["encryption_method"] = manifest.encryption_method;
  root["items"] = std::move(items);
  return json::Stringify(json::Value(std::move(root)));
}

bool ParseManifestItem(const json::Value& value, BackupManifestItem& item) {
  if (!value.IsObject()) return false;
  auto name = value.GetString("name");
  auto file = value.GetString("file");
  auto count = value.GetUInt("count");
  auto sha256 = value.GetString("sha256");
  if (!name || !file || !count || !sha256) return false;
  if (!IsBackupToken(*name) || !IsBackupToken(*file) || *file == kManifestFile) return false;
  if (!IsLowerHexSha256(*sha256)) return false;
  item.name = std::move(*name);
  item.file = std::move(*file);
  item.count = *count;
  item.sha256 = std::move(*sha256);
  return true;
}

// version is checked first so that a future format reports UnsupportedVersion
// even if its other fields no longer match this schema.
bool ParseManifest(std::string_view text, BackupManifest& manifest, BackupError* error) {
  const auto root = json::Parse(text);
  if (!root || !root->IsObject()) return Fail(error, BackupError::BadManifest);
  const auto version = root->GetInt("version");
  if (!version) return Fail(error, BackupError::BadManifest);
  if (*version != kManifestVersion) return Fail(error, BackupError::UnsupportedVersion);

  auto created_at = root->GetString("created_at");
  auto app_version = root->GetString("app_version");
  auto host_version = root->GetString("host_version");
  const auto encrypted = root->GetBool("encrypted");
  auto method = root->GetString("encryption_method");
  const auto* items = root->GetArray("items");
  if (!created_at || !app_version || !host_version || !encrypted || !method || !items) {
    return Fail(error, BackupError::BadManifest);
  }
  if (*method != (*encrypted ? kMethodDpapi : kMethodNone)) {
    return Fail(error, BackupError::BadManifest);
  }
  manifest.version = kManifestVersion;
  manifest.created_at = std::move(*created_at);
  manifest.app_version = std::move(*app_version);
  manifest.host_version = std::move(*host_version);
  manifest.encrypted = *encrypted;
  manifest.encryption_method = std::move(*method);
  manifest.items.clear();
  std::set<std::string> names;
  std::set<std::string> files;
  for (const auto& value : *items) {
    BackupManifestItem item;
    if (!ParseManifestItem(value, item) || !names.insert(item.name).second ||
        !files.insert(item.file).second) {
      return Fail(error, BackupError::BadManifest);
    }
    manifest.items.push_back(std::move(item));
  }
  return true;
}

bool ValidateItems(const std::vector<BackupItem>& items, BackupError* error) {
  std::set<std::string_view> names;
  std::set<std::string_view> files;
  for (const auto& item : items) {
    if (!IsBackupToken(item.name) || !IsBackupToken(item.file) || item.file == kManifestFile ||
        !names.insert(item.name).second || !files.insert(item.file).second) {
      return Fail(error, BackupError::InvalidPath);
    }
  }
  // One ZIP slot is taken by manifest.json.
  if (items.size() + 1 > ArchiveLimits().max_entries) return Fail(error, BackupError::TooLarge);
  return true;
}

bool EncodeItem(const BackupItem& item, bool encrypt, const learning::ByteCrypto& crypto,
                std::string& stored) {
  if (!encrypt) {
    stored = item.plaintext;
    return true;
  }
  if (!crypto.IsAvailable()) return false;
  std::vector<uint8_t> plain(item.plaintext.begin(), item.plaintext.end());
  std::vector<uint8_t> cipher;
  const bool ok = crypto.Encrypt(plain, cipher);
  learning::SecureErase(plain);
  if (!ok) return false;
  stored.assign(cipher.begin(), cipher.end());
  return true;
}

bool DecodeItem(std::string_view stored, bool encrypted, const learning::ByteCrypto& crypto,
                std::string& plaintext) {
  if (!encrypted) {
    plaintext.assign(stored);
    return true;
  }
  if (!crypto.IsAvailable()) return false;
  const std::vector<uint8_t> cipher(stored.begin(), stored.end());
  std::vector<uint8_t> plain;
  if (!crypto.Decrypt(cipher, plain)) {
    learning::SecureErase(plain);
    return false;
  }
  plaintext.assign(plain.begin(), plain.end());
  learning::SecureErase(plain);
  return true;
}

#ifdef _WIN32
// A local final path in the "X:\..." form a path is given in, for
// WriteTextFileAtomically and MoveFileExW. nullopt when dropping "\\?\" would
// change what it names: Win32 drops a trailing '.' or ' ' of a component and
// maps a reserved device name, which a "\\?\" path keeps.
std::optional<fs::path> DrivePathOf(std::wstring_view final_path) {
  if (!IsLocalDriveFinalPath(final_path)) return std::nullopt;
  fs::path path(final_path.substr(4));
  for (const auto& part : path.relative_path()) {
    const auto& name = part.native();
    if (name.empty() || name.back() == L'.' || name.back() == L' ' ||
        name.find(L':') != std::wstring::npos || IsReservedDeviceName(part)) {
      return std::nullopt;
    }
  }
  return path;
}
#endif

// Where WriteArchive writes. On Windows the parent directory stays open until
// the archive is committed.
struct Destination {
  fs::path path;
#ifdef _WIN32
  FileHandle parent{INVALID_HANDLE_VALUE};
#endif
};

// Rejects anything that WriteTextFileAtomically would silently accept: it
// creates missing parent directories and replaces an existing target.
// On Windows the archive is written under the final path of the parent
// directory, which must be on a local drive: a junction or link among the
// directories cannot lead to a network share. The parent is held open without
// sharing delete, so it cannot be renamed or deleted until the commit. The
// archive is still written and committed by path, not through this handle,
// and the parent shares write: an empty parent can be made a junction after
// this check, and the archive then goes where the junction leads.
bool CheckDestination(const fs::path& destination, Destination& resolved, BackupError* error) {
  if (!IsAcceptableArchivePath(destination)) return Fail(error, BackupError::InvalidPath);
  std::error_code ec;
#ifdef _WIN32
  resolved.parent.reset(OpenExisting(destination.parent_path(), FILE_LIST_DIRECTORY,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     FILE_FLAG_BACKUP_SEMANTICS));
  if (!resolved.parent.valid()) {
    const BackupError failure = OpenFailure(GetLastError());
    return Fail(error, failure == BackupError::SourceMissing ? BackupError::InvalidPath : failure);
  }
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(resolved.parent.get(), &info)) {
    return Fail(error, BackupError::Io);
  }
  if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
      GetFileType(resolved.parent.get()) != FILE_TYPE_DISK) {
    return Fail(error, BackupError::InvalidPath);
  }
  const auto final_parent = FinalPathOf(resolved.parent.get());
  const auto parent = final_parent ? DrivePathOf(*final_parent) : std::nullopt;
  if (!parent) return Fail(error, BackupError::InvalidPath);
  resolved.path = *parent / destination.filename();
#else
  if (!fs::is_directory(destination.parent_path(), ec)) {
    return Fail(error, BackupError::InvalidPath);
  }
  resolved.path = destination;
#endif
  const auto status = fs::symlink_status(resolved.path, ec);
  if (status.type() == fs::file_type::not_found) return true;
  if (ec) return Fail(error, BackupError::Io);
  return Fail(error, BackupError::DestinationExists);
}

// Writes a sibling temporary file, then moves it into place with an operation
// that fails when the destination already exists, so a file created after
// CheckDestination is never replaced.
bool CommitWithoutReplacing(const fs::path& destination, const std::string& archive,
                            BackupError* error) {
  auto temporary = destination;
  temporary +=
      ".tmp-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  if (!learning::WriteTextFileAtomically(temporary, archive)) return Fail(error, BackupError::Io);
  std::error_code ec;
#ifdef _WIN32
  // Without MOVEFILE_REPLACE_EXISTING the move fails if the target exists.
  const bool moved = MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH);
  const DWORD move_error = moved ? ERROR_SUCCESS : GetLastError();
  const bool exists = move_error == ERROR_ALREADY_EXISTS || move_error == ERROR_FILE_EXISTS;
#else
  fs::create_hard_link(temporary, destination, ec);
  const bool moved = !ec;
  const bool exists = ec == std::errc::file_exists;
#endif
  fs::remove(temporary, ec);
  if (moved) return true;
  return Fail(error, exists ? BackupError::DestinationExists : BackupError::Io);
}

bool WriteArchive(const fs::path& destination, const std::vector<BackupItem>& items, bool encrypt,
                  const learning::ByteCrypto& crypto, const BackupManifest& meta,
                  BackupManifest* written, uint64_t* file_size, BackupError* error) {
  Destination resolved;
  if (!CheckDestination(destination, resolved, error) || !ValidateItems(items, error)) {
    return false;
  }
  const ZipLimits limits = ArchiveLimits();

  BackupManifest manifest;
  manifest.version = kManifestVersion;
  manifest.created_at = meta.created_at;
  manifest.app_version = meta.app_version;
  manifest.host_version = meta.host_version;
  manifest.encrypted = encrypt;
  manifest.encryption_method = std::string(encrypt ? kMethodDpapi : kMethodNone);

  std::vector<ZipEntry> entries(1);
  for (const auto& item : items) {
    ZipEntry entry{item.file, {}};
    if (!EncodeItem(item, encrypt, crypto, entry.data)) return Fail(error, BackupError::Io);
    if (entry.data.size() > limits.max_entry_bytes) return Fail(error, BackupError::TooLarge);
    manifest.items.push_back({item.name, item.file, item.count, learning::Sha256Hex(entry.data)});
    entries.push_back(std::move(entry));
  }
  entries[0] = ZipEntry{std::string(kManifestFile), SerializeManifest(manifest)};
  if (entries[0].data.size() > limits.max_entry_bytes) return Fail(error, BackupError::TooLarge);

  // The whole archive must stay readable by ReadBackupArchive, whose file-size
  // cap is stricter than the sum of entry sizes.
  uint64_t total = 0;
  for (const auto& entry : entries) total += entry.data.size();
  if (total > limits.max_total_bytes) return Fail(error, BackupError::TooLarge);
  const std::string archive = BuildStoreOnlyZip(entries);
  if (archive.size() > kMaxArchiveBytes) return Fail(error, BackupError::TooLarge);

  if (!CommitWithoutReplacing(resolved.path, archive, error)) return false;
  if (written) *written = std::move(manifest);
  if (file_size) *file_size = archive.size();
  if (error) *error = BackupError::None;
  return true;
}

bool ReadSourceBytes(const fs::path& source, std::string& bytes, BackupError* error) {
  if (!IsAcceptableArchivePath(source)) return Fail(error, BackupError::InvalidPath);
  const BackupError read = ReadSourceFile(source, kMaxArchiveBytes, bytes);
  if (read != BackupError::None) return Fail(error, read);
  return true;
}

std::optional<std::vector<BackupItem>> ReadArchive(const fs::path& source,
                                                   const learning::ByteCrypto& crypto,
                                                   BackupManifest* manifest_out,
                                                   BackupError* error) {
  std::string bytes;
  if (!ReadSourceBytes(source, bytes, error)) return std::nullopt;

  ZipError zip_error = ZipError::None;
  const auto entries = ParseStoreOnlyZip(bytes, ArchiveLimits(), &zip_error);
  if (!entries) {
    const bool too_large = zip_error == ZipError::TooManyEntries ||
                           zip_error == ZipError::EntryTooLarge ||
                           zip_error == ZipError::TotalTooLarge;
    Fail(error, too_large ? BackupError::TooLarge : BackupError::NotArchive);
    return std::nullopt;
  }
  auto find_entry = [&](std::string_view name) -> const ZipEntry* {
    for (const auto& entry : *entries) {
      if (entry.name == name) return &entry;
    }
    return nullptr;
  };

  const ZipEntry* manifest_entry = find_entry(kManifestFile);
  BackupManifest manifest;
  if (!manifest_entry) {
    Fail(error, BackupError::BadManifest);
    return std::nullopt;
  }
  if (!ParseManifest(manifest_entry->data, manifest, error)) return std::nullopt;

  // Verify every listed item before decrypting any of them.
  std::vector<const ZipEntry*> stored;
  for (const auto& item : manifest.items) {
    const ZipEntry* entry = find_entry(item.file);
    if (!entry) {
      Fail(error, BackupError::MissingItem);
      return std::nullopt;
    }
    stored.push_back(entry);
  }
  for (size_t i = 0; i < stored.size(); ++i) {
    if (learning::Sha256Hex(stored[i]->data) != manifest.items[i].sha256) {
      Fail(error, BackupError::ChecksumMismatch);
      return std::nullopt;
    }
  }

  std::vector<BackupItem> items;
  for (size_t i = 0; i < stored.size(); ++i) {
    const auto& listed = manifest.items[i];
    BackupItem item{listed.name, listed.file, listed.count, {}};
    if (!DecodeItem(stored[i]->data, manifest.encrypted, crypto, item.plaintext)) {
      for (auto& decoded : items) learning::SecureErase(decoded.plaintext);
      Fail(error, BackupError::DecryptFailed);
      return std::nullopt;
    }
    items.push_back(std::move(item));
  }
  if (manifest_out) *manifest_out = std::move(manifest);
  if (error) *error = BackupError::None;
  return items;
}

}  // namespace backup_archive_detail

bool IsAcceptableUserDataSourcePath(const std::filesystem::path& path) {
  return backup_archive_detail::IsAcceptableSourcePath(path);
}

BackupError ReadUserDataSourceFile(const std::filesystem::path& path, size_t max_bytes,
                                   std::string& out) {
  try {
    return backup_archive_detail::ReadSourceFile(path, max_bytes, out);
  } catch (const std::exception&) {
    learning::SecureErase(out);
    return BackupError::Io;
  }
}

#ifdef _WIN32
bool IsLocalDriveFinalPath(std::wstring_view final_path) {
  constexpr std::wstring_view kPrefix = L"\\\\?\\";
  if (final_path.size() < kPrefix.size() + 3 || !final_path.starts_with(kPrefix)) return false;
  const wchar_t letter = final_path[kPrefix.size()];
  const bool is_letter = (letter >= L'A' && letter <= L'Z') || (letter >= L'a' && letter <= L'z');
  return is_letter && final_path[kPrefix.size() + 1] == L':' &&
         final_path[kPrefix.size() + 2] == L'\\';
}

namespace detail {
BackupError SourceOpenFailure(unsigned long win32_error) {
  return backup_archive_detail::OpenFailure(win32_error);
}

std::optional<std::filesystem::path> DrivePathOfFinalPath(std::wstring_view final_path) {
  return backup_archive_detail::DrivePathOf(final_path);
}
}  // namespace detail
#endif

std::string_view BackupErrorCode(BackupError error) {
  switch (error) {
    case BackupError::None:
      return "none";
    case BackupError::InvalidPath:
      return "invalid_path";
    case BackupError::DestinationExists:
      return "destination_exists";
    case BackupError::SourceMissing:
      return "source_missing";
    case BackupError::Io:
      return "io";
    case BackupError::TooLarge:
      return "too_large";
    case BackupError::NotArchive:
      return "not_archive";
    case BackupError::BadManifest:
      return "bad_manifest";
    case BackupError::UnsupportedVersion:
      return "unsupported_version";
    case BackupError::MissingItem:
      return "missing_item";
    case BackupError::ChecksumMismatch:
      return "checksum_mismatch";
    case BackupError::DecryptFailed:
      return "decrypt_failed";
    case BackupError::BadItem:
      return "bad_item";
    case BackupError::StoreUnavailable:
      return "store_unavailable";
  }
  return "unknown";
}

bool WriteBackupArchive(const std::filesystem::path& destination,
                        const std::vector<BackupItem>& items, bool encrypt,
                        const learning::ByteCrypto& crypto, const BackupManifest& meta,
                        BackupManifest* written, uint64_t* file_size, BackupError* error) {
  try {
    return backup_archive_detail::WriteArchive(destination, items, encrypt, crypto, meta, written,
                                               file_size, error);
  } catch (const std::exception&) {
    return backup_archive_detail::Fail(error, BackupError::Io);
  }
}

std::optional<std::vector<BackupItem>> ReadBackupArchive(const std::filesystem::path& source,
                                                         const learning::ByteCrypto& crypto,
                                                         BackupManifest* manifest,
                                                         BackupError* error) {
  try {
    return backup_archive_detail::ReadArchive(source, crypto, manifest, error);
  } catch (const std::exception&) {
    backup_archive_detail::Fail(error, BackupError::Io);
    return std::nullopt;
  }
}

}  // namespace azookey::host
