#include "azookey/core/HomophoneNotes.h"

#include "azookey/core/Utf8.h"

namespace azookey::core {
namespace {
// Section 3.2: no column holds a tab, a line break or another control character.
bool ValidColumn(std::string_view column) {
  if (column.empty()) return false;
  size_t offset = 0;
  char32_t cp{};
  while (offset < column.size()) {
    if (!DecodeNextUtf8(column, offset, cp) || cp < 0x20 || cp == 0x7f) return false;
  }
  return true;
}
}  // namespace

size_t HomophoneNoteIndex::Parse(std::string_view tsv) {
  notes_.clear();
  size_ = 0;
  size_t invalid = 0;
  while (!tsv.empty()) {
    const auto end = tsv.find('\n');
    const auto line = tsv.substr(0, end);
    tsv.remove_prefix(end == std::string_view::npos ? tsv.size() : end + 1);
    if (line.empty() || line.starts_with('#')) continue;
    const auto first = line.find('\t');
    const auto second = first == std::string_view::npos ? first : line.find('\t', first + 1);
    if (second == std::string_view::npos || line.find('\t', second + 1) != std::string_view::npos) {
      ++invalid;
      continue;
    }
    const auto reading = line.substr(0, first);
    const auto surface = line.substr(first + 1, second - first - 1);
    const auto note = line.substr(second + 1);
    if (!ValidColumn(reading) || !ValidColumn(surface) || !ValidColumn(note)) {
      ++invalid;
      continue;
    }
    auto& surfaces = notes_.try_emplace(std::string(reading)).first->second;
    if (!surfaces.try_emplace(std::string(surface), std::string(note)).second) {
      ++invalid;  // Section 3.2: (reading, surface) does not repeat.
      continue;
    }
    ++size_;
  }
  return invalid;
}

std::string_view HomophoneNoteIndex::Find(std::string_view reading,
                                          std::string_view surface) const {
  const auto by_reading = notes_.find(reading);
  if (by_reading == notes_.end()) return {};
  const auto by_surface = by_reading->second.find(surface);
  return by_surface == by_reading->second.end() ? std::string_view{} : by_surface->second;
}

std::vector<std::string_view> SelectHomophoneNotes(const HomophoneNoteIndex& index,
                                                   std::span<const HomophoneNoteQuery> candidates) {
  std::vector<std::string_view> notes(candidates.size());
  std::map<std::string_view, size_t> listed;  // reading -> candidates in the data
  for (size_t i = 0; i < candidates.size(); ++i) {
    notes[i] = index.Find(candidates[i].reading, candidates[i].surface);
    if (!notes[i].empty()) ++listed[candidates[i].reading];
  }
  for (size_t i = 0; i < candidates.size(); ++i) {
    if (notes[i].empty()) continue;
    // Section 4 items 2 and 3: another listed candidate of the same reading,
    // and no description from a rewriter.
    if (listed[candidates[i].reading] < 2 || !candidates[i].description.empty()) notes[i] = {};
  }
  return notes;
}

}  // namespace azookey::core
