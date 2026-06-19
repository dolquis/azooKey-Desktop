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
#include <optional>
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
    if (candidate.surface.empty() || !IsValidUtf8String(candidate.surface)) {
      continue;
    }
    if (std::find(seen_surfaces.begin(), seen_surfaces.end(), candidate.surface) !=
        seen_surfaces.end()) {
      continue;
    }
    seen_surfaces.push_back(candidate.surface);
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

bool DecodeTokens(llama_context* context, std::vector<llama_token>& tokens,
                  LlamaDecodeControl& control, const char* error_message) {
  if (tokens.empty()) {
    return true;
  }
  llama_kv_self_clear(context);
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
    auto batch = llama_batch_get_one(tokens.data() + offset, chunk_size);
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
};

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

void AppendCompletedBeam(std::vector<GeneratedCandidate>& generated, const LlamaBeam& beam) {
  if (!beam.surface.empty() && beam.output_tokens > 0) {
    generated.push_back(GeneratedCandidate{beam.surface, beam.total_logprob, beam.output_tokens});
  }
}

std::vector<llama_token> PromptWithBeamTokens(const std::vector<llama_token>& prompt_tokens,
                                              const LlamaBeam& beam) {
  std::vector<llama_token> tokens = prompt_tokens;
  tokens.insert(tokens.end(), beam.tokens.begin(), beam.tokens.end());
  return tokens;
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

    const size_t candidate_limit = RequestedCandidateLimit(conversion_context);
    const int32_t vocab_size = llama_vocab_n_tokens(vocab);
    const int32_t max_new = MaxNewTokensForReading(kana);
    LlamaDecodeControl decode_control{&conversion_context};
    LlamaDecodeAbortScope abort_scope(context, &decode_control);
    std::vector<GeneratedCandidate> generated;
    generated.reserve(candidate_limit);
    std::vector<LlamaBeam> beams(1);
    bool completed_quota_reached = false;

    for (int32_t step = 0; step < max_new && !beams.empty(); ++step) {
      std::vector<LlamaBeam> next_beams;
      next_beams.reserve(candidate_limit * candidate_limit);
      for (const auto& beam : beams) {
        if (IsCanceled(conversion_context)) {
          return {};
        }
        if (DeadlineExpired(conversion_context)) {
          AppendCompletedBeam(generated, beam);
          continue;
        }

        auto decoded_tokens = PromptWithBeamTokens(prompt_tokens, beam);
        if (!DecodeTokens(context, decoded_tokens, decode_control,
                          "llama.cpp beam decode failed")) {
          if (IsCanceled(conversion_context)) {
            return {};
          }
          AppendCompletedBeam(generated, beam);
          continue;
        }

        float* logits = llama_get_logits_ith(context, -1);
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
      beams = std::move(next_beams);
      if (CountSaneUniqueGeneratedCandidates(generated) >= candidate_limit) {
        completed_quota_reached = true;
        break;
      }
    }

    if (!completed_quota_reached) {
      for (const auto& beam : beams) {
        AppendCompletedBeam(generated, beam);
      }
    }
    std::sort(generated.begin(), generated.end(), [](const auto& lhs, const auto& rhs) {
      return BeamRankScore(lhs.total_logprob, lhs.token_count) >
             BeamRankScore(rhs.total_logprob, rhs.token_count);
    });

    return generated;
  }
#else
  bool mock_candidates_for_tests{false};

  std::vector<GeneratedCandidate> Generate(const std::string& kana,
                                           const core::ConversionContext& conversion_context) {
    if (IsCanceled(conversion_context)) {
      return {};
    }
    if (!mock_candidates_for_tests) {
      return {};
    }
    if (ToKatakana(kana) == u8"キャンセル") {
      // Test fixture: simulate scheduler cancellation after Convert has begun.
      if (conversion_context.cancel) {
        const_cast<std::atomic<bool>*>(conversion_context.cancel)->store(true,
                                                                         std::memory_order_relaxed);
      }
      return {};
    }
    if (ToKatakana(kana) == u8"ニホンゴ") {
      return {
          GeneratedCandidate{u8"日本語", -0.42, 2},
          GeneratedCandidate{u8"日本語入力", -1.4, 4},
          GeneratedCandidate{std::string("\xE3\x81", 2), -2.2, 1}};
    }
    if (ToKatakana(kana) == u8"セイン") {
      return {GeneratedCandidate{std::string("\xE3\x81", 2), -0.1, 1},
              GeneratedCandidate{u8"正しい", -1.0, 3}};
    }
    if (ToKatakana(kana) == u8"ジュウフク") {
      return {GeneratedCandidate{u8"重複", -0.2, 2},
              GeneratedCandidate{u8"重複", -0.4, 2},
              GeneratedCandidate{u8"別候補", -0.8, 3}};
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
      if (item.surface.empty()) {
        continue;
      }
      if (!IsValidUtf8String(item.surface)) {
        skipped_reason = "invalid-utf8-surface";
        continue;
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
