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

// destination must be an absolute, lexically normal path ending in ".zip"
// whose parent directory exists and which does not exist yet. Item names and
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
// InvalidPath (path rules, or source is not a regular file) / SourceMissing,
// TooLarge (file over 64 MiB, or ZIP limits), NotArchive (any other ZIP
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
