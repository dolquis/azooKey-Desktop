#include "azookey/learning/UserDictionaryTextImport.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <locale>
#include <map>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

#include "azookey/learning/UserDictionaryPos.h"

namespace azookey::learning {

namespace {

// ---- UTF-8 and UTF-16 (section 6.2) ----

void AppendUtf8(std::string& out, char32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

// Strict UTF-8: no overlong forms, surrogates or code points past U+10FFFF.
bool DecodeUtf8(std::string_view text, std::u32string* out) {
  size_t i = 0;
  while (i < text.size()) {
    const auto b0 = static_cast<unsigned char>(text[i]);
    char32_t cp = 0;
    size_t length = 0;
    char32_t minimum = 0;
    if (b0 < 0x80) {
      cp = b0;
      length = 1;
    } else if ((b0 & 0xE0) == 0xC0) {
      cp = b0 & 0x1F;
      length = 2;
      minimum = 0x80;
    } else if ((b0 & 0xF0) == 0xE0) {
      cp = b0 & 0x0F;
      length = 3;
      minimum = 0x800;
    } else if ((b0 & 0xF8) == 0xF0) {
      cp = b0 & 0x07;
      length = 4;
      minimum = 0x10000;
    } else {
      return false;
    }
    if (i + length > text.size()) return false;
    for (size_t k = 1; k < length; ++k) {
      const auto b = static_cast<unsigned char>(text[i + k]);
      if ((b & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (b & 0x3F);
    }
    if (cp < minimum || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
    if (out) out->push_back(cp);
    i += length;
  }
  return true;
}

bool DecodeUtf16(std::string_view bytes, bool big_endian, std::u32string* out) {
  if (bytes.size() % 2 != 0) return false;
  const auto unit = [&](size_t index) {
    const auto hi = static_cast<unsigned char>(bytes[index + (big_endian ? 0 : 1)]);
    const auto lo = static_cast<unsigned char>(bytes[index + (big_endian ? 1 : 0)]);
    return static_cast<char32_t>((hi << 8) | lo);
  };
  for (size_t i = 0; i < bytes.size(); i += 2) {
    const char32_t u = unit(i);
    if (u >= 0xD800 && u <= 0xDBFF) {
      if (i + 2 >= bytes.size()) return false;
      const char32_t low = unit(i + 2);
      if (low < 0xDC00 || low > 0xDFFF) return false;
      out->push_back(0x10000 + ((u - 0xD800) << 10) + (low - 0xDC00));
      i += 2;
    } else if (u >= 0xDC00 && u <= 0xDFFF) {
      return false;
    } else {
      out->push_back(u);
    }
  }
  return true;
}

#ifdef _WIN32
bool DecodeCp932(std::string_view bytes, std::u32string* out) {
  if (bytes.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) return false;
  const int size = static_cast<int>(bytes.size());
  const int wide_size =
      ::MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, bytes.data(), size, nullptr, 0);
  if (wide_size <= 0) return false;
  std::wstring wide(static_cast<size_t>(wide_size), L'\0');
  if (::MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, bytes.data(), size, wide.data(),
                            wide_size) != wide_size)
    return false;
  std::string utf16;
  utf16.reserve(wide.size() * 2);
  for (const wchar_t ch : wide) {
    utf16.push_back(static_cast<char>(ch & 0xFF));
    utf16.push_back(static_cast<char>((ch >> 8) & 0xFF));
  }
  return DecodeUtf16(utf16, false, out);
}
#endif

std::string EncodeUtf8(std::u32string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char32_t cp : text) AppendUtf8(out, cp);
  return out;
}

// ---- Lines and cells (section 6.4) ----

std::vector<std::u32string_view> SplitLines(std::u32string_view text) {
  std::vector<std::u32string_view> lines;
  size_t start = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] != U'\r' && text[i] != U'\n') continue;
    lines.push_back(text.substr(start, i - start));
    if (text[i] == U'\r' && i + 1 < text.size() && text[i + 1] == U'\n') ++i;
    start = i + 1;
  }
  if (start < text.size()) lines.push_back(text.substr(start));
  return lines;
}

std::vector<std::u32string_view> SplitTabs(std::u32string_view line) {
  std::vector<std::u32string_view> cells;
  size_t start = 0;
  for (size_t i = 0; i <= line.size(); ++i) {
    if (i == line.size() || line[i] == U'\t') {
      cells.push_back(line.substr(start, i - start));
      start = i + 1;
    }
  }
  return cells;
}

