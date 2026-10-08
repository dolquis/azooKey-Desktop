#include "azookey/core/BodyLogGate.h"

#include <cstdlib>
#include <memory>
#include <string_view>

namespace azookey::core {

bool BodyLoggingAllowed(const PrivacyPolicy& privacy, bool body_opt_in) noexcept {
  // Compile-time enforcement also covers manually constructed logger options.
#if defined(_DEBUG) && !defined(NDEBUG)
  return body_opt_in && !privacy.secure && privacy.detailed_logging_allowed;
#else
  (void)body_opt_in;
  (void)privacy;
  return false;
#endif
}

bool BodyLogOptInFromEnvironment() noexcept {
  try {
#ifdef _WIN32
    char* raw = nullptr;
    size_t size = 0;
    if (_dupenv_s(&raw, &size, "AZOOKEY_LOG_BODY") != 0 || !raw) return false;
    const std::unique_ptr<char, decltype(&std::free)> buffer(raw, &std::free);
    return std::string_view(buffer.get()) == "1";
#else
    const char* value = std::getenv("AZOOKEY_LOG_BODY");
    return value && std::string_view(value) == "1";
#endif
  } catch (...) {
    return false;
  }
}

}  // namespace azookey::core
