#include "azookey/tsf/AiInputGuard.h"

#include <initguid.h>
#include <inputscope.h>
#include <wrl/client.h>

#include <new>

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
    if (count > 0) classification = InputScopeClass::Normal;
    for (UINT i = 0; i < count; ++i)
      if (scopes[i] == IS_PASSWORD || scopes[i] == IS_NUMERIC_PASSWORD ||
          scopes[i] == IS_NUMERIC_PIN || scopes[i] == IS_ALPHANUMERIC_PIN ||
          scopes[i] == IS_ALPHANUMERIC_PIN_SET)
        classification = InputScopeClass::Password;
    CoTaskMemFree(scopes);
    return S_OK;
  }
  InputScopeClass classification{InputScopeClass::Unknown};
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

InputScopeClass ClassifyInputScope(ITfContext* context, TfClientId client_id) {
  if (!context) return InputScopeClass::Unknown;
  ComPtr<ScopeSession> session;
  session.Attach(new (std::nothrow) ScopeSession(context));
  if (!session) return InputScopeClass::Unknown;
  HRESULT executed = E_FAIL;
  const auto result =
      context->RequestEditSession(client_id, session.Get(), TF_ES_SYNC | TF_ES_READ, &executed);
  session->context = nullptr;  // Never retain a raw context if an app delays the session.
  if (FAILED(result) || executed != S_OK) return InputScopeClass::Unknown;
  return session->classification;
}

InputGateDecision CombineInputGate(InputScopeClass focus, InputScopeClass scope) {
  // AI needs both axes to say Normal, so anything unclassified refuses it.
  // Secure needs one axis to positively say Password, so nothing unclassified
  // claims it.
  if (focus == InputScopeClass::Password) return {false, true};
  return {focus == InputScopeClass::Normal && scope == InputScopeClass::Normal,
          scope == InputScopeClass::Password};
}

InputGateDecision EvaluateInputGate(ITfContext* context, TfClientId client_id) {
  const auto focus = ClassifyFocusWindow(FocusWindow());
  // A password-styled focus window is answer enough; skip the edit session.
  if (focus == InputScopeClass::Password) return CombineInputGate(focus, InputScopeClass::Unknown);
  return CombineInputGate(focus, ClassifyInputScope(context, client_id));
}
}  // namespace azookey::tsf
