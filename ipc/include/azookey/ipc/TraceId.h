#pragma once

#include <string>

namespace azookey::ipc {

// Generates a UUIDv7 trace ID. Values are strictly increasing in generation
// order within this process, including when the clock stalls or moves backward.
// Throws std::runtime_error if the system random source fails.
std::string GenerateTraceId();

}  // namespace azookey::ipc
