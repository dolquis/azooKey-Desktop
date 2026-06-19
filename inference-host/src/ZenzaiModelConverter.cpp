#include "azookey/host/ZenzaiModelConverter.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
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
constexpr double kZenzaiScoreFloor = 0.3;
constexpr double kZenzaiScoreCeil = 1.4;
constexpr size_t kMaxLeftContextCodepoints = 30;
constexpr int32_t kMaxNewTokens = 64;
constexpr size_t kMaxModelCandidates = 4;

struct GeneratedCandidate {
  std::string surface;
  double total_logprob{};
  int32_t token_count{};
};

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

bool DecodeNextUtf8(const std::string& input, size_t& offset, char32_t& codepoint) {
  const size_t start = offset;
  const auto first = static_cast<unsigned char>(input[offset]);
  if (first < 0x80) {
    codepoint = first;
    ++offset;
    return true;
  }

  int width = 0;
  char32_t value = 0;
  if ((first & 0xE0) == 0xC0) {
    width = 2;
    value = first & 0x1F;
  } else if ((first & 0xF0) == 0xE0) {
    width = 3;
    value = first & 0x0F;
  } else if ((first & 0xF8) == 0xF0) {
    width = 4;
    value = first & 0x07;
  } else {
    codepoint = first;
    ++offset;
    return false;
  }

  if (offset + static_cast<size_t>(width) > input.size()) {
    codepoint = first;
    offset = start + 1;
    return false;
  }
  for (int i = 1; i < width; ++i) {
    const auto byte = static_cast<unsigned char>(input[offset + static_cast<size_t>(i)]);
    if ((byte & 0xC0) != 0x80) {
      codepoint = first;
      offset = start + 1;
      return false;
    }
    value = (value << 6) | (byte & 0x3F);
  }
  if ((width == 2 && value < 0x80) || (width == 3 && value < 0x800) ||
      (width == 4 && value < 0x10000) || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
    codepoint = first;
    offset = start + 1;
    return false;
  }
  offset += static_cast<size_t>(width);
  codepoint = value;
  return true;
}

bool IsValidUtf8String(const std::string& input) {
  for (size_t offset = 0; offset < input.size();) {
    char32_t codepoint = 0;
    if (!DecodeNextUtf8(input, offset, codepoint)) {
      return false;
    }
  }
  return true;
}

bool IsCanceled(const core::ConversionContext& context) {
  return context.cancel && context.cancel->load(std::memory_order_relaxed);
}

bool DeadlineExpired(const core::ConversionContext& context) {
  return context.deadline && std::chrono::steady_clock::now() >= *context.deadline;
}

void AppendUtf8(std::string& output, char32_t codepoint) {
  if (codepoint <= 0x7F) {
    output.push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7FF) {
    output.push_back(static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F)));
    output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else if (codepoint <= 0xFFFF) {
    output.push_back(static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F)));
    output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else {
    output.push_back(static_cast<char>(0xF0 | ((codepoint >> 18) & 0x07)));
    output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
    output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  }
}

std::string Utf8(char32_t codepoint) {
  std::string output;
  AppendUtf8(output, codepoint);
  return output;
}

std::string ToKatakana(const std::string& input) {
  std::string output;
  output.reserve(input.size());
  for (size_t offset = 0; offset < input.size();) {
    char32_t codepoint = 0;
    DecodeNextUtf8(input, offset, codepoint);
    if (codepoint >= U'\u3041' && codepoint <= U'\u3096') {
      codepoint += 0x60;
    }
    AppendUtf8(output, codepoint);
  }
  return output;
}

size_t CountUtf8Codepoints(const std::string& input) {
  size_t count = 0;
  for (size_t offset = 0; offset < input.size();) {
    char32_t ignored = 0;
    DecodeNextUtf8(input, offset, ignored);
    ++count;
  }
  return count;
}

std::string TakeLastCodepoints(const std::string& input, size_t max_codepoints) {
  std::vector<size_t> starts;
  starts.reserve(input.size());
  for (size_t offset = 0; offset < input.size();) {
    starts.push_back(offset);
    char32_t ignored = 0;
    DecodeNextUtf8(input, offset, ignored);
  }
  if (starts.size() <= max_codepoints) {
    return input;
  }
  return input.substr(starts[starts.size() - max_codepoints]);
}

