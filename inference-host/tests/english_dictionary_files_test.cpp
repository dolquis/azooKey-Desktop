#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "azookey/host/EnglishCandidates.h"
#include "azookey/host/EnglishDictionaryFiles.h"
#include "azookey/host/EnglishDictionaryStore.h"

namespace {

namespace fs = std::filesystem;
using namespace azookey::host;

fs::path TestDir() {
  const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
  auto path = fs::temp_directory_path() /
              (std::string("azookey_english_bin_") + info->test_suite_name() + "_" + info->name());
  fs::remove_all(path);
  fs::create_directories(path);
  return path;
}

void WriteText(const fs::path& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary);
  out << text;
}

std::string ReadBytes(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::vector<std::string> Surfaces(const std::vector<EnglishDictionaryEntry>& entries) {
  std::vector<std::string> out;
  for (const auto& e : entries) out.push_back(e.surface);
  return out;
}

EnglishOverlayOp Upsert(const std::string& surface, uint32_t frequency, uint8_t flags = 0) {
  return {EnglishOverlayOpKind::Upsert, flags, EnglishLookupKey(surface), surface, frequency};
}

EnglishOverlayOp Delete(const std::string& surface) {
  return {EnglishOverlayOpKind::Delete, 0, EnglishLookupKey(surface), surface, 0};
}

std::shared_ptr<const EnglishBaseImage> Base(const std::string& tsv, uint32_t generation = 1) {
  return EnglishBaseImage::FromBytes(EncodeEnglishBase(ParseEnglishTsv(tsv), generation));
}

constexpr const char* kTsv =
    "apple\t542316\n"
    "Apple\t118242\tproper\n"
    "GitHub\t30551\tproper,tech\n"
    "NASA\t8123\tacronym\n"
    "the\t22038615\n";

// Holds the writer lock as another host would and parks the store's wait for
// it: the store's first poll after finding the lock held signals contention
// and blocks until Release(). The store's clock stands still until then, so
// the 500 ms budget cannot run out however slow the work in between is.
class EnglishDictLockBarrier {
 public:
  explicit EnglishDictLockBarrier(EnglishDictionaryStore& store)
      : store_(store), held_(EnglishDictionaryFileLock::Acquire(store.paths().lock)) {
    // After the release the clock runs again from where it stood, so a wait
    // still has its whole budget.
    store_.SetLockClockForTest({[this] {
                                  std::lock_guard lock(mutex_);
                                  return released_
                                             ? frozen_at_ +
                                                   (std::chrono::steady_clock::now() - released_at_)
                                             : frozen_at_;
                                },
                                [this](std::chrono::steady_clock::duration) {
                                  std::unique_lock lock(mutex_);
                                  contended_ = true;
                                  cv_.notify_all();
                                  cv_.wait(lock, [this] { return released_; });
                                }});
  }
  EnglishDictLockBarrier(const EnglishDictLockBarrier&) = delete;
  EnglishDictLockBarrier& operator=(const EnglishDictLockBarrier&) = delete;
  // Also on an early ASSERT return: nothing is left waiting or unjoined.
  ~EnglishDictLockBarrier() {
    Finish();
    store_.SetLockClockForTest({});
  }

  bool held() const { return held_.has_value(); }

  void Start(std::function<void()> work) {
    thread_ = std::thread([this, work = std::move(work)] {
      work();
      std::lock_guard lock(mutex_);
      finished_ = true;
      cv_.notify_all();
    });
  }

  // true once the work polled the held lock; false when it ended without.
  bool WaitForContention() {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [this] { return contended_ || finished_; });
    return contended_;
  }

  // Unlocks the file first, so the parked poll then takes the lock.
  void Release() {
    held_.reset();
    std::lock_guard lock(mutex_);
    if (released_) return;
    released_ = true;
    released_at_ = std::chrono::steady_clock::now();
    cv_.notify_all();
  }

  void Finish() {
    Release();
    if (thread_.joinable()) thread_.join();
  }

 private:
  EnglishDictionaryStore& store_;
  std::optional<EnglishDictionaryFileLock> held_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool contended_{false};
  bool finished_{false};
  bool released_{false};
  const std::chrono::steady_clock::time_point frozen_at_{std::chrono::steady_clock::now()};
  std::chrono::steady_clock::time_point released_at_;
  std::thread thread_;
};

// A lock wait clock that moves only when the wait sleeps, recording each
// sleep.
struct EnglishDictSteppedClock {
  std::chrono::steady_clock::time_point now{};
  std::vector<std::chrono::steady_clock::duration> sleeps;

  EnglishLockClock Clock() {
    return {[this] { return now; },
            [this](std::chrono::steady_clock::duration duration) {
              sleeps.push_back(duration);
              now += duration;
            }};
  }
};

}  // namespace

TEST(EnglishDictionaryFilesTest, BinaryRoundTripMatchesTheTsv) {
  const auto dir = TestDir();
  WriteText(dir / "english-words.tsv", kTsv);
  EnglishDictionaryStore store(dir / "english-words.tsv");
  const auto from_tsv = EnglishDictionary::ParseTsv(kTsv);
  for (const char* key : {"apple", "github", "nasa", "the", "missing"}) {
    EXPECT_EQ(Surfaces(store.Lookup(key)), Surfaces(from_tsv.Lookup(key))) << key;
  }

  // The store compiled english-words.bin next to the TSV (section 4.5).
  const auto bytes = ReadBytes(dir / "english-words.bin");
  ASSERT_GE(bytes.size(), kEnglishBaseHeaderSize);
  EXPECT_EQ(bytes.substr(0, 4), "AZED");
  const auto header = ReadEnglishBaseHeader(dir / "english-words.bin");
  ASSERT_TRUE(header);
  EXPECT_EQ(header->version, kEnglishBaseVersion);
  EXPECT_EQ(header->entry_count, 5u);
  EXPECT_EQ(header->flags & 1u, 1u);  // Sorted by key.
  EXPECT_EQ(header->records_offset, kEnglishBaseHeaderSize);

  const auto mapped = EnglishBaseImage::Map(dir / "english-words.bin");
  ASSERT_TRUE(mapped);
  const auto apple = mapped->Lookup("apple");
  ASSERT_EQ(apple.size(), 2u);
  EXPECT_EQ(apple[0].surface, "apple");  // Frequency highest first.
  EXPECT_EQ(apple[0].frequency, 542316u);
  EXPECT_EQ(apple[1].surface, "Apple");
  EXPECT_EQ(apple[1].flags, kEnglishWordProper);
  EXPECT_EQ(mapped->Lookup("nasa").front().flags, kEnglishWordAcronym);
  fs::remove_all(dir);
}

TEST(EnglishDictionaryFilesTest, FrequencySaturatesAtTheBinaryFieldWidth) {
  const auto records = ParseEnglishTsv("huge\t99999999999\n");
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].frequency, UINT32_MAX);
}

TEST(EnglishDictionaryFilesTest, ContentHashCoversEveryRecord) {
  // Same version, entry_count and generation, the same first record; only the
  // last surface differs (section 4.6).
  const auto a = Base("alpha\t10\nomega\t5\n", 7);
  const auto b = Base("alpha\t10\nOmega\t5\n", 7);
  ASSERT_TRUE(a && b);
  EXPECT_EQ(a->header().entry_count, b->header().entry_count);
  EXPECT_EQ(a->header().generation, b->header().generation);
  EXPECT_NE(a->header().content_hash, b->header().content_hash);
  EXPECT_EQ(a->header().content_hash, Base("alpha\t10\nomega\t5\n", 7)->header().content_hash);
}

TEST(EnglishDictionaryFilesTest, BadBinariesFallBackToTheTsvOrToNoDictionary) {
  const auto dir = TestDir();
  const auto tsv = dir / "english-words.tsv";
  const auto bin = dir / "english-words.bin";
  WriteText(tsv, "apple\t10\n");
  const auto good = EncodeEnglishBase(ParseEnglishTsv("Apple\t10\n"), 1);
  auto bad_magic = good;
  bad_magic[0] = 'X';
  auto bad_version = good;
  bad_version[4] = 9;
  const std::string truncated = good.substr(0, good.size() - 3 - 20);
  // Records intact, only the string pool cut short: caught by content_hash.
  const std::string short_pool = good.substr(0, good.size() - 2);
  for (const auto& bytes : {bad_magic, bad_version, truncated, short_pool}) {
    WriteText(bin, bytes);
    // Stamped as compiled from this TSV, so only its validity sends the store
    // to the TSV.
    fs::last_write_time(bin, fs::last_write_time(tsv));
    EnglishDictionaryStore store(tsv);
    EXPECT_EQ(Surfaces(store.Lookup("apple")), std::vector<std::string>{"apple"});
    // And the .bin was regenerated from the TSV.
    EXPECT_TRUE(EnglishBaseImage::Map(bin));
  }
  fs::remove(tsv);
  WriteText(bin, bad_magic);
  EnglishDictionaryStore store(tsv);
  EXPECT_TRUE(store.Lookup("apple").empty());
  EXPECT_FALSE(store.Read([](const EnglishDictionary* d) { return d != nullptr; }));
  fs::remove_all(dir);
}

