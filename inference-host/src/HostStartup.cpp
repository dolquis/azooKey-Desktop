#include "azookey/host/HostStartup.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

#if AZOOKEY_WITH_LLAMA_CPP
#include "ggml-backend.h"
#endif

namespace azookey::host {
struct SupervisorLifetime::Impl {
  bool enabled{false};
  mutable std::uint32_t error{0};
#ifdef _WIN32
  std::unique_ptr<void, decltype(&CloseHandle)> process{nullptr, CloseHandle};
#endif
};

SupervisorLifetime::SupervisorLifetime(std::uint32_t pid) : impl_(std::make_unique<Impl>()) {
  impl_->enabled = pid != 0;
#ifdef _WIN32
  if (impl_->enabled) {
    impl_->process.reset(OpenProcess(SYNCHRONIZE, FALSE, pid));
    if (!impl_->process) impl_->error = GetLastError();
  }
#endif
}

SupervisorLifetime::~SupervisorLifetime() = default;

bool SupervisorLifetime::IsRunning() const { return GetState() == State::Running; }

std::uint32_t SupervisorLifetime::ErrorCode() const { return impl_->error; }

SupervisorLifetime::State SupervisorLifetime::GetState() const {
  if (!impl_->enabled) return State::Running;
#ifdef _WIN32
  if (!impl_->process) return State::Failed;
  const auto wait = WaitForSingleObject(impl_->process.get(), 0);
  if (wait == WAIT_TIMEOUT) return State::Running;
  if (wait == WAIT_OBJECT_0) return State::Exited;
  impl_->error = GetLastError();
  return State::Failed;
#else
  return State::Failed;
#endif
}

std::size_t ProbeVulkanDevices() {
#if AZOOKEY_WITH_LLAMA_CPP
  const auto registry = ggml_backend_reg_by_name("Vulkan");
  return registry ? ggml_backend_reg_dev_count(registry) : 0;
#else
  return 0;
#endif
}
}  // namespace azookey::host
