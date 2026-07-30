#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

#include "Diagnostics.h"
#include "azookey/ipc/Json.h"

namespace {

enum class Command {
  None,
  Help,
  Json,
  Collect,
};

struct Options {
  Command command{Command::None};
  std::filesystem::path output;
};

void PrintUsage() {
  std::cout << "Usage:\n"
            << "  azookey_diag.exe --json\n"
            << "  azookey_diag.exe --collect [--output <zip-path>]\n\n"
            << "--json emits the stable M44 v1 diagnostic schema.\n"
            << "--collect writes a redacted diagnostic ZIP without input, candidate, "
               "dictionary, or learning bodies.\n";
}

std::filesystem::path DefaultOutputPath() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t value = std::chrono::system_clock::to_time_t(now);
  std::tm local{};
#ifdef _WIN32
  localtime_s(&local, &value);
#else
  localtime_r(&value, &local);
#endif
  std::ostringstream name;
  name << "azookey-diagnostics-" << std::put_time(&local, "%Y%m%d-%H%M%S") << ".zip";
  return std::filesystem::current_path() / name.str();
}

#ifdef _WIN32
bool ParseOptions(int argc, wchar_t** argv, Options* options, std::string* error) {
  for (int index = 1; index < argc; ++index) {
    const std::wstring argument = argv[index];
    if (argument == L"--help" || argument == L"-h") {
      if (options->command != Command::None) {
        *error = "choose exactly one of --help, --json, or --collect";
        return false;
      }
      options->command = Command::Help;
    } else if (argument == L"--json") {
      if (options->command != Command::None) {
        *error = "choose exactly one of --help, --json, or --collect";
        return false;
      }
      options->command = Command::Json;
    } else if (argument == L"--collect") {
      if (options->command != Command::None) {
        *error = "choose exactly one of --help, --json, or --collect";
        return false;
      }
      options->command = Command::Collect;
    } else if (argument == L"--output") {
      if (index + 1 >= argc) {
        *error = "--output requires a path";
        return false;
      }
      options->output = argv[++index];
    } else {
      *error = "unknown option";
      return false;
    }
  }
  if (options->command == Command::None) options->command = Command::Help;
  if (options->command != Command::Collect && !options->output.empty()) {
    *error = "--output requires --collect";
    return false;
  }
  return true;
}
#else
bool ParseOptions(int argc, char** argv, Options* options, std::string* error) {
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--help" || argument == "-h") {
      if (options->command != Command::None) {
        *error = "choose exactly one of --help, --json, or --collect";
        return false;
      }
      options->command = Command::Help;
    } else if (argument == "--json") {
      if (options->command != Command::None) {
        *error = "choose exactly one of --help, --json, or --collect";
        return false;
      }
      options->command = Command::Json;
    } else if (argument == "--collect") {
      if (options->command != Command::None) {
        *error = "choose exactly one of --help, --json, or --collect";
        return false;
      }
      options->command = Command::Collect;
    } else if (argument == "--output") {
      if (index + 1 >= argc) {
        *error = "--output requires a path";
        return false;
      }
      options->output = argv[++index];
    } else {
      *error = "unknown option";
      return false;
    }
  }
  if (options->command == Command::None) options->command = Command::Help;
  if (options->command != Command::Collect && !options->output.empty()) {
    *error = "--output requires --collect";
    return false;
  }
  return true;
}
#endif

int Run(const Options& options) {
  if (options.command == Command::Help) {
    PrintUsage();
    return 0;
  }
  auto result = azookey::diagnostics::ProbeSystem();
  if (options.command == Command::Json) {
    std::cout << azookey::diagnostics::SerializeReport(result.report) << '\n';
    return 0;
  }

  const auto output = options.output.empty() ? DefaultOutputPath() : options.output;
  std::error_code ec;
  if (!output.parent_path().empty()) {
    std::filesystem::create_directories(output.parent_path(), ec);
    if (ec) {
      std::cerr << "failed to create output directory: " << ec.message() << '\n';
      return 2;
    }
  }
  std::string error;
  if (!azookey::diagnostics::WriteZip(output, azookey::diagnostics::BuildCollectionEntries(result),
                                      &error)) {
    std::cerr << error << '\n';
    return 2;
  }
  azookey::ipc::json::Object response;
  response.emplace("output", azookey::ipc::json::Value(output.string()));
  response.emplace(
      "status", azookey::ipc::json::Value(azookey::diagnostics::StatusName(result.report.status)));
  std::cout << azookey::ipc::json::Stringify(azookey::ipc::json::Value(std::move(response)))
            << '\n';
  return 0;
}

}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
  Options options;
  std::string error;
  if (!ParseOptions(argc, argv, &options, &error)) {
    std::cerr << error << '\n';
    PrintUsage();
    return 2;
  }
  return Run(options);
}
