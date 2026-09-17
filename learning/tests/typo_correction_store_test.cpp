#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "azookey/learning/TypoCorrectionStore.h"

namespace {

using azookey::learning::kTypoCorrectionMaxReadingLength;
using azookey::learning::TypoCorrectionStore;

constexpr uint64_t kNow = 1'700'000'000;
constexpr uint64_t kDay = 24 * 60 * 60;

std::filesystem::path TempPath(const char* name) {
  auto path = std::filesystem::temp_directory_path() / name;
  std::error_code ec;
  std::filesystem::remove(path, ec);
  return path;
}

void WriteFile(const std::filesystem::path& path, const std::string& content) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << content;
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}  // namespace

TEST(TypoCorrectionStoreTest, CountsPairsAndAppliesOnlyAtMinCount) {
  TypoCorrectionStore store(TempPath("azookey_typo_count.tsv"));

  EXPECT_TRUE(store.Observe("こんちには", "こんにちは", kNow));
  EXPECT_FALSE(store.Lookup("こんちには", 3).has_value());
  EXPECT_TRUE(store.Observe("こんちには", "こんにちは", kNow));
  EXPECT_FALSE(store.Lookup("こんちには", 3).has_value());

  // The boundary count is enough: spec section 6 excludes counts *below*
  // typo_min_count, not counts equal to it.
  EXPECT_TRUE(store.Observe("こんちには", "こんにちは", kNow));
  const auto hit = store.Lookup("こんちには", 3);
  ASSERT_TRUE(hit.has_value());
  EXPECT_EQ(*hit, "こんにちは");
  EXPECT_EQ(store.size(), 1u);
}

TEST(TypoCorrectionStoreTest, LookupPrefersTheMostFrequentCorrection) {
  TypoCorrectionStore store(TempPath("azookey_typo_frequent.tsv"));
  for (int i = 0; i < 3; ++i) store.Observe("かんじへんかん", "かんじへんかんき", kNow);
  for (int i = 0; i < 5; ++i) store.Observe("かんじへんかん", "かんじへんこう", kNow);

  const auto hit = store.Lookup("かんじへんかん", 3);
  ASSERT_TRUE(hit.has_value());
  EXPECT_EQ(*hit, "かんじへんこう");
}

TEST(TypoCorrectionStoreTest, RejectsPairsOutsideTheAcceptFilters) {
  TypoCorrectionStore store(TempPath("azookey_typo_filters.tsv"));

  // Identical readings carry no correction.
  EXPECT_FALSE(store.Observe("あいうえお", "あいうえお", kNow));
  // One code point cannot be told apart from a candidate mis-selection.
  EXPECT_FALSE(store.Observe("い", "え", kNow));
  // An abandoned input leaves the committed reading empty.
  EXPECT_FALSE(store.Observe("あいう", "", kNow));
  EXPECT_FALSE(store.Observe("", "あいう", kNow));
  // Too far apart to be one mistyped key: four edits on a five-character
  // reading exceeds both the relative and the absolute limit.
  EXPECT_FALSE(store.Observe("あいうえお", "かきくけお", kNow));

  EXPECT_EQ(store.size(), 0u);
}

TEST(TypoCorrectionStoreTest, EditDistanceLimitScalesWithLengthAndIsCapped) {
  // max(1, ceil(len * 0.34)), capped at 3.
  EXPECT_EQ(TypoCorrectionStore::EditDistanceLimit(2), 1u);
  EXPECT_EQ(TypoCorrectionStore::EditDistanceLimit(3), 2u);
  EXPECT_EQ(TypoCorrectionStore::EditDistanceLimit(6), 3u);
  EXPECT_EQ(TypoCorrectionStore::EditDistanceLimit(40), 3u);
}

TEST(TypoCorrectionStoreTest, EditDistanceCountsCodePointsNotBytes) {
  // Each kana is three UTF-8 bytes; a byte-wise distance would report 3 here.
  EXPECT_EQ(TypoCorrectionStore::Utf8EditDistance("あい", "あう"), 1u);
  EXPECT_EQ(TypoCorrectionStore::Utf8CharLength("あいうえお"), 5u);
  EXPECT_EQ(TypoCorrectionStore::Utf8EditDistance("あいう", "あいう"), 0u);
  EXPECT_EQ(TypoCorrectionStore::Utf8EditDistance("", "あい"), 2u);
  // Transposition costs two edits under Levenshtein.
  EXPECT_EQ(TypoCorrectionStore::Utf8EditDistance("こんちには", "こんにちは"), 2u);
}

