#pragma once

#include <cstdint>

namespace azookey::tsf::internal {

inline int WrapCandidateSelectionIndex(int selected_idx, int delta, int count) {
  if (count <= 0) return 0;

  const int64_t count64 = count;
  int64_t normalized = selected_idx % count64;
  if (normalized < 0) normalized += count64;

  const int64_t offset = delta % count64;
  int64_t next = normalized + offset;
  if (next < 0) {
    next += count64;
  } else if (next >= count64) {
    next -= count64;
  }
  return static_cast<int>(next);
}

}  // namespace azookey::tsf::internal
