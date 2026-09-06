#include <array>

#include "azookey/core/BracketPairing.h"

namespace azookey::core {
namespace {
constexpr std::array<BracketPair, 14> kBracketPairs{{
    {U'（', U'）'},
    {U'「', U'」'},
    {U'『', U'』'},
    {U'【', U'】'},
    {U'〔', U'〕'},
    {U'［', U'］'},
    {U'｛', U'｝'},
    {U'〈', U'〉'},
    {U'《', U'》'},
    {U'“', U'”'},
    {U'‘', U'’'},
    {U'(', U')'},
    {U'[', U']'},
    {U'{', U'}'},
}};
}  // namespace

std::optional<BracketPair> LookupBracketPair(char32_t codepoint) {
  for (const auto& pair : kBracketPairs) {
    if (pair.open == codepoint) return pair;
  }
  return std::nullopt;
}

std::optional<BracketPair> LookupClosingBracket(char32_t codepoint) {
  for (const auto& pair : kBracketPairs) {
    if (pair.close == codepoint) return pair;
  }
  return std::nullopt;
}
}  // namespace azookey::core
