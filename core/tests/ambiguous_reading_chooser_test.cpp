#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "azookey/core/AmbiguousReadingChooser.h"
#include "azookey/core/RomajiKanaConverter.h"

namespace {

using azookey::core::ChooseAmbiguousReading;
using azookey::core::ReadingSplitCost;
using azookey::core::ReadingWordLookup;
using azookey::core::ReadingWordMatch;
using azookey::core::RomajiKanaConverter;

// A fake dictionary: reading -> frequency of its most common word.
ReadingWordLookup Dictionary(std::map<std::string, double> words) {
  return [words = std::move(words)](std::string_view suffix) {
    std::vector<ReadingWordMatch> matches;
    for (const auto& [reading, frequency] : words) {
      if (suffix.starts_with(reading)) matches.push_back({reading.size(), frequency});
    }
    return matches;
  };
}

const ReadingWordLookup kWords = Dictionary({
    {"ないぶ", 0.6},
    {"げんいん", 0.7},
    {"げんにん", 0.2},
    {"こんにちは", 0.8},
    {"こんい", 0.3},
    {"おんな", 0.7},
    {"みんな", 0.8},
    {"さんにん", 0.6},
    {"さんいん", 0.65},
    {"かんい", 0.5},
});

size_t Choose(const std::string& romaji) {
  return ChooseAmbiguousReading(RomajiKanaConverter::AmbiguousReadings(romaji), kWords);
}

}  // namespace

TEST(AmbiguousReadingChooserTest, SplitCostPrefersFewerCommonerWords) {
  // ないぶ (1 - 0.6 + 0.5) + げんいん (1 - 0.7 + 0.5).
  EXPECT_DOUBLE_EQ(ReadingSplitCost("ないぶげんいん", kWords), 0.9 + 0.8);
  // Three kana no word covers.
  EXPECT_DOUBLE_EQ(ReadingSplitCost("ぬぬぬ", kWords), 4.5);
  EXPECT_DOUBLE_EQ(ReadingSplitCost("", kWords), 0.0);
  EXPECT_DOUBLE_EQ(ReadingSplitCost("ぬぬ", {}), 3.0);
}

TEST(AmbiguousReadingChooserTest, ChoosesTheReadingTheDictionarySupports) {
  // げんいん is the commoner word, so ん + い wins (index 1).
  EXPECT_EQ(Choose("naibugenninn"), 1u);
  EXPECT_EQ(Choose("kanni"), 1u);
  // The default ん + な行 stays where only it forms words.
  EXPECT_EQ(Choose("konnichiha"), 0u);
  EXPECT_EQ(Choose("onna"), 0u);
  EXPECT_EQ(Choose("minna"), 0u);
}

TEST(AmbiguousReadingChooserTest, MarginKeepsTheDefaultWhenBothAreWords) {
  // さんいん is slightly commoner than さんにん, but not by more than the margin.
  EXPECT_EQ(Choose("sannin"), 0u);
  azookey::core::AmbiguousReadingCosts no_margin;
  no_margin.margin = 0.0;
  EXPECT_EQ(
      ChooseAmbiguousReading(RomajiKanaConverter::AmbiguousReadings("sannin"), kWords, no_margin),
      1u);
}

TEST(AmbiguousReadingChooserTest, PicksTheCheapestOfEveryReading) {
  // Readings: おんなげんにん, おんあげんにん, おんなげんいん, おんあげんいん.
  const auto readings = RomajiKanaConverter::AmbiguousReadings("onnagenninn");
  ASSERT_EQ(readings.size(), 4u);
  EXPECT_EQ(readings[ChooseAmbiguousReading(readings, kWords)], "おんなげんいん");
}

TEST(AmbiguousReadingChooserTest, NothingToChooseWithoutAlternativesOrDictionary) {
  EXPECT_EQ(ChooseAmbiguousReading({"げんにん"}, kWords), 0u);
  EXPECT_EQ(ChooseAmbiguousReading({}, kWords), 0u);
  EXPECT_EQ(ChooseAmbiguousReading({"げんにん", "げんいん"}, {}), 0u);
  // A match longer than the rest of the reading is ignored.
  const ReadingWordLookup overlong = [](std::string_view) {
    return std::vector<ReadingWordMatch>{{1000, 1.0}};
  };
  EXPECT_DOUBLE_EQ(ReadingSplitCost("ぬ", overlong), 1.5);
}
