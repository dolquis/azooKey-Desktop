#include "azookey/learning/Persona.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using azookey::learning::ComputePersona;
using azookey::learning::LearningAggregate;

LearningAggregate Pair(const std::string& surface, uint64_t commits) {
  LearningAggregate aggregate;
  aggregate.reading = "よみ";
  aggregate.surface = surface;
  aggregate.commit_count = commits;
  return aggregate;
}

TEST(PersonaTest, EmptyStoreHasNoSamplesAndZeroRatios) {
  const auto persona = ComputePersona({});
  EXPECT_EQ(persona.sample_count, 0u);
  EXPECT_DOUBLE_EQ(persona.polite_ratio, 0.0);
  EXPECT_DOUBLE_EQ(persona.casual_ratio, 0.0);
  EXPECT_DOUBLE_EQ(persona.technical_ratio, 0.0);
  EXPECT_DOUBLE_EQ(persona.kaomoji_ratio, 0.0);
}

TEST(PersonaTest, RatiosAreCommitWeightedSharesOfTheTotal) {
  const auto persona = ComputePersona({
      Pair("よろしくお願いします", 3),  // polite
      Pair("そうだよ", 1),              // casual
      Pair("user_id", 2),               // technical
      Pair("(^_^)", 1),                 // kaomoji
      Pair("日本語", 3),                // none
  });
  EXPECT_EQ(persona.sample_count, 10u);
  EXPECT_DOUBLE_EQ(persona.polite_ratio, 0.3);
  EXPECT_DOUBLE_EQ(persona.casual_ratio, 0.1);
  EXPECT_DOUBLE_EQ(persona.technical_ratio, 0.2);
  EXPECT_DOUBLE_EQ(persona.kaomoji_ratio, 0.1);
}

TEST(PersonaTest, PairsWithoutCommitsAreLeftOut) {
  // A pair only ever rejected has no commit to count.
  const auto persona = ComputePersona({Pair("ですね", 0), Pair("日本", 1)});
  EXPECT_EQ(persona.sample_count, 1u);
  EXPECT_DOUBLE_EQ(persona.polite_ratio, 0.0);
}

TEST(PersonaTest, ClassifiesEachMarkerOfTheSpec) {
  for (const char* polite : {"行きます", "そうです", "いただきます", "いただいた"}) {
    EXPECT_DOUBLE_EQ(ComputePersona({Pair(polite, 1)}).polite_ratio, 1.0) << polite;
  }
  for (const char* casual : {"いいだよ", "そうだね", "いいじゃん"}) {
    EXPECT_DOUBLE_EQ(ComputePersona({Pair(casual, 1)}).casual_ratio, 1.0) << casual;
  }
  for (const char* technical : {"HttpClient", "max_len", "v2", "__init__"}) {
    EXPECT_DOUBLE_EQ(ComputePersona({Pair(technical, 1)}).technical_ratio, 1.0) << technical;
  }
  // One character, no letter, or a non-identifier character.
  for (const char* other : {"a", "123", "__", "foo-bar", "user id", "ｕｓｅｒ"}) {
    EXPECT_DOUBLE_EQ(ComputePersona({Pair(other, 1)}).technical_ratio, 0.0) << other;
  }
}

TEST(PersonaTest, KaomojiIsARunOfThreeToTenSymbols) {
  for (const char* kaomoji : {"(^_^)", "(・ω・)", "orz(>_<)", "！？！"}) {
    EXPECT_DOUBLE_EQ(ComputePersona({Pair(kaomoji, 1)}).kaomoji_ratio, 1.0) << kaomoji;
  }
  // Two symbols, symbols split by words, a run longer than ten, and sentence
  // punctuation.
  for (const char* other : {"!?", "a!b?c", "。日本。", "-----------", "……。", "」、「"}) {
    EXPECT_DOUBLE_EQ(ComputePersona({Pair(other, 1)}).kaomoji_ratio, 0.0) << other;
  }
}

TEST(PersonaTest, MalformedUtf8DoesNotStallTheScan) {
  const auto persona = ComputePersona({Pair(std::string("\xE3\x81", 2) + "!!!", 1)});
  EXPECT_EQ(persona.sample_count, 1u);
  // The two broken bytes read as symbols, so the run is five long.
  EXPECT_DOUBLE_EQ(persona.kaomoji_ratio, 1.0);
}

}  // namespace
