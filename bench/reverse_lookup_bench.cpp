#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "BenchmarkCommandLine.h"
#include "azookey/core/DoubleArrayTrie.h"
#include "azookey/core/SimpleConverter.h"
#include "azookey/host/InferenceEngine.h"

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kWarmupRounds = 3;

// Hits and misses for the static layer.
const std::vector<std::string> kSurfaces = {"東京", "日本語", "変換", "明日", "漢字",
                                            "学校", "存在しない語彙", "東京都庁舎前"};

double Ms(Clock::time_point start, Clock::time_point end) {
  return std::chrono::duration<double, std::milli>(end - start).count();
}

double Percentile(std::vector<double> values, double percentile) {
  std::sort(values.begin(), values.end());
  const auto index = static_cast<size_t>((percentile / 100.0) * (values.size() - 1));
  return values[index];
}

void Report(const char* name, const std::vector<double>& values) {
  std::cout << name << " n=" << values.size() << " p50_ms=" << Percentile(values, 50)
            << " p95_ms=" << Percentile(values, 95)
            << " max_ms=" << *std::max_element(values.begin(), values.end()) << '\n';
}

class ReverseLookupBench {
 public:
  explicit ReverseLookupBench(std::filesystem::path dict) : dict_(std::move(dict)) {
    azookey::core::DoubleArrayTrie probe;
    if (!probe.Load(dict_)) throw std::runtime_error("cannot load " + dict_.string());
    layer_ = static_cast<azookey::learning::LayerId>(probe.LayerId());
  }

  // A fresh engine maps the artifact again, so its first query faults the pages
  // in. The OS file cache stays warm (flushing it needs admin rights).
  std::unique_ptr<azookey::host::InferenceEngine> MakeEngine(double* load_ms) const {
    auto engine = std::make_unique<azookey::host::InferenceEngine>(
        std::make_unique<azookey::core::SimpleConverter>(), nullptr,
        azookey::host::EngineConfig{});
    const auto start = Clock::now();
    const bool loaded = engine->LoadDictionaryLayer(layer_, dict_);
    *load_ms = Ms(start, Clock::now());
    if (!loaded) throw std::runtime_error("cannot load " + dict_.string());
    return engine;
  }

 private:
  std::filesystem::path dict_;
  azookey::learning::LayerId layer_{};
};

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path dict;
  int cold_runs = 20;
  int warm_iterations = 200;
  try {
    const auto args = azookey::bench::Utf8CommandLineArguments(argc, argv);
    for (size_t i = 1; i < args.size(); i += 2) {
      if (i + 1 >= args.size()) throw std::invalid_argument(args[i] + " requires a value");
      const auto& value = args[i + 1];
      if (args[i] == "--dict")
        dict = azookey::bench::Utf8Path(value);
      else if (args[i] == "--cold-runs")
        cold_runs = std::stoi(value);
      else if (args[i] == "--warm-iterations")
        warm_iterations = std::stoi(value);
      else
        throw std::invalid_argument("unknown option: " + args[i]);
    }
    if (dict.empty() || cold_runs < 1 || warm_iterations < 1)
      throw std::invalid_argument(
          "usage: azookey_reverse_lookup_bench --dict LAYER.azdic"
          " [--cold-runs N] [--warm-iterations N]");

    const ReverseLookupBench bench(dict);
    std::vector<double> load_ms, cold_ms;
    for (int run = 0; run < cold_runs; ++run) {
      double load = 0;
      const auto engine = bench.MakeEngine(&load);
      const auto& surface = kSurfaces[static_cast<size_t>(run) % kSurfaces.size()];
      const auto start = Clock::now();
      (void)engine->ReverseConvert(surface, 0);
      cold_ms.push_back(Ms(start, Clock::now()));
      load_ms.push_back(load);
    }

    double load = 0;
    const auto engine = bench.MakeEngine(&load);
    for (int round = 0; round < kWarmupRounds; ++round)
      for (const auto& surface : kSurfaces) (void)engine->ReverseConvert(surface, 0);
    std::vector<double> warm_ms;
    for (int i = 0; i < warm_iterations; ++i) {
      const auto& surface = kSurfaces[static_cast<size_t>(i) % kSurfaces.size()];
      const auto start = Clock::now();
      (void)engine->ReverseConvert(surface, 0);
      warm_ms.push_back(Ms(start, Clock::now()));
    }
    for (const auto& surface : kSurfaces)
      std::cout << "surface=" << surface << " reading=" << engine->ReverseConvert(surface, 0)
                << '\n';

    Report("load", load_ms);
    Report("reverse_convert_cold", cold_ms);
    Report("reverse_convert_warm", warm_ms);
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << '\n';
    return 2;
  }
  return 0;
}
