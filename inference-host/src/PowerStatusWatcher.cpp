#include "azookey/host/PowerStatusWatcher.h"

#include <future>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace azookey::host {

PowerSource QuerySystemPowerSource() { return QueryInferenceThreadEnvironment().power_source; }

PowerStateTracker::PowerStateTracker(PowerSourceProbe probe, Listener listener)
    : probe_(std::move(probe)), listener_(std::move(listener)), current_(Sample()) {}

PowerSource PowerStateTracker::Sample() const {
  if (!probe_) return PowerSource::Unknown;
  try {
    return probe_();
  } catch (...) {
    return PowerSource::Unknown;
  }
}

bool PowerStateTracker::OnPowerBroadcast(uintptr_t event) {
  if (event != kPowerBroadcastStatusChange && event != kPowerBroadcastResumeAutomatic) {
    return false;
  }
  const auto next = Sample();
  const auto previous = current_.exchange(next);
  if (previous == next) return false;
  if (listener_) listener_(previous, next);
  return true;
}

PowerStatusWatcher::PowerStatusWatcher(PowerStateTracker& tracker) : tracker_(tracker) {}

PowerStatusWatcher::~PowerStatusWatcher() { Stop(); }

#ifdef _WIN32

namespace {

constexpr wchar_t kWindowClass[] = L"AzooKeyHostPowerStatusWatcher";

LRESULT CALLBACK PowerWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_NCCREATE: {
      const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
      SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
      break;
    }
    case WM_POWERBROADCAST: {
      auto* tracker =
          reinterpret_cast<PowerStateTracker*>(GetWindowLongPtrW(window, GWLP_USERDATA));
      if (tracker) (void)tracker->OnPowerBroadcast(static_cast<uintptr_t>(wparam));
      return TRUE;
    }
    case WM_CLOSE:
      DestroyWindow(window);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

bool PowerStatusWatcher::Start() {
  if (thread_.joinable()) return window_ != nullptr;
  std::promise<void*> created;
  auto created_future = created.get_future();
  try {
    thread_ = std::thread(
        [this, &created] { Run([&created](void* window) { created.set_value(window); }); });
  } catch (...) {
    return false;
  }
  window_ = created_future.get();
  if (!window_) {
    thread_.join();
    return false;
  }
  return true;
}

void PowerStatusWatcher::Run(std::function<void(void*)> ready) {
  const HINSTANCE instance = GetModuleHandleW(nullptr);
  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.lpfnWndProc = PowerWindowProc;
  window_class.hInstance = instance;
  window_class.lpszClassName = kWindowClass;
  if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    ready(nullptr);
    return;
  }
  // A top-level window that is never shown: WM_POWERBROADCAST goes to
  // top-level windows only.
  HWND window = CreateWindowExW(0, kWindowClass, L"", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr,
                                instance, &tracker_);
  ready(window);
  if (!window) {
    (void)UnregisterClassW(kWindowClass, instance);
    return;
  }
  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  // Fails while another watcher's window still uses the class; that one
  // unregisters it when it ends.
  (void)UnregisterClassW(kWindowClass, instance);
}

void PowerStatusWatcher::Stop() {
  if (!thread_.joinable()) return;
  if (window_) PostMessageW(static_cast<HWND>(window_), WM_CLOSE, 0, 0);
  thread_.join();
  window_ = nullptr;
}

#else

bool PowerStatusWatcher::Start() { return false; }

void PowerStatusWatcher::Run(std::function<void(void*)> ready) { ready(nullptr); }

void PowerStatusWatcher::Stop() {}

#endif

}  // namespace azookey::host
