#include "runner/ReadObservation.h"

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <utility>

namespace azookey::compat_test {
namespace {

size_t SourceIndex(ReadSource source) { return static_cast<size_t>(source); }

bool IsAsciiLetter(wchar_t ch) { return (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z'); }

}  // namespace

TextCensus CensusOf(std::wstring_view text) {
  TextCensus census;
  census.utf16_length = text.size();
  for (const wchar_t ch : text) {
    if (IsAsciiLetter(ch)) {
      ++census.ascii_letters;
    } else if (ch >= 0x3041 && ch <= 0x309F) {
      ++census.hiragana;
    } else if (ch >= 0x30A0 && ch <= 0x30FF) {
      ++census.katakana;
    } else {
      ++census.other;
    }
  }
  return census;
}

TextRelation ClassifyRelation(const std::optional<std::wstring>& text,
                              std::string_view typed_ascii) {
  if (!text) return TextRelation::Absent;
  if (text->empty()) return TextRelation::Empty;
  if (!typed_ascii.empty() && text->size() == typed_ascii.size() &&
      std::equal(text->begin(), text->end(), typed_ascii.begin(), [](wchar_t left, char right) {
        return std::towlower(left) == std::towlower(static_cast<unsigned char>(right));
      })) {
    return TextRelation::TypedAscii;
  }
  return TextRelation::Other;
}

std::string VirtualKeyAction(unsigned virtual_key, bool with_modifier) {
  char label[24]{};
  std::snprintf(label, sizeof(label), "%svk:0x%02X", with_modifier ? "mod+" : "", virtual_key);
  return label;
}

std::string AsciiInputAction(size_t count) { return "ascii:" + std::to_string(count); }

std::string UnicodeInputAction(size_t count) { return "unicode:" + std::to_string(count); }

const char* ReadSourceName(ReadSource source) {
  switch (source) {
    case ReadSource::Value:
      return "value";
    case ReadSource::Text:
      return "text";
    case ReadSource::None:
      break;
  }
  return "none";
}

const char* ReadPhaseName(ReadPhase phase) {
  switch (phase) {
    case ReadPhase::Judge:
      return "judge";
    case ReadPhase::Settle:
      return "settle";
    case ReadPhase::PostFailure:
      return "post-failure";
  }
  return "unknown";
}

const char* TextRelationName(TextRelation relation) {
  switch (relation) {
    case TextRelation::Absent:
      return "absent";
    case TextRelation::Empty:
      return "empty";
    case TextRelation::TypedAscii:
      return "typed-ascii";
    case TextRelation::Other:
      break;
  }
  return "other";
}

void ReadTrace::Clear() {
  entries_.clear();
  for (size_t i = 0; i < 3; ++i) {
    previous_text_[i].reset();
    has_previous_[i] = false;
  }
}

void ReadTrace::RecordRead(ReadPhase phase, std::string_view action, uint64_t elapsed_ms,
                           const ObservedRead& read, std::string_view typed_ascii) {
  Entry entry;
  entry.phase = phase;
  entry.action = std::string(action);
  entry.source = read.source;
  entry.elapsed_ms = elapsed_ms;
  entry.relation = ClassifyRelation(read.text, typed_ascii);
  if (read.text) entry.census = CensusOf(*read.text);
  const size_t index = SourceIndex(read.source);
  entry.has_previous = has_previous_[index];
  entry.changed_from_previous = has_previous_[index] && previous_text_[index] != read.text;
  previous_text_[index] = read.text;
  has_previous_[index] = true;
  entries_.push_back(std::move(entry));
}

void ReadTrace::RecordSettleSummary(bool stable, unsigned reads, uint64_t elapsed_ms) {
  Entry entry;
  entry.is_summary = true;
  entry.phase = ReadPhase::Settle;
  entry.stable = stable;
  entry.reads = reads;
  entry.elapsed_ms = elapsed_ms;
  entries_.push_back(std::move(entry));
}

std::vector<std::string> ReadTrace::Lines() const {
  std::vector<std::string> lines;
  lines.reserve(entries_.size());
  size_t sequence = 0;
  for (const auto& entry : entries_) {
    std::string line;
    if (entry.is_summary) {
      line = "settle_summary stable=" + std::string(entry.stable ? "yes" : "no") +
             " reads=" + std::to_string(entry.reads) +
             " elapsed_ms=" + std::to_string(entry.elapsed_ms);
    } else {
      line = "read[" + std::to_string(sequence++) + "] phase=" + ReadPhaseName(entry.phase) +
             " action=" + (entry.action.empty() ? "none" : entry.action) +
             " source=" + ReadSourceName(entry.source) +
             " t_ms=" + std::to_string(entry.elapsed_ms) +
             " relation=" + TextRelationName(entry.relation) +
             " utf16_len=" + std::to_string(entry.census.utf16_length) +
             " ascii_letters=" + std::to_string(entry.census.ascii_letters) +
             " hiragana=" + std::to_string(entry.census.hiragana) +
             " katakana=" + std::to_string(entry.census.katakana) +
             " other=" + std::to_string(entry.census.other) + " changed=" +
             (!entry.has_previous           ? "n/a"
              : entry.changed_from_previous ? "yes"
                                            : "no");
    }
    lines.push_back(std::move(line));
  }
  return lines;
}

SettleOutcome SettleReads(const SettleOptions& options, const std::function<ObservedRead()>& read,
                          const std::function<uint64_t()>& now_ms,
                          const std::function<void(uint64_t)>& sleep_ms,
                          const std::function<void(const ObservedRead&)>& on_read) {
  const unsigned required = std::max(1u, options.required_consecutive);
  const uint64_t started_at = now_ms();
  SettleOutcome outcome;
  unsigned run = 0;
  while (true) {
    ObservedRead current = read();
    ++outcome.reads;
    if (on_read) on_read(current);
    run = outcome.reads > 1 && current.text == outcome.last.text ? run + 1 : 1;
    outcome.last = std::move(current);
    if (run >= required) {
      outcome.stable = true;
      break;
    }
    if (now_ms() - started_at >= options.ceiling_ms) break;
    sleep_ms(options.interval_ms);
  }
  return outcome;
}

}  // namespace azookey::compat_test
