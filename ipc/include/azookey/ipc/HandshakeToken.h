#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace azookey::ipc {

// Production clients read this file for every handshake. The environment
// override is retained for development and tests.
std::optional<std::filesystem::path> DefaultHandshakeTokenPath();
std::optional<std::string> ReadHandshakeTokenFile(const std::filesystem::path& path);
std::optional<std::string> ReadClientHandshakeToken();

// The Host publishes a fresh token before it starts accepting pipe clients.
std::optional<std::string> GenerateHandshakeToken();
bool PublishHandshakeToken(const std::filesystem::path& path, std::string_view token);

}  // namespace azookey::ipc
