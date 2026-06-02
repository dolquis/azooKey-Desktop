#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace azookey::host {

class RequestScheduler {
 public:
  uint64_t NextRequestId();
  void Cancel(uint64_t request_id);
  bool IsCanceled(uint64_t request_id) const;
  std::shared_ptr<std::atomic<bool>> TrackCancellation(uint64_t request_id);
  void CompleteRequest(uint64_t request_id);
  void MarkLatest(uint64_t request_id);
  bool IsLatest(uint64_t request_id) const;

 private:
  struct CancelState {
    std::shared_ptr<std::atomic<bool>> flag;
    std::size_t active_count{0};
  };

  void PruneInactiveBeforeLocked(uint64_t request_id);

  std::atomic<uint64_t> request_id_{0};
  mutable std::mutex mutex_;
  uint64_t latest_{};
  std::unordered_map<uint64_t, CancelState> cancel_states_;
};

}  // namespace azookey::host
