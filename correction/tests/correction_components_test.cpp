#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "azookey/correction/KeyboardAdjacencyModel.h"
#include "azookey/correction/ReadingHypothesis.h"
#include "azookey/correction/RomajiVariantNormalizer.h"

namespace {

using azookey::correction::CorrectionEdit;
using azookey::correction::CorrectionEditKind;
using azookey::correction::EndsWithVariantYoon;
using azookey::correction::InvertEdits;
using azookey::correction::KeyboardAdjacencyModel;
using azookey::correction::KeyboardLayout;
using azookey::correction::NormalizeRomajiVariants;
using azookey::correction::TypoType;
using azookey::correction::TypoTypeName;

// docs/typo-correction-learning-spec.md section 12.4.2 examples.
TEST(KeyboardAdjacencyModelTest, SectionExamplesAreAdjacent) {
  const KeyboardAdjacencyModel keyboard;
  EXPECT_EQ(keyboard.layout(), KeyboardLayout::Auto);
  for (const char32_t neighbour : {U'i', U'y', U'j'})
    EXPECT_TRUE(keyboard.IsAdjacent(U'u', neighbour));
  for (const char32_t neighbour : {U'a', U'd', U'w', U'x'}) {
    EXPECT_TRUE(keyboard.IsAdjacent(U's', neighbour));
  }
  // Section 12.3 reads kouhsyou as extra_char, so 'h' is not next to 'u'.
  EXPECT_FALSE(keyboard.IsAdjacent(U'u', U'h'));
  EXPECT_FALSE(keyboard.IsAdjacent(U's', U'e'));
  EXPECT_FALSE(keyboard.IsAdjacent(U's', U'z'));
  EXPECT_FALSE(keyboard.IsAdjacent(U'u', U'u'));
  EXPECT_FALSE(keyboard.IsAdjacent(U'-', U'p'));
}

TEST(KeyboardAdjacencyModelTest, SameFingerFollowsTouchTyping) {
  const KeyboardAdjacencyModel keyboard;
  EXPECT_TRUE(keyboard.IsSameFinger(U'r', U'b'));
  EXPECT_TRUE(keyboard.IsSameFinger(U'u', U'm'));
  EXPECT_TRUE(keyboard.IsSameFinger(U'q', U'z'));
  EXPECT_FALSE(keyboard.IsSameFinger(U't', U'y'));
  EXPECT_FALSE(keyboard.IsSameFinger(U'a', U'a'));
  EXPECT_FALSE(keyboard.IsSameFinger(U'\'', U'p'));
}

// Section 12.4.6 costs, in tenths. A pair that is adjacent and typed by the
// same finger takes the smaller cost instead of both bonuses.
TEST(KeyboardAdjacencyModelTest, SubstitutionCostTakesTheSmallerBonus) {
  const KeyboardAdjacencyModel keyboard;
  EXPECT_EQ(keyboard.SubstitutionCost(U'a', U'a'), 0);
  EXPECT_EQ(keyboard.SubstitutionCost(U'q', U'p'), 10);
  EXPECT_EQ(keyboard.SubstitutionCost(U'u', U'i'), 7);
  EXPECT_EQ(keyboard.SubstitutionCost(U'b', U'f'), 6);
  EXPECT_EQ(keyboard.SubstitutionCost(U'u', U'j'), 6);
  EXPECT_EQ(keyboard.SubstitutionCost(U'-', U'a'), 10);
}

// Section 12.4.3 table.
TEST(RomajiVariantNormalizerTest, RewritesTheSectionTable) {
  EXPECT_EQ(NormalizeRomajiVariants("si"), "shi");
  EXPECT_EQ(NormalizeRomajiVariants("ti"), "chi");
  EXPECT_EQ(NormalizeRomajiVariants("tu"), "tsu");
  EXPECT_EQ(NormalizeRomajiVariants("syo"), "sho");
  EXPECT_EQ(NormalizeRomajiVariants("syou"), "shou");
  EXPECT_EQ(NormalizeRomajiVariants("zya"), "ja");
  EXPECT_EQ(NormalizeRomajiVariants("jya"), "ja");
  EXPECT_EQ(NormalizeRomajiVariants("nn"), "n");
}

TEST(RomajiVariantNormalizerTest, RewritesWholeRowsAndMixedSpellings) {
  EXPECT_EQ(NormalizeRomajiVariants("tyuu"), "chuu");
  EXPECT_EQ(NormalizeRomajiVariants("kousyou"), "koushou");
  EXPECT_EQ(NormalizeRomajiVariants("shyou"), "shou");
  EXPECT_EQ(NormalizeRomajiVariants("chya"), "cha");
  EXPECT_EQ(NormalizeRomajiVariants("shi"), "shi");
  EXPECT_EQ(NormalizeRomajiVariants("tsu"), "tsu");
  EXPECT_EQ(NormalizeRomajiVariants("ryo"), "ryo");
  EXPECT_EQ(NormalizeRomajiVariants(""), "");
}

TEST(RomajiVariantNormalizerTest, VariantYoonEndsTheKeys) {
  for (const char* keys : {"syo", "sho", "kousyu", "tyo", "cho", "jo", "zyu"}) {
    EXPECT_TRUE(EndsWithVariantYoon(keys)) << keys;
  }
  for (const char* keys : {"ryo", "ko", "su", "syoni", ""})
    EXPECT_FALSE(EndsWithVariantYoon(keys)) << keys;
}

TEST(ReadingHypothesisTest, TypoTypeNamesFollowTheSectionTable) {
  const std::vector<std::pair<TypoType, std::string>> names = {
      {TypoType::AdjacentKey, "adjacent_key"},
      {TypoType::MissingChar, "missing_char"},
      {TypoType::ExtraChar, "extra_char"},
      {TypoType::Transposition, "transposition"},
      {TypoType::DoubleKey, "double_key"},
      {TypoType::RomajiVariant, "romaji_variant"},
      {TypoType::NHandling, "n_handling"},
      {TypoType::SmallTsu, "small_tsu"},
      {TypoType::LongVowel, "long_vowel"},
      {TypoType::DakutenConfusion, "dakuten_confusion"},
      {TypoType::KanaShapeConfusion, "kana_shape_confusion"},
  };
  for (const auto& [type, name] : names) EXPECT_EQ(TypoTypeName(type), name);
}

// Section 12.12.1: intended_pattern is the observed edits turned around.
TEST(ReadingHypothesisTest, InvertEditsTurnsEachEditAround) {
  const std::vector<CorrectionEdit> observed = {
      {CorrectionEditKind::Insertion, U'j', 0, U'u'},
      {CorrectionEditKind::Deletion, U'u', 0, U'o'},
      {CorrectionEditKind::Substitution, U'z', U'x', U'a'},
      {CorrectionEditKind::Transposition, U't', U's', U'^'},
  };
  const std::vector<CorrectionEdit> intended = {
      {CorrectionEditKind::Deletion, U'j', 0, U'u'},
      {CorrectionEditKind::Insertion, U'u', 0, U'o'},
      {CorrectionEditKind::Substitution, U'x', U'z', U'a'},
      {CorrectionEditKind::Transposition, U's', U't', U'^'},
  };
  EXPECT_EQ(InvertEdits(observed), intended);
  EXPECT_EQ(InvertEdits(intended), observed);
  EXPECT_TRUE(InvertEdits({}).empty());
}

}  // namespace