TEST(EnglishDictionaryFilesTest, ABinStampedWithTheTsvMtimeIsUsedAndAnyOtherIsRegenerated) {
  const auto dir = TestDir();
  const auto tsv = dir / "english-words.tsv";
  const auto bin = dir / "english-words.bin";
  WriteText(tsv, "apple\t10\n");
  {
    EnglishDictionaryStore store(tsv);
    EXPECT_EQ(Surfaces(store.Lookup("apple")), std::vector<std::string>{"apple"});
    EXPECT_EQ(fs::last_write_time(bin), fs::last_write_time(tsv));
  }
  // A .bin carrying the TSV's mtime is used without parsing the TSV.
  WriteText(bin, EncodeEnglishBase(ParseEnglishTsv("Apple\t10\n"), 2));
  fs::last_write_time(bin, fs::last_write_time(tsv));
  EnglishDictionaryStore store(tsv);
  EXPECT_EQ(Surfaces(store.Lookup("apple")), std::vector<std::string>{"Apple"});
  // A TSV edited after it is compiled again, by the same store.
  WriteText(tsv, "APPLE\t10\n");
  fs::last_write_time(tsv, fs::last_write_time(bin) + std::chrono::seconds(5));
  EXPECT_EQ(Surfaces(store.Lookup("apple")), std::vector<std::string>{"APPLE"});
  EXPECT_EQ(fs::last_write_time(bin), fs::last_write_time(tsv));
  EXPECT_EQ(EnglishBaseImage::Map(bin)->Lookup("apple").front().surface, "APPLE");
  // A TSV restored with an older timestamp (robocopy, backup) is read too,
  // also by a host started afterwards.
  WriteText(tsv, "aPPle\t10\n");
  fs::last_write_time(tsv, fs::last_write_time(bin) - std::chrono::hours(24));
  EXPECT_EQ(Surfaces(store.Lookup("apple")), std::vector<std::string>{"aPPle"});
  WriteText(tsv, "ApplE\t10\n");
  fs::last_write_time(tsv, fs::last_write_time(bin) - std::chrono::hours(48));
  EXPECT_EQ(Surfaces(EnglishDictionaryStore(tsv).Lookup("apple")),
            std::vector<std::string>{"ApplE"});
  // Within a host, a TSV whose size changes under the same mtime is read too.
  const auto stamp = fs::last_write_time(tsv);
  WriteText(tsv, "APPle\t10\nbanana\t1\n");
  fs::last_write_time(tsv, stamp);
  EXPECT_EQ(Surfaces(store.Lookup("apple")), std::vector<std::string>{"APPle"});

  // Without a TSV the .bin alone is the dictionary, also when it is the
  // configured path (a bundled .bin).
  fs::remove(tsv);
  EXPECT_EQ(Surfaces(store.Lookup("apple")), std::vector<std::string>{"APPle"});
  EnglishDictionaryStore bundled(bin);
  EXPECT_TRUE(bundled.paths().tsv.empty());
  EXPECT_EQ(bundled.paths().overlay, dir / "english-words.delta.bin");
  EXPECT_EQ(Surfaces(bundled.Lookup("apple")), std::vector<std::string>{"APPle"});
  fs::remove_all(dir);
}

TEST(EnglishDictionaryFilesTest, OverlayOpsReplayInArrivalOrderAndTheLastWins) {
  const auto base = Base("apple\t50\nApple\t40\tproper\nbanana\t30\n");
  // Unsorted, with several ops on the same (key, surface).
  const EnglishDictionary dictionary(
      base, {Upsert("apple", 5), Delete("Apple"), Upsert("APPLE", 45, kEnglishWordAcronym),
             Upsert("apple", 60), Upsert("cherry", 1), Delete("cherry"), Delete("banana"),
             Upsert("banana", 31)});
  const auto apple = dictionary.Lookup("apple");
  // Upsert replaced the base frequency, the tombstone hid "Apple", and the
  // merge is frequency-descending.
  EXPECT_EQ(Surfaces(apple), (std::vector<std::string>{"apple", "APPLE"}));
  EXPECT_EQ(apple[0].frequency, 60u);
  EXPECT_EQ(apple[1].flags, kEnglishWordAcronym);
  EXPECT_TRUE(dictionary.Lookup("cherry").empty());
  EXPECT_EQ(dictionary.Lookup("banana").front().frequency, 31u);
  EXPECT_EQ(dictionary.size(), 3u);  // apple, APPLE, banana.
}

TEST(EnglishDictionaryFilesTest, StoreWritesPersistAndCompactionMatchesTheMerge) {
  const auto dir = TestDir();
  const auto tsv = dir / "english-words.tsv";
  WriteText(tsv, "apple\t50\nApple\t40\tproper\nbanana\t30\n");
  EnglishDictionaryStore store(tsv);
  ASSERT_TRUE(store.Upsert("APPLE", 45, kEnglishWordAcronym));
  ASSERT_TRUE(store.Remove("Apple"));
  ASSERT_TRUE(store.Upsert("kotlin", 7, kEnglishWordTech));
  const auto expected_apple = Surfaces(store.Lookup("apple"));
  EXPECT_EQ(expected_apple, (std::vector<std::string>{"apple", "APPLE"}));
  // Another host reading the same files replays the same overlay.
  EnglishDictionaryStore other(tsv);
  EXPECT_EQ(Surfaces(other.Lookup("apple")), expected_apple);
  EXPECT_EQ(Surfaces(other.Lookup("kotlin")), std::vector<std::string>{"kotlin"});

  const auto before = ReadEnglishBaseHeader(store.paths().base);
  ASSERT_TRUE(store.Compact());
  const auto after = ReadEnglishBaseHeader(store.paths().base);
  ASSERT_TRUE(before && after);
  EXPECT_EQ(after->generation, before->generation + 1);
  EXPECT_EQ(after->entry_count, 4u);
  // The overlay now names the new base and is empty.
  const auto overlay = ReadEnglishOverlay(store.paths().overlay);
  ASSERT_TRUE(overlay);
  EXPECT_EQ(overlay->header.base_fingerprint, after->content_hash);
  EXPECT_EQ(overlay->header.op_count, 0u);
  EXPECT_EQ(fs::file_size(store.paths().overlay), kEnglishOverlayHeaderSize);
  const auto base = EnglishBaseImage::Map(store.paths().base);
  ASSERT_TRUE(base);
  std::vector<std::string> compacted;
  for (const auto& r : base->Lookup("apple")) compacted.push_back(r.surface);
  EXPECT_EQ(compacted, expected_apple);
  EXPECT_EQ(Surfaces(other.Lookup("apple")), expected_apple);

  // An append after compaction matches the new fingerprint and survives a reload.
  ASSERT_TRUE(store.Upsert("Swift", 3, kEnglishWordProper));
  EnglishDictionaryStore reloaded(tsv);
  EXPECT_EQ(Surfaces(reloaded.Lookup("swift")), std::vector<std::string>{"Swift"});
  EXPECT_EQ(Surfaces(reloaded.Lookup("kotlin")), std::vector<std::string>{"kotlin"});
  fs::remove_all(dir);
}

TEST(EnglishDictionaryFilesTest, MismatchedOrCorruptOverlaysAreReadAsEmpty) {
  const auto dir = TestDir();
  const auto tsv = dir / "english-words.tsv";
  WriteText(tsv, "apple\t50\n");
  EnglishDictionaryStore store(tsv);
  ASSERT_EQ(store.Lookup("apple").size(), 1u);
  const auto hash = ReadEnglishBaseHeader(store.paths().base)->content_hash;

  // An overlay for another base is discarded.
  ASSERT_TRUE(AppendEnglishOverlayOp(store.paths().overlay, hash + 1, Delete("apple")));
  EXPECT_EQ(Surfaces(EnglishDictionaryStore(tsv).Lookup("apple")),
            std::vector<std::string>{"apple"});

  // op_count claims more frames than the file holds.
  ASSERT_TRUE(ReinitEnglishOverlay(store.paths().overlay, hash));
  ASSERT_TRUE(AppendEnglishOverlayOp(store.paths().overlay, hash, Delete("apple")));
  auto bytes = ReadBytes(store.paths().overlay);
  bytes[12] = 5;
  WriteText(store.paths().overlay, bytes);
  EXPECT_FALSE(ReadEnglishOverlay(store.paths().overlay));
  EXPECT_EQ(Surfaces(EnglishDictionaryStore(tsv).Lookup("apple")),
            std::vector<std::string>{"apple"});

  // Not an overlay at all.
  WriteText(store.paths().overlay, "garbage");
  EXPECT_EQ(Surfaces(EnglishDictionaryStore(tsv).Lookup("apple")),
            std::vector<std::string>{"apple"});
  // The next writer reinitializes it before appending.
  ASSERT_TRUE(store.Upsert("Apple", 60, kEnglishWordProper));
  EXPECT_EQ(Surfaces(EnglishDictionaryStore(tsv).Lookup("apple")),
            (std::vector<std::string>{"Apple", "apple"}));
  fs::remove_all(dir);
}

