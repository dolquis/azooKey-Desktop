#include "azookey/core/Lattice.h"

#include <algorithm>
#include <numeric>
#include <tuple>

#include "azookey/core/DoubleArrayTrie.h"
#include "azookey/core/Utf8.h"

namespace azookey::core {

bool LatticeLimits::Stopped() const {
  if (cancel && cancel->load(std::memory_order_relaxed)) return true;
  return deadline && std::chrono::steady_clock::now() >= *deadline;
}

std::span<const uint32_t> Lattice::BeginningAt(uint32_t position) const noexcept {
  if (position >= length_) return {};
  return {by_begin_.data() + begin_offsets_[position],
          begin_offsets_[position + 1] - begin_offsets_[position]};
}

std::span<const uint32_t> Lattice::EndingAt(uint32_t position) const noexcept {
  if (position == 0 || position > length_) return {};
  return {by_end_.data() + end_offsets_[position],
          end_offsets_[position + 1] - end_offsets_[position]};
}

class LatticeBuilder {
 public:
  static LatticeBuildResult Build(std::string_view reading, const LatticeParameters& parameters,
                                  std::span<const ILatticeVocabulary* const> vocabularies,
                                  const LatticeLimits& limits, Lattice& out);

 private:
  struct Candidate {
    LatticeNode node;
    LatticeScript script;
  };
  static void Fold(std::vector<Candidate>& candidates, std::vector<LatticeNode>& nodes);
  static void Index(Lattice& lattice);
};

// Section 4.2: words of several layers with the same (reading, surface, ids, part of speech,
// spelling) become the one with the smallest cost after penalties; the earliest on a tie.
// Survivors keep the vocabulary order.
void LatticeBuilder::Fold(std::vector<Candidate>& candidates, std::vector<LatticeNode>& nodes) {
  std::vector<uint32_t> order(candidates.size());
  std::iota(order.begin(), order.end(), 0U);
  const auto key = [&](uint32_t i) {
    const auto& c = candidates[i];
    return std::tie(c.node.end, c.node.surface, c.node.left_id, c.node.right_id, c.node.pos,
                    c.node.word_class, c.script);
  };
  std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
    if (key(a) != key(b)) return key(a) < key(b);
    return candidates[a].node.cost < candidates[b].node.cost;
  });
  std::vector<bool> keep(candidates.size(), false);
  for (size_t i = 0; i < order.size(); ++i)
    if (i == 0 || key(order[i - 1]) != key(order[i])) keep[order[i]] = true;
  for (size_t i = 0; i < candidates.size(); ++i)
    if (keep[i]) nodes.push_back(candidates[i].node);
}

void LatticeBuilder::Index(Lattice& lattice) {
  const auto length = lattice.length_;
  const auto& nodes = lattice.nodes_;
  lattice.begin_offsets_.assign(length + 2, 0);
  lattice.end_offsets_.assign(length + 2, 0);
  for (const auto& node : nodes) {
    ++lattice.begin_offsets_[node.begin + 1];
    ++lattice.end_offsets_[node.end + 1];
  }
  for (uint32_t p = 1; p < length + 2; ++p) {
    lattice.begin_offsets_[p] += lattice.begin_offsets_[p - 1];
    lattice.end_offsets_[p] += lattice.end_offsets_[p - 1];
  }
  // Nodes are appended in begin order, so the begin index is the identity.
  lattice.by_begin_.resize(nodes.size());
  std::iota(lattice.by_begin_.begin(), lattice.by_begin_.end(), 0U);
  lattice.by_end_.resize(nodes.size());
  auto next = lattice.end_offsets_;
  for (uint32_t i = 0; i < nodes.size(); ++i) lattice.by_end_[next[nodes[i].end]++] = i;
}

