#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "azookey/host/CandidateTags.h"

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
