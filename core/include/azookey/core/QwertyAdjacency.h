#pragma once

namespace azookey::core {

// The letter block of a QWERTY keyboard (docs/typo-correction-learning-spec.md
// sections 12.4.2 and 12.4.6). JIS and US layouts place 'a'-'z' identically,
// so one table serves keyboardLayout "auto". Characters other than 'a'-'z' are
// never adjacent and never share a finger.

// Keys next to each other on the same row, or in the same touch-typing column
// on the rows directly above and below (q-a-z, w-s-x, ..., u-j-m, i-k, o-l).
// 'u' is adjacent to 'y', 'i' and 'j' but not to 'h'.
bool AreQwertyAdjacent(char a, char b);

// Keys typed by the same finger in standard touch typing: q a z / w s x /
// e d c / r f v t g b / y h n u j m / i k / o l / p.
bool AreQwertySameFinger(char a, char b);

// Section 12.4.6 initial costs, in tenths so that sums stay exact: 10 for a
// substitution, 7 when the keys are adjacent, 6 when the same finger types
// them. A pair that is both takes the smaller cost; the bonuses do not add up.
inline constexpr int kQwertyEditCostScale = 10;
inline constexpr int kQwertyBaseEditCost = 10;  // substitution, insertion, deletion
inline constexpr int kQwertyAdjacentSubstitutionCost = 7;
inline constexpr int kQwertySameFingerSubstitutionCost = 6;
inline constexpr int kQwertyTranspositionCost = 5;

// Cost of typing `typed` where `intended` was meant; 0 when they are equal.
int QwertySubstitutionCost(char typed, char intended);

}  // namespace azookey::core
