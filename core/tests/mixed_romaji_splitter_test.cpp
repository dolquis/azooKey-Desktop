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

// M63-B (sections 6.2 and 6.3): the mixed lattice.
azookey::core::MixedRomajiSplitOptions Inline() {
  azookey::core::MixedRomajiSplitOptions options;
  options.split_inline = true;
  return options;
}

std::string Describe(const azookey::core::MixedRomajiSplit& split) {
  std::string out;
  for (const auto& s : split.segments) {
    if (!out.empty()) out += " | ";
    if (s.kind == MixedRomajiSegmentKind::English) {
      out +=
          "E:" + s.surface + (s.confidence == EnglishSegmentConfidence::High ? "(high)" : "(low)");
    } else {
      out += "J:" + s.kana;
    }
  }
  return out;
}

TEST(MixedRomajiSplitterTest, LatticeSplitsASentenceAroundTheEnglishWords) {
  const auto dictionary =
      Dictionary({{"github", {"GitHub", "github"}}, {"push", {"push"}}, {"you", {"you"}}});
  const auto splits = SplitMixedRomaji("kyouhagithubnipushsita", dictionary, Inline());
  ASSERT_FALSE(splits.empty());
  EXPECT_EQ(Describe(splits.front()), "J:きょうは | E:GitHub(high) | J:に | E:push(high) | J:した");
  const auto& segments = splits.front().segments;
  ASSERT_EQ(segments.size(), 5u);
  EXPECT_EQ(segments[1].raw_begin, 6u);
  EXPECT_EQ(segments[1].raw_end, 12u);
  EXPECT_EQ(segments[4].raw_end, 22u);
  // Typed capitals keep the keystrokes (section 6.4).
  const auto typed = SplitMixedRomaji("kyouhaGitHubnipushsita", dictionary, Inline());
  ASSERT_FALSE(typed.empty());
  EXPECT_EQ(Describe(typed.front()), "J:きょうは | E:GitHub(high) | J:に | E:push(high) | J:した");
}

TEST(MixedRomajiSplitterTest, LatticeKeepsAJapaneseSentenceJapanese) {
  // you inside kyou leaves k alone, and a lone ASCII letter costs far more.
  const auto dictionary = Dictionary({{"you", {"you"}}, {"wot", {"wot"}}, {"tab", {"tab"}}});
  for (const char* raw : {"kyouha", "gohannwotaberu"}) {
    const auto splits = SplitMixedRomaji(raw, dictionary, Inline());
    ASSERT_FALSE(splits.empty()) << raw;
    ASSERT_EQ(splits.front().segments.size(), 1u) << Describe(splits.front());
    EXPECT_EQ(splits.front().segments.front().kind, MixedRomajiSegmentKind::Japanese) << raw;
    EXPECT_DOUBLE_EQ(splits.front().cost, 0.0) << raw;
  }
}

TEST(MixedRomajiSplitterTest, LatticeCostsFollowSection63) {
  // A word that reads as kana pays the penalty and stays J first, with the
  // low-confidence English split behind it.
  const auto kite = SplitMixedRomaji("kitekudasai", Dictionary({{"kite", {"kite"}}}), Inline());
  ASSERT_GE(kite.size(), 2u);
  EXPECT_EQ(Describe(kite.front()), "J:きてください");
  EXPECT_EQ(Describe(kite[1]), "E:kite(low) | J:ください");
  EXPECT_NEAR(kite[1].cost, 1.0 - 0.05 * 4 + 2.0, 1e-9);
  // Unnatural kana in the J reading (konoぉゔぇは) make the English split win
  // even for a word that reads as kana.
  const auto love = SplitMixedRomaji("konoloveha", Dictionary({{"love", {"love"}}}), Inline());
  ASSERT_FALSE(love.empty());
  EXPECT_EQ(Describe(love.front()), "J:この | E:love(low) | J:は");
  // Splits come cheapest first, at most max_splits of them, and no two J
  // segments are adjacent.
  auto options = Inline();
  options.max_splits = 3;
  const auto many = SplitMixedRomaji(
      "pushsitecommitsita",
      Dictionary({{"push", {"push"}}, {"commit", {"commit"}}, {"site", {"site"}}}), options);
  ASSERT_EQ(many.size(), 3u);
  for (size_t i = 1; i < many.size(); ++i) EXPECT_LE(many[i - 1].cost, many[i].cost);
  for (const auto& split : many) {
    for (size_t i = 1; i < split.segments.size(); ++i)
      EXPECT_FALSE(split.segments[i - 1].kind == MixedRomajiSegmentKind::Japanese &&
                   split.segments[i].kind == MixedRomajiSegmentKind::Japanese)
          << Describe(split);
  }
}

