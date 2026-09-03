#include "azookey/learning/LearningStore.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <locale>
#include <sstream>
#include <string_view>
#include <system_error>
#include <vector>

#include "azookey/learning/AtomicFile.h"

namespace azookey::learning {

namespace {

double DecayedWeight(const LearningRecord& rec, uint64_t now_epoch_sec) {
  const double elapsed_seconds =
      now_epoch_sec >= rec.last_updated_epoch_sec
          ? static_cast<double>(now_epoch_sec - rec.last_updated_epoch_sec)
          : 0.0;
  const double days = elapsed_seconds / (60.0 * 60.0 * 24.0);
  const double decay = std::exp(-0.15 * days);
  return rec.weight * decay;
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

std::string SerializedKey(const std::string& reading, const std::string& surface) {
  return EscapeTsvField(reading) + "\t" + EscapeTsvField(surface);
}
}  // namespace

LearningStore::LearningStore(std::filesystem::path path) : path_(std::move(path)) {}

bool LearningStore::Load() {
  table_.clear();
  dirty_ = false;
  std::ifstream ifs(path_);
  if (!ifs.is_open()) {
    return false;
  }
  std::string line;
  size_t line_number = 0;
  bool escaped_fields = false;
  while (std::getline(ifs, line)) {
    ++line_number;
    if (line_number == 1 && line == kLearningStoreEscapedTsvHeader) {
      escaped_fields = true;
      continue;
    }

    std::istringstream iss(line);
    std::string reading;
    std::string surface;
    std::string weight_timestamp;
    LearningRecord rec;
    if (!(std::getline(iss, reading, '\t') && std::getline(iss, surface, '\t') &&
          std::getline(iss, weight_timestamp))) {
      LogMalformedLine(path_, line_number);
      continue;
    }

    if (!ParseRecordValues(weight_timestamp, rec)) {
      LogMalformedLine(path_, line_number);
      continue;
    }
    if (escaped_fields) {
      reading = UnescapeTsvField(reading);
      surface = UnescapeTsvField(surface);
    }
    table_[reading].emplace(surface, rec);
  }
  return true;
}

bool LearningStore::Save() const {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << kLearningStoreEscapedTsvHeader << '\n';
  std::vector<std::pair<std::string, const LearningRecord*>> rows;
  rows.reserve(size());
  for (const auto& [reading, surfaces] : table_) {
    for (const auto& [surface, record] : surfaces) {
      rows.emplace_back(SerializedKey(reading, surface), &record);
    }
  }
  std::sort(rows.begin(), rows.end(),
            [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
  for (const auto& [key, record] : rows) {
    out << key << '\t' << record->weight << ' ' << record->last_updated_epoch_sec << '\n';
  }
  const bool saved = WriteTextFileAtomically(path_, out.str());
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
    count += surfaces.size();
  }
  return count;
}

std::vector<LearningEntry> LearningStore::All() const {
  std::vector<LearningEntry> entries;
  entries.reserve(size());
  for (const auto& [reading, surfaces] : table_) {
    for (const auto& [surface, record] : surfaces) {
      entries.push_back(LearningEntry{reading, surface, record});
    }
  }
  return entries;
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
    for (const auto& [surface, record] : it->second) {
      ++result.scanned_records;
      const double score = DecayedWeight(record, now_epoch_sec);
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

void LearningStore::Observe(const std::string& reading, const std::string& surface, double alpha,
                            uint64_t now_epoch_sec) {
  auto& rec = table_[reading][surface];
  rec.weight += alpha;
  rec.last_updated_epoch_sec = now_epoch_sec;
  dirty_ = true;
}

void LearningStore::ObserveCorrection(const std::string& reading,
                                      const std::string& rejected_surface,
                                      const std::string& selected_surface, double alpha,
                                      uint64_t now_epoch_sec) {
  Observe(reading, selected_surface, alpha, now_epoch_sec);

  auto& rejected = table_[reading][rejected_surface];
  rejected.weight = std::max(0.0, rejected.weight - alpha);
  rejected.last_updated_epoch_sec = now_epoch_sec;
  dirty_ = true;
}

void LearningStore::Prune(size_t max_records, double min_weight, uint64_t now_epoch_sec) {
  bool changed = false;
  if (min_weight > 0.0) {
    for (auto reading_it = table_.begin(); reading_it != table_.end();) {
      for (auto surface_it = reading_it->second.begin(); surface_it != reading_it->second.end();) {
        if (DecayedWeight(surface_it->second, now_epoch_sec) < min_weight) {
          surface_it = reading_it->second.erase(surface_it);
          changed = true;
        } else {
          ++surface_it;
        }
      }
      if (reading_it->second.empty()) {
        reading_it = table_.erase(reading_it);
      } else {
        ++reading_it;
      }
    }
  }

  const size_t record_count = size();
  if (max_records > 0 && record_count > max_records) {
    struct RankedRecord {
      std::string reading;
      std::string surface;
      std::string serialized_key;
      double score;
    };
    std::vector<RankedRecord> ranked;
    ranked.reserve(record_count);
    for (const auto& [reading, surfaces] : table_) {
      for (const auto& [surface, record] : surfaces) {
        ranked.push_back({reading, surface, SerializedKey(reading, surface),
                          DecayedWeight(record, now_epoch_sec)});
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
      reading_it->second.erase(ranked[i].surface);
      if (reading_it->second.empty()) {
        table_.erase(reading_it);
      }
    }
    changed = remove_count > 0;
  }

  if (changed) {
    dirty_ = true;
  }
}

double LearningStore::Score(const std::string& reading, const std::string& surface,
                            uint64_t now_epoch_sec) const {
  const auto reading_it = table_.find(reading);
  if (reading_it == table_.end()) {
    return 0.0;
  }
  const auto surface_it = reading_it->second.find(surface);
  if (surface_it == reading_it->second.end()) {
    return 0.0;
  }
  return DecayedWeight(surface_it->second, now_epoch_sec);
}

}  // namespace azookey::learning
