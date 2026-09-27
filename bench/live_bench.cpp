#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "BenchmarkCommandLine.h"
#include "BenchmarkCommit.h"
#include "BenchmarkResult.h"
#include "ConversionQuality.h"
#include "IpcBenchmark.h"
#include "TemporaryLearningFile.h"
#include "azookey/core/SimpleConverter.h"
#include "azookey/host/InferenceEngine.h"
#include "azookey/ipc/TraceId.h"
#include "azookey/logging/Phase.h"
#include "azookey/logging/RuntimeLogger.h"

#ifndef AZOOKEY_BENCH_COMMIT
#define AZOOKEY_BENCH_COMMIT "unknown"
#endif

#ifndef AZOOKEY_BENCH_CONFIG
#define AZOOKEY_BENCH_CONFIG "unknown"
#endif

namespace {

constexpr size_t kCurrentZenzaiBeamWidth = 4;
constexpr size_t kCurrentZenzaiMaxNewTokens = 64;
constexpr size_t kCurrentZenzaiContextBatchSize = 512;
constexpr size_t kCurrentZenzaiPromptTemplateVersion = 1;

void PrintUsage(const char* exe) {
  std::cerr << "Usage: " << exe
            << " [--max-p95-ms N] [--ipc] [--json] [--output PATH] [--baseline PATH]"
               " [--backend cpu|cuda] [--model PATH] [--trace [--trace-output PATH]]\n"
            << "       " << exe
            << " --eval JSONL --output JSON [--per-case JSONL] [--baseline JSON]"
               " [--backend cpu|cuda] [--model PATH] [--category NAME]"
               " [--iterations N] [--typo-mode off] [--trace [--trace-output PATH]]\n"
            << "Reports latency metrics by default. Pass --max-p95-ms to make p95 a hard "
               "failure gate. --json selects JSON stdout; --output writes the same JSON "
               "schema to a file. --ipc adds codec and Windows pipe echo timings. "
               "--eval selects the conversion-quality evaluator. --trace writes phase JSONL "
               "to trace.jsonl by default.\n";
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

azookey::logging::RuntimeLoggerOptions TraceLoggerOptions(const std::filesystem::path& output_path,
                                                          std::string component) {
  azookey::logging::RuntimeLoggerOptions options;
  options.enabled = true;
  options.component = std::move(component);
  options.output_path = output_path;
  // A benchmark trace must retain all samples instead of rotating at the runtime default (5 MiB).
  options.max_file_bytes = uintmax_t{1} << 40;
  return options;
}

std::vector<azookey::core::Candidate> QueryCandidates(
    azookey::host::InferenceEngine& engine, const std::string& kana, const std::string& context,
    uint64_t now_epoch_sec, uint32_t max_candidates, azookey::logging::RuntimeLogger* bench_logger,
    uint64_t& request_id, const std::string& backend) {
  if (!bench_logger) {
    return engine.QueryCandidates(kana, context, now_epoch_sec, nullptr, max_candidates, false);
  }
  const auto trace_id = azookey::ipc::GenerateTraceId();
  const azookey::host::InferenceTelemetry telemetry{++request_id, {}, trace_id};
  const auto start = std::chrono::steady_clock::now();
  auto candidates = engine.QueryCandidates(kana, context, now_epoch_sec, nullptr, max_candidates,
                                           false, &telemetry);
  const auto latency_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  bench_logger->Log(azookey::logging::RuntimeLogLevel::Info, "latency_phase",
                    {{"request_id", telemetry.request_id},
                     {"trace_id", azookey::logging::RuntimeLogSafeText(trace_id)},
                     {"phase", azookey::logging::RuntimeLogSafeText(std::string(
                                   azookey::logging::PhaseName(azookey::logging::Phase::Total)))},
                     {"latency_ms", latency_ms},
                     {"backend", azookey::logging::RuntimeLogSafeText(backend)},
                     {"result", azookey::logging::RuntimeLogSafeText("ok")}});
  return candidates;
}

bool SamePath(const std::filesystem::path& left, const std::filesystem::path& right) {
  return !left.empty() && !right.empty() &&
         std::filesystem::weakly_canonical(std::filesystem::absolute(left)) ==
             std::filesystem::weakly_canonical(std::filesystem::absolute(right));
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
  bool ipc_benchmark = false;
  std::filesystem::path output_path;
  std::filesystem::path baseline_path;
  std::filesystem::path trace_output_path;
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
      } else if (arg == "--ipc") {
        ipc_benchmark = true;
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
      } else if (arg == "--trace-output") {
        const auto value = RequireValue(argc, argv, i, "--trace-output");
        if (value.empty()) throw std::invalid_argument("--trace-output requires a non-empty path");
        trace_output_path = azookey::bench::Utf8Path(value);
      } else {
        throw std::invalid_argument("unknown option: " + arg);
      }
    }
    if (quality_options.typo_mode != "off") {
      throw std::invalid_argument("--typo-mode supports only off until M55");
    }
    if (quality_options.eval_path.empty() &&
        (!quality_options.per_case_path.empty() || quality_options.category != "all" ||
         quality_options.typo_mode != "off")) {
      throw std::invalid_argument("conversion-quality options require --eval");
    }
    if (!trace_output_path.empty() && !quality_options.trace) {
      throw std::invalid_argument("--trace-output requires --trace");
    }
    if (quality_options.trace && trace_output_path.empty()) {
      trace_output_path = "trace.jsonl";
    }
    if (quality_options.trace &&
        (SamePath(trace_output_path, output_path) ||
         SamePath(trace_output_path, quality_options.per_case_path) ||
         SamePath(trace_output_path, quality_options.eval_path) ||
         SamePath(trace_output_path, baseline_path) ||
         SamePath(trace_output_path, azookey::bench::Utf8Path(quality_options.model)))) {
      throw std::invalid_argument("--trace-output must differ from input and result paths");
    }
    if (!quality_options.eval_path.empty() && max_p95_ms) {
      throw std::invalid_argument("--max-p95-ms cannot be combined with --eval");
    }
    if (!quality_options.eval_path.empty() && (json_output || ipc_benchmark)) {
      throw std::invalid_argument("--json and --ipc cannot be combined with --eval");
    }
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << std::endl;
    PrintUsage(argv[0]);
    return 2;
  }

  std::optional<azookey::bench::TemporaryLearningFile> learning_file;
  try {
    learning_file.emplace();
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << std::endl;
    return 2;
  }

  std::optional<azookey::logging::RuntimeLogger> trace_logger;
  if (quality_options.trace) {
    try {
      const auto directory = trace_output_path.parent_path();
      if (!directory.empty()) std::filesystem::create_directories(directory);
      std::ofstream trace_output(trace_output_path, std::ios::binary | std::ios::trunc);
      if (!trace_output) throw std::runtime_error("cannot create trace output");
      trace_logger.emplace(TraceLoggerOptions(trace_output_path, "bench"));
    } catch (const std::exception& ex) {
      std::cerr << ex.what() << std::endl;
      return 2;
    }
  }

  // Keep store and engine after learning_file: engine shutdown flushes learning,
  // and the atomic writer would recreate the directory if cleanup ran first.
  azookey::learning::LearningStore store(learning_file->Path());
  azookey::host::EngineConfig engine_config;
  engine_config.backend = quality_options.backend == "cuda" ? azookey::host::BackendKind::Cuda
                                                            : azookey::host::BackendKind::Cpu;
  engine_config.model_path = quality_options.model;
  azookey::host::InferenceEngine engine(std::make_unique<azookey::core::SimpleConverter>(), &store,
                                        engine_config, trace_logger ? &*trace_logger : nullptr);
  if (!quality_options.model.empty() && !engine.LoadModel()) {
    std::cerr << "failed to load model" << std::endl;
    return 2;
  }

  if (!quality_options.eval_path.empty()) {
    quality_options.output_path = output_path;
    quality_options.baseline_path = baseline_path;
    quality_options.build_id = AZOOKEY_BENCH_COMMIT;
    if (!quality_options.model.empty()) {
      quality_options.decode = "beam";
      quality_options.beam_width = kCurrentZenzaiBeamWidth;
      quality_options.max_new_tokens = kCurrentZenzaiMaxNewTokens;
      quality_options.prompt_template_version = kCurrentZenzaiPromptTemplateVersion;
      // llama.cpp's effective thread count is not exposed by the converter yet (DEV-858).
      // Zero records that the compatibility value is unknown instead of reporting a false default.
      quality_options.thread_count = 0;
      quality_options.batch_size = kCurrentZenzaiContextBatchSize;
    }
    std::string error;
    uint64_t request_id = 0;
    const bool ok = azookey::bench::RunConversionQualityEvaluation(
        quality_options,
        [&engine, &request_id, &trace_logger, &quality_options](const std::string& input,
                                                                const std::string& context) {
          const auto now = static_cast<uint64_t>(
              std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
          return QueryCandidates(
              engine, input, context, now,
              static_cast<uint32_t>(azookey::bench::kConversionQualityCandidateLimit),
              trace_logger ? &*trace_logger : nullptr, request_id, quality_options.backend);
        },
        &error);
    if (!ok) {
      std::cerr << error << std::endl;
      return 2;
    }
    return 0;
  }

  if (quality_options.model.empty()) engine.LoadModel();

  const std::vector<std::string> inputs = {"わたし", "にほん", "とうきょう", "かなへんかん",
                                           "にほん"};
  std::vector<double> lat_ms;
  uint64_t request_id = 0;
  for (int i = 0; i < 200; ++i) {
    const auto& kana = inputs[static_cast<size_t>(i) % inputs.size()];
    auto t0 = std::chrono::steady_clock::now();
    auto now = static_cast<uint64_t>(
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
    (void)QueryCandidates(engine, kana, "", now, 0, trace_logger ? &*trace_logger : nullptr,
                          request_id, quality_options.backend);
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
  if (ipc_benchmark) {
    try {
      result.ipc_phases = azookey::bench::RunIpcBenchmark();
    } catch (const std::exception& ex) {
      std::cerr << ex.what() << std::endl;
      return 2;
    }
  }
  const auto json = azookey::bench::SerializeBenchmarkResult(result);

  if (!output_path.empty()) {
    std::string error;
    if (!azookey::bench::WriteBenchmarkResult(output_path, json, &error)) {
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
    if (result.ipc_phases) {
      const auto print_phase = [](const char* name, const azookey::bench::LatencyMetrics& m) {
        std::cout << name << " p50_ms=" << m.p50_ms << " p95_ms=" << m.p95_ms
                  << " p99_ms=" << m.p99_ms << " max_ms=" << m.max_ms << '\n';
      };
      print_phase("ipc_serialize", result.ipc_phases->serialize);
      print_phase("ipc_framing", result.ipc_phases->framing);
      print_phase("ipc_deserialize", result.ipc_phases->deserialize);
      if (result.ipc_phases->pipe_round_trip) {
        print_phase("ipc_pipe_round_trip", *result.ipc_phases->pipe_round_trip);
      } else {
        std::cout << "ipc_pipe_round_trip=unavailable (Windows only)\n";
      }
    }
  }
  if (const auto warning = azookey::bench::RegressionWarning(result)) {
    std::cerr << *warning << std::endl;
  }

  if (!result.threshold_passed) {
    std::cerr << "p95 exceeded threshold" << std::endl;
    return 1;
  }
  return 0;
}