TEST(EnglishDictionaryFilesTest, AppendCrashLeavesATornFrameThatIsIgnoredAndReclaimed) {
  const auto dir = TestDir();
  const auto overlay = dir / "english-words.delta.bin";
  constexpr uint32_t kHash = 0x1234;
  ASSERT_TRUE(AppendEnglishOverlayOp(overlay, kHash, Upsert("first", 1)));
  const auto size_after_first = fs::file_size(overlay);

  // The writer dies after the frame, before op_count covers it.
  std::vector<EnglishWriteStep> steps;
  EXPECT_FALSE(AppendEnglishOverlayOp(overlay, kHash, Upsert("torn", 2), [&](EnglishWriteStep s) {
    steps.push_back(s);
    return s != EnglishWriteStep::AppendFrameWritten;
  }));
  EXPECT_EQ(steps, std::vector<EnglishWriteStep>{EnglishWriteStep::AppendFrameWritten});
  EXPECT_GT(fs::file_size(overlay), size_after_first);
  auto contents = ReadEnglishOverlay(overlay);
  ASSERT_TRUE(contents);
  ASSERT_EQ(contents->ops.size(), 1u);
  EXPECT_EQ(contents->ops[0].surface, "first");

  // The next writer truncates the torn frame and appends after "first".
  ASSERT_TRUE(AppendEnglishOverlayOp(overlay, kHash, Upsert("second", 3)));
  contents = ReadEnglishOverlay(overlay);
  ASSERT_TRUE(contents);
  ASSERT_EQ(contents->ops.size(), 2u);
  EXPECT_EQ(contents->ops[1].surface, "second");
  EXPECT_EQ(fs::file_size(overlay), size_after_first + 10 + 6 + 6);
  fs::remove_all(dir);
}

TEST(EnglishDictionaryFilesTest, ReinitPublishesTheEmptyOverlayBeforeTheNewFingerprint) {
  const auto dir = TestDir();
  const auto overlay = dir / "english-words.delta.bin";
  const auto old_base = Base("apple\t50\n", 1);
  const auto new_base = Base("apple\t50\nzebra\t1\n", 2);
  ASSERT_TRUE(AppendEnglishOverlayOp(overlay, old_base->header().content_hash, Delete("apple")));
  ASSERT_TRUE(AppendEnglishOverlayOp(overlay, old_base->header().content_hash, Upsert("stale", 9)));

  // At every step a lock-free reader of the new base must not see the old
  // ops: a fingerprint written first would validate them for the new base.
  std::vector<EnglishWriteStep> steps;
  const auto reader_view = [&] {
    std::vector<EnglishOverlayOp> ops;
    const auto contents = ReadEnglishOverlay(overlay);
    if (contents && contents->header.base_fingerprint == new_base->header().content_hash)
      ops = contents->ops;
    return EnglishDictionary(new_base, ops);
  };
  const auto old_hash = old_base->header().content_hash;
  const auto new_hash = new_base->header().content_hash;
  ASSERT_TRUE(ReinitEnglishOverlay(overlay, new_hash, [&](EnglishWriteStep step) {
    steps.push_back(step);
    const auto view = reader_view();
    EXPECT_EQ(Surfaces(view.Lookup("apple")), std::vector<std::string>{"apple"});
    EXPECT_TRUE(view.Lookup("stale").empty());
    // The fingerprint changes only after op_count = 0 and the truncation.
    const auto header = ReadEnglishOverlayHeader(overlay);
    EXPECT_TRUE(header);
    if (!header) return true;
    EXPECT_EQ(header->op_count, 0u);
    EXPECT_EQ(header->base_fingerprint,
              step == EnglishWriteStep::ReinitFingerprintWritten ? new_hash : old_hash);
    if (step != EnglishWriteStep::ReinitCountCleared) {
      EXPECT_EQ(fs::file_size(overlay), kEnglishOverlayHeaderSize);
    }
    return true;
  }));
  EXPECT_EQ(steps, (std::vector<EnglishWriteStep>{EnglishWriteStep::ReinitCountCleared,
                                                  EnglishWriteStep::ReinitTruncated,
                                                  EnglishWriteStep::ReinitFingerprintWritten}));
  fs::remove_all(dir);
}

TEST(EnglishDictionaryFilesTest, ReaderNoticesABaseReplacedUnderItsMapping) {
  const auto dir = TestDir();
  const auto bin = dir / "english-words.bin";
  const auto first = EncodeEnglishBase(ParseEnglishTsv("apple\t10\n"), 3);
  // Same generation and entry count; only the content differs.
  const auto second = EncodeEnglishBase(ParseEnglishTsv("Apple\t10\n"), 3);
  WriteText(bin, first);
  EnglishDictionaryStore store(bin);
  const auto held = store.Read([](const EnglishDictionary* d) { return d->base(); });
  ASSERT_TRUE(held);
  EXPECT_EQ(Surfaces(store.Lookup("apple")), std::vector<std::string>{"apple"});

  // The writer replaces the base while the reader keeps it mapped; this needs
  // FILE_SHARE_DELETE and no open handle on the base (section 4.7).
  ASSERT_TRUE(WriteEnglishBase(EnglishDictionaryPaths::From(bin), second));
  EXPECT_EQ(ReadEnglishBaseHeader(bin)->generation, held->header().generation);
  EXPECT_NE(ReadEnglishBaseHeader(bin)->content_hash, held->header().content_hash);
  EXPECT_EQ(Surfaces(store.Lookup("apple")), std::vector<std::string>{"Apple"});
  // The old mapping still reads the old file.
  EXPECT_EQ(held->Lookup("apple").front().surface, "apple");
  fs::remove_all(dir);
}

TEST(EnglishDictionaryFilesTest, ReadRetriesWhenACompactionCrossesIt) {
  const auto dir = TestDir();
  const auto tsv = dir / "english-words.tsv";
  WriteText(tsv, "apple\t50\n");
  EnglishDictionaryStore writer(tsv);
  ASSERT_TRUE(writer.Upsert("kotlin", 7));
  EnglishDictionaryStore reader(tsv);
  ASSERT_EQ(Surfaces(reader.Lookup("kotlin")), std::vector<std::string>{"kotlin"});

  // Between the reader's base header sample and its overlay read, another host
  // compacts: "kotlin" moves into the new base and the overlay is emptied
  // (op_count = 0 while base_fingerprint still names the old base).
  bool crossed = false;
  reader.SetReadHookForTest([&] {
    if (crossed) return;
    crossed = true;
    writer.SetWriteHookForTest(
        [](EnglishWriteStep step) { return step != EnglishWriteStep::ReinitCountCleared; });
    EXPECT_FALSE(writer.Compact());
  });
  // Old base + emptied overlay would lose "kotlin"; the recheck retries.
  EXPECT_EQ(Surfaces(reader.Lookup("kotlin")), std::vector<std::string>{"kotlin"});
  EXPECT_TRUE(crossed);
  fs::remove_all(dir);
}

