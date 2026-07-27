#include "azookey/ipc/NamedPipeTransport.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cwchar>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "azookey/ipc/Limits.h"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <sddl.h>
#include <wil/resource.h>

#include "NamedPipeTransportInternal.h"

#endif

namespace azookey::ipc {

#ifdef _WIN32

namespace internal {

std::wstring BuildPipeSecuritySddl(const std::wstring& logon_sid, bool restricted_token) {
  // FILE_GENERIC_READ | FILE_GENERIC_WRITE | SYNCHRONIZE, expressed as
  // individual file rights rather than GENERIC_ALL. FILE_APPEND_DATA shares
  // FILE_CREATE_PIPE_INSTANCE's value and remains necessary for this
  // multi-instance duplex server.
  constexpr wchar_t kPipeAccessRights[] = L"0x12019f";
  std::wstring sddl = L"D:P(A;;";
  sddl += kPipeAccessRights;
  sddl += L";;;";
  sddl += logon_sid;
  sddl += L")";
  if (restricted_token) {
    sddl += L"(A;;";
    sddl += kPipeAccessRights;
    sddl += L";;;WD)";
  }
  return sddl;
}

HANDLE OpenPipeClientHandle(const std::wstring& pipe_name) {
  constexpr DWORD kClientAccess =
      FILE_READ_DATA | FILE_WRITE_DATA | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE;
  return CreateFileW(pipe_name.c_str(), kClientAccess, 0, nullptr, OPEN_EXISTING,
                     SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
}

ImmediateOverlappedResult QueryImmediateOverlappedTransfer(HANDLE pipe, OVERLAPPED& overlapped) {
  DWORD transferred = 0;
  const BOOL completed = GetOverlappedResult(pipe, &overlapped, &transferred, FALSE);
  return {completed, completed ? ERROR_SUCCESS : GetLastError(), transferred};
}

void CaptureImmediateOverlappedTransfer(HANDLE pipe, OVERLAPPED& overlapped, PipeIoResult& result,
                                        ImmediateOverlappedProbe probe) {
  // ERROR_MORE_DATA is a completed partial message-mode read; keep
  // its byte count while hard synchronous failures preserve their error.
  if (!result.ok && result.error != ERROR_MORE_DATA) {
    return;
  }

  const auto completed = probe(pipe, overlapped);
  if (completed.completed || completed.error == ERROR_MORE_DATA) {
    result.transferred = completed.transferred;
  }
}

}  // namespace internal

namespace {

using internal::CaptureImmediateOverlappedTransfer;
using internal::PipeIoResult;

constexpr DWORD kPipeBufferSize = 64 * 1024;
constexpr size_t kMaxTransientReadNoDataRetries = 100;
constexpr size_t kMaxTransientWriteNoProgressRetries = 100;
constexpr std::chrono::milliseconds kFailedAcceptBackoff(25);

// Frame deadlines (docs/dev-infrastructure-spec.md §6.4.7). These bound the time
// a *single frame already in flight* may take, which is a different layer from
// the request-level timeouts of §8.5.2: a connection sitting idle between
// requests is never charged against them. Reads get the tighter budget because
// the peer has already announced the frame; writes get a looser one because a
// momentarily unresponsive reader legitimately stalls a >64 KiB response once
// the pipe buffer fills.
constexpr std::chrono::milliseconds kFrameSoftDeadline(500);
constexpr std::chrono::milliseconds kFrameReadHardDeadline(2000);
constexpr std::chrono::milliseconds kFrameWriteHardDeadline(5000);

std::wstring Utf8ToWide(const std::string& value) {
  if (value.empty()) return {};
  const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                       static_cast<int>(value.size()), nullptr, 0);
  if (size <= 0) return {};
  std::wstring wide(static_cast<size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                      wide.data(), size);
  return wide;
}

std::string WideToUtf8(const std::wstring& value) {
  if (value.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                       static_cast<int>(value.size()), nullptr, 0,
                                       nullptr, nullptr);
  if (size <= 0) return {};
  std::string utf8(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                      utf8.data(), size, nullptr, nullptr);
  return utf8;
}

bool ForceSidFailureForTesting() {
#ifndef NDEBUG
  wchar_t forced[2]{};
  return GetEnvironmentVariableW(L"AZOOKEY_TEST_FORCE_SID_FAILURE", forced, 2) > 0;
#else
  return false;
#endif
}

bool QueryTokenInformationBuffer(HANDLE token, TOKEN_INFORMATION_CLASS info_class,
                                 std::vector<uint8_t>& buffer) {
  DWORD size = 0;
  if (GetTokenInformation(token, info_class, nullptr, 0, &size) ||
      GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) {
    return false;
  }
  buffer.resize(size);
  return GetTokenInformation(token, info_class, buffer.data(), size, &size) != FALSE;
}

std::wstring SidToString(PSID sid) {
  if (!sid || !IsValidSid(sid)) {
    return {};
  }
  LPWSTR sid_text = nullptr;
  if (!ConvertSidToStringSidW(sid, &sid_text)) {
    return {};
  }
  std::wstring result(sid_text);
  LocalFree(sid_text);
  return result;
}

std::wstring TokenUserSidString(HANDLE token) {
  std::vector<uint8_t> buffer;
  if (!QueryTokenInformationBuffer(token, TokenUser, buffer)) {
    return {};
  }
  auto* user = reinterpret_cast<TOKEN_USER*>(buffer.data());
  return SidToString(user->User.Sid);
}

std::wstring TokenLogonSidString(HANDLE token) {
  std::vector<uint8_t> buffer;
  if (!QueryTokenInformationBuffer(token, TokenGroups, buffer)) {
    return {};
  }
  auto* groups = reinterpret_cast<TOKEN_GROUPS*>(buffer.data());
  for (DWORD i = 0; i < groups->GroupCount; ++i) {
    const auto& group = groups->Groups[i];
    if ((group.Attributes & SE_GROUP_LOGON_ID) == SE_GROUP_LOGON_ID) {
      return SidToString(group.Sid);
    }
  }
  return {};
}

struct TokenIdentity {
  std::wstring user_sid;
  std::wstring logon_sid;

