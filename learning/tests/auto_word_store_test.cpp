#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "azookey/learning/AutoWordStore.h"

namespace {

using azookey::learning::AutoWord;
using azookey::learning::AutoWordSource;
using azookey::learning::AutoWordState;
using azookey::learning::AutoWordStore;

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

}  // namespace

TEST(AutoWordStoreTest, ObserveAddsPendingThenCountsRepeats) {
  AutoWordStore store(TempPath("azookey_auto_word_observe.tsv"));

  EXPECT_FALSE(store.Observe("あずきー", "あずきー", kNow, 3, false));
  EXPECT_EQ(store.Size(), 1u);
  auto pending = store.ListByState(AutoWordState::Pending);
  ASSERT_EQ(pending.size(), 1u);
  EXPECT_EQ(pending[0].count, 1u);
  EXPECT_EQ(pending[0].source, AutoWordSource::Mining);
  EXPECT_EQ(pending[0].first_seen_epoch, kNow);

  EXPECT_FALSE(store.Observe("あずきー", "あずきー", kNow + 10, 3, false));
  pending = store.ListByState(AutoWordState::Pending);
  ASSERT_EQ(pending.size(), 1u);
  EXPECT_EQ(pending[0].count, 2u);
  // first_seen is when the word appeared; last_seen tracks the latest sighting.
  EXPECT_EQ(pending[0].first_seen_epoch, kNow);
  EXPECT_EQ(pending[0].last_seen_epoch, kNow + 10);
}

TEST(AutoWordStoreTest, ConfirmModeNeverPromotesAutomatically) {
  AutoWordStore store(TempPath("azookey_auto_word_confirm_mode.tsv"));

  // auto_promote = false is registrationMode "confirm": the threshold is never
  // consulted, however many times the word is typed.
  for (int i = 0; i < 10; ++i) {
    EXPECT_FALSE(store.Observe("あずきー", "あずきー", kNow, 3, false));
  }
  EXPECT_EQ(store.ListByState(AutoWordState::Confirmed).size(), 0u);
  ASSERT_EQ(store.ListByState(AutoWordState::Pending).size(), 1u);
  EXPECT_EQ(store.ListByState(AutoWordState::Pending)[0].count, 10u);
}

TEST(AutoWordStoreTest, AutoModePromotesAtTheThreshold) {
  AutoWordStore store(TempPath("azookey_auto_word_auto_mode.tsv"));

  EXPECT_FALSE(store.Observe("あずきー", "あずきー", kNow, 3, true));
  EXPECT_FALSE(store.Observe("あずきー", "あずきー", kNow, 3, true));
  EXPECT_EQ(store.ListByState(AutoWordState::Confirmed).size(), 0u);

  // count == threshold promotes; the call that promotes returns true.
  EXPECT_TRUE(store.Observe("あずきー", "あずきー", kNow, 3, true));
  EXPECT_EQ(store.ListByState(AutoWordState::Confirmed).size(), 1u);
  EXPECT_EQ(store.ListByState(AutoWordState::Pending).size(), 0u);

  // Already confirmed: a further sighting is not a second promotion.
  EXPECT_FALSE(store.Observe("あずきー", "あずきー", kNow, 3, true));
}

TEST(AutoWordStoreTest, RejectedWordsAreNeverOfferedAgain) {
  AutoWordStore store(TempPath("azookey_auto_word_rejected.tsv"));
  store.Observe("あずきー", "あずきー", kNow, 3, false);
  ASSERT_TRUE(store.Reject("あずきー", "あずきー"));

  // The count does not move either, so it cannot climb back over a threshold.
  const auto before = store.ListByState(AutoWordState::Rejected);
  ASSERT_EQ(before.size(), 1u);
  for (int i = 0; i < 5; ++i) {
    EXPECT_FALSE(store.Observe("あずきー", "あずきー", kNow, 1, true));
  }
  const auto after = store.ListByState(AutoWordState::Rejected);
  ASSERT_EQ(after.size(), 1u);
  EXPECT_EQ(after[0].count, before[0].count);
  EXPECT_EQ(store.ListByState(AutoWordState::Pending).size(), 0u);
  EXPECT_EQ(store.ListByState(AutoWordState::Confirmed).size(), 0u);
  // The record is kept rather than erased; that record is what blocks re-mining.
  EXPECT_EQ(store.Size(), 1u);
}

