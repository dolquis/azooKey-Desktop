#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace azookey::learning {
class ByteCrypto;
}

namespace azookey::host {

// `azookey_inference_host newwords ...`: the M36-A approval path until the settings app
// grows a new-word pane (docs/auto-word-registration-spec.md section 7-3).
enum class NewWordsCliCommand {
  List,
  Confirm,
  Reject,
};

enum class NewWordsCliFormat {
  Json,
  Tsv,
};

struct NewWordsCliOptions {
  NewWordsCliCommand command{NewWordsCliCommand::List};
  // list: one of "pending", "confirmed", "rejected".
  std::string state{"pending"};
  std::string reading;
  std::string surface;
  NewWordsCliFormat format{NewWordsCliFormat::Json};
  bool offline{false};
};

struct NewWordsCliRunOptions {
  std::filesystem::path auto_word_store_path;
  std::string pipe_name;
  std::string handshake_token;
  bool prefer_pipe{true};
  uint32_t connect_timeout_ms{100};
  uint32_t response_timeout_ms{2000};
  const learning::ByteCrypto* crypto{nullptr};
};

struct NewWordsCliResult {
  int exit_code{0};
  std::vector<std::string> output_lines;
  std::string error;
};

std::optional<NewWordsCliOptions> ParseNewWordsCliArgs(const std::vector<std::string>& args,
                                                       std::string* error);

NewWordsCliResult RunNewWordsCli(const NewWordsCliOptions& options,
                                 const NewWordsCliRunOptions& run_options);

}  // namespace azookey::host
