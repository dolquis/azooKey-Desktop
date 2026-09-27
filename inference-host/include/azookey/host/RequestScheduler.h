#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace azookey::host {

// A scheduler-owned flag can wake a retry backoff as soon as Cancel arrives.
// It remains usable as std::atomic<bool> for existing inference paths.
class AiCancellationFlag final : public std::atomic<bool> {
 public:
  AiCancellationFlag() : std::atomic<bool>(false) {}
  void Cancel() {
    {
      std::lock_guard lock(mutex_);
      store(true, std::memory_order_release);
    }
    changed_.notify_all();
  }
  bool WaitUntil(std::chrono::steady_clock::time_point deadline) const {
    std::unique_lock lock(mutex_);
    return changed_.wait_until(lock, deadline, [this] { return load(std::memory_order_acquire); });
  }

 private:
  mutable std::mutex mutex_;
  mutable std::condition_variable changed_;
};

class RequestScheduler {
 public:
  static constexpr std::size_t kMaxPendingRequestsPerClient = 64;
  uint64_t NextRequestId();
  // Empty client_id is the protocol-v1 legacy namespace. Non-empty ids isolate
  // primary/control connections belonging to one TIP from other TIP instances.
  void RegisterClient(const std::string& client_id);
  void UnregisterClient(const std::string& client_id);
  void Cancel(uint64_t request_id);
  void Cancel(const std::string& client_id, uint64_t request_id);
  bool IsCanceled(uint64_t request_id) const;
  bool IsCanceled(const std::string& client_id, uint64_t request_id) const;
  // Returns nullptr when the client's pending-request or cancellation-state
  // limit is reached. Rejected requests must not call CompleteRequest.
  std::shared_ptr<AiCancellationFlag> TrackCancellation(uint64_t request_id);
  std::shared_ptr<AiCancellationFlag> TrackCancellation(const std::string& client_id,
                                                        uint64_t request_id);
  void CompleteRequest(uint64_t request_id);
  void CompleteRequest(const std::string& client_id, uint64_t request_id);
  void MarkLatest(uint64_t request_id);
  void MarkLatest(const std::string& client_id, uint64_t request_id);
  bool IsLatest(uint64_t request_id) const;
  bool IsLatest(const std::string& client_id, uint64_t request_id) const;

 private:
  struct CancelState {
    std::shared_ptr<AiCancellationFlag> flag;
    std::size_t active_count{0};
  };

  struct ClientState {
    uint64_t latest{};
    std::size_t active_connections{0};
    std::size_t pending_requests{0};
    std::unordered_map<uint64_t, CancelState> cancel_states;
  };

  void PruneInactiveBeforeLocked(ClientState& client, uint64_t request_id);

  std::atomic<uint64_t> request_id_{0};
  mutable std::mutex mutex_;
  std::unordered_map<std::string, ClientState> client_states_;
};

}  // namespace azookey::host
