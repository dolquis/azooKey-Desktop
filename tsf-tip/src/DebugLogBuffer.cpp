#include "azookey/tsf/DebugLogBuffer.h"

#include <string_view>
#include <utility>

#include "azookey/core/Utf8.h"

namespace azookey::tsf {
namespace {

std::size_t CodepointLength(std::string_view text) {
  std::size_t count = 0;
  std::size_t offset = 0;
  char32_t codepoint = 0;
  // Invalid bytes are consumed one at a time and counted as one unit each.
  while (offset < text.size()) {
    core::DecodeNextUtf8(text, offset, codepoint);
    ++count;
  }
  return count;
}

std::string RedactedText(std::size_t length) {
  return "<redacted len=" + std::to_string(length) + ">";
}

std::string BodyText(std::string_view text, std::size_t length, bool show_body) {
  if (!show_body) return RedactedText(length);
  std::string quoted = "\"";
  quoted.append(text);
  quoted.push_back('"');
  return quoted;
}

}  // namespace

std::string DebugLogBuffer::RenderIpc(const StoredIpc& stored, bool body_allowed) {
  const bool show_body = body_allowed && stored.body_retained;
  const auto& entry = stored.entry;
  std::string line = "[ipc] req=" + std::to_string(entry.req_id) + " " + entry.message_type +
                     " reading=" + BodyText(entry.reading, stored.reading_length, show_body) +
                     " top" + std::to_string(kDebugLogTopCandidates) + "=[";
  for (std::size_t i = 0; i < stored.candidate_lengths.size(); ++i) {
    if (i != 0) line += ", ";
    const std::string_view text =
        i < entry.candidates.size() ? std::string_view(entry.candidates[i]) : std::string_view();
    line += BodyText(text, stored.candidate_lengths[i], show_body);
  }
  line += "] latency_ms=" + std::to_string(entry.latency_ms);
  if (entry.stale_dropped) line += " stale_dropped";
  return line;
}

std::string DebugLogBuffer::RenderTransition(const DebugStateTransitionEntry& entry) {
  return "[state] " + entry.from + " -> " + entry.to;
}

std::string DebugLogBuffer::PushIpc(DebugIpcLogEntry entry, bool body_allowed) {
  if (entry.candidates.size() > kDebugLogTopCandidates)
    entry.candidates.resize(kDebugLogTopCandidates);
  StoredIpc stored;
  stored.reading_length = CodepointLength(entry.reading);
  stored.candidate_lengths.reserve(entry.candidates.size());
  for (const auto& candidate : entry.candidates)
    stored.candidate_lengths.push_back(CodepointLength(candidate));
  stored.body_retained = body_allowed;
  if (!body_allowed) {
    // Text that was not allowed out at push time must not linger in memory.
    entry.reading.clear();
    entry.candidates.clear();
  }
  stored.entry = std::move(entry);
  std::string line = RenderIpc(stored, body_allowed);

  std::lock_guard lock(mutex_);
  ipc_.push_back(std::move(stored));
  while (ipc_.size() > kDebugLogCapacity) ipc_.pop_front();
  return line;
}

std::string DebugLogBuffer::PushTransition(DebugStateTransitionEntry entry) {
  std::string line = RenderTransition(entry);
  std::lock_guard lock(mutex_);
  transitions_.push_back(std::move(entry));
  while (transitions_.size() > kDebugLogCapacity) transitions_.pop_front();
  return line;
}

std::vector<std::string> DebugLogBuffer::RenderLines(bool body_allowed) const {
  std::lock_guard lock(mutex_);
  std::vector<std::string> lines;
  lines.reserve(ipc_.size() + transitions_.size());
  for (const auto& stored : ipc_) lines.push_back(RenderIpc(stored, body_allowed));
  for (const auto& entry : transitions_) lines.push_back(RenderTransition(entry));
  return lines;
}

std::size_t DebugLogBuffer::ipc_size() const {
  std::lock_guard lock(mutex_);
  return ipc_.size();
}

std::size_t DebugLogBuffer::transition_size() const {
  std::lock_guard lock(mutex_);
  return transitions_.size();
}

void DebugLogBuffer::Clear() {
  std::lock_guard lock(mutex_);
  ipc_.clear();
  transitions_.clear();
}

}  // namespace azookey::tsf
