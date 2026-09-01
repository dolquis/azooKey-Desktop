#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "BenchmarkCommandLine.h"
#include "BenchmarkCommit.h"
#include "BenchmarkResult.h"
#include "ConversionQuality.h"
#include "azookey/core/SimpleConverter.h"
#include "azookey/host/InferenceEngine.h"

#ifndef AZOOKEY_BENCH_COMMIT
#define AZOOKEY_BENCH_COMMIT "unknown"
#endif

#ifndef AZOOKEY_BENCH_CONFIG
#define AZOOKEY_BENCH_CONFIG "unknown"
#endif

namespace {

void PrintUsage(const char* exe) {
  std::cerr << "Usage: " << exe << " [--max-p95-ms N] [--json] [--output PATH] [--baseline PATH]\n"
            << "       " << exe
            << " --eval JSONL --output JSON [--per-case JSONL] [--baseline JSON]"
               " [--backend cpu|cuda] [--model PATH] [--category NAME]"
               " [--iterations N] [--typo-mode off] [--trace]\n"
            << "Reports latency metrics by default. Pass --max-p95-ms to make p95 a hard "
               "failure gate. --json selects JSON stdout; --output writes the same JSON "
               "schema to a file. --eval selects the conversion-quality evaluator.\n";
}

double ParseDouble(const std::string& value, const char* name) {
  try {
    size_t idx = 0;
    const auto parsed = std::stod(value, &idx);
    if (idx != value.size()) {
      throw std::invalid_argument("trailing characters");
    }
    return parsed;
  } catch (const std::exception& ex) {
    throw std::invalid_argument(std::string(name) + " must be a number: " + ex.what());
  }
}

std::string RequireValue(int argc, char** argv, int& index, const char* option) {
  if (index + 1 >= argc) {
    throw std::invalid_argument(std::string(option) + " requires a value");
  }
  ++index;
  return argv[index];
}

size_t ParseSize(const std::string& value, const char* name) {
  try {
    size_t index = 0;
    const auto parsed = std::stoull(value, &index);
    if (index != value.size() || parsed == 0) throw std::invalid_argument("invalid value");
    return static_cast<size_t>(parsed);
  } catch (const std::exception& ex) {
    throw std::invalid_argument(std::string(name) + " must be a positive integer: " + ex.what());
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> utf8_args;
  try {
    utf8_args = azookey::bench::Utf8CommandLineArguments(argc, argv);
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << std::endl;
    return 2;
  }
  std::vector<char*> utf8_argv;
  utf8_argv.reserve(utf8_args.size());
  for (auto& arg : utf8_args) {
    utf8_argv.push_back(arg.data());
  }
  argc = static_cast<int>(utf8_argv.size());
  argv = utf8_argv.data();

  std::optional<double> max_p95_ms;
  bool json_output = false;
  std::filesystem::path output_path;
  std::filesystem::path baseline_path;
  azookey::bench::ConversionQualityOptions quality_options;
  try {
    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--help" || arg == "-h") {
        PrintUsage(argv[0]);
        return 0;
      }
      if (arg == "--max-p95-ms") {
        max_p95_ms = ParseDouble(RequireValue(argc, argv, i, "--max-p95-ms"), "--max-p95-ms");
      } else if (arg == "--json") {
        json_output = true;
      } else if (arg == "--output") {
        output_path = azookey::bench::Utf8Path(RequireValue(argc, argv, i, "--output"));
      } else if (arg == "--baseline") {
        baseline_path = azookey::bench::Utf8Path(RequireValue(argc, argv, i, "--baseline"));
      } else if (arg == "--eval") {
        quality_options.eval_path = azookey::bench::Utf8Path(RequireValue(argc, argv, i, "--eval"));
      } else if (arg == "--per-case") {
        quality_options.per_case_path =
            azookey::bench::Utf8Path(RequireValue(argc, argv, i, "--per-case"));
      } else if (arg == "--backend") {
        quality_options.backend = RequireValue(argc, argv, i, "--backend");
        if (quality_options.backend != "cpu" && quality_options.backend != "cuda") {
          throw std::invalid_argument("--backend must be cpu or cuda");
        }
      } else if (arg == "--model") {
        quality_options.model = RequireValue(argc, argv, i, "--model");
      } else if (arg == "--category") {
        quality_options.category = RequireValue(argc, argv, i, "--category");
      } else if (arg == "--iterations") {
        quality_options.iterations =
            ParseSize(RequireValue(argc, argv, i, "--iterations"), "--iterations");
      } else if (arg == "--typo-mode") {
        quality_options.typo_mode = RequireValue(argc, argv, i, "--typo-mode");
      } else if (arg == "--trace") {
        quality_options.trace = true;
      } else {
        throw std::invalid_argument("unknown option: " + arg);
      }
    }
    if (quality_options.eval_path.empty() &&
        (!quality_options.per_case_path.empty() || !quality_options.model.empty() ||
         quality_options.backend != "cpu" || quality_options.category != "all" ||
         quality_options.typo_mode != "off" || quality_options.trace)) {
      throw std::invalid_argument("conversion-quality options require --eval");
    }
    if (!quality_options.eval_path.empty() && max_p95_ms) {
      throw std::invalid_argument("--max-p95-ms cannot be combined with --eval");
    }
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << std::endl;
    PrintUsage(argv[0]);
    return 2;
  }

