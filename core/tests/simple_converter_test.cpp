#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "azookey/core/SimpleConverter.h"

namespace {

const azookey::core::Candidate* FindCandidate(const std::vector<azookey::core::Candidate>& candidates,
                                              const std::string& surface) {
  const auto it = std::find_if(candidates.begin(), candidates.end(), [&](const azookey::core::Candidate& c) {
    return c.surface == surface;
  });
  return it == candidates.end() ? nullptr : &*it;
}

// Fixture that writes TSV fixtures and removes them in TearDown so a failing
// assertion never leaves a stale file behind.
class SimpleConverterTsvTest : public ::testing::Test {
 protected:
  void WriteFixture(const std::string& path, const std::string& contents) {
    std::ofstream f(path);
    f << contents;
    f.close();
    temp_files_.push_back(path);
  }

  void TearDown() override {
    for (const auto& path : temp_files_) {
      std::remove(path.c_str());
    }
  }

 private:
  std::vector<std::string> temp_files_;
};

}  // namespace

TEST(SimpleConverterTest, BuiltinDictionary) {
  azookey::core::SimpleConverter converter;
  const auto candidates = converter.Convert("にほん", azookey::core::ConversionContext{});
  EXPECT_GE(candidates.size(), 3u);
  EXPECT_EQ(candidates.front().surface, "日本");

  converter.Learn("二本", "にほん");
  const auto relearned = converter.Convert("にほん", azookey::core::ConversionContext{});
  EXPECT_GE(relearned.size(), 3u);
}

TEST_F(SimpleConverterTsvTest, TsvLoad) {
  const std::string path = "simple_converter_tsv_fixture.tsv";
  WriteFixture(path,
               "# comment line\n"
               "\n"
               "あずきい\tazooKey\t1.0\tuser\n"
               "あずきい\tあずきい\t0.4\tidentity\n"
               "malformed line without tabs\n"
               "てすと\tテスト\tnotanumber\tuser\n");  // last row dropped (bad score)

  azookey::core::SimpleConverter converter;
  ASSERT_TRUE(converter.LoadFromTsv(path));

  const auto candidates = converter.Convert("あずきい", azookey::core::ConversionContext{});
  ASSERT_EQ(candidates.size(), 2u);
  EXPECT_EQ(candidates.front().surface, "azooKey");
  EXPECT_EQ(candidates.front().score, 1.0);

  // Built-in entry is preserved alongside TSV entries.
  const auto nihon = converter.Convert("にほん", azookey::core::ConversionContext{});
  EXPECT_FALSE(nihon.empty());

  // Missing file returns false but does not throw.
  EXPECT_FALSE(converter.LoadFromTsv("/nonexistent/azookey_no_such_file.tsv"));
}

TEST_F(SimpleConverterTsvTest, BigramContextUsesSuffixMatch) {
  const std::string path = "simple_converter_suffix_fixture.tsv";
  WriteFixture(path, "にほん\tNIPPON\t1.10\tfixture\n");

  azookey::core::SimpleConverter converter;
  ASSERT_TRUE(converter.LoadFromTsv(path));

  azookey::core::ConversionContext context;
  context.preceding_text = "きょうはにっぽん";
  const auto candidates = converter.Convert("にほん", context);
  ASSERT_FALSE(candidates.empty());
  EXPECT_EQ(candidates.front().surface, "日本");

  const auto* nihon = FindCandidate(candidates, "日本");
  ASSERT_NE(nihon, nullptr);
  EXPECT_NE(nihon->debug_info.find("ctx-bigram"), std::string::npos);
  EXPECT_NEAR(nihon->score, 1.15, 0.000001);
}

