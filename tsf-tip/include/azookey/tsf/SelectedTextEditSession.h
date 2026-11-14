#pragma once

// clang-format off
#include <windows.h>
#include <ctffunc.h>
#include <msctf.h>
// clang-format on
#include <wrl/client.h>

#include <cstddef>
#include <functional>
#include <string>

namespace azookey::tsf {

// The selection the M16 prompt window transforms (legacy-parity-spec §4.2).
// Unlike reconversion it takes any text, Japanese or not, and never widens an
// empty selection.
struct SelectedTextCapture {
  // Covers the selection; TSF keeps it in place across edits, so the later
  // replacement finds the same text.
  Microsoft::WRL::ComPtr<ITfRange> range;
  std::wstring text;
};

enum class SelectedTextRead {
  Ok,
  // No selection, or a collapsed one.
  Empty,
  // Longer than max_units: the caller refuses it rather than cutting it.
  TooLong,
  // The read session was refused or a read failed.
  Failed,
};

// UTF-16 code units of the longest selection the window takes.
inline constexpr size_t kMaxTransformSelectionUnits = 4096;

// Reads the default selection of `context` in a synchronous read-only edit
// session, as at most `max_units` UTF-16 code units. Call on the TSF owner
// thread during key handling. `capture` is filled only on Ok.
SelectedTextRead ReadSelectedText(ITfContext* context, TfClientId client_id, size_t max_units,
                                  SelectedTextCapture& capture) noexcept;

// Replaces `range` with `replacement` in a read-write edit session, granted
// now or later by the application. The session first checks that the range
// still holds `original` and leaves the document untouched otherwise
// (TF_E_NOCONVERSION). `done` runs inside the session with the outcome; it
// does not run when the session is never granted. Returns S_OK when the
// session ran now, TF_S_ASYNC when it is queued, or the failure.
HRESULT RequestSelectedTextReplacement(ITfRange* range, TfClientId client_id, std::wstring original,
                                       std::wstring replacement,
                                       std::function<void(HRESULT)> done) noexcept;

}  // namespace azookey::tsf
