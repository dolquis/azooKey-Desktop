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
#include "azookey/host/InferenceEngine.h"
#include "azookey/host/ZenzaiModelConverter.h"
#include "azookey/learning/DictionaryStore.h"

namespace {
double Percentile(std::vector<double> samples, double quantile) {
  std::sort(samples.begin(), samples.end());
  return samples[static_cast<size_t>(std::ceil(quantile * static_cast<double>(samples.size()))) -
                 1];
}

double P95(const std::vector<double>& samples) { return Percentile(samples, 0.95); }

double Mean(const std::vector<double>& samples) {
  return std::accumulate(samples.begin(), samples.end(), 0.0) / static_cast<double>(samples.size());
}

double RequireSameCandidates(const std::vector<azookey::core::Candidate>& before,
                             const std::vector<azookey::core::Candidate>& after,
                             bool exact_scores = false) {
  if (before.size() != after.size()) throw std::runtime_error("generation count changed");
  double max_score_delta = 0.0;
  for (size_t i = 0; i < before.size(); ++i) {
    if (before[i].surface != after[i].surface || before[i].source != after[i].source ||
        !std::isfinite(before[i].score) || !std::isfinite(after[i].score))
      throw std::runtime_error("generation changed after NLL");
    const auto delta = std::abs(before[i].score - after[i].score);
    if (exact_scores && (delta != 0.0 || before[i].debug_info != after[i].debug_info))
      throw std::runtime_error("unapplied dictionary candidate changed");
    max_score_delta = std::max(max_score_delta, delta);
  }
  return max_score_delta;
}
}  // namespace

