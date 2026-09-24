#include "azookey/host/PunctuationInserter.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include "azookey/core/PlatformPaths.h"
#include "azookey/core/Utf8.h"
#include "azookey/logging/RuntimeLogger.h"

namespace azookey::host {
namespace {

bool EndsWith(std::string_view value, std::string_view suffix) {
  return value.size() >= suffix.size() && value.substr(value.size() - suffix.size()) == suffix;
}

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.substr(0, prefix.size()) == prefix;
}

uint32_t Utf16Length(std::string_view value) {
  uint32_t length = 0;
  size_t offset = 0;
  char32_t codepoint = 0;
  while (offset < value.size()) {
    core::DecodeNextUtf8(value, offset, codepoint);
    length += codepoint > 0xFFFF ? 2 : 1;
  }
  return length;
}

size_t CodepointLength(std::string_view value) {
  size_t length = 0;
  size_t offset = 0;
  char32_t codepoint = 0;
  while (offset < value.size()) {
    core::DecodeNextUtf8(value, offset, codepoint);
    ++length;
  }
  return length;
}

bool IsPunctuationEnd(std::string_view value) {
  for (const auto suffix : {"、", "。", "，", "．", ",", ".", "!", "?", "！", "？"})
    if (EndsWith(value, suffix)) return true;
  return false;
}

bool IsPunctuationStart(std::string_view value) {
  for (const auto prefix : {"、", "。", "，", "．", ",",  ".", "!",  "?",  "！", "？",
                            "」", "』", "）", ")",  "】", "]", "〉", "》", "｝", "}"})
    if (StartsWith(value, prefix)) return true;
  return false;
}

bool IsAsciiWord(std::string_view value) {
  return !value.empty() && std::all_of(value.begin(), value.end(),
                                       [](unsigned char c) { return c >= 0x21 && c <= 0x7E; });
}

bool RecentComma(std::string_view value) {
  size_t offset = 0;
  char32_t codepoint = 0;
  std::vector<char32_t> chars;
  while (offset < value.size()) {
    core::DecodeNextUtf8(value, offset, codepoint);
    chars.push_back(codepoint);
  }
  const auto start = chars.size() > 6 ? chars.size() - 6 : 0;
  for (size_t i = start; i < chars.size(); ++i)
    if (chars[i] == U'、' || chars[i] == U'，') return true;
  return false;
}

core::SegmentPos AsPos(uint8_t raw) {
  return raw <= static_cast<uint8_t>(core::SegmentPos::English) ? static_cast<core::SegmentPos>(raw)
                                                                : core::SegmentPos::Unknown;
}

core::SegmentSemantic AsSem(uint8_t raw) {
  return raw <= static_cast<uint8_t>(core::SegmentSemantic::Number)
             ? static_cast<core::SegmentSemantic>(raw)
             : core::SegmentSemantic::Unknown;
}

core::SegmentPos InferPrevPos(const ipc::LiveSegment& segment) {
  const auto value = std::string_view(segment.surface);
  if (EndsWith(value, "しかし") || EndsWith(value, "だから") || EndsWith(value, "そして") ||
      EndsWith(value, "ただし") || EndsWith(value, "つまり") || EndsWith(value, "また"))
    return core::SegmentPos::Setsuzoku;
  if (EndsWith(value, "が")) {
    const auto stem = value.substr(0, value.size() - std::string_view("が").size());
    for (const auto suffix : {"た", "だ", "る", "い", "う", "す", "く", "む", "ぬ", "ぶ", "つ"})
      if (EndsWith(stem, suffix)) return core::SegmentPos::JoshiConj;
    return core::SegmentPos::JoshiCase;
  }
  return core::SegmentPos::Unknown;
}

core::SegmentPos InferNextHeadPos(const ipc::LiveSegment& segment) {
  const auto value = std::string_view(segment.surface);
  for (const auto prefix :
       {"いる", "ある", "おく", "みる", "しまう", "くれる", "もらう", "いく", "くる"})
    if (StartsWith(value, prefix)) return core::SegmentPos::HojoYougen;
  for (const auto prefix : {"思", "言", "考", "聞", "話", "呼", "書"})
    if (StartsWith(value, prefix)) return core::SegmentPos::Yougen;
  return core::SegmentPos::Unknown;
}

const core::PunctuationRule* BestRule(const core::PunctuationRules& rules,
                                      core::PunctuationKind kind, std::string_view surface) {
  const core::PunctuationRule* best = nullptr;
  for (const auto& rule : rules.rules()) {
    if (rule.kind != kind || !EndsWith(surface, rule.match)) continue;
    if (!best || rule.match.size() > best->match.size() ||
        (rule.match.size() == best->match.size() && rule.base_score > best->base_score))
      best = &rule;
  }
  return best;
}

void AppendSegment(PunctuationResult& out, ipc::LiveSegment segment) {
  segment.start_char = out.segments.empty() ? 0 : out.segments.back().end_char;
  segment.end_char = segment.start_char + Utf16Length(segment.surface);
  out.surface += segment.surface;
  out.segments.push_back(std::move(segment));
}

}  // namespace

