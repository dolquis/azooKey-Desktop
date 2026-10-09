#include "azookey/tsf/TipRuntimeLog.h"

#include <atomic>
#include <exception>
#include <new>
#include <string>

#include "azookey/core/EtwLogger.h"

namespace azookey::tsf {

#ifdef AZOOKEY_TSF_TESTING
namespace {
std::atomic<logging::RuntimeLogger*> g_logger_for_test{nullptr};
}  // namespace

void SetTipRuntimeLoggerForTest(logging::RuntimeLogger* logger) noexcept {
  g_logger_for_test.store(logger);
}
#endif

logging::RuntimeLogger& TipRuntimeLogger() {
#ifdef AZOOKEY_TSF_TESTING
  if (auto* logger = g_logger_for_test.load()) return *logger;
#endif
  static logging::RuntimeLogger logger(logging::RuntimeLoggerOptionsFromEnvironment("tip"));
  return logger;
}

void TipRuntimeLog(logging::RuntimeLogger& logger, logging::RuntimeLogLevel level,
                   std::string_view event, std::initializer_list<logging::RuntimeLogField> fields,
                   core::PrivacyPolicy privacy) {
#ifdef _DEBUG
  const auto record = logger.FormatRecord(level, event, fields, privacy);
  if (!record.empty()) OutputDebugStringA(("[azooKey TIP] " + record + "\n").c_str());
#endif
  logger.Log(level, event, fields, privacy);
}

std::string_view CurrentExceptionKind() noexcept {
  // Rethrowing with no active exception would call std::terminate.
  if (!std::current_exception()) return "none";
  try {
    throw;
  } catch (const std::bad_alloc&) {
    return "bad_alloc";
  } catch (const std::exception&) {
    return "std_exception";
  } catch (...) {
    return "unknown";
  }
}

void LogComBoundaryException(logging::RuntimeLogger& logger, std::string_view operation,
                             HRESULT hr) noexcept {
  // DLL exports and a failed TextService construction run without the
  // provider a live TextService holds, so pin it for this one event.
  core::EtwLogger::Register();
  core::EtwLogger::LogError(core::EtwModule::Tip, core::EtwErrorCode::Business, hr);
  core::EtwLogger::Unregister();
  try {
    TipRuntimeLog(
        logger, logging::RuntimeLogLevel::Error, "com_boundary_exception",
        {{"operation", logging::RuntimeLogSafeText(std::string(operation))},
         {"exception_kind", logging::RuntimeLogSafeText(std::string(CurrentExceptionKind()))},
         {"error_code", logging::RuntimeLogSafeText("business")},
         {"hresult", static_cast<int64_t>(hr)}});
  } catch (...) {
    // Diagnostics are best-effort; the caller still returns its HRESULT.
  }
}

void LogComBoundaryException(std::string_view operation, HRESULT hr) noexcept {
  try {
    LogComBoundaryException(TipRuntimeLogger(), operation, hr);
  } catch (...) {
    // Constructing the static logger must not leak through a COM boundary.
  }
}

}  // namespace azookey::tsf
