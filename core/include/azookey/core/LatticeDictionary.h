#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "azookey/core/DoubleArrayTrie.h"
#include "azookey/core/Lattice.h"
#include "azookey/core/LatticeConnection.h"

namespace azookey::core {

class StaticLatticeVocabulary;

// Part of speech of a word without connection ids (lattice-conversion-spec section 8.3).
struct LatticePosInfo {
  uint16_t pos{kNoLatticePos};
  LatticeWordClass word_class{LatticeWordClass::Independent};
  uint16_t left_id{};
  uint16_t right_id{};
};

// True when `surface` is made only of the characters of azdic spec section 15.5 bit 14.
bool IsLatticeKatakana(std::string_view surface) noexcept;

// lattice_core.azdic (spec section 8.2): its kana-only words, the connection matrix, and
// META.lattice. Load checks everything once; a failure leaves no lattice (spec section 3.6).
class LatticeDictionary {
 public:
  LatticeDictionary();
  ~LatticeDictionary();
  LatticeDictionary(const LatticeDictionary&) = delete;
  LatticeDictionary& operator=(const LatticeDictionary&) = delete;

  // Loads and checks lattice_core: the trie checks (1)-(3) (`verify` adds (3)), check (4) of
  // CMAT and of its own CONN, and the META.lattice keys of azdic spec section 15.12.
  bool Load(const std::filesystem::path& path, bool verify = false) noexcept;
  bool IsAvailable() const noexcept { return available_; }
  // Why the dictionary is unavailable. Holds no reading or surface.
  std::string_view Error() const noexcept { return error_; }

  const LatticeParameters& Parameters() const noexcept { return parameters_; }
  const ConnectionMatrix& Matrix() const noexcept { return matrix_; }
  // C, the scale of path costs (spec section 3.4).
  int32_t ScoreScale() const noexcept { return score_scale_; }
  // Word cost of mutable-layer words (spec section 5.3).
  int16_t DefaultWordCost() const noexcept { return default_word_cost_; }
  std::string_view ConnectionSpace() const noexcept { return connection_space_; }
  // META.lattice.pos_names: the part-of-speech index of LatticeEntry and LatticeWord.
  const std::vector<std::string>& PosNames() const noexcept { return pos_names_; }
  // The kana-only words of lattice_core itself.
  const ILatticeVocabulary& Vocabulary() const noexcept;

  // Index, class and representative ids of a UniDic part-of-speech name such as
  // "名詞-普通名詞-一般" (spec section 8.3). Names without representative ids get those of
  // 名詞-普通名詞-一般; names outside pos_names have no index.
  LatticePosInfo ResolvePosName(std::string_view name) const;
  // A mutable-layer word (spec sections 5.3 and 8.3): no part of speech, the ids of
  // 名詞-普通名詞-一般, the default word cost, and the katakana spelling taken from the surface.
  LatticeEntry MutableWord(uint32_t key_bytes, std::string_view surface) const;

 private:
  friend class StaticLatticeVocabulary;
  bool Fail(std::string reason);
  bool ParseLattice(std::string_view metadata);

  DoubleArrayTrie trie_;
  ConnectionMatrix matrix_;
  LatticeParameters parameters_;
  std::vector<std::string> pos_names_;
  std::map<std::string, uint16_t, std::less<>> pos_index_;
  std::vector<std::pair<uint16_t, uint16_t>> pos_name_ids_;  // kNoConnectionIndex pair: null
  std::pair<uint16_t, uint16_t> noun_ids_{};
  int32_t score_scale_{};
  int16_t default_word_cost_{};
  std::string connection_space_;
  // Set once the matrix and META.lattice are read, before lattice_core's own words are.
  bool tables_ready_{};
  bool available_{};
  std::string error_{"not loaded"};
  std::unique_ptr<StaticLatticeVocabulary> own_;
};

// The words of one static layer for the lattice (spec section 4.2). The constructor runs
// check (4) on the layer's CONN once; when CONN is absent or fails, the layer's words get the
// ids of spec section 8.3, and the layer's own lookups are untouched. The result does not
// change while the object lives. `dictionary` and `layer` must outlive it, and `layer` must not
// be loaded again meanwhile: rebuild the vocabulary after DoubleArrayTrie::Load. Like the trie,
// it is not thread-safe; the owner serializes queries.
class StaticLatticeVocabulary final : public ILatticeVocabulary {
 public:
  StaticLatticeVocabulary(const LatticeDictionary& dictionary, const DoubleArrayTrie& layer);

  // The layer's words carry their CONN variants.
  bool UsesConnections() const noexcept { return uses_connections_; }
  // Why the layer's CONN is not used; empty when it is used or the layer has none.
  std::string_view ConnectionError() const noexcept { return connection_error_; }

  bool CommonPrefix(std::string_view key, std::vector<LatticeEntry>& out) const override;

 private:
  void ReadConnections(const DoubleArrayTrie& layer, std::string_view metadata);

  const LatticeDictionary& dictionary_;
  const DoubleArrayTrie& layer_;
  bool ready_{};
  bool uses_connections_{};
  std::string connection_error_;
  ConnectionVariants variants_;
  std::vector<LatticePosInfo> connection_pos_;  // By META.conn_pos_table index; ids unused.
  std::vector<LatticePosInfo> table_pos_;       // By META.pos_table index.
  mutable std::vector<PrefixMatch> matches_;
  mutable std::vector<StaticEntryRef> refs_;
};

}  // namespace azookey::core
