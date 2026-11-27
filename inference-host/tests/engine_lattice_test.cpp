// M65 stage 1 (docs/lattice-conversion-spec.md sections 3.2 to 3.6): the engine adds the
// lattice's N-best on requests that do not go through the model, with the dictbuild fixture
// lattice/ (今日|は|いい|天気|です is its best path).
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../../learning/tests/TestByteCrypto.h"
#include "azookey/core/SimpleConverter.h"
#include "azookey/host/InferenceEngine.h"
#include "azookey/ipc/Json.h"
#include "azookey/learning/AutoWordStore.h"
#include "azookey/logging/RuntimeLogger.h"

namespace engine_lattice_test {
namespace {

using azookey::core::Candidate;
using azookey::core::CandidateSource;
using azookey::host::EngineConfig;
using azookey::host::InferenceEngine;

constexpr uint64_t kNow = 1'700'000'000ULL;
const std::string kSentence = "きょうはいいてんきです";
constexpr std::string_view kLatticePrefix = "lattice;rank=";

std::filesystem::path Fixture(const std::string& name) {
  return std::filesystem::path(AZOOKEY_DICT_FIXTURE) / name;
}

// A temporary path no other test run shares: the time and a counter.
std::filesystem::path UniqueTempPath(const std::string& stem, const std::string& extension) {
  static std::atomic<int> next_id{0};
  return std::filesystem::temp_directory_path() /
         (stem + "_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
          "_" + std::to_string(next_id++) + extension);
}

// A dict directory holding the fixture's sudachi_lexicon and the given lattice_core.
class LatticeDirectory {
 public:
  LatticeDirectory(const std::string& layer, const std::string& core) {
    path_ = std::filesystem::temp_directory_path() /
            ("azookey_engine_lattice_" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
             std::to_string(next_id_++));
    std::filesystem::create_directories(path_);
    std::filesystem::copy_file(Fixture("lattice/" + layer + ".azdic"),
                               path_ / "sudachi_lexicon.azdic");
    std::filesystem::copy_file(Fixture("lattice/" + core + ".azdic"), path_ / "lattice_core.azdic");
  }
  ~LatticeDirectory() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  LatticeDirectory(const LatticeDirectory&) = delete;
  LatticeDirectory& operator=(const LatticeDirectory&) = delete;
  const std::filesystem::path& Path() const { return path_; }

