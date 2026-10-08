#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "../../learning/tests/TestByteCrypto.h"
#include "azookey/core/PlatformPaths.h"
#include "azookey/core/SimpleConverter.h"
#include "azookey/host/EnglishCandidates.h"
#include "azookey/host/InferenceEngine.h"
#include "azookey/learning/LearningStore.h"

namespace {

namespace fs = std::filesystem;
using azookey::core::Candidate;
using azookey::core::CandidateTag;
using azookey::host::EnglishCandidateConfig;

std::vector<std::string> Surfaces(const std::vector<Candidate>& candidates) {
  std::vector<std::string> out;
  for (const auto& c : candidates) out.push_back(c.surface);
  return out;
}

EnglishCandidateConfig AllForms() {
  EnglishCandidateConfig config;
  config.full_width = true;
  return config;
}

fs::path TestDir() {
  const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
  auto path = fs::temp_directory_path() /
              (std::string("azookey_english_") + info->test_suite_name() + "_" + info->name());
  fs::remove_all(path);
  fs::create_directories(path);
  return path;
}

void WriteText(const fs::path& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary);
  out << text;
}

Candidate Japanese(const char* surface, double score) {
  Candidate c;
  c.surface = surface;
  c.reading = "かな";
  c.score = score;
  return c;
}

}  // namespace

TEST(EnglishCandidatesTest, BuildsTheSixFormsFromTheLowercasedRomaji) {
  for (const char* typed : {"apple", "Apple", "APPLE"}) {
    const auto result = azookey::host::BuildEnglishCandidates(typed, AllForms(), nullptr);
    EXPECT_EQ(Surfaces(result.candidates),
              (std::vector<std::string>{"apple", "Apple", "APPLE", "ａｐｐｌｅ", "Ａｐｐｌｅ",
                                        "ＡＰＰＬＥ"}))
        << typed;
    for (const auto& c : result.candidates) {
      EXPECT_EQ(c.tag, CandidateTag::English);
      EXPECT_EQ(c.reading, typed);  // Section 6.3: the raw romaji.
      EXPECT_EQ(c.source, azookey::core::CandidateSource::Heuristic);
    }
  }
}

TEST(EnglishCandidatesTest, KeepsAnUnusualCasingAsItsOwnCandidate) {
  const auto result = azookey::host::BuildEnglishCandidates("aPPle", {}, nullptr);
  EXPECT_EQ(Surfaces(result.candidates),
            (std::vector<std::string>{"aPPle", "apple", "Apple", "APPLE"}));
}

TEST(EnglishCandidatesTest, TogglesRemoveVariants) {
  EnglishCandidateConfig config;
  config.case_variants = false;
  EXPECT_EQ(Surfaces(azookey::host::BuildEnglishCandidates("apple", config, nullptr).candidates),
            (std::vector<std::string>{"apple"}));
  config.full_width = true;
  EXPECT_EQ(Surfaces(azookey::host::BuildEnglishCandidates("apple", config, nullptr).candidates),
            (std::vector<std::string>{"apple", "ａｐｐｌｅ"}));
}

TEST(EnglishCandidatesTest, GatesShortSymbolOnlyAndNonAsciiInput) {
  EXPECT_TRUE(azookey::host::BuildEnglishCandidates("a", {}, nullptr).candidates.empty());
  EnglishCandidateConfig longer;
  longer.min_length = 4;
  EXPECT_TRUE(azookey::host::BuildEnglishCandidates("abc", longer, nullptr).candidates.empty());
  EXPECT_FALSE(azookey::host::BuildEnglishCandidates("abcd", longer, nullptr).candidates.empty());
  EXPECT_TRUE(azookey::host::BuildEnglishCandidates("1234", {}, nullptr).candidates.empty());
  EXPECT_TRUE(azookey::host::BuildEnglishCandidates("!?", {}, nullptr).candidates.empty());
  EXPECT_TRUE(azookey::host::BuildEnglishCandidates("あい", {}, nullptr).candidates.empty());
  EXPECT_TRUE(azookey::host::BuildEnglishCandidates("ab cd", {}, nullptr).candidates.empty());
  // A pending trailing n is still romaji material (section 4.3 edges).
  EXPECT_FALSE(azookey::host::BuildEnglishCandidates("kan", {}, nullptr).candidates.empty());
}

