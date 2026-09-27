#pragma once

#include <Windows.h>

#include <cstddef>
#include <vector>

namespace azookey::compat_test::host_hang {

enum class WakeReason { Release, ParentExited, Deadline, Error };
enum class ResumeResult { Failed, ReleasedByRequest, ResumedByWatchdog };

inline ResumeResult ClassifyResumeExitCode(DWORD exit_code) {
  if (exit_code == 0) return ResumeResult::ReleasedByRequest;
  if (exit_code == 5) return ResumeResult::ResumedByWatchdog;
  return ResumeResult::Failed;
}

// The helper waits on [release, parent process] in that order.
inline WakeReason ClassifyWake(DWORD wait_result) {
  if (wait_result == WAIT_OBJECT_0) return WakeReason::Release;
  if (wait_result == WAIT_OBJECT_0 + 1) return WakeReason::ParentExited;
  if (wait_result == WAIT_TIMEOUT) return WakeReason::Deadline;
  return WakeReason::Error;
}

// Records only successful SuspendThread calls. Each successful ResumeThread
// clears exactly one entry, including when suspension was only partial.
class SuspensionLedger {
 public:
  explicit SuspensionLedger(size_t thread_count) : pending_(thread_count, false) {}

  bool MarkSuspended(size_t index) {
    if (index >= pending_.size() || pending_[index]) return false;
    pending_[index] = true;
    return true;
  }

  bool MarkResumed(size_t index) {
    if (index >= pending_.size() || !pending_[index]) return false;
    pending_[index] = false;
    return true;
  }

  bool NeedsResume(size_t index) const { return index < pending_.size() && pending_[index]; }

  bool AllResumed() const {
    for (bool pending : pending_) {
      if (pending) return false;
    }
    return true;
  }

  size_t size() const { return pending_.size(); }

 private:
  std::vector<bool> pending_;
};

}  // namespace azookey::compat_test::host_hang
