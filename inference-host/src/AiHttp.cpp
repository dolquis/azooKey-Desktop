#include "azookey/host/AiBackend.h"
#include "azookey/logging/RuntimeLogger.h"

#ifdef _WIN32
// Windows crypto declarations require Windows base types first.
// clang-format off
#include "azookey/host/HttpSession.h"
#include <wincrypt.h>
// clang-format on

#include <array>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <vector>

#endif

namespace azookey::host {
#ifdef _WIN32
namespace {
void LogHttpFailure(const char* stage, DWORD error = GetLastError()) {
  static logging::RuntimeLogger logger(logging::RuntimeLoggerOptionsFromEnvironment("host"));
  logger.Log(logging::RuntimeLogLevel::Error, "ai_http_failure",
             {{"stage", logging::RuntimeLogSafeText(stage)},
              {"win32_error", static_cast<uint64_t>(error)}});
}
struct SecretHeaders {
  std::wstring value;
  ~SecretHeaders() { SecureZeroMemory(value.data(), value.size() * sizeof(wchar_t)); }
};
struct InternetCloser {
  void operator()(void* handle) const { WinHttpCloseHandle(handle); }
};
using Internet = std::unique_ptr<void, InternetCloser>;
std::wstring Wide(const std::string& text) {
  const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                       static_cast<int>(text.size()), nullptr, 0);
  if (size <= 0) return {};
  std::wstring result(static_cast<size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                      result.data(), size);
  return result;
}
std::string DecodeKey(const std::string& stored) {
  if (!stored.starts_with("dpapi:")) return stored;
  const auto encoded = stored.substr(6);
  DWORD size = 0;
  if (!CryptStringToBinaryA(encoded.c_str(), static_cast<DWORD>(encoded.size()),
                            CRYPT_STRING_BASE64, nullptr, &size, nullptr, nullptr)) {
    LogHttpFailure("key_base64_size");
    return {};
  }
  std::vector<BYTE> encrypted(size);
  if (!CryptStringToBinaryA(encoded.c_str(), static_cast<DWORD>(encoded.size()),
                            CRYPT_STRING_BASE64, encrypted.data(), &size, nullptr, nullptr)) {
    LogHttpFailure("key_base64_decode");
    return {};
  }
  DATA_BLOB input{size, encrypted.data()}, output{};
  if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                          &output)) {
    LogHttpFailure("key_dpapi_reenter_required");
    return {};
  }
  std::string key(reinterpret_cast<char*>(output.pbData), output.cbData);
  SecureZeroMemory(output.pbData, output.cbData);
  LocalFree(output.pbData);
  return key;
}

// Only the caller invokes WinHTTP APIs. Callbacks publish completion state;
// cancellation closes an idle async handle and waits for HANDLE_CLOSING before
// releasing callback state or request/response buffers.
struct Operation {
  std::mutex mutex;
  std::condition_variable changed;
  DWORD completion{0};
  DWORD bytes{0};
  DWORD error{0};
  bool closed{false};
  HINTERNET request{nullptr};
  ~Operation() {
    if (!request) return;
    WinHttpCloseHandle(request);
    std::unique_lock lock(mutex);
    changed.wait(lock, [this] { return closed; });
  }
  static void CALLBACK Callback(HINTERNET, DWORD_PTR context, DWORD status, void* data,
                                DWORD size) {
    if (!context) return;
    auto& op = *reinterpret_cast<Operation*>(context);
    const std::lock_guard lock(op.mutex);
    if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING)
      op.closed = true;
    else if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) {
      op.error = static_cast<WINHTTP_ASYNC_RESULT*>(data)->dwError;
    } else if (status == WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE ||
               status == WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE ||
               status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE) {
      op.completion = status;
      op.bytes = size;
    }
    op.changed.notify_all();
  }
  AiErrorClass Wait(DWORD expected, const std::atomic<bool>* cancel, AiDeadline deadline) {
    std::unique_lock lock(mutex);
    for (;;) {
      if (cancel && cancel->load()) return AiErrorClass::Canceled;
      if (std::chrono::steady_clock::now() >= deadline) return AiErrorClass::Timeout;
      if (error)
        return error == ERROR_WINHTTP_TIMEOUT ? AiErrorClass::Timeout : AiErrorClass::Network;
      if (completion == expected) {
        completion = 0;
        return AiErrorClass::None;
      }
      changed.wait_for(lock, std::chrono::milliseconds(10));
    }
  }
};
}  // namespace
#endif

