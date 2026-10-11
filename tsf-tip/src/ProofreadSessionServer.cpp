#include "azookey/tsf/ProofreadSessionServer.h"

#include <array>
#include <filesystem>
#include <utility>
#include <vector>

#include "azookey/tsf/SettingsLauncher.h"
#include "azookey/tsf/TipRuntimeLog.h"

namespace azookey::tsf {
namespace {

namespace proofread = ipc::proofread;

constexpr wchar_t kWindowClass[] = L"AzooKeyTipProofreadSession";
constexpr UINT kMsgSelect = WM_APP + 1;
constexpr UINT kMsgEnd = WM_APP + 2;

bool IsHighSurrogate(wchar_t value) { return value >= 0xd800 && value <= 0xdbff; }
bool IsLowSurrogate(wchar_t value) { return value >= 0xdc00 && value <= 0xdfff; }

bool EnsureWindowClass(HINSTANCE instance, WNDPROC proc) {
  WNDCLASSEXW existing{};
  if (GetClassInfoExW(instance, kWindowClass, &existing)) return true;
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = proc;
  wc.hInstance = instance;
  wc.lpszClassName = kWindowClass;
  return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

DWORD TimeoutMs(std::chrono::milliseconds timeout) {
  if (timeout.count() <= 0) return 0;
  if (timeout.count() >= INFINITE) return INFINITE - 1;
  return static_cast<DWORD>(timeout.count());
}

enum class IoWait { Done, Stopped, TimedOut, Failed };

// Waits for an overlapped operation on `pipe`, or for stop or the timeout, in
// which case the operation is cancelled and drained before returning.
IoWait WaitIo(HANDLE pipe, OVERLAPPED& overlapped, HANDLE stop, DWORD timeout, DWORD* transferred) {
  const std::array<HANDLE, 2> handles{overlapped.hEvent, stop};
  const DWORD wait = WaitForMultipleObjects(2, handles.data(), FALSE, timeout);
  if (wait == WAIT_OBJECT_0)
    return GetOverlappedResult(pipe, &overlapped, transferred, FALSE) ? IoWait::Done
                                                                      : IoWait::Failed;
  CancelIoEx(pipe, &overlapped);
  DWORD ignored = 0;
  GetOverlappedResult(pipe, &overlapped, &ignored, TRUE);
  if (wait == WAIT_OBJECT_0 + 1) return IoWait::Stopped;
  return wait == WAIT_TIMEOUT ? IoWait::TimedOut : IoWait::Failed;
}

IoWait Transfer(bool write, HANDLE pipe, HANDLE event, HANDLE stop, uint8_t* data, DWORD size,
                DWORD timeout) {
  DWORD done = 0;
  while (done < size) {
    OVERLAPPED overlapped{};
    overlapped.hEvent = event;
    ResetEvent(event);
    const BOOL ok = write ? WriteFile(pipe, data + done, size - done, nullptr, &overlapped)
                          : ReadFile(pipe, data + done, size - done, nullptr, &overlapped);
    if (!ok && GetLastError() != ERROR_IO_PENDING) return IoWait::Failed;
    DWORD transferred = 0;
    const auto result = WaitIo(pipe, overlapped, stop, timeout, &transferred);
    if (result != IoWait::Done) return result;
    if (transferred == 0) return IoWait::Failed;
    done += transferred;
  }
  return IoWait::Done;
}

bool SamePath(const std::wstring& left, const std::wstring& right) {
  return CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()), right.c_str(),
                              static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

}  // namespace

bool ProofreadAllowedForToken(const ProofreadProcessToken& token) noexcept {
  return !token.app_container && !token.elevated &&
         token.integrity_rid >= SECURITY_MANDATORY_MEDIUM_RID &&
         token.integrity_rid < SECURITY_MANDATORY_HIGH_RID;
}

std::optional<ProofreadProcessToken> QueryCurrentProcessTokenForProofread() noexcept {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return std::nullopt;
  ProofreadProcessToken result;
  DWORD size = 0;
  DWORD app_container = 0;
  TOKEN_ELEVATION elevation{};
  std::array<uint8_t, sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE> label{};
  const bool ok =
      GetTokenInformation(token, TokenIsAppContainer, &app_container, sizeof(app_container),
                          &size) &&
      GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) &&
      GetTokenInformation(token, TokenIntegrityLevel, label.data(),
                          static_cast<DWORD>(label.size()), &size);
  CloseHandle(token);
  if (!ok) return std::nullopt;
  const auto* mandatory = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(label.data());
  const UCHAR count = *GetSidSubAuthorityCount(mandatory->Label.Sid);
  if (count == 0) return std::nullopt;
  result.app_container = app_container != 0;
  result.elevated = elevation.TokenIsElevated != 0;
  result.integrity_rid = *GetSidSubAuthority(mandatory->Label.Sid, count - 1u);
  return result;
}

bool ProofreadAllowedInCurrentProcess() noexcept {
  static const bool allowed = [] {
    const auto token = QueryCurrentProcessTokenForProofread();
    return token && ProofreadAllowedForToken(*token);
  }();
  return allowed;
}

uint64_t ProofreadTextHash(std::wstring_view text) noexcept {
  uint64_t hash = 14695981039346656037ull;
  for (const wchar_t unit : text) {
    hash ^= static_cast<uint16_t>(unit);
    hash *= 1099511628211ull;
  }
  return hash;
}

bool IsValidProofreadSelection(std::wstring_view text, uint64_t start, uint64_t length) noexcept {
  if (start > text.size() || length > text.size() - start) return false;
  const auto splits_pair = [&](uint64_t offset) {
    return offset > 0 && offset < text.size() && IsLowSurrogate(text[offset]) &&
           IsHighSurrogate(text[offset - 1]);
  };
  return !splits_pair(start) && !splits_pair(start + length);
}

ProofreadSessionServer::~ProofreadSessionServer() { Stop(); }

bool ProofreadSessionServer::Start(std::string_view token, std::string document_json,
                                   Callbacks callbacks, Options options) {
  Stop();
  if (!callbacks.on_select || !callbacks.on_end) return false;
  if (!proofread::EncodeFrame(document_json, proofread::kMaxDocumentFrameBytes)) return false;
  HINSTANCE instance = nullptr;
  GetModuleHandleExW(
      GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      reinterpret_cast<LPCWSTR>(&ProofreadSessionServer::WindowProc), &instance);
  if (!EnsureWindowClass(instance, &ProofreadSessionServer::WindowProc)) return false;

  pipe_ = proofread::CreateSessionPipe(token);
  stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  result_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  hwnd_ = CreateWindowExW(0, kWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance,
                          nullptr);
  if (pipe_ == INVALID_HANDLE_VALUE || !stop_event_ || !result_event_ || !hwnd_) {
    Stop();
    return false;
  }
  SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  document_json_ = std::move(document_json);
  callbacks_ = callbacks;
  options_ = options;
  if (!options_.verifier) options_.verifier = &ProofreadSessionServer::VerifySettingsAppClient;
  try {
    worker_ = std::thread([this] { Run(); });
  } catch (...) {
    Stop();
    return false;
  }
  return true;
}

void ProofreadSessionServer::CompleteSelection(ipc::proofread::SelectResult result) {
  std::lock_guard lock(mutex_);
  if (!result_event_) return;
  result_ = result;
  SetEvent(result_event_);
}

void ProofreadSessionServer::Stop() {
  if (stop_event_) SetEvent(stop_event_);
  if (worker_.joinable()) worker_.join();
  if (pipe_ != INVALID_HANDLE_VALUE) {
    DisconnectNamedPipe(pipe_);
    CloseHandle(pipe_);
    pipe_ = INVALID_HANDLE_VALUE;
  }
  if (hwnd_) {
    // Messages the worker already posted find no server and are dropped.
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
  std::lock_guard lock(mutex_);
  if (stop_event_) CloseHandle(stop_event_);
  if (result_event_) CloseHandle(result_event_);
  stop_event_ = nullptr;
  result_event_ = nullptr;
  pending_request_.reset();
  result_.reset();
  document_json_.clear();
  document_json_.shrink_to_fit();
}

bool ProofreadSessionServer::VerifySettingsAppClient(HANDLE pipe) noexcept {
  try {
    ULONG process_id = 0;
    if (!GetNamedPipeClientProcessId(pipe, &process_id)) return false;
    std::wstring expected;
    if (FAILED(ResolveSettingsExecutable(&expected))) return false;
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (!process) return false;
    std::vector<wchar_t> buffer(32768);
    DWORD length = static_cast<DWORD>(buffer.size());
    const BOOL ok = QueryFullProcessImageNameW(process, 0, buffer.data(), &length);
    CloseHandle(process);
    if (!ok) return false;
    std::error_code error;
    const auto actual =
        std::filesystem::weakly_canonical(std::wstring(buffer.data(), length), error);
    if (error) return false;
    const auto canonical_expected = std::filesystem::weakly_canonical(expected, error);
    if (error) return false;
    return SamePath(actual.wstring(), canonical_expected.wstring());
  } catch (...) {
    return false;
  }
}

LRESULT CALLBACK ProofreadSessionServer::WindowProc(HWND hwnd, UINT msg, WPARAM wparam,
                                                    LPARAM lparam) {
  auto* server = reinterpret_cast<ProofreadSessionServer*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (server && msg == kMsgSelect) {
    server->DeliverSelect();
    return 0;
  }
  if (server && msg == kMsgEnd) {
    // May destroy the server; nothing touches it afterwards.
    server->DeliverEnd();
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void ProofreadSessionServer::DeliverSelect() {
  std::optional<proofread::SelectRequest> request;
  {
    std::lock_guard lock(mutex_);
    request = std::exchange(pending_request_, std::nullopt);
  }
  if (request) callbacks_.on_select(callbacks_.context, *request);
}

void ProofreadSessionServer::DeliverEnd() {
  const auto callbacks = callbacks_;
  Stop();
  callbacks.on_end(callbacks.context);
}

void ProofreadSessionServer::Run() noexcept {
  const DWORD idle = TimeoutMs(options_.idle_timeout);
  const HANDLE io_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  bool stopped = false;
  [&] {
    if (!io_event) return;
    // Wait for a client the verifier accepts.
    for (int rejected = 0;;) {
      OVERLAPPED overlapped{};
      overlapped.hEvent = io_event;
      ResetEvent(io_event);
      if (!ConnectNamedPipe(pipe_, &overlapped)) {
        const DWORD error = GetLastError();
        if (error == ERROR_IO_PENDING) {
          DWORD ignored = 0;
          const auto wait = WaitIo(pipe_, overlapped, stop_event_, idle, &ignored);
          if (wait == IoWait::Stopped) stopped = true;
          if (wait != IoWait::Done) return;
        } else if (error != ERROR_PIPE_CONNECTED) {
          return;
        }
      }
      if (options_.verifier(pipe_)) break;
      TipRuntimeLog(TipRuntimeLogger(), logging::RuntimeLogLevel::Warn, "proofread_client_rejected",
                    {});
      DisconnectNamedPipe(pipe_);
      if (++rejected >= options_.max_rejected_clients) return;
    }

    auto document = proofread::EncodeFrame(document_json_, proofread::kMaxDocumentFrameBytes);
    if (!document) return;
    auto wait = Transfer(true, pipe_, io_event, stop_event_, document->data(),
                         static_cast<DWORD>(document->size()), idle);
    if (wait == IoWait::Stopped) stopped = true;
    if (wait != IoWait::Done) return;
    // Sent: the TIP keeps no copy of the text for the rest of the session.
    document->assign(document->size(), 0);
    {
      std::lock_guard lock(mutex_);
      document_json_.assign(document_json_.size(), '\0');
      document_json_.clear();
      document_json_.shrink_to_fit();
    }

    for (;;) {
      uint8_t header[4]{};
      wait = Transfer(false, pipe_, io_event, stop_event_, header, sizeof(header), idle);
      if (wait == IoWait::Stopped) stopped = true;
      if (wait != IoWait::Done) return;
      const auto size = proofread::DecodeFrameLength(header, proofread::kMaxRequestFrameBytes);
      if (!size) return;
      std::string body(*size, '\0');
      wait = Transfer(false, pipe_, io_event, stop_event_, reinterpret_cast<uint8_t*>(body.data()),
                      *size, idle);
      if (wait == IoWait::Stopped) stopped = true;
      if (wait != IoWait::Done) return;
      const auto request = proofread::ParseSelectRequest(body);
      if (!request) return;

      {
        std::lock_guard lock(mutex_);
        pending_request_ = *request;
        result_.reset();
        ResetEvent(result_event_);
      }
      if (!PostMessageW(hwnd_, kMsgSelect, 0, 0)) return;
      const std::array<HANDLE, 2> handles{result_event_, stop_event_};
      const DWORD answered = WaitForMultipleObjects(2, handles.data(), FALSE, idle);
      if (answered == WAIT_OBJECT_0 + 1) stopped = true;
      if (answered != WAIT_OBJECT_0) return;
      proofread::SelectResult result = proofread::SelectResult::Failed;
      {
        std::lock_guard lock(mutex_);
        if (result_) result = *result_;
      }
      auto response = proofread::EncodeFrame(proofread::BuildSelectResponse(result),
                                             proofread::kMaxRequestFrameBytes);
      if (!response) return;
      wait = Transfer(true, pipe_, io_event, stop_event_, response->data(),
                      static_cast<DWORD>(response->size()), idle);
      if (wait == IoWait::Stopped) stopped = true;
      if (wait != IoWait::Done) return;
    }
  }();
  if (io_event) CloseHandle(io_event);
  // Stop() is joining: it owns the teardown and runs no callback.
  if (!stopped && WaitForSingleObject(stop_event_, 0) != WAIT_OBJECT_0)
    PostMessageW(hwnd_, kMsgEnd, 0, 0);
}

}  // namespace azookey::tsf
