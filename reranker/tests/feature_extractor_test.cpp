#include <gtest/gtest.h>

#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>

#include "azookey/ipc/Json.h"
#include "azookey/reranker/FeatureExtractor.h"

namespace azookey::reranker {
namespace {

TEST(FeatureExtractorTest, MatchesFeaturesV1GoldenFixture) {
  std::ifstream file(AZOOKEY_FEATURES_V1_GOLDEN, std::ios::binary);
  ASSERT_TRUE(file.is_open());
  const std::string json{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  const auto fixture = ipc::json::Parse(json);
  ASSERT_TRUE(fixture && fixture->IsObject());
  const auto* names = fixture->GetArray("feature_names");
  ASSERT_NE(names, nullptr);
  ASSERT_EQ(names->size(), kFeatureNamesV1.size());
  for (size_t i = 0; i < names->size(); ++i) {
    ASSERT_TRUE(names->at(i).IsString());
    EXPECT_EQ(names->at(i).AsString(), kFeatureNamesV1[i]);
  }

  const auto* cases = fixture->GetArray("cases");
  ASSERT_NE(cases, nullptr);
  ASSERT_FALSE(cases->empty());
  for (const auto& row : *cases) {
    ASSERT_TRUE(row.IsObject());
    const auto name = row.GetString("name");
    const auto surface = row.GetString("surface");
    ASSERT_TRUE(name.has_value());
    ASSERT_TRUE(surface.has_value());
    SCOPED_TRACE(*name);
    for (const auto& [key, value] : row.AsObject()) {
      if (key == "name" || key == "surface") {
        ASSERT_TRUE(value.IsString()) << key;
      } else if (key == "expected") {
        ASSERT_TRUE(value.IsArray()) << key;
      } else if (key == "lattice_segment_count") {
        const auto count = row.GetUInt(key);
        ASSERT_TRUE(count && *count <= (std::numeric_limits<size_t>::max)()) << key;
      } else if (key == "zenzai_avg_logprob" || key == "dictionary_score" ||
                 key == "typo_confidence" || key == "user_frequency" || key == "recency_score") {
        ASSERT_TRUE(value.IsNumber()) << key;
      } else if (key == "is_named_entity" || key == "is_user_dict" || key == "is_neologism" ||
                 key == "is_typo_corrected") {
        ASSERT_TRUE(value.IsBool()) << key;
      } else {
        FAIL() << "Unknown golden case key: " << key;
      }
    }

    RerankSignals signals{};
    signals.zenzai_avg_logprob = row.GetNumber("zenzai_avg_logprob");
    signals.dictionary_score = row.GetNumber("dictionary_score");
    signals.typo_confidence = row.GetNumber("typo_confidence");
    signals.candidate_length = core::ComputeCandidateLength(*surface);
    if (const auto segment_count = row.GetUInt("lattice_segment_count")) {
      signals.lattice_segment_count = static_cast<size_t>(*segment_count);
    }
    signals.is_named_entity = row.GetBool("is_named_entity").value_or(false);
    signals.is_user_dict = row.GetBool("is_user_dict").value_or(false);
    signals.is_neologism = row.GetBool("is_neologism").value_or(false);
    signals.is_typo_corrected = row.GetBool("is_typo_corrected").value_or(false);
    UserSignals user{};
    if (const auto frequency = row.GetNumber("user_frequency")) user.user_frequency = *frequency;
    if (const auto recency = row.GetNumber("recency_score")) user.recency_score = *recency;

    const auto actual = ExtractFeaturesV1(signals, user);
    const auto* expected = row.GetArray("expected");
    ASSERT_NE(expected, nullptr);
    ASSERT_EQ(expected->size(), actual.size());
    for (size_t i = 0; i < actual.size(); ++i) {
      ASSERT_TRUE(expected->at(i).IsNumber());
      EXPECT_EQ(actual[i], static_cast<float>(expected->at(i).AsNumber())) << kFeatureNamesV1[i];
    }
  }
}

TEST(FeatureExtractorTest, DuplicateSurfaceRetainsSignalsFromBothSources) {
  core::Candidate dictionary;
  dictionary.surface = "変換候補";
  dictionary.rerank_signals = {.dictionary_score = 0.75,
                               .lattice_segment_count = 2,
                               .is_named_entity = true,
                               .is_user_dict = true};
  core::Candidate model;
  model.surface = dictionary.surface;
  model.rerank_signals = {.zenzai_avg_logprob = -1.25, .is_neologism = true};

  auto dictionary_wins = dictionary;
  ASSERT_EQ(dictionary_wins.rerank_signals.candidate_length, 0U);
  core::PreserveRerankSignals(dictionary_wins, model);
  auto model_wins = model;
  core::PreserveRerankSignals(model_wins, dictionary);

  const std::array<float, 12> expected{-1.25f, 0.75f, 0, 0, 0, 0, 4, 2, 1, 1, 1, 0};
  EXPECT_EQ(ExtractFeaturesV1(dictionary_wins.rerank_signals, {}), expected);
  EXPECT_EQ(ExtractFeaturesV1(model_wins.rerank_signals, {}), expected);

  auto preferred = dictionary;
  preferred.rerank_signals.dictionary_score = 0;
  preferred.rerank_signals.lattice_segment_count = 1;
  core::PreserveRerankSignals(preferred, dictionary);
  EXPECT_EQ(preferred.rerank_signals.dictionary_score, 0.0);
  EXPECT_EQ(preferred.rerank_signals.lattice_segment_count, 1U);
}

TEST(FeatureExtractorTest, LeavesInvalidInputsVisibleToTheScorer) {
  RerankSignals signals{};
  signals.zenzai_avg_logprob = std::numeric_limits<double>::quiet_NaN();
  signals.dictionary_score = std::numeric_limits<double>::max();
  const auto actual =
      ExtractFeaturesV1(signals, {.user_frequency = -std::numeric_limits<double>::infinity()});
  EXPECT_TRUE(std::isnan(actual[0]));
  EXPECT_EQ(actual[1], std::numeric_limits<float>::infinity());
  EXPECT_EQ(actual[2], -std::numeric_limits<float>::infinity());
}

}  // namespace
}  // namespace azookey::reranker
