#include "azookey/core/EtwLogger.h"

#ifdef _WIN32
// clang-format off
#include <windows.h>
#include <evntprov.h>
// clang-format on
#endif

namespace azookey::core {
namespace {
#ifdef _WIN32
constexpr GUID kProvider{
    0x89553f1c, 0x7d18, 0x4a6d, {0xa7, 0x20, 0x53, 0x49, 0x21, 0xc8, 0xb7, 0x30}};
SRWLOCK lock = SRWLOCK_INIT;
REGHANDLE provider = 0;
unsigned references = 0;
#endif
// Every event has a fixed, explicitly typed metadata record. Unused fields are zero.
void Write(std::uint16_t id, std::uint64_t a = 0, std::uint64_t b = 0, std::uint64_t c = 0,
           double elapsed = 0, const EtwGuid& client = {}, std::uint64_t result = 0) noexcept {
#ifdef _WIN32
  AcquireSRWLockShared(&lock);
  if (provider) {
    const EVENT_DESCRIPTOR event{id, 0, 0, 4, 0, 0, 1};
    if (EventEnabled(provider, &event)) {
      EVENT_DATA_DESCRIPTOR data[6];
      EventDataDescCreate(&data[0], &a, sizeof(a));
      EventDataDescCreate(&data[1], &b, sizeof(b));
      EventDataDescCreate(&data[2], &c, sizeof(c));
      EventDataDescCreate(&data[3], &elapsed, sizeof(elapsed));
      EventDataDescCreate(&data[4], client.data(), static_cast<ULONG>(client.size()));
      EventDataDescCreate(&data[5], &result, sizeof(result));
      EventWrite(provider, &event, 6, data);
    }
  }
  ReleaseSRWLockShared(&lock);
#else
  (void)id;
  (void)a;
  (void)b;
  (void)c;
  (void)elapsed;
  (void)client;
  (void)result;
#endif
}
}  // namespace
void EtwLogger::Register() noexcept {
#ifdef _WIN32
  AcquireSRWLockExclusive(&lock);
  if (references++ == 0) EventRegister(&kProvider, nullptr, nullptr, &provider);
  ReleaseSRWLockExclusive(&lock);
#endif
}
void EtwLogger::Unregister() noexcept {
#ifdef _WIN32
  AcquireSRWLockExclusive(&lock);
  if (references && --references == 0) {
    if (provider) EventUnregister(provider);
    provider = 0;
  }
  ReleaseSRWLockExclusive(&lock);
#endif
}
void EtwLogger::LogActivate(std::uint64_t id, const EtwGuid& profile) noexcept {
  Write(1000, id, 0, 0, 0, profile);
}
void EtwLogger::LogDeactivate(std::uint64_t id) noexcept { Write(1001, id); }
void EtwLogger::LogCompositionStart(std::uint64_t n) noexcept { Write(2000, n); }
void EtwLogger::LogCompositionEnd(std::uint64_t n, bool committed) noexcept {
  Write(2001, n, committed);
}
void EtwLogger::LogIpcRequest(std::uint64_t id, std::uint64_t type, std::uint64_t n,
                              const EtwGuid& client) noexcept {
  Write(3000, id, type, n, 0, client);
}
void EtwLogger::LogIpcResponse(std::uint64_t id, double ms, EtwResult result,
                               const EtwGuid& client) noexcept {
  Write(3001, id, 0, 0, ms, client, static_cast<std::uint64_t>(result));
}
void EtwLogger::LogIpcCancel(std::uint64_t id, const EtwGuid& client) noexcept {
  Write(3002, id, 0, 0, 0, client);
}
void EtwLogger::LogIpcPhase(std::uint64_t id, EtwPhase phase, double ms, EtwResult result,
                            const EtwGuid& client) noexcept {
  Write(3003, id, static_cast<std::uint64_t>(phase), 0, ms, client,
        static_cast<std::uint64_t>(result));
}
void EtwLogger::LogInferenceStart(std::uint64_t id, EtwBackend backend, std::uint64_t n,
                                  const EtwGuid& client) noexcept {
  Write(4000, id, static_cast<std::uint64_t>(backend), n, 0, client);
}
void EtwLogger::LogInferenceEnd(std::uint64_t id, std::uint64_t n, double ms, EtwResult result,
                                const EtwGuid& client) noexcept {
  Write(4001, id, n, 0, ms, client, static_cast<std::uint64_t>(result));
}
void EtwLogger::LogLearningObserve(std::uint64_t r, std::uint64_t s) noexcept { Write(5000, r, s); }
void EtwLogger::LogInferencePhase(std::uint64_t id, EtwPhase phase, EtwBackend backend, double ms,
                                  EtwResult result, const EtwGuid& client) noexcept {
  Write(4002, id, static_cast<std::uint64_t>(phase), static_cast<std::uint64_t>(backend), ms,
        client, static_cast<std::uint64_t>(result));
}
void EtwLogger::LogLearningForget(std::uint64_t r, std::uint64_t s) noexcept { Write(5001, r, s); }
void EtwLogger::LogError(EtwModule source, EtwErrorCode code, std::int32_t hr) noexcept {
  Write(9000, static_cast<std::uint64_t>(source), static_cast<std::uint64_t>(code),
        static_cast<std::uint32_t>(hr));
}
}  // namespace azookey::core
