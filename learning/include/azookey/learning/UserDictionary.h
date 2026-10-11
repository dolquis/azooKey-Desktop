#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "azookey/learning/DpapiCrypto.h"
#include "azookey/learning/ImportConflictPolicy.h"

namespace azookey::learning {

struct UserWord {
  std::string word;
  std::string ruby;
  std::optional<int32_t> cid;
  std::optional<int32_t> mid;
  std::optional<double> value;

  bool operator==(const UserWord& other) const noexcept {
    return word == other.word && ruby == other.ruby && cid == other.cid &&
           mid == other.mid && value == other.value;
  }
};

// User-managed dictionary of words. JSON is encrypted at path + ".enc".
//
// File schema (version 1):
//   { "version": 1, "entries": [
//       { "word": "azooKey", "ruby": "あずきい",
//         "cid": 1285, "mid": 501, "value": -5.0 }, ...
//   ] }
//
// Optional fields (cid, mid, value) are omitted from JSON when absent.
class UserDictionary {
 public:
  explicit UserDictionary(std::filesystem::path path, const ByteCrypto* crypto = nullptr);

  // Load entries from disk. Missing file -> empty dictionary, returns true.
  // Malformed legacy plaintext is quarantined when possible. Undecryptable
  // ciphertext is left in place and blocks Save.
  bool Load();

  // Load without quarantining or otherwise changing a malformed source file.
  bool LoadReadOnly();

  // Load, unless nothing in memory has changed since this instance (or the
  // one it was copied from) loaded or saved the encrypted file, and that file
  // is still the same one (size, last write time and file identity). Call it
  // under the dictionary's file lock, as Load. *skipped tells whether the
  // file was left unread (DEV-1572).
  bool ReloadIfChanged(bool* skipped = nullptr);

  // Persist current state as user-scoped ciphertext. Returns false after a
  // failed load or while unmigrated plaintext is present.
  bool Save() const;
  // Explicit JSON export. Writes plaintext only to the caller's chosen path.
  bool SavePlaintextExport(const std::filesystem::path& export_path) const;

  // Insert a new entry, or replace the existing entry that has the same
  // (word, ruby) pair. Returns true when a new entry was added (false on
  // in-place replacement).
  bool Add(const UserWord& w);

  // Remove the entry matching (word, ruby). Returns true if removed.
  bool Remove(const std::string& word, const std::string& ruby);

  // Replace all entries. Duplicate (word, ruby) pairs follow Add semantics:
  // later entries replace earlier entries.
  void ReplaceAll(const std::vector<UserWord>& entries);

  // Look up all entries by reading. Returns empty vector if unknown.
  std::vector<UserWord> Lookup(const std::string& ruby) const;

  // All entries flattened.
  std::vector<UserWord> All() const;

  size_t Size() const;
  uint64_t revision() const noexcept { return revision_; }
  void Clear();

  // The JSON text, as Save writes it before encryption.
  std::string SerializeText() const;
  // Replaces the contents with words parsed from `text`; touches no file.
  // Returns false (contents unchanged) when the JSON is not a dictionary.
  bool LoadText(std::string_view text);
  // True after a corrupt or undecipherable Load: Save refuses to run.
  bool save_blocked() const { return save_blocked_by_corrupt_load_; }
  // Adds every word of `other`. Only Overwrite replaces a word already
  // present under the same reading.
  ImportCounts Merge(const UserDictionary& other, ImportConflictPolicy policy);

  const std::filesystem::path& path() const { return path_; }
  std::filesystem::path storage_path() const { return EncryptedPathFor(path_); }

 private:
  // What identifies one version of the encrypted file on disk.
  struct FileStamp {
    uint64_t device{};
    uint64_t file_id{};
    // The upper half of the Windows 128-bit FileId; 0 elsewhere.
    uint64_t file_id_high{};
    uint64_t size{};
    int64_t write_time{};
    int64_t change_time{};
    bool operator==(const FileStamp&) const = default;
  };
  // The encrypted file the contents match, with the revision they had then.
  struct DiskState {
    FileStamp stamp;
    uint64_t revision{};
  };

  bool LoadImpl(bool quarantine_corrupt_file);
  std::string Serialize() const;
  std::optional<FileStamp> ReadStorageStamp() const;

  std::filesystem::path path_;
  const ByteCrypto* crypto_;
  std::map<std::string, std::vector<UserWord>> by_ruby_;
  bool save_blocked_by_corrupt_load_{false};
  uint64_t revision_{};
  // Set by a load of the encrypted file and by a save; Save is const.
  mutable std::optional<DiskState> disk_state_;
};

}  // namespace azookey::learning
