#include <Windows.h>
#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <thread>

#include "ProofreadSessionClient.h"
#include "azookey/ipc/ProofreadSession.h"

// The settings-app end of the batch correction session (rich-features-spec X-3-6), against a
// stand-in TIP server on the real session pipe.

namespace {

namespace proofread = azookey::ipc::proofread;
using azookey::settings::ProofreadSessionClient;

std::string FreshToken() {
  auto token = proofread::GenerateSessionToken();
  return token ? *token : std::string();
}

// Blocking server I/O on an overlapped pipe handle.
bool ServerTransfer(HANDLE pipe, bool write, uint8_t* data, DWORD size) {
  DWORD done = 0;
  while (done < size) {
    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD transferred = 0;
    BOOL ok = write ? WriteFile(pipe, data + done, size - done, nullptr, &overlapped)
                    : ReadFile(pipe, data + done, size - done, nullptr, &overlapped);
    if (ok || GetLastError() == ERROR_IO_PENDING)
      ok = GetOverlappedResult(pipe, &overlapped, &transferred, TRUE);
    CloseHandle(overlapped.hEvent);
    if (!ok || transferred == 0) return false;
    done += transferred;
  }
  return true;
}

bool ServerWrite(HANDLE pipe, const std::string& json, uint32_t max_bytes) {
  auto frame = proofread::EncodeFrame(json, max_bytes);
  return frame && ServerTransfer(pipe, true, frame->data(), static_cast<DWORD>(frame->size()));
}

std::optional<std::string> ServerRead(HANDLE pipe) {
  uint8_t header[4]{};
  if (!ServerTransfer(pipe, false, header, sizeof(header))) return std::nullopt;
  const auto size = proofread::DecodeFrameLength(header, proofread::kMaxRequestFrameBytes);
  if (!size) return std::nullopt;
  std::string body(*size, '\0');
  if (!ServerTransfer(pipe, false, reinterpret_cast<uint8_t*>(body.data()), *size))
    return std::nullopt;
  return body;
}

bool ServerAccept(HANDLE pipe) {
  OVERLAPPED overlapped{};
  overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  bool connected = ConnectNamedPipe(pipe, &overlapped) != FALSE;
  if (!connected) {
    const DWORD error = GetLastError();
    DWORD ignored = 0;
    connected = error == ERROR_PIPE_CONNECTED ||
                (error == ERROR_IO_PENDING &&
                 GetOverlappedResult(pipe, &overlapped, &ignored, TRUE) != FALSE);
  }
  CloseHandle(overlapped.hEvent);
  return connected;
}

std::string Document(uint64_t window) {
  proofread::DocumentMessage message;
  message.status = proofread::DocumentStatus::Captured;
  message.text = "アプリの本文";
  message.secure = false;
  message.learning_allowed = true;
  message.window = window;
  return proofread::BuildDocumentMessage(message);
}

// Serves one session on `token`: sends `document`, then runs `after` with the connected pipe.
class StandInTip {
 public:
  StandInTip(
      const std::string& token, std::string document,
      std::function<void(HANDLE)> after = [](HANDLE) {})
      : pipe_(proofread::CreateSessionPipe(token)) {
    thread_ = std::thread([this, document = std::move(document), after = std::move(after)] {
      if (pipe_ == INVALID_HANDLE_VALUE || !ServerAccept(pipe_)) return;
      if (ServerWrite(pipe_, document, proofread::kMaxDocumentFrameBytes)) after(pipe_);
    });
  }
  ~StandInTip() {
    thread_.join();
    if (pipe_ != INVALID_HANDLE_VALUE) CloseHandle(pipe_);
  }
  bool ok() const { return pipe_ != INVALID_HANDLE_VALUE; }

