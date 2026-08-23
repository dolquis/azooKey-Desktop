#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>

namespace azookey::bench {

inline constexpr int kBenchmarkSchemaVersion = 1;
inline constexpr double kDefaultRegressionWarningPercent = 10.0;

struct LatencyMetrics {
  double p50_ms{0.0};
  double p95_ms{0.0};
  double p99_ms{0.0};
  double max_ms{0.0};
};

struct BaselineComparison {
  std::string status{"not_provided"};
  std::optional<std::string> commit;
  std::optional<double> p95_change_percent;
  std::optional<double> p99_change_percent;
  double warning_percent{kDefaultRegressionWarningPercent};
  bool warning{false};
  std::string reason{"baseline not provided"};
};

struct BenchmarkResult {
  std::string bench;
  std::string commit;
  std::string config;
  size_t iterations{0};
  LatencyMetrics latency;
  std::optional<double> max_p95_ms;
  bool threshold_passed{true};
  BaselineComparison baseline;
};

BaselineComparison CompareBaseline(const std::filesystem::path& path, const std::string& bench,
                                   const std::string& config, const LatencyMetrics& current);
std::string SerializeBenchmarkResult(const BenchmarkResult& result);
bool WriteBenchmarkResult(const std::filesystem::path& path, const std::string& json,
                          std::string* error);
std::optional<std::string> RegressionWarning(const BenchmarkResult& result);

}  // namespace azookey::bench