  bool valid() const { return !user_sid.empty() && !logon_sid.empty(); }
};

TokenIdentity QueryTokenIdentity(HANDLE token) {
  return {TokenUserSidString(token), TokenLogonSidString(token)};
}

TokenIdentity CurrentProcessIdentity() {
  if (ForceSidFailureForTesting()) {
    return {};
  }
  HANDLE raw_token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) {
    return {};
  }
  wil::unique_handle token(raw_token);
  return QueryTokenIdentity(token.get());
}

std::wstring CurrentUserSidString() { return CurrentProcessIdentity().user_sid; }

bool ProcessMatchesCurrentLogon(ULONG process_id) {
  const auto current = CurrentProcessIdentity();
  if (!current.valid()) {
    return false;
  }

  wil::unique_handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id));
  if (!process) {
    return false;
  }
  HANDLE raw_token = nullptr;
  if (!OpenProcessToken(process.get(), TOKEN_QUERY, &raw_token)) {
    return false;
  }
  wil::unique_handle token(raw_token);
  const auto peer = QueryTokenIdentity(token.get());
  return peer.valid() && peer.user_sid == current.user_sid && peer.logon_sid == current.logon_sid;
}

bool PipeClientMatchesCurrentLogon(HANDLE pipe) {
  ULONG process_id = 0;
  return GetNamedPipeClientProcessId(pipe, &process_id) && ProcessMatchesCurrentLogon(process_id);
}

bool PipeServerMatchesCurrentLogon(HANDLE pipe) {
  ULONG process_id = 0;
  return GetNamedPipeServerProcessId(pipe, &process_id) && ProcessMatchesCurrentLogon(process_id);
}

bool CurrentProcessTokenIsRestrictedForTesting() {
#ifdef NDEBUG
  return false;
#else
  HANDLE raw_token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) {
    return false;
  }
  wil::unique_handle token(raw_token);
  const bool restricted = IsTokenRestricted(token.get()) != FALSE;
  return restricted;
#endif
}

bool AllowsSidFallback() {
#ifdef NDEBUG
  return false;
#else
  return true;
#endif
}

bool ForceZeroBytePipeWriteForTesting() {
#ifdef NDEBUG
  return false;
#else
  wchar_t forced[2]{};
  return GetEnvironmentVariableW(L"AZOOKEY_TEST_FORCE_ZERO_BYTE_PIPE_WRITE",
                                 forced, 2) > 0;
#endif
}

enum class ConnectWaitResult {
  Connected,
  Stopped,
  Failed,
};

#ifndef NDEBUG
std::optional<std::chrono::milliseconds> DeadlineOverrideForTesting(const wchar_t* name) {
  constexpr DWORD kBufferChars = 16;
  wchar_t buffer[kBufferChars]{};
  const DWORD copied = GetEnvironmentVariableW(name, buffer, kBufferChars);
  if (copied == 0 || copied >= kBufferChars) {
    return std::nullopt;
  }
  wchar_t* end = nullptr;
  const unsigned long value = std::wcstoul(buffer, &end, 10);
  if (end == buffer || value == 0) {
    return std::nullopt;
  }
  return std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(value));
}
#endif

// Per-connection deadline budgets. Resolved once when a connection is accepted
// rather than per frame: the env overrides exist only for tests, and reading
// them on the I/O path would put a registry-backed lookup in the hot loop.
struct FrameDeadlineConfig {
  std::chrono::milliseconds soft{kFrameSoftDeadline};
  std::chrono::milliseconds read_hard{kFrameReadHardDeadline};
  std::chrono::milliseconds write_hard{kFrameWriteHardDeadline};

