#pragma once

#include <cstddef>
#include <cstdint>

namespace azookey::ipc {

inline constexpr std::size_t kMaxJsonInputBytes = 1024 * 1024;
inline constexpr std::size_t kMaxJsonNestDepth = 64;
inline constexpr uint32_t kMaxFrameSize = static_cast<uint32_t>(kMaxJsonInputBytes);
inline constexpr uint32_t kMaxPipeInstances = 32;

// Longest CommitObservation resend backlog one TIP instance keeps while the Host
// is unreachable (DEV-554). Shared so the Host can size its dedupe ring to cover
// every observation the connected TIPs can still resend; see
// docs/learning-data-management-spec.md section 12.
inline constexpr std::size_t kMaxQueuedCommitObservations = 64;

}  // namespace azookey::ipc
