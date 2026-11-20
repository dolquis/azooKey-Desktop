#include "azookey/core/LatticeSearch.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <queue>
#include <set>
#include <tuple>
#include <unordered_map>
#include <utility>

#include "azookey/core/DoubleArrayTrie.h"
#include "azookey/core/LatticeDictionary.h"

namespace azookey::core {
namespace {

constexpr int32_t kUnreachable = std::numeric_limits<int32_t>::max();
constexpr uint32_t kNoItem = UINT32_MAX;
// Expansions between cancel and deadline checks of the backward search (spec section 6.4).
constexpr uint32_t kStopCheckInterval = 32;
// Complete paths taken out per requested path (spec section 6.2).
constexpr uint64_t kTakesPerPath = 64;

// What the previous word contributes to a segment boundary (spec sections 6.1 and 7.3).
enum class Previous : uint8_t { Other, Prefix, Unknown };
// The state a group of nodes leaves behind: its Previous (under constraints) in the low bits,
// and for a marked_only search whether its paths hold a marked node (user-dictionary-pos-spec
// section 5.2). A search without either keeps 0.
constexpr uint8_t kPreviousMask = 3;
constexpr uint8_t kMarkBit = 4;

Previous PreviousOf(LatticeWordClass word_class) {
  if (word_class == LatticeWordClass::Prefix) return Previous::Prefix;
  if (word_class == LatticeWordClass::Unknown) return Previous::Unknown;
  return Previous::Other;
}

// Section 7.3: a word boundary is a segment boundary when the next word is independent, a
// prefix or unknown, the previous word is not a prefix, and the two are not both unknown.
bool IsSegmentBoundary(Previous previous, LatticeWordClass next) {
  if (next == LatticeWordClass::Function || previous == Previous::Prefix) return false;
  return !(previous == Previous::Unknown && next == LatticeWordClass::Unknown);
}

// Conflict: the position is both broken and joined, so no transition there is allowed.
enum class Rule : uint8_t { None, Break, Join, Conflict };

using Chain = std::vector<uint32_t>;  // Node indices from the reading start.

class Searcher {
 public:
  Searcher(const Lattice& lattice, const ConnectionMatrix& matrix, const LatticeQuery& query)
      : lattice_(lattice), nodes_(lattice.Nodes()), matrix_(matrix), query_(query) {}

  LatticeResult Run();

 private:
  // Nodes that end at one position with one right id (and, under constraints, one previous
  // kind; in a marked_only search, one bit), ordered by forward cost. They share every
  // continuation.
  struct Group {
    uint16_t right_index{};
    uint8_t kind{};    // Previous and kMarkBit.
    uint32_t first{};  // Into group_nodes_.
    uint32_t count{};
  };
  // Predecessors of nodes that begin at one position with one left id and class: groups in
  // order of their best value, each group in order of forward cost.
  struct Row {
    int32_t best{};     // Connection plus the group's smallest forward cost.
    int32_t connect{};  // Connection from the group's right id to the list's left id.
    uint32_t group{};
  };
  struct Item {
    uint32_t node{};
    uint32_t parent{};  // Item of the rest of the path, or kNoItem for the sentence end.
    uint32_t list{};
    uint32_t row{};
    uint32_t column{};
    int32_t carried{};  // Cost of everything after the parent's first word, plus that word.
    int32_t tail{};     // Cost of everything after this item's first word.
    int32_t priority{};
  };
  struct Completed {
    uint32_t item{};
    int32_t cost{};
  };

  bool Allowed(uint8_t kind, const LatticeNode& next, uint32_t position) const;
  // What decides which groups may precede `node`: its left id, its class under constraints, and
  // its bit and mark in a marked_only search.
  uint32_t ListKey(const LatticeNode& node) const;
  int32_t EndCost(uint16_t right_index) const;
  bool Continuations(LatticeResult& result);
  void BuildGroups(uint32_t position);
  bool Forward(LatticeResult& result);
  // The predecessors of `next` at `position`, or of the sentence end when `next` is null.
  uint32_t ListFor(uint32_t position, const LatticeNode* next);
  void Push(uint32_t parent, uint32_t list, uint32_t row, uint32_t column, int32_t carried);
  Chain ChainOf(uint32_t item) const;
  Chain ViterbiChain() const;
  bool Emit(const Chain& chain, int32_t cost, LatticeResult& result);
  bool Flush(std::vector<Completed>& group, LatticeResult& result);
  void Backward(LatticeResult& result);

