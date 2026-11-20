#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "azookey/core/Lattice.h"

namespace azookey::core {

class LatticeDictionary;

// Segment-boundary constraints (lattice-conversion-spec section 7.3), as code point positions
// 1..n-1 of the reading. A position outside that range rejects the whole search.
struct LatticeConstraints {
  std::vector<uint32_t> segment_breaks;  // A segment boundary must fall here.
  std::vector<uint32_t> segment_joins;   // No segment boundary may fall here.
  // A word boundary must fall here, a segment boundary or not (M66). It only drops the nodes
  // that span the position and never rules a transition out.
  std::vector<uint32_t> word_boundaries;

  bool Empty() const noexcept {
    return segment_breaks.empty() && segment_joins.empty() && word_boundaries.empty();
  }
};

struct LatticeWord {
  uint32_t begin{};  // Code point positions of the reading the word covers.
  uint32_t end{};
  std::string surface;
  uint16_t left_id{};
  uint16_t right_id{};
  uint16_t pos{kNoLatticePos};  // Index of lattice_core's pos_names.
  LatticeWordClass word_class{LatticeWordClass::Independent};
};

// No independent word in the segment (spec section 7.1): head_pos maps to Unknown (7.4).
inline constexpr uint32_t kNoLatticeHead = UINT32_MAX;

struct LatticeSegment {
  uint32_t begin{};  // Code point positions of the reading.
  uint32_t end{};
  uint32_t first_word{};  // Words [first_word, first_word + word_count) of the path.
  uint32_t word_count{};
  uint32_t head{kNoLatticeHead};  // Path word index of the segment's first independent word.
};

// The internal result of one path (spec section 7.2). Candidates are made from it by the engine.
struct LatticePath {
  int32_t cost{};
  std::vector<LatticeWord> words;
  std::vector<LatticeSegment> segments;

  std::string Surface() const;
};

struct LatticeQuery {
  size_t max_paths{8};
  LatticeConstraints constraints;
  LatticeLimits limits;
  // Candidates of the head [0, p) of the reading (spec section 7.3, M66). The constraints must
  // hold exactly one word boundary p and nothing else; the search is rejected otherwise. Each
  // path then holds the words before p only, and its cost adds the smallest continuation from
  // p to the sentence end.
  bool head_only{};
  // Only paths through a marked node (LatticeEntry::marked), searched with the bit of
  // user-dictionary-pos-spec section 5.2 in the state; the result's best_cost holds the
  // smallest cost of any path. Constraints and head_only reject such a search.
  bool marked_only{};
};

struct LatticeResult {
  LatticeStatus status{LatticeStatus::Complete};
  // A Complete search stopped at 64 x N complete paths or 64 x N x (n + 1) expansions (n is p
  // for a head_only search).
  bool limited{};
  // Paths in cost order, one per surface (spec sections 6.2 and 6.3).
  std::vector<LatticePath> paths;
  uint32_t rejected_entries{};
  // A marked_only search whose forward pass ended: the smallest cost of any path, marked or not.
  std::optional<int32_t> best_cost;
};

// Forward Viterbi and backward A* over a built lattice, with the matrix it was built with
// (spec sections 6 and 7).
LatticeResult SearchLattice(const Lattice& lattice, const LatticeQuery& query);

// Builds and searches `reading` (normalized, UTF-8) with lattice_core's own words followed by
// `vocabularies`. Returns Unavailable when the dictionary is not loaded.
LatticeResult ConvertLattice(std::string_view reading, const LatticeDictionary& dictionary,
                             std::span<const ILatticeVocabulary* const> vocabularies,
                             const LatticeQuery& query);

}  // namespace azookey::core
