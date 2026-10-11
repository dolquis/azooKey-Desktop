#pragma once

#include <Windows.h>

#include <cstdint>

namespace azookey::tsf {

// One-shot timers on the TSF UI thread, backed by a message-only window so they
// do not share the candidate window's single scheduled slot (which prediction
// retries already use). The callback runs on the thread that created the
// window, from its message loop. Used for the dynamic punctuation idle timer
// and the bounded wait of the final request (docs/dynamic-punctuation-spec.md
// sections 4.3.1 and 7.3).
class DelayedActionTimer final {
 public:
  enum Slot : unsigned { kIdle = 0, kFinal = 1, kSlotCount = 2 };
  using Callback = void (*)(void* context, Slot slot) noexcept;

  DelayedActionTimer() = default;
  ~DelayedActionTimer();
  DelayedActionTimer(const DelayedActionTimer&) = delete;
  DelayedActionTimer& operator=(const DelayedActionTimer&) = delete;

  bool Create(Callback callback, void* context);
  void Destroy();
  // Re-arms the slot. False when the window is missing or SetTimer failed; the
  // caller must then behave as if the timer fired never (no hidden waiting).
  bool Arm(Slot slot, uint32_t delay_ms);
  void Cancel(Slot slot);
  void CancelAll();
  bool IsArmed(Slot slot) const { return slot < kSlotCount && armed_[slot]; }
  uint32_t ArmedDelayMs(Slot slot) const { return slot < kSlotCount ? delay_ms_[slot] : 0; }
  // Runs the same path as an expired timer, for tests that must not wait.
  void FireForTest(Slot slot);

 private:
  static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
  void OnTimer(Slot slot);

  HWND hwnd_{nullptr};
  Callback callback_{nullptr};
  void* context_{nullptr};
  bool armed_[kSlotCount]{};
  uint32_t delay_ms_[kSlotCount]{};
};

}  // namespace azookey::tsf
