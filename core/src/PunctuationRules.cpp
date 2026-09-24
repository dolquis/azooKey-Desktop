#include "azookey/core/PunctuationRules.h"

#include <charconv>
#include <cmath>
#include <iterator>
#include <string_view>
#include <system_error>
#include <utility>

#include "azookey/core/Utf8.h"

namespace azookey::core {
namespace {

std::string_view Trim(std::string_view value) {
  const auto first = value.find_first_not_of(" \t");
  if (first == std::string_view::npos) return {};
  const auto last = value.find_last_not_of(" \t");
  return value.substr(first, last - first + 1);
}

bool ValidMatch(std::string_view match) {
  if (match.empty() || match.find_first_of("\r\n\t") != std::string_view::npos) return false;
  size_t offset = 0;
  while (offset < match.size()) {
    char32_t cp{};
    if (!DecodeNextUtf8(match, offset, cp) || cp == U'、' || cp == U'。' || cp == U'，' ||
        cp == U'．')
      return false;
  }
  return true;
}

bool ParsePos(std::string_view name, SegmentPos& out) {
  constexpr std::string_view names[] = {
      "Unknown",  "Taigen",     "Yougen", "JoshiCase", "JoshiConj", "JoshiOther", "Setsuzoku",
      "Jodoushi", "HojoYougen", "Rentai", "Fukushi",   "Kigou",     "English"};
  for (size_t i = 0; i < std::size(names); ++i) {
    if (name == names[i]) {
      out = static_cast<SegmentPos>(i);
      return true;
    }
  }
  return false;
}

bool ParseSem(std::string_view name, SegmentSemantic& out) {
  constexpr std::string_view names[] = {"Unknown", "Generic",     "PersonName", "PlaceName",
                                        "OrgName", "ProductName", "DateTime",   "Number"};
  for (size_t i = 0; i < std::size(names); ++i) {
    if (name == names[i]) {
      out = static_cast<SegmentSemantic>(i);
      return true;
    }
  }
  return false;
}

bool ParseGuards(std::string_view text, std::vector<PunctuationGuard>& guards) {
  text = Trim(text);
  if (text.empty()) return true;
  while (true) {
    const auto separator = text.find(';');
    auto condition = Trim(text.substr(0, separator));
    if (condition.empty()) return false;
    PunctuationGuard guard;
    if (condition == "sentence_final") {
      guard.field = PunctuationGuard::Field::SentenceFinal;
    } else {
      const auto equals = condition.find('=');
      if (equals == std::string_view::npos) return false;
      guard.not_equal = equals != 0 && condition[equals - 1] == '!';
      const auto field = Trim(condition.substr(0, equals - (guard.not_equal ? 1 : 0)));
      const auto value = Trim(condition.substr(equals + 1));
      if (value.empty() || value.find_first_of("=!") != std::string_view::npos) return false;
      if (field == "prev_pos") {
        guard.field = PunctuationGuard::Field::PrevPos;
        if (!ParsePos(value, guard.pos)) return false;
      } else if (field == "next_head_pos") {
        guard.field = PunctuationGuard::Field::NextHeadPos;
        if (!ParsePos(value, guard.pos)) return false;
      } else if (field == "prev_sem") {
        guard.field = PunctuationGuard::Field::PrevSem;
        if (!ParseSem(value, guard.sem)) return false;
      } else if (field == "next_head_sem") {
        guard.field = PunctuationGuard::Field::NextHeadSem;
        if (!ParseSem(value, guard.sem)) return false;
      } else {
        return false;
      }
    }
    guards.push_back(guard);
    if (separator == std::string_view::npos) return true;
    text.remove_prefix(separator + 1);
  }
}

void Add(std::vector<PunctuationRule>& rules, PunctuationKind kind, std::string_view match,
         double score, std::string_view guard = {}) {
  PunctuationRule rule{kind, std::string(match), score, {}};
  ParseGuards(guard, rule.guards);
  rules.push_back(std::move(rule));
}

bool MatchPos(SegmentPos actual, SegmentPos expected, bool not_equal) {
  if (actual == SegmentPos::Unknown) return not_equal;
  return (actual == expected) != not_equal;
}

bool MatchSem(SegmentSemantic actual, SegmentSemantic expected, bool not_equal) {
  if (actual == SegmentSemantic::Unknown) return not_equal;
  return (actual == expected) != not_equal;
}

}  // namespace

PunctuationRules PunctuationRules::Default() {
  PunctuationRules result;
  auto& rules = result.rules_;
  for (const auto match : {"ので", "から", "ため"}) Add(rules, PunctuationKind::Comma, match, 0.80);
  for (const auto match : {"けど", "けれど", "のに", "ても", "でも"})
    Add(rules, PunctuationKind::Comma, match, 0.85);
  Add(rules, PunctuationKind::Comma, "が", 0.85, "prev_pos=JoshiConj");
  for (const auto match : {"て", "で"})
    Add(rules, PunctuationKind::Comma, match, 0.60, "next_head_pos!=HojoYougen");
  for (const auto match : {"し", "たり"}) Add(rules, PunctuationKind::Comma, match, 0.60);
  for (const auto match : {"ば", "なら", "たら"}) Add(rules, PunctuationKind::Comma, match, 0.70);
  Add(rules, PunctuationKind::Comma, "と", 0.70, "next_head_pos!=Taigen");
  for (const auto match : {"とき", "ところ", "場合", "うえで"})
    Add(rules, PunctuationKind::Comma, match, 0.65);
  for (const auto match : {"しかし", "だから", "また", "そして", "ただし", "つまり"})
    Add(rules, PunctuationKind::Comma, match, 0.90, "prev_pos=Setsuzoku");
  for (const auto match : {"です", "ます", "である", "した", "ない", "だ", "か"})
    Add(rules, PunctuationKind::Period, match, 1.0, "sentence_final");
  return result;
}

PunctuationRules PunctuationRules::ParseAndMerge(std::string_view tsv,
                                                 std::vector<size_t>* invalid_lines) {
  auto result = Default();
  if (tsv.starts_with("\xef\xbb\xbf")) tsv.remove_prefix(3);
  size_t line_number = 0;
  while (!tsv.empty()) {
    ++line_number;
    const auto end = tsv.find('\n');
    auto line = tsv.substr(0, end);
    tsv.remove_prefix(end == std::string_view::npos ? tsv.size() : end + 1);
    if (line.ends_with('\r')) line.remove_suffix(1);
    if (line_number == 1 && line.starts_with("# version:")) {
      if (Trim(line.substr(10)) != "1" && invalid_lines) invalid_lines->push_back(line_number);
      continue;
    }
    if (Trim(line).empty() || line.starts_with('#')) continue;
    const auto first = line.find('\t');
    const auto second = first == std::string_view::npos ? first : line.find('\t', first + 1);
    const auto third = second == std::string_view::npos ? second : line.find('\t', second + 1);
    const bool malformed =
        first == std::string_view::npos || second == std::string_view::npos ||
        (third != std::string_view::npos && line.find('\t', third + 1) != std::string_view::npos);
    if (malformed) {
      if (invalid_lines) invalid_lines->push_back(line_number);
      continue;
    }
    const auto kind_text = line.substr(0, first);
    const auto match = line.substr(first + 1, second - first - 1);
    const auto score_text = line.substr(second + 1, third - second - 1);
    const auto guard_text =
        third == std::string_view::npos ? std::string_view{} : line.substr(third + 1);
    double score{};
    const auto parsed =
        std::from_chars(score_text.data(), score_text.data() + score_text.size(), score);
    PunctuationRule rule;
    if (kind_text == "comma")
      rule.kind = PunctuationKind::Comma;
    else if (kind_text == "period")
      rule.kind = PunctuationKind::Period;
    else {
      if (invalid_lines) invalid_lines->push_back(line_number);
      continue;
    }
    rule.match = std::string(match);
    rule.base_score = score;
    if (!ValidMatch(match) || parsed.ec != std::errc{} ||
        parsed.ptr != score_text.data() + score_text.size() || !std::isfinite(score) ||
        score < 0.0 || score > 1.0 || !ParseGuards(guard_text, rule.guards)) {
      if (invalid_lines) invalid_lines->push_back(line_number);
      continue;
    }
    bool replaced = false;
    for (auto& existing : result.rules_) {
      if (existing.kind == rule.kind && existing.match == rule.match) {
        existing = std::move(rule);
        replaced = true;
        break;
      }
    }
    if (!replaced) result.rules_.push_back(std::move(rule));
  }
  return result;
}

bool PunctuationRules::MatchesGuard(const PunctuationRule& rule, SegmentPos prev_pos,
                                    SegmentSemantic prev_sem, SegmentPos next_head_pos,
                                    SegmentSemantic next_head_sem, bool sentence_final) {
  for (const auto& guard : rule.guards) {
    bool matches = false;
    switch (guard.field) {
      case PunctuationGuard::Field::PrevPos:
        matches = MatchPos(prev_pos, guard.pos, guard.not_equal);
        break;
      case PunctuationGuard::Field::NextHeadPos:
        matches = MatchPos(next_head_pos, guard.pos, guard.not_equal);
        break;
      case PunctuationGuard::Field::PrevSem:
        matches = MatchSem(prev_sem, guard.sem, guard.not_equal);
        break;
      case PunctuationGuard::Field::NextHeadSem:
        matches = MatchSem(next_head_sem, guard.sem, guard.not_equal);
        break;
      case PunctuationGuard::Field::SentenceFinal:
        matches = sentence_final;
        break;
    }
    if (!matches) return false;
  }
  return true;
}

}  // namespace azookey::core
