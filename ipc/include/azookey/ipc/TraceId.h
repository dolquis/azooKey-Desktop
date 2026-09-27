#pragma once

#include <string>
#include <string_view>

namespace azookey::ipc {

// Generates a UUIDv7 trace ID. Values are strictly increasing in generation
// order within this process, including when the clock stalls or moves backward.
// Throws std::runtime_error if the system random source fails.
std::string GenerateTraceId();

// Only UUIDv7-shaped IDs may be written to diagnostic logs. Envelope parsing
// remains backward compatible with older clients that send an empty trace ID.
bool IsValidTraceId(std::string_view trace_id) noexcept;

}  // namespace azookey::ipc
