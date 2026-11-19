#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "azookey/ipc/Json.h"
#include "azookey/learning/DpapiCrypto.h"
#include "azookey/learning/ImportConflictPolicy.h"

namespace azookey::learning {

struct UserWord {
  std::string word;
  std::string ruby;
  std::optional<int32_t> cid;
  std::optional<int32_t> mid;
  std::optional<double> value;
  // A part-of-speech id (UserDictionaryPos.h); absent for a noun. A string
  // the table does not hold is kept as it was read and means a noun.
  std::optional<std::string> pos;
  // Members of the entry this version does not define, written back as read
  // (docs/user-dictionary-pos-spec.md section 3.2). A `pos` that is not a
  // string is kept here under its own name.
  ipc::json::Object extra;

  bool operator==(const UserWord& other) const;
};

// User-managed dictionary of words. JSON is encrypted at path + ".enc".
//
// File schema (docs/user-dictionary-pos-spec.md section 3):
//   { "version": 2, "entries": [
//       { "word": "ググる", "ruby": "ぐぐる", "pos": "verb_godan_ra" },
//       { "word": "azooKey", "ruby": "あずきい",
//         "cid": 1285, "mid": 501, "value": -5.0 }, ...
//   ] }
//
// Optional fields (cid, mid, value, pos) are omitted from JSON when absent.
// Save writes version 2 when a word has a string `pos`, and version 1, the
// form of the versions before parts of speech, otherwise. Unknown members of
// the root and of each entry are written back. A dictionary of version 3 or
// later is read but never written (too_new).
class UserDictionary {
 public:
  explicit UserDictionary(std::filesystem::path path, const ByteCrypto* crypto = nullptr);

  // Load entries from disk. Missing file -> empty dictionary, returns true.
  // Malformed legacy plaintext is quarantined when possible. Undecryptable
  // ciphertext is left in place and blocks Save. A root object of version 3
  // or later loads (its entries, if they can be read) and returns true, but
  // is neither quarantined nor migrated, and blocks Save (too_new).
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
  // Text of version 3 or later loads and sets too_new.
  bool LoadText(std::string_view text);
  // True after a corrupt or undecipherable Load, or when the dictionary is
  // too new: Save refuses to run.
  bool save_blocked() const { return save_blocked_by_corrupt_load_ || too_new_; }
  // True when the dictionary read has version 3 or later (section 3.2).
  bool too_new() const { return too_new_; }
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

  enum class ParseResult { NotDictionary, Loaded, TooNew };

  bool LoadImpl(bool quarantine_corrupt_file);
  // Replaces the entries, the root's unknown members and too_new_ with what
  // `text` holds, unless it is NotDictionary (then nothing changes).
  ParseResult ParseInto(std::string_view text);
  std::string Serialize() const;
  std::optional<FileStamp> ReadStorageStamp() const;

  std::filesystem::path path_;
  const ByteCrypto* crypto_;
  std::map<std::string, std::vector<UserWord>> by_ruby_;
  // Members of the root this version does not define (section 3.2).
  ipc::json::Object root_extra_;
  bool too_new_{false};
  bool save_blocked_by_corrupt_load_{false};
  uint64_t revision_{};
  // Set by a load of the encrypted file and by a save; Save is const.
  mutable std::optional<DiskState> disk_state_;
};

}  // namespace azookey::learning
