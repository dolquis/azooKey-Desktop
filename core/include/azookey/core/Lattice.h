#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "azookey/core/LatticeConnection.h"

// Lattice for kana-kanji conversion without the model (docs/lattice-conversion-spec.md).
// Building and searching depend on no other layer; the engine connects DictionaryStore and
// lattice_core to ILatticeVocabulary (spec section 3.1).
namespace azookey::core {

// Classes that group the words of a path into segments (spec section 7.1).
enum class LatticeWordClass : uint8_t {
  Independent,  // A word that starts a segment, including words without a part of speech.
  Function,     // 品詞 1 is 助詞, 助動詞 or 接尾辞, or 品詞 2 is 非自立可能.
  Prefix,       // 品詞 1 is 接頭辞: joins the following word's segment.
  Unknown,      // Unknown-word node (spec section 4.3). Vocabularies do not return it.
};

// Spelling that selects the katakana penalty (spec section 5.2).
enum class LatticeScript : uint8_t {
  Other,
  KatakanaVariant,  // Katakana only, and the upstream normalized form is spelled otherwise.
  KatakanaOwn,      // Katakana only, and the word is spelled that way (a loanword).
};

// No part of speech in lattice_core's pos_names (META.lattice).
inline constexpr uint16_t kNoLatticePos = 0xffff;

// One word a vocabulary matched. Ids are upstream connection ids; pos indexes pos_names.
struct LatticeEntry {
  uint32_t key_bytes{};      // Bytes of the query its reading covers, ending on a code point.
  std::string_view surface;  // Valid until the vocabulary changes; checked as UTF-8 on output.
  uint16_t left_id{};
  uint16_t right_id{};
  int16_t word_cost{};
  uint16_t pos{kNoLatticePos};
  LatticeWordClass word_class{LatticeWordClass::Independent};
  LatticeScript script{LatticeScript::Other};
  // A conjugating user dictionary word, registered or conjugated (user-dictionary-pos-spec
  // section 5.2). Only a marked_only search (LatticeSearch.h) reads it.
  bool marked{};
};

// Dictionary words for the lattice (spec section 3.1).
class ILatticeVocabulary {
 public:
  virtual ~ILatticeVocabulary() = default;
  // Appends the words whose reading is a prefix of `key` (normalized UTF-8). Returns false when
  // the vocabulary failed on a corrupt reference; the request then gets no paths (spec 3.6).
  virtual bool CommonPrefix(std::string_view key, std::vector<LatticeEntry>& out) const = 0;
};

// Correction terms of META.lattice.penalties (spec section 5.2).
struct LatticePenalties {
  int32_t katakana_variant{};
  int32_t katakana_own{};
  int32_t node{};
  int32_t unknown{};
};

// What building needs from lattice_core. LatticeDictionary::Parameters() supplies it.
struct LatticeParameters {
  const ConnectionMatrix* matrix{};
  LatticePenalties penalties;
  // Representative ids of 名詞-普通名詞-一般, given to unknown-word nodes (spec section 4.3).
  uint16_t unknown_left_id{};
  uint16_t unknown_right_id{};
};

inline constexpr uint32_t kLatticeUnknownSource = UINT32_MAX;

struct LatticeNode {
  uint32_t begin{};  // Code point position in the reading.
  uint32_t end{};
  std::string_view surface;  // Borrowed from the vocabulary, or from the reading.
  int32_t cost{};            // Word cost plus penalties.
  uint16_t left_id{};
  uint16_t right_id{};
  uint16_t left_index{};  // CMAT indices of the ids.
  uint16_t right_index{};
  uint16_t pos{kNoLatticePos};
  LatticeWordClass word_class{LatticeWordClass::Independent};
  uint32_t source{};  // Vocabulary index, or kLatticeUnknownSource.
  bool marked{};      // LatticeEntry::marked.
  // In a lattice made by WithMarkBit: whether the paths through this copy of the node hold a
  // marked node up to and including it. 0 in a built lattice.
  uint8_t bit{};
};

// Nodes and the per-position indices of spec section 4.5. Searching only reads it.
class Lattice {
 public:
  uint32_t Length() const noexcept { return length_; }
  std::string_view Reading() const noexcept { return reading_; }
  // The matrix the node indices refer to; null for a lattice that was not built.
  const ConnectionMatrix* Matrix() const noexcept { return matrix_; }
  const std::vector<LatticeNode>& Nodes() const noexcept { return nodes_; }
  // Indices of the nodes beginning (ending) at code point position `position`, ascending.
  std::span<const uint32_t> BeginningAt(uint32_t position) const noexcept;
  std::span<const uint32_t> EndingAt(uint32_t position) const noexcept;

 private:
  friend class LatticeBuilder;
  std::string_view reading_;
  const ConnectionMatrix* matrix_{};
  uint32_t length_{};
  std::vector<LatticeNode> nodes_;
  std::vector<uint32_t> begin_offsets_, end_offsets_, by_begin_, by_end_;
};

enum class LatticeStage : uint8_t {
  Built,    // The lattice is built.
  Forward,  // The forward Viterbi pass is done.
  Settled,  // The backward search settled its first path.
};

// Stops of spec section 6.4. on_stage runs after each stage, so tests can raise cancel there
// without depending on time.
struct LatticeLimits {
  const std::atomic<bool>* cancel{nullptr};
  std::optional<std::chrono::steady_clock::time_point> deadline;
  std::function<void(LatticeStage)> on_stage;

  bool Stopped() const;
};

enum class LatticeStatus : uint8_t {
  Complete,     // Up to N paths.
  Truncated,    // Cancel or deadline after the forward pass: the best path only (spec 6.4).
  Aborted,      // Cancel or deadline before the forward pass ended: no paths.
  Rejected,     // Empty, invalid or over-long reading, or invalid constraints: no paths.
  Unavailable,  // lattice_core is not loaded: no paths.
  Failed,       // A vocabulary failed, or an output surface is not UTF-8: no paths (spec 3.6).
};

// Readings longer than this get no lattice (spec section 3.6).
inline constexpr uint32_t kMaxLatticeReading = 64;

struct LatticeBuildResult {
  LatticeStatus status{LatticeStatus::Complete};
  // Entries dropped because their reading length or ids do not fit the lattice.
  uint32_t rejected_entries{};
};

// Builds the lattice of a normalized reading (spec section 4). `reading`, the vocabularies'
// words and `parameters.matrix` must outlive `out`.
LatticeBuildResult BuildLattice(std::string_view reading, const LatticeParameters& parameters,
                                std::span<const ILatticeVocabulary* const> vocabularies,
                                const LatticeLimits& limits, Lattice& out);

// The lattice a marked_only search walks (user-dictionary-pos-spec section 5.2): a node that is
// not marked appears with bit 0, and again with bit 1 when a marked node ends at or before its
// beginning; a marked node appears once, with bit 1. Copies keep the order of `lattice`'s nodes,
// which must outlive the result along with what they borrow.
Lattice WithMarkBit(const Lattice& lattice);

}  // namespace azookey::core
