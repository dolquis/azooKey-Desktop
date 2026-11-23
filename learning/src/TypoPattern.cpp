#include "azookey/learning/TypoPattern.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <utility>

#include "azookey/core/Utf8.h"

namespace azookey::learning {

namespace {

constexpr std::string_view kInsertAfter = "_insert_after_";
constexpr std::string_view kDeleteAfter = "_delete_after_";
constexpr std::string_view kSubstitute = "_sub_";
constexpr std::string_view kSwap = "_swap";

constexpr std::array<std::string_view, 11> kTypoTypes = {
    "adjacent_key", "missing_char",      "extra_char",           "transposition",
    "double_key",   "romaji_variant",    "n_handling",           "small_tsu",
    "long_vowel",   "dakuten_confusion", "kana_shape_confusion",
};

// Decodes one code point at `offset`; false for an invalid sequence, so a
// pattern with a stray byte is rejected rather than reinterpreted.
bool TakeCodepoint(std::string_view text, size_t& offset, char32_t& codepoint) {
  return offset < text.size() && core::DecodeNextUtf8(text, offset, codepoint);
}

bool IsPrevious(char32_t ch) { return ch == kTypoPatternStart || IsTypoPatternCharacter(ch); }

bool IsValidToken(std::string_view token) {
  size_t offset = 0;
  char32_t first = 0;
  if (!TakeCodepoint(token, offset, first) || !IsTypoPatternCharacter(first)) return false;
  const std::string_view rest = token.substr(offset);

  // "<a><b>_swap"
  if (rest.size() > kSwap.size() && rest.substr(rest.size() - kSwap.size()) == kSwap) {
    const std::string_view middle = rest.substr(0, rest.size() - kSwap.size());
    size_t inner = 0;
    char32_t second = 0;
    return TakeCodepoint(middle, inner, second) && inner == middle.size() &&
           IsTypoPatternCharacter(second);
  }

  for (const auto& [keyword, previous_allowed] :
       {std::pair{kInsertAfter, true}, std::pair{kDeleteAfter, true},
        std::pair{kSubstitute, false}}) {
    if (rest.substr(0, keyword.size()) != keyword) continue;
    const std::string_view tail = rest.substr(keyword.size());
    size_t inner = 0;
    char32_t last = 0;
    if (!TakeCodepoint(tail, inner, last) || inner != tail.size()) return false;
    return previous_allowed ? IsPrevious(last) : IsTypoPatternCharacter(last);
  }
  return false;
}

double Sigmoid(double x) { return 1.0 / (1.0 + std::exp(-x)); }

}  // namespace

bool IsTypoPatternCharacter(char32_t ch) {
  // Hiragana U+3041-U+3096 and the prolonged sound mark U+30FC.
  return (ch >= U'a' && ch <= U'z') || ch == U'-' || ch == U'\'' ||
         (ch >= char32_t{0x3041} && ch <= char32_t{0x3096}) || ch == char32_t{0x30FC};
}

std::optional<std::string> AbstractTypoEdits(const std::vector<TypoEdit>& edits) {
  if (edits.empty() || edits.size() > kTypoPatternMaxEdits) return std::nullopt;
  std::string pattern;
  for (const auto& edit : edits) {
    if (!pattern.empty()) pattern.push_back('+');
    if (!IsTypoPatternCharacter(edit.first)) return std::nullopt;
    switch (edit.kind) {
      case TypoEditKind::Insertion:
      case TypoEditKind::Deletion:
        if (!IsPrevious(edit.previous)) return std::nullopt;
        core::AppendUtf8(pattern, edit.first);
        pattern.append(edit.kind == TypoEditKind::Insertion ? kInsertAfter : kDeleteAfter);
        core::AppendUtf8(pattern, edit.previous);
        break;
      case TypoEditKind::Substitution:
        if (!IsTypoPatternCharacter(edit.second)) return std::nullopt;
        core::AppendUtf8(pattern, edit.first);
        pattern.append(kSubstitute);
        core::AppendUtf8(pattern, edit.second);
        break;
      case TypoEditKind::Transposition:
        if (!IsTypoPatternCharacter(edit.second)) return std::nullopt;
        core::AppendUtf8(pattern, edit.first);
        core::AppendUtf8(pattern, edit.second);
        pattern.append(kSwap);
        break;
    }
  }
  if (!IsValidTypoPattern(pattern)) return std::nullopt;
  return pattern;
}

bool IsValidTypoPattern(std::string_view pattern) {
  if (pattern.empty() || pattern.size() > kTypoPatternMaxBytes) return false;
  size_t tokens = 0;
  size_t begin = 0;
  while (begin <= pattern.size()) {
    const size_t plus = pattern.find('+', begin);
    const size_t end = plus == std::string_view::npos ? pattern.size() : plus;
    if (++tokens > kTypoPatternMaxEdits || !IsValidToken(pattern.substr(begin, end - begin))) {
      return false;
    }
    if (plus == std::string_view::npos) break;
    begin = plus + 1;
  }
  return true;
}

bool IsKnownTypoType(std::string_view typo_type) {
  return std::find(kTypoTypes.begin(), kTypoTypes.end(), typo_type) != kTypoTypes.end();
}

double ComputeTypoConfidence(uint32_t accept_count, uint32_t reject_count,
                             std::optional<uint64_t> last_used_epoch_sec, uint64_t now_epoch_sec,
                             bool app_bonus_applies, const TypoConfidenceParams& params) {
  double recency_bonus = 0.0;
  if (last_used_epoch_sec) {
    const double idle_sec = now_epoch_sec > *last_used_epoch_sec
                                ? static_cast<double>(now_epoch_sec - *last_used_epoch_sec)
                                : 0.0;
    const double days_idle = idle_sec / 86400.0;
    // A true half-life: at days_idle == half_life the exponential is 0.5 and
    // the bonus is neutral.
    recency_bonus =
        params.recency_amplitude *
        (2.0 * std::exp(-std::log(2.0) * days_idle / params.recency_half_life_days) - 1.0);
  }
  const double logit = params.base + static_cast<double>(accept_count) * params.accept_weight -
                       static_cast<double>(reject_count) * params.reject_weight + recency_bonus +
                       (app_bonus_applies ? params.app_bonus : 0.0);
  return std::clamp(Sigmoid(logit), params.min_confidence, params.max_confidence);
}

}  // namespace azookey::learning
