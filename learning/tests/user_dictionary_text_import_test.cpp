#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

#include "TestByteCrypto.h"
#include "azookey/learning/UserDictionary.h"
#include "azookey/learning/UserDictionaryTextImport.h"

// docs/user-dictionary-pos-spec.md section 6 (DEV-2136). Every fixture is made
// here; no real dictionary of a user is read.
namespace {
namespace al = azookey::learning;
using al::TextImportEncoding;
using al::TextImportError;
using al::TextImportFormat;

// UTF-8 to UTF-16 code units, for building the UTF-16 fixtures.
std::u16string ToUtf16(std::string_view utf8) {
  std::u16string out;
  for (size_t i = 0; i < utf8.size();) {
    const auto b0 = static_cast<unsigned char>(utf8[i]);
    char32_t cp = 0;
    size_t length = 1;
    if (b0 < 0x80) {
      cp = b0;
    } else if ((b0 & 0xE0) == 0xC0) {
      cp = b0 & 0x1F;
      length = 2;
    } else if ((b0 & 0xF0) == 0xE0) {
      cp = b0 & 0x0F;
      length = 3;
    } else {
      cp = b0 & 0x07;
      length = 4;
    }
    for (size_t k = 1; k < length; ++k) cp = (cp << 6) | (utf8[i + k] & 0x3F);
    i += length;
    if (cp >= 0x10000) {
      cp -= 0x10000;
      out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
      out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
    } else {
      out.push_back(static_cast<char16_t>(cp));
    }
  }
  return out;
}

std::string Utf16Bytes(std::string_view utf8, bool big_endian, bool bom) {
  std::string out;
  auto units = ToUtf16(utf8);
  if (bom) units.insert(units.begin(), char16_t{0xFEFF});
  for (const char16_t unit : units) {
    const auto hi = static_cast<char>((unit >> 8) & 0xFF);
    const auto lo = static_cast<char>(unit & 0xFF);
    out.push_back(big_endian ? hi : lo);
    out.push_back(big_endian ? lo : hi);
  }
  return out;
}

const al::UserWord* Find(const al::TextImportParseResult& result, std::string_view ruby,
                         std::string_view word) {
  for (const auto& w : result.words) {
    if (w.ruby == ruby && w.word == word) return &w;
  }
  return nullptr;
}

constexpr std::string_view kMsImeText =
    "!Microsoft IME Dictionary Tool\r\n"
    "!Version:\t1.0\r\n"
    "\r\n"
    "ぐぐる\tググる\tら行五段\tコメント\r\n"
    "あずーきー\tazooKey\t固有名詞\r\n"
    "テスト\t試験\t名詞\r\n"
    "ぐーぐる\tGoogle\tら行五段\r\n"
    "よくせい\t抑制\t抑制単語\r\n"
    "かたこと\tカタコト\r\n";

void ExpectMsImeWords(const al::TextImportParseResult& result) {
  EXPECT_EQ(result.error, TextImportError::None);
  EXPECT_EQ(result.format, TextImportFormat::MsIme);
  ASSERT_EQ(result.words.size(), 3u);
  const auto* gugu = Find(result, "ぐぐる", "ググる");
  ASSERT_NE(gugu, nullptr);
  EXPECT_EQ(gugu->pos, std::optional<std::string>("verb_godan_ra"));
  const auto* azookey = Find(result, "あずーきー", "azooKey");
  ASSERT_NE(azookey, nullptr);
  EXPECT_EQ(azookey->pos, std::optional<std::string>("proper_noun"));
  // The reading's katakana become hiragana; a noun has no id.
  const auto* test = Find(result, "てすと", "試験");
  ASSERT_NE(test, nullptr);
  EXPECT_FALSE(test->pos.has_value());
  EXPECT_FALSE(test->cid.has_value());
  EXPECT_EQ(result.skipped.pos_mismatch, 1u);
  EXPECT_EQ(result.skipped.unsupported_pos, 1u);
  EXPECT_EQ(result.skipped.malformed, 1u);
  EXPECT_EQ(result.skipped.too_long, 0u);
}
}  // namespace

TEST(UserDictionaryTextImportTest, MsImeInUtf16LeWithBomFollowsTheTable) {
  const auto bytes = Utf16Bytes(kMsImeText, false, true);
  const auto result = al::ParseUserDictionaryText(bytes, TextImportFormat::Auto);
  EXPECT_EQ(result.encoding, TextImportEncoding::Utf16Le);
  ExpectMsImeWords(result);
  // The same file named explicitly.
  ExpectMsImeWords(al::ParseUserDictionaryText(bytes, TextImportFormat::MsIme));
}

