#pragma once

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
//     Server instances use FILE_FLAG_OVERLAPPED for cancelable accept while
//     preserving blocking wait mode for connected message I/O.
//
// Security (Windows):
//   - DACL is restricted to the current user's SID (RW only)
//   - Remote pipe clients are rejected; the transport is local-machine only
//   - Debug/test builds may add a restricted-token compatibility ACE; Release
//     remains current-user-only and fails closed if SID-based DACL creation fails
//   - One server can accept multiple clients (TIP + settings UI)

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
  // Poll for an inbound envelope for up to `timeout_ms` milliseconds.
  // Returns std::nullopt on timeout (caller should retry) or on pipe error.
  // Designed for use inside a loop so callers can drain the send queue between
  // polls without blocking indefinitely while the host processes a long query.
  std::optional<Envelope> ReceiveWithTimeout(uint32_t timeout_ms);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Build a per-user pipe name. On Windows derives `\\.\pipe\azookey-<sid>`
// from the current process token. On other platforms returns a stable
// placeholder so build/test code in mixed environments still compiles.
std::string DefaultPipeName();

}  // namespace azookey::ipc
