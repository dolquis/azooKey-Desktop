#include "azookey/core/RomajiKanaConverter.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <queue>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>

#include "azookey/core/QwertyAdjacency.h"
#include "azookey/core/Utf8.h"

namespace azookey::core {
namespace {

// Mirrors the legacy default table (AzooKeyKanaKanjiConverter defaultRomanToKanaMap, MIT;
// see THIRD_PARTY_LICENSES).
// Doubled consonants, "nn" and the "ny" exception are handled by Feed instead.
const std::unordered_map<std::string, std::string> kRomajiMap = {
    {"a", "あ"},     {"xa", "ぁ"},    {"la", "ぁ"},    {"i", "い"},     {"xi", "ぃ"},
    {"li", "ぃ"},    {"u", "う"},     {"wu", "う"},    {"vu", "ゔ"},    {"xu", "ぅ"},
    {"lu", "ぅ"},    {"e", "え"},     {"xe", "ぇ"},    {"le", "ぇ"},    {"o", "お"},
    {"xo", "ぉ"},    {"lo", "ぉ"},    {"ka", "か"},    {"ca", "か"},    {"ga", "が"},
    {"xka", "ゕ"},   {"lka", "ゕ"},   {"ki", "き"},    {"gi", "ぎ"},    {"ku", "く"},
    {"cu", "く"},    {"gu", "ぐ"},    {"ke", "け"},    {"ge", "げ"},    {"xke", "ゖ"},
    {"lke", "ゖ"},   {"ko", "こ"},    {"co", "こ"},    {"go", "ご"},    {"sa", "さ"},
    {"za", "ざ"},    {"si", "し"},    {"ci", "し"},    {"shi", "し"},   {"zi", "じ"},
    {"ji", "じ"},    {"su", "す"},    {"zu", "ず"},    {"se", "せ"},    {"ce", "せ"},
    {"ze", "ぜ"},    {"so", "そ"},    {"zo", "ぞ"},    {"ta", "た"},    {"da", "だ"},
    {"ti", "ち"},    {"chi", "ち"},   {"di", "ぢ"},    {"tu", "つ"},    {"tsu", "つ"},
    {"xtu", "っ"},   {"ltu", "っ"},   {"xtsu", "っ"},  {"ltsu", "っ"},  {"du", "づ"},
    {"te", "て"},    {"de", "で"},    {"to", "と"},    {"do", "ど"},    {"na", "な"},
    {"ni", "に"},    {"nu", "ぬ"},    {"ne", "ね"},    {"no", "の"},    {"ha", "は"},
    {"ba", "ば"},    {"pa", "ぱ"},    {"hi", "ひ"},    {"bi", "び"},    {"pi", "ぴ"},
    {"hu", "ふ"},    {"fu", "ふ"},    {"bu", "ぶ"},    {"pu", "ぷ"},    {"he", "へ"},
    {"be", "べ"},    {"pe", "ぺ"},    {"ho", "ほ"},    {"bo", "ぼ"},    {"po", "ぽ"},
    {"ma", "ま"},    {"mi", "み"},    {"mu", "む"},    {"me", "め"},    {"mo", "も"},
    {"ya", "や"},    {"xya", "ゃ"},   {"lya", "ゃ"},   {"yu", "ゆ"},    {"xyu", "ゅ"},
    {"lyu", "ゅ"},   {"yo", "よ"},    {"xyo", "ょ"},   {"lyo", "ょ"},   {"ra", "ら"},
    {"ri", "り"},    {"ru", "る"},    {"re", "れ"},    {"ro", "ろ"},    {"wa", "わ"},
    {"xwa", "ゎ"},   {"lwa", "ゎ"},   {"wyi", "ゐ"},   {"wye", "ゑ"},   {"wo", "を"},
    {"ye", "いぇ"},  {"va", "ゔぁ"},  {"vi", "ゔぃ"},  {"ve", "ゔぇ"},  {"vo", "ゔぉ"},
    {"kya", "きゃ"}, {"kyu", "きゅ"}, {"kye", "きぇ"}, {"kyo", "きょ"}, {"gya", "ぎゃ"},
    {"gyu", "ぎゅ"}, {"gye", "ぎぇ"}, {"gyo", "ぎょ"}, {"qa", "くぁ"},  {"kwa", "くぁ"},
    {"qwa", "くぁ"}, {"qi", "くぃ"},  {"kwi", "くぃ"}, {"qwi", "くぃ"}, {"qu", "くぅ"},
    {"kwu", "くぅ"}, {"qwu", "くぅ"}, {"qe", "くぇ"},  {"kwe", "くぇ"}, {"qwe", "くぇ"},
    {"qo", "くぉ"},  {"kwo", "くぉ"}, {"qwo", "くぉ"}, {"gwa", "ぐぁ"}, {"gwi", "ぐぃ"},
    {"gwu", "ぐぅ"}, {"gwe", "ぐぇ"}, {"gwo", "ぐぉ"}, {"sha", "しゃ"}, {"sya", "しゃ"},
    {"shu", "しゅ"}, {"syu", "しゅ"}, {"she", "しぇ"}, {"sye", "しぇ"}, {"sho", "しょ"},
    {"syo", "しょ"}, {"ja", "じゃ"},  {"zya", "じゃ"}, {"jya", "じゃ"}, {"jyi", "じぃ"},
    {"ju", "じゅ"},  {"zyu", "じゅ"}, {"jyu", "じゅ"}, {"je", "じぇ"},  {"zye", "じぇ"},
    {"jye", "じぇ"}, {"jo", "じょ"},  {"zyo", "じょ"}, {"jyo", "じょ"}, {"swa", "すぁ"},
    {"swi", "すぃ"}, {"swu", "すぅ"}, {"swe", "すぇ"}, {"swo", "すぉ"}, {"cha", "ちゃ"},
    {"cya", "ちゃ"}, {"tya", "ちゃ"}, {"tyi", "ちぃ"}, {"cyi", "ちぃ"}, {"chu", "ちゅ"},
    {"cyu", "ちゅ"}, {"tyu", "ちゅ"}, {"che", "ちぇ"}, {"cye", "ちぇ"}, {"tye", "ちぇ"},
    {"cho", "ちょ"}, {"cyo", "ちょ"}, {"tyo", "ちょ"}, {"tsa", "つぁ"}, {"tsi", "つぃ"},
    {"tse", "つぇ"}, {"tso", "つぉ"}, {"tha", "てゃ"}, {"thi", "てぃ"}, {"thu", "てゅ"},
    {"the", "てぇ"}, {"tho", "てょ"}, {"twa", "とぁ"}, {"twi", "とぃ"}, {"twu", "とぅ"},
    {"twe", "とぇ"}, {"two", "とぉ"}, {"dya", "ぢゃ"}, {"dyi", "ぢぃ"}, {"dyu", "ぢゅ"},
    {"dye", "ぢぇ"}, {"dyo", "ぢょ"}, {"dha", "でゃ"}, {"dhi", "でぃ"}, {"dhu", "でゅ"},
    {"dhe", "でぇ"}, {"dho", "でょ"}, {"dwa", "どぁ"}, {"dwi", "どぃ"}, {"dwu", "どぅ"},
    {"dwe", "どぇ"}, {"dwo", "どぉ"}, {"nya", "にゃ"}, {"nyi", "にぃ"}, {"nyu", "にゅ"},
    {"nye", "にぇ"}, {"nyo", "にょ"}, {"hya", "ひゃ"}, {"hyi", "ひぃ"}, {"hyu", "ひゅ"},
    {"hye", "ひぇ"}, {"hyo", "ひょ"}, {"bya", "びゃ"}, {"byi", "びぃ"}, {"byu", "びゅ"},
    {"bye", "びぇ"}, {"byo", "びょ"}, {"pya", "ぴゃ"}, {"pyi", "ぴぃ"}, {"pyu", "ぴゅ"},
    {"pye", "ぴぇ"}, {"pyo", "ぴょ"}, {"fa", "ふぁ"},  {"hwa", "ふぁ"}, {"fwa", "ふぁ"},
    {"fi", "ふぃ"},  {"hwi", "ふぃ"}, {"fwi", "ふぃ"}, {"fwu", "ふぅ"}, {"fe", "ふぇ"},
    {"hwe", "ふぇ"}, {"fwe", "ふぇ"}, {"fo", "ふぉ"},  {"hwo", "ふぉ"}, {"fwo", "ふぉ"},
    {"fya", "ふゃ"}, {"fyu", "ふゅ"}, {"fyo", "ふょ"}, {"mya", "みゃ"}, {"myi", "みぃ"},
    {"myu", "みゅ"}, {"mye", "みぇ"}, {"myo", "みょ"}, {"rya", "りゃ"}, {"ryi", "りぃ"},
    {"ryu", "りゅ"}, {"rye", "りぇ"}, {"ryo", "りょ"}, {"wi", "うぃ"},  {"we", "うぇ"},
    {"wha", "うぁ"}, {"whi", "うぃ"}, {"whu", "う"},   {"whe", "うぇ"}, {"who", "うぉ"},
    {"xn", "ん"},    {"zh", "←"},     {"zj", "↓"},     {"zk", "↑"},     {"zl", "→"}};

constexpr size_t kMaxRomajiKeyLength = 4;

// True when a longer built-in key still starts with `pending` (e.g. "xts" for "xtsu").
bool IsBuiltinRomajiPrefix(const std::string& pending) {
  return std::any_of(kRomajiMap.begin(), kRomajiMap.end(), [&](const auto& entry) {
    return entry.first.size() > pending.size() && entry.first.starts_with(pending);
  });
}

char AsciiLower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

std::string AsciiLower(std::string_view text) {
  std::string lower(text);
  for (char& c : lower) c = AsciiLower(c);
  return lower;
}

// Custom and AZIK tables keep letter case in their rules (docs/legacy-parity-spec.md section
// 5.1). A key matches the rule with the same case first, then the rule of its lowercase form,
// so a Shift-typed letter falls back to the lowercase rule when no capital rule exists.
const CustomRomajiRule* FindCustomRule(const CustomRomajiTable& table, std::string_view key) {
  if (const auto exact = table.find(key); exact != table.end()) return &exact->second;
  const std::string lower = AsciiLower(key);
  if (lower == key) return nullptr;
  const auto folded = table.find(lower);
  return folded != table.end() ? &folded->second : nullptr;
}

// Whether a rule key starts with `prefix`. `strictly_longer` ignores a key equal to the prefix.
bool HasKeyWithPrefix(const CustomRomajiTable& table, std::string_view prefix,
                      bool strictly_longer) {
  const auto it = strictly_longer ? table.upper_bound(prefix) : table.lower_bound(prefix);
  return it != table.end() && it->first.starts_with(prefix);
}

// The table can still continue `prefix`: through a rule with the same case, or through the
// lowercase form that a Shift-typed letter falls back to.
bool CanExtendCustomPrefix(const CustomRomajiTable& table, std::string_view prefix,
                           bool strictly_longer) {
  if (HasKeyWithPrefix(table, prefix, strictly_longer)) return true;
  const std::string lower = AsciiLower(prefix);
  return lower != prefix && HasKeyWithPrefix(table, lower, strictly_longer);
}

bool IsConsonant(char c) {
  const std::string vowels = "aeiou";
  return std::isalpha(static_cast<unsigned char>(c)) != 0 && vowels.find(c) == std::string::npos;
}

bool IsVowelOrY(char c) {
  const std::string set = "aeiouy";
  return set.find(c) != std::string::npos;
}

// The built-in keys in byte order, so a prefix lookup is a binary search.
const std::vector<std::string>& SortedBuiltinKeys() {
  static const std::vector<std::string> keys = [] {
    std::vector<std::string> sorted;
    sorted.reserve(kRomajiMap.size());
    for (const auto& entry : kRomajiMap) sorted.push_back(entry.first);
    std::sort(sorted.begin(), sorted.end());
    return sorted;
  }();
  return keys;
}

bool HasBuiltinKeyStartingWith(std::string_view prefix) {
  const auto& keys = SortedBuiltinKeys();
  const auto it = std::lower_bound(
      keys.begin(), keys.end(), prefix,
      [](const std::string& key, std::string_view value) { return std::string_view(key) < value; });
  return it != keys.end() && std::string_view(*it).starts_with(prefix);
}

// Hiragana U+3041-U+3096 and the prolonged sound mark U+30FC only, so a
// reading that kept a letter, or a table symbol such as the arrow of "zj", is
// not a kana interpretation. Empty text passes.
bool IsKanaText(std::string_view text) {
  size_t offset = 0;
  while (offset < text.size()) {
    char32_t codepoint = 0;
    if (!DecodeNextUtf8(text, offset, codepoint)) return false;
    if (!((codepoint >= 0x3041 && codepoint <= 0x3096) || codepoint == 0x30FC)) return false;
  }
  return true;
}

bool IsFuzzyInputCharacter(char c) { return (c >= 'a' && c <= 'z') || c == '-' || c == '\''; }

}  // namespace

std::string RomajiKanaConverter::Feed(char ascii) {
  if (custom_table_) {
    // The caller decides the letter case from Shift; the table lookup folds it as needed.
    pending_.push_back(ascii);
    return ConvertCustomPending(false);
  }
  const char lower = AsciiLower(ascii);
  // 長音: '-' キーは長音符「ー」に変換する（MS-IME 標準のローマ字挙動）。
  // 直前の未確定ローマ字があれば先に確定してから長音符を付与する。
  if (ascii == '-') {
    std::string out = ConvertPending(true);
    out += "ー";
    return out;
  }
  // n' / nn' end the syllable at ん and consume the apostrophe, so "gen'inn" reads
  // げんいん (Mozc's convention; the legacy table has no such rule).
  if (ascii == '\'' && SeparatesNWithApostrophe()) {
    pending_.clear();
    return "ん";
  }
  if (!std::isalpha(static_cast<unsigned char>(lower))) {
    std::string out = ConvertPending(true);
    out.push_back(ascii);
    return out;
  }

  if (pending_ == "nn") {
    pending_.clear();
    if (IsVowelOrY(lower)) {
      const size_t position = ambiguous_double_n_seen_++;
      const bool consume = position < 32 && ((consume_double_n_mask_ >> position) & 1u) != 0;
      if (!consume) pending_.push_back('n');
    }
    pending_.push_back(lower);
    return "ん" + ConvertPending(false);
  }

  if (pending_.size() == 1 && pending_[0] == 'n' && !IsVowelOrY(lower) && lower != 'n') {
    std::string out = "ん";
    pending_.clear();
    pending_.push_back(lower);
    out += ConvertPending(false);
    return out;
  }

  if (pending_.size() == 1 && pending_[0] == lower && IsConsonant(lower) && lower != 'n') {
    return "っ";
  }

  pending_.push_back(lower);
  return ConvertPending(false);
}

std::string RomajiKanaConverter::Flush() { return ConvertPending(true); }

void RomajiKanaConverter::Reset() { pending_.clear(); }

void RomajiKanaConverter::SetCustomTable(std::shared_ptr<const CustomRomajiTable> table) {
  custom_table_ = std::move(table);
}

bool RomajiKanaConverter::CanContinueCustomWith(char ascii) const {
  if (!custom_table_ || static_cast<unsigned char>(ascii) > 0x7f) return false;
  std::string prefix = pending_;
  prefix.push_back(ascii);
  const auto starts_rule = [&](std::string_view candidate) {
    return CanExtendCustomPrefix(*custom_table_, candidate, false);
  };
  return starts_rule(prefix) || (!pending_.empty() && starts_rule(std::string_view(&ascii, 1)));
}

void RomajiKanaConverter::PopPendingPreview() {
  if (pending_.empty()) return;
  const std::string previous_preview = PreviewPending();
  do {
    pending_.pop_back();
  } while (!pending_.empty() && PreviewPending() == previous_preview);
}

std::string RomajiKanaConverter::PreviewPending() const {
  if (!custom_table_ && (pending_ == "n" || pending_ == "nn")) {
    return "ん";
  }
  // A letter that matches no rule stays in the reading in lowercase.
  return custom_table_ ? AsciiLower(pending_) : pending_;
}

std::string RomajiKanaConverter::Preview(const std::string& ascii) {
  return Preview(ascii, nullptr);
}

std::string RomajiKanaConverter::Preview(const std::string& ascii,
                                         std::shared_ptr<const CustomRomajiTable> table) {
  RomajiKanaConverter converter;
  converter.SetCustomTable(std::move(table));
  std::string output;
  for (char raw : ascii) {
    output += converter.Feed(raw);
  }

  output += converter.PreviewPending();
  return output;
}

std::string RomajiKanaConverter::ConvertForCommit(const std::string& ascii) {
  return ConvertForCommit(ascii, nullptr);
}

std::string RomajiKanaConverter::ConvertForCommit(const std::string& ascii,
                                                  std::shared_ptr<const CustomRomajiTable> table) {
  RomajiKanaConverter converter;
  converter.SetCustomTable(std::move(table));
  std::string output;
  for (char raw : ascii) {
    output += converter.Feed(raw);
  }
  output += converter.Flush();
  return output;
}

std::vector<std::string> RomajiKanaConverter::AmbiguousReadings(const std::string& ascii,
                                                                size_t max_ambiguous) {
  return AmbiguousReadings(ascii, nullptr, max_ambiguous);
}

std::vector<std::string> RomajiKanaConverter::AmbiguousReadings(
    const std::string& ascii, std::shared_ptr<const CustomRomajiTable> table,
    size_t max_ambiguous) {
  const auto convert = [&](unsigned mask, size_t* ambiguous) {
    RomajiKanaConverter converter;
    converter.SetCustomTable(table);
    converter.consume_double_n_mask_ = mask;
    std::string output;
    for (char raw : ascii) {
      output += converter.Feed(raw);
    }
    output += converter.Flush();
    if (ambiguous) *ambiguous = converter.ambiguous_double_n_seen_;
    return output;
  };
  const auto ascii_letters = [](const std::string& text) {
    return std::count_if(text.begin(), text.end(),
                         [](char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; });
  };

  size_t ambiguous = 0;
  std::vector<std::string> readings{convert(0, &ambiguous)};
  const size_t varied = (std::min)({ambiguous, max_ambiguous, kMaxAmbiguousDoubleN});
  // A position varies only if reading it alone as ん + あ行/や行 converts as many letters
  // as the default does (a trailing "nny" or "nnyi" leaves romaji behind instead).
  const auto default_letters = ascii_letters(readings.front());
  unsigned valid = 0;
  for (size_t position = 0; position < varied; ++position) {
    const unsigned bit = 1u << position;
    if (ascii_letters(convert(bit, nullptr)) == default_letters) valid |= bit;
  }
  // Each position changes only its own syllable, so distinct masks give distinct readings.
  for (unsigned mask = 1; mask < (1u << varied); ++mask) {
    if ((mask & ~valid) != 0) continue;
    readings.push_back(convert(mask, nullptr));
  }
  return readings;
}

std::vector<FuzzyKanaCandidate> RomajiKanaConverter::FuzzyMatch(
    std::string_view input, size_t k, FuzzySubstitutions substitutions) const {
  if (custom_table_ || k == 0 || input.empty() || input.size() > kFuzzyMaxInputLength) return {};
  if (!std::all_of(input.begin(), input.end(), IsFuzzyInputCharacter)) return {};
  const std::string standard = ConvertForCommit(std::string(input));

  // A state of the weighted edit graph: `romaji` is the edited key sequence fed
  // so far, `position` the next input key, `kana` what `converter` has output.
  // CheapEdits and CostlyEdits stand for the edit successors of the state they
  // copy, at the cost of their cheapest successor: the cheap ones are a
  // transposition and substitutions of adjacent or same-finger keys, the costly
  // ones the other substitutions, insertions and deletions. Successors are
  // built only when their group comes up, so those that cost more than the k-th
  // reading are never built. A Finished state holds the flushed reading.
  enum class Step : unsigned char { Expand, CheapEdits, CostlyEdits, Finished };
  struct State {
    int cost = 0;
    std::string romaji;
    size_t position = 0;
    size_t edits = 0;
    Step step = Step::Expand;
    std::string kana;
    RomajiKanaConverter converter;
  };
  static_assert(kQwertyTranspositionCost <= kQwertySameFingerSubstitutionCost &&
                    kQwertySameFingerSubstitutionCost <= kQwertyAdjacentSubstitutionCost &&
                    kQwertyAdjacentSubstitutionCost < kQwertyBaseEditCost,
                "a group of successors must not cost more than any successor in it");

  std::map<std::string, std::pair<int, std::string>> found;  // kana -> (cost, romaji)
  int kth_cost = -1;
  // The queue holds indexes into `states`, so popping copies nothing. Cheapest
  // first; ties go by romaji so the order does not depend on the standard library.
  std::vector<State> states;
  const auto later = [&states](size_t a, size_t b) {
    const State& left = states[a];
    const State& right = states[b];
    return std::tie(left.cost, left.romaji, left.position, left.edits, left.step) >
           std::tie(right.cost, right.romaji, right.position, right.edits, right.step);
  };
  std::priority_queue<size_t, std::vector<size_t>, decltype(later)> queue(later);
  const auto push = [&](State state) {
    // Nothing above the k-th reading's cost is kept, and a reading already
    // found was found at a lower or equal cost.
    if (kth_cost >= 0 && state.cost > kth_cost) return;
    if (state.step == Step::Finished && found.count(state.kana) != 0) return;
    states.push_back(std::move(state));
    queue.push(states.size() - 1);
  };
  // Queues a state to expand. One that may not edit again only reads the rest
  // of the input, so it is read to the end and queued as finished at once.
  const auto push_expand = [&](State state) {
    if (state.edits >= kFuzzyMaxEdits) {
      std::string out;
      for (size_t index = state.position; index < input.size(); ++index) {
        out += state.converter.Feed(input[index]);
        if (!IsKanaText(out) || !state.converter.PendingCanContinue()) return;
      }
      state.romaji.append(input.substr(state.position));
      state.kana += out + state.converter.Flush();
      state.position = input.size();
      state.step = Step::Finished;
    }
    push(std::move(state));
  };
  // Feeds `keys` after state `from` and queues the result while the keys can
  // still become kana.
  const auto feed = [&](size_t from, std::string_view keys, size_t position, size_t edits,
                        int cost) {
    if (kth_cost >= 0 && cost > kth_cost) return;
    RomajiKanaConverter converter = states[from].converter;
    std::string out;
    for (char key : keys) {
      out += converter.Feed(key);
      if (!IsKanaText(out) || !converter.PendingCanContinue()) return;
    }
    push_expand(State{cost, states[from].romaji + std::string(keys), position, edits, Step::Expand,
                      states[from].kana + out, std::move(converter)});
  };
  // Queues the group of successors of state `from` at the cost of its cheapest one.
  const auto push_group = [&](size_t from, Step step, int extra_cost) {
    State group = states[from];
    group.step = step;
    group.cost += extra_cost;
    push(std::move(group));
  };
  push(State{});

  std::set<std::tuple<size_t, size_t, std::string>> expanded;
  size_t steps = 0;
  while (!queue.empty() && steps++ < kFuzzyNodeBudget) {
    // `states` grows while a state is handled, so it is read by index throughout.
    const size_t index = queue.top();
    queue.pop();
    const int cost = states[index].cost;
    // Keep the readings that tie with the k-th one, then pick by kana below.
    if (kth_cost >= 0 && cost > kth_cost) break;
    const Step step = states[index].step;
    const size_t position = states[index].position;
    const size_t edits = states[index].edits;
    const bool at_end = position == input.size();

    if (step == Step::Finished) {
      const std::string& kana = states[index].kana;
      if (kana != standard && !kana.empty() && IsKanaText(kana) &&
          found.emplace(kana, std::pair{cost, states[index].romaji}).second && found.size() == k) {
        kth_cost = cost;
      }
      continue;
    }

    if (step == Step::Expand) {
      if (!expanded.emplace(position, edits, states[index].romaji).second) continue;
      if (at_end) {
        State finished = states[index];
        finished.kana += finished.converter.Flush();
        finished.step = Step::Finished;
        push(std::move(finished));
      } else {
        feed(index, input.substr(position, 1), position + 1, edits, cost);
      }
      if (edits < kFuzzyMaxEdits) {
        if (!at_end) push_group(index, Step::CheapEdits, kQwertyTranspositionCost);
        push_group(index, Step::CostlyEdits, kQwertyBaseEditCost);
      }
      continue;
    }

    const size_t next_edits = edits + 1;
    const char typed = at_end ? '\0' : input[position];
    const bool typed_letter = typed >= 'a' && typed <= 'z';
    if (step == Step::CheapEdits) {
      const int base = cost - kQwertyTranspositionCost;
      if (position + 1 < input.size() && input[position + 1] != typed) {
        const char swapped[2] = {input[position + 1], typed};
        feed(index, std::string_view(swapped, 2), position + 2, next_edits,
             base + kQwertyTranspositionCost);
      }
      for (char key = 'a'; typed_letter && key <= 'z'; ++key) {
        const int substitution = QwertySubstitutionCost(typed, key);
        if (key == typed || substitution >= kQwertyBaseEditCost) continue;
        feed(index, std::string_view(&key, 1), position + 1, next_edits, base + substitution);
      }
      continue;
    }

    // Step::CostlyEdits.
    const int base = cost - kQwertyBaseEditCost;
    for (char key = 'a'; key <= 'z'; ++key) {
      feed(index, std::string_view(&key, 1), position, next_edits, base + kQwertyBaseEditCost);
    }
    constexpr char kApostrophe = '\'';
    feed(index, std::string_view(&kApostrophe, 1), position, next_edits,
         base + kQwertyBaseEditCost);
    if (at_end) continue;
    State skipped = states[index];
    skipped.step = Step::Expand;
    skipped.position = position + 1;
    skipped.edits = next_edits;
    skipped.cost = base + kQwertyBaseEditCost;
    push_expand(std::move(skipped));
    const bool far_keys = substitutions == FuzzySubstitutions::All;
    for (char key = 'a'; typed_letter && far_keys && key <= 'z'; ++key) {
      if (key == typed || QwertySubstitutionCost(typed, key) < kQwertyBaseEditCost) continue;
      feed(index, std::string_view(&key, 1), position + 1, next_edits, base + kQwertyBaseEditCost);
    }
  }

  std::vector<FuzzyKanaCandidate> candidates;
  candidates.reserve(found.size());
  for (const auto& [kana, entry] : found) {
    candidates.push_back(
        {kana, static_cast<double>(entry.first) / kQwertyEditCostScale, entry.second});
  }
  std::stable_sort(
      candidates.begin(), candidates.end(),
      [](const FuzzyKanaCandidate& a, const FuzzyKanaCandidate& b) { return a.cost < b.cost; });
  if (candidates.size() > k) candidates.resize(k);
  return candidates;
}

bool RomajiKanaConverter::PendingCanContinue() const {
  return pending_.empty() || pending_ == "nn" || HasBuiltinKeyStartingWith(pending_);
}

std::string RomajiKanaConverter::ConvertPending(bool force_flush) {
  if (custom_table_) return ConvertCustomPending(force_flush);
  std::string output;
  while (!pending_.empty()) {
    bool matched = false;
    const size_t n = std::min<size_t>(kMaxRomajiKeyLength, pending_.size());
    for (size_t len = n; len > 0; --len) {
      const std::string chunk = pending_.substr(0, len);
      auto it = kRomajiMap.find(chunk);
      if (it != kRomajiMap.end()) {
        output += it->second;
        pending_.erase(0, len);
        matched = true;
        break;
      }
    }
    if (!matched) {
      if (force_flush && pending_ == "nn") {
        output += "ん";
        pending_.clear();
        continue;
      }
      if (force_flush && pending_ == "n") {
        output += "ん";
        pending_.clear();
        continue;
      }
      if (force_flush || (pending_.size() >= 3 && !IsBuiltinRomajiPrefix(pending_))) {
        output.push_back(pending_.front());
        pending_.erase(0, 1);
      } else {
        break;
      }
    }
  }
  return output;
}

std::string RomajiKanaConverter::ConvertCustomPending(bool force_flush) {
  std::string output;
  while (!pending_.empty()) {
    const CustomRomajiRule* best = nullptr;
    for (size_t length = (std::min)(pending_.size(), size_t{8}); length > 0; --length) {
      best = FindCustomRule(*custom_table_, std::string_view(pending_.data(), length));
      if (best) break;
    }
    // A longer rule with the same case wins over the lowercase rule of a shorter match, so
    // wait for the next key (docs/legacy-parity-spec.md section 5.1).
    if (!force_flush && CanExtendCustomPrefix(*custom_table_, pending_, true)) break;
    if (best) {
      output += best->output;
      pending_.erase(0, best->consume);
    } else {
      output.push_back(AsciiLower(pending_.front()));
      pending_.erase(0, 1);
    }
  }
  return output;
}

}  // namespace azookey::core
