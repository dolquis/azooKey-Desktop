#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "azookey/core/LiveConversionChunker.h"

namespace {

using azookey::core::LiveConversionChunk;
using azookey::core::LiveConversionChunkKind;

std::vector<std::string> ChunkTexts(const std::vector<LiveConversionChunk>& chunks) {
  std::vector<std::string> texts;
  for (const auto& chunk : chunks) {
    texts.push_back(chunk.text);
  }
  return texts;
}

std::vector<LiveConversionChunkKind> ChunkKinds(const std::vector<LiveConversionChunk>& chunks) {
  std::vector<LiveConversionChunkKind> kinds;
  for (const auto& chunk : chunks) {
    kinds.push_back(chunk.kind);
  }
  return kinds;
}

}  // namespace

TEST(LiveConversionChunkerTest, ClassifiesJapaneseCodePointsForLiveConversion) {
  EXPECT_TRUE(azookey::core::IsJapaneseForLiveConversion(U'あ'));
  EXPECT_TRUE(azookey::core::IsJapaneseForLiveConversion(U'ア'));
  EXPECT_TRUE(azookey::core::IsJapaneseForLiveConversion(U'日'));
  EXPECT_TRUE(azookey::core::IsJapaneseForLiveConversion(U'ー'));
  EXPECT_TRUE(azookey::core::IsJapaneseForLiveConversion(U'ｶ'));
  EXPECT_TRUE(azookey::core::IsJapaneseForLiveConversion(U'ﾞ'));
  EXPECT_TRUE(azookey::core::IsJapaneseForLiveConversion(U'ﾟ'));
  EXPECT_FALSE(azookey::core::IsJapaneseForLiveConversion(U'a'));
  EXPECT_FALSE(azookey::core::IsJapaneseForLiveConversion(U'1'));
  EXPECT_FALSE(azookey::core::IsJapaneseForLiveConversion(U'、'));
}

TEST(LiveConversionChunkerTest, GroupsJapaneseSpansAndVerbatimSpans) {
  const auto chunks = azookey::core::GroupLiveConversionChunks("今日はabc123、いい天気!");

  EXPECT_EQ(ChunkTexts(chunks), (std::vector<std::string>{"今日は", "abc123、", "いい天気", "!"}));
  EXPECT_EQ(ChunkKinds(chunks), (std::vector<LiveConversionChunkKind>{
                                    LiveConversionChunkKind::Convert,
                                    LiveConversionChunkKind::Verbatim,
                                    LiveConversionChunkKind::Convert,
                                    LiveConversionChunkKind::Verbatim,
                                }));
  ASSERT_EQ(chunks.size(), 4u);
  EXPECT_EQ(chunks[0].byte_offset, 0u);
  EXPECT_EQ(chunks[1].byte_offset, std::string("今日は").size());
}

TEST(LiveConversionChunkerTest, TreatsKanaExtensionsAsJapaneseButKeepsPunctuationVerbatim) {
  const auto chunks = azookey::core::GroupLiveConversionChunks("ｶﾞﾊﾟコーヒー!?");

  EXPECT_EQ(ChunkTexts(chunks), (std::vector<std::string>{"ｶﾞﾊﾟコーヒー", "!?"}));
  EXPECT_EQ(ChunkKinds(chunks), (std::vector<LiveConversionChunkKind>{
                                    LiveConversionChunkKind::Convert,
                                    LiveConversionChunkKind::Verbatim,
                                }));
}

TEST(LiveConversionChunkerTest, RejectsUtf8EncodedSurrogateAsByteGranularUnits) {
  std::string previous{"\xed\xa0\x80", 3};
  previous += 'a';
  std::string current{"\xed\xa0", 2};
  current += 'b';

  const auto plan = azookey::core::ComputeLiveConversionChunkPlan(previous, current);

  EXPECT_EQ(plan.unchanged_prefix_bytes, 2u);
  EXPECT_EQ(plan.replace_old_begin_byte, 2u);
  EXPECT_EQ(plan.replace_new_begin_byte, 2u);
}

