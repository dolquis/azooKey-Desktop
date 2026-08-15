#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <gtest/gtest.h>
#include <msctf.h>
#include <olectl.h>

#include <string>

#include "azookey/tsf/TextServiceFactory.h"

namespace {

using DllGetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, LPVOID*);

#define WIDEN_LITERAL2(value) L##value
#define WIDEN_LITERAL(value) WIDEN_LITERAL2(value)

class MockThreadMgr final : public ITfThreadMgr,
                            public ITfKeystrokeMgr,
                            public ITfSource {
 public:
  ~MockThreadMgr() {
    if (key_sink_) key_sink_->Release();
    if (thread_sink_) thread_sink_->Release();
  }

  STDMETHODIMP QueryInterface(REFIID riid, void** ppvObject) override {
    if (!ppvObject) return E_POINTER;
    *ppvObject = nullptr;
    if (riid == IID_IUnknown || riid == IID_ITfThreadMgr) {
      *ppvObject = static_cast<ITfThreadMgr*>(this);
    } else if (riid == IID_ITfKeystrokeMgr) {
      *ppvObject = static_cast<ITfKeystrokeMgr*>(this);
    } else if (riid == IID_ITfSource) {
      *ppvObject = static_cast<ITfSource*>(this);
    } else {
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }

  STDMETHODIMP_(ULONG) AddRef() override {
    return static_cast<ULONG>(InterlockedIncrement(&ref_count_));
  }

  STDMETHODIMP_(ULONG) Release() override {
    return static_cast<ULONG>(InterlockedDecrement(&ref_count_));
  }

  STDMETHODIMP Activate(TfClientId* ptid) override {
    if (!ptid) return E_POINTER;
    *ptid = client_id;
    return S_OK;
  }
  STDMETHODIMP Deactivate() override { return S_OK; }
  STDMETHODIMP CreateDocumentMgr(ITfDocumentMgr** ppdim) override {
    if (ppdim) *ppdim = nullptr;
    return E_NOTIMPL;
  }
  STDMETHODIMP EnumDocumentMgrs(IEnumTfDocumentMgrs** ppEnum) override {
    if (ppEnum) *ppEnum = nullptr;
    return E_NOTIMPL;
  }
  STDMETHODIMP GetFocus(ITfDocumentMgr** ppdimFocus) override {
    if (ppdimFocus) *ppdimFocus = nullptr;
    return E_NOTIMPL;
  }
  STDMETHODIMP SetFocus(ITfDocumentMgr* pdimFocus) override {
    UNREFERENCED_PARAMETER(pdimFocus);
    return E_NOTIMPL;
  }
  STDMETHODIMP AssociateFocus(HWND hwnd,
                              ITfDocumentMgr* pdimNew,
                              ITfDocumentMgr** ppdimPrev) override {
    UNREFERENCED_PARAMETER(hwnd);
    UNREFERENCED_PARAMETER(pdimNew);
    if (ppdimPrev) *ppdimPrev = nullptr;
    return E_NOTIMPL;
  }
  STDMETHODIMP IsThreadFocus(BOOL* pfThreadFocus) override {
    if (!pfThreadFocus) return E_POINTER;
    *pfThreadFocus = FALSE;
    return S_OK;
  }
  STDMETHODIMP GetFunctionProvider(REFCLSID clsid,
                                   ITfFunctionProvider** ppFuncProv) override {
    UNREFERENCED_PARAMETER(clsid);
    if (ppFuncProv) *ppFuncProv = nullptr;
    return E_NOTIMPL;
  }
  STDMETHODIMP EnumFunctionProviders(IEnumTfFunctionProviders** ppEnum) override {
    if (ppEnum) *ppEnum = nullptr;
    return E_NOTIMPL;
  }
  STDMETHODIMP GetGlobalCompartment(ITfCompartmentMgr** ppCompMgr) override {
    if (ppCompMgr) *ppCompMgr = nullptr;
    return E_NOTIMPL;
  }

  STDMETHODIMP AdviseKeyEventSink(TfClientId tid,
                                  ITfKeyEventSink* pSink,
                                  BOOL fForeground) override {
    if (!pSink) return E_POINTER;
    key_advise_count++;
    key_advised = true;
    advised_client_id = tid;
    advised_foreground = fForeground;
    key_sink_ = pSink;
    key_sink_->AddRef();
    return S_OK;
  }

  STDMETHODIMP UnadviseKeyEventSink(TfClientId tid) override {
    key_unadvise_count++;
    unadvised_client_id = tid;
    key_advised = false;
    if (key_sink_) {
      key_sink_->Release();
      key_sink_ = nullptr;
    }
    return S_OK;
  }

  STDMETHODIMP GetForeground(CLSID* pclsid) override {
    if (!pclsid) return E_POINTER;
    *pclsid = azookey::tsf::kTextServiceClsid;
    return S_OK;
  }
  STDMETHODIMP TestKeyDown(WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override {
    UNREFERENCED_PARAMETER(wParam);
    UNREFERENCED_PARAMETER(lParam);
    if (pfEaten) *pfEaten = FALSE;
    return E_NOTIMPL;
  }
  STDMETHODIMP TestKeyUp(WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override {
    UNREFERENCED_PARAMETER(wParam);
    UNREFERENCED_PARAMETER(lParam);
    if (pfEaten) *pfEaten = FALSE;
    return E_NOTIMPL;
  }
  STDMETHODIMP KeyDown(WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override {
    UNREFERENCED_PARAMETER(wParam);
    UNREFERENCED_PARAMETER(lParam);
    if (pfEaten) *pfEaten = FALSE;
    return E_NOTIMPL;
  }
  STDMETHODIMP KeyUp(WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override {
    UNREFERENCED_PARAMETER(wParam);
    UNREFERENCED_PARAMETER(lParam);
    if (pfEaten) *pfEaten = FALSE;
    return E_NOTIMPL;
  }
  STDMETHODIMP GetPreservedKey(ITfContext* pic,
                               const TF_PRESERVEDKEY* pprekey,
                               GUID* pguid) override {
    UNREFERENCED_PARAMETER(pic);
    UNREFERENCED_PARAMETER(pprekey);
    if (pguid) *pguid = GUID_NULL;
    return E_NOTIMPL;
  }
  STDMETHODIMP IsPreservedKey(REFGUID rguid,
                              const TF_PRESERVEDKEY* pprekey,
                              BOOL* pfRegistered) override {
    UNREFERENCED_PARAMETER(rguid);
    UNREFERENCED_PARAMETER(pprekey);
    if (pfRegistered) *pfRegistered = FALSE;
    return E_NOTIMPL;
  }
  STDMETHODIMP PreserveKey(TfClientId tid,
                           REFGUID rguid,
                           const TF_PRESERVEDKEY* prekey,
                           const WCHAR* pchDesc,
                           ULONG cchDesc) override {
    UNREFERENCED_PARAMETER(tid);
    UNREFERENCED_PARAMETER(rguid);
    UNREFERENCED_PARAMETER(prekey);
    UNREFERENCED_PARAMETER(pchDesc);
    UNREFERENCED_PARAMETER(cchDesc);
    return E_NOTIMPL;
  }
  STDMETHODIMP UnpreserveKey(REFGUID rguid,
                             const TF_PRESERVEDKEY* pprekey) override {
    UNREFERENCED_PARAMETER(rguid);
    UNREFERENCED_PARAMETER(pprekey);
    return E_NOTIMPL;
  }
  STDMETHODIMP SetPreservedKeyDescription(REFGUID rguid,
                                          const WCHAR* pchDesc,
                                          ULONG cchDesc) override {
    UNREFERENCED_PARAMETER(rguid);
    UNREFERENCED_PARAMETER(pchDesc);
    UNREFERENCED_PARAMETER(cchDesc);
    return E_NOTIMPL;
  }
  STDMETHODIMP GetPreservedKeyDescription(REFGUID rguid,
                                          BSTR* pbstrDesc) override {
    UNREFERENCED_PARAMETER(rguid);
    if (pbstrDesc) *pbstrDesc = nullptr;
    return E_NOTIMPL;
  }
  STDMETHODIMP SimulatePreservedKey(ITfContext* pic,
                                    REFGUID rguid,
                                    BOOL* pfEaten) override {
    UNREFERENCED_PARAMETER(pic);
    UNREFERENCED_PARAMETER(rguid);
    if (pfEaten) *pfEaten = FALSE;
    return E_NOTIMPL;
  }

  STDMETHODIMP AdviseSink(REFIID riid,
                          IUnknown* punk,
                          DWORD* pdwCookie) override {
    if (!punk || !pdwCookie) return E_POINTER;
    if (riid != IID_ITfThreadMgrEventSink) return E_NOINTERFACE;
    source_advise_count++;
    source_advised = true;
    thread_sink_ = punk;
    thread_sink_->AddRef();
    *pdwCookie = sink_cookie;
    return S_OK;
  }

  STDMETHODIMP UnadviseSink(DWORD dwCookie) override {
    source_unadvise_count++;
    unadvised_cookie = dwCookie;
    source_advised = false;
    if (thread_sink_) {
      thread_sink_->Release();
      thread_sink_ = nullptr;
    }
    return dwCookie == sink_cookie ? S_OK : E_INVALIDARG;
  }

  TfClientId client_id{7};
  bool key_advised{false};
  bool source_advised{false};
  int key_advise_count{0};
  int key_unadvise_count{0};
  int source_advise_count{0};
  int source_unadvise_count{0};
  TfClientId advised_client_id{TF_CLIENTID_NULL};
  TfClientId unadvised_client_id{TF_CLIENTID_NULL};
  BOOL advised_foreground{FALSE};
  DWORD sink_cookie{42};
  DWORD unadvised_cookie{TF_INVALID_COOKIE};

 private:
  LONG ref_count_{1};
  ITfKeyEventSink* key_sink_{nullptr};
  IUnknown* thread_sink_{nullptr};
};

}  // namespace

TEST(TsfTipComSmokeTest, DllGetClassObjectCreatesInstance) {
  HMODULE module = LoadLibraryW(WIDEN_LITERAL(AZOOKEY_TSF_TIP_DLL_PATH));
  ASSERT_NE(module, nullptr) << "LoadLibraryW failed";

  auto* proc = reinterpret_cast<DllGetClassObjectFn>(
      GetProcAddress(module, "DllGetClassObject"));
  ASSERT_NE(proc, nullptr) << "DllGetClassObject export not found";

  IClassFactory* factory = nullptr;
  HRESULT hr = proc(azookey::tsf::kTextServiceClsid, IID_IClassFactory,
                    reinterpret_cast<void**>(&factory));
  ASSERT_TRUE(SUCCEEDED(hr) && factory != nullptr) << "DllGetClassObject failed";

  IUnknown* service = nullptr;
  hr = factory->CreateInstance(nullptr, IID_IUnknown,
                               reinterpret_cast<void**>(&service));
  factory->Release();
  ASSERT_TRUE(SUCCEEDED(hr) && service != nullptr)
      << "CreateInstance(IID_IUnknown) failed";

  service->Release();
}

TEST(TsfTipComSmokeTest, ActivateExAdvisesAndDeactivateUnadvisesSinks) {
  HMODULE module = LoadLibraryW(WIDEN_LITERAL(AZOOKEY_TSF_TIP_DLL_PATH));
  ASSERT_NE(module, nullptr) << "LoadLibraryW failed";

  auto* proc = reinterpret_cast<DllGetClassObjectFn>(
      GetProcAddress(module, "DllGetClassObject"));
  ASSERT_NE(proc, nullptr) << "DllGetClassObject export not found";

  IClassFactory* factory = nullptr;
  HRESULT hr = proc(azookey::tsf::kTextServiceClsid, IID_IClassFactory,
                    reinterpret_cast<void**>(&factory));
  ASSERT_TRUE(SUCCEEDED(hr) && factory != nullptr) << "DllGetClassObject failed";

  ITfTextInputProcessorEx* service = nullptr;
  hr = factory->CreateInstance(nullptr, IID_ITfTextInputProcessorEx,
                               reinterpret_cast<void**>(&service));
  factory->Release();
  ASSERT_TRUE(SUCCEEDED(hr) && service != nullptr)
      << "CreateInstance(IID_ITfTextInputProcessorEx) failed";

  MockThreadMgr thread_mgr;
  hr = service->ActivateEx(&thread_mgr, thread_mgr.client_id, 0);
  ASSERT_TRUE(SUCCEEDED(hr)) << "ActivateEx failed";
  EXPECT_TRUE(thread_mgr.key_advised);
  EXPECT_TRUE(thread_mgr.source_advised);
  EXPECT_EQ(thread_mgr.key_advise_count, 1);
  EXPECT_EQ(thread_mgr.source_advise_count, 1);
  EXPECT_EQ(thread_mgr.advised_client_id, thread_mgr.client_id);
  EXPECT_EQ(thread_mgr.advised_foreground, TRUE);

  hr = service->Deactivate();
  EXPECT_TRUE(SUCCEEDED(hr)) << "Deactivate failed";
  EXPECT_FALSE(thread_mgr.key_advised);
  EXPECT_FALSE(thread_mgr.source_advised);
  EXPECT_EQ(thread_mgr.key_unadvise_count, 1);
  EXPECT_EQ(thread_mgr.source_unadvise_count, 1);
  EXPECT_EQ(thread_mgr.unadvised_client_id, thread_mgr.client_id);
  EXPECT_EQ(thread_mgr.unadvised_cookie, thread_mgr.sink_cookie);

  service->Release();
}

namespace {

using DllRegisterFn = HRESULT(STDAPICALLTYPE*)();

bool IsProcessElevated() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
  TOKEN_ELEVATION elevation{};
  DWORD size = 0;
  const BOOL ok =
      GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
  CloseHandle(token);
  return ok && elevation.TokenIsElevated != 0;
}

// Opt-in gate. The round-trip mutates machine-wide TSF state and depends on an
// interactive TSF session (ctfmon), so an elevation check alone is not enough to
// keep it out of CI: GitHub's Windows runners run elevated but headless, where
// GetProfile cannot observe the just-registered profile. Require an explicit
// env opt-in so it only runs when a developer asks for it on a real session.
bool RegistrationSmokeOptedIn() {
  return GetEnvironmentVariableW(L"AZOOKEY_RUN_REGISTRATION_SMOKE", nullptr, 0) > 0;
}

std::wstring InprocServerKeyPath() {
  wchar_t clsid_str[64] = {};
  StringFromGUID2(azookey::tsf::kTextServiceClsid, clsid_str, ARRAYSIZE(clsid_str));
  return std::wstring(L"Software\\Classes\\CLSID\\") + clsid_str + L"\\InprocServer32";
}

bool InprocServerKeyExists() {
  HKEY hkey = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, InprocServerKeyPath().c_str(), 0, KEY_READ, &hkey) !=
      ERROR_SUCCESS)
    return false;
  RegCloseKey(hkey);
  return true;
}

HRESULT GetCategoryRegistrationState(REFGUID category, bool* registered) {
  if (!registered) return E_POINTER;
  *registered = false;

  ITfCategoryMgr* cat_mgr = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                                IID_ITfCategoryMgr, reinterpret_cast<void**>(&cat_mgr));
  if (FAILED(hr)) return hr;
  if (!cat_mgr) return E_NOINTERFACE;

  IEnumGUID* items = nullptr;
  hr = cat_mgr->EnumItemsInCategory(category, &items);
  cat_mgr->Release();
  if (FAILED(hr)) return hr;
  if (!items) return E_NOINTERFACE;

  GUID item{};
  ULONG fetched = 0;
  while ((hr = items->Next(1, &item, &fetched)) == S_OK) {
    if (fetched != 1) {
      hr = E_FAIL;
      break;
    }
    if (IsEqualGUID(item, azookey::tsf::kTextServiceClsid)) {
      *registered = true;
      break;
    }
  }
  items->Release();
  return hr == S_FALSE ? S_OK : hr;
}

struct RequiredCategory {
  const GUID* guid;
  const char* name;
};

const RequiredCategory kExpectedRegistrationCategories[] = {
    {&GUID_TFCAT_TIP_KEYBOARD, "GUID_TFCAT_TIP_KEYBOARD"},
    {&GUID_TFCAT_DISPLAYATTRIBUTEPROVIDER, "GUID_TFCAT_DISPLAYATTRIBUTEPROVIDER"},
    {&GUID_TFCAT_TIPCAP_UIELEMENTENABLED, "GUID_TFCAT_TIPCAP_UIELEMENTENABLED"},
    {&GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT, "GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT"},
};

#ifndef NDEBUG
HRESULT GetProfileRegistrationState(bool* registered) {
  if (!registered) return E_POINTER;
  *registered = false;

  ITfInputProcessorProfiles* profiles = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                                IID_ITfInputProcessorProfiles, reinterpret_cast<void**>(&profiles));
  if (FAILED(hr)) return hr;
  if (!profiles) return E_NOINTERFACE;

  ITfInputProcessorProfileMgr* profile_mgr = nullptr;
  hr = profiles->QueryInterface(IID_ITfInputProcessorProfileMgr,
                                reinterpret_cast<void**>(&profile_mgr));
  profiles->Release();
  if (FAILED(hr)) return hr;
  if (!profile_mgr) return E_NOINTERFACE;

  IEnumTfInputProcessorProfiles* items = nullptr;
  hr = profile_mgr->EnumProfiles(0x0411, &items);
  profile_mgr->Release();
  if (FAILED(hr)) return hr;
  if (!items) return E_NOINTERFACE;

  TF_INPUTPROCESSORPROFILE profile{};
  ULONG fetched = 0;
  while ((hr = items->Next(1, &profile, &fetched)) == S_OK) {
    if (fetched != 1) {
      hr = E_FAIL;
      break;
    }
    if (profile.dwProfileType == TF_PROFILETYPE_INPUTPROCESSOR &&
        IsEqualGUID(profile.clsid, azookey::tsf::kTextServiceClsid) &&
        IsEqualGUID(profile.guidProfile, azookey::tsf::kTextServiceProfileGuid)) {
      *registered = true;
      break;
    }
  }
  items->Release();
  return hr == S_FALSE ? S_OK : hr;
}

