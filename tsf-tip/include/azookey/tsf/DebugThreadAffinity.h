#pragma once

#ifdef _DEBUG

#include <Windows.h>

#include <cassert>

namespace azookey::tsf {

class DebugThreadAffinity {
 public:
  void BindToCurrentThread() noexcept { thread_id_ = GetCurrentThreadId(); }

  void BindOrAssertCurrentThread() noexcept {
    if (thread_id_ == 0) {
      BindToCurrentThread();
      return;
    }
    AssertCurrentThread();
  }

  [[nodiscard]] bool IsCurrentThread() const noexcept {
    return thread_id_ != 0 && thread_id_ == GetCurrentThreadId();
  }

  void AssertCurrentThreadIfBound() const noexcept {
    if (thread_id_ != 0) AssertCurrentThread();
  }

  void AssertCurrentThread() const noexcept { assert(IsCurrentThread()); }

 private:
  DWORD thread_id_{0};
};

}  // namespace azookey::tsf

#endif
