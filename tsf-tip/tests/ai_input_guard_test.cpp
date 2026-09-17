// Covers the privacy gate in front of AI cleanup and of M46 learning
// suppression. The gate decides whether a keystroke may leave the TIP, so a
// silent regression from fail-closed to fail-open is exactly the kind of defect
// no manual pass would notice (DEV-1125).
#include <gtest/gtest.h>
#include <initguid.h>
#include <inputscope.h>
#include <msctf.h>

#include <algorithm>
#include <string>
#include <vector>

#include "azookey/core/SecureApps.h"
#include "azookey/tsf/AiInputGuard.h"
#include "azookey/tsf/ForegroundAppDetector.h"

namespace {
using azookey::tsf::ClassifyFocusWindow;
using azookey::tsf::ClassifyInputScope;
using azookey::tsf::CombineInputGate;
using azookey::tsf::InputScopeClass;

class FakeRange final : public ITfRange {
 public:
  STDMETHODIMP QueryInterface(REFIID riid, void** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (riid != IID_IUnknown && riid != IID_ITfRange) return E_NOINTERFACE;
    *out = static_cast<ITfRange*>(this);
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override {
    return static_cast<ULONG>(InterlockedIncrement(&refs_));
  }
  STDMETHODIMP_(ULONG) Release() override {
    return static_cast<ULONG>(InterlockedDecrement(&refs_));
  }
  STDMETHODIMP GetText(TfEditCookie, DWORD, WCHAR*, ULONG, ULONG* length) override {
    if (length) *length = 0;
    return E_NOTIMPL;
  }
  STDMETHODIMP SetText(TfEditCookie, DWORD, const WCHAR*, LONG) override { return E_NOTIMPL; }
  STDMETHODIMP GetFormattedText(TfEditCookie, IDataObject** out) override {
    if (out) *out = nullptr;
    return E_NOTIMPL;
  }
  STDMETHODIMP GetEmbedded(TfEditCookie, REFGUID, REFIID, IUnknown** out) override {
    if (out) *out = nullptr;
    return E_NOTIMPL;
  }
  STDMETHODIMP InsertEmbedded(TfEditCookie, DWORD, IDataObject*) override { return E_NOTIMPL; }
  STDMETHODIMP ShiftStart(TfEditCookie, LONG, LONG* shifted, const TF_HALTCOND*) override {
    if (shifted) *shifted = 0;
    return E_NOTIMPL;
  }
  STDMETHODIMP ShiftEnd(TfEditCookie, LONG, LONG* shifted, const TF_HALTCOND*) override {
    if (shifted) *shifted = 0;
    return E_NOTIMPL;
  }
  STDMETHODIMP ShiftStartToRange(TfEditCookie, ITfRange*, TfAnchor) override { return E_NOTIMPL; }
  STDMETHODIMP ShiftEndToRange(TfEditCookie, ITfRange*, TfAnchor) override { return E_NOTIMPL; }
  STDMETHODIMP ShiftStartRegion(TfEditCookie, TfShiftDir, BOOL* no_region) override {
    if (no_region) *no_region = FALSE;
    return E_NOTIMPL;
  }
  STDMETHODIMP ShiftEndRegion(TfEditCookie, TfShiftDir, BOOL* no_region) override {
    if (no_region) *no_region = FALSE;
    return E_NOTIMPL;
  }
  STDMETHODIMP IsEmpty(TfEditCookie, BOOL* empty) override {
    if (empty) *empty = TRUE;
    return S_OK;
  }
  STDMETHODIMP Collapse(TfEditCookie, TfAnchor) override { return S_OK; }
  STDMETHODIMP IsEqualStart(TfEditCookie, ITfRange*, TfAnchor, BOOL* equal) override {
    if (equal) *equal = FALSE;
    return E_NOTIMPL;
  }
  STDMETHODIMP IsEqualEnd(TfEditCookie, ITfRange*, TfAnchor, BOOL* equal) override {
    if (equal) *equal = FALSE;
    return E_NOTIMPL;
  }
  STDMETHODIMP CompareStart(TfEditCookie, ITfRange*, TfAnchor, LONG* result) override {
    if (result) *result = 0;
    return E_NOTIMPL;
  }
  STDMETHODIMP CompareEnd(TfEditCookie, ITfRange*, TfAnchor, LONG* result) override {
    if (result) *result = 0;
    return E_NOTIMPL;
  }
  STDMETHODIMP AdjustForInsert(TfEditCookie, ULONG, BOOL* insert_ok) override {
    if (insert_ok) *insert_ok = TRUE;
    return S_OK;
  }
  STDMETHODIMP GetGravity(TfGravity* start, TfGravity* end) override {
    if (!start || !end) return E_POINTER;
    *start = *end = TF_GRAVITY_FORWARD;
    return S_OK;
  }
  STDMETHODIMP SetGravity(TfEditCookie, TfGravity, TfGravity) override { return S_OK; }
  STDMETHODIMP Clone(ITfRange** clone) override {
    if (!clone) return E_POINTER;
    *clone = nullptr;
    return E_NOTIMPL;
  }
  STDMETHODIMP GetContext(ITfContext** context) override {
    if (context) *context = nullptr;
    return E_NOTIMPL;
  }

 private:
  LONG refs_{1};
};

class FakeInputScope final : public ITfInputScope {
 public:
  explicit FakeInputScope(std::vector<InputScope> values) : scopes(std::move(values)) {}
  STDMETHODIMP QueryInterface(REFIID riid, void** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (riid != IID_IUnknown && riid != IID_ITfInputScope) return E_NOINTERFACE;
    *out = static_cast<ITfInputScope*>(this);
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override {
    return static_cast<ULONG>(InterlockedIncrement(&refs_));
  }
  STDMETHODIMP_(ULONG) Release() override {
    return static_cast<ULONG>(InterlockedDecrement(&refs_));
  }
  STDMETHODIMP GetInputScopes(InputScope** out, UINT* count) override {
    if (!out || !count) return E_POINTER;
    *out = nullptr;
    *count = 0;
    if (FAILED(get_result)) return get_result;
    // The guard frees this with CoTaskMemFree, as TSF requires of a provider.
    const UINT size = static_cast<UINT>(scopes.size());
    if (size) {
      auto* buffer = static_cast<InputScope*>(CoTaskMemAlloc(sizeof(InputScope) * size));
      if (!buffer) return E_OUTOFMEMORY;
      std::copy(scopes.begin(), scopes.end(), buffer);
      *out = buffer;
    }
    *count = size;
    return S_OK;
  }
  STDMETHODIMP GetPhrase(BSTR**, UINT* count) override {
    if (count) *count = 0;
    return E_NOTIMPL;
  }
  STDMETHODIMP GetRegularExpression(BSTR*) override { return E_NOTIMPL; }
  STDMETHODIMP GetSRGS(BSTR*) override { return E_NOTIMPL; }
  STDMETHODIMP GetXML(BSTR*) override { return E_NOTIMPL; }

  std::vector<InputScope> scopes;
  HRESULT get_result{S_OK};

 private:
  LONG refs_{1};
};

class FakeInputScopeProperty final : public ITfReadOnlyProperty {
 public:
  STDMETHODIMP QueryInterface(REFIID riid, void** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (riid != IID_IUnknown && riid != IID_ITfReadOnlyProperty) return E_NOINTERFACE;
    *out = static_cast<ITfReadOnlyProperty*>(this);
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override {
    return static_cast<ULONG>(InterlockedIncrement(&refs_));
  }
  STDMETHODIMP_(ULONG) Release() override {
    return static_cast<ULONG>(InterlockedDecrement(&refs_));
  }
  STDMETHODIMP GetType(GUID* type) override {
    if (type) *type = GUID_PROP_INPUTSCOPE;
    return S_OK;
  }
  STDMETHODIMP EnumRanges(TfEditCookie, IEnumTfRanges** out, ITfRange*) override {
    if (out) *out = nullptr;
    return E_NOTIMPL;
  }
  STDMETHODIMP GetValue(TfEditCookie, ITfRange*, VARIANT* value) override {
    if (!value) return E_POINTER;
    VariantInit(value);
    if (FAILED(get_value_result)) return get_value_result;
    if (!variant_is_unknown) {
      value->vt = VT_I4;
      value->lVal = 1;
      return S_OK;
    }
    value->vt = VT_UNKNOWN;
    value->punkVal = static_cast<IUnknown*>(scope);
    if (value->punkVal) value->punkVal->AddRef();
    return S_OK;
  }
  STDMETHODIMP GetContext(ITfContext** context) override {
    if (context) *context = nullptr;
    return E_NOTIMPL;
  }

  ITfInputScope* scope{nullptr};
  bool variant_is_unknown{true};
  HRESULT get_value_result{S_OK};

 private:
  LONG refs_{1};
};

// Only the three calls the guard makes carry behaviour; the rest of ITfContext
// is present because COM demands the whole vtable.
class FakeGuardContext final : public ITfContext {
 public:
  STDMETHODIMP QueryInterface(REFIID riid, void** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (riid != IID_IUnknown && riid != IID_ITfContext) return E_NOINTERFACE;
    *out = static_cast<ITfContext*>(this);
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override {
    return static_cast<ULONG>(InterlockedIncrement(&refs_));
  }
  STDMETHODIMP_(ULONG) Release() override {
    return static_cast<ULONG>(InterlockedDecrement(&refs_));
  }

  STDMETHODIMP RequestEditSession(TfClientId, ITfEditSession* session, DWORD flags,
                                  HRESULT* session_result) override {
    if (!session_result) return E_POINTER;
    last_flags = flags;
    ++request_count;
    if (FAILED(request_result)) {
      *session_result = E_FAIL;
      return request_result;
    }
    if (run_session && session) {
      *session_result = session->DoEditSession(1);
      return S_OK;
    }
    // TF_S_ASYNC stands for an app that defers the session past our answer.
    *session_result = deferred_result;
    return S_OK;
  }
  STDMETHODIMP InWriteSession(TfClientId, BOOL* write) override {
    if (write) *write = FALSE;
    return S_OK;
  }
  STDMETHODIMP GetSelection(TfEditCookie, ULONG, ULONG, TF_SELECTION* selection,
                            ULONG* fetched) override {
    if (!selection || !fetched) return E_POINTER;
    *fetched = 0;
    if (FAILED(selection_result)) return selection_result;
    selection[0] = {};
    selection[0].range = &range;
    range.AddRef();
    *fetched = 1;
    return S_OK;
  }
  STDMETHODIMP SetSelection(TfEditCookie, ULONG, const TF_SELECTION*) override { return E_NOTIMPL; }
  STDMETHODIMP GetStart(TfEditCookie, ITfRange** out) override {
    if (out) *out = nullptr;
    return E_NOTIMPL;
  }
  STDMETHODIMP GetEnd(TfEditCookie, ITfRange** out) override {
    if (out) *out = nullptr;
    return E_NOTIMPL;
  }
  STDMETHODIMP GetActiveView(ITfContextView** out) override {
    if (out) *out = nullptr;
    return E_NOTIMPL;
  }
  STDMETHODIMP EnumViews(IEnumTfContextViews** out) override {
    if (out) *out = nullptr;
    return E_NOTIMPL;
  }
  STDMETHODIMP GetStatus(TF_STATUS* status) override {
    if (status) *status = {};
    return S_OK;
  }
  STDMETHODIMP GetProperty(REFGUID, ITfProperty** out) override {
    if (out) *out = nullptr;
    return E_NOTIMPL;
  }
  STDMETHODIMP GetAppProperty(REFGUID guid, ITfReadOnlyProperty** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!provides_input_scope || guid != GUID_PROP_INPUTSCOPE) return E_NOTIMPL;
    *out = &property;
    property.AddRef();
    return S_OK;
  }
  STDMETHODIMP TrackProperties(const GUID**, ULONG, const GUID**, ULONG,
                               ITfReadOnlyProperty** out) override {
    if (out) *out = nullptr;
    return E_NOTIMPL;
  }
  STDMETHODIMP EnumProperties(IEnumTfProperties** out) override {
    if (out) *out = nullptr;
    return E_NOTIMPL;
  }
  STDMETHODIMP GetDocumentMgr(ITfDocumentMgr** out) override {
    if (out) *out = nullptr;
    return E_NOTIMPL;
  }
  STDMETHODIMP CreateRangeBackup(TfEditCookie, ITfRange*, ITfRangeBackup** out) override {
    if (out) *out = nullptr;
    return E_NOTIMPL;
  }

