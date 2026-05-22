#include "azookey/host/ZenzaiModelConverter.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>

#include "azookey/core/SimpleConverter.h"

namespace azookey::host {

namespace {

constexpr std::array<char, 4> kGgufMagic{'G', 'G', 'U', 'F'};

uint32_t ReadLe32(const std::array<unsigned char, 4>& bytes) {
  return static_cast<uint32_t>(bytes[0]) |
         (static_cast<uint32_t>(bytes[1]) << 8) |
         (static_cast<uint32_t>(bytes[2]) << 16) |
         (static_cast<uint32_t>(bytes[3]) << 24);
}

void AppendDebugTag(std::string& debug_info, const std::string& tag) {
  if (debug_info.empty()) {
    debug_info = tag;
    return;
  }
  debug_info += ";" + tag;
}

}  // namespace

ZenzaiLoadResult ProbeZenzaiGgufModel(const std::string& path) {
  ZenzaiLoadResult result;
  result.info.path = path;

  std::error_code ec;
  const auto status = std::filesystem::status(path, ec);
  if (ec) {
    result.error = "model file probe failed: " + ec.message();
    return result;
  }
  if (!std::filesystem::exists(status)) {
    result.error = "model file not found";
    return result;
  }
  if (!std::filesystem::is_regular_file(status)) {
    result.error = "model path is not a regular file";
    return result;
  }

  const auto size = std::filesystem::file_size(path, ec);
  if (ec) {
    result.error = "model file size probe failed: " + ec.message();
    return result;
  }
  result.info.file_size_bytes = static_cast<uint64_t>(size);
  if (size < 8) {
    result.error = "model file is too small to be GGUF";
    return result;
  }

  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    result.error = "model file could not be opened";
    return result;
  }

  std::array<char, 4> magic{};
  file.read(magic.data(), static_cast<std::streamsize>(magic.size()));
  if (!file || magic != kGgufMagic) {
    result.error = "model file is not GGUF";
    return result;
  }

  std::array<unsigned char, 4> version_bytes{};
  file.read(reinterpret_cast<char*>(version_bytes.data()),
            static_cast<std::streamsize>(version_bytes.size()));
  if (!file) {
    result.error = "model file is missing GGUF version";
    return result;
  }

  result.info.gguf_version = ReadLe32(version_bytes);
  if (result.info.gguf_version == 0 || result.info.gguf_version > 3) {
    result.error = "unsupported GGUF version: " +
                   std::to_string(result.info.gguf_version);
    return result;
  }

  result.ok = true;
  return result;
}

ZenzaiModelConverter::ZenzaiModelConverter(
    ZenzaiModelInfo info, std::unique_ptr<core::IConverter> fallback)
    : info_(std::move(info)),
      fallback_(std::move(fallback)) {
  if (!fallback_) {
    fallback_ = std::make_unique<core::SimpleConverter>();
  }
}

std::vector<core::Candidate> ZenzaiModelConverter::Convert(
    const std::string& kana, const core::ConversionContext& context) {
  auto candidates = fallback_->Convert(kana, context);
  TagFallback(candidates);
  return candidates;
}

std::vector<core::Candidate> ZenzaiModelConverter::PredictNext(
    const std::string& kana, const core::ConversionContext& context) {
  auto candidates = fallback_->PredictNext(kana, context);
  TagFallback(candidates);
  return candidates;
}

std::vector<core::Candidate> ZenzaiModelConverter::Correct(
    const std::string& kana,
    const core::CorrectionHint& hint,
    const core::ConversionContext& context) {
  auto candidates = fallback_->Correct(kana, hint, context);
  TagFallback(candidates);
  return candidates;
}

void ZenzaiModelConverter::Commit(
    const core::Candidate& selected_candidate,
    const core::ConversionContext& context) {
  fallback_->Commit(selected_candidate, context);
}

void ZenzaiModelConverter::Learn(const std::string& committed_surface,
                                 const std::string& committed_reading) {
  fallback_->Learn(committed_surface, committed_reading);
}

void ZenzaiModelConverter::TagFallback(
    std::vector<core::Candidate>& candidates) const {
  for (auto& candidate : candidates) {
    AppendDebugTag(candidate.debug_info, "zenzai-gguf-loaded");
    AppendDebugTag(candidate.debug_info, "fallback-converter");
  }
}

}  // namespace azookey::host
