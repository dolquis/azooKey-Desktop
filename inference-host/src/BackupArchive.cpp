#include "azookey/host/BackupArchive.h"

#include <chrono>
#include <exception>
#include <fstream>
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

// Absolute, no "." or ".." component, and a case-insensitive ".zip" extension.
bool IsAcceptableArchivePath(const fs::path& path) {
  if (!path.is_absolute() || !path.has_filename()) return false;
  // Two leading separators mean UNC (\\host\share) or a device namespace
  // (\\?\, \\.\); the settings app has no reason to name either.
  const auto& native = path.native();
  const auto is_separator = [](fs::path::value_type ch) { return ch == '\\' || ch == '/'; };
  if (native.size() >= 2 && is_separator(native[0]) && is_separator(native[1])) return false;
  // A ':' in the file name would address an NTFS alternate data stream.
  if (path.filename().native().find(fs::path::value_type{':'}) != fs::path::string_type::npos) {
    return false;
  }
  if (IsReservedDeviceName(path.filename())) return false;
  for (const auto& part : path.relative_path()) {
    if (part == "." || part == "..") return false;
  }
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

// Rejects anything that WriteTextFileAtomically would silently accept: it
// creates missing parent directories and replaces an existing target.
bool CheckDestination(const fs::path& destination, BackupError* error) {
  if (!IsAcceptableArchivePath(destination)) return Fail(error, BackupError::InvalidPath);
  std::error_code ec;
  if (!fs::is_directory(destination.parent_path(), ec)) {
    return Fail(error, BackupError::InvalidPath);
  }
  const auto status = fs::symlink_status(destination, ec);
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
  if (!CheckDestination(destination, error) || !ValidateItems(items, error)) return false;
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

  if (!CommitWithoutReplacing(destination, archive, error)) return false;
  if (written) *written = std::move(manifest);
  if (file_size) *file_size = archive.size();
  if (error) *error = BackupError::None;
  return true;
}

bool ReadSourceBytes(const fs::path& source, std::string& bytes, BackupError* error) {
  if (!IsAcceptableArchivePath(source)) return Fail(error, BackupError::InvalidPath);
  std::error_code ec;
  // symlink_status: a link is rejected rather than followed somewhere else.
  const auto status = fs::symlink_status(source, ec);
  if (status.type() == fs::file_type::not_found) return Fail(error, BackupError::SourceMissing);
  if (ec) return Fail(error, BackupError::Io);
  // Never open a directory: on Linux the open succeeds and the read throws.
  if (!fs::is_regular_file(status)) return Fail(error, BackupError::InvalidPath);
  const uintmax_t size = fs::file_size(source, ec);
  if (ec) return Fail(error, BackupError::Io);
  if (size > kMaxArchiveBytes) return Fail(error, BackupError::TooLarge);

  std::ifstream in(source, std::ios::binary);
  if (!in.is_open()) return Fail(error, BackupError::Io);
  bytes.assign(static_cast<size_t>(size), '\0');
  in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (static_cast<uintmax_t>(in.gcount()) != size) return Fail(error, BackupError::Io);
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
