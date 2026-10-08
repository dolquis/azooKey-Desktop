#include "azookey/learning/LearningStore.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <locale>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>
#include <vector>

#include "azookey/learning/AtomicFile.h"
#include "azookey/learning/LearningDecay.h"

namespace azookey::learning {

namespace {

double DecayedWeight(const LearningRecord& rec, uint64_t now_epoch_sec) {
  return rec.weight * LegacyRecency(rec.last_updated_epoch_sec, now_epoch_sec);
}

std::string EscapeTsvField(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (const char ch : value) {
    switch (ch) {
      case '\\':
        escaped += "\\\\";
        break;
      case '\t':
        escaped += "\\t";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      default:
        escaped.push_back(ch);
        break;
    }
  }
  return escaped;
}

std::string UnescapeTsvField(const std::string& value) {
  std::string unescaped;
  unescaped.reserve(value.size());
  for (size_t i = 0; i < value.size(); ++i) {
    if (value[i] != '\\' || i + 1 == value.size()) {
      unescaped.push_back(value[i]);
      continue;
    }

    const char escaped = value[++i];
    switch (escaped) {
      case '\\':
        unescaped.push_back('\\');
        break;
      case 't':
        unescaped.push_back('\t');
        break;
      case 'n':
        unescaped.push_back('\n');
        break;
      case 'r':
        unescaped.push_back('\r');
        break;
      default:
        unescaped.push_back('\\');
        unescaped.push_back(escaped);
        break;
    }
  }
  return unescaped;
}

void LogMalformedLine(const std::filesystem::path& path, size_t line_number) {
  const auto utf8_path = path.u8string();
  const std::string display_path(reinterpret_cast<const char*>(utf8_path.data()), utf8_path.size());
  std::cerr << "LearningStore: skipped malformed record in " << display_path << ":" << line_number
            << '\n';
}

bool IsAsciiWhitespace(char ch) {
  switch (ch) {
    case ' ':
    case '\t':
    case '\n':
    case '\r':
    case '\f':
    case '\v':
      return true;
    default:
      return false;
  }
}

std::string_view TrimLeadingAsciiWhitespace(std::string_view value) {
  while (!value.empty() && IsAsciiWhitespace(value.front())) {
    value.remove_prefix(1);
  }
  return value;
}

bool ConsumeAsciiToken(std::string_view& input, std::string_view& token) {
  input = TrimLeadingAsciiWhitespace(input);
  if (input.empty()) {
    return false;
  }

  size_t end = 0;
  while (end < input.size() && !IsAsciiWhitespace(input[end])) {
    ++end;
  }
  token = input.substr(0, end);
  input.remove_prefix(end);
  return true;
}

bool ParseFiniteNonNegativeDouble(std::string_view token, double& value) {
  if (token.empty()) {
    return false;
  }

  double parsed = 0.0;
  const char* const first = token.data();
  const char* const last = first + token.size();
  const auto result = std::from_chars(first, last, parsed);
  if (result.ec != std::errc{} || result.ptr != last || !std::isfinite(parsed) ||
      parsed < 0.0) {
    return false;
  }

  value = parsed;
  return true;
}

bool ParseUint64(std::string_view token, uint64_t& value) {
  if (token.empty()) {
    return false;
  }

  uint64_t parsed = 0;
  const char* const first = token.data();
  const char* const last = first + token.size();
  const auto result = std::from_chars(first, last, parsed);
  if (result.ec != std::errc{} || result.ptr != last) {
    return false;
  }

  value = parsed;
  return true;
}

bool ParseRecordValues(std::string_view value, LearningRecord& rec) {
  std::string_view weight_token;
  std::string_view timestamp_token;
  if (!ConsumeAsciiToken(value, weight_token) || !ConsumeAsciiToken(value, timestamp_token)) {
    return false;
  }
  if (!TrimLeadingAsciiWhitespace(value).empty()) {
    return false;
  }

  LearningRecord parsed;
  if (!ParseFiniteNonNegativeDouble(weight_token, parsed.weight) ||
      !ParseUint64(timestamp_token, parsed.last_updated_epoch_sec)) {
    return false;
  }

  rec = parsed;
  return true;
}

// M7 rows carry only the summed weight; estimate how many commits produced it
// so that the count-based user_score keeps the row's effect after migration.
void FillMigratedCounts(LearningRecord& rec) {
  if (rec.weight <= 0.0) return;
  // Bounded so that an absurd weight cannot overflow the conversion.
  constexpr double kMaxMigratedCommits = 1e12;
  const double commits =
      std::min(std::round(rec.weight / kLegacyCommitWeight), kMaxMigratedCommits);
  rec.commit_count = commits < 1.0 ? 1 : static_cast<uint64_t>(commits);
}

bool IsContextHash(std::string_view value) {
  if (value.empty()) return true;
  if (value.size() != 10 || value.substr(0, 2) != "0x") return false;
  return std::all_of(value.begin() + 2, value.end(),
                     [](char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'); });
}

std::vector<std::string_view> SplitTabs(std::string_view line) {
  std::vector<std::string_view> fields;
  size_t begin = 0;
  while (true) {
    const size_t end = line.find('\t', begin);
    fields.push_back(line.substr(begin, end == std::string_view::npos ? end : end - begin));
    if (end == std::string_view::npos) break;
    begin = end + 1;
  }
  return fields;
}

constexpr size_t kV2FieldCount = 10;

bool ParseV2Row(std::string_view line, LearningEntry& entry) {
  const auto fields = SplitTabs(line);
  if (fields.size() != kV2FieldCount || fields[0].empty() || fields[1].empty()) return false;
  LearningRecord rec;
  const auto event = ParseLearningEventType(fields[8]);
  if (!ParseFiniteNonNegativeDouble(fields[2], rec.weight) ||
      !ParseUint64(fields[3], rec.last_updated_epoch_sec) ||
      !ParseUint64(fields[4], rec.commit_count) || !ParseUint64(fields[5], rec.accept_count) ||
      !ParseUint64(fields[6], rec.reject_count) || !event || !IsContextHash(fields[9])) {
    return false;
  }
  rec.last_event = *event;
  rec.context_hash = std::string(fields[9]);
  entry.reading = UnescapeTsvField(std::string(fields[0]));
  entry.surface = UnescapeTsvField(std::string(fields[1]));
  entry.app_name = UnescapeTsvField(std::string(fields[7]));
  entry.record = std::move(rec);
  return true;
}

bool ParseLegacyRow(const std::string& line, bool escaped_fields, LearningEntry& entry) {
  std::istringstream iss(line);
  std::string weight_timestamp;
  if (!(std::getline(iss, entry.reading, '\t') && std::getline(iss, entry.surface, '\t') &&
        std::getline(iss, weight_timestamp))) {
    return false;
  }
  if (!ParseRecordValues(weight_timestamp, entry.record)) return false;
  if (escaped_fields) {
    entry.reading = UnescapeTsvField(entry.reading);
    entry.surface = UnescapeTsvField(entry.surface);
  }
  FillMigratedCounts(entry.record);
  return true;
}

using Table = std::map<std::string, std::map<std::string, std::map<std::string, LearningRecord>>>;

// Calls on_row(entry) for each parsed row and on_malformed(line_number) for
// each row that does not parse.
template <typename OnRow, typename OnMalformed>
void ParseRows(std::string_view text, OnRow on_row, OnMalformed on_malformed) {
  enum class Format { V2, Escaped, Legacy };
  std::istringstream ifs{std::string(text)};
  std::string line;
  size_t line_number = 0;
  Format format = Format::Legacy;
  while (std::getline(ifs, line)) {
    ++line_number;
    if (line_number == 1 && line == kLearningStoreV2Header) {
      format = Format::V2;
      continue;
    }
    if (line_number == 1 && line == kLearningStoreEscapedTsvHeader) {
      format = Format::Escaped;
      continue;
    }
    if (line.empty()) continue;

    LearningEntry entry;
    const bool parsed = format == Format::V2
                            ? ParseV2Row(line, entry)
                            : ParseLegacyRow(line, format == Format::Escaped, entry);
    if (parsed) {
      on_row(entry);
    } else {
      on_malformed(line_number);
    }
  }
}

// Duplicate rows keep the first occurrence (spec section 14.3). Returns false
// when any row was malformed and skipped.
bool ParseInto(std::string_view text, const std::filesystem::path& path, Table& table) {
  bool all_valid = true;
  ParseRows(
      text,
      [&](LearningEntry& entry) {
        table[entry.reading][entry.surface].emplace(entry.app_name, std::move(entry.record));
      },
      [&](size_t line_number) {
        all_valid = false;
        LogMalformedLine(path, line_number);
      });
  return all_valid;
}

bool IsForgotten(const LearningRecord& rec) {
  return rec.weight <= 0.0 && rec.commit_count == 0 && rec.accept_count == 0 &&
         rec.reject_count == 0;
}

bool AllForgotten(const std::map<std::string, LearningRecord>& rows) {
  return std::all_of(rows.begin(), rows.end(),
                     [](const auto& row) { return IsForgotten(row.second); });
}

bool HasNetRejection(const std::map<std::string, LearningRecord>& rows) {
  uint64_t rejects = 0;
  uint64_t accepts = 0;
  for (const auto& [_, record] : rows) {
    rejects += record.reject_count;
    accepts += record.accept_count;
  }
  return rejects > accepts;
}

std::string SerializedKey(const std::string& reading, const std::string& surface,
                          const std::string& app_name) {
  return EscapeTsvField(reading) + "\t" + EscapeTsvField(surface) + "\t" + EscapeTsvField(app_name);
}

double SumDecayedWeight(const std::map<std::string, LearningRecord>& rows, uint64_t now_epoch_sec) {
  double total = 0.0;
  for (const auto& [_, record] : rows) {
    total += DecayedWeight(record, now_epoch_sec);
  }
  return total;
}

void AddRecord(LearningRecord& into, const LearningRecord& from) {
  into.weight += from.weight;
  into.commit_count += from.commit_count;
  into.accept_count += from.accept_count;
  into.reject_count += from.reject_count;
  if (from.last_updated_epoch_sec >= into.last_updated_epoch_sec) {
    into.last_updated_epoch_sec = from.last_updated_epoch_sec;
    into.last_event = from.last_event;
    into.context_hash = from.context_hash;
  }
}
}  // namespace

std::string_view LearningEventTypeName(LearningEventType type) {
  switch (type) {
    case LearningEventType::None:
      return "";
    case LearningEventType::Commit:
      return "commit";
    case LearningEventType::CorrectionAccept:
      return "correction_accept";
    case LearningEventType::CorrectionReject:
      return "correction_reject";
    case LearningEventType::TypoAccept:
      return "typo_accept";
    case LearningEventType::TypoReject:
      return "typo_reject";
  }
  return "";
}

std::optional<LearningEventType> ParseLearningEventType(std::string_view name) {
  for (const auto type : {LearningEventType::None, LearningEventType::Commit,
                          LearningEventType::CorrectionAccept, LearningEventType::CorrectionReject,
                          LearningEventType::TypoAccept, LearningEventType::TypoReject}) {
    if (LearningEventTypeName(type) == name) return type;
  }
  return std::nullopt;
}

std::string_view ImportConflictPolicyName(ImportConflictPolicy policy) {
  switch (policy) {
    case ImportConflictPolicy::Merge:
      return "merge";
    case ImportConflictPolicy::Overwrite:
      return "overwrite";
    case ImportConflictPolicy::KeepBoth:
      return "keep_both";
  }
  return "merge";
}

std::optional<ImportConflictPolicy> ParseImportConflictPolicy(std::string_view name) {
  for (const auto policy : {ImportConflictPolicy::Merge, ImportConflictPolicy::Overwrite,
                            ImportConflictPolicy::KeepBoth}) {
    if (ImportConflictPolicyName(policy) == name) return policy;
  }
  return std::nullopt;
}

std::filesystem::path LearningStoreV2PathFor(const std::filesystem::path& legacy_path) {
  auto path = legacy_path;
  path.replace_filename(legacy_path.stem().string() + ".v2" + legacy_path.extension().string());
  return path;
}

std::optional<size_t> CountValidLearningRows(std::string_view text) {
  size_t rows = 0;
  bool valid = true;
  ParseRows(text, [&](LearningEntry&) { ++rows; }, [&](size_t) { valid = false; });
  return valid ? std::optional<size_t>(rows) : std::nullopt;
}

std::string NormalizeLearningAppName(std::string_view process_name) {
  const size_t separator = process_name.find_last_of("/\\");
  if (separator != std::string_view::npos) process_name.remove_prefix(separator + 1);
  std::string normalized(process_name);
  for (char& ch : normalized) {
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
  }
  return normalized;
}

LearningStore::LearningStore(std::filesystem::path path, const ByteCrypto* crypto)
    : path_(std::move(path)), crypto_(crypto ? crypto : &DpapiCrypto()) {}

bool LearningStore::Load() { return LoadImpl(true); }

bool LearningStore::LoadReadOnly() { return LoadImpl(false); }

bool LearningStore::LoadImpl(bool migrate_plaintext) {
  table_.clear();
  dirty_ = false;
  save_blocked_by_load_failure_ = false;
  // The v2 file wins once it exists. Until then the M7 file is the source; it
  // is never rewritten in v2, so an older Host can still read it.
  auto source_path = LearningStoreV2PathFor(path_);
  std::string text;
  auto source = ReadProtectedText(source_path, *crypto_, text);
  const bool from_legacy = source == ProtectedFileSource::Missing;
  if (from_legacy) {
    source_path = path_;
    source = ReadProtectedText(source_path, *crypto_, text);
  }
  if (source == ProtectedFileSource::Missing) {
    return false;
  }
  if (source == ProtectedFileSource::Error) {
    save_blocked_by_load_failure_ = true;
    return false;
  }
  ParseInto(text, source_path, table_);
  if (source == ProtectedFileSource::Plaintext && migrate_plaintext &&
      !MigratePlaintextFile(source_path, text, *crypto_)) {
    save_blocked_by_load_failure_ = true;
    SecureErase(text);
    return false;
  }
  SecureErase(text);
  // Migrated rows are written to the v2 file on the next flush.
  dirty_ = from_legacy && !table_.empty();
  return true;
}

std::string LearningStore::SerializeText() const {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << kLearningStoreV2Header << '\n';
  std::vector<std::pair<std::string, const LearningRecord*>> rows;
  rows.reserve(size());
  for (const auto& [reading, surfaces] : table_) {
    for (const auto& [surface, apps] : surfaces) {
      for (const auto& [app_name, record] : apps) {
        if (IsForgotten(record)) continue;
        rows.emplace_back(SerializedKey(reading, surface, app_name), &record);
      }
    }
  }
  std::sort(rows.begin(), rows.end(),
            [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
  for (const auto& [key, record] : rows) {
    // key is "reading\tsurface\tapp"; app_name moves after the counts.
    const size_t app_separator = key.rfind('\t');
    out << key.substr(0, app_separator) << '\t' << record->weight << '\t'
        << record->last_updated_epoch_sec << '\t' << record->commit_count << '\t'
        << record->accept_count << '\t' << record->reject_count << '\t'
        << key.substr(app_separator + 1) << '\t' << LearningEventTypeName(record->last_event)
        << '\t' << record->context_hash << '\n';
  }
  return out.str();
}

bool LearningStore::LoadText(std::string_view text) {
  table_.clear();
  dirty_ = true;
  return ParseInto(text, path_, table_);
}

bool LearningStore::save_blocked() const { return save_blocked_by_load_failure_; }

bool LearningStore::Save(std::chrono::milliseconds retry_budget) const {
  if (save_blocked_by_load_failure_) return false;
  const auto v2_path = LearningStoreV2PathFor(path_);
  // Until the v2 file exists the M7 file is the source of truth: refuse to
  // shadow it while it holds an orphan backup or unmigrated plaintext, and
  // finish its interrupted migration first.
  std::error_code encrypted_error;
  std::error_code plain_error;
  const bool v2_exists = std::filesystem::exists(EncryptedPathFor(v2_path), encrypted_error) ||
                         std::filesystem::exists(v2_path, plain_error);
  if (encrypted_error || plain_error) return false;
  if (!v2_exists && !SettleProtectedFile(path_, *crypto_)) return false;
  auto text = SerializeText();
  const bool saved = WriteProtectedText(v2_path, text, *crypto_, retry_budget);
  SecureErase(text);
  if (saved) {
    dirty_ = false;
  }
  return saved;
}

void LearningStore::Reset() {
  if (!table_.empty()) {
    dirty_ = true;
  }
  table_.clear();
}

bool LearningStore::dirty() const { return dirty_; }

size_t LearningStore::size() const {
  size_t count = 0;
  for (const auto& [_, surfaces] : table_) {
    for (const auto& [__, apps] : surfaces) {
      count += apps.size();
    }
  }
  return count;
}

std::vector<LearningEntry> LearningStore::All() const {
  std::vector<LearningEntry> entries;
  entries.reserve(size());
  for (const auto& [reading, surfaces] : table_) {
    for (const auto& [surface, apps] : surfaces) {
      for (const auto& [app_name, record] : apps) {
        entries.push_back(LearningEntry{reading, surface, record, app_name});
      }
    }
  }
  return entries;
}

std::vector<LearningAggregate> LearningStore::Aggregates() const {
  std::vector<LearningAggregate> aggregates;
  for (const auto& [reading, surfaces] : table_) {
    for (const auto& [surface, apps] : surfaces) {
      LearningAggregate aggregate{reading, surface};
      for (const auto& [app_name, record] : apps) {
        if (IsForgotten(record)) continue;
        aggregate.weight += record.weight;
        aggregate.last_updated_epoch_sec =
            std::max(aggregate.last_updated_epoch_sec, record.last_updated_epoch_sec);
        aggregate.commit_count += record.commit_count;
        aggregate.accept_count += record.accept_count;
        aggregate.reject_count += record.reject_count;
        if (!app_name.empty()) aggregate.app_names.push_back(app_name);
      }
      if (aggregate.weight > 0.0 || aggregate.commit_count > 0 || aggregate.accept_count > 0 ||
          aggregate.reject_count > 0) {
        aggregates.push_back(std::move(aggregate));
      }
    }
  }
  return aggregates;
}

const std::map<std::string, LearningRecord>* LearningStore::Rows(const std::string& reading,
                                                                 const std::string& surface) const {
  const auto reading_it = table_.find(reading);
  if (reading_it == table_.end()) return nullptr;
  const auto surface_it = reading_it->second.find(surface);
  return surface_it == reading_it->second.end() ? nullptr : &surface_it->second;
}

PrefixLookupResult LearningStore::LookupPrefix(const std::string& reading_prefix, size_t limit,
                                               double min_score, uint64_t now_epoch_sec) const {
  PrefixLookupResult result;
  if (reading_prefix.empty() || limit == 0) {
    return result;
  }

  for (auto it = table_.lower_bound(reading_prefix); it != table_.end(); ++it) {
    ++result.visited_readings;
    const auto& reading = it->first;
    if (reading.compare(0, reading_prefix.size(), reading_prefix) != 0) {
      break;
    }
    for (const auto& [surface, apps] : it->second) {
      ++result.scanned_records;
      // A forgotten pair never appears, whatever min_score the caller passes.
      if (AllForgotten(apps)) continue;
      const double score = SumDecayedWeight(apps, now_epoch_sec);
      if (score >= min_score) {
        result.matches.push_back(PrefixMatch{reading, surface, score});
      }
    }
  }

  const auto better_match = [](const PrefixMatch& lhs, const PrefixMatch& rhs) {
    if (lhs.score != rhs.score) return lhs.score > rhs.score;
    if (lhs.reading != rhs.reading) return lhs.reading < rhs.reading;
    return lhs.surface < rhs.surface;
  };
  const size_t kept = std::min(limit, result.matches.size());
  std::partial_sort(result.matches.begin(), result.matches.begin() + kept, result.matches.end(),
                    better_match);
  result.matches.resize(kept);
  return result;
}

std::string LearningStore::ReverseLookup(const std::string& surface, uint64_t now_epoch_sec) const {
  std::string best_reading;
  double best_score = 0.0;
  if (surface.empty()) return best_reading;
  for (const auto& [reading, surfaces] : table_) {
    const auto it = surfaces.find(surface);
    if (it == surfaces.end()) continue;
    // Readings iterate in ascending order, so ties keep the smallest one.
    const double score = SumDecayedWeight(it->second, now_epoch_sec);
    if (score > best_score) {
      best_score = score;
      best_reading = reading;
    }
  }
  return best_reading;
}

void LearningStore::Observe(const std::string& reading, const std::string& surface, double alpha,
                            uint64_t now_epoch_sec) {
  ObserveEvent(LearningObservation{reading, surface, "", LearningEventType::Commit, ""}, alpha,
               now_epoch_sec);
}

void LearningStore::ObserveCorrection(const std::string& reading,
                                      const std::string& rejected_surface,
                                      const std::string& selected_surface, double alpha,
                                      uint64_t now_epoch_sec) {
  ObserveEvent(
      LearningObservation{reading, selected_surface, "", LearningEventType::CorrectionAccept, ""},
      alpha, now_epoch_sec);
  ObserveEvent(
      LearningObservation{reading, rejected_surface, "", LearningEventType::CorrectionReject, ""},
      alpha, now_epoch_sec);
}

void LearningStore::ObserveEvent(const LearningObservation& observation, double alpha,
                                 uint64_t now_epoch_sec) {
  if (observation.event == LearningEventType::None) return;
  auto& rec = table_[observation.reading][observation.surface]
                    [NormalizeLearningAppName(observation.app_name)];
  switch (observation.event) {
    case LearningEventType::Commit:
      rec.weight += alpha;
      ++rec.commit_count;
      break;
    // An accepted correction or typo fix is still a commit of the surface.
    case LearningEventType::CorrectionAccept:
      rec.weight += alpha;
      ++rec.commit_count;
      ++rec.accept_count;
      break;
    case LearningEventType::TypoAccept:
      rec.weight += kTypoAcceptWeight;
      ++rec.commit_count;
      ++rec.accept_count;
      break;
    case LearningEventType::CorrectionReject:
      rec.weight = std::max(0.0, rec.weight - alpha);
      ++rec.reject_count;
      break;
    case LearningEventType::TypoReject:
      rec.weight = std::max(0.0, rec.weight - kTypoRejectWeight);
      ++rec.reject_count;
      break;
    case LearningEventType::None:
      break;
  }
  rec.last_updated_epoch_sec = now_epoch_sec;
  rec.last_event = observation.event;
  rec.context_hash = observation.context_hash;
  dirty_ = true;
}

bool LearningStore::Forget(const std::string& reading, const std::string& surface) {
  const auto reading_it = table_.find(reading);
  if (reading_it == table_.end()) return false;
  const auto surface_it = reading_it->second.find(surface);
  if (surface_it == reading_it->second.end()) return false;
  for (auto& [_, record] : surface_it->second) {
    record = LearningRecord{0.0, record.last_updated_epoch_sec};
  }
  dirty_ = true;
  return true;
}

void LearningStore::Prune(size_t max_records, double min_weight, uint64_t now_epoch_sec) {
  bool changed = false;
  if (min_weight > 0.0) {
    // The threshold applies to the pair, as Score() does. A pair the user has
    // rejected more often than accepted is kept so that its penalty (spec
    // section 6.2) survives; its weight is usually zero exactly because of
    // those rejections. max_records still bounds it below.
    for (auto reading_it = table_.begin(); reading_it != table_.end();) {
      auto& surfaces = reading_it->second;
      for (auto surface_it = surfaces.begin(); surface_it != surfaces.end();) {
        const auto& apps = surface_it->second;
        if (SumDecayedWeight(apps, now_epoch_sec) < min_weight && !HasNetRejection(apps)) {
          surface_it = surfaces.erase(surface_it);
          changed = true;
        } else {
          ++surface_it;
        }
      }
      reading_it = surfaces.empty() ? table_.erase(reading_it) : std::next(reading_it);
    }
  }

  const size_t record_count = size();
  if (max_records > 0 && record_count > max_records) {
    struct RankedRecord {
      std::string reading;
      std::string surface;
      std::string app_name;
      std::string serialized_key;
      double score;
    };
    std::vector<RankedRecord> ranked;
    ranked.reserve(record_count);
    for (const auto& [reading, surfaces] : table_) {
      for (const auto& [surface, apps] : surfaces) {
        for (const auto& [app_name, record] : apps) {
          ranked.push_back({reading, surface, app_name, SerializedKey(reading, surface, app_name),
                            DecayedWeight(record, now_epoch_sec)});
        }
      }
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto& lhs, const auto& rhs) {
      if (lhs.score == rhs.score) {
        return lhs.serialized_key < rhs.serialized_key;
      }
      return lhs.score < rhs.score;
    });

    const size_t remove_count = record_count - max_records;
    for (size_t i = 0; i < remove_count; ++i) {
      auto reading_it = table_.find(ranked[i].reading);
      if (reading_it == table_.end()) continue;
      auto surface_it = reading_it->second.find(ranked[i].surface);
      if (surface_it == reading_it->second.end()) continue;
      surface_it->second.erase(ranked[i].app_name);
      if (surface_it->second.empty()) reading_it->second.erase(surface_it);
      if (reading_it->second.empty()) table_.erase(reading_it);
    }
    changed = remove_count > 0;
  }

  if (changed) {
    dirty_ = true;
  }
}

double LearningStore::Score(const std::string& reading, const std::string& surface,
                            uint64_t now_epoch_sec) const {
  const auto* rows = Rows(reading, surface);
  return rows ? SumDecayedWeight(*rows, now_epoch_sec) : 0.0;
}

ImportCounts LearningStore::Merge(const LearningStore& other, ImportConflictPolicy policy) {
  ImportCounts counts;
  for (const auto& [reading, surfaces] : other.table_) {
    for (const auto& [surface, apps] : surfaces) {
      for (const auto& [app_name, record] : apps) {
        if (IsForgotten(record)) {
          ++counts.skipped;
          continue;
        }
        auto& existing_apps = table_[reading][surface];
        const auto [it, inserted] = existing_apps.emplace(app_name, record);
        // A forgotten local row no longer exists for the user; it is no clash.
        if (inserted || IsForgotten(it->second)) {
          it->second = record;
          ++counts.imported;
          continue;
        }
        ++counts.conflicts;
        switch (policy) {
          case ImportConflictPolicy::Merge:
            AddRecord(it->second, record);
            ++counts.imported;
            break;
          case ImportConflictPolicy::Overwrite:
            it->second = record;
            ++counts.imported;
            break;
          case ImportConflictPolicy::KeepBoth:
            ++counts.skipped;
            break;
        }
      }
    }
  }
  if (counts.imported > 0) dirty_ = true;
  return counts;
}

}  // namespace azookey::learning
