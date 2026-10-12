#pragma once

#include <Windows.h>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "azookey/ipc/ProofreadSession.h"

namespace azookey::settings {

// The settings-app end of the TIP's batch correction session
// (ipc/ProofreadSession.h, rich-features-spec X-3-6). Every call blocks, so the
// pane makes them off the UI thread; `timeout` bounds each wait.
class ProofreadSessionClient final {
 public:
  // Decides whether the process serving the pipe may be trusted with the session.
  using ServerVerifier = bool (*)(HANDLE pipe) noexcept;

  explicit ProofreadSessionClient(std::chrono::milliseconds timeout = std::chrono::seconds(10),
                                  ServerVerifier verifier = &VerifySessionServer) noexcept
      : timeout_(timeout), verifier_(verifier) {}
  ~ProofreadSessionClient();
  ProofreadSessionClient(const ProofreadSessionClient&) = delete;
  ProofreadSessionClient& operator=(const ProofreadSessionClient&) = delete;

  // Connects to the session pipe for `token` and reads the document the TIP
  // sends first. std::nullopt when the pipe is missing, the server is not the
  // process that owns the document's window, or the document is malformed.
  std::optional<ipc::proofread::DocumentMessage> Open(std::string_view token);
  // Asks the TIP to select [start, start + length) of the document text and,
  // with `reconvert`, to open reconversion candidates for it, offering
  // `suggestions` among them as text. The result is the selection's only.
  // std::nullopt when the session is gone or the answer is malformed.
  std::optional<ipc::proofread::SelectResult> Select(uint64_t start, uint64_t length,
                                                     bool reconvert = false,
                                                     std::vector<std::string> suggestions = {});
  void Close();

  // The server runs as this user in this logon session at medium integrity or
  // above. A failed query refuses it. A legitimate TIP never serves from a
  // lower integrity process, so a low or AppContainer process that names its
  // own pipe on the command line is refused here.
  static bool VerifySessionServer(HANDLE pipe) noexcept;

 private:
  bool Transfer(bool write, uint8_t* data, DWORD size);
  std::optional<std::string> ReadMessage(uint32_t max_bytes);

  std::chrono::milliseconds timeout_;
  ServerVerifier verifier_;
  std::mutex mutex_;
  HANDLE pipe_{INVALID_HANDLE_VALUE};
  HANDLE event_{nullptr};
};

}  // namespace azookey::settings