class ScopedForcedCategoryRegistrationFailure {
 public:
  ScopedForcedCategoryRegistrationFailure()
      : active_(SetEnvironmentVariableW(L"AZOOKEY_TEST_FAIL_CATEGORY_REGISTRATION", L"1") !=
                FALSE) {}
  ~ScopedForcedCategoryRegistrationFailure() {
    if (active_) SetEnvironmentVariableW(L"AZOOKEY_TEST_FAIL_CATEGORY_REGISTRATION", nullptr);
  }

  bool active() const { return active_; }

  bool Clear() {
    if (!active_) return true;
    if (!SetEnvironmentVariableW(L"AZOOKEY_TEST_FAIL_CATEGORY_REGISTRATION", nullptr)) return false;
    active_ = false;
    return true;
  }

 private:
  bool active_{false};
};
#endif

}  // namespace

// Machine-wide registration round-trip. This mutates machine-wide TSF state and
// relies on an interactive TSF session, so it is opt-in (env
// AZOOKEY_RUN_REGISTRATION_SMOKE) and elevation-gated, and never runs in CI:
// GitHub's Windows runners are elevated but headless, so GetProfile cannot see
// the just-registered profile there. A TearDown always unregisters so a failed
// assertion cannot leave azooKey registered machine-wide.
class TsfTipRegistrationSmokeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!RegistrationSmokeOptedIn())
      GTEST_SKIP() << "opt-in only: set AZOOKEY_RUN_REGISTRATION_SMOKE=1 in an "
                      "interactive, elevated session to exercise the round-trip "
                      "(never run in CI; headless runners lack an active TSF session)";
    if (!IsProcessElevated())
      GTEST_SKIP() << "machine-wide TSF registration requires elevation";

    module_ = LoadLibraryW(WIDEN_LITERAL(AZOOKEY_TSF_TIP_DLL_PATH));
    ASSERT_NE(module_, nullptr) << "LoadLibraryW failed";
    register_ = reinterpret_cast<DllRegisterFn>(GetProcAddress(module_, "DllRegisterServer"));
    unregister_ = reinterpret_cast<DllRegisterFn>(GetProcAddress(module_, "DllUnregisterServer"));
    ASSERT_NE(register_, nullptr) << "DllRegisterServer export not found";
    ASSERT_NE(unregister_, nullptr) << "DllUnregisterServer export not found";
    ASSERT_TRUE(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)));
    co_initialized_ = true;
  }

  void TearDown() override {
    if (unregister_) unregister_();  // best-effort cleanup; tolerates missing keys
    if (co_initialized_) CoUninitialize();
    if (module_) FreeLibrary(module_);
  }

  HMODULE module_{nullptr};
  DllRegisterFn register_{nullptr};
  DllRegisterFn unregister_{nullptr};
  bool co_initialized_{false};
};