LatticeBuildResult LatticeBuilder::Build(std::string_view reading,
                                         const LatticeParameters& parameters,
                                         std::span<const ILatticeVocabulary* const> vocabularies,
                                         const LatticeLimits& limits, Lattice& out) {
  out = Lattice{};
  LatticeBuildResult result;
  const auto* matrix = parameters.matrix;
  if (!matrix || !matrix->IsValid()) {
    result.status = LatticeStatus::Unavailable;
    return result;
  }
  const auto unknown_left = matrix->LeftIndex(parameters.unknown_left_id);
  const auto unknown_right = matrix->RightIndex(parameters.unknown_right_id);
  if (unknown_left == kNoConnectionIndex || unknown_right == kNoConnectionIndex) {
    result.status = LatticeStatus::Unavailable;
    return result;
  }
  if (reading.empty() || !IsValidUtf8(reading)) {
    result.status = LatticeStatus::Rejected;
    return result;
  }
  std::vector<uint32_t> offsets;
  for (size_t offset = 0; offset < reading.size();) {
    offsets.push_back(static_cast<uint32_t>(offset));
    char32_t codepoint = 0;
    DecodeNextUtf8(reading, offset, codepoint);
  }
  offsets.push_back(static_cast<uint32_t>(reading.size()));
  const auto length = static_cast<uint32_t>(offsets.size() - 1);
  if (length > kMaxLatticeReading) {
    result.status = LatticeStatus::Rejected;
    return result;
  }
  constexpr uint32_t kNotBoundary = UINT32_MAX;
  std::vector<uint32_t> position_at(reading.size() + 1, kNotBoundary);
  for (uint32_t p = 0; p <= length; ++p) position_at[offsets[p]] = p;

  const auto& penalties = parameters.penalties;
  std::vector<LatticeEntry> entries;
  std::vector<Candidate> candidates;
  Lattice lattice;
  lattice.reading_ = reading;
  lattice.length_ = length;
  for (uint32_t p = 0; p < length; ++p) {
    if (limits.Stopped()) {
      result.status = LatticeStatus::Aborted;
      return result;
    }
    const auto key = reading.substr(offsets[p]);
    candidates.clear();
    for (uint32_t source = 0; source < vocabularies.size(); ++source) {
      if (!vocabularies[source]) continue;
      entries.clear();
      if (!vocabularies[source]->CommonPrefix(key, entries)) {
        result.status = LatticeStatus::Failed;
        return result;
      }
      for (const auto& entry : entries) {
        const auto left = matrix->LeftIndex(entry.left_id);
        const auto right = matrix->RightIndex(entry.right_id);
        if (entry.key_bytes == 0 || entry.key_bytes > key.size() || entry.surface.empty() ||
            position_at[offsets[p] + entry.key_bytes] == kNotBoundary ||
            left == kNoConnectionIndex || right == kNoConnectionIndex) {
          ++result.rejected_entries;
          continue;
        }
        int32_t penalty = penalties.node;
        if (entry.script == LatticeScript::KatakanaVariant) penalty += penalties.katakana_variant;
        if (entry.script == LatticeScript::KatakanaOwn) penalty += penalties.katakana_own;
        LatticeNode node;
        node.begin = p;
        node.end = position_at[offsets[p] + entry.key_bytes];
        node.surface = entry.surface;
        node.cost = int32_t{entry.word_cost} + penalty;
        node.left_id = entry.left_id;
        node.right_id = entry.right_id;
        node.left_index = left;
        node.right_index = right;
        node.pos = entry.pos;
        node.word_class = entry.word_class == LatticeWordClass::Unknown
                              ? LatticeWordClass::Independent
                              : entry.word_class;
        node.source = source;
        candidates.push_back({node, entry.script});
      }
    }
    Fold(candidates, lattice.nodes_);
    // Section 4.3: one unknown-word node per position, so every position has a path.
    LatticeNode unknown;
    unknown.begin = p;
    unknown.end = p + 1;
    unknown.surface = reading.substr(offsets[p], offsets[p + 1] - offsets[p]);
    unknown.cost = penalties.unknown + penalties.node;
    unknown.left_id = parameters.unknown_left_id;
    unknown.right_id = parameters.unknown_right_id;
    unknown.left_index = unknown_left;
    unknown.right_index = unknown_right;
    unknown.word_class = LatticeWordClass::Unknown;
    unknown.source = kLatticeUnknownSource;
    lattice.nodes_.push_back(unknown);
  }
  Index(lattice);
  out = std::move(lattice);
  if (limits.on_stage) limits.on_stage(LatticeStage::Built);
  return result;
}

LatticeBuildResult BuildLattice(std::string_view reading, const LatticeParameters& parameters,
                                std::span<const ILatticeVocabulary* const> vocabularies,
                                const LatticeLimits& limits, Lattice& out) {
  return LatticeBuilder::Build(reading, parameters, vocabularies, limits, out);
}

}  // namespace azookey::core
