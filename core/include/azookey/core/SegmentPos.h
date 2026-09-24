#pragma once

#include <cstdint>

namespace azookey::core {

// Coarse categories shared by the converter and punctuation policy. Unknown
// keeps dictionary-free candidates and older hosts compatible.
enum class SegmentPos : uint8_t {
  Unknown = 0,
  Taigen = 1,
  Yougen = 2,
  JoshiCase = 3,
  JoshiConj = 4,
  JoshiOther = 5,
  Setsuzoku = 6,
  Jodoushi = 7,
  HojoYougen = 8,
  Rentai = 9,
  Fukushi = 10,
  Kigou = 11,
  English = 12,
};

enum class SegmentSemantic : uint8_t {
  Unknown = 0,
  Generic = 1,
  PersonName = 2,
  PlaceName = 3,
  OrgName = 4,
  ProductName = 5,
  DateTime = 6,
  Number = 7,
};

}  // namespace azookey::core
