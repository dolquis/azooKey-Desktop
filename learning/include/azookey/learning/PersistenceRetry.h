#pragma once

#include <chrono>

namespace azookey::learning {

// Default bound for retrying transient Windows sharing and lock violations
// while opening, flushing, or replacing a persisted file. Callers that hold a
// lock other requests wait on pass zero to try once and keep their data dirty.
inline constexpr std::chrono::milliseconds kTransientFileRetryBudget{500};

}  // namespace azookey::learning
