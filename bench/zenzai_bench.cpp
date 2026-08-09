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
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <process.h>
#include <shellapi.h>
#else
#include <unistd.h>
#endif

#include "azookey/core/SimpleConverter.h"
#include "azookey/host/InferenceEngine.h"

#ifndef AZOOKEY_WITH_LLAMA_CPP
#define AZOOKEY_WITH_LLAMA_CPP 0
#endif

namespace {

struct Options {
  std::string model_path;
  std::string input = "にほんご";
  std::string context;
  size_t iterations{50};
  size_t warmup{3};
  bool require_model{false};
  bool mock_zenzai{false};
  bool require_zenzai{false};
  std::optional<double> max_p95_ms;
  azookey::host::BackendKind backend{azookey::host::BackendKind::Cpu};
  std::optional<int32_t> n_gpu_layers;
};

void PrintUsage(const char* exe) {
  std::cerr << "Usage: " << exe
            << " [--model PATH] [--input KANA] [--context TEXT] [--iterations N]"
               " [--warmup N] [--backend cpu|cuda] [--n-gpu-layers N]"
               " [--max-p95-ms N] [--require-model] [--require-zenzai]"
               " [--mock-zenzai]\n"
            << "If --model is omitted, AZOOKEY_ZENZAI_MODEL is used when set; otherwise the "
               "bench reports status=skipped and exits 0 unless --require-model is present.\n";
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

#if defined(_WIN32)
std::optional<std::string> WideToUtf8(const wchar_t* value) {
  if (!value) {
    return std::nullopt;
  }
  const int required =
      WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, nullptr, 0, nullptr, nullptr);
  if (required <= 0) {
    return std::nullopt;
  }
  std::string result(static_cast<size_t>(required), '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, result.data(), required,
                          nullptr, nullptr) != required) {
    return std::nullopt;
  }
  result.pop_back();
  return result;
}
#endif

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
    } else if (arg == "--context") {
      options.context = RequireValue(argc, argv, i, "--context");
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
    } else if (arg == "--max-p95-ms") {
      options.max_p95_ms = ParseDouble(RequireValue(argc, argv, i, "--max-p95-ms"), "--max-p95-ms");
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
    std::cout << "status=skipped reason=model-not-provided llama_cpp=" << AZOOKEY_WITH_LLAMA_CPP
              << " hint=set-AZOOKEY_ZENZAI_MODEL-or-pass---model" << std::endl;
    return options.require_model ? 1 : 0;
  }

  const auto learning_path = UniqueTempPath("azookey_zenzai_bench_learning", ".tsv");
  std::remove(learning_path.string().c_str());

  azookey::learning::LearningStore store(learning_path.string());
  azookey::host::InferenceEngine engine(std::make_unique<azookey::core::SimpleConverter>(), &store,
                                        {});

  azookey::host::ModelLoadOptions load_options;
  load_options.path = options.model_path;
  load_options.backend = options.backend;
  load_options.n_gpu_layers = options.n_gpu_layers;
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

  for (size_t i = 0; i < options.warmup; ++i) {
    (void)engine.QueryCandidates(options.input, options.context, NowEpochSec(), nullptr, 0, false);
  }

  std::vector<double> lat_ms;
  lat_ms.reserve(options.iterations);
  std::vector<azookey::core::Candidate> last_candidates;
  for (size_t i = 0; i < options.iterations; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    last_candidates =
        engine.QueryCandidates(options.input, options.context, NowEpochSec(), nullptr, 0, false);
    const auto t1 = std::chrono::steady_clock::now();
    lat_ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
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
            << std::endl;

  std::remove(learning_path.string().c_str());
  if (mock_model_path) {
    std::remove(mock_model_path->string().c_str());
  }

  if (options.require_zenzai && zenzai_count == 0) {
    std::cerr << "zenzai candidate was required but not observed" << std::endl;
    return 1;
  }
  if (options.max_p95_ms && p95 >= *options.max_p95_ms) {
    std::cerr << "p95 exceeded threshold" << std::endl;
    return 1;
  }
  return 0;
}

int main(int argc, char** argv) {
#if defined(_WIN32)
  (void)argv;
  int wide_argc = 0;
  wchar_t** wide_argv = CommandLineToArgvW(GetCommandLineW(), &wide_argc);
  if (!wide_argv || wide_argc != argc) {
    if (wide_argv) LocalFree(wide_argv);
    std::cerr << "failed to read Unicode command-line arguments" << std::endl;
    return 2;
  }

  std::vector<std::string> utf8_args;
  utf8_args.reserve(static_cast<size_t>(wide_argc));
  for (int i = 0; i < wide_argc; ++i) {
    auto converted = WideToUtf8(wide_argv[i]);
    if (!converted) {
      LocalFree(wide_argv);
      std::cerr << "failed to convert command-line argument to UTF-8" << std::endl;
      return 2;
    }
    utf8_args.push_back(std::move(*converted));
  }
  LocalFree(wide_argv);

  std::vector<char*> utf8_argv;
  utf8_argv.reserve(utf8_args.size());
  for (auto& arg : utf8_args) {
    utf8_argv.push_back(arg.data());
  }
  return RunBench(static_cast<int>(utf8_argv.size()), utf8_argv.data());
#else
  return RunBench(argc, argv);
#endif
}