TEST(EnglishCandidatesTest, IntentSignalsFollowSection42) {
  using azookey::host::ComputeEnglishIntent;
  const auto ko = ComputeEnglishIntent("ko", false, 2);
  EXPECT_DOUBLE_EQ(ko.nonkana, 0.0);
  EXPECT_DOUBLE_EQ(ko.length, 0.0);
  EXPECT_DOUBLE_EQ(ko.Score(), 0.0);
  const auto the = ComputeEnglishIntent("Thanks", true, 2);
  EXPECT_DOUBLE_EQ(the.dict, 1.0);
  EXPECT_DOUBLE_EQ(the.nonkana, 1.0);  // "thanks" leaves ASCII after kana conversion.
  EXPECT_DOUBLE_EQ(the.cluster, 1.0);
  EXPECT_DOUBLE_EQ(the.length, 1.0);
  EXPECT_DOUBLE_EQ(the.upper, 1.0);
  EXPECT_DOUBLE_EQ(the.Score(), 1.0);
}

TEST(EnglishCandidatesTest, ParsesTheTsvDictionary) {
  azookey::host::EnglishDictionaryLoadStats stats;
  const auto dictionary = azookey::host::EnglishDictionary::ParseTsv(
      "\xEF\xBB\xBF# comment\n"
      "apple\t542316\n"
      "Apple\t118242\tproper\r\n"
      "\n"
      "GitHub\t30551\tproper,tech\n"
      "broken\tnot-a-number\n"
      "zero\t0\n"
      "\t5\n"
      "apple\t9\n",
      &stats);
  EXPECT_EQ(stats.skipped_lines, 3u);
  EXPECT_FALSE(stats.truncated);
  EXPECT_EQ(dictionary.size(), 3u);
  const auto& apple = dictionary.Lookup("apple");
  ASSERT_EQ(apple.size(), 2u);
  // The later "apple" line wins, so the proper noun now ranks first.
  EXPECT_EQ(apple[0].surface, "Apple");
  EXPECT_EQ(apple[0].flags, azookey::host::kEnglishWordProper);
  EXPECT_EQ(apple[1].surface, "apple");
  EXPECT_EQ(apple[1].frequency, 9u);
  const auto& github = dictionary.Lookup("github");
  ASSERT_EQ(github.size(), 1u);
  EXPECT_EQ(github[0].flags, azookey::host::kEnglishWordProper | azookey::host::kEnglishWordTech);
  EXPECT_TRUE(dictionary.Lookup("GitHub").empty());  // Keys are lowercase.
}

TEST(EnglishCandidatesTest, DictionarySurfacesComeFirstWithoutReplacingTheForms) {
  const auto dictionary = azookey::host::EnglishDictionary::ParseTsv("iPhone\t100\n");
  EnglishCandidateConfig config;
  config.dictionary_enabled = true;
  const auto with = azookey::host::BuildEnglishCandidates("iphone", config, &dictionary);
  EXPECT_EQ(Surfaces(with.candidates),
            (std::vector<std::string>{"iPhone", "iphone", "Iphone", "IPHONE"}));
  const auto without = azookey::host::BuildEnglishCandidates("iphone", {}, &dictionary);
  EXPECT_EQ(Surfaces(without.candidates), (std::vector<std::string>{"iphone", "Iphone", "IPHONE"}));
  EXPECT_GT(with.intent, without.intent);  // s_dict.
}

TEST(EnglishCandidatesTest, DictionarySkipsNonUtf8AndStopsAtTheEntryCap) {
  azookey::host::EnglishDictionaryLoadStats stats;
  const auto dictionary = azookey::host::EnglishDictionary::ParseTsv(
      "good\t5\nb\xFF\xFE"
      "d\t5\nok\t1\n",
      &stats);
  EXPECT_EQ(stats.skipped_lines, 1u);
  EXPECT_EQ(dictionary.size(), 2u);
  EXPECT_TRUE(dictionary
                  .Lookup("b\xFF\xFE"
                          "d")
                  .empty());

  std::string big;
  for (size_t i = 0; i <= azookey::host::kMaxEnglishDictionaryEntries; ++i)
    big += "w" + std::to_string(i) + "\t1\n";
  azookey::host::EnglishDictionaryLoadStats big_stats;
  const auto capped = azookey::host::EnglishDictionary::ParseTsv(big, &big_stats);
  EXPECT_TRUE(big_stats.truncated);
  EXPECT_EQ(capped.size(), azookey::host::kMaxEnglishDictionaryEntries);
}

