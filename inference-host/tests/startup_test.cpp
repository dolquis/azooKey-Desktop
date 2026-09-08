#include <gtest/gtest.h>

#include <memory>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

#include "azookey/host/HostStartup.h"

TEST(HostStartupTest, NoSupervisorDoesNotRestrictLifetime) {
  const azookey::host::SupervisorLifetime lifetime(0);
  EXPECT_TRUE(lifetime.IsRunning());
}

TEST(HostStartupTest, InvalidSupervisorFailsClosed) {
  const azookey::host::SupervisorLifetime lifetime(0xffffffffU);
  EXPECT_FALSE(lifetime.IsRunning());
  EXPECT_EQ(lifetime.GetState(), azookey::host::SupervisorLifetime::State::Failed);
#ifdef _WIN32
  EXPECT_NE(lifetime.ErrorCode(), 0U);
#endif
}

#ifdef _WIN32
TEST(HostStartupTest, ObservesProcessExitThroughRetainedHandle) {
  wchar_t system_directory[MAX_PATH]{};
  ASSERT_NE(GetSystemDirectoryW(system_directory, MAX_PATH), 0U);
  const std::wstring executable = std::wstring(system_directory) + L"\\cmd.exe";
  std::wstring command = L"\"" + executable + L"\" /d /c exit 0";
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  ASSERT_TRUE(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                             CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                             &process));
  const std::unique_ptr<void, decltype(&CloseHandle)> process_handle(process.hProcess, CloseHandle);
  const std::unique_ptr<void, decltype(&CloseHandle)> thread_handle(process.hThread, CloseHandle);
  const azookey::host::SupervisorLifetime lifetime(process.dwProcessId);
  EXPECT_TRUE(lifetime.IsRunning());
  EXPECT_NE(ResumeThread(thread_handle.get()), static_cast<DWORD>(-1));
  const DWORD result = WaitForSingleObject(process_handle.get(), 5000);
  if (result != WAIT_OBJECT_0) {
    TerminateProcess(process_handle.get(), 1);
  }
  ASSERT_EQ(result, WAIT_OBJECT_0);
  EXPECT_FALSE(lifetime.IsRunning());
  EXPECT_EQ(lifetime.GetState(), azookey::host::SupervisorLifetime::State::Exited);
}
#endif

#if !AZOOKEY_WITH_LLAMA_CPP
TEST(HostStartupTest, MockHostDoesNotAdvertiseVulkan) {
  EXPECT_EQ(azookey::host::ProbeVulkanDevices(), 0U);
}
#endif
