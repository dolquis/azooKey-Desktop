#include "azookey/learning/TypoCorrectionStore.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <locale>
#include <sstream>
#include <vector>

#include "azookey/core/Utf8.h"
#include "azookey/learning/AtomicFile.h"

namespace azookey::learning {

namespace {

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
      case '#':
        // Load() skips a line that starts with '#' as a comment, so a reading
        // beginning with one would make the whole record vanish on reload.
        escaped += "\\#";
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
      case '#':
        unescaped.push_back('#');
        break;
      default:
        unescaped.push_back('\\');
        unescaped.push_back(escaped);
        break;
    }
  }
  return unescaped;
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
  if (input.empty()) return false;
  size_t end = 0;
  while (end < input.size() && !IsAsciiWhitespace(input[end])) ++end;
  token = input.substr(0, end);
  input.remove_prefix(end);
  return true;
}

template <typename T>
bool ParseUnsigned(std::string_view token, T& value) {
  if (token.empty()) return false;
  T parsed = 0;
  const char* const first = token.data();
  const char* const last = first + token.size();
  const auto result = std::from_chars(first, last, parsed);
  if (result.ec != std::errc{} || result.ptr != last) return false;
  value = parsed;
  return true;
}

// "count last_updated_epoch", matching LearningStore's "weight epoch" tail.
bool ParseRecordValues(std::string_view value, TypoCorrectionRecord& rec) {
  std::string_view count_token;
  std::string_view timestamp_token;
  if (!ConsumeAsciiToken(value, count_token) || !ConsumeAsciiToken(value, timestamp_token)) {
    return false;
  }
  if (!TrimLeadingAsciiWhitespace(value).empty()) return false;

  TypoCorrectionRecord parsed;
  if (!ParseUnsigned(count_token, parsed.count) ||
      !ParseUnsigned(timestamp_token, parsed.last_updated_epoch_sec)) {
    return false;
  }
  // A zero count would let a row that was never observed satisfy min_count == 0.
  if (parsed.count == 0) return false;
  rec = parsed;
  return true;
}

void LogMalformedLine(const std::filesystem::path& path, size_t line_number) {
  const auto utf8_path = path.u8string();
  const std::string display_path(reinterpret_cast<const char*>(utf8_path.data()), utf8_path.size());
  std::cerr << "TypoCorrectionStore: skipped malformed record in " << display_path << ":"
            << line_number << '\n';
}

std::vector<char32_t> DecodeUtf8(std::string_view value) {
  std::vector<char32_t> codepoints;
  size_t offset = 0;
  char32_t codepoint = 0;
  while (offset < value.size()) {
    const size_t previous = offset;
    if (!core::DecodeNextUtf8(value, offset, codepoint)) {
      // DecodeNextUtf8 already consumed the single offending byte and reported
      // it as the codepoint, so an invalid sequence still advances.
      codepoints.push_back(codepoint);
      if (offset == previous) break;
      continue;
    }
    codepoints.push_back(codepoint);
  }
  return codepoints;
}

}  // namespace

TypoCorrectionStore::TypoCorrectionStore(std::filesystem::path path, const ByteCrypto* crypto)
    : path_(std::move(path)), crypto_(crypto ? crypto : &DpapiCrypto()) {}

size_t TypoCorrectionStore::Utf8CharLength(std::string_view value) {
  return DecodeUtf8(value).size();
}

size_t TypoCorrectionStore::Utf8EditDistance(std::string_view a, std::string_view b) {
  const auto lhs = DecodeUtf8(a);
  const auto rhs = DecodeUtf8(b);
  if (lhs.empty()) return rhs.size();
  if (rhs.empty()) return lhs.size();

  // Levenshtein over code points with a rolling row: readings are short, and the
  // row keeps the allocation proportional to the shorter side.
  std::vector<size_t> previous(rhs.size() + 1);
  std::vector<size_t> current(rhs.size() + 1);
  for (size_t j = 0; j <= rhs.size(); ++j) previous[j] = j;
  for (size_t i = 1; i <= lhs.size(); ++i) {
    current[0] = i;
    for (size_t j = 1; j <= rhs.size(); ++j) {
      const size_t substitution = previous[j - 1] + (lhs[i - 1] == rhs[j - 1] ? 0 : 1);
      current[j] = std::min({current[j - 1] + 1, previous[j] + 1, substitution});
    }
    previous.swap(current);
  }
  return previous[rhs.size()];
}

size_t TypoCorrectionStore::EditDistanceLimit(size_t reading_length) {
  // Section 6: relative to the reading length, capped so long pairs cannot drift
  // arbitrarily far apart.
  const double relative =
      std::ceil(static_cast<double>(reading_length) * kTypoCorrectionEditDistanceRatio);
  const size_t scaled = std::max<size_t>(1, static_cast<size_t>(relative));
  return std::min(scaled, kTypoCorrectionMaxEditDistance);
}

bool TypoCorrectionStore::IsLearnablePair(const std::string& wrong, const std::string& correct) {
  // Retyping the same reading carries no correction.
  if (wrong == correct) return false;
  // An abandoned input leaves an empty committed reading.
  if (wrong.empty() || correct.empty()) return false;

  const size_t wrong_length = Utf8CharLength(wrong);
  const size_t correct_length = Utf8CharLength(correct);
  // One-character readings cannot be told apart from an ordinary candidate
  // mis-selection.
  if (wrong_length < kTypoCorrectionMinReadingLength ||
      correct_length < kTypoCorrectionMinReadingLength) {
    return false;
  }

  // Bound the work before the quadratic step, not after.
  if (wrong_length > kTypoCorrectionMaxReadingLength ||
      correct_length > kTypoCorrectionMaxReadingLength) {
    return false;
  }

  const size_t limit = EditDistanceLimit(std::max(wrong_length, correct_length));
  // Edit distance is at least the length difference, so a pair that differs by
  // more than the limit is rejected without building the table at all.
  const size_t length_difference =
      wrong_length > correct_length ? wrong_length - correct_length : correct_length - wrong_length;
  if (length_difference > limit) return false;
  return Utf8EditDistance(wrong, correct) <= limit;
}