bool IsTrimmedSpace(char32_t cp) { return cp == U' ' || cp == U'\t' || cp == 0x3000; }

std::u32string_view Trim(std::u32string_view text) {
  while (!text.empty() && IsTrimmedSpace(text.front())) text.remove_prefix(1);
  while (!text.empty() && IsTrimmedSpace(text.back())) text.remove_suffix(1);
  return text;
}

// Katakana U+30A1..U+30F6 to hiragana, as the settings app's add does
// (learning-data-management-spec section 3).
std::u32string FoldKatakana(std::u32string_view text) {
  std::u32string out(text);
  for (auto& cp : out) {
    if (cp >= 0x30A1 && cp <= 0x30F6) cp -= 0x60;
  }
  return out;
}

bool HasControl(std::u32string_view text) {
  return std::any_of(text.begin(), text.end(),
                     [](char32_t cp) { return cp <= 0x1F || cp == 0x7F; });
}

bool StartsWithIgnoringAsciiCase(std::u32string_view text, std::string_view prefix) {
  if (text.size() < prefix.size()) return false;
  for (size_t i = 0; i < prefix.size(); ++i) {
    char32_t a = text[i];
    char32_t b = static_cast<unsigned char>(prefix[i]);
    if (a >= U'A' && a <= U'Z') a += 0x20;
    if (b >= U'A' && b <= U'Z') b += 0x20;
    if (a != b) return false;
  }
  return true;
}

// ---- Part-of-speech names (section 6.5) ----

// Half-width katakana U+FF61..U+FF9F as full-width, in code point order.
constexpr std::u32string_view kHalfWidthKana =
    U"。「」、・ヲァィゥェォャュョッーアイウエオカキクケコサシスセソタチツテトナニヌネノハヒフヘホ"
    U"マミムメモヤユヨラリルレロワン゛゜";
constexpr std::u32string_view kVoiceable = U"カキクケコサシスセソタチツテトハヒフヘホ";
constexpr std::u32string_view kSemiVoiceable = U"ハヒフヘホ";

// Section 6.4: trimmed, full-width letters and digits as ASCII, half-width
// katakana as full-width (a following ﾞ or ﾟ joins the kana before it).
std::string NormalizePosName(std::u32string_view name) {
  name = Trim(name);
  std::u32string out;
  for (const char32_t cp : name) {
    if ((cp >= 0xFF10 && cp <= 0xFF19) || (cp >= 0xFF21 && cp <= 0xFF3A) ||
        (cp >= 0xFF41 && cp <= 0xFF5A)) {
      out.push_back(cp - 0xFEE0);
      continue;
    }
    if (cp >= 0xFF61 && cp <= 0xFF9F) {
      const char32_t full = kHalfWidthKana[cp - 0xFF61];
      // The voiced and semi-voiced kana follow their base kana in Unicode.
      if (full == U'゛' && !out.empty()) {
        char32_t& last = out.back();
        if (last == U'ウ') {
          last = U'ヴ';
          continue;
        }
        if (kVoiceable.find(last) != std::u32string_view::npos) {
          last += 1;
          continue;
        }
      } else if (full == U'゜' && !out.empty()) {
        char32_t& last = out.back();
        if (kSemiVoiceable.find(last) != std::u32string_view::npos) {
          last += 2;
          continue;
        }
      }
      out.push_back(full);
      continue;
    }
    out.push_back(cp);
  }
  return EncodeUtf8(out);
}

struct PosName {
  std::string_view name;
  // The part-of-speech id; empty for a noun.
  std::string_view pos;
  // False for a name the table leaves out (unsupported_pos).
  bool supported;
};

