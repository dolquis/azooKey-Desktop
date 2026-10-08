#include "azookey/host/ModelsCli.h"

#include <charconv>
#include <exception>
#include <sstream>
#include <system_error>

#include "azookey/host/ModelBenchmark.h"
#include "azookey/host/ModelScanner.h"

namespace azookey::host {

namespace {

bool TakeArg(const std::vector<std::string>& args, size_t& i, std::string& value,
             std::string* error) {
  if (i + 1 >= args.size()) {
    *error = args[i] + " requires a value";
    return false;
  }
  value = args[++i];
  return true;
}

bool ParseCount(const std::string& text, uint32_t& out) {
  const auto* end = text.data() + text.size();
  const auto parsed = std::from_chars(text.data(), end, out);
  return parsed.ec == std::errc{} && parsed.ptr == end;
}

ModelsCliResult RunModelsCliUnchecked(const ModelsCliOptions& options,
                                      const ModelsCliRunOptions& run_options);

}  // namespace

std::optional<ModelsCliOptions> ParseModelsCliArgs(const std::vector<std::string>& args,
                                                   std::string* error) {
  std::string ignored;
  if (!error) error = &ignored;
  if (args.empty() || (args[0] != "list" && args[0] != "bench")) {
    *error = "models requires 'list' or 'bench'";
    return std::nullopt;
  }
  ModelsCliOptions options;
  options.command = args[0] == "list" ? ModelsCliCommand::List : ModelsCliCommand::Bench;
  const bool list = options.command == ModelsCliCommand::List;
  for (size_t i = 1; i < args.size(); ++i) {
    const auto& arg = args[i];
    std::string value;
    if (arg == "--json") {
      options.json = true;
    } else if (list && arg == "--sha256") {
      options.list.compute_sha256 = true;
    } else if (list && arg == "--dir") {
      if (!TakeArg(args, i, options.list.directory, error)) return std::nullopt;
    } else if (!list && arg == "--path") {
      if (!TakeArg(args, i, options.bench.path, error)) return std::nullopt;
    } else if (!list && arg == "--backend") {
      if (!TakeArg(args, i, options.bench.backend, error)) return std::nullopt;
    } else if (!list && arg == "--case") {
      if (!TakeArg(args, i, value, error)) return std::nullopt;
      options.bench.cases.push_back(value);
    } else if (!list && (arg == "--iterations" || arg == "--warmup")) {
      if (!TakeArg(args, i, value, error)) return std::nullopt;
      auto& target = arg == "--iterations" ? options.bench.iterations : options.bench.warmup;
      if (!ParseCount(value, target)) {
        *error = "invalid " + arg + " value";
        return std::nullopt;
      }
    } else {
      *error = "unknown models argument: " + arg;
      return std::nullopt;
    }
  }
  if (!list && options.bench.path.empty()) {
    *error = "models bench requires --path";
    return std::nullopt;
  }
  return options;
}

ModelsCliResult RunModelsCli(const ModelsCliOptions& options,
                             const ModelsCliRunOptions& run_options) {
  try {
    return RunModelsCliUnchecked(options, run_options);
  } catch (const std::exception&) {
    ModelsCliResult failed;
    failed.exit_code = 2;
    failed.error = "models command failed";
    return failed;
  }
}

namespace {

ModelsCliResult RunModelsCliUnchecked(const ModelsCliOptions& options,
                                      const ModelsCliRunOptions& run_options) {
  ModelsCliResult result;
  if (options.command == ModelsCliCommand::Bench) {
    const auto response = RunModelBenchmark(options.bench, ModelBenchmarkOptions{});
    if (options.json) {
      result.output_lines.push_back(ipc::BuildBenchmarkModelResponse(response));
    } else {
      std::ostringstream line;
      line << "status=" << response.status << " backend=" << response.backend
           << " iterations=" << response.iterations_completed << " p50_ms=" << response.p50_ms
           << " p95_ms=" << response.p95_ms << " p99_ms=" << response.p99_ms
           << " load_ms=" << response.load_ms;
      result.output_lines.push_back(line.str());
    }
    if (response.status != "success") {
      result.exit_code = 1;
      result.error = response.error.value_or(response.status);
    }
    return result;
  }

  ipc::ListModelsResponse response;
  const auto directory =
      ResolveModelListingDirectory(options.list.directory, run_options.models_dir);
  if (!directory) {
    response.ok = false;
    response.error = "directory_outside_models_root";
  } else {
    ModelScanOptions scan;
    scan.compute_sha256 = options.list.compute_sha256;
    // Offline: no Host is consulted, so nothing is reported as loaded.
    for (const auto& entry : ScanModelDirectory(*directory, scan))
      response.models.push_back(ToListedModel(entry, "not_loaded", {}));
  }
  if (options.json) {
    result.output_lines.push_back(ipc::BuildListModelsResponse(response));
  } else {
    for (const auto& m : response.models) {
      result.output_lines.push_back(m.format + "\t" +
                                    (m.valid ? "valid" : "invalid:" + m.last_error) + "\t" +
                                    std::to_string(m.size_bytes) + "\t" + m.path);
    }
  }
  if (!response.ok) {
    result.exit_code = 2;
    result.error = response.error.value_or("list failed");
  }
  return result;
}

}  // namespace

}  // namespace azookey::host
