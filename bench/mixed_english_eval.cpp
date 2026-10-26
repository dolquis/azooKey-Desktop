// M63-B section 15.2 quality gate for the mixed lattice
// (docs/mixed-english-auto-conversion-spec.md, docs/conversion-quality-benchmark-spec.md
// section 4.4). It measures the splitter only: which E segments are sure enough to turn
// English in the live conversion and the first candidate. No model is involved.
//
//   azookey_mixed_english_eval --data <jsonl> --dictionary <english-words.tsv>
//       [--min-precision 0.95] [--per-case] [--no-inline-high-confidence]
//
// Prints one JSON summary line and exits 1 when the gate fails: invalid cases,
// precision below the minimum, or a sure E segment in a Japanese-only sentence.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "azookey/core/MixedRomajiSplitter.h"
#include "azookey/core/RomajiKanaConverter.h"
#include "azookey/host/EnglishCandidates.h"
#include "azookey/ipc/Json.h"

namespace {

namespace j = azookey::ipc::json;
using azookey::core::EnglishSegmentConfidence;
using azookey::core::MixedRomajiSegmentKind;

struct Expected {
  size_t begin{};
  size_t end{};
  std::string surface;
  bool operator<(const Expected& other) const {
    return std::tie(begin, end, surface) < std::tie(other.begin, other.end, other.surface);
  }
};

struct Case {
  std::string id;
  std::string category;
  std::string raw_romaji;
  std::string reading;
  std::set<Expected> expected;
};

std::string Lower(std::string text) {
  for (auto& c : text)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return text;
}

std::optional<Case> ParseCase(const std::string& line, std::string& error) {
  const auto value = j::Parse(line);
  if (!value || !value->IsObject()) {
    error = "not a JSON object";
    return std::nullopt;
  }
  Case item;
  auto id = value->GetString("id");
  auto category = value->GetString("category");
  auto raw = value->GetString("raw_romaji");
  auto reading = value->GetString("reading");
  auto provenance = value->GetString("provenance");
  if (!id || !category || !raw || raw->empty() || provenance != "authored") {
    error = "missing id, category, raw_romaji or provenance=authored";
    return std::nullopt;
  }
  if (*category != "mixed_english" && *category != "japanese_only" &&
      *category != "romaji_like_english") {
    error = "unknown category";
    return std::nullopt;
  }
  item.id = std::move(*id);
  item.category = std::move(*category);
  item.raw_romaji = std::move(*raw);
  if (reading) item.reading = std::move(*reading);
  if (const auto* english = value->GetArray("expected_english")) {
    for (const auto& entry : *english) {
      const auto begin = entry.GetUInt("begin");
      const auto end = entry.GetUInt("end");
      auto surface = entry.GetString("surface");
      if (!begin || !end || !surface || *begin >= *end || *end > item.raw_romaji.size()) {
        error = "malformed expected_english";
        return std::nullopt;
      }
      item.expected.insert(
          {static_cast<size_t>(*begin), static_cast<size_t>(*end), std::move(*surface)});
    }
  }
  if (item.category == "japanese_only" && !item.expected.empty()) {
    error = "japanese_only with expected_english";
    return std::nullopt;
  }
  // The reading the TIP would send (section 5) is the built-in table's kana.
  // A Japanese sentence must spell its reading, so a typo in the data shows up
  // here instead of as a splitter result.
  const auto kana = azookey::core::RomajiKanaConverter::ConvertForCommit(Lower(item.raw_romaji));
  if (item.category != "mixed_english" && item.reading.empty()) {
    error = "a Japanese sentence needs its reading";
    return std::nullopt;
  }
  if (!item.reading.empty() && kana != item.reading) {
    error = "reading does not match the raw romaji (" + kana + ")";
    return std::nullopt;
  }
  return item;
}

int Usage() {
  std::cerr << "usage: azookey_mixed_english_eval --data <jsonl> --dictionary <tsv> "
               "[--min-precision 0.95] [--per-case] [--no-inline-high-confidence]\n";
  return 2;
}

// A run that cannot start says why on stdout too, so the CTest output shows it.
int Fail(const std::string& reason) {
  std::cerr << reason << "\n";
  j::Object summary;
  summary.emplace("bench", j::Value("azookey_mixed_english_eval"));
  summary.emplace("error", j::Value(reason));
  summary.emplace("passed", j::Value(false));
  std::cout << j::Stringify(j::Value(std::move(summary))) << "\n";
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  std::string data_path;
  std::string dictionary_path;
  double min_precision = 0.95;
  bool per_case = false;
  azookey::core::MixedRomajiSplitOptions options;
  options.split_inline = true;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--data" && i + 1 < argc) {
      data_path = argv[++i];
    } else if (arg == "--dictionary" && i + 1 < argc) {
      dictionary_path = argv[++i];
    } else if (arg == "--min-precision" && i + 1 < argc) {
      min_precision = std::strtod(argv[++i], nullptr);
    } else if (arg == "--per-case") {
      per_case = true;
    } else if (arg == "--no-inline-high-confidence") {
      options.inline_high_confidence = false;
    } else {
      return Usage();
    }
  }
  if (data_path.empty() || dictionary_path.empty()) return Usage();

  const auto dictionary = azookey::host::EnglishDictionary::LoadTsv(dictionary_path);
  if (!dictionary || dictionary->size() == 0) {
    return Fail("cannot read the English dictionary: " + dictionary_path);
  }
  const azookey::core::EnglishWordLookup lookup = [&](std::string_view key) {
    std::vector<azookey::core::EnglishWordMatch> matches;
    for (auto& entry : dictionary->Lookup(key))
      matches.push_back({std::move(entry.surface), entry.flags});
    return matches;
  };

  std::ifstream input(data_path, std::ios::binary);
  if (!input) {
    return Fail("cannot read the data: " + data_path);
  }
  size_t cases = 0;
  size_t invalid = 0;
  size_t predicted = 0;       // Sure E segments of the best split.
  size_t correct = 0;         // ... that match an expected E segment.
  size_t expected_total = 0;  // Expected E segments of the mixed sentences.
  size_t japanese_sure = 0;   // Japanese-only sentences with a sure E segment.
  size_t japanese_cases = 0;
  size_t romaji_like_sure_wrong = 0;
  j::Array failures;
  std::string line;
  size_t line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    std::string error;
    const auto item = ParseCase(line, error);
    if (!item) {
      ++invalid;
      std::cerr << data_path << ":" << line_number << ": " << error << "\n";
      continue;
    }
    ++cases;
    const auto splits = azookey::core::SplitMixedRomaji(item->raw_romaji, lookup, options);
    std::set<Expected> sure;
    if (!splits.empty()) {
      for (const auto& segment : splits.front().segments) {
        if (segment.kind == MixedRomajiSegmentKind::English &&
            segment.confidence == EnglishSegmentConfidence::High)
          sure.insert({segment.raw_begin, segment.raw_end, segment.surface});
      }
    }
    size_t hits = 0;
    for (const auto& s : sure) hits += item->expected.count(s);
    predicted += sure.size();
    correct += hits;
    if (item->category == "mixed_english") expected_total += item->expected.size();
    if (item->category == "japanese_only") {
      ++japanese_cases;
      if (!sure.empty()) ++japanese_sure;
    }
    if (item->category == "romaji_like_english") romaji_like_sure_wrong += sure.size() - hits;
    const bool wrong = hits != sure.size();
    if (per_case || wrong) {
      j::Object record;
      record.emplace("id", j::Value(item->id));
      record.emplace("category", j::Value(item->category));
      j::Array got;
      for (const auto& s : sure) {
        j::Object o;
        o.emplace("begin", j::Value(static_cast<uint64_t>(s.begin)));
        o.emplace("end", j::Value(static_cast<uint64_t>(s.end)));
        o.emplace("surface", j::Value(s.surface));
        o.emplace("correct", j::Value(item->expected.count(s) > 0));
        got.emplace_back(std::move(o));
      }
      record.emplace("sure_english", j::Value(std::move(got)));
      if (wrong) failures.emplace_back(record);
      if (per_case) std::cerr << j::Stringify(j::Value(std::move(record))) << "\n";
    }
  }
  const double precision =
      predicted == 0 ? 1.0 : static_cast<double>(correct) / static_cast<double>(predicted);
  const double recall = expected_total == 0
                            ? 0.0
                            : static_cast<double>(correct) / static_cast<double>(expected_total);
  const bool passed = invalid == 0 && cases > 0 && precision >= min_precision && japanese_sure == 0;
  j::Object summary;
  summary.emplace("bench", j::Value("azookey_mixed_english_eval"));
  summary.emplace("schemaVersion", j::Value(1));
  summary.emplace("cases", j::Value(static_cast<uint64_t>(cases)));
  summary.emplace("invalid", j::Value(static_cast<uint64_t>(invalid)));
  summary.emplace("sureEnglish", j::Value(static_cast<uint64_t>(predicted)));
  summary.emplace("sureEnglishCorrect", j::Value(static_cast<uint64_t>(correct)));
  summary.emplace("precision", j::Value(precision));
  summary.emplace("minPrecision", j::Value(min_precision));
  summary.emplace("recall", j::Value(recall));
  summary.emplace("japaneseOnlyCases", j::Value(static_cast<uint64_t>(japanese_cases)));
  summary.emplace("japaneseOnlyWithSureEnglish", j::Value(static_cast<uint64_t>(japanese_sure)));
  summary.emplace("romajiLikeSureWrong", j::Value(static_cast<uint64_t>(romaji_like_sure_wrong)));
  summary.emplace("inlineHighConfidence", j::Value(options.inline_high_confidence));
  summary.emplace("failures", j::Value(std::move(failures)));
  summary.emplace("passed", j::Value(passed));
  std::cout << j::Stringify(j::Value(std::move(summary))) << "\n";
  return passed ? 0 : 1;
}
