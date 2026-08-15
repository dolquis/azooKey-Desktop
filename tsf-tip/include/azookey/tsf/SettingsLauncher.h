#pragma once

#include <Windows.h>
#include <shellapi.h>

#include <string>

namespace azookey::tsf {

HRESULT LaunchSettingsApplication(HWND parent, LANGID langid, REFGUID profile);

#ifdef AZOOKEY_TSF_TESTING
namespace testing {

using ShellExecuteExFn = BOOL(WINAPI*)(SHELLEXECUTEINFOW*);

void SetSettingsLauncherForTest(std::wstring executable, ShellExecuteExFn shell_execute);
void ClearSettingsLauncherForTest();

}  // namespace testing
#endif

}  // namespace azookey::tsf
