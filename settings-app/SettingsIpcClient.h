#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace azookey::settings {

struct SettingsIpcOptions {
  std::string pipe_name;
  std::string handshake_token;
  uint32_t connect_timeout_ms{1000};
  uint32_t response_timeout_ms{3000};
};

struct SettingsIpcResult {
  bool ok{false};
  std::optional<std::string> error;
};

SettingsIpcOptions DefaultSettingsIpcOptions();
SettingsIpcResult NotifyHostOfSettingsChange(const SettingsIpcOptions& options);

}  // namespace azookey::settings
