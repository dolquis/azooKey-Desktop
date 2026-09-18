#include "azookey/tsf/IpcConnectionState.h"

#include <array>
#include <bit>

namespace azookey::tsf {
namespace {
using S = IpcConnectionState;
using E = IpcConnectionEvent;

// Self-loops are deliberately absent: a transition always changes the state,
// so every accepted event is worth a log record.
constexpr std::array<IpcConnectionTransition, 13> kTransitions{{
    {S::Disconnected, E::ConnectStarted, S::Connecting},
    {S::Connecting, E::PipeConnected, S::Handshaking},
    {S::Connecting, E::ConnectFailed, S::Disconnected},
    {S::Connecting, E::Stopped, S::Disconnected},
    {S::Handshaking, E::HandshakeAccepted, S::Ready},
    {S::Handshaking, E::HandshakeFailed, S::Disconnected},
    {S::Handshaking, E::Stopped, S::Disconnected},
    {S::Ready, E::ConnectionLost, S::Disconnected},
    {S::Ready, E::ResponseDeadlineExceeded, S::Degraded},
    {S::Ready, E::Stopped, S::Disconnected},
    {S::Degraded, E::ResponseRestored, S::Ready},
    {S::Degraded, E::ConnectionLost, S::Disconnected},
    {S::Degraded, E::Stopped, S::Disconnected},
}};

bool IsConnectAttemptState(IpcConnectionState state) {
  return state == S::Disconnected || state == S::Connecting || state == S::Handshaking;
}
}  // namespace

std::span<const IpcConnectionTransition> IpcConnectionTransitionTable() { return kTransitions; }

std::optional<IpcConnectionState> NextIpcConnectionState(IpcConnectionState from,
                                                         IpcConnectionEvent event) {
  for (const auto& transition : kTransitions) {
    if (transition.from == from && transition.event == event) return transition.to;
  }
  return std::nullopt;
}

std::string_view IpcConnectionStateName(IpcConnectionState state) {
  switch (state) {
    case S::Disconnected:
      return "disconnected";
    case S::Connecting:
      return "connecting";
    case S::Handshaking:
      return "handshaking";
    case S::Ready:
      return "ready";
    case S::Degraded:
      return "degraded";
  }
  return "unknown";
}

std::string_view IpcConnectionEventName(IpcConnectionEvent event) {
  switch (event) {
    case E::ConnectStarted:
      return "connect_started";
    case E::PipeConnected:
      return "pipe_connected";
    case E::ConnectFailed:
      return "connect_failed";
    case E::HandshakeAccepted:
      return "handshake_accepted";
    case E::HandshakeFailed:
      return "handshake_failed";
    case E::ConnectionLost:
      return "connection_lost";
    case E::ResponseDeadlineExceeded:
      return "response_deadline_exceeded";
    case E::ResponseRestored:
      return "response_restored";
    case E::Stopped:
      return "stopped";
  }
  return "unknown";
}

bool ShouldLogIpcConnectionTransition(IpcConnectionState from, IpcConnectionState to,
                                      uint32_t attempt) {
  if (!IsConnectAttemptState(from) || !IsConnectAttemptState(to)) return true;
  return attempt <= 1 || std::has_single_bit(attempt);
}

}  // namespace azookey::tsf
