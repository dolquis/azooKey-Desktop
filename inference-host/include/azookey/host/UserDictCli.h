#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace azookey::host {

enum class UserDictCliCommand {
  Add,
  Remove,
  List,
  Import,
  Export,
};

enum class UserDictCliFormat {
  Json,
  Tsv,
};

struct UserDictCliOptions {
  UserDictCliCommand command{UserDictCliCommand::List};
  std::string reading;
  std::string surface;
  std::optional<int32_t> cid;
  std::optional<int32_t> mid;
  std::optional<double> weight;
  std::string path;
  UserDictCliFormat format{UserDictCliFormat::Json};
  bool dry_run{false};
  bool offline{false};
};

struct UserDictCliRunOptions {
  std::string user_dict_path;
  std::string pipe_name;
  std::string handshake_token;
  bool prefer_pipe{true};
  uint32_t connect_timeout_ms{100};
  uint32_t response_timeout_ms{2000};
};

struct UserDictCliResult {
  int exit_code{0};
  std::vector<std::string> output_lines;
  std::string error;
};

std::optional<UserDictCliOptions> ParseUserDictCliArgs(const std::vector<std::string>& args,
                                                       std::string* error);

UserDictCliResult RunUserDictCli(const UserDictCliOptions& options,
                                 const UserDictCliRunOptions& run_options);

}  // namespace azookey::host
