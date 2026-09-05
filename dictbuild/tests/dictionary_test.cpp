#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <sstream>

#include "azookey/core/DoubleArrayTrie.h"
#include "azookey/core/SimpleConverter.h"
#include "azookey/host/DictionaryCandidateProvider.h"
#include "azookey/host/InferenceEngine.h"
#include "azookey/learning/DictionaryStore.h"

namespace {
using namespace azookey;
std::filesystem::path Fixture(const char* name) {
  return std::filesystem::path(AZOOKEY_DICT_FIXTURE) / name;
}

TEST(DictionaryTrie, SearchDirectionsAndShortestFirstLimit) {
  core::DoubleArrayTrie trie;
  ASSERT_TRUE(trie.Load(Fixture("valid.azdic"), true)) << trie.Error();
  std::vector<core::PrefixMatch> matches;
  trie.CommonPrefixSearch("とうきょうと", 0, matches);
  ASSERT_EQ(matches.size(), 3U);
  EXPECT_EQ(matches[0].key_length, std::string("とう").size());
  EXPECT_EQ(matches[2].key_length, std::string("とうきょうと").size());
  trie.PredictiveSearch("a", 2, matches);
  ASSERT_EQ(matches.size(), 2U);
  EXPECT_EQ(matches[0].key_length, 2U);
  EXPECT_EQ(matches[1].key_length, 2U);
  EXPECT_LT(matches[0].key_id, matches[1].key_id);
  core::PrefixMatch exact;
  ASSERT_TRUE(trie.ExactMatch("とうきょう", exact));
  std::vector<core::StaticDictionaryEntry> entries;
  ASSERT_TRUE(trie.ReadEntries(exact, entries));
  ASSERT_EQ(entries.size(), 1U);
  EXPECT_EQ(entries[0].surface, "東京");
}

TEST(DictionaryTrie, RejectsCorruptionIncludingValidHashBadReferences) {
  for (const char* name :
       {"magic", "version", "flags", "duplicate", "unaligned", "overflow", "hash", "entry", "kind",
        "string", "pos", "source", "truncated", "key_order_same_depth", "key_order_other_depth"}) {
    SCOPED_TRACE(name);
    core::DoubleArrayTrie trie;
    const auto path = Fixture((std::string(name) + ".azdic").c_str());
    EXPECT_FALSE(trie.Load(path, true));
    if (std::string_view(name).starts_with("key_order"))
      EXPECT_EQ(trie.Error(), "invalid key order");
    EXPECT_FALSE(trie.IsAvailable());
    core::PrefixMatch match;
    EXPECT_FALSE(trie.ExactMatch("とう", match));
  }
}

TEST(DictionaryTrie, LazyReferenceFailureDisablesOnlyItsLayer) {
  learning::DictionaryStore store;
  ASSERT_TRUE(store.LoadStatic(learning::LayerId::TechnicalTerms, Fixture("entry.azdic")));
  learning::DictionaryEntry local;
  local.surface = "local";
  local.reading = "aa";
  local.frequency = .4;
  store.ReplaceMutable(learning::LayerId::User, {local});
  const auto result = store.Lookup("aa", {});
  ASSERT_EQ(result.size(), 1U);
  EXPECT_EQ(result[0].surface, "local");
  EXPECT_FALSE(store.IsAvailable(learning::LayerId::TechnicalTerms));
}

TEST(DictionaryTrie, InvalidAndEmptyInputsDoNotThrow) {
  core::DoubleArrayTrie trie;
  ASSERT_TRUE(trie.Load(Fixture("valid.azdic")));
  for (const std::string input : {std::string{}, std::string("\xc0\xaf"),
                                  std::string("\xed\xa0\x80"), std::string("\xf4\x90\x80\x80")}) {
    std::vector<core::PrefixMatch> matches;
    trie.PredictiveSearch(input, 0, matches);
    EXPECT_TRUE(matches.empty());
    trie.CommonPrefixSearch(input, 0, matches);
    EXPECT_TRUE(matches.empty());
  }
  EXPECT_TRUE(trie.IsAvailable());
  EXPECT_EQ(core::NormalizeReading("カタカナＡ１"), "かたかなA1");
  EXPECT_EQ(core::ReadingAliases("づづづづ"), std::vector<std::string>{"づづづづ"});
}

TEST(DictionaryStore, StaticAndMutableMatchIndependentReference) {
  learning::DictionaryStore fixed, local;
  ASSERT_TRUE(fixed.LoadStatic(learning::LayerId::TechnicalTerms, Fixture("valid.azdic"), true));
  std::vector<learning::DictionaryEntry> entries;
  for (int i = 0; i < 200; ++i) {
    std::ostringstream key;
    key << "き" << std::setfill('0') << std::setw(4) << i;
    learning::DictionaryEntry e;
    e.surface = "word" + std::to_string(i);
    e.reading = key.str();
    e.frequency = .3;
    entries.push_back(e);
  }
  local.ReplaceMutable(learning::LayerId::User, entries);
  for (const auto mode : {learning::LookupMode::Exact, learning::LookupMode::CommonPrefix,
                          learning::LookupMode::PredictivePrefix}) {
    for (const auto& key : {"き", "き0", "き00", "き0001", "き0001あ", "き9"}) {
      learning::LookupContext ctx;
      ctx.mode = mode;
      ctx.max_results = 0;
      std::vector<std::string> expected, actual, mutable_result;
      for (const auto& e : entries) {
        const bool match = mode == learning::LookupMode::Exact ? e.reading == key
                           : mode == learning::LookupMode::CommonPrefix
                               ? std::string(key).starts_with(e.reading)
                               : e.reading.starts_with(key);
        if (match) expected.push_back(e.surface);
      }
      for (auto& e : fixed.Lookup(key, ctx)) actual.push_back(e.surface);
      for (auto& e : local.Lookup(key, ctx)) mutable_result.push_back(e.surface);
      std::sort(expected.begin(), expected.end());
      std::sort(actual.begin(), actual.end());
      std::sort(mutable_result.begin(), mutable_result.end());
      EXPECT_EQ(expected, actual);
      EXPECT_EQ(expected, mutable_result);
    }
  }
}

TEST(DictionaryStore, AliasKindScoringProvenanceAndDisable) {
  learning::DictionaryStore store;
  ASSERT_TRUE(store.LoadStatic(learning::LayerId::TechnicalTerms, Fixture("valid.azdic"), true));
  auto result = store.Lookup("ば", {});
  ASSERT_EQ(result.size(), 2U);
  EXPECT_EQ(result[0].surface, "場");
  EXPECT_EQ(result[1].kind, core::MatchKind::Alias);
  result = store.Lookup("てんそるあーるてぃー", {});
  ASSERT_EQ(result.size(), 1U);
  EXPECT_NEAR(result[0].score, 1.32, 1.0 / 65535);
  learning::DictionaryEntry entry;
  entry.surface = "TensorRT";
  entry.reading = "てんそるあーるてぃー";
  entry.frequency = .72;
  entry.category_mask = 2;
  store.ReplaceMutable(learning::LayerId::User, {entry});
  result = store.Lookup(entry.reading, {});
  ASSERT_EQ(result.size(), 1U);
  EXPECT_EQ(result[0].source, learning::LayerId::User);
  EXPECT_EQ(result[0].sources, (1U << 4) | (1U << 5));
  EXPECT_EQ(result[0].category_mask, (1U << 8) | 2U);
  learning::LookupContext static_only;
  static_only.excluded_layers = 1U << static_cast<unsigned>(learning::LayerId::User);
  EXPECT_EQ(store.Lookup(entry.reading, static_only)[0].source, learning::LayerId::TechnicalTerms);
  store.EnableLayer(learning::LayerId::User, false);
  EXPECT_EQ(store.Lookup(entry.reading, {})[0].source, learning::LayerId::TechnicalTerms);
  EXPECT_EQ(store.Lookup("かーと", {})[0].kind, core::MatchKind::LongVowelRelaxed);
}

TEST(DictionaryHost, SuppliesConversionAndPredictionAndTracksUserMutations) {
  host::InferenceEngine engine(std::make_unique<core::SimpleConverter>(), nullptr, {});
  ASSERT_TRUE(
      engine.LoadDictionaryLayer(learning::LayerId::TechnicalTerms, Fixture("valid.azdic"), true));
  const auto converted = engine.QueryCandidates("とうきょう", "", 0);
  EXPECT_TRUE(std::any_of(converted.begin(), converted.end(),
                          [](const auto& e) { return e.surface == "東京"; }));
  const auto predicted = engine.QueryPredictions("とう", "", 0);
  EXPECT_TRUE(std::any_of(predicted.begin(), predicted.end(),
                          [](const auto& e) { return e.surface == "東京都"; }));
  learning::UserDictionary user(Fixture("unused-user.json"));
  user.Add({"unique", "ゆにーく"});
  engine.SetUserDictionary(&user);
  auto result = engine.QueryPredictions("ゆに", "", 0);
  EXPECT_TRUE(std::any_of(result.begin(), result.end(),
                          [](const auto& e) { return e.surface == "unique"; }));
  user.Remove("unique", "ゆにーく");
  result = engine.QueryPredictions("ゆに", "", 0);
  EXPECT_FALSE(std::any_of(result.begin(), result.end(),
                           [](const auto& e) { return e.surface == "unique"; }));
}
TEST(DictionaryStore, AvailabilityAndLoadDiagnostics) {
  learning::DictionaryStore store;
  EXPECT_FALSE(store.IsAvailable(learning::LayerId::User));
  store.SetUserWords({});
  EXPECT_TRUE(store.IsAvailable(learning::LayerId::User));
  store.EnableLayer(learning::LayerId::User, false);
  EXPECT_TRUE(store.IsAvailable(learning::LayerId::User));
  EXPECT_FALSE(store.LoadStatic(learning::LayerId::Base, Fixture("valid.azdic")));
  EXPECT_FALSE(store.IsAvailable(learning::LayerId::Base));
  EXPECT_EQ(store.LayerError(learning::LayerId::Base), "layer id mismatch");
  EXPECT_FALSE(store.LoadStatic(learning::LayerId::Base, Fixture("magic.azdic")));
  EXPECT_NE(store.LayerError(learning::LayerId::Base), "layer id mismatch");
  EXPECT_FALSE(store.LayerError(learning::LayerId::Base).empty());
}

TEST(DictionaryHost, CapsEntriesPreservesScoresAndReadingContract) {
  learning::DictionaryStore store;
  std::vector<learning::UserWord> words;
  for (int i = 0; i < 80; ++i) {
    learning::UserWord word{"user" + std::to_string(i), "ヨミ"};
    word.value = i - 40.0;
    words.push_back(word);
  }
  store.SetUserWords(words);
  const auto predicted =
      host::DictionaryCandidates(store, "よ", learning::LookupMode::PredictivePrefix, 0, 32);
  ASSERT_EQ(predicted.size(), 32U);
  EXPECT_EQ(predicted.front().score, 39.0);
  EXPECT_EQ(predicted.back().score, 8.0);
  EXPECT_EQ(predicted.front().reading, "ヨミ");
  EXPECT_TRUE(
      host::DictionaryCandidates(store, "よみ", learning::LookupMode::PredictivePrefix, 0).empty());
  EXPECT_TRUE(host::DictionaryCandidates(store, "よ", learning::LookupMode::PredictivePrefix, 0, 0)
                  .empty());
  learning::UserWord negative{"negative", "ネガ"};
  negative.value = -5.0;
  store.SetUserWords({negative, {"default", "デフォルト"}});
  const auto negative_result =
      host::DictionaryCandidates(store, "ね", learning::LookupMode::PredictivePrefix, 0);
  ASSERT_EQ(negative_result.size(), 1U);
  EXPECT_EQ(negative_result[0].score, -5.0);
  const auto default_result = host::DictionaryCandidates(
      store, "で", learning::LookupMode::PredictivePrefix, 0, 32, true, 2.5);
  ASSERT_EQ(default_result.size(), 1U);
  EXPECT_EQ(default_result[0].score, 2.5);
  ASSERT_TRUE(store.LoadStatic(learning::LayerId::TechnicalTerms, Fixture("valid.azdic")));
  const auto relaxed = host::DictionaryCandidates(store, "かーと", learning::LookupMode::Exact, 0);
  ASSERT_EQ(relaxed.size(), 1U);
  EXPECT_EQ(relaxed[0].reading, "かーと");
  const auto extensions =
      host::DictionaryCandidates(store, "とう", learning::LookupMode::PredictivePrefix, 0, 1);
  ASSERT_EQ(extensions.size(), 1U);
  EXPECT_NE(extensions[0].surface, "都");
}

class PredictingConverter : public core::IConverter {
 public:
  std::vector<core::Candidate> Convert(const std::string&,
                                       const core::ConversionContext&) override {
    return {};
  }
  std::vector<core::Candidate> PredictNext(const std::string&,
                                           const core::ConversionContext&) override {
    return {{"model1", "よみ1", 1.0, core::CandidateSource::Model},
            {"model2", "よみ2", .9, core::CandidateSource::Model},
            {"model3", "よみ3", .8, core::CandidateSource::Model}};
  }
  std::vector<core::Candidate> Correct(const std::string&, const core::CorrectionHint&,
                                       const core::ConversionContext&) override {
    return {};
  }
  void Commit(const core::Candidate&, const core::ConversionContext&) override {}
  void Learn(const std::string&, const std::string&) override {}
};

TEST(DictionaryHost, ReservesConverterPredictionsWithLiveReranker) {
  learning::LearningStore store(Fixture("unused-learning.tsv"));
  host::InferenceEngine engine(std::make_unique<PredictingConverter>(), &store, {});
  ASSERT_TRUE(
      engine.LoadDictionaryLayer(learning::LayerId::TechnicalTerms, Fixture("valid.azdic")));
  auto result = engine.QueryPredictions("よ", "context", 0);
  ASSERT_EQ(result.size(), 5U);
  EXPECT_EQ(std::count_if(result.begin(), result.end(),
                          [](const auto& e) { return e.source == core::CandidateSource::Model; }),
            3);
  EXPECT_EQ(std::count_if(result.begin(), result.end(),
                          [](const auto& e) { return e.debug_info == "dictionary"; }),
            2);
  for (int i = 0; i < 3; ++i)
    store.Observe("よが" + std::to_string(i), "learned" + std::to_string(i), 5.0, 0);
  result = engine.QueryPredictions("よ", "context", 0);
  ASSERT_EQ(result.size(), 5U);
  EXPECT_EQ(std::count_if(result.begin(), result.end(),
                          [](const auto& e) { return e.source == core::CandidateSource::Model; }),
            2);
}

TEST(DictionaryHost, BundledDiscoveryReportsErrorsAndContinues) {
  host::InferenceEngine engine(std::make_unique<core::SimpleConverter>(), nullptr, {});
  const auto results = engine.LoadBundledDictionaryLayers(Fixture("bundled"));
  ASSERT_EQ(results.size(), 4U);
  EXPECT_FALSE(results[0].loaded);
  EXPECT_EQ(results[0].error, "layer id mismatch");
  EXPECT_FALSE(results[1].loaded);
  EXPECT_FALSE(results[1].error.empty());
  EXPECT_TRUE(results[3].loaded);
  EXPECT_TRUE(results[3].error.empty());
}
}  // namespace
