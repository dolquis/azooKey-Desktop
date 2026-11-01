#include "azookey/core/MixedRomajiSplitter.h"

#include <algorithm>
#include <array>
#include <optional>

#include "azookey/core/RomajiKanaConverter.h"
#include "azookey/core/Utf8.h"

namespace azookey::core {

namespace {

// Section 6.3. The weights of the specification are starting values; these
// are the ones the section 15.2 quality gate (bench/data/mixed_english_eval)
// was run with.
constexpr double kJAsciiPenalty = 10.0;     // Per ASCII letter left in the kana.
constexpr double kJUnnaturalPenalty = 1.5;  // Per unnatural kana.
// A J segment right after an E segment that starts with a vowel letter: the E
// edge took the consonant of the kana (pull|sita read as pulls|ita).
constexpr double kJVowelAfterEnglishPenalty = 1.0;
// Per Shift capital inside a J segment: a capital says the English is meant.
constexpr double kJCapitalPenalty = 2.0;
constexpr double kEBaseCost = 1.0;         // Per E edge.
constexpr double kELengthBonus = 0.05;     // Per character of an E edge.
constexpr double kEReadablePenalty = 2.0;  // An E edge that reads as kana.
constexpr double kEMinCost = 0.1;          // A long word never costs less.

bool IsAsciiUpper(char c) { return c >= 'A' && c <= 'Z'; }
bool IsAsciiLower(char c) { return c >= 'a' && c <= 'z'; }
bool IsPrintableAscii(char c) { return c > ' ' && c <= '~'; }

std::string Lower(std::string_view raw) {
  std::string lower(raw);
  for (auto& c : lower)
    if (IsAsciiUpper(c)) c = static_cast<char>(c - 'A' + 'a');
  return lower;
}

bool HasAsciiLetter(std::string_view text) {
  return std::any_of(text.begin(), text.end(), IsAsciiLower);
}

size_t CountAsciiLetters(std::string_view text) {
  return static_cast<size_t>(std::count_if(text.begin(), text.end(), IsAsciiLower));
}

bool IsSmallVowel(char32_t c) {
  return c == U'ぁ' || c == U'ぃ' || c == U'ぅ' || c == U'ぇ' || c == U'ぉ';
}

bool IsSmallY(char32_t c) { return c == U'ゃ' || c == U'ゅ' || c == U'ょ'; }

// The kana a small vowel combines with (ふぁ, てぃ, うぃ, しぇ, とぅ, ゔぁ ...).
bool TakesSmallVowel(char32_t c) {
  static constexpr std::array<char32_t, 17> kKana = {U'い', U'う', U'く', U'ぐ', U'し', U'じ',
                                                     U'す', U'ず', U'ち', U'ぢ', U'つ', U'て',
                                                     U'で', U'と', U'ど', U'ふ', U'ゔ'};
  return std::find(kKana.begin(), kKana.end(), c) != kKana.end();
}

// The kana a small ya / yu / yo combines with (きゃ, てゅ, ふゅ ...).
bool TakesSmallY(char32_t c) {
  static constexpr std::array<char32_t, 16> kKana = {U'き', U'ぎ', U'し', U'じ', U'ち', U'ぢ',
                                                     U'に', U'ひ', U'び', U'ぴ', U'み', U'り',
                                                     U'て', U'で', U'ふ', U'ゔ'};
  return std::find(kKana.begin(), kKana.end(), c) != kKana.end();
}

// Section 6.3 "unnatural kana": a small vowel or small ya / yu / yo that does
// not combine with the kana before it, and every ゔ (love -> ぉゔぇ).
size_t CountUnnaturalKana(std::string_view kana) {
  size_t count = 0;
  std::optional<char32_t> previous;
  size_t offset = 0;
  char32_t c = 0;
  while (offset < kana.size() && DecodeNextUtf8(kana, offset, c)) {
    if (c == U'ゔ' || (IsSmallVowel(c) && !(previous && TakesSmallVowel(*previous))) ||
        (IsSmallY(c) && !(previous && TakesSmallY(*previous))))
      ++count;
    previous = c;
  }
  return count;
}

// Section 6.3: an E edge of `length` characters.
double EnglishEdgeCost(size_t length, bool readable, bool shift) {
  // A Shift capital says the English is meant, so the readable penalty does
  // not apply to it.
  return std::max(kEMinCost, kEBaseCost - kELengthBonus * static_cast<double>(length)) +
         (readable && !shift ? kEReadablePenalty : 0.0);
}

// A J segment that starts right after an E segment: a leading vowel letter
// (the E edge took its consonant), or a leading ん (no Japanese word starts
// with it; tosca|nnohanasi against toscan|nohanasi).
double BoundaryCost(const std::string& lower, size_t begin) {
  const char c = lower[begin];
  if (c == 'a' || c == 'i' || c == 'u' || c == 'e' || c == 'o') return kJVowelAfterEnglishPenalty;
  if (c == 'n') {
    const char next = begin + 1 < lower.size() ? lower[begin + 1] : '\0';
    const bool vowel_or_y =
        next == 'a' || next == 'i' || next == 'u' || next == 'e' || next == 'o' || next == 'y';
    if (!vowel_or_y) return kJUnnaturalPenalty;
  }
  return 0.0;
}

double JapaneseEdgeCost(std::string_view raw, const std::string& lower, size_t begin, size_t end) {
  const auto kana = RomajiKanaConverter::ConvertForCommit(lower.substr(begin, end - begin));
  const auto typed = raw.substr(begin, end - begin);
  const auto capitals = std::count_if(typed.begin(), typed.end(), IsAsciiUpper);
  return kJAsciiPenalty * static_cast<double>(CountAsciiLetters(kana)) +
         kJUnnaturalPenalty * static_cast<double>(CountUnnaturalKana(kana)) +
         kJCapitalPenalty * static_cast<double>(capitals);
}

struct EnglishEdge {
  size_t end{};
  double cost{};
  std::string surface;
  EnglishSegmentConfidence confidence{EnglishSegmentConfidence::Low};
};

// One of the k best partial paths that end at a position in a state.
struct Node {
  double cost{};
  // The edge that reached this node: from (begin, from_state, from_rank).
  size_t begin{};
  int from_state{};
  size_t from_rank{};
  bool english{};
  size_t edge_index{};  // Into english_edges[begin] for an E edge.
};

enum State : int { kAfterEnglish = 0, kAfterJapanese = 1 };

class Lattice {
 public:
  Lattice(std::string_view raw, const std::string& lower, const EnglishWordLookup& lookup,
          const MixedRomajiSplitOptions& options)
      : raw_(raw), lower_(lower), options_(options), english_edges_(lower.size()) {
    for (size_t begin = 0; begin < lower_.size(); ++begin) {
      const bool shift = IsAsciiUpper(raw_[begin]);
      const size_t min_length =
          shift ? options_.min_shift_inline_english_length : options_.min_inline_english_length;
      const size_t max_end = std::min(lower_.size(), begin + options_.max_inline_english_length);
      for (size_t end = begin + std::max<size_t>(min_length, 1); end <= max_end; ++end) {
        const std::string key = lower_.substr(begin, end - begin);
        if (!HasAsciiLetter(key)) continue;
        const auto matches = lookup(key);
        if (matches.empty()) continue;
        const auto kana = RomajiKanaConverter::ConvertForCommit(key);
        const bool readable = !HasAsciiLetter(kana);
        EnglishEdge edge;
        edge.end = end;
        edge.cost = EnglishEdgeCost(end - begin, readable, shift);
        const auto typed = raw_.substr(begin, end - begin);
        const bool has_upper = std::any_of(typed.begin(), typed.end(), IsAsciiUpper);
        edge.surface = has_upper ? std::string(typed) : matches.front().surface;
        // Section 8.1: every lattice E edge matches the dictionary, so (ii) is
        // the letters left after the conversion.
        const bool sure = shift || !readable;
        edge.confidence = sure && options_.inline_high_confidence ? EnglishSegmentConfidence::High
                                                                  : EnglishSegmentConfidence::Low;
        english_edges_[begin].push_back(std::move(edge));
      }
    }
  }

