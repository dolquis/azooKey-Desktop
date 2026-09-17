#include "azookey/learning/AutoWordStore.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <locale>
#include <sstream>

#include "azookey/learning/AtomicFile.h"

namespace azookey::learning {

namespace {

constexpr std::string_view kColumnHeader =
    "# surface\treading\tsource\tstate\tcount\tfirst_seen_epoch\tlast_seen_epoch\tscore";

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
        // Load() skips a line that starts with '#' as a comment, so a surface
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

bool ParseScore(std::string_view token, double& value) {
  if (token.empty()) return false;
  double parsed = 0.0;
  const char* const first = token.data();
  const char* const last = first + token.size();
  const auto result = std::from_chars(first, last, parsed);
  if (result.ec != std::errc{} || result.ptr != last || !std::isfinite(parsed)) return false;
  value = parsed;
  return true;
}

void LogMalformedLine(const std::filesystem::path& path, size_t line_number) {
  const auto utf8_path = path.u8string();
  const std::string display_path(reinterpret_cast<const char*>(utf8_path.data()), utf8_path.size());
  std::cerr << "AutoWordStore: skipped malformed record in " << display_path << ":" << line_number
            << '\n';
}

}  // namespace

std::string_view AutoWordSourceName(AutoWordSource source) {
  return source == AutoWordSource::Trending ? "trending" : "mining";
}

std::string_view AutoWordStateName(AutoWordState state) {
  switch (state) {
    case AutoWordState::Confirmed:
      return "confirmed";
    case AutoWordState::Rejected:
      return "rejected";
    case AutoWordState::Pending:
      break;
  }
  return "pending";
}

bool ParseAutoWordSource(std::string_view value, AutoWordSource& out) {
  if (value == "mining") {
    out = AutoWordSource::Mining;
    return true;
  }
  if (value == "trending") {
    out = AutoWordSource::Trending;
    return true;
  }
  return false;
}

bool ParseAutoWordState(std::string_view value, AutoWordState& out) {
  if (value == "pending") {
    out = AutoWordState::Pending;
    return true;
  }
  if (value == "confirmed") {
    out = AutoWordState::Confirmed;
    return true;
  }
  if (value == "rejected") {
    out = AutoWordState::Rejected;
    return true;
  }
  return false;
}

AutoWordStore::AutoWordStore(std::filesystem::path path) : path_(std::move(path)) {}

AutoWord* AutoWordStore::FindLocked(const std::string& surface, const std::string& reading) {
  const auto by_reading = table_.find(reading);
  if (by_reading == table_.end()) return nullptr;
  const auto entry = by_reading->second.find(surface);
  if (entry == by_reading->second.end()) return nullptr;
  return &entry->second;
}

bool AutoWordStore::Observe(const std::string& surface, const std::string& reading,
                            uint64_t now_epoch, uint32_t promote_threshold, bool auto_promote) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (auto* existing = FindLocked(surface, reading)) {
    // A rejected word stays rejected: not even its count moves, so it cannot
    // creep back above the promotion threshold.
    if (existing->state == AutoWordState::Rejected) return false;
    if (existing->count < (std::numeric_limits<uint32_t>::max)()) ++existing->count;
    existing->last_seen_epoch = now_epoch;
    if (auto_promote && existing->state == AutoWordState::Pending &&
        existing->count >= promote_threshold) {
      existing->state = AutoWordState::Confirmed;
      return true;
    }
    return false;
  }

  AutoWord word;
  word.surface = surface;
  word.reading = reading;
  word.source = AutoWordSource::Mining;
  word.state = AutoWordState::Pending;
  word.count = 1;
  word.first_seen_epoch = now_epoch;
  word.last_seen_epoch = now_epoch;
  // A threshold of 1 means the very first sighting is enough to register.
  const bool promoted = auto_promote && word.count >= promote_threshold;
  if (promoted) word.state = AutoWordState::Confirmed;
  table_[reading].emplace(surface, std::move(word));
  return promoted;
}

void AutoWordStore::IngestTrending(const std::vector<AutoWord>& batch, uint64_t now_epoch,
                                   bool auto_promote) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& incoming : batch) {
    // A trending entry without a reading cannot be looked up later.
    if (incoming.surface.empty() || incoming.reading.empty()) continue;

    if (auto* existing = FindLocked(incoming.surface, incoming.reading)) {
      if (existing->state == AutoWordState::Rejected) continue;
      // Source stays Mining when the user has typed the word themselves: local
      // evidence outranks the remote list.
      existing->last_seen_epoch = now_epoch;
      existing->score = std::max(existing->score, incoming.score);
      if (auto_promote && existing->state == AutoWordState::Pending) {
        existing->state = AutoWordState::Confirmed;
      }
      continue;
    }

    AutoWord word = incoming;
    word.source = AutoWordSource::Trending;
    word.state = auto_promote ? AutoWordState::Confirmed : AutoWordState::Pending;
    word.first_seen_epoch = now_epoch;
    word.last_seen_epoch = now_epoch;
    table_[word.reading].emplace(word.surface, std::move(word));
  }
}

