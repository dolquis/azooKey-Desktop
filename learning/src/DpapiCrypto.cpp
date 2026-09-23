#include "azookey/learning/DpapiCrypto.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <system_error>

#include "azookey/learning/AtomicFile.h"
#include "azookey/learning/FileLock.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <dpapi.h>
#endif

namespace azookey::learning {
namespace {

class UserDpapiCrypto final : public ByteCrypto {
 public:
  bool IsAvailable() const override {
#ifdef _WIN32
    return true;
#else
    return false;
#endif
  }

  bool Encrypt(const std::vector<uint8_t>& plain, std::vector<uint8_t>& cipher) const override {
#ifdef _WIN32
    if (plain.size() > (std::numeric_limits<DWORD>::max)()) return false;
    DATA_BLOB input{static_cast<DWORD>(plain.size()), const_cast<BYTE*>(plain.data())};
    DATA_BLOB output{};
    if (!::CryptProtectData(&input, L"azooKey-learning", nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &output))
      return false;
    cipher.assign(output.pbData, output.pbData + output.cbData);
    ::LocalFree(output.pbData);
    return true;
#else
    (void)plain;
    (void)cipher;
    return false;
#endif
  }

  bool Decrypt(const std::vector<uint8_t>& cipher, std::vector<uint8_t>& plain) const override {
#ifdef _WIN32
    if (cipher.empty() || cipher.size() > (std::numeric_limits<DWORD>::max)()) return false;
    DATA_BLOB input{static_cast<DWORD>(cipher.size()), const_cast<BYTE*>(cipher.data())};
    DATA_BLOB output{};
    if (!::CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                              &output))
      return false;
    if (output.cbData)
      plain.assign(output.pbData, output.pbData + output.cbData);
    else
      plain.clear();
    if (output.cbData) ::SecureZeroMemory(output.pbData, output.cbData);
    ::LocalFree(output.pbData);
    return true;
#else
    (void)cipher;
    (void)plain;
    return false;
#endif
  }
};

bool ReadBytes(const std::filesystem::path& path, std::string& bytes) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return false;
  bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  return !input.bad();
}

bool Exists(const std::filesystem::path& path, bool& exists) {
  std::error_code ec;
  exists = std::filesystem::exists(path, ec);
  return !ec;
}

