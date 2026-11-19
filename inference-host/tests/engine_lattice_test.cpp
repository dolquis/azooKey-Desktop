// M65 stage 1 (docs/lattice-conversion-spec.md sections 3.2 to 3.6): the engine adds the
// lattice's N-best on requests that do not go through the model, with the dictbuild fixture
// lattice/ (今日|は|いい|天気|です is its best path).
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "azookey/core/SimpleConverter.h"
#include "azookey/host/InferenceEngine.h"

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

TEST(EngineLattice, SafeModeConvertsWithoutTheModelAndGetsTheLattice) {
  const auto engine = MakeEngine(NoLimitConfig(), Fixture("lattice"));
  engine->RestoreHealthState(azookey::host::HealthState::SafeMode);
  azookey::host::ModelLoadOptions options;
  options.path =
      (std::filesystem::temp_directory_path() / "azookey_engine_lattice_safe.gguf").string();
  // SafeMode refuses the load before reading the file, so the fallback converter stays active.
  ASSERT_FALSE(engine->LoadModelWithResult(options).ok);
  const auto lattice = LatticeCandidates(engine->QueryCandidates(kSentence, "", kNow));
  ASSERT_FALSE(lattice.empty());
  EXPECT_EQ(lattice.front().surface, "今日はいい天気です");
}

#if !AZOOKEY_WITH_LLAMA_CPP
// Section 3.2: a request the model converter fell back on gets the lattice; one it answered
// does not. The minimal GGUF loads only with the mock runtime (no real llama.cpp).
TEST(EngineLattice, ModelFallbacksGetTheLatticeAndAnsweredRequestsDoNot) {
  const auto model = std::filesystem::temp_directory_path() / "azookey_engine_lattice_model.gguf";
  {
    std::ofstream out(model, std::ios::binary);
    out.write("GGUF\x03\x00\x00\x00", 8);
  }
  const auto engine = MakeEngine(NoLimitConfig(), Fixture("lattice"));
  azookey::host::ModelLoadOptions options;
  options.path = model.string();
  options.mock_zenzai_candidates_for_tests = true;
  ASSERT_TRUE(engine->LoadModelWithResult(options).ok);

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
  std::error_code ec;
  std::filesystem::remove(model, ec);
}
#endif

}  // namespace
}  // namespace engine_lattice_test
