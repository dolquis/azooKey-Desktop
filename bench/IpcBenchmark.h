#pragma once

#include "BenchmarkResult.h"

namespace azookey::bench {

// Measures a fixed candidate payload. Throws on transport/codec failure.
// Pipe latency includes both codec directions and an in-process echo server;
// connection setup and inference are excluded. Non-Windows omits pipe latency.
// A second pass sends QueryCandidates through a real Dispatcher over a pipe and splits each
// round trip into send / Host handler / receive using one steady_clock. The Host side is an
// engine of its own (SimpleConverter, no model, no trace logger), so --model and --trace of the
// caller never reach it. Non-Windows omits these three as well.
IpcPhaseBreakdown RunIpcBenchmark();

}  // namespace azookey::bench
