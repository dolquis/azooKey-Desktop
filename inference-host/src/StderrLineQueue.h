#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace azookey::host::detail {

// Synchronization belongs to the caller. Only complete lines enter the queue;
// saturation must never leave a prefix that can join a later diagnostic.
class StderrLineQueue {
 public:
  explicit StderrLineQueue(std::size_t limit = 64 * 1024) : limit_(limit) {}

  void Append(std::string_view text) {
    while (!text.empty()) {
      const auto newline = text.find('\n');
      const bool complete = newline != std::string_view::npos;
      const auto size = complete ? newline + 1 : text.size();
      if (!dropping_) {
        if (size > limit_ - partial_.size()) {
          partial_.clear();
          dropping_ = true;
        } else {
          partial_.append(text.data(), size);
        }
      }
      text.remove_prefix(size);
      if (complete) {
        if (!dropping_ && partial_.size() <= limit_ - pending_.size()) {
          pending_ += partial_;
        }
        partial_.clear();
        dropping_ = false;
      }
    }
  }

  bool Empty() const { return pending_.empty(); }

  std::string Take() {
    std::string result;
    result.swap(pending_);
    return result;
  }

  void Clear() {
    pending_.clear();
    partial_.clear();
    dropping_ = false;
  }

 private:
  const std::size_t limit_;
  std::string pending_;
  std::string partial_;
  bool dropping_ = false;
};

}  // namespace azookey::host::detail