TEST_F(SimpleConverterTsvTest, BigramTsvLoadsAdditionalPairsAndSkipsBadRows) {
  const std::string dict_path = "simple_converter_bigram_dict_fixture.tsv";
  const std::string bigram_path = "simple_converter_bigram_fixture.tsv";
  WriteFixture(dict_path,
               "あずきい\tvalid\t1.0\tfixture\n"
               "あずきい\tbad\t1.1\tfixture\n");
  WriteFixture(bigram_path,
               "# comment line\n"
               "\n"
               "ctx\tbad\tnotanumber\n"
               "ctx\tvalid\t0.2\n"
               "malformed line without tabs\n"
               "\tmissing-key\t0.5\n"
               "ctx\t\t0.5\n");

  azookey::core::SimpleConverter converter;
  ASSERT_TRUE(converter.LoadFromTsv(dict_path));
  ASSERT_TRUE(converter.LoadBigramFromTsv(bigram_path));

  azookey::core::ConversionContext context;
  context.preceding_text = "prefix ctx";
  const auto candidates = converter.Convert("あずきい", context);
  ASSERT_FALSE(candidates.empty());
  EXPECT_EQ(candidates.front().surface, "valid");

  const auto* valid = FindCandidate(candidates, "valid");
  ASSERT_NE(valid, nullptr);
  EXPECT_NE(valid->debug_info.find("ctx-bigram"), std::string::npos);
  EXPECT_NEAR(valid->score, 1.2, 0.000001);

  const auto* bad = FindCandidate(candidates, "bad");
  ASSERT_NE(bad, nullptr);
  EXPECT_EQ(bad->debug_info.find("ctx-bigram"), std::string::npos);
  EXPECT_NEAR(bad->score, 1.1, 0.000001);

  EXPECT_FALSE(converter.LoadBigramFromTsv("/nonexistent/azookey_no_such_bigram_file.tsv"));
}

TEST_F(SimpleConverterTsvTest, EmptyPrecedingTextDoesNotApplyBigramBonus) {
  const std::string dict_path = "simple_converter_empty_context_dict_fixture.tsv";
  const std::string bigram_path = "simple_converter_empty_context_bigram_fixture.tsv";
  WriteFixture(dict_path,
               "あずきい\tvalid\t1.0\tfixture\n"
               "あずきい\tplain\t1.1\tfixture\n");
  WriteFixture(bigram_path, "ctx\tvalid\t0.2\n");

  azookey::core::SimpleConverter converter;
  ASSERT_TRUE(converter.LoadFromTsv(dict_path));
  ASSERT_TRUE(converter.LoadBigramFromTsv(bigram_path));

  const auto candidates = converter.Convert("あずきい", azookey::core::ConversionContext{});
  ASSERT_FALSE(candidates.empty());
  EXPECT_EQ(candidates.front().surface, "plain");

  const auto* valid = FindCandidate(candidates, "valid");
  ASSERT_NE(valid, nullptr);
  EXPECT_EQ(valid->debug_info.find("ctx-bigram"), std::string::npos);
  EXPECT_NEAR(valid->score, 1.0, 0.000001);
}

TEST_F(SimpleConverterTsvTest, BigramContextUsesLongestSuffixOnly) {
  const std::string dict_path = "simple_converter_longest_bigram_dict_fixture.tsv";
  const std::string bigram_path = "simple_converter_longest_bigram_fixture.tsv";
  WriteFixture(dict_path,
               "てすと\t短い\t1.0\tshort\n"
               "てすと\t長い\t1.0\tlong\n");
  WriteFixture(bigram_path,
               "にっぽん\t短い\t10.0\n"
               "きょうはにっぽん\t長い\t0.2\n");

  azookey::core::SimpleConverter converter;
  ASSERT_TRUE(converter.LoadFromTsv(dict_path));
  ASSERT_TRUE(converter.LoadBigramFromTsv(bigram_path));

  azookey::core::ConversionContext context;
  context.preceding_text = "きょうはにっぽん";
  const auto candidates = converter.Convert("てすと", context);
  ASSERT_FALSE(candidates.empty());
  EXPECT_EQ(candidates.front().surface, "長い");

  const auto* long_match = FindCandidate(candidates, "長い");
  ASSERT_NE(long_match, nullptr);
  EXPECT_NE(long_match->debug_info.find("ctx-bigram"), std::string::npos);
  EXPECT_NEAR(long_match->score, 1.2, 0.000001);

  const auto* short_match = FindCandidate(candidates, "短い");
  ASSERT_NE(short_match, nullptr);
  EXPECT_EQ(short_match->debug_info.find("ctx-bigram"), std::string::npos);
  EXPECT_NEAR(short_match->score, 1.0, 0.000001);
}

