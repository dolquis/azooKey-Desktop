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

bool UserDictionary::LoadImpl(bool quarantine_corrupt_file) {
  ++revision_;
  by_ruby_.clear();
  save_blocked_by_corrupt_load_ = false;
  std::string text;
  const auto source = ReadProtectedText(path_, *crypto_, text);
  if (source == ProtectedFileSource::Missing) {
    return true;  // missing file is fine
  }
  if (source == ProtectedFileSource::Error) {
    save_blocked_by_corrupt_load_ = true;
    return false;
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
  return WriteProtectedText(path_, Serialize(), *crypto_);
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