bool TypoCorrectionStore::Observe(const std::string& wrong, const std::string& correct,
                                  uint64_t now_epoch_sec) {
  if (!IsLearnablePair(wrong, correct)) return false;
  auto& record = table_[wrong][correct];
  // Saturate rather than wrap: a wrapped count would silently fall back under
  // min_count and drop a well-established correction.
  if (record.count < (std::numeric_limits<uint32_t>::max)()) ++record.count;
  record.last_updated_epoch_sec = now_epoch_sec;
  return true;
}

std::optional<std::string> TypoCorrectionStore::Lookup(const std::string& wrong,
                                                       uint32_t min_count) const {
  const auto it = table_.find(wrong);
  if (it == table_.end()) return std::nullopt;

  const std::string* best = nullptr;
  uint32_t best_count = 0;
  for (const auto& [correct, record] : it->second) {
    if (record.count < min_count) continue;
    // The map iterates in ascending surface order, so keeping the first of an
    // equal count makes ties deterministic.
    if (record.count > best_count) {
      best_count = record.count;
      best = &correct;
    }
  }
  if (!best) return std::nullopt;
  return *best;
}

std::optional<std::string> TypoCorrectionStore::Lookup(const std::string& wrong, uint32_t min_count,
                                                       uint64_t now_epoch_sec) const {
  const auto it = table_.find(wrong);
  if (it == table_.end()) return std::nullopt;

  const std::string* best = nullptr;
  uint32_t best_count = 0;
  for (const auto& [correct, record] : it->second) {
    if (record.count < min_count) continue;
    // A record last touched long ago describes a habit the user no longer has.
    // A timestamp in the future (clock change) is treated as current rather than
    // as an underflowed age.
    if (now_epoch_sec > record.last_updated_epoch_sec &&
        now_epoch_sec - record.last_updated_epoch_sec > kTypoCorrectionMaxRecordAgeSec) {
      continue;
    }
    if (record.count > best_count) {
      best_count = record.count;
      best = &correct;
    }
  }
  if (!best) return std::nullopt;
  return *best;
}

bool TypoCorrectionStore::Load() {
  table_.clear();
  save_blocked_by_load_failure_ = false;
  std::string text;
  const auto source = ReadProtectedText(path_, *crypto_, text);
  if (source == ProtectedFileSource::Missing) {
    // A store that was never written is an empty store, not a failure.
    return true;
  }
  if (source == ProtectedFileSource::Error) {
    save_blocked_by_load_failure_ = true;
    return false;
  }
  std::istringstream ifs(text);

  std::string line;
  size_t line_number = 0;
  bool escaped_fields = false;
  while (std::getline(ifs, line)) {
    ++line_number;
    if (line_number == 1 && line == kTypoCorrectionStoreEscapedTsvHeader) {
      escaped_fields = true;
      continue;
    }
    if (line.empty() || line.front() == '#') continue;

    std::istringstream iss(line);
    std::string wrong;
    std::string correct;
    std::string count_timestamp;
    TypoCorrectionRecord rec;
    if (!(std::getline(iss, wrong, '\t') && std::getline(iss, correct, '\t') &&
          std::getline(iss, count_timestamp))) {
      LogMalformedLine(path_, line_number);
      continue;
    }
    if (!ParseRecordValues(count_timestamp, rec)) {
      LogMalformedLine(path_, line_number);
      continue;
    }
    if (escaped_fields) {
      wrong = UnescapeTsvField(wrong);
      correct = UnescapeTsvField(correct);
    }
    // A row that no longer passes the accept filters (written by an older build,
    // or hand-edited) must not become a correction nobody can explain.
    if (!IsLearnablePair(wrong, correct)) {
      LogMalformedLine(path_, line_number);
      continue;
    }
    table_[wrong][correct] = rec;
  }
  if (source == ProtectedFileSource::Plaintext &&
      !MigratePlaintextFile(path_, text, *crypto_)) {
    save_blocked_by_load_failure_ = true;
    SecureErase(text);
    return false;
  }
  SecureErase(text);
  return true;
}

bool TypoCorrectionStore::Save() const {
  if (save_blocked_by_load_failure_) return false;
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << kTypoCorrectionStoreEscapedTsvHeader << '\n';
  for (const auto& [wrong, corrections] : table_) {
    for (const auto& [correct, record] : corrections) {
      out << EscapeTsvField(wrong) << '\t' << EscapeTsvField(correct) << '\t' << record.count << ' '
          << record.last_updated_epoch_sec << '\n';
    }
  }
  return WriteProtectedText(path_, out.str(), *crypto_);
}

void TypoCorrectionStore::Reset() { table_.clear(); }

size_t TypoCorrectionStore::size() const {
  size_t count = 0;
  for (const auto& [_, corrections] : table_) count += corrections.size();
  return count;
}

std::vector<TypoCorrectionEntry> TypoCorrectionStore::All() const {
  std::vector<TypoCorrectionEntry> entries;
  entries.reserve(size());
  for (const auto& [wrong, corrections] : table_) {
    for (const auto& [correct, record] : corrections) {
      entries.push_back(TypoCorrectionEntry{wrong, correct, record});
    }
  }
  return entries;
}

}  // namespace azookey::learning