  static FrameDeadlineConfig Load() {
    FrameDeadlineConfig config;
#ifndef NDEBUG
    if (const auto soft = DeadlineOverrideForTesting(L"AZOOKEY_TEST_FRAME_SOFT_DEADLINE_MS")) {
      config.soft = *soft;
    }
    if (const auto hard = DeadlineOverrideForTesting(L"AZOOKEY_TEST_FRAME_HARD_DEADLINE_MS")) {
      config.read_hard = *hard;
      config.write_hard = *hard;
    }
#endif
    return config;
  }
};

// Budget for one frame. Deliberately monotonic (steady_clock, not wall time) so
// a system clock adjustment can neither cut a healthy connection short nor keep
// a stalled one alive forever.
//
// A deadline starts once the frame is actually moving. The finest granularity
// available is a completed ReadFile/WriteFile chunk, so "moving" means the first
// chunk that transferred at least one byte -- pending overlapped I/O gives no
// visibility below that. That is enough for the cases this guards against: a
// peer that sends a header and then goes silent, one that sends a partial
// header, and one that trickles a body, since the budget is absolute over the
// whole frame rather than reset per chunk.
class FrameDeadline {
 public:
  FrameDeadline(std::chrono::milliseconds soft, std::chrono::milliseconds hard, bool armed)
      : soft_(soft), hard_(hard) {
    if (armed) {
      Arm();
    }
  }

  void Arm() {
    if (armed_) return;
    armed_ = true;
    start_ = std::chrono::steady_clock::now();
  }

  // INFINITE while unarmed: an idle connection may wait indefinitely for the
  // peer's next request.
  DWORD RemainingHardMs() const {
    if (!armed_) return INFINITE;
    const auto elapsed = Elapsed();
    if (elapsed >= hard_) return 0;
    return static_cast<DWORD>((hard_ - elapsed).count());
  }

  bool HardExpired() const { return armed_ && Elapsed() >= hard_; }
  bool SoftExceeded() const { return armed_ && Elapsed() >= soft_; }

 private:
  std::chrono::milliseconds Elapsed() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                 start_);
  }

  std::chrono::milliseconds soft_;
  std::chrono::milliseconds hard_;
  bool armed_{false};
  std::chrono::steady_clock::time_point start_{};
};

// Everything the framing helpers need about the connection they run on.
// `stop_event == nullptr` selects the synchronous path used by NamedPipeClient,
// whose handle is opened without FILE_FLAG_OVERLAPPED.
struct TransportContext {
  HANDLE stop_event{nullptr};
  FrameDeadlineConfig deadlines;
  std::atomic<uint64_t>* soft_deadline_counter{nullptr};
};

struct FrameIo {
  HANDLE stop_event{nullptr};
  FrameDeadline deadline;
};

HANDLE PrepareReusableOverlappedEvent() {
  thread_local wil::unique_event event;
  if (!event) {
    event.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  }
  if (!event) {
    return nullptr;
  }
  if (!ResetEvent(event.get())) {
    return nullptr;
  }
  return event.get();
}

// Every path out of a pending overlapped operation must reach here first. The
// OVERLAPPED lives on the caller's stack and the event is a thread-local reused
// across operations, so returning while the kernel still owns them would let a
// late completion write into a dead frame and signal the next operation's wait.
void CancelAndDrainOverlapped(HANDLE pipe, OVERLAPPED& overlapped, DWORD& transferred) {
  CancelIoEx(pipe, &overlapped);
  GetOverlappedResult(pipe, &overlapped, &transferred, TRUE);
}

PipeIoResult FinishOverlappedIo(HANDLE pipe, OVERLAPPED& overlapped, HANDLE stop_event,
                                DWORD timeout_ms) {
  HANDLE handles[2] = {overlapped.hEvent, stop_event};
  const DWORD handle_count = stop_event ? 2 : 1;
  const DWORD wait = WaitForMultipleObjects(handle_count, handles, FALSE, timeout_ms);
  if (wait == WAIT_OBJECT_0) {
    PipeIoResult result;
    result.ok = GetOverlappedResult(pipe, &overlapped, &result.transferred, FALSE);
    result.error = result.ok ? ERROR_SUCCESS : GetLastError();
    return result;
  }

  PipeIoResult result;
  if (handle_count == 2 && wait == WAIT_OBJECT_0 + 1) {
    result.stopped = true;
    result.error = ERROR_OPERATION_ABORTED;
  } else if (wait == WAIT_TIMEOUT) {
    result.timed_out = true;
    result.error = ERROR_TIMEOUT;
  } else {
    // Capture the wait failure before CancelIoEx overwrites the thread error.
    result.error = GetLastError();
  }
  // The drain can observe a completion that raced the deadline. The frame is
  // abandoned regardless: once the budget is spent, one more chunk cannot make
  // the frame whole, and a cancelled message-mode read leaves unread remainder
  // in the pipe that no amount of resynchronisation recovers.
  CancelAndDrainOverlapped(pipe, overlapped, result.transferred);
  return result;
}