TEST(UserDictionaryTextImportTest, Utf16BeAndUtf8WithBomAreRead) {
  auto result =
      al::ParseUserDictionaryText(Utf16Bytes(kMsImeText, true, true), TextImportFormat::Auto);
  EXPECT_EQ(result.encoding, TextImportEncoding::Utf16Be);
  ExpectMsImeWords(result);
  result = al::ParseUserDictionaryText(std::string("\xEF\xBB\xBF") + std::string(kMsImeText),
                                       TextImportFormat::Auto);
  EXPECT_EQ(result.encoding, TextImportEncoding::Utf8);
  ExpectMsImeWords(result);
}

TEST(UserDictionaryTextImportTest, GoogleInUtf8WithoutBomFollowsTheTable) {
  // The first row is a part of speech the table leaves out: still Google.
  constexpr std::string_view kText =
      "# comment\n"
      "さじぇ\tサジェ\tサジェストのみ\t\n"
      "ぐぐる\tググる\t動詞ラ行五段\t\n"
      "えもい\tエモい\t形容詞\t\n"
      "えーびーしー\tABC\tｱﾙﾌｧﾍﾞｯﾄ\t\n"
      "そしき\t組織名\t組織\t\n"
      "はしる\t走る\t動詞ハ行四段\t\n"
      "ぐぐる\tググる\t動詞ラ行五段\t後の行\n";
  const auto result = al::ParseUserDictionaryText(kText, TextImportFormat::Auto);
  EXPECT_EQ(result.error, TextImportError::None);
  EXPECT_EQ(result.encoding, TextImportEncoding::Utf8);
  EXPECT_EQ(result.format, TextImportFormat::Google);
  ASSERT_EQ(result.words.size(), 4u);
  EXPECT_EQ(Find(result, "ぐぐる", "ググる")->pos, std::optional<std::string>("verb_godan_ra"));
  EXPECT_EQ(Find(result, "えもい", "エモい")->pos, std::optional<std::string>("adjective"));
  // Half-width katakana in the name, the voiced mark joined.
  ASSERT_NE(Find(result, "えーびーしー", "ABC"), nullptr);
  EXPECT_FALSE(Find(result, "えーびーしー", "ABC")->pos.has_value());
  EXPECT_EQ(Find(result, "そしき", "組織名")->pos, std::optional<std::string>("organization"));
  EXPECT_EQ(result.duplicates_in_file, 1u);
  EXPECT_EQ(result.skipped.unsupported_pos, 2u);
}

TEST(UserDictionaryTextImportTest, AzookeyTsvChecksTheSixthColumnAndKeepsTheNumbers) {
  constexpr std::string_view kText =
      "ぐぐる\tググる\t\t\t\tverb_godan_ra\n"
      "あ\tA\t1285\t501\t-1.5\n"
      "い\tI\t\t\t\tnoun\n"
      "ぐぐる\tGoogle\t\t\t\tverb_godan_ra\n"
      "う\tU\tx\n"
      " カタ \t 語 \n";
  const auto result = al::ParseUserDictionaryText(kText, TextImportFormat::Auto);
  EXPECT_EQ(result.error, TextImportError::None);
  EXPECT_EQ(result.format, TextImportFormat::AzookeyTsv);
  ASSERT_EQ(result.words.size(), 3u);
  EXPECT_EQ(Find(result, "ぐぐる", "ググる")->pos, std::optional<std::string>("verb_godan_ra"));
  const auto* a = Find(result, "あ", "A");
  ASSERT_NE(a, nullptr);
  EXPECT_EQ(a->cid, std::optional<int32_t>(1285));
  EXPECT_EQ(a->mid, std::optional<int32_t>(501));
  EXPECT_EQ(a->value, std::optional<double>(-1.5));
  EXPECT_NE(Find(result, "かた", "語"), nullptr);
  EXPECT_EQ(result.skipped.unsupported_pos, 1u);
  EXPECT_EQ(result.skipped.pos_mismatch, 1u);
  EXPECT_EQ(result.skipped.malformed, 1u);
}

