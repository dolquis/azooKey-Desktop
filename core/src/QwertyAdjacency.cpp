#include "azookey/core/QwertyAdjacency.h"

#include <algorithm>
#include <array>
#include <cstdlib>

namespace azookey::core {
namespace {

// Row (0 = top), column within the row, and touch-typing finger (0 = left
// pinky ... 7 = right pinky) of each letter, indexed by letter - 'a'. Columns
// line up the touch-typing columns: q, a and z are column 0; u, j and m are
// column 6.
struct KeyPosition {
  int row;
  int column;
  int finger;
};

constexpr std::array<KeyPosition, 26> kKeys = {{
    {1, 0, 0},  // a
    {2, 4, 3},  // b
    {2, 2, 2},  // c
    {1, 2, 2},  // d
    {0, 2, 2},  // e
    {1, 3, 3},  // f
    {1, 4, 3},  // g
    {1, 5, 4},  // h
    {0, 7, 5},  // i
    {1, 6, 4},  // j
    {1, 7, 5},  // k
    {1, 8, 6},  // l
    {2, 6, 4},  // m
    {2, 5, 4},  // n
    {0, 8, 6},  // o
    {0, 9, 7},  // p
    {0, 0, 0},  // q
    {0, 3, 3},  // r
    {1, 1, 1},  // s
    {0, 4, 3},  // t
    {0, 6, 4},  // u
    {2, 3, 3},  // v
    {0, 1, 1},  // w
    {2, 1, 1},  // x
    {0, 5, 4},  // y
    {2, 0, 0},  // z
}};

bool IsQwertyLetter(char c) { return c >= 'a' && c <= 'z'; }

const KeyPosition& PositionOf(char c) { return kKeys[static_cast<size_t>(c - 'a')]; }

}  // namespace

bool AreQwertyAdjacent(char a, char b) {
  if (a == b || !IsQwertyLetter(a) || !IsQwertyLetter(b)) return false;
  const KeyPosition& left = PositionOf(a);
  const KeyPosition& right = PositionOf(b);
  const int rows = std::abs(left.row - right.row);
  const int columns = std::abs(left.column - right.column);
  return (rows == 0 && columns == 1) || (rows == 1 && columns == 0);
}

bool AreQwertySameFinger(char a, char b) {
  if (a == b || !IsQwertyLetter(a) || !IsQwertyLetter(b)) return false;
  return PositionOf(a).finger == PositionOf(b).finger;
}

int QwertySubstitutionCost(char typed, char intended) {
  if (typed == intended) return 0;
  int cost = kQwertyBaseEditCost;
  if (AreQwertyAdjacent(typed, intended)) cost = (std::min)(cost, kQwertyAdjacentSubstitutionCost);
  if (AreQwertySameFinger(typed, intended)) {
    cost = (std::min)(cost, kQwertySameFingerSubstitutionCost);
  }
  return cost;
}

}  // namespace azookey::core