PipeIoResult ReadPipeChunk(HANDLE pipe, uint8_t* data, DWORD size, const FrameIo& io) {
  if (!io.stop_event) {
    PipeIoResult result;
    result.ok = ReadFile(pipe, data, size, &result.transferred, nullptr);
    result.error = result.ok ? ERROR_SUCCESS : GetLastError();
    return result;
  }

  HANDLE event = PrepareReusableOverlappedEvent();
  if (!event) {
    PipeIoResult result;
    result.error = GetLastError();
    return result;
  }
  OVERLAPPED overlapped{};
  overlapped.hEvent = event;

  PipeIoResult result;
  result.ok = ReadFile(pipe, data, size, nullptr, &overlapped);
  result.error = result.ok ? ERROR_SUCCESS : GetLastError();
  if (!result.ok && result.error == ERROR_IO_PENDING) {
    return FinishOverlappedIo(pipe, overlapped, io.stop_event, io.deadline.RemainingHardMs());
  }
  CaptureImmediateOverlappedTransfer(pipe, overlapped, result);
  return result;
}

PipeIoResult WritePipeChunk(HANDLE pipe, const uint8_t* data, DWORD size, const FrameIo& io) {
  if (ForceZeroBytePipeWriteForTesting()) {
    PipeIoResult result;
    result.ok = TRUE;
    return result;
  }
  if (!io.stop_event) {
    PipeIoResult result;
    result.ok = WriteFile(pipe, data, size, &result.transferred, nullptr);
    result.error = result.ok ? ERROR_SUCCESS : GetLastError();
    return result;
  }

  HANDLE event = PrepareReusableOverlappedEvent();
  if (!event) {
    PipeIoResult result;
    result.error = GetLastError();
    return result;
  }
  OVERLAPPED overlapped{};
  overlapped.hEvent = event;

  PipeIoResult result;
  result.ok = WriteFile(pipe, data, size, nullptr, &overlapped);
  result.error = result.ok ? ERROR_SUCCESS : GetLastError();
  if (!result.ok && result.error == ERROR_IO_PENDING) {
    return FinishOverlappedIo(pipe, overlapped, io.stop_event, io.deadline.RemainingHardMs());
  }
  CaptureImmediateOverlappedTransfer(pipe, overlapped, result);
  return result;
}

ConnectWaitResult WaitForClientConnect(HANDLE pipe, HANDLE stop_event) {
  wil::unique_event event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  if (!event) {
    return ConnectWaitResult::Failed;
  }

  OVERLAPPED overlapped{};
  overlapped.hEvent = event.get();
  const BOOL ok = ConnectNamedPipe(pipe, &overlapped);
  if (ok) {
    return ConnectWaitResult::Connected;
  }

  switch (GetLastError()) {
    case ERROR_IO_PENDING:
      break;
    case ERROR_PIPE_CONNECTED:
      return ConnectWaitResult::Connected;
    default:
      return ConnectWaitResult::Failed;
  }

  HANDLE handles[2] = {event.get(), stop_event};
  const DWORD handle_count = stop_event ? 2 : 1;
  const DWORD wait = WaitForMultipleObjects(handle_count, handles, FALSE, INFINITE);
  if (wait == WAIT_OBJECT_0) {
    DWORD transferred = 0;
    return GetOverlappedResult(pipe, &overlapped, &transferred, FALSE)
               ? ConnectWaitResult::Connected
               : ConnectWaitResult::Failed;
  }
  if (handle_count == 2 && wait == WAIT_OBJECT_0 + 1) {
    CancelIoEx(pipe, &overlapped);
    DWORD transferred = 0;
    GetOverlappedResult(pipe, &overlapped, &transferred, TRUE);
    return ConnectWaitResult::Stopped;
  }
  return ConnectWaitResult::Failed;
}

struct SecurityDescriptor {
  PSECURITY_DESCRIPTOR descriptor{nullptr};

  SecurityDescriptor() = default;
  SecurityDescriptor(const SecurityDescriptor&) = delete;
  SecurityDescriptor& operator=(const SecurityDescriptor&) = delete;
  SecurityDescriptor(SecurityDescriptor&& other) noexcept : descriptor(other.descriptor) {
    other.descriptor = nullptr;
  }
  SecurityDescriptor& operator=(SecurityDescriptor&& other) noexcept {
    if (this != &other) {
      if (descriptor) {
        LocalFree(descriptor);
      }
      descriptor = other.descriptor;
      other.descriptor = nullptr;
    }
    return *this;
  }

  ~SecurityDescriptor() {
    if (descriptor) {
      LocalFree(descriptor);
    }
  }
};

SecurityDescriptor BuildCurrentLogonSecurityDescriptor() {
  SecurityDescriptor result;
  const auto identity = CurrentProcessIdentity();
  if (!identity.valid()) return result;

  // A logon SID narrows the pipe from "same account" to the current logon
  // session. The rights exclude GENERIC_ALL and its ACL/owner mutation rights.
  bool restricted_token = false;
#ifndef NDEBUG
  // Restricted-token test runners require an ACE that also matches a
  // restricting SID. Keep this out of Release, where the pipe fails closed.
  restricted_token = CurrentProcessTokenIsRestrictedForTesting();
#endif
  const auto sddl = internal::BuildPipeSecuritySddl(identity.logon_sid, restricted_token);
  ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                       &result.descriptor, nullptr);
  return result;
}

