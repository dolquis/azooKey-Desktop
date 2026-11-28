#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "BenchmarkCommandLine.h"
#include "BenchmarkCommit.h"
#include "BenchmarkResult.h"
#include "ConversionQuality.h"
#include "IpcBenchmark.h"
#include "PersonalEval.h"
#include "TemporaryLearningFile.h"
#include "azookey/core/DoubleArrayTrie.h"
#include "azookey/core/LatticeDictionary.h"
#include "azookey/core/LatticeSearch.h"
#include "azookey/core/SimpleConverter.h"
#include "azookey/host/InferenceEngine.h"
#include "azookey/host/UserDataPaths.h"
#include "azookey/ipc/Json.h"
#include "azookey/ipc/TraceId.h"
#include "azookey/learning/DpapiCrypto.h"
#include "azookey/learning/Sha256.h"
#include "azookey/learning/TypoLearningStore.h"
#include "azookey/logging/Phase.h"
#include "azookey/logging/RuntimeLogger.h"

#ifndef AZOOKEY_BENCH_COMMIT
#define AZOOKEY_BENCH_COMMIT "unknown"
#endif

#ifndef AZOOKEY_BENCH_CONFIG
#define AZOOKEY_BENCH_CONFIG "unknown"
#endif

namespace {

constexpr size_t kCurrentZenzaiBeamWidth = 4;
constexpr size_t kCurrentZenzaiMaxNewTokens = 64;
constexpr size_t kCurrentZenzaiContextBatchSize = 512;
constexpr size_t kCurrentZenzaiPromptTemplateVersion = 1;

void PrintUsage(const char* exe) {
  std::cerr
      << "Usage: " << exe
      << " [--max-p95-ms N] [--ipc] [--json] [--output PATH] [--baseline PATH]"
         " [--backend cpu|cuda] [--model PATH] [--trace [--trace-output PATH]]\n"
      << "       " << exe
      << " --eval JSONL --output JSON [--per-case JSONL] [--baseline JSON]"
         " [--backend cpu|cuda] [--model PATH] [--category NAME]"
         " [--iterations N] [--typo-mode off|suggest|rank|aggressive]"
         " [--trace [--trace-output PATH]]"
         " [--nll on|off] [--decode-stats JSONL] [--dict-dir DIR [--lattice on|off]]"
         " [--lattice-with-model K [--lattice-band best|lowest]] [--first-clause-probe]"
         " [--profile TEXT | --profile-file PATH]\n"
      << "       " << exe
      << " --personal-eval --output JSON [--learning PATH] [--settings PATH] [--max-cases N]"
         " [--per-case JSONL --include-personal-text] [--baseline JSON] [--backend cpu|cuda]"
         " [--model PATH] [--nll on|off] [--dict-dir DIR [--lattice on|off]] [--iterations N]"
         " [--profile TEXT | --profile-file PATH]\n"
      << "Reports latency metrics by default. Pass --max-p95-ms to make p95 a hard "
         "failure gate. --json selects JSON stdout; --output writes the same JSON "
         "schema to a file. --ipc adds codec, Windows pipe echo and send/Host/receive "
         "timings. --eval selects the conversion-quality evaluator. --trace writes phase JSONL "
         "to trace.jsonl by default. --nll sets NLL rerank (default off, the shipped "
         "setting). --decode-stats writes one line per query with its latency, deadline "
         "cut-off and candidate sources. --dict-dir loads the bundled static layers of DIR "
         "(the dict directory of dictbuild/build_bundled.py); --lattice defaults to on with it "
         "and then needs DIR/lattice_core.azdic. --lattice-with-model K (1 to 8) turns on "
         "lattice stage 2 for measurement: the lattice's first K paths join NLL rerank below "
         "the model's best candidate (--lattice-band best, the default) or its lowest; it needs "
         "--model, --nll on and --lattice on. --first-clause-probe also asks each reading's "
         "first segment (the lattice's best path) after the reading, as a first segment's "
         "request, and prints the latency of the two; it needs --model and --dict-dir. "
         "--personal-eval "
         "evaluates your own learning store, read without changing it, and writes aggregates "
         "only; --profile places a bench-only Zenzai profile in the prompt and needs --model.\n";
}

// The options a personal run accepts (conversion-quality-benchmark-spec section 15.5).
const std::set<std::string, std::less<>> kPersonalEvalOptions = {
    "--personal-eval", "--learning",     "--settings", "--max-cases", "--include-personal-text",
    "--profile",       "--profile-file", "--output",   "--baseline",  "--per-case",
    "--backend",       "--model",        "--dict-dir", "--lattice",   "--nll",
    "--iterations",
};

size_t ParseCount(const std::string& value, const char* name) {
  // Digits only: std::stoull also takes leading spaces and a sign, so " -1" would wrap around.
  const bool digits = !value.empty() && std::all_of(value.begin(), value.end(),
                                                    [](char ch) { return ch >= '0' && ch <= '9'; });
  try {
    if (!digits) throw std::invalid_argument("invalid value");
    return static_cast<size_t>(std::stoull(value));
  } catch (const std::exception&) {
    throw std::invalid_argument(std::string(name) + " must be a non-negative integer");
  }
}

const char* CandidateSourceName(azookey::core::CandidateSource source) {
  using azookey::core::CandidateSource;
  switch (source) {
    case CandidateSource::SystemDictionary:
      return "system_dictionary";
    case CandidateSource::UserDictionary:
      return "user_dictionary";
    case CandidateSource::Model:
      return "model";
    case CandidateSource::Llm:
      return "llm";
    case CandidateSource::Heuristic:
      return "heuristic";
    case CandidateSource::Learning:
      return "learning";
    case CandidateSource::Symbol:
      return "symbol";
    case CandidateSource::Emoji:
      return "emoji";
  }
  return "unknown";
}

bool HasDebugTag(const std::string& debug_info, std::string_view tag) {
  size_t start = 0;
  while (start <= debug_info.size()) {
    const auto end = debug_info.find(';', start);
    const auto length = (end == std::string::npos ? debug_info.size() : end) - start;
    if (std::string_view(debug_info).substr(start, length) == tag) return true;
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return false;
}

// DEV-2110 (segment-editing-spec section 9): the legacy Space candidates put the first segment's
// candidates first, which asks Zenzai twice, for the reading and then for its first segment. The
// probe takes the first segment from the lattice's best path of the whole reading
// (lattice-conversion-spec section 7.1) and times the first segment's request after the reading's.
class FirstClauseProbe {
 public:
  explicit FirstClauseProbe(const std::filesystem::path& dict_dir) {
    const auto core_path = dict_dir / "lattice_core.azdic";
    if (!dictionary_.Load(core_path))
      throw std::runtime_error("--first-clause-probe could not load lattice_core.azdic: " +
                               std::string(dictionary_.Error()));
    for (const char* name : {"base_lexicon", "sudachi_lexicon", "neologd_lexicon",
                             "named_entity_lexicon", "technical_terms_lexicon"}) {
      const auto path = dict_dir / (std::string(name) + ".azdic");
      if (!std::filesystem::exists(path)) continue;
      auto trie = std::make_unique<azookey::core::DoubleArrayTrie>();
      if (!trie->Load(path))
        throw std::runtime_error("--first-clause-probe could not load " + std::string(name));
      layers_.push_back(
          std::make_unique<azookey::core::StaticLatticeVocabulary>(dictionary_, *trie));
      vocabularies_.push_back(layers_.back().get());
      tries_.push_back(std::move(trie));
    }
  }

  // The first segment's end in code points, or nullopt when the best path is one segment.
  std::optional<uint32_t> FirstSegmentEnd(const std::string& kana) const {
    azookey::core::LatticeQuery query;
    query.max_paths = 1;
    const auto result = azookey::core::ConvertLattice(azookey::core::NormalizeReading(kana),
                                                      dictionary_, vocabularies_, query);
    if (result.paths.empty() || result.paths.front().segments.size() < 2) return std::nullopt;
    return result.paths.front().segments.front().end;
  }

 private:
  azookey::core::LatticeDictionary dictionary_;
  std::vector<std::unique_ptr<azookey::core::DoubleArrayTrie>> tries_;
  std::vector<std::unique_ptr<azookey::core::StaticLatticeVocabulary>> layers_;
  std::vector<const azookey::core::ILatticeVocabulary*> vocabularies_;
};

// The byte offset of code point `count` of UTF-8 `text`.
size_t CodepointOffset(const std::string& text, uint32_t count) {
  size_t offset = 0;
  for (uint32_t seen = 0; offset < text.size(); ++offset) {
    if ((static_cast<unsigned char>(text[offset]) & 0xC0) == 0x80) continue;
    if (seen++ == count) break;
  }
  return offset;
}

// The first segment's request of one query (DEV-2110).
struct FirstClauseSample {
  uint32_t end{};
  double latency_ms{};
  bool deadline_exceeded{};
  std::string top1;
};

// One line per query, so latency quantiles can exclude deadline cut-offs
// (docs/zenzai-inference-spec.md section 8.1) and unfinished generations can be counted. Taken
// right after the query, before another request replaces the engine's last decode stats.
azookey::ipc::json::Object DecodeStatsLine(
    uint64_t call, const std::string& input, const std::string& context, double latency_ms,
    azookey::host::InferenceEngine& engine,
    const std::vector<azookey::core::Candidate>& candidates) {
  namespace j = azookey::ipc::json;
  j::Object line;
  line.emplace("call", j::Value(call));
  line.emplace("input", j::Value(input));
  line.emplace("left_context", j::Value(context));
  line.emplace("latency_ms", j::Value(latency_ms));
  const auto stats = engine.last_zenzai_decode_stats();
  line.emplace("deadline_exceeded", stats ? j::Value(stats->deadline_exceeded) : j::Value());
  line.emplace("beam_tokens", stats ? j::Value(stats->beam_tokens) : j::Value());
  const auto error = engine.effective_last_error();
  line.emplace("last_error", error ? j::Value(*error) : j::Value());
  j::Array values;
  values.reserve(candidates.size());
  for (const auto& candidate : candidates) {
    j::Object value;
    value.emplace("surface", j::Value(candidate.surface));
    value.emplace("source", j::Value(CandidateSourceName(candidate.source)));
    value.emplace("unfinished", j::Value(HasDebugTag(candidate.debug_info, "unfinished")));
    // Lattice stage 2 (DEV-2102): a path it added, and whether NLL rerank scored the candidate.
    value.emplace("lattice_model", j::Value(candidate.debug_info.starts_with("lattice-model;")));
    value.emplace("nll_scored", j::Value(candidate.debug_info.starts_with("nll=") ||
                                         candidate.debug_info.find(";nll=") != std::string::npos));
    values.emplace_back(std::move(value));
  }
  line.emplace("candidates", j::Value(std::move(values)));
  return line;
}

azookey::ipc::json::Value FirstClauseValue(const FirstClauseSample& sample) {
  namespace j = azookey::ipc::json;
  j::Object probe;
  probe.emplace("end", j::Value(static_cast<uint64_t>(sample.end)));
  probe.emplace("latency_ms", j::Value(sample.latency_ms));
  probe.emplace("deadline_exceeded", j::Value(sample.deadline_exceeded));
  probe.emplace("top1", j::Value(sample.top1));
  return j::Value(std::move(probe));
}

double ParseDouble(const std::string& value, const char* name) {
  try {
    size_t idx = 0;
    const auto parsed = std::stod(value, &idx);
    if (idx != value.size()) {
      throw std::invalid_argument("trailing characters");
    }
    return parsed;
  } catch (const std::exception& ex) {
    throw std::invalid_argument(std::string(name) + " must be a number: " + ex.what());
  }
}

std::string RequireValue(int argc, char** argv, int& index, const char* option) {
  if (index + 1 >= argc) {
    throw std::invalid_argument(std::string(option) + " requires a value");
  }
  ++index;
  return argv[index];
}

size_t ParseSize(const std::string& value, const char* name) {
  try {
    size_t index = 0;
    const auto parsed = std::stoull(value, &index);
    if (index != value.size() || parsed == 0) throw std::invalid_argument("invalid value");
    return static_cast<size_t>(parsed);
  } catch (const std::exception& ex) {
    throw std::invalid_argument(std::string(name) + " must be a positive integer: " + ex.what());
  }
}

azookey::logging::RuntimeLoggerOptions TraceLoggerOptions(const std::filesystem::path& output_path,
                                                          std::string component) {
  azookey::logging::RuntimeLoggerOptions options;
  options.enabled = true;
  options.component = std::move(component);
  options.output_path = output_path;
  // A benchmark trace must retain all samples instead of rotating at the runtime default (5 MiB).
  options.max_file_bytes = uintmax_t{1} << 40;
  return options;
}

// `raw_keys` (typo-correction-learning-spec section 12.13) goes to the engine as a Host request's
// does; empty passes no options, so the reading alone makes the M55 hypotheses.
azookey::host::InferenceEngine::CandidatesResult QueryCandidates(
    azookey::host::InferenceEngine& engine, const std::string& kana, const std::string& context,
    uint64_t now_epoch_sec, uint32_t max_candidates, azookey::logging::RuntimeLogger* bench_logger,
    uint64_t& request_id, const std::string& backend, const std::string& raw_keys = {}) {
  azookey::host::MixedConversionOptions options;
  options.raw_keys = raw_keys;
  const auto* mixed = raw_keys.empty() ? nullptr : &options;
  if (!bench_logger) {
    return engine.QueryCandidatesEx(kana, context, now_epoch_sec, nullptr, max_candidates, false,
                                    nullptr, nullptr, mixed);
  }
  const auto trace_id = azookey::ipc::GenerateTraceId();
  const azookey::host::InferenceTelemetry telemetry{++request_id, {}, trace_id};
  const auto start = std::chrono::steady_clock::now();
  auto candidates = engine.QueryCandidatesEx(kana, context, now_epoch_sec, nullptr, max_candidates,
                                             false, &telemetry, nullptr, mixed);
  const auto latency_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  bench_logger->Log(azookey::logging::RuntimeLogLevel::Info, "latency_phase",
                    {{"request_id", telemetry.request_id},
                     {"trace_id", azookey::logging::RuntimeLogSafeText(trace_id)},
                     {"phase", azookey::logging::RuntimeLogSafeText(std::string(
                                   azookey::logging::PhaseName(azookey::logging::Phase::Total)))},
                     {"latency_ms", latency_ms},
                     {"backend", azookey::logging::RuntimeLogSafeText(backend)},
                     {"result", azookey::logging::RuntimeLogSafeText("ok")}});
  return candidates;
}

bool SamePath(const std::filesystem::path& left, const std::filesystem::path& right) {
  return !left.empty() && !right.empty() &&
         std::filesystem::weakly_canonical(std::filesystem::absolute(left)) ==
             std::filesystem::weakly_canonical(std::filesystem::absolute(right));
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> utf8_args;
  try {
    utf8_args = azookey::bench::Utf8CommandLineArguments(argc, argv);
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << std::endl;
    return 2;
  }
  std::vector<char*> utf8_argv;
  utf8_argv.reserve(utf8_args.size());
  for (auto& arg : utf8_args) {
    utf8_argv.push_back(arg.data());
  }
  argc = static_cast<int>(utf8_argv.size());
  argv = utf8_argv.data();

  std::optional<double> max_p95_ms;
  bool json_output = false;
  bool ipc_benchmark = false;
  std::filesystem::path output_path;
  std::filesystem::path baseline_path;
  std::filesystem::path trace_output_path;
  std::filesystem::path decode_stats_path;
  std::filesystem::path dict_dir;
  std::optional<bool> nll_enabled;
  std::optional<bool> lattice_enabled;
  // Lattice stage 2 (lattice-conversion-spec section 3.3, DEV-2102): K paths, and the band.
  std::optional<size_t> lattice_with_model;
  std::optional<std::string> lattice_band;
  // DEV-2110: the two Zenzai requests of Space with the first segment's candidates first.
  bool first_clause_probe = false;
  azookey::bench::ConversionQualityOptions quality_options;
  bool personal_eval = false;
  std::filesystem::path learning_path;
  std::filesystem::path settings_path;
  std::optional<size_t> max_cases;
  std::optional<std::string> profile;
  std::filesystem::path profile_file;
  std::set<std::string, std::less<>> seen_options;
  // A personal run or a profile may put private words on the command line, so an unknown
  // token is reported by position only (spec section 15.7).
  bool hide_arguments = false;
  for (int i = 1; i < argc; ++i) {
    // A prefix, so that a mistyped "--profile=TEXT" hides its text as well.
    const std::string_view arg = argv[i];
    hide_arguments =
        hide_arguments || arg.starts_with("--personal-eval") || arg.starts_with("--profile");
  }
  try {
    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--help" || arg == "-h") {
        PrintUsage(argv[0]);
        return 0;
      }
      if (arg.rfind("--", 0) == 0) seen_options.insert(arg);
      if (arg == "--personal-eval") {
        personal_eval = true;
      } else if (arg == "--learning") {
        const auto value = RequireValue(argc, argv, i, "--learning");
        if (value.empty()) throw std::invalid_argument("--learning requires a non-empty path");
        learning_path = azookey::bench::Utf8Path(value);
      } else if (arg == "--settings") {
        const auto value = RequireValue(argc, argv, i, "--settings");
        if (value.empty()) throw std::invalid_argument("--settings requires a non-empty path");
        settings_path = azookey::bench::Utf8Path(value);
      } else if (arg == "--max-cases") {
        max_cases = ParseCount(RequireValue(argc, argv, i, "--max-cases"), "--max-cases");
      } else if (arg == "--include-personal-text") {
        quality_options.include_personal_text = true;
      } else if (arg == "--profile") {
        profile = RequireValue(argc, argv, i, "--profile");
      } else if (arg == "--profile-file") {
        const auto value = RequireValue(argc, argv, i, "--profile-file");
        if (value.empty()) throw std::invalid_argument("--profile-file requires a non-empty path");
        profile_file = azookey::bench::Utf8Path(value);
      } else if (arg == "--max-p95-ms") {
        max_p95_ms = ParseDouble(RequireValue(argc, argv, i, "--max-p95-ms"), "--max-p95-ms");
      } else if (arg == "--json") {
        json_output = true;
      } else if (arg == "--ipc") {
        ipc_benchmark = true;
      } else if (arg == "--output") {
        output_path = azookey::bench::Utf8Path(RequireValue(argc, argv, i, "--output"));
      } else if (arg == "--baseline") {
        baseline_path = azookey::bench::Utf8Path(RequireValue(argc, argv, i, "--baseline"));
      } else if (arg == "--eval") {
        quality_options.eval_path = azookey::bench::Utf8Path(RequireValue(argc, argv, i, "--eval"));
      } else if (arg == "--per-case") {
        quality_options.per_case_path =
            azookey::bench::Utf8Path(RequireValue(argc, argv, i, "--per-case"));
      } else if (arg == "--backend") {
        quality_options.backend = RequireValue(argc, argv, i, "--backend");
        if (quality_options.backend != "cpu" && quality_options.backend != "cuda") {
          throw std::invalid_argument("--backend must be cpu or cuda");
        }
      } else if (arg == "--model") {
        quality_options.model = RequireValue(argc, argv, i, "--model");
      } else if (arg == "--category") {
        quality_options.category = RequireValue(argc, argv, i, "--category");
      } else if (arg == "--iterations") {
        quality_options.iterations =
            ParseSize(RequireValue(argc, argv, i, "--iterations"), "--iterations");
      } else if (arg == "--typo-mode") {
        quality_options.typo_mode = RequireValue(argc, argv, i, "--typo-mode");
      } else if (arg == "--trace") {
        quality_options.trace = true;
      } else if (arg == "--trace-output") {
        const auto value = RequireValue(argc, argv, i, "--trace-output");
        if (value.empty()) throw std::invalid_argument("--trace-output requires a non-empty path");
        trace_output_path = azookey::bench::Utf8Path(value);
      } else if (arg == "--nll") {
        const auto value = RequireValue(argc, argv, i, "--nll");
        if (value != "on" && value != "off") throw std::invalid_argument("--nll must be on or off");
        nll_enabled = value == "on";
      } else if (arg == "--decode-stats") {
        const auto value = RequireValue(argc, argv, i, "--decode-stats");
        if (value.empty()) throw std::invalid_argument("--decode-stats requires a non-empty path");
        decode_stats_path = azookey::bench::Utf8Path(value);
      } else if (arg == "--dict-dir") {
        const auto value = RequireValue(argc, argv, i, "--dict-dir");
        if (value.empty()) throw std::invalid_argument("--dict-dir requires a non-empty path");
        dict_dir = azookey::bench::Utf8Path(value);
      } else if (arg == "--lattice") {
        const auto value = RequireValue(argc, argv, i, "--lattice");
        if (value != "on" && value != "off") {
          throw std::invalid_argument("--lattice must be on or off");
        }
        lattice_enabled = value == "on";
      } else if (arg == "--lattice-with-model") {
        const auto paths =
            ParseSize(RequireValue(argc, argv, i, "--lattice-with-model"), "--lattice-with-model");
        if (paths > 8) throw std::invalid_argument("--lattice-with-model must be 1 to 8");
        lattice_with_model = paths;
      } else if (arg == "--lattice-band") {
        const auto value = RequireValue(argc, argv, i, "--lattice-band");
        if (value != "best" && value != "lowest")
          throw std::invalid_argument("--lattice-band must be best or lowest");
        lattice_band = value;
      } else if (arg == "--first-clause-probe") {
        first_clause_probe = true;
      } else if (hide_arguments) {
        throw std::invalid_argument("unknown option at argument " + std::to_string(i));
      } else {
        throw std::invalid_argument("unknown option: " + arg);
      }
    }
    if (personal_eval) {
      for (const auto& option : seen_options) {
        if (!kPersonalEvalOptions.contains(option)) {
          throw std::invalid_argument(option + " cannot be combined with --personal-eval");
        }
      }
      if (output_path.empty()) {
        throw std::invalid_argument("--output is required with --personal-eval");
      }
      if (!quality_options.per_case_path.empty() && !quality_options.include_personal_text) {
        throw std::invalid_argument(
            "--per-case requires --include-personal-text with --personal-eval");
      }
    } else if (!learning_path.empty() || !settings_path.empty() || max_cases ||
               quality_options.include_personal_text) {
      throw std::invalid_argument(
          "--learning, --settings, --max-cases and --include-personal-text require "
          "--personal-eval");
    }
    // A personal run is a conversion-quality run without an --eval file.
    const bool quality_run = !quality_options.eval_path.empty() || personal_eval;
    if (profile && !profile_file.empty()) {
      throw std::invalid_argument("--profile and --profile-file cannot be combined");
    }
    if ((profile || !profile_file.empty()) && !quality_run) {
      throw std::invalid_argument("--profile requires --eval or --personal-eval");
    }
    if ((profile || !profile_file.empty()) && quality_options.model.empty()) {
      throw std::invalid_argument("--profile requires --model");
    }
    if (!profile_file.empty()) {
      std::string profile_error;
      profile = azookey::bench::ReadBenchProfileFile(profile_file, &profile_error);
      if (!profile) throw std::invalid_argument(profile_error);
    } else if (profile) {
      std::string profile_error;
      if (!azookey::bench::ValidateBenchProfile(*profile, &profile_error)) {
        throw std::invalid_argument(profile_error);
      }
    }
    if (!quality_run && (!dict_dir.empty() || lattice_enabled)) {
      throw std::invalid_argument("--dict-dir and --lattice require --eval or --personal-eval");
    }
    // The shipped default is on, so a run with the bundled dictionaries is on unless told
    // otherwise (conversion-quality-benchmark-spec section 7).
    if (!lattice_enabled) lattice_enabled = !dict_dir.empty();
    if (*lattice_enabled && dict_dir.empty()) {
      throw std::invalid_argument("--lattice on requires --dict-dir");
    }
    if (quality_options.typo_mode != "off" && quality_options.typo_mode != "suggest" &&
        quality_options.typo_mode != "rank" && quality_options.typo_mode != "aggressive") {
      throw std::invalid_argument("--typo-mode must be off, suggest, rank or aggressive");
    }
    if (!quality_run && (!quality_options.per_case_path.empty() ||
                         quality_options.category != "all" || quality_options.typo_mode != "off")) {
      throw std::invalid_argument("conversion-quality options require --eval");
    }
    if (!quality_run && nll_enabled) {
      throw std::invalid_argument("--nll requires --eval or --personal-eval");
    }
    if (quality_options.eval_path.empty() && !decode_stats_path.empty()) {
      throw std::invalid_argument("--decode-stats requires --eval");
    }
    if (nll_enabled && quality_options.model.empty()) {
      throw std::invalid_argument("--nll requires --model");
    }
    // Stage 2 adds only the paths NLL scored, on requests the model answered.
    if (lattice_band && !lattice_with_model) {
      throw std::invalid_argument("--lattice-band requires --lattice-with-model");
    }
    if (lattice_with_model && (personal_eval || quality_options.eval_path.empty() ||
                               !nll_enabled.value_or(false) || !*lattice_enabled)) {
      throw std::invalid_argument(
          "--lattice-with-model requires --eval, --model, --nll on and --lattice on");
    }
    if (first_clause_probe && (personal_eval || quality_options.eval_path.empty() ||
                               quality_options.model.empty() || dict_dir.empty())) {
      throw std::invalid_argument("--first-clause-probe requires --eval, --model and --dict-dir");
    }
    if (!decode_stats_path.empty() &&
        (SamePath(decode_stats_path, output_path) ||
         SamePath(decode_stats_path, quality_options.per_case_path) ||
         SamePath(decode_stats_path, quality_options.eval_path) ||
         SamePath(decode_stats_path, baseline_path) ||
         SamePath(decode_stats_path, trace_output_path) ||
         SamePath(decode_stats_path, azookey::bench::Utf8Path(quality_options.model)))) {
      throw std::invalid_argument("--decode-stats must differ from input and result paths");
    }
    if (!trace_output_path.empty() && !quality_options.trace) {
      throw std::invalid_argument("--trace-output requires --trace");
    }
    if (quality_options.trace && trace_output_path.empty()) {
      trace_output_path = "trace.jsonl";
    }
    if (quality_options.trace &&
        (SamePath(trace_output_path, output_path) ||
         SamePath(trace_output_path, quality_options.per_case_path) ||
         SamePath(trace_output_path, quality_options.eval_path) ||
         SamePath(trace_output_path, baseline_path) ||
         SamePath(trace_output_path, azookey::bench::Utf8Path(quality_options.model)))) {
      throw std::invalid_argument("--trace-output must differ from input and result paths");
    }
    if (!quality_options.eval_path.empty() && max_p95_ms) {
      throw std::invalid_argument("--max-p95-ms cannot be combined with --eval");
    }
    if (!quality_options.eval_path.empty() && (json_output || ipc_benchmark)) {
      throw std::invalid_argument("--json and --ipc cannot be combined with --eval");
    }
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << std::endl;
    PrintUsage(argv[0]);
    return 2;
  }

