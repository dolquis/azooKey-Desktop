#pragma once

#include <coroutine>

#include "pch.h"

namespace azookey::settings {

// Resumes a coroutine on the dispatcher's thread, so a request run on a background thread can
// update controls when it finishes.
struct DispatcherQueueAwaiter {
  winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher;

  bool await_ready() const noexcept { return false; }

  void await_suspend(std::coroutine_handle<> continuation) const {
    if (!dispatcher.TryEnqueue([continuation]() noexcept { continuation.resume(); })) {
      throw winrt::hresult_error(E_ABORT, L"The UI dispatcher is shutting down.");
    }
  }

  void await_resume() const noexcept {}
};

inline DispatcherQueueAwaiter ResumeForeground(
    winrt::Microsoft::UI::Dispatching::DispatcherQueue const& dispatcher) {
  return {dispatcher};
}

}  // namespace azookey::settings
