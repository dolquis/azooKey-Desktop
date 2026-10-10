#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "azookey/host/CandidateTags.h"
#include "azookey/host/DictionaryCandidateProvider.h"

namespace {

namespace j = azookey::ipc::json;
using azookey::core::Candidate;
using azookey::core::CandidateTag;
using azookey::host::TagBoosts;

j::Object Profile(const std::string& json) {
  const auto parsed = j::Parse(json);
  EXPECT_TRUE(parsed && parsed->IsObject());
  return parsed && parsed->IsObject() ? parsed->AsObject() : j::Object{};
}

Candidate Tagged(std::string surface, double score, CandidateTag tag) {
  Candidate c;
  c.surface = std::move(surface);
  c.score = score;
  c.tag = tag;
  return c;
}

std::vector<std::string> Surfaces(const std::vector<Candidate>& candidates) {
  std::vector<std::string> surfaces;
  for (const auto& c : candidates) surfaces.push_back(c.surface);
  return surfaces;
}

TEST(CandidateTagsTest, TagNamesMatchIgnoringAsciiCase) {
  EXPECT_EQ(azookey::host::CandidateTagFromName("Technical"), CandidateTag::Technical);
  EXPECT_EQ(azookey::host::CandidateTagFromName("technical"), CandidateTag::Technical);
  EXPECT_EQ(azookey::host::CandidateTagFromName("ENGLISH"), CandidateTag::English);
  EXPECT_FALSE(azookey::host::CandidateTagFromName("None").has_value());
  EXPECT_FALSE(azookey::host::CandidateTagFromName("NamedEntity").has_value());
  EXPECT_FALSE(azookey::host::CandidateTagFromName("").has_value());
}

TEST(CandidateTagsTest, GlobalDefaultsAreNeutral) {
  const auto boosts = azookey::host::TagBoostsFromProfile(
      Profile(R"({"style":"auto","preferTechnicalTerms":false,"candidateTagBoosts":{}})"));
  EXPECT_TRUE(boosts.IsNeutral());
  EXPECT_TRUE(azookey::host::TagBoostsFromProfile({}).IsNeutral());
}

TEST(CandidateTagsTest, StyleAndPreferTechnicalTermsImplyTheirTagBoost) {
  const auto polite = azookey::host::TagBoostsFromProfile(Profile(R"({"style":"polite"})"));
  EXPECT_DOUBLE_EQ(polite.For(CandidateTag::Polite), azookey::host::kImplicitTagBoost);
  EXPECT_DOUBLE_EQ(polite.For(CandidateTag::Casual), 1.0);

  const auto casual = azookey::host::TagBoostsFromProfile(Profile(R"({"style":"casual"})"));
  EXPECT_DOUBLE_EQ(casual.For(CandidateTag::Casual), azookey::host::kImplicitTagBoost);

  const auto technical =
      azookey::host::TagBoostsFromProfile(Profile(R"({"preferTechnicalTerms":true})"));
  EXPECT_DOUBLE_EQ(technical.For(CandidateTag::Technical), azookey::host::kImplicitTagBoost);
}

TEST(CandidateTagsTest, ExplicitBoostsTakeTheLargerOfExplicitAndImplicit) {
  const auto larger = azookey::host::TagBoostsFromProfile(
      Profile(R"({"style":"technical","candidateTagBoosts":{"Technical":2.5}})"));
  EXPECT_DOUBLE_EQ(larger.For(CandidateTag::Technical), 2.5);

  const auto smaller = azookey::host::TagBoostsFromProfile(
      Profile(R"({"style":"technical","candidateTagBoosts":{"Technical":1.2}})"));
  EXPECT_DOUBLE_EQ(smaller.For(CandidateTag::Technical), azookey::host::kImplicitTagBoost);
}

TEST(CandidateTagsTest, BoostsAreClampedAndMalformedEntriesIgnored) {
  const auto boosts = azookey::host::TagBoostsFromProfile(
      Profile(R"({"candidateTagBoosts":{"English":100,"Polite":0.2,"Casual":"x","FutureTag":2.0},)"
              R"("style":7,"preferTechnicalTerms":"yes"})"));
  EXPECT_DOUBLE_EQ(boosts.For(CandidateTag::English), 3.0);
  EXPECT_DOUBLE_EQ(boosts.For(CandidateTag::Polite), 1.0);
  EXPECT_DOUBLE_EQ(boosts.For(CandidateTag::Casual), 1.0);
  EXPECT_DOUBLE_EQ(boosts.For(CandidateTag::Technical), 1.0);
}

TEST(CandidateTagsTest, AsciiDominantSurfacesAreTaggedEnglish) {
  std::vector<Candidate> candidates = {
      Tagged("TensorRT", 1.0, CandidateTag::None),
      Tagged("iPhone 15", 1.0, CandidateTag::None),
      Tagged("日本", 1.0, CandidateTag::None),
      Tagged("Aさん", 1.0, CandidateTag::None),
      Tagged("123", 1.0, CandidateTag::None),
      Tagged("", 1.0, CandidateTag::None),
      Tagged("Rust", 1.0, CandidateTag::Technical),
      Tagged("Aさ\tん\xE3\x80\x80x", 1.0, CandidateTag::None),
      Tagged("ABさ\xE3\x80\x80", 1.0, CandidateTag::None),
  };
  azookey::host::AssignHeuristicTags(candidates);
  EXPECT_EQ(candidates[0].tag, CandidateTag::English);
  EXPECT_EQ(candidates[1].tag, CandidateTag::English);
  EXPECT_EQ(candidates[2].tag, CandidateTag::None);
  EXPECT_EQ(candidates[3].tag, CandidateTag::None);  // One ASCII of three code points.
  EXPECT_EQ(candidates[4].tag, CandidateTag::None);  // No ASCII letter.
  EXPECT_EQ(candidates[5].tag, CandidateTag::None);
  EXPECT_EQ(candidates[6].tag, CandidateTag::Technical);  // A source tag is kept.
  // Tab and U+3000 are skipped like ' ': A, x of A,さ,ん,x is not a majority.
  EXPECT_EQ(candidates[7].tag, CandidateTag::None);
  EXPECT_EQ(candidates[8].tag, CandidateTag::English);  // A, B of A,B,さ once U+3000 is skipped.
}

TEST(CandidateTagsTest, DictionaryCategoriesAssignTechnicalBeforeEnglish) {
  for (uint16_t category = 0; category < 10; ++category) {
    SCOPED_TRACE(category);
    azookey::learning::DictionaryStore store;
    azookey::learning::DictionaryEntry entry;
    entry.surface = "TensorRT";
    entry.reading = "てんそる";
    entry.frequency = .72;
    entry.category_mask = static_cast<uint16_t>(1U << category);
    store.ReplaceMutable(azookey::learning::LayerId::AppSpecific, {entry});
    for (const auto mode :
         {azookey::learning::LookupMode::Exact, azookey::learning::LookupMode::PredictivePrefix}) {
      auto candidates = azookey::host::DictionaryCandidates(
          store, mode == azookey::learning::LookupMode::Exact ? "てんそる" : "てん", mode, 0, 10);
      ASSERT_EQ(candidates.size(), 1U);
      const bool technical = category == 4 || category == 5 || category == 8;
      EXPECT_EQ(candidates[0].tag, technical ? CandidateTag::Technical : CandidateTag::None);
      const double dictionary_score = candidates[0].score;
      azookey::host::AssignHeuristicTags(candidates);
      EXPECT_EQ(candidates[0].tag, technical ? CandidateTag::Technical : CandidateTag::English);
      // Tag assignment does not apply a profile boost to dictionary_score.
      EXPECT_DOUBLE_EQ(candidates[0].score, dictionary_score);
    }
  }
}

TEST(CandidateTagsTest, ApprovedStyleSuffixesTagUntaggedCandidates) {
  for (const auto suffix :
       {"です", "ます", "でした", "ました", "ません", "ませんでした", "ございます", "ございました",
        "ください", "だよ", "だね", "だぞ", "だぜ", "だろ", "じゃん"}) {
    SCOPED_TRACE(suffix);
    const bool casual =
        std::string_view(suffix).starts_with("だ") || std::string_view(suffix) == "じゃん";
    const auto expected = casual ? CandidateTag::Casual : CandidateTag::Polite;
    std::vector<Candidate> candidates = {
        Tagged(suffix, 1.0, CandidateTag::None),
        Tagged(std::string("文末") + suffix, -2.0, CandidateTag::None),
        Tagged(std::string("ABCDEFG") + suffix, 3.0, CandidateTag::None),
    };
    const auto original = candidates;
    azookey::host::AssignHeuristicTags(candidates);
    for (size_t i = 0; i < candidates.size(); ++i) {
      EXPECT_EQ(candidates[i].tag, expected);
      EXPECT_EQ(candidates[i].surface, original[i].surface);
      EXPECT_DOUBLE_EQ(candidates[i].score, original[i].score);
    }
  }
}

TEST(CandidateTagsTest, StyleSuffixesIgnoreOnlyTrailingWhitespaceAndApprovedPunctuation) {
  for (const auto tail :
       {"。", "！", "!", "？", "?", "　 \t\r\n\v\f。！!?？　", "\xC2\xA0\xE2\x80\xAF"}) {
    SCOPED_TRACE(tail);
    std::vector<Candidate> candidates = {
        Tagged(std::string("そうです") + tail, 1.0, CandidateTag::None),
        Tagged(std::string("そうだよ") + tail, 1.0, CandidateTag::None),
    };
    azookey::host::AssignHeuristicTags(candidates);
    EXPECT_EQ(candidates[0].tag, CandidateTag::Polite);
    EXPECT_EQ(candidates[1].tag, CandidateTag::Casual);
  }
  for (const auto surface : {"", "。！!?？　 \t", "サラダ", "そうだ", "ですけど", "だよね",
                             "ですと言った", "だよと言った", "です、", "です,", "です.", "です…",
                             "です」", "だよ😊", "で す", "だ よ"}) {
    SCOPED_TRACE(surface);
    std::vector<Candidate> candidates = {Tagged(surface, 1.0, CandidateTag::None)};
    azookey::host::AssignHeuristicTags(candidates);
    EXPECT_EQ(candidates[0].tag, CandidateTag::None);
  }
}

TEST(CandidateTagsTest, StyleSuffixesPreserveEveryExistingTagAndRejectInvalidUtf8) {
  for (const auto tag : {CandidateTag::Polite, CandidateTag::Casual, CandidateTag::Technical,
                         CandidateTag::English, CandidateTag::Kaomoji, CandidateTag::Idiom}) {
    SCOPED_TRACE(static_cast<int>(tag));
    std::vector<Candidate> candidates = {Tagged("そうです", 1.0, tag),
                                         Tagged("そうだよ", 1.0, tag)};
    azookey::host::AssignHeuristicTags(candidates);
    EXPECT_EQ(candidates[0].tag, tag);
    EXPECT_EQ(candidates[1].tag, tag);
  }
  std::vector<Candidate> malformed = {
      Tagged(std::string("\xFF") + "です", 1.0, CandidateTag::None),
      Tagged(std::string("だよ") + "\xE3\x80", 1.0, CandidateTag::None),
  };
  azookey::host::AssignHeuristicTags(malformed);
  EXPECT_EQ(malformed[0].tag, CandidateTag::None);
  EXPECT_EQ(malformed[1].tag, CandidateTag::None);
}

TEST(CandidateTagsTest, DictionaryCategoryUnionKeepsTechnicalForTheWinningLayer) {
  azookey::learning::DictionaryStore store;
  azookey::learning::DictionaryEntry entry;
  entry.surface = "専門用語";
  entry.reading = "せんもん";
  entry.frequency = .72;
  entry.category_mask = 1U << 5;  // software, from a lower-priority layer.
  store.ReplaceMutable(azookey::learning::LayerId::AppSpecific, {entry});
  entry.category_mask = 1U << 1;  // person_name; higher-priority user entry wins.
  store.ReplaceMutable(azookey::learning::LayerId::User, {entry});
  auto candidates = azookey::host::DictionaryCandidates(
      store, entry.reading, azookey::learning::LookupMode::Exact, 0, 10);
  ASSERT_EQ(candidates.size(), 1U);
  EXPECT_EQ(candidates[0].source, azookey::core::CandidateSource::UserDictionary);
  azookey::host::AssignHeuristicTags(candidates);
  EXPECT_EQ(candidates[0].tag, CandidateTag::Technical);
}

TEST(CandidateTagsTest, BoostMovesOnlyTheBoostedCandidatesUp) {
  std::vector<Candidate> candidates = {
      Tagged("a", 10.0, CandidateTag::None),
      Tagged("b", 8.0, CandidateTag::None),
      Tagged("c", 6.0, CandidateTag::English),
      Tagged("d", 4.0, CandidateTag::None),
  };
  TagBoosts boosts;
  boosts.multipliers[static_cast<size_t>(CandidateTag::English)] = 1.5;
  azookey::host::ApplyTagBoosts(candidates, boosts);
  EXPECT_EQ(Surfaces(candidates), (std::vector<std::string>{"a", "c", "b", "d"}));
  EXPECT_DOUBLE_EQ(candidates[1].score, 9.0);
}

TEST(CandidateTagsTest, NegativeScoresAreRaisedTowardZero) {
  std::vector<Candidate> candidates = {
      Tagged("a", -2.0, CandidateTag::None),
      Tagged("b", -3.0, CandidateTag::English),
  };
  TagBoosts boosts;
  boosts.multipliers[static_cast<size_t>(CandidateTag::English)] = 2.0;
  azookey::host::ApplyTagBoosts(candidates, boosts);
  EXPECT_EQ(Surfaces(candidates), (std::vector<std::string>{"b", "a"}));
  EXPECT_DOUBLE_EQ(candidates[0].score, -1.5);
}

TEST(CandidateTagsTest, NeutralBoostsLeaveUnsortedInputAlone) {
  std::vector<Candidate> candidates = {
      Tagged("low", 1.0, CandidateTag::English),
      Tagged("high", 5.0, CandidateTag::None),
  };
  azookey::host::ApplyTagBoosts(candidates, TagBoosts{});
  EXPECT_EQ(Surfaces(candidates), (std::vector<std::string>{"low", "high"}));
  EXPECT_DOUBLE_EQ(candidates[0].score, 1.0);
}

}  // namespace
