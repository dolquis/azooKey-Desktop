#include "azookey/learning/Persona.h"

#include <array>
#include <string>
#include <string_view>

namespace azookey::learning {
namespace {

constexpr size_t kMinKaomojiRun = 3;
constexpr size_t kMaxKaomojiRun = 10;

bool ContainsAny(std::string_view text, const std::array<std::string_view, 3>& markers) {
  for (const auto marker : markers) {
    if (text.find(marker) != std::string_view::npos) return true;
  }
  return false;
}

bool IsAsciiLetter(char32_t c) { return (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z'); }
bool IsAsciiDigit(char32_t c) { return c >= U'0' && c <= U'9'; }

bool IsTechnical(std::string_view surface) {
  if (surface.size() < 2) return false;
  bool has_letter = false;
  for (const char ch : surface) {
    const auto c = static_cast<char32_t>(static_cast<unsigned char>(ch));
    if (IsAsciiLetter(c)) {
      has_letter = true;
    } else if (!IsAsciiDigit(c) && c != U'_') {
      return false;
    }
  }
  return has_letter;
}

// Decodes one UTF-8 code point at `pos`; an invalid sequence yields U+FFFD and
// advances one byte, so a malformed surface cannot stall the scan.
char32_t NextCodePoint(std::string_view text, size_t& pos) {
  const auto lead = static_cast<unsigned char>(text[pos]);
  size_t length = 0;
  char32_t value = 0;
  if (lead < 0x80) {
    ++pos;
    return lead;
  }
  if ((lead & 0xE0) == 0xC0) {
    length = 2;
    value = lead & 0x1F;
  } else if ((lead & 0xF0) == 0xE0) {
    length = 3;
    value = lead & 0x0F;
  } else if ((lead & 0xF8) == 0xF0) {
    length = 4;
    value = lead & 0x07;
  } else {
    ++pos;
    return U'�';
  }
  if (pos + length > text.size()) {
    ++pos;
    return U'�';
  }
  for (size_t i = 1; i < length; ++i) {
    const auto next = static_cast<unsigned char>(text[pos + i]);
    if ((next & 0xC0) != 0x80) {
      ++pos;
      return U'�';
    }
    value = (value << 6) | (next & 0x3F);
  }
  pos += length;
  return value;
}

bool IsWordOrSpace(char32_t c) {
  return IsAsciiLetter(c) || IsAsciiDigit(c) || c == U' ' || c == U'\t' || c == U'　' ||
         // hiragana and katakana; the middle dot U+30FB is a symbol ("(・ω・)")
         (c >= 0x3040 && c <= 0x30FF && c != 0x30FB) ||
         (c >= 0x31F0 && c <= 0x31FF) ||    // katakana phonetic extensions
         (c >= 0x3400 && c <= 0x4DBF) ||    // CJK extension A
         (c >= 0x4E00 && c <= 0x9FFF) ||    // CJK unified ideographs
         (c >= 0xF900 && c <= 0xFAFF) ||    // CJK compatibility ideographs
         (c >= 0xFF10 && c <= 0xFF19) ||    // fullwidth digits
         (c >= 0xFF21 && c <= 0xFF3A) ||    // fullwidth uppercase
         (c >= 0xFF41 && c <= 0xFF5A) ||    // fullwidth lowercase
         (c >= 0xFF66 && c <= 0xFF9F) ||    // halfwidth katakana
         (c >= 0x20000 && c <= 0x3FFFF) ||  // CJK extensions B and later
         c == U'々' || c == U'〆';
}

// Sentence punctuation is not a face: "……。" or "」「" must not count.
bool IsSentencePunctuation(char32_t c) {
  return c == U'、' || c == U'。' || c == U'，' || c == U'．' || c == U'「' || c == U'」' ||
         c == U'『' || c == U'』' || c == U'…' || c == U'‥';
}

bool HasKaomoji(std::string_view surface) {
  size_t run = 0;
  size_t pos = 0;
  while (pos < surface.size()) {
    const char32_t c = NextCodePoint(surface, pos);
    if (IsWordOrSpace(c) || IsSentencePunctuation(c)) {
      if (run >= kMinKaomojiRun && run <= kMaxKaomojiRun) return true;
      run = 0;
    } else {
      ++run;
    }
  }
  return run >= kMinKaomojiRun && run <= kMaxKaomojiRun;
}

double Ratio(uint64_t part, uint64_t total) {
  return total == 0 ? 0.0 : static_cast<double>(part) / static_cast<double>(total);
}

}  // namespace

Persona ComputePersona(const std::vector<LearningAggregate>& aggregates) {
  static constexpr std::array<std::string_view, 3> kPolite{"ます", "です", "いただ"};
  static constexpr std::array<std::string_view, 3> kCasual{"だよ", "だね", "じゃん"};
  uint64_t total = 0;
  uint64_t polite = 0;
  uint64_t casual = 0;
  uint64_t technical = 0;
  uint64_t kaomoji = 0;
  for (const auto& aggregate : aggregates) {
    const uint64_t weight = aggregate.commit_count;
    if (weight == 0) continue;
    total += weight;
    const std::string_view surface = aggregate.surface;
    if (ContainsAny(surface, kPolite)) polite += weight;
    if (ContainsAny(surface, kCasual)) casual += weight;
    if (IsTechnical(surface)) technical += weight;
    if (HasKaomoji(surface)) kaomoji += weight;
  }
  Persona persona;
  persona.sample_count = total;
  persona.polite_ratio = Ratio(polite, total);
  persona.casual_ratio = Ratio(casual, total);
  persona.technical_ratio = Ratio(technical, total);
  persona.kaomoji_ratio = Ratio(kaomoji, total);
  return persona;
}

}  // namespace azookey::learning
