#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace azookey::host {

enum class LookupCliMode {
  ExactReading,
  ReadingPrefix,
  Surface,
};

enum class LookupCliFormat {
  Json,
  Tsv,
};

struct LookupCliOptions {
  LookupCliMode mode{LookupCliMode::ExactReading};
  std::string query;
  LookupCliFormat format{LookupCliFormat::Json};
};

struct LookupCliRunOptions {
  std::filesystem::path learning_path;
  std::filesystem::path user_dict_path;
  std::chrono::milliseconds user_dict_lock_timeout{5000};
};

struct LookupCliResult {
  int exit_code{0};
  std::vector<std::string> output_lines;
  std::string error;
};

std::optional<LookupCliOptions> ParseLookupCliArgs(const std::vector<std::string>& args,
                                                   std::string* error);

LookupCliResult RunLookupCli(const LookupCliOptions& options,
                             const LookupCliRunOptions& run_options);

}  // namespace azookey::host
