#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <optional>
#include <ostream>
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
  EditSpace space;  // the path expected to win the reading
};

// The discovered CTest name carries the printed parameter, so print only the
// case name: the default byte dump holds pointers that differ between builds.
void PrintTo(const TypoCase& value, std::ostream* out) { *out << value.name; }

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
  EXPECT_EQ(hit->edit_space, typo.space);
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
        TypoCase{"AdjacentSubstitution", "taberi", "たべり", "たべる", TypoType::AdjacentKey,
                 EditSpace::Keys},
        TypoCase{"AdjacentInsertion", "koujsyou", "こうjしょう", "こうしょう",
                 TypoType::AdjacentKey, EditSpace::Keys},
        TypoCase{"MissingChar", "hasuru", "はする", "はいする", TypoType::MissingChar,
                 EditSpace::Keys},
        TypoCase{"ExtraChar", "kouhsyou", "こうhしょう", "こうしょう", TypoType::ExtraChar,
                 EditSpace::Keys},
        TypoCase{"Transposition", "ahsita", "あhした", "あした", TypoType::Transposition,
                 EditSpace::Keys},
        TypoCase{"DoubleKey", "kiggou", "きっごう", "きごう", TypoType::DoubleKey, EditSpace::Keys},
        TypoCase{"RomajiVariantLongSyllable", "syonin", "しょにん", "しょうにん",
                 TypoType::RomajiVariant, EditSpace::Keys},
        TypoCase{"RomajiVariantMixedSpelling", "shyou", "sひょう", "しょう",
                 TypoType::RomajiVariant, EditSpace::Keys},
        TypoCase{"NHandling", "ashin", "あしん", "あんしん", TypoType::NHandling, EditSpace::Keys},
        TypoCase{"SmallTsu", "ketei", "けてい", "けってい", TypoType::SmallTsu, EditSpace::Keys},
        TypoCase{"LongVowel", "arigato", "ありがと", "ありがとう", TypoType::LongVowel,
                 EditSpace::Keys},
        // b and f share a finger (0.6), cheaper than the reading's substitution (1.0).
        TypoCase{"DakutenFromKeys", "buton", "ぶとん", "ふとん", TypoType::DakutenConfusion,
                 EditSpace::Keys},
        TypoCase{"DakutenFromReading", "", "きんこう", "ぎんこう", TypoType::DakutenConfusion,
                 EditSpace::Reading},
        // k and g are far keys: the key path leaves the substitution out and the
        // reading path gives ぎんこう at the same cost.
        TypoCase{"DakutenOfFarKeys", "kinkou", "きんこう", "ぎんこう", TypoType::DakutenConfusion,
                 EditSpace::Reading},
        TypoCase{"DakutenWithKanaShape", "", "かいしや", "がいしゃ", TypoType::DakutenConfusion,
                 EditSpace::Reading},
        // The left-out "l" of "silyain" ties the reading's substitution and wins as Keys.
        TypoCase{"KanaShapeFromKeys", "siyain", "しやいん", "しゃいん",
                 TypoType::KanaShapeConfusion, EditSpace::Keys},
        TypoCase{"KanaShapeFromHiraganaKeys", "おちや", "おちや", "おちゃ",
                 TypoType::KanaShapeConfusion, EditSpace::Reading}),
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

// Far keys fit no type. At the same cost, "kahweten" (the left-out w of "hwe")
// does, so かふぇてん comes back as missing_char rather than being dropped.
TEST(TypoCorrectionEngineTest, ASpellingThatFitsATypeGivesTheReading) {
  const auto hypotheses = Generate("かへてん", "kaheten");
  const ReadingHypothesis* hit = FindCorrection(hypotheses, "かふぇてん");
  ASSERT_NE(hit, nullptr);
  ASSERT_TRUE(hit->typo_type.has_value());
  EXPECT_EQ(*hit->typo_type, TypoType::MissingChar);
  EXPECT_EQ(hit->edit_space, EditSpace::Keys);
  const std::vector<CorrectionEdit> missing_w = {{CorrectionEditKind::Deletion, U'w', 0, U'h'}};
  EXPECT_EQ(hit->edits, missing_w);
  EXPECT_EQ(hit->cost, 1.0);
}

// A short input whose far-key substitution would change the reading: the key
// path leaves it out (a deletion and an insertion, 2.0, fall past the 256
// readings), so a voiced reading comes from the reading path at 1.0 and any
// other reading does not come back.
TEST(TypoCorrectionEngineTest, AFarKeySubstitutionOfAShortInput) {
  const auto voiced = Generate("あいか", "aika");
  const ReadingHypothesis* hit = FindCorrection(voiced, "あいが");
  ASSERT_NE(hit, nullptr);
  ASSERT_TRUE(hit->typo_type.has_value());
  EXPECT_EQ(*hit->typo_type, TypoType::DakutenConfusion);
  EXPECT_EQ(hit->edit_space, EditSpace::Reading);
  EXPECT_EQ(hit->cost, 1.0);

  const auto vowel = Generate("あいう", "aiu");
  EXPECT_EQ(FindCorrection(vowel, "おいう"), nullptr);
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

// Returns `pattern` for one (type, space, edits) and nothing otherwise. The
// default is the k typed for j of "kitaku" (じたく).
class FakePatternSource : public PersonalizedPatternSource {
 public:
  explicit FakePatternSource(PersonalizedPattern pattern, TypoType type = TypoType::AdjacentKey,
                             EditSpace space = EditSpace::Keys,
                             std::vector<CorrectionEdit> edits = {{CorrectionEditKind::Substitution,
                                                                   U'k', U'j', U'^'}})
      : pattern_(std::move(pattern)), type_(type), space_(space), edits_(std::move(edits)) {}

  std::optional<PersonalizedPattern> Find(TypoType type, EditSpace space,
                                          const std::vector<CorrectionEdit>& edits) const override {
    if (type == type_ && space == space_ && edits == edits_) return pattern_;
    return std::nullopt;
  }

 private:
  PersonalizedPattern pattern_;
  TypoType type_;
  EditSpace space_;
  std::vector<CorrectionEdit> edits_;
};

// Section 12.4.1: the personalized term is part of each path's cost, so a
// learned reading-side pattern lets its path win a reading it would tie on.
TEST(TypoCorrectionEngineTest, PersonalPatternIsAppliedBeforeAReadingKeepsOnePath) {
  const auto plain = Generate("しやいん", "siyain");
  const ReadingHypothesis* keys_path = FindCorrection(plain, "しゃいん");
  ASSERT_NE(keys_path, nullptr);
  EXPECT_EQ(keys_path->edit_space, EditSpace::Keys);
  EXPECT_EQ(keys_path->cost, 1.0);

  const FakePatternSource source({0.5, 0.8, "や_sub_ゃ"}, TypoType::KanaShapeConfusion,
                                 EditSpace::Reading,
                                 {{CorrectionEditKind::Substitution, U'や', U'ゃ', U'し'}});
  TypoCorrectionOptions options;
  options.personalized = &source;
  const auto personalized = Generate("しやいん", "siyain", options);
  const ReadingHypothesis* reading_path = FindCorrection(personalized, "しゃいん");
  ASSERT_NE(reading_path, nullptr);
  EXPECT_EQ(reading_path->edit_space, EditSpace::Reading);
  EXPECT_TRUE(reading_path->personalized);
  EXPECT_EQ(reading_path->pattern_id, "や_sub_ゃ");
  EXPECT_EQ(reading_path->cost, 0.5);
}

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
