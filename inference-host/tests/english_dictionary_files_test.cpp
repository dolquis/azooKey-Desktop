#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
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
  ASSERT_TRUE(store.Remove("apple"));
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
    auto held = EnglishDictionaryFileLock::Acquire(store.paths().lock);
    ASSERT_TRUE(held);
    std::thread append([&] { persisted = store.Upsert("Swift", 3); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    // Another host compacts while this one waits for the lock.
    ASSERT_TRUE(CompactEnglishDictionary(store.paths()));
    held.reset();
    append.join();
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

TEST(EnglishDictionaryFilesTest, AReaderDoesNotWaitBehindAWriter) {
  const auto dir = TestDir();
  const auto tsv = dir / "english-words.tsv";
  WriteText(tsv, "apple\t50\n");
  EnglishDictionaryStore store(tsv);
  ASSERT_EQ(store.Lookup("apple").size(), 1u);
  auto held = EnglishDictionaryFileLock::Acquire(store.paths().lock);
  ASSERT_TRUE(held);
  // The writer waits up to 500 ms for the file lock held above.
  std::thread writer([&] { (void)store.Upsert("kotlin", 7); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  // A changed TSV forces a rebuild; the reader keeps the previous snapshot
  // rather than queueing behind the writer.
  WriteText(tsv, "Apple\t50\n");
  fs::last_write_time(tsv, fs::last_write_time(store.paths().base) + std::chrono::seconds(5));
  const auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(Surfaces(store.Lookup("apple")), std::vector<std::string>{"apple"});
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(300));
  held.reset();
  writer.join();
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
  EXPECT_EQ(files, 4u);  // .tsv, .bin, .delta.bin, .lock
  fs::remove_all(dir);
}
