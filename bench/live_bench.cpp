#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <vector>

#include "azookey/core/SimpleConverter.h"
#include "azookey/host/InferenceEngine.h"

int main() {
  constexpr double kMaxP95Ms = 50.0;
  const auto learning_path =
      std::filesystem::temp_directory_path() / "azookey_bench_learning.tsv";
  std::remove(learning_path.string().c_str());

  azookey::learning::LearningStore store(learning_path.string());
  azookey::host::InferenceEngine engine(std::make_unique<azookey::core::SimpleConverter>(), &store, {});
  engine.LoadModel();

  const std::vector<std::string> inputs = {"わたし", "にほん", "とうきょう", "かなへんかん", "にほん"};
  std::vector<double> lat_ms;
  for (int i = 0; i < 200; ++i) {
    const auto& kana = inputs[static_cast<size_t>(i) % inputs.size()];
    auto t0 = std::chrono::steady_clock::now();
    auto now = static_cast<uint64_t>(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
    (void)engine.QueryCandidates(kana, "", now);
    auto t1 = std::chrono::steady_clock::now();
    lat_ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
  }

  std::sort(lat_ms.begin(), lat_ms.end());
  auto pct = [&](double p) {
    size_t idx = static_cast<size_t>((p / 100.0) * (lat_ms.size() - 1));
    return lat_ms[idx];
  };

  const double p50 = pct(50);
  const double p95 = pct(95);
  const double p99 = pct(99);
  std::cout << "p50_ms=" << p50 << " p95_ms=" << p95
            << " p99_ms=" << p99 << " max_p95_ms=" << kMaxP95Ms
            << std::endl;

  std::remove(learning_path.string().c_str());
  if (p95 >= kMaxP95Ms) {
    std::cerr << "p95 exceeded threshold" << std::endl;
    return 1;
  }
  return 0;
}