// Hold the source open against writers and renames through the final delete.
// The .enc mutex only coordinates new versions; an older process can still
// write the plaintext path without taking it.
class MigrationSource {
 public:
  explicit MigrationSource(const std::filesystem::path& path) : path_(path) {
#ifdef _WIN32
    handle_ = ::CreateFileW(path.wstring().c_str(), GENERIC_READ | DELETE, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
#endif
  }
  MigrationSource(const MigrationSource&) = delete;
  MigrationSource& operator=(const MigrationSource&) = delete;
  ~MigrationSource() {
#ifdef _WIN32
    if (handle_ != INVALID_HANDLE_VALUE) ::CloseHandle(handle_);
#endif
  }

  bool Read(std::string& bytes) const {
#ifdef _WIN32
    if (handle_ == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(handle_, &size) || size.QuadPart < 0 ||
        static_cast<unsigned long long>(size.QuadPart) > (std::numeric_limits<size_t>::max)())
      return false;
    LARGE_INTEGER start{};
    if (!::SetFilePointerEx(handle_, start, nullptr, FILE_BEGIN)) return false;
    bytes.resize(static_cast<size_t>(size.QuadPart));
    size_t offset = 0;
    while (offset < bytes.size()) {
      const DWORD chunk = static_cast<DWORD>((std::min)(
          bytes.size() - offset, static_cast<size_t>((std::numeric_limits<DWORD>::max)())));
      DWORD read = 0;
      if (!::ReadFile(handle_, bytes.data() + offset, chunk, &read, nullptr) || read == 0)
        return false;
      offset += read;
    }
    return true;
#else
    return ReadBytes(path_, bytes);
#endif
  }

  bool Remove() const {
#ifdef _WIN32
    if (handle_ == INVALID_HANDLE_VALUE) return false;
    FILE_DISPOSITION_INFO disposition{TRUE};
    return ::SetFileInformationByHandle(handle_, FileDispositionInfo, &disposition,
                                        sizeof(disposition)) != 0;
#else
    std::error_code ec;
    return std::filesystem::remove(path_, ec) && !ec;
#endif
  }

 private:
  std::filesystem::path path_;
#ifdef _WIN32
  HANDLE handle_{INVALID_HANDLE_VALUE};
#endif
};

bool EncryptAndWrite(const std::filesystem::path& encrypted_path, std::string_view text,
                     const ByteCrypto& crypto) {
  std::vector<uint8_t> plain(text.begin(), text.end());
  std::vector<uint8_t> cipher;
  const bool encrypted = crypto.Encrypt(plain, cipher);
  SecureErase(plain);
  if (!encrypted || cipher.empty()) return false;
  return WriteTextFileAtomically(
      encrypted_path, std::string(reinterpret_cast<const char*>(cipher.data()), cipher.size()));
}

bool DecryptFile(const std::filesystem::path& encrypted_path, const ByteCrypto& crypto,
                 std::string& text) {
  std::string bytes;
  if (!ReadBytes(encrypted_path, bytes)) return false;
  const std::vector<uint8_t> cipher(bytes.begin(), bytes.end());
  std::vector<uint8_t> plain;
  if (!crypto.Decrypt(cipher, plain)) {
    SecureErase(plain);
    return false;
  }
  text.assign(plain.begin(), plain.end());
  SecureErase(plain);
  return true;
}

constexpr std::string_view kSecretPrefix = "dpapi:";
constexpr std::string_view kBase64Alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string EncodeBase64(const std::vector<uint8_t>& bytes) {
  std::string encoded;
  encoded.reserve(((bytes.size() + 2) / 3) * 4);
  for (size_t i = 0; i < bytes.size(); i += 3) {
    const uint32_t block = (static_cast<uint32_t>(bytes[i]) << 16) |
                           (i + 1 < bytes.size() ? static_cast<uint32_t>(bytes[i + 1]) << 8 : 0) |
                           (i + 2 < bytes.size() ? static_cast<uint32_t>(bytes[i + 2]) : 0);
    encoded.push_back(kBase64Alphabet[(block >> 18) & 63]);
    encoded.push_back(kBase64Alphabet[(block >> 12) & 63]);
    encoded.push_back(i + 1 < bytes.size() ? kBase64Alphabet[(block >> 6) & 63] : '=');
    encoded.push_back(i + 2 < bytes.size() ? kBase64Alphabet[block & 63] : '=');
  }
  return encoded;
}

int Base64Digit(char ch) {
  const auto index = kBase64Alphabet.find(ch);
  return index == std::string_view::npos ? -1 : static_cast<int>(index);
}

bool DecodeBase64(std::string_view encoded, std::vector<uint8_t>& bytes) {
  bytes.clear();
  if (encoded.empty() || encoded.size() % 4 != 0) return false;
  for (size_t i = 0; i < encoded.size(); i += 4) {
    const int a = Base64Digit(encoded[i]);
    const int b = Base64Digit(encoded[i + 1]);
    const bool pad2 = encoded[i + 2] == '=';
    const bool pad3 = encoded[i + 3] == '=';
    const int c = pad2 ? 0 : Base64Digit(encoded[i + 2]);
    const int d = pad3 ? 0 : Base64Digit(encoded[i + 3]);
    if (a < 0 || b < 0 || c < 0 || d < 0 || (pad2 && !pad3) ||
        ((pad2 || pad3) && i + 4 != encoded.size()) || (pad2 && (b & 15) != 0) ||
        (pad3 && !pad2 && (c & 3) != 0))
      return false;
    const uint32_t block = (static_cast<uint32_t>(a) << 18) | (static_cast<uint32_t>(b) << 12) |
                           (static_cast<uint32_t>(c) << 6) | static_cast<uint32_t>(d);
    bytes.push_back(static_cast<uint8_t>(block >> 16));
    if (!pad2) bytes.push_back(static_cast<uint8_t>(block >> 8));
    if (!pad3) bytes.push_back(static_cast<uint8_t>(block));
  }
  return true;
}

}  // namespace

void SecureErase(std::vector<uint8_t>& bytes) noexcept {
  volatile uint8_t* data = bytes.data();
  for (size_t i = 0; i < bytes.size(); ++i) data[i] = 0;
  bytes.clear();
}

void SecureErase(std::string& text) noexcept {
  volatile char* data = text.data();
  for (size_t i = 0; i < text.size(); ++i) data[i] = 0;
  text.clear();
}

const ByteCrypto& DpapiCrypto() {
  static const UserDpapiCrypto crypto;
  return crypto;
}

std::filesystem::path EncryptedPathFor(const std::filesystem::path& plain_path) {
  auto path = plain_path;
  path += ".enc";
  return path;
}

