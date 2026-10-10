#pragma once

#include <cstdint>

namespace azookey::learning {

// Fixed .azdic category bit positions, auto-word-registration-spec section 14.4.
// Keep in sync with dictbuild.py CATEGORIES; dictbuild_python_tests checks the mapping.
enum class DictionaryCategory : uint8_t {
  General = 0,
  PersonName = 1,
  PlaceName = 2,
  StationName = 3,
  ProductName = 4,
  Software = 5,
  AnimeGame = 6,
  CompanyOrg = 7,
  Technical = 8,
  Neologism = 9,
};

constexpr uint16_t CategoryBit(DictionaryCategory category) {
  return static_cast<uint16_t>(1U << static_cast<unsigned>(category));
}

}  // namespace azookey::learning
