#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace azookey::learning {

// The part-of-speech ids a user dictionary word can carry
// (docs/user-dictionary-pos-spec.md section 2.1), in the order of that table.
// The noun has no id: a word without one is a noun.
const std::vector<std::string_view>& UserDictionaryPosIds();

bool IsKnownUserDictionaryPos(std::string_view pos);

// Whether the id names a conjugating part of speech (section 4.1).
bool IsConjugatingUserDictionaryPos(std::string_view pos);

enum class UserWordPosCheck {
  Ok,
  // Not an id of the table (section 2.1).
  UnknownPos,
  // A conjugating id on a word that does not meet section 4.4.
  PosMismatch,
};

// Whether `pos` may be given to the word (sections 2.1 and 4.4). The reading
// must end with the dictionary form's ending of the id, with at least one
// character before it, and the word must end with the same hiragana.
UserWordPosCheck CheckUserWordPos(std::string_view word, std::string_view ruby,
                                  std::string_view pos);

// What a user dictionary word's registered form looks up in lattice_core's part-of-speech table
// (sections 2.1, 4.3 and 4.5).
struct UserWordLatticePos {
  // 品詞 1〜4 of the section 2.1 table without "*", joined by "-" as META.lattice.pos_names
  // holds them ("動詞-一般").
  std::string_view name;
  // 活用型 ("五段-ラ行", "下一段-バ行", "サ行変格", "形容詞"), or "*" for an id that does not
  // conjugate.
  std::string_view conjugation_type;
  // 活用形 of the registered form: "終止形-一般", or "*" for an id that does not conjugate.
  std::string_view conjugation_form;
  bool conjugating{};
};

// nullopt for a noun: no id, an id the table does not hold, or a conjugating id on a word that
// does not meet section 4.4 (whose 活用型 is not known).
std::optional<UserWordLatticePos> UserWordLatticePosOf(std::string_view word, std::string_view ruby,
                                                       std::string_view pos);

// One form of the section 4.2 table.
struct UserWordForm {
  std::string reading;                // The registered reading's stem and the ending.
  std::string surface;                // The word's stem and the same ending (section 4.4).
  std::string_view conjugation_form;  // 活用形, such as "連用形-促音便".
};

// The forms section 4.2 makes of a word, in the order of its table, without the registered
// (terminal) form. None unless `pos` conjugates and the word meets section 4.4. Whether
// lattice_core has a row for each form is the caller's to check (section 4.5).
std::vector<UserWordForm> ExpandUserWord(std::string_view word, std::string_view ruby,
                                         std::string_view pos);

}  // namespace azookey::learning
