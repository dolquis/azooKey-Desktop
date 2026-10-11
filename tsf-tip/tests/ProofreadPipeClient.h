#pragma once

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "azookey/ipc/ProofreadSession.h"

namespace azookey::tsf::test {

// The settings-app end of a batch correction session, with blocking I/O, for
// tests that drive it from a helper thread.
class ProofreadPipeClient final {
 public:
  ProofreadPipeClient() = default;
  ~ProofreadPipeClient() { Close(); }
  ProofreadPipeClient(const ProofreadPipeClient&) = delete;
  ProofreadPipeClient& operator=(const ProofreadPipeClient&) = delete;

  bool Connect(std::string_view token) {
    const auto name = ipc::proofread::SessionPipeName(token);
    pipe_ = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0,
                        nullptr);
    return pipe_ != INVALID_HANDLE_VALUE;
  }

  std::optional<std::string> Read(uint32_t max_bytes) {
    uint8_t header[4]{};
    if (!ReadAll(header, sizeof(header))) return std::nullopt;
    const auto size = ipc::proofread::DecodeFrameLength(header, max_bytes);
    if (!size) return std::nullopt;
    std::string body(*size, '\0');
    if (!ReadAll(reinterpret_cast<uint8_t*>(body.data()), *size)) return std::nullopt;
    return body;
  }

  bool Write(std::string_view json, uint32_t max_bytes = ipc::proofread::kMaxRequestFrameBytes) {
    const auto frame = ipc::proofread::EncodeFrame(json, max_bytes);
    return frame && WriteRaw(frame->data(), static_cast<DWORD>(frame->size()));
  }

  bool WriteRaw(const uint8_t* data, DWORD size) {
    DWORD written = 0;
    return WriteFile(pipe_, data, size, &written, nullptr) && written == size;
  }

  void Close() {
    if (pipe_ != INVALID_HANDLE_VALUE) CloseHandle(pipe_);
    pipe_ = INVALID_HANDLE_VALUE;
  }

 private:
  bool ReadAll(uint8_t* data, DWORD size) {
    DWORD done = 0;
    while (done < size) {
      DWORD read = 0;
      if (!ReadFile(pipe_, data + done, size - done, &read, nullptr) || read == 0) return false;
      done += read;
    }
    return true;
  }

  HANDLE pipe_{INVALID_HANDLE_VALUE};
};

// Runs the thread's message loop until `done` holds or the deadline passes,
// so callbacks posted to a message-only window run without a fixed sleep.
inline bool PumpUntil(const std::function<bool()>& done,
                      std::chrono::milliseconds limit = std::chrono::seconds(10)) {
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (!done()) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return false;
    // `done` may also change on another thread, so wake at least every 10 ms.
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    MsgWaitForMultipleObjects(0, nullptr, FALSE, static_cast<DWORD>((std::min)(remaining, 10LL)),
                              QS_ALLINPUT);
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  return true;
}

}  // namespace azookey::tsf::test