TEST_F(TsfTipRegistrationSmokeTest, RegisterPublishesProfileAndUnregisterRemovesIt) {
  ASSERT_EQ(register_(), S_OK);
  ASSERT_EQ(register_(), S_OK) << "re-registering the same profile must be idempotent";
  EXPECT_TRUE(InprocServerKeyExists()) << "InprocServer32 not written under HKLM";

  ITfInputProcessorProfiles* profiles = nullptr;
  ASSERT_EQ(CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                             IID_ITfInputProcessorProfiles,
                             reinterpret_cast<void**>(&profiles)),
            S_OK);
  ITfInputProcessorProfileMgr* mgr = nullptr;
  ASSERT_EQ(profiles->QueryInterface(IID_ITfInputProcessorProfileMgr,
                                     reinterpret_cast<void**>(&mgr)),
            S_OK);

  // The whole point of DEV-157: the profile is actually registered with TSF and
  // under the keyboard category, so azooKey can appear as an input method.
  TF_INPUTPROCESSORPROFILE profile{};
  EXPECT_EQ(mgr->GetProfile(TF_PROFILETYPE_INPUTPROCESSOR, 0x0411,
                            azookey::tsf::kTextServiceClsid,
                            azookey::tsf::kTextServiceProfileGuid, nullptr, &profile),
            S_OK)
      << "TSF profile not registered (DEV-157 regression)";
  EXPECT_TRUE(IsEqualGUID(profile.catid, GUID_TFCAT_TIP_KEYBOARD))
      << "profile not registered under GUID_TFCAT_TIP_KEYBOARD";
  for (const RequiredCategory& category : kExpectedRegistrationCategories) {
    bool category_registered = false;
    ASSERT_EQ(GetCategoryRegistrationState(*category.guid, &category_registered), S_OK)
        << "failed to query TSF category: " << category.name;
    EXPECT_TRUE(category_registered) << "required TSF category not registered: " << category.name;
  }

  ASSERT_EQ(unregister_(), S_OK);
  TF_INPUTPROCESSORPROFILE removed{};
  EXPECT_NE(mgr->GetProfile(TF_PROFILETYPE_INPUTPROCESSOR, 0x0411,
                            azookey::tsf::kTextServiceClsid,
                            azookey::tsf::kTextServiceProfileGuid, nullptr, &removed),
            S_OK)
      << "TSF profile still present after DllUnregisterServer";
  EXPECT_FALSE(InprocServerKeyExists()) << "InprocServer32 not removed from HKLM";
  for (const RequiredCategory& category : kExpectedRegistrationCategories) {
    bool category_registered = true;
    ASSERT_EQ(GetCategoryRegistrationState(*category.guid, &category_registered), S_OK)
        << "failed to query TSF category after unregister: " << category.name;
    EXPECT_FALSE(category_registered)
        << "DllUnregisterServer left a TSF category behind: " << category.name;
  }

  mgr->Release();
  profiles->Release();
}

