#include "azookey/host/ZenzaiModelConverter.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>

#ifndef AZOOKEY_WITH_LLAMA_CPP
#define AZOOKEY_WITH_LLAMA_CPP 0
#endif

#if AZOOKEY_WITH_LLAMA_CPP
#include <gguf.h>
#include <llama.h>
#endif

namespace azookey::host {

namespace {

constexpr std::array<char, 4> kGgufMagic{'G', 'G', 'U', 'F'};
constexpr std::string_view kGgufPreTokenizerKey = "tokenizer.ggml.pre";
constexpr std::string_view kGgufEosTokenIdKey = "tokenizer.ggml.eos_token_id";
#if AZOOKEY_WITH_LLAMA_CPP
constexpr std::string_view kGgufTokensKey = "tokenizer.ggml.tokens";
#endif
constexpr std::string_view kZenzaiPreTokenizer = "gpt2-small-japanese-char";
constexpr std::string_view kGpt2PreTokenizer = "gpt-2";
constexpr std::string_view kBosTokenPiece = "<s>";
constexpr std::string_view kEosTokenPiece = "</s>";
constexpr double kZenzaiScoreFloor = 0.3;
constexpr double kZenzaiScoreCeil = 1.4;
// Keep aligned with settings/mvp-settings.schema.json maxContextLength.maximum.
constexpr size_t kMaxLeftContextCodepoints = 30;
constexpr int32_t kMaxNewTokens = 64;
constexpr size_t kMaxModelCandidates = 4;

struct GeneratedCandidate {
  std::string surface;
  double total_logprob{};
  int32_t token_count{};
  bool allow_incomplete_utf8_suffix{};
};

size_t RequestedCandidateLimit(const core::ConversionContext& context) {
  if (context.live || context.max_candidates == 1) {
    return 1;
  }
  if (context.max_candidates > 0) {
    return std::min<size_t>(context.max_candidates, kMaxModelCandidates);
  }
  return kMaxModelCandidates;
}

uint32_t ReadLe32(const std::array<unsigned char, 4>& bytes) {
  return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
         (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}

#if AZOOKEY_WITH_LLAMA_CPP
class LlamaLogCapture {
 public:
  LlamaLogCapture() : lock_(CaptureMutex()) {
    std::call_once(LogCallbackOnce(), [] { llama_log_set(&CaptureLog, nullptr); });
    ActiveCapture() = this;
  }

  ~LlamaLogCapture() { ActiveCapture() = nullptr; }

  LlamaLogCapture(const LlamaLogCapture&) = delete;
  LlamaLogCapture& operator=(const LlamaLogCapture&) = delete;

  std::string error() const {
    std::lock_guard<std::mutex> lock(error_mutex_);
    const auto first = error_.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
      return {};
    }
    const auto last = error_.find_last_not_of(" \t\r\n");
    return error_.substr(first, last - first + 1);
  }

 private:
  static constexpr size_t kMaxCapturedErrorBytes = 4096;

  static std::mutex& CaptureMutex() {
    static std::mutex mutex;
    return mutex;
  }

  static std::once_flag& LogCallbackOnce() {
    static std::once_flag flag;
    return flag;
  }

  static LlamaLogCapture*& ActiveCapture() {
    thread_local LlamaLogCapture* capture = nullptr;
    return capture;
  }

  static void CaptureLog(ggml_log_level level, const char* text, void*) {
    if (!text) {
      return;
    }

    std::fputs(text, stderr);

    auto* capture = ActiveCapture();
    if (!capture) {
      return;
    }
    std::lock_guard<std::mutex> lock(capture->error_mutex_);
    if (level == GGML_LOG_LEVEL_ERROR) {
      capture->capturing_continuation_ = true;
    } else if (level != GGML_LOG_LEVEL_CONT) {
      capture->capturing_continuation_ = false;
    }
    if (level != GGML_LOG_LEVEL_ERROR &&
        !(level == GGML_LOG_LEVEL_CONT && capture->capturing_continuation_)) {
      return;
    }

    const size_t remaining = kMaxCapturedErrorBytes - capture->error_.size();
    capture->error_.append(text, std::min(remaining, std::char_traits<char>::length(text)));
  }

  std::unique_lock<std::mutex> lock_;
  mutable std::mutex error_mutex_;
  std::string error_;
  bool capturing_continuation_{false};
};

std::optional<ZenzaiTokenizerMetadata> ReadGgufTokenizerMetadata(const std::string& path) {
  const gguf_init_params params{
      /* .no_alloc = */ true,
      /* .ctx = */ nullptr,
  };
  std::unique_ptr<gguf_context, decltype(&gguf_free)> context(
      gguf_init_from_file(path.c_str(), params), &gguf_free);
  if (!context) {
    return std::nullopt;
  }

  ZenzaiTokenizerMetadata metadata;
  const auto pre_tokenizer_key = gguf_find_key(context.get(), kGgufPreTokenizerKey.data());
  if (pre_tokenizer_key >= 0 &&
      gguf_get_kv_type(context.get(), pre_tokenizer_key) == GGUF_TYPE_STRING) {
    const char* value = gguf_get_val_str(context.get(), pre_tokenizer_key);
    if (value) {
      metadata.pre_tokenizer = value;
    }
  }

  const auto eos_token_id_key = gguf_find_key(context.get(), kGgufEosTokenIdKey.data());
  if (eos_token_id_key >= 0 &&
      gguf_get_kv_type(context.get(), eos_token_id_key) == GGUF_TYPE_UINT32) {
    metadata.eos_token_id = gguf_get_val_u32(context.get(), eos_token_id_key);
  }

  const auto tokens_key = gguf_find_key(context.get(), kGgufTokensKey.data());
  if (tokens_key >= 0 && gguf_get_kv_type(context.get(), tokens_key) == GGUF_TYPE_ARRAY &&
      gguf_get_arr_type(context.get(), tokens_key) == GGUF_TYPE_STRING) {
    const auto token_count = gguf_get_arr_n(context.get(), tokens_key);
    metadata.vocabulary.reserve(token_count);
    for (size_t i = 0; i < token_count; ++i) {
      const char* token = gguf_get_arr_str(context.get(), tokens_key, i);
      metadata.vocabulary.emplace_back(token ? token : "");
    }
  }
  return metadata;
}
#endif

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

std::optional<size_t> CompleteUtf8PrefixLength(const std::string& input) {
  for (size_t offset = 0; offset < input.size();) {
    const size_t start = offset;
    char32_t codepoint = 0;
    if (DecodeNextUtf8(input, offset, codepoint)) {
      continue;
    }

    const auto first = static_cast<unsigned char>(input[start]);
    size_t width = 0;
    if ((first & 0xE0) == 0xC0) {
      width = 2;
    } else if ((first & 0xF0) == 0xE0) {
      width = 3;
    } else if ((first & 0xF8) == 0xF0) {
      width = 4;
    }
    if (width == 0 || start + width <= input.size()) {
      return std::nullopt;
    }
    for (size_t i = start + 1; i < input.size(); ++i) {
      if ((static_cast<unsigned char>(input[i]) & 0xC0) != 0x80) {
        return std::nullopt;
      }
    }
    return start;
  }
  return input.size();
}

std::optional<std::string> UsableGeneratedSurface(const GeneratedCandidate& candidate) {
  if (candidate.surface.empty()) {
    return std::nullopt;
  }
  if (IsValidUtf8String(candidate.surface)) {
    return candidate.surface;
  }
  if (!candidate.allow_incomplete_utf8_suffix) {
    return std::nullopt;
  }
  const auto prefix_length = CompleteUtf8PrefixLength(candidate.surface);
  if (!prefix_length || *prefix_length == 0 || *prefix_length >= candidate.surface.size()) {
    return std::nullopt;
  }
  return candidate.surface.substr(0, *prefix_length);
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

void SortAndLimitModelCandidates(std::vector<core::Candidate>& candidates, size_t limit) {
  std::stable_sort(candidates.begin(), candidates.end(),
                   [](const auto& lhs, const auto& rhs) { return lhs.score > rhs.score; });
  if (candidates.size() > limit) {
    candidates.resize(limit);
  }
}

#if AZOOKEY_WITH_LLAMA_CPP
size_t CountSaneUniqueGeneratedCandidates(const std::vector<GeneratedCandidate>& candidates) {
  std::vector<std::string> seen_surfaces;
  seen_surfaces.reserve(candidates.size());
  for (const auto& candidate : candidates) {
    const auto surface = UsableGeneratedSurface(candidate);
    if (!surface) {
      continue;
    }
    if (std::find(seen_surfaces.begin(), seen_surfaces.end(), *surface) != seen_surfaces.end()) {
      continue;
    }
    seen_surfaces.push_back(*surface);
  }
  return seen_surfaces.size();
}

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

struct LlamaDecodeControl {
  const core::ConversionContext* conversion_context{};

  bool ShouldAbort() const {
    return conversion_context &&
           (IsCanceled(*conversion_context) || DeadlineExpired(*conversion_context));
  }
};

bool LlamaDecodeAbortCallback(void* data) {
  auto* control = static_cast<LlamaDecodeControl*>(data);
  return control && control->ShouldAbort();
}

class LlamaDecodeAbortScope {
 public:
  LlamaDecodeAbortScope(llama_context* context, LlamaDecodeControl* control) : context_(context) {
    llama_set_abort_callback(context_, LlamaDecodeAbortCallback, control);
  }

  ~LlamaDecodeAbortScope() { llama_set_abort_callback(context_, nullptr, nullptr); }

  LlamaDecodeAbortScope(const LlamaDecodeAbortScope&) = delete;
  LlamaDecodeAbortScope& operator=(const LlamaDecodeAbortScope&) = delete;

 private:
  llama_context* context_;
};

bool DecodeTokens(llama_context* context, const std::vector<llama_token>& tokens,
                  LlamaDecodeControl& control, const char* error_message) {
  if (tokens.empty()) {
    return true;
  }
  const auto context_batch = std::max<uint32_t>(1, llama_n_batch(context));
  const int32_t chunk_limit =
      std::max<int32_t>(1, std::min<int32_t>(32, static_cast<int32_t>(context_batch)));
  for (size_t offset = 0; offset < tokens.size();) {
    if (control.ShouldAbort()) {
      return false;
    }
    const auto remaining = tokens.size() - offset;
    const int32_t chunk_size =
        static_cast<int32_t>(std::min<size_t>(remaining, static_cast<size_t>(chunk_limit)));
    // llama_batch_get_one stores this pointer but llama_decode only reads token IDs.
    auto* chunk_tokens = const_cast<llama_token*>(tokens.data() + offset);
    auto batch = llama_batch_get_one(chunk_tokens, chunk_size);
    const int32_t result = llama_decode(context, batch);
    if (result == 2 || control.ShouldAbort()) {
      return false;
    }
    if (result != 0) {
      throw std::runtime_error(error_message);
    }
    offset += static_cast<size_t>(chunk_size);
  }
  return true;
}

struct LlamaBeam {
  std::vector<llama_token> tokens;
  std::string surface;
  double total_logprob{};
  int32_t output_tokens{};
  int32_t sequence_id{};
  int32_t parent_sequence_id{};
};

bool DecodeBeamStep(llama_context* context, const std::vector<LlamaBeam>& beams, llama_pos position,
                    LlamaDecodeControl& control) {
  if (beams.empty()) {
    return true;
  }

  struct BatchGuard {
    llama_batch batch;
    ~BatchGuard() { llama_batch_free(batch); }
  } batch_guard{llama_batch_init(static_cast<int32_t>(beams.size()), 0, 1)};
  auto& batch = batch_guard.batch;
  if (!batch.token || !batch.pos || !batch.n_seq_id || !batch.seq_id || !batch.logits) {
    throw std::bad_alloc();
  }
  batch.n_tokens = static_cast<int32_t>(beams.size());
  for (int32_t i = 0; i < batch.n_tokens; ++i) {
    const auto& beam = beams[static_cast<size_t>(i)];
    batch.token[i] = beam.tokens.back();
    batch.pos[i] = position;
    batch.n_seq_id[i] = 1;
    batch.seq_id[i][0] = beam.sequence_id;
    batch.logits[i] = 1;
  }

  if (control.ShouldAbort()) {
    return false;
  }
  const int32_t result = llama_decode(context, batch);
  if (result == 2 || control.ShouldAbort()) {
    return false;
  }
  if (result != 0) {
    throw std::runtime_error("llama.cpp beam batch decode failed");
  }
  return true;
}

double BeamRankScore(double total_logprob, int32_t output_tokens) {
  if (output_tokens <= 0) {
    return -std::numeric_limits<double>::infinity();
  }
  return total_logprob / static_cast<double>(output_tokens);
}

void PruneBeams(std::vector<LlamaBeam>& beams, size_t max_beams) {
  std::sort(beams.begin(), beams.end(), [](const auto& lhs, const auto& rhs) {
    return BeamRankScore(lhs.total_logprob, lhs.output_tokens) >
           BeamRankScore(rhs.total_logprob, rhs.output_tokens);
  });
  if (beams.size() > max_beams) {
    beams.resize(max_beams);
  }
}

void AppendCompletedBeam(std::vector<GeneratedCandidate>& generated, const LlamaBeam& beam,
                         bool allow_incomplete_utf8_suffix = false) {
  if (!beam.surface.empty() && beam.output_tokens > 0) {
    generated.push_back(GeneratedCandidate{beam.surface, beam.total_logprob, beam.output_tokens,
                                           allow_incomplete_utf8_suffix});
  }
}

#endif

}  // namespace

struct ZenzaiModelRuntime {
  std::optional<ZenzaiDecodeStats> last_decode_stats;
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
  std::vector<llama_token> cached_prompt_tokens;