AiHttpResponse PostAiHttp(const AiBackendOptions& options, const std::string& body,
                          const std::atomic<bool>* cancel, AiDeadline deadline) {
  AiHttpResponse response;
  response.error = AiErrorClass::Network;
#ifdef _WIN32
  if (cancel && cancel->load()) {
    response.error = AiErrorClass::Canceled;
    return response;
  }
  auto key = DecodeKey(options.api_key);
  if (key.empty() || key.find_first_of("\r\n\0", 0, 3) != std::string::npos) {
    response.error = AiErrorClass::Auth;
    return response;
  }
  auto endpoint = options.endpoint;
  while (!endpoint.empty() && endpoint.back() == '/') endpoint.pop_back();
  const auto url = Wide(endpoint + "/chat/completions");
  URL_COMPONENTS parts{};
  parts.dwStructSize = sizeof(parts);
  parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength =
      parts.dwUserNameLength = parts.dwPasswordLength = static_cast<DWORD>(-1);
  if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts) ||
      parts.dwUserNameLength || parts.dwPasswordLength || parts.dwExtraInfoLength) {
    response.error = AiErrorClass::Parse;
    return response;
  }
  const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
  const std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
  const bool loopback = host == L"127.0.0.1" || host == L"[::1]" || host == L"::1";
  if (parts.nScheme != INTERNET_SCHEME_HTTPS &&
      !(loopback && parts.nScheme == INTERNET_SCHEME_HTTP)) {
    response.error = AiErrorClass::Parse;
    return response;
  }
  Internet session(
      OpenHttpSession(L"azooKey-AI/1.0", loopback, true, 10000, 10000, options.timeout_ms));
  if (!session) {
    LogHttpFailure("session");
    return response;
  }
  Internet connection(WinHttpConnect(session.get(), host.c_str(), parts.nPort, 0));
  if (!connection) {
    LogHttpFailure("connect");
    return response;
  }
  std::array<char, 8192> buffer{};  // Must outlive Operation's close notification.
  SecretHeaders headers;
  Operation operation;
  Internet pending(
      WinHttpOpenRequest(connection.get(), L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                         WINHTTP_DEFAULT_ACCEPT_TYPES,
                         parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0));
  if (!pending) {
    LogHttpFailure("open_request");
    return response;
  }
  DWORD disable = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES;
  DWORD autologon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
  if (!WinHttpSetOption(pending.get(), WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable)) ||
      !WinHttpSetOption(pending.get(), WINHTTP_OPTION_AUTOLOGON_POLICY, &autologon,
                        sizeof(autologon))) {
    LogHttpFailure("security_options");
    return response;
  }
  const DWORD_PTR context = reinterpret_cast<DWORD_PTR>(&operation);
  if (!WinHttpSetOption(pending.get(), WINHTTP_OPTION_CONTEXT_VALUE,
                        const_cast<DWORD_PTR*>(&context), sizeof(context))) {
    LogHttpFailure("callback_context");
    return response;
  }
  if (WinHttpSetStatusCallback(
          pending.get(), Operation::Callback,
          WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES,
          0) == WINHTTP_INVALID_STATUS_CALLBACK) {
    LogHttpFailure("callback_registration");
    return response;
  }
  operation.request = pending.release();
  headers.value = L"Content-Type: application/json\r\nAuthorization: Bearer " + Wide(key) + L"\r\n";
  SecureZeroMemory(key.data(), key.size());
  const bool sent =
      WinHttpSendRequest(operation.request, headers.value.c_str(),
                         static_cast<DWORD>(headers.value.size()), const_cast<char*>(body.data()),
                         static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), context);
  if (!sent) {
    LogHttpFailure("send");
    return response;
  }
  response.error = operation.Wait(WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE, cancel, deadline);
  if (response.error != AiErrorClass::None) return response;
  if (!WinHttpReceiveResponse(operation.request, nullptr)) {
    LogHttpFailure("receive");
    response.error = AiErrorClass::Network;
    return response;
  }
  response.error = operation.Wait(WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE, cancel, deadline);
  if (response.error != AiErrorClass::None) return response;
  DWORD status = 0, status_size = sizeof(status);
  if (!WinHttpQueryHeaders(operation.request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                           WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                           WINHTTP_NO_HEADER_INDEX)) {
    LogHttpFailure("status_headers");
    response.error = AiErrorClass::Network;
    return response;
  }
  response.status = status;
  response.error = AiErrorClass::None;
  DWORD retry = 0, retry_size = sizeof(retry);
  if (WinHttpQueryHeaders(operation.request, WINHTTP_QUERY_RETRY_AFTER | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &retry, &retry_size,
                          WINHTTP_NO_HEADER_INDEX))
    response.retry_after_seconds = retry;
  if (status != 200) {
    // Provider bodies can echo input or credentials; only record bounded metadata.
    static logging::RuntimeLogger logger(logging::RuntimeLoggerOptionsFromEnvironment("host"));
    logger.Log(logging::RuntimeLogLevel::Error, "ai_http_status",
               {{"status", static_cast<uint64_t>(status)}});
    return response;
  }
  for (;;) {
    if (!WinHttpReadData(operation.request, buffer.data(), static_cast<DWORD>(buffer.size()),
                         nullptr)) {
      LogHttpFailure("read_body");
      response.error = AiErrorClass::Network;
      return response;
    }
    response.error = operation.Wait(WINHTTP_CALLBACK_STATUS_READ_COMPLETE, cancel, deadline);
    if (response.error != AiErrorClass::None) return response;
    if (operation.bytes == 0) break;
    if (response.body.size() + operation.bytes > 262144) {
      response.error = AiErrorClass::Parse;
      return response;
    }
    response.body.append(buffer.data(), operation.bytes);
  }
#else
  (void)options;
  (void)body;
  (void)cancel;
  (void)deadline;
#endif
  return response;
}
}  // namespace azookey::host
