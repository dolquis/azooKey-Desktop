#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "../../learning/tests/TestByteCrypto.h"
#include "azookey/core/DoubleArrayTrie.h"
#include "azookey/core/SimpleConverter.h"
#include "azookey/host/DictionaryCandidateProvider.h"
#include "azookey/host/InferenceEngine.h"
#include "azookey/host/SettingsStore.h"
#include "azookey/learning/AutoWordStore.h"
#include "azookey/learning/DictionaryStore.h"
#include "azookey/learning/UserDictionaryPos.h"

namespace {
using namespace azookey;
std::filesystem::path Fixture(const char* name) {
  return std::filesystem::path(AZOOKEY_DICT_FIXTURE) / name;
}

class ScopedMiningDirectory {
 public:
  ScopedMiningDirectory() {
    for (int attempt = 0; attempt < 16; ++attempt) {
      path_ = std::filesystem::temp_directory_path() /
              ("azookey_dictionary_mining_" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
               std::to_string(next_id_++));
      if (std::filesystem::create_directory(path_)) return;
    }
    throw std::runtime_error("Could not create dictionary mining test directory");
  }
  ~ScopedMiningDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  std::filesystem::path File(const char* name) const { return path_ / name; }

 private:
  std::filesystem::path path_;
  static inline std::atomic<uint64_t> next_id_{0};
};

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

TEST(DictionaryTrie, ExactSurfaceLookupUsesStaticRecords) {
  core::DoubleArrayTrie trie;
  ASSERT_TRUE(trie.Load(Fixture("valid.azdic"), true));
  std::vector<core::StaticDictionaryEntry> entries;
  trie.LookupSurface("東京", entries);
  ASSERT_EQ(entries.size(), 1U);
  EXPECT_EQ(entries[0].reading, "とうきょう");
  trie.LookupSurface("存在しない表層形", entries);
  EXPECT_TRUE(entries.empty());
  trie.LookupSurface(std::string("\xc0\xaf"), entries);
  EXPECT_TRUE(entries.empty());
}

TEST(DictionaryTrie, SurfaceIndexMatchesScanAndOlderArtifactsStillLoad) {
  core::DoubleArrayTrie indexed, scanned, extended;
  ASSERT_TRUE(indexed.Load(Fixture("valid.azdic"), true)) << indexed.Error();
  ASSERT_TRUE(scanned.Load(Fixture("no_surface_index.azdic"), true)) << scanned.Error();
  ASSERT_TRUE(extended.Load(Fixture("unknown_section.azdic"), true)) << extended.Error();
  std::vector<std::string> surfaces = {"都",       "東京", "東京都",   "日本", "場",   "ヴァ",
                                       "TensorRT", "東",   "東京都庁", "",     "word", "zzz"};
  for (int i = 0; i < 200; ++i) surfaces.push_back("word" + std::to_string(i));
  for (int i = 0; i < 8; ++i) surfaces.push_back("high" + std::to_string(i));
  const auto readings = [](const core::DoubleArrayTrie& trie, const std::string& surface) {
    std::vector<core::StaticDictionaryEntry> entries;
    trie.LookupSurface(surface, entries);
    std::vector<std::string> result;
    for (const auto& entry : entries) result.push_back(entry.surface + "/" + entry.reading);
    return result;
  };
  for (const auto& surface : surfaces) {
    SCOPED_TRACE(surface);
    const auto expected = readings(scanned, surface);
    EXPECT_EQ(readings(indexed, surface), expected);
    EXPECT_EQ(readings(extended, surface), expected);
  }
  EXPECT_EQ(readings(indexed, "東京"), std::vector<std::string>{"東京/とうきょう"});
  // ENTS is ordered by reading, so a shared surface comes back in that order.
  EXPECT_EQ(readings(indexed, "日本"), (std::vector<std::string>{"日本/にっぽん", "日本/にほん"}));
  EXPECT_TRUE(indexed.IsAvailable());
}

// CONN and CMAT (azdic spec section 15.5) are sections this reader does not know. A layer that
// carries CONN answers every lookup as the same words without it, and lattice_core still loads.
TEST(DictionaryTrie, ConnectionSectionsLeaveLookupsUnchanged) {
  core::DoubleArrayTrie plain, linked, lattice;
  ASSERT_TRUE(plain.Load(Fixture("valid.azdic"), true)) << plain.Error();
  ASSERT_TRUE(linked.Load(Fixture("connections.azdic"), true)) << linked.Error();
  ASSERT_TRUE(lattice.Load(Fixture("lattice_core.azdic"), true)) << lattice.Error();
  const auto describe = [](const core::DoubleArrayTrie& trie,
                           const std::vector<core::PrefixMatch>& matches) {
    std::vector<std::string> result;
    for (const auto& match : matches) {
      std::vector<core::StaticDictionaryEntry> entries;
      EXPECT_TRUE(trie.ReadEntries(match, entries));
      for (const auto& entry : entries)
        result.push_back(std::to_string(match.key_length) + ":" + entry.surface + "/" +
                         entry.reading + "/" + std::to_string(entry.cost) + "/" +
                         std::to_string(entry.pos_id));
    }
    return result;
  };
  for (const std::string key :
       {"とうきょうと", "とう", "にほん", "にっぽん", "a", "aaaaa", "き", "よ", "ば", "ゔぁ"}) {
    SCOPED_TRACE(key);
    std::vector<core::PrefixMatch> expected, actual;
    plain.CommonPrefixSearch(key, 0, expected);
    linked.CommonPrefixSearch(key, 0, actual);
    EXPECT_EQ(describe(linked, actual), describe(plain, expected));
    plain.PredictiveSearch(key, 0, expected);
    linked.PredictiveSearch(key, 0, actual);
    EXPECT_EQ(describe(linked, actual), describe(plain, expected));
    core::PrefixMatch exact;
    EXPECT_EQ(linked.ExactMatch(key, exact), plain.ExactMatch(key, exact));
  }
  std::vector<core::StaticDictionaryEntry> expected, actual;
  plain.LookupSurface("日本", expected);
  linked.LookupSurface("日本", actual);
  ASSERT_EQ(actual.size(), expected.size());
  for (size_t i = 0; i < actual.size(); ++i) EXPECT_EQ(actual[i].reading, expected[i].reading);
  EXPECT_TRUE(linked.IsAvailable());
  // lattice_core holds the kana words under the sudachi_lexicon layer id (azdic spec 15.12).
  EXPECT_EQ(lattice.LayerId(), 1U);
  core::PrefixMatch particle;
  ASSERT_TRUE(lattice.ExactMatch("は", particle));
  std::vector<core::StaticDictionaryEntry> entries;
  ASSERT_TRUE(lattice.ReadEntries(particle, entries));
  ASSERT_EQ(entries.size(), 1U);
  EXPECT_EQ(entries[0].surface, "は");
}

TEST(DictionaryTrie, CorruptSurfaceIndexDisablesOnlyItsLayerOnLookup) {
  for (const auto& [name, error] :
       {std::pair{"surface_index_entry", "invalid surface index"},
        std::pair{"surface_index_duplicate", "invalid surface index order"},
        std::pair{"surface_index_mismatch", "surface index mismatch"}}) {
    SCOPED_TRACE(name);
    learning::DictionaryStore store;
    ASSERT_TRUE(store.LoadStatic(learning::LayerId::TechnicalTerms,
                                 Fixture((std::string(name) + ".azdic").c_str())));
    learning::DictionaryEntry local;
    local.surface = "東京";
    local.reading = "ひがしきょう";
    local.frequency = .1;
    store.ReplaceMutable(learning::LayerId::User, {local});
    const auto found = store.ReverseLookup("東京", {});
    ASSERT_TRUE(found);
    EXPECT_EQ(found->source, learning::LayerId::User);
    EXPECT_FALSE(store.IsAvailable(learning::LayerId::TechnicalTerms));
    EXPECT_EQ(store.LayerError(learning::LayerId::TechnicalTerms), error);
  }
}

TEST(DictionaryStore, ReverseLookupRespectsLayerSelectionAndUserWords) {
  learning::DictionaryStore store;
  ASSERT_TRUE(store.LoadStatic(learning::LayerId::TechnicalTerms, Fixture("valid.azdic"), true));
  ASSERT_TRUE(store.ReverseLookup("東京", {}));
  EXPECT_EQ(store.ReverseLookup("東京", {})->normalized_reading, "とうきょう");
  learning::UserWord user_word{"東京", "とうきょうユーザー"};
  user_word.value = 5.0;
  store.SetUserWords({user_word});
  ASSERT_TRUE(store.ReverseLookup("東京", {}));
  EXPECT_EQ(store.ReverseLookup("東京", {})->source, learning::LayerId::User);
  store.EnableLayer(learning::LayerId::User, false);
  EXPECT_EQ(store.ReverseLookup("東京", {})->source, learning::LayerId::TechnicalTerms);
  store.EnableLayer(learning::LayerId::TechnicalTerms, false);
  EXPECT_FALSE(store.ReverseLookup("東京", {}));
}

TEST(DictionaryTrie, RejectsCorruptionIncludingValidHashBadReferences) {
  for (const char* name : {"magic",
                           "version",
                           "flags",
                           "duplicate",
                           "unaligned",
                           "overflow",
                           "hash",
                           "entry",
                           "kind",
                           "string",
                           "pos",
                           "source",
                           "truncated",
                           "key_order_same_depth",
                           "key_order_other_depth",
                           "surface_index_size",
                           "surface_index_entry",
                           "surface_index_order",
                           "surface_index_duplicate",
                           "surface_index_mismatch"}) {
    SCOPED_TRACE(name);
    core::DoubleArrayTrie trie;
    const auto path = Fixture((std::string(name) + ".azdic").c_str());
    EXPECT_FALSE(trie.Load(path, true));
    if (std::string_view(name).starts_with("key_order"))
      EXPECT_EQ(trie.Error(), "invalid key order");
    if (std::string_view(name) == "surface_index_size")
      EXPECT_EQ(trie.Error(), "invalid section size");
    if (std::string_view(name) == "surface_index_entry")
      EXPECT_EQ(trie.Error(), "invalid surface index");
    if (std::string_view(name).starts_with("surface_index_") &&
        !std::string_view(name).ends_with("size") && !std::string_view(name).ends_with("entry"))
      EXPECT_EQ(trie.Error(), "invalid surface index order");
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

TEST(DictionaryStore, KnownWordMembershipIgnoresStaticSwitchesAndPreservesReadingSemantics) {
  learning::DictionaryStore store;
  ASSERT_TRUE(store.LoadStatic(learning::LayerId::TechnicalTerms, Fixture("valid.azdic"), true));
  store.EnableLayer(learning::LayerId::TechnicalTerms, false);
  EXPECT_TRUE(store.ContainsKnownWord("トウキョウ", "東京"));
  EXPECT_TRUE(store.ContainsKnownWord("ば", "ヴァ"));
  EXPECT_TRUE(store.ContainsKnownWord("かーと", "緩"));
  EXPECT_FALSE(store.ContainsKnownWord("とう", "東京"));
  EXPECT_FALSE(store.ContainsKnownWord("とうきょう", "東京都"));
  EXPECT_FALSE(store.ContainsKnownWord("とうきょう", "試験専用語"));
  EXPECT_FALSE(store.ContainsKnownWord("", "東京"));
  EXPECT_TRUE(store.Lookup("とうきょう", {}).empty());
  EXPECT_FALSE(store.ReverseLookup("東京", {}));

  std::vector<learning::DictionaryEntry> entries;
  for (int i = 0; i < 64; ++i) {
    learning::DictionaryEntry entry;
    entry.surface = "試験語" + std::to_string(i);
    entry.reading = "しけんご";
    entry.frequency = i == 63 ? 0 : 1;
    entries.push_back(entry);
  }
  store.ReplaceMutable(learning::LayerId::User, entries);
  EXPECT_TRUE(store.ContainsKnownWord("シケンゴ", "試験語63"));
  store.EnableLayer(learning::LayerId::User, false);
  EXPECT_FALSE(store.ContainsKnownWord("しけんご", "試験語63"));
}

TEST(DictionaryStore, KnownWordMembershipRequiresAvailableValidatedStaticData) {
  learning::DictionaryStore store;
  store.EnableLayer(learning::LayerId::TechnicalTerms, false);
  EXPECT_FALSE(store.ContainsKnownWord("とうきょう", "東京"));
  ASSERT_TRUE(store.LoadStatic(learning::LayerId::TechnicalTerms, Fixture("valid.azdic"), true));
  EXPECT_TRUE(store.ContainsKnownWord("とうきょう", "東京"));
  // A failed replacement must not retain membership from the previous artifact.
  EXPECT_FALSE(store.LoadStatic(learning::LayerId::TechnicalTerms, Fixture("missing.azdic")));
  EXPECT_FALSE(store.ContainsKnownWord("とうきょう", "東京"));
  EXPECT_FALSE(store.LoadStatic(learning::LayerId::TechnicalTerms, Fixture("hash.azdic"), true));
  EXPECT_FALSE(store.ContainsKnownWord("とうきょう", "東京"));
  EXPECT_FALSE(store.LoadStatic(learning::LayerId::Sudachi, Fixture("valid.azdic"), true));
  EXPECT_FALSE(store.ContainsKnownWord("とうきょう", "東京"));
  ASSERT_TRUE(store.LoadStatic(learning::LayerId::TechnicalTerms, Fixture("entry.azdic")));
  EXPECT_FALSE(store.ContainsKnownWord("aa", "短"));
  EXPECT_FALSE(store.IsAvailable(learning::LayerId::TechnicalTerms));
  ASSERT_TRUE(store.LoadStatic(learning::LayerId::TechnicalTerms, Fixture("valid.azdic"), true));
  EXPECT_TRUE(store.ContainsKnownWord("とうきょう", "東京"));
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

class TechnicalDuplicateConverter : public PredictingConverter {
 public:
  explicit TechnicalDuplicateConverter(core::CandidateTag tag) : tag_(tag) {}
  std::vector<core::Candidate> Convert(const std::string& kana,
                                       const core::ConversionContext&) override {
    core::Candidate candidate{"TensorRT", kana, 10.0, core::CandidateSource::Model};
    candidate.tag = tag_;
    return {candidate};
  }
  std::vector<core::Candidate> PredictNext(const std::string&,
                                           const core::ConversionContext&) override {
    // high7 is behind the first two dictionary additions and must still get
    // its category when the five display slots have already filled.
    core::Candidate candidate{"high7", "よ7", 10.0, core::CandidateSource::Model};
    candidate.tag = tag_;
    return {candidate, {"model", "よもでる", 9.0, core::CandidateSource::Model}};
  }

 private:
  core::CandidateTag tag_;
};

TEST(DictionaryHost, TechnicalCategorySurvivesHigherScoreConverterDuplicates) {
  for (const auto tag : {core::CandidateTag::None, core::CandidateTag::English}) {
    SCOPED_TRACE(static_cast<int>(tag));
    host::InferenceEngine engine(std::make_unique<TechnicalDuplicateConverter>(tag), nullptr, {});
    ASSERT_TRUE(engine.LoadDictionaryLayer(learning::LayerId::TechnicalTerms,
                                           Fixture("valid.azdic"), true));
    const auto candidates =
        engine.QueryCandidates("てんそるあーるてぃー", "", 0, nullptr, 10, false);
    ASSERT_EQ(candidates.size(), 1U);
    EXPECT_EQ(candidates[0].tag, core::CandidateTag::Technical);
    EXPECT_EQ(candidates[0].source, core::CandidateSource::Model);
    EXPECT_DOUBLE_EQ(candidates[0].score, 10.0);
  }
}

TEST(DictionaryHost, TechnicalCategorySurvivesPredictionDisplayAndDictionaryLimits) {
  for (const auto tag : {core::CandidateTag::None, core::CandidateTag::English}) {
    SCOPED_TRACE(static_cast<int>(tag));
    learning::test::TestByteCrypto crypto;
    ScopedMiningDirectory temp;
    learning::LearningStore learning_store(temp.File("learning.tsv"), &crypto);
    learning_store.Observe("よがくしゅう", "学習予測", 5.0, 1);
    host::InferenceEngine engine(std::make_unique<TechnicalDuplicateConverter>(tag),
                                 &learning_store, {});
    ASSERT_TRUE(engine.LoadDictionaryLayer(learning::LayerId::TechnicalTerms,
                                           Fixture("valid.azdic"), true));
    const auto predictions = engine.QueryPredictions("よ", "", 1);
    ASSERT_EQ(predictions.size(), 5U);
    EXPECT_EQ(predictions[0].surface, "学習予測");
    ASSERT_EQ(predictions[1].surface, "high7");
    EXPECT_EQ(predictions[1].tag, core::CandidateTag::Technical);
    EXPECT_EQ(predictions[1].source, core::CandidateSource::Model);
    EXPECT_DOUBLE_EQ(predictions[1].score, 10.0);
    EXPECT_EQ(predictions[2].surface, "model");
    EXPECT_EQ(predictions[3].tag, core::CandidateTag::Technical);
    EXPECT_EQ(predictions[4].tag, core::CandidateTag::Technical);
  }
}

TEST(DictionaryHost, DisabledStaticKnownWordsStayOutOfMiningAcrossSettingsReload) {
  for (const auto& [layer, name] :
       {std::pair{learning::LayerId::Sudachi, "sudachi_lexicon.azdic"},
        std::pair{learning::LayerId::NamedEntity, "named_entity_lexicon.azdic"},
        std::pair{learning::LayerId::TechnicalTerms, "technical_terms_lexicon.azdic"}}) {
    SCOPED_TRACE(name);
    for (const bool auto_register : {false, true}) {
      SCOPED_TRACE(auto_register);
      ScopedMiningDirectory temp;
      learning::test::TestByteCrypto crypto;
      learning::AutoWordStore auto_words(temp.File("auto_words.tsv"), &crypto);
      const auto write_settings = [&](bool enabled) {
        std::ofstream out(temp.File("settings.json"));
        out << R"({"dictionary":{"sudachiEnabled":)" << (enabled ? "true" : "false")
            << R"(,"namedEntityEnabled":)" << (enabled ? "true" : "false")
            << R"(,"technicalTermsEnabled":)" << (enabled ? "true" : "false")
            << R"(},"autoWordRegistration":{"miningEnabled":true,"miningMinCount":2,)"
            << R"("registrationMode":")" << (auto_register ? "auto" : "confirm") << R"("}})";
      };
      write_settings(true);
      host::SettingsStore settings(temp.File("settings.json"));
      ASSERT_EQ(settings.Load().status, host::SettingsLoadStatus::Loaded);
      const auto config = host::ApplyRuntimeSettingsToEngineConfig({}, settings.settings());
      // This converter has no lexicon or learned commits, so only the loaded layer
      // can supply the known word in any lookup direction.
      host::InferenceEngine engine(std::make_unique<PredictingConverter>(), nullptr, config);
      engine.SetAutoWordStore(&auto_words);
      ASSERT_TRUE(engine.LoadDictionaryLayer(layer, Fixture("settings-layers") / name, true));
      const auto contains_known = [](const auto& candidates) {
        return std::any_of(candidates.begin(), candidates.end(),
                           [](const auto& entry) { return entry.surface == "東京"; });
      };
      EXPECT_TRUE(contains_known(engine.QueryCandidates("とうきょう", "", 100)));
      EXPECT_TRUE(contains_known(engine.QueryPredictions("とう", "", 100)));
      EXPECT_EQ(engine.ReverseConvert("東京", 100), "とうきょう");
      ASSERT_TRUE(engine.CommitObservation("とうきょう", "東京", 100));
      EXPECT_EQ(auto_words.Size(), 0u);

      write_settings(false);
      ASSERT_EQ(settings.Reload().status, host::SettingsLoadStatus::Loaded);
      engine.ApplyConfig(
          host::ApplyRuntimeSettingsToEngineConfig(engine.config(), settings.settings()));
      EXPECT_TRUE(engine.config().auto_word_mining_enabled);
      EXPECT_EQ(engine.config().auto_word_auto_register, auto_register);
      for (uint64_t now = 101; now <= 103; ++now)
        ASSERT_TRUE(engine.CommitObservation("トウキョウ", "東京", now));
      EXPECT_EQ(auto_words.Size(), 0u);
      EXPECT_FALSE(contains_known(engine.QueryCandidates("とうきょう", "", 103)));
      EXPECT_FALSE(contains_known(engine.QueryPredictions("とう", "", 103)));
      EXPECT_TRUE(engine.ReverseConvert("東京", 103).empty());

      engine.EnableDictionaryLayer(layer, true);
      EXPECT_TRUE(contains_known(engine.QueryCandidates("とうきょう", "", 104)));
      EXPECT_TRUE(contains_known(engine.QueryPredictions("とう", "", 104)));
      EXPECT_EQ(engine.ReverseConvert("東京", 104), "とうきょう");
      ASSERT_TRUE(engine.CommitObservation("とうきょう", "東京", 104));
      EXPECT_EQ(auto_words.Size(), 0u);
      engine.EnableDictionaryLayer(layer, false);
      // Loading again while OFF must keep the distinction between membership
      // and candidate participation, including a different surface at the same reading.
      ASSERT_TRUE(engine.LoadDictionaryLayer(layer, Fixture("settings-layers") / name, true));
      for (uint64_t now = 105; now <= 107; ++now) {
        ASSERT_TRUE(engine.CommitObservation("とうきょう", "東京", now));
        ASSERT_TRUE(engine.CommitObservation("とうきょう", "試験専用語", now));
      }
      EXPECT_EQ(auto_words.Size(), 1u);
      const auto mined = auto_words.ListByState(auto_register ? learning::AutoWordState::Confirmed
                                                              : learning::AutoWordState::Pending);
      ASSERT_EQ(mined.size(), 1u);
      EXPECT_EQ(mined[0].surface, "試験専用語");
      EXPECT_EQ(mined[0].reading, "とうきょう");
      EXPECT_EQ(mined[0].count, 3u);
      learning::AutoWordStore reloaded(temp.File("auto_words.tsv"), &crypto);
      ASSERT_TRUE(reloaded.Load());
      EXPECT_EQ(reloaded.Size(), 1u);
    }
  }
}

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
  ASSERT_EQ(results.size(), 5U);
  EXPECT_FALSE(results[0].loaded);
  EXPECT_EQ(results[0].error, "layer id mismatch");
  EXPECT_FALSE(results[1].loaded);
  EXPECT_FALSE(results[1].error.empty());
  EXPECT_TRUE(results[3].loaded);
  EXPECT_TRUE(results[3].error.empty());
  // DEV-2101: lattice_core comes last; without it only the lattice is off.
  EXPECT_EQ(results[4].name, "lattice_core.azdic");
  EXPECT_FALSE(results[4].loaded);
  EXPECT_FALSE(results[4].error.empty());
}

TEST(DictionaryHost, SettingsApplyStaticLayerSwitchesToAllLookupDirections) {
  host::EngineConfig config;
  config.dictionary.sudachi_enabled = false;
  config.dictionary.named_entity_enabled = false;
  config.dictionary.technical_terms_enabled = false;
  host::InferenceEngine engine(std::make_unique<core::SimpleConverter>(), nullptr, config);
  const auto loaded = engine.LoadBundledDictionaryLayers(Fixture("settings-layers"));
  ASSERT_EQ(loaded.size(), 5u);  // The static layers, then lattice_core (absent here).
  for (size_t i = 1; i < 4; ++i) ASSERT_TRUE(loaded[i].loaded) << loaded[i].error;
  const auto contains_fixture_word = [](const auto& candidates) {
    return std::any_of(candidates.begin(), candidates.end(),
                       [](const auto& candidate) { return candidate.surface == "high0"; });
  };
  EXPECT_FALSE(contains_fixture_word(engine.QueryCandidates("よ0", "", 0)));
  EXPECT_FALSE(contains_fixture_word(engine.QueryPredictions("よ", "", 0)));
  EXPECT_TRUE(engine.ReverseConvert("high0", 0).empty());
  // Even with every optional static layer disabled, the kana fallback remains.
  EXPECT_FALSE(engine.QueryCandidates("かな", "", 0).empty());

  for (const auto member : {&host::DictionaryLayerConfig::sudachi_enabled,
                            &host::DictionaryLayerConfig::named_entity_enabled,
                            &host::DictionaryLayerConfig::technical_terms_enabled}) {
    config.dictionary.*member = true;
    engine.ApplyConfig(config);
    EXPECT_TRUE(contains_fixture_word(engine.QueryCandidates("よ0", "", 0)));
    EXPECT_TRUE(contains_fixture_word(engine.QueryPredictions("よ", "", 0)));
    EXPECT_EQ(engine.ReverseConvert("high0", 0), "よ0");
    config.dictionary.*member = false;
    engine.ApplyConfig(config);
    EXPECT_FALSE(contains_fixture_word(engine.QueryCandidates("よ0", "", 0)));
    EXPECT_FALSE(contains_fixture_word(engine.QueryPredictions("よ", "", 0)));
    EXPECT_TRUE(engine.ReverseConvert("high0", 0).empty());
  }
}

TEST(DictionaryHost, EnabledMissingNeologdPackKeepsOtherLayersAvailable) {
  host::EngineConfig config;
  config.dictionary.neologd_enabled = true;
  host::InferenceEngine engine(std::make_unique<core::SimpleConverter>(), nullptr, config);
  EXPECT_FALSE(
      engine.LoadDictionaryLayer(learning::LayerId::Neologd, Fixture("missing-neologd.azdic")));
  ASSERT_TRUE(engine.LoadDictionaryLayer(learning::LayerId::TechnicalTerms,
                                         Fixture("settings-layers/technical_terms_lexicon.azdic")));
  const auto candidates = engine.QueryCandidates("よ0", "", 0);
  EXPECT_TRUE(std::any_of(candidates.begin(), candidates.end(),
                          [](const auto& candidate) { return candidate.surface == "high0"; }));
  EXPECT_EQ(engine.ReverseConvert("high0", 0), "よ0");
}

std::string Environment(const char* name) {
#ifdef _WIN32
  char* value = nullptr;
  size_t length = 0;
  if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) return {};
  std::string result(value);
  std::free(value);
  return result;
#else
  const char* value = std::getenv(name);
  return value ? value : "";
#endif
}

// The release payload is built from pinned upstreams (dictbuild/build_bundled.py), so this
// runs only when AZOOKEY_BUNDLED_DICTIONARY_DIR points at its dict directory.
TEST(DictionaryHost, ShippedBundleLoadsAndReverseConverts) {
  const auto directory = Environment("AZOOKEY_BUNDLED_DICTIONARY_DIR");
  if (directory.empty()) GTEST_SKIP() << "AZOOKEY_BUNDLED_DICTIONARY_DIR is not set";
  core::DoubleArrayTrie trie;
  ASSERT_TRUE(trie.Load(std::filesystem::path(directory) / "sudachi_lexicon.azdic", true))
      << trie.Error();
  core::DoubleArrayTrie technical;
  ASSERT_TRUE(
      technical.Load(std::filesystem::path(directory) / "technical_terms_lexicon.azdic", true))
      << technical.Error();
  host::InferenceEngine engine(std::make_unique<core::SimpleConverter>(), nullptr, {});
  const auto results = engine.LoadBundledDictionaryLayers(directory);
  ASSERT_EQ(results.size(), 5U);
  EXPECT_EQ(results[4].name, "lattice_core.azdic");
  EXPECT_TRUE(results[4].loaded) << results[4].error;
  EXPECT_EQ(results[1].name, "sudachi_lexicon.azdic");
  EXPECT_TRUE(results[1].loaded) << results[1].error;
  EXPECT_EQ(engine.ReverseConvert("東京", 0), "とうきょう");
  EXPECT_EQ(engine.ReverseConvert("変換", 0), "へんかん");
  EXPECT_EQ(results[3].name, "technical_terms_lexicon.azdic");
  ASSERT_TRUE(results[3].loaded) << results[3].error;
  engine.EnableDictionaryLayer(learning::LayerId::Sudachi, false);
  EXPECT_EQ(engine.ReverseConvert("TensorRT", 0), "てんそるあーるてぃー");
  const auto candidates = engine.QueryCandidates("てんそるあーるてぃー", "", 0);
  EXPECT_TRUE(std::any_of(candidates.begin(), candidates.end(),
                          [](const auto& candidate) { return candidate.surface == "TensorRT"; }));
}

// DEV-2101: DictionaryStore owns lattice_core and the lattice vocabularies of its layers
// (docs/lattice-conversion-spec.md sections 3.1, 3.6 and 8.2).
std::vector<std::string> LatticeSurfaces(const learning::DictionaryStore& store,
                                         std::string_view reading, size_t max_paths = 4) {
  core::LatticeQuery query;
  query.max_paths = max_paths;
  std::vector<std::string> surfaces;
  for (const auto& path : store.ConvertLattice(reading, query).paths)
    surfaces.push_back(path.Surface());
  return surfaces;
}

TEST(DictionaryStoreLattice, ConvertsWithLatticeCoreStaticLayersAndUserWords) {
  learning::DictionaryStore store;
  ASSERT_TRUE(store.LoadLatticeCore(Fixture("lattice/lattice_core.azdic"), true))
      << store.LatticeError();
  ASSERT_TRUE(
      store.LoadStatic(learning::LayerId::Sudachi, Fixture("lattice/sudachi_lexicon.azdic")));
  EXPECT_TRUE(store.IsLatticeAvailable());
  EXPECT_TRUE(store.LatticeError().empty());
  EXPECT_EQ(store.LatticeScoreScale(), 10000);
  EXPECT_TRUE(store.LatticeConnectionError(learning::LayerId::Sudachi).empty());
  EXPECT_EQ(LatticeSurfaces(store, "きょうはいいてんきです"),
            (std::vector<std::string>{"今日はいい天気です", "今日はいい転機です",
                                      "京はいい天気です", "今日はいいテンキです"}));
  // lattice_core is not a layer: its kana-only words stay out of the lookups.
  learning::LookupContext exact;
  EXPECT_TRUE(store.Lookup("です", exact).empty());
  EXPECT_FALSE(store.ContainsKnownWord("です", "です"));

  // The user dictionary's words enter with the default ids and word cost (spec section 5.3).
  store.SetUserWords({{"ピヨ", "ぴよ", std::nullopt, std::nullopt, std::nullopt}});
  EXPECT_EQ(LatticeSurfaces(store, "ぴよは", 1), std::vector<std::string>{"ピヨは"});
  store.EnableLayer(learning::LayerId::User, false);
  EXPECT_EQ(LatticeSurfaces(store, "ぴよは", 1), std::vector<std::string>{"ぴよは"});
  // A disabled static layer leaves the lattice too.
  store.EnableLayer(learning::LayerId::Sudachi, false);
  EXPECT_EQ(LatticeSurfaces(store, "てんき", 1), std::vector<std::string>{"てんき"});
  store.EnableLayer(learning::LayerId::Sudachi, true);
  EXPECT_EQ(LatticeSurfaces(store, "てんき", 1), std::vector<std::string>{"天気"});
}

TEST(DictionaryStoreLattice, RebuildsVocabulariesWhenALayerOrLatticeCoreLoadsAgain) {
  learning::DictionaryStore store;
  core::LatticeQuery query;
  EXPECT_EQ(store.ConvertLattice("てんき", query).status, core::LatticeStatus::Unavailable);
  EXPECT_EQ(store.LatticeError(), "not loaded");
  EXPECT_EQ(store.LatticeScoreScale(), 0);
  // A layer loaded before lattice_core gets its vocabulary once lattice_core loads.
  ASSERT_TRUE(
      store.LoadStatic(learning::LayerId::Sudachi, Fixture("lattice/sudachi_lexicon.azdic")));
  ASSERT_TRUE(store.LoadLatticeCore(Fixture("lattice/lattice_core.azdic")));
  EXPECT_EQ(LatticeSurfaces(store, "てんき", 1), std::vector<std::string>{"天気"});

  // Loading the layer again rebuilds its vocabulary: here its CONN is in another id space.
  ASSERT_TRUE(store.LoadStatic(learning::LayerId::Sudachi, Fixture("lattice/layer_space.azdic")));
  EXPECT_FALSE(store.LatticeConnectionError(learning::LayerId::Sudachi).empty());
  // Its words take the default ids (spec section 8.3), so テンキ is no longer a variant.
  const auto defaults = LatticeSurfaces(store, "てんき");
  EXPECT_NE(std::find(defaults.begin(), defaults.end(), "天気"), defaults.end());
  learning::LookupContext exact;
  EXPECT_FALSE(store.Lookup("てんき", exact).empty());
  ASSERT_TRUE(
      store.LoadStatic(learning::LayerId::Sudachi, Fixture("lattice/sudachi_lexicon.azdic")));
  EXPECT_TRUE(store.LatticeConnectionError(learning::LayerId::Sudachi).empty());

  // A broken lattice_core disables the lattice only; the layers keep answering lookups.
  EXPECT_FALSE(store.LoadLatticeCore(Fixture("lattice/core_no_cmat.azdic")));
  EXPECT_FALSE(store.IsLatticeAvailable());
  EXPECT_FALSE(store.LatticeError().empty());
  EXPECT_EQ(store.LatticeScoreScale(), 0);
  EXPECT_EQ(store.ConvertLattice("てんき", query).status, core::LatticeStatus::Unavailable);
  EXPECT_FALSE(store.Lookup("てんき", exact).empty());
  EXPECT_FALSE(store.LoadLatticeCore(Fixture("lattice/missing.azdic")));
  EXPECT_EQ(store.ConvertLattice("てんき", query).status, core::LatticeStatus::Unavailable);
  // Loading it again rebuilds every layer's vocabulary against the new dictionary.
  ASSERT_TRUE(store.LoadLatticeCore(Fixture("lattice/lattice_core.azdic")));
  EXPECT_TRUE(store.LatticeConnectionError(learning::LayerId::Sudachi).empty());
  EXPECT_EQ(LatticeSurfaces(store, "てんき", 1), std::vector<std::string>{"天気"});
}

// M71-B (DEV-2137): user dictionary words with a part of speech (user-dictionary-pos-spec
// sections 4 and 5) on the lattice_user fixture, whose rows are SudachiDict-shaped.
learning::UserWord UserWordWithPos(std::string word, std::string ruby,
                                   std::optional<std::string> pos) {
  learning::UserWord user_word;
  user_word.word = std::move(word);
  user_word.ruby = std::move(ruby);
  user_word.pos = std::move(pos);
  return user_word;
}

void LoadConjugationFixture(learning::DictionaryStore& store) {
  ASSERT_TRUE(store.LoadLatticeCore(Fixture("lattice_user/lattice_core.azdic"), true))
      << store.LatticeError();
  ASSERT_TRUE(
      store.LoadStatic(learning::LayerId::Sudachi, Fixture("lattice_user/sudachi_lexicon.azdic")));
}

// Section 9 (dictbuild): the forms section 4.2 makes of the representative words are the forms
// SudachiDict gives them, as extract_sudachi converts them, and the part-of-speech table built
// from those rows has the row of each (part of speech, conjugation type, conjugation form).
TEST(UserWordConjugation, FormsMatchTheSudachiFormsOfTheRepresentativeWords) {
  core::LatticeDictionary dictionary;
  ASSERT_TRUE(dictionary.Load(Fixture("lattice_user/lattice_core.azdic"), true))
      << dictionary.Error();
  struct Row {
    std::string reading, surface, name, type, form;
    int left{}, right{};
  };
  const auto split = [](std::string_view text, char separator) {
    std::vector<std::string> fields;
    for (size_t start = 0;;) {
      const auto end = text.find(separator, start);
      fields.emplace_back(text.substr(start, end == std::string_view::npos ? end : end - start));
      if (end == std::string_view::npos) return fields;
      start = end + 1;
    }
  };
  // The connection TSV: reading, surface, left id, right id, cost, pos, conjugation, normalized
  // form, and the six part-of-speech fields joined by ",".
  std::map<std::string, std::vector<Row>> by_word;
  std::ifstream connections(Fixture("lattice_user/conjugation.conn.tsv"), std::ios::binary);
  ASSERT_TRUE(connections.is_open());
  for (std::string line; std::getline(connections, line);) {
    if (line.empty() || line[0] == '#' || line.rfind("reading\t", 0) == 0) continue;
    const auto fields = split(line, '\t');
    ASSERT_EQ(fields.size(), 9U) << line;
    const auto pos = split(fields[8], ',');
    ASSERT_EQ(pos.size(), 6U) << line;
    std::string name;
    for (size_t i = 0; i < 4; ++i) {
      if (pos[i] == "*") continue;
      if (!name.empty()) name += "-";
      name += pos[i];
    }
    by_word[fields[7]].push_back(
        {fields[0], fields[1], name, pos[4], pos[5], std::stoi(fields[2]), std::stoi(fields[3])});
  }
  struct Representative {
    const char* word;
    const char* ruby;
    const char* pos;
  };
  const Representative words[] = {
      {"走る", "はしる", "verb_godan_ra"},      {"笑う", "わらう", "verb_godan_wa"},
      {"書く", "かく", "verb_godan_ka"},        {"泳ぐ", "およぐ", "verb_godan_ga"},
      {"話す", "はなす", "verb_godan_sa"},      {"待つ", "まつ", "verb_godan_ta"},
      {"死ぬ", "しぬ", "verb_godan_na"},        {"遊ぶ", "あそぶ", "verb_godan_ba"},
      {"読む", "よむ", "verb_godan_ma"},        {"見る", "みる", "verb_ichidan"},
      {"食べる", "たべる", "verb_ichidan"},     {"察する", "さっする", "verb_sahen"},
      {"高い", "たかい", "adjective"},          {"行く", "いく", "verb_godan_ka"},
      {"試みる", "こころみる", "verb_ichidan"}, {"逝く", "いく", "verb_godan_ka"},
  };
  for (const auto& representative : words) {
    const auto& rows = by_word[representative.word];
    ASSERT_FALSE(rows.empty()) << representative.word;
    const auto lattice_pos = learning::UserWordLatticePosOf(
        representative.word, representative.ruby, representative.pos);
    ASSERT_TRUE(lattice_pos) << representative.word;
    using Form = std::tuple<std::string, std::string, std::string>;  // form, reading, surface
    std::set<Form> sudachi, table;
    for (const auto& row : rows) {
      // Sections 2.1 and 4.3 give the word the conjugation type SudachiDict gives it.
      EXPECT_EQ(row.type, lattice_pos->conjugation_type) << representative.word;
      // Section 4.5: the table has the row of each form, here under the word's own part of
      // speech (見る and 行く are 動詞-非自立可能 in SudachiDict).
      const auto found = dictionary.ResolvePosRow(row.name, row.type, row.form);
      ASSERT_TRUE(found) << representative.word << " " << row.form;
      EXPECT_EQ(found->left_id, row.left) << representative.word << " " << row.form;
      EXPECT_EQ(found->right_id, row.right) << representative.word << " " << row.form;
      if (row.form == lattice_pos->conjugation_form) {
        EXPECT_EQ(row.reading, representative.ruby);
        EXPECT_EQ(row.surface, representative.word);
        continue;
      }
      sudachi.emplace(row.form, row.reading, row.surface);
    }
    // The rows the implementation looks up are those of the section 2.1 name (動詞-一般 for a
    // verb); 試みる and 逝く give that name the types of 見る and 行く.
    EXPECT_TRUE(dictionary.ResolvePosRow(lattice_pos->name, lattice_pos->conjugation_type,
                                         lattice_pos->conjugation_form))
        << representative.word;
    for (const auto& form :
         learning::ExpandUserWord(representative.word, representative.ruby, representative.pos)) {
      EXPECT_TRUE(dictionary.ResolvePosRow(lattice_pos->name, lattice_pos->conjugation_type,
                                           form.conjugation_form))
          << representative.word << " " << form.conjugation_form;
      table.emplace(std::string(form.conjugation_form), form.reading, form.surface);
    }
    EXPECT_EQ(table, sudachi) << representative.word;
  }
}

// Sections 4.5, 4.6 and 5.1: the forms of a conjugating word are lattice nodes, so ぐぐった
// converts to ググった without the model, and no lookup, membership test or reverse lookup
// sees them.
TEST(DictionaryStoreLattice, ConjugatedUserWordsBecomeNodesOnlyForTheLattice) {
  learning::DictionaryStore store;
  LoadConjugationFixture(store);
  store.SetUserWords({UserWordWithPos("ググる", "ぐぐる", std::nullopt)});
  EXPECT_EQ(store.UserWordFormCount(), 0U);
  EXPECT_EQ(LatticeSurfaces(store, "ぐぐった", 1), std::vector<std::string>{"ぐぐった"});
  store.SetUserWords({UserWordWithPos("ググる", "ぐぐる", "verb_godan_ra")});
  EXPECT_EQ(store.UserWordFormCount(), 7U);
  EXPECT_FALSE(store.UserWordFormsTruncated());
  EXPECT_EQ(LatticeSurfaces(store, "ぐぐった", 1), std::vector<std::string>{"ググった"});
  EXPECT_EQ(LatticeSurfaces(store, "ぐぐろう", 1), std::vector<std::string>{"ググろう"});

  for (const auto mode : {learning::LookupMode::Exact, learning::LookupMode::CommonPrefix,
                          learning::LookupMode::PredictivePrefix}) {
    learning::LookupContext context;
    context.mode = mode;
    for (const auto* reading : {"ぐぐっ", "ぐぐった", "ぐぐろう", "ぐぐ"})
      for (const auto& entry : store.Lookup(reading, context))
        EXPECT_EQ(entry.surface, "ググる") << reading << " " << static_cast<int>(mode);
  }
  learning::LookupContext predictive;
  predictive.mode = learning::LookupMode::PredictivePrefix;
  const auto predicted = store.Lookup("ぐぐ", predictive);
  ASSERT_EQ(predicted.size(), 1U);
  EXPECT_EQ(predicted.front().surface, "ググる");
  EXPECT_TRUE(store.ContainsKnownWord("ぐぐる", "ググる"));
  EXPECT_FALSE(store.ContainsKnownWord("ぐぐっ", "ググっ"));
  EXPECT_FALSE(store.ContainsKnownWord("ぐぐろう", "ググろう"));
  learning::LookupContext exact;
  EXPECT_FALSE(store.ReverseLookup("ググっ", exact));
  EXPECT_TRUE(store.ReverseLookup("ググる", exact));
}

// Section 4.6: the forms are built again whenever the user words or lattice_core load, follow
// the user dictionary's switch, and section 5.2 matches them from any position of a reading.
TEST(DictionaryStoreLattice, UserWordFormsFollowTheWordsLatticeCoreAndTheSwitch) {
  learning::DictionaryStore store;
  // Words published before lattice_core: built once it loads.
  store.SetUserWords({UserWordWithPos("ググる", "ぐぐる", "verb_godan_ra")});
  EXPECT_EQ(store.UserWordFormCount(), 0U);
  EXPECT_FALSE(store.HasUserWordForms("ぐぐった"));
  ASSERT_TRUE(store.LoadLatticeCore(Fixture("lattice_user/lattice_core.azdic")));
  EXPECT_EQ(store.UserWordFormCount(), 7U);
  EXPECT_TRUE(store.HasUserWordForms("ぐぐった"));
  EXPECT_TRUE(store.HasUserWordForms("きょうはぐぐったよ"));
  EXPECT_FALSE(store.HasUserWordForms("ぐぐ"));  // Shorter than every form.
  EXPECT_FALSE(store.HasUserWordForms("てんき"));
  EXPECT_FALSE(store.HasUserWordForms(""));
  // The user layer published by ReplaceMutable carries its part of speech too.
  learning::DictionaryEntry entry;
  entry.surface = "バズる";
  entry.reading = "ばずる";
  entry.frequency = .4;
  entry.pos = "verb_godan_ra";
  store.ReplaceMutable(learning::LayerId::User, {entry});
  EXPECT_EQ(store.UserWordFormCount(), 7U);
  EXPECT_TRUE(store.HasUserWordForms("ばずった"));
  EXPECT_FALSE(store.HasUserWordForms("ぐぐった"));
  EXPECT_EQ(LatticeSurfaces(store, "ばずった", 1), std::vector<std::string>{"バズった"});
  // A disabled user dictionary takes its forms out of the lattice and of the check.
  store.EnableLayer(learning::LayerId::User, false);
  EXPECT_FALSE(store.HasUserWordForms("ばずった"));
  EXPECT_EQ(LatticeSurfaces(store, "ばずった", 1), std::vector<std::string>{"ばずった"});
  store.EnableLayer(learning::LayerId::User, true);
  EXPECT_TRUE(store.HasUserWordForms("ばずった"));
  // No usable lattice_core, no forms; loading one again builds them against it.
  EXPECT_FALSE(store.LoadLatticeCore(Fixture("lattice/core_no_cmat.azdic")));
  EXPECT_EQ(store.UserWordFormCount(), 0U);
  EXPECT_FALSE(store.HasUserWordForms("ばずった"));
  ASSERT_TRUE(store.LoadLatticeCore(Fixture("lattice_user/lattice_core.azdic")));
  EXPECT_EQ(store.UserWordFormCount(), 7U);
  // Section 4.5: a lattice_core without the rows of 動詞-一般 makes none of the forms.
  ASSERT_TRUE(store.LoadLatticeCore(Fixture("lattice/lattice_core.azdic")));
  EXPECT_EQ(store.UserWordFormCount(), 0U);
  EXPECT_FALSE(store.HasUserWordForms("ばずった"));
}

// Section 4.6: the forms are made in the ascending order of the user dictionary's keys, and
// the ones past the limit are not made.
TEST(DictionaryStoreLattice, UserWordFormsStopAtTheLimitInKeyOrder) {
  EXPECT_EQ(learning::kMaxUserWordForms, 100000U);
  learning::DictionaryStore store;
  LoadConjugationFixture(store);
  store.SetUserWords({UserWordWithPos("バズる", "ばずる", "verb_godan_ra"),
                      UserWordWithPos("ググる", "ぐぐる", "verb_godan_ra")});
  EXPECT_EQ(store.UserWordFormCount(), 14U);
  EXPECT_FALSE(store.UserWordFormsTruncated());
  // Exactly the limit is not over it.
  store.SetUserWordFormLimitForTests(14);
  EXPECT_EQ(store.UserWordFormCount(), 14U);
  EXPECT_FALSE(store.UserWordFormsTruncated());
  // ぐぐる comes first: all its forms, then the first form of ばずる (its 未然形).
  store.SetUserWordFormLimitForTests(8);
  EXPECT_EQ(store.UserWordFormCount(), 8U);
  EXPECT_TRUE(store.UserWordFormsTruncated());
  EXPECT_TRUE(store.HasUserWordForms("ぐぐった"));
  EXPECT_TRUE(store.HasUserWordForms("ばずら"));
  EXPECT_FALSE(store.HasUserWordForms("ばずった"));

  // Two words of one key that ReplaceMutable publishes go by their part of speech, whatever the
  // order given: verb_godan_ra before verb_ichidan, so the limit keeps the 五段 forms.
  const auto word = [](std::string pos) {
    learning::DictionaryEntry entry;
    entry.surface = "タべる";
    entry.reading = "たべる";
    entry.frequency = .4;
    entry.pos = std::move(pos);
    return entry;
  };
  store.ReplaceMutable(learning::LayerId::User, {word("verb_ichidan"), word("verb_godan_ra")});
  store.SetUserWordFormLimitForTests(7);
  EXPECT_TRUE(store.UserWordFormsTruncated());
  EXPECT_TRUE(store.HasUserWordForms("たべっ"));     // 五段の連用形-促音便
  EXPECT_FALSE(store.HasUserWordForms("たべよう"));  // 一段の意志推量形、一段の語幹
}

// Section 4.5: a registered form takes the row of its part of speech (the terminal form of a
// conjugating word), a prefix joins the next word's segment, and a noun keeps the default ids.
// Section 5.2: the marked search finds the path through a conjugated form with the smallest
// cost of any path beside it.
TEST(DictionaryStoreLattice, UserWordsTakeTheRowsOfTheirPartOfSpeech) {
  learning::DictionaryStore store;
  LoadConjugationFixture(store);
  core::LatticeDictionary dictionary;
  ASSERT_TRUE(dictionary.Load(Fixture("lattice_user/lattice_core.azdic")));
  store.SetUserWords({UserWordWithPos("ググる", "ぐぐる", "verb_godan_ra"),
                      UserWordWithPos("超", "ちょう", "prefix"),
                      UserWordWithPos("ピヨ", "ぴよ", std::nullopt)});
  core::LatticeQuery query;
  query.max_paths = 1;
  const auto verb = store.ConvertLattice("ぐぐる", query);
  ASSERT_EQ(verb.paths.size(), 1U);
  ASSERT_EQ(verb.paths[0].words.size(), 1U);
  const auto terminal = dictionary.ResolvePosRow("動詞-一般", "五段-ラ行", "終止形-一般");
  ASSERT_TRUE(terminal);
  // The registered form and its 連体形 have the same surface and cost; the smaller ids, those
  // of the terminal form, come first (lattice-conversion-spec section 6.3).
  EXPECT_EQ(verb.paths[0].words[0].left_id, terminal->left_id);
  EXPECT_EQ(verb.paths[0].words[0].pos, terminal->pos);
  EXPECT_EQ(dictionary.PosNames()[verb.paths[0].words[0].pos], "動詞-一般");
  // The fixture has no 接頭辞 row: its name still makes 超 a prefix of 天気's segment.
  const auto prefixed = store.ConvertLattice("ちょうてんき", query);
  ASSERT_EQ(prefixed.paths.size(), 1U);
  EXPECT_EQ(prefixed.paths[0].Surface(), "超天気");
  EXPECT_EQ(prefixed.paths[0].segments.size(), 1U);
  EXPECT_EQ(prefixed.paths[0].words[0].word_class, core::LatticeWordClass::Prefix);
  const auto noun = store.ConvertLattice("ぴよ", query);
  ASSERT_EQ(noun.paths.size(), 1U);
  EXPECT_EQ(noun.paths[0].words[0].pos, core::kNoLatticePos);
  EXPECT_EQ(noun.paths[0].words[0].left_id, dictionary.Parameters().unknown_left_id);

  // にほんご: 日本|語 costs 4000; through にほん of ニホむ and 語, 14000; of ニホぬ, one more
  // (the fixture's one costly connection).
  core::LatticeQuery marked;
  marked.max_paths = 1;
  marked.marked_only = true;
  store.SetUserWords({UserWordWithPos("ニホむ", "にほむ", "verb_godan_ma")});
  const auto through_ma = store.ConvertLattice("にほんご", marked);
  ASSERT_EQ(through_ma.paths.size(), 1U);
  EXPECT_EQ(through_ma.paths[0].Surface(), "ニホん語");
  EXPECT_EQ(through_ma.paths[0].cost, 14000);
  ASSERT_TRUE(through_ma.best_cost);
  EXPECT_EQ(*through_ma.best_cost, 4000);
  EXPECT_EQ(LatticeSurfaces(store, "にほんご", 1), std::vector<std::string>{"日本語"});
  store.SetUserWords({UserWordWithPos("ニホぬ", "にほぬ", "verb_godan_na")});
  const auto through_na = store.ConvertLattice("にほんご", marked);
  ASSERT_EQ(through_na.paths.size(), 1U);
  EXPECT_EQ(through_na.paths[0].cost, 14001);
  // A noun is not marked: no marked path.
  store.SetUserWords({UserWordWithPos("ニホん", "にほん", std::nullopt)});
  const auto unmarked = store.ConvertLattice("にほんご", marked);
  EXPECT_TRUE(unmarked.paths.empty());
  ASSERT_TRUE(unmarked.best_cost);
}
}  // namespace