  const Lattice& lattice_;
  const std::vector<LatticeNode>& nodes_;
  const ConnectionMatrix& matrix_;
  const LatticeQuery& query_;
  uint32_t length_{};  // The searched reading: p for a head_only search.
  uint32_t full_length_{};
  // head_only: B(r) of spec section 7.3 by CMAT right index, the smallest cost from the end of a
  // word with right id r through the rest of the reading to the sentence end.
  std::unordered_map<uint16_t, int32_t> continuation_;
  uint16_t start_right_{};
  uint16_t end_left_{};
  bool constrained_{};
  // A marked_only search over a WithMarkBit lattice (user-dictionary-pos-spec section 5.2).
  bool marked_{};
  // marked_only: the smallest cost of any path, whatever its bit.
  int32_t unconstrained_cost_{kUnreachable};
  std::vector<Rule> rules_;
  std::vector<int32_t> forward_;
  std::vector<uint32_t> back_;
  std::vector<Group> groups_;
  std::vector<uint32_t> group_nodes_;
  std::vector<uint32_t> group_begin_, group_end_;
  uint32_t best_end_{UINT32_MAX};  // Last node of the forward pass's best path.
  int32_t best_cost_{kUnreachable};
  std::vector<std::vector<Row>> lists_;
  std::unordered_map<uint64_t, uint32_t> list_index_;
  std::vector<Item> items_;
  // Smaller priority first; on a tie the newer item, which completes paths sooner.
  std::priority_queue<std::pair<int32_t, uint32_t>, std::vector<std::pair<int32_t, uint32_t>>,
                      std::greater<>>
      heap_;
  std::set<std::string> emitted_;
  bool failed_{};
};

bool Searcher::Allowed(uint8_t kind, const LatticeNode& next, uint32_t position) const {
  // A copy with bit 1 follows a marked path or is marked itself; one with bit 0 neither.
  if (marked_ && next.bit != (((kind & kMarkBit) != 0 || next.marked) ? 1 : 0)) return false;
  if (!constrained_ || rules_[position] == Rule::None) return true;
  if (rules_[position] == Rule::Conflict) return false;
  const bool boundary =
      IsSegmentBoundary(static_cast<Previous>(kind & kPreviousMask), next.word_class);
  return rules_[position] == Rule::Break ? boundary : !boundary;
}

uint32_t Searcher::ListKey(const LatticeNode& node) const {
  uint32_t key = uint32_t{node.left_index} << 4;
  if (constrained_) key |= static_cast<uint32_t>(node.word_class);
  if (marked_) key |= (node.marked ? 4U : 0U) | (node.bit ? 8U : 0U);
  return key;
}

// What follows the last word of a path that ends where the search ends: the sentence end, or
// for a head_only search B(r) in its place (spec section 7.3).
int32_t Searcher::EndCost(uint16_t right_index) const {
  if (!query_.head_only) return matrix_.Cost(right_index, end_left_);
  const auto found = continuation_.find(right_index);
  return found == continuation_.end() ? kUnreachable : found->second;
}

// Section 7.3: B(r) for the right ids of the words that end at p, by a backward pass over the
// rest [p, n) of the reading. The rest has no constraint, so each node beginning at p or later
// takes part, and only the smallest continuation per left id matters to what precedes it.
bool Searcher::Continuations(LatticeResult& result) {
  using Lefts = std::unordered_map<uint16_t, int32_t>;
  std::vector<Lefts> beginning(full_length_ + 1);
  for (auto position = full_length_; position-- > length_;) {
    if (query_.limits.Stopped()) {
      result.status = LatticeStatus::Aborted;
      return false;
    }
    auto& lefts = beginning[position];
    for (const auto index : lattice_.BeginningAt(position)) {
      const auto& node = nodes_[index];
      int32_t rest = kUnreachable;
      if (node.end == full_length_) {
        rest = matrix_.Cost(node.right_index, end_left_);
      } else {
        for (const auto& [left, cost] : beginning[node.end])
          rest = (std::min)(rest, matrix_.Cost(node.right_index, left) + cost);
      }
      if (rest == kUnreachable) continue;
      const auto total = node.cost + rest;
      const auto [slot, inserted] = lefts.emplace(node.left_index, total);
      if (!inserted) slot->second = (std::min)(slot->second, total);
    }
  }
  for (const auto index : lattice_.EndingAt(length_)) {
    const auto right = nodes_[index].right_index;
    if (continuation_.count(right)) continue;
    int32_t best = kUnreachable;
    for (const auto& [left, cost] : beginning[length_])
      best = (std::min)(best, matrix_.Cost(right, left) + cost);
    continuation_.emplace(right, best);
  }
  return true;
}

void Searcher::BuildGroups(uint32_t position) {
  group_begin_[position] = static_cast<uint32_t>(groups_.size());
  std::vector<uint32_t> ending;
  for (const auto index : lattice_.EndingAt(position))
    if (forward_[index] != kUnreachable) ending.push_back(index);
  const auto kind = [&](uint32_t index) {
    const auto& node = nodes_[index];
    return static_cast<uint8_t>(
        (constrained_ ? static_cast<uint8_t>(PreviousOf(node.word_class)) : 0) |
        (node.bit ? kMarkBit : 0));
  };
  std::sort(ending.begin(), ending.end(), [&](uint32_t a, uint32_t b) {
    return std::tuple(nodes_[a].right_index, kind(a), forward_[a], a) <
           std::tuple(nodes_[b].right_index, kind(b), forward_[b], b);
  });
  for (size_t i = 0; i < ending.size(); ++i) {
    const auto index = ending[i];
    if (i == 0 || nodes_[ending[i - 1]].right_index != nodes_[index].right_index ||
        kind(ending[i - 1]) != kind(index))
      groups_.push_back(
          {nodes_[index].right_index, kind(index), static_cast<uint32_t>(group_nodes_.size()), 0});
    group_nodes_.push_back(index);
    ++groups_.back().count;
  }
  group_end_[position] = static_cast<uint32_t>(groups_.size());
}

// Section 6.1: the smallest cost from the sentence start through each node, with the
// constraints applied as transition rules.
bool Searcher::Forward(LatticeResult& result) {
  forward_.assign(nodes_.size(), kUnreachable);
  back_.assign(nodes_.size(), UINT32_MAX);
  group_begin_.assign(length_ + 1, 0);
  group_end_.assign(length_ + 1, 0);
  // Section 7.3: a node across a segment break or a word boundary takes no part.
  std::vector<bool> spans_break(nodes_.size(), false);
  for (const auto* positions :
       {&query_.constraints.segment_breaks, &query_.constraints.word_boundaries})
    for (const auto position : *positions)
      for (uint32_t i = 0; i < nodes_.size(); ++i)
        if (nodes_[i].begin < position && position < nodes_[i].end) spans_break[i] = true;
  std::unordered_map<uint32_t, std::pair<int32_t, uint32_t>> incoming;
  for (uint32_t position = 0; position < length_; ++position) {
    if (query_.limits.Stopped()) {
      result.status = LatticeStatus::Aborted;
      return false;
    }
    if (position > 0) BuildGroups(position);
    incoming.clear();
    for (const auto index : lattice_.BeginningAt(position)) {
      const auto& node = nodes_[index];
      if (spans_break[index]) continue;
      int32_t best = kUnreachable;
      uint32_t from = UINT32_MAX;
      if (position == 0) {
        // The sentence start holds no marked node: a first word's copy has its own mark as bit.
        if (marked_ && node.bit != (node.marked ? 1 : 0)) continue;
        best = matrix_.Cost(start_right_, node.left_index);
      } else {
        const auto key = ListKey(node);
        auto found = incoming.find(key);
        if (found == incoming.end()) {
          std::pair<int32_t, uint32_t> value{kUnreachable, UINT32_MAX};
          for (auto g = group_begin_[position]; g < group_end_[position]; ++g) {
            const auto& group = groups_[g];
            if (!Allowed(group.kind, node, position)) continue;
            const auto first = group_nodes_[group.first];
            const auto cost = forward_[first] + matrix_.Cost(group.right_index, node.left_index);
            if (cost < value.first) value = {cost, first};
          }
          found = incoming.emplace(key, value).first;
        }
        std::tie(best, from) = found->second;
      }
      if (best == kUnreachable) continue;
      forward_[index] = best + node.cost;
      back_[index] = from;
    }
  }
  BuildGroups(length_);
  for (auto g = group_begin_[length_]; g < group_end_[length_]; ++g) {
    const auto first = group_nodes_[groups_[g].first];
    const auto end = EndCost(groups_[g].right_index);
    if (end == kUnreachable) continue;
    const auto cost = forward_[first] + end;
    unconstrained_cost_ = (std::min)(unconstrained_cost_, cost);
    // user-dictionary-pos-spec section 5.2: a marked_only search ends only with bit 1.
    if (marked_ && (groups_[g].kind & kMarkBit) == 0) continue;
    if (cost < best_cost_) {
      best_cost_ = cost;
      best_end_ = first;
    }
  }
  return true;
}

uint32_t Searcher::ListFor(uint32_t position, const LatticeNode* next) {
  const uint64_t key = next ? (uint64_t{position} << 32) | ListKey(*next) : UINT64_MAX;
  const auto found = list_index_.find(key);
  if (found != list_index_.end()) return found->second;
  std::vector<Row> rows;
  for (auto g = group_begin_[position]; g < group_end_[position]; ++g) {
    const auto& group = groups_[g];
    if (next ? !Allowed(group.kind, *next, position)
             : marked_ && (group.kind & kMarkBit) == 0)  // Section 5.2: the end takes bit 1.
      continue;
    const auto connect = next ? int32_t{matrix_.Cost(group.right_index, next->left_index)}
                              : EndCost(group.right_index);
    if (connect == kUnreachable) continue;
    rows.push_back({forward_[group_nodes_[group.first]] + connect, connect, g});
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
    return std::tie(a.best, a.group) < std::tie(b.best, b.group);
  });
  lists_.push_back(std::move(rows));
  const auto index = static_cast<uint32_t>(lists_.size() - 1);
  list_index_.emplace(key, index);
  return index;
}

void Searcher::Push(uint32_t parent, uint32_t list, uint32_t row, uint32_t column,
                    int32_t carried) {
  const auto& entry = lists_[list][row];
  const auto node = group_nodes_[groups_[entry.group].first + column];
  Item item{node, parent, list, row, column, carried, entry.connect + carried, 0};
  item.priority = forward_[node] + item.tail;
  items_.push_back(item);
  heap_.emplace(item.priority, UINT32_MAX - static_cast<uint32_t>(items_.size() - 1));
}

Chain Searcher::ChainOf(uint32_t item) const {
  Chain chain;
  for (auto current = item; current != kNoItem; current = items_[current].parent)
    chain.push_back(items_[current].node);
  return chain;
}

Chain Searcher::ViterbiChain() const {
  Chain chain;
  for (auto current = best_end_; current != UINT32_MAX; current = back_[current])
    chain.push_back(current);
  std::reverse(chain.begin(), chain.end());
  return chain;
}

// Section 7.1 grouping and section 7.2 output. False when a surface is not UTF-8 (spec 3.6).
bool Searcher::Emit(const Chain& chain, int32_t cost, LatticeResult& result) {
  LatticePath path;
  path.cost = cost;
  for (size_t i = 0; i < chain.size(); ++i) {
    const auto& node = nodes_[chain[i]];
    if (!IsValidUtf8(node.surface)) {
      failed_ = true;
      return false;
    }
    const auto index = static_cast<uint32_t>(i);
    if (i == 0 || IsSegmentBoundary(PreviousOf(nodes_[chain[i - 1]].word_class), node.word_class))
      path.segments.push_back({node.begin, node.end, index, 0, kNoLatticeHead});
    auto& segment = path.segments.back();
    segment.end = node.end;
    ++segment.word_count;
    if (segment.head == kNoLatticeHead && (node.word_class == LatticeWordClass::Independent ||
                                           node.word_class == LatticeWordClass::Unknown))
      segment.head = index;
    path.words.push_back({node.begin, node.end, std::string(node.surface), node.left_id,
                          node.right_id, node.pos, node.word_class});
  }
  result.paths.push_back(std::move(path));
  return true;
}

// Section 6.3: complete paths of one cost, ordered by surface bytes, segment boundaries, word
// boundaries, the ids and parts of speech of the words, then their nodes in lattice order (a
// total order, so no standard library picks differently), keep the first of each surface.
bool Searcher::Flush(std::vector<Completed>& group, LatticeResult& result) {
  struct Key {
    std::string surface;
    std::vector<uint32_t> segment_bounds;
    std::vector<uint32_t> word_bounds;
    std::vector<std::tuple<uint16_t, uint16_t, uint16_t>> words;
    Chain chain;
    int32_t cost{};
  };
  std::vector<Key> keys;
  for (const auto& completed : group) {
    Key key;
    key.chain = ChainOf(completed.item);
    key.cost = completed.cost;
    for (size_t i = 0; i < key.chain.size(); ++i) {
      const auto& node = nodes_[key.chain[i]];
      key.surface.append(node.surface);
      key.words.emplace_back(node.left_id, node.right_id, node.pos);
      if (i == 0) continue;
      key.word_bounds.push_back(node.begin);
      if (IsSegmentBoundary(PreviousOf(nodes_[key.chain[i - 1]].word_class), node.word_class))
        key.segment_bounds.push_back(node.begin);
    }
    keys.push_back(std::move(key));
  }
  group.clear();
  std::sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) {
    return std::tie(a.surface, a.segment_bounds, a.word_bounds, a.words, a.chain) <
           std::tie(b.surface, b.segment_bounds, b.word_bounds, b.words, b.chain);
  });
  for (const auto& key : keys) {
    if (result.paths.size() >= query_.max_paths) break;
    if (!emitted_.insert(key.surface).second) continue;
    if (!Emit(key.chain, key.cost, result)) return false;
  }
  return true;
}

