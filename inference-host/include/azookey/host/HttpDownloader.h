#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

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
};

struct HttpDownloadResult {
  HttpDownloadStatus status{HttpDownloadStatus::Failed};
  bool resumed{false};
  uint64_t bytes_received{0};
  std::optional<std::string> error;

  bool ok() const { return status != HttpDownloadStatus::Failed; }
};

class HttpDownloader {
 public:
  explicit HttpDownloader(std::wstring user_agent = L"azooKey-Desktop/1.0");

  // Writes only to <destination>.part until SHA256 verification succeeds.
  // A failed request never replaces an existing destination file.
  HttpDownloadResult Download(const HttpDownloadRequest& request) const;

 private:
  std::wstring user_agent_;
};

}  // namespace azookey::host