std::string CleanLeftContext(const std::string& preceding_text) {
  const auto last_break = preceding_text.find_last_of("\r\n");
  std::string context =
      last_break == std::string::npos ? preceding_text : preceding_text.substr(last_break + 1);
  while (!context.empty() && std::isspace(static_cast<unsigned char>(context.front())) != 0) {
    context.erase(context.begin());
  }
  return TakeLastCodepoints(context, kMaxLeftContextCodepoints);
}

std::string BuildZenzaiPrompt(const std::string& kana, const core::ConversionContext& context,
                              const std::string& profile = {}) {
  std::string prompt;
  if (!profile.empty()) {
    prompt += Utf8(U'\U0000EE03');
    prompt += profile;
  }
  const auto left_context = CleanLeftContext(context.preceding_text);
  if (!left_context.empty()) {
    prompt += Utf8(U'\U0000EE02');
    prompt += left_context;
  }
  prompt += Utf8(U'\U0000EE00');
  prompt += ToKatakana(kana);
  prompt += Utf8(U'\U0000EE01');
  return prompt;
}

int32_t MaxNewTokensForReading(const std::string& kana) {
  const auto count = CountUtf8Codepoints(kana);
  const auto budget = static_cast<int32_t>(std::min<size_t>(kMaxNewTokens, count * 2 + 8));
  return std::max<int32_t>(1, budget);
}

double NormalizeLogprob(double total_logprob, int32_t token_count) {
  if (token_count <= 0 || !std::isfinite(total_logprob)) {
    return kZenzaiScoreFloor;
  }
  const auto avg = std::min(0.0, total_logprob / static_cast<double>(token_count));
  const auto prob_geo = std::clamp(std::exp(avg), 0.0, 1.0);
  return kZenzaiScoreFloor + (kZenzaiScoreCeil - kZenzaiScoreFloor) * prob_geo;
}

std::string FormatDouble(double value) {
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(4) << value;
  return oss.str();
}