constexpr std::array kMsImePosNames{
    PosName{"名詞", "", true},
    PosName{"副詞的名詞", "", true},
    PosName{"その他自立語", "", true},
    PosName{"慣用句", "", true},
    PosName{"独立語", "", true},
    PosName{"短縮よみ", "", true},
    PosName{"さ変名詞", "noun_sahen", true},
    PosName{"ざ変名詞", "noun_sahen", true},
    PosName{"形容動詞", "noun_adjectival", true},
    PosName{"形動名詞", "noun_adjectival", true},
    PosName{"さ変形動名詞", "noun_adjectival", true},
    PosName{"固有名詞", "proper_noun", true},
    PosName{"人名", "person_name", true},
    PosName{"姓", "surname", true},
    PosName{"名", "given_name", true},
    PosName{"地名その他", "place_name", true},
    PosName{"副詞", "adverb", true},
    PosName{"連体詞", "adnominal", true},
    PosName{"接続詞", "conjunction", true},
    PosName{"感動詞", "interjection", true},
    PosName{"接頭語", "prefix", true},
    PosName{"姓名接頭語", "prefix", true},
    PosName{"地名接頭語", "prefix", true},
    PosName{"接尾語", "suffix", true},
    PosName{"姓名接尾語", "suffix", true},
    PosName{"地名接尾語", "suffix", true},
    PosName{"助数詞", "counter", true},
    PosName{"顔文字", "symbol", true},
    PosName{"か行五段", "verb_godan_ka", true},
    PosName{"が行五段", "verb_godan_ga", true},
    PosName{"さ行五段", "verb_godan_sa", true},
    PosName{"た行五段", "verb_godan_ta", true},
    PosName{"な行五段", "verb_godan_na", true},
    PosName{"ば行五段", "verb_godan_ba", true},
    PosName{"ま行五段", "verb_godan_ma", true},
    PosName{"ら行五段", "verb_godan_ra", true},
    PosName{"あわ行五段", "verb_godan_wa", true},
    PosName{"一段動詞", "verb_ichidan", true},
    PosName{"形容詞", "adjective", true},
    PosName{"抑制単語", "", false},
};

constexpr std::array kGooglePosNames{
    PosName{"品詞なし", "", true},
    PosName{"名詞", "", true},
    PosName{"短縮よみ", "", true},
    PosName{"数", "", true},
    PosName{"アルファベット", "", true},
    PosName{"独立語", "", true},
    PosName{"名詞サ変", "noun_sahen", true},
    PosName{"名詞形動", "noun_adjectival", true},
    PosName{"固有名詞", "proper_noun", true},
    PosName{"人名", "person_name", true},
    PosName{"姓", "surname", true},
    PosName{"名", "given_name", true},
    PosName{"地名", "place_name", true},
    PosName{"組織", "organization", true},
    PosName{"副詞", "adverb", true},
    PosName{"連体詞", "adnominal", true},
    PosName{"接続詞", "conjunction", true},
    PosName{"感動詞", "interjection", true},
    PosName{"接頭語", "prefix", true},
    PosName{"接尾一般", "suffix", true},
    PosName{"接尾人名", "suffix", true},
    PosName{"接尾地名", "suffix", true},
    PosName{"助数詞", "counter", true},
    PosName{"記号", "symbol", true},
    PosName{"顔文字", "symbol", true},
    PosName{"句読点", "symbol", true},
    PosName{"動詞カ行五段", "verb_godan_ka", true},
    PosName{"動詞ガ行五段", "verb_godan_ga", true},
    PosName{"動詞サ行五段", "verb_godan_sa", true},
    PosName{"動詞タ行五段", "verb_godan_ta", true},
    PosName{"動詞ナ行五段", "verb_godan_na", true},
    PosName{"動詞バ行五段", "verb_godan_ba", true},
    PosName{"動詞マ行五段", "verb_godan_ma", true},
    PosName{"動詞ラ行五段", "verb_godan_ra", true},
    PosName{"動詞ワ行五段", "verb_godan_wa", true},
    PosName{"動詞一段", "verb_ichidan", true},
    PosName{"動詞サ変", "verb_sahen", true},
    PosName{"形容詞", "adjective", true},
    PosName{"サジェストのみ", "", false},
    PosName{"抑制単語", "", false},
    PosName{"終助詞", "", false},
    PosName{"動詞ハ行四段", "", false},
    PosName{"動詞カ変", "", false},
    PosName{"動詞ザ変", "", false},
    PosName{"動詞ラ変", "", false},
};

template <size_t N>
const PosName* FindPosName(const std::array<PosName, N>& table, std::string_view name) {
  const auto it = std::find_if(table.begin(), table.end(),
                               [&](const PosName& entry) { return entry.name == name; });
  return it == table.end() ? nullptr : &*it;
}

// ---- Rows ----

