#pragma once

#include <array>
#include <cstdint>

namespace azookey::core {
using EtwGuid = std::array<std::uint8_t, 16>;
enum class EtwModule : std::uint64_t { Tip, Host, Settings, Ipc };
enum class EtwErrorCode : std::uint64_t { Transport, Protocol, Business };
enum class EtwBackend : std::uint64_t { Unknown, Kana, Neural, Ai };
enum class EtwResult : std::uint64_t { Success, Timeout, Disconnected, Failed, Cancelled };
enum class EtwPhase : std::uint64_t { Converter, AiTransform, FrameWrite, FrameRead };

// No string-taking overloads: even opt-in logging cannot put input text in ETW.
class EtwLogger {
 public:
  static void Register() noexcept;
  static void Unregister() noexcept;
  static void LogActivate(std::uint64_t client_id, const EtwGuid& profile) noexcept;
  static void LogDeactivate(std::uint64_t client_id) noexcept;
  static void LogCompositionStart(std::uint64_t length) noexcept;
  static void LogCompositionEnd(std::uint64_t length, bool committed) noexcept;
  static void LogIpcRequest(std::uint64_t request, std::uint64_t message_type,
                            std::uint64_t payload_size, const EtwGuid& client = {}) noexcept;
  static void LogIpcResponse(std::uint64_t request, double latency_ms,
                             EtwResult result = EtwResult::Success,
                             const EtwGuid& client = {}) noexcept;
  static void LogIpcCancel(std::uint64_t target, const EtwGuid& client = {}) noexcept;
  static void LogIpcPhase(std::uint64_t request, EtwPhase phase, double latency_ms,
                          EtwResult result, const EtwGuid& client = {}) noexcept;
  static void LogInferenceStart(std::uint64_t request, EtwBackend backend, std::uint64_t kana_len,
                                const EtwGuid& client = {}) noexcept;
  static void LogInferenceEnd(std::uint64_t request, std::uint64_t candidates, double latency_ms,
                              EtwResult result = EtwResult::Success,
                              const EtwGuid& client = {}) noexcept;
  static void LogLearningObserve(std::uint64_t reading_len, std::uint64_t surface_len) noexcept;
  static void LogInferencePhase(std::uint64_t request, EtwPhase phase, EtwBackend backend,
                                double latency_ms, EtwResult result,
                                const EtwGuid& client = {}) noexcept;
  static void LogLearningForget(std::uint64_t reading_len, std::uint64_t surface_len) noexcept;
  static void LogError(EtwModule source, EtwErrorCode code, std::int32_t hr) noexcept;
};
}  // namespace azookey::core
