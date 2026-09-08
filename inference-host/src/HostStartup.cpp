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
#ifdef _WIN32
  std::unique_ptr<void, decltype(&CloseHandle)> process{nullptr, CloseHandle};
#endif
};

SupervisorLifetime::SupervisorLifetime(std::uint32_t pid) : impl_(std::make_unique<Impl>()) {
  impl_->enabled = pid != 0;
#ifdef _WIN32
  if (impl_->enabled) impl_->process.reset(OpenProcess(SYNCHRONIZE, FALSE, pid));
#endif
}

SupervisorLifetime::~SupervisorLifetime() = default;

bool SupervisorLifetime::IsRunning() const {
  if (!impl_->enabled) return true;
#ifdef _WIN32
  return impl_->process && WaitForSingleObject(impl_->process.get(), 0) == WAIT_TIMEOUT;
#else
  return false;
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