TEST(TypoCorrectionStoreTest, IgnoresRecordsThatAreTooOld) {
  TypoCorrectionStore store(TempPath("azookey_typo_age.tsv"));
  const uint64_t long_ago = kNow - 200 * kDay;
  for (int i = 0; i < 3; ++i) store.Observe("こんちには", "こんにちは", long_ago);

  // Without a clock the record still applies; with one it is past the cutoff.
  EXPECT_TRUE(store.Lookup("こんちには", 3).has_value());
  EXPECT_FALSE(store.Lookup("こんちには", 3, kNow).has_value());

  // A fresh observation revives it.
  store.Observe("こんちには", "こんにちは", kNow);
  EXPECT_TRUE(store.Lookup("こんちには", 3, kNow).has_value());
}

TEST(TypoCorrectionStoreTest, SaveLoadRoundTrip) {
  const auto path = TempPath("azookey_typo_roundtrip.tsv");
  {
    TypoCorrectionStore store(path);
    for (int i = 0; i < 4; ++i) store.Observe("こんちには", "こんにちは", kNow);
    store.Observe("あいさつ", "あいさい", kNow);
    ASSERT_TRUE(store.Save());
  }

  TypoCorrectionStore reloaded(path);
  ASSERT_TRUE(reloaded.Load());
  EXPECT_EQ(reloaded.size(), 2u);
  const auto entries = reloaded.All();
  ASSERT_EQ(entries.size(), 2u);
  const auto hit = reloaded.Lookup("こんちには", 4, kNow);
  ASSERT_TRUE(hit.has_value());
  EXPECT_EQ(*hit, "こんにちは");
  // The timestamp survives the round trip, so the age filter still works.
  EXPECT_FALSE(reloaded.Lookup("こんちには", 4, kNow + 200 * kDay).has_value());

  std::error_code ec;
  std::filesystem::remove(path, ec);
}

TEST(TypoCorrectionStoreTest, RoundTripsReadingsContainingTabsAndNewlines) {
  const auto path = TempPath("azookey_typo_escape.tsv");
  // Not a reading a TIP would send, but an unescaped tab would silently split
  // the row into the wrong columns.
  const std::string wrong = "あい\tう";
  const std::string correct = "あい\nう";
  {
    TypoCorrectionStore store(path);
    ASSERT_TRUE(store.Observe(wrong, correct, kNow));
    ASSERT_TRUE(store.Save());
  }

  TypoCorrectionStore reloaded(path);
  ASSERT_TRUE(reloaded.Load());
  ASSERT_EQ(reloaded.size(), 1u);
  const auto entries = reloaded.All();
  EXPECT_EQ(entries[0].wrong_reading, wrong);
  EXPECT_EQ(entries[0].correct_reading, correct);

  std::error_code ec;
  std::filesystem::remove(path, ec);
}

TEST(TypoCorrectionStoreTest, MissingFileLoadsAsEmptyStore) {
  const auto path = TempPath("azookey_typo_missing.tsv");
  TypoCorrectionStore store(path);
  EXPECT_TRUE(store.Load());
  EXPECT_EQ(store.size(), 0u);
}

