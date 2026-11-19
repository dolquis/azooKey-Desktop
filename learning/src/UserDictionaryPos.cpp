#include "azookey/learning/UserDictionaryPos.h"

#include <algorithm>
#include <array>

namespace azookey::learning {

namespace {

enum class Kind { Plain, Godan, Ichidan, Sahen, Adjective };

struct PosRow {
  std::string_view id;
  Kind kind;
  // The dictionary form's ending of the reading (section 4.2, first row).
  std::string_view ending;
};

// docs/user-dictionary-pos-spec.md section 2.1, in the table's order.
constexpr std::array kRows{
    PosRow{"noun_sahen", Kind::Plain, ""},      PosRow{"noun_adjectival", Kind::Plain, ""},
    PosRow{"proper_noun", Kind::Plain, ""},     PosRow{"person_name", Kind::Plain, ""},
    PosRow{"surname", Kind::Plain, ""},         PosRow{"given_name", Kind::Plain, ""},
    PosRow{"place_name", Kind::Plain, ""},      PosRow{"organization", Kind::Plain, ""},
    PosRow{"adverb", Kind::Plain, ""},          PosRow{"adnominal", Kind::Plain, ""},
    PosRow{"conjunction", Kind::Plain, ""},     PosRow{"interjection", Kind::Plain, ""},
    PosRow{"prefix", Kind::Plain, ""},          PosRow{"suffix", Kind::Plain, ""},
    PosRow{"counter", Kind::Plain, ""},         PosRow{"symbol", Kind::Plain, ""},
    PosRow{"verb_godan_ka", Kind::Godan, "く"}, PosRow{"verb_godan_ga", Kind::Godan, "ぐ"},
    PosRow{"verb_godan_sa", Kind::Godan, "す"}, PosRow{"verb_godan_ta", Kind::Godan, "つ"},
    PosRow{"verb_godan_na", Kind::Godan, "ぬ"}, PosRow{"verb_godan_ba", Kind::Godan, "ぶ"},
    PosRow{"verb_godan_ma", Kind::Godan, "む"}, PosRow{"verb_godan_ra", Kind::Godan, "る"},
    PosRow{"verb_godan_wa", Kind::Godan, "う"}, PosRow{"verb_ichidan", Kind::Ichidan, "る"},
    PosRow{"verb_sahen", Kind::Sahen, "する"},  PosRow{"adjective", Kind::Adjective, "い"},
};

// Section 4.3: the kana before the final "る" of an ichidan verb, the i-row
// and e-row kana of the カ・ガ・サ・ザ・タ・ダ・ナ・ハ・バ・マ・ラ・ア rows.
constexpr std::array<std::string_view, 24> kIchidanStemEnds{
    "き", "け", "ぎ", "げ", "し", "せ", "じ", "ぜ", "ち", "て", "ぢ", "で",
    "に", "ね", "ひ", "へ", "び", "べ", "み", "め", "り", "れ", "い", "え",
};

const PosRow* FindRow(std::string_view pos) {
  const auto it =
      std::find_if(kRows.begin(), kRows.end(), [&](const PosRow& row) { return row.id == pos; });
  return it == kRows.end() ? nullptr : &*it;
}

bool EndsWith(std::string_view text, std::string_view suffix) {
  return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

// The last UTF-8 code point of a non-empty text.
std::string_view LastCodePoint(std::string_view text) {
  size_t start = text.size() - 1;
  while (start > 0 && (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80) --start;
  return text.substr(start);
}

}  // namespace

const std::vector<std::string_view>& UserDictionaryPosIds() {
  static const std::vector<std::string_view> ids = [] {
    std::vector<std::string_view> out;
    out.reserve(kRows.size());
    for (const auto& row : kRows) out.push_back(row.id);
    return out;
  }();
  return ids;
}

bool IsKnownUserDictionaryPos(std::string_view pos) { return FindRow(pos) != nullptr; }

bool IsConjugatingUserDictionaryPos(std::string_view pos) {
  const auto* row = FindRow(pos);
  return row && row->kind != Kind::Plain;
}

UserWordPosCheck CheckUserWordPos(std::string_view word, std::string_view ruby,
                                  std::string_view pos) {
  const auto* row = FindRow(pos);
  if (!row) return UserWordPosCheck::UnknownPos;
  if (row->kind == Kind::Plain) return UserWordPosCheck::Ok;
  // Section 4.4: a stem of at least one character before the ending.
  if (ruby.size() <= row->ending.size() || !EndsWith(ruby, row->ending) ||
      !EndsWith(word, row->ending))
    return UserWordPosCheck::PosMismatch;
  if (row->kind == Kind::Ichidan) {
    const auto stem_end = LastCodePoint(ruby.substr(0, ruby.size() - row->ending.size()));
    if (std::find(kIchidanStemEnds.begin(), kIchidanStemEnds.end(), stem_end) ==
        kIchidanStemEnds.end())
      return UserWordPosCheck::PosMismatch;
  }
  return UserWordPosCheck::Ok;
}

}  // namespace azookey::learning