TEST(EnglishDictionaryFilesTest, CompactionCrashAfterTheRenameLosesNothing) {
  const auto dir = TestDir();
  const auto tsv = dir / "english-words.tsv";
  WriteText(tsv, "apple\t50\n");
  {
    EnglishDictionaryStore writer(tsv);
    ASSERT_TRUE(writer.Upsert("kotlin", 7));
    ASSERT_TRUE(writer.Remove("apple"));
    const auto generation = ReadEnglishBaseHeader(writer.paths().base)->generation;
    // The writer dies with the merged base live and the overlay untouched.
    writer.SetWriteHookForTest(
        [](EnglishWriteStep step) { return step != EnglishWriteStep::CompactBaseReplaced; });
    EXPECT_FALSE(writer.Compact());
    const auto header = ReadEnglishBaseHeader(writer.paths().base);
    EXPECT_EQ(header->generation, generation + 1);
    EXPECT_NE(ReadEnglishOverlayHeader(writer.paths().overlay)->base_fingerprint,
              header->content_hash);
  }
  EnglishDictionaryStore reader(tsv);
  // The stale overlay is not re-applied: "kotlin" appears once, "apple" stays gone.
  EXPECT_EQ(Surfaces(reader.Lookup("kotlin")), std::vector<std::string>{"kotlin"});
  EXPECT_TRUE(reader.Lookup("apple").empty());

  // The next append reinitializes the stale overlay first, so its word
  // survives a reload instead of being discarded with the stale frames.
  ASSERT_TRUE(reader.Upsert("Swift", 3, kEnglishWordProper));
  const auto overlay = ReadEnglishOverlay(reader.paths().overlay);
  ASSERT_TRUE(overlay);
  EXPECT_EQ(overlay->header.base_fingerprint,
            ReadEnglishBaseHeader(reader.paths().base)->content_hash);
  EXPECT_EQ(overlay->ops.size(), 1u);
  EnglishDictionaryStore reloaded(tsv);
  EXPECT_EQ(Surfaces(reloaded.Lookup("swift")), std::vector<std::string>{"Swift"});
  EXPECT_EQ(Surfaces(reloaded.Lookup("kotlin")), std::vector<std::string>{"kotlin"});
  fs::remove_all(dir);
}

TEST(EnglishDictionaryFilesTest, AppendAfterABaseRecompileReinitializesTheOverlay) {
  const auto dir = TestDir();
  const auto tsv = dir / "english-words.tsv";
  WriteText(tsv, "apple\t50\n");
  EnglishDictionaryStore store(tsv);
  ASSERT_EQ(store.Lookup("apple").size(), 1u);
  // Straight to the overlay, so no journal entry carries it past the
  // recompile (section 4.8).
  ASSERT_TRUE(AppendEnglishOverlayOp(store.paths().overlay,
                                     ReadEnglishBaseHeader(store.paths().base)->content_hash,
                                     Delete("apple")));
  EXPECT_TRUE(store.Lookup("apple").empty());
  // The TSV changes; the recompiled base no longer matches the overlay.
  WriteText(tsv, "apple\t50\nbanana\t5\n");
  fs::last_write_time(tsv, fs::last_write_time(store.paths().base) + std::chrono::seconds(5));
  EXPECT_EQ(Surfaces(store.Lookup("apple")), std::vector<std::string>{"apple"});
  ASSERT_TRUE(store.Upsert("Banana", 9));
  EnglishDictionaryStore reloaded(tsv);
  EXPECT_EQ(Surfaces(reloaded.Lookup("banana")), (std::vector<std::string>{"Banana", "banana"}));
  EXPECT_EQ(Surfaces(reloaded.Lookup("apple")), std::vector<std::string>{"apple"});
  fs::remove_all(dir);
}

TEST(EnglishDictionaryFilesTest, WithoutTheWriterLockOpsStayInMemory) {
  const auto dir = TestDir();
  const auto tsv = dir / "english-words.tsv";
  WriteText(tsv, "apple\t50\n");
  EnglishDictionaryStore store(tsv);
  ASSERT_EQ(store.Lookup("apple").size(), 1u);
  {
    const auto held = EnglishDictionaryFileLock::Acquire(store.paths().lock);
    ASSERT_TRUE(held);
    EXPECT_FALSE(EnglishDictionaryFileLock::Acquire(store.paths().lock));
    EXPECT_FALSE(store.Upsert("kotlin", 7));
  }
  // Applied for this host, never written for the others.
  EXPECT_EQ(Surfaces(store.Lookup("kotlin")), std::vector<std::string>{"kotlin"});
  EXPECT_TRUE(EnglishDictionaryStore(tsv).Lookup("kotlin").empty());
  fs::remove_all(dir);
}

TEST(EnglishDictionaryFilesTest, OverlayCompactsOnceItOutgrowsTheThreshold) {
  const auto dir = TestDir();
  const auto tsv = dir / "english-words.tsv";
  WriteText(tsv, "apple\t50\n");
  EnglishDictionaryStore store(tsv);
  for (int i = 0; i < 65; ++i) ASSERT_TRUE(store.Upsert("word" + std::to_string(i), 1));
  // The 65th op crossed the minimum of 64 and folded everything into the base.
  EXPECT_EQ(ReadEnglishOverlayHeader(store.paths().overlay)->op_count, 0u);
  EXPECT_EQ(ReadEnglishBaseHeader(store.paths().base)->entry_count, 66u);
  EXPECT_EQ(Surfaces(EnglishDictionaryStore(tsv).Lookup("word64")),
            std::vector<std::string>{"word64"});
  fs::remove_all(dir);
}

TEST(EnglishDictionaryFilesTest, CompactionLeavesABaseOlderThanTheTsvToTheRecompile) {
  const auto dir = TestDir();
  const auto tsv = dir / "english-words.tsv";
  WriteText(tsv, "apple\t50\n");
  EnglishDictionaryStore store(tsv);
  ASSERT_TRUE(store.Upsert("kotlin", 7));
  // The TSV is edited before anything reads it again.
  WriteText(tsv, "Apple\t50\n");
  fs::last_write_time(tsv, fs::last_write_time(store.paths().base) + std::chrono::seconds(5));
  // Compacting the old base would stamp it newer than the edit and hide it.
  EXPECT_FALSE(store.Compact());
  EXPECT_EQ(Surfaces(store.Lookup("apple")), std::vector<std::string>{"Apple"});
  fs::remove_all(dir);
}

TEST(EnglishDictionaryFilesTest, StoreDiscardsTheOverlayOfABaseRegeneratedBehindTheFirstRecord) {
  const auto dir = TestDir();
  const auto bin = dir / "english-words.bin";
  WriteText(bin, EncodeEnglishBase(ParseEnglishTsv("alpha\t10\nomega\t5\n"), 3));
  EnglishDictionaryStore store(bin);
  ASSERT_TRUE(store.Upsert("alpha", 999));
  ASSERT_EQ(store.Lookup("alpha").front().frequency, 999u);
  // Same version, entry_count, generation and first record; only the last
  // record differs. Only the whole-content hash tells the bases apart.
  ASSERT_TRUE(WriteEnglishBase(store.paths(),
                               EncodeEnglishBase(ParseEnglishTsv("alpha\t10\nOmega\t5\n"), 3)));
  EXPECT_EQ(store.Lookup("alpha").front().frequency, 10u);
  EXPECT_EQ(Surfaces(store.Lookup("omega")), std::vector<std::string>{"Omega"});
  EXPECT_EQ(EnglishDictionaryStore(bin).Lookup("alpha").front().frequency, 10u);
  fs::remove_all(dir);
}

TEST(EnglishDictionaryFilesTest, AppendWaitingForTheLockAttachesToTheCompactedBase) {
  const auto dir = TestDir();
  const auto tsv = dir / "english-words.tsv";
  WriteText(tsv, "apple\t50\n");
  EnglishDictionaryStore store(tsv);
  ASSERT_TRUE(store.Upsert("kotlin", 7));
  bool persisted = false;
  {
    EnglishDictLockBarrier barrier(store);
    ASSERT_TRUE(barrier.held());
    barrier.Start([&] { persisted = static_cast<bool>(store.Upsert("Swift", 3)); });
    ASSERT_TRUE(barrier.WaitForContention());
    // Another host compacts while this one waits for the lock.
    ASSERT_TRUE(CompactEnglishDictionary(store.paths()));
    barrier.Finish();
  }
  EXPECT_TRUE(persisted);
  EnglishDictionaryStore reloaded(tsv);
  EXPECT_EQ(Surfaces(reloaded.Lookup("swift")), std::vector<std::string>{"Swift"});
  EXPECT_EQ(Surfaces(reloaded.Lookup("kotlin")), std::vector<std::string>{"kotlin"});
  fs::remove_all(dir);
}

TEST(EnglishDictionaryFilesTest, APersistedOpOverridesAnOlderMemoryOpOnTheSameEntry) {
  const auto dir = TestDir();
  const auto tsv = dir / "english-words.tsv";
  WriteText(tsv, "apple\t50\n");
  EnglishDictionaryStore store(tsv);
  {
    const auto held = EnglishDictionaryFileLock::Acquire(store.paths().lock);
    ASSERT_TRUE(held);
    EXPECT_FALSE(store.Upsert("Foo", 5));  // Kept in memory only.
  }
  ASSERT_EQ(Surfaces(store.Lookup("foo")), std::vector<std::string>{"Foo"});
  // The later delete reaches the file and must win over the older memory op,
  // also after compaction.
  ASSERT_TRUE(store.Remove("Foo"));
  EXPECT_TRUE(store.Lookup("foo").empty());
  ASSERT_TRUE(store.Compact());
  EXPECT_TRUE(store.Lookup("foo").empty());
  fs::remove_all(dir);
}

