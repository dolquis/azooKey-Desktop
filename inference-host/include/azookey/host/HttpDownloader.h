#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace azookey::host {

enum class HttpDownloadStatus : uint8_t {
  Downloaded,
  AlreadyValid,
  Failed,
};

struct HttpDownloadRequest {
  std::wstring url;
  std::filesystem::path destination;
  std::string expected_sha256;
  uint64_t max_bytes{0};
  uint32_t connect_timeout_ms{15'000};
  uint32_t send_timeout_ms{15'000};
  uint32_t receive_timeout_ms{30'000};
  // Cooperative cancellation between blocking operations; in-flight WinHTTP calls
  // still use the timeouts above. The callback runs on the calling thread.
  std::function<bool()> cancelled;
};

struct HttpTextRequest {
  std::wstring url;
  uint64_t max_bytes{0};
  uint32_t connect_timeout_ms{15'000};
  uint32_t send_timeout_ms{15'000};
  uint32_t receive_timeout_ms{30'000};
  std::function<bool()> cancelled;
};

struct HttpTextResult {
  std::string body;
  std::optional<std::string> error;

  bool ok() const { return !error.has_value(); }
};

struct HttpDownloadResult {
  HttpDownloadStatus status{HttpDownloadStatus::Failed};
  bool resumed{false};
  uint64_t bytes_received{0};
  std::optional<std::string> error;

  bool ok() const { return status != HttpDownloadStatus::Failed; }
};

// Lowercase hex SHA-256 of a file (Windows CNG). nullopt with *error set when
// the file cannot be read, and always on platforms without CNG.
std::optional<std::string> ComputeFileSha256(const std::filesystem::path& path, std::string* error);

// Lowercase hex SHA-256 of exactly these bytes, including embedded NULs (Windows CNG).
// Empty input is valid. Returns nullopt on failure or on platforms without CNG.
// error may be null; otherwise failures set *error.
std::optional<std::string> ComputeSha256(std::string_view bytes, std::string* error);

class HttpDownloader {
 public:
  explicit HttpDownloader(std::wstring user_agent = L"azooKey-Desktop/1.0");

  // Writes only to <destination>.part until SHA256 verification succeeds.
  // A failed request never replaces an existing destination file.
  HttpDownloadResult Download(const HttpDownloadRequest& request) const;

  // Bounded GET for small metadata such as checksum sidecars. No file is written
  // and no SHA256 verification is performed. Cancellation is cooperative, as above.
  // Failed requests return an empty body.
  HttpTextResult FetchText(const HttpTextRequest& request) const;

 private:
  std::wstring user_agent_;
};

}  // namespace azookey::host
