#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace azookey::learning {

// Byte-oriented boundary so persistence failures can be tested without a
// second Windows account. Production uses user-scoped Windows DPAPI.
class ByteCrypto {
 public:
  virtual ~ByteCrypto() = default;
  virtual bool Encrypt(const std::vector<uint8_t>& plain,
                       std::vector<uint8_t>& cipher) const = 0;
  virtual bool Decrypt(const std::vector<uint8_t>& cipher,
                       std::vector<uint8_t>& plain) const = 0;
  virtual bool IsAvailable() const { return true; }
};

const ByteCrypto& DpapiCrypto();
void SecureErase(std::vector<uint8_t>& bytes) noexcept;
void SecureErase(std::string& text) noexcept;

std::filesystem::path EncryptedPathFor(const std::filesystem::path& plain_path);

enum class ProtectedFileSource { Missing, Encrypted, Plaintext, Error };

// Encrypted data always takes precedence when both paths exist. Error never
// falls back to plaintext: doing so could replace another user's ciphertext.
ProtectedFileSource ReadProtectedText(const std::filesystem::path& plain_path,
                                      const ByteCrypto& crypto, std::string& text);

// Call only after the caller has accepted the plaintext format. Keeps the
// original bytes in .bak, and removes plaintext only after the encrypted
// atomic write succeeds. A stale or different .bak is never overwritten.
bool MigratePlaintextFile(const std::filesystem::path& plain_path, std::string_view text,
                          const ByteCrypto& crypto);

// Refuses to overwrite undecipherable ciphertext or unmigrated plaintext.
bool WriteProtectedText(const std::filesystem::path& plain_path, std::string_view text,
                        const ByteCrypto& crypto);

enum class SecretStatus { Ok, InvalidEncoding, CryptoUnavailable, CryptoFailure };

struct SecretResult {
  SecretStatus status{SecretStatus::CryptoFailure};
  std::string value;
  bool encrypted{false};
  SecretResult(SecretStatus result_status, std::string result_value, bool is_encrypted)
      : status(result_status), value(std::move(result_value)), encrypted(is_encrypted) {}
  SecretResult(const SecretResult&) = delete;
  SecretResult& operator=(const SecretResult&) = delete;
  SecretResult(SecretResult&& other) noexcept
      : status(other.status), value(std::move(other.value)), encrypted(other.encrypted) {
    SecureErase(other.value);
  }
  SecretResult& operator=(SecretResult&& other) noexcept {
    if (this != &other) {
      SecureErase(value);
      status = other.status;
      value = std::move(other.value);
      encrypted = other.encrypted;
      SecureErase(other.value);
    }
    return *this;
  }
  ~SecretResult() { SecureErase(value); }
  explicit operator bool() const { return status == SecretStatus::Ok; }
};

// A missing prefix denotes the legacy plaintext setting. Empty values remain
// empty. The caller must never persist or use the value when status is not Ok.
SecretResult ProtectSecret(std::string_view plain, const ByteCrypto& crypto = DpapiCrypto());
SecretResult UnprotectSecret(std::string_view stored, const ByteCrypto& crypto = DpapiCrypto());

}  // namespace azookey::learning