TEST(EnglishDictionaryFilesTest, AHeldLockIsPolledEvery10MsAndGivenUpAfter500Ms) {
  const auto dir = TestDir();
  const auto lock_path = dir / "english-words.lock";
  const auto held = EnglishDictionaryFileLock::Acquire(lock_path);
  ASSERT_TRUE(held);
  EnglishDictSteppedClock clock;
  EXPECT_FALSE(EnglishDictionaryFileLock::Acquire(lock_path, clock.Clock()));
  // Attempts at 0, 10, ..., 490 ms sleep; the one at 500 ms gives up.
  EXPECT_EQ(clock.sleeps.size(), 50u);
  for (const auto& duration : clock.sleeps) EXPECT_EQ(duration, std::chrono::milliseconds(10));
  // A zero budget tries once.
  EnglishDictSteppedClock once;
  EXPECT_FALSE(
      EnglishDictionaryFileLock::Acquire(lock_path, once.Clock(), std::chrono::milliseconds{0}));
  EXPECT_TRUE(once.sleeps.empty());
  fs::remove_all(dir);
}

TEST(EnglishDictionaryFilesTest, AReaderRecompilingTheTsvDoesNotWaitForTheWriterLock) {
  const auto dir = TestDir();
  const auto tsv = dir / "english-words.tsv";
  WriteText(tsv, "apple\t50\n");
  EnglishDictSteppedClock clock;
  EnglishDictionaryStore store(tsv);
  ASSERT_EQ(Surfaces(store.Lookup("apple")), std::vector<std::string>{"apple"});
  store.SetLockClockForTest(clock.Clock());
  {
    // Another host holds the lock when the edited TSV is read: the reader
    // serves it from memory at once and leaves the .bin alone.
    const auto held = EnglishDictionaryFileLock::Acquire(store.paths().lock);
    ASSERT_TRUE(held);
    WriteText(tsv, "Apple\t50\n");
    fs::last_write_time(tsv, fs::last_write_time(store.paths().base) + std::chrono::seconds(5));
    EXPECT_EQ(Surfaces(store.Lookup("apple")), std::vector<std::string>{"Apple"});
    EXPECT_TRUE(clock.sleeps.empty());
    EXPECT_NE(fs::last_write_time(store.paths().base), fs::last_write_time(tsv));
  }
  // The next writer that holds the lock compiles the .bin (section 4.5).
  EXPECT_TRUE(store.Upsert("kotlin", 7));
  EnglishDictionaryStore reloaded(tsv);
  EXPECT_EQ(Surfaces(reloaded.Lookup("apple")), std::vector<std::string>{"Apple"});
  EXPECT_EQ(Surfaces(reloaded.Lookup("kotlin")), std::vector<std::string>{"kotlin"});
  fs::remove_all(dir);
}

TEST(EnglishDictionaryFilesTest, AWriterThatMissedTheLockRecompilesWithoutWaitingAgain) {
  const auto dir = TestDir();
  const auto tsv = dir / "english-words.tsv";
  WriteText(tsv, "apple\t50\n");
  EnglishDictSteppedClock clock;
  EnglishDictionaryStore store(tsv);
  ASSERT_EQ(store.Lookup("apple").size(), 1u);
  store.SetLockClockForTest(clock.Clock());
  const auto held = EnglishDictionaryFileLock::Acquire(store.paths().lock);
  ASSERT_TRUE(held);
  WriteText(tsv, "Apple\t50\n");
  fs::last_write_time(tsv, fs::last_write_time(store.paths().base) + std::chrono::seconds(5));
  // One 500 ms wait for the append; the recompile of the edited TSV after it
  // only tries the lock once.
  EXPECT_FALSE(store.Upsert("kotlin", 7));
  EXPECT_EQ(clock.sleeps.size(), 50u);
  EXPECT_EQ(Surfaces(store.Lookup("apple")), std::vector<std::string>{"Apple"});
  EXPECT_EQ(Surfaces(store.Lookup("kotlin")), std::vector<std::string>{"kotlin"});
  fs::remove_all(dir);
}

TEST(EnglishDictionaryFilesTest, AReaderDoesNotWaitBehindAWriter) {
  const auto dir = TestDir();
  const auto tsv = dir / "english-words.tsv";
  WriteText(tsv, "apple\t50\n");
  EnglishDictionaryStore store(tsv);
  ASSERT_EQ(store.Lookup("apple").size(), 1u);
  EnglishDictLockBarrier barrier(store);
  ASSERT_TRUE(barrier.held());
  // The writer holds the update mutex while it waits for the file lock held
  // by the barrier, and cannot move on until the barrier is released.
  barrier.Start([&] { (void)store.Upsert("kotlin", 7); });
  ASSERT_TRUE(barrier.WaitForContention());
  // A changed TSV forces a rebuild; the reader keeps the previous snapshot
  // rather than queueing behind the writer. A reader that queued would not
  // return before the release; the wait below only bounds that failure.
  WriteText(tsv, "Apple\t50\n");
  fs::last_write_time(tsv, fs::last_write_time(store.paths().base) + std::chrono::seconds(5));
  auto reader = std::async(std::launch::async, [&] { return Surfaces(store.Lookup("apple")); });
  const bool returned = reader.wait_for(std::chrono::seconds(20)) == std::future_status::ready;
  barrier.Finish();
  EXPECT_TRUE(returned);
  EXPECT_EQ(reader.get(), std::vector<std::string>{"apple"});
  EXPECT_EQ(Surfaces(store.Lookup("apple")), std::vector<std::string>{"Apple"});
  fs::remove_all(dir);
}

TEST(EnglishDictionaryFilesTest, MalformedBasesAndOverlaysAreRejected) {
  const auto good = EncodeEnglishBase(ParseEnglishTsv("apple\t10\nbanana\t5\n"), 1);
  ASSERT_TRUE(EnglishBaseImage::FromBytes(good));
  EXPECT_FALSE(EnglishBaseImage::FromBytes(""));
  EXPECT_FALSE(EnglishBaseImage::FromBytes(good.substr(0, kEnglishBaseHeaderSize)));
  // A header alone is a valid, empty base.
  const auto empty = EnglishBaseImage::FromBytes(EncodeEnglishBase({}, 1));
  ASSERT_TRUE(empty);
  EXPECT_TRUE(empty->Lookup("apple").empty());
  // A record whose string offset points past the end.
  auto bad_offset = good;
  bad_offset[kEnglishBaseHeaderSize + 8] = '\xFF';
  bad_offset[kEnglishBaseHeaderSize + 9] = '\xFF';
  EXPECT_FALSE(EnglishBaseImage::FromBytes(bad_offset));
  // An entry_count far beyond the records and the entry bound.
  auto huge_count = good;
  huge_count[8] = huge_count[9] = huge_count[10] = '\xFF';
  huge_count[11] = '\x7F';
  EXPECT_FALSE(EnglishBaseImage::FromBytes(huge_count));

  const auto dir = TestDir();
  const auto overlay = dir / "english-words.delta.bin";
  ASSERT_TRUE(AppendEnglishOverlayOp(overlay, 1, Upsert("apple", 1)));
  auto bytes = ReadBytes(overlay);
  bytes[16 + 2] = '\xFF';  // key_len runs past the file.
  WriteText(overlay, bytes);
  EXPECT_FALSE(ReadEnglishOverlay(overlay));
  bytes = ReadBytes(overlay);
  bytes[16 + 2] = 5;
  bytes[16 + 4] = '\xFF';  // surface_len runs past the file.
  WriteText(overlay, bytes);
  EXPECT_FALSE(ReadEnglishOverlay(overlay));
  fs::remove_all(dir);
}

TEST(EnglishDictionaryFilesTest, CompactionKeepsAnOverlayItCannotReadWhole) {
  const auto dir = TestDir();
  const auto tsv = dir / "english-words.tsv";
  WriteText(tsv, "apple\t50\n");
  EnglishDictionaryStore store(tsv);
  ASSERT_TRUE(store.Upsert("kotlin", 7));
  // op_count claims a frame the file does not hold (a writer mid-way, or an
  // overlay past the size bound): compaction must not reset it.
  auto bytes = ReadBytes(store.paths().overlay);
  bytes[12] = 2;
  WriteText(store.paths().overlay, bytes);
  EXPECT_FALSE(CompactEnglishDictionary(store.paths()));
  EXPECT_EQ(ReadBytes(store.paths().overlay), bytes);
  fs::remove_all(dir);
}