enum class RowOutcome { Word, Malformed, TooLong, UnsupportedPos, PosMismatch };

bool ParseInt32Cell(const std::string& cell, std::optional<int32_t>* out) {
  if (cell.empty()) return true;
  char* end = nullptr;
  errno = 0;
  const long long value = std::strtoll(cell.c_str(), &end, 10);
  if (errno != 0 || end == cell.c_str() || *end != '\0' ||
      value < (std::numeric_limits<int32_t>::min)() ||
      value > (std::numeric_limits<int32_t>::max)())
    return false;
  *out = static_cast<int32_t>(value);
  return true;
}

bool ParseDoubleCell(const std::string& cell, std::optional<double>* out) {
  if (cell.empty()) return true;
  std::istringstream stream(cell);
  stream.imbue(std::locale::classic());
  double value = 0.0;
  stream >> value;
  if (stream.fail() || !stream.eof() || !std::isfinite(value)) return false;
  *out = value;
  return true;
}

// The azookey_tsv row structure of userdict import (learning-data-management-spec
// section 13): 2 to 6 cells, cid / mid / weight readable when present.
std::optional<UserWord> ParseAzookeyTsvCells(const std::vector<std::u32string_view>& cells,
                                             std::string* pos_cell) {
  if (cells.size() < 2 || cells.size() > 6) return std::nullopt;
  std::array<std::string, 6> text;
  for (size_t i = 0; i < cells.size(); ++i) text[i] = EncodeUtf8(cells[i]);
  UserWord word;
  if (!ParseInt32Cell(text[2], &word.cid) || !ParseInt32Cell(text[3], &word.mid) ||
      !ParseDoubleCell(text[4], &word.value))
    return std::nullopt;
  *pos_cell = text[5];
  return word;
}

RowOutcome ParseRow(std::u32string_view line, TextImportFormat format, UserWord* out) {
  const auto cells = SplitTabs(line);
  std::optional<std::string> pos;
  bool unsupported_pos = false;
  UserWord word;
  if (format == TextImportFormat::AzookeyTsv) {
    std::string pos_cell;
    auto parsed = ParseAzookeyTsvCells(cells, &pos_cell);
    if (!parsed) return RowOutcome::Malformed;
    word = std::move(*parsed);
    // Section 6.4: an id outside the table is left out, not kept.
    if (!pos_cell.empty()) {
      unsupported_pos = !IsKnownUserDictionaryPos(pos_cell);
      pos = std::move(pos_cell);
    }
  } else {
    if (cells.size() < 3) return RowOutcome::Malformed;
    const auto name = NormalizePosName(cells[2]);
    const PosName* entry = format == TextImportFormat::MsIme ? FindPosName(kMsImePosNames, name)
                                                             : FindPosName(kGooglePosNames, name);
    unsupported_pos = !entry || !entry->supported;
    if (!unsupported_pos && !entry->pos.empty()) pos = std::string(entry->pos);
  }
  // Section 6.4: the reading and the word are checked before the part of speech.
  const auto reading = FoldKatakana(Trim(cells[0]));
  const auto surface = Trim(cells[1]);
  if (reading.empty() || surface.empty() || HasControl(reading) || HasControl(surface))
    return RowOutcome::Malformed;
  if (reading.size() > 64 || surface.size() > 128) return RowOutcome::TooLong;
  if (unsupported_pos) return RowOutcome::UnsupportedPos;
  word.ruby = EncodeUtf8(reading);
  word.word = EncodeUtf8(surface);
  if (pos && CheckUserWordPos(word.word, word.ruby, *pos) != UserWordPosCheck::Ok)
    return RowOutcome::PosMismatch;
  word.pos = std::move(pos);
  *out = std::move(word);
  return RowOutcome::Word;
}

bool IsComment(std::u32string_view line, TextImportFormat format) {
  if (line.empty()) return false;
  if (format == TextImportFormat::MsIme) return line.front() == U'!';
  if (format == TextImportFormat::Google) return line.front() == U'#';
  return false;
}

// Section 6.3, step 1: headers of formats this version does not read.
bool HasForeignHeader(std::u32string_view first) {
  return StartsWithIgnoringAsciiCase(first, "# Gboard Dictionary version:") ||
         StartsWithIgnoringAsciiCase(first, "!!DICUT") ||
         StartsWithIgnoringAsciiCase(first, "!!ATOK_TANGO_TEXT_HEADER");
}

