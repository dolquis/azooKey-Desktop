// azookey_ime_switch.exe: moves the interactive session off azooKey before the
// TIP registration is removed (DEV-1654). A GUI-subsystem helper so the MSI
// custom action never flashes a console. It always exits without blocking the
// uninstall: the outcome is logged and reflected only in the exit code.

#include <Windows.h>
#include <msctf.h>

#include <string>
#include <vector>

#include "ImeSwitchPolicy.h"
#include "azookey/logging/RuntimeLogger.h"
#include "azookey/tsf/TextServiceFactory.h"

namespace {

using azookey::tsf::ime_switch::ProfileApi;
using azookey::tsf::ime_switch::ProfileEntry;
using azookey::tsf::ime_switch::SwitchOutcome;

constexpr LANGID kJapaneseLangId = 0x0411;

ProfileEntry ToEntry(const TF_INPUTPROCESSORPROFILE& profile) {
  ProfileEntry entry;
  entry.is_layout = profile.dwProfileType == TF_PROFILETYPE_KEYBOARDLAYOUT;
  entry.clsid = profile.clsid;
  entry.profile = profile.guidProfile;
  entry.lang = profile.langid;
  entry.hkl = profile.hkl;
  entry.enabled = (profile.dwFlags & TF_IPP_FLAG_ENABLED) != 0;
  return entry;
}

class TsfProfileApi final : public ProfileApi {
 public:
  explicit TsfProfileApi(ITfInputProcessorProfileMgr* manager) : manager_(manager) {}

  std::optional<ProfileEntry> ActiveProfile() override {
    TF_INPUTPROCESSORPROFILE active{};
    if (FAILED(manager_->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &active))) return std::nullopt;
    return ToEntry(active);
  }

  HRESULT EnumProfiles(LANGID lang, std::vector<ProfileEntry>* out) override {
    IEnumTfInputProcessorProfiles* raw = nullptr;
    HRESULT hr = manager_->EnumProfiles(lang, &raw);
    if (FAILED(hr) || !raw) return FAILED(hr) ? hr : E_FAIL;
    TF_INPUTPROCESSORPROFILE profile{};
    ULONG fetched = 0;
    while (raw->Next(1, &profile, &fetched) == S_OK && fetched == 1) {
      out->push_back(ToEntry(profile));
    }
    raw->Release();
    return S_OK;
  }

  HRESULT Activate(const ProfileEntry& target) override {
    return manager_->ActivateProfile(
        target.is_layout ? TF_PROFILETYPE_KEYBOARDLAYOUT : TF_PROFILETYPE_INPUTPROCESSOR,
        target.lang, target.clsid, target.profile, target.hkl, TF_IPPMF_FORSESSION);
  }

 private:
  ITfInputProcessorProfileMgr* manager_;
};

const char* OutcomeName(SwitchOutcome outcome) {
  switch (outcome) {
    case SwitchOutcome::Switched:
      return "switched";
    case SwitchOutcome::AlreadyElsewhere:
      return "already_elsewhere";
    case SwitchOutcome::NoTarget:
      return "no_target";
    case SwitchOutcome::EnumFailed:
      return "enum_failed";
    case SwitchOutcome::ActivateFailed:
      return "activate_failed";
  }
  return "unknown";
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  // Unlike the opt-in TIP logger this one-shot cleanup always logs: a failed
  // switch is the only trace of why a session kept a dead input method.
  auto options = azookey::logging::RuntimeLoggerOptionsFromEnvironment("ime_switch");
  options.enabled = true;
  azookey::logging::RuntimeLogger logger(options);

  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const bool com_ready = SUCCEEDED(com);
  if (!com_ready && com != RPC_E_CHANGED_MODE) {
    logger.Log(azookey::logging::RuntimeLogLevel::Warn, "ime_switch_com_init_failed",
               {{"hr", static_cast<int64_t>(com)}});
    return 1;
  }

  int exit_code = 1;
  ITfInputProcessorProfileMgr* manager = nullptr;
  const HRESULT created =
      CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                       IID_ITfInputProcessorProfileMgr, reinterpret_cast<void**>(&manager));
  if (FAILED(created) || !manager) {
    logger.Log(azookey::logging::RuntimeLogLevel::Warn, "ime_switch_profile_mgr_failed",
               {{"hr", static_cast<int64_t>(created)}});
  } else {
    TsfProfileApi api(manager);
    const SwitchOutcome outcome = azookey::tsf::ime_switch::SwitchAwayFromOwnProfile(
        api, azookey::tsf::kTextServiceClsid, kJapaneseLangId);
    exit_code = azookey::tsf::ime_switch::ExitCodeFor(outcome);
    logger.Log(exit_code == 0 ? azookey::logging::RuntimeLogLevel::Info
                              : azookey::logging::RuntimeLogLevel::Warn,
               "ime_switch_result",
               {{"outcome", azookey::logging::RuntimeLogSafeText(OutcomeName(outcome))}});
    manager->Release();
  }

  if (com_ready) CoUninitialize();
  return exit_code;
}
