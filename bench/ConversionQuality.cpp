#include "ConversionQuality.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <bcrypt.h>
#include <psapi.h>
#endif

#include "azookey/ipc/Json.h"

namespace azookey::bench {
namespace {

namespace j = azookey::ipc::json;

struct EvaluationCase {
  std::string id;
  std::string input;
  std::string expected;
  std::vector<std::string> acceptable;
  std::vector<std::string> categories;
  std::string left_context;
  std::string observed_reading;
  std::string intended_reading;
  // Spec section 4.3: the keys typed for observed_reading, empty when the case has none.
  std::string raw_keys;
  std::optional<std::string> typo_type;
  bool typo{false};
};

struct EvaluatedCase {
  EvaluationCase source;
  std::vector<core::Candidate> candidates;
  CandidateMatchMetrics match;
  size_t rank{0};
  bool reading_fidelity{false};
  bool typo_corrected{false};
  double latency_ms{0.0};
};

struct Aggregate {
  size_t count{0};
  size_t top1{0};
  size_t top3{0};
  size_t top5{0};
  double reciprocal_rank_sum{0.0};
  size_t exact{0};
  size_t nfkc_exact{0};
  size_t acceptable{0};
  size_t edit_distance{0};
  size_t reference_length{0};
  size_t nfkc_edit_distance{0};
  size_t nfkc_reference_length{0};
  size_t reading_fidelity{0};
  size_t reading_count{0};
  size_t typo_count{0};
  size_t typo_top1{0};
  size_t typo_top5{0};
  size_t clean_count{0};
  size_t clean_false_positive{0};
  size_t clean_overcorrection{0};
  std::vector<double> latencies;
};

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) throw std::runtime_error("failed to open file: " + path.string());
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

void WriteFile(const std::filesystem::path& path, const std::string& content) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output.is_open()) throw std::runtime_error("failed to open output: " + path.string());
  output.write(content.data(), static_cast<std::streamsize>(content.size()));
  if (!output) throw std::runtime_error("failed to write output: " + path.string());
}

const j::Value& Required(const j::Object& object, std::string_view key) {
  const auto found = object.find(std::string(key));
  if (found == object.end())
    throw std::runtime_error("missing required field: " + std::string(key));
  return found->second;
}

std::string RequiredString(const j::Object& object, std::string_view key) {
  const auto& value = Required(object, key);
  if (!value.IsString() || value.AsString().empty()) {
    throw std::runtime_error("field must be a non-empty string: " + std::string(key));
  }
  return value.AsString();
}

std::string OptionalString(const j::Object& object, std::string_view key) {
  const auto found = object.find(std::string(key));
  if (found == object.end()) return {};
  if (!found->second.IsString()) {
    throw std::runtime_error("field must be a string: " + std::string(key));
  }
  return found->second.AsString();
}

std::vector<std::string> StringArray(const j::Object& object, std::string_view key, bool required) {
  const auto found = object.find(std::string(key));
  if (found == object.end()) {
    if (required) throw std::runtime_error("missing required field: " + std::string(key));
    return {};
  }
  if (!found->second.IsArray()) {
    throw std::runtime_error("field must be an array: " + std::string(key));
  }
  std::vector<std::string> values;
  for (const auto& value : found->second.AsArray()) {
    if (!value.IsString() || value.AsString().empty()) {
      throw std::runtime_error("array must contain non-empty strings: " + std::string(key));
    }
    values.push_back(value.AsString());
  }
  if (required && values.empty()) {
    throw std::runtime_error("array must not be empty: " + std::string(key));
  }
  return values;
}

EvaluationCase ParseCase(const std::string& line, size_t line_number) {
  auto parsed = j::Parse(line);
  if (!parsed || !parsed->IsObject()) {
    throw std::runtime_error("invalid JSON object at line " + std::to_string(line_number));
  }
  const auto& object = parsed->AsObject();
  EvaluationCase result;
  result.id = RequiredString(object, "id");
  result.categories = StringArray(object, "category", true);
  result.left_context = OptionalString(object, "left_context");
  (void)RequiredString(object, "provenance");

  const auto input = object.find("input");
  if (input != object.end()) {
    if (!input->second.IsString() || input->second.AsString().empty()) {
      throw std::runtime_error("field must be a non-empty string: input");
    }
    result.input = input->second.AsString();
    result.expected = RequiredString(object, "expected");
    result.acceptable = StringArray(object, "acceptable", false);
    return result;
  }

  result.typo = true;
  result.observed_reading = RequiredString(object, "observed_reading");
  result.intended_reading = RequiredString(object, "intended_reading");
  result.raw_keys = OptionalString(object, "raw_keys");
  result.expected = RequiredString(object, "expected_surface");
  const auto typo_type = object.find("typo_type");
  if (typo_type == object.end()) throw std::runtime_error("missing required field: typo_type");
  if (typo_type->second.IsString()) {
    result.typo_type = typo_type->second.AsString();
  } else if (!typo_type->second.IsNull()) {
    throw std::runtime_error("field must be a string or null: typo_type");
  }
  return result;
}

std::vector<EvaluationCase> LoadCases(const std::filesystem::path& path,
                                      const std::string& category) {
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open())
    throw std::runtime_error("failed to open evaluation data: " + path.string());
  std::vector<EvaluationCase> cases;
  std::string line;
  size_t line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    auto parsed = ParseCase(line, line_number);
    if (category == "all" || std::find(parsed.categories.begin(), parsed.categories.end(),
                                       category) != parsed.categories.end()) {
      cases.push_back(std::move(parsed));
    }
  }
  if (cases.empty()) throw std::runtime_error("evaluation data contains no selected cases");
  return cases;
}