std::optional<TextImportFormat> DetectFormat(const std::vector<std::u32string_view>& lines) {
  const auto first = std::find_if(lines.begin(), lines.end(),
                                  [](std::u32string_view line) { return !line.empty(); });
  if (first == lines.end()) return std::nullopt;
  if (StartsWithIgnoringAsciiCase(*first, "!Microsoft IME")) return TextImportFormat::MsIme;
  const auto data = std::find_if(lines.begin(), lines.end(), [](std::u32string_view line) {
    return !line.empty() && line.front() != U'#';
  });
  if (data == lines.end()) return std::nullopt;
  const auto cells = SplitTabs(*data);
  if (cells.size() >= 3 && FindPosName(kGooglePosNames, NormalizePosName(cells[2])))
    return TextImportFormat::Google;
  std::string pos_cell;
  if (ParseAzookeyTsvCells(cells, &pos_cell)) return TextImportFormat::AzookeyTsv;
  return std::nullopt;
}

}  // namespace

std::string_view TextImportFormatName(TextImportFormat format) {
  switch (format) {
    case TextImportFormat::Auto:
      return "auto";
    case TextImportFormat::MsIme:
      return "msime";
    case TextImportFormat::Google:
      return "google";
    case TextImportFormat::AzookeyTsv:
      return "azookey_tsv";
  }
  return "auto";
}

std::optional<TextImportFormat> ParseTextImportFormat(std::string_view name) {
  for (const auto format : {TextImportFormat::Auto, TextImportFormat::MsIme,
                            TextImportFormat::Google, TextImportFormat::AzookeyTsv}) {
    if (TextImportFormatName(format) == name) return format;
  }
  return std::nullopt;
}

std::string_view TextImportEncodingName(TextImportEncoding encoding) {
  switch (encoding) {
    case TextImportEncoding::Utf8:
      return "utf-8";
    case TextImportEncoding::Utf16Le:
      return "utf-16le";
    case TextImportEncoding::Utf16Be:
      return "utf-16be";
    case TextImportEncoding::Cp932:
      return "cp932";
  }
  return "utf-8";
}

std::string_view TextImportErrorName(TextImportError error) {
  switch (error) {
    case TextImportError::None:
      return "";
    case TextImportError::SourceMissing:
      return "source_missing";
    case TextImportError::TooLarge:
      return "too_large";
    case TextImportError::UnsupportedEncoding:
      return "unsupported_encoding";
    case TextImportError::BadEncoding:
      return "bad_encoding";
    case TextImportError::UnknownFormat:
      return "unknown_format";
    case TextImportError::NoEntries:
      return "no_entries";
    case TextImportError::ReadFailed:
      return "read_failed";
  }
  return "";
}

