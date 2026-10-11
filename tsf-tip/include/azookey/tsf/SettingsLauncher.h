#pragma once

#include <Windows.h>
#include <shellapi.h>

#include <string>
#include <string_view>

namespace azookey::tsf {

HRESULT LaunchSettingsApplication(HWND parent, LANGID langid, REFGUID profile);
// Opens the batch correction view of the settings app on a session pipe
// (rich-features-spec X-3-6). Only the token is on the command line.
HRESULT LaunchSettingsApplicationForProofread(HWND parent, std::string_view session_token);
// azookey_settings.exe next to this module (or the test override).
HRESULT ResolveSettingsExecutable(std::wstring* executable);

#ifdef AZOOKEY_TSF_TESTING
namespace testing {

using ShellExecuteExFn = BOOL(WINAPI*)(SHELLEXECUTEINFOW*);

void SetSettingsLauncherForTest(std::wstring executable, ShellExecuteExFn shell_execute);
void ClearSettingsLauncherForTest();

}  // namespace testing
#endif

}  // namespace azookey::tsf