#ifndef NDEBUG
TEST_F(TsfTipRegistrationSmokeTest, FailedCategoryRegistrationRollsBackAndRetrySucceeds) {
  ScopedForcedCategoryRegistrationFailure forced_failure;
  ASSERT_TRUE(forced_failure.active()) << "failed to enable the debug-only failure hook";

  EXPECT_EQ(register_(), SELFREG_E_CLASS);
  EXPECT_FALSE(InprocServerKeyExists()) << "failed registration left InprocServer32 behind";
  bool profile_registered = false;
  ASSERT_EQ(GetProfileRegistrationState(&profile_registered), S_OK)
      << "failed to enumerate TSF profiles after rollback";
  EXPECT_FALSE(profile_registered) << "failed registration left the TSF profile behind";

  for (const RequiredCategory& category : kExpectedRegistrationCategories) {
    bool category_registered = true;
    ASSERT_EQ(GetCategoryRegistrationState(*category.guid, &category_registered), S_OK)
        << "failed to enumerate TSF category after rollback: " << category.name;
    EXPECT_FALSE(category_registered)
        << "failed registration left a TSF category behind: " << category.name;
  }

  ASSERT_TRUE(forced_failure.Clear()) << "failed to disable the debug-only failure hook";
  ASSERT_EQ(register_(), S_OK) << "registration retry required an explicit unregister";
  EXPECT_TRUE(InprocServerKeyExists());
  ASSERT_EQ(GetProfileRegistrationState(&profile_registered), S_OK)
      << "failed to enumerate TSF profiles after retry";
  EXPECT_TRUE(profile_registered);
}
#endif
