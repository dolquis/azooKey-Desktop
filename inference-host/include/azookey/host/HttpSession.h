#pragma once
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

namespace azookey::host {
// Shared proxy/TLS policy for downloads and AI requests. TLS certificate validation
// remains WinHTTP's default. Only literal loopback addresses bypass system proxy.
inline HINTERNET OpenHttpSession(const wchar_t* agent, bool loopback, bool asynchronous,
                                 int connect_ms, int send_ms, int receive_ms) {
  const auto session = WinHttpOpen(
      agent, loopback ? WINHTTP_ACCESS_TYPE_NO_PROXY : WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, asynchronous ? WINHTTP_FLAG_ASYNC : 0);
  if (session && !WinHttpSetTimeouts(session, connect_ms, connect_ms, send_ms, receive_ms)) {
    WinHttpCloseHandle(session);
    return nullptr;
  }
  return session;
}
}  // namespace azookey::host
#endif
