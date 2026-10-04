#include "azookey/tsf/AiInputGuard.h"

#include <initguid.h>
#include <inputscope.h>
#include <wrl/client.h>

#include <new>
#include <string>
#include <utility>

namespace azookey::tsf {
namespace {
using Microsoft::WRL::ComPtr;
class ScopeSession final : public ITfEditSession {
 public:
  explicit ScopeSession(ITfContext* value) : context(value) {}
  STDMETHODIMP QueryInterface(REFIID id, void** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (id != IID_IUnknown && id != IID_ITfEditSession) return E_NOINTERFACE;
    *out = static_cast<ITfEditSession*>(this);
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs); }
  STDMETHODIMP_(ULONG) Release() override {
    const auto left = InterlockedDecrement(&refs);
    if (!left) delete this;
    return left;
  }
  STDMETHODIMP DoEditSession(TfEditCookie cookie) override {
    if (!context) return E_FAIL;
    TF_SELECTION selection{};
    ULONG fetched = 0;
    if (FAILED(context->GetSelection(cookie, TF_DEFAULT_SELECTION, 1, &selection, &fetched)) ||
        fetched != 1 || !selection.range)
      return E_FAIL;
    ComPtr<ITfRange> range;
    range.Attach(selection.range);
    probe.status = InputScopeProbeStatus::NoProperty;
    ComPtr<ITfReadOnlyProperty> property;
    if (FAILED(context->GetAppProperty(GUID_PROP_INPUTSCOPE, &property)) || !property)
      return E_FAIL;
    VARIANT value;
    VariantInit(&value);
    const auto result = property->GetValue(cookie, range.Get(), &value);
    ComPtr<ITfInputScope> scope;
    if (SUCCEEDED(result) && value.vt == VT_UNKNOWN && value.punkVal)
      value.punkVal->QueryInterface(IID_PPV_ARGS(&scope));
    VariantClear(&value);
    if (!scope) return E_FAIL;
    InputScope* scopes = nullptr;
    UINT count = 0;
    if (FAILED(scope->GetInputScopes(&scopes, &count))) return E_FAIL;
    // An empty scope set says nothing about the field, so it stays Unknown.
    probe.status = count > 0 ? InputScopeProbeStatus::Read : InputScopeProbeStatus::Empty;
    bool password = false;
    bool private_scope = false;
    for (UINT i = 0; i < count; ++i) {
      if (scopes[i] == IS_PASSWORD || scopes[i] == IS_NUMERIC_PASSWORD ||
          scopes[i] == IS_NUMERIC_PIN || scopes[i] == IS_ALPHANUMERIC_PIN ||
          scopes[i] == IS_ALPHANUMERIC_PIN_SET)
        password = true;
      // Chromium marks password fields IS_PRIVATE, never IS_PASSWORD.
      if (scopes[i] == IS_PRIVATE) private_scope = true;
      if (i < kMaxLoggedScopes) {
        if (i) probe.scopes += ',';
        probe.scopes += std::to_string(static_cast<int>(scopes[i]));
      }
    }
    CoTaskMemFree(scopes);
    if (password)
      probe.classification = InputScopeClass::Password;
    else if (private_scope)
      probe.classification = InputScopeClass::Private;
    else if (count > 0)
      probe.classification = InputScopeClass::Normal;
    return S_OK;
  }
  static constexpr UINT kMaxLoggedScopes = 8;
  InputScopeProbe probe{InputScopeClass::Unknown, InputScopeProbeStatus::SessionFailed, {}};
  ITfContext* context;

 private:
  LONG refs{1};
};

HWND FocusWindow() {
  GUITHREADINFO info{sizeof(info)};
  if (!GetGUIThreadInfo(GetCurrentThreadId(), &info)) return nullptr;
  return info.hwndFocus;
}
}  // namespace

InputScopeClass ClassifyFocusWindow(HWND focus) {
  if (!focus) return InputScopeClass::Unknown;
  wchar_t name[32]{};
  if (!GetClassNameW(focus, name, 32)) return InputScopeClass::Unknown;
  if ((_wcsicmp(name, L"Edit") == 0 || _wcsnicmp(name, L"RichEdit", 8) == 0) &&
      (GetWindowLongPtrW(focus, GWL_STYLE) & ES_PASSWORD))
    return InputScopeClass::Password;
  return InputScopeClass::Normal;
}

