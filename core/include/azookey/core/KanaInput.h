#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace azookey::core {

// Kana input (JIS and US layouts): docs/input-style-spec.md section 4.

// The kana a physical key makes, or std::nullopt when the table has no entry for it
// (docs/input-style-spec.md sections 4.1, 4.2 and 4.5). `scan_code` is bits 16-23 of the key
// message's lParam. A key with the extended flag is never looked up, because a numpad `/` shares the
// scan code of the `/` key. Shift is the only modifier; the caller skips Ctrl, Alt and Win.
// The voicing-mark keys return U+309B / U+309C (see AsVoicingMark).
std::optional<std::string_view> LookupKanaJis(uint8_t scan_code, bool extended, bool shift);
std::optional<std::string_view> LookupKanaUs(uint8_t scan_code, bool extended, bool shift);

enum class VoicingMark {
  kDakuten,     // ゛ (U+309B)
  kHandakuten,  // ゜ (U+309C)
};

// The voicing mark a kana lookup result stands for, or std::nullopt for any other kana.
std::optional<VoicingMark> AsVoicingMark(std::string_view kana);

struct VoicingResult {
  // Bytes at the end of the reading that `text` replaces; 0 when `text` is appended.
  size_t replace_bytes = 0;
  std::string text;
};

// Applies a voicing mark to the reading before the cursor (section 4.3). The mark combines with
// the last character only: う -> ゔ, か-row -> が-row, さ-row -> ざ-row, た-row -> だ-row (ち ->
// ぢ, つ -> づ included), は-row -> ば-row; the handakuten combines with は-row only. Otherwise
// (empty reading, an uncombinable or already voiced character) the result appends the standalone
// mark.
VoicingResult ApplyVoicingMark(std::string_view reading_before_cursor, VoicingMark mark);

}  // namespace azookey::core