TEST(EnglishDictionaryFilesTest, AnUnwritableDictionaryFolderFallsBackToTheTsvInMemory) {
  const auto dir = TestDir();
  const auto tsv = dir / "english-words.tsv";
  WriteText(tsv, "apple\t50\n");
  // Neither the lock nor the .bin can be created (a directory is in their
  // place), as in a read-only share or under Program Files.
  fs::create_directories(dir / "english-words.lock");
  EnglishDictionaryStore store(tsv);
  EXPECT_EQ(Surfaces(store.Lookup("apple")), std::vector<std::string>{"apple"});
  EXPECT_FALSE(fs::exists(dir / "english-words.bin"));
  EXPECT_FALSE(store.Upsert("kotlin", 7));
  EXPECT_EQ(Surfaces(store.Lookup("kotlin")), std::vector<std::string>{"kotlin"});
  EXPECT_FALSE(fs::exists(dir / "english-words.delta.bin"));
  // No temporary files are left behind.
  size_t files = 0;
  for (const auto& entry : fs::directory_iterator(dir)) files += entry.is_regular_file() ? 1 : 0;
  EXPECT_EQ(files, 1u);
  fs::remove_all(dir);
}

TEST(EnglishDictionaryFilesTest, ABinWhoseMtimeCannotBeStampedIsNotUsedOrRetried) {
  const auto dir = TestDir();
  const auto tsv = dir / "english-words.tsv";
  WriteText(tsv, "apple\t50\n");
  // Far from now, so an unstamped .bin written in the same clock tick cannot
  // match it by chance.
  fs::last_write_time(tsv, fs::last_write_time(tsv) - std::chrono::hours(1));
  int tsv_loads = 0;
  EnglishDictionaryStore store(tsv, [&](const EnglishDictionaryLoadReport& report) {
    if (std::string(report.source) == "tsv") ++tsv_loads;
  });
  // The temporary .bin's mtime cannot be stamped (a share by an antivirus or
  // indexer, or a coarse clock that rounds it), so it never goes live.
  store.SetWriteHookForTest(
      [](EnglishWriteStep step) { return step != EnglishWriteStep::BaseStamped; });
  for (int i = 0; i < 5; ++i)
    EXPECT_EQ(Surfaces(store.Lookup("apple")), std::vector<std::string>{"apple"});
  // Served from memory, parsed once, not rewritten on every read.
  EXPECT_EQ(tsv_loads, 1);
  EXPECT_FALSE(fs::exists(store.paths().base));
  // With the write failing under the lock, an op is not retried into a .bin.
  EXPECT_FALSE(store.Upsert("kotlin", 7));
  EXPECT_EQ(Surfaces(store.Lookup("kotlin")), std::vector<std::string>{"kotlin"});
  EXPECT_EQ(tsv_loads, 1);
  // Once the TSV changes, the .bin is tried again.
  store.SetWriteHookForTest({});
  const auto before = fs::last_write_time(tsv);
  WriteText(tsv, "Apple\t50\n");
  fs::last_write_time(tsv, before + std::chrono::seconds(5));
  EXPECT_EQ(Surfaces(store.Lookup("apple")), std::vector<std::string>{"Apple"});
  EXPECT_EQ(tsv_loads, 2);
  EXPECT_EQ(fs::last_write_time(store.paths().base), fs::last_write_time(tsv));
  EXPECT_TRUE(store.Upsert("Swift", 3));
  fs::remove_all(dir);
}

TEST(EnglishDictionaryFilesTest, ABaseKeptInMemoryForABusyLockGetsItsBinOnTheNextWrite) {
  const auto dir = TestDir();
  const auto tsv = dir / "english-words.tsv";
  WriteText(tsv, "apple\t50\n");
  EnglishDictionaryStore store(tsv);
  {
    // Another host holds the lock during the first read: the TSV is served
    // from memory and no .bin is written.
    const auto held = EnglishDictionaryFileLock::Acquire(store.paths().lock);
    ASSERT_TRUE(held);
    EXPECT_EQ(Surfaces(store.Lookup("apple")), std::vector<std::string>{"apple"});
    EXPECT_FALSE(fs::exists(store.paths().base));
  }
  // Nothing on disk changed, but this writer now holds the lock: it compiles
  // the .bin and persists the op instead of keeping it in memory.
  EXPECT_TRUE(store.Upsert("kotlin", 7));
  EXPECT_TRUE(EnglishBaseImage::Map(store.paths().base));
  EXPECT_EQ(Surfaces(EnglishDictionaryStore(tsv).Lookup("kotlin")),
            std::vector<std::string>{"kotlin"});
  fs::remove_all(dir);
}

TEST(EnglishDictionaryFilesTest, ACompactionWhoseStampFailsLeavesTheBaseAndOverlay) {
  const auto dir = TestDir();
  const auto tsv = dir / "english-words.tsv";
  WriteText(tsv, "apple\t50\n");
  EnglishDictionaryStore store(tsv);
  ASSERT_TRUE(store.Upsert("kotlin", 7));
  const auto base_before = ReadBytes(store.paths().base);
  const auto overlay_before = ReadBytes(store.paths().overlay);
  // The merged base cannot be stamped with the TSV's mtime: it must not go
  // live, or the next start would recompile from the TSV and drop "kotlin".
  store.SetWriteHookForTest(
      [](EnglishWriteStep step) { return step != EnglishWriteStep::BaseStamped; });
  EXPECT_FALSE(store.Compact());
  EXPECT_EQ(ReadBytes(store.paths().base), base_before);
  EXPECT_EQ(ReadBytes(store.paths().overlay), overlay_before);
  EXPECT_EQ(fs::last_write_time(store.paths().base), fs::last_write_time(tsv));
  EXPECT_EQ(Surfaces(EnglishDictionaryStore(tsv).Lookup("kotlin")),
            std::vector<std::string>{"kotlin"});
  // No temporary file is left next to it.
  size_t files = 0;
  for (const auto& entry : fs::directory_iterator(dir)) files += entry.is_regular_file() ? 1 : 0;
  EXPECT_EQ(files, 5u);  // .tsv, .bin, .delta.bin, .added.bin, .lock
  fs::remove_all(dir);
}

// Section 4.8 journal: the bytes a writer produces, for tests that need a
// journal of a given shape without appending it op by op.
std::string JournalBytes(const std::vector<EnglishOverlayOp>& ops) {
  std::string bytes = "AZEJ";
  const auto u32 = [&](uint32_t v) {
    for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<char>(v >> (8 * i)));
  };
  u32(kEnglishJournalVersion);
  u32(0);
  u32(static_cast<uint32_t>(ops.size()));
  for (const auto& op : ops) {
    bytes.push_back(static_cast<char>(op.kind));
    bytes.push_back(static_cast<char>(op.flags));
    bytes.push_back(static_cast<char>(op.key.size() & 0xFF));
    bytes.push_back(static_cast<char>(op.key.size() >> 8));
    bytes.push_back(static_cast<char>(op.surface.size() & 0xFF));
    bytes.push_back(static_cast<char>(op.surface.size() >> 8));
    u32(op.frequency);
    bytes += op.key + op.surface;
  }
  return bytes;
}

std::vector<std::string> RecordSurfaces(const std::vector<EnglishWordRecord>& records) {
  std::vector<std::string> out;
  for (const auto& r : records) out.push_back(r.surface);
  return out;
}

TEST(EnglishDictionaryJournalTest, TheLatestOpPerEntryDecidesAndTheTsvKeepsItsFrequency) {
  const auto records = ParseEnglishTsv("apple\t50\nApple\t40\tproper\nbanana\t30\n");
  const auto merged = ApplyEnglishJournal(records, {
                                                       Upsert("kotlin", 1),
                                                       Delete("kotlin"),
                                                       Delete("Swift"),
                                                       Upsert("Swift", 1),
                                                       Delete("Apple"),
                                                       Upsert("apple", 1),
                                                       Upsert("banana", 1, kEnglishWordTech),
                                                   });
  // kotlin's delete came last; Swift's upsert came last; the delete of a TSV
  // word hides it; an upsert of a TSV word keeps the TSV's record.
  EXPECT_EQ(RecordSurfaces(merged), (std::vector<std::string>{"apple", "banana", "Swift"}));
  EXPECT_EQ(merged[0].frequency, 50u);
  EXPECT_EQ(merged[1].frequency, 30u);
  EXPECT_EQ(merged[1].flags, 0u);
  EXPECT_EQ(merged[2].frequency, 1u);
}

TEST(EnglishDictionaryJournalTest, AtMostTheEntryBoundIsApplied) {
  std::vector<EnglishOverlayOp> ops;
  for (uint32_t i = 0; i <= kMaxEnglishJournalEntries; ++i) {
    ops.push_back(Upsert("w" + std::to_string(i), 1));
  }
  EXPECT_EQ(ApplyEnglishJournal({}, ops).size(), kMaxEnglishJournalEntries);
}

