#pragma once

#include <Windows.h>
#include <msctf.h>
#include <wrl/client.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "azookey/core/CharacterFormCycle.h"

namespace azookey::tsf {

// Kept by TextService between consecutive NonConvert keystrokes. It is valid
// only while the selected text still equals cycle.forms[index].
struct CharacterFormCycleState {
  core::CharacterFormCycle cycle;
  size_t index{core::CharacterFormCycle::kHiragana};
};

struct CharacterFormCycleResult {
  // True once text was replaced, even if restoring the selection then failed.
  // The caller must consume the key in that case to avoid a second edit.
  bool applied{false};
  HRESULT status{S_FALSE};
  std::optional<CharacterFormCycleState> state;
};

// A verified, collapsed caret immediately after our last committed surface.
// Range anchors follow document edits, but are revalidated before every write.
struct CharacterFormRecentCommit {
  Microsoft::WRL::ComPtr<ITfContext> context;
  Microsoft::WRL::ComPtr<ITfRange> caret;
  core::CharacterFormCycle cycle;
  std::wstring surface;
  size_t next_index{core::CharacterFormCycle::kHiragana};
};

struct CharacterFormRecentCommitResult {
  // A failed insertion normally restores the original caret and passes the
  // key through. If restoring it also fails, consume the key: host insertion
  // over the temporary selection could otherwise delete the committed text.
  bool consume_key{false};
  bool replaced{false};
  HRESULT status{S_FALSE};
  std::optional<CharacterFormRecentCommit> state;
};

class CharacterFormEditSession {
 public:
  // Read-only preflight for OnTestKeyDown. A missing or unsupported selection
  // leaves the key available to the application.
  static bool CanCycleSelection(ITfContext* context, TfClientId client_id,
                                const std::optional<CharacterFormCycleState>& previous);

  // Call on the TSF owner thread during key handling. No selection, unsupported
  // text, or a refused edit leaves the document untouched and returns !applied.
  static CharacterFormCycleResult CycleSelection(
      ITfContext* context, TfClientId client_id,
      const std::optional<CharacterFormCycleState>& previous);

  // Call only after a commit edit session actually completed. The returned
  // anchor is absent unless the caret is collapsed immediately after exactly
  // committed_surface and reading supports the five-form cycle.
  static std::optional<CharacterFormRecentCommit> CaptureRecentCommit(
      ITfContext* context, TfClientId client_id, std::string_view reading,
      std::string_view committed_surface);

  // Safely rewrites the last committed surface when there is no selection and
  // the caret/context/text still match the captured commit.
  static CharacterFormRecentCommitResult CycleRecentCommit(
      ITfContext* context, TfClientId client_id, const CharacterFormRecentCommit& previous);
};

}  // namespace azookey::tsf
