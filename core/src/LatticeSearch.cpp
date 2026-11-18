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
  // kind), ordered by forward cost. They share every continuation.
  struct Group {
    uint16_t right_index{};
    Previous kind{};
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

  bool Allowed(Previous previous, LatticeWordClass next, uint32_t position) const;
  uint32_t ClassKey(LatticeWordClass word_class) const;
  void BuildGroups(uint32_t position);
  bool Forward(LatticeResult& result);
  uint32_t ListFor(uint32_t position, uint16_t left_index, LatticeWordClass word_class, bool end);
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
  uint32_t length_{};
  uint16_t start_right_{};
  uint16_t end_left_{};
  bool constrained_{};
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

bool Searcher::Allowed(Previous previous, LatticeWordClass next, uint32_t position) const {
  if (!constrained_ || rules_[position] == Rule::None) return true;
  if (rules_[position] == Rule::Conflict) return false;
  const bool boundary = IsSegmentBoundary(previous, next);
  return rules_[position] == Rule::Break ? boundary : !boundary;
}

uint32_t Searcher::ClassKey(LatticeWordClass word_class) const {
  return constrained_ ? static_cast<uint32_t>(word_class) : 0;
}

void Searcher::BuildGroups(uint32_t position) {
  group_begin_[position] = static_cast<uint32_t>(groups_.size());
  std::vector<uint32_t> ending;
  for (const auto index : lattice_.EndingAt(position))
    if (forward_[index] != kUnreachable) ending.push_back(index);
  const auto kind = [&](uint32_t index) {
    return constrained_ ? PreviousOf(nodes_[index].word_class) : Previous::Other;
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
  std::vector<bool> spans_break(nodes_.size(), false);
  for (const auto position : query_.constraints.segment_breaks)
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
        best = matrix_.Cost(start_right_, node.left_index);
      } else {
        const auto key = (uint32_t{node.left_index} << 2) | ClassKey(node.word_class);
        auto found = incoming.find(key);
        if (found == incoming.end()) {
          std::pair<int32_t, uint32_t> value{kUnreachable, UINT32_MAX};
          for (auto g = group_begin_[position]; g < group_end_[position]; ++g) {
            const auto& group = groups_[g];
            if (!Allowed(group.kind, node.word_class, position)) continue;
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
    const auto cost = forward_[first] + matrix_.Cost(groups_[g].right_index, end_left_);
    if (cost < best_cost_) {
      best_cost_ = cost;
      best_end_ = first;
    }
  }
  return true;
}

uint32_t Searcher::ListFor(uint32_t position, uint16_t left_index, LatticeWordClass word_class,
                           bool end) {
  const uint64_t key =
      end ? UINT64_MAX
          : (uint64_t{position} << 32) | (uint64_t{left_index} << 2) | ClassKey(word_class);
  const auto found = list_index_.find(key);
  if (found != list_index_.end()) return found->second;
  std::vector<Row> rows;
  for (auto g = group_begin_[position]; g < group_end_[position]; ++g) {
    const auto& group = groups_[g];
    if (!end && !Allowed(group.kind, word_class, position)) continue;
    const auto connect = int32_t{matrix_.Cost(group.right_index, left_index)};
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
  const auto end_list = ListFor(length_, end_left_, LatticeWordClass::Independent, true);
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
    const auto list = ListFor(node.begin, node.left_index, node.word_class, false);
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
  if (!mark(query_.constraints.segment_joins, Rule::Join) ||
      !mark(query_.constraints.segment_breaks, Rule::Break)) {
    result.status = LatticeStatus::Rejected;
    return result;
  }
  constrained_ = !query_.constraints.Empty();
  if (query_.max_paths == 0) return result;
  if (!Forward(result)) return result;
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
