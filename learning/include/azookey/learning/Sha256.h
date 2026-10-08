#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace azookey::learning {

// Portable FIPS 180-4 SHA-256. Used for backup integrity checks, so it must
// give identical results on every platform and must not depend on OS APIs.
std::array<uint8_t, 32> Sha256(std::string_view data);

// Lowercase hexadecimal form of Sha256 (64 characters).
std::string Sha256Hex(std::string_view data);

}  // namespace azookey::learning
