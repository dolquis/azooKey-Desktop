#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>

#include "TestByteCrypto.h"
#include "azookey/learning/DpapiCrypto.h"
#include "azookey/learning/UserDictionary.h"
#include "azookey/learning/UserDictionaryPos.h"

namespace {
void RemoveStoreFiles(const std::filesystem::path& path) {
  std::error_code ec;
  std::filesystem::remove(path, ec);
  std::filesystem::remove(azookey::learning::EncryptedPathFor(path), ec);
  auto backup = path;
  backup += ".bak";
  std::filesystem::remove(backup, ec);
}
}  // namespace

TEST(UserDictionaryTest, AddLookupRemove) {
  const std::string p1 =
      (std::filesystem::temp_directory_path() / "azookey_user_dict_t_ignored.json").string();
  azookey::learning::UserDictionary dict(p1, &azookey::learning::test::Crypto());
  azookey::learning::UserWord w1;
  w1.word = "azooKey";
  w1.ruby = "あずきい";
  w1.cid = 1285;
  w1.value = -5.0;

  azookey::learning::UserWord w2;
  w2.word = "azooKey社";
  w2.ruby = "あずきい";

  EXPECT_TRUE(dict.Add(w1));
  EXPECT_TRUE(dict.Add(w2));
  EXPECT_EQ(dict.Size(), 2u);

  const auto hits = dict.Lookup("あずきい");
  EXPECT_EQ(hits.size(), 2u);

  // Adding same (word, ruby) replaces in place rather than duplicating.
  azookey::learning::UserWord w1b = w1;
  w1b.value = -10.0;
  EXPECT_FALSE(dict.Add(w1b));
  EXPECT_EQ(dict.Size(), 2u);
  auto hits2 = dict.Lookup("あずきい");
  bool found = false;
  for (const auto& h : hits2) {
    if (h.word == "azooKey") {
      ASSERT_TRUE(h.value.has_value());
      EXPECT_EQ(*h.value, -10.0);
      found = true;
    }
  }
  EXPECT_TRUE(found);

  EXPECT_TRUE(dict.Remove("azooKey", "あずきい"));
  EXPECT_EQ(dict.Size(), 1u);
  EXPECT_FALSE(dict.Remove("azooKey", "あずきい"));
  EXPECT_EQ(dict.Lookup("あずきい").size(), 1u);
}

TEST(UserDictionaryTest, SaveLoadRoundTrip) {
  const char* path = "azookey_user_dict_roundtrip.json";
  RemoveStoreFiles(path);

  {
    azookey::learning::UserDictionary dict(path, &azookey::learning::test::Crypto());
    azookey::learning::UserWord w;
    w.word = "日本語";
    w.ruby = "にほんご";
    w.cid = 1;
    w.mid = 2;
    w.value = 0.5;
    dict.Add(w);

    azookey::learning::UserWord w2;
    w2.word = "プログラム";
    w2.ruby = "ぷろぐらむ";
    dict.Add(w2);
    EXPECT_TRUE(dict.Save());
  }

  azookey::learning::UserDictionary loaded(path, &azookey::learning::test::Crypto());
  ASSERT_TRUE(loaded.Load());
  EXPECT_EQ(loaded.Size(), 2u);

  auto hits = loaded.Lookup("にほんご");
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_EQ(hits[0].word, "日本語");
  ASSERT_TRUE(hits[0].cid.has_value());
  EXPECT_EQ(*hits[0].cid, 1);
  ASSERT_TRUE(hits[0].value.has_value());
  EXPECT_EQ(*hits[0].value, 0.5);

  auto hits2 = loaded.Lookup("ぷろぐらむ");
  ASSERT_EQ(hits2.size(), 1u);
  EXPECT_FALSE(hits2[0].cid.has_value());
  EXPECT_FALSE(hits2[0].value.has_value());

  RemoveStoreFiles(path);
}

