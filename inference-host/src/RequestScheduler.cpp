#include "azookey/host/RequestScheduler.h"

namespace azookey::host {

uint64_t RequestScheduler::NextRequestId() { return ++request_id_; }

void RequestScheduler::RegisterClient(const std::string& client_id) {
  if (client_id.empty()) return;

  std::lock_guard<std::mutex> lock(mutex_);
  ++client_states_[client_id].active_connections;
}

void RequestScheduler::UnregisterClient(const std::string& client_id) {
  if (client_id.empty()) return;

  std::lock_guard<std::mutex> lock(mutex_);
  const auto client = client_states_.find(client_id);
  if (client == client_states_.end()) return;

  if (client->second.active_connections > 0) {
    --client->second.active_connections;
  }
  if (client->second.active_connections == 0) {
    client_states_.erase(client);
  }
}

void RequestScheduler::Cancel(uint64_t request_id) { Cancel(std::string(), request_id); }

void RequestScheduler::Cancel(const std::string& client_id, uint64_t request_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto& cancel_states = client_states_[client_id].cancel_states;
  auto it = cancel_states.find(request_id);
  if (it == cancel_states.end()) {
    it = cancel_states
             .emplace(request_id, CancelState{std::make_shared<std::atomic<bool>>(false), 0})
             .first;
  }
  it->second.flag->store(true, std::memory_order_release);
}

bool RequestScheduler::IsCanceled(uint64_t request_id) const {
  return IsCanceled(std::string(), request_id);
}

bool RequestScheduler::IsCanceled(const std::string& client_id, uint64_t request_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto client = client_states_.find(client_id);
  if (client == client_states_.end()) return false;
  const auto it = client->second.cancel_states.find(request_id);
  return it != client->second.cancel_states.end() &&
         it->second.flag->load(std::memory_order_acquire);
}

std::shared_ptr<std::atomic<bool>> RequestScheduler::TrackCancellation(uint64_t request_id) {
  return TrackCancellation(std::string(), request_id);
}

std::shared_ptr<std::atomic<bool>> RequestScheduler::TrackCancellation(const std::string& client_id,
                                                                       uint64_t request_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto& state = client_states_[client_id].cancel_states[request_id];
  if (!state.flag) {
    state.flag = std::make_shared<std::atomic<bool>>(false);
  }
  ++state.active_count;
  return state.flag;
}

void RequestScheduler::CompleteRequest(uint64_t request_id) {
  CompleteRequest(std::string(), request_id);
}

void RequestScheduler::CompleteRequest(const std::string& client_id, uint64_t request_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto client = client_states_.find(client_id);
  if (client == client_states_.end()) {
    return;
  }
  auto& cancel_states = client->second.cancel_states;
  auto it = cancel_states.find(request_id);
  if (it == cancel_states.end()) {
    return;
  }
  if (it->second.active_count > 0) {
    --it->second.active_count;
  }
  if (it->second.active_count == 0) {
    cancel_states.erase(it);
  }
}

void RequestScheduler::MarkLatest(uint64_t request_id) { MarkLatest(std::string(), request_id); }

void RequestScheduler::MarkLatest(const std::string& client_id, uint64_t request_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto& client = client_states_[client_id];
  client.latest = request_id;
  PruneInactiveBeforeLocked(client, request_id);
}

bool RequestScheduler::IsLatest(uint64_t request_id) const {
  return IsLatest(std::string(), request_id);
}

bool RequestScheduler::IsLatest(const std::string& client_id, uint64_t request_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto client = client_states_.find(client_id);
  return client != client_states_.end() && client->second.latest == request_id;
}

void RequestScheduler::PruneInactiveBeforeLocked(ClientState& client, uint64_t request_id) {
  for (auto it = client.cancel_states.begin(); it != client.cancel_states.end();) {
    if (it->first <= request_id && it->second.active_count == 0) {
      it = client.cancel_states.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace azookey::host
