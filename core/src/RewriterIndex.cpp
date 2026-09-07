#include "azookey/core/RewriterIndex.h"

#include <algorithm>
#include <charconv>
#include <optional>
#include <set>
#include <tuple>
#include <utility>

#include "azookey/core/Utf8.h"

namespace azookey::core {
namespace {
std::vector<std::string_view> Split(std::string_view input, char delimiter) {
  std::vector<std::string_view> result;
  for (;;) {
    const auto pos = input.find(delimiter);
    result.push_back(input.substr(0, pos));
    if (pos == std::string_view::npos) return result;
    input.remove_prefix(pos + 1);
  }
}
bool ValidText(std::string_view input) {
  size_t offset = 0;
  char32_t cp{};
  while (offset < input.size()) {
    if (!DecodeNextUtf8(input, offset, cp) || cp < 0x20 || cp == 0x7f) return false;
  }
  return true;
}
bool ValidKeys(std::string_view column, bool trigger) {
  if (column.empty()) return true;
  std::set<std::string_view> seen;
  for (const auto key : Split(column, '|')) {
    if (key.empty() || !seen.insert(key).second || !ValidText(key)) return false;
    if (trigger ? NormalizeEmojiTrigger(key) != key : NormalizeRewriterReading(key) != key)
      return false;
  }
  return true;
}
size_t Codepoints(std::string_view input) {
  size_t offset = 0, count = 0;
  char32_t cp{};
  while (offset < input.size()) {
    if (!DecodeNextUtf8(input, offset, cp)) return 33;
    ++count;
  }
  return count;
}
int Match(std::string_view query, std::string_view key) {
  if (query == key) return 0;
  if (key.starts_with(query)) return 1;
  if (query.size() == 1) return -1;
  size_t next = 0;
  for (char ch : key) {
    if (ch == query[next] && ++next == query.size()) return 2;
  }
  return -1;
}
}  // namespace

size_t RewriterIndex::Parse(std::string_view tsv) {
  if (tsv.starts_with("\xef\xbb\xbf")) tsv.remove_prefix(3);
  entries_.clear();
  readings_.clear();
  size_t invalid = 0;
  std::set<std::string_view> surfaces;
  const bool emoji = source_ == CandidateSource::Emoji;
  for (auto line : Split(tsv, '\n')) {
    if (line.ends_with('\r')) line.remove_suffix(1);
    if (line.empty() || line.starts_with('#')) continue;
    const auto columns = Split(line, '\t');
    if (columns.size() != (emoji ? 5u : 4u)) {
      ++invalid;
      continue;
    }
    const auto surface = columns[0];
    const auto reading = columns[1];
    const auto triggers = emoji ? columns[2] : std::string_view{};
    const auto name = columns[emoji ? 3 : 2];
    const auto rank_text = columns.back();
    uint32_t rank{};
    const auto parsed =
        std::from_chars(rank_text.data(), rank_text.data() + rank_text.size(), rank);
    if (surface.empty() || name.empty() || !ValidText(surface) || !ValidText(name) ||
        surface.find('|') != std::string_view::npos || name.find('|') != std::string_view::npos ||
        (reading.empty() && triggers.empty()) || !ValidKeys(reading, false) ||
        !ValidKeys(triggers, true) || rank_text.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != rank_text.data() + rank_text.size() || !surfaces.insert(surface).second) {
      ++invalid;
      continue;
    }
    const auto index = entries_.size();
    Entry entry{std::string(surface), std::string(name), rank, {}};
    if (!triggers.empty())
      for (const auto key : Split(triggers, '|')) entry.triggers.emplace_back(key);
    entries_.push_back(std::move(entry));
    if (!reading.empty())
      for (const auto key : Split(reading, '|')) readings_[std::string(key)].push_back(index);
  }
  for (auto& [key, indices] : readings_) {
    (void)key;
    std::sort(indices.begin(), indices.end(), [&](size_t left, size_t right) {
      const auto& a = entries_[left];
      const auto& b = entries_[right];
      return a.rank != b.rank ? a.rank > b.rank : a.surface < b.surface;
    });
  }
  return invalid;
}

Candidate RewriterIndex::ToCandidate(size_t index, std::string_view reading) const {
  const auto& entry = entries_[index];
  Candidate candidate;
  candidate.surface = entry.surface;
  candidate.reading = reading;
  candidate.source = source_;
  candidate.description = entry.name;
  candidate.debug_info =
      source_ == CandidateSource::Emoji ? "emoji-rewriter:lookup" : "symbol-rewriter:lookup";
  return candidate;
}

std::vector<Candidate> RewriterIndex::LookupReading(std::string_view reading) const {
  if (Codepoints(reading) > 32) return {};
  const auto normalized = NormalizeRewriterReading(reading);
  if (normalized.empty() || Codepoints(normalized) > 32) return {};
  const auto found = readings_.find(normalized);
  if (found == readings_.end()) return {};
  std::vector<Candidate> result;
  for (const auto index : found->second) {
    result.push_back(ToCandidate(index, reading));
    if (result.size() == 4) break;
  }
  return result;
}

std::vector<Candidate> RewriterIndex::SearchTrigger(std::string_view query, size_t limit) const {
  if (source_ != CandidateSource::Emoji || query.size() > 32) return {};
  const auto normalized = NormalizeEmojiTrigger(query);
  if (normalized.empty() || normalized.size() > 32) return {};
  using Key = std::tuple<int, size_t, uint32_t, std::string_view, std::string_view>;
  std::vector<std::pair<Key, size_t>> matches;
  for (size_t index = 0; index < entries_.size(); ++index) {
    const auto& entry = entries_[index];
    std::optional<Key> best;
    for (const auto& trigger : entry.triggers) {
      const int match = Match(normalized, trigger);
      if (match < 0) continue;
      Key key{match, trigger.size(), UINT32_MAX - entry.rank, trigger, entry.surface};
      if (!best || key < *best) best = key;
    }
    if (best) matches.emplace_back(*best, index);
  }
  std::sort(matches.begin(), matches.end());
  std::vector<Candidate> result;
  for (const auto& match : matches) {
    result.push_back(ToCandidate(match.second, {}));
    if (limit != 0 && result.size() == limit) break;
  }
  return result;
}

size_t RewriterIndex::EstimatedMemoryBytes() const {
  size_t bytes = sizeof(*this) + entries_.capacity() * sizeof(Entry);
  for (const auto& entry : entries_) {
    bytes += entry.surface.capacity() + entry.name.capacity() + 2;
    bytes += entry.triggers.capacity() * sizeof(std::string);
    for (const auto& trigger : entry.triggers) bytes += trigger.capacity() + 1;
  }
  for (const auto& [key, indices] : readings_)
    bytes += sizeof(decltype(readings_)::value_type) + 4 * sizeof(void*) + key.capacity() + 1 +
             indices.capacity() * sizeof(size_t);
  return bytes;
}

}  // namespace azookey::core
