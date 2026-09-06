#include "azookey/core/KatakanaRewriter.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "azookey/core/Utf8.h"

namespace azookey::core {
namespace {

std::string_view HalfwidthKatakana(char32_t codepoint) {
  switch (codepoint) {
    case U'ァ':
      return "ｧ";
    case U'ア':
      return "ｱ";
    case U'ィ':
      return "ｨ";
    case U'イ':
      return "ｲ";
    case U'ゥ':
      return "ｩ";
    case U'ウ':
      return "ｳ";
    case U'ェ':
      return "ｪ";
    case U'エ':
      return "ｴ";
    case U'ォ':
      return "ｫ";
    case U'オ':
      return "ｵ";
    case U'カ':
      return "ｶ";
    case U'ガ':
      return "ｶﾞ";
    case U'キ':
      return "ｷ";
    case U'ギ':
      return "ｷﾞ";
    case U'ク':
      return "ｸ";
    case U'グ':
      return "ｸﾞ";
    case U'ケ':
      return "ｹ";
    case U'ゲ':
      return "ｹﾞ";
    case U'コ':
      return "ｺ";
    case U'ゴ':
      return "ｺﾞ";
    case U'サ':
      return "ｻ";
    case U'ザ':
      return "ｻﾞ";
    case U'シ':
      return "ｼ";
    case U'ジ':
      return "ｼﾞ";
    case U'ス':
      return "ｽ";
    case U'ズ':
      return "ｽﾞ";
    case U'セ':
      return "ｾ";
    case U'ゼ':
      return "ｾﾞ";
    case U'ソ':
      return "ｿ";
    case U'ゾ':
      return "ｿﾞ";
    case U'タ':
      return "ﾀ";
    case U'ダ':
      return "ﾀﾞ";
    case U'チ':
      return "ﾁ";
    case U'ヂ':
      return "ﾁﾞ";
    case U'ッ':
      return "ｯ";
    case U'ツ':
      return "ﾂ";
    case U'ヅ':
      return "ﾂﾞ";
    case U'テ':
      return "ﾃ";
    case U'デ':
      return "ﾃﾞ";
    case U'ト':
      return "ﾄ";
    case U'ド':
      return "ﾄﾞ";
    case U'ナ':
      return "ﾅ";
    case U'ニ':
      return "ﾆ";
    case U'ヌ':
      return "ﾇ";
    case U'ネ':
      return "ﾈ";
    case U'ノ':
      return "ﾉ";
    case U'ハ':
      return "ﾊ";
    case U'バ':
      return "ﾊﾞ";
    case U'パ':
      return "ﾊﾟ";
    case U'ヒ':
      return "ﾋ";
    case U'ビ':
      return "ﾋﾞ";
    case U'ピ':
      return "ﾋﾟ";
    case U'フ':
      return "ﾌ";
    case U'ブ':
      return "ﾌﾞ";
    case U'プ':
      return "ﾌﾟ";
    case U'ヘ':
      return "ﾍ";
    case U'ベ':
      return "ﾍﾞ";
    case U'ペ':
      return "ﾍﾟ";
    case U'ホ':
      return "ﾎ";
    case U'ボ':
      return "ﾎﾞ";
    case U'ポ':
      return "ﾎﾟ";
    case U'マ':
      return "ﾏ";
    case U'ミ':
      return "ﾐ";
    case U'ム':
      return "ﾑ";
    case U'メ':
      return "ﾒ";
    case U'モ':
      return "ﾓ";
    case U'ャ':
      return "ｬ";
    case U'ヤ':
      return "ﾔ";
    case U'ュ':
      return "ｭ";
    case U'ユ':
      return "ﾕ";
    case U'ョ':
      return "ｮ";
    case U'ヨ':
      return "ﾖ";
    case U'ラ':
      return "ﾗ";
    case U'リ':
      return "ﾘ";
    case U'ル':
      return "ﾙ";
    case U'レ':
      return "ﾚ";
    case U'ロ':
      return "ﾛ";
    case U'ワ':
      return "ﾜ";
    case U'ヲ':
      return "ｦ";
    case U'ン':
      return "ﾝ";
    case U'ヴ':
      return "ｳﾞ";
    case U'ー':
      return "ｰ";
    default:
      return {};
  }
}

Candidate MakeCandidate(const std::string& reading, std::string surface, std::string description,
                        std::string debug_tag) {
  Candidate candidate;
  candidate.surface = std::move(surface);
  candidate.reading = reading;
  candidate.score = 0.05;
  candidate.source = CandidateSource::Heuristic;
  candidate.debug_info = "katakana-rewriter:" + std::move(debug_tag);
  candidate.description = std::move(description);
  return candidate;
}

}  // namespace

std::vector<Candidate> ExpandKatakanaCandidates(const std::string& reading) {
  if (reading.empty()) return {};

  std::string fullwidth;
  std::string halfwidth;
  fullwidth.reserve(reading.size());
  halfwidth.reserve(reading.size());
  bool has_hiragana = false;
  bool halfwidth_supported = true;
  for (size_t offset = 0; offset < reading.size();) {
    char32_t codepoint = 0;
    if (!DecodeNextUtf8(reading, offset, codepoint)) return {};
    if (codepoint == U'ー') {
      AppendUtf8(fullwidth, codepoint);
      if (halfwidth_supported) halfwidth += HalfwidthKatakana(codepoint);
      continue;
    }
    // Iteration marks and combining dakuten are above U+3096, so this range
    // check also excludes them as required by spec §18.2.
    if (codepoint < U'ぁ' || codepoint > U'ゖ') {
      return {};
    }
    has_hiragana = true;
    const char32_t katakana = codepoint + 0x60;
    AppendUtf8(fullwidth, katakana);
    const auto mapped = HalfwidthKatakana(katakana);
    if (mapped.empty()) {
      halfwidth_supported = false;
    } else if (halfwidth_supported) {
      halfwidth += mapped;
    }
  }
  if (!has_hiragana) return {};

  std::vector<Candidate> candidates;
  candidates.reserve(2);
  candidates.push_back(MakeCandidate(reading, std::move(fullwidth), "全角カタカナ", "fullwidth"));
  if (halfwidth_supported) {
    candidates.push_back(MakeCandidate(reading, std::move(halfwidth), "半角カタカナ", "halfwidth"));
  }
  return candidates;
}

}  // namespace azookey::core