InputScopeProbe ProbeInputScope(ITfContext* context, TfClientId client_id) {
  if (!context) return {};
  ComPtr<ScopeSession> session;
  session.Attach(new (std::nothrow) ScopeSession(context));
  if (!session) return {InputScopeClass::Unknown, InputScopeProbeStatus::SessionFailed, {}};
  HRESULT executed = E_FAIL;
  const auto result =
      context->RequestEditSession(client_id, session.Get(), TF_ES_SYNC | TF_ES_READ, &executed);
  session->context = nullptr;  // Never retain a raw context if an app delays the session.
  if (FAILED(result) || executed != S_OK) {
    auto failed = std::move(session->probe);
    failed.classification = InputScopeClass::Unknown;
    // A session the app deferred or refused never reached the property.
    if (FAILED(result) || executed == TF_S_ASYNC)
      failed.status = InputScopeProbeStatus::SessionFailed;
    return failed;
  }
  return std::move(session->probe);
}

InputScopeClass ClassifyInputScope(ITfContext* context, TfClientId client_id) {
  return ProbeInputScope(context, client_id).classification;
}

bool IsContextKeyboardDisabled(ITfContext* context) {
  if (!context) return false;
  ComPtr<ITfCompartmentMgr> manager;
  if (FAILED(context->QueryInterface(IID_PPV_ARGS(&manager))) || !manager) return false;
  for (const GUID* id : {&GUID_COMPARTMENT_KEYBOARD_DISABLED, &GUID_COMPARTMENT_EMPTYCONTEXT}) {
    ComPtr<ITfCompartment> compartment;
    if (FAILED(manager->GetCompartment(*id, &compartment)) || !compartment) continue;
    VARIANT value;
    VariantInit(&value);
    const bool set =
        SUCCEEDED(compartment->GetValue(&value)) && value.vt == VT_I4 && value.lVal != 0;
    VariantClear(&value);
    if (set) return true;
  }
  return false;
}

InputGateDecision CombineInputGate(InputScopeClass focus, InputScopeClass scope) {
  // AI needs both axes to say Normal, so anything unclassified refuses it.
  // Secure needs one axis to positively say Password, so nothing unclassified
  // claims it. IS_PRIVATE only withholds learning (spec section 3 `private`).
  InputGateDecision decision;
  decision.focus = focus;
  decision.scope.classification = scope;
  if (focus == InputScopeClass::Password) {
    decision.secure = true;
    decision.learning_allowed = false;
    return decision;
  }
  decision.ai_allowed = focus == InputScopeClass::Normal && scope == InputScopeClass::Normal;
  decision.secure = scope == InputScopeClass::Password;
  decision.learning_allowed = !decision.secure && scope != InputScopeClass::Private;
  return decision;
}

InputGateDecision EvaluateInputGate(ITfContext* context, TfClientId client_id) {
  // A context the app disabled for keyboard input is Chromium's password field;
  // that is answer enough, so skip the window and the edit session.
  if (IsContextKeyboardDisabled(context)) {
    auto decision = CombineInputGate(InputScopeClass::Unknown, InputScopeClass::Unknown);
    decision.secure = true;
    decision.learning_allowed = false;
    decision.keyboard_disabled = true;
    decision.scope.status = InputScopeProbeStatus::Skipped;
    return decision;
  }
  const auto focus = ClassifyFocusWindow(FocusWindow());
  // A password-styled focus window is answer enough; skip the edit session.
  if (focus == InputScopeClass::Password) {
    auto decision = CombineInputGate(focus, InputScopeClass::Unknown);
    decision.scope.status = InputScopeProbeStatus::Skipped;
    return decision;
  }
  auto scope = ProbeInputScope(context, client_id);
  auto decision = CombineInputGate(focus, scope.classification);
  decision.scope = std::move(scope);
  return decision;
}

std::string_view InputScopeClassName(InputScopeClass value) noexcept {
  switch (value) {
    case InputScopeClass::Password:
      return "password";
    case InputScopeClass::Private:
      return "private";
    case InputScopeClass::Normal:
      return "normal";
    case InputScopeClass::Unknown:
      break;
  }
  return "unknown";
}

std::string_view InputScopeProbeStatusName(InputScopeProbeStatus value) noexcept {
  switch (value) {
    case InputScopeProbeStatus::SessionFailed:
      return "session_failed";
    case InputScopeProbeStatus::NoProperty:
      return "no_property";
    case InputScopeProbeStatus::Empty:
      return "empty";
    case InputScopeProbeStatus::Read:
      return "read";
    case InputScopeProbeStatus::Skipped:
      return "skipped";
    case InputScopeProbeStatus::NoContext:
      break;
  }
  return "no_context";
}
}  // namespace azookey::tsf