  const auto learning_path = std::filesystem::temp_directory_path() / "azookey_bench_learning.tsv";
  std::remove(learning_path.string().c_str());

  azookey::learning::LearningStore store(learning_path.string());
  azookey::host::EngineConfig engine_config;
  engine_config.backend = quality_options.backend == "cuda" ? azookey::host::BackendKind::Cuda
                                                            : azookey::host::BackendKind::Cpu;
  engine_config.model_path = quality_options.model;
  azookey::host::InferenceEngine engine(std::make_unique<azookey::core::SimpleConverter>(), &store,
                                        engine_config);
  if (!quality_options.model.empty() && !engine.LoadModel()) {
    std::remove(learning_path.string().c_str());
    std::cerr << "failed to load evaluation model" << std::endl;
    return 2;
  }

  if (!quality_options.eval_path.empty()) {
    quality_options.output_path = output_path;
    quality_options.baseline_path = baseline_path;
    quality_options.build_id = AZOOKEY_BENCH_COMMIT;
    std::string error;
    const bool ok = azookey::bench::RunConversionQualityEvaluation(
        quality_options,
        [&engine](const std::string& input, const std::string& context) {
          const auto now = static_cast<uint64_t>(
              std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
          return engine.QueryCandidates(input, context, now, nullptr, 5, false);
        },
        &error);
    std::remove(learning_path.string().c_str());
    if (!ok) {
      std::cerr << error << std::endl;
      return 2;
    }
    return 0;
  }

  engine.LoadModel();

  const std::vector<std::string> inputs = {"わたし", "にほん", "とうきょう", "かなへんかん",
                                           "にほん"};
  std::vector<double> lat_ms;
  for (int i = 0; i < 200; ++i) {
    const auto& kana = inputs[static_cast<size_t>(i) % inputs.size()];
    auto t0 = std::chrono::steady_clock::now();
    auto now = static_cast<uint64_t>(
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
    (void)engine.QueryCandidates(kana, "", now);
    auto t1 = std::chrono::steady_clock::now();
    lat_ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
  }

  std::sort(lat_ms.begin(), lat_ms.end());
  auto pct = [&](double p) {
    const size_t idx = static_cast<size_t>((p / 100.0) * static_cast<double>(lat_ms.size() - 1));
    return lat_ms[idx];
  };

  const double p50 = pct(50);
  const double p95 = pct(95);
  const double p99 = pct(99);

  azookey::bench::BenchmarkResult result;
  result.bench = "azookey_bench";
  result.commit = AZOOKEY_BENCH_COMMIT;
  result.config = AZOOKEY_BENCH_CONFIG;
  result.iterations = lat_ms.size();
  result.latency = {p50, p95, p99, lat_ms.back()};
  result.max_p95_ms = max_p95_ms;
  result.threshold_passed = !max_p95_ms || p95 < *max_p95_ms;
  result.baseline =
      azookey::bench::CompareBaseline(baseline_path, result.bench, result.config, result.latency);
  const auto json = azookey::bench::SerializeBenchmarkResult(result);

  if (!output_path.empty()) {
    std::string error;
    if (!azookey::bench::WriteBenchmarkResult(output_path, json, &error)) {
      std::remove(learning_path.string().c_str());
      std::cerr << error << std::endl;
      return 2;
    }
  }

  if (json_output) {
    std::cout << json << std::endl;
  } else {
    std::cout << "p50_ms=" << p50 << " p95_ms=" << p95 << " p99_ms=" << p99;
    if (max_p95_ms) {
      std::cout << " max_p95_ms=" << *max_p95_ms;
    } else {
      std::cout << " max_p95_ms=none";
    }
    std::cout << std::endl;
  }
  if (const auto warning = azookey::bench::RegressionWarning(result)) {
    std::cerr << *warning << std::endl;
  }

  std::remove(learning_path.string().c_str());
  if (!result.threshold_passed) {
    std::cerr << "p95 exceeded threshold" << std::endl;
    return 1;
  }
  return 0;
}