double Ratio(size_t numerator, size_t denominator) {
  return denominator == 0 ? 0.0 : static_cast<double>(numerator) / denominator;
}

double Percentile(std::vector<double> values, double fraction) {
  if (values.empty()) return 0.0;
  std::sort(values.begin(), values.end());
  const size_t index = static_cast<size_t>(fraction * static_cast<double>(values.size() - 1));
  return values[index];
}

void Add(Aggregate* aggregate, const EvaluatedCase& result, bool include_primary_metrics,
         std::string_view typo_mode) {
  if (include_primary_metrics) {
    ++aggregate->count;
    if (result.rank == 1) ++aggregate->top1;
    if (result.rank > 0 && result.rank <= 3) ++aggregate->top3;
    if (result.rank > 0 && result.rank <= kConversionQualityCandidateLimit) ++aggregate->top5;
    if (result.rank > 0) aggregate->reciprocal_rank_sum += 1.0 / result.rank;
    aggregate->exact += result.match.exact_match ? 1 : 0;
    aggregate->nfkc_exact += result.match.nfkc_exact_match ? 1 : 0;
    aggregate->acceptable += result.match.acceptable_match ? 1 : 0;
    aggregate->edit_distance += result.match.edit_distance;
    aggregate->reference_length += result.match.reference_length;
    aggregate->nfkc_edit_distance += result.match.nfkc_edit_distance;
    aggregate->nfkc_reference_length += result.match.nfkc_reference_length;
    aggregate->latencies.push_back(result.latency_ms);
  }
  if (!result.source.typo) {
    if (include_primary_metrics) {
      ++aggregate->reading_count;
      aggregate->reading_fidelity += result.reading_fidelity ? 1 : 0;
    }
  } else if (result.source.typo_type) {
    ++aggregate->typo_count;
    aggregate->typo_top1 += result.rank == 1 ? 1 : 0;
    aggregate->typo_top5 +=
        result.rank > 0 && result.rank <= kConversionQualityCandidateLimit ? 1 : 0;
  } else {
    ++aggregate->clean_count;
    aggregate->clean_false_positive += result.typo_corrected ? 1 : 0;
    aggregate->clean_overcorrection += typo_mode == "aggressive" && result.rank == 0 ? 1 : 0;
  }
}

j::Object SerializeMetrics(const Aggregate& aggregate, bool include_diagnostics) {
  j::Object output;
  output.emplace(
      "MRR",
      j::Value(aggregate.count == 0 ? 0.0 : aggregate.reciprocal_rank_sum / aggregate.count));
  output.emplace("acceptable_match_rate", j::Value(Ratio(aggregate.acceptable, aggregate.count)));
  output.emplace("cer", j::Value(Ratio(aggregate.edit_distance, aggregate.reference_length)));
  output.emplace("exact_match_rate", j::Value(Ratio(aggregate.exact, aggregate.count)));
  output.emplace("latency_p95_ms", j::Value(Percentile(aggregate.latencies, 0.95)));
  output.emplace("latency_p99_ms", j::Value(Percentile(aggregate.latencies, 0.99)));
  output.emplace("nfkc_cer",
                 j::Value(Ratio(aggregate.nfkc_edit_distance, aggregate.nfkc_reference_length)));
  output.emplace("nfkc_exact_match_rate", j::Value(Ratio(aggregate.nfkc_exact, aggregate.count)));
  output.emplace("reading_fidelity_rate",
                 j::Value(Ratio(aggregate.reading_fidelity, aggregate.reading_count)));
  output.emplace("top1_accuracy", j::Value(Ratio(aggregate.top1, aggregate.count)));
  output.emplace("top3_accuracy", j::Value(Ratio(aggregate.top3, aggregate.count)));
  output.emplace("top5_accuracy", j::Value(Ratio(aggregate.top5, aggregate.count)));
  if (include_diagnostics) {
    output.emplace("typo_correction_top1_accuracy",
                   j::Value(Ratio(aggregate.typo_top1, aggregate.typo_count)));
    output.emplace("typo_correction_top5_accuracy",
                   j::Value(Ratio(aggregate.typo_top5, aggregate.typo_count)));
    output.emplace("typo_false_positive_rate",
                   j::Value(Ratio(aggregate.clean_false_positive, aggregate.clean_count)));
    output.emplace("typo_overcorrection_rate",
                   j::Value(Ratio(aggregate.clean_overcorrection, aggregate.clean_count)));
  }
  return output;
}

j::Array CandidateSurfaces(const std::vector<core::Candidate>& candidates) {
  j::Array values;
  values.reserve(candidates.size());
  for (const auto& candidate : candidates) values.emplace_back(candidate.surface);
  return values;
}

