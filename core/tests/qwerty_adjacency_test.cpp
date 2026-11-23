#include <gtest/gtest.h>

#include "azookey/core/QwertyAdjacency.h"

namespace {

using azookey::core::AreQwertyAdjacent;
using azookey::core::AreQwertySameFinger;
using azookey::core::QwertySubstitutionCost;

// docs/typo-correction-learning-spec.md section 12.4.2 examples, plus the keys
// section 12.3 keeps apart (kouhsyou is extra_char, so 'h' is not next to 'u').
TEST(QwertyAdjacencyTest, RowNeighboursAndColumnNeighboursAreAdjacent) {
  for (const char key : {'i', 'y', 'j'}) EXPECT_TRUE(AreQwertyAdjacent('u', key)) << key;
  for (const char key : {'a', 'd', 'w', 'x'}) EXPECT_TRUE(AreQwertyAdjacent('s', key)) << key;
  for (const char key : {'h', 'k', 'm', 'o'}) EXPECT_FALSE(AreQwertyAdjacent('u', key)) << key;
  for (const char key : {'e', 'z', 'q'}) EXPECT_FALSE(AreQwertyAdjacent('s', key)) << key;
  EXPECT_TRUE(AreQwertyAdjacent('p', 'o'));
  EXPECT_TRUE(AreQwertyAdjacent('m', 'j'));
  EXPECT_TRUE(AreQwertyAdjacent('n', 'h'));
  EXPECT_FALSE(AreQwertyAdjacent('l', 'p'));
  EXPECT_FALSE(AreQwertyAdjacent('a', 'a'));
  EXPECT_FALSE(AreQwertyAdjacent('-', 'p'));
  EXPECT_FALSE(AreQwertyAdjacent('A', 'S'));
}

TEST(QwertyAdjacencyTest, AdjacencyIsSymmetric) {
  for (char a = 'a'; a <= 'z'; ++a) {
    for (char b = 'a'; b <= 'z'; ++b) {
      EXPECT_EQ(AreQwertyAdjacent(a, b), AreQwertyAdjacent(b, a)) << a << b;
      EXPECT_EQ(AreQwertySameFinger(a, b), AreQwertySameFinger(b, a)) << a << b;
      EXPECT_EQ(QwertySubstitutionCost(a, b), QwertySubstitutionCost(b, a)) << a << b;
    }
  }
}

TEST(QwertyAdjacencyTest, SameFingerFollowsTouchTyping) {
  const char* fingers[] = {"qaz", "wsx", "edc", "rfvtgb", "yhnujm", "ik", "ol", "p"};
  for (const char* keys : fingers) {
    for (const char* a = keys; *a; ++a) {
      for (const char* b = keys; *b; ++b) {
        EXPECT_EQ(AreQwertySameFinger(*a, *b), *a != *b) << *a << *b;
      }
    }
  }
  EXPECT_FALSE(AreQwertySameFinger('t', 'y'));
  EXPECT_FALSE(AreQwertySameFinger('k', 'l'));
  EXPECT_FALSE(AreQwertySameFinger('\'', 'p'));
}

// Section 12.4.6 in tenths: the bonuses for adjacent keys and the same finger
// do not add up; a pair that is both takes the smaller cost.
TEST(QwertyAdjacencyTest, SubstitutionCostTakesTheSmallerBonus) {
  EXPECT_EQ(QwertySubstitutionCost('a', 'a'), 0);
  EXPECT_EQ(QwertySubstitutionCost('q', 'p'), 10);
  EXPECT_EQ(QwertySubstitutionCost('u', 'i'), 7);
  EXPECT_EQ(QwertySubstitutionCost('r', 'b'), 6);
  EXPECT_EQ(QwertySubstitutionCost('u', 'j'), 6);
  EXPECT_EQ(QwertySubstitutionCost('-', 'a'), 10);
}

}  // namespace