TEST(EnglishCandidatesTest, ProperAndAcronymFlagsReorderTheHalfWidthForms) {
  EnglishCandidateConfig config;
  config.dictionary_enabled = true;
  const auto proper = azookey::host::EnglishDictionary::ParseTsv("Apple\t5\tproper\n");
  EXPECT_EQ(Surfaces(azookey::host::BuildEnglishCandidates("apple", config, &proper).candidates),
            (std::vector<std::string>{"Apple", "apple", "APPLE"}));
  const auto acronym = azookey::host::EnglishDictionary::ParseTsv("nasa\t5\tacronym\n");
  EXPECT_EQ(Surfaces(azookey::host::BuildEnglishCandidates("nasa", config, &acronym).candidates),
            (std::vector<std::string>{"nasa", "NASA", "Nasa"}));
}

TEST(EnglishCandidatesTest, DictionarySurfacesAreCappedAtFive) {
  // Seven surfaces under the one key "abc", most frequent first.
  const auto dictionary = azookey::host::EnglishDictionary::ParseTsv(
      "ABc\t9\nAbC\t8\naBC\t7\nAbc\t6\nabC\t5\naBc\t4\nABC\t3\n");
  EnglishCandidateConfig config;
  config.dictionary_enabled = true;
  config.case_variants = false;
  const auto result = azookey::host::BuildEnglishCandidates("abc", config, &dictionary);
  EXPECT_EQ(Surfaces(result.candidates),
            (std::vector<std::string>{"ABc", "AbC", "aBC", "Abc", "abC", "abc"}));
}

TEST(EnglishCandidatesTest, LearnedSurfacesFollowTheDictionary) {
  const auto result =
      azookey::host::BuildEnglishCandidates("apple", {}, nullptr, {"APPLE", "Apple Inc."});
  EXPECT_EQ(Surfaces(result.candidates),
            (std::vector<std::string>{"APPLE", "Apple Inc.", "apple", "Apple"}));
}

TEST(EnglishCandidatesTest, PlacementNeverTakesTheFirstJapaneseCandidate) {
  const auto english = azookey::host::BuildEnglishCandidates("apple", {}, nullptr).candidates;
  std::vector<Candidate> weak = {Japanese("あ", 9), Japanese("い", 8), Japanese("う", 7),
                                 Japanese("え", 6), Japanese("お", 5), Japanese("か", 4)};
  azookey::host::PlaceEnglishCandidates(weak, english, 0.2, 0.6);
  ASSERT_EQ(weak.size(), 9u);
  EXPECT_EQ(weak[0].surface, "あ");
  EXPECT_EQ(weak[5].surface, "apple");  // Below the top five Japanese.
  EXPECT_EQ(weak[8].surface, "か");
  EXPECT_DOUBLE_EQ(weak[5].score, 5.0);

  std::vector<Candidate> strong = {Japanese("あ", 9), Japanese("い", 8)};
  azookey::host::PlaceEnglishCandidates(strong, english, 0.6, 0.6);
  EXPECT_EQ(Surfaces(strong), (std::vector<std::string>{"あ", "apple", "Apple", "APPLE", "い"}));

  std::vector<Candidate> duplicate = {Japanese("Apple", 9)};
  azookey::host::PlaceEnglishCandidates(duplicate, english, 1.0, 0.6);
  EXPECT_EQ(Surfaces(duplicate), (std::vector<std::string>{"Apple", "apple", "APPLE"}));
}

TEST(EnglishCandidatesTest, OnlyAsciiReadingsWithTheEnglishTagAreEnglishCommits) {
  constexpr auto kEnglish = static_cast<uint8_t>(CandidateTag::English);
  EXPECT_TRUE(azookey::host::IsEnglishObservation("apple", kEnglish));
  EXPECT_FALSE(azookey::host::IsEnglishObservation("apple", 0));
  EXPECT_FALSE(azookey::host::IsEnglishObservation("あいふぉん", kEnglish));
  EXPECT_FALSE(azookey::host::IsEnglishObservation("", kEnglish));
}

