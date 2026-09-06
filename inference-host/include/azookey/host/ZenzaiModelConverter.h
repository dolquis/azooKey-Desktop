#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "azookey/core/IConverter.h"
#include "azookey/host/ZenzaiDecodeStats.h"

namespace azookey::host {

struct ZenzaiModelInfo {
  std::string path;
  uint32_t gguf_version{};
  uint64_t file_size_bytes{};
};

struct ZenzaiRuntimeOptions {
  int32_t n_gpu_layers{};
  std::optional<int32_t> n_threads;
  // Test-only fixture switch for no-llama builds; production callers leave this false.
  bool mock_candidates_for_tests{false};
  // Select only the Vulkan registry's device; CPU loads pass an empty device list.
  bool use_vulkan{false};
};

int32_t RecommendedZenzaiThreadCount(uint32_t hardware_threads);
size_t CommonPrefixLength(const std::vector<int32_t>& lhs, const std::vector<int32_t>& rhs);

struct BeamSequenceCopy {
  int32_t source_sequence{};
  int32_t destination_sequence{};
  bool operator==(const BeamSequenceCopy&) const = default;
};

struct BeamSequencePlan {
  std::vector<int32_t> assignments;
  std::vector<int32_t> releases;
  std::vector<BeamSequenceCopy> copies;
};

BeamSequencePlan PlanBeamSequenceAssignments(const std::vector<int32_t>& parent_sequences,
                                             const std::vector<int32_t>& active_sequences,
                                             int32_t max_working_sequences = 4);

struct ZenzaiModelRuntime;

struct ZenzaiLoadResult {
  ZenzaiLoadResult();
  ~ZenzaiLoadResult();
  ZenzaiLoadResult(ZenzaiLoadResult&&) noexcept;
  ZenzaiLoadResult& operator=(ZenzaiLoadResult&&) noexcept;
  ZenzaiLoadResult(const ZenzaiLoadResult&) = delete;
  ZenzaiLoadResult& operator=(const ZenzaiLoadResult&) = delete;

  bool ok{false};
  ZenzaiModelInfo info;
  std::string error;
  std::unique_ptr<ZenzaiModelRuntime> runtime;
};

// Tokenizer-related GGUF metadata that the loader may need to override at load time.
// Populated from the GGUF key-value store in llama.cpp builds; declared unconditionally so the
// override policy can be exercised without llama.cpp.
struct ZenzaiTokenizerMetadata {
  std::optional<std::string> pre_tokenizer;
  std::optional<uint32_t> eos_token_id;
  std::vector<std::string> vocabulary;
};

// One llama.cpp `kv_overrides` entry, described without depending on llama.h.
struct ZenzaiKvOverride {
  enum class Type { String, Int };

  std::string key;
  Type type{Type::String};
  std::string string_value;
  int64_t int_value{};
};

ZenzaiLoadResult ProbeZenzaiGgufModel(const std::string& path);
ZenzaiLoadResult LoadZenzaiGgufModel(const std::string& path,
                                     const ZenzaiRuntimeOptions& options = {});
std::optional<std::string_view> ResolveZenzaiPreTokenizerOverride(std::string_view pre_tokenizer);
std::optional<uint32_t> ResolveZenzaiEosTokenOverride(uint32_t declared_eos_token_id,
                                                      const std::vector<std::string>& vocabulary);
// Decides which GGUF KV entries the Zenzai loader overrides for `metadata`, in the order they are
// handed to llama.cpp. Returns an empty vector when the GGUF needs no override.
std::vector<ZenzaiKvOverride> BuildZenzaiKvOverrides(const ZenzaiTokenizerMetadata& metadata);

class ZenzaiModelConverter final : public core::IConverter {
 public:
  ZenzaiModelConverter(ZenzaiLoadResult&& loaded, core::IConverter* fallback);
  ~ZenzaiModelConverter() override;

  const ZenzaiModelInfo& info() const { return info_; }
  bool runtime_loaded() const { return runtime_ != nullptr; }
  std::optional<std::string> last_error() const { return last_error_; }
  std::optional<ZenzaiDecodeStats> last_decode_stats() const;
  std::vector<int32_t> TokenizePromptForValidation(const std::string& kana,
                                                   const core::ConversionContext& context) const;

  std::vector<core::Candidate> Convert(const std::string& kana,
                                       const core::ConversionContext& context) override;
  std::vector<core::Candidate> PredictNext(const std::string& kana,
                                           const core::ConversionContext& context) override;
  std::vector<core::Candidate> Correct(const std::string& kana, const core::CorrectionHint& hint,
                                       const core::ConversionContext& context) override;
  void Commit(const core::Candidate& selected_candidate,
              const core::ConversionContext& context) override;
  void Learn(const std::string& committed_surface, const std::string& committed_reading) override;

 private:
  std::vector<core::Candidate> DegradeToFallback(const std::string& kana,
                                                 const core::ConversionContext& context,
                                                 const std::string& reason);
  void TagFallback(std::vector<core::Candidate>& candidates) const;

  ZenzaiModelInfo info_;
  std::unique_ptr<ZenzaiModelRuntime> runtime_;
  core::IConverter* fallback_;
  std::optional<std::string> last_error_;
};

}  // namespace azookey::host