  bool HasEnglishEdge() const {
    return std::any_of(english_edges_.begin(), english_edges_.end(),
                       [](const auto& edges) { return !edges.empty(); });
  }

  std::vector<MixedRomajiSplit> Best() {
    const size_t n = lower_.size();
    // nodes_[position][state]: the k best partial paths, cheapest first.
    nodes_.assign(n + 1, {});
    nodes_[0][kAfterEnglish].push_back({});
    for (size_t begin = 0; begin < n; ++begin) {
      for (int state : {kAfterEnglish, kAfterJapanese}) {
        const auto& here = nodes_[begin][state];
        for (size_t rank = 0; rank < here.size(); ++rank) {
          const double cost = here[rank].cost;
          for (size_t index = 0; index < english_edges_[begin].size(); ++index) {
            const auto& edge = english_edges_[begin][index];
            Offer(edge.end, kAfterEnglish, {cost + edge.cost, begin, state, rank, true, index});
          }
          // Section 6.2: J edges never follow one another.
          if (state == kAfterJapanese) continue;
          const double boundary = begin > 0 ? BoundaryCost(lower_, begin) : 0.0;
          for (size_t end = begin + 1; end <= n; ++end)
            Offer(end, kAfterJapanese,
                  {cost + boundary + JapaneseCost(begin, end), begin, state, rank});
        }
      }
    }
    std::vector<std::pair<int, size_t>> ends;
    for (int state : {kAfterEnglish, kAfterJapanese})
      for (size_t rank = 0; rank < nodes_[n][state].size(); ++rank) ends.emplace_back(state, rank);
    std::stable_sort(ends.begin(), ends.end(), [&](const auto& l, const auto& r) {
      return nodes_[n][l.first][l.second].cost < nodes_[n][r.first][r.second].cost;
    });
    if (ends.size() > options_.max_splits) ends.resize(options_.max_splits);
    std::vector<MixedRomajiSplit> splits;
    for (const auto& [state, rank] : ends) splits.push_back(Trace(n, state, rank));
    return splits;
  }

