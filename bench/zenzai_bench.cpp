#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include "BenchmarkCommandLine.h"
#include "BenchmarkCommit.h"
#include "BenchmarkResult.h"
#include "azookey/core/SimpleConverter.h"
#include "azookey/host/InferenceEngine.h"
#include "azookey/host/ZenzaiModelConverter.h"

#ifndef AZOOKEY_WITH_LLAMA_CPP
#define AZOOKEY_WITH_LLAMA_CPP 0
#endif

#ifndef AZOOKEY_BENCH_COMMIT
#define AZOOKEY_BENCH_COMMIT "unknown"
#endif

#ifndef AZOOKEY_BENCH_CONFIG
#define AZOOKEY_BENCH_CONFIG "unknown"
#endif

namespace {

struct Options {
  std::string model_path;
  std::string input = "にほんご";
  std::vector<std::string> sequence_inputs;
  std::vector<std::string> sequence_contexts;
  std::string context;
  size_t iterations{50};
  size_t warmup{3};
  bool require_model{false};
  bool mock_zenzai{false};
  bool require_zenzai{false};
  bool json_output{false};
  std::filesystem::path output_path;
  std::filesystem::path baseline_path;
  std::optional<std::vector<int32_t>> expected_prompt_token_ids;
  std::optional<double> max_p95_ms;
  azookey::host::BackendKind backend{azookey::host::BackendKind::Cpu};
  std::optional<int32_t> n_gpu_layers;
  std::optional<int32_t> n_threads;
  std::optional<size_t> cancel_probe_delay_ms;
};

void PrintUsage(const char* exe) {
  std::cerr << "Usage: " << exe
            << " [--model PATH] [--input KANA] [--sequence-input KANA]"
               " [--context TEXT] [--sequence-context TEXT] [--cancel-probe-ms N] [--iterations N]"
               " [--warmup N] [--backend cpu|cuda] [--n-gpu-layers N] [--threads N]"
               " [--max-p95-ms N] [--require-model] [--require-zenzai]"
               " [--json] [--output PATH] [--baseline PATH]"
               " [--expected-prompt-token-ids ID[,ID...]]"
               " [--mock-zenzai]\n"
            << "If --model is omitted, AZOOKEY_ZENZAI_MODEL is used when set; otherwise the "
               "bench reports status=skipped and exits 0 unless --require-model is present. "
               "--json selects JSON stdout; --output writes the same JSON schema to a file.\n";
}

std::string EnvOrEmpty(const char* name) {
#if defined(_MSC_VER)
  char* value = nullptr;
  size_t len = 0;
  if (_dupenv_s(&value, &len, name) != 0 || value == nullptr) {
    return {};
  }
  std::string result(value, len > 0 ? len - 1 : 0);
  std::free(value);
  return result;
#else
  const char* value = std::getenv(name);
  return value ? std::string(value) : std::string();
#endif
}

size_t ParseSize(const std::string& value, const char* name) {
  try {
    if (!value.empty() && value.front() == '-') {
      throw std::invalid_argument("negative value");
    }
    size_t idx = 0;
    const auto parsed = std::stoull(value, &idx, 10);
    if (idx != value.size()) {
      throw std::invalid_argument("trailing characters");
    }
    return static_cast<size_t>(parsed);
  } catch (const std::exception& ex) {
    throw std::invalid_argument(std::string(name) +
                                " must be a non-negative integer: " + ex.what());
  }
}

int32_t ParseI32(const std::string& value, const char* name) {
  try {
    size_t idx = 0;
    const auto parsed = std::stol(value, &idx, 10);
    if (idx != value.size()) {
      throw std::invalid_argument("trailing characters");
    }
    return static_cast<int32_t>(parsed);
  } catch (const std::exception& ex) {
    throw std::invalid_argument(std::string(name) + " must be an integer: " + ex.what());
  }
}

std::vector<int32_t> ParseTokenIds(const std::string& value, const char* name) {
  if (value.empty()) {
    throw std::invalid_argument(std::string(name) + " must not be empty");
  }
  std::vector<int32_t> token_ids;
  size_t offset = 0;
  while (offset <= value.size()) {
    const auto separator = value.find(',', offset);
    const auto token = value.substr(
        offset, separator == std::string::npos ? std::string::npos : separator - offset);
    if (token.empty()) {
      throw std::invalid_argument(std::string(name) + " contains an empty token ID");
    }
    size_t parsed_length = 0;
    long long parsed = 0;
    try {
      parsed = std::stoll(token, &parsed_length, 10);
    } catch (const std::exception& ex) {
      throw std::invalid_argument(std::string(name) + " must contain integers: " + ex.what());
    }
    if (parsed_length != token.size() || parsed < 0 ||
        parsed > std::numeric_limits<int32_t>::max()) {
      throw std::invalid_argument(std::string(name) + " contains an invalid token ID: " + token);
    }
    token_ids.push_back(static_cast<int32_t>(parsed));
    if (separator == std::string::npos) {
      break;
    }
    offset = separator + 1;
  }
  return token_ids;
}

std::string FormatTokenIds(const std::vector<int32_t>& token_ids) {
  std::string result;
  for (size_t i = 0; i < token_ids.size(); ++i) {
    if (i > 0) {
      result += ',';
    }
    result += std::to_string(token_ids[i]);
  }
  return result;
}

std::string FormatBoolSamples(const std::vector<bool>& samples) {
  std::string result;
  for (size_t i = 0; i < samples.size(); ++i) {
    if (i > 0) {
      result += ',';
    }
    result += samples[i] ? '1' : '0';
  }
  return result;
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

uint32_t CurrentProcessId() {
#if defined(_WIN32)
  return static_cast<uint32_t>(_getpid());
#else
  return static_cast<uint32_t>(getpid());
#endif
}

std::filesystem::path UniqueTempPath(const std::string& stem, const std::string& extension) {
  static std::atomic<uint64_t> counter{0};
  const auto ticks = std::chrono::system_clock::now().time_since_epoch().count();
  const auto sequence = counter.fetch_add(1, std::memory_order_relaxed);
  return std::filesystem::temp_directory_path() /
         (stem + "-" + std::to_string(CurrentProcessId()) + "-" + std::to_string(ticks) + "-" +
          std::to_string(sequence) + extension);
}

std::string RequireValue(int argc, char** argv, int& index, const char* option) {
  if (index + 1 >= argc) {
    throw std::invalid_argument(std::string(option) + " requires a value");
  }
  ++index;
  return argv[index];
}

Options ParseOptions(int argc, char** argv) {
  Options options;
  options.model_path = EnvOrEmpty("AZOOKEY_ZENZAI_MODEL");
  bool model_path_from_cli = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      PrintUsage(argv[0]);
      std::exit(0);
    }
    if (arg == "--model") {
      options.model_path = RequireValue(argc, argv, i, "--model");
      model_path_from_cli = true;
    } else if (arg == "--input") {
      options.input = RequireValue(argc, argv, i, "--input");
    } else if (arg == "--sequence-input") {
      options.sequence_inputs.push_back(RequireValue(argc, argv, i, "--sequence-input"));
    } else if (arg == "--context") {
      options.context = RequireValue(argc, argv, i, "--context");
    } else if (arg == "--sequence-context") {
      options.sequence_contexts.push_back(RequireValue(argc, argv, i, "--sequence-context"));
    } else if (arg == "--cancel-probe-ms") {
      options.cancel_probe_delay_ms =
          ParseSize(RequireValue(argc, argv, i, "--cancel-probe-ms"), "--cancel-probe-ms");
    } else if (arg == "--iterations") {
      options.iterations = ParseSize(RequireValue(argc, argv, i, "--iterations"), "--iterations");
    } else if (arg == "--warmup") {
      options.warmup = ParseSize(RequireValue(argc, argv, i, "--warmup"), "--warmup");
    } else if (arg == "--backend") {
      const auto backend = RequireValue(argc, argv, i, "--backend");
      if (backend == "cpu") {
        options.backend = azookey::host::BackendKind::Cpu;
      } else if (backend == "cuda") {
        options.backend = azookey::host::BackendKind::Cuda;
      } else {
        throw std::invalid_argument("--backend must be cpu or cuda");
      }
    } else if (arg == "--n-gpu-layers") {
      options.n_gpu_layers =
          ParseI32(RequireValue(argc, argv, i, "--n-gpu-layers"), "--n-gpu-layers");
    } else if (arg == "--threads") {
      options.n_threads = ParseI32(RequireValue(argc, argv, i, "--threads"), "--threads");
      if (*options.n_threads <= 0) {
        throw std::invalid_argument("--threads must be greater than 0");
      }
    } else if (arg == "--max-p95-ms") {
      options.max_p95_ms = ParseDouble(RequireValue(argc, argv, i, "--max-p95-ms"), "--max-p95-ms");
    } else if (arg == "--json") {
      options.json_output = true;
    } else if (arg == "--output") {
      options.output_path = azookey::bench::Utf8Path(RequireValue(argc, argv, i, "--output"));
    } else if (arg == "--baseline") {
      options.baseline_path = azookey::bench::Utf8Path(RequireValue(argc, argv, i, "--baseline"));
    } else if (arg == "--expected-prompt-token-ids") {
      options.expected_prompt_token_ids =
          ParseTokenIds(RequireValue(argc, argv, i, "--expected-prompt-token-ids"),
                        "--expected-prompt-token-ids");
    } else if (arg == "--require-model") {
      options.require_model = true;
    } else if (arg == "--mock-zenzai") {
      options.mock_zenzai = true;
    } else if (arg == "--require-zenzai") {
      options.require_zenzai = true;
    } else {
      throw std::invalid_argument("unknown option: " + arg);
    }
  }

