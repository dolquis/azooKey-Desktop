#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace azookey::tsf {

// Primary-connection state of the TIP IPC worker (docs/dev-infrastructure-spec.md
// §8.2). Only the worker's primary pipe drives it; the short-lived control
// connection used for out-of-band Cancel is not part of this machine.
enum class IpcConnectionState {
  Disconnected,
  Connecting,
  Handshaking,
  Ready,
  Degraded,
};

enum class IpcConnectionEvent {
  // A connect attempt starts.
  ConnectStarted,
  // The pipe opened; the handshake is about to be sent.
  PipeConnected,
  // The pipe could not be opened within the connect timeout.
  ConnectFailed,
  // The Host accepted the handshake.
  HandshakeAccepted,
  // The handshake failed to send, timed out, or was rejected.
  HandshakeFailed,
  // An established connection dropped (send/receive failure, refused refresh).
  ConnectionLost,
  // The Host stayed silent past the hard-timeout threshold while the pipe
  // remained open (connected-but-silent).
  ResponseDeadlineExceeded,
  // The Host answered again after being marked degraded.
  ResponseRestored,
  // Deactivate stopped the worker.
  Stopped,
};

struct IpcConnectionTransition {
  IpcConnectionState from;
  IpcConnectionEvent event;
  IpcConnectionState to;
};

// Every legal transition. An event that is not listed for the current state is
// rejected by NextIpcConnectionState.
std::span<const IpcConnectionTransition> IpcConnectionTransitionTable();

std::optional<IpcConnectionState> NextIpcConnectionState(IpcConnectionState from,
                                                         IpcConnectionEvent event);

// Stable wire names for the structured log. Fixed vocabulary only, so a
// transition record can never carry input text.
std::string_view IpcConnectionStateName(IpcConnectionState state);
std::string_view IpcConnectionEventName(IpcConnectionEvent event);

// While the Host is down (or keeps refusing the handshake) the worker cycles
// through Disconnected, Connecting and Handshaking on every backoff tick. Those
// transitions are logged only when `attempt` (1-based connect attempt since the
// last Ready) is 1 or a power of two, so a long outage produces a logarithmic
// number of records and every transition of one attempt is judged by the same
// number. Transitions into or out of Ready / Degraded are always logged.
bool ShouldLogIpcConnectionTransition(IpcConnectionState from, IpcConnectionState to,
                                      uint32_t attempt);

}  // namespace azookey::tsf
