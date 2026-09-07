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
}  // namespace
}  // namespace azookey::core