  if (options.mock_zenzai && !model_path_from_cli) {
    options.model_path.clear();
  }
  if (options.iterations == 0) {
    throw std::invalid_argument("--iterations must be greater than 0");
  }
  if (!options.sequence_contexts.empty() &&
      options.sequence_contexts.size() != options.sequence_inputs.size()) {
    throw std::invalid_argument("--sequence-context count must match --sequence-input count");
  }
  return options;
}

void WriteMinimalGguf(const std::filesystem::path& path, uint32_t version = 3) {
  std::ofstream out(path, std::ios::binary);
  out.write("GGUF", 4);
  const unsigned char bytes[4] = {
      static_cast<unsigned char>(version & 0xFF),
      static_cast<unsigned char>((version >> 8) & 0xFF),
      static_cast<unsigned char>((version >> 16) & 0xFF),
      static_cast<unsigned char>((version >> 24) & 0xFF),
  };
  out.write(reinterpret_cast<const char*>(bytes), 4);
}

uint64_t NowEpochSec() {
  return static_cast<uint64_t>(
      std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
}

double Percentile(const std::vector<double>& sorted, double percentile) {
  if (sorted.empty()) {
    return 0.0;
  }
  const double clamped = std::clamp(percentile, 0.0, 100.0);
  const size_t idx =
      static_cast<size_t>(std::ceil((clamped / 100.0) * static_cast<double>(sorted.size() - 1)));
  return sorted[idx];
}

bool HasZenzaiTag(const azookey::core::Candidate& candidate) {
  return candidate.source == azookey::core::CandidateSource::Model &&
         candidate.debug_info.rfind("zenzai;", 0) == 0;
}

std::string OptionalString(const std::optional<std::string>& value) {
  return value ? *value : std::string("none");
}

std::string BackendName(azookey::host::BackendKind backend) {
  switch (backend) {
    case azookey::host::BackendKind::Cpu:
      return "cpu";
    case azookey::host::BackendKind::Cuda:
      return "cuda";
  }
  return "unknown";
}

}  // namespace

