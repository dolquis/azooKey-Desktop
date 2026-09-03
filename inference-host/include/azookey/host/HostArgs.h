#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "azookey/host/InferenceEngine.h"

namespace azookey::host {

struct HostArgs {
  EngineConfig config;
  std::optional<std::filesystem::path> explicit_learning_path;
  std::optional<std::filesystem::path> explicit_user_dict_path;
  std::string mock_dict_path;
  bool explicit_backend{false};
  bool explicit_model_path{false};
  bool pipe_mode{false};
  std::string pipe_name;
  std::string handshake_token;
  std::optional<std::vector<std::string>> userdict_args;
  std::optional<std::vector<std::string>> lookup_args;
};

struct HostArgsParseResult {
  HostArgs args;
  std::optional<std::string> error;

  explicit operator bool() const { return !error.has_value(); }
};

HostArgsParseResult ParseHostArgs(const std::vector<std::string>& argv, EngineConfig initial_config,
                                  std::string default_handshake_token = {});

}  // namespace azookey::host
