#include "azookey/host/ZenzaiModelConverter.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>

#ifndef AZOOKEY_WITH_LLAMA_CPP
#define AZOOKEY_WITH_LLAMA_CPP 0
#endif

#if AZOOKEY_WITH_LLAMA_CPP
#include <llama.h>
#endif

namespace azookey::host {

namespace {

constexpr std::array<char, 4> kGgufMagic{'G', 'G', 'U', 'F'};

uint32_t ReadLe32(const std::array<unsigned char, 4>& bytes) {
  return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
         (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}

void AppendDebugTag(std::string& debug_info, const std::string& tag) {
  if (debug_info.empty()) {
    debug_info = tag;
    return;
  }
  debug_info += ";" + tag;
}

#if AZOOKEY_WITH_LLAMA_CPP
class LlamaBackendSession {
 public:
  LlamaBackendSession() { llama_backend_init(); }
  ~LlamaBackendSession() { llama_backend_free(); }
};

LlamaBackendSession& LlamaBackend() {
  static LlamaBackendSession session;
  return session;
}
#endif

}  // namespace

struct ZenzaiModelRuntime {
#if AZOOKEY_WITH_LLAMA_CPP
  ~ZenzaiModelRuntime() {
    if (context) {
      llama_free(context);
    }
    if (model) {
      llama_model_free(model);
    }
  }

  ZenzaiModelRuntime() = default;
  ZenzaiModelRuntime(const ZenzaiModelRuntime&) = delete;
  ZenzaiModelRuntime& operator=(const ZenzaiModelRuntime&) = delete;

  llama_model* model{nullptr};
  llama_context* context{nullptr};
#endif
};

ZenzaiLoadResult::ZenzaiLoadResult() = default;
ZenzaiLoadResult::~ZenzaiLoadResult() = default;
ZenzaiLoadResult::ZenzaiLoadResult(ZenzaiLoadResult&&) noexcept = default;
ZenzaiLoadResult& ZenzaiLoadResult::operator=(ZenzaiLoadResult&&) noexcept = default;

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
    result.error = "unsupported GGUF version: " + std::to_string(result.info.gguf_version);
    return result;
  }

  result.ok = true;
  return result;
}

ZenzaiLoadResult LoadZenzaiGgufModel(const std::string& path, const ZenzaiRuntimeOptions& options) {
  auto result = ProbeZenzaiGgufModel(path);
  if (!result.ok) {
    return result;
  }

#if AZOOKEY_WITH_LLAMA_CPP
  (void)LlamaBackend();
  auto runtime = std::make_unique<ZenzaiModelRuntime>();

  auto model_params = llama_model_default_params();
  model_params.n_gpu_layers = options.n_gpu_layers;
  runtime->model = llama_model_load_from_file(path.c_str(), model_params);
  if (!runtime->model) {
    result.ok = false;
    result.error = "llama.cpp model load failed";
    return result;
  }

  auto context_params = llama_context_default_params();
  context_params.n_ctx = 512;
  context_params.n_batch = 64;
  context_params.n_ubatch = 64;
  runtime->context = llama_init_from_model(runtime->model, context_params);
  if (!runtime->context) {
    result.ok = false;
    result.error = "llama.cpp context creation failed";
    return result;
  }

  result.runtime = std::move(runtime);
#else
  (void)options;
  result.runtime = std::make_unique<ZenzaiModelRuntime>();
#endif
  result.ok = true;
  result.error.clear();
  return result;
}

ZenzaiModelConverter::ZenzaiModelConverter(ZenzaiLoadResult&& loaded, core::IConverter* fallback)
    : info_(std::move(loaded.info)), runtime_(std::move(loaded.runtime)), fallback_(fallback) {}

ZenzaiModelConverter::~ZenzaiModelConverter() = default;

std::vector<core::Candidate> ZenzaiModelConverter::Convert(const std::string& kana,
                                                           const core::ConversionContext& context) {
  if (!fallback_) return {};
  auto candidates = fallback_->Convert(kana, context);
  TagFallback(candidates);
  return candidates;
}

std::vector<core::Candidate> ZenzaiModelConverter::PredictNext(
    const std::string& kana, const core::ConversionContext& context) {
  if (!fallback_) return {};
  auto candidates = fallback_->PredictNext(kana, context);
  TagFallback(candidates);
  return candidates;
}

std::vector<core::Candidate> ZenzaiModelConverter::Correct(const std::string& kana,
                                                           const core::CorrectionHint& hint,
                                                           const core::ConversionContext& context) {
  if (!fallback_) return {};
  auto candidates = fallback_->Correct(kana, hint, context);
  TagFallback(candidates);
  return candidates;
}

void ZenzaiModelConverter::Commit(const core::Candidate& selected_candidate,
                                  const core::ConversionContext& context) {
  if (!fallback_) return;
  fallback_->Commit(selected_candidate, context);
}

void ZenzaiModelConverter::Learn(const std::string& committed_surface,
                                 const std::string& committed_reading) {
  if (!fallback_) return;
  fallback_->Learn(committed_surface, committed_reading);
}

void ZenzaiModelConverter::TagFallback(std::vector<core::Candidate>& candidates) const {
  for (auto& candidate : candidates) {
    AppendDebugTag(candidate.debug_info, "zenzai-gguf-loaded");
    AppendDebugTag(candidate.debug_info, "fallback-converter");
  }
}

}  // namespace azookey::host
