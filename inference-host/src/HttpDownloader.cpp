#include "azookey/host/HttpDownloader.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cwctype>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
// Windows crypto and WinHTTP headers require the base Windows declarations first.
// clang-format off
#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>
// clang-format on
#endif

namespace azookey::host {

static_assert(sizeof(HttpDownloadStatus) == sizeof(uint8_t));

namespace {

HttpDownloadResult Failure(std::string message) {
  HttpDownloadResult result;
  result.error = std::move(message);
  return result;
}

#ifdef _WIN32

constexpr DWORD kHttpStatusRangeNotSatisfiable = 416;

std::string WindowsError(std::string_view operation, DWORD error = GetLastError()) {
  return std::string(operation) + " failed (Windows error " + std::to_string(error) + ")";
}

template <typename T, auto Close>
class UniqueHandle {
 public:
  UniqueHandle() = default;
  explicit UniqueHandle(T value) : value_(value) {}
  ~UniqueHandle() { reset(); }

  UniqueHandle(const UniqueHandle&) = delete;
  UniqueHandle& operator=(const UniqueHandle&) = delete;
  UniqueHandle(UniqueHandle&& other) noexcept : value_(other.release()) {}
  UniqueHandle& operator=(UniqueHandle&& other) noexcept {
    if (this != &other) reset(other.release());
    return *this;
  }

  explicit operator bool() const { return value_ != nullptr && value_ != INVALID_HANDLE_VALUE; }
  T get() const { return value_; }
  T release() {
    T value = value_;
    value_ = nullptr;
    return value;
  }
  void reset(T value = nullptr) {
    if (*this) Close(value_);
    value_ = value;
  }

 private:
  T value_{nullptr};
};

using UniqueInternetHandle = UniqueHandle<HINTERNET, WinHttpCloseHandle>;

void CloseKernelHandle(HANDLE handle) { CloseHandle(handle); }
using UniqueFileHandle = UniqueHandle<HANDLE, CloseKernelHandle>;

void CloseAlgorithm(BCRYPT_ALG_HANDLE handle) { BCryptCloseAlgorithmProvider(handle, 0); }
using UniqueAlgorithmHandle = UniqueHandle<BCRYPT_ALG_HANDLE, CloseAlgorithm>;

void CloseHash(BCRYPT_HASH_HANDLE handle) { BCryptDestroyHash(handle); }
using UniqueHashHandle = UniqueHandle<BCRYPT_HASH_HANDLE, CloseHash>;

bool IsValidSha256(std::string_view value) {
  return value.size() == 64 && std::all_of(value.begin(), value.end(),
                                           [](unsigned char ch) { return std::isxdigit(ch); });
}

std::string LowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return value;
}

std::optional<std::string> Sha256File(const std::filesystem::path& path, std::string* error) {
  UniqueFileHandle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!file) {
    *error = WindowsError("opening file for SHA256");
    return std::nullopt;
  }