TEST(AutoWordStoreTest, ConfirmRejectAndLookupConfirmed) {
  AutoWordStore store(TempPath("azookey_auto_word_states.tsv"));
  store.Observe("あずきー", "あずきー", kNow, 3, false);
  store.Observe("あずきー社", "あずきー", kNow, 3, false);

  // Pending words never reach candidate lookup.
  EXPECT_EQ(store.LookupConfirmed("あずきー").size(), 0u);

  EXPECT_TRUE(store.Confirm("あずきー", "あずきー"));
  // Confirming twice reports "nothing changed".
  EXPECT_FALSE(store.Confirm("あずきー", "あずきー"));
  // An unknown key cannot be resolved.
  EXPECT_FALSE(store.Confirm("しらないご", "しらないご"));

  const auto confirmed = store.LookupConfirmed("あずきー");
  ASSERT_EQ(confirmed.size(), 1u);
  EXPECT_EQ(confirmed[0].surface, "あずきー");
}

TEST(AutoWordStoreTest, IngestTrendingSkipsRejectedAndKeepsMiningSource) {
  AutoWordStore store(TempPath("azookey_auto_word_trending.tsv"));
  store.Observe("あずきー", "あずきー", kNow, 3, false);
  store.Observe("きゃくたい", "きゃくたい", kNow, 3, false);
  ASSERT_TRUE(store.Reject("きゃくたい", "きゃくたい"));

  std::vector<AutoWord> batch;
  AutoWord mined_collision;
  mined_collision.surface = "あずきー";
  mined_collision.reading = "あずきー";
  mined_collision.source = AutoWordSource::Trending;
  mined_collision.score = 0.9;
  batch.push_back(mined_collision);

  AutoWord rejected_collision;
  rejected_collision.surface = "きゃくたい";
  rejected_collision.reading = "きゃくたい";
  rejected_collision.source = AutoWordSource::Trending;
  batch.push_back(rejected_collision);

  AutoWord fresh;
  fresh.surface = "しんごたんご";
  fresh.reading = "しんごたんご";
  fresh.source = AutoWordSource::Trending;
  batch.push_back(fresh);

  AutoWord no_reading;
  no_reading.surface = "よみなし";
  batch.push_back(no_reading);

  store.IngestTrending(batch, kNow + 100, false);

  // Local observation wins the source on a collision.
  const auto pending = store.ListByState(AutoWordState::Pending);
  bool saw_mined = false;
  bool saw_fresh = false;
  for (const auto& word : pending) {
    if (word.surface == "あずきー") {
      EXPECT_EQ(word.source, AutoWordSource::Mining);
      saw_mined = true;
    }
    if (word.surface == "しんごたんご") {
      EXPECT_EQ(word.source, AutoWordSource::Trending);
      saw_fresh = true;
    }
  }
  EXPECT_TRUE(saw_mined);
  EXPECT_TRUE(saw_fresh);
  // The rejected word stays rejected, and the entry with no reading is dropped.
  EXPECT_EQ(store.ListByState(AutoWordState::Rejected).size(), 1u);
  EXPECT_EQ(store.Size(), 3u);
}

TEST(AutoWordStoreTest, PrunePendingDropsOnlyStalePendingWords) {
  AutoWordStore store(TempPath("azookey_auto_word_prune.tsv"));
  store.Observe("ふるいご", "ふるいご", kNow - 100 * kDay, 3, false);
  store.Observe("あたらしいご", "あたらしいご", kNow, 3, false);
  store.Observe("かくていご", "かくていご", kNow - 100 * kDay, 3, false);
  ASSERT_TRUE(store.Confirm("かくていご", "かくていご"));
  store.Observe("きゃくたいご", "きゃくたいご", kNow - 100 * kDay, 3, false);
  ASSERT_TRUE(store.Reject("きゃくたいご", "きゃくたいご"));

  EXPECT_EQ(store.PrunePending(kNow, azookey::learning::kAutoWordDefaultPendingMaxAgeSec), 1u);
  // Confirmed and rejected words survive regardless of age.
  EXPECT_EQ(store.Size(), 3u);
  EXPECT_EQ(store.ListByState(AutoWordState::Pending).size(), 1u);
  EXPECT_EQ(store.ListByState(AutoWordState::Confirmed).size(), 1u);
  EXPECT_EQ(store.ListByState(AutoWordState::Rejected).size(), 1u);
}

