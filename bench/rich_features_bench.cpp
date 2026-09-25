#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "BenchmarkCommandLine.h"
#include "BenchmarkCommit.h"
#include "BenchmarkResult.h"
#include "azookey/core/SimpleConverter.h"
#include "azookey/host/InferenceEngine.h"

#ifndef AZOOKEY_BENCH_COMMIT
#define AZOOKEY_BENCH_COMMIT "unknown"
#endif

#ifndef AZOOKEY_BENCH_CONFIG
#define AZOOKEY_BENCH_CONFIG "unknown"
#endif

namespace {

constexpr int kWarmupIterations = 20;
constexpr int kMeasuredIterations = 200;
constexpr double kMaxP95Ms = 30.0;

double Percentile(const std::vector<double>& sorted, double percentile) {
  const auto index = static_cast<size_t>((percentile / 100.0) * (sorted.size() - 1));
  return sorted[index];
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> utf8_args;
  try {
    utf8_args = azookey::bench::Utf8CommandLineArguments(argc, argv);
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << '\n';
    return 2;
  }

  bool json_output = false;
  std::filesystem::path output_path;
  std::filesystem::path baseline_path;
  double max_p95_ms = kMaxP95Ms;
  try {
    for (size_t i = 1; i < utf8_args.size(); ++i) {
      const auto& arg = utf8_args[i];
      if (arg == "--help" || arg == "-h") {
        std::cout << "Usage: azookey_rich_features_bench [--json] [--output PATH]"
                     " [--baseline PATH] [--max-p95-ms N]\n";
        return 0;
      }
      if (arg == "--json") {
        json_output = true;
        continue;
      }
      if (i + 1 >= utf8_args.size()) throw std::invalid_argument(arg + " requires a value");
      const auto& value = utf8_args[++i];
      if (arg == "--output") {
        output_path = azookey::bench::Utf8Path(value);
      } else if (arg == "--baseline") {
        baseline_path = azookey::bench::Utf8Path(value);
      } else if (arg == "--max-p95-ms") {
        size_t parsed_length = 0;
        max_p95_ms = std::stod(value, &parsed_length);
        if (parsed_length != value.size() || !std::isfinite(max_p95_ms) || max_p95_ms <= 0.0)
          throw std::invalid_argument("--max-p95-ms must be a positive finite number");
      } else {
        throw std::invalid_argument("unknown option: " + arg);
      }
    }
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << '\n';
    return 2;
  }

  azookey::host::InferenceEngine engine(std::make_unique<azookey::core::SimpleConverter>(),
                                        nullptr, {});
  const std::vector<std::string> inputs = {"わたし", "にほん", "とうきょう", "かなへんかん",
                                           "にほん"};
  const auto query = [&](int iteration) {
    const auto& kana = inputs[static_cast<size_t>(iteration) % inputs.size()];
    return engine.QueryLiveConversion(kana, "", 0, nullptr);
  };
  for (int i = 0; i < kWarmupIterations; ++i) {
    if (!query(i)) {
      std::cerr << "live conversion returned no candidate during warm-up\n";
      return 2;
    }
  }

  std::vector<double> latency_ms;
  latency_ms.reserve(kMeasuredIterations);
  for (int i = 0; i < kMeasuredIterations; ++i) {
    const auto start = std::chrono::steady_clock::now();
    const auto candidate = query(i);
    const auto end = std::chrono::steady_clock::now();
    if (!candidate) {
      std::cerr << "live conversion returned no candidate\n";
      return 2;
    }
    latency_ms.push_back(std::chrono::duration<double, std::milli>(end - start).count());
  }
  std::sort(latency_ms.begin(), latency_ms.end());

  azookey::bench::BenchmarkResult result;
  result.bench = "azookey_rich_features_bench";
  result.commit = AZOOKEY_BENCH_COMMIT;
  result.config = AZOOKEY_BENCH_CONFIG;
  result.iterations = latency_ms.size();
  result.latency = {Percentile(latency_ms, 50), Percentile(latency_ms, 95),
                    Percentile(latency_ms, 99), latency_ms.back()};
  result.max_p95_ms = max_p95_ms;
  result.threshold_passed = result.latency.p95_ms <= max_p95_ms;
  result.baseline =
      azookey::bench::CompareBaseline(baseline_path, result.bench, result.config, result.latency);
  const auto serialized = azookey::bench::SerializeBenchmarkResult(result);
  if (!output_path.empty()) {
    std::string error;
    if (!azookey::bench::WriteBenchmarkResult(output_path, serialized, &error)) {
      std::cerr << error << '\n';
      return 2;
    }
  }
  if (json_output) {
    std::cout << serialized << '\n';
  } else {
    std::cout << "live_conversion warmup=" << kWarmupIterations
              << " iterations=" << kMeasuredIterations << " p50_ms=" << result.latency.p50_ms
              << " p95_ms=" << result.latency.p95_ms << " p99_ms=" << result.latency.p99_ms
              << " max_ms=" << result.latency.max_ms << " max_p95_ms=" << max_p95_ms << '\n';
  }
  if (const auto warning = azookey::bench::RegressionWarning(result))
    std::cerr << *warning << '\n';
  if (!result.threshold_passed) {
    std::cerr << "live conversion p95 exceeded threshold\n";
    return 1;
  }
  return 0;
}