TEST(UserDictionaryTest, ReplaceAllUsesAddSemanticsForDuplicates) {
  const std::string path =
      (std::filesystem::temp_directory_path() / "azookey_user_dict_replace_all.json").string();
  RemoveStoreFiles(path);

  azookey::learning::UserWord first;
  first.word = "azooKey";
  first.ruby = "azookey";
  first.value = -5.0;

  azookey::learning::UserWord replacement = first;
  replacement.value = -10.0;

  azookey::learning::UserWord other;
  other.word = "Nihongo";
  other.ruby = "nihongo";

  azookey::learning::UserDictionary dict(path, &azookey::learning::test::Crypto());
  dict.ReplaceAll({first, other, replacement});
  EXPECT_EQ(dict.Size(), 2u);
  auto hits = dict.Lookup("azookey");
  ASSERT_EQ(hits.size(), 1u);
  ASSERT_TRUE(hits.front().value.has_value());
  EXPECT_DOUBLE_EQ(*hits.front().value, -10.0);
}

TEST(UserDictionaryTest, LoadMissingFileIsOk) {
  const std::string p2 =
      (std::filesystem::temp_directory_path() / "azookey_user_dict_definitely_missing.json")
          .string();
  azookey::learning::UserDictionary dict(p2, &azookey::learning::test::Crypto());
  EXPECT_TRUE(dict.Load());
  EXPECT_EQ(dict.Size(), 0u);
}

TEST(UserDictionaryTest, LoadMalformedRejects) {
  const auto root = std::filesystem::temp_directory_path() / "azookey_user_dict_malformed";
  const auto path = root / "user_dict.json";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  {
    std::ofstream f(path);
    ASSERT_TRUE(f.is_open());
    f << "not json at all";
  }
  azookey::learning::UserDictionary dict(path.string(), &azookey::learning::test::Crypto());
  EXPECT_FALSE(dict.Load());

  size_t corrupt_files = 0;
  for (const auto& entry : std::filesystem::directory_iterator(root)) {
    if (entry.path().filename().string().find(".corrupt.") != std::string::npos) {
      ++corrupt_files;
    }
  }
  EXPECT_EQ(corrupt_files, 1u);

  azookey::learning::UserWord replacement;
  replacement.word = "azooKey";
  replacement.ruby = "あずきい";
  dict.Add(replacement);
  EXPECT_TRUE(dict.Save());
  EXPECT_TRUE(std::filesystem::exists(azookey::learning::EncryptedPathFor(path)));

  std::filesystem::remove_all(root);
}

TEST(UserDictionaryTest, ZeroByteFileLoadsAsEmptyAndMigrates) {
  const auto root = std::filesystem::temp_directory_path() / "azookey_user_dict_zero_byte";
  const auto path = root / "user_dict.json";
  auto backup = path;
  backup += ".bak";
  const auto encrypted = azookey::learning::EncryptedPathFor(path);
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  {
    std::ofstream f(path, std::ios::binary);
    ASSERT_TRUE(f.is_open());
  }

  azookey::learning::UserDictionary read_only(path, &azookey::learning::test::Crypto());
  EXPECT_TRUE(read_only.LoadReadOnly());
  EXPECT_EQ(read_only.Size(), 0u);
  EXPECT_TRUE(std::filesystem::exists(path));
  EXPECT_FALSE(std::filesystem::exists(backup));
  EXPECT_FALSE(std::filesystem::exists(encrypted));

  azookey::learning::UserDictionary dict(path, &azookey::learning::test::Crypto());
  EXPECT_TRUE(dict.Load());
  EXPECT_EQ(dict.Size(), 0u);
  EXPECT_FALSE(std::filesystem::exists(path));
  ASSERT_TRUE(std::filesystem::exists(backup));
  EXPECT_EQ(std::filesystem::file_size(backup), 0u);
  EXPECT_TRUE(std::filesystem::exists(encrypted));
  for (const auto& entry : std::filesystem::directory_iterator(root)) {
    EXPECT_EQ(entry.path().filename().string().find(".corrupt."), std::string::npos);
  }

  azookey::learning::UserDictionary reloaded(path, &azookey::learning::test::Crypto());
  EXPECT_TRUE(reloaded.Load());
  EXPECT_EQ(reloaded.Size(), 0u);
  azookey::learning::UserWord word;
  word.word = "azooKey";
  word.ruby = "あずきい";
  reloaded.Add(word);
  EXPECT_TRUE(reloaded.Save());

  azookey::learning::UserDictionary saved(path, &azookey::learning::test::Crypto());
  ASSERT_TRUE(saved.Load());
  EXPECT_EQ(saved.Size(), 1u);

  std::filesystem::remove_all(root);
}