TextImportParseResult ParseUserDictionaryText(std::string_view bytes, TextImportFormat format) {
  TextImportParseResult result;
  if (bytes.size() > kTextImportMaxBytes) {
    result.error = TextImportError::TooLarge;
    return result;
  }

  // Section 6.2.
  std::u32string text;
  bool decoded = false;
  if (bytes.starts_with("\xEF\xBB\xBF")) {
    result.encoding = TextImportEncoding::Utf8;
    decoded = DecodeUtf8(bytes.substr(3), &text);
  } else if (bytes.starts_with("\xFF\xFE")) {
    result.encoding = TextImportEncoding::Utf16Le;
    decoded = DecodeUtf16(bytes.substr(2), false, &text);
  } else if (bytes.starts_with("\xFE\xFF")) {
    result.encoding = TextImportEncoding::Utf16Be;
    decoded = DecodeUtf16(bytes.substr(2), true, &text);
  } else if (DecodeUtf8(bytes, nullptr)) {
    result.encoding = TextImportEncoding::Utf8;
    decoded = DecodeUtf8(bytes, &text);
  } else {
#ifdef _WIN32
    result.encoding = TextImportEncoding::Cp932;
    decoded = DecodeCp932(bytes, &text);
#else
    result.error = TextImportError::UnsupportedEncoding;
    return result;
#endif
  }
  if (!decoded || text.find(U'\0') != std::u32string::npos) {
    result.error = TextImportError::BadEncoding;
    result.encoding.reset();
    return result;
  }

  const auto lines = SplitLines(text);
  if (lines.size() > kTextImportMaxLines) {
    result.error = TextImportError::TooLarge;
    return result;
  }

  // Section 6.3.
  const auto first = std::find_if(lines.begin(), lines.end(),
                                  [](std::u32string_view line) { return !line.empty(); });
  if (first != lines.end() && HasForeignHeader(*first)) {
    result.error = TextImportError::UnknownFormat;
    return result;
  }
  if (format == TextImportFormat::Auto) {
    const auto detected = DetectFormat(lines);
    if (!detected) {
      result.error = TextImportError::UnknownFormat;
      return result;
    }
    format = *detected;
  } else if (format == TextImportFormat::MsIme &&
             (first == lines.end() || !StartsWithIgnoringAsciiCase(*first, "!Microsoft IME"))) {
    result.error = TextImportError::UnknownFormat;
    return result;
  }
  result.format = format;

  // Sections 6.4 and 6.6: the last row of a key wins.
  std::map<std::pair<std::string, std::string>, size_t> index;
  for (const auto line : lines) {
    if (line.empty() || IsComment(line, format)) continue;
    UserWord word;
    switch (ParseRow(line, format, &word)) {
      case RowOutcome::Word: {
        auto key = std::make_pair(word.ruby, word.word);
        if (const auto it = index.find(key); it != index.end()) {
          result.words[it->second] = std::move(word);
          ++result.duplicates_in_file;
        } else {
          index.emplace(std::move(key), result.words.size());
          result.words.push_back(std::move(word));
        }
        break;
      }
      case RowOutcome::Malformed:
        ++result.skipped.malformed;
        break;
      case RowOutcome::TooLong:
        ++result.skipped.too_long;
        break;
      case RowOutcome::UnsupportedPos:
        ++result.skipped.unsupported_pos;
        break;
      case RowOutcome::PosMismatch:
        ++result.skipped.pos_mismatch;
        break;
    }
  }
  if (result.words.empty()) result.error = TextImportError::NoEntries;
  return result;
}

TextImportError ReadUserDictionaryTextFile(const std::filesystem::path& path, std::string& out,
                                           size_t max_bytes) {
  out.clear();
  std::error_code ec;
  const auto status = std::filesystem::status(path, ec);
  if (ec || !std::filesystem::exists(status)) return TextImportError::SourceMissing;
  if (!std::filesystem::is_regular_file(status)) return TextImportError::ReadFailed;
  std::ifstream input(path, std::ios::binary);
  if (!input) return TextImportError::ReadFailed;
  // At most one byte past the limit is read (section 6.7).
  out.resize(max_bytes + 1);
  input.read(out.data(), static_cast<std::streamsize>(out.size()));
  if (input.bad()) {
    SecureErase(out);
    out.clear();
    return TextImportError::ReadFailed;
  }
  out.resize(static_cast<size_t>(input.gcount()));
  if (out.size() > max_bytes) {
    SecureErase(out);
    out.clear();
    return TextImportError::TooLarge;
  }
  return TextImportError::None;
}

TextImportMergeCounts CountUserDictionaryTextMerge(
    const std::set<std::pair<std::string, std::string>>& keys, const std::vector<UserWord>& words,
    ImportConflictPolicy policy) {
  TextImportMergeCounts counts;
  for (const auto& word : words) {
    if (!keys.contains({word.ruby, word.word})) {
      ++counts.added;
    } else if (policy == ImportConflictPolicy::Overwrite) {
      ++counts.replaced;
    } else {
      ++counts.kept;
    }
  }
  return counts;
}

std::optional<TextImportMergeCounts> MergeUserDictionaryText(UserDictionary& dictionary,
                                                             const std::vector<UserWord>& words,
                                                             ImportConflictPolicy policy,
                                                             size_t max_words) {
  std::set<std::pair<std::string, std::string>> keys;
  for (const auto& word : dictionary.All()) keys.emplace(word.ruby, word.word);
  const auto counts = CountUserDictionaryTextMerge(keys, words, policy);
  if (dictionary.Size() + counts.added > max_words) return std::nullopt;
  for (const auto& word : words) {
    if (!keys.contains({word.ruby, word.word}) || policy == ImportConflictPolicy::Overwrite)
      dictionary.Add(word);
  }
  return counts;
}

}  // namespace azookey::learning
