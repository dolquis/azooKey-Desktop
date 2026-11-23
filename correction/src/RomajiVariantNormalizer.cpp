#include "azookey/correction/RomajiVariantNormalizer.h"

#include <array>

namespace azookey::correction {
namespace {

bool IsVowelAfterY(char c) { return c == 'a' || c == 'u' || c == 'e' || c == 'o'; }

}  // namespace

std::string NormalizeRomajiVariants(std::string_view romaji) {
  std::string normalized;
  normalized.reserve(romaji.size() + romaji.size() / 2);
  size_t index = 0;
  while (index < romaji.size()) {
    const std::string_view rest = romaji.substr(index);
    if (rest.starts_with("shy") || rest.starts_with("chy")) {
      normalized.append(rest.substr(0, 2));
      index += 3;
    } else if (rest.size() >= 3 && rest[1] == 'y' && IsVowelAfterY(rest[2]) &&
               (rest[0] == 's' || rest[0] == 'z' || rest[0] == 'j' || rest[0] == 't')) {
      normalized.append(rest[0] == 's' ? "sh" : rest[0] == 't' ? "ch" : "j");
      index += 2;
    } else if (rest.starts_with("si")) {
      normalized.append("shi");
      index += 2;
    } else if (rest.starts_with("ti")) {
      normalized.append("chi");
      index += 2;
    } else if (rest.starts_with("tu")) {
      normalized.append("tsu");
      index += 2;
    } else if (rest.starts_with("nn")) {
      normalized.push_back('n');
      index += 2;
    } else {
      normalized.push_back(rest[0]);
      index += 1;
    }
  }
  return normalized;
}

bool EndsWithVariantYoon(std::string_view romaji) {
  const std::string normalized = NormalizeRomajiVariants(romaji);
  constexpr std::array<std::string_view, 6> kSyllables = {"sho", "shu", "cho", "chu", "jo", "ju"};
  for (const std::string_view syllable : kSyllables) {
    if (std::string_view(normalized).ends_with(syllable)) return true;
  }
  return false;
}

}  // namespace azookey::correction
