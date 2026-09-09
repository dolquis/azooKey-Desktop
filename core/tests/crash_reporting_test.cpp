#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "azookey/core/CrashReporting.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <DbgHelp.h>
#include <process.h>

namespace {
namespace fs = std::filesystem;
using namespace azookey::core;
constexpr DWORD kPreviousFilterExit = 73;

LONG WINAPI PreviousFilter(EXCEPTION_POINTERS*) {
  ExitProcess(kPreviousFilterExit);
}

class CrashReportingTest : public testing::Test {
 protected:
  fs::path root;
  fs::path directory;
  std::optional<DWORD> RunProbe(const wchar_t* mode) {
    wchar_t executable[32768]{};
    const auto length = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
    if (length == 0 || length >= std::size(executable)) {
      ADD_FAILURE() << "Cannot resolve test executable";
      return std::nullopt;
    }
    const auto child = _wspawnl(_P_NOWAIT, executable, executable, mode, directory.c_str(), nullptr);
    if (child == -1) {
      ADD_FAILURE() << "Cannot start crash probe";
      return std::nullopt;
    }
    const auto process = reinterpret_cast<HANDLE>(child);
    const DWORD waited = WaitForSingleObject(process, 5000);
    DWORD exit_code{};
    const bool exited = waited == WAIT_OBJECT_0 && GetExitCodeProcess(process, &exit_code);
    if (!exited) {
      TerminateProcess(process, 74);
      WaitForSingleObject(process, 1000);
      ADD_FAILURE() << "Crash probe did not exit within 5 seconds; wait=" << waited;
    }
    CloseHandle(process);
    return exited ? std::optional<DWORD>(exit_code) : std::nullopt;
  }
  void SetUp() override {
    do {
      root = fs::temp_directory_path() /
             ("azookey-crash-report-test-" + std::to_string(std::random_device{}()));
    } while (!fs::create_directory(root));
    directory = root / "crashes";
  }
  void TearDown() override {
    CrashReporting::Shutdown();
    std::error_code ec;
    fs::remove_all(root, ec);
  }
};

TEST_F(CrashReportingTest, OffNeverCreatesArtifactsAndOptOutTakesEffect) {
  CrashReporting::Initialize(CrashModule::Host, CrashConsent::Off, directory);
  EXPECT_EQ(CrashReporting::Status(), CrashStatus::Disabled);
  EXPECT_FALSE(CrashReporting::WriteReport(42, 43, 44));
  EXPECT_FALSE(fs::exists(directory));
  CrashReporting::SetConsent(CrashConsent::Local);
  EXPECT_EQ(CrashReporting::Status(), CrashStatus::Ready);
  CrashReporting::SetConsent(CrashConsent::Off);
  EXPECT_FALSE(CrashReporting::WriteReport(42, 43, 44));
  EXPECT_TRUE(fs::is_empty(directory));
}

TEST_F(CrashReportingTest, InvalidDestinationFallsBackWithoutThrowing) {
  std::ofstream(directory) << "not a directory";
  CrashReporting::Initialize(CrashModule::Settings, CrashConsent::Local, directory);
  EXPECT_EQ(CrashReporting::Status(), CrashStatus::DirectoryUnavailable);
  EXPECT_FALSE(CrashReporting::WriteReport(42, 43, 44));
  EXPECT_TRUE(fs::is_regular_file(directory));
}

TEST_F(CrashReportingTest, RecreatesRemovedDirectoryAndDoesNotOverwriteExistingReport) {
  CrashReporting::Initialize(CrashModule::Host, CrashConsent::Local, directory);
  fs::remove(directory);
  ASSERT_TRUE(CrashReporting::WriteReport(42, 43, 44));
  const auto first = fs::directory_iterator(directory)->path();
  ASSERT_TRUE(fs::is_regular_file(first));
  const auto size = fs::file_size(first);
  // A same-second report may be refused; it must never truncate the first.
  (void)CrashReporting::WriteReport(52, 53, 54);
  EXPECT_EQ(fs::file_size(first), size);
}

TEST_F(CrashReportingTest, ControlledCrashProducesOnlyAllowlistedStreamsWithoutMemory) {
  const auto exit_code = RunProbe(L"--crash-probe");
  ASSERT_TRUE(exit_code);
  ASSERT_NE(*exit_code, 0u);
  EXPECT_NE(*exit_code, kPreviousFilterExit);
  ASSERT_TRUE(fs::exists(directory));
  const auto first = fs::directory_iterator(directory);
  ASSERT_NE(first, fs::directory_iterator{});
  const auto path = first->path();
  std::ifstream input(path, std::ios::binary);
  std::string data{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  ASSERT_GE(data.size(), sizeof(MINIDUMP_HEADER));
  EXPECT_EQ(data.find("private-input-candidate-prompt-api-key-sentinel"), std::string::npos);
  EXPECT_EQ(data.find(root.string()), std::string::npos);
  const auto* header = reinterpret_cast<const MINIDUMP_HEADER*>(data.data());
  EXPECT_EQ(header->Signature, MINIDUMP_SIGNATURE);
  ASSERT_EQ(header->NumberOfStreams, 4u);
  const auto bounded = [&](size_t offset, size_t length) {
    return offset <= data.size() && length <= data.size() - offset;
  };
  ASSERT_TRUE(bounded(header->StreamDirectoryRva,
                      header->NumberOfStreams * sizeof(MINIDUMP_DIRECTORY)));
  const auto* entries = reinterpret_cast<const MINIDUMP_DIRECTORY*>(
      data.data() + header->StreamDirectoryRva);
  for (ULONG index = 0; index < header->NumberOfStreams; ++index) {
    const auto& stream = entries[index];
    ASSERT_TRUE(bounded(stream.Location.Rva, stream.Location.DataSize));
    EXPECT_GE(stream.Location.Rva, header->StreamDirectoryRva +
                                      header->NumberOfStreams * sizeof(MINIDUMP_DIRECTORY));
    EXPECT_TRUE(stream.StreamType == SystemInfoStream || stream.StreamType == ExceptionStream ||
                stream.StreamType == ThreadListStream || stream.StreamType == ModuleListStream);
    for (ULONG previous = 0; previous < index; ++previous) {
      EXPECT_NE(entries[previous].StreamType, stream.StreamType);
      const auto& other = entries[previous].Location;
      EXPECT_TRUE(stream.Location.Rva + stream.Location.DataSize <= other.Rva ||
                  other.Rva + other.DataSize <= stream.Location.Rva);
    }
  }
  const auto read = [&](ULONG type, ULONG size) -> void* {
    PMINIDUMP_DIRECTORY stream{};
    void* value{};
    ULONG length{};
    if (!MiniDumpReadDumpStream(data.data(), type, &stream, &value, &length) || length < size) {
      ADD_FAILURE() << "Missing or truncated stream " << type;
      return nullptr;
    }
    return value;
  };
  const auto* exception = static_cast<MINIDUMP_EXCEPTION_STREAM*>(read(ExceptionStream, sizeof(MINIDUMP_EXCEPTION_STREAM)));
  ASSERT_NE(exception, nullptr);
  EXPECT_EQ(exception->ExceptionRecord.ExceptionCode, 0xe0420001u);
  EXPECT_EQ(exception->ExceptionRecord.NumberParameters, 0u);
  EXPECT_EQ(exception->ThreadContext.DataSize, 0u);
  EXPECT_EQ(exception->ThreadContext.Rva, 0u);
  const auto* threads = static_cast<MINIDUMP_THREAD_LIST*>(
      read(ThreadListStream, sizeof(ULONG32) + sizeof(MINIDUMP_THREAD)));
  ASSERT_NE(threads, nullptr);
  ASSERT_EQ(threads->NumberOfThreads, 1u);
  EXPECT_EQ(threads->Threads[0].Stack.Memory.DataSize, 0u);
  EXPECT_EQ(threads->Threads[0].Stack.Memory.Rva, 0u);
  EXPECT_EQ(threads->Threads[0].Stack.StartOfMemoryRange, 0u);
  EXPECT_EQ(threads->Threads[0].ThreadContext.DataSize, 0u);
  EXPECT_EQ(threads->Threads[0].ThreadContext.Rva, 0u);
  EXPECT_EQ(threads->Threads[0].Teb, 0u);
  const auto* modules = static_cast<MINIDUMP_MODULE_LIST*>(
      read(ModuleListStream, sizeof(ULONG32) + sizeof(MINIDUMP_MODULE)));
  ASSERT_NE(modules, nullptr);
  ASSERT_EQ(modules->NumberOfModules, 1u);
  ASSERT_TRUE(bounded(modules->Modules[0].ModuleNameRva, sizeof(ULONG32)));
  const auto* name = reinterpret_cast<const MINIDUMP_STRING*>(data.data() + modules->Modules[0].ModuleNameRva);
  ASSERT_EQ(name->Length % sizeof(wchar_t), 0u);
  ASSERT_TRUE(bounded(modules->Modules[0].ModuleNameRva,
                      sizeof(ULONG32) + static_cast<size_t>(name->Length)));
  EXPECT_EQ(std::wstring(name->Buffer, name->Length / sizeof(wchar_t)), L"azookey_inference_host.exe");
  EXPECT_EQ(modules->Modules[0].CvRecord.DataSize, 0u);
  EXPECT_EQ(modules->Modules[0].MiscRecord.DataSize, 0u);
  EXPECT_NE(read(SystemInfoStream, sizeof(MINIDUMP_SYSTEM_INFO)), nullptr);
}

TEST_F(CrashReportingTest, WritingReservesCapacityAndPrunesOldestAcrossModules) {
  fs::create_directory(directory);
  const auto now = fs::file_time_type::clock::now();
  std::vector<fs::path> existing;
  for (int index = 0; index < 5; ++index) {
    const auto prefix = index % 2 == 0 ? "azookey-host-" : "azookey-settings-";
    const auto path = directory / (std::string(prefix) + "20260101T000000Z-" +
                                   std::to_string(20000 + index) + ".dmp");
    std::ofstream(path, std::ios::binary) << "existing report";
    fs::last_write_time(path, now - std::chrono::hours(index + 1));
    existing.push_back(path);
  }
  CrashReporting::Initialize(CrashModule::Host, CrashConsent::Local, directory);
  ASSERT_TRUE(CrashReporting::WriteReport(42, 43, 44));
  EXPECT_FALSE(fs::exists(existing.back()));
  for (size_t index = 0; index + 1 < existing.size(); ++index)
    EXPECT_TRUE(fs::exists(existing[index]));
  EXPECT_EQ(std::distance(fs::directory_iterator(directory), fs::directory_iterator{}), 5);
  // Same-second filenames may collide. Neither success nor refusal may exceed the cap.
  for (int attempt = 0; attempt < 3; ++attempt) {
    (void)CrashReporting::WriteReport(52, 53, 54);
    EXPECT_LE(std::distance(fs::directory_iterator(directory), fs::directory_iterator{}), 5);
  }
  EXPECT_TRUE(fs::exists(existing.front()));
}

TEST_F(CrashReportingTest, OffCrashUsesPreviousFilterWithoutCreatingDirectory) {
  const auto exit_code = RunProbe(L"--crash-off-probe");
  ASSERT_TRUE(exit_code);
  EXPECT_EQ(*exit_code, kPreviousFilterExit);
  EXPECT_FALSE(fs::exists(directory));
}

TEST_F(CrashReportingTest, FailedCrashWriteReachesPreviousFilterWithinDeadline) {
  std::ofstream(directory) << "preserve-existing-file";
  const auto exit_code = RunProbe(L"--crash-failure-probe");
  ASSERT_TRUE(exit_code);
  EXPECT_EQ(*exit_code, kPreviousFilterExit);
  ASSERT_TRUE(fs::is_regular_file(directory));
  std::ifstream input(directory);
  std::string contents{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  EXPECT_EQ(contents, "preserve-existing-file");
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc == 3 && (std::wstring_view(argv[1]) == L"--crash-probe" ||
                    std::wstring_view(argv[1]) == L"--crash-off-probe" ||
                    std::wstring_view(argv[1]) == L"--crash-failure-probe")) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetUnhandledExceptionFilter(&PreviousFilter);
    // Deliberately resident on the crashing stack; it must never enter the artifact.
    volatile char sentinel[] = "private-input-candidate-prompt-api-key-sentinel";
    (void)sentinel;
    azookey::core::CrashReporting::Initialize(azookey::core::CrashModule::Host,
        std::wstring_view(argv[1]) == L"--crash-off-probe" ? azookey::core::CrashConsent::Off
                                                         : azookey::core::CrashConsent::Local,
        argv[2]);
    RaiseException(0xe0420001, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    return 99;
  }
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
#else
TEST(CrashReportingTest, NonWindowsIsSafeNoop) {
  using namespace azookey::core;
  CrashReporting::Initialize(CrashModule::Host, CrashConsent::Local);
  EXPECT_EQ(CrashReporting::Status(), CrashStatus::Unsupported);
  EXPECT_FALSE(CrashReporting::WriteReport(1, 2, 3));
  CrashReporting::Shutdown();
}
int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
#endif