TEST(EnglishCandidatesTest, EngineKeepsEnglishLearningOutOfTheKanaStore) {
  const auto dir = TestDir();
  azookey::learning::LearningStore kana(dir / "learning.tsv", &azookey::learning::test::Crypto());
  azookey::learning::LearningStore english(dir / "english_learning.tsv",
                                           &azookey::learning::test::Crypto());
  azookey::host::InferenceEngine engine(std::make_unique<azookey::core::SimpleConverter>(), &kana,
                                        {});
  engine.SetEnglishLearningStore(&english);
  constexpr uint64_t kNow = 1'700'000'000;

  EXPECT_TRUE(engine.CommitEnglishObservation("Apple", "Apple Inc.", kNow, "obs-1"));
  EXPECT_FALSE(engine.CommitEnglishObservation("Apple", "Apple Inc.", kNow, "obs-1"));
  EXPECT_EQ(kana.size(), 0u);
  EXPECT_EQ(english.size(), 1u);
  EXPECT_GT(english.Score("apple", "Apple Inc.", kNow), 0.0);
  EXPECT_FALSE(english.dirty());  // Saved on commit.

  // Retyping the same romaji offers the learned surface again (section 5).
  const auto again = engine.QueryEnglishCandidates("apple", kNow);
  EXPECT_EQ(Surfaces(again.candidates),
            (std::vector<std::string>{"Apple Inc.", "apple", "Apple", "APPLE"}));

  // A multi-segment commit routes its English segment the same way.
  azookey::ipc::CommitSegmentsObservationRequest segments;
  azookey::ipc::ObservedSegment kana_segment;
  kana_segment.reading = "きょう";
  kana_segment.chosen.surface = "今日";
  azookey::ipc::ObservedSegment english_segment;
  english_segment.reading = "github";
  english_segment.chosen.surface = "GitHub";
  english_segment.chosen.tag = static_cast<uint8_t>(CandidateTag::English);
  segments.segments = {kana_segment, english_segment};
  EXPECT_TRUE(engine.CommitSegmentsObservation(segments, kNow));
  EXPECT_GT(kana.Score("きょう", "今日", kNow), 0.0);
  EXPECT_DOUBLE_EQ(kana.Score("github", "GitHub", kNow), 0.0);
  EXPECT_GT(english.Score("github", "GitHub", kNow), 0.0);
  fs::remove_all(dir);
}

TEST(EnglishCandidatesTest, EngineLoadsAndReloadsTheDictionary) {
  const auto dir = TestDir();
  const auto path = dir / "english-words.tsv";
  WriteText(path, "iPhone\t100\n");
  azookey::host::InferenceEngine engine(std::make_unique<azookey::core::SimpleConverter>(), nullptr,
                                        {});
  azookey::host::EngineConfig config;
  config.english.dictionary_enabled = true;
  config.english.dictionary_path = azookey::core::PathToUtf8(path);
  engine.ApplyConfig(config);
  EXPECT_EQ(engine.QueryEnglishCandidates("iphone", 0).candidates.front().surface, "iPhone");

  WriteText(path, "IPHONE\t100\n");
  fs::last_write_time(path, fs::last_write_time(path) + std::chrono::seconds(5));
  EXPECT_EQ(engine.QueryEnglishCandidates("iphone", 0).candidates.front().surface, "IPHONE");

  fs::remove(path);  // A missing dictionary falls back to the baseline forms.
  EXPECT_EQ(engine.QueryEnglishCandidates("iphone", 0).candidates.front().surface, "iphone");

  // A corrupt file (not text at all) or a directory in its place is not fatal.
  WriteText(path, std::string("\x00\xFF\x01\xFE\tGGUF", 9));
  EXPECT_EQ(engine.QueryEnglishCandidates("iphone", 0).candidates.front().surface, "iphone");
  fs::remove(path);
  fs::create_directories(path);
  EXPECT_EQ(engine.QueryEnglishCandidates("iphone", 0).candidates.front().surface, "iphone");
  fs::remove_all(dir);
}
