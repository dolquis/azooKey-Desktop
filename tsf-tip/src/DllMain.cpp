#include <Windows.h>
#include <msctf.h>
#include <olectl.h>
#include <shlwapi.h>

#include <string>

#include "azookey/tsf/DisplayAttribute.h"
#include "azookey/tsf/TextServiceFactory.h"

static HMODULE g_hmod = nullptr;

// Japanese (ja-JP) language id for the registered profile.
static constexpr LANGID kJapaneseLangId = 0x0411;

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
  UNREFERENCED_PARAMETER(reserved);
  if (reason == DLL_PROCESS_ATTACH) g_hmod = module;
  return TRUE;
}

extern "C" STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
  if (!ppv) return E_INVALIDARG;
  *ppv = nullptr;
  if (rclsid != azookey::tsf::kTextServiceClsid) return CLASS_E_CLASSNOTAVAILABLE;
  auto* factory = new azookey::tsf::TextServiceFactory();
  const auto hr = factory->QueryInterface(riid, ppv);
  factory->Release();
  return hr;
}

extern "C" STDAPI DllCanUnloadNow() { return S_FALSE; }

// Writes a REG_SZ value under HKCU.  Returns false on any failure.
static bool RegSetSz(HKEY root, const wchar_t* subkey, const wchar_t* name,
                     const wchar_t* value) {
  HKEY hkey = nullptr;
  if (RegCreateKeyExW(root, subkey, 0, nullptr, 0, KEY_WRITE, nullptr, &hkey, nullptr) !=
      ERROR_SUCCESS)
    return false;
  const DWORD size = static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t));
  const LSTATUS st = RegSetValueExW(hkey, name, 0, REG_SZ,
                                    reinterpret_cast<const BYTE*>(value), size);
  RegCloseKey(hkey);
  return st == ERROR_SUCCESS;
}

class ScopedComInit {
 public:
  ScopedComInit() : hr_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
  ~ScopedComInit() {
    if (hr_ == S_OK || hr_ == S_FALSE) CoUninitialize();
  }

  HRESULT hr() const { return hr_; }
  bool ok() const { return SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE; }

 private:
  HRESULT hr_;
};

extern "C" STDAPI DllRegisterServer() {
  if (!g_hmod) return E_UNEXPECTED;

  wchar_t dll_path[MAX_PATH] = {};
  if (!GetModuleFileNameW(g_hmod, dll_path, MAX_PATH)) return HRESULT_FROM_WIN32(GetLastError());

  // Derive the CLSID string from kTextServiceClsid instead of hand-copying it,
  // so the registry path can never drift from the GUID the class factory uses.
  wchar_t clsid_str[64] = {};
  if (StringFromGUID2(azookey::tsf::kTextServiceClsid, clsid_str, ARRAYSIZE(clsid_str)) == 0)
    return SELFREG_E_CLASS;

  // 1. Standard COM in-proc server registration, machine-wide (HKLM). The TIP
  //    is loaded into arbitrary processes (other users, elevated apps, the
  //    secure desktop), so the CLSID must resolve for all of them — an HKCU
  //    registration would only work for the installing user. Writing HKLM
  //    requires elevation; scripts/register-dev.ps1 elevates before regsvr32.
  const std::wstring clsid_key = std::wstring(L"Software\\Classes\\CLSID\\") + clsid_str;
  const std::wstring inproc = clsid_key + L"\\InprocServer32";
  if (!RegSetSz(HKEY_LOCAL_MACHINE, inproc.c_str(), nullptr, dll_path)) return SELFREG_E_CLASS;
  if (!RegSetSz(HKEY_LOCAL_MACHINE, inproc.c_str(), L"ThreadingModel", L"Apartment"))
    return SELFREG_E_CLASS;
  RegSetSz(HKEY_LOCAL_MACHINE, clsid_key.c_str(), nullptr, L"azooKey TSF TIP");

  ScopedComInit com;
  if (!com.ok()) return SELFREG_E_CLASS;

  // 2. TSF self-registration. This writes the CTF\TIP entries that TSF actually
  //    enumerates. The previously hand-written CLSID\...\Profiles keys were
  //    never read by TSF, which is why azooKey did not appear as an input
  //    method (Linear DEV-157). ITfInputProcessorProfileMgr::RegisterProfile is
  //    the Vista+ recommended call; fall back to the legacy Register +
  //    AddLanguageProfile pair if the manager interface is unavailable.
  ITfInputProcessorProfiles* profiles = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr,
                                CLSCTX_INPROC_SERVER, IID_ITfInputProcessorProfiles,
                                reinterpret_cast<void**>(&profiles));
  if (FAILED(hr) || !profiles) return SELFREG_E_CLASS;

  static constexpr wchar_t kProfileDesc[] = L"azooKey";
  const ULONG desc_len = static_cast<ULONG>(wcslen(kProfileDesc));
  const ULONG path_len = static_cast<ULONG>(wcslen(dll_path));

  ITfInputProcessorProfileMgr* profile_mgr = nullptr;
  if (SUCCEEDED(profiles->QueryInterface(IID_ITfInputProcessorProfileMgr,
                                         reinterpret_cast<void**>(&profile_mgr))) &&
      profile_mgr) {
    // bEnabledByDefault=TRUE so azooKey is enabled and selectable for ja-JP
    // immediately after registration (the M2 flow), instead of requiring the
    // user to add it manually in Settings. This makes it available — not the
    // forced active default — so the user's existing IME is not displaced.
    hr = profile_mgr->RegisterProfile(
        azookey::tsf::kTextServiceClsid, kJapaneseLangId,
        azookey::tsf::kTextServiceProfileGuid, kProfileDesc, desc_len, dll_path, path_len,
        /*uIconIndex=*/0, /*hklsubstitute=*/nullptr, /*dwPreferredLayout=*/0,
        /*bEnabledByDefault=*/TRUE, /*dwFlags=*/0);
    profile_mgr->Release();
  } else {
    hr = profiles->Register(azookey::tsf::kTextServiceClsid);
    if (SUCCEEDED(hr))
      hr = profiles->AddLanguageProfile(azookey::tsf::kTextServiceClsid, kJapaneseLangId,
                                        azookey::tsf::kTextServiceProfileGuid, kProfileDesc,
                                        desc_len, dll_path, path_len, /*uIconIndex=*/0);
    // The legacy AddLanguageProfile has no enabled-by-default parameter, so
    // enable it explicitly to match the RegisterProfile path above.
    if (SUCCEEDED(hr))
      hr = profiles->EnableLanguageProfileByDefault(azookey::tsf::kTextServiceClsid,
                                                    kJapaneseLangId,
                                                    azookey::tsf::kTextServiceProfileGuid, TRUE);
  }
  profiles->Release();
  if (FAILED(hr)) return SELFREG_E_CLASS;

  // 3. Category registration. A keyboard TIP MUST register GUID_TFCAT_TIP_KEYBOARD
  //    or the OS does not treat it as a keyboard input method (so it never
  //    reaches the keyboard list). DISPLAYATTRIBUTEPROVIDER lets TSF resolve our
  //    preedit-underline display attribute (we implement ITfDisplayAttributeProvider).
  //    UIELEMENTENABLED advertises that candidate UI is published through TSF
  //    (ITfUIElementMgr / ITfCandidateListUIElement) so UI-less hosts can activate
  //    and route candidate drawing through their app-side UIElement sink.
  ITfCategoryMgr* cat_mgr = nullptr;
  hr = CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER, IID_ITfCategoryMgr,
                        reinterpret_cast<void**>(&cat_mgr));
  if (FAILED(hr) || !cat_mgr) return SELFREG_E_CLASS;
  static const GUID kCategories[] = {GUID_TFCAT_TIP_KEYBOARD, GUID_TFCAT_DISPLAYATTRIBUTEPROVIDER,
                                     GUID_TFCAT_TIPCAP_UIELEMENTENABLED};
  for (const GUID& category : kCategories) {
    hr = cat_mgr->RegisterCategory(azookey::tsf::kTextServiceClsid, category,
                                   azookey::tsf::kTextServiceClsid);
    if (FAILED(hr)) break;
  }
  cat_mgr->Release();
  if (FAILED(hr)) return SELFREG_E_CLASS;

  return S_OK;
}