HANDLE CreatePipeInstance(const std::string& pipe_name, bool first_instance) {
  const auto wide_name = Utf8ToWide(pipe_name);
  if (wide_name.empty()) return INVALID_HANDLE_VALUE;

  // Build per-logon DACL. Debug/test builds may fall back to process-default
  // security in restricted environments; Release fails closed.
  auto security = BuildCurrentLogonSecurityDescriptor();
  if (!security.descriptor && !AllowsSidFallback()) {
    return INVALID_HANDLE_VALUE;
  }

  SECURITY_ATTRIBUTES attrs;
  attrs.nLength = sizeof(attrs);
  attrs.lpSecurityDescriptor = security.descriptor;  // nullptr = default security
  attrs.bInheritHandle = FALSE;

  DWORD open_mode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED;
  if (first_instance) {
    open_mode |= FILE_FLAG_FIRST_PIPE_INSTANCE;
  }
  return CreateNamedPipeW(
      wide_name.c_str(), open_mode,
      PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
      kMaxPipeInstances, kPipeBufferSize, kPipeBufferSize, 0, &attrs);
}

bool ReadBytes(HANDLE pipe, uint8_t* data, size_t size, FrameIo& io) {
  size_t offset = 0;
  size_t transient_no_data_retries = 0;
  while (offset < size) {
    // Chunks that complete synchronously never reach FinishOverlappedIo, and the
    // retry paths below loop back here, so the budget is re-checked every pass.
    if (io.deadline.HardExpired()) {
      return false;
    }
    const DWORD chunk =
        static_cast<DWORD>(std::min<size_t>(size - offset, static_cast<size_t>(kPipeBufferSize)));
    const auto result = ReadPipeChunk(pipe, data + offset, chunk, io);
    if (result.transferred > 0) {
      // The frame is in flight now; the rest of it is on the clock.
      io.deadline.Arm();
    }
    if (!result.ok) {
      if (result.stopped || result.timed_out) return false;
      const auto err = result.error;
      if (err == ERROR_NO_DATA) {
        if (offset == 0 || ++transient_no_data_retries > kMaxTransientReadNoDataRetries) {
          return false;
        }
        Sleep(1);
        continue;
      }
      if (err == ERROR_MORE_DATA && result.transferred > 0) {
        offset += result.transferred;
        transient_no_data_retries = 0;
        continue;
      }
      return false;
    }
    if (result.transferred == 0) {
      return false;
    }
    offset += result.transferred;
    transient_no_data_retries = 0;
  }
  return true;
}

bool WriteBytes(HANDLE pipe, const uint8_t* data, size_t size, FrameIo& io) {
  size_t offset = 0;
  size_t no_progress_retries = 0;
  while (offset < size) {
    if (io.deadline.HardExpired()) {
      return false;
    }
    const DWORD chunk =
        static_cast<DWORD>(std::min<size_t>(size - offset, static_cast<size_t>(kPipeBufferSize)));
    const auto result = WritePipeChunk(pipe, data + offset, chunk, io);
    if (!result.ok) {
      if (result.stopped || result.timed_out) return false;
      const auto err = result.error;
      if (err == ERROR_PIPE_BUSY) {
        if (++no_progress_retries > kMaxTransientWriteNoProgressRetries) {
          return false;
        }
        Sleep(1);
        continue;
      }
      return false;
    }
    if (result.transferred == 0) {
      if (++no_progress_retries > kMaxTransientWriteNoProgressRetries) {
        return false;
      }
      Sleep(1);
      continue;
    }
    offset += result.transferred;
    no_progress_retries = 0;
  }
  // FlushFileBuffers on a named pipe can block until the peer drains all data;
  // completed WriteFile chunks are enough for this framed message transport.
  return true;
}

void NoteSoftDeadline(const TransportContext& ctx, const FrameDeadline& deadline) {
  // Observation only: the frame is not dropped. M41 wires this to the
  // structured log; until then it stays a counter so the threshold has an
  // observable effect and can be regression-tested.
  if (ctx.soft_deadline_counter && deadline.SoftExceeded()) {
    ctx.soft_deadline_counter->fetch_add(1, std::memory_order_relaxed);
  }
}

std::optional<Envelope> ReadFramedEnvelope(HANDLE pipe, FrameIo& io) {
  uint8_t header[4]{};
  if (!ReadBytes(pipe, header, sizeof(header), io)) {
    return std::nullopt;
  }

  const uint32_t size = static_cast<uint32_t>(header[0]) | (static_cast<uint32_t>(header[1]) << 8) |
                        (static_cast<uint32_t>(header[2]) << 16) |
                        (static_cast<uint32_t>(header[3]) << 24);
  if (size == 0 || size > kMaxFrameSize) {
    return std::nullopt;
  }

  std::vector<uint8_t> payload(size);
  if (!ReadBytes(pipe, payload.data(), payload.size(), io)) {
    return std::nullopt;
  }

  const std::string json(payload.begin(), payload.end());
  return Deserialize(json);
}

