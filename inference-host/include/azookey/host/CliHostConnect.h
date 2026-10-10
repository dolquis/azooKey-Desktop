#pragma once

#include <cstdint>
#include <string>

#include "azookey/ipc/NamedPipeTransport.h"

namespace azookey::host {

// Connects a CLI to the running Host. A missing pipe fails after
// `connect_timeout_ms` as before; a pipe that exists but has no free instance
// keeps being retried until `busy_timeout_ms` (measured from the call) runs
// out. The Host listens on one pipe instance at a time and creates the next
// only after accepting a client, so CLIs started together find it busy
// (DEV-1521). If the pipe disappears while waiting, the call fails within one
// retry slice.
bool ConnectToRunningHost(ipc::NamedPipeClient& client, const std::string& pipe_name,
                          uint32_t connect_timeout_ms, uint32_t busy_timeout_ms);

}  // namespace azookey::host
