// Lattice building and search on in-memory vocabularies and matrices
// (docs/lattice-conversion-spec.md sections 4 to 7 and 11).
#include "azookey/core/Lattice.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "azookey/core/LatticeConnection.h"
#include "azookey/core/LatticeSearch.h"

namespace {
using namespace azookey::core;

// A CMAT section whose ids 0..count-1 all have indices equal to the id.
class TestMatrix {
 public:
  TestMatrix(uint32_t count, const std::function<int16_t(uint32_t, uint32_t)>& cost) {
    const auto u16 = [&](uint32_t value) {
      bytes_.push_back(static_cast<char>(value & 0xff));
      bytes_.push_back(static_cast<char>((value >> 8) & 0xff));
    };
    const auto u32 = [&](uint32_t value) {
      u16(value & 0xffff);
      u16(value >> 16);
    };
    for (const auto value : {count, count, count, count, 0U, 0U}) u32(value);
    for (int table = 0; table < 2; ++table)
      for (uint32_t id = 0; id < count; ++id) u16(id);
    while (bytes_.size() % 8) bytes_.push_back('\0');
    for (uint32_t right = 0; right < count; ++right)
      for (uint32_t left = 0; left < count; ++left) u16(static_cast<uint16_t>(cost(right, left)));
    std::string error;
    parsed_ = matrix_.Parse(bytes_, error);
  }
  TestMatrix(const TestMatrix&) = delete;
  TestMatrix& operator=(const TestMatrix&) = delete;
  bool Parsed() const { return parsed_; }
  const ConnectionMatrix& Matrix() const { return matrix_; }

 private:
  std::string bytes_;
  ConnectionMatrix matrix_;
  bool parsed_{};
};

struct Word {
  std::string reading;
  std::string surface;
  uint16_t left_id{};
  uint16_t right_id{};
  int16_t cost{};
  LatticeWordClass word_class{LatticeWordClass::Independent};
  LatticeScript script{LatticeScript::Other};
  uint16_t pos{kNoLatticePos};
};

class WordVocabulary : public ILatticeVocabulary {
 public:
  explicit WordVocabulary(std::vector<Word> words) : words_(std::move(words)) {}
  bool CommonPrefix(std::string_view key, std::vector<LatticeEntry>& out) const override {
    if (fail_) return false;
    for (const auto& word : words_) {
      if (key.substr(0, word.reading.size()) != word.reading) continue;
      out.push_back({static_cast<uint32_t>(word.reading.size()), word.surface, word.left_id,
                     word.right_id, word.cost, word.pos, word.word_class, word.script});
    }
    return true;
  }
  void Fail() { fail_ = true; }