  void InvalidatePromptCache() {
    cached_prompt_tokens.clear();
    if (context) {
      llama_memory_clear(llama_get_memory(context), false);
    }
  }

  std::vector<llama_token> TokenizePrompt(const std::string& kana,
                                          const core::ConversionContext& conversion_context) const {
    if (!model) {
      throw std::runtime_error("llama.cpp model is not ready");
    }
    const auto* vocab = llama_model_get_vocab(model);
    if (!vocab) {
      throw std::runtime_error("llama.cpp vocab is not available");
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
    return prompt_tokens;
  }

  std::vector<GeneratedCandidate> Generate(const std::string& kana,
                                           const core::ConversionContext& conversion_context) {
    last_decode_stats.reset();
    if (!model || !context) {
      throw std::runtime_error("llama.cpp runtime is not ready");
    }

    struct PromptCacheGuard {
      ZenzaiModelRuntime* runtime;
      bool preserve{false};
      ~PromptCacheGuard() {
        if (!preserve) {
          runtime->InvalidatePromptCache();
        }
      }
    } prompt_cache_guard{this};

    const auto* vocab = llama_model_get_vocab(model);
    if (!vocab) {
      throw std::runtime_error("llama.cpp vocab is not available");
    }
    if (IsCanceled(conversion_context)) {
      return {};
    }

    auto prompt_tokens = TokenizePrompt(kana, conversion_context);

    const size_t candidate_limit = RequestedCandidateLimit(conversion_context);
    const int32_t vocab_size = llama_vocab_n_tokens(vocab);
    const int32_t max_new = MaxNewTokensForReading(kana);
    LlamaDecodeControl decode_control{&conversion_context};
    LlamaDecodeAbortScope abort_scope(context, &decode_control);
    std::vector<GeneratedCandidate> generated;
    generated.reserve(candidate_limit);
    std::vector<LlamaBeam> beams(1);
    bool completed_quota_reached = false;

    ZenzaiDecodeStats decode_stats;
    constexpr int32_t kMaxWorkingSequences = 4;
    for (int32_t sequence = 1; sequence <= kMaxWorkingSequences; ++sequence) {
      (void)llama_memory_seq_rm(llama_get_memory(context), sequence, -1, -1);
    }
    size_t retained_prompt_tokens = 0;
    if (!cached_prompt_tokens.empty()) {
      retained_prompt_tokens = CommonPrefixLength(cached_prompt_tokens, prompt_tokens);
      // llama.cpp does not retain logits for an arbitrary cached prefix. Re-decode at least the
      // final prompt token so step zero observes logits for the current prompt.
      retained_prompt_tokens =
          std::min(retained_prompt_tokens, prompt_tokens.empty() ? 0u : prompt_tokens.size() - 1);
      if (!llama_memory_seq_rm(llama_get_memory(context), 0,
                               static_cast<llama_pos>(retained_prompt_tokens), -1)) {
        throw std::runtime_error("llama.cpp prompt KV suffix reset failed");
      }
    } else {
      llama_memory_clear(llama_get_memory(context), false);
    }
    std::vector<llama_token> prompt_suffix(prompt_tokens.begin() + retained_prompt_tokens,
                                           prompt_tokens.end());
    decode_stats.prompt_tokens = static_cast<uint64_t>(prompt_suffix.size());
    decode_stats.prompt_reused_tokens = static_cast<uint64_t>(retained_prompt_tokens);
    const auto prompt_decode_start = std::chrono::steady_clock::now();
    const bool prompt_decoded =
        DecodeTokens(context, prompt_suffix, decode_control, "llama.cpp prompt decode failed");
    decode_stats.prompt_decode_ms = std::chrono::duration<double, std::milli>(
                                        std::chrono::steady_clock::now() - prompt_decode_start)
                                        .count();
    if (!prompt_decoded) {
      if (DeadlineExpired(conversion_context)) {
        decode_stats.deadline_exceeded = true;
        last_decode_stats = decode_stats;
      }
      return {};
    }
    cached_prompt_tokens = prompt_tokens;
    const auto prompt_size = static_cast<llama_pos>(prompt_tokens.size());

    for (int32_t step = 0; step < max_new && !beams.empty(); ++step) {
      if (IsCanceled(conversion_context)) {
        return {};
      }
      if (DeadlineExpired(conversion_context)) {
        decode_stats.deadline_exceeded = true;
        for (const auto& beam : beams) {
          AppendCompletedBeam(generated, beam, true);
        }
        beams.clear();
        break;
      }

      if (step > 0) {
        const auto beam_decode_start = std::chrono::steady_clock::now();
        const bool beam_decoded =
            DecodeBeamStep(context, beams, prompt_size + step - 1, decode_control);
        decode_stats.beam_decode_ms += std::chrono::duration<double, std::milli>(
                                           std::chrono::steady_clock::now() - beam_decode_start)
                                           .count();
        decode_stats.beam_tokens += static_cast<uint64_t>(beams.size());
        ++decode_stats.beam_decode_evaluations;
        if (!beam_decoded) {
          if (IsCanceled(conversion_context)) {
            return {};
          }
          decode_stats.deadline_exceeded = true;
          for (const auto& beam : beams) {
            AppendCompletedBeam(generated, beam, true);
          }
          beams.clear();
          break;
        }
      }

      std::vector<LlamaBeam> next_beams;
      next_beams.reserve(candidate_limit * candidate_limit);
      for (size_t beam_index = 0; beam_index < beams.size(); ++beam_index) {
        const auto& beam = beams[beam_index];
        float* logits =
            llama_get_logits_ith(context, step == 0 ? -1 : static_cast<int32_t>(beam_index));
        if (!logits) {
          throw std::runtime_error("llama.cpp logits are not available");
        }

        auto choices = CollectTokenChoices(vocab, logits, vocab_size, candidate_limit);
        if (choices.empty()) {
          throw std::runtime_error("llama.cpp did not select a token");
        }

        for (const auto& choice : choices) {
          if (llama_vocab_is_eog(vocab, choice.token)) {
            AppendCompletedBeam(generated, beam);
            continue;
          }

          auto next = beam;
          next.parent_sequence_id = beam.sequence_id;
          next.tokens.push_back(choice.token);
          next.surface += TokenPiece(vocab, choice.token);
          next.total_logprob += choice.logprob;
          ++next.output_tokens;
          if (!next.surface.empty()) {
            next_beams.push_back(std::move(next));
          }
        }
      }

      PruneBeams(next_beams, candidate_limit);
      std::vector<int32_t> parent_sequences;
      parent_sequences.reserve(next_beams.size());
      for (const auto& beam : next_beams) {
        parent_sequences.push_back(beam.parent_sequence_id);
      }
      std::vector<int32_t> active_sequences;
      active_sequences.reserve(beams.size());
      for (const auto& beam : beams) {
        if (beam.sequence_id > 0) {
          active_sequences.push_back(beam.sequence_id);
        }
      }
      const auto sequence_plan =
          PlanBeamSequenceAssignments(parent_sequences, active_sequences, kMaxWorkingSequences);
      for (const int32_t sequence : sequence_plan.releases) {
        (void)llama_memory_seq_rm(llama_get_memory(context), sequence, -1, -1);
      }
      for (const auto& copy : sequence_plan.copies) {
        llama_memory_seq_cp(llama_get_memory(context), copy.source_sequence,
                            copy.destination_sequence, -1, -1);
      }
      for (size_t i = 0; i < next_beams.size(); ++i) {
        next_beams[i].sequence_id = sequence_plan.assignments[i];
      }
      beams = std::move(next_beams);
      if (CountSaneUniqueGeneratedCandidates(generated) >= candidate_limit) {
        completed_quota_reached = true;
        break;
      }
    }

    if (!completed_quota_reached) {
      for (const auto& beam : beams) {
        AppendCompletedBeam(generated, beam, true);
      }
    }
    std::sort(generated.begin(), generated.end(), [](const auto& lhs, const auto& rhs) {
      return BeamRankScore(lhs.total_logprob, lhs.token_count) >
             BeamRankScore(rhs.total_logprob, rhs.token_count);
    });

    last_decode_stats = decode_stats;
    prompt_cache_guard.preserve = true;
    return generated;
  }
#else
  bool mock_candidates_for_tests{false};

  std::vector<GeneratedCandidate> Generate(const std::string& kana,
                                           const core::ConversionContext& conversion_context) {
    last_decode_stats.reset();
    if (IsCanceled(conversion_context)) {
      return {};
    }
    if (!mock_candidates_for_tests) {
      return {};
    }
    if (ToKatakana(kana) == "キャンセル") {
      // Test fixture: simulate scheduler cancellation after Convert has begun.
      if (conversion_context.cancel) {
        const_cast<std::atomic<bool>*>(conversion_context.cancel)
            ->store(true, std::memory_order_relaxed);
      }
      return {};
    }
    if (ToKatakana(kana) == "ニホンゴ") {
      return {GeneratedCandidate{"日本語", -0.42, 2}, GeneratedCandidate{"日本語入力", -1.4, 4},
              GeneratedCandidate{std::string("\xE3\x81", 2), -2.2, 1}};
    }
    if (ToKatakana(kana) == "セイン") {
      return {GeneratedCandidate{std::string("\xE3\x81", 2), -0.1, 1},
              GeneratedCandidate{"正しい", -1.0, 3}};
    }
    if (ToKatakana(kana) == "ジュウフク") {
      return {GeneratedCandidate{"重複", -0.2, 2}, GeneratedCandidate{"重複", -0.4, 2},
              GeneratedCandidate{"別候補", -0.8, 3}};
    }
    if (ToKatakana(kana) == "ムコウ") {
      return {GeneratedCandidate{std::string("\xE3\x81", 2), -0.42, 1}};
    }
    if (ToKatakana(kana) == "チュウダン") {
      ZenzaiDecodeStats decode_stats;
      decode_stats.deadline_exceeded = true;
      last_decode_stats = decode_stats;
      return {
          GeneratedCandidate{std::string("日本語") + std::string("\xE3\x81", 2), -0.42, 3, true}};
    }
    if (ToKatakana(kana) == "ナイブムコウ") {
      return {GeneratedCandidate{std::string("日") + std::string("\xE3X", 2), -0.42, 2, true}};
    }
    return {};
  }
#endif
};

ZenzaiLoadResult::ZenzaiLoadResult() = default;
ZenzaiLoadResult::~ZenzaiLoadResult() = default;
ZenzaiLoadResult::ZenzaiLoadResult(ZenzaiLoadResult&&) noexcept = default;
ZenzaiLoadResult& ZenzaiLoadResult::operator=(ZenzaiLoadResult&&) noexcept = default;

int32_t RecommendedZenzaiThreadCount(uint32_t hardware_threads) {
  constexpr uint32_t kMaximumThreads = 8;
  return static_cast<int32_t>(std::clamp(hardware_threads, 1u, kMaximumThreads));
}

size_t CommonPrefixLength(const std::vector<int32_t>& lhs, const std::vector<int32_t>& rhs) {
  const auto mismatch = std::mismatch(lhs.begin(), lhs.end(), rhs.begin(), rhs.end());
  return static_cast<size_t>(std::distance(lhs.begin(), mismatch.first));
}

BeamSequencePlan PlanBeamSequenceAssignments(const std::vector<int32_t>& parent_sequences,
                                             const std::vector<int32_t>& active_sequences,
                                             int32_t max_working_sequences) {
  if (max_working_sequences <= 0 ||
      parent_sequences.size() > static_cast<size_t>(max_working_sequences)) {
    throw std::invalid_argument("invalid beam sequence capacity");
  }

  std::vector<bool> active(static_cast<size_t>(max_working_sequences) + 1, false);
  for (const int32_t sequence : active_sequences) {
    if (sequence <= 0 || sequence > max_working_sequences || active[sequence]) {
      throw std::invalid_argument("invalid active beam sequence");
    }
    active[sequence] = true;
  }

  BeamSequencePlan plan;
  plan.assignments.assign(parent_sequences.size(), 0);
  std::vector<bool> retained(static_cast<size_t>(max_working_sequences) + 1, false);
  for (size_t i = 0; i < parent_sequences.size(); ++i) {
    const int32_t parent = parent_sequences[i];
    if (parent < 0 || parent > max_working_sequences || (parent > 0 && !active[parent])) {
      throw std::invalid_argument("invalid parent beam sequence");
    }
    if (parent > 0 && !retained[parent]) {
      plan.assignments[i] = parent;
      retained[parent] = true;
    }
  }

  for (int32_t sequence = 1; sequence <= max_working_sequences; ++sequence) {
    if (active[sequence] && !retained[sequence]) {
      plan.releases.push_back(sequence);
    }
  }

  int32_t next_free = 1;
  for (size_t i = 0; i < plan.assignments.size(); ++i) {
    if (plan.assignments[i] != 0) {
      continue;
    }
    while (next_free <= max_working_sequences && retained[next_free]) {
      ++next_free;
    }
    if (next_free > max_working_sequences) {
      throw std::logic_error("beam sequence planner exhausted capacity");
    }
    plan.assignments[i] = next_free;
    retained[next_free] = true;
    plan.copies.push_back(BeamSequenceCopy{parent_sequences[i], next_free});
  }
  return plan;
}

std::optional<std::string_view> ResolveZenzaiPreTokenizerOverride(std::string_view pre_tokenizer) {
  if (pre_tokenizer == kZenzaiPreTokenizer) {
    return kGpt2PreTokenizer;
  }
  return std::nullopt;
}

std::optional<uint32_t> ResolveZenzaiEosTokenOverride(uint32_t declared_eos_token_id,
                                                      const std::vector<std::string>& vocabulary) {
  if (declared_eos_token_id >= vocabulary.size() ||
      vocabulary[declared_eos_token_id] != kBosTokenPiece) {
    return std::nullopt;
  }

  const auto eos = std::find(vocabulary.begin(), vocabulary.end(), kEosTokenPiece);
  if (eos == vocabulary.end()) {
    return std::nullopt;
  }
  const auto eos_token_id = static_cast<size_t>(std::distance(vocabulary.begin(), eos));
  if (eos_token_id > std::numeric_limits<uint32_t>::max()) {
    return std::nullopt;
  }
  return static_cast<uint32_t>(eos_token_id);
}

std::vector<ZenzaiKvOverride> BuildZenzaiKvOverrides(const ZenzaiTokenizerMetadata& metadata) {
  std::vector<ZenzaiKvOverride> overrides;

  const auto pre_tokenizer_override =
      metadata.pre_tokenizer ? ResolveZenzaiPreTokenizerOverride(*metadata.pre_tokenizer)
                             : std::nullopt;
  if (pre_tokenizer_override) {
    ZenzaiKvOverride entry;
    entry.key = kGgufPreTokenizerKey;
    entry.type = ZenzaiKvOverride::Type::String;
    entry.string_value = *pre_tokenizer_override;
    overrides.push_back(std::move(entry));
  }

  const auto eos_token_override =
      metadata.eos_token_id
          ? ResolveZenzaiEosTokenOverride(*metadata.eos_token_id, metadata.vocabulary)
          : std::nullopt;
  if (eos_token_override) {
    ZenzaiKvOverride entry;
    entry.key = kGgufEosTokenIdKey;
    entry.type = ZenzaiKvOverride::Type::Int;
    entry.int_value = *eos_token_override;
    overrides.push_back(std::move(entry));
  }

  return overrides;
}

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
  LlamaLogCapture log_capture;