std::vector<AutoWord> AutoWordStore::ListByState(AutoWordState state) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<AutoWord> result;
  for (const auto& [reading, surfaces] : table_) {
    for (const auto& [surface, word] : surfaces) {
      if (word.state == state) result.push_back(word);
    }
  }
  return result;
}

bool AutoWordStore::SetStateLocked(const std::string& surface, const std::string& reading,
                                   AutoWordState state) {
  auto* word = FindLocked(surface, reading);
  if (!word) return false;
  if (word->state == state) return false;
  word->state = state;
  return true;
}

bool AutoWordStore::Confirm(const std::string& surface, const std::string& reading) {
  std::lock_guard<std::mutex> lock(mutex_);
  return SetStateLocked(surface, reading, AutoWordState::Confirmed);
}

bool AutoWordStore::Reject(const std::string& surface, const std::string& reading) {
  std::lock_guard<std::mutex> lock(mutex_);
  // Rejected entries are kept, not erased: the record is what stops the word
  // from being mined again.
  return SetStateLocked(surface, reading, AutoWordState::Rejected);
}

std::vector<AutoWord> AutoWordStore::LookupConfirmed(const std::string& reading) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<AutoWord> result;
  const auto by_reading = table_.find(reading);
  if (by_reading == table_.end()) return result;
  for (const auto& [surface, word] : by_reading->second) {
    if (word.state == AutoWordState::Confirmed) result.push_back(word);
  }
  return result;
}

size_t AutoWordStore::PrunePending(uint64_t now_epoch, uint64_t max_age_sec) {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t removed = 0;
  for (auto reading = table_.begin(); reading != table_.end();) {
    auto& surfaces = reading->second;
    for (auto surface = surfaces.begin(); surface != surfaces.end();) {
      const AutoWord& word = surface->second;
      // A last_seen in the future (clock change) must not read as an enormous
      // age and sweep a word the user just typed.
      const bool expired = word.state == AutoWordState::Pending &&
                           now_epoch > word.last_seen_epoch &&
                           now_epoch - word.last_seen_epoch > max_age_sec;
      if (expired) {
        surface = surfaces.erase(surface);
        ++removed;
      } else {
        ++surface;
      }
    }
    reading = surfaces.empty() ? table_.erase(reading) : std::next(reading);
  }
  return removed;
}

size_t AutoWordStore::Size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t count = 0;
  for (const auto& [reading, surfaces] : table_) count += surfaces.size();
  return count;
}

void AutoWordStore::Reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  table_.clear();
}

bool AutoWordStore::Load() {
  std::lock_guard<std::mutex> lock(mutex_);
  table_.clear();
  std::ifstream ifs(path_);
  if (!ifs.is_open()) {
    // No file yet is an empty store, matching UserDictionary::Load.
    return true;
  }

  std::string line;
  size_t line_number = 0;
  while (std::getline(ifs, line)) {
    ++line_number;
    if (line.empty() || line.front() == '#') continue;

    std::istringstream iss(line);
    std::string fields[8];
    bool complete = true;
    for (size_t i = 0; i < 8 && complete; ++i) {
      // The last column runs to end of line; the rest are tab-delimited.
      complete = static_cast<bool>(i + 1 == 8 ? std::getline(iss, fields[i])
                                              : std::getline(iss, fields[i], '\t'));
    }
    if (!complete) {
      LogMalformedLine(path_, line_number);
      continue;
    }

    AutoWord word;
    word.surface = UnescapeTsvField(fields[0]);
    word.reading = UnescapeTsvField(fields[1]);
    if (word.surface.empty() || word.reading.empty() ||
        !ParseAutoWordSource(fields[2], word.source) || !ParseAutoWordState(fields[3], word.state) ||
        !ParseUnsigned(fields[4], word.count) ||
        !ParseUnsigned(fields[5], word.first_seen_epoch) ||
        !ParseUnsigned(fields[6], word.last_seen_epoch) || !ParseScore(fields[7], word.score)) {
      LogMalformedLine(path_, line_number);
      continue;
    }
    table_[word.reading].emplace(word.surface, std::move(word));
  }
  return true;
}

bool AutoWordStore::Save() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << kAutoWordStoreTsvHeader << '\n';
  // Written as a comment so the column order is documented in the file without
  // the reader having to tell a header row apart from a record.
  out << kColumnHeader << '\n';
  for (const auto& [reading, surfaces] : table_) {
    for (const auto& [surface, word] : surfaces) {
      out << EscapeTsvField(word.surface) << '\t' << EscapeTsvField(word.reading) << '\t'
          << AutoWordSourceName(word.source) << '\t' << AutoWordStateName(word.state) << '\t'
          << word.count << '\t' << word.first_seen_epoch << '\t' << word.last_seen_epoch << '\t'
          << word.score << '\n';
    }
  }
  return WriteTextFileAtomically(path_, out.str());
}

}  // namespace azookey::learning
