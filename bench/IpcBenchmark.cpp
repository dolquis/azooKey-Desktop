#include "IpcBenchmark.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "azookey/ipc/Messages.h"
#include "azookey/ipc/NamedPipeTransport.h"
#include "azookey/ipc/Payloads.h"

namespace azookey::bench {
namespace {
using Clock = std::chrono::steady_clock;
constexpr size_t kSamples = 200;
constexpr size_t kWarmup = 20;

double Milliseconds(Clock::time_point start, Clock::time_point end) {
  return std::chrono::duration<double, std::milli>(end - start).count();
}

LatencyMetrics Summarize(std::vector<double> samples) {
  std::sort(samples.begin(), samples.end());
  const auto percentile = [&](size_t p) { return samples[(samples.size() - 1) * p / 100]; };
  return {percentile(50), percentile(95), percentile(99), samples.back()};
}
}  // namespace

IpcPhaseBreakdown RunIpcBenchmark() {
  ipc::QueryCandidatesResponse payload;
  for (int i = 0; i < 10; ++i) {
    payload.candidates.push_back(
        {"日本語候補" + std::to_string(i), "にほんごこうほ", 1.0 / (i + 1), "dictionary"});
  }
  ipc::Envelope envelope;
  envelope.type = ipc::MessageType::QueryCandidates;
  envelope.trace_id = "ipc-benchmark";
  envelope.payload_json = ipc::BuildQueryCandidatesResponse(payload);
  std::vector<double> serialize, framing, deserialize, pipe;
#ifdef _WIN32
  ipc::NamedPipeServer server;
  ipc::NamedPipeClient client;
  const auto name = ipc::DefaultPipeName() + "-bench-" + std::to_string(GetCurrentProcessId()) +
                    "-" + std::to_string(Clock::now().time_since_epoch().count());
  if (!server.Start(
          name,
          [](const ipc::Envelope& request) -> std::optional<ipc::Envelope> { return request; }) ||
      !client.Connect(name, 5000)) {
    throw std::runtime_error("IPC benchmark pipe setup failed");
  }
#endif
  for (size_t i = 0; i < kWarmup + kSamples; ++i) {
    envelope.request_id = i + 1;
    const auto t0 = Clock::now();
    auto encoded = ipc::Serialize(envelope);
    const auto t1 = Clock::now();
    if (!encoded) throw std::runtime_error("IPC benchmark serialize failed");
    const auto f0 = Clock::now();
    auto frame = ipc::EncodeLengthPrefixed(*encoded);
    auto restored = frame ? ipc::DecodeLengthPrefixed(*frame) : std::nullopt;
    const auto f1 = Clock::now();
    if (!restored) throw std::runtime_error("IPC benchmark framing failed");
    const auto d0 = Clock::now();
    auto decoded = ipc::Deserialize(*restored);
    const auto d1 = Clock::now();
    if (!decoded || decoded->payload_json != envelope.payload_json) {
      throw std::runtime_error("IPC benchmark payload mismatch");
    }
#ifdef _WIN32
    const auto p0 = Clock::now();
    if (!client.Send(envelope)) throw std::runtime_error("IPC benchmark send failed");
    auto response = client.ReceiveWithTimeout(5000);
    const auto p1 = Clock::now();
    if (!response || response->request_id != envelope.request_id ||
        response->payload_json != envelope.payload_json) {
      throw std::runtime_error("IPC benchmark echo failed");
    }
    if (i >= kWarmup) pipe.push_back(Milliseconds(p0, p1));
#endif
    if (i >= kWarmup) {
      serialize.push_back(Milliseconds(t0, t1));
      framing.push_back(Milliseconds(f0, f1));
      deserialize.push_back(Milliseconds(d0, d1));
    }
  }
  IpcPhaseBreakdown result;
  result.samples = kSamples;
  result.payload_bytes = envelope.payload_json.size();
  result.serialize = Summarize(std::move(serialize));
  result.framing = Summarize(std::move(framing));
  result.deserialize = Summarize(std::move(deserialize));
  if (!pipe.empty()) result.pipe_round_trip = Summarize(std::move(pipe));
  return result;
}

}  // namespace azookey::bench
