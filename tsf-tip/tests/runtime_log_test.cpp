#include <Windows.h>
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <new>
#include <stdexcept>
#include <string>
#include <system_error>

#include "azookey/tsf/TipRuntimeLog.h"

namespace {

using azookey::logging::RuntimeLogger;
using azookey::logging::RuntimeLoggerOptions;
using azookey::tsf::CurrentExceptionKind;
using azookey::tsf::LogComBoundaryException;

class TipRuntimeLogTest : public ::testing::Test {
 protected:
  void SetUp() override {
    static std::atomic<unsigned> sequence{0};
    root = std::filesystem::temp_directory_path() /
           (L"azookey-tip-runtime-log-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(sequence++));
    std::filesystem::create_directories(root);
    output = root / L"tip.jsonl";
  }
  void TearDown() override {
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }

  RuntimeLoggerOptions Options(bool enabled) const {
    RuntimeLoggerOptions options;
    options.enabled = enabled;
    options.component = "tip";
    options.logs_directory = root;
    options.output_path = output;
    return options;
  }

  std::string ReadOutput() const {
    std::ifstream in(output, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  }

  std::filesystem::path root;
  std::filesystem::path output;
};

TEST_F(TipRuntimeLogTest, RecordsBadAllocWithOperationKindAndHresult) {
  RuntimeLogger logger(Options(true));
  try {
    throw std::bad_alloc();
  } catch (const std::bad_alloc&) {
    LogComBoundaryException(logger, "TextService::OnKeyDown", E_OUTOFMEMORY);
  }

  const std::string log = ReadOutput();
  EXPECT_NE(log.find("\"component\":\"tip\""), std::string::npos) << log;
  EXPECT_NE(log.find("\"level\":\"error\""), std::string::npos) << log;
  EXPECT_NE(log.find("\"event\":\"com_boundary_exception\""), std::string::npos) << log;
  EXPECT_NE(log.find("\"operation\":\"TextService::OnKeyDown\""), std::string::npos) << log;
  EXPECT_NE(log.find("\"exception_kind\":\"bad_alloc\""), std::string::npos) << log;
  EXPECT_NE(log.find("\"hresult\":" + std::to_string(static_cast<int64_t>(E_OUTOFMEMORY))),
            std::string::npos)
      << log;
}

TEST_F(TipRuntimeLogTest, RecordsStdExceptionKindWithoutItsMessage) {
  RuntimeLogger logger(Options(true));
  try {
    throw std::runtime_error("composition-text-must-not-leak");
  } catch (...) {
    LogComBoundaryException(logger, "DllRegisterServer", E_UNEXPECTED);
  }

  const std::string log = ReadOutput();
  EXPECT_NE(log.find("\"operation\":\"DllRegisterServer\""), std::string::npos) << log;
  EXPECT_NE(log.find("\"exception_kind\":\"std_exception\""), std::string::npos) << log;
  EXPECT_EQ(log.find("composition-text-must-not-leak"), std::string::npos) << log;
}

TEST_F(TipRuntimeLogTest, RecordsNonStandardExceptionAsUnknown) {
  RuntimeLogger logger(Options(true));
  try {
    throw 42;
  } catch (...) {
    LogComBoundaryException(logger, "EditSession::DoEditSession", E_FAIL);
  }

  const std::string log = ReadOutput();
  EXPECT_NE(log.find("\"exception_kind\":\"unknown\""), std::string::npos) << log;
  EXPECT_NE(log.find("\"hresult\":" + std::to_string(static_cast<int64_t>(E_FAIL))),
            std::string::npos)
      << log;
}

TEST_F(TipRuntimeLogTest, DisabledLoggerWritesNothing) {
  RuntimeLogger logger(Options(false));
  try {
    throw std::bad_alloc();
  } catch (const std::bad_alloc&) {
    LogComBoundaryException(logger, "DllGetClassObject", E_OUTOFMEMORY);
  }

  EXPECT_FALSE(std::filesystem::exists(output));
}

TEST_F(TipRuntimeLogTest, WriteFailureKeepsTheHresultPath) {
  // A directory at the output path makes the logger's own write fail.
  std::filesystem::create_directories(output);
  RuntimeLogger logger(Options(true));
  HRESULT returned = S_OK;
  try {
    throw std::runtime_error("boom");
  } catch (...) {
    LogComBoundaryException(logger, "DllUnregisterServer", E_UNEXPECTED);
    returned = E_UNEXPECTED;
  }

  EXPECT_EQ(returned, E_UNEXPECTED);
  EXPECT_TRUE(std::filesystem::is_directory(output));
}

TEST_F(TipRuntimeLogTest, ExceptionKindOutsideHandlerIsNone) {
  EXPECT_EQ(CurrentExceptionKind(), "none");
}

}  // namespace