std::optional<Envelope> ReadEnvelope(HANDLE pipe, const TransportContext& ctx) {
  // One budget spans header and body: it stays unarmed while the connection is
  // idle and starts as soon as the peer sends the first byte of the header, so
  // a peer that announces a frame and then goes silent is bounded.
  FrameIo io{ctx.stop_event,
             FrameDeadline(ctx.deadlines.soft, ctx.deadlines.read_hard, /*armed=*/false)};
  auto envelope = ReadFramedEnvelope(pipe, io);
  NoteSoftDeadline(ctx, io.deadline);
  return envelope;
}

bool WriteEnvelope(HANDLE pipe, const Envelope& envelope, const TransportContext& ctx) {
  const auto json = Serialize(envelope);
  if (!json) return false;
  const auto frame = EncodeLengthPrefixed(*json);
  if (!frame) return false;
  // Sending has no idle phase, so the budget is armed from the first chunk.
  FrameIo io{ctx.stop_event,
             FrameDeadline(ctx.deadlines.soft, ctx.deadlines.write_hard, /*armed=*/true)};
  const bool ok = WriteBytes(pipe, frame->data(), frame->size(), io);
  NoteSoftDeadline(ctx, io.deadline);
  return ok;
}

struct ClientState {
  explicit ClientState(wil::unique_hfile handle) : pipe(std::move(handle)) {}

  std::mutex mutex;
  wil::unique_hfile pipe;
  bool closed{false};
};

void CloseClientPipe(const std::shared_ptr<ClientState>& client) {
  std::lock_guard<std::mutex> lock(client->mutex);
  if (!client->closed && client->pipe) {
    CancelIoEx(client->pipe.get(), nullptr);
    client->pipe.reset();
    client->closed = true;
  }
}

void CancelClientPipeIo(const std::shared_ptr<ClientState>& client) {
  std::lock_guard<std::mutex> lock(client->mutex);
  if (!client->closed && client->pipe) {
    CancelIoEx(client->pipe.get(), nullptr);
  }
}

}  // namespace

struct NamedPipeServer::Impl {
  std::atomic<bool> running{false};
  std::string pipe_name;
  ConnectionFactory conn_factory;
  std::thread accept_thread;
  std::condition_variable client_cv;
  std::size_t active_client_threads{0};

  std::atomic<uint64_t> soft_deadline_exceeded{0};

  mutable std::mutex mutex;
  wil::unique_hfile listen_pipe;
  wil::unique_event stop_event;
  std::vector<std::shared_ptr<ClientState>> clients;

