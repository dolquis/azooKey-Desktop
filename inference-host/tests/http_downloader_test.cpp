#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <gtest/gtest.h>
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include "azookey/host/HttpDownloader.h"

namespace {

constexpr char kFixtureBytes[] = {'G', 'G', 'U', 'F', '\x03', '\0', '\0', '\0', 'f', 'i', 'x',
                                  't', 'u', 'r', 'e', '-',    'm',  'o',  'd',  'e', 'l'};
constexpr std::string_view kFixture(kFixtureBytes, sizeof(kFixtureBytes));
constexpr std::string_view kFixtureSha256 =
    "cce8fa5c83c8d9494aec0ed9c4ec59739e15c6352216e2f4377b3d8194e48082";

class WinsockScope {
 public:
  WinsockScope() {
    WSADATA data{};
    ok_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }
  ~WinsockScope() {
    if (ok_) WSACleanup();
  }
  bool ok() const { return ok_; }

 private:
  bool ok_{false};
};

class TempDirectory {
 public:
  TempDirectory() {
    static std::atomic<uint64_t> next_id{0};
    path_ = std::filesystem::temp_directory_path() /
            ("azookey-http-downloader-" + std::to_string(GetCurrentProcessId()) + "-" +
             std::to_string(next_id.fetch_add(1)));
    std::filesystem::create_directories(path_);
  }
  ~TempDirectory() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

bool SendAll(SOCKET socket, std::string_view data) {
  size_t sent = 0;
  while (sent < data.size()) {
    const int chunk = send(socket, data.data() + sent, static_cast<int>(data.size() - sent), 0);
    if (chunk <= 0) return false;
    sent += static_cast<size_t>(chunk);
  }
  return true;
}

class LocalHttpServer {
 public:
  LocalHttpServer(std::string body, bool honor_range)
      : body_(std::move(body)), honor_range_(honor_range) {
    socket_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_ == INVALID_SOCKET) return;

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(socket_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) ==
            SOCKET_ERROR ||
        listen(socket_, 1) == SOCKET_ERROR) {
      closesocket(socket_);
      socket_ = INVALID_SOCKET;
      return;
    }

    int length = sizeof(address);
    if (getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &length) == SOCKET_ERROR) {
      closesocket(socket_);
      socket_ = INVALID_SOCKET;
      return;
    }
    port_ = ntohs(address.sin_port);
    worker_ = std::thread([this] { ServeOne(); });
  }

  ~LocalHttpServer() {
    if (socket_ != INVALID_SOCKET) {
      shutdown(socket_, SD_BOTH);
      closesocket(socket_);
      socket_ = INVALID_SOCKET;
    }
    Wait();
  }

  bool ok() const { return socket_ != INVALID_SOCKET; }
  std::wstring url() const {
    return L"http://127.0.0.1:" + std::to_wstring(port_) + L"/model.gguf";
  }
  void Wait() {
    if (worker_.joinable()) worker_.join();
  }
  std::string request() const {
    std::lock_guard lock(mutex_);
    return request_;
  }

 private:
  void ServeOne() {
    const SOCKET client = accept(socket_, nullptr, nullptr);
    if (client == INVALID_SOCKET) return;

    std::string request;
    char buffer[1024];
    while (request.find("\r\n\r\n") == std::string::npos) {
      const int read = recv(client, buffer, sizeof(buffer), 0);
      if (read <= 0) break;
      request.append(buffer, static_cast<size_t>(read));
    }
    {
      std::lock_guard lock(mutex_);
      request_ = request;
    }

    size_t offset = 0;
    bool partial = false;
    bool range_not_satisfiable = false;
    constexpr std::string_view kRange = "Range: bytes=";
    const auto range = request.find(kRange);
    if (honor_range_ && range != std::string::npos) {
      const auto begin = range + kRange.size();
      const auto end = request.find('-', begin);
      if (end != std::string::npos) {
        try {
          offset = std::stoull(request.substr(begin, end - begin));
          partial = offset < body_.size();
          range_not_satisfiable = offset >= body_.size();
        } catch (...) {
          offset = 0;
        }
      }
    }

    const std::string_view payload = range_not_satisfiable
                                         ? std::string_view{}
                                         : std::string_view(body_).substr(partial ? offset : 0);
    std::string response = range_not_satisfiable ? "HTTP/1.1 416 Range Not Satisfiable\r\n"
                                                 : (partial ? "HTTP/1.1 206 Partial Content\r\n"
                                                            : "HTTP/1.1 200 OK\r\n");
    if (partial) {
      response += "Content-Range: bytes " + std::to_string(offset) + "-" +
                  std::to_string(body_.size() - 1) + "/" + std::to_string(body_.size()) + "\r\n";
    } else if (range_not_satisfiable) {
      response += "Content-Range: bytes */" + std::to_string(body_.size()) + "\r\n";
    }
    response +=
        "Content-Length: " + std::to_string(payload.size()) + "\r\nConnection: close\r\n\r\n";
    response.append(payload);
    SendAll(client, response);
    shutdown(client, SD_BOTH);
    closesocket(client);
  }

  std::string body_;
  bool honor_range_{false};
  SOCKET socket_{INVALID_SOCKET};
  uint16_t port_{0};
  std::thread worker_;
  mutable std::mutex mutex_;
  std::string request_;
};

void WriteFile(const std::filesystem::path& path, std::string_view contents) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

azookey::host::HttpDownloadRequest Request(std::wstring url,
                                           const std::filesystem::path& destination,
                                           std::string expected_sha256) {
  azookey::host::HttpDownloadRequest request;
  request.url = std::move(url);
  request.destination = destination;
  request.expected_sha256 = std::move(expected_sha256);
  request.connect_timeout_ms = 1000;
  request.send_timeout_ms = 1000;
  request.receive_timeout_ms = 1000;
  return request;
}

