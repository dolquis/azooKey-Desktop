#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "azookey/ipc/Payloads.h"

namespace azookey::tsf {

// DEV-1806 (docs/legacy-parity-spec.md section 2.3.1): a piece of a live answer
// the preedit carries while the answer for a changed reading is on its way.
// `cut_after` marks where the preedit may stop showing converted text and go on
// with the rest of the reading as typed.
struct LivePiece {
  std::string reading;
  std::string surface;
  bool auto_punctuation{false};
  bool cut_after{false};
};

// The pieces of the answer `surface` for `reading`, or none when it cannot be
// carried. Each segment the Host returned (the whole surface without segments)
// is split at the kana it shares with its reading when that split is the only
// one, so a shorter reading can keep the converted start; a segment that does
// not split is one piece. An answer whose request asked for punctuation and
// that has no usable segments may end in an automatic mark, so it gives none.
std::vector<LivePiece> BuildLivePieces(const std::string& reading, const std::string& surface,
                                       const std::vector<ipc::LiveSegment>& segments,
                                       bool auto_punctuation);

struct CarriedLiveText {
  std::string surface;     // The converted text kept.
  size_t reading_bytes{};  // The bytes of the typed reading it stands for.
};

// The longest converted start of `pieces` the reading `typed` begins with that
// ends at a cut. Automatic marks are kept only before a kept piece.
CarriedLiveText CarryLiveText(const std::vector<LivePiece>& pieces, std::string_view typed);

}  // namespace azookey::tsf
