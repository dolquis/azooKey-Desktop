#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "BenchmarkCommandLine.h"
#include "azookey/ipc/Json.h"
#include "azookey/logging/Phase.h"

namespace {

namespace json = azookey::ipc::json;

constexpr std::array kPhases = {
    azookey::logging::Phase::KeyDown,        azookey::logging::Phase::RomajiConvert,
    azookey::logging::Phase::IpcSerialize,   azookey::logging::Phase::PipeSend,
    azookey::logging::Phase::HostQueueWait,  azookey::logging::Phase::ModelInference,
    azookey::logging::Phase::Rerank,         azookey::logging::Phase::PipeRecv,
    azookey::logging::Phase::StalenessCheck, azookey::logging::Phase::UiApply,
    azookey::logging::Phase::Total,
};

struct TraceSummary {
  std::map<std::string, std::vector<double>> phases;
  size_t skipped_lines{0};
};

void WarnSkipped(TraceSummary& summary, size_t line_number, std::string_view reason) {
  constexpr size_t kMaxWarnings = 5;
  ++summary.skipped_lines;
  if (summary.skipped_lines <= kMaxWarnings) {
    std::cerr << "warning: trace line " << line_number << " skipped: " << reason << '\n';
  } else if (summary.skipped_lines == kMaxWarnings + 1) {
    std::cerr << "warning: further skipped-line warnings suppressed\n";
  }
}

bool IsKnownPhase(std::string_view name) {
  return std::any_of(kPhases.begin(), kPhases.end(), [name](azookey::logging::Phase phase) {
    return azookey::logging::PhaseName(phase) == name;
  });
}

double Percentile(const std::vector<double>& sorted, size_t percentile) {
  return sorted[(sorted.size() - 1) * percentile / 100];
}

TraceSummary ReadTrace(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("cannot open trace file");

  TraceSummary summary;
  std::string line;
  size_t line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    const auto value = json::Parse(line);
    if (!value || !value->IsObject()) {
      WarnSkipped(summary, line_number, "invalid JSON object");
      continue;
    }
    const auto phase = value->GetString("phase");
    if (!phase) {
      WarnSkipped(summary, line_number, "missing or invalid phase");
      continue;
    }
    if (!IsKnownPhase(*phase)) {
      WarnSkipped(summary, line_number, "unknown phase");
      continue;
    }
    const auto latency_ms = value->GetNumber("latency_ms");
    // key_down is a zero-offset anchor in the M51 schema and may carry only t_ms.
    if (*phase == azookey::logging::PhaseName(azookey::logging::Phase::KeyDown) && !latency_ms) {
      const auto offset_ms = value->GetNumber("t_ms");
      if (offset_ms && std::isfinite(*offset_ms) && *offset_ms == 0.0) continue;
    }
    if (!latency_ms || !std::isfinite(*latency_ms) || *latency_ms < 0.0) {
      WarnSkipped(summary, line_number, "missing or invalid latency_ms");
      continue;
    }
    summary.phases[*phase].push_back(*latency_ms);
  }
  if (input.bad()) throw std::runtime_error("failed to read trace file");
  for (auto& [phase, samples] : summary.phases) {
    (void)phase;
    std::sort(samples.begin(), samples.end());
  }
  if (summary.phases.empty()) throw std::runtime_error("trace file has no valid phase samples");
  return summary;
}

std::string ToJson(const TraceSummary& summary) {
  json::Object phases;
  for (const auto& [name, samples] : summary.phases) {
    json::Object metrics;
    metrics.emplace("p50_ms", json::Value(Percentile(samples, 50)));
    metrics.emplace("p95_ms", json::Value(Percentile(samples, 95)));
    metrics.emplace("p99_ms", json::Value(Percentile(samples, 99)));
    metrics.emplace("samples", json::Value(static_cast<uint64_t>(samples.size())));
    phases.emplace(name, json::Value(std::move(metrics)));
  }
  const auto total = summary.phases.find("total");
  json::Object root;
  root.emplace("phases", json::Value(std::move(phases)));
  root.emplace(
      "samples",
      json::Value(static_cast<uint64_t>(total == summary.phases.end() ? 0 : total->second.size())));
  root.emplace("schema_version", json::Value(1));
  root.emplace("skipped_lines", json::Value(static_cast<uint64_t>(summary.skipped_lines)));
  return json::Stringify(json::Value(std::move(root)));
}

void PrintSummary(const TraceSummary& summary) {
  const auto total = summary.phases.find("total");
  const size_t total_samples = total == summary.phases.end() ? 0 : total->second.size();
  std::cout << "QueryCandidates latency summary (N=" << total_samples
            << ", skipped=" << summary.skipped_lines << ")\n";
  std::cout << std::setprecision(6);
  for (const auto phase : kPhases) {
    const auto name = azookey::logging::PhaseName(phase);
    const auto found = summary.phases.find(std::string(name));
    if (found == summary.phases.end()) continue;
    const auto& samples = found->second;
    std::cout << "  " << name << " (N=" << samples.size() << "): p50=" << Percentile(samples, 50)
              << " ms p95=" << Percentile(samples, 95) << " ms p99=" << Percentile(samples, 99)
              << " ms\n";
  }
}

void PrintUsage(const char* exe) {
  std::cerr << "Usage: " << exe << " TRACE.jsonl [--summary] [--json]\n";
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto args = azookey::bench::Utf8CommandLineArguments(argc, argv);
    std::filesystem::path input_path;
    bool json_output = false;
    for (size_t i = 1; i < args.size(); ++i) {
      if (args[i] == "--help" || args[i] == "-h") {
        PrintUsage(args[0].c_str());
        return 0;
      }
      if (args[i] == "--summary") {
        continue;
      }
      if (args[i] == "--json") {
        json_output = true;
        continue;
      }
      if (args[i].starts_with('-') || !input_path.empty()) {
        throw std::invalid_argument("unknown or duplicate argument: " + args[i]);
      }
      input_path = azookey::bench::Utf8Path(args[i]);
    }
    if (input_path.empty()) throw std::invalid_argument("trace file path is required");
    const auto summary = ReadTrace(input_path);
    if (json_output) {
      std::cout << ToJson(summary) << '\n';
    } else {
      PrintSummary(summary);
    }
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << '\n';
    PrintUsage(argc > 0 ? argv[0] : "azookey_trace_viewer");
    return 2;
  }
}
