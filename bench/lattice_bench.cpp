// Micro benchmark of lattice building and N-best search (docs/lattice-conversion-spec.md
// section 9): windows of exactly 16, 32 and 64 code points cut from the evaluation readings.
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

// The file bytes [begin, end) that count as the lattice's memory (spec section 9): all of
// lattice_core, and the CONN section of each layer.
struct LatticeRange {
  std::filesystem::path file;
  uint64_t begin{};
  uint64_t end{};
};

// [offset, offset + size) of section `name`, read from the .azdic section table
// (docs/azdic-physical-layer-spec.md section 15.5); nullopt without that section.
std::optional<LatticeRange> SectionRange(const std::filesystem::path& file, const char (&name)[5]) {
  std::ifstream input(file, std::ios::binary);
  unsigned char header[64];
  if (!input.read(reinterpret_cast<char*>(header), sizeof(header))) return std::nullopt;
  const auto load = [](const unsigned char* bytes, int width) {
    uint64_t value = 0;
    for (int i = width - 1; i >= 0; --i) value = (value << 8) | bytes[i];
    return value;
  };
  const auto count = load(header + 28, 4);
  for (uint64_t i = 0; i < count; ++i) {
    unsigned char entry[24];
    if (!input.read(reinterpret_cast<char*>(entry), sizeof(entry))) return std::nullopt;
    if (std::equal(entry, entry + 4, name)) {
      const auto offset = load(entry + 8, 8);
      return LatticeRange{file, offset, offset + load(entry + 16, 8)};
    }
  }
  return std::nullopt;
}

#ifdef _WIN32
// Bytes of each range resident in this process: the working set pages of a view mapped from the
// range's file at a file offset inside the range. DoubleArrayTrie maps a whole file at offset 0.
// Empty when the working set cannot be read.
std::vector<uint64_t> ResidentBytes(const std::vector<LatticeRange>& ranges) {
  SYSTEM_INFO system{};
  GetSystemInfo(&system);
  const uint64_t page = system.dwPageSize;
  std::vector<unsigned char> buffer(sizeof(PSAPI_WORKING_SET_INFORMATION));
  for (;;) {
    if (QueryWorkingSet(GetCurrentProcess(), buffer.data(), static_cast<DWORD>(buffer.size())))
      break;
    if (GetLastError() != ERROR_BAD_LENGTH) return {};
    const auto entries =
        reinterpret_cast<PSAPI_WORKING_SET_INFORMATION*>(buffer.data())->NumberOfEntries;
    buffer.resize(sizeof(PSAPI_WORKING_SET_INFORMATION) +
                  (entries + 4096) * sizeof(PSAPI_WORKING_SET_BLOCK));
  }
  const auto* info = reinterpret_cast<const PSAPI_WORKING_SET_INFORMATION*>(buffer.data());
  std::map<const void*, std::wstring> mapped_files;  // File name by view base.
  std::vector<uint64_t> resident(ranges.size());
  for (ULONG_PTR i = 0; i < info->NumberOfEntries; ++i) {
    const auto address = static_cast<uint64_t>(info->WorkingSetInfo[i].VirtualPage) * page;
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(reinterpret_cast<const void*>(address), &region, sizeof(region)) ||
        region.Type != MEM_MAPPED)
      continue;
    auto name = mapped_files.find(region.AllocationBase);
    if (name == mapped_files.end()) {
      wchar_t device_path[1024];
      const auto length =
          GetMappedFileNameW(GetCurrentProcess(), region.AllocationBase, device_path, 1024);
      std::wstring file(device_path, length);
      name = mapped_files.emplace(region.AllocationBase, file.substr(file.rfind(L'\\') + 1)).first;
    }
    const uint64_t offset = address - reinterpret_cast<uint64_t>(region.AllocationBase);
    for (size_t r = 0; r < ranges.size(); ++r) {
      if (_wcsicmp(name->second.c_str(), ranges[r].file.filename().wstring().c_str()) != 0)
        continue;
      const auto begin = (std::max)(offset, ranges[r].begin);
      const auto end = (std::min)(offset + page, ranges[r].end);
      if (begin < end) resident[r] += end - begin;
    }
  }
  return resident;
}
#endif