  BCRYPT_ALG_HANDLE algorithm_raw = nullptr;
  NTSTATUS status =
      BCryptOpenAlgorithmProvider(&algorithm_raw, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
  if (status < 0) {
    *error = "opening SHA256 provider failed (NTSTATUS " + std::to_string(status) + ")";
    return std::nullopt;
  }
  UniqueAlgorithmHandle algorithm(algorithm_raw);

  DWORD object_size = 0;
  DWORD copied = 0;
  status =
      BCryptGetProperty(algorithm.get(), BCRYPT_OBJECT_LENGTH,
                        reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &copied, 0);
  if (status < 0) {
    *error = "querying SHA256 object length failed (NTSTATUS " + std::to_string(status) + ")";
    return std::nullopt;
  }
  std::vector<UCHAR> object(object_size);
  BCRYPT_HASH_HANDLE hash_raw = nullptr;
  status = BCryptCreateHash(algorithm.get(), &hash_raw, object.data(), object_size, nullptr, 0, 0);
  if (status < 0) {
    *error = "creating SHA256 hash failed (NTSTATUS " + std::to_string(status) + ")";
    return std::nullopt;
  }
  UniqueHashHandle hash(hash_raw);

  std::array<UCHAR, 65'536> buffer{};
  for (;;) {
    DWORD read = 0;
    if (!ReadFile(file.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
      *error = WindowsError("reading file for SHA256");
      return std::nullopt;
    }
    if (read == 0) break;
    status = BCryptHashData(hash.get(), buffer.data(), read, 0);
    if (status < 0) {
      *error = "updating SHA256 hash failed (NTSTATUS " + std::to_string(status) + ")";
      return std::nullopt;
    }
  }

  std::array<UCHAR, 32> digest{};
  status = BCryptFinishHash(hash.get(), digest.data(), static_cast<ULONG>(digest.size()), 0);
  if (status < 0) {
    *error = "finishing SHA256 hash failed (NTSTATUS " + std::to_string(status) + ")";
    return std::nullopt;
  }

  constexpr std::string_view kHex = "0123456789abcdef";
  std::string encoded;
  encoded.reserve(digest.size() * std::size_t{2});
  for (const auto byte : digest) {
    encoded.push_back(kHex.at(byte >> 4));
    encoded.push_back(kHex.at(byte & 0x0f));
  }
  return encoded;
}

bool FileMatches(const std::filesystem::path& path, std::string_view expected, std::string* error) {
  auto actual = Sha256File(path, error);
  return actual && *actual == expected;
}

std::optional<uint64_t> RegularFileSize(const std::filesystem::path& path, std::string* error) {
  std::error_code ec;
  const bool exists = std::filesystem::exists(path, ec);
  if (!exists && (!ec || ec == std::errc::no_such_file_or_directory)) return uint64_t{0};
  if (ec) {
    *error = "probing file failed: " + ec.message();
    return std::nullopt;
  }
  if (!std::filesystem::is_regular_file(path, ec) || ec) {
    *error = "download path is not a regular file";
    return std::nullopt;
  }
  const auto size = std::filesystem::file_size(path, ec);
  if (ec) {
    *error = "reading download file size failed: " + ec.message();
    return std::nullopt;
  }
  return static_cast<uint64_t>(size);
}

struct ParsedUrl {
  std::wstring host;
  std::wstring path;
  INTERNET_PORT port{};
  bool secure{false};
};

std::optional<ParsedUrl> ParseUrl(const std::wstring& url, std::string* error) {
  URL_COMPONENTS components{};
  components.dwStructSize = sizeof(components);
  components.dwHostNameLength = static_cast<DWORD>(-1);
  components.dwUrlPathLength = static_cast<DWORD>(-1);
  components.dwExtraInfoLength = static_cast<DWORD>(-1);
  if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &components)) {
    *error = WindowsError("parsing download URL");
    return std::nullopt;
  }
  if (components.nScheme != INTERNET_SCHEME_HTTP && components.nScheme != INTERNET_SCHEME_HTTPS) {
    *error = "download URL must use HTTP or HTTPS";
    return std::nullopt;
  }
  if (components.dwHostNameLength == 0) {
    *error = "download URL has no host";
    return std::nullopt;
  }

  ParsedUrl parsed;
  parsed.host.assign(components.lpszHostName, components.dwHostNameLength);
  std::wstring normalized_host = parsed.host;
  std::transform(normalized_host.begin(), normalized_host.end(), normalized_host.begin(),
                 [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
  const bool loopback = normalized_host == L"localhost" || normalized_host == L"127.0.0.1" ||
                        normalized_host == L"::1" || normalized_host == L"[::1]";
  if (components.nScheme == INTERNET_SCHEME_HTTP && !loopback) {
    *error = "cleartext HTTP downloads are only allowed for loopback tests";
    return std::nullopt;
  }
  if (components.dwUrlPathLength > 0) {
    parsed.path.assign(components.lpszUrlPath, components.dwUrlPathLength);
  } else {
    parsed.path = L"/";
  }
  if (components.dwExtraInfoLength > 0) {
    parsed.path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
  }
  parsed.port = components.nPort;
  parsed.secure = components.nScheme == INTERNET_SCHEME_HTTPS;
  return parsed;
}

std::optional<DWORD> QueryStatusCode(HINTERNET request, std::string* error) {
  DWORD status = 0;
  DWORD size = sizeof(status);
  if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                           WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)) {
    *error = WindowsError("reading HTTP status");
    return std::nullopt;
  }
  return status;
}

