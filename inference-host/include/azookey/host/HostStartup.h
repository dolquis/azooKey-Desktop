#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace azookey::host {
// Enumerate only. This does not load models, open IPC, or provision user data.
std::size_t ProbeVulkanDevices();

// Retains a process handle so PID reuse cannot keep an orphaned host alive.
class SupervisorLifetime {
 public:
  explicit SupervisorLifetime(std::uint32_t pid);
  ~SupervisorLifetime();
  SupervisorLifetime(const SupervisorLifetime&) = delete;
  SupervisorLifetime& operator=(const SupervisorLifetime&) = delete;
  bool IsRunning() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace azookey::host