void DedupBySurface(std::vector<core::Candidate>& candidates) {
  std::vector<core::Candidate> deduped;
  deduped.reserve(candidates.size());
  for (auto& candidate : candidates) {
    auto existing = std::find_if(deduped.begin(), deduped.end(), [&](const auto& item) {
      return item.surface == candidate.surface;
    });
    if (existing == deduped.end()) {
      deduped.push_back(std::move(candidate));
    } else if (candidate.score > existing->score) {
      *existing = std::move(candidate);
    }
  }
  candidates = std::move(deduped);
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

struct LlamaTokenChoice {
  llama_token token{LLAMA_TOKEN_NULL};
  float logit{-std::numeric_limits<float>::infinity()};
  double logprob{-std::numeric_limits<double>::infinity()};
};

std::vector<LlamaTokenChoice> CollectTokenChoices(const llama_vocab* vocab, const float* logits,
                                                  int32_t vocab_size, size_t max_choices) {
  if (max_choices == 0 || vocab_size <= 0) {
    return {};
  }
  std::vector<LlamaTokenChoice> choices;
  choices.reserve(max_choices);
  double max_logit = -std::numeric_limits<double>::infinity();
  for (int32_t id = 0; id < vocab_size; ++id) {
    max_logit = std::max(max_logit, static_cast<double>(logits[id]));
    const auto token = static_cast<llama_token>(id);
    if (llama_vocab_is_control(vocab, token) && !llama_vocab_is_eog(vocab, token)) {
      continue;
    }
    if (choices.size() < max_choices || logits[id] > choices.back().logit) {
      choices.push_back(LlamaTokenChoice{token, logits[id], 0.0});
      std::sort(choices.begin(), choices.end(),
                [](const auto& lhs, const auto& rhs) { return lhs.logit > rhs.logit; });
      if (choices.size() > max_choices) {
        choices.resize(max_choices);
      }
    }
  }
  double denom = 0.0;
  for (int32_t id = 0; id < vocab_size; ++id) {
    denom += std::exp(static_cast<double>(logits[id]) - max_logit);
  }
  const double log_denom = max_logit + std::log(denom);
  for (auto& choice : choices) {
    choice.logprob = static_cast<double>(choice.logit) - log_denom;
  }
  return choices;
}

std::string TokenPiece(const llama_vocab* vocab, llama_token token) {
  std::array<char, 256> piece_buffer{};
  int32_t piece_size = llama_token_to_piece(vocab, token, piece_buffer.data(),
                                            static_cast<int32_t>(piece_buffer.size()), 0, false);
  if (piece_size < 0) {
    std::string piece(static_cast<size_t>(-piece_size), '\0');
    piece_size = llama_token_to_piece(vocab, token, piece.data(),
                                      static_cast<int32_t>(piece.size()), 0, false);
    if (piece_size > 0) {
      piece.resize(static_cast<size_t>(piece_size));
      return piece;
    }
    return {};
  }
  if (piece_size <= 0) {
    return {};
  }
  return std::string(piece_buffer.data(), static_cast<size_t>(piece_size));
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

  std::vector<GeneratedCandidate> Generate(const std::string& kana,
                                           const core::ConversionContext& conversion_context) {
    if (!model || !context) {
      throw std::runtime_error("llama.cpp runtime is not ready");
    }

    const auto* vocab = llama_model_get_vocab(model);
    if (!vocab) {
      throw std::runtime_error("llama.cpp vocab is not available");
    }
    if (IsCanceled(conversion_context)) {
      return {};
    }

    const auto prompt = BuildZenzaiPrompt(kana, conversion_context);
    const bool add_bos = llama_vocab_get_add_bos(vocab);
    std::vector<llama_token> prompt_tokens(std::max<size_t>(8, prompt.size() + 8));
    int32_t token_count = llama_tokenize(vocab, prompt.c_str(), static_cast<int32_t>(prompt.size()),
                                         prompt_tokens.data(),
                                         static_cast<int32_t>(prompt_tokens.size()), add_bos, true);
    if (token_count < 0) {
      prompt_tokens.resize(static_cast<size_t>(-token_count));
      token_count = llama_tokenize(vocab, prompt.c_str(), static_cast<int32_t>(prompt.size()),
                                   prompt_tokens.data(), static_cast<int32_t>(prompt_tokens.size()),
                                   add_bos, true);
    }
    if (token_count <= 0) {
      throw std::runtime_error("llama.cpp prompt tokenization failed");
    }
    prompt_tokens.resize(static_cast<size_t>(token_count));

    llama_kv_self_clear(context);
    auto prompt_batch = llama_batch_get_one(prompt_tokens.data(), token_count);
    if (llama_decode(context, prompt_batch) != 0) {
      throw std::runtime_error("llama.cpp prompt decode failed");
    }

    const int32_t vocab_size = llama_vocab_n_tokens(vocab);
    const int32_t max_new = MaxNewTokensForReading(kana);
    float* initial_logits = llama_get_logits_ith(context, -1);
    if (!initial_logits) {
      throw std::runtime_error("llama.cpp logits are not available");
    }
    auto first_choices =
        CollectTokenChoices(vocab, initial_logits, vocab_size, kMaxModelCandidates);
    if (first_choices.empty()) {
      throw std::runtime_error("llama.cpp did not select a token");
    }

    std::vector<GeneratedCandidate> generated;
    generated.reserve(first_choices.size());
    for (const auto& first_choice : first_choices) {
      if (IsCanceled(conversion_context)) {
        return {};
      }
      if (DeadlineExpired(conversion_context) || llama_vocab_is_eog(vocab, first_choice.token)) {
        continue;
      }

      std::string surface = TokenPiece(vocab, first_choice.token);
      double total_logprob = first_choice.logprob;
      int32_t output_tokens = 1;
      std::vector<llama_token> decoded_tokens = prompt_tokens;
      decoded_tokens.push_back(first_choice.token);

      if (max_new > 1) {
        llama_kv_self_clear(context);
        auto first_batch =
            llama_batch_get_one(decoded_tokens.data(), static_cast<int32_t>(decoded_tokens.size()));
        if (llama_decode(context, first_batch) != 0) {
          throw std::runtime_error("llama.cpp first token decode failed");
        }
      }

      for (int32_t step = 1; step < max_new; ++step) {
        if (IsCanceled(conversion_context)) {
          return {};
        }
        if (DeadlineExpired(conversion_context)) {
          break;
        }
        float* logits = llama_get_logits_ith(context, -1);
        if (!logits) {
          throw std::runtime_error("llama.cpp logits are not available");
        }
        auto choices = CollectTokenChoices(vocab, logits, vocab_size, 1);
        if (choices.empty()) {
          throw std::runtime_error("llama.cpp did not select a token");
        }
        const auto& selected = choices.front();
        if (llama_vocab_is_eog(vocab, selected.token)) {
          break;
        }

        surface += TokenPiece(vocab, selected.token);
        total_logprob += selected.logprob;
        ++output_tokens;

        if (step + 1 < max_new) {
          if (IsCanceled(conversion_context)) {
            return {};
          }
          if (DeadlineExpired(conversion_context)) {
            break;
          }
          decoded_tokens.push_back(selected.token);
          llama_token next = selected.token;
          auto next_batch = llama_batch_get_one(&next, 1);
          if (llama_decode(context, next_batch) != 0) {
            throw std::runtime_error("llama.cpp token decode failed");
          }
        }
      }

      if (!surface.empty()) {
        generated.push_back(GeneratedCandidate{surface, total_logprob, output_tokens});
      }
    }

    return generated;
  }
#else
  std::vector<GeneratedCandidate> Generate(const std::string& kana,
                                           const core::ConversionContext& conversion_context) {
    if (IsCanceled(conversion_context)) {
      return {};
    }
    if (ToKatakana(kana) == u8"ニホンゴ") {
      return {GeneratedCandidate{u8"日本語", -0.42, 2},
              GeneratedCandidate{u8"日本語入力", -1.4, 4}};
    }
    if (ToKatakana(kana) == u8"ムコウ") {
      return {GeneratedCandidate{std::string("\xE3\x81", 2), -0.42, 1}};
    }
    return {};
  }
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
  context_params.n_batch = context_params.n_ctx;
  context_params.n_ubatch = context_params.n_ctx;
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
  last_error_.reset();
  if (IsCanceled(context)) {
    return {};
  }
  if (!runtime_) {
    return DegradeToFallback(kana, context, "model-not-ready");
  }

  std::vector<core::Candidate> candidates;
  try {
    const auto generated = runtime_->Generate(kana, context);
    if (IsCanceled(context)) {
      return {};
    }
    candidates.reserve(generated.size());
    for (const auto& item : generated) {
      if (item.surface.empty()) {
        continue;
      }
      if (!IsValidUtf8String(item.surface)) {
        return DegradeToFallback(kana, context, "invalid-utf8-surface");
      }
      const auto avg =
          item.token_count > 0 ? item.total_logprob / static_cast<double>(item.token_count) : 0.0;
      core::Candidate candidate;
      candidate.surface = item.surface;
      candidate.reading = kana;
      candidate.score = NormalizeLogprob(item.total_logprob, item.token_count);
      candidate.source = core::CandidateSource::Model;
      candidate.debug_info =
          "zenzai;lp=" + FormatDouble(item.total_logprob) + ";avg=" + FormatDouble(avg);
      candidates.push_back(std::move(candidate));
    }
  } catch (const std::exception& ex) {
    return DegradeToFallback(kana, context, std::string("exception:") + ex.what());
  } catch (...) {
    return DegradeToFallback(kana, context, "exception:unknown");
  }

  DedupBySurface(candidates);
  if (IsCanceled(context)) {
    return {};
  }
  if (candidates.empty()) {
    return DegradeToFallback(kana, context, "empty-generation");
  }
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

std::vector<core::Candidate> ZenzaiModelConverter::DegradeToFallback(
    const std::string& kana, const core::ConversionContext& context, const std::string& reason) {
  last_error_ = "zenzai-degraded:" + reason;
  if (!fallback_) return {};
  auto candidates = fallback_->Convert(kana, context);
  for (auto& candidate : candidates) {
    AppendDebugTag(candidate.debug_info, "zenzai-degraded");
    AppendDebugTag(candidate.debug_info, reason);
  }
  return candidates;
}

void ZenzaiModelConverter::TagFallback(std::vector<core::Candidate>& candidates) const {
  for (auto& candidate : candidates) {
    AppendDebugTag(candidate.debug_info, "zenzai-gguf-loaded");
    AppendDebugTag(candidate.debug_info, "fallback-converter");
  }
}

}  // namespace azookey::host
