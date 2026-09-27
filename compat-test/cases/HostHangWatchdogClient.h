#pragma once

#include <Windows.h>

#include "cases/HostHangWatchdog.h"
#include "cases/HostProcessSupport.h"

namespace azookey::compat_test::host_hang {

// The child process owns all suspend counts. The parent can only request its
// release and wait for the child to report that every thread was resumed.
class WatchdogClient {
 public:
  explicit WatchdogClient(host_process::HostProcess host) : host_(host) {}
  ~WatchdogClient();

  WatchdogClient(const WatchdogClient&) = delete;
  WatchdogClient& operator=(const WatchdogClient&) = delete;

  bool Start();
  bool active() const;
  ResumeResult Resume();
  HANDLE process() const { return host_process_handle_; }

 private:
  host_process::HostProcess host_;
  HANDLE host_process_handle_{nullptr};
  HANDLE child_process_{nullptr};
  HANDLE ready_{nullptr};
  HANDLE release_{nullptr};
  HANDLE done_{nullptr};
  bool release_requested_{false};
};

}  // namespace azookey::compat_test::host_hang
