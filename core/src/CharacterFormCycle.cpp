#include "azookey/core/CharacterFormCycle.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "azookey/core/KatakanaRewriter.h"
#include "azookey/core/Utf8.h"

namespace azookey::core {
namespace {

using KanaMapping = std::pair<std::u32string_view, std::string_view>;
using ScalarMapping = std::pair<char32_t, std::string_view>;

// Use the same canonical spellings as RomajiKanaConverter for its supported
// syllables. The extra small kana permit a complete form for selected text.
constexpr KanaMapping kDigraphs[] = {
    {U"きゃ", "kya"}, {U"きゅ", "kyu"}, {U"きょ", "kyo"}, {U"しゃ", "sha"}, {U"しゅ", "shu"},
    {U"しょ", "sho"}, {U"ちゃ", "cha"}, {U"ちゅ", "chu"}, {U"ちょ", "cho"}, {U"にゃ", "nya"},
    {U"にゅ", "nyu"}, {U"にょ", "nyo"}, {U"ひゃ", "hya"}, {U"ひゅ", "hyu"}, {U"ひょ", "hyo"},
    {U"みゃ", "mya"}, {U"みゅ", "myu"}, {U"みょ", "myo"}, {U"りゃ", "rya"}, {U"りゅ", "ryu"},
    {U"りょ", "ryo"}, {U"ぎゃ", "gya"}, {U"ぎゅ", "gyu"}, {U"ぎょ", "gyo"}, {U"じゃ", "ja"},
    {U"じゅ", "ju"},  {U"じぇ", "je"},  {U"じょ", "jo"},  {U"ぢゃ", "dya"}, {U"ぢゅ", "dyu"},
    {U"ぢょ", "dyo"}, {U"びゃ", "bya"}, {U"びゅ", "byu"}, {U"びょ", "byo"}, {U"ぴゃ", "pya"},
    {U"ぴゅ", "pyu"}, {U"ぴょ", "pyo"},
};

constexpr ScalarMapping kScalars[] = {
    {U'ぁ', "a"},  {U'あ', "a"},  {U'ぃ', "i"},  {U'い', "i"},   {U'ぅ', "u"},   {U'う', "u"},
    {U'ぇ', "e"},  {U'え', "e"},  {U'ぉ', "o"},  {U'お', "o"},   {U'か', "ka"},  {U'き', "ki"},
    {U'く', "ku"}, {U'け', "ke"}, {U'こ', "ko"}, {U'さ', "sa"},  {U'し', "shi"}, {U'す', "su"},
    {U'せ', "se"}, {U'そ', "so"}, {U'た', "ta"}, {U'ち', "chi"}, {U'つ', "tsu"}, {U'て', "te"},
    {U'と', "to"}, {U'な', "na"}, {U'に', "ni"}, {U'ぬ', "nu"},  {U'ね', "ne"},  {U'の', "no"},
    {U'は', "ha"}, {U'ひ', "hi"}, {U'ふ', "fu"}, {U'へ', "he"},  {U'ほ', "ho"},  {U'ま', "ma"},
    {U'み', "mi"}, {U'む', "mu"}, {U'め', "me"}, {U'も', "mo"},  {U'ゃ', "ya"},  {U'や', "ya"},
    {U'ゅ', "yu"}, {U'ゆ', "yu"}, {U'ょ', "yo"}, {U'よ', "yo"},  {U'ら', "ra"},  {U'り', "ri"},
    {U'る', "ru"}, {U'れ', "re"}, {U'ろ', "ro"}, {U'ゎ', "wa"},  {U'わ', "wa"},  {U'を', "wo"},
    {U'ん', "n"},  {U'が', "ga"}, {U'ぎ', "gi"}, {U'ぐ', "gu"},  {U'げ', "ge"},  {U'ご', "go"},
    {U'ざ', "za"}, {U'じ', "ji"}, {U'ず', "zu"}, {U'ぜ', "ze"},  {U'ぞ', "zo"},  {U'だ', "da"},
    {U'ぢ', "di"}, {U'づ', "du"}, {U'で', "de"}, {U'ど', "do"},  {U'ば', "ba"},  {U'び', "bi"},
    {U'ぶ', "bu"}, {U'べ', "be"}, {U'ぼ', "bo"}, {U'ぱ', "pa"},  {U'ぴ', "pi"},  {U'ぷ', "pu"},
    {U'ぺ', "pe"}, {U'ぽ', "po"}, {U'ゔ', "vu"},
};

bool IsSmallKana(char32_t value) {
  return value == U'ぁ' || value == U'ぃ' || value == U'ぅ' || value == U'ぇ' || value == U'ぉ' ||
         value == U'ゃ' || value == U'ゅ' || value == U'ょ' || value == U'ゎ';
}

std::pair<std::string_view, size_t> RomanizeUnit(std::u32string_view input, size_t offset) {
  if (offset + 1 < input.size()) {
    for (const auto& [kana, romaji] : kDigraphs) {
      if (input.substr(offset, 2) == kana) return {romaji, 2};
    }
    // Never invent a romanization for an unsupported contracted syllable.
    if (IsSmallKana(input[offset + 1])) return {{}, 0};
  }
  for (const auto& [kana, romaji] : kScalars) {
    if (input[offset] == kana) return {romaji, 1};
  }
  return {{}, 0};
}

std::string Romanize(std::u32string_view input) {
  std::string result;
  for (size_t i = 0; i < input.size();) {
    if (input[i] == U'ー') {
      if (result.empty()) return {};
      result.push_back('-');
      ++i;
      continue;
    }
    if (input[i] == U'っ') {
      if (i + 1 == input.size() || IsSmallKana(input[i + 1])) return {};
      const auto [next, length] = RomanizeUnit(input, i + 1);
      if (length == 0 || next.empty() || next.front() == 'a' || next.front() == 'e' ||
          next.front() == 'i' || next.front() == 'o' || next.front() == 'u') {
        return {};
      }
      result.push_back(next.front() == 'c' ? 't' : next.front());
      ++i;
      continue;
    }
    const auto [romaji, length] = RomanizeUnit(input, i);
    if (length == 0) return {};
    result += romaji;
    if (input[i] == U'ん' && i + 1 < input.size()) {
      const auto [next, next_length] = RomanizeUnit(input, i + 1);
      if (next_length != 0 && !next.empty() &&
          (next.front() == 'a' || next.front() == 'e' || next.front() == 'i' ||
           next.front() == 'o' || next.front() == 'u' || next.front() == 'y')) {
        result.push_back('\'');
      }
    }
    i += length;
  }
  return result;
}

bool IsHiragana(char32_t value) { return value >= U'ぁ' && value <= U'ゖ'; }

std::string_view ScalarRomaji(char32_t value) {
  for (const auto& [kana, romaji] : kScalars) {
    if (value == kana) return romaji;
  }
  return {};
}

// Lowercase Hepburn for a run of hiragana and ー. A run Romanize rejects (a
// contracted syllable it has no spelling for, a trailing っ) is spelled kana by
// kana, small kana with an x so the spelling still types the same kana.
std::string RomanizeRun(std::u32string_view run) {
  std::string spelled = Romanize(run);
  if (!spelled.empty()) return spelled;
  for (const char32_t value : run) {
    if (value == U'ー') {
      spelled.push_back('-');
    } else if (value == U'っ') {
      spelled += "xtu";
    } else if (const auto romaji = ScalarRomaji(value); !romaji.empty()) {
      if (IsSmallKana(value)) spelled.push_back('x');
      spelled += romaji;
    } else {
      AppendUtf8(spelled, value);
    }
  }
  return spelled;
}

// The ASCII typed for a symbol the reading took in: the table of
// tsf-deep-integration-spec.md §3.5 read backwards. ￥ was typed as ¥.
std::string_view TypedSymbol(char32_t value) {
  static constexpr ScalarMapping kSymbols[] = {
      {U'”', "\""}, {U'’', "'"},  {U'〜', "~"}, {U'『', "{"}, {U'』', "}"}, {U'ー', "-"},
      {U'￥', "¥"}, {U'「', "["}, {U'」', "]"}, {U'・', "/"}, {U'、', ","}, {U'。', "."},
  };
  for (const auto& [symbol, typed] : kSymbols) {
    if (value == symbol) return typed;
  }
  return {};
}

std::string RespellRoman(std::u32string_view reading) {
  std::string spelled;
  std::u32string run;
  for (const char32_t value : reading) {
    if (IsHiragana(value) || value == U'ー') {
      run.push_back(value);
      continue;
    }
    spelled += RomanizeRun(run);
    run.clear();
    if (value >= U'！' && value <= U'～') {
      spelled.push_back(static_cast<char>(value - 0xfee0));
    } else if (const auto typed = TypedSymbol(value); !typed.empty()) {
      spelled += typed;
    } else {
      AppendUtf8(spelled, value);
    }
  }
  return spelled + RomanizeRun(run);
}

// Half-width forms exist for kana, ASCII symbols and a few marks only; ’ ” 〜
// 『』 stay as they are (spec §3.6).
void AppendHalfwidth(std::string& output, char32_t value) {
  char32_t hiragana = value;
  if (value >= U'ァ' && value <= U'ヶ' && IsHiragana(value - 0x60)) hiragana = value - 0x60;
  if (IsHiragana(hiragana)) {
    std::string kana;
    AppendUtf8(kana, hiragana);
    const auto forms = ExpandKatakanaCandidates(kana);
    if (!forms.empty()) {
      output += forms.back().surface;
      return;
    }
  }
  static constexpr ScalarMapping kMarks[] = {
      {U'ー', "ｰ"}, {U'￥', "¥"}, {U'「', "｢"}, {U'」', "｣"},
      {U'、', "､"}, {U'。', "｡"}, {U'・', "･"},
  };
  for (const auto& [mark, halfwidth] : kMarks) {
    if (value == mark) {
      output += halfwidth;
      return;
    }
  }
  if (value >= U'！' && value <= U'～') value -= 0xfee0;
  AppendUtf8(output, value);
}

std::string FullwidthRoman(std::string_view roman) {
  std::string wide;
  for (size_t offset = 0; offset < roman.size();) {
    char32_t value = 0;
    if (!DecodeNextUtf8(roman, offset, value)) return std::string(roman);
    if (value > 0x20 && value < 0x7f) value += 0xfee0;
    if (value == U'¥') value = U'￥';
    AppendUtf8(wide, value);
  }
  return wide;
}

}  // namespace

std::string BuildCommitForm(std::string_view reading, CommitForm form,
                            std::string_view typed_romaji) {
  std::u32string codepoints;
  for (size_t offset = 0; offset < reading.size();) {
    char32_t codepoint = 0;
    if (!DecodeNextUtf8(reading, offset, codepoint)) return std::string(reading);
    codepoints.push_back(codepoint);
  }
  std::string text;
  switch (form) {
    case CommitForm::Hiragana:
      return std::string(reading);
    case CommitForm::Katakana:
      for (const char32_t value : codepoints)
        AppendUtf8(text, IsHiragana(value) ? value + 0x60 : value);
      return text;
    case CommitForm::HalfwidthKatakana:
      for (const char32_t value : codepoints) AppendHalfwidth(text, value);
      return text;
    case CommitForm::FullwidthRoman:
    case CommitForm::HalfwidthRoman:
      text = typed_romaji.empty() ? RespellRoman(codepoints) : std::string(typed_romaji);
      return form == CommitForm::HalfwidthRoman ? text : FullwidthRoman(text);
  }
  return std::string(reading);
}

std::optional<CharacterFormCycle> BuildCharacterFormCycle(std::string_view hiragana) {
  if (hiragana.empty()) return std::nullopt;
  const auto katakana = ExpandKatakanaCandidates(std::string(hiragana));
  if (katakana.size() != 2) return std::nullopt;

  std::u32string codepoints;
  for (size_t offset = 0; offset < hiragana.size();) {
    char32_t codepoint = 0;
    if (!DecodeNextUtf8(hiragana, offset, codepoint)) return std::nullopt;
    codepoints.push_back(codepoint);
  }
  std::string ascii = Romanize(codepoints);
  if (ascii.empty()) return std::nullopt;

  std::string fullwidth;
  fullwidth.reserve(ascii.size() * 3);
  for (char& c : ascii) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    const char32_t wide = c == '\'' ? U'＇' : static_cast<char32_t>(c) + 0xfee0;
    AppendUtf8(fullwidth, wide);
  }
  return CharacterFormCycle{{std::string(hiragana), katakana[0].surface, katakana[1].surface,
                             std::move(fullwidth), std::move(ascii)}};
}

}  // namespace azookey::core
