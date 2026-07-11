#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace azookey::host {

class RequestScheduler {
 public:
  uint64_t NextRequestId();
  // Empty client_id is the protocol-v1 legacy namespace. Non-empty ids isolate
  // primary/control connections belonging to one TIP from other TIP instances.
  void RegisterClient(const std::string& client_id);
  void UnregisterClient(const std::string& client_id);
  void Cancel(uint64_t request_id);
  void Cancel(const std::string& client_id, uint64_t request_id);
  bool IsCanceled(uint64_t request_id) const;
  bool IsCanceled(const std::string& client_id, uint64_t request_id) const;
  std::shared_ptr<std::atomic<bool>> TrackCancellation(uint64_t request_id);
  std::shared_ptr<std::atomic<bool>> TrackCancellation(const std::string& client_id,
                                                       uint64_t request_id);
  void CompleteRequest(uint64_t request_id);
  void CompleteRequest(const std::string& client_id, uint64_t request_id);
  void MarkLatest(uint64_t request_id);
  void MarkLatest(const std::string& client_id, uint64_t request_id);
  bool IsLatest(uint64_t request_id) const;
  bool IsLatest(const std::string& client_id, uint64_t request_id) const;

 private:
  struct CancelState {
    std::shared_ptr<std::atomic<bool>> flag;
    std::size_t active_count{0};
  };

  struct ClientState {
    uint64_t latest{};
    std::size_t active_connections{0};
    std::unordered_map<uint64_t, CancelState> cancel_states;
  };

  void PruneInactiveBeforeLocked(ClientState& client, uint64_t request_id);

  std::atomic<uint64_t> request_id_{0};
  mutable std::mutex mutex_;
  std::unordered_map<std::string, ClientState> client_states_;
};

}  // namespace azookey::host
