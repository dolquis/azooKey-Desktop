#pragma once

#include <msctf.h>
#include <wrl/client.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "azookey/ipc/Limits.h"

namespace azookey::tsf {

// The document text the batch correction view checks (rich-features-spec
// X-3-6). Offsets are UTF-16 code units of `text`, matching DetectAnomalies.
struct ProofreadDocument {
  std::wstring text;
  // Start of the selection (or the document start) within `text`.
  size_t anchor{0};
  // The readable text around the selection did not fit and was cut.
  bool truncated{false};
  // Covers `text` in the document; TSF keeps it in place across edits, so a
  // later edit session can find the window again.
  Microsoft::WRL::ComPtr<ITfRange> range;
};

struct ProofreadWindow {
  size_t start{0};
  size_t length{0};
};

// The UTF-16 code units read on each side of the selection start. One code
// unit is at least one UTF-8 byte, so a window that fits the byte limit never
// needs more than this on either side.
inline constexpr size_t kProofreadReadCodeUnits = ipc::kMaxAnomalyTextBytes;

// The largest window around `anchor` whose UTF-8 form fits `max_utf8_bytes`,
// grown one code point at a time on both sides. A surrogate pair is never
// split; an unpaired surrogate counts as the 3 bytes of U+FFFD.
ProofreadWindow SelectProofreadWindow(std::wstring_view text, size_t anchor,
                                      size_t max_utf8_bytes) noexcept;

// Reads the text around the selection start of `context` in a synchronous
// read-only edit session and cuts it to the DetectAnomalies limit. The caller
// must first rule out secure and no-learning contexts. Returns std::nullopt
// when the session is refused, a read fails, or the document reads empty.
std::optional<ProofreadDocument> ReadProofreadDocument(ITfContext* context,
                                                       TfClientId client_id) noexcept;

}  // namespace azookey::tsf