 private:
  double JapaneseCost(size_t begin, size_t end) {
    auto& cached = japanese_costs_[begin * (lower_.size() + 1) + end];
    if (!cached) cached = JapaneseEdgeCost(raw_, lower_, begin, end);
    return *cached;
  }

  void Offer(size_t position, int state, Node node) {
    auto& list = nodes_[position][state];
    const auto at = std::upper_bound(list.begin(), list.end(), node.cost,
                                     [](double cost, const Node& n) { return cost < n.cost; });
    if (static_cast<size_t>(at - list.begin()) >= options_.max_splits) return;
    list.insert(at, node);
    if (list.size() > options_.max_splits) list.pop_back();
  }

  MixedRomajiSplit Trace(size_t position, int state, size_t rank) const {
    MixedRomajiSplit split;
    split.cost = nodes_[position][state][rank].cost;
    while (position > 0) {
      const Node& node = nodes_[position][state][rank];
      MixedRomajiSegment segment;
      segment.raw_begin = node.begin;
      segment.raw_end = position;
      if (node.english) {
        const auto& edge = english_edges_[node.begin][node.edge_index];
        segment.kind = MixedRomajiSegmentKind::English;
        segment.surface = edge.surface;
        segment.confidence = edge.confidence;
      } else {
        segment.kana =
            RomajiKanaConverter::ConvertForCommit(lower_.substr(node.begin, position - node.begin));
      }
      split.segments.push_back(std::move(segment));
      position = node.begin;
      state = node.from_state;
      rank = node.from_rank;
    }
    std::reverse(split.segments.begin(), split.segments.end());
    return split;
  }

