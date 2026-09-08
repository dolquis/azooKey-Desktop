#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "azookey/core/RomajiKanaConverter.h"
#include "azookey/core/Utf8.h"

namespace azookey::core {

struct BatchRomajiChunk {
  std::string raw_romaji;
  std::string reading;
};

// Keep one converter across transport chunks: flushing at an arbitrary raw
// byte boundary would turn the 'n' in a split 'na' into a spurious 'ん'.
inline std::vector<BatchRomajiChunk> SplitBatchRomaji(const std::string& raw,
                                                      size_t max_bytes = 512) {
  std::vector<BatchRomajiChunk> chunks;
  RomajiKanaConverter converter;
  BatchRomajiChunk current;
  for (size_t i = 0; i < raw.size(); ++i) {
    const char byte = raw[i];
    current.raw_romaji.push_back(byte);
    current.reading += converter.Feed(byte);
    const bool scalar_boundary =
        i + 1 == raw.size() || (static_cast<unsigned char>(raw[i + 1]) & 0xC0) != 0x80;
    if (scalar_boundary && current.raw_romaji.size() >= max_bytes && !current.reading.empty() &&
        (!converter.HasPending() || current.raw_romaji.size() >= max_bytes + 4)) {
      chunks.push_back(std::move(current));
      current = {};
    }
  }
  current.reading += converter.Flush();
  if (current.raw_romaji.empty() && !chunks.empty()) {
    chunks.back().reading += current.reading;
  } else if (!current.raw_romaji.empty() || !current.reading.empty()) {
    chunks.push_back(std::move(current));
  }
  return chunks;
}

// Prefer sentence boundaries; hard splitting never separates a UTF-8 scalar.
// The small model chunk leaves room for prompt, context and output tokens.
inline std::vector<std::string> SplitBatchConversion(std::string_view text, size_t max_bytes = 96) {
  std::vector<std::string> chunks;
  size_t begin = 0;
  while (begin < text.size()) {
    size_t end = begin;
    size_t sentence_end = begin;
    while (end < text.size()) {
      size_t next = end;
      char32_t codepoint{};
      DecodeNextUtf8(text, next, codepoint);
      if (next - begin > max_bytes && end != begin) break;
      end = next;
      if (codepoint == U'。' || codepoint == U'！' || codepoint == U'？' || codepoint == U'\n' ||
          codepoint == U'.' || codepoint == U'!' || codepoint == U'?') {
        sentence_end = end;
        break;
      }
    }
    if (sentence_end != begin) end = sentence_end;
    chunks.emplace_back(text.substr(begin, end - begin));
    begin = end;
  }
  return chunks;
}

}  // namespace azookey::core