  void AcceptLoop() {
    while (running.load()) {
      HANDLE current = INVALID_HANDLE_VALUE;
      {
        std::lock_guard<std::mutex> lock(mutex);
        current = listen_pipe.get();
      }
      if (current == INVALID_HANDLE_VALUE) {
        running.store(false);
        break;
      }

      const auto connect_result = WaitForClientConnect(current, stop_event.get());
      const bool connected =
          connect_result == ConnectWaitResult::Connected && PipeClientMatchesCurrentLogon(current);
      if (connect_result == ConnectWaitResult::Connected && !connected) {
        DisconnectNamedPipe(current);
      }

      wil::unique_hfile current_pipe;
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (listen_pipe.get() == current) {
          current_pipe = std::move(listen_pipe);
        }
      }

      if (!running.load() || connect_result == ConnectWaitResult::Stopped) {
        break;
      }

      if (connected) {
        DWORD mode = PIPE_READMODE_MESSAGE | PIPE_WAIT;
        if (!SetNamedPipeHandleState(current, &mode, nullptr, nullptr)) {
          // Keep the connection: ReadBytes/WriteBytes still validate the
          // length-prefixed frame and fail closed on unrecoverable I/O errors.
        }

        // Build a per-connection handler so each client has isolated state
        // (e.g. independent authentication flags). Guard against factory
        // exceptions (e.g. OOM during Dispatcher construction) so a single
        // bad connection does not bring down the whole server via std::terminate.
        MessageHandler conn_handler;
        bool handler_ok = true;
        try {
          conn_handler = conn_factory ? conn_factory() : MessageHandler{};
        } catch (...) {
          handler_ok = false;
        }
        if (!handler_ok) {
          DisconnectNamedPipe(current);
        } else {
          auto client = std::make_shared<ClientState>(std::move(current_pipe));
          {
            std::lock_guard<std::mutex> lock(mutex);
            clients.push_back(client);
            ++active_client_threads;
          }
          try {
            std::thread([this, client, conn_handler = std::move(conn_handler)]() mutable {
              ClientLoop(client, std::move(conn_handler));
            }).detach();
          } catch (...) {
            CloseClientPipe(client);
            {
              std::lock_guard<std::mutex> lock(mutex);
              clients.erase(std::remove(clients.begin(), clients.end(), client), clients.end());
              if (active_client_threads > 0) {
                --active_client_threads;
              }
            }
            client_cv.notify_all();
          }
        }
      } else {
        if (connect_result == ConnectWaitResult::Failed) {
          std::this_thread::sleep_for(kFailedAcceptBackoff);
        }
      }

      {
        std::unique_lock<std::mutex> lock(mutex);
        client_cv.wait(lock, [this] {
          return !running.load() || clients.size() < kMaxPipeInstances;
        });
        if (!running.load()) {
          break;
        }
      }

      wil::unique_hfile next_pipe(CreatePipeInstance(pipe_name, false));
      if (!next_pipe) {
        running.store(false);
        break;
      }

      {
        std::lock_guard<std::mutex> lock(mutex);
        if (!running.load()) {
          break;
        }
        listen_pipe = std::move(next_pipe);
      }
    }
  }

  void ClientLoop(std::shared_ptr<ClientState> client, MessageHandler conn_handler) {
    const TransportContext ctx{stop_event.get(), FrameDeadlineConfig::Load(),
                               &soft_deadline_exceeded};
    while (running.load()) {
      HANDLE pipe = INVALID_HANDLE_VALUE;
      {
        std::lock_guard<std::mutex> lock(client->mutex);
        if (client->closed) break;
        pipe = client->pipe.get();
      }

      auto request = ReadEnvelope(pipe, ctx);
      if (!request) break;

      std::optional<Envelope> response;
      try {
        response = conn_handler ? conn_handler(*request) : std::nullopt;
      } catch (...) {
        break;
      }

      if (response && !WriteEnvelope(pipe, *response, ctx)) {
        break;
      }
    }

    HANDLE pipe = INVALID_HANDLE_VALUE;
    {
      std::lock_guard<std::mutex> lock(client->mutex);
      pipe = client->pipe.get();
    }
    if (pipe != INVALID_HANDLE_VALUE) {
      DisconnectNamedPipe(pipe);
    }
    CloseClientPipe(client);

    {
      std::lock_guard<std::mutex> lock(mutex);
      clients.erase(std::remove(clients.begin(), clients.end(), client), clients.end());
      if (active_client_threads > 0) {
        --active_client_threads;
      }
    }
    client_cv.notify_all();
  }
};

struct NamedPipeClient::Impl {
  std::mutex mutex;
  wil::unique_hfile pipe;
  bool connected{false};
};

NamedPipeServer::NamedPipeServer() : impl_(std::make_unique<Impl>()) {}
NamedPipeServer::~NamedPipeServer() { Stop(); }

bool NamedPipeServer::Start(const std::string& pipe_name, MessageHandler handler) {
  if (!handler) return false;
  // Wrap in shared_ptr so all connections invoke the same handler object,
  // preserving any shared mutable state the caller may hold inside it.
  auto shared = std::make_shared<MessageHandler>(std::move(handler));
  return Start(pipe_name, [shared]() -> MessageHandler {
    return [shared](const Envelope& env) -> std::optional<Envelope> { return (*shared)(env); };
  });
}

bool NamedPipeServer::Start(const std::string& pipe_name, ConnectionFactory factory) {
  if (pipe_name.empty() || !factory) {
    return false;
  }

  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->running.load()) {
    return false;
  }

  if (!impl_->stop_event) {
    impl_->stop_event.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!impl_->stop_event) {
      return false;
    }
  }
  ResetEvent(impl_->stop_event.get());

  wil::unique_hfile first_pipe(CreatePipeInstance(pipe_name, true));
  if (!first_pipe) {
    return false;
  }

  impl_->pipe_name = pipe_name;
  impl_->conn_factory = std::move(factory);
  impl_->listen_pipe = std::move(first_pipe);
  impl_->running.store(true);
  impl_->accept_thread = std::thread([this]() { impl_->AcceptLoop(); });
  return true;
}

void NamedPipeServer::Stop() {
  std::vector<std::shared_ptr<ClientState>> clients;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->running.load() && !impl_->accept_thread.joinable()) {
      return;
    }
    impl_->running.store(false);
    if (impl_->stop_event) {
      SetEvent(impl_->stop_event.get());
    }
    impl_->client_cv.notify_all();
    clients = impl_->clients;
  }

  for (const auto& client : clients) {
    CancelClientPipeIo(client);
  }

  if (impl_->accept_thread.joinable()) {
    impl_->accept_thread.join();
  }

  {
    std::unique_lock<std::mutex> lock(impl_->mutex);
    impl_->client_cv.wait(lock, [this] { return impl_->active_client_threads == 0; });
    impl_->listen_pipe.reset();
    clients = impl_->clients;
    impl_->clients.clear();
  }

  for (const auto& client : clients) {
    CloseClientPipe(client);
  }
}