j::Object SerializePerCase(const EvaluatedCase& result) {
  j::Object output;
  output.emplace("candidates", j::Value(CandidateSurfaces(result.candidates)));
  output.emplace("category", j::Value(result.source.categories.front()));
  output.emplace("cer", j::Value(result.match.cer));
  output.emplace("exact_match", j::Value(result.match.exact_match));
  output.emplace("expected", j::Value(result.source.expected));
  output.emplace("id", j::Value(result.source.id));
  output.emplace("latency_ms", j::Value(result.latency_ms));
  output.emplace("nfkc_cer", j::Value(result.match.nfkc_cer));
  output.emplace("nfkc_exact_match", j::Value(result.match.nfkc_exact_match));
  output.emplace("rank", j::Value(static_cast<uint64_t>(result.rank)));
  output.emplace("top1",
                 j::Value(result.candidates.empty() ? "" : result.candidates.front().surface));
  if (result.source.typo) {
    output.emplace("intended_reading", j::Value(result.source.intended_reading));
    output.emplace("observed_reading", j::Value(result.source.observed_reading));
    output.emplace("typo_corrected", j::Value(result.typo_corrected));
    output.emplace("typo_type", result.source.typo_type ? j::Value(*result.source.typo_type)
                                                        : j::Value(j::Null{}));
  } else {
    output.emplace("acceptable_match", j::Value(result.match.acceptable_match));
    output.emplace("input", j::Value(result.source.input));
    output.emplace("reading_fidelity", j::Value(result.reading_fidelity));
  }
  return output;
}

std::string Iso8601Now() {
  const auto now = std::chrono::system_clock::now();
  const auto value = std::chrono::system_clock::to_time_t(now);
  std::tm utc{};
#if defined(_WIN32)
  gmtime_s(&utc, &value);
#else
  gmtime_r(&value, &utc);
#endif
  std::ostringstream output;
  output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
  return output.str();
}

std::string Hex(const std::vector<unsigned char>& bytes) {
  constexpr char digits[] = "0123456789abcdef";
  std::string output;
  output.reserve(bytes.size() * 2);
  for (const auto byte : bytes) {
    output.push_back(digits[byte >> 4]);
    output.push_back(digits[byte & 0x0f]);
  }
  return output;
}

// SHA-256 of the concatenation of each part's prefix followed by its file bytes.
std::string Sha256Files(const std::vector<std::pair<std::string, std::filesystem::path>>& parts,
                        bool normalize_crlf) {
#if defined(_WIN32)
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  DWORD object_size = 0;
  DWORD result_size = 0;
  DWORD hash_size = 0;
  std::vector<unsigned char> object;
  std::vector<unsigned char> digest;
  auto cleanup = [&]() {
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
  };
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
      BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size),
                        sizeof(object_size), &result_size, 0) < 0 ||
      BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hash_size),
                        sizeof(hash_size), &result_size, 0) < 0) {
    cleanup();
    throw std::runtime_error("failed to initialize SHA-256");
  }
  object.resize(object_size);
  digest.resize(hash_size);
  if (BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0) < 0) {
    cleanup();
    throw std::runtime_error("failed to calculate SHA-256");
  }
  const auto update = [&](const unsigned char* bytes, size_t size) {
    if (size == 0) return;
    if (size > std::numeric_limits<ULONG>::max() ||
        BCryptHashData(hash, const_cast<PUCHAR>(bytes), static_cast<ULONG>(size), 0) < 0) {
      cleanup();
      throw std::runtime_error("failed to calculate SHA-256");
    }
  };
  std::array<unsigned char, 64 * 1024> buffer{};
  for (const auto& [prefix, path] : parts) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
      cleanup();
      throw std::runtime_error("failed to open file for SHA-256: " + path.string());
    }
    update(reinterpret_cast<const unsigned char*>(prefix.data()), prefix.size());
    bool pending_cr = false;
    while (input) {
      input.read(reinterpret_cast<char*>(buffer.data()),
                 static_cast<std::streamsize>(buffer.size()));
      const auto count = input.gcount();
      if (count <= 0) continue;
      if (!normalize_crlf) {
        update(buffer.data(), static_cast<size_t>(count));
        continue;
      }
      std::vector<unsigned char> normalized;
      normalized.reserve(static_cast<size_t>(count) + 1);
      for (std::streamsize index = 0; index < count; ++index) {
        const auto byte = buffer[static_cast<size_t>(index)];
        if (pending_cr) {
          if (byte == '\n') {
            normalized.push_back('\n');
            pending_cr = false;
            continue;
          }
          normalized.push_back('\r');
          pending_cr = false;
        }
        if (byte == '\r') {
          pending_cr = true;
        } else {
          normalized.push_back(byte);
        }
      }
      update(normalized.data(), normalized.size());
    }
    if (pending_cr) {
      constexpr unsigned char carriage_return = '\r';
      update(&carriage_return, 1);
    }
    if (!input.eof()) {
      cleanup();
      throw std::runtime_error("failed to calculate SHA-256");
    }
  }
  if (BCryptFinishHash(hash, digest.data(), hash_size, 0) < 0) {
    cleanup();
    throw std::runtime_error("failed to calculate SHA-256");
  }
  cleanup();
  return Hex(digest);
#else
  (void)parts;
  (void)normalize_crlf;
  throw std::runtime_error("conversion-quality SHA-256 is only available on Windows");
#endif
}

std::string Sha256File(const std::filesystem::path& path, std::string prefix, bool normalize_crlf) {
  return Sha256Files({{std::move(prefix), path}}, normalize_crlf);
}

std::string Sha256File(const std::filesystem::path& path) { return Sha256File(path, {}, false); }

double PeakMemoryMb() {
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS counters{};
  counters.cb = sizeof(counters);
  if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) return 0.0;
  return static_cast<double>(counters.PeakWorkingSetSize) / (1024.0 * 1024.0);