 private:
  std::filesystem::path path_;
  static inline int next_id_ = 0;
};

EngineConfig NoLimitConfig(bool lattice = true) {
  EngineConfig config;
  config.max_candidates = 0;  // N stays at its default of 8 (section 3.5).
  config.lattice.enabled = lattice;
  return config;
}

std::unique_ptr<InferenceEngine> MakeEngine(const EngineConfig& config,
                                            const std::filesystem::path& directory,
                                            bool expect_lattice = true) {
  auto engine = std::make_unique<InferenceEngine>(
      std::make_unique<azookey::core::SimpleConverter>(), nullptr, config);
  const auto results = engine->LoadBundledDictionaryLayers(directory);
  EXPECT_EQ(results.size(), config.lattice.enabled ? 5U : 4U);
  if (config.lattice.enabled) {
    EXPECT_EQ(results.back().name, "lattice_core.azdic");
    EXPECT_EQ(results.back().loaded, expect_lattice) << results.back().error;
  }
  return engine;
}

std::vector<Candidate> LatticeCandidates(const std::vector<Candidate>& candidates) {
  std::vector<Candidate> lattice;
  for (const auto& candidate : candidates)
    if (candidate.debug_info.starts_with(kLatticePrefix)) lattice.push_back(candidate);
  return lattice;
}

std::vector<std::string> Describe(const std::vector<Candidate>& candidates) {
  std::vector<std::string> described;
  for (const auto& c : candidates)
    described.push_back(c.surface + "|" + c.reading + "|" + std::to_string(c.score) + "|" +
                        c.debug_info);
  return described;
}

TEST(EngineLattice, ModelNotLoadedAddsTheNBestInItsScoreBand) {
  const auto engine = MakeEngine(NoLimitConfig(), Fixture("lattice"));
  const auto candidates = engine->QueryCandidates(kSentence, "", kNow);
  const auto lattice = LatticeCandidates(candidates);
  ASSERT_GE(lattice.size(), 4U);
  ASSERT_LE(lattice.size(), 8U);
  // Path costs 24500, 27000, 29500, 30500 and C = 10000 (section 3.4).
  const std::vector<std::pair<std::string, double>> expected = {{"今日はいい天気です", 1.0},
                                                                {"今日はいい転機です", 0.75},
                                                                {"京はいい天気です", 0.5},
                                                                {"今日はいいテンキです", 0.4}};
  for (size_t i = 0; i < lattice.size(); ++i) {
    EXPECT_EQ(lattice[i].source, CandidateSource::SystemDictionary);
    EXPECT_EQ(lattice[i].reading, kSentence);
    // No tag of its own; AssignHeuristicTags applies as to the other candidates (section 3.4).
    EXPECT_GE(lattice[i].score, 0.1);
    EXPECT_LE(lattice[i].score, 1.0);
    if (i < expected.size()) {
      EXPECT_EQ(lattice[i].debug_info, std::string(kLatticePrefix) + std::to_string(i + 1));
      EXPECT_EQ(lattice[i].surface, expected[i].first);
      EXPECT_DOUBLE_EQ(lattice[i].score, expected[i].second);
    }
  }
  // The converter's candidates come first (zenzai-inference-spec section 7.1 step 5).
  ASSERT_FALSE(candidates.empty());
  EXPECT_FALSE(candidates.front().debug_info.starts_with(kLatticePrefix));

  // Live conversion takes one path (section 3.5).
  const auto live = engine->QueryCandidatesEx(kSentence, "", kNow, nullptr, 0, true);
  const auto live_lattice = LatticeCandidates(live.candidates);
  ASSERT_EQ(live_lattice.size(), 1U);
  EXPECT_EQ(live_lattice.front().surface, "今日はいい天気です");
}

TEST(EngineLattice, ConfirmedAutoWordsAreNodesUntilForgottenOrRejected) {
  using azookey::host::LearningDataStore;
  using azookey::learning::AutoWordSaveOutcome;
  using azookey::learning::AutoWordState;

  const LatticeDirectory directory("sudachi_lexicon", "lattice_core");
  azookey::learning::AutoWordStore auto_words(directory.Path() / "auto_words.tsv",
                                              &azookey::learning::test::Crypto());
  ASSERT_TRUE(auto_words.Observe("古美", "こみ", kNow, 1, true));
  ASSERT_TRUE(auto_words.Observe("湖海", "コミ", kNow, 1, true));
  auto_words.Observe("小実", "こみ", kNow, 1, false);
  ASSERT_TRUE(auto_words.Observe("小美", "こみ", kNow, 1, true));
  ASSERT_TRUE(auto_words.Reject("小美", "こみ"));
  const auto engine = MakeEngine(NoLimitConfig(), directory.Path());
  engine->SetAutoWordStore(&auto_words);

  // There is no exact auto-word for the whole reading, and the word starts after the first kana.
  const auto query = [&] { return LatticeCandidates(engine->QueryCandidates("ぬこみ", "", kNow)); };
  const auto has_surface = [](const std::vector<Candidate>& candidates, std::string_view surface) {
    return std::any_of(candidates.begin(), candidates.end(),
                       [&](const Candidate& candidate) { return candidate.surface == surface; });
  };
  const auto initial = query();
  ASSERT_TRUE(has_surface(initial, "ぬ古美"));
  ASSERT_TRUE(has_surface(initial, "ぬ湖海"));
  EXPECT_FALSE(has_surface(initial, "ぬ小実"));
  EXPECT_FALSE(has_surface(initial, "ぬ小美"));

  engine->EnableDictionaryLayer(azookey::learning::LayerId::AutoWords, false);
  const auto disabled = query();
  EXPECT_FALSE(has_surface(disabled, "ぬ古美"));
  EXPECT_FALSE(has_surface(disabled, "ぬ湖海"));
  engine->EnableDictionaryLayer(azookey::learning::LayerId::AutoWords, true);
  EXPECT_TRUE(has_surface(query(), "ぬ古美"));

  const auto page = engine->ListLearningEntries(LearningDataStore::AutoWord, "", 0, 10);
  const auto forgotten = std::find_if(page.entries.begin(), page.entries.end(),
                                      [](const auto& entry) { return entry.surface == "古美"; });
  ASSERT_NE(forgotten, page.entries.end());
  ASSERT_EQ(engine->ForgetLearningEntry(LearningDataStore::AutoWord, forgotten->id),
            InferenceEngine::ForgetOutcome::Forgotten);
  const auto after_forget = query();
  EXPECT_FALSE(has_surface(after_forget, "ぬ古美"));
  ASSERT_TRUE(has_surface(after_forget, "ぬ湖海"));

  // ResolveNewWord reject publishes the new state through this store transaction.
  ASSERT_EQ(auto_words.SetStateAndSave("湖海", "コミ", AutoWordState::Rejected),
            AutoWordSaveOutcome::Changed);
  EXPECT_FALSE(has_surface(query(), "ぬ湖海"));
}

TEST(EngineLattice, OffOrWithoutAUsableLatticeCoreTheCandidatesStayAsBefore) {
  // The same layers on both sides: only lattice_core differs.
  const LatticeDirectory good("sudachi_lexicon", "lattice_core");
  const auto off = MakeEngine(NoLimitConfig(false), good.Path());
  const auto before = off->QueryCandidates(kSentence, "", kNow);
  EXPECT_TRUE(LatticeCandidates(before).empty());

  // Section 3.6: a lattice_core without CMAT leaves the lattice off and nothing else.
  const LatticeDirectory broken("sudachi_lexicon", "core_no_cmat");
  const auto unusable = MakeEngine(NoLimitConfig(), broken.Path(), false);
  EXPECT_EQ(Describe(unusable->QueryCandidates(kSentence, "", kNow)), Describe(before));
  EXPECT_EQ(Describe(unusable->QueryCandidates("てんき", "", kNow)),
            Describe(off->QueryCandidates("てんき", "", kNow)));

  // Over 64 code points: no lattice (section 3.6).
  std::string long_reading;
  for (int i = 0; i < 65; ++i) long_reading += "て";
  const auto on = MakeEngine(NoLimitConfig(), Fixture("lattice"));
  EXPECT_TRUE(LatticeCandidates(on->QueryCandidates(long_reading, "", kNow)).empty());
  EXPECT_FALSE(
      LatticeCandidates(on->QueryCandidates(long_reading.substr(0, 64 * 3), "", kNow)).empty());
}

TEST(EngineLattice, ALayerWhoseConnectionsFailKeepsItsLookupsAndEntersWithDefaultIds) {
  const LatticeDirectory good("sudachi_lexicon", "lattice_core");
  const LatticeDirectory other_space("layer_space", "lattice_core");
  const auto expected = MakeEngine(NoLimitConfig(), good.Path());
  const auto engine = MakeEngine(NoLimitConfig(), other_space.Path());
  // Surfaces the layer's exact lookup supplied, also when a lattice path of the same surface
  // took their place (DedupMergedCandidates keeps "dup:dictionary").
  const auto dictionary_words = [](const std::vector<Candidate>& candidates) {
    std::vector<std::string> words;
    for (const auto& c : candidates)
      if (c.debug_info.find("dictionary") != std::string::npos) words.push_back(c.surface);
    std::sort(words.begin(), words.end());
    return words;
  };
  // The layer's exact lookups and reverse lookups do not change (section 3.6).
  const auto words = dictionary_words(engine->QueryCandidates("てんき", "", kNow));
  EXPECT_EQ(words, dictionary_words(expected->QueryCandidates("てんき", "", kNow)));
  EXPECT_EQ(words, (std::vector<std::string>{"テンキ", "天気", "転機"}));
  EXPECT_EQ(engine->ReverseConvert("天気", kNow), "てんき");
  const auto lattice = LatticeCandidates(engine->QueryCandidates(kSentence, "", kNow));
  ASSERT_FALSE(lattice.empty());
  EXPECT_TRUE(std::any_of(lattice.begin(), lattice.end(),
                          [](const Candidate& c) { return c.surface == "今日はいい天気です"; }));
}

TEST(EngineLattice, AnotherReadingIsConvertedWithoutTheLattice) {
  const auto engine = MakeEngine(NoLimitConfig(), Fixture("lattice"));
  // Precondition: alone, the lattice splits いいてんき into a path no dictionary holds.
  const auto alone = LatticeCandidates(engine->QueryCandidates("いいてんき", "", kNow));
  ASSERT_TRUE(std::any_of(alone.begin(), alone.end(),
                          [](const Candidate& c) { return c.surface == "いい天気"; }));
  // DEV-1773 without a model: the other reading's dictionary words follow the typed ones; its
  // lattice paths do not.
  const auto split =
      engine->QueryAmbiguousCandidatesEx(kSentence, {"いいてんき"}, "", kNow, nullptr);
  for (const auto& c : split.candidates) {
    if (c.reading != "いいてんき") continue;
    EXPECT_FALSE(c.debug_info.starts_with(kLatticePrefix)) << c.surface << " " << c.debug_info;
    EXPECT_NE(c.surface, "いい天気");
  }
  const auto words = engine->QueryAmbiguousCandidatesEx(kSentence, {"てんき"}, "", kNow, nullptr);
  EXPECT_TRUE(std::any_of(words.candidates.begin(), words.candidates.end(), [](const Candidate& c) {
    return c.reading == "てんき" && c.surface == "転機" && c.debug_info.starts_with("dictionary");
  }));
}

TEST(EngineLattice, TheRequestsOwnDeadlineBoundsTheLattice) {
  const auto engine = MakeEngine(NoLimitConfig(), Fixture("lattice"));
  // Section 3.2: no time left (the M63-B extra conversion's deadline, for one) means no lattice.
  azookey::host::MixedConversionOptions past;
  past.deadline = std::chrono::steady_clock::now() - std::chrono::seconds(1);
  past.typo_correction = false;
  const auto late =
      engine->QueryCandidatesEx(kSentence, "", kNow, nullptr, 0, false, nullptr, nullptr, &past);
  EXPECT_TRUE(LatticeCandidates(late.candidates).empty());
  azookey::host::MixedConversionOptions future = past;
  future.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  const auto in_time =
      engine->QueryCandidatesEx(kSentence, "", kNow, nullptr, 0, false, nullptr, nullptr, &future);
  EXPECT_FALSE(LatticeCandidates(in_time.candidates).empty());
}

// The options the Dispatcher gives a first segment's request (segment-editing-spec section 5.3);
// an empty remaining reading is an ordinary request converted as typed.
azookey::host::MixedConversionOptions FirstSegment(const std::string& remaining) {
  azookey::host::MixedConversionOptions options;
  options.typo_correction = false;
  options.remaining_reading = remaining;
  return options;
}

std::vector<Candidate> QueryFirstSegment(InferenceEngine& engine, const std::string& reading,
                                         const std::string& remaining) {
  const auto options = FirstSegment(remaining);
  return engine.QueryCandidatesEx(reading, "", kNow, nullptr, 0, false, nullptr, nullptr, &options)
      .candidates;
}

// M66 (segment-editing-spec section 5.3, lattice-conversion-spec section 7.3): with a remaining
// reading, the lattice searches both with a word boundary at e and gives the surfaces before e.
// B(r) enters the costs: お|京 is 4000 behind お|今日 (B(20) - B(10) = 1000) instead of 3000.
TEST(EngineLattice, AFirstSegmentsCandidatesAreItsSurfacesBeforeTheRemainingReading) {
  const auto engine = MakeEngine(NoLimitConfig(), Fixture("lattice"));
  const std::string head = "おきょう";
  const std::string rest = "はいいてんきです";
  const auto score_of = [](const std::vector<Candidate>& lattice, const std::string& surface) {
    const auto found = std::find_if(lattice.begin(), lattice.end(),
                                    [&](const Candidate& c) { return c.surface == surface; });
    return found == lattice.end() ? -1.0 : found->score;
  };

  // Without the remaining reading: the first segment's reading alone (path costs 13500, 16500).
  const auto alone = LatticeCandidates(QueryFirstSegment(*engine, head, ""));
  ASSERT_GE(alone.size(), 2U);
  EXPECT_EQ(alone[0].surface, "お今日");
  EXPECT_DOUBLE_EQ(alone[0].score, 1.0);
  EXPECT_DOUBLE_EQ(score_of(alone, "お京"), 0.7);

  // Path costs 31000 and 35000: each adds B of its last word's right id.
  const auto first = LatticeCandidates(QueryFirstSegment(*engine, head, rest));
  ASSERT_GE(first.size(), 2U);
  ASSERT_LE(first.size(), 8U);
  EXPECT_EQ(first[0].surface, "お今日");
  EXPECT_DOUBLE_EQ(first[0].score, 1.0);
  EXPECT_DOUBLE_EQ(score_of(first, "お京"), 0.6);
  for (size_t i = 0; i < first.size(); ++i) {
    // Section 3.4 with the first segment's reading as the request's (segment-editing 5.3).
    EXPECT_EQ(first[i].reading, head);
    EXPECT_EQ(first[i].source, CandidateSource::SystemDictionary);
    EXPECT_EQ(first[i].debug_info, std::string(kLatticePrefix) + std::to_string(i + 1));
    EXPECT_EQ(first[i].surface.find("は"), std::string::npos) << first[i].surface;
  }

  // Over 64 code points together: the first segment alone. Up to 64: still together.
  const auto padded = [&](size_t total) {
    std::string padding;
    for (size_t i = 4 + 8; i < total; ++i) padding += "て";
    return rest + padding;
  };
  EXPECT_DOUBLE_EQ(
      score_of(LatticeCandidates(QueryFirstSegment(*engine, head, padded(64))), "お京"), 0.6);
  EXPECT_EQ(Describe(LatticeCandidates(QueryFirstSegment(*engine, head, padded(65)))),
            Describe(alone));
}

// segment-editing-spec section 10: a request without the remaining reading keeps its candidates.
TEST(EngineLattice, ARequestWithoutTheRemainingReadingKeepsItsCandidates) {
  const auto engine = MakeEngine(NoLimitConfig(), Fixture("lattice"));
  const auto typed = engine->QueryCandidatesEx(kSentence, "", kNow, nullptr, 0, false).candidates;
  EXPECT_EQ(Describe(QueryFirstSegment(*engine, kSentence, "")), Describe(typed));
  const auto lattice = LatticeCandidates(typed);
  ASSERT_FALSE(lattice.empty());
  EXPECT_EQ(lattice.front().surface, "今日はいい天気です");
}

TEST(EngineLattice, SafeModeConvertsWithoutTheModelAndGetsTheLattice) {
  const auto engine = MakeEngine(NoLimitConfig(), Fixture("lattice"));
  engine->RestoreHealthState(azookey::host::HealthState::SafeMode);
  azookey::host::ModelLoadOptions options;
  options.path = UniqueTempPath("azookey_engine_lattice_safe", ".gguf").string();
  // SafeMode refuses the load before reading the file, so the fallback converter stays active.
  ASSERT_FALSE(engine->LoadModelWithResult(options).ok);
  const auto lattice = LatticeCandidates(engine->QueryCandidates(kSentence, "", kNow));
  ASSERT_FALSE(lattice.empty());
  EXPECT_EQ(lattice.front().surface, "今日はいい天気です");
}

// Section 3.6: the degradations are logged by their result (and the layer) only, never with the
// reading or a surface, and a canceled request logs none.
TEST(EngineLattice, DegradationsAreLoggedWithoutTheReading) {
  const auto log_path = UniqueTempPath("azookey_engine_lattice_log", ".jsonl");
  std::string long_reading;
  for (int i = 0; i < 65; ++i) long_reading += "て";
  {
    azookey::logging::RuntimeLoggerOptions log_options;
    log_options.enabled = true;
    log_options.component = "host";
    log_options.output_path = log_path;
    azookey::logging::RuntimeLogger logger(log_options);
    // A layer whose CONN is in another id space is logged once, when it loads.
    const LatticeDirectory other_space("layer_space", "lattice_core");
    InferenceEngine engine(std::make_unique<azookey::core::SimpleConverter>(), nullptr,
                           NoLimitConfig(), &logger);
    const auto results = engine.LoadBundledDictionaryLayers(other_space.Path());
    ASSERT_TRUE(results.back().loaded) << results.back().error;
    (void)engine.QueryCandidates(long_reading, "", kNow);  // rejected
    std::atomic<bool> cancel{true};
    EXPECT_TRUE(engine.QueryCandidates(kSentence, "", kNow, &cancel).empty());
    EXPECT_FALSE(LatticeCandidates(engine.QueryCandidates(kSentence, "", kNow)).empty());
  }
  std::ifstream stream(log_path);
  ASSERT_TRUE(stream.is_open());
  size_t connections = 0;
  std::vector<std::string> conversions;
  for (std::string line; std::getline(stream, line);) {
    const auto value = azookey::ipc::json::Parse(line);
    ASSERT_TRUE(value) << line;
    const auto event = value->GetString("event");
    if (!event || !event->starts_with("lattice_")) continue;
    EXPECT_EQ(line.find("て"), std::string::npos) << line;
    EXPECT_EQ(line.find("きょう"), std::string::npos) << line;
    std::set<std::string> fields;
    for (const auto& [key, field] : value->AsObject()) {
      (void)field;
      if (key != "ts" && key != "component" && key != "level" && key != "event") fields.insert(key);
    }
    if (*event == "lattice_connections") {
      ++connections;
      EXPECT_EQ(fields, (std::set<std::string>{"layer", "result"}));
      EXPECT_EQ(value->GetString("layer"), "sudachi_lexicon");
      EXPECT_EQ(value->GetString("result"), "unused");
    } else {
      EXPECT_EQ(*event, "lattice_conversion");
      EXPECT_EQ(fields, std::set<std::string>{"result"});
      conversions.push_back(value->GetString("result").value_or(""));
    }
  }
  stream.close();
  EXPECT_EQ(connections, 1U);
  // Only the over-long reading; the canceled and the complete requests log nothing.
  EXPECT_EQ(conversions, std::vector<std::string>{"rejected"});
  std::error_code ec;
  std::filesystem::remove(log_path, ec);
}

// M71-B (DEV-2137): an in-memory user dictionary of one word (never loaded or saved).
std::unique_ptr<azookey::learning::UserDictionary> UserDictionaryWith(
    const std::string& word, const std::string& ruby, std::optional<std::string> pos) {
  auto dictionary = std::make_unique<azookey::learning::UserDictionary>(
      UniqueTempPath("azookey_engine_lattice_user", ".json"));
  azookey::learning::UserWord user_word;
  user_word.word = word;
  user_word.ruby = ruby;
  user_word.pos = std::move(pos);
  dictionary->Add(user_word);
  return dictionary;
}

constexpr std::string_view kUserWordPathDebug = "lattice-user";

std::vector<Candidate> UserWordPaths(const std::vector<Candidate>& candidates) {
  std::vector<Candidate> paths;
  for (const auto& candidate : candidates)
    if (candidate.debug_info == kUserWordPathDebug) paths.push_back(candidate);
  return paths;
}

// user-dictionary-pos-spec section 5.1 on the lattice_user fixture: without the model, the forms
// of a registered verb are lattice nodes like the other words, so ぐぐった converts to ググった,
// and the user dictionary's list holds the registered form only (section 4.6).
TEST(EngineLattice, ARegisteredVerbConvertsConjugatedWithoutTheModel) {
  const auto with_pos = UserDictionaryWith("ググる", "ぐぐる", "verb_godan_ra");
  const auto without_pos = UserDictionaryWith("ググる", "ぐぐる", std::nullopt);
  const auto engine = MakeEngine(NoLimitConfig(), Fixture("lattice_user"));
  engine->SetUserDictionary(with_pos.get());
  const auto candidates = engine->QueryCandidates("ぐぐった", "", kNow);
  const auto lattice = LatticeCandidates(candidates);
  ASSERT_FALSE(lattice.empty());
  EXPECT_EQ(lattice.front().surface, "ググった");
  EXPECT_EQ(lattice.front().reading, "ぐぐった");
  EXPECT_EQ(lattice.front().debug_info, std::string(kLatticePrefix) + "1");
  // Section 5.1: no conjugated-word path of section 5.2 without the model.
  EXPECT_TRUE(UserWordPaths(candidates).empty());
  const auto page =
      engine->ListLearningEntries(azookey::host::LearningDataStore::UserDictionary, "", 0, 100);
  ASSERT_EQ(page.entries.size(), 1U);
  EXPECT_EQ(page.entries[0].surface, "ググる");
  // Without its part of speech, the word has no 連用形.
  engine->SetUserDictionary(without_pos.get());
  for (const auto& c : engine->QueryCandidates("ぐぐった", "", kNow))
    EXPECT_NE(c.surface, "ググった") << c.debug_info;
  engine->SetUserDictionary(nullptr);
}

#if !AZOOKEY_WITH_LLAMA_CPP
// Section 3.2: a request the model converter fell back on gets the lattice; one it answered
// does not. The minimal GGUF loads only with the mock runtime (no real llama.cpp).
TEST(EngineLattice, ModelFallbacksGetTheLatticeAndAnsweredRequestsDoNot) {
  const auto model = UniqueTempPath("azookey_engine_lattice_model", ".gguf");
  {
    std::ofstream out(model, std::ios::binary);
    out.write("GGUF\x03\x00\x00\x00", 8);
  }
  const auto engine = MakeEngine(NoLimitConfig(), Fixture("lattice"));
  azookey::host::ModelLoadOptions options;
  options.path = model.string();
  options.mock_zenzai_candidates_for_tests = true;
  ASSERT_TRUE(engine->LoadModelWithResult(options).ok);
  // The 15 ms budget of section 3.2 would let a slow build (coverage) cut the lattice off by
  // time; an hour leaves only the conditions under test. The budget itself is checked below.
  engine->SetLatticeFallbackBudgetForTests(std::chrono::hours(1));

  // The mock generates nothing for this reading: zenzai-degraded.
  const auto degraded = engine->QueryCandidates(kSentence, "", kNow);
  ASSERT_FALSE(degraded.empty());
  EXPECT_TRUE(std::any_of(degraded.begin(), degraded.end(), [](const Candidate& c) {
    return c.debug_info.find("zenzai-degraded") != std::string::npos;
  }));
  const auto lattice = LatticeCandidates(degraded);
  ASSERT_FALSE(lattice.empty());
  EXPECT_EQ(lattice.front().surface, "今日はいい天気です");

  // zenzai-deadline-fallback: the lattice's only path is the reading itself, which takes the
  // place of the fallback's identity candidate (score 1.0 over 0.6).
  const auto deadline = engine->QueryCandidates("じかんぎれ", "", kNow);
  EXPECT_TRUE(std::any_of(deadline.begin(), deadline.end(), [](const Candidate& c) {
    return c.debug_info.starts_with("lattice;rank=1") &&
           c.debug_info.find("zenzai-deadline-fallback") != std::string::npos;
  }));

  // The model answered: no lattice (stage 2 is off).
  const auto answered = engine->QueryCandidates("にほんご", "", kNow);
  ASSERT_FALSE(answered.empty());
  EXPECT_EQ(answered.front().source, CandidateSource::Model);
  EXPECT_TRUE(LatticeCandidates(answered).empty());

  // A spent budget cuts the lattice off before its forward pass: no paths (section 6.4). The
  // deadline has passed before the search starts, so this does not depend on the machine's speed.
  engine->SetLatticeFallbackBudgetForTests(std::chrono::nanoseconds(0));
  const auto spent = engine->QueryCandidates(kSentence, "", kNow);
  ASSERT_FALSE(spent.empty());
  EXPECT_TRUE(LatticeCandidates(spent).empty());
  std::error_code ec;
  std::filesystem::remove(model, ec);
}

// M66 (segment-editing-spec section 5.3): a first segment the model fell back on gets the
// first segment's lattice candidates; one the model answered is converted as without the
// remaining reading.
TEST(EngineLattice, AFirstSegmentFollowsTheModelPathRules) {
  const auto model = UniqueTempPath("azookey_engine_lattice_first_segment", ".gguf");
  {
    std::ofstream out(model, std::ios::binary);
    out.write("GGUF\x03\x00\x00\x00", 8);
  }
  const auto engine = MakeEngine(NoLimitConfig(), Fixture("lattice"));
  azookey::host::ModelLoadOptions options;
  options.path = model.string();
  options.mock_zenzai_candidates_for_tests = true;
  ASSERT_TRUE(engine->LoadModelWithResult(options).ok);
  engine->SetLatticeFallbackBudgetForTests(std::chrono::hours(1));

  // The mock generates nothing for おきょう: zenzai-degraded, and the lattice of section 5.3.
  const auto degraded = QueryFirstSegment(*engine, "おきょう", "はいいてんきです");
  EXPECT_TRUE(std::any_of(degraded.begin(), degraded.end(), [](const Candidate& c) {
    return c.debug_info.find("zenzai-degraded") != std::string::npos;
  }));
  const auto lattice = LatticeCandidates(degraded);
  ASSERT_GE(lattice.size(), 2U);
  EXPECT_EQ(lattice[0].surface, "お今日");
  EXPECT_EQ(lattice[0].reading, "おきょう");
  EXPECT_EQ(lattice[1].surface, "お京");
  EXPECT_DOUBLE_EQ(lattice[1].score, 0.6);

  // The model answered: no lattice, and the remaining reading changes nothing.
  const auto answered = QueryFirstSegment(*engine, "にほんご", "です");
  ASSERT_FALSE(answered.empty());
  EXPECT_EQ(answered.front().source, CandidateSource::Model);
  EXPECT_TRUE(LatticeCandidates(answered).empty());
  EXPECT_EQ(Describe(answered), Describe(QueryFirstSegment(*engine, "にほんご", "")));
  std::error_code ec;
  std::filesystem::remove(model, ec);
}

// An engine over the lattice_user fixture with the mock model loaded (it answers にほんご and
// じゅうふく, and falls back on the other readings), and the 15 ms of section 5.2 lifted so a
// slow build does not cut the path off by time.
std::unique_ptr<InferenceEngine> MakeModelEngine(const EngineConfig& config,
                                                 const std::filesystem::path& model,
                                                 bool expect_lattice = true) {
  {
    std::ofstream out(model, std::ios::binary);
    out.write("GGUF\x03\x00\x00\x00", 8);
  }
  auto engine = MakeEngine(config, Fixture("lattice_user"), expect_lattice);
  azookey::host::ModelLoadOptions options;
  options.path = model.string();
  options.mock_zenzai_candidates_for_tests = true;
  EXPECT_TRUE(engine->LoadModelWithResult(options).ok);
  engine->SetLatticeFallbackBudgetForTests(std::chrono::hours(1));
  return engine;
}

// M71-B (DEV-2137, user-dictionary-pos-spec sections 5.2 and 5.3): a request the model answered
// gets one path through a conjugated user dictionary word, just below the model's first
// candidate. On the fixture, にほんご costs 4000 as 日本|語 and 14000 through にほん, the
// 撥音便 of ニホむ: T (10000) above, so it is added.
TEST(EngineLattice, AModelAnsweredRequestGetsThePathThroughAConjugatedUserWord) {
  const auto model = UniqueTempPath("azookey_engine_lattice_user_path", ".gguf");
  const auto dictionary = UserDictionaryWith("ニホむ", "にほむ", "verb_godan_ma");
  const auto engine = MakeModelEngine(NoLimitConfig(), model);
  engine->SetUserDictionary(dictionary.get());

  const auto answered = engine->QueryCandidates("にほんご", "", kNow);
  ASSERT_FALSE(answered.empty());
  EXPECT_EQ(answered.front().source, CandidateSource::Model);
  double model_best = 0;
  for (const auto& c : answered)
    if (c.source == CandidateSource::Model) model_best = (std::max)(model_best, c.score);
  const auto paths = UserWordPaths(answered);
  ASSERT_EQ(paths.size(), 1U) << ::testing::PrintToString(Describe(answered));
  EXPECT_EQ(paths[0].surface, "ニホん語");
  EXPECT_EQ(paths[0].reading, "にほんご");
  EXPECT_EQ(paths[0].source, CandidateSource::SystemDictionary);
  EXPECT_DOUBLE_EQ(paths[0].score, (std::max)(0.1, model_best - 0.01));
  // Stage 2 stays off: no N-best of the lattice beside it.
  EXPECT_TRUE(LatticeCandidates(answered).empty());

  // Live conversion (and reconversion, sent as live) gets none.
  EXPECT_TRUE(
      UserWordPaths(engine->QueryCandidatesEx("にほんご", "", kNow, nullptr, 0, true).candidates)
          .empty());
  // A reading no form matches gets none: the model answers じゅうふく too.
  const auto unrelated = engine->QueryCandidates("じゅうふく", "", kNow);
  ASSERT_FALSE(unrelated.empty());
  EXPECT_EQ(unrelated.front().source, CandidateSource::Model);
  EXPECT_TRUE(UserWordPaths(unrelated).empty());
  // A first segment's request is searched as its own reading (segment-editing-spec 5.3).
  const auto first = QueryFirstSegment(*engine, "にほんご", "です");
  const auto first_paths = UserWordPaths(first);
  ASSERT_EQ(first_paths.size(), 1U);
  EXPECT_EQ(first_paths[0].surface, "ニホん語");
  EXPECT_EQ(first_paths[0].reading, "にほんご");
  // A request the model fell back on gets the lattice of section 3.2 instead.
  const auto fallen = engine->QueryCandidates("にほんごです", "", kNow);
  EXPECT_FALSE(LatticeCandidates(fallen).empty());
  EXPECT_TRUE(UserWordPaths(fallen).empty());
  // The user dictionary switched off.
  engine->EnableDictionaryLayer(azookey::learning::LayerId::User, false);
  EXPECT_TRUE(UserWordPaths(engine->QueryCandidates("にほんご", "", kNow)).empty());
  engine->EnableDictionaryLayer(azookey::learning::LayerId::User, true);
  EXPECT_EQ(UserWordPaths(engine->QueryCandidates("にほんご", "", kNow)).size(), 1U);
  // A spent budget stops the search before its forward pass (section 5.4).
  engine->SetLatticeFallbackBudgetForTests(std::chrono::nanoseconds(0));
  EXPECT_TRUE(UserWordPaths(engine->QueryCandidates("にほんご", "", kNow)).empty());
  // A canceled request returns nothing at all.
  engine->SetLatticeFallbackBudgetForTests(std::chrono::hours(1));
  std::atomic<bool> cancel{true};
  EXPECT_TRUE(engine->QueryCandidates("にほんご", "", kNow, &cancel).empty());
  engine->SetUserDictionary(nullptr);
  std::error_code ec;
  std::filesystem::remove(model, ec);
}

// Section 5.2's threshold: through にほん of ニホぬ the path costs one more than through ニホむ
// (the fixture's one costly connection), 10001 above the best path, so it is not added; and
// without lattice_core, or with a noun of the same reading, there is no path.
TEST(EngineLattice, TheConjugatedUserWordPathStaysWithinTOfTheBestPath) {
  const auto model = UniqueTempPath("azookey_engine_lattice_user_threshold", ".gguf");
  const auto over = UserDictionaryWith("ニホぬ", "にほぬ", "verb_godan_na");
  const auto noun = UserDictionaryWith("ニホん", "にほん", std::nullopt);
  const auto at = UserDictionaryWith("ニホむ", "にほむ", "verb_godan_ma");
  const auto engine = MakeModelEngine(NoLimitConfig(), model);
  engine->SetUserDictionary(over.get());
  const auto above = engine->QueryCandidates("にほんご", "", kNow);
  ASSERT_FALSE(above.empty());
  EXPECT_EQ(above.front().source, CandidateSource::Model);
  EXPECT_TRUE(UserWordPaths(above).empty());
  engine->SetUserDictionary(noun.get());
  EXPECT_TRUE(UserWordPaths(engine->QueryCandidates("にほんご", "", kNow)).empty());
  engine->SetUserDictionary(at.get());
  EXPECT_EQ(UserWordPaths(engine->QueryCandidates("にほんご", "", kNow)).size(), 1U);
  engine->SetUserDictionary(nullptr);
  std::error_code ec;
  std::filesystem::remove(model, ec);
}

// Section 5.4: without lattice_core (the lattice off at load leaves it unloaded), no path.
TEST(EngineLattice, NoConjugatedUserWordPathWithoutLatticeCore) {
  const auto model = UniqueTempPath("azookey_engine_lattice_user_no_core", ".gguf");
  const auto dictionary = UserDictionaryWith("ニホむ", "にほむ", "verb_godan_ma");
  const auto engine = MakeModelEngine(NoLimitConfig(false), model);
  engine->SetUserDictionary(dictionary.get());
  const auto plain = engine->QueryCandidates("にほんご", "", kNow);
  ASSERT_FALSE(plain.empty());
  EXPECT_EQ(plain.front().source, CandidateSource::Model);
  EXPECT_TRUE(UserWordPaths(plain).empty());
  engine->SetUserDictionary(nullptr);
  std::error_code ec;
  std::filesystem::remove(model, ec);
}

// The lattice switched off after lattice_core loaded counts as no lattice_core: the path stops
// with stage 1, and comes back when the lattice is switched on again.
TEST(EngineLattice, NoConjugatedUserWordPathWithTheLatticeSwitchedOff) {
  const auto model = UniqueTempPath("azookey_engine_lattice_user_switched_off", ".gguf");
  const auto dictionary = UserDictionaryWith("ニホむ", "にほむ", "verb_godan_ma");
  const auto engine = MakeModelEngine(NoLimitConfig(), model);
  engine->SetUserDictionary(dictionary.get());
  // Precondition: with the lattice on, the path is added.
  ASSERT_EQ(UserWordPaths(engine->QueryCandidates("にほんご", "", kNow)).size(), 1U);
  engine->ApplyConfig(NoLimitConfig(false));
  const auto off = engine->QueryCandidates("にほんご", "", kNow);
  ASSERT_FALSE(off.empty());
  EXPECT_EQ(off.front().source, CandidateSource::Model);
  EXPECT_TRUE(UserWordPaths(off).empty());
  // Stage 1 stops too: a request the model fell back on gets no lattice.
  EXPECT_TRUE(LatticeCandidates(engine->QueryCandidates("にほんごです", "", kNow)).empty());
  engine->ApplyConfig(NoLimitConfig());
  EXPECT_EQ(UserWordPaths(engine->QueryCandidates("にほんご", "", kNow)).size(), 1U);
  engine->SetUserDictionary(nullptr);
  std::error_code ec;
  std::filesystem::remove(model, ec);
}
#endif

}  // namespace
}  // namespace engine_lattice_test