  std::string_view raw_;
  const std::string& lower_;
  const MixedRomajiSplitOptions& options_;
  std::vector<std::vector<EnglishEdge>> english_edges_;
  std::vector<std::array<std::vector<Node>, 2>> nodes_;
  std::vector<std::optional<double>> japanese_costs_ =
      std::vector<std::optional<double>>((lower_.size() + 1) * (lower_.size() + 1));
};

}  // namespace

std::vector<MixedRomajiSplit> SplitMixedRomaji(std::string_view raw_romaji,
                                               const EnglishWordLookup& lookup,
                                               const MixedRomajiSplitOptions& options) {
  if (raw_romaji.empty() || raw_romaji.size() > options.max_raw_length || options.max_splits == 0 ||
      !std::all_of(raw_romaji.begin(), raw_romaji.end(), IsPrintableAscii))
    return {};
  const auto lower = Lower(raw_romaji);
  const auto kana = RomajiKanaConverter::ConvertForCommit(lower);

  MixedRomajiSegment segment;
  segment.raw_end = raw_romaji.size();
  const bool has_letter = HasAsciiLetter(lower);
  const bool long_enough = raw_romaji.size() >= options.min_english_length;
  const bool shift_first = IsAsciiUpper(raw_romaji.front());
  const auto matches =
      has_letter && long_enough && lookup ? lookup(lower) : std::vector<EnglishWordMatch>{};
  // Section 8.2: a whole-composition word, without the section 6.3 penalty.
  if (!matches.empty()) {
    segment.kind = MixedRomajiSegmentKind::English;
    // Section 6.4: typed capitals keep the keystrokes; otherwise the most
    // frequent dictionary surface.
    const bool has_upper = std::any_of(raw_romaji.begin(), raw_romaji.end(), IsAsciiUpper);
    segment.surface = has_upper ? std::string(raw_romaji) : matches.front().surface;
    // Section 8.1: (i) a Shift capital first, or (ii) letters left after the
    // romaji-to-kana conversion together with the dictionary match.
    segment.confidence = shift_first || HasAsciiLetter(kana) ? EnglishSegmentConfidence::High
                                                             : EnglishSegmentConfidence::Low;
    return {MixedRomajiSplit{{std::move(segment)}, 0.0}};
  }
  // Sections 6.2 and 6.3: the mixed lattice, when it has an E edge to offer.
  if (options.split_inline && lookup && has_letter) {
    Lattice lattice(raw_romaji, lower, lookup, options);
    if (lattice.HasEnglishEdge()) {
      auto splits = lattice.Best();
      const bool any_english =
          !splits.empty() &&
          std::any_of(splits.front().segments.begin(), splits.front().segments.end(),
                      [](const auto& s) { return s.kind == MixedRomajiSegmentKind::English; });
      if (any_english || !shift_first) return splits;
    }
  }
  // Section 6.1: a Shift capital first makes the whole composition E.
  if (has_letter && long_enough && shift_first) {
    segment.kind = MixedRomajiSegmentKind::English;
    segment.surface = std::string(raw_romaji);
    segment.confidence = EnglishSegmentConfidence::High;
  } else {
    segment.kana = kana;
  }
  return {MixedRomajiSplit{{std::move(segment)}, 0.0}};
}

std::vector<MixedRomajiAlternative> ReadableEnglishAlternatives(
    std::string_view raw_romaji, const MixedRomajiSplit& adopted, const EnglishWordLookup& lookup,
    const MixedRomajiSplitOptions& options) {
  std::vector<MixedRomajiAlternative> alternatives;
  if (!options.split_inline || !lookup || raw_romaji.empty() ||
      raw_romaji.size() > options.max_raw_length ||
      !std::all_of(raw_romaji.begin(), raw_romaji.end(), IsPrintableAscii))
    return alternatives;
  const auto lower = Lower(raw_romaji);
  const size_t min_length =
      std::max(options.min_readable_english_length, options.min_inline_english_length);
  for (size_t index = 0; index < adopted.segments.size(); ++index) {
    const auto& segment = adopted.segments[index];
    if (segment.kind != MixedRomajiSegmentKind::Japanese || segment.raw_end > lower.size() ||
        segment.raw_begin >= segment.raw_end)
      continue;
    const size_t s = segment.raw_begin;
    const size_t t = segment.raw_end;
    // A J segment of the lattice follows an E one unless it starts the
    // composition (section 6.2: J edges never follow one another).
    const double before = s > 0 ? BoundaryCost(lower, s) : 0.0;
    const double adopted_cost = before + JapaneseEdgeCost(raw_romaji, lower, s, t);
    const auto whole_kana = RomajiKanaConverter::ConvertForCommit(lower.substr(s, t - s));
    for (size_t begin = s; begin < t; ++begin) {
      // A Shift capital first makes the edge sure (section 8.1 (i)), never one
      // of these.
      if (IsAsciiUpper(raw_romaji[begin])) continue;
      const size_t max_end = std::min(t, begin + options.max_inline_english_length);
      for (size_t end = begin + min_length; end <= max_end; ++end) {
        // The whole composition as one word is M63-A (section 8.2).
        if (begin == 0 && end == lower.size()) continue;
        const std::string key = lower.substr(begin, end - begin);
        const auto kana = RomajiKanaConverter::ConvertForCommit(key);
        if (HasAsciiLetter(kana)) continue;  // Not readable: the lattice's own call.
        // Section 8.3 item 3: the kana around the word are the J segment's
        // kana cut at the word.
        const auto head = RomajiKanaConverter::ConvertForCommit(lower.substr(s, begin - s));
        const auto tail = RomajiKanaConverter::ConvertForCommit(lower.substr(end, t - end));
        if (head + kana + tail != whole_kana) continue;
        // Trap 1 of DEV-1820: the J pieces cost no more than the J segment, so
        // the path differs from the adopted one by the E edge alone.
        const double head_cost =
            begin > s ? before + JapaneseEdgeCost(raw_romaji, lower, s, begin) : 0.0;
        const double tail_cost =
            end < t ? BoundaryCost(lower, end) + JapaneseEdgeCost(raw_romaji, lower, end, t) : 0.0;
        constexpr double kEpsilon = 1e-9;
        if (head_cost + tail_cost > adopted_cost + kEpsilon) continue;
        const auto matches = lookup(key);
        // A common word: a lowercase surface without the proper / acronym flags.
        const bool common = std::any_of(matches.begin(), matches.end(), [&](const auto& match) {
          return match.flags == 0 && match.surface == key;
        });
        if (!common) continue;
        MixedRomajiAlternative alternative;
        auto& split = alternative.split;
        split.segments.insert(split.segments.end(), adopted.segments.begin(),
                              adopted.segments.begin() + static_cast<std::ptrdiff_t>(index));
        if (begin > s) {
          MixedRomajiSegment piece;
          piece.raw_begin = s;
          piece.raw_end = begin;
          piece.kana = head;
          split.segments.push_back(std::move(piece));
        }
        MixedRomajiSegment english;
        english.kind = MixedRomajiSegmentKind::English;
        english.raw_begin = begin;
        english.raw_end = end;
        const auto typed = raw_romaji.substr(begin, end - begin);
        const bool has_upper = std::any_of(typed.begin(), typed.end(), IsAsciiUpper);
        english.surface = has_upper ? std::string(typed) : matches.front().surface;
        english.confidence = EnglishSegmentConfidence::Low;
        alternative.english_segment = split.segments.size();
        split.segments.push_back(std::move(english));
        if (end < t) {
          MixedRomajiSegment piece;
          piece.raw_begin = end;
          piece.raw_end = t;
          piece.kana = tail;
          split.segments.push_back(std::move(piece));
        }
        split.segments.insert(split.segments.end(),
                              adopted.segments.begin() + static_cast<std::ptrdiff_t>(index) + 1,
                              adopted.segments.end());
        split.cost = adopted.cost - adopted_cost + head_cost + tail_cost +
                     EnglishEdgeCost(end - begin, true, false);
        alternatives.push_back(std::move(alternative));
        if (alternatives.size() == options.max_splits) return alternatives;
      }
    }
  }
  return alternatives;
}

}  // namespace azookey::core
