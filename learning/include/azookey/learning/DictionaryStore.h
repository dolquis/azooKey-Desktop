#pragma once

#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "azookey/core/DoubleArrayTrie.h"
#include "azookey/core/LatticeDictionary.h"
#include "azookey/core/LatticeSearch.h"
#include "azookey/learning/UserDictionary.h"

namespace azookey::learning {
enum class LayerId : uint8_t {
  Base,
  Sudachi,
  Neologd,
  NamedEntity,
  TechnicalTerms,
  User,
  AutoWords,
  AppSpecific
};
enum class LookupMode { Exact, CommonPrefix, PredictivePrefix };

struct DictionaryEntry {
  std::string surface;
  std::string reading;
  std::string normalized_reading;
  double frequency{};
  uint16_t category_mask{};
  LayerId source{LayerId::Base};
  uint16_t sources{};
  core::MatchKind kind{core::MatchKind::Exact};
  bool confirmed{true};
  uint64_t last_used{};
  double score{};
  bool use_user_word_score{};
  std::optional<double> user_word_score;
};
struct LookupContext {
  LookupMode mode{LookupMode::Exact};
  size_t max_results{32};
  bool exclude_exact_key{};
  double user_word_default_score{1.5};
  uint64_t now_epoch_sec{};
  uint16_t excluded_layers{};
  // Category ids follow auto-word-registration-spec section 14.4.
  std::map<uint16_t, double> category_boosts;
  double named_entity_boost{1.0};
};

// Caller serializes reads and updates. Mutable persistence remains with its owner.
class DictionaryStore {
 public:
  bool LoadStatic(LayerId layer, const std::filesystem::path& path, bool verify = false);
  void ReplaceMutable(LayerId layer, std::vector<DictionaryEntry> entries);
  void SetUserWords(const std::vector<UserWord>& words);
  void EnableLayer(LayerId layer, bool enabled);
  // Successfully loaded/initialized, independently of enabled state or entry count.
  bool IsAvailable(LayerId layer) const;
  std::string LayerError(LayerId layer) const;
  static double LayerPriority(LayerId layer, bool confirmed = true);
  std::vector<DictionaryEntry> Lookup(std::string_view reading, const LookupContext& context) const;
  // Exact membership for mining: loaded static layers count even when disabled.
  // Reading normalization/aliases/long-vowel fallback match Lookup; mutable switches still apply.
  bool ContainsKnownWord(std::string_view reading, std::string_view surface) const;
  std::optional<DictionaryEntry> ReverseLookup(std::string_view surface,
                                               const LookupContext& context) const;

  // lattice_core.azdic (lattice-conversion-spec section 8.2). It is not a layer: Lookup,
  // ContainsKnownWord and ReverseLookup never see its words. Loading it, or a static layer, again
  // rebuilds the lattice vocabularies of the layers, whose CONN is checked there, once per load.
  bool LoadLatticeCore(const std::filesystem::path& path, bool verify = false);
  bool IsLatticeAvailable() const;
  // Why the lattice is unavailable; empty while it is available. Holds no reading or surface.
  std::string LatticeError() const;
  // Why the lattice does not use a loaded static layer's CONN; empty when it does or there is none.
  std::string LatticeConnectionError(LayerId layer) const;
  // C of lattice-conversion-spec section 3.4; 0 while the lattice is unavailable.
  int32_t LatticeScoreScale() const;
  // Builds and searches `reading` (spec sections 4 and 6) with lattice_core's words, the enabled
  // static layers, and the words of the enabled mutable layers (spec section 5.3). The paths own
  // their strings. Unavailable without a usable lattice_core.
  core::LatticeResult ConvertLattice(std::string_view reading,
                                     const core::LatticeQuery& query) const;

 private:
  using Index = std::map<std::string, std::vector<DictionaryEntry>>;
  void QueryLayer(size_t layer, std::string_view key, const LookupContext& context,
                  std::vector<DictionaryEntry>& out, bool include_disabled_static = false) const;
  // Drops the lattice vocabulary of static layer `index`, which refers to its trie.
  void ResetLatticeLayer(size_t index);
  void BuildLatticeLayer(size_t index);
  std::array<std::unique_ptr<core::DoubleArrayTrie>, 5> static_;
  // Declared after static_ and lattice_: destroyed first, as they refer to both.
  std::unique_ptr<core::LatticeDictionary> lattice_;
  std::array<std::unique_ptr<core::StaticLatticeVocabulary>, 5> lattice_layers_;
  std::array<Index, 3> mutable_;
  std::array<bool, 3> mutable_available_{};
  std::array<std::string, 5> static_errors_;
  std::array<bool, 8> enabled_{{true, true, false, true, true, true, true, true}};
};
}  // namespace azookey::learning