int RunBench(int argc, char** argv) {
  Options options;
  try {
    options = ParseOptions(argc, argv);
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << std::endl;
    PrintUsage(argv[0]);
    return 2;
  }

  std::optional<std::filesystem::path> mock_model_path;
  if (options.model_path.empty() && options.mock_zenzai && !AZOOKEY_WITH_LLAMA_CPP) {
    mock_model_path = UniqueTempPath("azookey_zenzai_bench_mock", ".gguf");
    WriteMinimalGguf(*mock_model_path);
    options.model_path = mock_model_path->string();
    options.require_zenzai = true;
  }

  if (options.model_path.empty()) {
    if (options.json_output || !options.output_path.empty()) {
      std::cerr << "benchmark result unavailable: model not provided" << std::endl;
      return 2;
    }
    auto& stream = options.json_output ? std::cerr : std::cout;
    stream << "status=skipped reason=model-not-provided llama_cpp=" << AZOOKEY_WITH_LLAMA_CPP
           << " hint=set-AZOOKEY_ZENZAI_MODEL-or-pass---model" << std::endl;
    return options.require_model ? 1 : 0;
  }

  std::optional<std::vector<int32_t>> prompt_token_ids;
  if (options.expected_prompt_token_ids) {
#if AZOOKEY_WITH_LLAMA_CPP
    auto validation_load = azookey::host::LoadZenzaiGgufModel(options.model_path);
    if (!validation_load.ok) {
      std::cerr << "status=prompt-token-validation-load-failed error=" << validation_load.error
                << std::endl;
      return 1;
    }
    azookey::core::SimpleConverter validation_fallback;
    azookey::host::ZenzaiModelConverter validation_converter(std::move(validation_load),
                                                             &validation_fallback);
    azookey::core::ConversionContext validation_context;
    validation_context.preceding_text = options.context;
    try {
      prompt_token_ids =
          validation_converter.TokenizePromptForValidation(options.input, validation_context);
    } catch (const std::exception& ex) {
      std::cerr << "status=prompt-token-validation-failed error=" << ex.what() << std::endl;
      return 1;
    }
    if (*prompt_token_ids != *options.expected_prompt_token_ids) {
      std::cerr << "status=prompt-token-mismatch expected_prompt_token_ids="
                << FormatTokenIds(*options.expected_prompt_token_ids)
                << " actual_prompt_token_ids=" << FormatTokenIds(*prompt_token_ids) << std::endl;
      return 1;
    }
#else
    std::cerr << "status=prompt-token-validation-unavailable reason=llama-cpp-disabled"
              << std::endl;
    return 1;
#endif
  }

  const auto learning_path = UniqueTempPath("azookey_zenzai_bench_learning", ".tsv");
  std::remove(learning_path.string().c_str());

  azookey::learning::LearningStore store(learning_path);
  azookey::host::InferenceEngine engine(std::make_unique<azookey::core::SimpleConverter>(), &store,
                                        {});

  azookey::host::ModelLoadOptions load_options;
  load_options.path = options.model_path;
  load_options.backend = options.backend;
  load_options.n_gpu_layers = options.n_gpu_layers;
  load_options.n_threads = options.n_threads;
  load_options.mock_zenzai_candidates_for_tests = options.mock_zenzai && !AZOOKEY_WITH_LLAMA_CPP;

  const auto load_start = std::chrono::steady_clock::now();
  const auto load_result = engine.LoadModelWithResult(load_options);
  const auto load_end = std::chrono::steady_clock::now();
  const double load_ms = std::chrono::duration<double, std::milli>(load_end - load_start).count();
  if (!load_result.ok) {
    std::cerr << "status=load-failed llama_cpp=" << AZOOKEY_WITH_LLAMA_CPP << " load_ms=" << load_ms
              << " error=" << OptionalString(load_result.error) << std::endl;
    std::remove(learning_path.string().c_str());
    if (mock_model_path) {
      std::remove(mock_model_path->string().c_str());
    }
    return 1;
  }

  if (options.cancel_probe_delay_ms) {
    std::atomic<bool> cancel{false};
    std::jthread cancel_thread([&cancel, delay_ms = *options.cancel_probe_delay_ms] {
      std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
      cancel.store(true, std::memory_order_relaxed);
    });
    const auto canceled =
        engine.QueryCandidates(options.input, options.context, NowEpochSec(), &cancel, 0, false);
    cancel_thread.join();
    if (!cancel.load(std::memory_order_relaxed) || !canceled.empty()) {
      std::cerr << "status=cancel-probe-failed" << std::endl;
      return 1;
    }
  }

  for (size_t i = 0; i < options.warmup; ++i) {
    const auto& input = options.sequence_inputs.empty()
                            ? options.input
                            : options.sequence_inputs[i % options.sequence_inputs.size()];
    const auto& context = options.sequence_contexts.empty()
                              ? options.context
                              : options.sequence_contexts[i % options.sequence_contexts.size()];
    (void)engine.QueryCandidates(input, context, NowEpochSec(), nullptr, 0, false);
  }

  std::vector<double> lat_ms;
  lat_ms.reserve(options.iterations);
  std::vector<double> prompt_decode_ms;
  prompt_decode_ms.reserve(options.iterations);
  std::vector<double> beam_decode_ms;
  beam_decode_ms.reserve(options.iterations);
  uint64_t prompt_decode_tokens = 0;
  uint64_t prompt_reused_tokens = 0;
  uint64_t beam_decode_tokens = 0;
  size_t beam_decode_evaluations = 0;
  std::vector<bool> deadline_cutoff_samples;
  deadline_cutoff_samples.reserve(options.iterations);
  size_t deadline_cutoff_count = 0;
  std::vector<azookey::core::Candidate> last_candidates;
  for (size_t i = 0; i < options.iterations; ++i) {
    const auto& input = options.sequence_inputs.empty()
                            ? options.input
                            : options.sequence_inputs[i % options.sequence_inputs.size()];
    const auto& context = options.sequence_contexts.empty()
                              ? options.context
                              : options.sequence_contexts[i % options.sequence_contexts.size()];
    const auto t0 = std::chrono::steady_clock::now();
    last_candidates = engine.QueryCandidates(input, context, NowEpochSec(), nullptr, 0, false);
    const auto t1 = std::chrono::steady_clock::now();
    lat_ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    bool deadline_exceeded = false;
    if (const auto stats = engine.last_zenzai_decode_stats()) {
      prompt_decode_ms.push_back(stats->prompt_decode_ms);
      beam_decode_ms.push_back(stats->beam_decode_ms);
      prompt_decode_tokens += stats->prompt_tokens;
      prompt_reused_tokens += stats->prompt_reused_tokens;
      beam_decode_tokens += stats->beam_tokens;
      beam_decode_evaluations += stats->beam_decode_evaluations;
      deadline_exceeded = stats->deadline_exceeded;
    }
    deadline_cutoff_samples.push_back(deadline_exceeded);
    deadline_cutoff_count += deadline_exceeded ? 1u : 0u;
  }

  std::sort(lat_ms.begin(), lat_ms.end());
  const double p50 = Percentile(lat_ms, 50.0);
  const double p95 = Percentile(lat_ms, 95.0);
  const double p99 = Percentile(lat_ms, 99.0);

  const auto zenzai_count =
      std::count_if(last_candidates.begin(), last_candidates.end(),
                    [](const auto& candidate) { return HasZenzaiTag(candidate); });
  const std::string top_surface =
      last_candidates.empty() ? "none" : last_candidates.front().surface;
  const std::string top_debug = last_candidates.empty()
                                    ? "none"
                                    : OptionalString(last_candidates.front().debug_info.empty()
                                                         ? std::optional<std::string>()
                                                         : last_candidates.front().debug_info);

  azookey::bench::BenchmarkResult result;
  result.bench = "azookey_zenzai_bench";
  result.commit = AZOOKEY_BENCH_COMMIT;
  result.config = AZOOKEY_BENCH_CONFIG;
  result.iterations = options.iterations;
  result.latency = {p50, p95, p99, lat_ms.back()};
  result.max_p95_ms = options.max_p95_ms;
  result.threshold_passed = !options.max_p95_ms || p95 < *options.max_p95_ms;
  result.baseline = azookey::bench::CompareBaseline(options.baseline_path, result.bench,
                                                    result.config, result.latency);
  result.deadline_cutoffs = azookey::bench::DeadlineCutoffMetrics{
      deadline_cutoff_samples, deadline_cutoff_count,
      static_cast<double>(deadline_cutoff_count) /
          static_cast<double>(deadline_cutoff_samples.size())};
  if (!prompt_decode_ms.empty()) {
    std::sort(prompt_decode_ms.begin(), prompt_decode_ms.end());
    std::sort(beam_decode_ms.begin(), beam_decode_ms.end());
    const auto latency_metrics = [](const std::vector<double>& samples) {
      return azookey::bench::LatencyMetrics{Percentile(samples, 50.0), Percentile(samples, 95.0),
                                            Percentile(samples, 99.0), samples.back()};
    };
    result.decode_phases = azookey::bench::DecodePhaseBreakdown{
        {prompt_decode_ms.size(), prompt_decode_tokens, prompt_reused_tokens,
         latency_metrics(prompt_decode_ms)},
        {beam_decode_ms.size(), beam_decode_tokens, beam_decode_evaluations,
         latency_metrics(beam_decode_ms)}};
  }
  const auto json = azookey::bench::SerializeBenchmarkResult(result);

  if (!options.output_path.empty()) {
    std::string error;
    if (!azookey::bench::WriteBenchmarkResult(options.output_path, json, &error)) {
      std::remove(learning_path.string().c_str());
      if (mock_model_path) {
        std::remove(mock_model_path->string().c_str());
      }
      std::cerr << error << std::endl;
      return 2;
    }
  }

  if (options.json_output) {
    std::cout << json << std::endl;
  } else {
    std::cout << std::fixed << std::setprecision(4) << "status=ok"
              << " llama_cpp=" << AZOOKEY_WITH_LLAMA_CPP
              << " mock_zenzai=" << (load_options.mock_zenzai_candidates_for_tests ? 1 : 0)
              << " requested_backend=" << BackendName(options.backend)
              << " effective_backend=" << BackendName(engine.backend())
              << " load_warning=" << OptionalString(load_result.error)
              << " model_loaded=" << (engine.model_loaded() ? 1 : 0) << " load_ms=" << load_ms
              << " p50_ms=" << p50 << " p95_ms=" << p95 << " p99_ms=" << p99
              << " iterations=" << options.iterations << " candidates=" << last_candidates.size()
              << " zenzai_candidates=" << zenzai_count << " top_surface=" << top_surface
              << " top_debug_info=" << top_debug
              << " effective_last_error=" << OptionalString(engine.effective_last_error())
              << " deadline_cutoff_samples=" << FormatBoolSamples(deadline_cutoff_samples)
              << " deadline_cutoff_count=" << deadline_cutoff_count
              << " deadline_cutoff_rate=" << result.deadline_cutoffs->rate << " prompt_token_ids="
              << (prompt_token_ids ? FormatTokenIds(*prompt_token_ids)
                                   : std::string("not-checked"));
    if (result.decode_phases) {
      std::cout << " prompt_decode_p50_ms=" << result.decode_phases->prompt.latency.p50_ms
                << " prompt_decode_tokens=" << result.decode_phases->prompt.tokens
                << " prompt_reused_tokens=" << result.decode_phases->prompt.reused_tokens
                << " beam_decode_p50_ms=" << result.decode_phases->beam.latency.p50_ms
                << " beam_decode_tokens=" << result.decode_phases->beam.tokens
                << " beam_decode_evaluations=" << result.decode_phases->beam.evaluations;
    }
    std::cout << std::endl;
  }
  if (const auto warning = azookey::bench::RegressionWarning(result)) {
    std::cerr << *warning << std::endl;
  }

  std::remove(learning_path.string().c_str());
  if (mock_model_path) {
    std::remove(mock_model_path->string().c_str());
  }

  if (options.require_zenzai && zenzai_count == 0) {
    std::cerr << "zenzai candidate was required but not observed" << std::endl;
    return 1;
  }
  if (!result.threshold_passed) {
    std::cerr << "p95 exceeded threshold" << std::endl;
    return 1;
  }
  return 0;
}

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
  return RunBench(static_cast<int>(utf8_argv.size()), utf8_argv.data());
}
