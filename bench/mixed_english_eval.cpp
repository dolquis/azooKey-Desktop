// M63-B section 15.2 quality gate for the mixed lattice
// (docs/mixed-english-auto-conversion-spec.md, docs/conversion-quality-benchmark-spec.md
// section 4.4). It measures the splitter only: which E segments are sure enough to turn
// English in the live conversion and the first candidate. No model is involved.
//
//   azookey_mixed_english_eval --data <jsonl> --dictionary <english-words.tsv>
//       [--min-precision 0.95] [--per-case] [--no-inline-high-confidence]
//       [--model <gguf> [--bundled-dictionaries <dir>] [--threads N]
//        [--conversion-deadline-ms N]]
//
// Prints one JSON summary line and exits 1 when the gate fails: invalid cases,
// precision below the minimum, or a sure E segment in a Japanese-only sentence.
//
// With --model, it also asks a Host engine with that model for the first
// candidate and the live conversion of each case with M63 on and off (the
// section 15.2 comparison on the real model, recorded by hand, never in CI) and
// adds a "host" object to the summary. Speed is not measured. The dictionary is
// copied to a temporary directory first, as the Host's dictionary store writes
// files next to it.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "azookey/core/MixedRomajiSplitter.h"
#include "azookey/core/PlatformPaths.h"
#include "azookey/core/RomajiKanaConverter.h"
#include "azookey/core/SimpleConverter.h"
#include "azookey/host/Dispatcher.h"
#include "azookey/host/EnglishCandidates.h"
#include "azookey/host/InferenceEngine.h"
#include "azookey/host/RequestScheduler.h"
#include "azookey/ipc/Json.h"
#include "azookey/ipc/Payloads.h"
#include "azookey/learning/LearningStore.h"

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
               "[--min-precision 0.95] [--per-case] [--no-inline-high-confidence] "
               "[--model <gguf> [--bundled-dictionaries <dir>] [--threads N] "
               "[--conversion-deadline-ms N]]\n";
  return 2;
}

// The --model comparison: one engine and Dispatcher, asked with M63 on and off.
class HostComparison {
 public:
  struct Answer {
    std::string first;
    size_t first_segments{};
    std::string live;
    size_t live_segments{};
  };

  // Throws std::runtime_error when the model or the dictionary cannot be used.
  HostComparison(const std::string& model, const std::string& dictionary,
                 const std::string& bundled, std::optional<int32_t> threads,
                 std::chrono::milliseconds conversion_deadline)
      : directory_(std::filesystem::temp_directory_path() /
                   ("azookey_mixed_english_eval-" +
                    std::to_string(std::chrono::system_clock::now().time_since_epoch().count()))) {
    std::filesystem::create_directories(directory_);
    const auto copied = directory_ / "english-words.tsv";
    std::filesystem::copy_file(azookey::core::Utf8Path(dictionary), copied);
    store_ =
        std::make_unique<azookey::learning::LearningStore>((directory_ / "learning.tsv").string());
    azookey::host::EngineConfig config;
    config.english.dictionary_enabled = true;
    config.english.dictionary_path = azookey::core::PathToUtf8(copied);
    engine_ = std::make_unique<azookey::host::InferenceEngine>(
        std::make_unique<azookey::core::SimpleConverter>(), store_.get(), config);
    if (!bundled.empty()) {
      for (const auto& result :
           engine_->LoadBundledDictionaryLayers(azookey::core::Utf8Path(bundled)))
        if (!result.loaded)
          std::cerr << "dictionary " << result.name << ": " << result.error << "\n";
    }
    azookey::host::ModelLoadOptions load;
    load.path = model;
    load.n_threads = threads;
    const auto loaded = engine_->LoadModelWithResult(load);
    if (!loaded.ok) throw std::runtime_error("cannot load the model: " + loaded.error.value_or(""));
    azookey::host::DispatcherConfig dispatcher_config;
    // Quality, not speed: by default an hour, as the section 10 deadline would
    // drop conversions on a busy machine. Each conversion keeps the Zenzai budget.
    dispatcher_config.mixed_english_conversion_deadline = conversion_deadline;
    dispatcher_ = std::make_unique<azookey::host::Dispatcher>(engine_.get(), &scheduler_, nullptr,
                                                              dispatcher_config);
  }

  ~HostComparison() {
    dispatcher_.reset();
    engine_.reset();
    store_.reset();
    std::error_code ignored;
    std::filesystem::remove_all(directory_, ignored);
  }

  Answer Ask(const std::string& raw_romaji, const std::string& kana, bool mixed) {
    Answer answer;
    azookey::ipc::QueryCandidatesRequest query;
    query.reading = kana;
    query.raw_romaji = raw_romaji;
    query.english_candidates = true;
    query.mixed_english = mixed;
    query.secure = false;
    if (const auto response = Dispatch(azookey::ipc::MessageType::QueryCandidates,
                                       azookey::ipc::BuildQueryCandidatesRequest(query))) {
      if (const auto parsed = azookey::ipc::ParseQueryCandidatesResponse(response->payload_json);
          parsed && !parsed->candidates.empty()) {
        answer.first = parsed->candidates.front().surface;
        answer.first_segments = parsed->candidates.front().segments.size();
      }
    }
    azookey::ipc::QueryLiveConversionRequest live{kana, ""};
    live.raw_romaji = raw_romaji;
    live.mixed_english = mixed;
    live.secure = false;
    if (const auto response = Dispatch(azookey::ipc::MessageType::QueryLiveConversion,
                                       azookey::ipc::BuildQueryLiveConversionRequest(live))) {
      if (const auto parsed =
              azookey::ipc::ParseQueryLiveConversionResponse(response->payload_json)) {
        answer.live = parsed->surface;
        answer.live_segments = parsed->segments.size();
      }
    }
    return answer;
  }

