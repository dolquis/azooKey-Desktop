#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "BenchmarkCommandLine.h"
#include "azookey/core/SimpleConverter.h"
#include "azookey/host/ZenzaiModelConverter.h"

// Fixed, public homophone fixture. No user input or model path is printed.
int main(int argc, char** argv) {
  try {
    const auto args = azookey::bench::Utf8CommandLineArguments(argc, argv);
    std::string model, left_context, expected_top;
    int iterations = 10, top_k = 8, threads = 8;
    for (size_t i = 1; i < args.size(); ++i) {
      const auto& key = args[i];
      if (i + 1 == args.size()) throw std::runtime_error("missing option value");
      const auto& value = args[++i];
      if (key == "--model")
        model = value;
      else if (key == "--context")
        left_context = value;
      else if (key == "--expect-top")
        expected_top = value;
      else if (key == "--iterations")
        iterations = std::stoi(value);
      else if (key == "--top-k")
        top_k = std::stoi(value);
      else if (key == "--threads")
        threads = std::stoi(value);
      else
        throw std::runtime_error("unknown option");
    }
    if (model.empty() || iterations < 1 || iterations > 1000 || top_k < 1 || top_k > 8 ||
        threads < 1 || threads > 8)
      throw std::runtime_error("invalid benchmark options");
#if !AZOOKEY_WITH_LLAMA_CPP
    std::cerr << "status=unavailable reason=llama-cpp-disabled\n";
    return 1;
#endif
    azookey::host::ZenzaiRuntimeOptions options;
    options.n_threads = threads;
    auto loaded = azookey::host::LoadZenzaiGgufModel(model, options);
    if (!loaded.ok) throw std::runtime_error("model load failed");
    azookey::core::SimpleConverter fallback;
    azookey::host::ZenzaiModelConverter converter(std::move(loaded), &fallback);
    azookey::core::ConversionContext context;
    context.preceding_text = left_context;
    std::vector<std::string> surfaces{"構成", "公正", "校正", "更生",
                                      "厚生", "後世", "恒星", "攻勢"};
    surfaces.resize(static_cast<size_t>(top_k));
    std::vector<double> elapsed, prefix;
    azookey::host::NllEvaluation last;
    // Two warm-ups, then measure the scoring layer after generation has used the same KV.
    for (int i = -2; i < iterations; ++i) {
      context.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
      const auto generated = converter.Convert("こうせい", context);
      if (generated.empty() || generated.front().source != azookey::core::CandidateSource::Model)
        throw std::runtime_error("real generation unavailable");
      context.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
      const auto start = std::chrono::steady_clock::now();
      last = converter.EvaluateNllForValidation("こうせい", surfaces, context);
      const double ms =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
              .count();
      if (i >= 0) {
        elapsed.push_back(ms);
        prefix.push_back(last.prefix_ms);
      }
    }
    auto reversed = surfaces;
    std::reverse(reversed.begin(), reversed.end());
    context.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    const auto reverse_scores = converter.EvaluateNllForValidation("こうせい", reversed, context);
    for (size_t i = 0; i < last.scores.size(); ++i) {
      if (!std::isfinite(last.scores[i]) ||
          std::abs(last.scores[i] - reverse_scores.scores[last.scores.size() - 1 - i]) > 1e-5)
        throw std::runtime_error("candidate order changed NLL");
    }
    const auto winner = static_cast<size_t>(
        std::min_element(last.scores.begin(), last.scores.end()) - last.scores.begin());
    if (!expected_top.empty() && surfaces[winner] != expected_top)
      throw std::runtime_error("top candidate mismatch");
    std::vector<azookey::core::Candidate> candidates;
    for (size_t i = 0; i < surfaces.size(); ++i) {
      azookey::core::Candidate candidate;
      candidate.surface = surfaces[i];
      candidate.score = i == 0 ? 1.0 : 0.95;
      candidate.source = azookey::core::CandidateSource::SystemDictionary;
      candidates.push_back(candidate);
    }
    auto user = candidates.front();
    user.surface = "user-fixture";
    user.source = azookey::core::CandidateSource::UserDictionary;
    user.score = 1.5;
    user.debug_info = "user-dict";
    candidates.push_back(user);
    const auto selected = azookey::host::SelectNllCandidates(candidates, top_k);
    if (azookey::host::ApplyNllScores(candidates, selected, last.scores, 0.15) != surfaces.size())
      throw std::runtime_error("incomplete NLL application");
    if (candidates.back().score != user.score || candidates.back().debug_info != user.debug_info)
      throw std::runtime_error("excluded source was changed");
    const auto top =
        std::max_element(candidates.begin(), candidates.end() - 1,
                         [](const auto& a, const auto& b) { return a.score < b.score; });
    if (!expected_top.empty() && top->surface != expected_top)
      throw std::runtime_error("composed top candidate mismatch");
    std::sort(elapsed.begin(), elapsed.end());
    const auto p95 = elapsed[static_cast<size_t>(std::ceil(0.95 * iterations)) - 1];
    const auto prefix_mean = std::accumulate(prefix.begin(), prefix.end(), 0.0) / iterations;
    std::cout << "status=ok llama_cpp=1 backend=cpu threads=" << threads
              << " hardware_threads=" << std::thread::hardware_concurrency()
              << " iterations=" << iterations << " warmup=2 top_k=" << top_k
              << " elapsed_p95_ms=" << p95 << " prefix_mean_ms=" << prefix_mean
              << " within_default_budget=" << (p95 <= 20.0)
              << " order_invariant=1 top=" << surfaces[winner] << '\n';
    for (size_t i = 0; i < last.scores.size(); ++i)
      std::cout << "fixture=" << surfaces[i] << " nll=" << last.scores[i] << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "status=failed reason=" << error.what() << '\n';
    return 1;
  }
}