 private:
  std::vector<Word> words_;
  bool fail_{};
};

LatticePenalties NoPenalties() { return {0, 0, 0, 0}; }

LatticeParameters Parameters(const TestMatrix& matrix, LatticePenalties penalties) {
  return {&matrix.Matrix(), penalties, 1, 1};
}

// Everything the tests compare about a path.
struct Summary {
  int32_t cost{};
  std::string surface;
  std::vector<uint32_t> segment_bounds;
  bool operator==(const Summary&) const = default;
};

std::ostream& operator<<(std::ostream& out, const Summary& summary) {
  out << summary.cost << " " << summary.surface << " [";
  for (const auto bound : summary.segment_bounds) out << bound << " ";
  return out << "]";
}

Summary Summarize(const LatticePath& path) {
  Summary summary{path.cost, path.Surface(), {}};
  for (size_t i = 1; i < path.segments.size(); ++i)
    summary.segment_bounds.push_back(path.segments[i].begin);
  return summary;
}

std::vector<Summary> Summaries(const LatticeResult& result) {
  std::vector<Summary> summaries;
  for (const auto& path : result.paths) summaries.push_back(Summarize(path));
  return summaries;
}

bool IsBoundary(LatticeWordClass previous, LatticeWordClass next) {
  if (next == LatticeWordClass::Function || previous == LatticeWordClass::Prefix) return false;
  return !(previous == LatticeWordClass::Unknown && next == LatticeWordClass::Unknown);
}

// Every BOS-to-EOS path, with the constraints of spec section 7.3 applied literally, ordered
// as spec section 6.3 orders them and folded by surface.
struct Exhaustive {
  std::vector<Summary> ranked;
  size_t paths{};
};

Exhaustive Enumerate(const Lattice& lattice, const ConnectionMatrix& matrix,
                     const LatticeConstraints& constraints = {}) {
  struct Full {
    Summary summary;
    std::vector<uint32_t> word_bounds;
    std::vector<std::tuple<uint16_t, uint16_t, uint16_t>> ids;
  };
  std::vector<Full> all;
  const auto& nodes = lattice.Nodes();
  const std::set<uint32_t> breaks(constraints.segment_breaks.begin(),
                                  constraints.segment_breaks.end());
  const std::set<uint32_t> joins(constraints.segment_joins.begin(),
                                 constraints.segment_joins.end());
  std::vector<uint32_t> path;
  std::function<void(uint32_t)> walk = [&](uint32_t position) {
    if (position == lattice.Length()) {
      Full full;
      int32_t cost = 0;
      uint16_t right = matrix.RightIndex(0);
      for (size_t i = 0; i < path.size(); ++i) {
        const auto& node = nodes[path[i]];
        cost += matrix.Cost(right, node.left_index) + node.cost;
        right = node.right_index;
        full.summary.surface.append(node.surface);
        full.ids.emplace_back(node.left_id, node.right_id, node.pos);
        if (i == 0) continue;
        const bool boundary = IsBoundary(nodes[path[i - 1]].word_class, node.word_class);
        full.word_bounds.push_back(node.begin);
        if (boundary) full.summary.segment_bounds.push_back(node.begin);
        if ((breaks.count(node.begin) && !boundary) || (joins.count(node.begin) && boundary))
          return;
      }
      for (const auto position_break : breaks) {
        if (std::find(full.word_bounds.begin(), full.word_bounds.end(), position_break) ==
            full.word_bounds.end())
          return;
      }
      full.summary.cost = cost + matrix.Cost(right, matrix.LeftIndex(0));
      all.push_back(std::move(full));
      return;
    }
    for (const auto index : lattice.BeginningAt(position)) {
      path.push_back(index);
      walk(nodes[index].end);
      path.pop_back();
    }
  };
  walk(0);
  std::sort(all.begin(), all.end(), [](const Full& a, const Full& b) {
    return std::tie(a.summary.cost, a.summary.surface, a.summary.segment_bounds, a.word_bounds,
                    a.ids) < std::tie(b.summary.cost, b.summary.surface, b.summary.segment_bounds,
                                      b.word_bounds, b.ids);
  });
  Exhaustive result;
  result.paths = all.size();
  std::set<std::string> seen;
  for (const auto& full : all)
    if (seen.insert(full.summary.surface).second) result.ranked.push_back(full.summary);
  return result;
}

// xorshift32: the same sequence on every standard library.
class Random {
 public:
  explicit Random(uint32_t seed) : state_(seed ? seed : 1) {}
  uint32_t Next(uint32_t bound) {
    state_ ^= state_ << 13;
    state_ ^= state_ >> 17;
    state_ ^= state_ << 5;
    return state_ % bound;
  }

