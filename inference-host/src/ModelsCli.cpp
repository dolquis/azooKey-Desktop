#include "azookey/host/ModelsCli.h"

#include <charconv>
#include <exception>
#include <fstream>
#include <sstream>
#include <system_error>

#include "azookey/core/PlatformPaths.h"
#include "azookey/host/ModelBenchmark.h"
#include "azookey/host/ModelCatalog.h"
#include "azookey/host/ModelFetch.h"
#include "azookey/host/ModelScanner.h"
#include "azookey/ipc/Json.h"

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
  if (!args.empty() && (args[0] == "catalog" || args[0] == "fetch")) {
    ModelsCliOptions options;
    options.command = args[0] == "catalog" ? ModelsCliCommand::Catalog : ModelsCliCommand::Fetch;
    const bool fetch = options.command == ModelsCliCommand::Fetch;
    for (size_t i = 1; i < args.size(); ++i) {
      // fetch <id> and fetch --id <id> are the same; the id may be given once.
      const bool named = fetch && args[i] == "--id";
      const bool positional = fetch && !args[i].empty() && args[i][0] != '-';
      if ((named || positional) && !options.fetch_model_id.empty()) {
        *error = "models fetch takes one model id";
        return std::nullopt;
      }
      if (named) {
        if (!TakeArg(args, i, options.fetch_model_id, error)) return std::nullopt;
      } else if (positional) {
        options.fetch_model_id = args[i];
      } else {
        *error = "unknown models argument: " + args[i];
        return std::nullopt;
      }
    }
    return options;
  }
  if (args.empty() || (args[0] != "list" && args[0] != "bench")) {
    *error = "models requires 'list', 'bench', 'catalog' or 'fetch'";
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

// privacy.mode=offline forbids the fetch. A missing settings.json is the defaults (normal);
// one that cannot be read or parsed also forbids it, since its mode is unknown.
// Read once when the fetch starts; the CLI is short-lived and does not follow a later change.
bool SettingsAllowNetwork(const std::filesystem::path& settings_path) {
  if (settings_path.empty()) return false;
  std::error_code ec;
  if (!std::filesystem::exists(settings_path, ec)) return !ec;
  std::ifstream in(settings_path, std::ios::binary);
  if (!in) return false;
  std::ostringstream content;
  content << in.rdbuf();
  if (!in && !in.eof()) return false;
  const auto parsed = ipc::json::Parse(content.str());
  if (!parsed || !parsed->IsObject()) return false;
  const auto& root = parsed->AsObject();
  const auto privacy = root.find("privacy");
  if (privacy == root.end() || !privacy->second.IsObject()) return true;
  return privacy->second.GetString("mode").value_or("normal") != "offline";
}

ModelsCliResult RunCatalogCommand(const ModelsCliOptions& options,
                                  const ModelsCliRunOptions& run_options) {
  ModelsCliResult result;
  const auto parsed = ParseModelCatalogJson(DefaultModelCatalogJson());
  if (!parsed.ok) {
    result.exit_code = 2;
    result.error = "embedded model catalog is invalid: " + parsed.error.value_or("");
    return result;
  }
  const auto& catalog = parsed.catalog;
  const auto zenzai_dir = DefaultZenzaiModelDirectory(run_options.models_dir);

  if (options.command == ModelsCliCommand::Catalog) {
    for (const auto& entry : catalog.models) {
      const auto resolved = ResolveModelCatalogModel(catalog, zenzai_dir, entry.id);
      const bool present = resolved.model && resolved.model->exists;
      result.output_lines.push_back(
          entry.id + "\t" + (entry.id == catalog.default_model_id ? "default" : "-") + "\t" +
          std::string(ZenzaiPromptFormatName(entry.prompt_format)) + "\t" +
          std::to_string(entry.size_bytes) + "\t" + (present ? "present" : "missing") + "\t" +
          core::PathToUtf8(zenzai_dir / entry.file_name));
    }
    return result;
  }

  // Without a resolved data root the models directory would be relative to the current
  // directory and privacy.mode could not be read.
  if (run_options.models_dir.empty() || run_options.settings_path.empty()) {
    result.exit_code = 2;
    result.error =
        "paths_unavailable: models fetch requires the models directory and settings.json";
    return result;
  }
  const auto& id =
      options.fetch_model_id.empty() ? catalog.default_model_id : options.fetch_model_id;
  const auto resolved = ResolveModelCatalogModel(catalog, zenzai_dir, id);
  if (!resolved.model) {
    result.exit_code = 2;
    result.error = resolved.error.value_or("unknown model id");
    return result;
  }
  const auto network_allowed = SettingsAllowNetwork(run_options.settings_path);
  HttpPackDownloader http;
  IPackDownloader& downloader = run_options.downloader ? *run_options.downloader : http;
  const auto fetched = FetchCatalogModel(resolved.model->entry, zenzai_dir, downloader,
                                         [network_allowed] { return network_allowed; });
  std::ostringstream line;
  line << "status=" << ModelFetchStatusName(fetched.status) << " id=" << id
       << " bytes=" << fetched.bytes_received;
  if (fetched.ok()) line << " path=" << core::PathToUtf8(fetched.path);
  result.output_lines.push_back(line.str());
  if (!fetched.ok()) {
    result.exit_code = 1;
    if (fetched.status == ModelFetchStatus::Offline) {
      result.error = "privacy.mode=offline (or an unreadable settings.json) forbids the fetch";
    } else if (fetched.status == ModelFetchStatus::NotFetchable) {
      result.error = "the catalog entry has no source";
    } else {
      result.error = fetched.error + (fetched.detail.empty() ? "" : ": " + fetched.detail);
    }
  }
  return result;
}

ModelsCliResult RunModelsCliUnchecked(const ModelsCliOptions& options,
                                      const ModelsCliRunOptions& run_options) {
  if (options.command == ModelsCliCommand::Catalog || options.command == ModelsCliCommand::Fetch) {
    return RunCatalogCommand(options, run_options);
  }
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
