#include "azookey/host/CliHostConnect.h"

#include <chrono>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace azookey::host {
namespace {

#ifdef _WIN32
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
  const auto start = std::chrono::steady_clock::now();
  if (client.Connect(pipe_name, connect_timeout_ms)) return true;
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();
  // Connect retries a busy pipe on its own, so one more call with the rest of
  // the budget is enough; any other failure returns from it immediately.
  if (elapsed < busy_timeout_ms && HostPipeIsBusy(pipe_name))
    return client.Connect(pipe_name, static_cast<uint32_t>(busy_timeout_ms - elapsed));
  return false;
#else
  (void)busy_timeout_ms;
  return client.Connect(pipe_name, connect_timeout_ms);
#endif
}

}  // namespace azookey::host
