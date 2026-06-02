#include "azookey/host/RequestScheduler.h"

namespace azookey::host {

uint64_t RequestScheduler::NextRequestId() { return ++request_id_; }

void RequestScheduler::Cancel(uint64_t request_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = cancel_states_.find(request_id);
  if (it == cancel_states_.end()) {
    it = cancel_states_
             .emplace(request_id,
                      CancelState{std::make_shared<std::atomic<bool>>(false), false})
             .first;
  }
  it->second.flag->store(true, std::memory_order_release);
}

bool RequestScheduler::IsCanceled(uint64_t request_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = cancel_states_.find(request_id);
  return it != cancel_states_.end() &&
         it->second.flag->load(std::memory_order_acquire);
}

std::shared_ptr<std::atomic<bool>> RequestScheduler::TrackCancellation(uint64_t request_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto& state = cancel_states_[request_id];
  if (!state.flag) {
    state.flag = std::make_shared<std::atomic<bool>>(false);
  }
  state.active = true;
  return state.flag;
}

void RequestScheduler::CompleteRequest(uint64_t request_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  cancel_states_.erase(request_id);
}

void RequestScheduler::MarkLatest(uint64_t request_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  latest_ = request_id;
  PruneInactiveBeforeLocked(request_id);
}

bool RequestScheduler::IsLatest(uint64_t request_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return latest_ == request_id;
}

void RequestScheduler::PruneInactiveBeforeLocked(uint64_t request_id) {
  for (auto it = cancel_states_.begin(); it != cancel_states_.end();) {
    if (it->first < request_id && !it->second.active) {
      it = cancel_states_.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace azookey::host
