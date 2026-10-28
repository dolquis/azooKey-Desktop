// DEV-1734 (docs/legacy-parity-spec.md section 5.4): which reading of an
// ambiguous "nn" + vowel the candidate window converts, and what comparing the
// readings costs.
//
//   azookey_ambiguous_reading_bench --dict-dir DIR [--model PATH] [--iterations N]
//
// DIR holds the bundled .azdic layers. Prints one line per romaji (the typed and
// the other reading, each one's best model score, the reading the window
// converts and its first candidates), then the window's time with one reading
// (QueryCandidatesEx) and with two (QueryAmbiguousCandidatesEx).
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "BenchmarkCommandLine.h"
#include "azookey/core/RomajiKanaConverter.h"
#include "azookey/core/SimpleConverter.h"
#include "azookey/host/InferenceEngine.h"

namespace {

using Clock = std::chrono::steady_clock;
using azookey::host::InferenceEngine;

// The issue's examples, spellings where both readings are words, and spellings
// that must keep the typed reading.
const std::vector<std::string> kRomaji = {"naibugenninn", "genninn", "zenninn",  "sannin",
                                          "kinnyuu",      "kanni",   "tannin",   "konnichiha",
                                          "onna",         "minna",   "onnanoko", "kanninn",
                                          "sennenn",      "tennin",  "minnnade", "konnnichiha"};

double Ms(Clock::time_point start, Clock::time_point end) {
  return std::chrono::duration<double, std::milli>(end - start).count();
}

double Percentile(std::vector<double> values, double percentile) {
  std::sort(values.begin(), values.end());
  const auto index = static_cast<size_t>((percentile / 100.0) * (values.size() - 1));
  return values[index];
}

std::optional<double> BestModelScore(const InferenceEngine::CandidatesResult& result) {
  std::optional<double> best;
  for (const auto& candidate : result.candidates)
    if (candidate.source == azookey::core::CandidateSource::Model)
      best = best ? (std::max)(*best, candidate.score) : candidate.score;
  return best;
}

std::string Score(const std::optional<double>& score) {
  return score ? std::to_string(*score) : std::string("none");
}

void Report(const char* name, const std::vector<double>& values) {
  if (values.empty()) return;
  std::cout << name << " n=" << values.size() << " p50_ms=" << Percentile(values, 50)
            << " p95_ms=" << Percentile(values, 95)
            << " max_ms=" << *std::max_element(values.begin(), values.end()) << '\n';
}

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path dict_dir;
  std::filesystem::path model;
  int iterations = 5;
  try {
    const auto args = azookey::bench::Utf8CommandLineArguments(argc, argv);
    for (size_t i = 1; i < args.size(); i += 2) {
      if (i + 1 >= args.size()) throw std::invalid_argument(args[i] + " requires a value");
      const auto& value = args[i + 1];
      if (args[i] == "--dict-dir")
        dict_dir = azookey::bench::Utf8Path(value);
      else if (args[i] == "--model")
        model = azookey::bench::Utf8Path(value);
      else if (args[i] == "--iterations")
        iterations = std::stoi(value);
      else
        throw std::invalid_argument("unknown option: " + args[i]);
    }
    if (dict_dir.empty() || iterations < 1)
      throw std::invalid_argument(
          "usage: azookey_ambiguous_reading_bench --dict-dir DIR [--model PATH]"
          " [--iterations N]");

    InferenceEngine engine(std::make_unique<azookey::core::SimpleConverter>(), nullptr,
                           azookey::host::EngineConfig{});
    for (const auto& layer : engine.LoadBundledDictionaryLayers(dict_dir))
      if (!layer.loaded) std::cerr << "dictionary layer not loaded: " << layer.error << '\n';
    if (!model.empty()) {
      azookey::host::ModelLoadOptions options;
      options.path = model.string();
      const auto loaded = engine.LoadModelWithResult(options);
      if (!loaded.ok) throw std::runtime_error("cannot load the model");
    }

    std::vector<double> one_reading;
    std::vector<double> two_readings;
    for (const auto& romaji : kRomaji) {
      auto readings = azookey::core::RomajiKanaConverter::AmbiguousReadings(romaji, 1);
      const std::string typed = readings.front();
      readings.erase(readings.begin());
      const auto typed_only = engine.QueryCandidatesEx(typed, "", 0, nullptr);
      const auto other_only = readings.empty()
                                  ? InferenceEngine::CandidatesResult{}
                                  : engine.QueryCandidatesEx(readings.front(), "", 0, nullptr);
      const auto window = engine.QueryAmbiguousCandidatesEx(typed, readings, "", 0, nullptr);
      std::cout << "romaji=" << romaji << " typed=" << typed
                << " other=" << (readings.empty() ? std::string("-") : readings.front())
                << " typed_score=" << Score(BestModelScore(typed_only))
                << " other_score=" << Score(BestModelScore(other_only)) << " converted="
                << (window.corrected_reading.empty() ? typed : window.corrected_reading)
                << " first=";
      for (size_t i = 0; i < window.candidates.size() && i < 3; ++i)
        std::cout << (i ? "," : "") << window.candidates[i].surface;
      // After a switch, the typed reading's words kept in the window and where.
      if (!window.corrected_reading.empty()) {
        std::cout << " typed_kept=";
        bool first_kept = true;
        for (size_t i = 0; i < window.candidates.size(); ++i) {
          if (window.candidates[i].reading != typed) continue;
          std::cout << (first_kept ? "" : ",") << window.candidates[i].surface << '@' << (i + 1);
          first_kept = false;
        }
        std::cout << '/' << window.candidates.size() << " window=";
        for (size_t i = 0; i < window.candidates.size(); ++i)
          std::cout << (i ? "," : "") << window.candidates[i].surface;
      }
      std::cout << '\n';
      if (readings.empty()) continue;
      for (int i = 0; i < iterations; ++i) {
        auto start = Clock::now();
        (void)engine.QueryCandidatesEx(typed, "", 0, nullptr);
        one_reading.push_back(Ms(start, Clock::now()));
        start = Clock::now();
        (void)engine.QueryAmbiguousCandidatesEx(typed, readings, "", 0, nullptr);
        two_readings.push_back(Ms(start, Clock::now()));
      }
    }
    Report("window readings=1", one_reading);
    Report("window readings=2", two_readings);
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << '\n';
    return 2;
  }
  return 0;
}
