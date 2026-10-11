#include "ProofreadSessionClient.h"

#include <utility>
#include <vector>

namespace azookey::settings {

namespace proofread = azookey::ipc::proofread;

ProofreadSessionClient::~ProofreadSessionClient() { Close(); }

void ProofreadSessionClient::Close() {
  std::lock_guard lock(mutex_);
  if (pipe_ != INVALID_HANDLE_VALUE) CloseHandle(pipe_);
  if (event_) CloseHandle(event_);
  pipe_ = INVALID_HANDLE_VALUE;
  event_ = nullptr;
}

bool ProofreadSessionClient::Transfer(bool write, uint8_t* data, DWORD size) {
  const auto timeout = static_cast<DWORD>(timeout_.count());
  DWORD done = 0;
  while (done < size) {
    OVERLAPPED overlapped{};
    overlapped.hEvent = event_;
    ResetEvent(event_);
    const BOOL ok = write ? WriteFile(pipe_, data + done, size - done, nullptr, &overlapped)
                          : ReadFile(pipe_, data + done, size - done, nullptr, &overlapped);
    if (!ok && GetLastError() != ERROR_IO_PENDING) return false;
    if (WaitForSingleObject(event_, timeout) != WAIT_OBJECT_0) {
      CancelIoEx(pipe_, &overlapped);
      DWORD ignored = 0;
      GetOverlappedResult(pipe_, &overlapped, &ignored, TRUE);
      return false;
    }
    DWORD transferred = 0;
    if (!GetOverlappedResult(pipe_, &overlapped, &transferred, FALSE) || transferred == 0)
      return false;
    done += transferred;
  }
  return true;
}

std::optional<std::string> ProofreadSessionClient::ReadMessage(uint32_t max_bytes) {
  uint8_t header[4]{};
  if (!Transfer(false, header, sizeof(header))) return std::nullopt;
  const auto size = proofread::DecodeFrameLength(header, max_bytes);
  if (!size) return std::nullopt;
  std::string body(*size, '\0');
  if (!Transfer(false, reinterpret_cast<uint8_t*>(body.data()), *size)) return std::nullopt;
  return body;
}

std::optional<proofread::DocumentMessage> ProofreadSessionClient::Open(std::string_view token) {
  if (!proofread::IsValidSessionToken(token)) return std::nullopt;
  {
    std::lock_guard lock(mutex_);
    if (pipe_ != INVALID_HANDLE_VALUE) return std::nullopt;
    const auto name = proofread::SessionPipeName(token);
    if (!WaitNamedPipeW(name.c_str(), static_cast<DWORD>(timeout_.count()))) return std::nullopt;
    // Identification only: the TIP cannot act as this process.
    pipe_ = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                        FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                        nullptr);
    event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  }
  if (pipe_ == INVALID_HANDLE_VALUE || !event_) {
    Close();
    return std::nullopt;
  }
  std::optional<proofread::DocumentMessage> document;
  {
    std::lock_guard lock(mutex_);
    if (const auto json = ReadMessage(proofread::kMaxDocumentFrameBytes))
      document = proofread::ParseDocumentMessage(*json);
  }
  // The document names the application window; the pipe must be served from
  // that application, so another process holding the name is refused.
  ULONG server_process = 0;
  DWORD window_process = 0;
  if (document && document->window != 0) {
    const auto window = reinterpret_cast<HWND>(static_cast<uintptr_t>(document->window));
    if (!GetNamedPipeServerProcessId(pipe_, &server_process) || !IsWindow(window) ||
        GetWindowThreadProcessId(window, &window_process) == 0 || window_process != server_process)
      document.reset();
  }
  if (!document) Close();
  return document;
}

std::optional<proofread::SelectResult> ProofreadSessionClient::Select(uint64_t start,
                                                                      uint64_t length) {
  std::lock_guard lock(mutex_);
  if (pipe_ == INVALID_HANDLE_VALUE) return std::nullopt;
  auto frame = proofread::EncodeFrame(proofread::BuildSelectRequest({start, length}),
                                      proofread::kMaxRequestFrameBytes);
  if (!frame || !Transfer(true, frame->data(), static_cast<DWORD>(frame->size())))
    return std::nullopt;
  const auto json = ReadMessage(proofread::kMaxRequestFrameBytes);
  if (!json) return std::nullopt;
  return proofread::ParseSelectResponse(*json);
}

}  // namespace azookey::settings
