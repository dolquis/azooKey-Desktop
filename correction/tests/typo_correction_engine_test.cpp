#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "azookey/correction/TypoCorrectionEngine.h"

namespace {

using azookey::correction::CorrectionEdit;
using azookey::correction::CorrectionEditKind;
using azookey::correction::EditSpace;
using azookey::correction::GenerateReadingHypotheses;
using azookey::correction::HypothesisSource;
using azookey::correction::InvertEdits;
using azookey::correction::kGeneralTypoConfidence;
using azookey::correction::kMaxTypoHypotheses;
using azookey::correction::PersonalizedPattern;
using azookey::correction::PersonalizedPatternSource;
using azookey::correction::ReadingHypothesis;
using azookey::correction::TypoCorrectionOptions;
using azookey::correction::TypoType;
using azookey::correction::TypoTypeName;

std::vector<ReadingHypothesis> Generate(std::string_view reading, std::string_view keys,
                                        const TypoCorrectionOptions& options = {}) {
  return GenerateReadingHypotheses({reading, keys}, options);
}

// The correction hypothesis for `reading`, or null.
const ReadingHypothesis* FindCorrection(const std::vector<ReadingHypothesis>& hypotheses,
                                        std::string_view reading) {
  const auto it = std::find_if(hypotheses.begin(), hypotheses.end(), [&](const auto& hypothesis) {
    return hypothesis.source == HypothesisSource::TypoCorrection && hypothesis.reading == reading;
  });
  return it == hypotheses.end() ? nullptr : &*it;
}
// The pointer would outlive a temporary list.
const ReadingHypothesis* FindCorrection(std::vector<ReadingHypothesis>&&,
                                        std::string_view) = delete;

void ExpectOnlyTheOriginal(const std::vector<ReadingHypothesis>& hypotheses,
                           std::string_view reading) {
  ASSERT_EQ(hypotheses.size(), 1u);
  const ReadingHypothesis& original = hypotheses.front();
  EXPECT_EQ(original.reading, reading);
  EXPECT_EQ(original.source, HypothesisSource::Original);
  EXPECT_FALSE(original.typo_type.has_value());
  EXPECT_EQ(original.confidence, 1.0);
  EXPECT_EQ(original.cost, 0.0);
  EXPECT_EQ(original.edit_distance, 0u);
  EXPECT_TRUE(original.edits.empty());
}

struct TypoCase {
  const char* name;
  const char* keys;
  const char* observed;
  const char* intended;
  TypoType type;
};

class TypoTypeTest : public ::testing::TestWithParam<TypoCase> {};

// docs/typo-correction-learning-spec.md section 12.3: each type yields its
// intended reading with that type.
TEST_P(TypoTypeTest, GeneratesTheIntendedReadingWithItsType) {
  const TypoCase& typo = GetParam();
  const auto hypotheses = Generate(typo.observed, typo.keys);
  ASSERT_FALSE(hypotheses.empty());
  EXPECT_EQ(hypotheses.front().source, HypothesisSource::Original);
  EXPECT_EQ(hypotheses.front().reading, typo.observed);

  const ReadingHypothesis* hit = FindCorrection(hypotheses, typo.intended);
  ASSERT_NE(hit, nullptr);
  ASSERT_TRUE(hit->typo_type.has_value());
  EXPECT_EQ(TypoTypeName(*hit->typo_type), TypoTypeName(typo.type));
  EXPECT_EQ(hit->confidence, kGeneralTypoConfidence);
  EXPECT_FALSE(hit->personalized);
  EXPECT_TRUE(hit->pattern_id.empty());
  EXPECT_FALSE(hit->edits.empty());
  EXPECT_LE(hit->edits.size(), 2u);
  EXPECT_GT(hit->cost, 0.0);
}

INSTANTIATE_TEST_SUITE_P(
    Section12_3, TypoTypeTest,
    ::testing::Values(
        TypoCase{"AdjacentSubstitution", "taberi", "たべり", "たべる", TypoType::AdjacentKey},
        TypoCase{"AdjacentInsertion", "koujsyou", "こうjしょう", "こうしょう",
                 TypoType::AdjacentKey},
        TypoCase{"MissingChar", "hasuru", "はする", "はいする", TypoType::MissingChar},
        TypoCase{"ExtraChar", "kouhsyou", "こうhしょう", "こうしょう", TypoType::ExtraChar},
        TypoCase{"Transposition", "ahsita", "あhした", "あした", TypoType::Transposition},
        TypoCase{"DoubleKey", "kiggou", "きっごう", "きごう", TypoType::DoubleKey},
        TypoCase{"RomajiVariantLongSyllable", "syonin", "しょにん", "しょうにん",
                 TypoType::RomajiVariant},
        TypoCase{"RomajiVariantMixedSpelling", "shyou", "sひょう", "しょう",
                 TypoType::RomajiVariant},
        TypoCase{"NHandling", "ashin", "あしん", "あんしん", TypoType::NHandling},
        TypoCase{"SmallTsu", "ketei", "けてい", "けってい", TypoType::SmallTsu},
        TypoCase{"LongVowel", "arigato", "ありがと", "ありがとう", TypoType::LongVowel},
        TypoCase{"DakutenFromKeys", "kinkou", "きんこう", "ぎんこう", TypoType::DakutenConfusion},
        TypoCase{"DakutenFromReading", "", "きんこう", "ぎんこう", TypoType::DakutenConfusion},
        TypoCase{"DakutenWithKanaShape", "", "かいしや", "がいしゃ", TypoType::DakutenConfusion},
        TypoCase{"KanaShapeFromKeys", "siyain", "しやいん", "しゃいん",
                 TypoType::KanaShapeConfusion},
        TypoCase{"KanaShapeFromHiraganaKeys", "おちや", "おちや", "おちゃ",
                 TypoType::KanaShapeConfusion}),
    [](const ::testing::TestParamInfo<TypoCase>& info) { return std::string(info.param.name); });

TEST(TypoCorrectionEngineTest, EmptyReadingYieldsNothing) {
  EXPECT_TRUE(Generate("", "").empty());
  EXPECT_TRUE(Generate("", "kite").empty());
}

// Section 12.16 LEN_MIN: two code points or fewer are not corrected.
TEST(TypoCorrectionEngineTest, ShortReadingKeepsOnlyTheOriginal) {
  ExpectOnlyTheOriginal(Generate("きて", "kite"), "きて");
  ExpectOnlyTheOriginal(Generate("かに", ""), "かに");
  EXPECT_GT(Generate("けてい", "ketei").size(), 1u);
}

// Section 12.12.1: keys or readings outside the alphabet are not abstracted.
TEST(TypoCorrectionEngineTest, CharactersOutsideTheAlphabetKeepOnlyTheOriginal) {
  ExpectOnlyTheOriginal(Generate("カイシャ", ""), "カイシャ");
  ExpectOnlyTheOriginal(Generate("かいしゃ", "kai5ya"), "かいしゃ");
  ExpectOnlyTheOriginal(Generate("かいしゃ", "Kaisya"), "かいしゃ");
  ExpectOnlyTheOriginal(Generate("かいしゃ1", ""), "かいしゃ1");
}

TEST(TypoCorrectionEngineTest, OverlongInputKeepsOnlyTheOriginal) {
  std::string reading;
  for (int i = 0; i < 64; ++i) reading += "か";
  EXPECT_GT(Generate(reading, "").size(), 1u);
  ExpectOnlyTheOriginal(Generate(reading + "か", ""), reading + "か");
  ExpectOnlyTheOriginal(Generate("あああ", std::string(65, 'a')), "あああ");
}

TEST(TypoCorrectionEngineTest, InvalidUtf8KeepsOnlyTheOriginal) {
  const std::string reading = std::string("かいし") + "\xff";
  ExpectOnlyTheOriginal(Generate(reading, ""), reading);
  ExpectOnlyTheOriginal(Generate("かいしゃ", "\xff"), "かいしゃ");
}

TEST(TypoCorrectionEngineTest, LimitCapsTheCorrections) {
  TypoCorrectionOptions options;
  options.limit = 0;
  ExpectOnlyTheOriginal(Generate("こうjしょう", "koujsyou", options), "こうjしょう");

  options.limit = 3;
  const auto capped = Generate("こうjしょう", "koujsyou", options);
  const auto full = Generate("こうjしょう", "koujsyou");
  ASSERT_EQ(capped.size(), 4u);
  ASSERT_GT(full.size(), 4u);
  for (size_t i = 0; i < capped.size(); ++i) EXPECT_EQ(capped[i].reading, full[i].reading);

  options.limit = kMaxTypoHypotheses * 4;
  EXPECT_LE(Generate("こうjしょう", "koujsyou", options).size(), kMaxTypoHypotheses + 1);
}

// The interpretation of "no hypothesis for an input that needs no correction"
// within this library: the correct reading comes first and no correction
// repeats it. The dictionary constraint (section 12.4.4) and the activation
// gate (section 12.5) are the Host's.
TEST(TypoCorrectionEngineTest, CorrectReadingComesFirstAndIsNotRepeated) {
  const auto hypotheses = Generate("たべる", "taberu");
  ASSERT_GT(hypotheses.size(), 1u);
  EXPECT_EQ(hypotheses.front().reading, "たべる");
  EXPECT_EQ(hypotheses.front().source, HypothesisSource::Original);
  for (size_t i = 1; i < hypotheses.size(); ++i) {
    EXPECT_EQ(hypotheses[i].source, HypothesisSource::TypoCorrection);
    EXPECT_NE(hypotheses[i].reading, "たべる");
  }
}

TEST(TypoCorrectionEngineTest, CorrectionsAreCheapestFirstThenByReading) {
  const auto hypotheses = Generate("こうjしょう", "koujsyou");
  ASSERT_GT(hypotheses.size(), 2u);
  for (size_t i = 2; i < hypotheses.size(); ++i) {
    EXPECT_LE(std::tie(hypotheses[i - 1].cost, hypotheses[i - 1].reading),
              std::tie(hypotheses[i].cost, hypotheses[i].reading));
  }
  for (const auto& hypothesis : hypotheses) {
    EXPECT_EQ(std::count_if(hypotheses.begin(), hypotheses.end(),
                            [&](const auto& other) { return other.reading == hypothesis.reading; }),
              1);
  }
  EXPECT_EQ(Generate("こうjしょう", "koujsyou").size(), hypotheses.size());
}

TEST(TypoCorrectionEngineTest, KeysThatDoNotSpellTheReadingAddNoKeyHypotheses) {
  const auto hypotheses = Generate("こうしょ", "koushou");
  ASSERT_GT(hypotheses.size(), 1u);
  for (size_t i = 1; i < hypotheses.size(); ++i) {
    EXPECT_EQ(hypotheses[i].edit_space, EditSpace::Reading) << hypotheses[i].reading;
  }
  EXPECT_NE(FindCorrection(hypotheses, "こうじょ"), nullptr);
}

// docs/legacy-parity-spec.md section 5.4: reading "nn" + vowel the other way
// is a reading choice, not a correction.
TEST(TypoCorrectionEngineTest, AmbiguousDoubleNReadingIsNotACorrection) {
  const auto hypotheses = Generate("かんに", "kanni");
  EXPECT_EQ(FindCorrection(hypotheses, "かんい"), nullptr);
  const ReadingHypothesis* dropped_n = FindCorrection(hypotheses, "かに");
  ASSERT_NE(dropped_n, nullptr);
  ASSERT_TRUE(dropped_n->typo_type.has_value());
  EXPECT_EQ(*dropped_n->typo_type, TypoType::NHandling);
}

TEST(TypoCorrectionEngineTest, KeyEditsDescribeWhatWasTyped) {
  const auto adjacent = Generate("こうjしょう", "koujsyou");
  const ReadingHypothesis* inserted = FindCorrection(adjacent, "こうしょう");
  ASSERT_NE(inserted, nullptr);
  EXPECT_EQ(inserted->edit_space, EditSpace::Keys);
  const std::vector<CorrectionEdit> extra_j = {{CorrectionEditKind::Insertion, U'j', 0, U'u'}};
  EXPECT_EQ(inserted->edits, extra_j);
  const std::vector<CorrectionEdit> drop_j = {{CorrectionEditKind::Deletion, U'j', 0, U'u'}};
  EXPECT_EQ(InvertEdits(inserted->edits), drop_j);
  EXPECT_EQ(inserted->cost, 1.0);
  EXPECT_EQ(inserted->edit_distance, 1u);

  // A doubled or left-out key reads "after the same key".
  const auto doubled_key = Generate("きっごう", "kiggou");
  const ReadingHypothesis* doubled = FindCorrection(doubled_key, "きごう");
  ASSERT_NE(doubled, nullptr);
  const std::vector<CorrectionEdit> extra_g = {{CorrectionEditKind::Insertion, U'g', 0, U'g'}};
  EXPECT_EQ(doubled->edits, extra_g);
  const auto small_tsu = Generate("けてい", "ketei");
  const ReadingHypothesis* left_out = FindCorrection(small_tsu, "けってい");
  ASSERT_NE(left_out, nullptr);
  const std::vector<CorrectionEdit> missing_t = {{CorrectionEditKind::Deletion, U't', 0, U't'}};
  EXPECT_EQ(left_out->edits, missing_t);

  const auto transposition = Generate("あhした", "ahsita");
  const ReadingHypothesis* swapped = FindCorrection(transposition, "あした");
  ASSERT_NE(swapped, nullptr);
  const std::vector<CorrectionEdit> swap_hs = {
      {CorrectionEditKind::Transposition, U'h', U's', U'a'}};
  EXPECT_EQ(swapped->edits, swap_hs);
  EXPECT_EQ(swapped->cost, 0.5);

  const auto substitution = Generate("たべり", "taberi");
  const ReadingHypothesis* substituted = FindCorrection(substitution, "たべる");
  ASSERT_NE(substituted, nullptr);
  const std::vector<CorrectionEdit> i_for_u = {
      {CorrectionEditKind::Substitution, U'i', U'u', U'r'}};
  EXPECT_EQ(substituted->edits, i_for_u);
  EXPECT_EQ(substituted->cost, 0.7);
}

TEST(TypoCorrectionEngineTest, ReadingEditsSubstituteKana) {
  const auto hypotheses = Generate("かいしや", "");
  const ReadingHypothesis* hit = FindCorrection(hypotheses, "がいしゃ");
  ASSERT_NE(hit, nullptr);
  EXPECT_EQ(hit->edit_space, EditSpace::Reading);
  const std::vector<CorrectionEdit> edits = {
      {CorrectionEditKind::Substitution, U'か', U'が', U'^'},
      {CorrectionEditKind::Substitution, U'や', U'ゃ', U'し'},
  };
  EXPECT_EQ(hit->edits, edits);
  EXPECT_EQ(hit->cost, 2.0);
  EXPECT_EQ(hit->edit_distance, 2u);
}

class FakePatternSource : public PersonalizedPatternSource {
 public:
  explicit FakePatternSource(PersonalizedPattern pattern) : pattern_(std::move(pattern)) {}