// Section 6.2: paths from the sentence end in cost order, with the forward costs as exact
// estimates. Each list is expanded lazily (best child, then siblings), which keeps the order.
void Searcher::Backward(LatticeResult& result) {
  // Bounded so the limits below cannot overflow; no caller asks for this many paths.
  const uint64_t paths = std::min<uint64_t>(query_.max_paths, uint64_t{1} << 20);
  const uint64_t take_limit = kTakesPerPath * paths;
  const uint64_t expansion_limit = take_limit * (uint64_t{length_} + 1);
  const auto end_list = ListFor(length_, nullptr);
  if (!lists_[end_list].empty()) Push(kNoItem, end_list, 0, 0, 0);
  std::vector<Completed> group;
  uint64_t takes = 0, expansions = 0;
  bool settled = false;
  while (true) {
    bool flushed = false;
    if (!group.empty() && (heap_.empty() || heap_.top().first > group.front().cost)) {
      if (!Flush(group, result)) return;
      if (result.paths.size() >= query_.max_paths) return;
      flushed = true;
      if (!settled && !result.paths.empty()) {
        settled = true;
        if (query_.limits.on_stage) query_.limits.on_stage(LatticeStage::Settled);
      }
    }
    if (heap_.empty()) return;
    if (takes >= take_limit || expansions >= expansion_limit) {
      result.limited = true;
      if (!group.empty() && !Flush(group, result)) return;
      if (result.paths.empty()) Emit(ViterbiChain(), best_cost_, result);
      return;
    }
    // Checked every kStopCheckInterval expansions and after each settled group.
    if ((flushed || expansions % kStopCheckInterval == 0) && query_.limits.Stopped()) {
      // Section 6.4: the best path only; the forward pass's when A* has not settled one.
      result.status = LatticeStatus::Truncated;
      if (result.paths.size() > 1) result.paths.resize(1);
      if (result.paths.empty()) Emit(ViterbiChain(), best_cost_, result);
      return;
    }
    const auto index = UINT32_MAX - heap_.top().second;
    heap_.pop();
    ++expansions;
    const auto item = items_[index];
    const auto& group_of = groups_[lists_[item.list][item.row].group];
    if (item.column + 1 < group_of.count)
      Push(item.parent, item.list, item.row, item.column + 1, item.carried);
    if (item.column == 0 && item.row + 1 < lists_[item.list].size())
      Push(item.parent, item.list, item.row + 1, 0, item.carried);
    const auto& node = nodes_[item.node];
    if (node.begin == 0) {
      ++takes;
      group.push_back({index, item.priority});
      continue;
    }
    const auto list = ListFor(node.begin, &node);
    if (!lists_[list].empty()) Push(index, list, 0, 0, node.cost + item.tail);
  }
}