  auto model_params = llama_model_default_params();
  model_params.n_gpu_layers = options.n_gpu_layers;
  const auto tokenizer_metadata = ReadGgufTokenizerMetadata(path);
  const auto described_overrides = tokenizer_metadata ? BuildZenzaiKvOverrides(*tokenizer_metadata)
                                                      : std::vector<ZenzaiKvOverride>{};
  // llama.cpp reads the array until it hits an entry with an empty key, so keep a zeroed tail.
  std::vector<llama_model_kv_override> kv_overrides(described_overrides.size() + 1);
  for (size_t i = 0; i < described_overrides.size(); ++i) {
    const auto& described = described_overrides[i];
    auto& entry = kv_overrides[i];
    std::snprintf(entry.key, sizeof(entry.key), "%.*s", static_cast<int>(described.key.size()),
                  described.key.data());
    switch (described.type) {
      case ZenzaiKvOverride::Type::String:
        entry.tag = LLAMA_KV_OVERRIDE_TYPE_STR;
        std::snprintf(entry.val_str, sizeof(entry.val_str), "%.*s",
                      static_cast<int>(described.string_value.size()),
                      described.string_value.data());
        break;
      case ZenzaiKvOverride::Type::Int:
        entry.tag = LLAMA_KV_OVERRIDE_TYPE_INT;
        entry.val_i64 = described.int_value;
        break;
    }
  }
  if (!described_overrides.empty()) {
    model_params.kv_overrides = kv_overrides.data();
  }
  runtime->model = llama_model_load_from_file(path.c_str(), model_params);
  if (!runtime->model) {
    result.ok = false;
    result.error = "llama.cpp model load failed";
    const auto detail = log_capture.error();
    if (!detail.empty()) {
      result.error += ": " + detail;
    }
    return result;
  }

