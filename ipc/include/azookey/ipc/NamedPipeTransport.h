#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "azookey/ipc/Messages.h"

namespace azookey::ipc {

// Transport abstraction over a Windows Named Pipe.
//
// The header is platform-agnostic so the rest of the code base can refer to
// these types from any build. The actual transport is implemented under
// `#ifdef _WIN32`. On other platforms a no-op stub is compiled so unit tests
// of higher layers still link.
//
// Wire format:
//   - Each message is one Envelope serialized via ipc::Serialize
//   - Framed with EncodeLengthPrefixed (4-byte little-endian length prefix)
//   - Pipe is PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE on Windows.
//     Server and client handles use FILE_FLAG_OVERLAPPED while preserving
//     blocking wait mode for connected message I/O.
//
// Security (Windows):
//   - DACL is restricted to the current logon SID with scoped pipe rights
//   - Remote pipe clients are rejected; the transport is local-machine only
//   - The first server instance claims the name exclusively, and both ends
//     verify that the peer process belongs to the same user and logon
//   - Clients request SecurityIdentification so a squatting server cannot
//     impersonate the hosting application process
//   - Debug/test builds may add a restricted-token compatibility ACE; Release
//     remains current-logon-only and fails closed if SID-based DACL creation fails
//   - One server can accept multiple clients (TIP + settings UI)
//
// Frame deadlines (Windows):
//   - A connection waiting between requests is never on the clock, but once a
//     frame starts arriving the rest of it must land within a monotonic budget.
//     Expiry closes that one connection and leaves other clients untouched.
//   - Server and client handles use overlapped I/O so pending reads and writes
//     can be canceled at the hard deadline or when the connection is closed
//     (docs/dev-infrastructure-spec.md §6.4.7 and §6.4.8).

class NamedPipeServer {
 public:
  // Handler returns std::nullopt for fire-and-forget messages; otherwise the
  // returned Envelope is sent back to the originating client.
  using MessageHandler = std::function<std::optional<Envelope>(const Envelope&)>;

  // Factory called once per accepted client; returns a per-connection handler.
  // Use this overload when handler state (e.g. auth flags) must be isolated
  // across simultaneous clients.
  using ConnectionFactory = std::function<MessageHandler()>;

  NamedPipeServer();
  ~NamedPipeServer();

  NamedPipeServer(const NamedPipeServer&) = delete;
  NamedPipeServer& operator=(const NamedPipeServer&) = delete;

  // Start listening on `pipe_name` (e.g. "\\\\.\\pipe\\azookey-<sid>").
  // handler is shared across all client connections (backward-compatible form).
  bool Start(const std::string& pipe_name, MessageHandler handler);

  // Per-connection factory variant: factory() is called once per accepted
  // client and the returned handler is used exclusively for that connection.
  bool Start(const std::string& pipe_name, ConnectionFactory factory);

  // Idempotent. After return, no more handler callbacks will fire.
  void Stop();

  bool IsRunning() const;
  std::size_t ActiveClientCountForTesting() const;

  // Number of frames on this server whose soft deadline was exceeded. Soft
  // exceedance never drops a frame; it only marks a connection as slow. M41
  // will surface this through the structured log.
  std::uint64_t SoftDeadlineExceededCount() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class NamedPipeClient {
 public:
  NamedPipeClient();
  ~NamedPipeClient();

  NamedPipeClient(const NamedPipeClient&) = delete;
  NamedPipeClient& operator=(const NamedPipeClient&) = delete;

  // Connect to the server's pipe. Blocks up to `timeout_ms` waiting for the
  // pipe to become available (WaitNamedPipe). Returns false on timeout or
  // platform error.
  bool Connect(const std::string& pipe_name, uint32_t timeout_ms = 5000);
  void Disconnect();
  bool IsConnected() const;

  bool Send(const Envelope& envelope);
  std::optional<Envelope> Receive();
  // Wait up to `timeout_ms` for the complete frame. An idle timeout preserves
  // the connection and unread response; a timeout after the frame starts drops
  // the connection because the framing cannot be resumed safely.
  std::optional<Envelope> ReceiveWithTimeout(uint32_t timeout_ms);

  // Poll phase A for `poll_timeout_ms`, then bound an in-progress frame by the
  // earlier of `request_deadline` and the transport read hard deadline. Passing
  // no request deadline is intended for batch waits that have no request-level
  // wall-clock bound.
  std::optional<Envelope> ReceiveWithTimeout(
      uint32_t poll_timeout_ms,
      std::optional<std::chrono::steady_clock::time_point> request_deadline);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Build a per-user pipe name. On Windows derives `\\.\pipe\azookey-<sid>`
// from the current process token. On other platforms returns a stable
// placeholder so build/test code in mixed environments still compiles.
std::string DefaultPipeName();

}  // namespace azookey::ipc
