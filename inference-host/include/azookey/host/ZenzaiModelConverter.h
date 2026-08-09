#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "azookey/core/IConverter.h"

namespace azookey::host {

struct ZenzaiModelInfo {
  std::string path;
  uint32_t gguf_version{};
  uint64_t file_size_bytes{};
};

struct ZenzaiRuntimeOptions {
  int32_t n_gpu_layers{};
  // Test-only fixture switch for no-llama builds; production callers leave this false.
  bool mock_candidates_for_tests{false};
};

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

ZenzaiLoadResult ProbeZenzaiGgufModel(const std::string& path);
ZenzaiLoadResult LoadZenzaiGgufModel(const std::string& path,
                                     const ZenzaiRuntimeOptions& options = {});
std::optional<std::string_view> ResolveZenzaiPreTokenizerOverride(std::string_view pre_tokenizer);
std::optional<uint32_t> ResolveZenzaiEosTokenOverride(uint32_t declared_eos_token_id,
                                                      const std::vector<std::string>& vocabulary);

class ZenzaiModelConverter final : public core::IConverter {
 public:
  ZenzaiModelConverter(ZenzaiLoadResult&& loaded, core::IConverter* fallback);
  ~ZenzaiModelConverter() override;

  const ZenzaiModelInfo& info() const { return info_; }
  bool runtime_loaded() const { return runtime_ != nullptr; }
  std::optional<std::string> last_error() const { return last_error_; }
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