 private:
  HANDLE pipe_;
  std::thread thread_;
};

TEST(ProofreadSessionClientTest, ReadsTheDocumentAndSelects) {
  const auto token = FreshToken();
  std::optional<proofread::SelectRequest> seen;
  {
    StandInTip tip(token, Document(0), [&](HANDLE pipe) {
      if (const auto json = ServerRead(pipe)) seen = proofread::ParseSelectRequest(*json);
      ServerWrite(pipe, proofread::BuildSelectResponse(proofread::SelectResult::DocumentChanged),
                  proofread::kMaxRequestFrameBytes);
    });
    ASSERT_TRUE(tip.ok());
    ProofreadSessionClient client;
    const auto document = client.Open(token);
    ASSERT_TRUE(document);
    EXPECT_EQ(document->status, proofread::DocumentStatus::Captured);
    EXPECT_EQ(document->text, "アプリの本文");
    EXPECT_FALSE(document->secure);
    EXPECT_TRUE(document->learning_allowed);
    EXPECT_EQ(client.Select(3, 2), proofread::SelectResult::DocumentChanged);
  }
  ASSERT_TRUE(seen);
  EXPECT_EQ(seen->start, 3u);
  EXPECT_EQ(seen->length, 2u);
}

TEST(ProofreadSessionClientTest, AcceptsAServerThatOwnsTheDocumentWindow) {
  // A window of this process: the stand-in server runs here too.
  const HWND window =
      CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
  ASSERT_NE(window, nullptr);
  const auto token = FreshToken();
  {
    StandInTip tip(token, Document(reinterpret_cast<uintptr_t>(window)));
    ProofreadSessionClient client;
    EXPECT_TRUE(client.Open(token));
  }
  DestroyWindow(window);
}

TEST(ProofreadSessionClientTest, RefusesAServerThatDoesNotOwnTheDocumentWindow) {
  const HWND foreign = GetDesktopWindow();
  DWORD owner = 0;
  GetWindowThreadProcessId(foreign, &owner);
  if (!foreign || owner == GetCurrentProcessId()) GTEST_SKIP() << "no window of another process";
  const auto token = FreshToken();
  StandInTip tip(token, Document(reinterpret_cast<uintptr_t>(foreign)));
  ProofreadSessionClient client;
  EXPECT_FALSE(client.Open(token));
  // The refused session is closed; nothing more is sent.
  EXPECT_FALSE(client.Select(0, 1));
}

bool RefuseServer(HANDLE) noexcept { return false; }

// A server that fails the identity check gets nothing read from it.
TEST(ProofreadSessionClientTest, RefusesAServerThatFailsTheIdentityCheck) {
  const auto token = FreshToken();
  StandInTip tip(token, Document(0));
  ProofreadSessionClient client(std::chrono::seconds(10), &RefuseServer);
  EXPECT_FALSE(client.Open(token));
  EXPECT_FALSE(client.Select(0, 1));
}

// The stand-in server is this process: same user and logon, at medium or above.
TEST(ProofreadSessionClientTest, IdentityCheckAdmitsAServerOfThisLogonSession) {
  const auto token = FreshToken();
  const HANDLE pipe = proofread::CreateSessionPipe(token);
  ASSERT_NE(pipe, INVALID_HANDLE_VALUE);
  const auto name = proofread::SessionPipeName(token);
  const HANDLE client = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                    OPEN_EXISTING, 0, nullptr);
  ASSERT_NE(client, INVALID_HANDLE_VALUE);
  EXPECT_TRUE(ProofreadSessionClient::VerifySessionServer(client));
  // A handle that is not a pipe cannot be checked, so it is refused.
  EXPECT_FALSE(ProofreadSessionClient::VerifySessionServer(GetCurrentProcess()));
  CloseHandle(client);
  CloseHandle(pipe);
}

TEST(ProofreadSessionClientTest, RefusesAMalformedDocument) {
  const auto token = FreshToken();
  StandInTip tip(token, R"({"kind":"document","status":"blocked","text":"leaked"})");
  ProofreadSessionClient client;
  EXPECT_FALSE(client.Open(token));
}

TEST(ProofreadSessionClientTest, AnUnansweredSelectTimesOut) {
  const auto token = FreshToken();
  HANDLE answered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  {
    StandInTip tip(token, Document(0), [&](HANDLE pipe) {
      (void)ServerRead(pipe);
      // Hold the request without an answer until the client gave up.
      WaitForSingleObject(answered, 10000);
    });
    ProofreadSessionClient client(std::chrono::milliseconds(2000));
    ASSERT_TRUE(client.Open(token));
    EXPECT_FALSE(client.Select(0, 1));
    SetEvent(answered);
    client.Close();
  }
  CloseHandle(answered);
}

TEST(ProofreadSessionClientTest, NoSessionWithoutAPipeOrWithABadToken) {
  ProofreadSessionClient client(std::chrono::milliseconds(50));
  EXPECT_FALSE(client.Open(FreshToken()));
  EXPECT_FALSE(client.Open("not-a-token"));
  EXPECT_FALSE(client.Select(0, 1));
}

}  // namespace
