#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "azookey/core/MixedRomajiSplitter.h"

namespace {

using azookey::core::EnglishSegmentConfidence;
using azookey::core::EnglishWordLookup;
using azookey::core::EnglishWordMatch;
using azookey::core::MixedRomajiSegment;
using azookey::core::MixedRomajiSegmentKind;
using azookey::core::SplitMixedRomaji;

EnglishWordLookup Dictionary(std::map<std::string, std::vector<std::string>> words,
                             std::vector<std::string>* keys = nullptr) {
  return [words = std::move(words), keys](std::string_view key) {
    if (keys) keys->emplace_back(key);
    std::vector<EnglishWordMatch> matches;
    if (const auto it = words.find(std::string(key)); it != words.end())
      for (const auto& surface : it->second) matches.push_back({surface, 0});
    return matches;
  };
}

MixedRomajiSegment Whole(std::string_view raw, const EnglishWordLookup& lookup) {
  const auto splits = SplitMixedRomaji(raw, lookup);
  EXPECT_EQ(splits.size(), 1u) << raw;
  if (splits.size() != 1u || splits.front().segments.size() != 1u) {
    ADD_FAILURE() << raw;
    return {};
  }
  const auto& segment = splits.front().segments.front();
  EXPECT_EQ(segment.raw_begin, 0u) << raw;
  EXPECT_EQ(segment.raw_end, raw.size()) << raw;
  return segment;
}

// Section 8.2: the whole composition is one E segment when it matches the
// dictionary or starts with a Shift capital, and section 8.1 decides how sure.
TEST(MixedRomajiSplitterTest, WholeCompositionFollowsSection82) {
  std::vector<std::string> keys;
  const auto dictionary = Dictionary(
      {{"github", {"GitHub", "github"}}, {"kite", {"kite"}}, {"apple", {"apple"}}}, &keys);

  // Letters left after romaji-to-kana conversion plus a match: high, and the
  // most frequent dictionary surface.
  auto github = Whole("github", dictionary);
  EXPECT_EQ(github.kind, MixedRomajiSegmentKind::English);
  EXPECT_EQ(github.surface, "GitHub");
  EXPECT_EQ(github.confidence, EnglishSegmentConfidence::High);

  // Reads as romaji: no section 6.3 penalty in M63-A, so E but low.
  auto kite = Whole("kite", dictionary);
  EXPECT_EQ(kite.kind, MixedRomajiSegmentKind::English);
  EXPECT_EQ(kite.surface, "kite");
  EXPECT_EQ(kite.confidence, EnglishSegmentConfidence::Low);

  // A Shift capital first is high and keeps the keystrokes (section 6.4).
  auto shifted = Whole("Kite", dictionary);
  EXPECT_EQ(shifted.kind, MixedRomajiSegmentKind::English);
  EXPECT_EQ(shifted.surface, "Kite");
  EXPECT_EQ(shifted.confidence, EnglishSegmentConfidence::High);

  // Capitals elsewhere keep the keystrokes; confidence comes from (ii).
  auto mixed_case = Whole("gItHub", dictionary);
  EXPECT_EQ(mixed_case.surface, "gItHub");
  EXPECT_EQ(mixed_case.confidence, EnglishSegmentConfidence::High);

  // No match and no Shift: one J segment with the built-in table's kana.
  auto japanese = Whole("kyouha", dictionary);
  EXPECT_EQ(japanese.kind, MixedRomajiSegmentKind::Japanese);
  EXPECT_EQ(japanese.kana, "きょうは");
  EXPECT_TRUE(japanese.surface.empty());

  // The dictionary is looked up by the lowercased key.
  EXPECT_EQ(std::find(keys.begin(), keys.end(), "gItHub"), keys.end()) << "not lowercased";
  EXPECT_EQ(std::count(keys.begin(), keys.end(), "github"), 2);
}

// Section 6.1: without a dictionary only a Shift capital first makes E.
TEST(MixedRomajiSplitterTest, WithoutADictionaryOnlyShiftMakesEnglish) {
  const EnglishWordLookup none;
  auto github = Whole("github", none);
  EXPECT_EQ(github.kind, MixedRomajiSegmentKind::Japanese);
  EXPECT_FALSE(github.kana.empty());
  auto shifted = Whole("GitHub", none);
  EXPECT_EQ(shifted.kind, MixedRomajiSegmentKind::English);
  EXPECT_EQ(shifted.surface, "GitHub");
  EXPECT_EQ(shifted.confidence, EnglishSegmentConfidence::High);
}

// M60 section 4.2 gating: below the minimum length, or without a letter, the
// composition stays J.
TEST(MixedRomajiSplitterTest, GatingKeepsShortOrLetterlessInputJapanese) {
  const auto dictionary = Dictionary({{"a", {"a"}}, {"go", {"go"}}});
  EXPECT_EQ(Whole("a", dictionary).kind, MixedRomajiSegmentKind::Japanese);
  EXPECT_EQ(Whole("A", dictionary).kind, MixedRomajiSegmentKind::Japanese);
  EXPECT_EQ(Whole("go", dictionary).kind, MixedRomajiSegmentKind::English);
  EXPECT_EQ(Whole("--", dictionary).kind, MixedRomajiSegmentKind::Japanese);
  azookey::core::MixedRomajiSplitOptions longer;
  longer.min_english_length = 3;
  const auto splits = SplitMixedRomaji("go", dictionary, longer);
  ASSERT_EQ(splits.size(), 1u);
  EXPECT_EQ(splits.front().segments.front().kind, MixedRomajiSegmentKind::Japanese);
}

// Section 10: over the limit, empty, or not printable ASCII is no split at
// all, which the caller handles as M63 off.
TEST(MixedRomajiSplitterTest, OutOfBoundsInputHasNoSplit) {
  const auto dictionary = Dictionary({{"github", {"GitHub"}}});
  const std::string limit(64, 'a');
  EXPECT_EQ(SplitMixedRomaji(limit, dictionary).size(), 1u);
  EXPECT_TRUE(SplitMixedRomaji(limit + "a", dictionary).empty());
  EXPECT_TRUE(SplitMixedRomaji("", dictionary).empty());
  EXPECT_TRUE(SplitMixedRomaji("git hub", dictionary).empty());
  EXPECT_TRUE(SplitMixedRomaji("git\thub", dictionary).empty());
  EXPECT_TRUE(SplitMixedRomaji("gith\xC3\xBC"
                               "b",
                               dictionary)
                  .empty());
  azookey::core::MixedRomajiSplitOptions none;
  none.max_splits = 0;
  EXPECT_TRUE(SplitMixedRomaji("github", dictionary, none).empty());
}

}  // namespace
