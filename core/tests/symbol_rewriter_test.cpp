#include <gtest/gtest.h>

#include "azookey/core/BracketPairing.h"
#include "azookey/core/SymbolRewriter.h"
#include "azookey/core/Utf8.h"

namespace azookey::core {
namespace {
Candidate MakeCandidate(std::string surface, CandidateSource source = CandidateSource::Model) {
  Candidate candidate;
  candidate.surface = std::move(surface);
  candidate.source = source;
  candidate.description = "original annotation";
  return candidate;
}
std::vector<std::string> Surfaces(const std::vector<Candidate>& candidates) {
  std::vector<std::string> result;
  for (const auto& candidate : candidates) result.push_back(candidate.surface);
  return result;
}

TEST(SymbolRewriter, ExpandsFirstSeedInFixedOrderAndPreservesOriginals) {
  std::vector<Candidate> candidates{MakeCandidate("通常"), MakeCandidate("「"), MakeCandidate("【"),
                                    MakeCandidate("）」")};
  AppendSymbolChain(candidates, "かっこ");
  EXPECT_EQ(Surfaces(candidates), (std::vector<std::string>{"通常", "「", "【", "）」", "『", "〔",
                                                            "（", "［", "｛", "〈", "《"}));
  EXPECT_EQ(candidates[1].source, CandidateSource::Model);
  EXPECT_EQ(candidates[1].description, "original annotation");
  for (size_t i = 4; i < candidates.size(); ++i) {
    EXPECT_EQ(candidates[i].source, CandidateSource::Symbol);
    EXPECT_EQ(candidates[i].reading, "かっこ");
    EXPECT_FALSE(candidates[i].description.empty());
    EXPECT_TRUE(candidates[i].debug_info.starts_with("symbol-rewriter:"));
  }
  const auto once = Surfaces(candidates);
  AppendSymbolChain(candidates, "かっこ");
  EXPECT_EQ(Surfaces(candidates), once);
}

TEST(SymbolRewriter, DoesNotRotateFamilyOrExpandASecondFamily) {
  std::vector<Candidate> candidates{MakeCandidate("（"), MakeCandidate("」")};
  AppendSymbolChain(candidates, "かっこ");
  EXPECT_EQ(Surfaces(candidates),
            (std::vector<std::string>{"（", "」", "「", "『", "【", "〔", "［", "｛", "〈", "《"}));
}

TEST(SymbolRewriter, PairFamilyMatchesBuiltinPairingWithoutMixingSingleBrackets) {
  std::vector<Candidate> candidates{MakeCandidate("「」"), MakeCandidate("『』")};
  AppendSymbolChain(candidates, "かぎかっこ");
  ASSERT_EQ(candidates.size(), 9u);
  for (const auto& candidate : candidates) {
    size_t offset = 0;
    char32_t open{}, close{};
    ASSERT_TRUE(DecodeNextUtf8(candidate.surface, offset, open));
    ASSERT_TRUE(DecodeNextUtf8(candidate.surface, offset, close));
    EXPECT_EQ(offset, candidate.surface.size());
    const auto pair = LookupBracketPair(open);
    ASSERT_TRUE(pair);
    EXPECT_EQ(pair->close, close);
  }
}

TEST(SymbolRewriter, ClosingFamilyAndNonSeedInputs) {
  std::vector<Candidate> candidates{MakeCandidate("）")};
  AppendSymbolChain(candidates, "かっこ");
  EXPECT_EQ(Surfaces(candidates),
            (std::vector<std::string>{"）", "」", "』", "】", "〕", "］", "｝", "〉", "》"}));
  candidates = {MakeCandidate("文「」"), MakeCandidate("")};
  AppendSymbolChain(candidates, "かっこ");
  EXPECT_EQ(candidates.size(), 2u);
}

TEST(RewriterMerge, ReservesOrdinaryCandidateAndDropsEmojiFirst) {
  const std::vector<Candidate> ordinary{MakeCandidate("変換1"), MakeCandidate("変換2")};
  const std::vector<Candidate> symbols{MakeCandidate("「"), MakeCandidate("『")};
  const std::vector<Candidate> emoji{MakeCandidate("😄"), MakeCandidate("😊")};
  EXPECT_EQ(Surfaces(MergeRewriterCandidates(ordinary, symbols, emoji, 1)),
            (std::vector<std::string>{"変換1"}));
  EXPECT_EQ(Surfaces(MergeRewriterCandidates(ordinary, symbols, emoji, 3)),
            (std::vector<std::string>{"変換1", "「", "『"}));
  EXPECT_EQ(Surfaces(MergeRewriterCandidates(ordinary, symbols, emoji, 4)),
            (std::vector<std::string>{"変換1", "「", "『", "😄"}));
  EXPECT_EQ(Surfaces(MergeRewriterCandidates({}, symbols, emoji, 1)),
            (std::vector<std::string>{"「"}));
}

TEST(RewriterMerge, DeduplicatesTailsAndCapsEachEvenWithUnlimitedTotal) {
  const std::vector<Candidate> ordinary{MakeCandidate("1"), MakeCandidate("2")};
  const std::vector<Candidate> symbols{MakeCandidate("1"), MakeCandidate("a"), MakeCandidate("a"),
                                       MakeCandidate("b"), MakeCandidate("c"), MakeCandidate("d"),
                                       MakeCandidate("e")};
  const std::vector<Candidate> emoji{MakeCandidate("b"), MakeCandidate("x"), MakeCandidate("y"),
                                     MakeCandidate("z"), MakeCandidate("w"), MakeCandidate("v")};
  EXPECT_EQ(Surfaces(MergeRewriterCandidates(ordinary, symbols, emoji, 0)),
            (std::vector<std::string>{"1", "2", "a", "b", "c", "d", "x", "y", "z", "w"}));
  EXPECT_EQ(Surfaces(MergeRewriterCandidates(ordinary, {}, {}, 1)),
            (std::vector<std::string>{"1"}));
}

std::vector<Candidate> Series(const std::string& prefix, size_t count,
                              CandidateSource source = CandidateSource::Model) {
  std::vector<Candidate> result;
  for (size_t i = 0; i < count; ++i)
    result.push_back(MakeCandidate(prefix + std::to_string(i), source));
  return result;
}

// Spec 9.5 / 19.5: the Host asks for max_candidates + reserve, where reserve is 4 per enabled
// rewriter, so the ordinary count never depends on which rewriters are enabled.
TEST(RewriterMerge, OrdinaryCountAndOrderAreInvariantAcrossEnablementCombinations) {
  for (const size_t ordinary_count : {1u, 2u, 3u, 5u, 9u, 10u, 20u}) {
    const auto ordinary = Series("o", ordinary_count);
    for (const bool symbol_on : {false, true}) {
      for (const bool emoji_on : {false, true}) {
        const size_t reserve = (symbol_on ? 4u : 0u) + (emoji_on ? 4u : 0u);
        const auto merged = MergeRewriterCandidates(
            ordinary,
            symbol_on ? Series("s", 6, CandidateSource::Symbol) : std::vector<Candidate>{},
            emoji_on ? Series("e", 6, CandidateSource::Emoji) : std::vector<Candidate>{},
            ordinary_count + reserve);
        std::vector<std::string> expected = Surfaces(ordinary);
        if (symbol_on) expected.insert(expected.end(), {"s0", "s1", "s2", "s3"});
        if (emoji_on) expected.insert(expected.end(), {"e0", "e1", "e2", "e3"});
        EXPECT_EQ(Surfaces(merged), expected)
            << "ordinary=" << ordinary_count << " symbol=" << symbol_on << " emoji=" << emoji_on;
      }
    }
  }
}

TEST(RewriterMerge, TailsFollowOrdinaryInSymbolThenEmojiOrderWithSources) {
  const auto merged =
      MergeRewriterCandidates(Series("o", 2), Series("s", 4, CandidateSource::Symbol),
                              Series("e", 4, CandidateSource::Emoji), 10);
  ASSERT_EQ(merged.size(), 10u);
  for (size_t i = 0; i < 2; ++i) EXPECT_EQ(merged[i].source, CandidateSource::Model);
  for (size_t i = 2; i < 6; ++i) EXPECT_EQ(merged[i].source, CandidateSource::Symbol);
  for (size_t i = 6; i < 10; ++i) EXPECT_EQ(merged[i].source, CandidateSource::Emoji);
}

TEST(RewriterMerge, CrossTailDuplicatesAreDroppedFromTheLaterTail) {
  const auto merged = MergeRewriterCandidates(Series("o", 1), {MakeCandidate("☺")},
                                              {MakeCandidate("☺"), MakeCandidate("😄")}, 9);
  EXPECT_EQ(Surfaces(merged), (std::vector<std::string>{"o0", "☺", "😄"}));
}

TEST(RewriterMerge, BothTailsEmptyLeavesOrdinaryUntouched) {
  const auto ordinary = Series("o", 3);
  EXPECT_EQ(Surfaces(MergeRewriterCandidates(ordinary, {}, {}, 3)), Surfaces(ordinary));
  EXPECT_EQ(Surfaces(MergeRewriterCandidates(ordinary, {}, {}, 0)), Surfaces(ordinary));
  EXPECT_TRUE(MergeRewriterCandidates({}, {}, {}, 5).empty());
}

// Spec 19.4 / 19.5: the chain runs on the Host response, after the tails have been merged.
TEST(RewriterMerge, ChainExpandsTheMergedListAndDropsFamilyMembersAlreadyPresent) {
  std::vector<Candidate> merged =
      MergeRewriterCandidates({MakeCandidate("かぎかっこ")},
                              {MakeCandidate("「」", CandidateSource::Symbol),
                               MakeCandidate("『』", CandidateSource::Symbol)},
                              {MakeCandidate("😄", CandidateSource::Emoji)}, 9);
  AppendSymbolChain(merged, "かぎかっこ");
  EXPECT_EQ(Surfaces(merged),
            (std::vector<std::string>{"かぎかっこ", "「」", "『』", "😄", "【】", "〔〕", "（）",
                                      "［］", "｛｝", "〈〉", "《》"}));
  // The reserved tail count is not affected by the chain candidates appended afterwards.
  EXPECT_EQ(merged.size(), 4u + 7u);
}

TEST(RewriterMerge, ChainStillWorksWhenNeitherDataFileProducedCandidates) {
  // Missing symbol and emoji data yield empty tails; a system-dictionary symbol is the seed.
  std::vector<Candidate> merged = MergeRewriterCandidates(
      {MakeCandidate("通常"), MakeCandidate("「", CandidateSource::SystemDictionary)}, {}, {}, 2);
  AppendSymbolChain(merged, "かっこ");
  ASSERT_EQ(merged.size(), 2u + 8u);
  EXPECT_EQ(merged[2].surface, "『");
  EXPECT_EQ(merged[2].source, CandidateSource::Symbol);
}

TEST(SymbolRewriter, DoesNotExpandEmojiOnlyOrPlainCandidates) {
  std::vector<Candidate> candidates{MakeCandidate("😄", CandidateSource::Emoji),
                                    MakeCandidate("文字")};
  AppendSymbolChain(candidates, "わらい");
  EXPECT_EQ(Surfaces(candidates), (std::vector<std::string>{"😄", "文字"}));
}
}  // namespace
}  // namespace azookey::core