  FakeRange range;
  FakeInputScopeProperty property;
  bool provides_input_scope{true};
  bool run_session{true};
  HRESULT request_result{S_OK};
  HRESULT deferred_result{TF_S_ASYNC};
  HRESULT selection_result{S_OK};
  int request_count{0};
  DWORD last_flags{0};

 private:
  LONG refs_{1};
};

// Wires a scope provider into the context the way a cooperating app would.
InputScopeClass ClassifyWithScopes(FakeGuardContext& context, FakeInputScope& scope) {
  context.property.scope = &scope;
  return ClassifyInputScope(&context, 1);
}
}  // namespace

TEST(TsfTipAiInputGuardTest, PasswordAndPinScopesClassifyAsPassword) {
  for (const auto value : {IS_PASSWORD, IS_NUMERIC_PASSWORD, IS_NUMERIC_PIN, IS_ALPHANUMERIC_PIN,
                           IS_ALPHANUMERIC_PIN_SET}) {
    FakeGuardContext context;
    FakeInputScope scope({IS_DEFAULT, value});
    EXPECT_EQ(ClassifyWithScopes(context, scope), InputScopeClass::Password) << value;
    const auto gate = CombineInputGate(InputScopeClass::Normal, InputScopeClass::Password);
    EXPECT_FALSE(gate.ai_allowed) << value;
    EXPECT_TRUE(gate.secure) << value;
  }
}

TEST(TsfTipAiInputGuardTest, OrdinaryScopeClassifiesAsNormalAndIsNotSecure) {
  FakeGuardContext context;
  FakeInputScope scope({IS_DEFAULT});
  EXPECT_EQ(ClassifyWithScopes(context, scope), InputScopeClass::Normal);
  EXPECT_EQ(context.last_flags, static_cast<DWORD>(TF_ES_SYNC | TF_ES_READ));
}

TEST(TsfTipAiInputGuardTest, EmptyScopeSetIsUnknownSoAiIsRefusedButLearningIsNot) {
  FakeGuardContext context;
  FakeInputScope scope({});
  // Fail closed for AI (spec section 2) without claiming a password was found:
  // treating "no scopes" as secure would silently stop learning everywhere.
  EXPECT_EQ(ClassifyWithScopes(context, scope), InputScopeClass::Unknown);
  const auto gate = CombineInputGate(InputScopeClass::Normal, InputScopeClass::Unknown);
  EXPECT_FALSE(gate.ai_allowed);
  EXPECT_FALSE(gate.secure);
}

// The one combination that must let AI through. Without this, a regression that
// refused every context would still satisfy every other case in this file.
TEST(TsfTipAiInputGuardTest, BothAxesNormalIsTheOnlyCombinationThatAllowsAi) {
  const auto allowed = CombineInputGate(InputScopeClass::Normal, InputScopeClass::Normal);
  EXPECT_TRUE(allowed.ai_allowed);
  EXPECT_FALSE(allowed.secure);

  for (const auto focus : {InputScopeClass::Unknown, InputScopeClass::Password})
    for (const auto scope :
         {InputScopeClass::Unknown, InputScopeClass::Password, InputScopeClass::Normal})
      EXPECT_FALSE(CombineInputGate(focus, scope).ai_allowed);
  for (const auto scope : {InputScopeClass::Unknown, InputScopeClass::Password})
    EXPECT_FALSE(CombineInputGate(InputScopeClass::Normal, scope).ai_allowed);

  // A password-styled focus window is secure whatever the scope says.
  for (const auto scope :
       {InputScopeClass::Unknown, InputScopeClass::Password, InputScopeClass::Normal})
    EXPECT_TRUE(CombineInputGate(InputScopeClass::Password, scope).secure);
  // An unresolvable focus window alone never claims secure; the foreground-app
  // axis in TextService::ResolvePrivacy is what backstops that case.
  EXPECT_FALSE(CombineInputGate(InputScopeClass::Unknown, InputScopeClass::Normal).secure);
}

TEST(TsfTipAiInputGuardTest, MissingOrUnusableInputScopePropertyIsUnknown) {
  {
    FakeGuardContext context;
    context.provides_input_scope = false;
    EXPECT_EQ(ClassifyInputScope(&context, 1), InputScopeClass::Unknown);
  }
  {
    FakeGuardContext context;
    FakeInputScope scope({IS_DEFAULT});
    context.property.get_value_result = E_FAIL;
    EXPECT_EQ(ClassifyWithScopes(context, scope), InputScopeClass::Unknown);
  }
  {
    FakeGuardContext context;
    FakeInputScope scope({IS_DEFAULT});
    context.property.variant_is_unknown = false;
    EXPECT_EQ(ClassifyWithScopes(context, scope), InputScopeClass::Unknown);
  }
  {
    FakeGuardContext context;
    FakeInputScope scope({IS_DEFAULT});
    scope.get_result = E_FAIL;
    EXPECT_EQ(ClassifyWithScopes(context, scope), InputScopeClass::Unknown);
  }
  {
    FakeGuardContext context;
    FakeInputScope scope({IS_DEFAULT});
    context.selection_result = E_FAIL;
    EXPECT_EQ(ClassifyWithScopes(context, scope), InputScopeClass::Unknown);
  }
}

TEST(TsfTipAiInputGuardTest, DeferredOrRefusedEditSessionIsUnknown) {
  {
    FakeGuardContext context;
    FakeInputScope scope({IS_DEFAULT});
    context.run_session = false;  // App answered but ran the session later.
    EXPECT_EQ(ClassifyWithScopes(context, scope), InputScopeClass::Unknown);
  }
  {
    FakeGuardContext context;
    FakeInputScope scope({IS_DEFAULT});
    context.request_result = TF_E_SYNCHRONOUS;
    EXPECT_EQ(ClassifyWithScopes(context, scope), InputScopeClass::Unknown);
  }
}

TEST(TsfTipAiInputGuardTest, NullContextIsUnknownAndRefusesAi) {
  EXPECT_EQ(ClassifyInputScope(nullptr, 1), InputScopeClass::Unknown);
  const auto gate = azookey::tsf::EvaluateInputGate(nullptr, 1);
  EXPECT_FALSE(gate.ai_allowed);
  EXPECT_FALSE(gate.secure);
}

TEST(TsfTipAiInputGuardTest, PasswordStyledEditWindowClassifiesAsPassword) {
  HWND window = CreateWindowExW(0, L"EDIT", L"", WS_POPUP | ES_PASSWORD, 0, 0, 10, 10, nullptr,
                                nullptr, GetModuleHandleW(nullptr), nullptr);
  ASSERT_NE(window, nullptr);
  EXPECT_EQ(ClassifyFocusWindow(window), InputScopeClass::Password);
  DestroyWindow(window);

  HWND plain = CreateWindowExW(0, L"EDIT", L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
  ASSERT_NE(plain, nullptr);
  EXPECT_EQ(ClassifyFocusWindow(plain), InputScopeClass::Normal);
  DestroyWindow(plain);
}

TEST(TsfTipAiInputGuardTest, NoFocusWindowIsUnknown) {
  EXPECT_EQ(ClassifyFocusWindow(nullptr), InputScopeClass::Unknown);
}

// The bundled list ships with the app and is only ever changed by a reviewed
// repository change (spec section 4.1.1), so it is pinned here verbatim.
TEST(TsfTipSecureAppsTest, BundledDefaultsMatchTheSpecList) {
  const std::vector<std::string_view> expected{
      "keepass.exe",  "keepassxc.exe",          "1password.exe", "bitwarden.exe",
      "lastpass.exe", "credentialuibroker.exe", "lsass.exe"};
  const std::vector<std::string_view> actual(azookey::core::kDefaultSecureApps.begin(),
                                             azookey::core::kDefaultSecureApps.end());
  EXPECT_EQ(actual, expected);
}

TEST(TsfTipSecureAppsTest, EffectiveListIsDefaultsUnionUserAdditionsAndIgnoresCase) {
  const std::vector<std::string> user{"mysecret.exe"};
  EXPECT_TRUE(azookey::core::IsSecureApp("keepass.exe", {}));
  EXPECT_TRUE(azookey::core::IsSecureApp("KeePass.exe", {}));
  EXPECT_TRUE(azookey::core::IsSecureApp("MySecret.EXE", user));
  EXPECT_FALSE(azookey::core::IsSecureApp("notepad.exe", user));
  EXPECT_FALSE(azookey::core::IsSecureApp("", user));
  // A user list cannot subtract a bundled default (spec section 4.1).
  EXPECT_TRUE(azookey::core::IsSecureApp("lsass.exe", user));
}

TEST(TsfTipSecureAppsTest, InvalidSecureAppsDegradeToDefaultsOnly) {
  const auto parse = [](const char* json) {
    const auto value = azookey::ipc::json::Parse(json);
    return value ? azookey::core::ParseSecureApps(*value) : std::vector<std::string>{};
  };
  EXPECT_TRUE(parse(R"({"privacy":{"secureApps":"keepass.exe"}})").empty());
  EXPECT_TRUE(parse(R"({"privacy":{"secureApps":[1,true,""]}})").empty());
  EXPECT_TRUE(parse(R"({"privacy":{}})").empty());
  EXPECT_TRUE(parse(R"({})").empty());
  EXPECT_EQ(parse(R"({"privacy":{"secureApps":["MyApp.EXE",5]}})"),
            std::vector<std::string>{"myapp.exe"});
}
