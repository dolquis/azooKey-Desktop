#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace azookey::learning {

// user-learning-enhancement-spec section 8.1: the last K code points of the
// left context are hashed, never stored.
inline constexpr size_t kContextHashCodePoints = 8;
inline constexpr std::string_view kEmptyContextHash = "0x00000000";

// NFC on Windows (NormalizeString). Other platforms return the input
// unchanged: the Host only receives text from the Windows TIP, and the
// portable build exists for tests. Invalid UTF-8 is returned unchanged.
std::string NormalizeNfc(std::string_view utf8);

// "0x%08x" of the first four bytes (big endian) of SHA-256 over the UTF-8 of
// the last kContextHashCodePoints code points of NFC(left_context). An empty
// context yields kEmptyContextHash. 32 bits cannot be inverted to the text.
std::string ContextHash(std::string_view left_context_utf8);

}  // namespace azookey::learning
