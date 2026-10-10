#pragma once

#include <cstdint>
#include <string>

#include "azookey/ipc/NamedPipeTransport.h"

namespace azookey::host {

// Connects a CLI to the running Host. A missing pipe fails after
// `connect_timeout_ms` as before; a pipe that exists but has no free instance
// keeps being retried until `busy_timeout_ms` (measured from the call) runs
// out, because the Host accepts one client at a time and concurrent CLIs queue
// behind each other (DEV-1521).
bool ConnectToRunningHost(ipc::NamedPipeClient& client, const std::string& pipe_name,
                          uint32_t connect_timeout_ms, uint32_t busy_timeout_ms);

}  // namespace azookey::host