 private:
  uint32_t state_;
};

constexpr const char* kKana[] = {"あ", "い", "う"};
constexpr const char* kSurfaces[] = {"甲", "乙", "丙", "丁", "甲乙"};
constexpr LatticeWordClass kClasses[] = {LatticeWordClass::Independent, LatticeWordClass::Function,
                                         LatticeWordClass::Prefix};

std::string RandomKana(Random& random, uint32_t length) {
  std::string text;
  for (uint32_t i = 0; i < length; ++i) text += kKana[random.Next(3)];
  return text;
}

std::vector<Word> RandomWords(Random& random) {
  std::vector<Word> words;
  const auto count = 6 + random.Next(6);
  for (uint32_t i = 0; i < count; ++i) {
    Word word;
    word.reading = RandomKana(random, 1 + random.Next(3));
    word.surface = kSurfaces[random.Next(5)];
    word.left_id = static_cast<uint16_t>(1 + random.Next(5));
    word.right_id = static_cast<uint16_t>(1 + random.Next(5));
    word.cost = static_cast<int16_t>(random.Next(4) * 500);
    word.word_class = kClasses[random.Next(3)];
    words.push_back(word);
  }
  return words;
}

std::unique_ptr<TestMatrix> RandomMatrix(Random& random) {
  std::vector<int16_t> costs(36);
  for (auto& cost : costs) cost = static_cast<int16_t>(random.Next(5) * 250);
  return std::make_unique<TestMatrix>(
      6, [costs](uint32_t right, uint32_t left) { return costs[right * 6 + left]; });
}

LatticeResult Search(const Lattice& lattice, const TestMatrix& matrix, size_t max_paths,
                     LatticeConstraints constraints = {}) {
  LatticeQuery query;
  query.max_paths = max_paths;
  query.constraints = std::move(constraints);
  EXPECT_EQ(lattice.Matrix(), &matrix.Matrix());
  return SearchLattice(lattice, query);
}

TEST(LatticeSearch, BestAndNBestMatchExhaustiveEnumeration) {
  size_t compared = 0;
  for (uint32_t seed = 1; seed <= 120; ++seed) {
    Random random(seed);
    const auto matrix = RandomMatrix(random);
    ASSERT_TRUE(matrix->Parsed());
    WordVocabulary vocabulary(RandomWords(random));
    const ILatticeVocabulary* vocabularies[] = {&vocabulary};
    const auto reading = RandomKana(random, 2 + random.Next(4));
    // Small penalties keep unknown-word nodes in the running.
    Lattice lattice;
    ASSERT_EQ(
        BuildLattice(reading, Parameters(*matrix, {0, 0, 100, 900}), vocabularies, {}, lattice)
            .status,
        LatticeStatus::Complete);
    const auto expected = Enumerate(lattice, matrix->Matrix());
    ASSERT_FALSE(expected.ranked.empty());
    const auto best = Search(lattice, *matrix, 1);
    ASSERT_EQ(best.status, LatticeStatus::Complete);
    ASSERT_EQ(best.paths.size(), 1U);
    EXPECT_EQ(best.paths[0].cost, expected.ranked[0].cost) << "seed " << seed;
    const auto nbest = Search(lattice, *matrix, 5);
    if (nbest.limited) continue;
    ++compared;
    const std::vector<Summary> top(
        expected.ranked.begin(),
        expected.ranked.begin() + std::min<size_t>(5, expected.ranked.size()));
    EXPECT_EQ(Summaries(nbest), top) << "seed " << seed;
  }
  EXPECT_GE(compared, 100U);
}

TEST(LatticeSearch, ConstraintsMatchExhaustiveEnumeration) {
  size_t compared = 0;
  for (uint32_t seed = 1; seed <= 120; ++seed) {
    Random random(seed * 7919);
    const auto matrix = RandomMatrix(random);
    WordVocabulary vocabulary(RandomWords(random));
    const ILatticeVocabulary* vocabularies[] = {&vocabulary};
    const auto length = 3 + random.Next(3);
    const auto reading = RandomKana(random, length);
    Lattice lattice;
    ASSERT_EQ(
        BuildLattice(reading, Parameters(*matrix, {0, 0, 100, 900}), vocabularies, {}, lattice)
            .status,
        LatticeStatus::Complete);
    LatticeConstraints constraints;
    const auto first = 1 + random.Next(length - 1);
    const auto second = 1 + random.Next(length - 1);
    constraints.segment_breaks.push_back(first);
    if (second != first) constraints.segment_joins.push_back(second);
    const auto expected = Enumerate(lattice, matrix->Matrix(), constraints);
    const auto result = Search(lattice, *matrix, 4, constraints);
    ASSERT_EQ(result.status, LatticeStatus::Complete);
    if (result.limited) continue;
    ++compared;
    const std::vector<Summary> top(
        expected.ranked.begin(),
        expected.ranked.begin() + std::min<size_t>(4, expected.ranked.size()));
    EXPECT_EQ(Summaries(result), top) << "seed " << seed;
    for (const auto& path : result.paths) {
      std::vector<uint32_t> bounds;
      for (size_t i = 1; i < path.segments.size(); ++i) bounds.push_back(path.segments[i].begin);
      EXPECT_NE(std::find(bounds.begin(), bounds.end(), first), bounds.end()) << "seed " << seed;
      if (second != first)
        EXPECT_EQ(std::find(bounds.begin(), bounds.end(), second), bounds.end()) << "seed " << seed;
    }
  }
  EXPECT_GE(compared, 100U);
}

// Section 6.3: equal costs are ordered by surface bytes; one surface keeps its smallest
// segment-boundary list.
TEST(LatticeSearch, TiesAreOrderedBySurfaceThenBoundaries) {
  TestMatrix matrix(4, [](uint32_t, uint32_t) { return int16_t{0}; });
  WordVocabulary vocabulary({{"あ", "A", 2, 2}, {"い", "I", 2, 2}, {"あい", "AI", 2, 2}});
  const ILatticeVocabulary* vocabularies[] = {&vocabulary};
  Lattice lattice;
  ASSERT_EQ(
      BuildLattice("あい", Parameters(matrix, NoPenalties()), vocabularies, {}, lattice).status,
      LatticeStatus::Complete);
  const auto result = Search(lattice, matrix, 8);
  ASSERT_EQ(result.status, LatticeStatus::Complete);
  const std::vector<Summary> expected = {
      {0, "AI", {}}, {0, "Aい", {1}}, {0, "あI", {1}}, {0, "あい", {}}};
  EXPECT_EQ(Summaries(result), expected);
  ASSERT_EQ(result.paths.size(), 4U);
  // [AI] and [A][I] share the surface; the one without a boundary is kept.
  EXPECT_EQ(result.paths[0].words.size(), 1U);
  // Unknown-word nodes in a row stay in one segment (section 7.1).
  EXPECT_EQ(result.paths[3].segments.size(), 1U);
  // The same input gives the same order.
  EXPECT_EQ(Summaries(Search(lattice, matrix, 8)), expected);
}

TEST(LatticeSearch, SegmentsAttachFunctionWordsAndPrefixes) {
  TestMatrix matrix(8, [](uint32_t, uint32_t) { return int16_t{0}; });
  WordVocabulary vocabulary({
      {"わたし", "私", 2, 2, 0},
      {"は", "は", 3, 3, 0, LatticeWordClass::Function},
      {"お", "お", 4, 4, 0, LatticeWordClass::Prefix},
      {"ちゃ", "茶", 2, 2, 0},
      {"を", "を", 3, 3, 0, LatticeWordClass::Function},
  });
  const ILatticeVocabulary* vocabularies[] = {&vocabulary};
  const auto run = [&](std::string_view reading) {
    Lattice lattice;
    EXPECT_EQ(BuildLattice(reading, Parameters(matrix, {0, 0, 0, 10000}), vocabularies, {}, lattice)
                  .status,
              LatticeStatus::Complete);
    auto result = Search(lattice, matrix, 1);
    EXPECT_EQ(result.paths.size(), 1U);
    return result.paths.empty() ? LatticePath{} : result.paths[0];
  };
  const auto sentence = run("わたしはおちゃを");
  EXPECT_EQ(sentence.Surface(), "私はお茶を");
  ASSERT_EQ(sentence.segments.size(), 2U);
  EXPECT_EQ(sentence.segments[0].begin, 0U);
  EXPECT_EQ(sentence.segments[0].end, 4U);
  EXPECT_EQ(sentence.segments[0].word_count, 2U);
  EXPECT_EQ(sentence.segments[0].head, 0U);
  EXPECT_EQ(sentence.segments[1].begin, 4U);
  EXPECT_EQ(sentence.segments[1].first_word, 2U);
  EXPECT_EQ(sentence.segments[1].word_count, 3U);
  EXPECT_EQ(sentence.segments[1].head, 3U);  // 茶: the prefix is skipped.
  // A path that starts with a function word: that segment has no head (section 7.1).
  const auto leading = run("はわたし");
  ASSERT_EQ(leading.segments.size(), 2U);
  EXPECT_EQ(leading.segments[0].head, kNoLatticeHead);
  EXPECT_EQ(leading.segments[1].head, 1U);
}

TEST(LatticeSearch, UnknownOnlyReadingStillHasAPath) {
  TestMatrix matrix(2, [](uint32_t, uint32_t) { return int16_t{5}; });
  Lattice lattice;
  ASSERT_EQ(
      BuildLattice("ぬぬぬ", Parameters(matrix, {6000, 0, 3000, 20000}), {}, {}, lattice).status,
      LatticeStatus::Complete);
  const auto result = Search(lattice, matrix, 8);
  ASSERT_EQ(result.status, LatticeStatus::Complete);
  ASSERT_EQ(result.paths.size(), 1U);
  EXPECT_EQ(result.paths[0].Surface(), "ぬぬぬ");
  EXPECT_EQ(result.paths[0].cost, 3 * 23000 + 4 * 5);
  ASSERT_EQ(result.paths[0].segments.size(), 1U);
  EXPECT_EQ(result.paths[0].segments[0].head, 0U);
  EXPECT_EQ(result.paths[0].words[0].word_class, LatticeWordClass::Unknown);
}

TEST(LatticeBuild, PenaltiesAndFoldingAcrossVocabularies) {
  TestMatrix matrix(4, [](uint32_t, uint32_t) { return int16_t{0}; });
  WordVocabulary first(
      {{"か", "カ", 2, 2, 100, LatticeWordClass::Independent, LatticeScript::KatakanaVariant},
       {"か", "蚊", 2, 2, 500}});
  WordVocabulary second(
      {{"か", "カ", 2, 2, 50, LatticeWordClass::Independent, LatticeScript::KatakanaVariant},
       {"か", "カ", 2, 2, 10, LatticeWordClass::Independent, LatticeScript::KatakanaOwn},
       {"か", "蚊", 2, 2, 700}});
  const ILatticeVocabulary* vocabularies[] = {&first, &second};
  Lattice lattice;
  ASSERT_EQ(
      BuildLattice("か", Parameters(matrix, {6000, 40, 3000, 20000}), vocabularies, {}, lattice)
          .status,
      LatticeStatus::Complete);
  std::vector<std::tuple<std::string, int32_t, uint32_t>> nodes;
  for (const auto& node : lattice.Nodes())
    nodes.emplace_back(std::string(node.surface), node.cost, node.source);
  // Same word, ids and spelling: the cheaper one survives in vocabulary order. Another
  // spelling kind stays a separate node. The unknown-word node comes last.
  const std::vector<std::tuple<std::string, int32_t, uint32_t>> expected = {
      {"蚊", 3500, 0}, {"カ", 9050, 1}, {"カ", 3050, 1}, {"か", 23000, kLatticeUnknownSource}};
  EXPECT_EQ(nodes, expected);
}

TEST(LatticeBuild, RejectsBadInputAndEntries) {
  TestMatrix matrix(4, [](uint32_t, uint32_t) { return int16_t{0}; });
  WordVocabulary vocabulary({{"あ", "A", 2, 2}, {"あ", "B", 9, 2}, {"あ", "", 2, 2}});
  const ILatticeVocabulary* vocabularies[] = {&vocabulary};
  Lattice lattice;
  const auto parameters = Parameters(matrix, NoPenalties());
  EXPECT_EQ(BuildLattice("", parameters, vocabularies, {}, lattice).status,
            LatticeStatus::Rejected);
  EXPECT_EQ(BuildLattice("\xff", parameters, vocabularies, {}, lattice).status,
            LatticeStatus::Rejected);
  std::string long_reading;
  for (uint32_t i = 0; i <= kMaxLatticeReading; ++i) long_reading += "あ";
  EXPECT_EQ(BuildLattice(long_reading, parameters, vocabularies, {}, lattice).status,
            LatticeStatus::Rejected);
  EXPECT_EQ(BuildLattice(long_reading.substr(3), parameters, vocabularies, {}, lattice).status,
            LatticeStatus::Complete);
  // Id 9 is outside the matrix and the empty surface has nothing to show.
  const auto built = BuildLattice("あ", parameters, vocabularies, {}, lattice);
  EXPECT_EQ(built.status, LatticeStatus::Complete);
  EXPECT_EQ(built.rejected_entries, 2U);
  EXPECT_EQ(lattice.Nodes().size(), 2U);
  // A reading length that ends inside a character.
  WordVocabulary split({{"\xe3\x81", "X", 2, 2}});
  const ILatticeVocabulary* split_vocabularies[] = {&split};
  EXPECT_EQ(BuildLattice("あ", parameters, split_vocabularies, {}, lattice).rejected_entries, 1U);

  ASSERT_EQ(BuildLattice("あい", parameters, vocabularies, {}, lattice).status,
            LatticeStatus::Complete);
  LatticeConstraints outside;
  outside.segment_breaks = {2};
  EXPECT_EQ(Search(lattice, matrix, 4, outside).status, LatticeStatus::Rejected);
  LatticeConstraints zero;
  zero.segment_joins = {0};
  EXPECT_EQ(Search(lattice, matrix, 4, zero).status, LatticeStatus::Rejected);
  EXPECT_TRUE(Search(lattice, matrix, 0).paths.empty());
  // One position both broken and joined: no path satisfies both.
  LatticeConstraints conflict;
  conflict.segment_breaks = {1};
  conflict.segment_joins = {1};
  const auto none = Search(lattice, matrix, 4, conflict);
  EXPECT_EQ(none.status, LatticeStatus::Complete);
  EXPECT_TRUE(none.paths.empty());
  LatticeConstraints only_break;
  only_break.segment_breaks = {1};
  EXPECT_FALSE(Search(lattice, matrix, 4, only_break).paths.empty());
  // A lattice that was never built has no matrix to search with.
  Lattice unbuilt;
  EXPECT_EQ(SearchLattice(unbuilt, LatticeQuery{}).status, LatticeStatus::Rejected);
}

TEST(LatticeBuild, VocabularyFailureAndInvalidSurfaceGiveNoPaths) {
  TestMatrix matrix(4, [](uint32_t, uint32_t) { return int16_t{0}; });
  WordVocabulary failing({{"あ", "A", 2, 2}});
  failing.Fail();
  const ILatticeVocabulary* vocabularies[] = {&failing};
  Lattice lattice;
  EXPECT_EQ(BuildLattice("あ", Parameters(matrix, NoPenalties()), vocabularies, {}, lattice).status,
            LatticeStatus::Failed);
  WordVocabulary broken({{"あ", "\xc3", 2, 2, -100}});
  const ILatticeVocabulary* broken_vocabularies[] = {&broken};
  ASSERT_EQ(BuildLattice("あ", Parameters(matrix, NoPenalties()), broken_vocabularies, {}, lattice)
                .status,
            LatticeStatus::Complete);
  const auto result = Search(lattice, matrix, 4);
  EXPECT_EQ(result.status, LatticeStatus::Failed);
  EXPECT_TRUE(result.paths.empty());
}

// Section 6.2: at most 64 x N complete paths are taken out.
TEST(LatticeSearch, TakesAtMostSixtyFourCompletePathsPerRequestedPath) {
  TestMatrix matrix(300, [](uint32_t, uint32_t) { return int16_t{0}; });
  std::vector<Word> words;
  for (uint16_t id = 1; id <= 200; ++id) words.push_back({"あ", "X", id, id, int16_t(id)});
  words.push_back({"あ", "Y", 1, 1, 1000});
  WordVocabulary vocabulary(words);
  const ILatticeVocabulary* vocabularies[] = {&vocabulary};
  Lattice lattice;
  ASSERT_EQ(
      BuildLattice("あ", Parameters(matrix, {0, 0, 0, 30000}), vocabularies, {}, lattice).status,
      LatticeStatus::Complete);
  const auto limited = Search(lattice, matrix, 2);
  EXPECT_EQ(limited.status, LatticeStatus::Complete);
  EXPECT_TRUE(limited.limited);
  ASSERT_EQ(limited.paths.size(), 1U);
  EXPECT_EQ(limited.paths[0].Surface(), "X");
  const auto enough = Search(lattice, matrix, 4);
  EXPECT_FALSE(enough.limited);
  ASSERT_EQ(enough.paths.size(), 3U);
  EXPECT_EQ(enough.paths[1].Surface(), "Y");
}

// Section 6.4: a stop before the forward pass ends gives nothing; after it, the best path only.
TEST(LatticeSearch, CancelAndDeadlineFollowSection64) {
  TestMatrix matrix(4, [](uint32_t right, uint32_t left) {
    return static_cast<int16_t>((right * 3 + left) % 4);
  });
  WordVocabulary vocabulary({{"あ", "A", 2, 2, 10},
                             {"あ", "B", 3, 3, 20},
                             {"い", "I", 2, 3, 10},
                             {"あい", "AI", 3, 2, 30}});
  const ILatticeVocabulary* vocabularies[] = {&vocabulary};
  const auto parameters = Parameters(matrix, {0, 0, 10, 200});
  Lattice lattice;
  ASSERT_EQ(BuildLattice("あいあい", parameters, vocabularies, {}, lattice).status,
            LatticeStatus::Complete);
  const auto full = Search(lattice, matrix, 4);
  ASSERT_EQ(full.paths.size(), 4U);

  std::atomic<bool> cancel{true};
  LatticeLimits cancelled;
  cancelled.cancel = &cancel;
  EXPECT_EQ(BuildLattice("あいあい", parameters, vocabularies, cancelled, lattice).status,
            LatticeStatus::Aborted);
  LatticeLimits late;
  late.deadline = std::chrono::steady_clock::now() - std::chrono::seconds(1);
  EXPECT_EQ(BuildLattice("あいあい", parameters, vocabularies, late, lattice).status,
            LatticeStatus::Aborted);

  ASSERT_EQ(BuildLattice("あいあい", parameters, vocabularies, {}, lattice).status,
            LatticeStatus::Complete);
  const auto stop_at = [&](LatticeStage stage) {
    std::atomic<bool> flag{false};
    LatticeQuery query;
    query.max_paths = 4;
    query.limits.cancel = &flag;
    query.limits.on_stage = [&, stage](LatticeStage reached) {
      if (reached == stage) flag = true;
    };
    // The Built stage is reported by BuildLattice, so it is replayed here before searching.
    if (stage == LatticeStage::Built) flag = true;
    return SearchLattice(lattice, query);
  };
  const auto before = stop_at(LatticeStage::Built);
  EXPECT_EQ(before.status, LatticeStatus::Aborted);
  EXPECT_TRUE(before.paths.empty());
  // After the forward pass but before A* settles a path: the forward pass's best path.
  const auto after = stop_at(LatticeStage::Forward);
  EXPECT_EQ(after.status, LatticeStatus::Truncated);
  ASSERT_EQ(after.paths.size(), 1U);
  EXPECT_EQ(after.paths[0].cost, full.paths[0].cost);
  // After A* settles its first path: that path, and nothing after it.
  const auto settled = stop_at(LatticeStage::Settled);
  EXPECT_EQ(settled.status, LatticeStatus::Truncated);
  ASSERT_EQ(settled.paths.size(), 1U);
  EXPECT_EQ(settled.paths[0].Surface(), full.paths[0].Surface());
  EXPECT_EQ(settled.paths[0].cost, full.paths[0].cost);
  // A deadline that passes after the forward pass truncates the same way as cancel.
  LatticeQuery timed;
  timed.max_paths = 4;
  timed.limits.on_stage = [&timed](LatticeStage reached) {
    if (reached == LatticeStage::Forward)
      timed.limits.deadline = std::chrono::steady_clock::now() - std::chrono::seconds(1);
  };
  const auto expired = SearchLattice(lattice, timed);
  EXPECT_EQ(expired.status, LatticeStatus::Truncated);
  ASSERT_EQ(expired.paths.size(), 1U);
  EXPECT_EQ(expired.paths[0].cost, full.paths[0].cost);

  // BuildLattice reports Built after the lattice is complete.
  std::atomic<bool> flag{false};
  LatticeLimits staged;
  staged.cancel = &flag;
  staged.on_stage = [&](LatticeStage reached) {
    if (reached == LatticeStage::Built) flag = true;
  };
  EXPECT_EQ(BuildLattice("あいあい", parameters, vocabularies, staged, lattice).status,
            LatticeStatus::Complete);
  EXPECT_TRUE(flag.load());
}

}  // namespace
