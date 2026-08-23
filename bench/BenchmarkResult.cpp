#include "BenchmarkResult.h"

#include <cmath>
#include <fstream>
#include <sstream>
#include <utility>

#include "azookey/ipc/Json.h"

namespace azookey::bench {

namespace {

using azookey::ipc::json::Null;
using azookey::ipc::json::Object;
using azookey::ipc::json::Value;

Value OptionalNumber(const std::optional<double>& value) {
  return value ? Value(*value) : Value(Null{});
}

Value OptionalString(const std::optional<std::string>& value) {
  return value ? Value(*value) : Value(Null{});
}

std::optional<double> ChangePercent(double current, double baseline) {
  if (!std::isfinite(current) || !std::isfinite(baseline) || baseline <= 0.0) {
    return std::nullopt;
  }
  return ((current - baseline) / baseline) * 100.0;
}

BaselineComparison Unavailable(std::string status, std::string reason) {
  BaselineComparison comparison;
  comparison.status = std::move(status);
  comparison.reason = std::move(reason);
  return comparison;
}

std::string DisplayPath(const std::filesystem::path& path) {
  const auto utf8 = path.generic_u8string();
  return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

}  // namespace

BaselineComparison CompareBaseline(const std::filesystem::path& path, const std::string& bench,
                                   const std::string& config, const LatencyMetrics& current) {
  if (path.empty()) {
    return {};
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return Unavailable("unavailable", "baseline file could not be opened");
  }
  std::ostringstream contents;
  contents << input.rdbuf();
  if (input.bad()) {
    return Unavailable("unavailable", "baseline file could not be read");
  }

  const auto parsed = azookey::ipc::json::Parse(contents.str());
  if (!parsed || !parsed->IsObject()) {
    return Unavailable("incompatible", "baseline is not a JSON object");
  }
  const auto version = parsed->GetInt("schemaVersion");
  const auto baseline_bench = parsed->GetString("bench");
  const auto baseline_commit = parsed->GetString("commit");
  const auto baseline_config = parsed->GetString("config");
  const auto* latency = parsed->GetObject("latencyMs");
  if (!version || *version != kBenchmarkSchemaVersion || !baseline_bench || !baseline_commit ||
      !baseline_config || latency == nullptr) {
    return Unavailable("incompatible", "baseline schema is incomplete or unsupported");
  }
  if (*baseline_bench != bench) {
    return Unavailable("incompatible", "baseline bench name does not match");
  }
  if (*baseline_config != config) {
    return Unavailable("incompatible", "baseline build config does not match");
  }

  const Value latency_value(*latency);
  const auto p95 = latency_value.GetNumber("p95");
  const auto p99 = latency_value.GetNumber("p99");
  if (!p95 || !p99) {
    return Unavailable("incompatible", "baseline latency metrics are missing");
  }

  auto p95_change = ChangePercent(current.p95_ms, *p95);
  auto p99_change = ChangePercent(current.p99_ms, *p99);
  if (!p95_change || !p99_change) {
    return Unavailable("incompatible", "baseline latency metrics are not comparable");
  }

  BaselineComparison comparison;
  comparison.status = "compared";
  comparison.commit = *baseline_commit;
  comparison.p95_change_percent = *p95_change;
  comparison.p99_change_percent = *p99_change;
  const double p95_change_ms = current.p95_ms - *p95;
  const double p99_change_ms = current.p99_ms - *p99;
  comparison.warning =
      (*p95_change > comparison.warning_percent && p95_change_ms > comparison.minimum_change_ms) ||
      (*p99_change > comparison.warning_percent && p99_change_ms > comparison.minimum_change_ms);
  comparison.reason = comparison.warning ? "p95 or p99 regression exceeds warning threshold"
                                         : "within regression warning threshold or noise floor";
  return comparison;
}

std::string SerializeBenchmarkResult(const BenchmarkResult& result) {
  Object latency;
  latency.emplace("max", Value(result.latency.max_ms));
  latency.emplace("p50", Value(result.latency.p50_ms));
  latency.emplace("p95", Value(result.latency.p95_ms));
  latency.emplace("p99", Value(result.latency.p99_ms));

  Object threshold;
  threshold.emplace("maxP95Ms", OptionalNumber(result.max_p95_ms));
  threshold.emplace("passed", Value(result.threshold_passed));

  Object baseline;
  baseline.emplace("commit", OptionalString(result.baseline.commit));
  baseline.emplace("minimumChangeMs", Value(result.baseline.minimum_change_ms));
  baseline.emplace("p95ChangePercent", OptionalNumber(result.baseline.p95_change_percent));
  baseline.emplace("p99ChangePercent", OptionalNumber(result.baseline.p99_change_percent));
  baseline.emplace("reason", Value(result.baseline.reason));
  baseline.emplace("status", Value(result.baseline.status));
  baseline.emplace("warning", Value(result.baseline.warning));
  baseline.emplace("warningPercent", Value(result.baseline.warning_percent));

  Object root;
  root.emplace("baseline", Value(std::move(baseline)));
  root.emplace("bench", Value(result.bench));
  root.emplace("commit", Value(result.commit));
  root.emplace("config", Value(result.config));
  root.emplace("iterations", Value(static_cast<uint64_t>(result.iterations)));
  root.emplace("latencyMs", Value(std::move(latency)));
  root.emplace("schemaVersion", Value(kBenchmarkSchemaVersion));
  root.emplace("threshold", Value(std::move(threshold)));
  return azookey::ipc::json::Stringify(Value(std::move(root)));
}

bool WriteBenchmarkResult(const std::filesystem::path& path, const std::string& json,
                          std::string* error) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    if (error) *error = "failed to open benchmark JSON output: " + DisplayPath(path);
    return false;
  }
  output << json << '\n';
  output.close();
  if (!output) {
    if (error) *error = "failed to write benchmark JSON output: " + DisplayPath(path);
    return false;
  }
  return true;
}

std::optional<std::string> RegressionWarning(const BenchmarkResult& result) {
  if (!result.baseline.warning || !result.baseline.p95_change_percent ||
      !result.baseline.p99_change_percent) {
    return std::nullopt;
  }
  std::ostringstream message;
  message << "benchmark regression warning: p95=" << *result.baseline.p95_change_percent
          << "% p99=" << *result.baseline.p99_change_percent
          << "% baseline=" << result.baseline.commit.value_or("unknown")
          << " threshold=" << result.baseline.warning_percent
          << "% minimum_change_ms=" << result.baseline.minimum_change_ms;
  return message.str();
}

}  // namespace azookey::bench
