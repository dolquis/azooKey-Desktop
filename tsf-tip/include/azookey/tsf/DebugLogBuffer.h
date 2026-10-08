#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace azookey::tsf {

// Debug window log rings (docs/legacy-parity-spec.md §8). Platform neutral so
// the formatting and redaction rules are testable without a window.
inline constexpr std::size_t kDebugLogCapacity = 50;
inline constexpr std::size_t kDebugLogTopCandidates = 3;

struct DebugIpcLogEntry {
  std::uint64_t req_id{0};
  // Message name such as "QueryCandidates". Never put user text here.
  std::string message_type;
  // Body fields (UTF-8). Shown only when the M41 body gate is open.
  std::string reading;
  std::vector<std::string> candidates;
  std::uint32_t latency_ms{0};
  bool stale_dropped{false};
};

struct DebugStateTransitionEntry {
  std::string from;
  std::string to;
};

// Keeps the newest kDebugLogCapacity entries of each ring. Thread safe: TIP
// callbacks may push from the IPC thread while the UI thread renders.
class DebugLogBuffer {
 public:
  // Stores the entry with candidates truncated to kDebugLogTopCandidates. When
  // body_allowed is false the body text is dropped before it is stored and only
  // its code point length is kept. Returns the line as rendered with that gate.
  std::string PushIpc(DebugIpcLogEntry entry, bool body_allowed);
  std::string PushTransition(DebugStateTransitionEntry entry);

  // IPC lines oldest to newest, then transition lines oldest to newest. Body
  // text appears only when body_allowed is true and it was stored with the
  // gate open; otherwise it is rendered as `<redacted len=N>`.
  std::vector<std::string> RenderLines(bool body_allowed) const;

  std::size_t ipc_size() const;
  std::size_t transition_size() const;
  void Clear();

 private:
  struct StoredIpc {
    DebugIpcLogEntry entry;
    std::size_t reading_length{0};
    std::vector<std::size_t> candidate_lengths;
    bool body_retained{false};
  };

  static std::string RenderIpc(const StoredIpc& stored, bool body_allowed);
  static std::string RenderTransition(const DebugStateTransitionEntry& entry);

  mutable std::mutex mutex_;
  std::deque<StoredIpc> ipc_;
  std::deque<DebugStateTransitionEntry> transitions_;
};

}  // namespace azookey::tsf
