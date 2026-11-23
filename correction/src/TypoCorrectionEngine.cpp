#include "azookey/correction/TypoCorrectionEngine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <map>
#include <tuple>
#include <utility>

#include "azookey/core/QwertyAdjacency.h"
#include "azookey/core/RomajiKanaConverter.h"
#include "azookey/core/Utf8.h"
#include "azookey/correction/RomajiVariantNormalizer.h"

namespace azookey::correction {
namespace {

constexpr int kBaseCost = core::kQwertyBaseEditCost;
constexpr int kTranspositionCost = core::kQwertyTranspositionCost;

// Decodes every code point; false for invalid UTF-8.
bool Decode(std::string_view text, std::u32string& codepoints) {
  codepoints.clear();
  size_t offset = 0;
  while (offset < text.size()) {
    char32_t codepoint = 0;
    if (!core::DecodeNextUtf8(text, offset, codepoint)) return false;
    codepoints.push_back(codepoint);
  }
  return true;
}

std::string Encode(const std::u32string& codepoints) {
  std::string text;
  for (const char32_t codepoint : codepoints) core::AppendUtf8(text, codepoint);
  return text;
}

bool IsKana(char32_t c) { return (c >= 0x3041 && c <= 0x3096) || c == 0x30FC; }

// Section 12.12.1 / 12.13 alphabet.
bool IsAlphabet(char32_t c) {
  return (c >= U'a' && c <= U'z') || c == U'-' || c == U'\'' || IsKana(c);
}

bool IsAsciiKey(char32_t c) { return c >= U'a' && c <= U'z'; }

bool IsConsonant(char32_t c) {
  return IsAsciiKey(c) && c != U'a' && c != U'i' && c != U'u' && c != U'e' && c != U'o' &&
         c != U'n';
}

// Section 12.3 dakuten_confusion and kana_shape_confusion: the kana of each
// group stand in for one another.
enum class KanaConfusion { Dakuten, Shape };

struct KanaGroup {
  std::string_view members;
  KanaConfusion kind;
};

constexpr std::array<KanaGroup, 38> kKanaGroups = {{
    {"かが", KanaConfusion::Dakuten},   {"きぎ", KanaConfusion::Dakuten},
    {"くぐ", KanaConfusion::Dakuten},   {"けげ", KanaConfusion::Dakuten},
    {"こご", KanaConfusion::Dakuten},   {"さざ", KanaConfusion::Dakuten},
    {"しじ", KanaConfusion::Dakuten},   {"すず", KanaConfusion::Dakuten},
    {"せぜ", KanaConfusion::Dakuten},   {"そぞ", KanaConfusion::Dakuten},
    {"ただ", KanaConfusion::Dakuten},   {"ちぢ", KanaConfusion::Dakuten},
    {"つづ", KanaConfusion::Dakuten},   {"てで", KanaConfusion::Dakuten},
    {"とど", KanaConfusion::Dakuten},   {"はばぱ", KanaConfusion::Dakuten},
    {"ひびぴ", KanaConfusion::Dakuten}, {"ふぶぷ", KanaConfusion::Dakuten},
    {"へべぺ", KanaConfusion::Dakuten}, {"ほぼぽ", KanaConfusion::Dakuten},
    {"うゔ", KanaConfusion::Dakuten},   {"あぁ", KanaConfusion::Shape},
    {"いぃ", KanaConfusion::Shape},     {"うぅ", KanaConfusion::Shape},
    {"えぇ", KanaConfusion::Shape},     {"おぉ", KanaConfusion::Shape},
    {"つっ", KanaConfusion::Shape},     {"やゃ", KanaConfusion::Shape},
    {"ゆゅ", KanaConfusion::Shape},     {"よょ", KanaConfusion::Shape},
    {"わゎ", KanaConfusion::Shape},     {"かゕ", KanaConfusion::Shape},
    {"けゖ", KanaConfusion::Shape},     {"ぬめ", KanaConfusion::Shape},
    {"わねれ", KanaConfusion::Shape},   {"るろ", KanaConfusion::Shape},
    {"はほ", KanaConfusion::Shape},     {"さち", KanaConfusion::Shape},
}};

using KanaAlternatives = std::map<char32_t, std::vector<std::pair<char32_t, KanaConfusion>>>;

const KanaAlternatives& Alternatives() {
  static const KanaAlternatives alternatives = [] {
    KanaAlternatives table;
    for (const KanaGroup& group : kKanaGroups) {
      std::u32string members;
      Decode(group.members, members);
      for (const char32_t from : members) {
        for (const char32_t to : members) {
          if (from == to) continue;
          auto& list = table[from];
          const bool known = std::any_of(list.begin(), list.end(),
                                         [&](const auto& entry) { return entry.first == to; });
          if (!known) list.emplace_back(to, group.kind);
        }
      }
    }
    return table;
  }();
  return alternatives;
}

std::optional<KanaConfusion> ConfusionBetween(char32_t from, char32_t to) {
  const auto& table = Alternatives();
  const auto it = table.find(from);
  if (it == table.end()) return std::nullopt;
  for (const auto& [alternative, kind] : it->second) {
    if (alternative == to) return kind;
  }
  return std::nullopt;
}

TypoType TypeOf(KanaConfusion kind) {
  return kind == KanaConfusion::Dakuten ? TypoType::DakutenConfusion : TypoType::KanaShapeConfusion;
}

// The reading type when `reading` differs from `observed` only by kana
// confusions; otherwise none.
std::optional<TypoType> ReadingTypeOf(const std::u32string& observed,
                                      const std::u32string& reading) {
  if (observed.size() != reading.size()) return std::nullopt;
  std::optional<TypoType> type;
  for (size_t index = 0; index < observed.size(); ++index) {
    if (observed[index] == reading[index]) continue;
    const auto kind = ConfusionBetween(observed[index], reading[index]);
    if (!kind) return std::nullopt;
    type = type ? (std::min)(*type, TypeOf(*kind)) : TypeOf(*kind);
  }
  return type;
}

size_t EditDistance(const std::u32string& a, const std::u32string& b) {
  std::vector<size_t> row(b.size() + 1);
  for (size_t j = 0; j <= b.size(); ++j) row[j] = j;
  for (size_t i = 1; i <= a.size(); ++i) {
    size_t diagonal = row[0];
    row[0] = i;
    for (size_t j = 1; j <= b.size(); ++j) {
      const size_t above = row[j];
      row[j] = (std::min)({above + 1, row[j - 1] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0 : 1)});
      diagonal = above;
    }
  }
  return row[b.size()];
}

// An edit and where it sits in the observed keys: the index of the extra,
// substituted or first swapped key, or for a left-out key the index it would
// take.
struct PlacedEdit {
  size_t position = 0;
  CorrectionEdit edit;
};

char32_t KeyBefore(const std::u32string& observed, size_t position) {
  return position == 0 ? kEditStart : observed[position - 1];
}

// Weighted optimal string alignment (section 12.4.6 costs) of the observed
// keys against the intended ones. Returns the cost in tenths and the edits in
// observed order. An extra or left-out key inside a run of the same key is
// placed at the end of the run, so a doubled key reads "g after g".
std::pair<int, std::vector<PlacedEdit>> Align(const std::u32string& observed,
                                              const std::u32string& intended,
                                              const KeyboardAdjacencyModel& keyboard) {
  const size_t rows = observed.size() + 1;
  const size_t columns = intended.size() + 1;
  std::vector<int> distance(rows * columns);
  const auto at = [&](size_t i, size_t j) -> int& { return distance[i * columns + j]; };
  const auto substitution = [&](size_t i, size_t j) {
    return keyboard.SubstitutionCost(observed[i - 1], intended[j - 1]);
  };
  const auto swapped = [&](size_t i, size_t j) {
    return i > 1 && j > 1 && observed[i - 1] == intended[j - 2] &&
           observed[i - 2] == intended[j - 1] && observed[i - 1] != observed[i - 2];
  };
  for (size_t i = 0; i < rows; ++i) at(i, 0) = static_cast<int>(i) * kBaseCost;
  for (size_t j = 0; j < columns; ++j) at(0, j) = static_cast<int>(j) * kBaseCost;
  for (size_t i = 1; i < rows; ++i) {
    for (size_t j = 1; j < columns; ++j) {
      int best = at(i - 1, j - 1) + substitution(i, j);
      best = (std::min)({best, at(i - 1, j) + kBaseCost, at(i, j - 1) + kBaseCost});
      if (swapped(i, j)) best = (std::min)(best, at(i - 2, j - 2) + kTranspositionCost);
      at(i, j) = best;
    }
  }

  std::vector<PlacedEdit> edits;
  size_t i = observed.size();
  size_t j = intended.size();
  while (i > 0 || j > 0) {
    if (i > 0 && j > 0 && observed[i - 1] == intended[j - 1] && at(i, j) == at(i - 1, j - 1)) {
      --i;
      --j;
    } else if (swapped(i, j) && at(i, j) == at(i - 2, j - 2) + kTranspositionCost) {
      edits.push_back(
          {i - 2, {CorrectionEditKind::Transposition, observed[i - 2], observed[i - 1], 0}});
      i -= 2;
      j -= 2;
    } else if (i > 0 && j > 0 && observed[i - 1] != intended[j - 1] &&
               at(i, j) == at(i - 1, j - 1) + substitution(i, j)) {
      edits.push_back(
          {i - 1, {CorrectionEditKind::Substitution, observed[i - 1], intended[j - 1], 0}});
      --i;
      --j;
    } else if (i > 0 && at(i, j) == at(i - 1, j) + kBaseCost) {
      edits.push_back({i - 1, {CorrectionEditKind::Insertion, observed[i - 1], 0, 0}});
      --i;
    } else {
      edits.push_back({i, {CorrectionEditKind::Deletion, intended[j - 1], 0, 0}});
      --j;
    }
  }
  std::reverse(edits.begin(), edits.end());
  for (PlacedEdit& placed : edits) {
    const char32_t key = placed.edit.first;
    if (placed.edit.kind == CorrectionEditKind::Insertion) {
      while (placed.position + 1 < observed.size() && observed[placed.position + 1] == key) {
        ++placed.position;
      }
    } else if (placed.edit.kind == CorrectionEditKind::Deletion) {
      while (placed.position < observed.size() && observed[placed.position] == key) {
        ++placed.position;
      }
    }
    placed.edit.previous = KeyBefore(observed, placed.position);
  }
  std::stable_sort(edits.begin(), edits.end(), [](const PlacedEdit& a, const PlacedEdit& b) {
    return a.position < b.position;
  });
  return {at(observed.size(), intended.size()), std::move(edits)};
}

std::string Narrow(const std::u32string& keys) {
  std::string text;
  text.reserve(keys.size());
  for (const char32_t key : keys) text.push_back(static_cast<char>(key));
  return text;
}

std::u32string Applied(std::u32string keys, const PlacedEdit& placed) {
  const CorrectionEdit& edit = placed.edit;
  switch (edit.kind) {
    case CorrectionEditKind::Insertion:
      keys.erase(placed.position, 1);
      break;
    case CorrectionEditKind::Deletion:
      keys.insert(keys.begin() + static_cast<std::ptrdiff_t>(placed.position), edit.first);
      break;
    case CorrectionEditKind::Substitution:
      keys[placed.position] = edit.second;
      break;
    case CorrectionEditKind::Transposition:
      std::swap(keys[placed.position], keys[placed.position + 1]);
      break;
  }
  return keys;
}

// Section 12.3 for one key edit, checked in this order: n_handling, small_tsu,
// double_key, romaji_variant, long_vowel, transposition, adjacent_key,
// extra_char, missing_char. None when no type fits.
std::optional<TypoType> ClassifyKeyEdit(const std::u32string& observed, const PlacedEdit& placed,
                                        const KeyboardAdjacencyModel& keyboard) {
  const CorrectionEdit& edit = placed.edit;
  const bool insertion = edit.kind == CorrectionEditKind::Insertion;
  const bool deletion = edit.kind == CorrectionEditKind::Deletion;
  const bool substitution = edit.kind == CorrectionEditKind::Substitution;
  const auto is_o_or_u = [](char32_t c) { return c == U'o' || c == U'u'; };

  if ((insertion || deletion) && (edit.first == U'n' || edit.first == U'\'')) {
    return TypoType::NHandling;
  }
  if (deletion && edit.first == edit.previous && IsConsonant(edit.first)) return TypoType::SmallTsu;
  if (insertion && edit.first == edit.previous) return TypoType::DoubleKey;
  const std::string typed = Narrow(observed);
  if (NormalizeRomajiVariants(Narrow(Applied(observed, placed))) ==
      NormalizeRomajiVariants(typed)) {
    return TypoType::RomajiVariant;
  }
  if ((insertion || deletion) && edit.first == U'u' && is_o_or_u(edit.previous) &&
      EndsWithVariantYoon(std::string_view(typed).substr(0, placed.position))) {
    return TypoType::RomajiVariant;
  }
  if ((insertion || deletion) && is_o_or_u(edit.first) && is_o_or_u(edit.previous)) {
    return TypoType::LongVowel;
  }
  if (substitution && edit.previous == U'o' && is_o_or_u(edit.first) && is_o_or_u(edit.second)) {
    return TypoType::LongVowel;
  }
  if (edit.kind == CorrectionEditKind::Transposition) return TypoType::Transposition;
  if (substitution && (keyboard.IsAdjacent(edit.first, edit.second) ||
                       keyboard.IsSameFinger(edit.first, edit.second))) {
    return TypoType::AdjacentKey;
  }
  if (insertion && keyboard.IsAdjacent(edit.first, edit.previous)) return TypoType::AdjacentKey;
  if (insertion) return TypoType::ExtraChar;
  if (deletion) return TypoType::MissingChar;
  return std::nullopt;
}

bool IsUsable(const PersonalizedPattern& pattern) {
  return std::isfinite(pattern.cost_reduction) && pattern.cost_reduction >= 0.0 &&
         std::isfinite(pattern.confidence) && pattern.confidence >= 0.0 &&
         pattern.confidence <= 1.0;
}

struct Candidate {
  int edit_cost = 0;  // tenths, before the personalized reduction
  EditSpace space = EditSpace::Keys;
  TypoType type = TypoType::AdjacentKey;
  std::vector<CorrectionEdit> edits;
  // Set by Offer.
  double cost = 0.0;
  double confidence = kGeneralTypoConfidence;
  std::string pattern_id;
  bool personalized = false;
};

using CandidateMap = std::map<std::string, Candidate>;  // by reading

// Applies the personal pattern of the candidate's edits (section 12.4.1
// personalized_pattern_cost), then keeps the cheapest candidate of each
// reading, so the paths to a reading compete on their reduced cost.
class CandidateSet {
 public:
  explicit CandidateSet(const PersonalizedPatternSource* personalized)
      : personalized_(personalized) {}