// Fixed, public homophone fixture. No user input or model path is printed.
int main(int argc, char** argv) {
  try {
    const auto args = azookey::bench::Utf8CommandLineArguments(argc, argv);
    std::string model, left_context, expected_top, dictionary, backend = "cpu";
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
      else if (key == "--backend")
        backend = value;
      else if (key == "--query-dictionary")
        dictionary = value;
      else
        throw std::runtime_error("unknown option");
    }
    if (model.empty() || iterations < 1 || iterations > 1000 || top_k < 1 || top_k > 8 ||
        threads < 1 || threads > 8 || (backend != "cpu" && backend != "vulkan"))
      throw std::runtime_error("invalid benchmark options");
#if !AZOOKEY_WITH_LLAMA_CPP
    std::cerr << "status=unavailable reason=llama-cpp-disabled\n";
    return 1;
#endif
    azookey::host::ZenzaiRuntimeOptions options;
    options.n_threads = threads;
    options.use_vulkan = backend == "vulkan";
    options.n_gpu_layers = options.use_vulkan ? 99 : 0;
    const auto default_budget_ms = azookey::host::NllConfig{}.budget_ms;
    {
      auto loaded = azookey::host::LoadZenzaiGgufModel(model, options);
      if (!loaded.ok) throw std::runtime_error("model load failed");
      azookey::core::SimpleConverter fallback;
      azookey::host::ZenzaiModelConverter converter(std::move(loaded), &fallback);
      azookey::core::ConversionContext context;
      context.preceding_text = left_context;
      std::vector<std::string> surfaces{"構成", "公正", "校正", "更生",
                                        "厚生", "後世", "恒星", "攻勢"};
      surfaces.resize(static_cast<size_t>(top_k));
      std::vector<double> elapsed, prefix, cached_convert, after_nll_convert;
      std::vector<double> cached_prompt, after_nll_prompt;
      double generation_score_delta = 0.0;
      azookey::host::NllEvaluation last;
      // Two warm-ups, then measure the scoring layer after generation has used the same KV.
      for (int i = -2; i < iterations; ++i) {
        context.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        const auto generated = converter.Convert("こうせい", context);
        if (generated.empty() || generated.front().source != azookey::core::CandidateSource::Model)
          throw std::runtime_error("real generation unavailable");
        const auto measure_convert = [&](std::vector<double>& durations,
                                         std::vector<double>& prompts) {
          context.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
          const auto begin = std::chrono::steady_clock::now();
          const auto result = converter.Convert("こうせい", context);
          const double duration =
              std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin)
                  .count();
          generation_score_delta =
              std::max(generation_score_delta, RequireSameCandidates(generated, result));
          const auto stats = converter.last_decode_stats();
          if (!stats || stats->deadline_exceeded)
            throw std::runtime_error("decode stats unavailable");
          if (i >= 0) {
            durations.push_back(duration);
            prompts.push_back(stats->prompt_decode_ms);
          }
        };
        measure_convert(cached_convert, cached_prompt);
        context.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        const auto start = std::chrono::steady_clock::now();
        last = converter.EvaluateNllForValidation("こうせい", surfaces, context);
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
        if (last.scores.size() != surfaces.size())
          throw std::runtime_error("incomplete NLL scores");
        if (i >= 0) {
          elapsed.push_back(ms);
          prefix.push_back(last.prefix_ms);
        }
        measure_convert(after_nll_convert, after_nll_prompt);
      }
      auto reversed = surfaces;
      std::reverse(reversed.begin(), reversed.end());
      context.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
      const auto reverse_scores = converter.EvaluateNllForValidation("こうせい", reversed, context);
      if (reverse_scores.scores.size() != last.scores.size())
        throw std::runtime_error("incomplete reverse NLL scores");
      for (size_t i = 0; i < last.scores.size(); ++i) {
        if (!std::isfinite(last.scores[i]) ||
            !std::isfinite(reverse_scores.scores[last.scores.size() - 1 - i]) ||
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
      const auto unadjusted_candidates = candidates;
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
      const auto p95 = P95(elapsed);
      const auto over_budget =
          std::count_if(elapsed.begin(), elapsed.end(),
                        [default_budget_ms](double ms) { return ms > default_budget_ms; });
      std::cout << "status=ok llama_cpp=1 backend=" << backend << " threads=" << threads
                << " hardware_threads=" << std::thread::hardware_concurrency()
                << " iterations=" << iterations << " warmup=2 top_k=" << top_k
                << " n=" << elapsed.size() << " elapsed_p50_ms=" << Percentile(elapsed, 0.5)
                << " elapsed_max_ms=" << *std::max_element(elapsed.begin(), elapsed.end())
                << " budget_ms=" << default_budget_ms << " elapsed_p95_ms=" << p95
                << " prefix_mean_ms=" << Mean(prefix) << " prefix_p95_ms=" << P95(prefix)
                << " over_default_budget=" << over_budget
                << " over_default_budget_rate=" << static_cast<double>(over_budget) / iterations
                << " cached_convert_p95_ms=" << P95(cached_convert)
                << " after_nll_convert_p95_ms=" << P95(after_nll_convert)
                << " cached_prompt_mean_ms=" << Mean(cached_prompt)
                << " after_nll_prompt_mean_ms=" << Mean(after_nll_prompt)
                << " within_default_budget=" << (p95 <= default_budget_ms)
                << " order_invariant=1 generation_order_invariant=1"
                << " generation_max_score_delta=" << generation_score_delta
                << " top=" << surfaces[winner] << '\n';
      for (size_t i = 0; i < last.scores.size(); ++i)
        std::cout << "fixture=" << surfaces[i] << " nll=" << last.scores[i] << '\n';
      for (size_t i = 0; i < elapsed.size(); ++i)
        std::cout << "nll_sample=" << i << " elapsed_ms=" << elapsed[i]
                  << " prefix_ms=" << prefix[i] << " cached_convert_ms=" << cached_convert[i]
                  << " after_nll_convert_ms=" << after_nll_convert[i] << '\n';
      // Check all-or-nothing at its input boundary: engine deduplication includes
      // changing model score/debug details in merged output even with NLL disabled.
      for (int i = 0; i < 3; ++i) {
        auto bounded_candidates = unadjusted_candidates;
        context.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        azookey::host::NllConfig config;
        config.enabled = true;
        config.top_k = top_k;
        const auto outcome = converter.RerankNll("こうせい", bounded_candidates, context, config);
        if (outcome.reason == "budget_exceeded" || outcome.reason == "circuit_open") {
          (void)RequireSameCandidates(unadjusted_candidates, bounded_candidates, true);
          if (outcome.applied != 0) throw std::runtime_error("partial NLL application");
        } else if (outcome.reason != "applied" || outcome.applied != surfaces.size()) {
          throw std::runtime_error("unexpected bounded NLL outcome");
        }
        std::cout << "bounded_sample=" << i << " reason=" << outcome.reason
                  << " applied=" << outcome.applied << " elapsed_ms=" << outcome.elapsed_ms
                  << " all_or_nothing=1\n";
      }
    }  // Release the scoring model and KV before loading the query engine's model.
    // Optional production path. Keep each engine alive across requests so circuit
    // opening and the following request's prefix rebuild remain observable.
    if (!dictionary.empty()) {
      for (bool enabled : {false, true}) {
        azookey::host::EngineConfig config;
        config.model_path = model;
        config.backend = options.use_vulkan ? azookey::host::BackendKind::Vulkan
                                            : azookey::host::BackendKind::Cpu;
        config.inference_threads = threads;
        config.max_candidates = 32;
        config.nll.top_k = top_k;
        azookey::host::InferenceEngine engine(std::make_unique<azookey::core::SimpleConverter>(),
                                              nullptr, config);
        if (!engine.LoadModel()) throw std::runtime_error("query model load failed");
        const auto layer = azookey::learning::LayerId::TechnicalTerms;
        const auto dictionary_path = azookey::bench::Utf8Path(dictionary);
        if (!engine.LoadDictionaryLayer(layer, dictionary_path, true)) {
          // Diagnose with the same loader without extending the production engine API.
          azookey::learning::DictionaryStore diagnostic;
          const bool reloaded = diagnostic.LoadStatic(layer, dictionary_path, true);
          throw std::runtime_error("query dictionary load failed: " +
                                   (reloaded ? std::string("retry succeeded; fixture changed")
                                             : diagnostic.LayerError(layer)));
        }
        std::vector<azookey::core::Candidate> baseline;
        for (int i = 0; i < 2; ++i)
          baseline = engine.QueryCandidates("こうせい", left_context, 0, nullptr, 32, false);
        config.nll.enabled = enabled;
        engine.ApplyConfig(config);
        std::vector<double> query_ms;
        double query_score_delta = 0.0;
        for (int i = 0; i < iterations; ++i) {
          const auto start = std::chrono::steady_clock::now();
          const auto result =
              engine.QueryCandidates("こうせい", left_context, 0, nullptr, 32, false);
          const double ms =
              std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                  .count();
          const auto stats = engine.last_zenzai_decode_stats();
          const auto error = engine.effective_last_error();
          const bool budget_exceeded = error && *error == "nll-scorer:budget_exceeded";
          if (result.empty() || !stats || stats->deadline_exceeded || (error && !budget_exceeded))
            throw std::runtime_error("query generation unavailable");
          const auto targets = std::count_if(result.begin(), result.end(), [](const auto& c) {
            return c.source == azookey::core::CandidateSource::SystemDictionary;
          });
          if (targets == 0) throw std::runtime_error("query fixture has no NLL targets");
          const auto applied = std::count_if(result.begin(), result.end(), [](const auto& c) {
            return c.debug_info.find("nll=") != std::string::npos;
          });
          if (applied == 0)
            query_score_delta =
                std::max(query_score_delta, RequireSameCandidates(baseline, result));
          query_ms.push_back(ms);
          std::cout << "query_nll=" << enabled << " sample=" << i << " elapsed_ms=" << ms
                    << " prompt_ms=" << stats->prompt_decode_ms
                    << " reused_tokens=" << stats->prompt_reused_tokens
                    << " budget_exceeded=" << budget_exceeded << " targets_returned=" << targets
                    << " applied_returned=" << applied << '\n';
        }
        std::cout << "query_nll=" << enabled << " elapsed_p95_ms=" << P95(query_ms)
                  << " n=" << query_ms.size() << " elapsed_p50_ms=" << Percentile(query_ms, 0.5)
                  << " elapsed_max_ms=" << *std::max_element(query_ms.begin(), query_ms.end())
                  << " unapplied_max_score_delta=" << query_score_delta
                  << " warmup=2 iterations=" << iterations << " budget_ms=" << default_budget_ms
                  << '\n';
      }
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "status=failed reason=" << error.what() << '\n';
    return 1;
  }
}