#else
  return 0.0;
#endif
}

j::Value ReadBaseline(const std::filesystem::path& path) {
  auto parsed = j::Parse(ReadFile(path));
  if (!parsed || !parsed->IsObject()) throw std::runtime_error("invalid baseline JSON");
  return std::move(*parsed);
}

// The compatibility keys (spec section 14.1) on which `baseline` differs from `config`, in the
// order below. "version" and "config" stand for a baseline without a usable value for them.
std::vector<std::string> IncompatibleBaselineKeys(const j::Value& baseline,
                                                  const j::Object& config) {
  std::vector<std::string> incompatible;
  const auto* baseline_version = baseline.Find("version");
  if (!baseline_version || !baseline_version->IsNumber() || baseline_version->AsNumber() != 1.0) {
    incompatible.emplace_back("version");
  }
  const auto* baseline_config = baseline.FindObject("config");
  if (!baseline_config) {
    incompatible.emplace_back("config");
    return incompatible;
  }
  const auto compatible_value = [](const j::Value& current, const j::Value& baseline) {
    if (current.IsNull() && baseline.IsNull()) return true;
    if (current.IsBool() && baseline.IsBool()) return current.AsBool() == baseline.AsBool();
    if (current.IsNumber() && baseline.IsNumber()) {
      return current.AsNumber() == baseline.AsNumber();
    }
    if (current.IsString() && baseline.IsString()) {
      return current.AsString() == baseline.AsString();
    }
    return false;
  };
  constexpr std::array compatible_keys = {
      "model_sha256",
      "backend",
      "decode",
      "beam_width",
      "n_best",
      "max_new_tokens",
      "prompt_template_version",
      "thread_count",
      "batch_size",
      "typo_correction_mode",
      "learning_state",
      "dictionary_bundle_sha256",
      "lattice",
      "lattice_with_model",
      "nll",
      "nll_breaker",
      "eval_source",
      "profile_sha256",
      "category_filter",
      "eval_dataset_sha256",
  };
  // Keys added after results were first kept: a result without one took the value below
  // (spec section 14.1). Every other key must be present on both sides.
  const auto value_or_default = [](const j::Object& object,
                                   std::string_view key) -> std::optional<j::Value> {
    if (const auto found = object.find(std::string(key)); found != object.end()) {
      return found->second;
    }
    if (key == "nll") return j::Value("off");
    if (key == "lattice_with_model") return j::Value("off");
    if (key == "nll_breaker") return j::Value("product");
    if (key == "eval_source") return j::Value("bundled");
    if (key == "profile_sha256") return j::Value("none");
    return std::nullopt;
  };
  for (const auto* key : compatible_keys) {
    const auto current = value_or_default(config, key);
    const auto previous = value_or_default(*baseline_config, key);
    if (!current || !previous || !compatible_value(*current, *previous)) {
      incompatible.emplace_back(key);
    }
  }
  return incompatible;
}

j::Object BaselineMetricsDiff(const j::Value& baseline, const j::Object& summary,
                              const j::Object& categories) {
  j::Object diff;
  const auto* baseline_summary = baseline.FindObject("summary");
  if (!baseline_summary) throw std::runtime_error("baseline is missing summary");
  const auto add_diff = [&](const std::string& key, const j::Object& current,
                            const j::Object& baseline, const std::string& prefix,
                            j::Object* output) {
    const auto current_value = current.find(key);
    const auto baseline_value = baseline.find(key);
    if (current_value != current.end() && baseline_value != baseline.end() &&
        current_value->second.IsNumber() && baseline_value->second.IsNumber()) {
      output->emplace(prefix + key, j::Value(current_value->second.AsNumber() -
                                             baseline_value->second.AsNumber()));
    }
  };
  constexpr std::array keys = {"top1_accuracy",         "top5_accuracy", "exact_match_rate",
                               "nfkc_exact_match_rate", "cer",           "nfkc_cer"};
  for (const auto* key : keys) add_diff(key, summary, *baseline_summary, "", &diff);
  const auto* baseline_categories = baseline.FindObject("by_category");
  if (!baseline_categories) return diff;
  for (const auto& [category, current] : categories) {
    const auto previous = baseline_categories->find(category);
    if (!current.IsObject() || previous == baseline_categories->end() ||
        !previous->second.IsObject()) {
      continue;
    }
    for (const auto* key : keys) {
      add_diff(key, current.AsObject(), previous->second.AsObject(), category + ".", &diff);
    }
  }
  return diff;
}

j::Object BaselineDiff(const std::filesystem::path& path, const j::Object& config,
                       const j::Object& summary, const j::Object& categories) {
  if (path.empty()) return {};
  const auto baseline = ReadBaseline(path);
  const auto incompatible = IncompatibleBaselineKeys(baseline, config);
  if (!incompatible.empty()) {
    throw std::runtime_error("baseline is incompatible: " + incompatible.front());
  }
  return BaselineMetricsDiff(baseline, summary, categories);
}

size_t AcceptedRank(const std::vector<core::Candidate>& candidates, const EvaluationCase& item) {
  for (size_t index = 0; index < candidates.size(); ++index) {
    if (candidates[index].surface == item.expected ||
        std::find(item.acceptable.begin(), item.acceptable.end(), candidates[index].surface) !=
            item.acceptable.end()) {
      return index + 1;
    }
  }
  return 0;
}

