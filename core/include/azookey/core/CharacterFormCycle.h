#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
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

// The forms F6-F10 commit (tsf-deep-integration-spec.md §3.6).
enum class CommitForm : uint8_t {
  Hiragana,
  Katakana,
  HalfwidthKatakana,
  FullwidthRoman,
  HalfwidthRoman,
};

// The text F6-F10 commit for a reading, which may hold the digits and symbols
// of spec §3.5. The roman forms spell `typed_romaji` when it is non-empty (the
// frontend passes it only when it still spells the reading); otherwise the kana
// are respelled in lowercase Hepburn and the symbols map back to the ASCII typed.
std::string BuildCommitForm(std::string_view reading, CommitForm form,
                            std::string_view typed_romaji = {});

}  // namespace azookey::core
