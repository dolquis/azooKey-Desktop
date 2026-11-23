#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "azookey/core/CustomRomajiLoader.h"

namespace azookey::core {

// docs/typo-correction-learning-spec.md section 12.4.6.
struct FuzzyKanaCandidate {
  std::string kana;
  double cost = 0.0;   // weighted edit distance from the input (section 12.4.6 table)
  std::string romaji;  // the edited romaji that converts to `kana`
};

// The substitutions FuzzyMatch may make.
enum class FuzzySubstitutions {
  All,           // any letter for any other letter
  NearKeysOnly,  // only keys adjacent or typed by the same finger (QwertyAdjacency.h)
};

class RomajiKanaConverter {
 public:
  std::string Feed(char ascii);
  std::string Flush();
  void Reset();
  // Set only at a composition boundary; copies retain their own table snapshot.
  // A null table uses the built-in romaji mappings.
  void SetCustomTable(std::shared_ptr<const CustomRomajiTable> table);
  bool HasCustomTable() const { return static_cast<bool>(custom_table_); }
  // Whether this ASCII character continues a rule or begins one after pending bytes flush.
  bool CanContinueCustomWith(char ascii) const;
  // Whether Feed('\'') ends a pending built-in "n" / "nn" as ん and consumes the
  // apostrophe (Mozc's n' convention, DEV-1748). False with a custom table.
  bool SeparatesNWithApostrophe() const {
    return !custom_table_ && (pending_ == "n" || pending_ == "nn");
  }
  bool HasPending() const { return !pending_.empty(); }
  void PopPendingPreview();
  std::string PreviewPending() const;

  static std::string Preview(const std::string& ascii);
  static std::string Preview(const std::string& ascii,
                             std::shared_ptr<const CustomRomajiTable> table);
  static std::string ConvertForCommit(const std::string& ascii);
  static std::string ConvertForCommit(const std::string& ascii,
                                      std::shared_ptr<const CustomRomajiTable> table);

  static constexpr size_t kMaxAmbiguousDoubleN = 3;
  // Commit readings of `ascii` when each built-in "nn" + vowel/y is read either as
  // ん + な行 (the Feed default, always first) or as ん + あ行/や行. Only the first
  // `max_ambiguous` (at most kMaxAmbiguousDoubleN) such positions vary; later ones keep
  // the default. A position does not vary when reading it alone that way leaves a
  // different count of unconverted ASCII letters than the default (e.g. a trailing
  // "nny" or "nnyi"). A custom table has no ambiguous positions, so it yields its
  // single commit reading.
  static std::vector<std::string> AmbiguousReadings(const std::string& ascii,
                                                    size_t max_ambiguous = kMaxAmbiguousDoubleN);
  static std::vector<std::string> AmbiguousReadings(const std::string& ascii,
                                                    std::shared_ptr<const CustomRomajiTable> table,
                                                    size_t max_ambiguous = kMaxAmbiguousDoubleN);

  static constexpr size_t kFuzzyMaxEdits = 2;
  static constexpr size_t kFuzzyMaxInputLength = 64;
  static constexpr size_t kFuzzyNodeBudget = 20000;
  // Section 12.4.6: up to `k` kana readings of the built-in table that `input`
  // reaches within kFuzzyMaxEdits edits, cheapest first and then by kana. An
  // edit substitutes a letter, inserts a letter or '\'', deletes a key, or swaps
  // two neighbouring keys. Each reading appears once at its lowest cost; the
  // reading ConvertForCommit gives `input` and readings that keep a letter or a
  // symbol are left out. Empty with a custom table, for an empty or longer than
  // kFuzzyMaxInputLength input, or for a key other than 'a'-'z', '-' and '\''.
  // The search stops after kFuzzyNodeBudget states, so a long input may yield
  // fewer readings. The pending keys of this converter are not used.
  // NearKeysOnly leaves out substitutions of keys that are neither adjacent nor
  // typed by the same finger.
  std::vector<FuzzyKanaCandidate> FuzzyMatch(
      std::string_view input, size_t k,
      FuzzySubstitutions substitutions = FuzzySubstitutions::All) const;

 private:
  std::string pending_;
  std::shared_ptr<const CustomRomajiTable> custom_table_;
  // Bit i set: the i-th ambiguous "nn" + vowel/y consumes both n (used by AmbiguousReadings).
  unsigned consume_double_n_mask_ = 0;
  size_t ambiguous_double_n_seen_ = 0;
  std::string ConvertPending(bool force_flush);
  std::string ConvertCustomPending(bool force_flush);
  // Built-in table only: the pending keys can still become kana.
  bool PendingCanContinue() const;
};

}  // namespace azookey::core
