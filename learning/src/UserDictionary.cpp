#include "azookey/learning/UserDictionary.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#else
#include <sys/stat.h>
#endif

#include "azookey/ipc/Json.h"
#include "azookey/learning/AtomicFile.h"

namespace azookey::learning {

namespace j = ::azookey::ipc::json;

namespace {

j::Value WordToJson(const UserWord& w) {
  j::Object o;
  o.emplace("word", j::Value(w.word));
  o.emplace("ruby", j::Value(w.ruby));
  if (w.cid) o.emplace("cid", j::Value(static_cast<double>(*w.cid)));
  if (w.mid) o.emplace("mid", j::Value(static_cast<double>(*w.mid)));
  if (w.value) o.emplace("value", j::Value(*w.value));
  return j::Value(std::move(o));
}

std::optional<UserWord> WordFromJson(const j::Value& v) {
  if (!v.IsObject()) return std::nullopt;
  UserWord w;
  auto word = v.GetString("word");
  auto ruby = v.GetString("ruby");
  if (!word || !ruby) return std::nullopt;
  w.word = std::move(*word);
  w.ruby = std::move(*ruby);
  if (auto cid = v.GetInt("cid")) w.cid = static_cast<int32_t>(*cid);
  if (auto mid = v.GetInt("mid")) w.mid = static_cast<int32_t>(*mid);
  if (auto val = v.GetNumber("value")) w.value = *val;
  return w;
}

bool QuarantineCorruptFile(const std::filesystem::path& source) {
  if (!std::filesystem::exists(source)) return true;

  const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
  auto backup = source;
  backup += ".corrupt." + std::to_string(stamp);
  std::error_code ec;
  std::filesystem::rename(source, backup, ec);
  if (!ec) return true;

  ec.clear();
  std::filesystem::copy_file(source, backup, std::filesystem::copy_options::none, ec);
  if (ec) return false;
  ec.clear();
  std::filesystem::remove(source, ec);
  return !ec;
}

}  // namespace

UserDictionary::UserDictionary(std::filesystem::path path, const ByteCrypto* crypto)
    : path_(std::move(path)), crypto_(crypto ? crypto : &DpapiCrypto()) {}

bool UserDictionary::Load() { return LoadImpl(true); }

bool UserDictionary::LoadReadOnly() { return LoadImpl(false); }

// DEV-1572: a writer replaces the encrypted file through a new temporary file,
// so a file written since this instance last read or wrote it differs in its
// identity (NTFS file index, or inode), and also in size or write time unless
// the write landed within the file system's timestamp granularity. Every
// mutator bumps revision_, so an equal revision means the contents are still
// the ones that file holds. When the encrypted file exists, Load reads it and
// nothing else, so skipping the load reads the same state Load would have.
bool UserDictionary::ReloadIfChanged(bool* skipped) {
  if (skipped) *skipped = false;
  if (disk_state_ && disk_state_->revision == revision_) {
    if (const auto stamp = ReadStorageStamp(); stamp && *stamp == disk_state_->stamp) {
      if (skipped) *skipped = true;
      return true;
    }
  }
  return Load();
}

std::optional<UserDictionary::FileStamp> UserDictionary::ReadStorageStamp() const {
  FileStamp stamp;
#ifdef _WIN32
  const HANDLE file = ::CreateFileW(storage_path().c_str(), FILE_READ_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (file == INVALID_HANDLE_VALUE) return std::nullopt;
  BY_HANDLE_FILE_INFORMATION info{};
  FILE_BASIC_INFO basic{};
  const bool read = ::GetFileInformationByHandle(file, &info) &&
                    ::GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof(basic));
  ::CloseHandle(file);
  if (!read) return std::nullopt;
  stamp.device = info.dwVolumeSerialNumber;
  stamp.file_id = (static_cast<uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
  stamp.size = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
  stamp.write_time = basic.LastWriteTime.QuadPart;
  stamp.change_time = basic.ChangeTime.QuadPart;
#else
  struct stat info {};
  if (::stat(storage_path().c_str(), &info) != 0) return std::nullopt;
  stamp.device = static_cast<uint64_t>(info.st_dev);
  stamp.file_id = static_cast<uint64_t>(info.st_ino);
  stamp.size = static_cast<uint64_t>(info.st_size);
  stamp.write_time = static_cast<int64_t>(info.st_mtim.tv_sec) * 1'000'000'000 +
                     static_cast<int64_t>(info.st_mtim.tv_nsec);
  stamp.change_time = static_cast<int64_t>(info.st_ctim.tv_sec) * 1'000'000'000 +
                      static_cast<int64_t>(info.st_ctim.tv_nsec);
#endif
  return stamp;
}

bool UserDictionary::LoadImpl(bool quarantine_corrupt_file) {
  ++revision_;
  by_ruby_.clear();
  save_blocked_by_corrupt_load_ = false;
  disk_state_.reset();
  // Taken before the read: a file replaced in between then fails to match.
  const auto stamp = ReadStorageStamp();
  std::string text;
  const auto source = ReadProtectedText(path_, *crypto_, text);
  const auto remember_encrypted_file = [&] {
    if (source == ProtectedFileSource::Encrypted && stamp)
      disk_state_ = DiskState{*stamp, revision_};
  };
  if (source == ProtectedFileSource::Missing) {
    return true;  // missing file is fine
  }
  if (source == ProtectedFileSource::Error) {
    save_blocked_by_corrupt_load_ = true;
    return false;
  }
  // A zero-byte file holds no entries; read it as an empty dictionary and
  // migrate it like a zero-byte TSV store. Migration leaves an empty .enc.
  if (text.empty()) {
    if (source == ProtectedFileSource::Plaintext && quarantine_corrupt_file &&
        !MigratePlaintextFile(path_, text, *crypto_)) {
      save_blocked_by_corrupt_load_ = true;
      return false;
    }
    remember_encrypted_file();
    return true;
  }
  auto v = j::Parse(text);
  if (!v || !v->IsObject()) {
    save_blocked_by_corrupt_load_ = source == ProtectedFileSource::Encrypted ||
                                    !quarantine_corrupt_file || !QuarantineCorruptFile(path_);
    SecureErase(text);
    return false;
  }
  const auto* entries = v->GetArray("entries");
  if (!entries) {
    save_blocked_by_corrupt_load_ = source == ProtectedFileSource::Encrypted ||
                                    !quarantine_corrupt_file || !QuarantineCorruptFile(path_);
    SecureErase(text);
    return false;
  }
  for (const auto& e : *entries) {
    if (auto w = WordFromJson(e)) {
      by_ruby_[w->ruby].push_back(std::move(*w));
    }
  }
  if (source == ProtectedFileSource::Plaintext && quarantine_corrupt_file &&
      !MigratePlaintextFile(path_, text, *crypto_)) {
    save_blocked_by_corrupt_load_ = true;
    SecureErase(text);
    return false;
  }
  SecureErase(text);
  remember_encrypted_file();
  return true;
}

static bool SameExportTarget(const std::filesystem::path& left,
                             const std::filesystem::path& right) {
  std::error_code ec;
  const auto absolute_left = std::filesystem::absolute(left, ec).lexically_normal();
  if (ec) return true;
  ec.clear();
  const auto absolute_right = std::filesystem::absolute(right, ec).lexically_normal();
  if (ec) return true;
  if (absolute_left == absolute_right) return true;
#ifdef _WIN32
  const auto wide_left = absolute_left.wstring();
  const auto wide_right = absolute_right.wstring();
  if (wide_left.size() > (std::numeric_limits<int>::max)() ||
      wide_right.size() > (std::numeric_limits<int>::max)())
    return true;
  const int comparison =
      ::CompareStringOrdinal(wide_left.c_str(), static_cast<int>(wide_left.size()),
                             wide_right.c_str(), static_cast<int>(wide_right.size()), TRUE);
  if (comparison == 0 || comparison == CSTR_EQUAL) return true;
#endif
  ec.clear();
  const bool left_exists = std::filesystem::exists(absolute_left, ec);
  if (ec) return true;
  const bool right_exists = std::filesystem::exists(absolute_right, ec);
  if (ec) return true;
  if (left_exists && right_exists) {
    ec.clear();
    const bool equivalent = std::filesystem::equivalent(absolute_left, absolute_right, ec);
    return ec || equivalent;
  }
  return false;
}

bool UserDictionary::Save() const {
  if (save_blocked_by_corrupt_load_) {
    return false;
  }
  disk_state_.reset();
  if (!WriteProtectedText(path_, Serialize(), *crypto_)) return false;
  // Read after the write, under the caller's file lock: no other writer can
  // have replaced the file in between.
  if (const auto stamp = ReadStorageStamp()) disk_state_ = DiskState{*stamp, revision_};
  return true;
}

bool UserDictionary::SavePlaintextExport(const std::filesystem::path& export_path) const {
  if (save_blocked_by_corrupt_load_) return false;
  auto backup = path_;
  backup += ".bak";
  if (SameExportTarget(export_path, path_) || SameExportTarget(export_path, storage_path()) ||
      SameExportTarget(export_path, backup))
    return false;
  return WriteTextFileAtomically(export_path, Serialize());
}

std::string UserDictionary::Serialize() const {
  j::Object root;
  root.emplace("version", j::Value(1));
  j::Array entries;
  for (const auto& [ruby, bucket] : by_ruby_) {
    for (const auto& w : bucket) {
      entries.push_back(WordToJson(w));
    }
  }
  root.emplace("entries", j::Value(std::move(entries)));
  return j::Stringify(j::Value(std::move(root)));
}

std::string UserDictionary::SerializeText() const { return Serialize(); }

bool UserDictionary::LoadText(std::string_view text) {
  auto v = j::Parse(text);
  const auto* entries = v && v->IsObject() ? v->GetArray("entries") : nullptr;
  if (!entries) return false;
  Clear();
  for (const auto& e : *entries) {
    if (auto w = WordFromJson(e)) {
      by_ruby_[w->ruby].push_back(std::move(*w));
    }
  }
  return true;
}

ImportCounts UserDictionary::Merge(const UserDictionary& other, ImportConflictPolicy policy) {
  ImportCounts counts;
  for (const auto& word : other.All()) {
    auto& bucket = by_ruby_[word.ruby];
    auto it = std::find_if(bucket.begin(), bucket.end(),
                           [&](const UserWord& x) { return x.word == word.word; });
    if (it == bucket.end()) {
      bucket.push_back(word);
      ++counts.imported;
      continue;
    }
    ++counts.conflicts;
    // learning-data-management-spec section 6: merge keeps the local word.
    if (policy == ImportConflictPolicy::Overwrite) {
      *it = word;
      ++counts.imported;
    } else {
      ++counts.skipped;
    }
  }
  if (counts.imported > 0) ++revision_;
  return counts;
}

bool UserDictionary::Add(const UserWord& w) {
  ++revision_;
  auto& bucket = by_ruby_[w.ruby];
  auto it = std::find_if(bucket.begin(), bucket.end(),
                         [&](const UserWord& x) { return x.word == w.word; });
  if (it != bucket.end()) {
    *it = w;
    return false;
  }
  bucket.push_back(w);
  return true;
}

bool UserDictionary::Remove(const std::string& word, const std::string& ruby) {
  auto bit = by_ruby_.find(ruby);
  if (bit == by_ruby_.end()) return false;
  auto& bucket = bit->second;
  auto it =
      std::find_if(bucket.begin(), bucket.end(), [&](const UserWord& x) { return x.word == word; });
  if (it == bucket.end()) return false;
  bucket.erase(it);
  ++revision_;
  if (bucket.empty()) by_ruby_.erase(bit);
  return true;
}

void UserDictionary::ReplaceAll(const std::vector<UserWord>& entries) {
  Clear();
  for (const auto& entry : entries) {
    Add(entry);
  }
}

std::vector<UserWord> UserDictionary::Lookup(const std::string& ruby) const {
  auto it = by_ruby_.find(ruby);
  if (it == by_ruby_.end()) return {};
  return it->second;
}

std::vector<UserWord> UserDictionary::All() const {
  std::vector<UserWord> out;
  for (const auto& [ruby, bucket] : by_ruby_) {
    for (const auto& w : bucket) out.push_back(w);
  }
  return out;
}

size_t UserDictionary::Size() const {
  size_t n = 0;
  for (const auto& [ruby, bucket] : by_ruby_) n += bucket.size();
  return n;
}

void UserDictionary::Clear() {
  by_ruby_.clear();
  ++revision_;
}

}  // namespace azookey::learning