extern "C" STDAPI DllUnregisterServer() {
  wchar_t clsid_str[64] = {};
  const bool have_clsid =
      StringFromGUID2(azookey::tsf::kTextServiceClsid, clsid_str, ARRAYSIZE(clsid_str)) != 0;

  {
    ScopedComInit com;
    if (com.ok()) {
      // Remove category registrations. ITfInputProcessorProfiles::Unregister
      // below also clears categories, but unregister them explicitly so a
      // failure partway through still leaves no stale category entries.
      // UnregisterCategory of an absent category is a no-op, so this is safe
      // even after partial registration failures.
      ITfCategoryMgr* cat_mgr = nullptr;
      if (SUCCEEDED(CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                                     IID_ITfCategoryMgr, reinterpret_cast<void**>(&cat_mgr))) &&
          cat_mgr) {
        static const GUID kCategories[] = {GUID_TFCAT_TIP_KEYBOARD,
                                           GUID_TFCAT_DISPLAYATTRIBUTEPROVIDER,
                                           GUID_TFCAT_TIPCAP_UIELEMENTENABLED};
        for (const GUID& category : kCategories)
          cat_mgr->UnregisterCategory(azookey::tsf::kTextServiceClsid, category,
                                      azookey::tsf::kTextServiceClsid);
        cat_mgr->Release();
      }

      // Remove the language profile and the text-service registration.
      ITfInputProcessorProfiles* profiles = nullptr;
      if (SUCCEEDED(CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr,
                                     CLSCTX_INPROC_SERVER, IID_ITfInputProcessorProfiles,
                                     reinterpret_cast<void**>(&profiles))) &&
          profiles) {
        ITfInputProcessorProfileMgr* profile_mgr = nullptr;
        if (SUCCEEDED(profiles->QueryInterface(IID_ITfInputProcessorProfileMgr,
                                               reinterpret_cast<void**>(&profile_mgr))) &&
            profile_mgr) {
          profile_mgr->UnregisterProfile(azookey::tsf::kTextServiceClsid, kJapaneseLangId,
                                         azookey::tsf::kTextServiceProfileGuid, 0);
          profile_mgr->Release();
        }
        // Unregister removes the text service plus any remaining profiles and
        // categories registered under the CLSID.
        profiles->Unregister(azookey::tsf::kTextServiceClsid);
        profiles->Release();
      }
    }
  }

  // Remove the standard COM in-proc server entries we created (HKLM subtree).
  // SHDeleteKeyW tolerates a missing key, so this is safe even if registration
  // never ran or was already removed.
  if (have_clsid) {
    const std::wstring clsid_key = std::wstring(L"Software\\Classes\\CLSID\\") + clsid_str;
    SHDeleteKeyW(HKEY_LOCAL_MACHINE, clsid_key.c_str());
  }

  return S_OK;
}
