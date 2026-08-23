#pragma once

#include <cstddef>
#include <cstdint>

namespace azookey::host {

struct ZenzaiDecodeStats {
  double prompt_decode_ms{};
  double beam_decode_ms{};
  uint64_t prompt_tokens{};
  uint64_t beam_tokens{};
  size_t beam_decode_evaluations{};
};

}  // namespace azookey::host
