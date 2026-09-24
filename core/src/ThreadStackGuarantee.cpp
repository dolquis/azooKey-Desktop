#include "azookey/core/ThreadStackGuarantee.h"

#ifdef _WIN32
#include <atomic>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace azookey::core {
namespace {
#ifdef _WIN32
constexpr ULONG kCrashFilterStackGuarantee = 64 * 1024;
INIT_ONCE fls_init = INIT_ONCE_STATIC_INIT;
std::atomic<DWORD> fls_index{FLS_OUT_OF_INDEXES};
char stack_guarantee_marker;

BOOL CALLBACK InitializeFls(PINIT_ONCE, PVOID, PVOID*) {
  const auto index = FlsAlloc(nullptr);
  if (index == FLS_OUT_OF_INDEXES) return FALSE;
  fls_index.store(index, std::memory_order_release);
  return TRUE;
}
#endif
}  // namespace

void ReserveCurrentThreadStack() noexcept {
#ifdef _WIN32
  if (!InitOnceExecuteOnce(&fls_init, &InitializeFls, nullptr, nullptr)) return;
  ULONG bytes = kCrashFilterStackGuarantee;
  if (SetThreadStackGuarantee(&bytes)) {
    const auto index = fls_index.load(std::memory_order_acquire);
    (void)FlsSetValue(index, &stack_guarantee_marker);
  }
#endif
}

bool HasCurrentThreadStackGuarantee() noexcept {
#ifdef _WIN32
  const auto index = fls_index.load(std::memory_order_acquire);
  return index != FLS_OUT_OF_INDEXES && FlsGetValue(index) != nullptr;
#else
  return false;
#endif
}

}  // namespace azookey::core
