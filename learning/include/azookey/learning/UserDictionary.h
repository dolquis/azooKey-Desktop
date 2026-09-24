#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "azookey/learning/DpapiCrypto.h"

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

  const std::filesystem::path& path() const { return path_; }
  std::filesystem::path storage_path() const { return EncryptedPathFor(path_); }

 private:
  bool LoadImpl(bool quarantine_corrupt_file);
  std::string Serialize() const;

  std::filesystem::path path_;
  const ByteCrypto* crypto_;
  std::map<std::string, std::vector<UserWord>> by_ruby_;
  bool save_blocked_by_corrupt_load_{false};
  uint64_t revision_{};
};

}  // namespace azookey::learning
