#pragma once

#include <memory>

#include "azookey/core/CustomRomajiLoader.h"

namespace azookey::core {

// Built-in AZIK rules for `inputStyle=azik` (docs/input-style-spec.md section 3). A rule table
// like a custom TSV: set it with RomajiKanaConverter::SetCustomTable so conversion takes the
// table-driven path and skips Feed's built-in romaji special cases. The table is immutable and
// shared; the same pointer is returned on every call.
std::shared_ptr<const CustomRomajiTable> BuiltinAzikTable();

}  // namespace azookey::core
