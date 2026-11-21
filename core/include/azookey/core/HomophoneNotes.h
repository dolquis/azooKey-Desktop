#pragma once

#include <cstddef>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace azookey::core {

// docs/homophone-notes-spec.md. No I/O; a Host may share one immutable index.
class HomophoneNoteIndex {
 public:
  // Section 3.2 rows of (reading, surface, note). Rows that break the grammar
  // are skipped, as is a repeated (reading, surface) after its first row; the
  // result is their count (section 7). Replaces previous data.
  size_t Parse(std::string_view tsv);
  // The note of (reading, surface), or empty when the data has none.
  std::string_view Find(std::string_view reading, std::string_view surface) const;
  size_t size() const { return size_; }

 private:
  // reading -> surface -> note
  std::map<std::string, std::map<std::string, std::string, std::less<>>, std::less<>> notes_;
  size_t size_{0};
};

struct HomophoneNoteQuery {
  std::string_view reading;
  std::string_view surface;
  std::string_view description;
};

// Section 4 over one candidate list: the note for each candidate, in its
// order, or empty where one of the three conditions fails. The caller writes
// the non-empty ones into the descriptions; order and count stay as they are.
std::vector<std::string_view> SelectHomophoneNotes(const HomophoneNoteIndex& index,
                                                   std::span<const HomophoneNoteQuery> candidates);

}  // namespace azookey::core