void ValidateRunOptions(const ConversionQualityOptions& options) {
  if (options.iterations == 0) throw std::invalid_argument("--iterations must be greater than 0");
  if (options.n_best == 0 || options.n_best > kConversionQualityCandidateLimit) {
    throw std::invalid_argument("n_best must be between 1 and 5");
  }
  // Spec section 7. The v1 auto_replace is not a bench mode: the result records aggressive.
  if (options.typo_mode != "off" && options.typo_mode != "suggest" && options.typo_mode != "rank" &&
      options.typo_mode != "aggressive") {
    throw std::invalid_argument("--typo-mode must be off, suggest, rank or aggressive");
  }
}

// Queries the case options.iterations times, keeping the last candidates and the median latency.
EvaluatedCase EvaluateCase(EvaluationCase item, const ConversionQualityOptions& options,
                           const CandidateQuery& query) {
  const std::string& reading = item.typo ? item.observed_reading : item.input;
  std::vector<double> latencies;
  CandidateQueryResult last;
  latencies.reserve(options.iterations);
  for (size_t iteration = 0; iteration < options.iterations; ++iteration) {
    const auto start = std::chrono::steady_clock::now();
    auto current = query(reading, item.left_context, item.raw_keys);
    const auto end = std::chrono::steady_clock::now();
    latencies.push_back(std::chrono::duration<double, std::milli>(end - start).count());
    if (iteration + 1 == options.iterations) last = std::move(current);
  }
  auto& candidates = last.candidates;
  if (candidates.size() > options.n_best) candidates.resize(options.n_best);
  std::sort(latencies.begin(), latencies.end());
  EvaluatedCase result;
  result.source = std::move(item);
  result.candidates = std::move(candidates);
  result.rank = AcceptedRank(result.candidates, result.source);
  const std::string top1 = result.candidates.empty() ? "" : result.candidates.front().surface;
  result.match = EvaluateCandidateMatch(top1, result.source.expected, result.source.acceptable);
  result.reading_fidelity =
      !result.candidates.empty() && result.candidates.front().reading == result.source.input;
  // Spec section 4.3.1: the first candidate is a correction, either one the correction placed
  // there or any candidate of a conversion that ran on a corrected reading.
  result.typo_corrected = !last.corrected_reading.empty() ||
                          (!result.candidates.empty() &&
                           result.candidates.front().debug_info == kTypoCorrectionDebugInfo);
  result.latency_ms = latencies[latencies.size() / 2];
  return result;
}

j::Object SerializeCategories(const std::map<std::string, Aggregate>& by_category) {
  j::Object categories_json;
  for (const auto& [name, aggregate] : by_category) {
    auto metrics = SerializeMetrics(aggregate, false);
    metrics.emplace("n", j::Value(static_cast<uint64_t>(aggregate.count)));
    categories_json.emplace(name, j::Value(std::move(metrics)));
  }
  return categories_json;
}

j::Object SerializeSubsets(const Aggregate& katakana) {
  auto katakana_json = SerializeMetrics(katakana, false);
  katakana_json.emplace("n", j::Value(static_cast<uint64_t>(katakana.count)));
  j::Object subsets_json;
  subsets_json.emplace("katakana", j::Value(std::move(katakana_json)));
  return subsets_json;
}

j::Object BuildConfig(const ConversionQualityOptions& options, std::string_view eval_source,
                      std::string eval_dataset_sha256) {
  j::Object config;
  config.emplace("backend", j::Value(options.backend));
  config.emplace("batch_size", j::Value(options.batch_size));
  config.emplace("beam_width", j::Value(options.beam_width));
  config.emplace("build_id", j::Value(options.build_id));
  config.emplace("category_filter", j::Value(options.category));
  config.emplace("decode", j::Value(options.decode));
  config.emplace("dictionary_bundle_sha256", j::Value(options.dictionary_bundle_sha256));
  config.emplace("eval_dataset_sha256", j::Value(std::move(eval_dataset_sha256)));
  config.emplace("eval_source", j::Value(std::string(eval_source)));
  config.emplace("host_version", j::Value(options.host_version));
  config.emplace("lattice", j::Value(options.lattice));
  config.emplace("lattice_with_model", j::Value(options.lattice_with_model));
  config.emplace("learning_state", j::Value(options.learning_state));
  config.emplace("max_new_tokens", j::Value(options.max_new_tokens));
  config.emplace("model", j::Value(options.model.empty()
                                       ? "fallback"
                                       : std::filesystem::path(options.model).filename().string()));
  config.emplace("model_sha256", j::Value(options.model.empty() ? "" : Sha256File(options.model)));
  config.emplace("n_best", j::Value(options.n_best));
  config.emplace("nll", j::Value(options.nll));
  config.emplace("nll_breaker", j::Value(options.nll_breaker));
  config.emplace("profile_sha256", j::Value(options.profile_sha256));
  config.emplace("prompt_template_version", j::Value(options.prompt_template_version));
  config.emplace("thread_count", j::Value(options.thread_count));
  config.emplace("typo_correction_mode", j::Value(options.typo_mode));
  return config;
}

std::string PersonalCaseId(size_t ordinal) {
  std::ostringstream id;
  id << "personal_" << std::setw(6) << std::setfill('0') << ordinal;
  return id.str();
}

}  // namespace

