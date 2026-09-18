#pragma once

#include <optional>

namespace azookey::core {

// Only the TIP reads the document. Unknown selection means a failed read, not
// an empty document; it must never authorize a destructive action.
struct EditContextHint {
  std::optional<char32_t> char_before;
  std::optional<char32_t> char_after;
  std::optional<bool> selection_collapsed;
};

}  // namespace azookey::core
