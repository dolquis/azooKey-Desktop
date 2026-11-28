#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace azookey::learning {

class ByteCrypto;

struct RerankerExposure {
  std::string observation_id;
  std::string left_context_hash;
  std::string reading;
  std::string chosen_surface;
  std::vector<std::string> shown_surfaces;
  std::vector<std::array<float, 12>> features;
  uint64_t shown_at_ms{};
  uint64_t commit_ts{};
};

struct RerankerTraceStats {
  uint64_t records{};
  uint64_t size_bytes{};
  uint64_t retention_days{30};
  bool snapshot_ready{};
};

struct RerankerTraceOptions {
  size_t max_records{10000};
  size_t max_ciphertext_bytes{16 * 1024 * 1024};
  size_t max_pending_records{1024};
  size_t max_pending_bytes{1024 * 1024};
  std::chrono::milliseconds flush_interval{15000};
  std::chrono::milliseconds sweep_interval{60000};
  std::function<uint64_t()> wall_clock_ms{};
  std::function<void(std::string_view)> log_reason{};
};

class RerankerTraceStore {
 public:
  // A nonempty path names the encrypted snapshot. An empty path disables the store.
  explicit RerankerTraceStore(std::filesystem::path trace_path);
  RerankerTraceStore(std::filesystem::path trace_path,
                     std::function<void(std::string_view)> log_reason);
  RerankerTraceStore(std::filesystem::path trace_path, const ByteCrypto& crypto,
                     RerankerTraceOptions options);
  ~RerankerTraceStore();

  bool Enqueue(RerankerExposure exposure);
  bool EnqueueCorrection(std::string observation_id, std::string target_observation_id,
                         std::optional<std::string> accepted_surface, uint64_t correction_ts);
  void SetRecording(bool enabled, bool learning_allowed);
  RerankerTraceStats Stats() const;

  // Wait after revoking recording so a previously admitted publication finishes
  // before the caller acknowledges the change. Call without conversion locks.
  void WaitForPublication();

  // Disk operations serialize with background batches. Call without conversion locks.
  bool Flush();
  bool Forget(std::string_view reading, std::string_view surface, bool* removed = nullptr);
  bool Reset();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace azookey::learning