double Mb(uint64_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); }

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
  // --head: also search each window for the candidates of its first `head` code points
  // (spec section 7.3, M66). 0 leaves the output as without the option.
  size_t head = 0;
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
      else if (args[i] == "--head")
        head = static_cast<size_t>(std::stoul(value));
      else
        throw std::invalid_argument("unknown option: " + args[i]);
    }
    if (dict_dir.empty() || window_count == 0 || iterations < 1)
      throw std::invalid_argument(
          "usage: azookey_lattice_bench --dict-dir DIR [--eval FILE] [--windows N]"
          " [--iterations N] [--max-p95-ms-16 MS] [--max-p95-ms-32 MS] [--head CP]");

    const auto rss_before = ResidentMb();
    core::LatticeDictionary dictionary;
    auto start = Clock::now();
    const auto core_path = dict_dir / "lattice_core.azdic";
    if (!dictionary.Load(core_path))
      throw std::runtime_error("lattice_core: " + std::string(dictionary.Error()));
    std::vector<LatticeRange> lattice_ranges{
        {core_path, 0, static_cast<uint64_t>(std::filesystem::file_size(core_path))}};
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
      if (const auto connections = SectionRange(path, "CONN"))
        lattice_ranges.push_back(*connections);
      vocabularies.push_back(layer.get());
      tries.push_back(std::move(trie));
      layers.push_back(std::move(layer));
    }

    // After the check (4) scans: lattice_core and every CONN are paged in once.
    const auto rss_loaded = ResidentMb();
    const auto codepoints = ReadCodepoints(eval);
    bool within_budget = true;
    for (const size_t max_paths : {size_t{16}, size_t{8}}) {
      // 64 code points, the longest reading with a lattice, has no budget of its own (spec 3.2).
      for (const size_t length : {size_t{16}, size_t{32}, size_t{64}}) {
        const auto windows = Windows(codepoints, length, window_count);
        // With --head, the same windows again as a first segment and its remaining reading
        // (segment-editing-spec section 5.3). The budgets gate the whole-reading search only.
        for (const bool head_only : {false, true}) {
          if (head_only && (head == 0 || head >= length)) continue;
          core::LatticeQuery query;
          query.max_paths = max_paths;
          if (head_only) {
            query.constraints.word_boundaries = {static_cast<uint32_t>(head)};
            query.head_only = true;
          }
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
          std::cout << (head_only ? "lattice_head_search" : "lattice_build_search")
                    << " length=" << length;
          if (head_only) std::cout << " head=" << head;
          std::cout << " n=" << max_paths << " count=" << times.size()
                    << " p50_ms=" << Percentile(times, 50) << " p95_ms=" << p95
                    << " p99_ms=" << Percentile(times, 99)
                    << " max_ms=" << *std::max_element(times.begin(), times.end())
                    << " mean_paths=" << static_cast<double>(paths) / times.size()
                    << " limited=" << limited;
          for (const auto& [status, count] : statuses) std::cout << ' ' << status << '=' << count;
          std::cout << '\n';
          const auto budget = max_p95.find(length);
          if (!head_only && max_paths == 16 && budget != max_p95.end() && p95 > budget->second) {
            std::cout << "over_budget length=" << length << " p95_ms=" << p95
                      << " max_p95_ms=" << budget->second << '\n';
            within_budget = false;
          }
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
    // The lattice's own memory (spec section 9): lattice_core and every CONN, all resident in the
    // worst case (mapped), and as resident after the queries (resident).
    uint64_t lattice_core_bytes = lattice_ranges.front().end, connection_bytes = 0;
    for (size_t i = 1; i < lattice_ranges.size(); ++i)
      connection_bytes += lattice_ranges[i].end - lattice_ranges[i].begin;
    std::cout << "lattice_mapped_mb=" << Mb(lattice_core_bytes + connection_bytes)
              << " lattice_core_mapped_mb=" << Mb(lattice_core_bytes)
              << " connection_mapped_mb=" << Mb(connection_bytes);
#ifdef _WIN32
    const auto resident = ResidentBytes(lattice_ranges);
    if (!resident.empty()) {
      uint64_t resident_connections = 0;
      for (size_t i = 1; i < resident.size(); ++i) resident_connections += resident[i];
      std::cout << " lattice_resident_mb=" << Mb(resident.front() + resident_connections)
                << " lattice_core_resident_mb=" << Mb(resident.front())
                << " connection_resident_mb=" << Mb(resident_connections);
    }
#endif
    std::cout << '\n';
    return within_budget ? 0 : 1;
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << '\n';
    return 2;
  }
}
