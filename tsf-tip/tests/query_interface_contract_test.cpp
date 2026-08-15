#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <gtest/gtest.h>
#include <msctf.h>

#include <string>
#include <vector>

#include "azookey/tsf/CandidateListUIElement.h"
#include "azookey/tsf/DisplayAttribute.h"
#include "azookey/tsf/SettingsLauncher.h"
#include "azookey/tsf/TextService.h"
#include "azookey/tsf/TextServiceFactory.h"

namespace {

template <typename T>
void ExpectNullOutParamReturnsPointer(T& object) {
  EXPECT_EQ(object.QueryInterface(IID_IUnknown, nullptr), E_POINTER);
}

template <typename T>
void ExpectUnsupportedInterfaceClearsOutParam(T& object) {
  void* out = &object;
  EXPECT_EQ(object.QueryInterface(IID_IDispatch, &out), E_NOINTERFACE);
  EXPECT_EQ(out, nullptr);
}

struct ShellExecuteCapture {
  HWND parent = nullptr;
  std::wstring executable;
  std::wstring parameters;
  std::wstring working_directory;
  ULONG mask = 0;
  int show = 0;
};

ShellExecuteCapture g_shell_execute_capture;

BOOL WINAPI CaptureShellExecute(SHELLEXECUTEINFOW* info) {
  g_shell_execute_capture.parent = info->hwnd;
  g_shell_execute_capture.executable = info->lpFile;
  g_shell_execute_capture.parameters = info->lpParameters;
  g_shell_execute_capture.working_directory = info->lpDirectory;
  g_shell_execute_capture.mask = info->fMask;
  g_shell_execute_capture.show = info->nShow;
  return TRUE;
}

BOOL WINAPI FailShellExecuteWithoutLastError(SHELLEXECUTEINFOW*) {
  SetLastError(ERROR_SUCCESS);
  return FALSE;
}

struct SettingsLauncherReset {
  ~SettingsLauncherReset() { azookey::tsf::testing::ClearSettingsLauncherForTest(); }
};

}  // namespace

TEST(TsfTipQueryInterfaceContractTest, NullOutParamReturnsPointer) {
  azookey::tsf::TextService service;
  azookey::tsf::EditSession edit_session(&service, nullptr);
  azookey::tsf::TextServiceFactory factory;
  azookey::tsf::InputDisplayAttributeInfo input_attribute;
  azookey::tsf::EnumDisplayAttributeInfo attribute_enumerator;
  azookey::tsf::CandidateListUIElement candidates({L"candidate"}, 0);

  ExpectNullOutParamReturnsPointer(service);
  ExpectNullOutParamReturnsPointer(edit_session);
  ExpectNullOutParamReturnsPointer(factory);
  ExpectNullOutParamReturnsPointer(input_attribute);
  ExpectNullOutParamReturnsPointer(attribute_enumerator);
  ExpectNullOutParamReturnsPointer(candidates);
}

TEST(TsfTipQueryInterfaceContractTest, UnsupportedInterfaceClearsOutParam) {
  azookey::tsf::TextService service;
  azookey::tsf::EditSession edit_session(&service, nullptr);
  azookey::tsf::TextServiceFactory factory;
  azookey::tsf::InputDisplayAttributeInfo input_attribute;
  azookey::tsf::EnumDisplayAttributeInfo attribute_enumerator;
  azookey::tsf::CandidateListUIElement candidates({L"candidate"}, 0);

  ExpectUnsupportedInterfaceClearsOutParam(service);
  ExpectUnsupportedInterfaceClearsOutParam(edit_session);
  ExpectUnsupportedInterfaceClearsOutParam(factory);
  ExpectUnsupportedInterfaceClearsOutParam(input_attribute);
  ExpectUnsupportedInterfaceClearsOutParam(attribute_enumerator);
  ExpectUnsupportedInterfaceClearsOutParam(candidates);
}