std::optional<std::wstring> QueryHeader(HINTERNET request, DWORD query) {
  DWORD size = 0;
  if (WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER,
                          &size, WINHTTP_NO_HEADER_INDEX) ||
      GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
    return std::nullopt;
  }
  std::wstring value(size / sizeof(wchar_t), L'\0');
  if (!WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX, value.data(), &size,
                           WINHTTP_NO_HEADER_INDEX)) {
    return std::nullopt;
  }
  value.resize(size / sizeof(wchar_t));
  while (!value.empty() && value.back() == L'\0') value.pop_back();
  return value;
}

bool ContentRangeStartsAt(std::wstring_view value, uint64_t expected) {
  constexpr std::wstring_view kPrefix = L"bytes ";
  if (!value.starts_with(kPrefix)) return false;
  value.remove_prefix(kPrefix.size());
  const auto dash = value.find(L'-');
  if (dash == std::wstring_view::npos || dash == 0) return false;
  uint64_t start = 0;
  for (const auto ch : value.substr(0, dash)) {
    if (ch < L'0' || ch > L'9') return false;
    const auto digit = static_cast<uint64_t>(ch - L'0');
    if (start > (std::numeric_limits<uint64_t>::max() - digit) / 10) return false;
    start = start * 10 + digit;
  }
  return start == expected;
}

UniqueFileHandle OpenPartFile(const std::filesystem::path& path, bool append, std::string* error) {
  UniqueFileHandle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                    append ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                    nullptr));
  if (!file) {
    *error = WindowsError("opening partial download");
    return {};
  }
  if (append) {
    LARGE_INTEGER end{};
    end.QuadPart = 0;
    if (!SetFilePointerEx(file.get(), end, nullptr, FILE_END)) {
      *error = WindowsError("seeking partial download");
      return {};
    }
  }
  return file;
}

bool ReadResponseToFile(HINTERNET request, HANDLE file, uint64_t* bytes_received,
                        std::string* error) {
  std::array<unsigned char, 65'536> buffer{};
  for (;;) {
    DWORD available = 0;
    if (!WinHttpQueryDataAvailable(request, &available)) {
      *error = WindowsError("querying HTTP response data");
      return false;
    }
    if (available == 0) break;
    while (available > 0) {
      const DWORD requested = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
      DWORD read = 0;
      if (!WinHttpReadData(request, buffer.data(), requested, &read)) {
        *error = WindowsError("reading HTTP response");
        return false;
      }
      if (read == 0) {
        *error = "HTTP response ended before the advertised data was read";
        return false;
      }
      DWORD written = 0;
      if (!WriteFile(file, buffer.data(), read, &written, nullptr) || written != read) {
        *error = WindowsError("writing partial download");
        return false;
      }
      *bytes_received += read;
      available -= read;
    }
  }
  if (!FlushFileBuffers(file)) {
    *error = WindowsError("flushing partial download");
    return false;
  }
  return true;
}

bool PromotePartFile(const std::filesystem::path& part, const std::filesystem::path& destination,
                     std::string* error) {
  if (!MoveFileExW(part.c_str(), destination.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    *error = WindowsError("promoting verified download");
    return false;
  }
  return true;
}

#endif

}  // namespace

HttpDownloader::HttpDownloader(std::wstring user_agent) : user_agent_(std::move(user_agent)) {}

