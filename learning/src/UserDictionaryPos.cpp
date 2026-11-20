#include "azookey/learning/UserDictionaryPos.h"

#include <algorithm>
#include <array>
#include <utility>

namespace azookey::learning {

namespace {

enum class Kind { Plain, Godan, Ichidan, Sahen, Adjective };

struct PosRow {
  std::string_view id;
  Kind kind;
  // The dictionary form's ending of the reading (section 4.2, first row).
  std::string_view ending;
  // 品詞 1〜4 of the section 2.1 table, as META.lattice.pos_names holds them.
  std::string_view name;
};

// docs/user-dictionary-pos-spec.md section 2.1, in the table's order.
constexpr std::array kRows{
    PosRow{"noun_sahen", Kind::Plain, "", "名詞-普通名詞-サ変可能"},
    PosRow{"noun_adjectival", Kind::Plain, "", "形状詞-一般"},
    PosRow{"proper_noun", Kind::Plain, "", "名詞-固有名詞-一般"},
    PosRow{"person_name", Kind::Plain, "", "名詞-固有名詞-人名-一般"},
    PosRow{"surname", Kind::Plain, "", "名詞-固有名詞-人名-姓"},
    PosRow{"given_name", Kind::Plain, "", "名詞-固有名詞-人名-名"},
    PosRow{"place_name", Kind::Plain, "", "名詞-固有名詞-地名-一般"},
    PosRow{"organization", Kind::Plain, "", "名詞-固有名詞-一般"},
    PosRow{"adverb", Kind::Plain, "", "副詞"},
    PosRow{"adnominal", Kind::Plain, "", "連体詞"},
    PosRow{"conjunction", Kind::Plain, "", "接続詞"},
    PosRow{"interjection", Kind::Plain, "", "感動詞-一般"},
    PosRow{"prefix", Kind::Plain, "", "接頭辞"},
    PosRow{"suffix", Kind::Plain, "", "接尾辞-名詞的-一般"},
    PosRow{"counter", Kind::Plain, "", "接尾辞-名詞的-助数詞"},
    PosRow{"symbol", Kind::Plain, "", "記号-一般"},
    PosRow{"verb_godan_ka", Kind::Godan, "く", "動詞-一般"},
    PosRow{"verb_godan_ga", Kind::Godan, "ぐ", "動詞-一般"},
    PosRow{"verb_godan_sa", Kind::Godan, "す", "動詞-一般"},
    PosRow{"verb_godan_ta", Kind::Godan, "つ", "動詞-一般"},
    PosRow{"verb_godan_na", Kind::Godan, "ぬ", "動詞-一般"},
    PosRow{"verb_godan_ba", Kind::Godan, "ぶ", "動詞-一般"},
    PosRow{"verb_godan_ma", Kind::Godan, "む", "動詞-一般"},
    PosRow{"verb_godan_ra", Kind::Godan, "る", "動詞-一般"},
    PosRow{"verb_godan_wa", Kind::Godan, "う", "動詞-一般"},
    PosRow{"verb_ichidan", Kind::Ichidan, "る", "動詞-一般"},
    PosRow{"verb_sahen", Kind::Sahen, "する", "動詞-一般"},
    PosRow{"adjective", Kind::Adjective, "い", "形容詞-一般"},
};

// Section 4.2 for one 五段 row: its 活用型, the kana of the あ段 (未然形), い段 (連用形) and え段
// (仮定形 and 命令形), the お段 with "う" (意志推量形), and the euphonic 連用形 (none for サ行).
struct GodanRow {
  std::string_view id;
  std::string_view type;
  std::string_view a, i, e, volitional;
  std::string_view euphonic_form, euphonic_ending;
};

constexpr std::array kGodanRows{
    GodanRow{"verb_godan_ka", "五段-カ行", "か", "き", "け", "こう", "連用形-イ音便", "い"},
    GodanRow{"verb_godan_ga", "五段-ガ行", "が", "ぎ", "げ", "ごう", "連用形-イ音便", "い"},
    GodanRow{"verb_godan_sa", "五段-サ行", "さ", "し", "せ", "そう", "", ""},
    GodanRow{"verb_godan_ta", "五段-タ行", "た", "ち", "て", "とう", "連用形-促音便", "っ"},
    GodanRow{"verb_godan_na", "五段-ナ行", "な", "に", "ね", "のう", "連用形-撥音便", "ん"},
    GodanRow{"verb_godan_ba", "五段-バ行", "ば", "び", "べ", "ぼう", "連用形-撥音便", "ん"},
    GodanRow{"verb_godan_ma", "五段-マ行", "ま", "み", "め", "もう", "連用形-撥音便", "ん"},
    GodanRow{"verb_godan_ra", "五段-ラ行", "ら", "り", "れ", "ろう", "連用形-促音便", "っ"},
    GodanRow{"verb_godan_wa", "五段-ワア行", "わ", "い", "え", "おう", "連用形-促音便", "っ"},
};

// Section 4.3: the kana before the final "る" of an ichidan verb, the i-row
// and e-row kana of the カ・ガ・サ・ザ・タ・ダ・ナ・ハ・バ・マ・ラ・ア rows, with the 活用型 each
// gives.
struct IchidanStemEnd {
  std::string_view kana;
  std::string_view type;
};

constexpr std::array kIchidanStemEnds{
    IchidanStemEnd{"き", "上一段-カ行"}, IchidanStemEnd{"け", "下一段-カ行"},
    IchidanStemEnd{"ぎ", "上一段-ガ行"}, IchidanStemEnd{"げ", "下一段-ガ行"},
    IchidanStemEnd{"し", "上一段-サ行"}, IchidanStemEnd{"せ", "下一段-サ行"},
    IchidanStemEnd{"じ", "上一段-ザ行"}, IchidanStemEnd{"ぜ", "下一段-ザ行"},
    IchidanStemEnd{"ち", "上一段-タ行"}, IchidanStemEnd{"て", "下一段-タ行"},
    IchidanStemEnd{"ぢ", "上一段-ダ行"}, IchidanStemEnd{"で", "下一段-ダ行"},
    IchidanStemEnd{"に", "上一段-ナ行"}, IchidanStemEnd{"ね", "下一段-ナ行"},
    IchidanStemEnd{"ひ", "上一段-ハ行"}, IchidanStemEnd{"へ", "下一段-ハ行"},
    IchidanStemEnd{"び", "上一段-バ行"}, IchidanStemEnd{"べ", "下一段-バ行"},
    IchidanStemEnd{"み", "上一段-マ行"}, IchidanStemEnd{"め", "下一段-マ行"},
    IchidanStemEnd{"り", "上一段-ラ行"}, IchidanStemEnd{"れ", "下一段-ラ行"},
    IchidanStemEnd{"い", "上一段-ア行"}, IchidanStemEnd{"え", "下一段-ア行"},
};

constexpr std::string_view kTerminalForm = "終止形-一般";
constexpr std::string_view kNoConjugation = "*";

const PosRow* FindRow(std::string_view pos) {
  const auto it =
      std::find_if(kRows.begin(), kRows.end(), [&](const PosRow& row) { return row.id == pos; });
  return it == kRows.end() ? nullptr : &*it;
}

const GodanRow& FindGodanRow(std::string_view pos) {
  return *std::find_if(kGodanRows.begin(), kGodanRows.end(),
                       [&](const GodanRow& row) { return row.id == pos; });
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

// Section 4.3: the 活用型 of an ichidan reading, or nullptr when the kana before "る" decides none.
const IchidanStemEnd* FindIchidanStemEnd(std::string_view ruby) {
  const auto stem_end = LastCodePoint(ruby.substr(0, ruby.size() - std::string_view("る").size()));
  const auto it = std::find_if(kIchidanStemEnds.begin(), kIchidanStemEnds.end(),
                               [&](const IchidanStemEnd& end) { return end.kana == stem_end; });
  return it == kIchidanStemEnds.end() ? nullptr : &*it;
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
  if (row->kind == Kind::Ichidan && !FindIchidanStemEnd(ruby)) return UserWordPosCheck::PosMismatch;
  return UserWordPosCheck::Ok;
}

std::optional<UserWordLatticePos> UserWordLatticePosOf(std::string_view word, std::string_view ruby,
                                                       std::string_view pos) {
  const auto* row = FindRow(pos);
  if (!row) return std::nullopt;
  if (row->kind == Kind::Plain)
    return UserWordLatticePos{row->name, kNoConjugation, kNoConjugation, false};
  if (CheckUserWordPos(word, ruby, pos) != UserWordPosCheck::Ok) return std::nullopt;
  std::string_view type;
  switch (row->kind) {
    case Kind::Godan:
      type = FindGodanRow(pos).type;
      break;
    case Kind::Ichidan:
      type = FindIchidanStemEnd(ruby)->type;
      break;
    case Kind::Sahen:
      type = "サ行変格";
      break;
    case Kind::Adjective:
      type = "形容詞";
      break;
    case Kind::Plain:
      break;
  }
  return UserWordLatticePos{row->name, type, kTerminalForm, true};
}

std::vector<UserWordForm> ExpandUserWord(std::string_view word, std::string_view ruby,
                                         std::string_view pos) {
  std::vector<UserWordForm> forms;
  const auto* row = FindRow(pos);
  if (!row || row->kind == Kind::Plain || CheckUserWordPos(word, ruby, pos) != UserWordPosCheck::Ok)
    return forms;
  // Section 4.4: the word ends with the same hiragana as the reading, so both lose it.
  const auto reading_stem = ruby.substr(0, ruby.size() - row->ending.size());
  const auto surface_stem = word.substr(0, word.size() - row->ending.size());
  const auto add = [&](std::string_view form, std::string_view ending) {
    UserWordForm made;
    made.reading.append(reading_stem).append(ending);
    made.surface.append(surface_stem).append(ending);
    made.conjugation_form = form;
    forms.push_back(std::move(made));
  };
  // Section 4.2, row by row of its table.
  switch (row->kind) {
    case Kind::Godan: {
      const auto& godan = FindGodanRow(pos);
      add("未然形-一般", godan.a);
      add("連用形-一般", godan.i);
      // The "行く" type takes the 促音便 in place of the イ音便.
      if (pos == "verb_godan_ka" && (EndsWith(ruby, "いく") || EndsWith(ruby, "ゆく")))
        add("連用形-促音便", "っ");
      else if (!godan.euphonic_form.empty())
        add(godan.euphonic_form, godan.euphonic_ending);
      add("連体形-一般", row->ending);
      add("仮定形-一般", godan.e);
      add("命令形", godan.e);
      add("意志推量形", godan.volitional);
      break;
    }
    case Kind::Ichidan:
      add("未然形-一般", "");
      add("連用形-一般", "");
      add("連体形-一般", "る");
      add("仮定形-一般", "れ");
      add("命令形", "ろ");
      add("意志推量形", "よう");
      break;
    case Kind::Sahen:
      add("未然形-一般", "し");
      add("未然形-セ", "せ");
      add("連用形-一般", "し");
      add("連体形-一般", "する");
      add("仮定形-一般", "すれ");
      add("命令形", "しろ");
      add("意志推量形", "しよう");
      break;
    case Kind::Adjective:
      add("連用形-一般", "く");
      add("連用形-促音便", "かっ");
      add("連体形-一般", "い");
      add("仮定形-一般", "けれ");
      add("意志推量形", "かろう");
      add("語幹-一般", "");
      break;
    case Kind::Plain:
      break;
  }
  return forms;
}

}  // namespace azookey::learning
