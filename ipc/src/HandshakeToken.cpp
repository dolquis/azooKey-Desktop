#include "azookey/ipc/HandshakeToken.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <random>
#include <system_error>
#include <vector>

#include "azookey/core/PlatformPaths.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <bcrypt.h>
#include <sddl.h>
#endif

namespace azookey::ipc {
namespace {

bool IsToken(std::string_view value) {
  if (value.size() != 32) return false;
  for (const char c : value) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
      return false;
    }
  }
  return true;
}

std::optional<std::string> EnvironmentToken() {
#ifdef _WIN32
  char* value = nullptr;
  size_t length = 0;
  if (_dupenv_s(&value, &length, "AZOOKEY_IPC_HANDSHAKE_TOKEN") != 0 || !value) {
    return std::nullopt;
  }
  std::string token(value);
  std::free(value);
  return token.empty() ? std::nullopt : std::optional<std::string>(std::move(token));
#else
  const char* value = std::getenv("AZOOKEY_IPC_HANDSHAKE_TOKEN");
  return value && *value ? std::optional<std::string>(value) : std::nullopt;
#endif
}

#ifdef _WIN32
bool WritePrivateFile(const std::filesystem::path& path, std::string_view content) {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
  DWORD required = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &required);
  std::vector<unsigned char> token_info(required);
  const bool got_user = required > 0 &&
                        GetTokenInformation(token, TokenUser, token_info.data(), required, &required);
  CloseHandle(token);
  if (!got_user) return false;

  wchar_t* sid = nullptr;
  if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(token_info.data())->User.Sid, &sid)) {
    return false;
  }
  // Protect the DACL so a permissive parent cannot expose the temporary file
  // before its atomic rename. SYSTEM and administrators retain recovery access.
  const std::wstring sddl =
      std::wstring(L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;") + sid + L")";
  LocalFree(sid);
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                              &descriptor, nullptr)) {
    return false;
  }
  SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, &attributes, CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  LocalFree(descriptor);
  if (file == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const bool ok = WriteFile(file, content.data(), static_cast<DWORD>(content.size()), &written,
                            nullptr) &&
                  written == content.size() && FlushFileBuffers(file);
  CloseHandle(file);
  if (!ok) {
    DeleteFileW(path.c_str());
    return false;
  }
  return true;
}
#endif

}  // namespace

std::optional<std::filesystem::path> DefaultHandshakeTokenPath() {
  const auto local_app_data = core::GetLocalAppDataDirectory();
  if (!local_app_data) return std::nullopt;
  return *local_app_data / "azooKey" / "config" / "ipc-token";
}

std::optional<std::string> ReadHandshakeTokenFile(const std::filesystem::path& path) {
  std::array<char, 33> buffer{};
#ifdef _WIN32
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return std::nullopt;
  DWORD read = 0;
  const bool ok = ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr);
  CloseHandle(file);
  if (!ok || read != 32) return std::nullopt;
#else
  std::ifstream input(path, std::ios::binary);
  if (!input) return std::nullopt;
  input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
  if (input.gcount() != 32 || !input.eof()) return std::nullopt;
#endif
  std::string token(buffer.data(), 32);
  return IsToken(token) ? std::optional<std::string>(std::move(token)) : std::nullopt;
}

std::optional<std::string> ReadClientHandshakeToken() {
  if (auto override_token = EnvironmentToken()) return override_token;
  const auto path = DefaultHandshakeTokenPath();
  return path ? ReadHandshakeTokenFile(*path) : std::nullopt;
}

std::optional<std::string> GenerateHandshakeToken() {
  std::array<unsigned char, 16> bytes{};
#ifdef _WIN32
  if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                                       BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
    return std::nullopt;
  }
#else
  std::random_device random;
  for (auto& byte : bytes) byte = static_cast<unsigned char>(random());
#endif
  constexpr char hex[] = "0123456789abcdef";
  std::string token(32, '0');
  for (size_t i = 0; i < bytes.size(); ++i) {
    token[i * 2] = hex[bytes[i] >> 4];
    token[i * 2 + 1] = hex[bytes[i] & 0x0f];
  }
  return token;
}

bool PublishHandshakeToken(const std::filesystem::path& path, std::string_view token) {
  if (!IsToken(token) || path.empty()) return false;
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec) return false;

  static std::atomic<uint64_t> sequence{0};
  auto temp = path;
  temp += ".tmp." + std::to_string(
#ifdef _WIN32
      GetCurrentProcessId()
#else
      std::chrono::steady_clock::now().time_since_epoch().count()
#endif
  ) + "." + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
#ifdef _WIN32
  if (!WritePrivateFile(temp, token)) return false;
  const bool renamed = MoveFileExW(temp.c_str(), path.c_str(),
                                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
  if (!renamed) DeleteFileW(temp.c_str());
  return renamed;
#else
  {
    std::ofstream output(temp, std::ios::binary | std::ios::trunc);
    output.write(token.data(), static_cast<std::streamsize>(token.size()));
    if (!output) {
      std::filesystem::remove(temp, ec);
      return false;
    }
  }
  std::filesystem::rename(temp, path, ec);
  if (ec) std::filesystem::remove(temp, ec);
  return !ec;
#endif
}

}  // namespace azookey::ipc