TEST(UserDictionaryTextImportTest, EncodingFailures) {
  // A BOM decides the encoding; the rest must decode under it.
  EXPECT_EQ(al::ParseUserDictionaryText("\xEF\xBB\xBF\xFF\xFE", TextImportFormat::Auto).error,
            TextImportError::BadEncoding);
  EXPECT_EQ(
      al::ParseUserDictionaryText(std::string("\xFF\xFE\x41", 3), TextImportFormat::Auto).error,
      TextImportError::BadEncoding);
  // An unpaired surrogate.
  EXPECT_EQ(al::ParseUserDictionaryText(std::string("\xFF\xFE\x00\xD8\x41\x00", 6),
                                        TextImportFormat::Auto)
                .error,
            TextImportError::BadEncoding);
  // Behind a UTF-8 BOM, an overlong form and an encoded surrogate are not UTF-8.
  EXPECT_EQ(
      al::ParseUserDictionaryText("\xEF\xBB\xBF\xC0\xAF\tA\n", TextImportFormat::Auto).error,
      TextImportError::BadEncoding);
  EXPECT_EQ(
      al::ParseUserDictionaryText("\xEF\xBB\xBF\xED\xA0\x80\tA\n", TextImportFormat::Auto).error,
      TextImportError::BadEncoding);
  // A low surrogate without its high one.
  EXPECT_EQ(al::ParseUserDictionaryText(std::string("\xFF\xFE\x00\xDC\x41\x00", 6),
                                        TextImportFormat::Auto)
                .error,
            TextImportError::BadEncoding);
  // U+0000 in the content.
  EXPECT_EQ(al::ParseUserDictionaryText(std::string("a\tA\0", 4), TextImportFormat::Auto).error,
            TextImportError::BadEncoding);
#ifndef _WIN32
  // No CP932 decoder outside Windows.
  EXPECT_EQ(al::ParseUserDictionaryText("\x82\xA0\tA\n", TextImportFormat::Auto).error,
            TextImportError::UnsupportedEncoding);
#endif
}