HttpDownloadResult HttpDownloader::Download(const HttpDownloadRequest& request) const {
#ifndef _WIN32
  (void)request;
  return Failure("HTTP downloads are only supported on Windows");
#else
  if (request.url.empty() || request.destination.empty() ||
      !IsValidSha256(request.expected_sha256)) {
    return Failure("download request requires an HTTP URL, destination, and 64-character SHA256");
  }
  const auto expected = LowerAscii(request.expected_sha256);

  std::error_code ec;
  if (std::filesystem::is_regular_file(request.destination, ec) && !ec) {
    std::string hash_error;
    if (FileMatches(request.destination, expected, &hash_error)) {
      HttpDownloadResult result;
      result.status = HttpDownloadStatus::AlreadyValid;
      return result;
    }
  }

  const auto parent = request.destination.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent, ec);
    if (ec) return Failure("creating download directory failed: " + ec.message());
  }
  auto part = request.destination;
  part += L".part";

  std::string error;
  auto parsed = ParseUrl(request.url, &error);
  if (!parsed) return Failure(std::move(error));

  UniqueInternetHandle session(WinHttpOpen(user_agent_.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
  if (!session) return Failure(WindowsError("opening WinHTTP session"));
  if (!WinHttpSetTimeouts(session.get(), static_cast<int>(request.connect_timeout_ms),
                          static_cast<int>(request.connect_timeout_ms),
                          static_cast<int>(request.send_timeout_ms),
                          static_cast<int>(request.receive_timeout_ms))) {
    return Failure(WindowsError("setting WinHTTP timeouts"));
  }

  UniqueInternetHandle connection(
      WinHttpConnect(session.get(), parsed->host.c_str(), parsed->port, 0));
  if (!connection) return Failure(WindowsError("opening WinHTTP connection"));

  for (int attempt = 0; attempt < 2; ++attempt) {
    auto part_size = RegularFileSize(part, &error);
    if (!part_size) return Failure(std::move(error));
    const uint64_t resume_offset = attempt == 0 ? *part_size : 0;

    UniqueInternetHandle http_request(WinHttpOpenRequest(
        connection.get(), L"GET", parsed->path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, parsed->secure ? WINHTTP_FLAG_SECURE : 0));
    if (!http_request) return Failure(WindowsError("opening HTTP request"));

    if (resume_offset > 0) {
      const auto range = L"Range: bytes=" + std::to_wstring(resume_offset) + L"-";
      if (!WinHttpAddRequestHeaders(http_request.get(), range.c_str(),
                                    static_cast<DWORD>(range.size()),
                                    WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE)) {
        return Failure(WindowsError("adding HTTP Range header"));
      }
    }
    if (!WinHttpSendRequest(http_request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(http_request.get(), nullptr)) {
      return Failure(WindowsError("downloading file"));
    }

    auto status = QueryStatusCode(http_request.get(), &error);
    if (!status) return Failure(std::move(error));

    if (*status == kHttpStatusRangeNotSatisfiable && resume_offset > 0) {
      std::string hash_error;
      if (FileMatches(part, expected, &hash_error)) {
        if (!PromotePartFile(part, request.destination, &error)) return Failure(std::move(error));
        HttpDownloadResult result;
        result.status = HttpDownloadStatus::Downloaded;
        result.resumed = true;
        return result;
      }
      if (attempt == 0) continue;
    }

    const bool partial = *status == HTTP_STATUS_PARTIAL_CONTENT;
    if (*status != HTTP_STATUS_OK && !partial) {
      return Failure("HTTP download failed with status " + std::to_string(*status));
    }
    if (partial) {
      const auto content_range = QueryHeader(http_request.get(), WINHTTP_QUERY_CONTENT_RANGE);
      if (resume_offset == 0 || !content_range ||
          !ContentRangeStartsAt(*content_range, resume_offset)) {
        return Failure("HTTP partial response has an invalid Content-Range");
      }
    }

    HttpDownloadResult result;
    result.resumed = partial;
    auto file = OpenPartFile(part, partial, &error);
    if (!file) return Failure(std::move(error));
    if (!ReadResponseToFile(http_request.get(), file.get(), &result.bytes_received, &error)) {
      return Failure(std::move(error));
    }
    file.reset();

    std::string hash_error;
    if (!FileMatches(part, expected, &hash_error)) {
      return Failure(hash_error.empty() ? "downloaded file SHA256 does not match expected value"
                                        : std::move(hash_error));
    }
    if (!PromotePartFile(part, request.destination, &error)) return Failure(std::move(error));
    result.status = HttpDownloadStatus::Downloaded;
    return result;
  }

  return Failure("HTTP resume could not be restarted");
#endif
}

}  // namespace azookey::host
