#pragma once

#include <cstdint>
#include <vector>

#include "azookey/learning/LearningStore.h"

namespace azookey::learning {

// Writing-style ratios of rich-features-spec X-2-7. Each ratio is the share of
// commits whose surface matches the style, so the four need not sum to 1.
struct Persona {
  double polite_ratio{};
  double casual_ratio{};
  double technical_ratio{};
  double kaomoji_ratio{};
  // Commits the ratios were computed from; 0 leaves every ratio 0.
  uint64_t sample_count{};
};

// The store keeps one aggregate per (reading, surface), not the committed
// sentences, so this approximates X-2-7: each pair's surface is classified and
// weighted by its commit_count. A marker split across two committed segments
// ("だ" + "よ") is not seen.
//
// - polite: the surface contains "ます", "です" or "いただ"
// - casual: the surface contains "だよ", "だね" or "じゃん"
// - technical: the surface is ASCII letters, digits and "_" only, at least two
//   characters long, with at least one letter
// - kaomoji: the surface has a run of 3 to 10 symbols (code points that are
//   not ASCII letters or digits, kana, kanji, fullwidth letters or digits,
//   whitespace, or the sentence punctuation 、。，．「」『』…‥)
Persona ComputePersona(const std::vector<LearningAggregate>& aggregates);

}  // namespace azookey::learning