  // Spec section 15.2: the store and the settings at the Host's default locations unless given.
  // Only resolved here: nothing creates, migrates or locks them.
  azookey::bench::PersonalEvaluationInput personal_input;
  if (personal_eval) {
    const bool explicit_settings = !settings_path.empty();
    if (learning_path.empty() || settings_path.empty()) {
      azookey::host::UserDataPathInputs inputs;
      inputs.local_app_data = azookey::host::GetPlatformLocalAppData();
      const auto defaults = azookey::host::ResolveUserDataPaths(inputs);
      if (!defaults) {
        std::cerr << "cannot resolve the default user data location; pass --learning and --settings"
                  << std::endl;
        return 2;
      }
      if (learning_path.empty()) learning_path = defaults->learning_path;
      if (settings_path.empty()) settings_path = defaults->settings_path;
    }
    // weakly_canonical can throw; an unusable path is an input error like the others.
    try {
      const auto v2_path = azookey::learning::LearningStoreV2PathFor(learning_path);
      for (const auto& written : {output_path, quality_options.per_case_path}) {
        for (const auto& input :
             {learning_path, azookey::learning::EncryptedPathFor(learning_path), v2_path,
              azookey::learning::EncryptedPathFor(v2_path), settings_path}) {
          if (SamePath(written, input)) {
            throw std::invalid_argument(
                "--output and --per-case must differ from the learning store and the settings");
          }
        }
      }
      if (SamePath(output_path, quality_options.per_case_path)) {
        throw std::invalid_argument("--output and --per-case must differ");
      }
    } catch (const std::exception& ex) {
      std::cerr << ex.what() << std::endl;
      return 2;
    }
    std::string error;
    const auto privacy =
        azookey::bench::LoadPersonalPrivacy(settings_path, explicit_settings, &error);
    if (!privacy) {
      std::cerr << error << std::endl;
      return 2;
    }
    // Read-only and never handed to the engine, so it is neither migrated nor saved. A path the
    // stream cannot read (libstdc++ throws for a directory) is the same input error as a failed
    // load, with a fixed message.
    try {
      azookey::learning::LearningStore personal_store(learning_path);
      size_t malformed_rows = 0;
      if (!personal_store.LoadReadOnly(&malformed_rows)) {
        throw std::runtime_error("load failed");
      }
      personal_input = azookey::bench::BuildPersonalCases(
          personal_store.All(), malformed_rows, *privacy,
          max_cases.value_or(azookey::bench::kDefaultPersonalMaxCases));
    } catch (const std::exception&) {
      std::cerr << "cannot read the learning store; evaluation does not run" << std::endl;
      return 2;
    }
  }

