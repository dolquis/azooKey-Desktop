#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include "SettingsIpcClient.h"
#include "azookey/ipc/Messages.h"
#include "azookey/ipc/NamedPipeTransport.h"
#include "azookey/ipc/Payloads.h"

namespace {

azookey::ipc::Envelope ResponseFor(const azookey::ipc::Envelope& request, std::string payload) {
  azookey::ipc::Envelope response;
  response.request_id = request.request_id;
  response.trace_id = request.trace_id;
  response.type = request.type;
  response.payload_json = std::move(payload);
  return response;
}

}  // namespace

TEST(SettingsIpcClientTest, HandshakesThenSendsEmptyUpdateConfigPayload) {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const std::string pipe_name = "\\\\.\\pipe\\azookey-settings-test-" +
                                std::to_string(GetCurrentProcessId()) + "-" + std::to_string(stamp);
  std::atomic<bool> saw_empty_update{false};
  std::atomic<bool> authenticated{false};

  azookey::ipc::NamedPipeServer server;
  ASSERT_TRUE(server.Start(pipe_name, [&] {
    return [&](const azookey::ipc::Envelope& request) -> std::optional<azookey::ipc::Envelope> {
      if (request.type == azookey::ipc::MessageType::Handshake) {
        const auto parsed = azookey::ipc::ParseHandshakeRequest(request.payload_json);
        authenticated = parsed && parsed->tip_version == "settings-app" &&
                        parsed->protocol_version == azookey::ipc::kHandshakeProtocolVersion &&
                        parsed->capabilities == std::vector<std::string>{"settings"};
        azookey::ipc::HandshakeResponse response;
        response.host_version = "test";
        response.accepted = authenticated;
        return ResponseFor(request, azookey::ipc::BuildHandshakeResponse(response));
      }
      if (request.type == azookey::ipc::MessageType::UpdateConfig && authenticated) {
        saw_empty_update = request.payload_json == "{}";
        azookey::ipc::UpdateConfigResponse response;
        response.ok = true;
        return ResponseFor(request, azookey::ipc::BuildUpdateConfigResponse(response));
      }
      return std::nullopt;
    };
  }));

  azookey::settings::SettingsIpcOptions options;
  options.pipe_name = pipe_name;
  options.connect_timeout_ms = 1000;
  options.response_timeout_ms = 1000;
  const auto result = azookey::settings::NotifyHostOfSettingsChange(options);
  server.Stop();

  EXPECT_TRUE(result.ok) << result.error.value_or("");
  EXPECT_TRUE(authenticated);
  EXPECT_TRUE(saw_empty_update);
}
