#pragma once

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

}  // namespace azookey::learning