LatticeResult Searcher::Run() {
  LatticeResult result;
  length_ = lattice_.Length();
  if (length_ == 0) {
    result.status = LatticeStatus::Rejected;
    return result;
  }
  start_right_ = matrix_.RightIndex(0);
  end_left_ = matrix_.LeftIndex(0);
  if (start_right_ == kNoConnectionIndex || end_left_ == kNoConnectionIndex) {
    result.status = LatticeStatus::Unavailable;
    return result;
  }
  rules_.assign(length_ + 1, Rule::None);
  const auto mark = [&](const std::vector<uint32_t>& positions, Rule rule) {
    for (const auto position : positions) {
      if (position == 0 || position >= length_) return false;
      rules_[position] =
          rules_[position] == Rule::None || rules_[position] == rule ? rule : Rule::Conflict;
    }
    return true;
  };
  const auto& constraints = query_.constraints;
  const bool boundaries_inside =
      std::all_of(constraints.word_boundaries.begin(), constraints.word_boundaries.end(),
                  [&](uint32_t position) { return position > 0 && position < length_; });
  // Section 7.3: the head of the reading is searched with one word boundary and nothing else.
  const bool head_shape = constraints.word_boundaries.size() == 1 &&
                          constraints.segment_breaks.empty() && constraints.segment_joins.empty();
  // user-dictionary-pos-spec section 5.2 searches the reading as typed, without constraints.
  const bool marked_shape = !query_.marked_only || (constraints.Empty() && !query_.head_only);
  if (!mark(constraints.segment_joins, Rule::Join) ||
      !mark(constraints.segment_breaks, Rule::Break) || !boundaries_inside ||
      (query_.head_only && !head_shape) || !marked_shape) {
    result.status = LatticeStatus::Rejected;
    return result;
  }
  // Word boundaries rule no transition out, so they add no word class to the state (7.3).
  constrained_ = !constraints.segment_breaks.empty() || !constraints.segment_joins.empty();
  marked_ = query_.marked_only;
  if (query_.max_paths == 0) return result;
  full_length_ = length_;
  if (query_.head_only) {
    length_ = constraints.word_boundaries.front();
    if (!Continuations(result)) return result;
  }
  if (!Forward(result)) return result;
  if (marked_ && unconstrained_cost_ != kUnreachable) result.best_cost = unconstrained_cost_;
  if (query_.limits.on_stage) query_.limits.on_stage(LatticeStage::Forward);
  if (best_end_ == UINT32_MAX) return result;
  Backward(result);
  if (failed_) {
    result.status = LatticeStatus::Failed;
    result.paths.clear();
  }
  return result;
}

}  // namespace