TEST(EnglishDictionaryJournalTest, StoreOpsOutliveATsvRecompile) {
  const auto dir = TestDir();
  const auto tsv = dir / "english-words.tsv";
  WriteText(tsv, "apple\t50\nApple\t40\tproper\n");
  EnglishDictionaryStore store(tsv);
  ASSERT_EQ(store.Lookup("apple").size(), 2u);
  EXPECT_EQ(store.Upsert("Kotlin", 1).result, EnglishWriteResult::Persisted);
  EXPECT_EQ(store.Remove("Apple").result, EnglishWriteResult::Persisted);
  EXPECT_EQ(store.Upsert("apple", 1).result, EnglishWriteResult::Persisted);
  const auto journal = ReadEnglishJournal(store.paths().journal);
  ASSERT_TRUE(journal);
  EXPECT_EQ(journal->ops.size(), 3u);

  // The TSV is edited: the base is recompiled and the overlay discarded, but
  // the journal is replayed over the new TSV.
  WriteText(tsv, "apple\t50\nApple\t40\tproper\nbanana\t30\n");
  fs::last_write_time(tsv, fs::last_write_time(store.paths().base) + std::chrono::seconds(5));
  EXPECT_EQ(Surfaces(store.Lookup("kotlin")), std::vector<std::string>{"Kotlin"});
  // The delete still hides "Apple" although the TSV has it; "apple" keeps the
  // TSV's frequency rather than the upsert's.
  const auto apple = store.Lookup("apple");
  EXPECT_EQ(Surfaces(apple), std::vector<std::string>{"apple"});
  ASSERT_EQ(apple.size(), 1u);
  EXPECT_EQ(apple[0].frequency, 50u);
  EXPECT_EQ(Surfaces(store.Lookup("banana")), std::vector<std::string>{"banana"});
  // And so does every other host, from the files alone.
  EXPECT_EQ(Surfaces(EnglishDictionaryStore(tsv).Lookup("kotlin")),
            std::vector<std::string>{"Kotlin"});
  // Compaction folds the overlay into the base but leaves the journal.
  ASSERT_TRUE(store.Upsert("Swift", 1));
  ASSERT_TRUE(store.Compact());
  EXPECT_EQ(ReadEnglishJournal(store.paths().journal)->ops.size(), 4u);
  fs::remove_all(dir);
}

TEST(EnglishDictionaryJournalTest, WithoutTheWriterLockNothingIsJournaled) {
  const auto dir = TestDir();
  const auto tsv = dir / "english-words.tsv";
  WriteText(tsv, "apple\t50\n");
  EnglishDictionaryStore store(tsv);
  ASSERT_EQ(store.Lookup("apple").size(), 1u);
  {
    const auto held = EnglishDictionaryFileLock::Acquire(store.paths().lock);
    ASSERT_TRUE(held);
    EXPECT_EQ(store.Upsert("kotlin", 7).result, EnglishWriteResult::MemoryOnly);
  }
  EXPECT_FALSE(fs::exists(store.paths().journal));
  fs::remove_all(dir);
}

TEST(EnglishDictionaryJournalTest, AppendCrashLeavesATornFrameThatIsIgnoredAndReclaimed) {
  const auto dir = TestDir();
  const auto journal = dir / "english-words.added.bin";
  // A new journal is written whole and renamed into place.
  EXPECT_EQ(AppendEnglishJournalOp(journal, Upsert("first", 1)),
            EnglishJournalAppendResult::Appended);
  const auto size_after_first = fs::file_size(journal);

  // The writer dies after the frame, before op_count covers it.
  std::vector<EnglishWriteStep> steps;
  EXPECT_EQ(AppendEnglishJournalOp(journal, Upsert("torn", 2),
                                   [&](EnglishWriteStep s) {
                                     steps.push_back(s);
                                     return s != EnglishWriteStep::JournalFrameWritten;
                                   }),
            EnglishJournalAppendResult::Failed);
  EXPECT_EQ(steps, std::vector<EnglishWriteStep>{EnglishWriteStep::JournalFrameWritten});
  EXPECT_GT(fs::file_size(journal), size_after_first);
  auto contents = ReadEnglishJournal(journal);
  ASSERT_TRUE(contents);
  ASSERT_EQ(contents->ops.size(), 1u);
  EXPECT_EQ(contents->ops[0].surface, "first");

  // The next writer truncates the torn frame and appends after "first".
  EXPECT_EQ(AppendEnglishJournalOp(journal, Upsert("second", 3)),
            EnglishJournalAppendResult::Appended);
  contents = ReadEnglishJournal(journal);
  ASSERT_TRUE(contents);
  ASSERT_EQ(contents->ops.size(), 2u);
  EXPECT_EQ(contents->ops[1].surface, "second");
  EXPECT_EQ(fs::file_size(journal), size_after_first + 10 + 2 * std::string("second").size());
  fs::remove_all(dir);
}

TEST(EnglishDictionaryJournalTest, RepeatedOpsAreRewrittenToTheLatestPerEntry) {
  const auto dir = TestDir();
  const auto journal = dir / "english-words.added.bin";
  ASSERT_EQ(AppendEnglishJournalOp(journal, Upsert("other", 1)),
            EnglishJournalAppendResult::Appended);
  for (uint32_t i = 0; i < 2 * kMinEnglishJournalRewriteOps; ++i) {
    ASSERT_EQ(AppendEnglishJournalOp(journal, Upsert("same", i)),
              EnglishJournalAppendResult::Appended);
  }
  auto contents = ReadEnglishJournal(journal);
  ASSERT_TRUE(contents);
  EXPECT_LE(contents->op_count, kMinEnglishJournalRewriteOps);
  const auto merged = ApplyEnglishJournal({}, contents->ops);
  ASSERT_EQ(merged.size(), 2u);
  EXPECT_EQ(merged[0].surface, "other");
  EXPECT_EQ(merged[1].frequency, 2 * kMinEnglishJournalRewriteOps - 1);

  // A rewrite that dies before the rename leaves the old journal in place.
  const auto before = ReadBytes(journal);
  for (uint32_t i = 0; i < 2 * kMinEnglishJournalRewriteOps; ++i) {
    const auto result = AppendEnglishJournalOp(
        journal, Upsert("same", 1000 + i),
        [](EnglishWriteStep s) { return s != EnglishWriteStep::JournalRewriteWritten; });
    if (result == EnglishJournalAppendResult::Failed) break;
  }
  contents = ReadEnglishJournal(journal);
  ASSERT_TRUE(contents);
  EXPECT_GT(contents->op_count, 0u);
  EXPECT_EQ(ApplyEnglishJournal({}, contents->ops).size(), 2u);
  // No temporary file is left next to it.
  size_t files = 0;
  for (const auto& entry : fs::directory_iterator(dir)) files += entry.is_regular_file() ? 1 : 0;
  EXPECT_EQ(files, 1u);
  EXPECT_NE(ReadBytes(journal), before);  // The frame appends before the rewrite stuck.
  fs::remove_all(dir);
}

TEST(EnglishDictionaryJournalTest, AFullJournalTakesNoNewEntryButUpdatesAKnownOne) {
  const auto dir = TestDir();
  const auto journal = dir / "english-words.added.bin";
  std::vector<EnglishOverlayOp> ops;
  for (uint32_t i = 0; i < kMaxEnglishJournalEntries; ++i) {
    ops.push_back(Upsert("w" + std::to_string(i), 1));
  }
  WriteText(journal, JournalBytes(ops));
  EXPECT_EQ(AppendEnglishJournalOp(journal, Upsert("extra", 1)), EnglishJournalAppendResult::Full);
  EXPECT_EQ(AppendEnglishJournalOp(journal, Delete("w7")), EnglishJournalAppendResult::Appended);
  const auto contents = ReadEnglishJournal(journal);
  ASSERT_TRUE(contents);
  EXPECT_EQ(contents->ops.size(), kMaxEnglishJournalEntries + 1);

  // The store keeps the op in the overlay and says the journal was full.
  const auto tsv = dir / "english-words.tsv";
  WriteText(tsv, "apple\t50\n");
  EnglishDictionaryStore store(tsv);
  EXPECT_EQ(store.Upsert("extra", 1).result, EnglishWriteResult::JournalFull);
  EXPECT_EQ(Surfaces(EnglishDictionaryStore(tsv).Lookup("extra")),
            std::vector<std::string>{"extra"});
  fs::remove_all(dir);
}

