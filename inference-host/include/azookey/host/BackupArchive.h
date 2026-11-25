#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "azookey/learning/DpapiCrypto.h"

namespace azookey::host {

// Learning-data backup container (learning-data-management-spec section 5):
// a STORE-only ZIP holding a plaintext manifest.json plus one entry per item.
struct BackupItem {
  std::string name;  // e.g. "learning"
  std::string file;  // ZIP entry name, e.g. "learning.tsv.enc"
  uint64_t count{};
  std::string plaintext;
};

struct BackupManifestItem {
  std::string name;
  std::string file;
  uint64_t count{};
  // Lowercase hex SHA-256 of the stored entry bytes (after encryption).
  std::string sha256;
};

struct BackupManifest {
  int version{1};
  std::string created_at;
  std::string app_version;
  std::string host_version;
  bool encrypted{true};
  std::string encryption_method;  // "dpapi-user-scope" when encrypted, else "none".
  std::vector<BackupManifestItem> items;
};

enum class BackupError {
  None,
  InvalidPath,
  DestinationExists,
  SourceMissing,
  Io,
  TooLarge,
  NotArchive,
  BadManifest,
  UnsupportedVersion,
  MissingItem,
  ChecksumMismatch,
  DecryptFailed,
  // Raised by the caller of ReadBackupArchive: a decrypted item does not
  // parse as its store's format.
  BadItem,
  // Raised by the caller: a selected store failed to load and refuses to
  // save, so exporting or importing it would report a false success.
  StoreUnavailable,
};

// snake_case identifier for IPC responses and logs ("invalid_path", ...).
std::string_view BackupErrorCode(BackupError error);

// The string rules of a path the settings app names for an import or export
// (learning-data-management-spec section 5), without the extension: absolute,
// no "." or ".." component, no UNC or device namespace, and no component with
// a ':' or a reserved device name. Where the path leads is checked when the
// file is opened (ReadUserDataSourceFile).
bool IsAcceptableUserDataSourcePath(const std::filesystem::path& path);

// Reads a file the settings app names for an import
// (learning-data-management-spec section 5; user-dictionary-pos-spec section
// 6.8 uses it): IsAcceptableUserDataSourcePath, then the file is read from
// the handle its checks were made on. A link as the last component (a
// symbolic link, a junction, a WSL symbolic link) is refused, not followed.
// On Windows the file it leads to must be on a local drive: a mapped network
// drive, or a directory link or junction leading to a network share, is
// refused. Errors: InvalidPath (path rules, a link, not a regular file, not
// on a local drive), SourceMissing, TooLarge
// (over max_bytes, refused before reading), Io (cannot be opened or read, or
// changed while read). On failure `out` is erased; the caller erases it with
// SecureErase after use.
BackupError ReadUserDataSourceFile(const std::filesystem::path& path, size_t max_bytes,
                                   std::string& out);

#ifdef _WIN32
// Whether a GetFinalPathNameByHandleW(VOLUME_NAME_DOS) result names a local
// drive ("\\?\X:\..."), not a network share ("\\?\UNC\...") or a volume
// without a drive letter.
bool IsLocalDriveFinalPath(std::wstring_view final_path);
#endif

// destination must be an absolute, lexically normal path ending in ".zip"
// whose parent directory exists and which does not exist yet. On Windows the
// archive is written under the final path of the parent directory, which must
// be on a local drive (as for ReadUserDataSourceFile). Item names and
// files must match [a-z0-9_.]+, be unique, and file must not be
// "manifest.json". With encrypt, every plaintext is encrypted with crypto.
// Only created_at, app_version and host_version are taken from meta.
// Failures: InvalidPath, DestinationExists, TooLarge, Io (including an
// encryption failure). Outputs are written only on success; error always.
bool WriteBackupArchive(const std::filesystem::path& destination,
                        const std::vector<BackupItem>& items, bool encrypt,
                        const learning::ByteCrypto& crypto, const BackupManifest& meta,
                        BackupManifest* written, uint64_t* file_size, BackupError* error);

// Checks run in this order, reporting the first failure:
// InvalidPath (".zip" and the rules of ReadUserDataSourceFile) /
// SourceMissing / Io, TooLarge (file over 64 MiB, or ZIP limits), NotArchive (any other ZIP
// defect), BadManifest (manifest.json absent, unparsable, or without an
// integer version), UnsupportedVersion, BadManifest (any other field),
// MissingItem, ChecksumMismatch (all items are verified before any is
// decrypted), DecryptFailed.
// ZIP entries not listed in the manifest are ignored. Items are returned in
// manifest order with plaintext filled in.
std::optional<std::vector<BackupItem>> ReadBackupArchive(const std::filesystem::path& source,
                                                         const learning::ByteCrypto& crypto,
                                                         BackupManifest* manifest,
                                                         BackupError* error);

}  // namespace azookey::host