TEST(LiveConversionChunkerTest, ComputesFreshPlan) {
  const auto plan = azookey::core::ComputeLiveConversionChunkPlan("", "今日はabc");

  EXPECT_EQ(plan.unchanged_prefix_bytes, 0u);
  EXPECT_EQ(plan.unchanged_suffix_bytes, 0u);
  EXPECT_EQ(plan.replace_old_begin_byte, 0u);
  EXPECT_EQ(plan.replace_old_end_byte, 0u);
  EXPECT_EQ(plan.replace_new_begin_byte, 0u);
  EXPECT_EQ(plan.replace_new_end_byte, std::string("今日はabc").size());
  EXPECT_EQ(ChunkTexts(plan.replacement_chunks), (std::vector<std::string>{"今日は", "abc"}));
}

TEST(LiveConversionChunkerTest, ComputesAppendPlan) {
  const auto plan = azookey::core::ComputeLiveConversionChunkPlan("今日はabc", "今日はabc123");

  EXPECT_EQ(plan.unchanged_prefix_bytes, std::string("今日はabc").size());
  EXPECT_EQ(plan.unchanged_suffix_bytes, 0u);
  EXPECT_EQ(plan.replace_old_begin_byte, std::string("今日はabc").size());
  EXPECT_EQ(plan.replace_old_end_byte, std::string("今日はabc").size());
  EXPECT_EQ(plan.replace_new_begin_byte, std::string("今日はabc").size());
  EXPECT_EQ(plan.replace_new_end_byte, std::string("今日はabc123").size());
  EXPECT_EQ(ChunkTexts(plan.replacement_chunks), (std::vector<std::string>{"123"}));
  EXPECT_EQ(ChunkKinds(plan.replacement_chunks),
            (std::vector<LiveConversionChunkKind>{LiveConversionChunkKind::Verbatim}));
}

TEST(LiveConversionChunkerTest, ComputesMiddleInsertPlan) {
  const auto plan = azookey::core::ComputeLiveConversionChunkPlan("今日は天気", "今日はいい天気");

  EXPECT_EQ(plan.unchanged_prefix_bytes, std::string("今日は").size());
  EXPECT_EQ(plan.unchanged_suffix_bytes, std::string("天気").size());
  EXPECT_EQ(plan.replace_old_begin_byte, std::string("今日は").size());
  EXPECT_EQ(plan.replace_old_end_byte, std::string("今日は").size());
  EXPECT_EQ(plan.replace_new_begin_byte, std::string("今日は").size());
  EXPECT_EQ(plan.replace_new_end_byte, std::string("今日はいい").size());
  EXPECT_EQ(ChunkTexts(plan.replacement_chunks), (std::vector<std::string>{"いい"}));
  EXPECT_EQ(ChunkKinds(plan.replacement_chunks),
            (std::vector<LiveConversionChunkKind>{LiveConversionChunkKind::Convert}));
}

TEST(LiveConversionChunkerTest, ComputesDeletePlan) {
  const auto plan = azookey::core::ComputeLiveConversionChunkPlan("今日はいい天気", "今日は天気");

  EXPECT_EQ(plan.unchanged_prefix_bytes, std::string("今日は").size());
  EXPECT_EQ(plan.unchanged_suffix_bytes, std::string("天気").size());
  EXPECT_EQ(plan.replace_old_begin_byte, std::string("今日は").size());
  EXPECT_EQ(plan.replace_old_end_byte, std::string("今日はいい").size());
  EXPECT_EQ(plan.replace_new_begin_byte, std::string("今日は").size());
  EXPECT_EQ(plan.replace_new_end_byte, std::string("今日は").size());
  EXPECT_TRUE(plan.replacement_chunks.empty());
}
