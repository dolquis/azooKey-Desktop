#include "SettingsIpcClient.h"

#include <cstdlib>
#include <utility>

#include "azookey/ipc/Messages.h"
#include "azookey/ipc/NamedPipeTransport.h"
#include "azookey/ipc/Payloads.h"

namespace azookey::settings {
namespace {

constexpr uint64_t kHandshakeRequestId = 1;
constexpr uint64_t kUpdateConfigRequestId = 2;

azookey::ipc::Envelope MakeEnvelope(uint64_t request_id, azookey::ipc::MessageType type,
                                    std::string payload) {
  azookey::ipc::Envelope envelope;
  envelope.request_id = request_id;
  envelope.trace_id = "settings-app";
  envelope.type = type;
  envelope.payload_json = std::move(payload);
  return envelope;
}

std::string ReadHandshakeToken() {
  size_t required = 0;
  if (getenv_s(&required, nullptr, 0, "AZOOKEY_IPC_HANDSHAKE_TOKEN") != 0 || required == 0) {
    return {};
  }
  std::string value(required, '\0');
  if (getenv_s(&required, value.data(), value.size(), "AZOOKEY_IPC_HANDSHAKE_TOKEN") != 0) {
    return {};
  }
  if (!value.empty() && value.back() == '\0') value.pop_back();
  return value;
}

SettingsIpcResult Failure(std::string error) {
  SettingsIpcResult result;
  result.error = std::move(error);
  return result;
}

}  // namespace

SettingsIpcOptions DefaultSettingsIpcOptions() {
  SettingsIpcOptions options;
  options.pipe_name = azookey::ipc::DefaultPipeName();
  options.handshake_token = ReadHandshakeToken();
  return options;
}

SettingsIpcResult NotifyHostOfSettingsChange(const SettingsIpcOptions& options) {
  if (options.pipe_name.empty()) return Failure("could not resolve the per-user IPC pipe");

  azookey::ipc::NamedPipeClient client;
  if (!client.Connect(options.pipe_name, options.connect_timeout_ms)) {
    return Failure("settings were saved, but the inference host is not running");
  }

  azookey::ipc::HandshakeRequest handshake;
  handshake.tip_version = "settings-app";
  handshake.protocol_version = azookey::ipc::kHandshakeProtocolVersion;
  handshake.capabilities = {"settings"};
  handshake.client_id = "settings-app";
  handshake.handshake_token = options.handshake_token;
  if (!client.Send(MakeEnvelope(kHandshakeRequestId, azookey::ipc::MessageType::Handshake,
                                azookey::ipc::BuildHandshakeRequest(handshake)))) {
    return Failure("settings were saved, but the host handshake could not be sent");
  }
  const auto handshake_response = client.ReceiveWithTimeout(options.response_timeout_ms);
  if (!handshake_response || handshake_response->request_id != kHandshakeRequestId ||
      handshake_response->type != azookey::ipc::MessageType::Handshake) {
    return Failure("settings were saved, but the host handshake timed out");
  }
  const auto handshake_payload =
      azookey::ipc::ParseHandshakeResponse(handshake_response->payload_json);
  if (!handshake_payload || !handshake_payload->accepted) {
    return Failure("settings were saved, but the host rejected the handshake");
  }

  if (!client.Send(
          MakeEnvelope(kUpdateConfigRequestId, azookey::ipc::MessageType::UpdateConfig, "{}"))) {
    return Failure("settings were saved, but UpdateConfig could not be sent");
  }
  const auto update_response = client.ReceiveWithTimeout(options.response_timeout_ms);
  if (!update_response || update_response->request_id != kUpdateConfigRequestId ||
      update_response->type != azookey::ipc::MessageType::UpdateConfig) {
    return Failure("settings were saved, but UpdateConfig timed out");
  }
  const auto payload = azookey::ipc::ParseUpdateConfigResponse(update_response->payload_json);
  if (!payload)
    return Failure("settings were saved, but UpdateConfig returned an invalid response");
  if (!payload->ok) {
    return Failure(payload->error.value_or("settings were saved, but the host rejected them"));
  }

  SettingsIpcResult result;
  result.ok = true;
  return result;
}

}  // namespace azookey::settings
