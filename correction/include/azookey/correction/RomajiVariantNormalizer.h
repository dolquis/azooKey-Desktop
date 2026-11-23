#pragma once

#include <string>
#include <string_view>

namespace azookey::correction {

// docs/typo-correction-learning-spec.md section 12.4.3: rewrites spellings to
// one canonical form, left to right. si -> shi, ti -> chi, tu -> tsu,
// sy + a/u/e/o -> sh, zy / jy + a/u/e/o -> j, ty + a/u/e/o -> ch, nn -> n, and
// the mixed spellings shy -> sh, chy -> ch. Other characters stay as they are.
// The built-in table reads each pair the same way except nn and the mixed
// spellings, so the result is for comparing spellings, not for conversion.
std::string NormalizeRomajiVariants(std::string_view romaji);

// True when `romaji`, normalized, ends with sho, shu, cho, chu, jo or ju. After
// such a syllable a left-out or extra 'u' is the syo / syou spelling variant of
// section 12.3 rather than a long vowel slip.
bool EndsWithVariantYoon(std::string_view romaji);

}  // namespace azookey::correction