 private:
  std::optional<azookey::ipc::Envelope> Dispatch(azookey::ipc::MessageType type,
                                                 std::string payload) {
    azookey::ipc::Envelope request;
    request.version = 1;
    request.request_id = ++request_id_;
    request.type = type;
    request.payload_json = std::move(payload);
    return dispatcher_->Dispatch(request);
  }

  std::filesystem::path directory_;
  std::unique_ptr<azookey::learning::LearningStore> store_;
  std::unique_ptr<azookey::host::InferenceEngine> engine_;
  azookey::host::RequestScheduler scheduler_;
  std::unique_ptr<azookey::host::Dispatcher> dispatcher_;
  uint64_t request_id_{};
};

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
  std::string model_path;
  std::string bundled_path;
  std::optional<int32_t> threads;
  std::chrono::milliseconds conversion_deadline = std::chrono::hours(1);
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
    } else if (arg == "--model" && i + 1 < argc) {
      model_path = argv[++i];
    } else if (arg == "--bundled-dictionaries" && i + 1 < argc) {
      bundled_path = argv[++i];
    } else if (arg == "--conversion-deadline-ms" && i + 1 < argc) {
      conversion_deadline = std::chrono::milliseconds(std::strtoll(argv[++i], nullptr, 10));
    } else if (arg == "--threads" && i + 1 < argc) {
      threads = static_cast<int32_t>(std::strtol(argv[++i], nullptr, 10));
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
  std::unique_ptr<HostComparison> host;
  if (!model_path.empty()) {
    if (!options.inline_high_confidence) return Fail("--model compares the Host as shipped");
    try {
      host = std::make_unique<HostComparison>(model_path, dictionary_path, bundled_path, threads,
                                              conversion_deadline);
    } catch (const std::exception& e) {
      return Fail(e.what());
    }
  }
  // Section 15.2 on the real model: what M63 on changes against off.
  size_t japanese_first_changed = 0;
  size_t japanese_live_changed = 0;
  size_t mixed_cases = 0;
  size_t mixed_first_with_segments = 0;
  size_t mixed_first_with_expected = 0;
  size_t live_sure_inline = 0;  // Cases whose split has a sure in-sentence E segment.
  size_t live_reconverted = 0;  // ... whose live conversion lost it (section 10).
  size_t romaji_like_first_changed = 0;
  j::Array host_changes;
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
    if (host) {
      const auto kana =
          azookey::core::RomajiKanaConverter::ConvertForCommit(Lower(item->raw_romaji));
      const auto off = host->Ask(item->raw_romaji, kana, false);
      const auto on = host->Ask(item->raw_romaji, kana, true);
      const bool inline_sure =
          !splits.empty() && splits.front().segments.size() > 1 && !sure.empty();
      if (inline_sure) {
        ++live_sure_inline;
        if (on.live_segments == 0) ++live_reconverted;
      }
      bool changed = false;
      if (item->category == "japanese_only") {
        japanese_first_changed += on.first != off.first;
        japanese_live_changed += on.live != off.live;
        changed = on.first != off.first || on.live != off.live;
      } else if (item->category == "romaji_like_english") {
        romaji_like_first_changed += on.first != off.first;
        changed = on.first != off.first;
      } else {
        ++mixed_cases;
        mixed_first_with_segments += on.first_segments > 0;
        bool all = !item->expected.empty();
        for (const auto& e : item->expected)
          all = all && on.first.find(e.surface) != std::string::npos;
        mixed_first_with_expected += all;
      }
      j::Object record;
      record.emplace("id", j::Value(item->id));
      record.emplace("category", j::Value(item->category));
      record.emplace("offFirst", j::Value(off.first));
      record.emplace("onFirst", j::Value(on.first));
      record.emplace("onFirstSegments", j::Value(static_cast<uint64_t>(on.first_segments)));
      record.emplace("offLive", j::Value(off.live));
      record.emplace("onLive", j::Value(on.live));
      record.emplace("onLiveSegments", j::Value(static_cast<uint64_t>(on.live_segments)));
      if (per_case) std::cerr << j::Stringify(j::Value(record)) << "\n";
      if (changed) host_changes.emplace_back(std::move(record));
    }
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
  if (host) {
    j::Object compared;
    compared.emplace("japaneseOnlyFirstChanged",
                     j::Value(static_cast<uint64_t>(japanese_first_changed)));
    compared.emplace("japaneseOnlyLiveChanged",
                     j::Value(static_cast<uint64_t>(japanese_live_changed)));
    compared.emplace("romajiLikeFirstChanged",
                     j::Value(static_cast<uint64_t>(romaji_like_first_changed)));
    compared.emplace("mixedCases", j::Value(static_cast<uint64_t>(mixed_cases)));
    compared.emplace("mixedFirstWithSegments",
                     j::Value(static_cast<uint64_t>(mixed_first_with_segments)));
    compared.emplace("mixedFirstWithExpectedEnglish",
                     j::Value(static_cast<uint64_t>(mixed_first_with_expected)));
    compared.emplace("liveSureInline", j::Value(static_cast<uint64_t>(live_sure_inline)));
    compared.emplace("liveReconverted", j::Value(static_cast<uint64_t>(live_reconverted)));
    compared.emplace("conversionDeadlineMs",
                     j::Value(static_cast<uint64_t>(conversion_deadline.count())));
    compared.emplace("changed", j::Value(std::move(host_changes)));
    summary.emplace("host", j::Value(std::move(compared)));
  }
  summary.emplace("passed", j::Value(passed));
  std::cout << j::Stringify(j::Value(std::move(summary))) << "\n";
  return passed ? 0 : 1;
}
