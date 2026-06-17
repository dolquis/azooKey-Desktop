#pragma once

#include <string_view>

// Canonical latency / observability phase enum.
//
// This is the single source of truth for the `phase` field emitted in the
// JSON Lines structured logs (docs/dev-infrastructure-spec.md §7.2 / §7.7.2)
// and consumed by the M51 trace_viewer. TIP / IPC / inference-host all map
// their measurements onto these values so a single user action (one
// `trace_id`) can be reconstructed across process boundaries.
//
// Versioning policy (docs/dev-infrastructure-spec.md §7.7.2):
//   - Adding a new phase value is a backward-compatible change (patch):
//     existing log readers ignore unknown phases.
//   - Removing or renaming an existing phase value is a breaking change
//     (major): trace_viewer aggregation and compat tooling depend on the
//     stable wire names returned by PhaseName().
//   - The wire name (PhaseName) is the stable contract, not the enumerator
//     order. Keep PhaseName() and Phase in sync.
//
// M41 (coarse) vs M51 (full): there is one taxonomy. M41 records only a
// subset of these phases; M51 records the full set. The old M41 phase names
// map onto this enum 1:1 (do NOT collapse them into romaji_convert):
//   serialize    -> ipc_serialize
//   send         -> pipe_send
//   host_compute -> model_inference
//   recv         -> pipe_recv
//   apply_ui     -> ui_apply

namespace azookey::logging {

enum class Phase {
  // tsf-tip: TIP received the key event (trace anchor, t_ms = 0).
  KeyDown,
  // core / tsf-tip: romaji -> kana conversion.
  RomajiConvert,
  // ipc / tsf-tip: request payload built (JSON serialize).
  IpcSerialize,
  // ipc: Named Pipe send.
  PipeSend,
  // inference-host: time spent waiting in the request scheduler queue.
  HostQueueWait,
  // inference-host: SimpleConverter / Zenzai inference.
  ModelInference,
  // inference-host: learning / user dict / tag boost / Tiny / BERT rerank.
  Rerank,
  // ipc / tsf-tip: response received.
  PipeRecv,
  // tsf-tip: request_id / trace_id staleness check (M10).
  StalenessCheck,
  // tsf-tip: CandidateWindow / Preedit update.
  UiApply,
  // tsf-tip: key_down -> ui_apply total wall-clock.
  Total,
};

// Stable wire name used in the JSONL `phase` field. Keep in sync with Phase.
constexpr std::string_view PhaseName(Phase phase) {
  switch (phase) {
    case Phase::KeyDown:
      return "key_down";
    case Phase::RomajiConvert:
      return "romaji_convert";
    case Phase::IpcSerialize:
      return "ipc_serialize";
    case Phase::PipeSend:
      return "pipe_send";
    case Phase::HostQueueWait:
      return "host_queue_wait";
    case Phase::ModelInference:
      return "model_inference";
    case Phase::Rerank:
      return "rerank";
    case Phase::PipeRecv:
      return "pipe_recv";
    case Phase::StalenessCheck:
      return "staleness_check";
    case Phase::UiApply:
      return "ui_apply";
    case Phase::Total:
      return "total";
  }
  return "unknown";
}

}  // namespace azookey::logging
