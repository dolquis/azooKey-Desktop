#pragma once

#include <optional>
#include <string_view>

namespace azookey::host {

// Prompt format of a Zenzai GGUF. The behaviour of each value is fixed in
// docs/zenzai-inference-spec.md §2.4.2.
enum class ZenzaiPromptFormat {
  // zenz-v3: [CTX] only with a left context, [PROF] carries the instruction profile.
  ZenzV3,
  // kanakanji-edge-v1: [CTX] always, no [PROF], control markers are CONTROL tokens.
  KanakanjiEdgeV1,
};

inline constexpr std::string_view kZenzV3PromptFormatName = "zenz-v3";
inline constexpr std::string_view kKanakanjiEdgeV1PromptFormatName = "kanakanji-edge-v1";

inline std::optional<ZenzaiPromptFormat> ParseZenzaiPromptFormat(std::string_view name) {
  if (name == kZenzV3PromptFormatName) return ZenzaiPromptFormat::ZenzV3;
  if (name == kKanakanjiEdgeV1PromptFormatName) return ZenzaiPromptFormat::KanakanjiEdgeV1;
  return std::nullopt;
}

inline std::string_view ZenzaiPromptFormatName(ZenzaiPromptFormat format) {
  return format == ZenzaiPromptFormat::KanakanjiEdgeV1 ? kKanakanjiEdgeV1PromptFormatName
                                                       : kZenzV3PromptFormatName;
}

// Whether the format carries an instruction profile ([PROF]). Local AI tasks that need one are
// disabled for formats that do not.
inline bool ZenzaiPromptFormatUsesInstructionProfile(ZenzaiPromptFormat format) {
  return format == ZenzaiPromptFormat::ZenzV3;
}

}  // namespace azookey::host
