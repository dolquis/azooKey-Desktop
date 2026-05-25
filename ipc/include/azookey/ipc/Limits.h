#pragma once

#include <cstddef>
#include <cstdint>

namespace azookey::ipc {

inline constexpr std::size_t kMaxJsonInputBytes = 1024 * 1024;
inline constexpr std::size_t kMaxJsonNestDepth = 64;
inline constexpr uint32_t kMaxFrameSize = static_cast<uint32_t>(kMaxJsonInputBytes);
inline constexpr uint32_t kMaxPipeInstances = 32;

}  // namespace azookey::ipc