TEST(UserDictionaryTest, SaveCreatesParentAndLeavesNoTempFile) {
  const auto root = std::filesystem::temp_directory_path() / "azookey_user_dict_atomic_test";
  const auto path = root / "nested" / "user_dict.json";
  std::filesystem::remove_all(root);

  azookey::learning::UserDictionary dict(path.string(), &azookey::learning::test::Crypto());
  azookey::learning::UserWord word;
  word.word = "azooKey";
  word.ruby = "あずきい";
  dict.Add(word);
  EXPECT_TRUE(dict.Save());
  EXPECT_TRUE(std::filesystem::exists(azookey::learning::EncryptedPathFor(path)));

  size_t temp_files = 0;
  for (const auto& entry : std::filesystem::directory_iterator(path.parent_path())) {
    if (entry.path().filename().string().find(".tmp.") != std::string::npos) {
      ++temp_files;
    }
  }
  EXPECT_EQ(temp_files, 0u);

  azookey::learning::UserDictionary loaded(path.string(), &azookey::learning::test::Crypto());
  EXPECT_TRUE(loaded.Load());
  EXPECT_EQ(loaded.Size(), 1u);

  std::filesystem::remove_all(root);
}

#ifdef _WIN32
TEST(UserDictionaryTest, NonAsciiWindowsPathRoundTripsAndPreservesUndecryptableFile) {
  const auto root = std::filesystem::temp_directory_path() / L"azookey_非ASCII_辞書";
  const auto path = root / L"ユーザー辞書.json";
  std::filesystem::remove_all(root);

  azookey::learning::UserDictionary dictionary(path, &azookey::learning::test::Crypto());
  dictionary.Add({"日本語", "にほんご", 1285, 501, -5.0});
  ASSERT_TRUE(dictionary.Save());
  EXPECT_TRUE(std::filesystem::exists(azookey::learning::EncryptedPathFor(path)));

  azookey::learning::UserDictionary loaded(path, &azookey::learning::test::Crypto());
  ASSERT_TRUE(loaded.Load());
  const auto entries = loaded.Lookup("にほんご");
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries.front().word, "日本語");

  {
    std::ofstream output(azookey::learning::EncryptedPathFor(path),
                         std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output);
    output << "not valid json";
  }
  azookey::learning::UserDictionary malformed(path, &azookey::learning::test::Crypto());
  EXPECT_FALSE(malformed.Load());
  EXPECT_TRUE(std::filesystem::exists(azookey::learning::EncryptedPathFor(path)));
  size_t corrupt_files = 0;
  for (const auto& entry : std::filesystem::directory_iterator(root)) {
    if (entry.path().filename().wstring().find(L".corrupt.") != std::wstring::npos) {
      ++corrupt_files;
    }
  }
  EXPECT_EQ(corrupt_files, 0u);
  EXPECT_FALSE(malformed.Save());

  std::filesystem::remove_all(root);
}
#endif

// DEV-1572: ReloadIfChanged skips the read only while memory still holds what
// the encrypted file on disk holds.
TEST(UserDictionaryTest, ReloadIfChangedSkipsOnlyTheFileThisInstanceLastSaved) {
  const auto path = std::filesystem::temp_directory_path() / "azookey_user_dict_reload_if.json";
  RemoveStoreFiles(path);
  azookey::learning::UserDictionary dict(path, &azookey::learning::test::Crypto());
  ASSERT_TRUE(dict.Load());
  dict.Add({"ああ", "ああ"});
  ASSERT_TRUE(dict.Save());

  bool skipped = false;
  ASSERT_TRUE(dict.ReloadIfChanged(&skipped));
  EXPECT_TRUE(skipped);
  // A copy carries what it was copied from.
  auto copy = dict;
  ASSERT_TRUE(copy.ReloadIfChanged(&skipped));
  EXPECT_TRUE(skipped);

  // Another writer (another process, in production) replaces the file with
  // one of the same size, usually within the same timestamp tick.
  {
    azookey::learning::UserDictionary other(path, &azookey::learning::test::Crypto());
    ASSERT_TRUE(other.Load());
    ASSERT_TRUE(other.Remove("ああ", "ああ"));
    other.Add({"いい", "いい"});
    ASSERT_TRUE(other.Save());
  }
  ASSERT_TRUE(dict.ReloadIfChanged(&skipped));
  EXPECT_FALSE(skipped);
  EXPECT_TRUE(dict.Lookup("ああ").empty());
  EXPECT_EQ(dict.Lookup("いい").size(), 1u);
  // The file just read is now the one memory holds.
  ASSERT_TRUE(dict.ReloadIfChanged(&skipped));
  EXPECT_TRUE(skipped);

  // An unsaved change in memory makes the next call read the file again.
  dict.Add({"うう", "うう"});
  ASSERT_TRUE(dict.ReloadIfChanged(&skipped));
  EXPECT_FALSE(skipped);
  EXPECT_TRUE(dict.Lookup("うう").empty());
  RemoveStoreFiles(path);
}