TEST(TsfTipQueryInterfaceContractTest, TextServiceExposesConfigureFunction) {
  azookey::tsf::TextService service;
  ITfFnConfigure* configure = nullptr;

  ASSERT_EQ(service.QueryInterface(IID_ITfFnConfigure, reinterpret_cast<void**>(&configure)), S_OK);
  ASSERT_NE(configure, nullptr);

  IUnknown* service_identity = nullptr;
  IUnknown* configure_identity = nullptr;
  ASSERT_EQ(service.QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&service_identity)),
            S_OK);
  ASSERT_EQ(configure->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&configure_identity)),
            S_OK);
  EXPECT_EQ(service_identity, configure_identity);

  BSTR display_name = nullptr;
  EXPECT_EQ(configure->GetDisplayName(&display_name), S_OK);
  ASSERT_NE(display_name, nullptr);
  EXPECT_STREQ(display_name, L"azooKey Settings");
  SysFreeString(display_name);

  configure_identity->Release();
  service_identity->Release();
  configure->Release();
}

TEST(TsfTipQueryInterfaceContractTest, TextServiceExposesFunctionBaseInterface) {
  azookey::tsf::TextService service;
  ITfFunction* function = nullptr;

  ASSERT_EQ(service.QueryInterface(IID_ITfFunction, reinterpret_cast<void**>(&function)), S_OK);
  ASSERT_NE(function, nullptr);

  IUnknown* service_identity = nullptr;
  IUnknown* function_identity = nullptr;
  ASSERT_EQ(service.QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&service_identity)),
            S_OK);
  ASSERT_EQ(function->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&function_identity)),
            S_OK);
  EXPECT_EQ(service_identity, function_identity);

  function_identity->Release();
  service_identity->Release();
  function->Release();
}

TEST(TsfTipQueryInterfaceContractTest, ConfigureShowLaunchesSettingsWithProfileContext) {
  SettingsLauncherReset reset;
  g_shell_execute_capture = {};
  constexpr wchar_t kExecutable[] = L"C:\\Program Files\\azooKey\\azookey_settings.exe";
  azookey::tsf::testing::SetSettingsLauncherForTest(kExecutable, CaptureShellExecute);

  azookey::tsf::TextService service;
  ITfFnConfigure* configure = nullptr;
  ASSERT_EQ(service.QueryInterface(IID_ITfFnConfigure, reinterpret_cast<void**>(&configure)), S_OK);
  ASSERT_NE(configure, nullptr);

  const HWND parent = GetDesktopWindow();
  constexpr GUID kProfile = {
      0x8d80b94e, 0x75ba, 0x47d4, {0x90, 0xb9, 0x68, 0x97, 0x7a, 0x5a, 0x6e, 0x2b}};
  EXPECT_EQ(configure->Show(parent, 0x0411, kProfile), S_OK);
  EXPECT_EQ(g_shell_execute_capture.parent, parent);
  EXPECT_EQ(g_shell_execute_capture.executable, kExecutable);
  EXPECT_EQ(g_shell_execute_capture.parameters,
            L"--langid 0x0411 --profile {8D80B94E-75BA-47D4-90B9-68977A5A6E2B}");
  EXPECT_EQ(g_shell_execute_capture.working_directory, L"C:\\Program Files\\azooKey");
  EXPECT_NE(g_shell_execute_capture.mask & SEE_MASK_NOCLOSEPROCESS, 0u);
  EXPECT_NE(g_shell_execute_capture.mask & SEE_MASK_FLAG_LOG_USAGE, 0u);
  EXPECT_EQ(g_shell_execute_capture.show, SW_SHOWNORMAL);

  configure->Release();
}

TEST(TsfTipQueryInterfaceContractTest, ConfigureShowReturnsFailureWhenShellHasNoLastError) {
  SettingsLauncherReset reset;
  constexpr wchar_t kExecutable[] = L"C:\\Program Files\\azooKey\\azookey_settings.exe";
  azookey::tsf::testing::SetSettingsLauncherForTest(kExecutable, FailShellExecuteWithoutLastError);

  azookey::tsf::TextService service;
  constexpr GUID kProfile = {
      0x8d80b94e, 0x75ba, 0x47d4, {0x90, 0xb9, 0x68, 0x97, 0x7a, 0x5a, 0x6e, 0x2b}};
  EXPECT_EQ(service.Show(GetDesktopWindow(), 0x0411, kProfile), E_FAIL);
}
