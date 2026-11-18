// Micro benchmark of lattice building and N-best search (docs/lattice-conversion-spec.md
// section 9): windows of exactly 16 and 32 code points cut from the evaluation readings.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "BenchmarkCommandLine.h"
#include "azookey/core/DoubleArrayTrie.h"
#include "azookey/core/LatticeDictionary.h"
#include "azookey/core/LatticeSearch.h"
#include "azookey/core/Utf8.h"
#include "azookey/ipc/Json.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <psapi.h>
#else
#include <unistd.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;
namespace core = azookey::core;

// Static layers in DictionaryStore order, by their bundled file names.
constexpr const char* kLayers[] = {"base_lexicon", "sudachi_lexicon", "neologd_lexicon",
                                   "named_entity_lexicon", "technical_terms_lexicon"};

double Ms(Clock::time_point start, Clock::time_point end) {
  return std::chrono::duration<double, std::milli>(end - start).count();
}

double Percentile(std::vector<double> values, double percentile) {
  std::sort(values.begin(), values.end());
  const auto index = static_cast<size_t>((percentile / 100.0) * (values.size() - 1));
  return values[index];
}

// Resident memory of this process in MB, or a negative value when it cannot be read.
double ResidentMb() {
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS counters{};
  if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) return -1;
  return static_cast<double>(counters.WorkingSetSize) / (1024.0 * 1024.0);
#else
  std::ifstream statm("/proc/self/statm");
  long pages = 0, resident = 0;
  if (!(statm >> pages >> resident)) return -1;
  return static_cast<double>(resident) * static_cast<double>(sysconf(_SC_PAGESIZE)) /
         (1024.0 * 1024.0);
#endif
}

// Code points of every evaluation reading, concatenated in file order.
std::vector<std::string> ReadCodepoints(const std::filesystem::path& eval) {
  std::ifstream input(eval, std::ios::binary);
  if (!input) throw std::runtime_error("cannot read " + eval.string());
  std::vector<std::string> codepoints;
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty()) continue;
    const auto value = azookey::ipc::json::Parse(line);
    const auto reading = value ? value->GetString("input") : std::nullopt;
    if (!reading) throw std::runtime_error("evaluation line without input");
    const auto normalized = core::NormalizeReading(*reading);
    for (size_t offset = 0; offset < normalized.size();) {
      const auto start = offset;
      char32_t codepoint = 0;
      core::DecodeNextUtf8(normalized, offset, codepoint);
      codepoints.push_back(normalized.substr(start, offset - start));
    }
  }
  return codepoints;
}

// `count` windows of `length` code points at evenly spread offsets.
std::vector<std::string> Windows(const std::vector<std::string>& codepoints, size_t length,
                                 size_t count) {
  if (codepoints.size() < length) throw std::runtime_error("evaluation readings are too short");
  const auto span = codepoints.size() - length + 1;
  std::vector<std::string> windows;
  for (size_t i = 0; i < count; ++i) {
    const auto start = (i * span) / count;
    std::string window;
    for (size_t j = 0; j < length; ++j) window += codepoints[start + j];
    windows.push_back(std::move(window));
  }
  return windows;
}

