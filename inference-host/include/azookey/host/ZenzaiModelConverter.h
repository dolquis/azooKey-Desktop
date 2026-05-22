#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "azookey/core/IConverter.h"

namespace azookey::host {

struct ZenzaiModelInfo {
  std::string path;
  uint32_t gguf_version{};
  uint64_t file_size_bytes{};
};

struct ZenzaiLoadResult {
  bool ok{false};
  ZenzaiModelInfo info;
  std::string error;
};

ZenzaiLoadResult ProbeZenzaiGgufModel(const std::string& path);

class ZenzaiModelConverter final : public core::IConverter {
 public:
  ZenzaiModelConverter(ZenzaiModelInfo info,
                       std::unique_ptr<core::IConverter> fallback);

  const ZenzaiModelInfo& info() const { return info_; }

  std::vector<core::Candidate> Convert(
      const std::string& kana, const core::ConversionContext& context) override;
  std::vector<core::Candidate> PredictNext(
      const std::string& kana, const core::ConversionContext& context) override;
  std::vector<core::Candidate> Correct(
      const std::string& kana,
      const core::CorrectionHint& hint,
      const core::ConversionContext& context) override;
  void Commit(const core::Candidate& selected_candidate,
              const core::ConversionContext& context) override;
  void Learn(const std::string& committed_surface,
             const std::string& committed_reading) override;

 private:
  void TagFallback(std::vector<core::Candidate>& candidates) const;

  ZenzaiModelInfo info_;
  std::unique_ptr<core::IConverter> fallback_;
};

}  // namespace azookey::host
