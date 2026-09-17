#pragma once
#include <msctf.h>
#include <windows.h>

namespace azookey::tsf {
// How a context classifies for privacy. `Unknown` is "could not tell", which
// the two axes read differently: the AI axis refuses it (spec section 2 fail
// closed), the secure axis does not, because spec section 4 makes password-field
// detection best effort and a silent learning blackout is its own defect.
enum class InputScopeClass { Unknown, Password, Normal };

// Win32 half: the focus window's class name and ES_PASSWORD style.
InputScopeClass ClassifyFocusWindow(HWND focus);
// TSF half: GUID_PROP_INPUTSCOPE read through a synchronous read-only session.
// Depends only on the context and client id, so a mock context can drive it.
InputScopeClass ClassifyInputScope(ITfContext* context, TfClientId client_id);

struct InputGateDecision {
  bool ai_allowed{false};
  bool secure{false};
};

// Pure composition of the two classifications. Kept separate from the probes so
// the asymmetry between the axes can be pinned without a focus window, which a
// test process does not have.
InputGateDecision CombineInputGate(InputScopeClass focus, InputScopeClass scope);

// Owner-thread only. No UIA or network calls. Probes the focus window and the
// context once each, then derives both axes from that one probe.
InputGateDecision EvaluateInputGate(ITfContext* context, TfClientId client_id);
}  // namespace azookey::tsf