TEST(AutoWordStoreTest, SaveLoadRoundTrip) {
  const auto path = TempPath("azookey_auto_word_roundtrip.tsv");
  {
    AutoWordStore store(path);
    store.Observe("あずきー", "あずきー", kNow, 3, false);
    store.Observe("あずきー", "あずきー", kNow + 5, 3, false);
    store.Observe("かくていご", "かくていご", kNow, 3, false);
    ASSERT_TRUE(store.Confirm("かくていご", "かくていご"));
    store.Observe("きゃくたいご", "きゃくたいご", kNow, 3, false);
    ASSERT_TRUE(store.Reject("きゃくたいご", "きゃくたいご"));
    ASSERT_TRUE(store.Save());
  }

  AutoWordStore reloaded(path);
  ASSERT_TRUE(reloaded.Load());
  EXPECT_EQ(reloaded.Size(), 3u);
  const auto pending = reloaded.ListByState(AutoWordState::Pending);
  ASSERT_EQ(pending.size(), 1u);
  EXPECT_EQ(pending[0].surface, "あずきー");
  EXPECT_EQ(pending[0].count, 2u);
  EXPECT_EQ(pending[0].first_seen_epoch, kNow);
  EXPECT_EQ(pending[0].last_seen_epoch, kNow + 5);
  EXPECT_EQ(reloaded.LookupConfirmed("かくていご").size(), 1u);
  // A rejected word survives the round trip, so it stays blocked after restart.
  ASSERT_EQ(reloaded.ListByState(AutoWordState::Rejected).size(), 1u);
  EXPECT_FALSE(reloaded.Observe("きゃくたいご", "きゃくたいご", kNow, 1, true));

  std::error_code ec;
  std::filesystem::remove(path, ec);
}

TEST(AutoWordStoreTest, RoundTripsSurfacesContainingTabs) {
  const auto path = TempPath("azookey_auto_word_escape.tsv");
  const std::string surface = "あず\tきー";
  {
    AutoWordStore store(path);
    store.Observe(surface, "あずきー", kNow, 3, false);
    ASSERT_TRUE(store.Save());
  }

  AutoWordStore reloaded(path);
  ASSERT_TRUE(reloaded.Load());
  const auto pending = reloaded.ListByState(AutoWordState::Pending);
  ASSERT_EQ(pending.size(), 1u);
  EXPECT_EQ(pending[0].surface, surface);

  std::error_code ec;
  std::filesystem::remove(path, ec);
}

TEST(AutoWordStoreTest, MissingFileLoadsAsEmptyStore) {
  AutoWordStore store(TempPath("azookey_auto_word_missing.tsv"));
  EXPECT_TRUE(store.Load());
  EXPECT_EQ(store.Size(), 0u);
}

TEST(AutoWordStoreTest, SkipsCorruptRowsAndKeepsTheRest) {
  const auto path = TempPath("azookey_auto_word_corrupt.tsv");
  WriteFile(path,
            "# azookey-auto-word-store v1\n"
            "# surface\treading\tsource\tstate\tcount\tfirst_seen_epoch\tlast_seen_epoch\tscore\n"
            "あずきー\tあずきー\tmining\tpending\t2\t1700000000\t1700000005\t0\n"
            "たりない\tたりない\tmining\tpending\t2\n"
            "へんなそーす\tへんなそーす\tunknown\tpending\t2\t1700000000\t1700000000\t0\n"
            "へんなすてーと\tへんなすてーと\tmining\tunknown\t2\t1700000000\t1700000000\t0\n"
            "かずでない\tかずでない\tmining\tpending\tx\t1700000000\t1700000000\t0\n"
            "\t\tmining\tpending\t1\t1700000000\t1700000000\t0\n"
            "かくてい\tかくてい\ttrending\tconfirmed\t7\t1700000000\t1700000100\t0.75\n");

  AutoWordStore store(path);
  // A corrupt file is not a failed load: the readable rows are kept.
  EXPECT_TRUE(store.Load());
  EXPECT_EQ(store.Size(), 2u);
  const auto pending = store.ListByState(AutoWordState::Pending);
  ASSERT_EQ(pending.size(), 1u);
  EXPECT_EQ(pending[0].surface, "あずきー");
  const auto confirmed = store.LookupConfirmed("かくてい");
  ASSERT_EQ(confirmed.size(), 1u);
  EXPECT_EQ(confirmed[0].source, AutoWordSource::Trending);
  EXPECT_EQ(confirmed[0].count, 7u);
  EXPECT_DOUBLE_EQ(confirmed[0].score, 0.75);

  std::error_code ec;
  std::filesystem::remove(path, ec);
}

TEST(AutoWordStoreTest, ResetClearsTheTable) {
  AutoWordStore store(TempPath("azookey_auto_word_reset.tsv"));
  store.Observe("あずきー", "あずきー", kNow, 3, false);
  ASSERT_EQ(store.Size(), 1u);

  store.Reset();
  EXPECT_EQ(store.Size(), 0u);
  EXPECT_EQ(store.ListByState(AutoWordState::Pending).size(), 0u);
}