TEST(HttpDownloaderTest, Sha256MismatchNeverPromotesPartFile) {
  WinsockScope winsock;
  ASSERT_TRUE(winsock.ok());
  TempDirectory temp;
  LocalHttpServer server(std::string(kFixture), false);
  ASSERT_TRUE(server.ok());
  const auto destination = temp.path() / "model.gguf";

  azookey::host::HttpDownloader downloader;
  auto result = downloader.Download(Request(server.url(), destination, std::string(64, '0')));

  EXPECT_EQ(result.status, azookey::host::HttpDownloadStatus::Failed);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_NE(result.error->find("SHA256"), std::string::npos) << *result.error;
  EXPECT_FALSE(std::filesystem::exists(destination));
  EXPECT_TRUE(std::filesystem::exists(destination.wstring() + L".part"));
}

TEST(HttpDownloaderTest, ResumesPartFileWithRangeAndPromotesOnlyAfterVerification) {
  WinsockScope winsock;
  ASSERT_TRUE(winsock.ok());
  TempDirectory temp;
  LocalHttpServer server(std::string(kFixture), true);
  ASSERT_TRUE(server.ok());
  const auto destination = temp.path() / "model.gguf";
  constexpr size_t kPrefixSize = 7;
  WriteFile(destination.wstring() + L".part", kFixture.substr(0, kPrefixSize));

  azookey::host::HttpDownloader downloader;
  auto result =
      downloader.Download(Request(server.url(), destination, std::string(kFixtureSha256)));

  EXPECT_EQ(result.status, azookey::host::HttpDownloadStatus::Downloaded);
  EXPECT_TRUE(result.resumed);
  EXPECT_EQ(ReadFile(destination), kFixture);
  EXPECT_FALSE(std::filesystem::exists(destination.wstring() + L".part"));
  EXPECT_NE(server.request().find("Range: bytes=7-"), std::string::npos);
}

TEST(HttpDownloaderTest, RestartsFromBeginningWhenServerIgnoresRange) {
  WinsockScope winsock;
  ASSERT_TRUE(winsock.ok());
  TempDirectory temp;
  LocalHttpServer server(std::string(kFixture), false);
  ASSERT_TRUE(server.ok());
  const auto destination = temp.path() / "model.gguf";
  WriteFile(destination.wstring() + L".part", "stale-part");

  azookey::host::HttpDownloader downloader;
  auto result =
      downloader.Download(Request(server.url(), destination, std::string(kFixtureSha256)));

  EXPECT_EQ(result.status, azookey::host::HttpDownloadStatus::Downloaded);
  EXPECT_FALSE(result.resumed);
  EXPECT_EQ(ReadFile(destination), kFixture);
  EXPECT_NE(server.request().find("Range: bytes=10-"), std::string::npos);
}

TEST(HttpDownloaderTest, PromotesCompletePartAfterRangeNotSatisfiable) {
  WinsockScope winsock;
  ASSERT_TRUE(winsock.ok());
  TempDirectory temp;
  LocalHttpServer server(std::string(kFixture), true);
  ASSERT_TRUE(server.ok());
  const auto destination = temp.path() / "model.gguf";
  WriteFile(destination.wstring() + L".part", kFixture);

  azookey::host::HttpDownloader downloader;
  auto result =
      downloader.Download(Request(server.url(), destination, std::string(kFixtureSha256)));

  EXPECT_EQ(result.status, azookey::host::HttpDownloadStatus::Downloaded);
  EXPECT_TRUE(result.resumed);
  EXPECT_EQ(result.bytes_received, 0u);
  EXPECT_EQ(ReadFile(destination), kFixture);
  EXPECT_FALSE(std::filesystem::exists(destination.wstring() + L".part"));
  EXPECT_NE(server.request().find("Range: bytes=21-"), std::string::npos);
}

TEST(HttpDownloaderTest, ValidExistingFileNeedsNoNetwork) {
  TempDirectory temp;
  const auto destination = temp.path() / "model.gguf";
  WriteFile(destination, kFixture);

  azookey::host::HttpDownloader downloader;
  auto result = downloader.Download(
      Request(L"http://127.0.0.1:1/unavailable.gguf", destination, std::string(kFixtureSha256)));

  EXPECT_EQ(result.status, azookey::host::HttpDownloadStatus::AlreadyValid);
  EXPECT_EQ(ReadFile(destination), kFixture);
}

TEST(HttpDownloaderTest, NetworkFailurePreservesExistingFile) {
  TempDirectory temp;
  const auto destination = temp.path() / "model.gguf";
  WriteFile(destination, "existing-model");

  azookey::host::HttpDownloader downloader;
  auto result = downloader.Download(
      Request(L"http://127.0.0.1:1/unavailable.gguf", destination, std::string(kFixtureSha256)));

  EXPECT_EQ(result.status, azookey::host::HttpDownloadStatus::Failed);
  EXPECT_EQ(ReadFile(destination), "existing-model");
}

TEST(HttpDownloaderTest, RejectsCleartextHttpForNonLoopbackHost) {
  TempDirectory temp;
  const auto destination = temp.path() / "model.gguf";

  azookey::host::HttpDownloader downloader;
  auto result = downloader.Download(
      Request(L"http://example.com/model.gguf", destination, std::string(kFixtureSha256)));

  EXPECT_EQ(result.status, azookey::host::HttpDownloadStatus::Failed);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_NE(result.error->find("loopback"), std::string::npos) << *result.error;
  EXPECT_FALSE(std::filesystem::exists(destination));
  EXPECT_FALSE(std::filesystem::exists(destination.wstring() + L".part"));
}

}  // namespace