  std::optional<PersonalizedPattern> Find(TypoType type, EditSpace space,
                                          const std::vector<CorrectionEdit>& edits) const override {
    const std::vector<CorrectionEdit> k_for_j = {
        {CorrectionEditKind::Substitution, U'k', U'j', U'^'}};
    if (type == TypoType::AdjacentKey && space == EditSpace::Keys && edits == k_for_j) {
      return pattern_;
    }
    return std::nullopt;
  }

 private:
  PersonalizedPattern pattern_;
};

TEST(TypoCorrectionEngineTest, PersonalPatternLowersTheCostAndMarksTheHypothesis) {
  const auto plain = Generate("きたく", "kitaku");
  const ReadingHypothesis* general = FindCorrection(plain, "じたく");
  ASSERT_NE(general, nullptr);
  EXPECT_EQ(general->cost, 0.7);

  const FakePatternSource source({0.5, 0.9, "k_sub_j"});
  TypoCorrectionOptions options;
  options.personalized = &source;
  const auto hypotheses = Generate("きたく", "kitaku", options);
  ASSERT_GT(hypotheses.size(), 2u);
  const ReadingHypothesis& first = hypotheses[1];
  EXPECT_EQ(first.reading, "じたく");
  EXPECT_TRUE(first.personalized);
  EXPECT_EQ(first.confidence, 0.9);
  EXPECT_EQ(first.pattern_id, "k_sub_j");
  EXPECT_NEAR(first.cost, 0.2, 1e-9);
  for (size_t i = 2; i < hypotheses.size(); ++i) {
    EXPECT_FALSE(hypotheses[i].personalized);
    EXPECT_EQ(hypotheses[i].confidence, kGeneralTypoConfidence);
  }

  const FakePatternSource large({5.0, 0.9, "k_sub_j"});
  options.personalized = &large;
  const auto with_large = Generate("きたく", "kitaku", options);
  const ReadingHypothesis* floored = FindCorrection(with_large, "じたく");
  ASSERT_NE(floored, nullptr);
  EXPECT_EQ(floored->cost, 0.0);
}

TEST(TypoCorrectionEngineTest, UnusablePersonalPatternIsIgnored) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  for (const PersonalizedPattern& pattern :
       {PersonalizedPattern{nan, 0.9, "nan"}, PersonalizedPattern{-0.1, 0.9, "negative"},
        PersonalizedPattern{0.5, 1.5, "confidence"},
        PersonalizedPattern{0.5, nan, "nan_confidence"}}) {
    const FakePatternSource source(pattern);
    TypoCorrectionOptions options;
    options.personalized = &source;
    const auto hypotheses = Generate("きたく", "kitaku", options);
    const ReadingHypothesis* hit = FindCorrection(hypotheses, "じたく");
    ASSERT_NE(hit, nullptr) << pattern.pattern_id;
    EXPECT_FALSE(hit->personalized) << pattern.pattern_id;
    EXPECT_EQ(hit->cost, 0.7) << pattern.pattern_id;
    EXPECT_EQ(hit->confidence, kGeneralTypoConfidence) << pattern.pattern_id;
  }
}

}  // namespace