// DEV-1572: a dictionary that never read or wrote the encrypted file always reads it.
TEST(UserDictionaryTest, ReloadIfChangedReadsWithoutAKnownFile) {
  const auto path = std::filesystem::temp_directory_path() / "azookey_user_dict_reload_new.json";
  RemoveStoreFiles(path);
  {
    azookey::learning::UserDictionary writer(path, &azookey::learning::test::Crypto());
    writer.Add({"ええ", "ええ"});
    ASSERT_TRUE(writer.Save());
  }
  azookey::learning::UserDictionary dict(path, &azookey::learning::test::Crypto());
  bool skipped = true;
  ASSERT_TRUE(dict.ReloadIfChanged(&skipped));
  EXPECT_FALSE(skipped);
  EXPECT_EQ(dict.Lookup("ええ").size(), 1u);
  RemoveStoreFiles(path);
}

// docs/user-dictionary-pos-spec.md section 3 (DEV-2135).
namespace {
namespace al = azookey::learning;

std::string ReadBytes(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void WriteBytes(const std::filesystem::path& path, std::string_view bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

size_t CountCorruptFiles(const std::filesystem::path& root) {
  size_t count = 0;
  for (const auto& entry : std::filesystem::directory_iterator(root)) {
    if (entry.path().filename().string().find(".corrupt.") != std::string::npos) ++count;
  }
  return count;
}

al::UserWord Word(std::string word, std::string ruby, std::optional<std::string> pos = {}) {
  al::UserWord w;
  w.word = std::move(word);
  w.ruby = std::move(ruby);
  w.pos = std::move(pos);
  return w;
}
}  // namespace

// Section 3.3: a dictionary without parts of speech keeps the version 1 form,
// byte for byte, of the versions before them.
TEST(UserDictionaryPosTest, WithoutPosWritesTheVersion1FormAsBefore) {
  al::UserDictionary dict("azookey_user_dict_v1_golden.json", &al::test::Crypto());
  dict.Add({"日本語", "にほんご", 1, 2, 0.5});
  dict.Add(Word("ググる", "ぐぐる"));
  EXPECT_EQ(dict.SerializeText(),
            R"({"entries":[{"ruby":"ぐぐる","word":"ググる"},)"
            R"({"cid":1,"mid":2,"ruby":"にほんご","value":0.5,"word":"日本語"}],"version":1})");
}

TEST(UserDictionaryPosTest, PosWritesVersion2AndRoundTrips) {
  const auto root = std::filesystem::temp_directory_path() / "azookey_user_dict_pos_v2";
  const auto path = root / "user_dict.json";
  std::filesystem::remove_all(root);
  {
    al::UserDictionary dict(path, &al::test::Crypto());
    dict.Add(Word("ググる", "ぐぐる", "verb_godan_ra"));
    dict.Add(Word("日本語", "にほんご"));
    EXPECT_EQ(dict.SerializeText(),
              R"({"entries":[{"pos":"verb_godan_ra","ruby":"ぐぐる","word":"ググる"},)"
              R"({"ruby":"にほんご","word":"日本語"}],"version":2})");
    ASSERT_TRUE(dict.Save());
  }
  al::UserDictionary loaded(path, &al::test::Crypto());
  ASSERT_TRUE(loaded.Load());
  const auto hits = loaded.Lookup("ぐぐる");
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_EQ(hits.front().pos, std::optional<std::string>("verb_godan_ra"));
  EXPECT_FALSE(loaded.Lookup("にほんご").front().pos.has_value());
  // Without the word that has a part of speech, version 1 again.
  ASSERT_TRUE(loaded.Remove("ググる", "ぐぐる"));
  EXPECT_EQ(loaded.SerializeText(),
            R"({"entries":[{"ruby":"にほんご","word":"日本語"}],"version":1})");
  std::filesystem::remove_all(root);
}

// Section 3.2: unknown members of the root and of an entry, and a part of
// speech this version does not know, survive a save and a load.
TEST(UserDictionaryPosTest, UnknownMembersAndUnknownPosSurviveSaveAndLoad) {
  const auto root = std::filesystem::temp_directory_path() / "azookey_user_dict_pos_unknown";
  const auto path = root / "user_dict.json";
  std::filesystem::remove_all(root);
  constexpr std::string_view kText =
      R"({"entries":[{"note":"x","pos":"verb_kahen_future","ruby":"くる","weight_v3":1.25,)"
      R"("word":"来る"}],"future_root":{"a":[1,2]},"version":2})";
  {
    al::UserDictionary dict(path, &al::test::Crypto());
    ASSERT_TRUE(dict.LoadText(kText));
    EXPECT_FALSE(dict.too_new());
    const auto hits = dict.Lookup("くる");
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits.front().pos, std::optional<std::string>("verb_kahen_future"));
    EXPECT_EQ(hits.front().extra.size(), 2u);
    ASSERT_TRUE(dict.Save());
  }
  al::UserDictionary loaded(path, &al::test::Crypto());
  ASSERT_TRUE(loaded.Load());
  EXPECT_EQ(loaded.SerializeText(), kText);
  std::filesystem::remove_all(root);
}

// Section 3.2: a `pos` that is not a string is no part of speech; it is kept,
// and with no other part of speech the dictionary is written as version 1.
TEST(UserDictionaryPosTest, NonStringPosIsKeptAndWritesVersion1) {
  al::UserDictionary dict("azookey_user_dict_pos_non_string.json", &al::test::Crypto());
  ASSERT_TRUE(dict.LoadText(R"({"version":2,"entries":[{"word":"a","ruby":"あ","pos":7}]})"));
  const auto hits = dict.Lookup("あ");
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_FALSE(hits.front().pos.has_value());
  EXPECT_EQ(dict.SerializeText(), R"({"entries":[{"pos":7,"ruby":"あ","word":"a"}],"version":1})");
}

// Section 3.2: a version that is not an integer, or is 0 or less, reads as 1.
TEST(UserDictionaryPosTest, VersionThatIsNotAnIntegerOrBelowOneReadsAsVersion1) {
  for (const std::string_view version : {R"("3")", "2.5", "3.5", "0", "-4", "null"}) {
    SCOPED_TRACE(std::string(version));
    al::UserDictionary dict("azookey_user_dict_pos_version.json", &al::test::Crypto());
    const std::string text = std::string(R"({"version":)") + std::string(version) +
                             R"(,"entries":[{"word":"a","ruby":"あ"}]})";
    ASSERT_TRUE(dict.LoadText(text));
    EXPECT_FALSE(dict.too_new());
    EXPECT_FALSE(dict.save_blocked());
    EXPECT_EQ(dict.Size(), 1u);
    EXPECT_EQ(dict.SerializeText(), R"({"entries":[{"ruby":"あ","word":"a"}],"version":1})");
  }
  al::UserDictionary missing("azookey_user_dict_pos_version.json", &al::test::Crypto());
  ASSERT_TRUE(missing.LoadText(R"({"entries":[]})"));
  EXPECT_FALSE(missing.too_new());
}

TEST(UserDictionaryPosTest, LoadTextOfVersion3OrLaterSetsTooNew) {
  for (const std::string_view version : {"3", "3.0", "17", "9223372036854775808", "1e300"}) {
    SCOPED_TRACE(std::string(version));
    al::UserDictionary dict("azookey_user_dict_pos_too_new.json", &al::test::Crypto());
    ASSERT_TRUE(dict.LoadText(std::string(R"({"version":)") + std::string(version) +
                              R"(,"entries":[{"word":"a","ruby":"あ"}]})"));
    EXPECT_TRUE(dict.too_new());
    EXPECT_TRUE(dict.save_blocked());
    EXPECT_EQ(dict.Size(), 1u);
  }
  // Even with entries in a shape this version cannot read.
  al::UserDictionary reshaped("azookey_user_dict_pos_too_new.json", &al::test::Crypto());
  ASSERT_TRUE(reshaped.LoadText(R"({"version":3,"entries":{"shape":"new"}})"));
  EXPECT_TRUE(reshaped.too_new());
  EXPECT_EQ(reshaped.Size(), 0u);
}

// Section 3.2: a dictionary of version 3 or later serves its words, but no save
// or export changes or writes it.
TEST(UserDictionaryPosTest, EncryptedVersion3IsReadButNeverWritten) {
  const auto root = std::filesystem::temp_directory_path() / "azookey_user_dict_pos_v3_enc";
  const auto path = root / "user_dict.json";
  const auto encrypted = al::EncryptedPathFor(path);
  const auto exported = root / "export.json";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  ASSERT_TRUE(al::WriteProtectedText(
      path, R"({"version":3,"entries":[{"word":"ググる","ruby":"ぐぐる","pos":"verb_godan_ra"}]})",
      al::test::Crypto()));
  const auto before = ReadBytes(encrypted);

  al::UserDictionary dict(path, &al::test::Crypto());
  ASSERT_TRUE(dict.Load());
  EXPECT_TRUE(dict.too_new());
  EXPECT_EQ(dict.Lookup("ぐぐる").size(), 1u);
  dict.Add(Word("日本語", "にほんご"));
  EXPECT_FALSE(dict.Save());
  EXPECT_FALSE(dict.SavePlaintextExport(exported));
  EXPECT_FALSE(std::filesystem::exists(exported));
  ASSERT_TRUE(dict.ReloadIfChanged());
  EXPECT_TRUE(dict.too_new());
  EXPECT_EQ(ReadBytes(encrypted), before);
  EXPECT_EQ(CountCorruptFiles(root), 0u);
  std::filesystem::remove_all(root);
}

// Section 3.2: legacy plaintext of version 3 or later is neither migrated nor
// quarantined, even when its structure cannot be read.
TEST(UserDictionaryPosTest, PlaintextVersion3IsNeitherMigratedNorQuarantined) {
  for (const std::string_view text :
       {std::string_view(R"({"version":3,"entries":[{"word":"a","ruby":"あ"}]})"),
        std::string_view(R"({"version":4,"entries":{"shape":"new"}})")}) {
    SCOPED_TRACE(std::string(text));
    const auto root = std::filesystem::temp_directory_path() / "azookey_user_dict_pos_v3_plain";
    const auto path = root / "user_dict.json";
    auto backup = path;
    backup += ".bak";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    WriteBytes(path, text);

    al::UserDictionary dict(path, &al::test::Crypto());
    EXPECT_TRUE(dict.Load());
    EXPECT_TRUE(dict.too_new());
    dict.Add(Word("日本語", "にほんご"));
    EXPECT_FALSE(dict.Save());
    EXPECT_EQ(ReadBytes(path), text);
    EXPECT_FALSE(std::filesystem::exists(al::EncryptedPathFor(path)));
    EXPECT_FALSE(std::filesystem::exists(backup));
    EXPECT_EQ(CountCorruptFiles(root), 0u);
    std::filesystem::remove_all(root);
  }
}

// Section 3.4: Add replaces the word with its part of speech and unknown members.
TEST(UserDictionaryPosTest, AddReplacesPosAndUnknownMembers) {
  al::UserDictionary dict("azookey_user_dict_pos_add.json", &al::test::Crypto());
  ASSERT_TRUE(
      dict.LoadText(R"({"version":2,"entries":[)"
                    R"({"word":"ググる","ruby":"ぐぐる","pos":"verb_godan_ra","note":1}]})"));
  EXPECT_FALSE(dict.Add(Word("ググる", "ぐぐる")));
  const auto hits = dict.Lookup("ぐぐる");
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_FALSE(hits.front().pos.has_value());
  EXPECT_TRUE(hits.front().extra.empty());
}

// Section 3.4: a backup import keeps or replaces a word together with its part of speech.
TEST(UserDictionaryPosTest, MergeTreatsPosAsPartOfTheWord) {
  auto imported_word = Word("ググる", "ぐぐる");
  imported_word.value = -1.0;
  al::UserDictionary imported("azookey_user_dict_pos_merge_in.json", &al::test::Crypto());
  imported.Add(imported_word);

  for (const auto policy : {al::ImportConflictPolicy::Merge, al::ImportConflictPolicy::KeepBoth}) {
    al::UserDictionary local("azookey_user_dict_pos_merge.json", &al::test::Crypto());
    local.Add(Word("ググる", "ぐぐる", "verb_godan_ra"));
    local.Merge(imported, policy);
    const auto hits = local.Lookup("ぐぐる");
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits.front().pos, std::optional<std::string>("verb_godan_ra"));
    EXPECT_FALSE(hits.front().value.has_value());
  }
  al::UserDictionary local("azookey_user_dict_pos_merge.json", &al::test::Crypto());
  local.Add(Word("ググる", "ぐぐる", "verb_godan_ra"));
  local.Merge(imported, al::ImportConflictPolicy::Overwrite);
  const auto hits = local.Lookup("ぐぐる");
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_FALSE(hits.front().pos.has_value());
  EXPECT_EQ(hits.front().value, std::optional<double>(-1.0));
}