ProtectedFileSource ReadProtectedText(const std::filesystem::path& plain_path,
                                      const ByteCrypto& crypto, std::string& text) {
  SecureErase(text);
  bool encrypted_exists = false;
  if (!Exists(EncryptedPathFor(plain_path), encrypted_exists)) return ProtectedFileSource::Error;
  if (encrypted_exists) {
    return DecryptFile(EncryptedPathFor(plain_path), crypto, text) ? ProtectedFileSource::Encrypted
                                                                   : ProtectedFileSource::Error;
  }
  bool plain_exists = false;
  if (!Exists(plain_path, plain_exists)) return ProtectedFileSource::Error;
  if (!plain_exists) {
    auto backup = plain_path;
    backup += ".bak";
    bool backup_exists = false;
    if (!Exists(backup, backup_exists)) return ProtectedFileSource::Error;
    return backup_exists ? ProtectedFileSource::Error : ProtectedFileSource::Missing;
  }
  if (!ReadBytes(plain_path, text)) {
    SecureErase(text);
    return ProtectedFileSource::Error;
  }
  return ProtectedFileSource::Plaintext;
}

bool MigratePlaintextFile(const std::filesystem::path& plain_path, std::string_view text,
                          const ByteCrypto& crypto) {
  const auto encrypted_path = EncryptedPathFor(plain_path);
  auto lock = AcquireExclusiveFileLockForPath(encrypted_path);
  if (!lock) return false;
  bool encrypted_exists = false;
  if (!Exists(encrypted_path, encrypted_exists) || encrypted_exists) return false;
  std::string current;
  if (!ReadBytes(plain_path, current) || current != text) return false;

  auto backup = plain_path;
  backup += ".bak";
  bool backup_exists = false;
  if (!Exists(backup, backup_exists)) return false;
  if (backup_exists) {
    std::string previous;
    if (!ReadBytes(backup, previous) || previous != current) return false;
  } else {
    std::error_code ec;
    if (!std::filesystem::copy_file(plain_path, backup, std::filesystem::copy_options::none, ec) ||
        ec || !FlushFileToDisk(backup))
      return false;
    std::string copied;
    if (!ReadBytes(backup, copied) || copied != current) return false;
  }
  MigrationSource source(plain_path);
  if (!source.Read(current) || current != text) return false;
  if (!EncryptAndWrite(encrypted_path, text, crypto)) return false;
  if (!source.Read(current) || current != text) return false;
  return source.Remove();
}

bool WriteProtectedText(const std::filesystem::path& plain_path, std::string_view text,
                        const ByteCrypto& crypto) {
  const auto encrypted_path = EncryptedPathFor(plain_path);
  auto lock = AcquireExclusiveFileLockForPath(encrypted_path);
  if (!lock) return false;
  bool plain_exists = false;
  bool encrypted_exists = false;
  auto backup = plain_path;
  backup += ".bak";
  bool backup_exists = false;
  if (!Exists(plain_path, plain_exists) || plain_exists ||
      !Exists(encrypted_path, encrypted_exists) || !Exists(backup, backup_exists) ||
      (backup_exists && !encrypted_exists))
    return false;
  if (encrypted_exists) {
    std::string previous;
    const bool can_decrypt = DecryptFile(encrypted_path, crypto, previous);
    SecureErase(previous);
    if (!can_decrypt) return false;
  }
  return EncryptAndWrite(encrypted_path, text, crypto);
}

SecretResult ProtectSecret(std::string_view plain, const ByteCrypto& crypto) {
  if (plain.empty()) return {SecretStatus::Ok, {}, false};
  if (!crypto.IsAvailable()) return {SecretStatus::CryptoUnavailable, {}, false};
  std::vector<uint8_t> input(plain.begin(), plain.end());
  std::vector<uint8_t> cipher;
  const bool encrypted = crypto.Encrypt(input, cipher);
  SecureErase(input);
  if (!encrypted || cipher.empty()) return {SecretStatus::CryptoFailure, {}, false};
  return {SecretStatus::Ok, std::string(kSecretPrefix) + EncodeBase64(cipher), true};
}

SecretResult UnprotectSecret(std::string_view stored, const ByteCrypto& crypto) {
  if (!stored.starts_with(kSecretPrefix)) return {SecretStatus::Ok, std::string(stored), false};
  stored.remove_prefix(kSecretPrefix.size());
  std::vector<uint8_t> cipher;
  if (!DecodeBase64(stored, cipher)) return {SecretStatus::InvalidEncoding, {}, true};
  if (!crypto.IsAvailable()) return {SecretStatus::CryptoUnavailable, {}, true};
  std::vector<uint8_t> plain;
  if (!crypto.Decrypt(cipher, plain)) {
    SecureErase(plain);
    return {SecretStatus::CryptoFailure, {}, true};
  }
  std::string value(plain.begin(), plain.end());
  SecureErase(plain);
  return {SecretStatus::Ok, std::move(value), true};
}

}  // namespace azookey::learning
