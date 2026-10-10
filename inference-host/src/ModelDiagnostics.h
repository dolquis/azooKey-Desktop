#pragma once

#include <iostream>
#include <string_view>

namespace azookey::host::detail {

// The Host's llama callback and its preload fixture share the same route.
// On Windows, StderrPipeWriter owns pipe writes and their cancellation.
inline void WriteModelDiagnostic(std::string_view text) {
  std::cerr.write(text.data(), static_cast<std::streamsize>(text.size()));
}

}  // namespace azookey::host::detail
