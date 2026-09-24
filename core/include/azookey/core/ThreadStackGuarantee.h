#pragma once

namespace azookey::core {

// Reserve exception-handler stack on the calling thread or fiber. Call at the
// entry point of application-owned workers; this does not install a filter.
void ReserveCurrentThreadStack() noexcept;

// Safe to query from the unhandled exception filter. A failed reservation or
// an unrelated thread/fiber must leave stack overflow handling to the OS.
bool HasCurrentThreadStackGuarantee() noexcept;

}  // namespace azookey::core