// The boundary and capital costs the quality gate needed on top of section 6.3.
TEST(MixedRomajiSplitterTest, LatticeBoundaryAndCapitalCosts) {
  // A J segment that starts with a vowel right after an E segment means the E
  // edge took the consonant of the kana: pull|sita, not pulls|ita.
  EXPECT_EQ(
      Describe(SplitMixedRomaji("kinoupullsita",
                                Dictionary({{"pull", {"pull"}}, {"pulls", {"pulls"}}}), Inline())
                   .front()),
      "J:きのう | E:pull(high) | J:した");
  // No Japanese word starts with ん: scan|nohanasi, not tosca|nnohanasi.
  EXPECT_EQ(
      Describe(SplitMixedRomaji("tomodatitoscannohanasi",
                                Dictionary({{"scan", {"scan"}}, {"tosca", {"Tosca"}}}), Inline())
                   .front()),
      "J:ともだちと | E:scan(high) | J:のはなし");
  // A Shift capital in a J segment costs, and the readable penalty does not
  // apply to a word that starts with one.
  EXPECT_EQ(
      Describe(SplitMixedRomaji("Sitewomite", Dictionary({{"site", {"site"}}}), Inline()).front()),
      "E:Site(high) | J:をみて");
}

// Decision (a): an in-sentence E edge needs 3 characters, 2 when it starts
// with a Shift capital.
TEST(MixedRomajiSplitterTest, LatticeMinimumLengths) {
  const auto dictionary = Dictionary({{"pc", {"PC"}}});
  const auto lower = SplitMixedRomaji("pcwotukau", dictionary, Inline());
  ASSERT_EQ(lower.size(), 1u);
  EXPECT_EQ(lower.front().segments.front().kind, MixedRomajiSegmentKind::Japanese);
  const auto shifted = SplitMixedRomaji("PCwotukau", dictionary, Inline());
  ASSERT_FALSE(shifted.empty());
  EXPECT_EQ(Describe(shifted.front()), "E:PC(high) | J:をつかう");
}

TEST(MixedRomajiSplitterTest, LatticeIsOptInAndHasAKillSwitch) {
  const auto dictionary = Dictionary({{"github", {"GitHub"}}});
  // Off (the default): the M63-A whole-composition judgement only.
  const auto off = SplitMixedRomaji("kyouhagithubni", dictionary);
  ASSERT_EQ(off.size(), 1u);
  EXPECT_EQ(off.front().segments.front().kind, MixedRomajiSegmentKind::Japanese);
  // Without a dictionary there is no E edge to split on (section 6.1).
  EXPECT_EQ(Describe(SplitMixedRomaji("kyouhagithubni", EnglishWordLookup{}, Inline()).front()),
            "J:きょうはぎてゅbに");
  // Section 15.2: the kill switch leaves every lattice E low.
  auto options = Inline();
  options.inline_high_confidence = false;
  EXPECT_EQ(Describe(SplitMixedRomaji("kyouhagithubni", dictionary, options).front()),
            "J:きょうは | E:GitHub(low) | J:に");
  // A whole-composition word keeps the M63-A judgement with the lattice on.
  EXPECT_EQ(Describe(SplitMixedRomaji("github", dictionary, Inline()).front()), "E:GitHub(high)");
  // A Shift capital first that the lattice leaves all J is still one E
  // segment (section 6.1), as with the lattice off.
  EXPECT_EQ(Describe(SplitMixedRomaji("Kyouha", dictionary, Inline()).front()), "E:Kyouha(high)");
}

// Section 8.3 item 3 (DEV-1820): a readable word inside a J segment of the
// adopted split, turned English with everything else as adopted.
std::vector<std::string> Alternatives(std::string_view raw, const EnglishWordLookup& lookup,
                                      const azookey::core::MixedRomajiSplitOptions& options) {
  const auto splits = SplitMixedRomaji(raw, lookup, options);
  std::vector<std::string> out;
  if (splits.empty()) return out;
  for (const auto& alternative :
       azookey::core::ReadableEnglishAlternatives(raw, splits.front(), lookup, options)) {
    const auto& english = alternative.split.segments[alternative.english_segment];
    EXPECT_EQ(english.kind, MixedRomajiSegmentKind::English) << raw;
    EXPECT_EQ(english.confidence, EnglishSegmentConfidence::Low) << raw;
    out.push_back(Describe(alternative.split));
  }
  return out;
}

