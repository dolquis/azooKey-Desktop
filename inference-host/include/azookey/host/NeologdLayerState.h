#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "azookey/ipc/Payloads.h"

namespace azookey::host {

// Outcome of the startup neologd pack load, read by QueryDiagnostics
// (auto-word-registration-spec section 15.14, DEV-1534). The pack worker
// writes it and every per-connection Dispatcher reads it, so access is locked.
class NeologdLayerState {
 public:
  void Set(std::string_view state, std::optional<std::string> reason = std::nullopt) {
    std::lock_guard<std::mutex> lock(mutex_);
    status_.state = std::string(state);
    status_.reason = std::move(reason);
  }

  ipc::NeologdLayerStatus Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
  }

 private:
  mutable std::mutex mutex_;
  ipc::NeologdLayerStatus status_;
};

}  // namespace azookey::host
