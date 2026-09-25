#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace azookey::core {

// Forms are ordered for the NonConvert cycle in tsf-deep-integration-spec.md §3.2.
struct CharacterFormCycle {
  static constexpr size_t kHiragana = 0;
  static constexpr size_t kKatakana = 1;
  static constexpr size_t kHalfwidthKatakana = 2;
  static constexpr size_t kFullwidthAlphanumeric = 3;
  static constexpr size_t kAscii = 4;

  std::array<std::string, 5> forms;
};

// Builds every form from the original hiragana reading. Returns nullopt for
// empty, malformed UTF-8, mixed text, or kana without a complete mapping.
std::optional<CharacterFormCycle> BuildCharacterFormCycle(std::string_view hiragana);

}  // namespace azookey::core