std::string LatticePath::Surface() const {
  std::string surface;
  for (const auto& word : words) surface += word.surface;
  return surface;
}

LatticeResult SearchLattice(const Lattice& lattice, const LatticeQuery& query) {
  if (!lattice.Matrix() || !lattice.Matrix()->IsValid()) {
    LatticeResult result;
    result.status = lattice.Matrix() ? LatticeStatus::Unavailable : LatticeStatus::Rejected;
    return result;
  }
  if (query.marked_only) {
    // The copies borrow what `lattice` borrows, which outlives the search.
    const auto marked = WithMarkBit(lattice);
    return Searcher(marked, *marked.Matrix(), query).Run();
  }
  return Searcher(lattice, *lattice.Matrix(), query).Run();
}

LatticeResult ConvertLattice(std::string_view reading, const LatticeDictionary& dictionary,
                             std::span<const ILatticeVocabulary* const> vocabularies,
                             const LatticeQuery& query) {
  LatticeResult result;
  if (!dictionary.IsAvailable()) {
    result.status = LatticeStatus::Unavailable;
    return result;
  }
  std::vector<const ILatticeVocabulary*> all{&dictionary.Vocabulary()};
  all.insert(all.end(), vocabularies.begin(), vocabularies.end());
  Lattice lattice;
  const auto built = BuildLattice(reading, dictionary.Parameters(), all, query.limits, lattice);
  if (built.status != LatticeStatus::Complete) {
    result.status = built.status;
    result.rejected_entries = built.rejected_entries;
    return result;
  }
  result = SearchLattice(lattice, query);
  result.rejected_entries = built.rejected_entries;
  return result;
}

}  // namespace azookey::core
