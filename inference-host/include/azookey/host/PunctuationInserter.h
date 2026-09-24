#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "azookey/core/PunctuationRules.h"
#include "azookey/ipc/Payloads.h"

namespace azookey::host {

struct PunctuationResult {
  std::string surface;
  std::vector<ipc::LiveSegment> segments;
};

// Operates on converted segments. The input reading is never changed: inserted
// punctuation has an empty reading and is represented by its own segment.
class PunctuationInserter {
 public:
  static PunctuationResult Insert(const std::vector<ipc::LiveSegment>& converted,
                                  const core::PunctuationRules& rules, std::string_view style,
                                  double boundary_confidence);
  // Missing/unreadable files use built-in rules. Reading on each enabled
  // request also makes edits visible to the next composition without touching
  // an in-flight preedit.
  static core::PunctuationRules LoadRules(std::string_view configured_path);
};

}  // namespace azookey::host