TEST(SimpleConverterTest, PrefixFallback) {
  azookey::core::SimpleConverter converter;
  const auto candidates = converter.Convert("にほん", azookey::core::ConversionContext{});
  EXPECT_FALSE(candidates.empty());

  const auto cands = converter.Convert("とうきょ", azookey::core::ConversionContext{});
  EXPECT_FALSE(cands.empty());
}

TEST_F(SimpleConverterTsvTest, LongReadingPrefixFallback) {
  const std::string path = "simple_converter_long_reading_fixture.tsv";
  WriteFixture(path,
               "あずきいきーぼーど\tazooKey Keyboard\t1.0\tuser\n"
               "あずきいきーぼーど\tあずきいキーボード\t0.8\tuser\n");

  azookey::core::SimpleConverter converter;
  ASSERT_TRUE(converter.LoadFromTsv(path));

  const auto cands = converter.Convert("あずきいきーぼー", azookey::core::ConversionContext{});
  ASSERT_FALSE(cands.empty());
  EXPECT_EQ(cands.front().surface, "azooKey Keyboard");
  EXPECT_EQ(cands.front().reading, "あずきいきーぼーど");
}

TEST(SimpleConverterTest, DebugInfoFormatting) {
  azookey::core::SimpleConverter converter;

  azookey::core::ConversionContext rejection_context;
  rejection_context.rejected_surfaces = {"未知"};
  const auto unknown = converter.Convert("未知", rejection_context);
  ASSERT_FALSE(unknown.empty());
  auto rejected_it = std::find_if(unknown.begin(), unknown.end(), [](const azookey::core::Candidate& c) {
    return c.surface == "未知";
  });
  ASSERT_NE(rejected_it, unknown.end());
  EXPECT_EQ(rejected_it->debug_info, "identity;ctx-rejected");

  const auto predicted = converter.PredictNext("未知", azookey::core::ConversionContext{});
  ASSERT_FALSE(predicted.empty());
  auto predicted_it = std::find_if(predicted.begin(), predicted.end(), [](const azookey::core::Candidate& c) {
    return c.surface == "未知";
  });
  ASSERT_NE(predicted_it, predicted.end());
  EXPECT_EQ(predicted_it->debug_info, "identity;predict");
}

TEST(SimpleConverterTest, ContextAware) {
  azookey::core::SimpleConverter converter;

  azookey::core::CorrectionHint hint;
  hint.rejected_surface = "日本";
  const auto corrected = converter.Correct("にほん", hint, azookey::core::ConversionContext{});
  ASSERT_FALSE(corrected.empty());
  EXPECT_NE(corrected.front().surface, "日本");

  azookey::core::ConversionContext rejection_context;
  rejection_context.rejected_surfaces = {"日本"};
  const auto context_filtered = converter.Convert("にほん", rejection_context);
  ASSERT_FALSE(context_filtered.empty());
  EXPECT_NE(context_filtered.front().surface, "日本");

  azookey::core::ConversionContext bigram_context;
  bigram_context.preceding_text = "にっぽん";
  const auto bigram_ranked = converter.Convert("にほん", bigram_context);
  ASSERT_FALSE(bigram_ranked.empty());
  EXPECT_EQ(bigram_ranked.front().surface, "日本");

  converter.Commit({"NIPPON", "にほん", 1.0, azookey::core::CandidateSource::UserDictionary, "manual"},
                   azookey::core::ConversionContext{});
  const auto committed = converter.Convert("にほん", azookey::core::ConversionContext{});
  ASSERT_FALSE(committed.empty());
  EXPECT_EQ(committed.front().surface, "NIPPON");
}