  auto context_params = llama_context_default_params();
  context_params.n_ctx = 512;
  context_params.n_batch = context_params.n_ctx;
  context_params.n_ubatch = context_params.n_ctx;
  context_params.n_seq_max = 5;      // prompt sequence 0 plus working beam sequences 1..4
  context_params.kv_unified = true;  // beam sequences share the decoded prompt prefix
  const auto n_threads =
      options.n_threads.value_or(RecommendedZenzaiThreadCount(std::thread::hardware_concurrency()));
  context_params.n_threads = n_threads;
  context_params.n_threads_batch = n_threads;
  runtime->context = llama_init_from_model(runtime->model, context_params);
  if (!runtime->context) {
    result.ok = false;
    result.error = "llama.cpp context creation failed";
    const auto detail = log_capture.error();
    if (!detail.empty()) {
      result.error += ": " + detail;
    }
    return result;
  }

  result.runtime = std::move(runtime);
#else
  auto runtime = std::make_unique<ZenzaiModelRuntime>();
  runtime->mock_candidates_for_tests = options.mock_candidates_for_tests;
  result.runtime = std::move(runtime);
#endif
  result.ok = true;
  result.error.clear();
  return result;
}

ZenzaiModelConverter::ZenzaiModelConverter(ZenzaiLoadResult&& loaded, core::IConverter* fallback)
    : info_(std::move(loaded.info)), runtime_(std::move(loaded.runtime)), fallback_(fallback) {}

ZenzaiModelConverter::~ZenzaiModelConverter() = default;

std::optional<ZenzaiDecodeStats> ZenzaiModelConverter::last_decode_stats() const {
  return runtime_ ? runtime_->last_decode_stats : std::nullopt;
}

std::vector<int32_t> ZenzaiModelConverter::TokenizePromptForValidation(
    const std::string& kana, const core::ConversionContext& context) const {
#if AZOOKEY_WITH_LLAMA_CPP
  if (!runtime_) {
    throw std::runtime_error("llama.cpp runtime is not ready");
  }
  const auto tokens = runtime_->TokenizePrompt(kana, context);
  return {tokens.begin(), tokens.end()};
#else
  (void)kana;
  (void)context;
  throw std::runtime_error("prompt token validation requires llama.cpp");
#endif
}

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
  std::optional<std::string> skipped_reason;
  try {
    const auto generated = runtime_->Generate(kana, context);
    if (IsCanceled(context)) {
      return {};
    }
    candidates.reserve(generated.size());
    for (const auto& item : generated) {
      const auto surface = UsableGeneratedSurface(item);
      if (!surface) {
        skipped_reason = "invalid-utf8-surface";
        continue;
      }
      const bool trimmed_incomplete_utf8_suffix = surface->size() != item.surface.size();
      // Keep the full decoding-path cost. Dropping the partial token's logprob would
      // overstate the probability of the prefix that is safe to display.
      const auto avg =
          item.token_count > 0 ? item.total_logprob / static_cast<double>(item.token_count) : 0.0;
      core::Candidate candidate;
      candidate.surface = *surface;
      candidate.reading = kana;
      candidate.score = NormalizeLogprob(item.total_logprob, item.token_count);
      candidate.source = core::CandidateSource::Model;
      candidate.debug_info =
          "zenzai;lp=" + FormatDouble(item.total_logprob) + ";avg=" + FormatDouble(avg);
      if (trimmed_incomplete_utf8_suffix) {
        AppendDebugTag(candidate.debug_info, "utf8-prefix-trimmed");
      }
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
    return DegradeToFallback(kana, context, skipped_reason.value_or("empty-generation"));
  }
  SortAndLimitModelCandidates(candidates, RequestedCandidateLimit(context));
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