  std::optional<azookey::bench::TemporaryLearningFile> learning_file;
  try {
    learning_file.emplace();
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << std::endl;
    return 2;
  }

  std::optional<azookey::logging::RuntimeLogger> trace_logger;
  if (quality_options.trace) {
    try {
      const auto directory = trace_output_path.parent_path();
      if (!directory.empty()) std::filesystem::create_directories(directory);
      std::ofstream trace_output(trace_output_path, std::ios::binary | std::ios::trunc);
      if (!trace_output) throw std::runtime_error("cannot create trace output");
      trace_logger.emplace(TraceLoggerOptions(trace_output_path, "bench"));
    } catch (const std::exception& ex) {
      std::cerr << ex.what() << std::endl;
      return 2;
    }
  }

  // Keep store and engine after learning_file: engine shutdown flushes learning,
  // and the atomic writer would recreate the directory if cleanup ran first.
  azookey::learning::LearningStore store(learning_file->Path());
  // Spec section 7: a mode other than off corrects with an empty typo store beside the temporary
  // learning file (learning_state "empty"). Nothing records a typo, so it writes no file.
  std::optional<azookey::learning::TypoLearningStore> typo_store;
  if (quality_options.typo_mode != "off") {
    typo_store.emplace(learning_file->Path().parent_path() / "typo_corrections.tsv");
    const auto now = static_cast<uint64_t>(
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
    if (typo_store->Load(now).mode != azookey::learning::TypoLearningMode::V2) {
      std::cerr << "cannot start an empty typo store" << std::endl;
      return 2;
    }
  }
  azookey::host::EngineConfig engine_config;
  // The engine's own default is suggest; the bench converts with the mode it records.
  engine_config.typo_correction_mode = quality_options.typo_mode;
  engine_config.backend = quality_options.backend == "cuda" ? azookey::host::BackendKind::Cuda
                                                            : azookey::host::BackendKind::Cpu;
  engine_config.model_path = quality_options.model;
  if (nll_enabled) engine_config.nll.enabled = *nll_enabled;
  // lattice-conversion-spec section 10.1: A and B differ only in the lattice's internal switch.
  engine_config.lattice.enabled = *lattice_enabled;
  if (lattice_with_model) {
    engine_config.lattice.with_model = true;
    engine_config.lattice.with_model_paths = *lattice_with_model;
    engine_config.lattice.with_model_band = lattice_band.value_or("best") == "lowest"
                                                ? azookey::host::LatticeModelBand::BelowModelLowest
                                                : azookey::host::LatticeModelBand::BelowModelBest;
    quality_options.lattice_with_model =
        "k=" + std::to_string(*lattice_with_model) + ";band=" + lattice_band.value_or("best");
  }
  azookey::host::InferenceEngine engine(std::make_unique<azookey::core::SimpleConverter>(), &store,
                                        engine_config, trace_logger ? &*trace_logger : nullptr);
  if (typo_store) engine.SetTypoStore(&*typo_store);
  if (!quality_options.model.empty() && !engine.LoadModel()) {
    std::cerr << "failed to load model" << std::endl;
    return 2;
  }
  if (!dict_dir.empty()) {
    try {
      // Hash first: it also rejects a directory without any .azdic file.
      quality_options.dictionary_bundle_sha256 = azookey::bench::DictionaryBundleSha256(dict_dir);
    } catch (const std::exception& ex) {
      std::cerr << ex.what() << std::endl;
      return 2;
    }
    bool any_loaded = false;
    for (const auto& layer : engine.LoadBundledDictionaryLayers(dict_dir)) {
      if (layer.name == "lattice_core.azdic") {
        // Section 10.1: B without lattice_core would measure A's candidates as B.
        if (!layer.loaded) {
          std::cerr << "--lattice on could not load lattice_core.azdic: " << layer.error
                    << std::endl;
          return 2;
        }
      } else if (layer.loaded) {
        any_loaded = true;
      } else if (std::filesystem::exists(dict_dir / layer.name)) {
        // A present but unreadable layer would silently change the measured configuration.
        std::cerr << "dictionary layer " << layer.name << " failed to load: " << layer.error
                  << std::endl;
        return 2;
      }
    }
    if (!any_loaded) {
      std::cerr << "--dict-dir holds no bundled dictionary layer" << std::endl;
      return 2;
    }
  }
  quality_options.lattice = *lattice_enabled ? "on" : "off";
  // Recorded from the engine, so the compatibility key reflects the setting it ran with.
  quality_options.nll = engine.config().nll.enabled ? "on" : "off";
  if (profile) {
    // Spec section 15.5: only a format with [PROF] can carry the profile.
    const auto format = engine.health_snapshot().prompt_format;
    if (!format || !azookey::host::ZenzaiPromptFormatUsesInstructionProfile(*format)) {
      std::cerr << "--profile needs a Zenzai model whose prompt format has [PROF]" << std::endl;
      return 2;
    }
    engine.SetBenchInstructionProfile(*profile);
    quality_options.profile_sha256 = azookey::learning::Sha256Hex(*profile);
    if (!personal_eval) std::cerr << azookey::bench::kUnsaltedHashWarning << std::endl;
  }

  if (!quality_options.eval_path.empty() || personal_eval) {
    quality_options.output_path = output_path;
    quality_options.baseline_path = baseline_path;
    quality_options.build_id = AZOOKEY_BENCH_COMMIT;
    if (!quality_options.model.empty()) {
      quality_options.decode = "beam";
      quality_options.beam_width = kCurrentZenzaiBeamWidth;
      quality_options.max_new_tokens = kCurrentZenzaiMaxNewTokens;
      quality_options.prompt_template_version = kCurrentZenzaiPromptTemplateVersion;
      // llama.cpp's effective thread count is not exposed by the converter yet (DEV-858).
      // Zero records that the compatibility value is unknown instead of reporting a false default.
      quality_options.thread_count = 0;
      quality_options.batch_size = kCurrentZenzaiContextBatchSize;
    }
    std::ofstream decode_stats;
    if (!decode_stats_path.empty()) {
      const auto directory = decode_stats_path.parent_path();
      std::error_code ec;
      if (!directory.empty()) std::filesystem::create_directories(directory, ec);
      decode_stats.open(decode_stats_path, std::ios::binary | std::ios::trunc);
      if (!decode_stats) {
        std::cerr << "cannot create decode stats output" << std::endl;
        return 2;
      }
    }
    std::string error;
    uint64_t request_id = 0;
    uint64_t decode_stats_call = 0;
    std::optional<FirstClauseProbe> probe;
    if (first_clause_probe) {
      try {
        probe.emplace(dict_dir);
      } catch (const std::exception& ex) {
        std::cerr << ex.what() << std::endl;
        return 2;
      }
    }
    // DEV-2110: per query of a reading the probe splits, the reading's latency, the first
    // segment's, and whether the first segment's request hit the Zenzai deadline.
    size_t probe_queries = 0;
    std::vector<double> probe_whole_ms;
    std::vector<double> probe_first_ms;
    std::vector<double> probe_sum_ms;
    size_t probe_first_deadline = 0;
    const azookey::bench::CandidateQuery query =
        [&engine, &request_id, &trace_logger, &quality_options, &decode_stats, &decode_stats_call,
         &probe, &probe_queries, &probe_whole_ms, &probe_first_ms, &probe_sum_ms,
         &probe_first_deadline](const std::string& input, const std::string& context,
                                const std::string& raw_keys) {
          const auto now = static_cast<uint64_t>(
              std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
          const auto start = std::chrono::steady_clock::now();
          auto result = QueryCandidates(
              engine, input, context, now,
              static_cast<uint32_t>(azookey::bench::kConversionQualityCandidateLimit),
              trace_logger ? &*trace_logger : nullptr, request_id, quality_options.backend,
              raw_keys);
          const auto latency_ms =
              std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                  .count();
          std::optional<azookey::ipc::json::Object> stats_line;
          if (decode_stats.is_open())
            stats_line = DecodeStatsLine(++decode_stats_call, input, context, latency_ms, engine,
                                         result.candidates);
          if (probe) {
            ++probe_queries;
            if (const auto end = probe->FirstSegmentEnd(input)) {
              const auto split = CodepointOffset(input, *end);
              // segment-editing-spec section 5.1: the first segment's request, as the TIP sends it.
              azookey::host::MixedConversionOptions options;
              options.typo_correction = false;
              options.remaining_reading = input.substr(split);
              const auto first_start = std::chrono::steady_clock::now();
              const auto first = engine.QueryCandidatesEx(
                  input.substr(0, split), context, now, nullptr,
                  static_cast<uint32_t>(azookey::bench::kConversionQualityCandidateLimit), false,
                  nullptr, nullptr, &options);
              FirstClauseSample sample;
              sample.end = *end;
              sample.latency_ms = std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - first_start)
                                      .count();
              const auto first_stats = engine.last_zenzai_decode_stats();
              sample.deadline_exceeded = first_stats && first_stats->deadline_exceeded;
              if (!first.candidates.empty()) sample.top1 = first.candidates.front().surface;
              probe_whole_ms.push_back(latency_ms);
              probe_first_ms.push_back(sample.latency_ms);
              probe_sum_ms.push_back(latency_ms + sample.latency_ms);
              probe_first_deadline += sample.deadline_exceeded;
              if (stats_line) stats_line->emplace("first_clause", FirstClauseValue(sample));
            }
          }
          if (stats_line) {
            decode_stats << azookey::ipc::json::Stringify(
                                azookey::ipc::json::Value(std::move(*stats_line)))
                         << '\n';
            if (!decode_stats) throw std::runtime_error("cannot write decode stats output");
          }
          azookey::bench::CandidateQueryResult output(std::move(result.candidates));
          output.corrected_reading = std::move(result.corrected_reading);
          return output;
        };
    const bool ok = personal_eval ? azookey::bench::RunPersonalEvaluation(
                                        quality_options, personal_input, query, std::cerr, &error)
                                  : azookey::bench::RunConversionQualityEvaluation(quality_options,
                                                                                   query, &error);
    if (!ok) {
      std::cerr << error << std::endl;
      return 2;
    }
    if (probe) {
      // The 750 ms is the TIP's wait for the candidate window (legacy-parity-spec section 1.2).
      const auto over = static_cast<size_t>(std::count_if(probe_sum_ms.begin(), probe_sum_ms.end(),
                                                          [](double ms) { return ms > 750.0; }));
      const auto quantile = [](const std::vector<double>& values, double fraction) {
        return azookey::bench::LatencyPercentile(values, fraction);
      };
      std::cout << "first_clause_probe queries=" << probe_queries
                << " split=" << probe_sum_ms.size()
                << " whole_p50_ms=" << quantile(probe_whole_ms, 0.5)
                << " whole_p95_ms=" << quantile(probe_whole_ms, 0.95)
                << " first_p50_ms=" << quantile(probe_first_ms, 0.5)
                << " first_p95_ms=" << quantile(probe_first_ms, 0.95)
                << " sum_p50_ms=" << quantile(probe_sum_ms, 0.5)
                << " sum_p95_ms=" << quantile(probe_sum_ms, 0.95) << " sum_over_750ms=" << over
                << " first_deadline_exceeded=" << probe_first_deadline << std::endl;
    }
    if (decode_stats.is_open()) {
      decode_stats.flush();
      decode_stats.close();
      if (!decode_stats) {
        std::cerr << "cannot write decode stats output" << std::endl;
        return 2;
      }
    }
    return 0;
  }

  if (quality_options.model.empty()) engine.LoadModel();

  const std::vector<std::string> inputs = {"わたし", "にほん", "とうきょう", "かなへんかん",
                                           "にほん"};
  std::vector<double> lat_ms;
  uint64_t request_id = 0;
  for (int i = 0; i < 200; ++i) {
    const auto& kana = inputs[static_cast<size_t>(i) % inputs.size()];
    auto t0 = std::chrono::steady_clock::now();
    auto now = static_cast<uint64_t>(
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
    (void)QueryCandidates(engine, kana, "", now, 0, trace_logger ? &*trace_logger : nullptr,
                          request_id, quality_options.backend);
    auto t1 = std::chrono::steady_clock::now();
    lat_ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
  }

  std::sort(lat_ms.begin(), lat_ms.end());
  auto pct = [&](double p) {
    const size_t idx = static_cast<size_t>((p / 100.0) * static_cast<double>(lat_ms.size() - 1));
    return lat_ms[idx];
  };

  const double p50 = pct(50);
  const double p95 = pct(95);
  const double p99 = pct(99);

  azookey::bench::BenchmarkResult result;
  result.bench = "azookey_bench";
  result.commit = AZOOKEY_BENCH_COMMIT;
  result.config = AZOOKEY_BENCH_CONFIG;
  result.iterations = lat_ms.size();
  result.latency = {p50, p95, p99, lat_ms.back()};
  result.max_p95_ms = max_p95_ms;
  result.threshold_passed = !max_p95_ms || p95 < *max_p95_ms;
  result.baseline =
      azookey::bench::CompareBaseline(baseline_path, result.bench, result.config, result.latency);
  if (ipc_benchmark) {
    try {
      result.ipc_phases = azookey::bench::RunIpcBenchmark();
    } catch (const std::exception& ex) {
      std::cerr << ex.what() << std::endl;
      return 2;
    }
  }
  const auto json = azookey::bench::SerializeBenchmarkResult(result);

  if (!output_path.empty()) {
    std::string error;
    if (!azookey::bench::WriteBenchmarkResult(output_path, json, &error)) {
      std::cerr << error << std::endl;
      return 2;
    }
  }

  if (json_output) {
    std::cout << json << std::endl;
  } else {
    std::cout << "p50_ms=" << p50 << " p95_ms=" << p95 << " p99_ms=" << p99;
    if (max_p95_ms) {
      std::cout << " max_p95_ms=" << *max_p95_ms;
    } else {
      std::cout << " max_p95_ms=none";
    }
    std::cout << std::endl;
    if (result.ipc_phases) {
      const auto print_phase = [](const char* name, const azookey::bench::LatencyMetrics& m) {
        std::cout << name << " p50_ms=" << m.p50_ms << " p95_ms=" << m.p95_ms
                  << " p99_ms=" << m.p99_ms << " max_ms=" << m.max_ms << '\n';
      };
      print_phase("ipc_serialize", result.ipc_phases->serialize);
      print_phase("ipc_framing", result.ipc_phases->framing);
      print_phase("ipc_deserialize", result.ipc_phases->deserialize);
      if (result.ipc_phases->pipe_round_trip) {
        print_phase("ipc_pipe_round_trip", *result.ipc_phases->pipe_round_trip);
      } else {
        std::cout << "ipc_pipe_round_trip=unavailable (Windows only)\n";
      }
      if (result.ipc_phases->send && result.ipc_phases->host_compute &&
          result.ipc_phases->receive) {
        print_phase("ipc_send", *result.ipc_phases->send);
        print_phase("ipc_host_compute", *result.ipc_phases->host_compute);
        print_phase("ipc_receive", *result.ipc_phases->receive);
      } else {
        std::cout << "ipc_send/ipc_host_compute/ipc_receive=unavailable (Windows only)\n";
      }
    }
  }
  if (const auto warning = azookey::bench::RegressionWarning(result)) {
    std::cerr << *warning << std::endl;
  }

  if (!result.threshold_passed) {
    std::cerr << "p95 exceeded threshold" << std::endl;
    return 1;
  }
  return 0;
}