  void Offer(std::string reading, Candidate candidate) {
    candidate.cost = static_cast<double>(candidate.edit_cost) / core::kQwertyEditCostScale;
    if (personalized_) {
      const auto pattern = personalized_->Find(candidate.type, candidate.space, candidate.edits);
      if (pattern && IsUsable(*pattern)) {
        candidate.cost = (std::max)(0.0, candidate.cost - pattern->cost_reduction);
        candidate.confidence = pattern->confidence;
        candidate.pattern_id = pattern->pattern_id;
        candidate.personalized = true;
      }
    }
    const auto [it, inserted] = candidates_.try_emplace(std::move(reading), candidate);
    if (!inserted &&
        std::tie(candidate.cost, candidate.space) < std::tie(it->second.cost, it->second.space)) {
      it->second = std::move(candidate);
    }
  }

  CandidateMap& candidates() { return candidates_; }

 private:
  const PersonalizedPatternSource* personalized_;
  CandidateMap candidates_;
};

void AddKeyCandidates(std::string_view observed_reading, const std::u32string& observed,
                      const std::u32string& keys, const KeyboardAdjacencyModel& keyboard,
                      CandidateSet& candidates) {
  const std::string typed = Narrow(keys);
  if (core::RomajiKanaConverter::ConvertForCommit(typed) != observed_reading) return;
  const auto ambiguous = core::RomajiKanaConverter::AmbiguousReadings(typed);
  core::RomajiKanaConverter converter;
  for (const auto& fuzzy : converter.FuzzyMatch(typed, kMaxTypoHypotheses)) {
    if (std::find(ambiguous.begin(), ambiguous.end(), fuzzy.kana) != ambiguous.end()) continue;
    const std::u32string intended(fuzzy.romaji.begin(), fuzzy.romaji.end());
    auto [cost, placed] = Align(keys, intended, keyboard);
    if (placed.empty() || placed.size() > kTypoMaxEdits) continue;
    std::u32string reading;
    if (!Decode(fuzzy.kana, reading)) continue;
    std::optional<TypoType> type = ReadingTypeOf(observed, reading);
    if (!type) {
      for (const PlacedEdit& edit : placed) {
        const auto edit_type = ClassifyKeyEdit(keys, edit, keyboard);
        if (!edit_type) {
          type.reset();
          break;
        }
        type = type ? (std::min)(*type, *edit_type) : *edit_type;
      }
    }
    if (!type) continue;
    Candidate candidate;
    candidate.edit_cost = cost;
    candidate.space = EditSpace::Keys;
    candidate.type = *type;
    for (const PlacedEdit& edit : placed) candidate.edits.push_back(edit.edit);
    candidates.Offer(fuzzy.kana, std::move(candidate));
  }
}

void AddReadingCandidates(const std::u32string& observed, CandidateSet& candidates) {
  if (!std::all_of(observed.begin(), observed.end(), IsKana)) return;
  const auto& alternatives = Alternatives();
  std::vector<size_t> positions;
  for (size_t index = 0; index < observed.size(); ++index) {
    if (alternatives.count(observed[index]) != 0) positions.push_back(index);
  }
  const auto substitute = [&](std::u32string& reading, Candidate& candidate, size_t position,
                              char32_t to, KanaConfusion kind) {
    candidate.edits.push_back(
        {CorrectionEditKind::Substitution, observed[position], to, KeyBefore(observed, position)});
    candidate.type =
        candidate.edits.size() == 1 ? TypeOf(kind) : (std::min)(candidate.type, TypeOf(kind));
    candidate.edit_cost += kBaseCost;
    reading[position] = to;
  };
  for (size_t first = 0; first < positions.size(); ++first) {
    for (const auto& [to, kind] : alternatives.at(observed[positions[first]])) {
      std::u32string reading = observed;
      Candidate single;
      single.space = EditSpace::Reading;
      substitute(reading, single, positions[first], to, kind);
      candidates.Offer(Encode(reading), single);
      for (size_t second = first + 1; second < positions.size(); ++second) {
        for (const auto& [to2, kind2] : alternatives.at(observed[positions[second]])) {
          std::u32string pair_reading = reading;
          Candidate pair = single;
          substitute(pair_reading, pair, positions[second], to2, kind2);
          candidates.Offer(Encode(pair_reading), std::move(pair));
        }
      }
    }
  }
}

}  // namespace

