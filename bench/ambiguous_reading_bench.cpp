// DEV-1734 (docs/legacy-parity-spec.md section 5.4): which reading of an
// ambiguous "nn" + vowel the Host converts with a real static dictionary, and
// how long the choice takes.
//
//   azookey_ambiguous_reading_bench --dict sudachi_lexicon.azdic [--iterations N]
//
// Prints one line per romaji (the readings and the chosen one), then the time of
// ChooseAmbiguousReading by the number of readings compared. No model is loaded.
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "BenchmarkCommandLine.h"
#include "azookey/core/AmbiguousReadingChooser.h"
#include "azookey/core/DoubleArrayTrie.h"
#include "azookey/core/RomajiKanaConverter.h"
#include "azookey/core/SimpleConverter.h"
#include "azookey/host/DictionaryCandidateProvider.h"
#include "azookey/host/InferenceEngine.h"
#include "azookey/learning/DictionaryStore.h"

namespace {

using Clock = std::chrono::steady_clock;

// The issue's examples, and spellings where both readings are words.
const std::vector<std::string> kRomaji = {"naibugenninn",
                                          "genninn",
                                          "konnichiha",
                                          "konnichihatenkigaiidesune",
                                          "onna",
                                          "minna",
                                          "minnnade",
                                          "kanni",
                                          "sannin",
                                          "tannin",
                                          "kinnyuu",
                                          "zenninn",
                                          "kanninn",
                                          "onnanoko",
                                          "sennenn",
                                          "tennin",
                                          "genninnwosirabeteminnnani",
                                          "onnagenninnminnna"};

double Ms(Clock::time_point start, Clock::time_point end) {
  return std::chrono::duration<double, std::milli>(end - start).count();
}

double Percentile(std::vector<double> values, double percentile) {
  std::sort(values.begin(), values.end());
  const auto index = static_cast<size_t>((percentile / 100.0) * (values.size() - 1));
  return values[index];
}

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path dict;
  int iterations = 200;
  try {
    const auto args = azookey::bench::Utf8CommandLineArguments(argc, argv);
    for (size_t i = 1; i < args.size(); i += 2) {
      if (i + 1 >= args.size()) throw std::invalid_argument(args[i] + " requires a value");
      const auto& value = args[i + 1];
      if (args[i] == "--dict")
        dict = azookey::bench::Utf8Path(value);
      else if (args[i] == "--iterations")
        iterations = std::stoi(value);
      else
        throw std::invalid_argument("unknown option: " + args[i]);
    }
    if (dict.empty() || iterations < 1)
      throw std::invalid_argument(
          "usage: azookey_ambiguous_reading_bench --dict LAYER.azdic [--iterations N]");

    azookey::core::DoubleArrayTrie probe;
    if (!probe.Load(dict)) throw std::runtime_error("cannot load " + dict.string());
    azookey::host::InferenceEngine engine(std::make_unique<azookey::core::SimpleConverter>(),
                                          nullptr, azookey::host::EngineConfig{});
    if (!engine.LoadDictionaryLayer(static_cast<azookey::learning::LayerId>(probe.LayerId()), dict))
      throw std::runtime_error("cannot load " + dict.string());

    // The same lookup on a store of its own, for each reading's split cost.
    azookey::learning::DictionaryStore store;
    if (!store.LoadStatic(static_cast<azookey::learning::LayerId>(probe.LayerId()), dict))
      throw std::runtime_error("cannot load " + dict.string());
    const auto lookup = azookey::host::StaticReadingWordLookup(store);

    std::map<size_t, std::vector<double>> by_count;
    for (const auto& romaji : kRomaji) {
      auto readings = azookey::core::RomajiKanaConverter::AmbiguousReadings(romaji);
      const std::string first = readings.front();
      readings.erase(readings.begin());
      const auto chosen = engine.ChooseAmbiguousReading(first, readings);
      std::cout << "romaji=" << romaji << " default=" << first << " others=";
      for (size_t i = 0; i < readings.size(); ++i) std::cout << (i ? "," : "") << readings[i];
      std::cout << " chosen=" << chosen
                << " costs=" << azookey::core::ReadingSplitCost(first, lookup);
      for (const auto& reading : readings)
        std::cout << ',' << azookey::core::ReadingSplitCost(reading, lookup);
      std::cout << '\n';
      auto& times = by_count[readings.size() + 1];
      for (int i = 0; i < iterations; ++i) {
        const auto start = Clock::now();
        (void)engine.ChooseAmbiguousReading(first, readings);
        times.push_back(Ms(start, Clock::now()));
      }
    }
    for (const auto& [count, times] : by_count)
      std::cout << "choose readings=" << count << " n=" << times.size()
                << " p50_ms=" << Percentile(times, 50) << " p95_ms=" << Percentile(times, 95)
                << " max_ms=" << *std::max_element(times.begin(), times.end()) << '\n';
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << '\n';
    return 2;
  }
  return 0;
}