PunctuationResult PunctuationInserter::Insert(const std::vector<ipc::LiveSegment>& converted,
                                              const core::PunctuationRules& rules,
                                              std::string_view style, double boundary_confidence) {
  PunctuationResult out;
  if (converted.empty()) return out;
  const std::string comma = style == "fullwidth_latin" ? "，" : "、";
  const std::string period = style == "fullwidth_latin" ? "．" : "。";
  size_t total_chars = 0;
  for (const auto& segment : converted) total_chars += CodepointLength(segment.surface);
  for (size_t i = 0; i < converted.size(); ++i) {
    const auto& current = converted[i];
    AppendSegment(out, current);
    if (i + 1 == converted.size()) continue;
    const auto& next = converted[i + 1];
    if (current.score < boundary_confidence || IsPunctuationEnd(current.surface) ||
        IsPunctuationStart(next.surface) || RecentComma(out.surface) ||
        (IsAsciiWord(current.surface) && IsAsciiWord(next.surface)))
      continue;
    const auto* rule = BestRule(rules, core::PunctuationKind::Comma, current.surface);
    if (!rule || rule->base_score < 0.5) continue;
    auto pos = AsPos(current.pos);
    auto head_pos = AsPos(next.head_pos);
    if (pos == core::SegmentPos::Unknown) pos = InferPrevPos(current);
    if (head_pos == core::SegmentPos::Unknown) head_pos = InferNextHeadPos(next);
    // Unknown after surface inference gives no evidence that "と" is a quote.
    // Prefer suppressing a comma inside a nominal conjunction ("私と彼").
    if (rule->match == "と" && head_pos == core::SegmentPos::Unknown)
      head_pos = core::SegmentPos::Taigen;
    if (!core::PunctuationRules::MatchesGuard(*rule, pos, AsSem(current.sem), head_pos,
                                              AsSem(next.head_sem), false))
      continue;
    ipc::LiveSegment inserted;
    inserted.surface = comma;
    inserted.auto_punctuation = true;
    inserted.pos = static_cast<uint8_t>(core::SegmentPos::Kigou);
    AppendSegment(out, std::move(inserted));
  }
  const auto& last = converted.back();
  if (!(converted.size() == 1 && total_chars < 8) && !IsPunctuationEnd(last.surface) &&
      !IsAsciiWord(last.surface)) {
    const auto* rule = BestRule(rules, core::PunctuationKind::Period, last.surface);
    if (rule && rule->base_score > 0 &&
        core::PunctuationRules::MatchesGuard(*rule, AsPos(last.pos), AsSem(last.sem),
                                             core::SegmentPos::Unknown,
                                             core::SegmentSemantic::Unknown, true)) {
      ipc::LiveSegment inserted;
      inserted.surface = period;
      inserted.auto_punctuation = true;
      inserted.pos = static_cast<uint8_t>(core::SegmentPos::Kigou);
      AppendSegment(out, std::move(inserted));
    }
  }
  return out;
}

core::PunctuationRules PunctuationInserter::LoadRules(std::string_view configured_path) {
  std::filesystem::path path;
  constexpr std::string_view prefix = "%LOCALAPPDATA%\\";
  if (configured_path.starts_with(prefix)) {
    const auto local = core::GetLocalAppDataDirectory();
    if (!local) return core::PunctuationRules::Default();
    path = *local / core::Utf8Path(configured_path.substr(prefix.size()));
  } else {
    path = core::Utf8Path(configured_path);
  }
  static std::mutex cache_mutex;
  static std::filesystem::path cached_path;
  static std::filesystem::file_time_type cached_write_time{};
  static core::PunctuationRules cached_rules;
  std::lock_guard lock(cache_mutex);
  std::error_code error;
  const auto write_time = std::filesystem::last_write_time(path, error);
  if (error) return core::PunctuationRules::Default();
  if (path == cached_path && write_time == cached_write_time) return cached_rules;
  constexpr uintmax_t kMaxRulesBytes = 1024 * 1024;
  const auto size = std::filesystem::file_size(path, error);
  if (error || size > kMaxRulesBytes) return core::PunctuationRules::Default();
  std::ifstream input(path, std::ios::binary);
  if (!input) return core::PunctuationRules::Default();
  std::string tsv(static_cast<size_t>(size), '\0');
  input.read(tsv.data(), static_cast<std::streamsize>(size));
  if (input.gcount() != static_cast<std::streamsize>(size) ||
      input.peek() != std::char_traits<char>::eof())
    return core::PunctuationRules::Default();
  std::vector<size_t> invalid_lines;
  auto rules = core::PunctuationRules::ParseAndMerge(tsv, &invalid_lines);
  if (!invalid_lines.empty()) {
    static logging::RuntimeLogger logger(logging::RuntimeLoggerOptionsFromEnvironment("host"));
    logger.Log(logging::RuntimeLogLevel::Warn, "punctuation_rules_invalid_lines",
               {{"invalid_lines", static_cast<uint64_t>(invalid_lines.size())}});
  }
  cached_path = path;
  cached_write_time = write_time;
  cached_rules = rules;
  return rules;
}

}  // namespace azookey::host
