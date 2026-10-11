#pragma once

#include <Windows.h>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include "azookey/ipc/ProofreadSession.h"

namespace azookey::tsf {

// Integrity, elevation and AppContainer state of the process the TIP runs in.
struct ProofreadProcessToken {
  bool app_container{false};
  bool elevated{false};
  DWORD integrity_rid{0};
};

// The batch correction view runs only in a medium-integrity, unelevated,
// non-AppContainer process: an elevated process would start the settings app
// elevated, and a low or AppContainer process cannot serve the session pipe
// to it (rich-features-spec X-3-6).
bool ProofreadAllowedForToken(const ProofreadProcessToken& token) noexcept;
std::optional<ProofreadProcessToken> QueryCurrentProcessTokenForProofread() noexcept;
bool ProofreadAllowedInCurrentProcess() noexcept;

// FNV-1a over the UTF-16 code units; compares the checked window with the
// document when a select request arrives without keeping the text.
uint64_t ProofreadTextHash(std::wstring_view text) noexcept;

// True when [start, start + length) lies within `text` and neither end
// falls between the two halves of a surrogate pair.
bool IsValidProofreadSelection(std::wstring_view text, uint64_t start, uint64_t length) noexcept;

// The TIP end of one session (ipc/ProofreadSession.h). Start, CompleteSelection
// and Stop run on the TSF UI thread. A worker thread serves the pipe and hands
// select requests and the end of the session back to the UI thread through a
// message-only window, so the callbacks also run on the UI thread.
class ProofreadSessionServer final {
 public:
  struct Callbacks {
    void* context{nullptr};
    // A select request; answer it with CompleteSelection.
    void (*on_select)(void* context, ipc::proofread::SelectRequest request) noexcept {nullptr};
    // The session ended on its own (client gone, idle, or a protocol error).
    // The server may be destroyed from inside this callback.
    void (*on_end)(void* context) noexcept {nullptr};
  };
  // Decides whether the connected client may receive the document.
  using ClientVerifier = bool (*)(HANDLE pipe) noexcept;

  struct Options {
    std::chrono::milliseconds idle_timeout{std::chrono::minutes(10)};
    // Defaults to VerifySettingsAppClient.
    ClientVerifier verifier{nullptr};
    // Clients refused before the session ends.
    int max_rejected_clients{8};
  };

  ProofreadSessionServer() = default;
  ~ProofreadSessionServer();
  ProofreadSessionServer(const ProofreadSessionServer&) = delete;
  ProofreadSessionServer& operator=(const ProofreadSessionServer&) = delete;

  // Creates the pipe for `token` before returning, so the settings app can be
  // launched right after. False when the pipe, window or thread is missing.
  bool Start(std::string_view token, std::string document_json, Callbacks callbacks,
             Options options);
  // The answer to the latest select request.
  void CompleteSelection(ipc::proofread::SelectResult result);
  // Ends the session. No callback runs after this returns.
  void Stop();
  bool active() const { return hwnd_ != nullptr; }
#ifdef AZOOKEY_TSF_TESTING
  bool holds_document_for_test() {
    std::lock_guard lock(mutex_);
    return !document_json_.empty();
  }
#endif

  // The client is the settings app next to this TIP (SettingsLauncher).
  static bool VerifySettingsAppClient(HANDLE pipe) noexcept;

 private:
  static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
  void Run() noexcept;
  void DeliverSelect();
  void DeliverEnd();
  void WipeDocument();
  void WipeDocumentLocked();

  HWND hwnd_{nullptr};
  HANDLE pipe_{INVALID_HANDLE_VALUE};
  HANDLE stop_event_{nullptr};
  HANDLE result_event_{nullptr};
  std::thread worker_;
  std::string document_json_;
  Callbacks callbacks_;
  Options options_;
  std::mutex mutex_;
  std::optional<ipc::proofread::SelectRequest> pending_request_;
  std::optional<ipc::proofread::SelectResult> result_;
};

}  // namespace azookey::tsf