TEST(UserDictionaryPosTest, PosIdsFollowTheTableOfSection2_1) {
  const auto& ids = al::UserDictionaryPosIds();
  ASSERT_EQ(ids.size(), 28u);
  EXPECT_EQ(ids.front(), "noun_sahen");
  EXPECT_EQ(ids.back(), "adjective");
  for (const auto id : ids) EXPECT_TRUE(al::IsKnownUserDictionaryPos(id)) << id;
  EXPECT_FALSE(al::IsKnownUserDictionaryPos(""));
  EXPECT_FALSE(al::IsKnownUserDictionaryPos("noun"));
  EXPECT_TRUE(al::IsConjugatingUserDictionaryPos("verb_godan_wa"));
  EXPECT_TRUE(al::IsConjugatingUserDictionaryPos("adjective"));
  EXPECT_FALSE(al::IsConjugatingUserDictionaryPos("noun_sahen"));
}

// Sections 4.3 and 4.4.
TEST(UserDictionaryPosTest, CheckUserWordPosAppliesSection4_4) {
  using al::UserWordPosCheck;
  struct Case {
    const char* word;
    const char* ruby;
    const char* pos;
    UserWordPosCheck expected;
  };
  const Case cases[] = {
      {"ググる", "ぐぐる", "verb_godan_ra", UserWordPosCheck::Ok},
      {"Google", "ぐぐる", "verb_godan_ra", UserWordPosCheck::PosMismatch},
      {"ググル", "ぐぐる", "verb_godan_ra", UserWordPosCheck::PosMismatch},
      {"ググる", "ぐぐる", "verb_godan_ka", UserWordPosCheck::PosMismatch},
      {"る", "る", "verb_godan_ra", UserWordPosCheck::PosMismatch},
      {"書く", "かく", "verb_godan_ka", UserWordPosCheck::Ok},
      {"笑う", "わらう", "verb_godan_wa", UserWordPosCheck::Ok},
      {"食べる", "たべる", "verb_ichidan", UserWordPosCheck::Ok},
      {"見る", "みる", "verb_ichidan", UserWordPosCheck::Ok},
      {"考える", "かんがえる", "verb_ichidan", UserWordPosCheck::Ok},
      {"ぺる", "ぺる", "verb_ichidan", UserWordPosCheck::PosMismatch},
      {"ゃる", "ゃる", "verb_ichidan", UserWordPosCheck::PosMismatch},
      {"ググる", "ぐぐる", "verb_ichidan", UserWordPosCheck::PosMismatch},
      {"る", "る", "verb_ichidan", UserWordPosCheck::PosMismatch},
      {"察する", "さっする", "verb_sahen", UserWordPosCheck::Ok},
      {"する", "する", "verb_sahen", UserWordPosCheck::PosMismatch},
      {"エモい", "えもい", "adjective", UserWordPosCheck::Ok},
      {"エモイ", "えもい", "adjective", UserWordPosCheck::PosMismatch},
      {"azooKey", "あずーきー", "proper_noun", UserWordPosCheck::Ok},
      {"azooKey", "あずーきー", "noun", UserWordPosCheck::UnknownPos},
      {"azooKey", "あずーきー", "", UserWordPosCheck::UnknownPos},
  };
  for (const auto& c : cases) {
    EXPECT_EQ(al::CheckUserWordPos(c.word, c.ruby, c.pos), c.expected)
        << c.word << " / " << c.ruby << " / " << c.pos;
  }
}