std::vector<ReadingHypothesis> GenerateReadingHypotheses(const TypoCorrectionInput& input,
                                                         const TypoCorrectionOptions& options) {
  std::vector<ReadingHypothesis> hypotheses;
  if (input.observed_reading.empty()) return hypotheses;
  ReadingHypothesis original;
  original.reading = std::string(input.observed_reading);
  hypotheses.push_back(original);

  std::u32string observed;
  std::u32string keys;
  if (!Decode(input.observed_reading, observed) || !Decode(input.raw_keys, keys)) return hypotheses;
  if (observed.size() <= kTypoMinReadingLength || observed.size() > kTypoMaxInputLength ||
      keys.size() > kTypoMaxInputLength) {
    return hypotheses;
  }
  if (!std::all_of(observed.begin(), observed.end(), IsAlphabet) ||
      !std::all_of(keys.begin(), keys.end(), IsAlphabet)) {
    return hypotheses;
  }
  const size_t limit = (std::min)(options.limit, kMaxTypoHypotheses);
  if (limit == 0) return hypotheses;

  const KeyboardAdjacencyModel keyboard(options.keyboard_layout);
  CandidateSet candidates(options.personalized);
  const bool romaji_keys = !keys.empty() && std::all_of(keys.begin(), keys.end(), [](char32_t c) {
    return IsAsciiKey(c) || c == U'-' || c == U'\'';
  });
  if (romaji_keys) AddKeyCandidates(input.observed_reading, observed, keys, keyboard, candidates);
  AddReadingCandidates(observed, candidates);

  std::vector<ReadingHypothesis> corrections;
  corrections.reserve(candidates.candidates().size());
  for (auto& [reading, candidate] : candidates.candidates()) {
    ReadingHypothesis hypothesis;
    hypothesis.reading = reading;
    hypothesis.source = HypothesisSource::TypoCorrection;
    hypothesis.confidence = candidate.confidence;
    hypothesis.typo_type = candidate.type;
    hypothesis.pattern_id = std::move(candidate.pattern_id);
    hypothesis.personalized = candidate.personalized;
    hypothesis.cost = candidate.cost;
    hypothesis.edits = std::move(candidate.edits);
    hypothesis.edit_space = candidate.space;
    corrections.push_back(std::move(hypothesis));
  }
  std::stable_sort(corrections.begin(), corrections.end(),
                   [](const ReadingHypothesis& a, const ReadingHypothesis& b) {
                     return std::tie(a.cost, a.reading) < std::tie(b.cost, b.reading);
                   });
  if (corrections.size() > limit) corrections.resize(limit);
  for (ReadingHypothesis& hypothesis : corrections) {
    std::u32string reading;
    Decode(hypothesis.reading, reading);
    hypothesis.edit_distance = EditDistance(observed, reading);
    hypotheses.push_back(std::move(hypothesis));
  }
  return hypotheses;
}

}  // namespace azookey::correction