std::string EvaluationDatasetSha256(const std::filesystem::path& path) {
  const auto absolute = std::filesystem::absolute(path).lexically_normal();
  auto data_root = absolute.parent_path();
  while (!data_root.empty() &&
         !(data_root.filename() == "data" && data_root.parent_path().filename() == "bench")) {
    const auto parent = data_root.parent_path();
    if (parent == data_root) {
      data_root.clear();
      break;
    }
    data_root = parent;
  }
  if (data_root.empty()) {
    throw std::invalid_argument("evaluation data must be located under bench/data");
  }
  const auto relative = absolute.lexically_relative(data_root);
  if (relative.empty() || relative.is_absolute() || *relative.begin() == "..") {
    throw std::invalid_argument("failed to derive evaluation data path relative to bench/data");
  }
  const auto relative_utf8 = relative.generic_u8string();
  std::string prefix(reinterpret_cast<const char*>(relative_utf8.data()), relative_utf8.size());
  prefix.push_back('\0');
  return Sha256File(absolute, prefix, true);
}

std::string DictionaryBundleSha256(const std::filesystem::path& directory) {
  std::vector<std::pair<std::string, std::filesystem::path>> parts;
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".azdic") continue;
    const auto name = entry.path().filename().u8string();
    std::string prefix(reinterpret_cast<const char*>(name.data()), name.size());
    prefix.push_back('\0');
    parts.emplace_back(std::move(prefix), entry.path());
  }
  if (parts.empty()) {
    throw std::invalid_argument("--dict-dir holds no .azdic file: " + directory.string());
  }
  // The NUL right after each name keeps the byte order of the names themselves.
  std::sort(parts.begin(), parts.end());
  return Sha256Files(parts, false);
}

double LatencyPercentile(std::vector<double> values, double fraction) {
  return Percentile(std::move(values), fraction);
}

bool ContainsKatakana(const std::string& text) {
  const auto code_points = DecodeUtf8CodePoints(text);
  return std::any_of(code_points.begin(), code_points.end(),
                     [](char32_t c) { return c >= 0x30A1 && c <= 0x30FA; });
}

std::vector<char32_t> DecodeUtf8CodePoints(const std::string& text) {
  std::vector<char32_t> output;
  size_t offset = 0;
  while (offset < text.size()) {
    const unsigned char lead = static_cast<unsigned char>(text[offset++]);
    char32_t codepoint = 0;
    size_t continuation = 0;
    if (lead <= 0x7f) {
      codepoint = lead;
    } else if ((lead & 0xe0) == 0xc0) {
      codepoint = lead & 0x1f;
      continuation = 1;
    } else if ((lead & 0xf0) == 0xe0) {
      codepoint = lead & 0x0f;
      continuation = 2;
    } else if ((lead & 0xf8) == 0xf0) {
      codepoint = lead & 0x07;
      continuation = 3;
    } else {
      throw std::invalid_argument("invalid UTF-8 leading byte");
    }
    if (offset + continuation > text.size()) throw std::invalid_argument("truncated UTF-8");
    for (size_t index = 0; index < continuation; ++index) {
      const unsigned char byte = static_cast<unsigned char>(text[offset++]);
      if ((byte & 0xc0) != 0x80) throw std::invalid_argument("invalid UTF-8 continuation byte");
      codepoint = (codepoint << 6) | (byte & 0x3f);
    }
    const bool overlong = (continuation == 1 && codepoint < 0x80) ||
                          (continuation == 2 && codepoint < 0x800) ||
                          (continuation == 3 && codepoint < 0x10000);
    if (overlong || codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
      throw std::invalid_argument("invalid UTF-8 code point");
    }
    output.push_back(codepoint);
  }
  return output;
}

std::string NormalizeNfkc(const std::string& text) {
  if (text.empty()) return {};
#if defined(_WIN32)
  const int wide_size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                            static_cast<int>(text.size()), nullptr, 0);
  if (wide_size <= 0) throw std::invalid_argument("invalid UTF-8 for NFKC normalization");
  std::wstring wide(static_cast<size_t>(wide_size), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                      wide.data(), wide_size);
  int normalized_capacity = NormalizeString(NormalizationKC, wide.data(), wide_size, nullptr, 0);
  if (normalized_capacity <= 0) throw std::runtime_error("NFKC normalization failed");
  std::wstring normalized;
  int normalized_size = 0;
  for (int attempt = 0; attempt < 10; ++attempt) {
    normalized.assign(static_cast<size_t>(normalized_capacity), L'\0');
    normalized_size = NormalizeString(NormalizationKC, wide.data(), wide_size, normalized.data(),
                                      normalized_capacity);
    if (normalized_size > 0) break;
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || normalized_size == 0) {
      throw std::runtime_error("NFKC normalization failed");
    }
    normalized_capacity = -normalized_size;
  }
  if (normalized_size <= 0) throw std::runtime_error("NFKC normalization failed");
  normalized.resize(static_cast<size_t>(normalized_size));
  const int utf8_size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, normalized.data(),
                                            normalized_size, nullptr, 0, nullptr, nullptr);
  if (utf8_size <= 0) throw std::runtime_error("failed to encode normalized UTF-8");
  std::string output(static_cast<size_t>(utf8_size), '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, normalized.data(), normalized_size,
                      output.data(), utf8_size, nullptr, nullptr);
  return output;
#else
  (void)text;
  throw std::runtime_error("NFKC normalization is only available on Windows");
#endif
}