bool NamedPipeServer::IsRunning() const { return impl_->running.load(); }

std::size_t NamedPipeServer::ActiveClientCountForTesting() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->clients.size();
}

std::uint64_t NamedPipeServer::SoftDeadlineExceededCount() const {
  return impl_->soft_deadline_exceeded.load(std::memory_order_relaxed);
}

NamedPipeClient::NamedPipeClient() : impl_(std::make_unique<Impl>()) {}
NamedPipeClient::~NamedPipeClient() { Disconnect(); }

bool NamedPipeClient::Connect(const std::string& pipe_name, uint32_t timeout_ms) {
  Disconnect();
  const auto wide_name = Utf8ToWide(pipe_name);
  if (wide_name.empty()) {
    return false;
  }

  const auto start = std::chrono::steady_clock::now();
  while (true) {
    wil::unique_hfile pipe(internal::OpenPipeClientHandle(wide_name));
    if (pipe) {
      if (!PipeServerMatchesCurrentLogon(pipe.get())) {
        return false;
      }
      DWORD mode = PIPE_READMODE_MESSAGE;
      if (!SetNamedPipeHandleState(pipe.get(), &mode, nullptr, nullptr)) {
        return false;
      }
      std::lock_guard<std::mutex> lock(impl_->mutex);
      impl_->pipe = std::move(pipe);
      impl_->connected = true;
      return true;
    }

    const auto err = GetLastError();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    if (elapsed.count() >= timeout_ms) {
      return false;
    }

    const auto remaining = static_cast<DWORD>(timeout_ms - elapsed.count());
    if (err == ERROR_PIPE_BUSY) {
      WaitNamedPipeW(wide_name.c_str(), remaining);
    } else if (err == ERROR_FILE_NOT_FOUND) {
      Sleep(std::min<DWORD>(25, remaining));
    } else {
      return false;
    }
  }
}

void NamedPipeClient::Disconnect() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->pipe.reset();
  impl_->connected = false;
}

bool NamedPipeClient::IsConnected() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->connected;
}

bool NamedPipeClient::Send(const Envelope& envelope) {
  HANDLE pipe = INVALID_HANDLE_VALUE;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->connected) return false;
    pipe = impl_->pipe.get();
  }

  if (!WriteEnvelope(pipe, envelope, TransportContext{})) {
    Disconnect();
    return false;
  }
  return true;
}

std::optional<Envelope> NamedPipeClient::Receive() {
  HANDLE pipe = INVALID_HANDLE_VALUE;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->connected) return std::nullopt;
    pipe = impl_->pipe.get();
  }

  auto envelope = ReadEnvelope(pipe, TransportContext{});
  if (!envelope) {
    Disconnect();
  }
  return envelope;
}

std::optional<Envelope> NamedPipeClient::ReceiveWithTimeout(uint32_t timeout_ms) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    {
      std::lock_guard<std::mutex> lock(impl_->mutex);
      if (!impl_->connected) return std::nullopt;
      pipe = impl_->pipe.get();
    }
    DWORD avail = 0;
    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr)) {
      Disconnect();
      return std::nullopt;
    }
    if (avail > 0) {
      return Receive();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return std::nullopt;
}

std::string DefaultPipeName() {
  const auto sid = CurrentUserSidString();
  if (sid.empty()) {
    return AllowsSidFallback() ? "\\\\.\\pipe\\azookey-default" : std::string();
  }
  return "\\\\.\\pipe\\azookey-" + WideToUtf8(sid);
}

#else  // !_WIN32

struct NamedPipeServer::Impl {};
struct NamedPipeClient::Impl {};

NamedPipeServer::NamedPipeServer() : impl_(std::make_unique<Impl>()) {}
NamedPipeServer::~NamedPipeServer() = default;
bool NamedPipeServer::Start(const std::string&, MessageHandler) { return false; }
bool NamedPipeServer::Start(const std::string&, ConnectionFactory) { return false; }
void NamedPipeServer::Stop() {}
bool NamedPipeServer::IsRunning() const { return false; }
std::size_t NamedPipeServer::ActiveClientCountForTesting() const { return 0; }
std::uint64_t NamedPipeServer::SoftDeadlineExceededCount() const { return 0; }

NamedPipeClient::NamedPipeClient() : impl_(std::make_unique<Impl>()) {}
NamedPipeClient::~NamedPipeClient() = default;
bool NamedPipeClient::Connect(const std::string&, uint32_t) { return false; }
void NamedPipeClient::Disconnect() {}
bool NamedPipeClient::IsConnected() const { return false; }
bool NamedPipeClient::Send(const Envelope&) { return false; }
std::optional<Envelope> NamedPipeClient::Receive() { return std::nullopt; }
std::optional<Envelope> NamedPipeClient::ReceiveWithTimeout(uint32_t) { return std::nullopt; }

std::string DefaultPipeName() {
  return "/tmp/azookey-namedpipe-unsupported";
}

#endif

}  // namespace azookey::ipc
