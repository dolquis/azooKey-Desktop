#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "azookey/core/Candidate.h"

namespace azookey::bench {

inline constexpr size_t kConversionQualityCandidateLimit = 5;

struct CandidateMatchMetrics {
  bool exact_match{false};
  bool nfkc_exact_match{false};
  bool acceptable_match{false};
  size_t edit_distance{0};
  size_t reference_length{0};
  size_t nfkc_edit_distance{0};
  size_t nfkc_reference_length{0};
  double cer{0.0};
  double nfkc_cer{0.0};
};

struct ConversionQualityOptions {
  std::filesystem::path eval_path;
  std::filesystem::path output_path;
  std::filesystem::path per_case_path;
  std::filesystem::path baseline_path;
  std::string backend{"cpu"};
  std::string model;
  std::string build_id{"unknown"};
  std::string host_version{"0.1.0"};
  std::string category{"all"};
  std::string decode{"simple"};
  std::string learning_state{"empty"};
  std::string typo_mode{"off"};
  // Compatibility keys of the bundled dictionary run (spec section 14.1): DictionaryBundleSha256
  // of --dict-dir, and whether lattice conversion stage 1 was on.
  std::string dictionary_bundle_sha256{"none"};
  std::string lattice{"off"};
  // Lattice conversion stage 2 (lattice-conversion-spec section 3.3): "off", or "k=<K>;band=<B>"
  // of --lattice-with-model and --lattice-band. A result without the key counts as off.
  std::string lattice_with_model{"off"};
  // Whether NLL rerank was on (spec section 14.1). A result without the key counts as off.
  std::string nll{"off"};
  // SHA-256 of the bench-only Zenzai profile (spec section 15.5), "none" without one.
  std::string profile_sha256{"none"};
  // The personal bench writes --per-case only when the user allowed it (spec section 15.7).
  bool include_personal_text{false};
  size_t beam_width{1};
  size_t n_best{kConversionQualityCandidateLimit};
  size_t max_new_tokens{0};
  size_t prompt_template_version{0};
  size_t thread_count{1};
  size_t batch_size{1};
  size_t iterations{30};
  bool trace{false};
};

// The debug_info of a candidate the typo correction made (typo-correction-learning-spec section
// 5-2), the mark the TIP renders as a suggestion.
inline constexpr std::string_view kTypoCorrectionDebugInfo = "typo-correction";

struct CandidateQueryResult {
  CandidateQueryResult() = default;
  // Implicit, so a query that only has candidates can return them as they are.
  CandidateQueryResult(std::vector<core::Candidate> candidates)
      : candidates(std::move(candidates)) {}

  std::vector<core::Candidate> candidates;
  // Non-empty when the conversion ran on a corrected reading instead of the given one (the v1
  // pair under aggressive, InferenceEngine::CandidatesResult::corrected_reading).
  std::string corrected_reading;
};

// Called with the reading, the left context and the case's raw_keys (empty for a case without
// them, spec section 4.3).
using CandidateQuery =
    std::function<CandidateQueryResult(const std::string&, const std::string&, const std::string&)>;

// Printed on every personal run and on every run with a profile (spec section 15.7).
inline constexpr std::string_view kUnsaltedHashWarning =
    "do not share this result file: eval_dataset_sha256 and profile_sha256 are unsalted "
    "SHA-256 values, so the learning data behind a small store or a short profile can be "
    "guessed by hashing candidate strings";
inline constexpr std::string_view kPersonalTextWarning =
    "--per-case writes the readings and surfaces of your learning data in plain text; the file "
    "is not encrypted and deleting all learning data does not remove it, so delete it yourself";

// Rows, readings and lines left out of the personal bench, by reason (spec section 15.6).
struct PersonalSkipCounts {
  uint64_t malformed_row{};
  uint64_t no_net_commit{};
  uint64_t secure_app{};
  uint64_t app_privacy_mode{};
  uint64_t app_learning_disabled{};
  uint64_t over_max_cases{};
};

// One surface of a reading, summed over its app rows (spec section 15.3).
struct PersonalSurfaceCounts {
  std::string surface;
  uint64_t commit_count{};
  uint64_t reject_count{};
};

struct PersonalEvaluationCase {
  std::string reading;
  std::string expected;
  std::vector<std::string> acceptable;
  std::string category;
  // Every surface left after the exclusions (net committed, offset and net rejected), ordered
  // by surface bytes.
  std::vector<PersonalSurfaceCounts> surfaces;
};

struct PersonalEvaluationInput {
  // In the section 15.3 order.
  std::vector<PersonalEvaluationCase> cases;
  PersonalSkipCounts skipped;
  // Rows read in a form the bench can evaluate; skipped.malformed_row counts the others.
  uint64_t rows_read{};
  // The section 15.7 eval_dataset_sha256.
  std::string dataset_sha256;
};

std::vector<char32_t> DecodeUtf8CodePoints(const std::string& text);
std::string NormalizeNfkc(const std::string& text);
size_t LevenshteinDistance(const std::vector<char32_t>& hypothesis,
                           const std::vector<char32_t>& reference);
CandidateMatchMetrics EvaluateCandidateMatch(const std::string& top1, const std::string& expected,
                                             const std::vector<std::string>& acceptable);
std::string EvaluationDatasetSha256(const std::filesystem::path& path);
// SHA-256 over the .azdic files of `directory`, ordered by file name bytes, each framed as
// file name (UTF-8) + 0x00 + file bytes (spec section 14.1). Throws when there is none.
std::string DictionaryBundleSha256(const std::filesystem::path& directory);
// Whether `text` holds a katakana letter, U+30A1..U+30FA (lattice-conversion-spec section 10.2).
bool ContainsKatakana(const std::string& text);
// The floor(fraction x (n - 1))-th of the n sorted values, 0 when empty (spec section 8).
double LatencyPercentile(std::vector<double> values, double fraction);

bool RunConversionQualityEvaluation(const ConversionQualityOptions& options,
                                    const CandidateQuery& query, std::string* error);

// The personal regression bench (spec section 15): writes the aggregates to options.output_path
// and the warnings and the names of incompatible baseline keys to `diagnostics`, never a reading,
// a surface or a profile. options.eval_path and options.category are not used.
bool RunPersonalEvaluation(const ConversionQualityOptions& options,
                           const PersonalEvaluationInput& input, const CandidateQuery& query,
                           std::ostream& diagnostics, std::string* error);

}  // namespace azookey::bench
