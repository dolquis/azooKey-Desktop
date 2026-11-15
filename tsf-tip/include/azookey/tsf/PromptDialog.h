#pragma once

#include <windows.h>

#include <functional>
#include <string>

#include "azookey/tsf/PromptDialogModel.h"

namespace azookey::tsf {

// The M16 prompt window (legacy-parity-spec §4.3, native-ui-spec §4.4): a
// modeless Win32 window owned by the application's top-level window. It runs
// no message loop of its own; the application's loop drives it, so Enter and
// Esc are taken by subclassing its controls instead of IsDialogMessage.
// UI thread only. The object outlives its window: Close() destroys the window
// and the object can be created again.
class PromptDialog {
 public:
  struct Callbacks {
    std::function<void(std::wstring prompt)> submit;
    std::function<void()> apply;
    // Cancel, Esc, the close box, the window losing activation, or the window
    // destroyed with its owner.
    std::function<void()> cancel;
  };

  static constexpr int kIdPrompt = 1001;
  static constexpr int kIdSelection = 1002;
  static constexpr int kIdResult = 1003;
  static constexpr int kIdStatus = 1004;
  static constexpr int kIdApply = 1005;
  static constexpr int kIdSubmit = IDOK;
  static constexpr int kIdCancel = IDCANCEL;

  PromptDialog() = default;
  ~PromptDialog();
  PromptDialog(const PromptDialog&) = delete;
  PromptDialog& operator=(const PromptDialog&) = delete;

  // Creates the window hidden, centered on `owner`. It is owned by `owner`
  // only when that window belongs to this thread: owning a window of another
  // thread would join the two input queues. False when it cannot.
  bool Create(HWND owner, Callbacks callbacks) noexcept;
  // Shows and activates the window with the prompt field focused.
  void Show() noexcept;
  void Update(const PromptDialogView& view) noexcept;
  // Destroys the window without calling back. Safe from inside a callback.
  void Close() noexcept;
  bool IsOpen() const noexcept { return hwnd_ != nullptr; }
  HWND hwnd() const noexcept { return hwnd_; }

#ifdef AZOOKEY_TSF_TESTING
  std::wstring text_for_test(int id) const;
  bool enabled_for_test(int id) const;
  bool visible_for_test(int id) const;
  bool read_only_for_test(int id) const;
  void set_text_for_test(int id, const std::wstring& text);
  void command_for_test(int id);
  void key_for_test(int id, WPARAM key);
  void deactivate_for_test();
#endif

 private:
  static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
  static LRESULT CALLBACK ControlProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
  LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
  void CreateControls();
  // Centered on the owner, or at `suggested` (WM_DPICHANGED).
  void Layout(const RECT* suggested = nullptr);
  void Submit();
  void Apply();
  void Cancel();
  HWND Control(int id) const noexcept;

  HWND hwnd_{nullptr};
  HWND owner_{nullptr};
  HFONT font_{nullptr};
  Callbacks callbacks_;
  PromptDialogState state_{PromptDialogState::Editing};
  bool activated_{false};
  bool closing_{false};
  bool placing_{false};  // Layout() is moving the window.
};

}  // namespace azookey::tsf
