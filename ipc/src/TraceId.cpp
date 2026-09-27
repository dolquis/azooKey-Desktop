#include "azookey/ipc/TraceId.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <bcrypt.h>
#endif

namespace azookey::ipc {
namespace {

constexpr uint64_t kMaxTimestamp = (uint64_t{1} << 48) - 1;
constexpr uint64_t kMaxRandomB = (uint64_t{1} << 62) - 1;
constexpr uint16_t kMaxRandomA = (uint16_t{1} << 12) - 1;

struct TraceIdState {
  std::mutex mutex;
  uint64_t timestamp_ms = 0;
  uint16_t random_a = 0;
  uint64_t random_b = 0;
  bool initialized = false;
};

void SeedRandom(TraceIdState& state) {
  std::array<unsigned char, 10> bytes{};
#ifdef _WIN32
  if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                                      BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
    throw std::runtime_error("UUIDv7 random source failed");
  }
#else
  static std::random_device random;
  for (auto& byte : bytes) byte = static_cast<unsigned char>(random());
#endif
  state.random_a =
      static_cast<uint16_t>((static_cast<uint16_t>(bytes[0]) << 8) | bytes[1]) & kMaxRandomA;
  state.random_b = 0;
  for (std::size_t i = 2; i < bytes.size(); ++i) {
    state.random_b = (state.random_b << 8) | bytes[i];
  }
  state.random_b &= kMaxRandomB;
}

void IncrementRandom(TraceIdState& state) {
  if (state.random_b < kMaxRandomB) {
    ++state.random_b;
  } else if (state.random_a < kMaxRandomA) {
    state.random_b = 0;
    ++state.random_a;
  } else {
    if (state.timestamp_ms == kMaxTimestamp) {
      throw std::overflow_error("UUIDv7 timestamp exhausted");
    }
    ++state.timestamp_ms;
    state.random_a = 0;
    state.random_b = 0;
  }
}

std::string FormatUuid(uint64_t timestamp_ms, uint16_t random_a, uint64_t random_b) {
  std::array<unsigned char, 16> bytes{};
  for (std::size_t i = 0; i < 6; ++i) {
    bytes[i] = static_cast<unsigned char>(timestamp_ms >> (40 - 8 * i));
  }
  bytes[6] = static_cast<unsigned char>(0x70 | (random_a >> 8));
  bytes[7] = static_cast<unsigned char>(random_a);
  bytes[8] = static_cast<unsigned char>(0x80 | (random_b >> 56));
  for (std::size_t i = 9; i < bytes.size(); ++i) {
    bytes[i] = static_cast<unsigned char>(random_b >> (8 * (15 - i)));
  }

  constexpr char hex[] = "0123456789abcdef";
  std::string id;
  id.reserve(36);
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) id.push_back('-');
    id.push_back(hex[bytes[i] >> 4]);
    id.push_back(hex[bytes[i] & 0x0f]);
  }
  return id;
}

}  // namespace

std::string GenerateTraceId() {
  static TraceIdState state;
  std::lock_guard lock(state.mutex);

  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  const auto now_ms = now > 0 ? static_cast<uint64_t>(now) : uint64_t{0};
  if (now_ms > kMaxTimestamp) throw std::overflow_error("UUIDv7 timestamp exhausted");

  if (!state.initialized || now_ms > state.timestamp_ms) {
    SeedRandom(state);
    state.timestamp_ms = now_ms;
    state.initialized = true;
  } else {
    IncrementRandom(state);
  }
  return FormatUuid(state.timestamp_ms, state.random_a, state.random_b);
}

}  // namespace azookey::ipc
