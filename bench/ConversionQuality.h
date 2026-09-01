#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
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
  size_t beam_width{1};
  size_t n_best{kConversionQualityCandidateLimit};
  size_t max_new_tokens{0};
  size_t prompt_template_version{0};
  size_t thread_count{1};
  size_t batch_size{1};
  size_t iterations{30};
  bool trace{false};
};

using CandidateQuery =
    std::function<std::vector<core::Candidate>(const std::string&, const std::string&)>;

std::vector<char32_t> DecodeUtf8CodePoints(const std::string& text);
std::string NormalizeNfkc(const std::string& text);
size_t LevenshteinDistance(const std::vector<char32_t>& hypothesis,
                           const std::vector<char32_t>& reference);
CandidateMatchMetrics EvaluateCandidateMatch(const std::string& top1, const std::string& expected,
                                             const std::vector<std::string>& acceptable);
std::string EvaluationDatasetSha256(const std::filesystem::path& path);

bool RunConversionQualityEvaluation(const ConversionQualityOptions& options,
                                    const CandidateQuery& query, std::string* error);

}  // namespace azookey::bench
