#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "azookey/learning/DpapiCrypto.h"

namespace azookey::learning::test {

// Deterministic test-only cipher. Never use this for persisted user data.
class TestByteCrypto final : public ByteCrypto {
 public:
  bool Encrypt(const std::vector<uint8_t>& plain, std::vector<uint8_t>& cipher) const override {
    cipher = {0xA5};
    for (uint8_t byte : plain) cipher.push_back(byte ^ 0x5A);
    return true;
  }

  bool Decrypt(const std::vector<uint8_t>& cipher, std::vector<uint8_t>& plain) const override {
    if (cipher.empty() || cipher.front() != 0xA5) return false;
    plain.clear();
    for (size_t i = 1; i < cipher.size(); ++i) plain.push_back(cipher[i] ^ 0x5A);
    return true;
  }
};

inline const ByteCrypto& Crypto() {
#ifdef _WIN32
  return DpapiCrypto();
#else
  static const TestByteCrypto crypto;
  return crypto;
#endif
}

}  // namespace azookey::learning::test
