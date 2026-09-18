// Pins the TIP IPC worker's connection state machine (docs/dev-infrastructure-spec.md
// §8.2, DEV-1173). The worker drives it from real pipe events; these cases cover
// every transition without a pipe so a table edit cannot silently widen or drop one.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "azookey/tsf/IpcConnectionState.h"

namespace {
using azookey::tsf::IpcConnectionEvent;
using azookey::tsf::IpcConnectionState;
using S = IpcConnectionState;
using E = IpcConnectionEvent;

const std::vector<S> kAllStates = {S::Disconnected, S::Connecting, S::Handshaking, S::Ready,
                                   S::Degraded};
const std::vector<E> kAllEvents = {
    E::ConnectStarted,           E::PipeConnected,    E::ConnectFailed,
    E::HandshakeAccepted,        E::HandshakeFailed,  E::ConnectionLost,
    E::ResponseDeadlineExceeded, E::ResponseRestored, E::Stopped};

// The expected machine, written out independently of the implementation table.
const std::vector<std::tuple<S, E, S>> kExpected = {
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
};

TEST(TsfTipIpcConnectionStateTest, EveryListedTransitionReachesItsTarget) {
  for (const auto& [from, event, to] : kExpected) {
    const auto next = azookey::tsf::NextIpcConnectionState(from, event);
    ASSERT_TRUE(next.has_value()) << azookey::tsf::IpcConnectionStateName(from) << " + "
                                  << azookey::tsf::IpcConnectionEventName(event);
    EXPECT_EQ(*next, to) << azookey::tsf::IpcConnectionStateName(from) << " + "
                         << azookey::tsf::IpcConnectionEventName(event);
  }
}

TEST(TsfTipIpcConnectionStateTest, EveryUnlistedPairIsRejected) {
  size_t accepted = 0;
  for (const auto from : kAllStates) {
    for (const auto event : kAllEvents) {
      const bool listed = std::any_of(kExpected.begin(), kExpected.end(), [&](const auto& row) {
        return std::get<0>(row) == from && std::get<1>(row) == event;
      });
      const auto next = azookey::tsf::NextIpcConnectionState(from, event);
      EXPECT_EQ(next.has_value(), listed) << azookey::tsf::IpcConnectionStateName(from) << " + "
                                          << azookey::tsf::IpcConnectionEventName(event);
      if (next) ++accepted;
    }
  }
  EXPECT_EQ(accepted, kExpected.size());
  EXPECT_EQ(azookey::tsf::IpcConnectionTransitionTable().size(), kExpected.size());
}

TEST(TsfTipIpcConnectionStateTest, NoTransitionIsASelfLoop) {
  for (const auto& transition : azookey::tsf::IpcConnectionTransitionTable()) {
    EXPECT_NE(transition.from, transition.to);
  }
}

TEST(TsfTipIpcConnectionStateTest, EveryStateIsReachableFromDisconnected) {
  std::set<S> reached = {S::Disconnected};
  bool grew = true;
  while (grew) {
    grew = false;
    for (const auto& transition : azookey::tsf::IpcConnectionTransitionTable()) {
      if (reached.count(transition.from) && reached.insert(transition.to).second) grew = true;
    }
  }
  EXPECT_EQ(reached.size(), kAllStates.size());
}

TEST(TsfTipIpcConnectionStateTest, WireNamesAreFixedLowercaseAndUnique) {
  std::set<std::string> names;
  for (const auto state : kAllStates) {
    const std::string name(azookey::tsf::IpcConnectionStateName(state));
    EXPECT_NE(name, "unknown");
    EXPECT_TRUE(names.insert(name).second) << name;
    for (const char c : name) EXPECT_TRUE((c >= 'a' && c <= 'z') || c == '_') << name;
  }
  EXPECT_EQ(azookey::tsf::IpcConnectionStateName(S::Ready), "ready");
  EXPECT_EQ(azookey::tsf::IpcConnectionStateName(S::Degraded), "degraded");
  names.clear();
  for (const auto event : kAllEvents) {
    const std::string name(azookey::tsf::IpcConnectionEventName(event));
    EXPECT_NE(name, "unknown");
    EXPECT_TRUE(names.insert(name).second) << name;
    for (const char c : name) EXPECT_TRUE((c >= 'a' && c <= 'z') || c == '_') << name;
  }
}

TEST(TsfTipIpcConnectionStateTest, FailedAttemptTransitionsAreLoggedLogarithmically) {
  std::vector<uint32_t> logged;
  for (uint32_t attempt = 1; attempt <= 20; ++attempt) {
    if (azookey::tsf::ShouldLogIpcConnectionTransition(S::Connecting, S::Disconnected, attempt))
      logged.push_back(attempt);
  }
  EXPECT_EQ(logged, (std::vector<uint32_t>{1, 2, 4, 8, 16}));
  EXPECT_FALSE(azookey::tsf::ShouldLogIpcConnectionTransition(S::Disconnected, S::Connecting, 3));
  EXPECT_FALSE(azookey::tsf::ShouldLogIpcConnectionTransition(S::Handshaking, S::Disconnected, 5));
}

TEST(TsfTipIpcConnectionStateTest, HostFacingTransitionsAreAlwaysLogged) {
  for (const uint32_t attempts : {0u, 3u, 5u, 1000u}) {
    EXPECT_TRUE(azookey::tsf::ShouldLogIpcConnectionTransition(S::Handshaking, S::Ready, attempts));
    EXPECT_TRUE(azookey::tsf::ShouldLogIpcConnectionTransition(S::Ready, S::Degraded, attempts));
    EXPECT_TRUE(azookey::tsf::ShouldLogIpcConnectionTransition(S::Degraded, S::Ready, attempts));
    EXPECT_TRUE(
        azookey::tsf::ShouldLogIpcConnectionTransition(S::Ready, S::Disconnected, attempts));
    EXPECT_TRUE(
        azookey::tsf::ShouldLogIpcConnectionTransition(S::Connecting, S::Handshaking, attempts));
  }
}
}  // namespace