size_t LevenshteinDistance(const std::vector<char32_t>& hypothesis,
                           const std::vector<char32_t>& reference) {
  std::vector<size_t> previous(reference.size() + 1);
  std::vector<size_t> current(reference.size() + 1);
  std::iota(previous.begin(), previous.end(), 0);
  for (size_t row = 1; row <= hypothesis.size(); ++row) {
    current[0] = row;
    for (size_t column = 1; column <= reference.size(); ++column) {
      const size_t substitution =
          previous[column - 1] + (hypothesis[row - 1] == reference[column - 1] ? 0 : 1);
      current[column] = std::min({previous[column] + 1, current[column - 1] + 1, substitution});
    }
    previous.swap(current);
  }
  return previous.back();
}

CandidateMatchMetrics EvaluateCandidateMatch(const std::string& top1, const std::string& expected,
                                             const std::vector<std::string>& acceptable) {
  CandidateMatchMetrics result;
  result.exact_match = top1 == expected;
  result.acceptable_match = !result.exact_match && std::find(acceptable.begin(), acceptable.end(),
                                                             top1) != acceptable.end();
  const auto hypothesis = DecodeUtf8CodePoints(top1);
  const auto reference = DecodeUtf8CodePoints(expected);
  result.edit_distance = LevenshteinDistance(hypothesis, reference);
  result.reference_length = reference.size();
  result.cer = reference.empty() ? (hypothesis.empty() ? 0.0 : 1.0)
                                 : Ratio(result.edit_distance, result.reference_length);

  const auto normalized_top1 = NormalizeNfkc(top1);
  const auto normalized_expected = NormalizeNfkc(expected);
  result.nfkc_exact_match = normalized_top1 == normalized_expected;
  const auto normalized_hypothesis = DecodeUtf8CodePoints(normalized_top1);
  const auto normalized_reference = DecodeUtf8CodePoints(normalized_expected);
  result.nfkc_edit_distance = LevenshteinDistance(normalized_hypothesis, normalized_reference);
  result.nfkc_reference_length = normalized_reference.size();
  result.nfkc_cer = normalized_reference.empty()
                        ? (normalized_hypothesis.empty() ? 0.0 : 1.0)
                        : Ratio(result.nfkc_edit_distance, result.nfkc_reference_length);
  return result;
}

bool RunConversionQualityEvaluation(const ConversionQualityOptions& options,
                                    const CandidateQuery& query, std::string* error) {
  try {
    if (options.eval_path.empty()) throw std::invalid_argument("--eval is required");
    if (options.output_path.empty())
      throw std::invalid_argument("--output is required with --eval");
    ValidateRunOptions(options);
    auto cases = LoadCases(options.eval_path, options.category);
    std::vector<EvaluatedCase> evaluated;
    evaluated.reserve(cases.size());
    Aggregate summary;
    std::map<std::string, Aggregate> by_category;
    // Cases whose expected surface holds katakana (lattice-conversion-spec section 10.2).
    Aggregate katakana;
    for (auto& item : cases) {
      auto result = EvaluateCase(std::move(item), options, query);
      const bool typo_clean = result.source.typo && !result.source.typo_type.has_value();
      Add(&summary, result, !typo_clean, options.typo_mode);
      if (!typo_clean) {
        Add(&by_category[result.source.categories.front()], result, true, options.typo_mode);
        if (ContainsKatakana(result.source.expected)) {
          Add(&katakana, result, true, options.typo_mode);
        }
      }
      evaluated.push_back(std::move(result));
    }

    auto summary_json = SerializeMetrics(summary, true);
    summary_json.emplace("memory_peak_mb", j::Value(PeakMemoryMb()));
    auto categories_json = SerializeCategories(by_category);
    auto config = BuildConfig(options, "bundled", EvaluationDatasetSha256(options.eval_path));

    j::Object root;
    root.emplace("by_category", j::Value(categories_json));
    root.emplace("by_subset", j::Value(SerializeSubsets(katakana)));
    root.emplace("diff_vs_baseline", j::Value(BaselineDiff(options.baseline_path, config,
                                                           summary_json, categories_json)));
    root.emplace("config", j::Value(std::move(config)));
    root.emplace("summary", j::Value(std::move(summary_json)));
    root.emplace("timestamp", j::Value(Iso8601Now()));
    root.emplace("version", j::Value(1));
    WriteFile(options.output_path, j::Stringify(j::Value(std::move(root))) + "\n");

    if (!options.per_case_path.empty()) {
      std::ostringstream jsonl;
      for (const auto& result : evaluated) {
        jsonl << j::Stringify(j::Value(SerializePerCase(result))) << '\n';
      }
      WriteFile(options.per_case_path, jsonl.str());
    }
    return true;
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return false;
  }
}