TEST(TypoCorrectionStoreTest, SkipsCorruptRowsAndKeepsTheRest) {
  const auto path = TempPath("azookey_typo_corrupt.tsv");
  WriteFile(path,
            "# azookey-typo-correction-tsv escaped=1\n"
            "こんちには\tこんにちは\t4 1700000000\n"
            "missing-columns\n"
            "あいうえ\tあいうお\tnot-a-number 1700000000\n"
            "あいうえ\tあいうお\t3\n"
            "かきくけ\tかきくこ\t0 1700000000\n"
            "ばらばら\tぜんぜんちがう\t9 1700000000\n"
            "さしすせ\tさしすそ\t2 1700000000\n");

  TypoCorrectionStore store(path);
  // A corrupt file is not a failed load: the readable rows are kept.
  EXPECT_TRUE(store.Load());
  // The malformed rows, the zero count, and the pair that no longer passes the
  // accept filters are all dropped.
  EXPECT_EQ(store.size(), 2u);
  EXPECT_TRUE(store.Lookup("こんちには", 4, kNow).has_value());
  EXPECT_TRUE(store.Lookup("さしすせ", 2, kNow).has_value());
  EXPECT_FALSE(store.Lookup("あいうえ", 1, kNow).has_value());
  EXPECT_FALSE(store.Lookup("かきくけ", 1, kNow).has_value());
  EXPECT_FALSE(store.Lookup("ばらばら", 1, kNow).has_value());

  std::error_code ec;
  std::filesystem::remove(path, ec);
}

TEST(TypoCorrectionStoreTest, SavedFileCarriesTheEscapedHeader) {
  const auto path = TempPath("azookey_typo_header.tsv");
  TypoCorrectionStore store(path);
  store.Observe("こんちには", "こんにちは", kNow);
  ASSERT_TRUE(store.Save());

  const auto content = ReadFile(path);
  EXPECT_EQ(content.rfind(std::string(azookey::learning::kTypoCorrectionStoreEscapedTsvHeader), 0),
            0u);

  std::error_code ec;
  std::filesystem::remove(path, ec);
}

TEST(TypoCorrectionStoreTest, ResetClearsTheTable) {
  TypoCorrectionStore store(TempPath("azookey_typo_reset.tsv"));
  for (int i = 0; i < 3; ++i) store.Observe("こんちには", "こんにちは", kNow);
  ASSERT_EQ(store.size(), 1u);

  store.Reset();
  EXPECT_EQ(store.size(), 0u);
  EXPECT_FALSE(store.Lookup("こんちには", 1, kNow).has_value());
}

TEST(TypoCorrectionStoreTest, RejectsOverlongReadingsWithoutRunningTheDistanceTable) {
  TypoCorrectionStore store(TempPath("azookey_typo_overlong.tsv"));

  // Readings arrive from the TIP over IPC, where one frame can carry a megabyte.
  // The accept filter is quadratic, so an over-long pair has to be refused on
  // length alone rather than after building the table.
  std::string long_reading;
  for (size_t i = 0; i < kTypoCorrectionMaxReadingLength + 1; ++i) long_reading += "あ";
  std::string long_variant = long_reading;
  long_variant += "い";
  EXPECT_FALSE(store.Observe(long_reading, long_variant, kNow));

  // A pair whose lengths differ by more than the limit is refused too.
  EXPECT_FALSE(store.Observe("あい", "あいうえおかき", kNow));

  // Just inside the cap still works.
  std::string at_cap;
  for (size_t i = 0; i < kTypoCorrectionMaxReadingLength; ++i) at_cap += "あ";
  std::string at_cap_variant = at_cap;
  at_cap_variant.replace(at_cap_variant.size() - 3, 3, "い");
  EXPECT_TRUE(store.Observe(at_cap, at_cap_variant, kNow));
  EXPECT_EQ(store.size(), 1u);
}

TEST(TypoCorrectionStoreTest, RoundTripsReadingsThatStartWithAComment) {
  const auto path = TempPath("azookey_typo_hash.tsv");
  // Load() treats a leading '#' as a comment, so an unescaped one would drop the
  // whole record on the next read.
  const std::string wrong = "#あい";
  const std::string correct = "#あう";
  {
    TypoCorrectionStore store(path);
    ASSERT_TRUE(store.Observe(wrong, correct, kNow));
    ASSERT_TRUE(store.Save());
  }

  TypoCorrectionStore reloaded(path);
  ASSERT_TRUE(reloaded.Load());
  ASSERT_EQ(reloaded.size(), 1u);
  const auto entries = reloaded.All();
  EXPECT_EQ(entries[0].wrong_reading, wrong);
  EXPECT_EQ(entries[0].correct_reading, correct);

  std::error_code ec;
  std::filesystem::remove(path, ec);
}
