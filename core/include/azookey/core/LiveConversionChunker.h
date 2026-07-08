#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace azookey::core {

enum class LiveConversionChunkKind {
  Convert,
  Verbatim,
};

struct LiveConversionChunk {
  LiveConversionChunkKind kind{LiveConversionChunkKind::Verbatim};
  std::string text;
  size_t byte_offset{};
};

struct LiveConversionChunkPlan {
  std::vector<LiveConversionChunk> chunks;
  std::vector<LiveConversionChunk> replacement_chunks;
  size_t unchanged_prefix_bytes{};
  size_t unchanged_suffix_bytes{};
  size_t replace_old_begin_byte{};
  size_t replace_old_end_byte{};
  size_t replace_new_begin_byte{};
  size_t replace_new_end_byte{};
};

bool IsJapaneseForLiveConversion(char32_t code_point);
std::vector<LiveConversionChunk> GroupLiveConversionChunks(std::string_view text);
LiveConversionChunkPlan ComputeLiveConversionChunkPlan(std::string_view previous_text,
                                                       std::string_view current_text);

}  // namespace azookey::core