bool RunPersonalEvaluation(const ConversionQualityOptions& options,
                           const PersonalEvaluationInput& input, const CandidateQuery& query,
                           std::ostream& diagnostics, std::string* error) {
  try {
    if (options.output_path.empty()) {
      throw std::invalid_argument("--output is required with --personal-eval");
    }
    if (!options.per_case_path.empty() && !options.include_personal_text) {
      throw std::invalid_argument(
          "--per-case requires --include-personal-text with --personal-eval");
    }
    ValidateRunOptions(options);
    // Spec section 15.5: no category filter, whatever the caller left in the options.
    auto personal_options = options;
    personal_options.category = "all";
    diagnostics << "personal-eval: " << kUnsaltedHashWarning << std::endl;

    std::vector<EvaluatedCase> evaluated;
    Aggregate summary;
    std::map<std::string, Aggregate> by_category;
    Aggregate katakana;
    // Spec section 15.6.
    uint64_t net_commits = 0;
    uint64_t net_commits_top1 = 0;
    uint64_t net_commits_top5 = 0;
    size_t rejected_cases = 0;
    size_t rejected_top1 = 0;
    size_t ordinal = 0;
    for (const auto& item : input.cases) {
      EvaluationCase source;
      source.id = PersonalCaseId(++ordinal);
      source.input = item.reading;
      source.expected = item.expected;
      source.acceptable = item.acceptable;
      source.categories = {item.category};
      auto result = EvaluateCase(std::move(source), personal_options, query);
      const auto shown = [&result](const std::string& surface, size_t limit) {
        const size_t end = (std::min)(limit, result.candidates.size());
        for (size_t index = 0; index < end; ++index) {
          if (result.candidates[index].surface == surface) return true;
        }
        return false;
      };
      bool has_rejected = false;
      bool top1_rejected = false;
      for (const auto& surface : item.surfaces) {
        if (surface.commit_count > surface.reject_count) {
          const uint64_t net = surface.commit_count - surface.reject_count;
          net_commits += net;
          if (shown(surface.surface, 1)) net_commits_top1 += net;
          if (shown(surface.surface, kConversionQualityCandidateLimit)) net_commits_top5 += net;
        } else if (surface.commit_count < surface.reject_count) {
          has_rejected = true;
          top1_rejected = top1_rejected || shown(surface.surface, 1);
        }
      }
      if (has_rejected) {
        ++rejected_cases;
        if (top1_rejected) ++rejected_top1;
      }
      Add(&summary, result, true, personal_options.typo_mode);
      Add(&by_category[item.category], result, true, personal_options.typo_mode);
      if (ContainsKatakana(item.expected)) Add(&katakana, result, true, personal_options.typo_mode);
      if (!personal_options.per_case_path.empty()) evaluated.push_back(std::move(result));
    }

    // The section 6.2 typo metrics do not apply to the user's commits.
    auto summary_json = SerializeMetrics(summary, false);
    summary_json.emplace("memory_peak_mb", j::Value(PeakMemoryMb()));
    summary_json.emplace("commit_coverage_top1", j::Value(Ratio(net_commits_top1, net_commits)));
    summary_json.emplace("commit_coverage_top5", j::Value(Ratio(net_commits_top5, net_commits)));
    summary_json.emplace("rejected_top1_rate", j::Value(Ratio(rejected_top1, rejected_cases)));
    summary_json.emplace("n_cases", j::Value(static_cast<uint64_t>(input.cases.size())));
    summary_json.emplace("n_rejected_cases", j::Value(static_cast<uint64_t>(rejected_cases)));
    summary_json.emplace("n_rows_read", j::Value(input.rows_read));
    j::Object skipped;
    skipped.emplace("app_learning_disabled", j::Value(input.skipped.app_learning_disabled));
    skipped.emplace("app_privacy_mode", j::Value(input.skipped.app_privacy_mode));
    skipped.emplace("malformed_row", j::Value(input.skipped.malformed_row));
    skipped.emplace("no_net_commit", j::Value(input.skipped.no_net_commit));
    skipped.emplace("over_max_cases", j::Value(input.skipped.over_max_cases));
    skipped.emplace("secure_app", j::Value(input.skipped.secure_app));
    summary_json.emplace("skipped", j::Value(std::move(skipped)));
    auto categories_json = SerializeCategories(by_category);
    auto config = BuildConfig(personal_options, "personal", input.dataset_sha256);

    // Spec section 15.7: an incompatible baseline gives no diff and only the key names.
    j::Object diff;
    if (!personal_options.baseline_path.empty()) {
      const auto baseline = ReadBaseline(personal_options.baseline_path);
      const auto incompatible = IncompatibleBaselineKeys(baseline, config);
      if (incompatible.empty()) {
        diff = BaselineMetricsDiff(baseline, summary_json, categories_json);
      } else {
        diagnostics << "personal-eval: baseline is incompatible, so diff_vs_baseline is empty:";
        for (const auto& key : incompatible) diagnostics << ' ' << key;
        diagnostics << std::endl;
      }
    }

    j::Object root;
    root.emplace("by_category", j::Value(categories_json));
    root.emplace("by_subset", j::Value(SerializeSubsets(katakana)));
    root.emplace("diff_vs_baseline", j::Value(std::move(diff)));
    root.emplace("config", j::Value(std::move(config)));
    root.emplace("summary", j::Value(std::move(summary_json)));
    root.emplace("timestamp", j::Value(Iso8601Now()));
    root.emplace("version", j::Value(1));
    WriteFile(personal_options.output_path, j::Stringify(j::Value(std::move(root))) + "\n");

    if (!personal_options.per_case_path.empty()) {
      diagnostics << "personal-eval: " << kPersonalTextWarning << std::endl;
      std::ostringstream jsonl;
      for (const auto& result : evaluated) {
        jsonl << j::Stringify(j::Value(SerializePerCase(result))) << '\n';
      }
      WriteFile(personal_options.per_case_path, jsonl.str());
    }
    return true;
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return false;
  }
}

}  // namespace azookey::bench
