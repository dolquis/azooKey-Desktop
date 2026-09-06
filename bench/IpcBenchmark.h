#pragma once

#include "BenchmarkResult.h"

namespace azookey::bench {

// Measures a fixed candidate payload. Throws on transport/codec failure.
// Pipe latency includes both codec directions and an in-process echo server;
// connection setup and inference are excluded. Non-Windows omits pipe latency.
IpcPhaseBreakdown RunIpcBenchmark();

}  // namespace azookey::bench
