#pragma once

#include "azookey/core/PrivacyPolicy.h"

namespace azookey::core {

// M41 body-log gate (docs/dev-infrastructure-spec.md). Body text such as the
// reading or candidates may leave the process only when all of these hold:
// a Debug build (_DEBUG without NDEBUG, decided when azookey_core is compiled),
// the AZOOKEY_LOG_BODY opt-in, a non-secure context, and a privacy policy that
// allows detailed logging. Release builds always return false.
bool BodyLoggingAllowed(const PrivacyPolicy& privacy, bool body_opt_in) noexcept;

// True only when AZOOKEY_LOG_BODY is exactly "1". Any failure reads as false.
bool BodyLogOptInFromEnvironment() noexcept;

}  // namespace azookey::core
