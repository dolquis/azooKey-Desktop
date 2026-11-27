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
class AutoWordStore;

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
  // A user dictionary word's part-of-speech id (UserDictionaryPos.h); empty for a noun and for
  // the words of the other layers. Only the lattice reads it (user-dictionary-pos-spec 4.5).
  std::string pos;
};

// Most forms the index of conjugated user dictionary words holds (user-dictionary-pos-spec
// section 4.6).
inline constexpr size_t kMaxUserWordForms = 100'000;
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
  // static layers, the words of the enabled mutable layers (spec section 5.3), and while the
  // user dictionary is enabled, its conjugated forms (user-dictionary-pos-spec section 4.6).
  // When supplied and enabled, auto_words contributes confirmed words to this search.
  // The paths own their strings. Unavailable without a usable lattice_core.
  core::LatticeResult ConvertLattice(std::string_view reading,
                                     const core::LatticeQuery& query,
                                     const AutoWordStore* auto_words = nullptr) const;

  // user-dictionary-pos-spec section 4.6: the forms of the user dictionary's conjugating words
  // that lattice_core has rows for, built whenever the user words or lattice_core load. They
  // reach ConvertLattice only: Lookup, ContainsKnownWord and ReverseLookup never see them.
  // True when the user dictionary is enabled, lattice_core is usable, and from some position of
  // the normalized `reading` a form's reading is a prefix of the rest (section 5.2).
  bool HasUserWordForms(std::string_view reading) const;
  size_t UserWordFormCount() const { return user_form_count_; }
  // The last build stopped at the limit (kMaxUserWordForms) and left forms out.
  bool UserWordFormsTruncated() const { return user_forms_truncated_; }
  // Test-only: a smaller limit than kMaxUserWordForms; the index is built again with it.
  void SetUserWordFormLimitForTests(size_t limit);

 private:
  using Index = std::map<std::string, std::vector<DictionaryEntry>>;
  // One conjugated form with the row of lattice_core's part-of-speech table it takes.
  struct FormNode {
    std::string surface;
    uint16_t left_id{};
    uint16_t right_id{};
    uint16_t pos{core::kNoLatticePos};
    core::LatticeWordClass word_class{core::LatticeWordClass::Independent};
  };
  using FormIndex = std::map<std::string, std::vector<FormNode>, std::less<>>;
  class FormVocabulary;  // The lattice vocabulary of user_forms_.
  void RebuildUserWordForms();
  void QueryLayer(size_t layer, std::string_view key, const LookupContext& context,
                  std::vector<DictionaryEntry>& out, bool include_disabled_static = false) const;
  // Drops the lattice vocabulary of static layer `index`, which refers to its trie.
  void ResetLatticeLayer(size_t index);
  void BuildLatticeLayer(size_t index);
  std::array<std::unique_ptr<core::DoubleArrayTrie>, 5> static_;
  std::unique_ptr<core::LatticeDictionary> lattice_;
  // Declared after static_ and lattice_: destroyed first, as they refer to both.
  std::array<std::unique_ptr<core::StaticLatticeVocabulary>, 5> lattice_layers_;
  std::array<Index, 3> mutable_;
  std::array<bool, 3> mutable_available_{};
  // Keyed by the normalized reading of each form; no alias keys (user-dictionary-pos-spec 4.6).
  FormIndex user_forms_;
  size_t user_form_count_{};
  size_t user_form_key_bytes_{};  // The longest key.
  size_t user_form_limit_{kMaxUserWordForms};
  bool user_forms_truncated_{};
  std::array<std::string, 5> static_errors_;
  std::array<bool, 8> enabled_{{true, true, false, true, true, true, true, true}};
};
}  // namespace azookey::learning