TEST(EnglishDictionaryJournalTest, AnUnreadableJournalIsMovedAsideAndStartsAnew) {
  const auto dir = TestDir();
  const auto tsv = dir / "english-words.tsv";
  WriteText(tsv, "apple\t50\n");
  EnglishDictionaryStore store(tsv);
  ASSERT_EQ(store.Lookup("apple").size(), 1u);  // Compiles the base on first use.
  WriteText(store.paths().journal, "garbage");
  // A recompile with it applies nothing.
  WriteText(tsv, "apple\t50\nbanana\t30\n");
  fs::last_write_time(tsv, fs::last_write_time(store.paths().base) + std::chrono::seconds(5));
  EXPECT_EQ(Surfaces(store.Lookup("banana")), std::vector<std::string>{"banana"});

  EXPECT_EQ(store.Upsert("kotlin", 1).result, EnglishWriteResult::JournalReset);
  const auto journal = ReadEnglishJournal(store.paths().journal);
  ASSERT_TRUE(journal);
  ASSERT_EQ(journal->ops.size(), 1u);
  EXPECT_EQ(journal->ops[0].surface, "kotlin");
  size_t aside = 0;
  for (const auto& entry : fs::directory_iterator(dir)) {
    const auto name = entry.path().filename().string();
    if (name.rfind("english-words.added.bin.corrupt.", 0) == 0) {
      ++aside;
      EXPECT_EQ(ReadBytes(entry.path()), "garbage");
    }
  }
  EXPECT_EQ(aside, 1u);
  fs::remove_all(dir);
}

// A journal that exists but cannot be opened or read now is neither replaced
// by a new one nor moved aside: the append fails and the file stays. A
// directory in its place stands in for it (it fails to open on Windows and to
// read elsewhere).
TEST(EnglishDictionaryJournalTest, AJournalThatCannotBeReadNowIsLeftInPlace) {
  const auto dir = TestDir();
  const auto journal = dir / "english-words.added.bin";
  fs::create_directories(journal / "inside");
  EXPECT_EQ(AppendEnglishJournalOp(journal, Upsert("kotlin", 1)),
            EnglishJournalAppendResult::Failed);
  EXPECT_TRUE(fs::is_directory(journal / "inside"));
  size_t entries = 0;
  for (const auto& entry : fs::directory_iterator(dir)) {
    (void)entry;
    ++entries;
  }
  EXPECT_EQ(entries, 1u);
  fs::remove_all(dir);
}

// The overlay took the op but the journal did not: the op stays for this and
// every other host until the next TSV recompile, and the result says so.
TEST(EnglishDictionaryJournalTest, AJournalFailureAfterTheOverlayKeepsTheOpInTheOverlay) {
  const auto dir = TestDir();
  const auto tsv = dir / "english-words.tsv";
  WriteText(tsv, "apple\t50\n");
  EnglishDictionaryStore store(tsv);
  ASSERT_EQ(store.Lookup("apple").size(), 1u);
  // A new journal is written whole and renamed; stop before the rename.
  store.SetWriteHookForTest(
      [](EnglishWriteStep step) { return step != EnglishWriteStep::JournalRewriteWritten; });
  const auto outcome = store.Upsert("kotlin", 7);
  EXPECT_EQ(outcome.result, EnglishWriteResult::JournalFailed);
  EXPECT_TRUE(outcome);
  store.SetWriteHookForTest({});
  EXPECT_FALSE(fs::exists(store.paths().journal));
  EXPECT_EQ(Surfaces(EnglishDictionaryStore(tsv).Lookup("kotlin")),
            std::vector<std::string>{"kotlin"});
  fs::remove_all(dir);
}

// Two hosts writing at once each journal every op under the writer lock.
TEST(EnglishDictionaryJournalTest, TwoStoresWritingAtOnceJournalEveryOp) {
  const auto dir = TestDir();
  const auto tsv = dir / "english-words.tsv";
  WriteText(tsv, "apple\t50\n");
  EnglishDictionaryStore first(tsv);
  EnglishDictionaryStore second(tsv);
  ASSERT_EQ(first.Lookup("apple").size(), 1u);
  ASSERT_EQ(second.Lookup("apple").size(), 1u);
  constexpr int kWords = 20;
  const auto write = [](EnglishDictionaryStore& store, const std::string& prefix) {
    int persisted = 0;
    for (int i = 0; i < kWords; ++i) {
      if (store.Upsert(prefix + std::to_string(i), 1).result == EnglishWriteResult::Persisted)
        ++persisted;
    }
    return persisted;
  };
  auto a = std::async(std::launch::async, [&] { return write(first, "alpha"); });
  auto b = std::async(std::launch::async, [&] { return write(second, "beta"); });
  const int persisted = a.get() + b.get();
  const auto journal = ReadEnglishJournal(first.paths().journal);
  ASSERT_TRUE(journal);
  // An op whose writer lock wait ran out stays in memory and is not journaled.
  EXPECT_EQ(journal->ops.size(), static_cast<size_t>(persisted));
  EXPECT_GT(persisted, 0);
  fs::remove_all(dir);
}

// A journal that is there but cannot be read when the TSV is recompiled is not
// taken for a missing one: the base is served from memory without a stamp,
// and the next writer that holds the lock compiles it again with the journal.
TEST(EnglishDictionaryJournalTest, ARecompileThatCannotReadTheJournalDoesNotStampTheBase) {
  const auto dir = TestDir();
  const auto tsv = dir / "english-words.tsv";
  WriteText(tsv, "apple\t50\n");
  EnglishDictionaryStore store(tsv);
  ASSERT_EQ(store.Lookup("apple").size(), 1u);
  ASSERT_EQ(store.Upsert("Kotlin", 1).result, EnglishWriteResult::Persisted);
  // The journal cannot be read for now: a directory stands in its place.
  const auto kept = dir / "journal.kept";
  fs::rename(store.paths().journal, kept);
  fs::create_directories(store.paths().journal / "inside");
  WriteText(tsv, "apple\t50\nbanana\t30\n");
  fs::last_write_time(tsv, fs::last_write_time(store.paths().base) + std::chrono::seconds(5));
  EXPECT_EQ(Surfaces(store.Lookup("banana")), std::vector<std::string>{"banana"});
  EXPECT_NE(fs::last_write_time(store.paths().base), fs::last_write_time(tsv));

  // Readable again: the next write compiles the base with the journal and
  // stamps it.
  fs::remove_all(store.paths().journal);
  fs::rename(kept, store.paths().journal);
  ASSERT_TRUE(store.Upsert("Swift", 1));
  EXPECT_EQ(fs::last_write_time(store.paths().base), fs::last_write_time(tsv));
  EXPECT_EQ(Surfaces(EnglishDictionaryStore(tsv).Lookup("kotlin")),
            std::vector<std::string>{"Kotlin"});
  EXPECT_EQ(Surfaces(EnglishDictionaryStore(tsv).Lookup("swift")),
            std::vector<std::string>{"Swift"});
  fs::remove_all(dir);
}

// While the journal stays unreadable, the same TSV is compiled once: neither
// the writes nor the reads after them compile it again. A writer that finds
// the journal readable compiles and stamps it.
TEST(EnglishDictionaryJournalTest, AnUnreadableJournalCostsOneCompilePerTsv) {
  const auto dir = TestDir();
  const auto tsv = dir / "english-words.tsv";
  WriteText(tsv, "apple\t50\n");
  int compiles = 0;
  EnglishDictionaryStore store(tsv, [&](const EnglishDictionaryLoadReport& report) {
    if (std::string(report.source) == "tsv") ++compiles;
  });
  ASSERT_EQ(store.Lookup("apple").size(), 1u);
  ASSERT_EQ(compiles, 1);
  fs::create_directories(store.paths().journal / "inside");
  WriteText(tsv, "apple\t50\nbanana\t30\n");
  fs::last_write_time(tsv, fs::last_write_time(store.paths().base) + std::chrono::seconds(5));
  ASSERT_EQ(store.Lookup("banana").size(), 1u);
  EXPECT_EQ(compiles, 2);
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(store.Upsert("word" + std::to_string(i), 1).result, EnglishWriteResult::MemoryOnly);
    EXPECT_EQ(store.Lookup("word" + std::to_string(i)).size(), 1u);
  }
  EXPECT_EQ(compiles, 2);
  EXPECT_NE(fs::last_write_time(store.paths().base), fs::last_write_time(tsv));

  fs::remove_all(store.paths().journal);
  EXPECT_EQ(store.Upsert("Swift", 1).result, EnglishWriteResult::Persisted);
  EXPECT_EQ(compiles, 3);
  EXPECT_EQ(fs::last_write_time(store.paths().base), fs::last_write_time(tsv));
  fs::remove_all(dir);
}
