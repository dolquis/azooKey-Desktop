#pragma once
#include <msctf.h>
#include <windows.h>

#include <string>
#include <string_view>

namespace azookey::tsf {
// How a context classifies for privacy. `Unknown` is "could not tell", which
// the two axes read differently: the AI axis refuses it (spec section 2 fail
// closed), the secure axis does not, because spec section 4 makes password-field
// detection best effort and a silent learning blackout is its own defect.
// `Private` is IS_PRIVATE: the app asks not to learn, but the field is not
// known to be a password (Chromium sends it for incognito fields too).
enum class InputScopeClass { Unknown, Password, Private, Normal };

// Why the TSF probe ended where it did. Logged next to the classification so a
// field report can tell "the app gave no scope" from "the app gave IS_DEFAULT".
// SessionFailed: the read session was refused, deferred, or had no selection.
// NoProperty: no GUID_PROP_INPUTSCOPE, or its value was not a readable scope.
// Skipped: a disabled context or a password-styled window already decided.
enum class InputScopeProbeStatus { NoContext, SessionFailed, NoProperty, Empty, Read, Skipped };

struct InputScopeProbe {
  InputScopeClass classification{InputScopeClass::Unknown};
  InputScopeProbeStatus status{InputScopeProbeStatus::NoContext};
  // The InputScope enum values as decimal, comma separated. Never field text.
  std::string scopes;
};

// Win32 half: the focus window's class name and ES_PASSWORD style.
InputScopeClass ClassifyFocusWindow(HWND focus);
// TSF half: GUID_PROP_INPUTSCOPE read through a synchronous read-only session.
// Depends only on the context and client id, so a mock context can drive it.
InputScopeProbe ProbeInputScope(ITfContext* context, TfClientId client_id);
InputScopeClass ClassifyInputScope(ITfContext* context, TfClientId client_id);
// GUID_COMPARTMENT_KEYBOARD_DISABLED or GUID_COMPARTMENT_EMPTYCONTEXT set on the
// context. Chromium sets both on its password-field context; TSF still calls
// the key sinks, so the TIP itself must leave such a context alone. A context
// whose compartments cannot be read is not disabled.
bool IsContextKeyboardDisabled(ITfContext* context);

struct InputGateDecision {
  bool ai_allowed{false};
  bool secure{false};
  bool learning_allowed{true};
  // Probe results behind the decision, for diagnostics only.
  bool keyboard_disabled{false};
  InputScopeClass focus{InputScopeClass::Unknown};
  InputScopeProbe scope;
};

// Pure composition of the two classifications. Kept separate from the probes so
// the asymmetry between the axes can be pinned without a focus window, which a
// test process does not have.
InputGateDecision CombineInputGate(InputScopeClass focus, InputScopeClass scope);

// Owner-thread only. No UIA or network calls. Probes the context compartments,
// the focus window and the input scope once each, then derives every axis from
// that one probe.
InputGateDecision EvaluateInputGate(ITfContext* context, TfClientId client_id);

std::string_view InputScopeClassName(InputScopeClass value) noexcept;
std::string_view InputScopeProbeStatusName(InputScopeProbeStatus value) noexcept;
}  // namespace azookey::tsf
