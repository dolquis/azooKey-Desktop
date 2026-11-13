#include "IpcBenchmark.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "azookey/host/Dispatcher.h"
#include "azookey/host/RequestScheduler.h"
#include "azookey/ipc/Messages.h"
#include "azookey/ipc/NamedPipeTransport.h"
#include "azookey/ipc/Payloads.h"
#include "azookey/ipc/TraceId.h"

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

#ifdef _WIN32
struct HostBreakdownSamples {
  std::vector<double> send, host_compute, receive;
};

// The handler runs on the server's connection thread; the client reads these after Receive
// returns, so the timestamps are published through atomics.
struct HandlerStamps {
  std::atomic<Clock::rep> entry{0};
  std::atomic<Clock::rep> exit{0};
};

Clock::time_point FromRep(Clock::rep rep) { return Clock::time_point(Clock::duration(rep)); }

// Real QueryCandidates round trips: send is Send() start -> handler entry (client write plus
// the server's read and wake-up), host_compute is the Dispatcher call, receive is handler exit
// -> response fully read (server write plus client read). The three sum to the round trip.
HostBreakdownSamples MeasureHostBreakdown(host::InferenceEngine& engine) {
  host::RequestScheduler scheduler;
  host::Dispatcher dispatcher(&engine, &scheduler, nullptr);
  HandlerStamps stamps;
  ipc::NamedPipeServer server;
  ipc::NamedPipeClient client;
  const auto name = ipc::DefaultPipeName() + "-bench-host-" +
                    std::to_string(GetCurrentProcessId()) + "-" +
                    std::to_string(Clock::now().time_since_epoch().count());
  if (!server.Start(name,
                    [&](const ipc::Envelope& request) -> std::optional<ipc::Envelope> {
                      stamps.entry.store(Clock::now().time_since_epoch().count());
                      auto response = dispatcher.Dispatch(request);
                      stamps.exit.store(Clock::now().time_since_epoch().count());
                      return response;
                    }) ||
      !client.Connect(name, 5000)) {
    throw std::runtime_error("IPC benchmark host pipe setup failed");
  }
  static constexpr std::array<const char*, 5> kReadings = {"わたし", "にほん", "とうきょう",
                                                           "かなへんかん", "にほん"};
  HostBreakdownSamples samples;
  for (size_t i = 0; i < kWarmup + kSamples; ++i) {
    ipc::QueryCandidatesRequest query;
    query.reading = kReadings[i % kReadings.size()];
    ipc::Envelope request;
    request.request_id = i + 1;
    request.trace_id = ipc::GenerateTraceId();
    request.type = ipc::MessageType::QueryCandidates;
    request.payload_json = ipc::BuildQueryCandidatesRequest(query);
    const auto start = Clock::now();
    if (!client.Send(request)) throw std::runtime_error("IPC benchmark host send failed");
    auto response = client.ReceiveWithTimeout(5000);
    const auto end = Clock::now();
    const auto parsed = response && response->request_id == request.request_id &&
                                response->trace_id == request.trace_id
                            ? ipc::ParseQueryCandidatesResponse(response->payload_json)
                            : std::nullopt;
    // An error or empty reply would otherwise be timed as if it were a real conversion.
    if (!parsed || !parsed->ok || parsed->candidates.empty()) {
      throw std::runtime_error("IPC benchmark host response invalid");
    }
    const auto entry = FromRep(stamps.entry.load());
    const auto exit = FromRep(stamps.exit.load());
    if (entry < start || exit < entry || end < exit) {
      throw std::runtime_error("IPC benchmark host timestamps out of order");
    }
    if (i < kWarmup) continue;
    samples.send.push_back(Milliseconds(start, entry));
    samples.host_compute.push_back(Milliseconds(entry, exit));
    samples.receive.push_back(Milliseconds(exit, end));
  }
  return samples;
}
#endif
}  // namespace

IpcPhaseBreakdown RunIpcBenchmark(host::InferenceEngine& engine) {
  ipc::QueryCandidatesResponse payload;
  for (int i = 0; i < 10; ++i) {
    payload.candidates.push_back(
        {"日本語候補" + std::to_string(i), "にほんごこうほ", 1.0 / (i + 1), "dictionary"});
  }
  ipc::Envelope envelope;
  envelope.type = ipc::MessageType::QueryCandidates;
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
    envelope.trace_id = ipc::GenerateTraceId();
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
    if (!decoded || decoded->trace_id != envelope.trace_id ||
        decoded->payload_json != envelope.payload_json) {
      throw std::runtime_error("IPC benchmark payload mismatch");
    }
#ifdef _WIN32
    const auto p0 = Clock::now();
    if (!client.Send(envelope)) throw std::runtime_error("IPC benchmark send failed");
    auto response = client.ReceiveWithTimeout(5000);
    const auto p1 = Clock::now();
    if (!response || response->request_id != envelope.request_id ||
        response->trace_id != envelope.trace_id ||
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
#ifdef _WIN32
  auto host_samples = MeasureHostBreakdown(engine);
  result.send = Summarize(std::move(host_samples.send));
  result.host_compute = Summarize(std::move(host_samples.host_compute));
  result.receive = Summarize(std::move(host_samples.receive));
#else
  (void)engine;
#endif
  return result;
}

}  // namespace azookey::bench
