#pragma once

#include <Windows.h>

#include <initializer_list>
#include <string_view>

#include "azookey/core/PrivacyPolicy.h"
#include "azookey/logging/RuntimeLogger.h"

namespace azookey::tsf {

// Process-wide opt-in TIP logger (AZOOKEY_LOG=1, component "tip").
logging::RuntimeLogger& TipRuntimeLogger();

#ifdef AZOOKEY_TSF_TESTING
// Routes TipRuntimeLogger() to `logger` until called again with nullptr. Set it
// before constructing a TextService: its IPC client keeps the logger it gets.
void SetTipRuntimeLoggerForTest(logging::RuntimeLogger* logger) noexcept;
#endif

// Writes one record through `logger`. Debug builds also mirror it to
// OutputDebugStringA, matching the TIP diagnostics contract.
void TipRuntimeLog(logging::RuntimeLogger& logger, logging::RuntimeLogLevel level,
                   std::string_view event, std::initializer_list<logging::RuntimeLogField> fields,
                   core::PrivacyPolicy privacy = {});

// Classifies the exception currently being handled as "bad_alloc",
// "std_exception", or "unknown" ("none" outside a catch handler).
std::string_view CurrentExceptionKind() noexcept;

// Records an exception that a COM boundary converted to `hr`. Call it from the
// catch handler before returning. Only the operation name, the exception kind,
// and the HRESULT are recorded (error_code "business", mirrored to the ETW
// Error event); exception messages may carry input text or paths and are never
// logged. Never throws, including on allocation failure.
void LogComBoundaryException(logging::RuntimeLogger& logger, std::string_view operation,
                             HRESULT hr) noexcept;
void LogComBoundaryException(std::string_view operation, HRESULT hr) noexcept;

}  // namespace azookey::tsf
