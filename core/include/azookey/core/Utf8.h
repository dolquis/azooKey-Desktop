#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace azookey::core {

// Decode one Unicode scalar. Invalid input returns false, consumes one byte,
// and reports that unsigned byte as codepoint. At end, outputs are unchanged.
bool DecodeNextUtf8(std::string_view input, size_t& offset, char32_t& codepoint);

// Append one Unicode scalar value as UTF-8. Callers must pass a value accepted
// by DecodeNextUtf8's scalar-value contract.
void AppendUtf8(std::string& output, char32_t codepoint);

// Preserve the host's suffix-trim contract: continuation bytes remain attached
// to the preceding byte, even for malformed input. This is not UTF-8 validation.
std::string TakeLastUtf8Codepoints(std::string_view input, size_t max_codepoints);

}  // namespace azookey::core