TEST(MixedRomajiSplitterTest, ReadableWordInAJapaneseSegmentTurnsEnglishOnItsOwn) {
  const auto dictionary = Dictionary({{"sit", {"sit"}}, {"kit", {"kit"}}, {"kite", {"kite"}}});
  const auto splits = SplitMixedRomaji("asitakitekudasai", dictionary, Inline());
  ASSERT_FALSE(splits.empty());
  EXPECT_EQ(Describe(splits.front()), "J:あしたきてください");
  // Trap 1: the cheaper paths through sit and kit (a|sit|akitekudasai) leave
  // ASCII in their kana, so only kite, with the J segment cut at its kana.
  const auto alternatives = azookey::core::ReadableEnglishAlternatives(
      "asitakitekudasai", splits.front(), dictionary, Inline());
  ASSERT_EQ(alternatives.size(), 1u);
  EXPECT_EQ(Describe(alternatives[0].split), "J:あした | E:kite(low) | J:ください");
  EXPECT_EQ(alternatives[0].english_segment, 1u);
  EXPECT_EQ(alternatives[0].split.segments[1].raw_begin, 5u);
  EXPECT_EQ(alternatives[0].split.segments[1].raw_end, 9u);
  // The path costs the E edge alone more than the adopted one.
  EXPECT_NEAR(alternatives[0].split.cost, splits.front().cost + 1.0 - 0.05 * 4 + 2.0, 1e-9);
  // In a sentence with E segments too, the other segments stay as adopted.
  EXPECT_EQ(Alternatives("githubmadeni", Dictionary({{"github", {"GitHub"}}, {"made", {"made"}}}),
                         Inline()),
            std::vector<std::string>{"E:GitHub(high) | E:made(low) | J:に"});
  EXPECT_EQ(Alternatives("asitamadeni", Dictionary({{"made", {"made"}}}), Inline()),
            std::vector<std::string>{"J:あした | E:made(low) | J:に"});
}

// Trap 2: most Japanese sentences hold a readable word, so only a common word
// of 4 letters or more whose kana and costs cut the J segment cleanly.
TEST(MixedRomajiSplitterTest, ReadableWordsNeedAClean4LetterCommonWord) {
  const auto none = std::vector<std::string>{};
  // you inside kyou leaves k alone in the kana.
  EXPECT_EQ(Alternatives("kyouhaamegahuru", Dictionary({{"you", {"you"}}}), Inline()), none);
  // A J piece starting with a vowel right after the word pays the boundary
  // cost (eki|made|aruita).
  EXPECT_EQ(Alternatives("ekimadearuita", Dictionary({{"made", {"made"}}}), Inline()), none);
  // Costs that would only offset one another: each of these passes every other
  // check, and the J pieces cost no more than the J segment.
  // - A word that starts a J segment paying the vowel cost after excel would
  //   drop that cost (excel|area|niiku).
  EXPECT_EQ(
      Describe(SplitMixedRomaji("excelareaniiku",
                                Dictionary({{"excel", {"Excel"}}, {"area", {"area"}}}), Inline())
                   .front()),
      "E:Excel(high) | J:あれあにいく");
  EXPECT_EQ(Alternatives("excelareaniiku", Dictionary({{"excel", {"Excel"}}, {"area", {"area"}}}),
                         Inline()),
            none);
  // - A typed capital costs in the J segment (asita|kiTe|kudasai).
  EXPECT_EQ(
      Describe(
          SplitMixedRomaji("asitakiTekudasai", Dictionary({{"kite", {"kite"}}}), Inline()).front()),
      "J:あしたきてください");
  EXPECT_EQ(Alternatives("asitakiTekudasai", Dictionary({{"kite", {"kite"}}}), Inline()), none);
  // - An unnatural kana (ゔ) costs in the J segment (ano|vane|ga).
  EXPECT_EQ(
      Describe(SplitMixedRomaji("anovanega", Dictionary({{"vane", {"vane"}}}), Inline()).front()),
      "J:あのゔぁねが");
  EXPECT_EQ(Alternatives("anovanega", Dictionary({{"vane", {"vane"}}}), Inline()), none);
  // Three letters are too short; a proper noun or acronym only is no common
  // word; a Shift capital first is the lattice's own call.
  EXPECT_EQ(Alternatives("kenwomiru", Dictionary({{"ken", {"ken"}}}), Inline()), none);
  EXPECT_EQ(Alternatives("kimononiau", Dictionary({{"mono", {"Mono"}}}), Inline()), none);
  const auto flagged = [](std::string_view key) {
    return key == "mono" ? std::vector<EnglishWordMatch>{{"mono", 1}}
                         : std::vector<EnglishWordMatch>{};
  };
  EXPECT_EQ(Alternatives("kimononiau", flagged, Inline()), none);
  EXPECT_EQ(Alternatives("hayakuKitekudasai", Dictionary({{"kite", {"kite"}}}), Inline()).size(),
            0u);
  // The whole composition is M63-A, and the lattice off or no dictionary has
  // none.
  EXPECT_EQ(Alternatives("kite", Dictionary({{"kite", {"kite"}}}), Inline()), none);
  const auto splits = SplitMixedRomaji("asitakitekudasai", Dictionary({{"kite", {"kite"}}}));
  ASSERT_FALSE(splits.empty());
  EXPECT_TRUE(azookey::core::ReadableEnglishAlternatives("asitakitekudasai", splits.front(),
                                                         Dictionary({{"kite", {"kite"}}}))
                  .empty());
  EXPECT_TRUE(azookey::core::ReadableEnglishAlternatives("asitakitekudasai", splits.front(),
                                                         EnglishWordLookup{}, Inline())
                  .empty());
}

}  // namespace