#ifdef _WIN32
TEST(UserDictionaryTextImportTest, MsImeInCp932IsReadOnWindows) {
  const auto wide = ToUtf16(kMsImeText);
  const int size =
      ::WideCharToMultiByte(932, 0, reinterpret_cast<const wchar_t*>(wide.data()),
                            static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
  ASSERT_GT(size, 0);
  std::string bytes(static_cast<size_t>(size), '\0');
  ASSERT_EQ(
      ::WideCharToMultiByte(932, 0, reinterpret_cast<const wchar_t*>(wide.data()),
                            static_cast<int>(wide.size()), bytes.data(), size, nullptr, nullptr),
      size);
  const auto result = al::ParseUserDictionaryText(bytes, TextImportFormat::Auto);
  EXPECT_EQ(result.encoding, TextImportEncoding::Cp932);
  ExpectMsImeWords(result);
  // Bytes that are neither UTF-8 nor CP932.
  EXPECT_EQ(al::ParseUserDictionaryText("\x82\tA\n", TextImportFormat::Auto).error,
            TextImportError::BadEncoding);
}
#endif

TEST(UserDictionaryTextImportTest, FormatDetection) {
  for (const auto format : {TextImportFormat::Auto, TextImportFormat::Google,
                            TextImportFormat::AzookeyTsv, TextImportFormat::MsIme}) {
    SCOPED_TRACE(std::string(al::TextImportFormatName(format)));
    EXPECT_EQ(
        al::ParseUserDictionaryText("# Gboard Dictionary version:1\na\tA\tja-JP\n", format).error,
        TextImportError::UnknownFormat);
    EXPECT_EQ(al::ParseUserDictionaryText("!!ATOK_TANGO_TEXT_HEADER_1\na\tA\t名詞\n", format).error,
              TextImportError::UnknownFormat);
    EXPECT_EQ(al::ParseUserDictionaryText("!!dicut\na\tA\t名詞\n", format).error,
              TextImportError::UnknownFormat);
  }
  // Kotoeri's quoted CSV has no TAB.
  EXPECT_EQ(al::ParseUserDictionaryText("\"あ\",\"A\",\"名詞\"\n", TextImportFormat::Auto).error,
            TextImportError::UnknownFormat);
  // An explicit msime needs its header line.
  EXPECT_EQ(al::ParseUserDictionaryText("a\tA\t名詞\n", TextImportFormat::MsIme).error,
            TextImportError::UnknownFormat);
  // The header is matched without regard to ASCII case.
  EXPECT_EQ(
      al::ParseUserDictionaryText("!microsoft ime\na\tA\t名詞\n", TextImportFormat::Auto).format,
      TextImportFormat::MsIme);
  // An explicit format is not detected again.
  const auto as_google = al::ParseUserDictionaryText("a\tA\n", TextImportFormat::Google);
  EXPECT_EQ(as_google.error, TextImportError::NoEntries);
  EXPECT_EQ(as_google.skipped.malformed, 1u);
}

TEST(UserDictionaryTextImportTest, RowsAreCheckedForLengthAndControlCharacters) {
  std::string text = "!Microsoft IME\r";
  text += std::string(65 * 3, '\0');
  for (size_t i = 0; i < 65; ++i) text.replace(15 + i * 3, 3, "あ");
  text += "\tA\t名詞\r";
  text += "い\t" + std::string(129, 'w') + "\t名詞\n";
  text += "う\tU\x01\t名詞\r\n";
  text += "え\tE\t名詞";
  const auto result = al::ParseUserDictionaryText(text, TextImportFormat::Auto);
  EXPECT_EQ(result.error, TextImportError::None);
  EXPECT_EQ(result.skipped.too_long, 2u);
  EXPECT_EQ(result.skipped.malformed, 1u);
  ASSERT_EQ(result.words.size(), 1u);
  EXPECT_EQ(result.words.front().ruby, "え");
}

TEST(UserDictionaryTextImportTest, Limits) {
  EXPECT_EQ(al::ParseUserDictionaryText(std::string(al::kTextImportMaxBytes + 1, 'a'),
                                        TextImportFormat::Auto)
                .error,
            TextImportError::TooLarge);
  // Empty lines count toward the line limit.
  EXPECT_EQ(al::ParseUserDictionaryText("a\tA\n" + std::string(al::kTextImportMaxLines, '\n'),
                                        TextImportFormat::Auto)
                .error,
            TextImportError::TooLarge);
  EXPECT_EQ(
      al::ParseUserDictionaryText("!Microsoft IME\r\n!comment\r\n", TextImportFormat::Auto).error,
      TextImportError::NoEntries);
}

TEST(UserDictionaryTextImportTest, ReadFileStopsPastTheLimit) {
  const auto root = std::filesystem::temp_directory_path() / "azookey_user_dict_text_read";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  std::string out;
  EXPECT_EQ(al::ReadUserDictionaryTextFile(root / "missing.txt", out),
            TextImportError::SourceMissing);
  EXPECT_EQ(al::ReadUserDictionaryTextFile(root, out), TextImportError::ReadFailed);
  {
    std::ofstream file(root / "dict.txt", std::ios::binary);
    file << "0123456789";
  }
  EXPECT_EQ(al::ReadUserDictionaryTextFile(root / "dict.txt", out, 10), TextImportError::None);
  EXPECT_EQ(out, "0123456789");
  EXPECT_EQ(al::ReadUserDictionaryTextFile(root / "dict.txt", out, 9), TextImportError::TooLarge);
  EXPECT_TRUE(out.empty());
  std::filesystem::remove_all(root);
}

// Section 6.6.
TEST(UserDictionaryTextImportTest, MergeKeepsOrReplacesAClashAndChecksTheWordLimit) {
  const auto make = [] {
    al::UserDictionary dict("azookey_user_dict_text_merge.json", &al::test::Crypto());
    al::UserWord existing;
    existing.word = "ググる";
    existing.ruby = "ぐぐる";
    existing.value = -2.0;
    dict.Add(existing);
    return dict;
  };
  std::vector<al::UserWord> words(2);
  words[0].word = "ググる";
  words[0].ruby = "ぐぐる";
  words[0].pos = "verb_godan_ra";
  words[1].word = "試験";
  words[1].ruby = "てすと";

  for (const auto policy : {al::ImportConflictPolicy::Merge, al::ImportConflictPolicy::KeepBoth}) {
    auto dict = make();
    const auto counts = al::MergeUserDictionaryText(dict, words, policy);
    ASSERT_TRUE(counts.has_value());
    EXPECT_EQ(counts->added, 1u);
    EXPECT_EQ(counts->kept, 1u);
    EXPECT_EQ(counts->replaced, 0u);
    EXPECT_EQ(dict.Size(), 2u);
    EXPECT_EQ(dict.Lookup("ぐぐる").front().value, std::optional<double>(-2.0));
    EXPECT_FALSE(dict.Lookup("ぐぐる").front().pos.has_value());
  }
  auto dict = make();
  const auto counts = al::MergeUserDictionaryText(dict, words, al::ImportConflictPolicy::Overwrite);
  ASSERT_TRUE(counts.has_value());
  EXPECT_EQ(counts->added, 1u);
  EXPECT_EQ(counts->replaced, 1u);
  EXPECT_FALSE(dict.Lookup("ぐぐる").front().value.has_value());
  EXPECT_EQ(dict.Lookup("ぐぐる").front().pos, std::optional<std::string>("verb_godan_ra"));

  // The dry run counts the same way from the keys alone.
  const std::set<std::pair<std::string, std::string>> keys{{"ぐぐる", "ググる"}};
  const auto dry = al::CountUserDictionaryTextMerge(keys, words, al::ImportConflictPolicy::Merge);
  EXPECT_EQ(dry.added, 1u);
  EXPECT_EQ(dry.kept, 1u);

  // Past the word limit nothing changes.
  auto full = make();
  EXPECT_FALSE(al::MergeUserDictionaryText(full, words, al::ImportConflictPolicy::Overwrite, 1));
  EXPECT_EQ(full.Size(), 1u);
  EXPECT_EQ(full.Lookup("ぐぐる").front().value, std::optional<double>(-2.0));
  EXPECT_TRUE(al::MergeUserDictionaryText(full, words, al::ImportConflictPolicy::Merge, 2));
}