const char* StatusName(core::LatticeStatus status) {
  switch (status) {
    case core::LatticeStatus::Complete:
      return "complete";
    case core::LatticeStatus::Truncated:
      return "truncated";
    case core::LatticeStatus::Aborted:
      return "aborted";
    case core::LatticeStatus::Rejected:
      return "rejected";
    case core::LatticeStatus::Unavailable:
      return "unavailable";
    case core::LatticeStatus::Failed:
      return "failed";
  }
  return "unknown";
}

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path dict_dir;
  std::filesystem::path eval = AZOOKEY_LATTICE_BENCH_EVAL;
  size_t window_count = 200;
  int iterations = 30;
  std::map<size_t, double> max_p95;  // By reading length; checked with N = 16.
  try {
    const auto args = azookey::bench::Utf8CommandLineArguments(argc, argv);
    for (size_t i = 1; i < args.size(); i += 2) {
      if (i + 1 >= args.size()) throw std::invalid_argument(args[i] + " requires a value");
      const auto& value = args[i + 1];
      if (args[i] == "--dict-dir")
        dict_dir = azookey::bench::Utf8Path(value);
      else if (args[i] == "--eval")
        eval = azookey::bench::Utf8Path(value);
      else if (args[i] == "--windows")
        window_count = static_cast<size_t>(std::stoul(value));
      else if (args[i] == "--iterations")
        iterations = std::stoi(value);
      else if (args[i] == "--max-p95-ms-16")
        max_p95[16] = std::stod(value);
      else if (args[i] == "--max-p95-ms-32")
        max_p95[32] = std::stod(value);
      else
        throw std::invalid_argument("unknown option: " + args[i]);
    }
    if (dict_dir.empty() || window_count == 0 || iterations < 1)
      throw std::invalid_argument(
          "usage: azookey_lattice_bench --dict-dir DIR [--eval FILE] [--windows N]"
          " [--iterations N] [--max-p95-ms-16 MS] [--max-p95-ms-32 MS]");

    const auto rss_before = ResidentMb();
    core::LatticeDictionary dictionary;
    auto start = Clock::now();
    if (!dictionary.Load(dict_dir / "lattice_core.azdic"))
      throw std::runtime_error("lattice_core: " + std::string(dictionary.Error()));
    std::cout << "lattice_core_load_ms=" << Ms(start, Clock::now()) << '\n';
    std::vector<std::unique_ptr<core::DoubleArrayTrie>> tries;
    std::vector<std::unique_ptr<core::StaticLatticeVocabulary>> layers;
    std::vector<const core::ILatticeVocabulary*> vocabularies;
    for (const char* name : kLayers) {
      const auto path = dict_dir / (std::string(name) + ".azdic");
      if (!std::filesystem::exists(path)) continue;
      auto trie = std::make_unique<core::DoubleArrayTrie>();
      start = Clock::now();
      if (!trie->Load(path))
        throw std::runtime_error(std::string(name) + ": " + std::string(trie->Error()));
      const auto load_ms = Ms(start, Clock::now());
      start = Clock::now();
      auto layer = std::make_unique<core::StaticLatticeVocabulary>(dictionary, *trie);
      std::cout << "layer=" << name << " load_ms=" << load_ms
                << " connection_check_ms=" << Ms(start, Clock::now())
                << " uses_connections=" << (layer->UsesConnections() ? 1 : 0) << '\n';
      vocabularies.push_back(layer.get());
      tries.push_back(std::move(trie));
      layers.push_back(std::move(layer));
    }

    // After the check (4) scans: lattice_core and every CONN are paged in once.
    const auto rss_loaded = ResidentMb();
    const auto codepoints = ReadCodepoints(eval);
    bool within_budget = true;
    for (const size_t max_paths : {size_t{16}, size_t{8}}) {
      for (const size_t length : {size_t{16}, size_t{32}}) {
        const auto windows = Windows(codepoints, length, window_count);
        core::LatticeQuery query;
        query.max_paths = max_paths;
        for (const auto& window : windows)  // Warm-up: page the dictionaries in.
          (void)core::ConvertLattice(window, dictionary, vocabularies, query);
        std::vector<double> times;
        std::map<std::string, size_t> statuses;
        size_t paths = 0, limited = 0;
        for (int round = 0; round < iterations; ++round) {
          for (const auto& window : windows) {
            start = Clock::now();
            const auto result = core::ConvertLattice(window, dictionary, vocabularies, query);
            times.push_back(Ms(start, Clock::now()));
            ++statuses[StatusName(result.status)];
            paths += result.paths.size();
            limited += result.limited ? 1 : 0;
          }
        }
        const auto p95 = Percentile(times, 95);
        std::cout << "lattice_build_search length=" << length << " n=" << max_paths
                  << " count=" << times.size() << " p50_ms=" << Percentile(times, 50)
                  << " p95_ms=" << p95 << " p99_ms=" << Percentile(times, 99)
                  << " max_ms=" << *std::max_element(times.begin(), times.end())
                  << " mean_paths=" << static_cast<double>(paths) / times.size()
                  << " limited=" << limited;
        for (const auto& [status, count] : statuses) std::cout << ' ' << status << '=' << count;
        std::cout << '\n';
        const auto budget = max_p95.find(length);
        if (max_paths == 16 && budget != max_p95.end() && p95 > budget->second) {
          std::cout << "over_budget length=" << length << " p95_ms=" << p95
                    << " max_p95_ms=" << budget->second << '\n';
          within_budget = false;
        }
      }
    }
    // Reference values (spec section 9): the queries also page in the static layers' own
    // sections, which belong to azdic spec section 15.9's budget rather than the lattice's.
    const auto rss_after = ResidentMb();
    if (rss_before >= 0 && rss_loaded >= 0 && rss_after >= 0)
      std::cout << "resident_mb_before=" << rss_before << " resident_mb_loaded=" << rss_loaded
                << " resident_mb_after_queries=" << rss_after
                << " resident_mb_increase_loaded=" << rss_loaded - rss_before
                << " resident_mb_increase_total=" << rss_after - rss_before << '\n';
    return within_budget ? 0 : 1;
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << '\n';
    return 2;
  }
}
