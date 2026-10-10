#include "azookey/host/CliHostConnect.h"

#include <algorithm>
#include <chrono>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace azookey::host {
namespace {

#ifdef _WIN32
constexpr uint32_t kBusySliceMs = 100;

// True when the pipe has instances but none is free (or one has just been
// freed). No instance at all means no Host, which must keep failing fast.
bool HostPipeIsBusy(const std::string& pipe_name) {
  const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, pipe_name.data(),
                                         static_cast<int>(pipe_name.size()), nullptr, 0);
  if (length <= 0) return false;
  std::wstring wide(static_cast<size_t>(length), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, pipe_name.data(),
                          static_cast<int>(pipe_name.size()), wide.data(), length) != length)
    return false;
  if (WaitNamedPipeW(wide.c_str(), 1)) return true;
  return GetLastError() == ERROR_SEM_TIMEOUT;
}
#endif

}  // namespace

bool ConnectToRunningHost(ipc::NamedPipeClient& client, const std::string& pipe_name,
                          uint32_t connect_timeout_ms, uint32_t busy_timeout_ms) {
#ifdef _WIN32
  using Clock = std::chrono::steady_clock;
  const auto start = Clock::now();
  if (client.Connect(pipe_name, connect_timeout_ms)) return true;
  // Wait in short slices and look at the pipe between them, so a Host that
  // exits while the CLI waits still fails within one slice rather than after
  // the whole budget (Connect keeps retrying a missing pipe until its timeout).
  for (;;) {
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
    if (elapsed >= busy_timeout_ms || !HostPipeIsBusy(pipe_name)) return false;
    const auto slice = (std::min)(kBusySliceMs, static_cast<uint32_t>(busy_timeout_ms - elapsed));
    const auto slice_start = Clock::now();
    if (client.Connect(pipe_name, slice)) return true;
    // A busy or missing pipe uses up the slice; an early return is a failure
    // that retrying cannot fix (another logon's server, a handle error).
    if (Clock::now() - slice_start < std::chrono::milliseconds(slice)) return false;
  }
#else
  (void)busy_timeout_ms;
  return client.Connect(pipe_name, connect_timeout_ms);
#endif
}

}  // namespace azookey::host
