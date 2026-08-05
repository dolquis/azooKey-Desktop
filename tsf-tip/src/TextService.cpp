#include "azookey/tsf/TextService.h"

#include <shellscalingapi.h>

#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <new>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>

#include "azookey/ipc/NamedPipeTransport.h"
#include "azookey/ipc/Payloads.h"
#include "azookey/logging/RuntimeLogger.h"
#include "azookey/tsf/DisplayAttribute.h"

namespace {

constexpr const char* kTipVersion = "0.1.0";
constexpr uint32_t kQueryCandidatesFastTimeoutMs = 150;
constexpr uint32_t kCancelConnectTimeoutMs = 200;
constexpr uint32_t kCancelHandshakeTimeoutMs = 500;
constexpr uint32_t kTimeoutCancelConnectTimeoutMs = 10;
constexpr uint32_t kTimeoutCancelHandshakeTimeoutMs = 10;

std::string CreateIpcClientId() {
  GUID guid{};
  if (FAILED(CoCreateGuid(&guid))) return {};

  char buffer[37]{};
  const int written = std::snprintf(
      buffer, sizeof(buffer), "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
      static_cast<unsigned long>(guid.Data1), static_cast<unsigned int>(guid.Data2),
      static_cast<unsigned int>(guid.Data3), static_cast<unsigned int>(guid.Data4[0]),
      static_cast<unsigned int>(guid.Data4[1]), static_cast<unsigned int>(guid.Data4[2]),
      static_cast<unsigned int>(guid.Data4[3]), static_cast<unsigned int>(guid.Data4[4]),
      static_cast<unsigned int>(guid.Data4[5]), static_cast<unsigned int>(guid.Data4[6]),
      static_cast<unsigned int>(guid.Data4[7]));
  return written == 36 ? std::string(buffer, 36) : std::string();
}

azookey::logging::RuntimeLogger& TipRuntimeLogger() {
  static azookey::logging::RuntimeLogger logger(
      azookey::logging::RuntimeLoggerOptionsFromEnvironment("tip"));
  return logger;
}

azookey::logging::RuntimeLogSafeText SafeLogText(std::string value) {
  return azookey::logging::RuntimeLogSafeText(std::move(value));
}

void RuntimeLog(azookey::logging::RuntimeLogLevel level, std::string_view event,
                std::initializer_list<azookey::logging::RuntimeLogField> fields = {}) {
  auto& logger = TipRuntimeLogger();
#ifdef _DEBUG
  const auto record = logger.FormatRecord(level, event, fields);
  if (!record.empty()) OutputDebugStringA(("[azooKey TIP] " + record + "\n").c_str());
#endif
  logger.Log(level, event, fields);
}

std::string IpcHandshakeTokenFromEnv() {
#if defined(_MSC_VER)
  char* value = nullptr;
  size_t length = 0;
  if (_dupenv_s(&value, &length, "AZOOKEY_IPC_HANDSHAKE_TOKEN") != 0 || value == nullptr) {
    return {};
  }
  std::string result(value);
  std::free(value);
  return result;
#else
  const char* value = std::getenv("AZOOKEY_IPC_HANDSHAKE_TOKEN");
  return value ? std::string(value) : std::string();
#endif
}

std::wstring Utf8ToWide(const std::string& utf8) {
  if (utf8.empty()) return {};
  const int len =
      MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
  if (len <= 0) return {};
  std::wstring result(len, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), result.data(), len);
  return result;
}

bool IsVirtualKeyDown(int vk) { return (GetKeyState(vk) & 0x8000) != 0; }

bool HasSystemModifierDown() {
  return IsVirtualKeyDown(VK_CONTROL) || IsVirtualKeyDown(VK_LCONTROL) ||
         IsVirtualKeyDown(VK_RCONTROL) || IsVirtualKeyDown(VK_MENU) || IsVirtualKeyDown(VK_LMENU) ||
         IsVirtualKeyDown(VK_RMENU) || IsVirtualKeyDown(VK_LWIN) || IsVirtualKeyDown(VK_RWIN);
}

struct OemCompositionSymbol {
  char raw;
  std::string surface;
};

std::string ApplyDefaultCompositionPunctuation(const std::string& text) {
  std::string surface;
  for (const char c : text) {
    switch (c) {
      case ',':
        surface += "、";
        break;
      case '.':
        surface += "。";
        break;
      default:
        surface.push_back(c);
        break;
    }
  }
  return surface;
}

std::optional<OemCompositionSymbol> TranslateOemCompositionSymbol(WPARAM virtual_key,
                                                                  LPARAM key_data) {
  if (virtual_key != VK_OEM_COMMA && virtual_key != VK_OEM_PERIOD && virtual_key != VK_OEM_2) {
    return std::nullopt;
  }

  std::array<BYTE, 256> keyboard_state{};
  if (!GetKeyboardState(keyboard_state.data())) return std::nullopt;

  const HKL keyboard_layout = GetKeyboardLayout(0);
  UINT scan_code = static_cast<UINT>((static_cast<ULONG_PTR>(key_data) >> 16) & 0xff);
  if (scan_code == 0) {
    scan_code = MapVirtualKeyExW(static_cast<UINT>(virtual_key), MAPVK_VK_TO_VSC, keyboard_layout);
  }

  std::array<WCHAR, 4> translated{};
  constexpr UINT kDoNotChangeKeyboardState = 1u << 2;
  const int translated_count = ToUnicodeEx(
      static_cast<UINT>(virtual_key), scan_code, keyboard_state.data(), translated.data(),
      static_cast<int>(translated.size()), kDoNotChangeKeyboardState, keyboard_layout);
  if (translated_count != 1) return std::nullopt;

  switch (translated[0]) {
    case L',':
    case L'、':
      return OemCompositionSymbol{',', "、"};
    case L'.':
    case L'。':
      return OemCompositionSymbol{'.', "。"};
    case L'/':
      return OemCompositionSymbol{'/', "/"};
    case L'<':
      return OemCompositionSymbol{'<', "<"};
    case L'>':
      return OemCompositionSymbol{'>', ">"};
    case L'?':
      return OemCompositionSymbol{'?', "?"};
    default:
      return std::nullopt;
  }
}

bool SameComIdentity(IUnknown* lhs, IUnknown* rhs) {
  if (lhs == rhs) return true;
  if (!lhs || !rhs) return lhs == rhs;
  IUnknown* lhs_unknown = nullptr;
  IUnknown* rhs_unknown = nullptr;
  const HRESULT lhs_hr = lhs->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&lhs_unknown));
  const HRESULT rhs_hr = rhs->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&rhs_unknown));
  const bool same = SUCCEEDED(lhs_hr) && SUCCEEDED(rhs_hr) && lhs_unknown == rhs_unknown;
  if (lhs_unknown) lhs_unknown->Release();
  if (rhs_unknown) rhs_unknown->Release();
  return same;
}

using GetGuiThreadInfoFn = BOOL(WINAPI*)(DWORD, PGUITHREADINFO);
using ClientToScreenFn = BOOL(WINAPI*)(HWND, LPPOINT);
using GetPhysicalCursorPosFn = BOOL(WINAPI*)(LPPOINT);
using LogicalToPhysicalPointForPerMonitorDpiFn = BOOL(WINAPI*)(HWND, LPPOINT);
using GetMonitorScalePercentFn = UINT (*)(POINT);

struct CaretWin32Api {
  GetGuiThreadInfoFn get_gui_thread_info;
  ClientToScreenFn client_to_screen;
  GetPhysicalCursorPosFn get_physical_cursor_pos;
  LogicalToPhysicalPointForPerMonitorDpiFn logical_to_physical_point;
  GetMonitorScalePercentFn get_monitor_scale_percent;
};

struct CaretAnchor {
  POINT point{0, 0};
  bool valid{false};
};

constexpr UINT kDefaultMonitorScalePercent = 100;

UINT GetMonitorScalePercent(POINT screen_point) {
  const HMONITOR monitor = MonitorFromPoint(screen_point, MONITOR_DEFAULTTONEAREST);
  if (!monitor) return kDefaultMonitorScalePercent;

  DEVICE_SCALE_FACTOR scale_factor = SCALE_100_PERCENT;
  if (FAILED(GetScaleFactorForMonitor(monitor, &scale_factor))) {
    return kDefaultMonitorScalePercent;
  }
  return static_cast<UINT>(scale_factor);
}

CaretWin32Api DefaultCaretWin32Api() {
  return {&::GetGUIThreadInfo, &::ClientToScreen, &::GetPhysicalCursorPos,
          &::LogicalToPhysicalPointForPerMonitorDPI, &GetMonitorScalePercent};
}

#ifdef AZOOKEY_TSF_TESTING
CaretWin32Api g_caret_win32_api = DefaultCaretWin32Api();
#endif

CaretWin32Api CurrentCaretWin32Api() {
#ifdef AZOOKEY_TSF_TESTING
  return g_caret_win32_api;
#else
  return DefaultCaretWin32Api();
#endif
}

bool IsUsableTextExtent(const RECT& rect) {
  return rect.right > rect.left && rect.bottom > rect.top;
}

bool IsZeroRect(const RECT& rect) {
  return rect.left == 0 && rect.top == 0 && rect.right == 0 && rect.bottom == 0;
}

constexpr LONG kCursorFallbackCaretHeight = 16;

CaretAnchor ResolveCaretAnchor(const RECT* text_ext_rect, HWND text_extent_window = nullptr) {
  const CaretWin32Api api = CurrentCaretWin32Api();
  if (text_ext_rect && IsUsableTextExtent(*text_ext_rect)) {
    POINT point{text_ext_rect->left, text_ext_rect->bottom};
    if (text_extent_window && api.logical_to_physical_point) {
      POINT physical_point = point;
      if (api.logical_to_physical_point(text_extent_window, &physical_point)) {
        point = physical_point;
      }
    }
    return {point, true};
  }

  GUITHREADINFO thread_info{};
  thread_info.cbSize = sizeof(thread_info);
  if (api.get_gui_thread_info && api.client_to_screen && api.get_gui_thread_info(0, &thread_info) &&
      thread_info.hwndCaret && !IsZeroRect(thread_info.rcCaret)) {
    POINT point{thread_info.rcCaret.left, thread_info.rcCaret.bottom};
    if (api.client_to_screen(thread_info.hwndCaret, &point)) {
      if (api.logical_to_physical_point) {
        POINT physical_point = point;
        if (api.logical_to_physical_point(thread_info.hwndCaret, &physical_point)) {
          point = physical_point;
        }
      }
      return {point, true};
    }
  }

  POINT point{};
  if (api.get_physical_cursor_pos && api.get_physical_cursor_pos(&point)) {
    const UINT scale_percent = api.get_monitor_scale_percent ? api.get_monitor_scale_percent(point)
                                                             : kDefaultMonitorScalePercent;
    point.y += scale_percent > 0
                   ? MulDiv(kCursorFallbackCaretHeight, static_cast<int>(scale_percent), 100)
                   : kCursorFallbackCaretHeight;
    return {point, true};
  }
  return {};
}

bool IsExpectedIpcResponse(const azookey::ipc::Envelope& response, uint64_t expected_request_id,
                           azookey::ipc::MessageType expected_type) {
  return (expected_request_id == 0 || response.request_id == expected_request_id) &&
         response.type == expected_type;
}

#ifdef AZOOKEY_TSF_TESTING
std::atomic<int> g_com_boundary_allocation_failures{0};
std::atomic<int> g_pending_commit_observation_failures{0};
#endif

template <typename T, typename... Args>
T* NewComBoundaryObject(Args&&... args) {
#ifdef AZOOKEY_TSF_TESTING
  if (azookey::tsf::testing::ConsumeComBoundaryAllocationFailureForTest()) {
    return nullptr;
  }
#endif
  return new (std::nothrow) T(std::forward<Args>(args)...);
}

}  // namespace

namespace azookey::tsf {

#ifdef AZOOKEY_TSF_TESTING
namespace testing {

void FailNextComBoundaryAllocationForTest() { g_com_boundary_allocation_failures.store(1); }

void ClearComBoundaryAllocationFailureForTest() { g_com_boundary_allocation_failures.store(0); }

bool ConsumeComBoundaryAllocationFailureForTest() {
  int remaining = g_com_boundary_allocation_failures.load();
  while (remaining > 0) {
    if (g_com_boundary_allocation_failures.compare_exchange_weak(remaining, remaining - 1)) {
      return true;
    }
  }
  return false;
}

void FailNextPendingCommitObservationForTest() { g_pending_commit_observation_failures.store(1); }

void ClearPendingCommitObservationFailureForTest() {
  g_pending_commit_observation_failures.store(0);
}

bool ConsumePendingCommitObservationFailureForTest() {
  int remaining = g_pending_commit_observation_failures.load();
  while (remaining > 0) {
    if (g_pending_commit_observation_failures.compare_exchange_weak(remaining, remaining - 1)) {
      return true;
    }
  }
  return false;
}

bool IsExpectedIpcResponseForTest(const ipc::Envelope& response, uint64_t expected_request_id,
                                  ipc::MessageType expected_type) {
  return IsExpectedIpcResponse(response, expected_request_id, expected_type);
}

void SetCaretWin32ApiForTest(
    GetGuiThreadInfoFnForTest get_gui_thread_info, ClientToScreenFnForTest client_to_screen,
    GetPhysicalCursorPosFnForTest get_physical_cursor_pos,
    LogicalToPhysicalPointForPerMonitorDpiFnForTest logical_to_physical_point,
    GetMonitorScalePercentFnForTest get_monitor_scale_percent) {
  g_caret_win32_api = {get_gui_thread_info, client_to_screen, get_physical_cursor_pos,
                       logical_to_physical_point, get_monitor_scale_percent};
}

void ClearCaretWin32ApiForTest() { g_caret_win32_api = DefaultCaretWin32Api(); }

CaretAnchorForTest ResolveCaretAnchorForTest(const RECT* text_ext_rect, HWND text_extent_window) {
  const CaretAnchor anchor = ResolveCaretAnchor(text_ext_rect, text_extent_window);
  return {anchor.point, anchor.valid};
}

}  // namespace testing
#endif

TextService::TextService() : ipc_client_id_(CreateIpcClientId()) {
  if (ipc_client_id_.empty()) {
    RuntimeLog(azookey::logging::RuntimeLogLevel::Warn, "ipc_client_id_fallback");
  }
}

TextService::~TextService() {
  StopIpcWorker();
  ClearCommitContext();
}

STDMETHODIMP TextService::QueryInterface(REFIID riid, void** ppvObj) {
  if (!ppvObj) return E_POINTER;
  *ppvObj = nullptr;
  if (riid == IID_IUnknown || riid == IID_ITfTextInputProcessor ||
      riid == IID_ITfTextInputProcessorEx) {
    *ppvObj = static_cast<ITfTextInputProcessorEx*>(this);
  } else if (riid == IID_ITfKeyEventSink) {
    *ppvObj = static_cast<ITfKeyEventSink*>(this);
  } else if (riid == IID_ITfThreadMgrEventSink) {
    *ppvObj = static_cast<ITfThreadMgrEventSink*>(this);
  } else if (riid == IID_ITfCompositionSink) {
    *ppvObj = static_cast<ITfCompositionSink*>(this);
  } else if (riid == IID_ITfDisplayAttributeProvider) {
    *ppvObj = static_cast<ITfDisplayAttributeProvider*>(this);
  } else {
    return E_NOINTERFACE;
  }
  AddRef();
  return S_OK;
}

STDMETHODIMP_(ULONG) TextService::AddRef() {
  return static_cast<ULONG>(InterlockedIncrement(&ref_count_));
}
STDMETHODIMP_(ULONG) TextService::Release() {
  const auto c = static_cast<ULONG>(InterlockedDecrement(&ref_count_));
  if (c == 0) delete this;
  return c;
}

STDMETHODIMP TextService::Activate(ITfThreadMgr* ptim, TfClientId tid) {
  return ActivateEx(ptim, tid, 0);
}

STDMETHODIMP TextService::ActivateEx(ITfThreadMgr* ptim, TfClientId tid, DWORD dwFlags) {
  // dwFlags does not officially enumerate the UIElement bit, so UI-less state
  // is taken from ITfThreadMgrEx::GetActiveFlags instead (spec §2.10).
  UNREFERENCED_PARAMETER(dwFlags);
  if (!ptim) return E_INVALIDARG;
  if (thread_mgr_) return E_UNEXPECTED;

  thread_mgr_ = ptim;
  client_id_ = tid;
  thread_mgr_->AddRef();

  // Detect UI-less mode (Windows 11 / Office). ITfThreadMgrEx may be absent on
  // older hosts; default to false and treat any failure as "not UI-less".
  ui_less_mode_ = false;
  {
    ITfThreadMgrEx* thread_mgr_ex = nullptr;
    if (SUCCEEDED(thread_mgr_->QueryInterface(IID_ITfThreadMgrEx,
                                              reinterpret_cast<void**>(&thread_mgr_ex))) &&
        thread_mgr_ex) {
      DWORD active_flags = 0;
      if (SUCCEEDED(thread_mgr_ex->GetActiveFlags(&active_flags)))
        ui_less_mode_ = (active_flags & TF_TMF_UIELEMENTENABLEDONLY) != 0;
      thread_mgr_ex->Release();
    }
  }
  candidate_ui_.SetUiLessMode(ui_less_mode_);

  HRESULT hr = AdviseTextServiceSinks();
  if (FAILED(hr)) {
    RuntimeLog(azookey::logging::RuntimeLogLevel::Error, "tsf_sink_advise_failed");
    UnadviseTextServiceSinks();
    thread_mgr_->Release();
    thread_mgr_ = nullptr;
    client_id_ = TF_CLIENTID_NULL;
    return hr;
  }

  candidate_ui_.Create();
  candidate_ui_.SetOnClick([this](int idx) {
    selected_candidate_idx_ = idx;
    if (active_context_) CommitSelected(active_context_);
  });
  candidate_ui_.SetOnCandidatesReady(&TextService::OnCandidatesReady, this);

  StartIpcWorker();
  return S_OK;
}

STDMETHODIMP TextService::Deactivate() {
  HRESULT result = UnadviseTextServiceSinks();

  StopIpcWorker();

  CleanupForLifecycleLoss(active_context_, /*release_active_context=*/false,
                          LifecycleCleanupFailurePolicy::ReleaseComposition);
  candidate_ui_.Destroy();

  if (active_context_) {
    active_context_->Release();
    active_context_ = nullptr;
  }
  if (thread_mgr_) {
    thread_mgr_->Release();
    thread_mgr_ = nullptr;
  }
  client_id_ = TF_CLIENTID_NULL;
  return result;
}

HRESULT TextService::AdviseTextServiceSinks() {
  if (!thread_mgr_) return E_UNEXPECTED;

  ITfKeystrokeMgr* key_mgr = nullptr;
  HRESULT hr = thread_mgr_->QueryInterface(IID_ITfKeystrokeMgr, reinterpret_cast<void**>(&key_mgr));
  if (FAILED(hr) || !key_mgr) return FAILED(hr) ? hr : E_NOINTERFACE;

  hr = key_mgr->AdviseKeyEventSink(client_id_, this, TRUE);
  key_mgr->Release();
  if (FAILED(hr)) return hr;
  key_event_sink_advised_ = true;

  ITfSource* source = nullptr;
  hr = thread_mgr_->QueryInterface(IID_ITfSource, reinterpret_cast<void**>(&source));
  if (FAILED(hr) || !source) return FAILED(hr) ? hr : E_NOINTERFACE;

  hr = source->AdviseSink(IID_ITfThreadMgrEventSink, static_cast<ITfThreadMgrEventSink*>(this),
                          &thread_mgr_sink_cookie_);
  source->Release();
  if (FAILED(hr)) return hr;
  return S_OK;
}

HRESULT TextService::UnadviseTextServiceSinks() {
  HRESULT result = S_OK;

  if (thread_mgr_ && thread_mgr_sink_cookie_ != TF_INVALID_COOKIE) {
    ITfSource* source = nullptr;
    HRESULT hr = thread_mgr_->QueryInterface(IID_ITfSource, reinterpret_cast<void**>(&source));
    if (SUCCEEDED(hr) && source) {
      hr = source->UnadviseSink(thread_mgr_sink_cookie_);
      source->Release();
    }
    if (FAILED(hr)) {
      RuntimeLog(azookey::logging::RuntimeLogLevel::Warn, "thread_manager_sink_unadvise_failed");
      result = hr;
    } else {
      thread_mgr_sink_cookie_ = TF_INVALID_COOKIE;
    }
  }

  if (thread_mgr_ && key_event_sink_advised_) {
    ITfKeystrokeMgr* key_mgr = nullptr;
    HRESULT hr =
        thread_mgr_->QueryInterface(IID_ITfKeystrokeMgr, reinterpret_cast<void**>(&key_mgr));
    if (SUCCEEDED(hr) && key_mgr) {
      hr = key_mgr->UnadviseKeyEventSink(client_id_);
      key_mgr->Release();
    }
    if (FAILED(hr)) {
      RuntimeLog(azookey::logging::RuntimeLogLevel::Warn, "key_event_sink_unadvise_failed");
      if (SUCCEEDED(result)) result = hr;
    } else {
      key_event_sink_advised_ = false;
    }
  }

  return result;
}

STDMETHODIMP TextService::OnSetFocus(BOOL foreground) {
  if (!foreground) {
    CleanupForLifecycleLoss(active_context_, /*release_active_context=*/true,
                            LifecycleCleanupFailurePolicy::PreserveComposition);
  }
  return S_OK;
}

STDMETHODIMP TextService::OnTestKeyDown(ITfContext* context, WPARAM wParam, LPARAM lParam,
                                        BOOL* eaten) {
  UNREFERENCED_PARAMETER(context);
  if (!eaten) return E_INVALIDARG;
  *eaten = FALSE;
  try {
#ifdef AZOOKEY_TSF_TESTING
    if (testing::ConsumeComBoundaryAllocationFailureForTest()) {
      throw std::bad_alloc();
    }
#endif

    if (HasSystemModifierDown()) return S_OK;
    if (committing_) {
      *eaten = TRUE;
      return S_OK;
    }

    const bool has_preedit = !preedit_kana_.empty() || romaji_.HasPending();
    const bool cand_visible = candidate_ui_.IsShowing();
    const auto composition_symbol = TranslateOemCompositionSymbol(wParam, lParam);

    if (wParam >= 'A' && wParam <= 'Z') {
      *eaten = TRUE;
    } else if (wParam == VK_OEM_MINUS || wParam == VK_SUBTRACT) {
      // 長音: ハイフンキー（主キー・テンキー）は composition 中のみ長音符「ー」として取り込む。
      // composition が無いときは通常のハイフンとしてアプリへ通す。
      *eaten = has_preedit ? TRUE : FALSE;
    } else if (composition_symbol) {
      // 明示句読点と slash は composition 中だけ取り込む。composition が無いときは
      // アプリへパススルーし、通常の記号入力を横取りしない。
      *eaten = has_preedit ? TRUE : FALSE;
    } else if (wParam == VK_BACK) {
      *eaten = has_preedit ? TRUE : FALSE;
    } else if (wParam == VK_SPACE) {
      *eaten = (has_preedit || cand_visible) ? TRUE : FALSE;
    } else if (wParam == VK_UP || wParam == VK_DOWN) {
      *eaten = cand_visible ? TRUE : FALSE;
    } else if (wParam == VK_RETURN) {
      *eaten = (cand_visible || has_preedit) ? TRUE : FALSE;
    } else if (wParam == VK_ESCAPE) {
      *eaten = (cand_visible || has_preedit) ? TRUE : FALSE;
    } else if (wParam >= '1' && wParam <= '9') {
      *eaten = cand_visible ? TRUE : FALSE;
    }
    return S_OK;
  } catch (const std::bad_alloc&) {
    return E_OUTOFMEMORY;
  } catch (...) {
    return E_FAIL;
  }
}

STDMETHODIMP TextService::OnTestKeyUp(ITfContext* context, WPARAM wParam, LPARAM lParam,
                                      BOOL* eaten) {
  UNREFERENCED_PARAMETER(context);
  UNREFERENCED_PARAMETER(wParam);
  UNREFERENCED_PARAMETER(lParam);
  if (!eaten) return E_INVALIDARG;
  *eaten = FALSE;
  return S_OK;
}

STDMETHODIMP TextService::OnKeyDown(ITfContext* context, WPARAM wParam, LPARAM lParam,
                                    BOOL* eaten) {
  if (!eaten) return E_INVALIDARG;
  *eaten = FALSE;
  try {
    if (HasSystemModifierDown()) return S_OK;

    if (committing_) {
      ITfContext* retry_context = commit_context_ ? commit_context_ : active_context_;
      const HRESULT retry_hr = retry_context ? RequestCommitEditSession(retry_context) : E_FAIL;
      if (retry_hr == E_OUTOFMEMORY) return retry_hr;
      if (SUCCEEDED(retry_hr)) {
        preedit_kana_.clear();
        romaji_.Reset();
        ClearBatchState();
        *eaten = TRUE;
        return S_OK;
      } else {
        // Keep queued commit and preserved preedit isolated from new input.
        *eaten = TRUE;
        return S_OK;
      }
    }

    struct PreeditRollbackState {
      std::string preedit;
      core::RomajiKanaConverter romaji;
      bool candidate_window_visible{false};
      int selected_candidate_idx{0};
      std::vector<ipc::CandidateField> shown_candidates;
      std::vector<ipc::CandidateField> cached_candidates;
      bool candidate_window_show_pending{false};
      std::string batch_raw_romaji;
      bool batch_query_in_progress{false};
    };
    auto capture_preedit_rollback_state = [&]() {
      PreeditRollbackState state;
      state.preedit = preedit_kana_;
      state.romaji = romaji_;
      state.candidate_window_visible = candidate_ui_.IsShowing();
      state.selected_candidate_idx = selected_candidate_idx_;
      state.shown_candidates = shown_candidates_;
      state.batch_raw_romaji = batch_raw_romaji_;
      state.batch_query_in_progress = batch_query_in_progress_;
      {
        std::lock_guard<std::mutex> lk(candidates_mtx_);
        state.cached_candidates = candidates_;
        state.candidate_window_show_pending = candidate_window_show_pending_;
      }
      return state;
    };
    auto restore_preedit_state = [&](const PreeditRollbackState& state) {
      preedit_kana_ = state.preedit;
      romaji_ = state.romaji;
      selected_candidate_idx_ = state.selected_candidate_idx;
      shown_candidates_ = state.shown_candidates;
      batch_raw_romaji_ = state.batch_raw_romaji;
      batch_query_in_progress_ = state.batch_query_in_progress;
      {
        std::lock_guard<std::mutex> lk(candidates_mtx_);
        candidates_ = state.cached_candidates;
        candidate_window_show_pending_ = state.candidate_window_show_pending;
      }
      if (state.candidate_window_visible && !state.shown_candidates.empty()) {
        std::vector<std::wstring> items;
        for (const auto& c : state.shown_candidates) items.push_back(Utf8ToWide(c.surface));
        const POINT pt = CandidateAnchorPoint();
        candidate_ui_.BeginUI(thread_mgr_, pt, items, state.selected_candidate_idx);
      } else {
        candidate_ui_.EndUI();
      }
    };
    auto request_preedit_update_or_restore_on_oom =
        [&](const PreeditRollbackState& rollback_state) -> HRESULT {
      const HRESULT update_hr = RequestPreeditUpdate(context);
      if (update_hr == E_OUTOFMEMORY) {
        restore_preedit_state(rollback_state);
        return update_hr;
      }
      return S_OK;
    };

    const bool cand_visible = candidate_ui_.IsShowing();
    const bool has_preedit = !preedit_kana_.empty() || romaji_.HasPending();
    const auto composition_symbol = TranslateOemCompositionSymbol(wParam, lParam);
    if (wParam >= 'A' && wParam <= 'Z') {
      const auto rollback_state = capture_preedit_rollback_state();
      // Hide candidate window when the user resumes typing.
      if (cand_visible) {
        candidate_ui_.EndUI();
        selected_candidate_idx_ = 0;
      }
      // Always clear stale candidates when the reading changes, not only when
      // the window is visible — otherwise a Space press before the fresh
      // QueryCandidates response arrives snapshots the old cache.
      {
        std::lock_guard<std::mutex> lk(candidates_mtx_);
        candidates_.clear();
        candidate_window_show_pending_ = false;
      }
      if (BatchRomajiEnabled()) {
        batch_raw_romaji_.push_back(
            static_cast<char>(std::tolower(static_cast<unsigned char>(wParam))));
        RefreshBatchPreeditSurface();
        batch_query_in_progress_ = false;
        CancelPendingQueriesForLifecycle();
      } else {
        preedit_kana_ += romaji_.Feed(static_cast<char>(wParam));
      }
      const HRESULT update_hr = request_preedit_update_or_restore_on_oom(rollback_state);
      if (FAILED(update_hr)) return update_hr;
      if (!BatchRomajiEnabled()) PostQueryCandidates(CurrentPreeditSurface());
      *eaten = TRUE;

    } else if ((wParam == VK_OEM_MINUS || wParam == VK_SUBTRACT) &&
               (BatchRomajiEnabled() ? !batch_raw_romaji_.empty()
                                     : (!preedit_kana_.empty() || romaji_.HasPending()))) {
      // 長音: composition 中のハイフンキー（主キー・テンキー）を長音符「ー」として
      // preedit に取り込む。composition が無いときは本分岐に入らず、ハイフンはアプリへ
      // パススルーする。
      const auto rollback_state = capture_preedit_rollback_state();
      if (cand_visible) {
        candidate_ui_.EndUI();
        selected_candidate_idx_ = 0;
      }
      {
        std::lock_guard<std::mutex> lk(candidates_mtx_);
        candidates_.clear();
        candidate_window_show_pending_ = false;
      }
      if (BatchRomajiEnabled()) {
        batch_raw_romaji_.push_back('-');
        RefreshBatchPreeditSurface();
        batch_query_in_progress_ = false;
        CancelPendingQueriesForLifecycle();
      } else {
        preedit_kana_ += romaji_.Feed('-');
      }
      const HRESULT update_hr = request_preedit_update_or_restore_on_oom(rollback_state);
      if (FAILED(update_hr)) return update_hr;
      if (!BatchRomajiEnabled()) PostQueryCandidates(CurrentPreeditSurface());
      *eaten = TRUE;

    } else if (composition_symbol && has_preedit) {
      const auto rollback_state = capture_preedit_rollback_state();
      if (cand_visible) {
        candidate_ui_.EndUI();
        selected_candidate_idx_ = 0;
      }
      {
        std::lock_guard<std::mutex> lk(candidates_mtx_);
        candidates_.clear();
        candidate_window_show_pending_ = false;
      }
      if (BatchRomajiEnabled()) {
        batch_raw_romaji_.push_back(composition_symbol->raw);
        RefreshBatchPreeditSurface();
        batch_query_in_progress_ = false;
        CancelPendingQueriesForLifecycle();
      } else {
        preedit_kana_ += romaji_.Flush();
        preedit_kana_ += composition_symbol->surface;
      }
      const HRESULT update_hr = request_preedit_update_or_restore_on_oom(rollback_state);
      if (FAILED(update_hr)) return update_hr;
      if (!BatchRomajiEnabled()) PostQueryCandidates(CurrentPreeditSurface());
      *eaten = TRUE;

    } else if (wParam == VK_BACK) {
      const auto rollback_state = capture_preedit_rollback_state();
      if (cand_visible) {
        candidate_ui_.EndUI();
        selected_candidate_idx_ = 0;
      }
      if (BatchRomajiEnabled() && !batch_raw_romaji_.empty()) {
        {
          std::lock_guard<std::mutex> lk(candidates_mtx_);
          candidates_.clear();
          candidate_window_show_pending_ = false;
        }
        batch_raw_romaji_.pop_back();
        RefreshBatchPreeditSurface();
        batch_query_in_progress_ = false;
        CancelPendingQueriesForLifecycle();
        const HRESULT update_hr = request_preedit_update_or_restore_on_oom(rollback_state);
        if (FAILED(update_hr)) return update_hr;
        *eaten = TRUE;
      } else if (romaji_.HasPending()) {
        {
          std::lock_guard<std::mutex> lk(candidates_mtx_);
          candidates_.clear();
          candidate_window_show_pending_ = false;
        }
        romaji_.PopPendingPreview();
        const HRESULT update_hr = request_preedit_update_or_restore_on_oom(rollback_state);
        if (FAILED(update_hr)) return update_hr;
        const std::string reading = CurrentPreeditSurface();
        if (!reading.empty()) PostQueryCandidates(reading);
        *eaten = TRUE;
      } else if (!preedit_kana_.empty()) {
        {
          std::lock_guard<std::mutex> lk(candidates_mtx_);
          candidates_.clear();
          candidate_window_show_pending_ = false;
        }
        auto& s = preedit_kana_;
        size_t i = s.size();
        while (i > 0 && (s[i - 1] & 0xC0) == 0x80) --i;
        if (i > 0) --i;
        s.erase(i);
        const HRESULT update_hr = request_preedit_update_or_restore_on_oom(rollback_state);
        if (FAILED(update_hr)) return update_hr;
        const std::string reading = CurrentPreeditSurface();
        if (!reading.empty()) PostQueryCandidates(reading);
        *eaten = TRUE;
      }

    } else if (wParam == VK_SPACE) {
      if (BatchRomajiEnabled() && !batch_raw_romaji_.empty()) {
        *eaten = TRUE;
        if (cand_visible) {
          const auto rollback_state = capture_preedit_rollback_state();
          const HRESULT move_hr = candidate_ui_.MoveSelection(+1);
          if (FAILED(move_hr)) return move_hr;
          selected_candidate_idx_ = candidate_ui_.GetSelected();
          const HRESULT update_hr = request_preedit_update_or_restore_on_oom(rollback_state);
          if (FAILED(update_hr)) return update_hr;
        } else if (batch_query_in_progress_) {
          // Batch conversion is already waiting for a response.  Eat repeated
          // Space presses without re-sending the same request.
        } else {
          const std::string reading = BatchReadingForConversion();
          {
            std::lock_guard<std::mutex> lk(candidates_mtx_);
            candidates_.clear();
            shown_candidates_.clear();
            candidate_window_show_pending_ = true;
          }
          batch_query_in_progress_ = true;
          PostBatchConversion(reading, batch_raw_romaji_);
        }
      } else {
        // Flush any pending romaji so the reading is complete.
        const auto rollback_state = capture_preedit_rollback_state();
        const std::string flushed = romaji_.Flush();
        if (!flushed.empty()) {
          preedit_kana_ += flushed;
          const HRESULT update_hr = request_preedit_update_or_restore_on_oom(rollback_state);
          if (FAILED(update_hr)) return update_hr;
          // Reading changed due to romaji flush; old candidates are now stale.
          // Clear them and query for the updated reading so the candidate window
          // (opened below) reflects the complete input, not the pre-flush state.
          {
            std::lock_guard<std::mutex> lk(candidates_mtx_);
            candidates_.clear();
            candidate_window_show_pending_ = false;
          }
          PostQueryCandidates(CurrentPreeditSurface());
        }
        if (!preedit_kana_.empty()) {
          // Always eat Space during preedit — even if candidates haven't arrived
          // yet — to prevent a literal space leaking into the application.
          *eaten = TRUE;
          if (cand_visible) {
            // Cycle to next candidate using the existing snapshot.
            const HRESULT move_hr = candidate_ui_.MoveSelection(+1);
            if (FAILED(move_hr)) return move_hr;
            selected_candidate_idx_ = candidate_ui_.GetSelected();
            const HRESULT update_hr = request_preedit_update_or_restore_on_oom(rollback_state);
            if (FAILED(update_hr)) return update_hr;
          } else {
            // Snapshot the candidate list so commit always reflects what was
            // displayed, even if a late QueryCandidates response arrives later.
            std::vector<ipc::CandidateField> snapshot;
            bool wait_for_candidates = false;
            {
              std::lock_guard<std::mutex> lk(candidates_mtx_);
              if (candidates_.empty()) {
                candidate_window_show_pending_ = true;
                wait_for_candidates = true;
              } else {
                candidate_window_show_pending_ = false;
                shown_candidates_ = candidates_;
                snapshot = shown_candidates_;
              }
            }
            if (!wait_for_candidates) {
              std::vector<std::wstring> items;
              for (const auto& c : snapshot) items.push_back(Utf8ToWide(c.surface));
              if (!items.empty()) {
                selected_candidate_idx_ = 0;
                const POINT pt = CandidateAnchorPoint();
                const HRESULT begin_hr = candidate_ui_.BeginUI(thread_mgr_, pt, items, 0);
                if (FAILED(begin_hr)) return begin_hr;
                const HRESULT update_hr = request_preedit_update_or_restore_on_oom(rollback_state);
                if (FAILED(update_hr)) return update_hr;
              }
            }
          }
        }
      }

    } else if (wParam == VK_UP) {
      if (cand_visible) {
        const auto rollback_state = capture_preedit_rollback_state();
        const HRESULT move_hr = candidate_ui_.MoveSelection(-1);
        if (FAILED(move_hr)) return move_hr;
        selected_candidate_idx_ = candidate_ui_.GetSelected();
        const HRESULT update_hr = request_preedit_update_or_restore_on_oom(rollback_state);
        if (FAILED(update_hr)) return update_hr;
        *eaten = TRUE;
      }

    } else if (wParam == VK_DOWN) {
      if (cand_visible) {
        const auto rollback_state = capture_preedit_rollback_state();
        const HRESULT move_hr = candidate_ui_.MoveSelection(+1);
        if (FAILED(move_hr)) return move_hr;
        selected_candidate_idx_ = candidate_ui_.GetSelected();
        const HRESULT update_hr = request_preedit_update_or_restore_on_oom(rollback_state);
        if (FAILED(update_hr)) return update_hr;
        *eaten = TRUE;
      }

    } else if (wParam == VK_RETURN) {
      if (cand_visible) {
        const HRESULT commit_hr = CommitSelected(context);
        if (FAILED(commit_hr)) return commit_hr;
        *eaten = TRUE;
      } else if (BatchRomajiEnabled() && batch_query_in_progress_) {
        *eaten = TRUE;
      } else if (!preedit_kana_.empty() || romaji_.HasPending()) {
        const HRESULT commit_hr = CommitPreeditAsIs(context);
        if (FAILED(commit_hr)) return commit_hr;
        *eaten = TRUE;
      }

    } else if (wParam >= '1' && wParam <= '9') {
      if (cand_visible) {
        int idx = static_cast<int>(wParam - '1');
        if (idx < candidate_ui_.GetCount()) {
          selected_candidate_idx_ = idx;
          const HRESULT commit_hr = CommitSelected(context);
          if (FAILED(commit_hr)) return commit_hr;
        }
        *eaten = TRUE;
      }

    } else if (wParam == VK_ESCAPE) {
      if (cand_visible) {
        const auto rollback_state = capture_preedit_rollback_state();
        candidate_ui_.EndUI();
        selected_candidate_idx_ = 0;
        const HRESULT update_hr = request_preedit_update_or_restore_on_oom(rollback_state);
        if (FAILED(update_hr)) return update_hr;
        *eaten = TRUE;
      } else if (BatchRomajiEnabled() && batch_query_in_progress_) {
        CancelPendingQueriesForLifecycle();
        {
          std::lock_guard<std::mutex> lk(candidates_mtx_);
          candidates_.clear();
          candidate_window_show_pending_ = false;
        }
        batch_query_in_progress_ = false;
        *eaten = TRUE;
      } else if (!preedit_kana_.empty() || romaji_.HasPending()) {
        const auto rollback_state = capture_preedit_rollback_state();
        preedit_kana_.clear();
        romaji_.Reset();
        ClearBatchState();
        {
          std::lock_guard<std::mutex> lk(candidates_mtx_);
          candidates_.clear();
          candidate_window_show_pending_ = false;
        }
        const HRESULT update_hr = request_preedit_update_or_restore_on_oom(rollback_state);
        if (FAILED(update_hr)) return update_hr;
        *eaten = TRUE;
      }
    }
  } catch (const std::bad_alloc&) {
    return E_OUTOFMEMORY;
  } catch (...) {
    return E_FAIL;
  }
  return S_OK;
}

STDMETHODIMP TextService::OnKeyUp(ITfContext* context, WPARAM wParam, LPARAM lParam, BOOL* eaten) {
  UNREFERENCED_PARAMETER(context);
  UNREFERENCED_PARAMETER(wParam);
  UNREFERENCED_PARAMETER(lParam);
  if (!eaten) return E_INVALIDARG;
  *eaten = FALSE;
  return S_OK;
}

STDMETHODIMP TextService::OnPreservedKey(ITfContext* context, REFGUID rguid, BOOL* eaten) {
  UNREFERENCED_PARAMETER(context);
  UNREFERENCED_PARAMETER(rguid);
  if (!eaten) return E_INVALIDARG;
  *eaten = FALSE;
  return S_OK;
}

STDMETHODIMP TextService::OnInitDocumentMgr(ITfDocumentMgr* pdim) {
  UNREFERENCED_PARAMETER(pdim);
  return S_OK;
}
STDMETHODIMP TextService::OnUninitDocumentMgr(ITfDocumentMgr* pdim) {
  if (ActiveContextBelongsToDocumentMgr(pdim)) {
    CleanupForLifecycleLoss(active_context_, /*release_active_context=*/true,
                            LifecycleCleanupFailurePolicy::ReleaseComposition);
  }
  return S_OK;
}
STDMETHODIMP TextService::OnSetFocus(ITfDocumentMgr* pdimFocus, ITfDocumentMgr* pdimPrevFocus) {
  if (!SameComIdentity(pdimFocus, pdimPrevFocus)) {
    CleanupForLifecycleLoss(active_context_, /*release_active_context=*/true,
                            LifecycleCleanupFailurePolicy::PreserveComposition);
  }
  return S_OK;
}
STDMETHODIMP TextService::OnPushContext(ITfContext* pic) {
  UNREFERENCED_PARAMETER(pic);
  ClearCandidateStateForLifecycle();
  CancelPendingQueriesForLifecycle();
  return S_OK;
}
STDMETHODIMP TextService::OnPopContext(ITfContext* pic) {
  if (pic && active_context_ && SameComIdentity(pic, active_context_)) {
    CleanupForLifecycleLoss(pic, /*release_active_context=*/true,
                            LifecycleCleanupFailurePolicy::PreserveComposition);
  }
  return S_OK;
}

STDMETHODIMP TextService::OnCompositionTerminated(TfEditCookie /*ecWrite*/,
                                                  ITfComposition* pComposition) {
  if (composition_ == pComposition) {
    composition_->Release();
    composition_ = nullptr;
  }
  ClearCandidateStateForLifecycle();
  CancelPendingQueriesForLifecycle();
  const bool preserve_pending_commit = committing_;
  const std::string pending_commit_surface = commit_surface_;
  const auto pending_commit_observation = pending_commit_observation_;
  ITfContext* pending_commit_context = commit_context_;
  if (pending_commit_context) pending_commit_context->AddRef();
  ClearTextStateForLifecycle();
  if (preserve_pending_commit) {
    committing_ = true;
    commit_surface_ = pending_commit_surface;
    pending_commit_observation_ = pending_commit_observation;
    SetCommitContext(pending_commit_context);
  }
  if (pending_commit_context) pending_commit_context->Release();
  return S_OK;
}

STDMETHODIMP TextService::EnumDisplayAttributeInfo(IEnumTfDisplayAttributeInfo** ppEnum) {
  if (!ppEnum) return E_INVALIDARG;
  *ppEnum = nullptr;
  try {
    auto* enumerator = NewComBoundaryObject<azookey::tsf::EnumDisplayAttributeInfo>();
    if (!enumerator) return E_OUTOFMEMORY;
    *ppEnum = enumerator;
    return S_OK;
  } catch (const std::bad_alloc&) {
    return E_OUTOFMEMORY;
  } catch (...) {
    return E_FAIL;
  }
}

STDMETHODIMP TextService::GetDisplayAttributeInfo(REFGUID guidInfo,
                                                  ITfDisplayAttributeInfo** ppInfo) {
  if (!ppInfo) return E_INVALIDARG;
  *ppInfo = nullptr;
  if (IsEqualGUID(guidInfo, kInputAttributeGuid)) {
    try {
      auto* info = NewComBoundaryObject<InputDisplayAttributeInfo>();
      if (!info) return E_OUTOFMEMORY;
      *ppInfo = info;
      return S_OK;
    } catch (const std::bad_alloc&) {
      return E_OUTOFMEMORY;
    } catch (...) {
      return E_FAIL;
    }
  }
  return E_INVALIDARG;
}

HRESULT TextService::RequestPreeditUpdate(ITfContext* context, bool* request_accepted) {
  if (request_accepted) *request_accepted = false;
  if (!context) return E_INVALIDARG;
  ITfEditSession* edit = NewComBoundaryObject<EditSession>(this, context);
  if (!edit) return E_OUTOFMEMORY;
  if (active_context_ != context) {
    if (active_context_) active_context_->Release();
    active_context_ = context;
    active_context_->AddRef();
  }
  HRESULT hr_session = S_OK;
  HRESULT hr = context->RequestEditSession(client_id_, edit, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE,
                                           &hr_session);
  edit->Release();
  if (request_accepted) *request_accepted = SUCCEEDED(hr);
  // For synchronous sessions TSF writes DoEditSession's result into hr_session
  // while returning S_OK from RequestEditSession itself.  Return hr_session so
  // callers see real failures.  For async sessions hr_session stays S_OK
  // (initialized value), so this is always safe to return.
  return SUCCEEDED(hr) ? hr_session : hr;
}

HRESULT TextService::RequestCommitEditSession(ITfContext* context) {
  if (!context) return E_INVALIDARG;
  if (active_context_ != context) {
    if (active_context_) active_context_->Release();
    active_context_ = context;
    active_context_->AddRef();
  }

  ITfEditSession* edit = NewComBoundaryObject<EditSession>(this, context);
  if (!edit) return E_OUTOFMEMORY;
  HRESULT hr_session = E_FAIL;
  const HRESULT hr =
      context->RequestEditSession(client_id_, edit, TF_ES_SYNC | TF_ES_READWRITE, &hr_session);
  edit->Release();
  if (FAILED(hr)) return hr;
  if (FAILED(hr_session)) return hr_session;
  // RequestEditSession can report S_OK even when a sync session was not run.
  // DoEditSession clears this state only after the commit path really finishes.
  const bool commit_completed = !committing_ && commit_surface_.empty();
  if (commit_completed) ClearCommitContext();
  return commit_completed ? S_OK : E_FAIL;
}

void TextService::ClearCandidateStateForLifecycle() {
  candidate_ui_.EndUI();
  selected_candidate_idx_ = 0;
  shown_candidates_.clear();
  caret_pt_ = {0, 0};
  caret_pt_valid_ = false;
  {
    std::lock_guard<std::mutex> lk(candidates_mtx_);
    candidates_.clear();
    candidate_window_show_pending_ = false;
  }
}

void TextService::CancelPendingQueriesForLifecycle() {
  std::lock_guard<std::mutex> lk(ipc_mtx_);
  const bool can_send_cancel = ipc_thread_.joinable() && !ipc_stop_.load();
  bool need_notify = false;
  if (ipc_has_request_) {
    if (can_send_cancel) {
      ipc_send_queue_.push_back(
          {ipc::MessageType::Cancel, ipc::BuildCancel({ipc_pending_id_}), false, ipc_pending_id_});
      need_notify = true;
    }
    ipc_has_request_ = false;
  }
  if (can_send_cancel && ipc_inflight_id_ != 0) {
    ipc_send_queue_.push_back(
        {ipc::MessageType::Cancel, ipc::BuildCancel({ipc_inflight_id_}), false, ipc_inflight_id_});
    need_notify = true;
  }
  ++ipc_pending_id_;
  if (need_notify) ipc_cv_.notify_one();
}

void TextService::SetCommitContext(ITfContext* context) {
  if (commit_context_ == context) return;
  ClearCommitContext();
  commit_context_ = context;
  if (commit_context_) commit_context_->AddRef();
}

void TextService::ClearCommitContext() {
  if (commit_context_) {
    commit_context_->Release();
    commit_context_ = nullptr;
  }
}

void TextService::PostPendingCommitObservation() {
  if (!pending_commit_observation_) return;
  auto pending = std::move(*pending_commit_observation_);
  pending_commit_observation_.reset();
  try {
#ifdef AZOOKEY_TSF_TESTING
    if (testing::ConsumePendingCommitObservationFailureForTest()) {
      throw std::bad_alloc();
    }
#endif
    PostCommitObservation(pending.reading, pending.chosen, pending.shown);
  } catch (...) {
    // The document commit has already succeeded by the time pending commit
    // observations are posted. Drop best-effort learning telemetry rather than
    // reporting commit failure and making the same text retryable.
  }
}

void TextService::ClearTextStateForLifecycle() {
  preedit_kana_.clear();
  romaji_.Reset();
  ClearBatchState();
  committing_ = false;
  commit_surface_.clear();
  pending_commit_observation_.reset();
  ClearCommitContext();
}

bool TextService::ActiveContextBelongsToDocumentMgr(ITfDocumentMgr* document_mgr) const {
  if (!document_mgr || !active_context_) return false;

  ITfDocumentMgr* active_document_mgr = nullptr;
  if (SUCCEEDED(active_context_->GetDocumentMgr(&active_document_mgr)) && active_document_mgr) {
    const bool owns_active_context = SameComIdentity(active_document_mgr, document_mgr);
    active_document_mgr->Release();
    if (owns_active_context) return true;
  }

  ITfContext* context = nullptr;
  if (SUCCEEDED(document_mgr->GetTop(&context)) && context) {
    const bool is_active_context = SameComIdentity(context, active_context_);
    context->Release();
    if (is_active_context) return true;
  }
  context = nullptr;
  if (SUCCEEDED(document_mgr->GetBase(&context)) && context) {
    const bool is_active_context = SameComIdentity(context, active_context_);
    context->Release();
    if (is_active_context) return true;
  }
  return false;
}

bool TextService::RequestLifecycleCommitOrEndComposition(ITfContext* context) {
  std::string pending_surface;
  if (committing_) {
    pending_surface = commit_surface_;
  } else if (!batch_raw_romaji_.empty()) {
    pending_surface = BatchReadingForConversion();
  } else {
    pending_surface = preedit_kana_;
    if (romaji_.HasPending()) {
      auto romaji_preview = romaji_;
      pending_surface += romaji_preview.Flush();
    }
  }
  const bool has_lifecycle_commit_surface = committing_ || !pending_surface.empty();
  if (!composition_ && !has_lifecycle_commit_surface) return true;
  if (!context) return false;

  const std::string saved_preedit = preedit_kana_;
  const bool saved_committing = committing_;
  const std::string saved_commit_surface = commit_surface_;

  const bool commit_without_composition = composition_ == nullptr;
  if (has_lifecycle_commit_surface) {
    committing_ = true;
    commit_surface_ = pending_surface;
    preedit_kana_.clear();
  } else {
    // The empty-preedit edit session ends the TSF composition without replacing
    // its range text, so focus loss/Deactivate auto-commits the current preedit
    // as specified in docs/tsf-deep-integration-spec.md §4.3.
    preedit_kana_.clear();
    committing_ = false;
    commit_surface_.clear();
  }

  ITfEditSession* edit = NewComBoundaryObject<EditSession>(this, context);
  if (!edit) {
    preedit_kana_ = saved_preedit;
    committing_ = saved_committing;
    commit_surface_ = saved_commit_surface;
    RuntimeLog(azookey::logging::RuntimeLogLevel::Error,
               "lifecycle_cleanup_edit_session_allocation_failed");
    return false;
  }
  HRESULT hr_session = E_FAIL;
  const HRESULT hr =
      context->RequestEditSession(client_id_, edit, TF_ES_SYNC | TF_ES_READWRITE, &hr_session);
  edit->Release();

  const bool completed = SUCCEEDED(hr) && SUCCEEDED(hr_session) &&
                         (commit_without_composition ? (!committing_ && commit_surface_.empty())
                                                     : (composition_ == nullptr));
  if (!completed) {
    preedit_kana_ = saved_preedit;
    committing_ = saved_committing;
    commit_surface_ = saved_commit_surface;
    RuntimeLog(azookey::logging::RuntimeLogLevel::Warn,
               "lifecycle_cleanup_edit_session_incomplete");
  }
  return completed;
}

void TextService::CleanupForLifecycleLoss(ITfContext* context, bool release_active_context,
                                          LifecycleCleanupFailurePolicy failure_policy) {
  ClearCandidateStateForLifecycle();
  CancelPendingQueriesForLifecycle();

  ITfContext* cleanup_context = context ? context : active_context_;
  const bool lifecycle_text_committed = RequestLifecycleCommitOrEndComposition(cleanup_context);
  if (lifecycle_text_committed) {
    ClearTextStateForLifecycle();
  } else {
    if (failure_policy == LifecycleCleanupFailurePolicy::PreserveComposition) {
      // Focus/context loss can race with transient TSF lock denial. Keep the
      // active context/composition so a later accepted session or termination
      // callback can finish the lifecycle cleanup.
      return;
    }
    if (composition_) {
      composition_->Release();
      composition_ = nullptr;
    }
    ClearTextStateForLifecycle();
  }

  if (release_active_context &&
      (!cleanup_context || SameComIdentity(cleanup_context, active_context_))) {
    if (active_context_) {
      active_context_->Release();
      active_context_ = nullptr;
    }
  }
}

// --- Commit helpers (M5) ---

HRESULT TextService::CommitSelected(ITfContext* context) {
  if (!context) return S_OK;

  // Use the snapshot taken when Space opened the window so that a late
  // QueryCandidates response cannot silently alter what gets committed.
  ipc::CandidateField chosen;
  std::vector<ipc::CandidateField> shown = shown_candidates_;
  if (!shown_candidates_.empty() && selected_candidate_idx_ >= 0 &&
      selected_candidate_idx_ < static_cast<int>(shown_candidates_.size())) {
    chosen = shown_candidates_[selected_candidate_idx_];
  }
  shown_candidates_.clear();
  {
    std::lock_guard<std::mutex> lk(candidates_mtx_);
    candidates_.clear();
    candidate_window_show_pending_ = false;
  }
  const std::string reading =
      batch_raw_romaji_.empty() ? preedit_kana_ : BatchReadingForConversion();

  candidate_ui_.EndUI();
  selected_candidate_idx_ = 0;

  // M10: cancel queued AND in-flight QC so the host can abort early.
  // Also bump ipc_pending_id_ so any response already en-route fails the
  // staleness check (req_id == ipc_pending_id_) even if no new request is
  // queued after this commit.
  {
    std::lock_guard<std::mutex> lk(ipc_mtx_);
    bool need_notify = false;
    if (ipc_has_request_) {
      ipc_send_queue_.push_back(
          {ipc::MessageType::Cancel, ipc::BuildCancel({ipc_pending_id_}), false, ipc_pending_id_});
      ipc_has_request_ = false;
      need_notify = true;
    }
    if (ipc_inflight_id_ != 0) {
      ipc_send_queue_.push_back({ipc::MessageType::Cancel, ipc::BuildCancel({ipc_inflight_id_}),
                                 false, ipc_inflight_id_});
      need_notify = true;
    }
    ++ipc_pending_id_;
    if (need_notify) ipc_cv_.notify_one();
  }

  commit_surface_ = chosen.surface.empty() ? reading : chosen.surface;
  committing_ = true;
  SetCommitContext(context);
  if (!chosen.surface.empty() && !reading.empty()) {
    pending_commit_observation_ = PendingCommitObservation{reading, chosen, shown};
  } else {
    pending_commit_observation_.reset();
  }
  // Defer clearing preedit and recording learning until the synchronous commit
  // edit session has actually completed.  A request accepted asynchronously is
  // not enough: SetText/EndComposition may still fail and the user input must
  // stay retryable.
  const HRESULT commit_hr = RequestCommitEditSession(context);
  if (SUCCEEDED(commit_hr)) {
    preedit_kana_.clear();
    romaji_.Reset();
    ClearBatchState();
  } else if (!committing_) {
    commit_surface_.clear();
    pending_commit_observation_.reset();
  }
  return commit_hr == E_OUTOFMEMORY ? commit_hr : S_OK;
}

HRESULT TextService::CommitPreeditAsIs(ITfContext* context) {
  if (!context) return S_OK;

  std::string surface;
  if (!batch_raw_romaji_.empty()) {
    surface = BatchReadingForConversion();
  } else {
    // Flush any pending romaji first.
    const std::string flushed = romaji_.Flush();
    preedit_kana_ += flushed;
    surface = preedit_kana_;
  }

  if (surface.empty()) return S_OK;

  candidate_ui_.EndUI();
  selected_candidate_idx_ = 0;
  {
    std::lock_guard<std::mutex> lk(candidates_mtx_);
    candidates_.clear();
    candidate_window_show_pending_ = false;
  }

  // M10: cancel queued AND in-flight QC; bump pending_id to invalidate
  // any response already en-route (mirrors CommitSelected logic).
  {
    std::lock_guard<std::mutex> lk(ipc_mtx_);
    bool need_notify = false;
    if (ipc_has_request_) {
      ipc_send_queue_.push_back(
          {ipc::MessageType::Cancel, ipc::BuildCancel({ipc_pending_id_}), false, ipc_pending_id_});
      ipc_has_request_ = false;
      need_notify = true;
    }
    if (ipc_inflight_id_ != 0) {
      ipc_send_queue_.push_back({ipc::MessageType::Cancel, ipc::BuildCancel({ipc_inflight_id_}),
                                 false, ipc_inflight_id_});
      need_notify = true;
    }
    ++ipc_pending_id_;
    if (need_notify) ipc_cv_.notify_one();
  }

  commit_surface_ = surface;
  committing_ = true;
  SetCommitContext(context);
  pending_commit_observation_.reset();
  // Same deferred-clear pattern as CommitSelected: preserve preedit until the
  // synchronous commit edit session has actually completed.
  const HRESULT commit_hr = RequestCommitEditSession(context);
  if (SUCCEEDED(commit_hr)) {
    preedit_kana_.clear();
    romaji_.Reset();
    ClearBatchState();
  } else if (!committing_) {
    commit_surface_.clear();
  }
  return commit_hr == E_OUTOFMEMORY ? commit_hr : S_OK;
}

// --- IPC worker (M4 + M6 + M10) ---

void TextService::StartIpcWorker() {
  ipc_stop_.store(false);
  ipc_thread_ = std::thread(&TextService::IpcWorkerThread, this);
}

void TextService::StopIpcWorker() {
  if (!ipc_thread_.joinable()) return;
  {
    std::lock_guard<std::mutex> lock(ipc_mtx_);
    ipc_stop_.store(true);
  }
  ipc_cv_.notify_one();
  ipc_client_.Disconnect();
  ipc_thread_.join();
}

std::string TextService::IpcPipeName() const {
#ifdef AZOOKEY_TSF_TESTING
  if (!ipc_pipe_name_for_test_.empty()) return ipc_pipe_name_for_test_;
#endif
  return ipc::DefaultPipeName();
}

// Interruptible backoff between reconnect attempts. Returns true when the
// worker should stop (Deactivate set ipc_stop_); StopIpcWorker notifies
// ipc_cv_ so this wakes promptly instead of sleeping out the full delay.
bool TextService::WaitForReconnectOrStop(uint32_t delay_ms) {
  std::unique_lock<std::mutex> lock(ipc_mtx_);
  ipc_cv_.wait_for(lock, std::chrono::milliseconds(delay_ms), [this] { return ipc_stop_.load(); });
  return ipc_stop_.load();
}

bool TextService::WaitForIpcResponseOrStop(uint32_t timeout_ms, uint64_t expected_request_id,
                                           ipc::MessageType expected_type) {
  using namespace std::chrono;

  constexpr uint32_t kResponsePollMs = 50;
  const auto deadline = steady_clock::now() + milliseconds(timeout_ms);

  while (!ipc_stop_.load() && ipc_client_.IsConnected()) {
    const auto now = steady_clock::now();
    if (now >= deadline) break;

    const auto remaining_ms = duration_cast<milliseconds>(deadline - now).count();
    uint32_t wait_ms = kResponsePollMs;
    if (remaining_ms > 0 && remaining_ms < static_cast<long long>(wait_ms)) {
      wait_ms = static_cast<uint32_t>(remaining_ms);
    }

    auto response = ipc_client_.ReceiveWithTimeout(wait_ms);
    if (response) {
      if (!IsExpectedIpcResponse(*response, expected_request_id, expected_type)) {
        if (expected_request_id != 0 && response->request_id != expected_request_id) {
          RuntimeLog(
              azookey::logging::RuntimeLogLevel::Warn, "ipc_stale_response",
              {{"request_id", response->request_id}, {"expected_request_id", expected_request_id}});
        } else {
          RuntimeLog(azookey::logging::RuntimeLogLevel::Warn, "ipc_unexpected_response_type",
                     {{"request_id", response->request_id},
                      {"response_type", SafeLogText(TypeToString(response->type))},
                      {"expected_type", SafeLogText(TypeToString(expected_type))}});
        }
        continue;
      }
      return true;
    }
  }

  return false;
}

// Send the activation handshake and wait (bounded) for acceptance. The bounded
// wait prevents a host that accepts the pipe but never replies from hanging the
// reconnect loop. Returns false on send failure, timeout, or rejection.
bool TextService::PerformHandshake() {
  constexpr uint32_t kHandshakeTimeoutMs = 3000;
  return PerformHandshake(ipc_client_, kHandshakeTimeoutMs, "tip-activate-handshake",
                          /*update_host_options=*/true);
}

bool TextService::PerformHandshake(ipc::NamedPipeClient& client, uint32_t timeout_ms,
                                   const std::string& trace_id, bool update_host_options) {
  using namespace azookey::ipc;

  HandshakeRequest hs;
  hs.tip_version = kTipVersion;
  hs.protocol_version = 1;
  hs.capabilities = {"ping", "query_candidates", "query_batch_conversion", "commit_observation",
                     "cancel"};
  hs.client_id = ipc_client_id_;
  hs.handshake_token = IpcHandshakeTokenFromEnv();

  Envelope henv;
  henv.version = 1;
  henv.request_id = 1;
  henv.trace_id = trace_id;
  henv.type = MessageType::Handshake;
  henv.payload_json = BuildHandshakeRequest(hs);

  if (!client.Send(henv)) {
    RuntimeLog(azookey::logging::RuntimeLogLevel::Warn, "ipc_handshake_send_failed");
    return false;
  }
  auto hres = client.ReceiveWithTimeout(timeout_ms);
  auto hpayload = hres ? ParseHandshakeResponse(hres->payload_json) : std::nullopt;
  if (!hpayload || !hpayload->accepted) {
    RuntimeLog(azookey::logging::RuntimeLogLevel::Warn, "ipc_handshake_rejected");
    return false;
  }
  if (update_host_options) {
    ObserveHostGeneration(hpayload->host_generation_id);
    batch_romaji_conversion_.store(hpayload->batch_romaji_conversion, std::memory_order_relaxed);
    batch_romaji_preview_romaji_.store(hpayload->batch_romaji_preview_style == "romaji",
                                       std::memory_order_relaxed);
    batch_conversion_ai_cleanup_.store(hpayload->batch_conversion_mode == "ai-cleanup",
                                       std::memory_order_relaxed);
    batch_auto_punctuation_.store(hpayload->batch_auto_punctuation, std::memory_order_relaxed);
    RuntimeLog(azookey::logging::RuntimeLogLevel::Info, "ipc_connected",
               {{"host_version", SafeLogText(hpayload->host_version)},
                {"host_generation_id", SafeLogText(hpayload->host_generation_id)}});
  }
  return true;
}

bool TextService::ObserveHostGeneration(const std::string& host_generation_id) {
  std::string previous_generation_id;
  bool first_observation = false;
  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(ipc_mtx_);
    if (!ipc_has_known_host_generation_) {
      if (host_generation_id.empty()) return false;
      ipc_has_known_host_generation_ = true;
      ipc_host_generation_id_ = host_generation_id;
      first_observation = true;
    } else if (ipc_host_generation_id_ != host_generation_id) {
      previous_generation_id = ipc_host_generation_id_;
      ipc_host_generation_id_ = host_generation_id;
      // A pending reading remains valid for the replacement Host. Give it a
      // new request id so any response from the previous generation is stale,
      // but keep ipc_has_request_ set so the reading is reissued after the
      // handshake completes.
      ++ipc_pending_id_;
      ipc_inflight_id_ = 0;
      changed = true;
    }
  }

  if (first_observation) {
    RuntimeLog(azookey::logging::RuntimeLogLevel::Info, "ipc_host_generation_observed",
               {{"host_generation_id", SafeLogText(host_generation_id)}});
  }
  if (!changed) return false;

  {
    std::lock_guard<std::mutex> lock(candidates_mtx_);
    candidates_.clear();
  }
  RuntimeLog(azookey::logging::RuntimeLogLevel::Warn, "ipc_host_generation_changed",
             {{"previous_host_generation_id", SafeLogText(previous_generation_id)},
              {"host_generation_id", SafeLogText(host_generation_id)}});
  return true;
}

bool TextService::SendCancelOutOfBand(uint64_t target_request_id) {
  return SendCancelOutOfBand(target_request_id, kCancelConnectTimeoutMs, kCancelHandshakeTimeoutMs);
}

bool TextService::SendCancelOutOfBand(uint64_t target_request_id, uint32_t connect_timeout_ms,
                                      uint32_t handshake_timeout_ms) {
  using namespace azookey::ipc;

  const auto pipe_name = IpcPipeName();
  if (pipe_name.empty()) return false;

  NamedPipeClient cancel_client;
  if (!cancel_client.Connect(pipe_name, connect_timeout_ms)) {
    RuntimeLog(azookey::logging::RuntimeLogLevel::Warn, "ipc_cancel_connect_failed",
               {{"target_request_id", target_request_id}});
    return false;
  }
  if (!PerformHandshake(cancel_client, handshake_timeout_ms, "tip-cancel-handshake",
                        /*update_host_options=*/false)) {
    return false;
  }

  Envelope cancel_env;
  cancel_env.version = 1;
  cancel_env.request_id = 2;
  cancel_env.trace_id = "tip-oob-cancel";
  cancel_env.type = MessageType::Cancel;
  cancel_env.payload_json = BuildCancel({target_request_id});
  if (!cancel_client.Send(cancel_env)) {
    RuntimeLog(azookey::logging::RuntimeLogLevel::Warn, "ipc_cancel_send_failed",
               {{"target_request_id", target_request_id}});
    return false;
  }
  return true;
}

// Re-arm a QueryCandidates that was pulled from the queue but not delivered
// because the pipe dropped, so the reconnected serve loop re-issues it without
// the user retyping. Skipped when a newer request or a commit already
// superseded it (ipc_pending_id_ has moved on).
void TextService::RearmPendingQuery(uint64_t req_id) {
  std::lock_guard<std::mutex> lock(ipc_mtx_);
  if (ipc_pending_id_ == req_id) ipc_has_request_ = true;
}

void TextService::IpcWorkerThread() {
  using namespace azookey::ipc;

  const auto pipe_name = IpcPipeName();
  if (pipe_name.empty()) {
    RuntimeLog(azookey::logging::RuntimeLogLevel::Error, "ipc_pipe_name_unavailable");
    return;
  }
  // Reconnect with exponential backoff so the worker survives a host that is
  // started after the TIP, crashes, or restarts. The thread exits only on
  // Deactivate (ipc_stop_), never on a dropped connection (DEV-168).
  constexpr uint32_t kConnectTimeoutMs = 500;
  constexpr uint32_t kBackoffMinMs = 250;
  constexpr uint32_t kBackoffMaxMs = 3000;
  uint32_t backoff_ms = kBackoffMinMs;

  while (!ipc_stop_.load()) {
    const bool established =
        ipc_client_.Connect(pipe_name, kConnectTimeoutMs) && PerformHandshake();
    if (!established) {
      ipc_client_.Disconnect();
      if (WaitForReconnectOrStop(backoff_ms)) break;
      backoff_ms *= 2;
      if (backoff_ms > kBackoffMaxMs) backoff_ms = kBackoffMaxMs;
      continue;
    }

    // Healthy connection: reset backoff and serve until the pipe drops. A
    // QueryCandidates enqueued while the host was down is still pending and is
    // picked up immediately, so candidates recover without the user retyping.
    backoff_ms = kBackoffMinMs;
    ServeConnection();

    ipc_client_.Disconnect();
    {
      std::lock_guard<std::mutex> lock(ipc_mtx_);
      ipc_inflight_id_ = 0;
    }
  }

  RuntimeLog(azookey::logging::RuntimeLogLevel::Info, "ipc_worker_stopped");
}

// Serve QueryCandidates / fire-and-forget traffic over an already-handshaken
// connection. Returns when the pipe drops (so the caller reconnects) or when
// Deactivate sets ipc_stop_. Cancel can use a short-lived control connection so
// an in-flight query on the primary pipe can be interrupted before it replies.
void TextService::ServeConnection() {
  using namespace azookey::ipc;

  uint64_t next_id = 2;

  while (true) {
    std::string reading;
    std::string raw_romaji;
    std::string batch_mode;
    uint64_t req_id = 0;
    bool has_qc = false;
    bool is_batch = false;
    std::vector<IpcSendItem> to_send;

    {
      std::unique_lock<std::mutex> lock(ipc_mtx_);
      ipc_cv_.wait(lock, [this] {
        return ipc_stop_.load() || ipc_has_request_ || !ipc_send_queue_.empty();
      });
      if (ipc_stop_.load()) break;

      to_send = std::move(ipc_send_queue_);
      ipc_send_queue_.clear();

      if (ipc_has_request_) {
        reading = ipc_pending_reading_;
        raw_romaji = ipc_pending_raw_romaji_;
        batch_mode = ipc_pending_batch_mode_;
        req_id = ipc_pending_id_;
        is_batch = ipc_pending_is_batch_;
        ipc_has_request_ = false;
        has_qc = true;
      }
    }

    // Drain fire-and-forget queue (CommitObservation, Cancel) first. A send or
    // response failure here means the pipe dropped (e.g. the host restarted):
    // re-arm any query pulled this iteration and return so the outer loop
    // reconnects, instead of looping back to the wait on a dead connection.
    for (auto& item : to_send) {
      Envelope env;
      env.version = 1;
      env.request_id = next_id++;
      env.trace_id = "tip-faf";
      env.type = item.type;
      env.payload_json = item.payload_json;
      bool sent_out_of_band = false;
      if (item.type == MessageType::Cancel && item.cancel_target_id != 0) {
        sent_out_of_band = SendCancelOutOfBand(item.cancel_target_id);
        if (!sent_out_of_band) {
          RuntimeLog(azookey::logging::RuntimeLogLevel::Warn, "ipc_cancel_fallback",
                     {{"target_request_id", item.cancel_target_id}});
        }
      }
      if (!sent_out_of_band && !ipc_client_.Send(env)) {
        if (!ipc_stop_.load())
          RuntimeLog(azookey::logging::RuntimeLogLevel::Warn, "ipc_queue_send_failed",
                     {{"message_type", SafeLogText(TypeToString(item.type))}});
        if (has_qc) RearmPendingQuery(req_id);
        return;
      }
      if (!sent_out_of_band && item.expects_response) {
        constexpr uint32_t kFafResponseTimeoutMs = 3000;
        if (!WaitForIpcResponseOrStop(kFafResponseTimeoutMs, env.request_id, item.type)) {
          if (ipc_stop_.load()) return;
          if (!ipc_stop_.load())
            RuntimeLog(azookey::logging::RuntimeLogLevel::Warn, "ipc_queue_receive_failed",
                       {{"message_type", SafeLogText(TypeToString(item.type))}});
          if (has_qc) RearmPendingQuery(req_id);
          return;
        }
      }
    }

    if (!has_qc || reading.empty()) continue;

    Envelope qenv;
    qenv.version = 1;
    qenv.request_id = req_id;
    if (is_batch) {
      QueryBatchConversionRequest qreq;
      qreq.reading = reading;
      qreq.raw_romaji = raw_romaji;
      qreq.mode = batch_mode.empty() ? "neural" : batch_mode;
      qreq.auto_punctuation = batch_auto_punctuation_.load(std::memory_order_relaxed);
      qreq.max_candidates = 9;
      qenv.trace_id = "tip-batch-query";
      qenv.type = MessageType::QueryBatchConversion;
      qenv.payload_json = BuildQueryBatchConversionRequest(qreq);
    } else {
      QueryCandidatesRequest qreq;
      qreq.reading = reading;
      qreq.left_context = "";
      qreq.max_candidates = 9;
      qreq.live = true;
      qenv.trace_id = "tip-key-query";
      qenv.type = MessageType::QueryCandidates;
      qenv.payload_json = BuildQueryCandidatesRequest(qreq);
    }

    // Mark the request as in-flight so CommitSelected can cancel it even
    // after ipc_has_request_ has been cleared by this thread.
    {
      std::lock_guard<std::mutex> lock(ipc_mtx_);
      ipc_inflight_id_ = req_id;
    }

    if (!ipc_client_.Send(qenv)) {
      {
        std::lock_guard<std::mutex> lock(ipc_mtx_);
        ipc_inflight_id_ = 0;
      }
      // Pipe died after we cleared ipc_has_request_: re-arm so the reconnected
      // loop re-issues this reading without the user retyping.
      RearmPendingQuery(req_id);
      if (!ipc_stop_.load())
        RuntimeLog(azookey::logging::RuntimeLogLevel::Warn, "ipc_query_send_failed",
                   {{"request_id", req_id}});
      break;
    }

    // If a Cancel targeting our just-sent QC arrives, the host returns no
    // response (Dispatcher::HandleQueryCandidates returns std::nullopt for
    // canceled requests).  Track that so we abandon the receive instead of
    // spinning until the pipe disconnects.
    bool cancel_inflight = false;
    bool cancel_inflight_out_of_band = false;

    // Drain fire-and-forget items (Cancel) that were enqueued while we were
    // preparing/sending this query.  Only items that do NOT expect a response
    // are sent here — response-awaiting items (CommitObservation) stay in the
    // queue for the next loop iteration so their response isn't confused with
    // the pending QueryCandidates response.  Sending cancels here rather than
    // after Receive() means they reach the host pipeline as soon as possible.
    {
      std::vector<IpcSendItem> faf_now;
      {
        std::lock_guard<std::mutex> lock(ipc_mtx_);
        std::vector<IpcSendItem> deferred;
        for (auto& item : ipc_send_queue_) {
          if (!item.expects_response)
            faf_now.push_back(std::move(item));
          else
            deferred.push_back(std::move(item));
        }
        ipc_send_queue_ = std::move(deferred);
      }
      for (auto& item : faf_now) {
        Envelope env;
        env.version = 1;
        env.request_id = next_id++;
        env.trace_id = "tip-faf";
        env.type = item.type;
        env.payload_json = item.payload_json;
        bool sent_out_of_band = false;
        if (item.type == MessageType::Cancel && item.cancel_target_id != 0) {
          sent_out_of_band = SendCancelOutOfBand(item.cancel_target_id);
          if (!sent_out_of_band) {
            RuntimeLog(azookey::logging::RuntimeLogLevel::Warn, "ipc_post_query_cancel_fallback",
                       {{"target_request_id", item.cancel_target_id}});
          }
        }
        if (!sent_out_of_band && !ipc_client_.Send(env))
          RuntimeLog(azookey::logging::RuntimeLogLevel::Warn, "ipc_post_query_cancel_send_failed",
                     {{"message_type", SafeLogText(TypeToString(item.type))}});
        if (item.type == MessageType::Cancel && item.cancel_target_id == req_id) {
          cancel_inflight = true;
          cancel_inflight_out_of_band = sent_out_of_band;
        }
      }
    }

    // Poll in 50ms slices so cancels enqueued while the host processes the
    // query can be forwarded without waiting for the full response. Normal
    // live QueryCandidates has a request-level deadline; batch conversion can
    // legitimately take longer and keeps the existing unbounded wait.
    const auto query_deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kQueryCandidatesFastTimeoutMs);
    bool query_deadline_exceeded = false;
    std::optional<ipc::Envelope> qres;
    while (!cancel_inflight_out_of_band && !ipc_stop_.load() && ipc_client_.IsConnected()) {
      uint32_t wait_ms = 50;
      if (!is_batch) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= query_deadline) {
          query_deadline_exceeded = true;
          break;
        }
        const auto remaining_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(query_deadline - now).count();
        if (remaining_ms > 0 && remaining_ms < static_cast<long long>(wait_ms)) {
          wait_ms = static_cast<uint32_t>(remaining_ms);
        }
        if (wait_ms == 0) wait_ms = 1;
      }

      qres = ipc_client_.ReceiveWithTimeout(wait_ms);
      if (qres) {
        if (!IsExpectedIpcResponse(*qres, req_id, qenv.type)) {
          if (qres->request_id != req_id) {
            RuntimeLog(azookey::logging::RuntimeLogLevel::Warn, "ipc_stale_response",
                       {{"request_id", qres->request_id}, {"expected_request_id", req_id}});
          } else {
            RuntimeLog(azookey::logging::RuntimeLogLevel::Warn, "ipc_unexpected_response_type",
                       {{"request_id", qres->request_id},
                        {"response_type", SafeLogText(TypeToString(qres->type))},
                        {"expected_type", SafeLogText(TypeToString(qenv.type))}});
          }
          qres.reset();
          continue;
        }
        break;
      }
      // Drain fire-and-forget items (Cancel) that arrived while we waited.
      std::vector<IpcSendItem> mid_faf;
      {
        std::lock_guard<std::mutex> lk(ipc_mtx_);
        std::vector<IpcSendItem> deferred;
        for (auto& item : ipc_send_queue_) {
          if (!item.expects_response)
            mid_faf.push_back(std::move(item));
          else
            deferred.push_back(std::move(item));
        }
        ipc_send_queue_ = std::move(deferred);
      }
      for (auto& item : mid_faf) {
        ipc::Envelope env;
        env.version = 1;
        env.request_id = next_id++;
        env.trace_id = "tip-faf-mid";
        env.type = item.type;
        env.payload_json = item.payload_json;
        bool sent_out_of_band = false;
        if (item.type == ipc::MessageType::Cancel && item.cancel_target_id != 0) {
          sent_out_of_band = SendCancelOutOfBand(item.cancel_target_id);
          if (!sent_out_of_band) {
            RuntimeLog(azookey::logging::RuntimeLogLevel::Warn, "ipc_mid_receive_cancel_fallback",
                       {{"target_request_id", item.cancel_target_id}});
          }
        }
        if (!sent_out_of_band && !ipc_client_.Send(env))
          RuntimeLog(azookey::logging::RuntimeLogLevel::Warn, "ipc_mid_receive_cancel_send_failed",
                     {{"message_type", SafeLogText(ipc::TypeToString(item.type))}});
        if (item.type == ipc::MessageType::Cancel && item.cancel_target_id == req_id) {
          cancel_inflight = true;
          cancel_inflight_out_of_band = sent_out_of_band;
        }
      }
      if (cancel_inflight_out_of_band) break;
    }
    {
      std::lock_guard<std::mutex> lock(ipc_mtx_);
      ipc_inflight_id_ = 0;
    }
    std::vector<CandidateField> response_candidates;
    if (cancel_inflight && cancel_inflight_out_of_band && !qres) {
      RuntimeLog(azookey::logging::RuntimeLogLevel::Info, "ipc_query_cancelled_out_of_band",
                 {{"request_id", req_id}, {"result", SafeLogText("cancelled")}});
      ++next_id;
      continue;
    }
    if (!qres && query_deadline_exceeded && !is_batch) {
      ipc::Envelope cancel_env;
      cancel_env.version = 1;
      cancel_env.request_id = next_id++;
      cancel_env.trace_id = "tip-query-timeout-cancel";
      cancel_env.type = ipc::MessageType::Cancel;
      cancel_env.payload_json = ipc::BuildCancel({req_id});
      const bool sent_out_of_band = SendCancelOutOfBand(req_id, kTimeoutCancelConnectTimeoutMs,
                                                        kTimeoutCancelHandshakeTimeoutMs);
      if (!sent_out_of_band && !ipc_client_.Send(cancel_env)) {
        RearmPendingQuery(req_id);
        if (!ipc_stop_.load())
          RuntimeLog(azookey::logging::RuntimeLogLevel::Warn,
                     "ipc_query_timeout_cancel_send_failed", {{"request_id", req_id}});
        break;
      } else if (!sent_out_of_band) {
        RuntimeLog(azookey::logging::RuntimeLogLevel::Warn, "ipc_query_timeout_cancel_fallback",
                   {{"request_id", req_id}});
      }

      CandidateField fallback;
      fallback.surface = reading;
      fallback.reading = reading;
      fallback.source = "fallback";
      response_candidates.push_back(std::move(fallback));
      RuntimeLog(azookey::logging::RuntimeLogLevel::Warn, "ipc_query_timeout",
                 {{"request_id", req_id}, {"result", SafeLogText("cancelled")}});
    } else if (!qres) {
      // Host died after the query was sent: re-arm so the reconnected loop
      // re-issues it (recover without retyping).
      RearmPendingQuery(req_id);
      if (!ipc_stop_.load())
        RuntimeLog(azookey::logging::RuntimeLogLevel::Warn, "ipc_query_receive_failed",
                   {{"request_id", req_id}});
      break;
    }

    if (cancel_inflight) {
      // If cancel fell back to the main pipe, the host can still return this
      // query response before seeing Cancel. Consume and discard here to keep
      // stream framing aligned for subsequent receives.
      RuntimeLog(azookey::logging::RuntimeLogLevel::Info, "ipc_cancelled_reply_discarded",
                 {{"request_id", req_id}, {"result", SafeLogText("cancelled")}});
      ++next_id;
      continue;
    }

    if (qres && is_batch) {
      auto qpayload = ParseQueryBatchConversionResponse(qres->payload_json);
      if (!qpayload || qpayload->canceled) {
        ++next_id;
        continue;
      }
      if (!qpayload->segments.empty()) {
        response_candidates = qpayload->segments.front().candidates;
      }
      if (response_candidates.empty() && !qpayload->full_surface.empty()) {
        CandidateField fallback;
        fallback.surface = qpayload->full_surface;
        fallback.reading = reading;
        fallback.source = "model";
        response_candidates.push_back(std::move(fallback));
      }
      if (response_candidates.empty()) {
        CandidateField fallback;
        fallback.surface = reading;
        fallback.reading = reading;
        fallback.source = "fallback";
        response_candidates.push_back(std::move(fallback));
      }
    } else if (qres) {
      auto qpayload = ParseQueryCandidatesResponse(qres->payload_json);
      if (!qpayload) {
        ++next_id;
        continue;
      }
      response_candidates = std::move(qpayload->candidates);
    }

    // M10: discard stale responses.
    // Conditions for freshness:
    //   1. No newer QC is already queued (ipc_has_request_ false).
    //   2. ipc_pending_id_ still equals req_id — no commit/clear bumped it
    //      while this response was in-transit.
    bool is_fresh = false;
    {
      std::lock_guard<std::mutex> lock(ipc_mtx_);
      is_fresh = !ipc_has_request_ && (req_id == ipc_pending_id_);
    }

    if (is_fresh) {
      if (!response_candidates.empty()) {
        RuntimeLog(azookey::logging::RuntimeLogLevel::Info, "ipc_candidates_received",
                   {{"request_id", req_id},
                    {"reading_length", static_cast<uint64_t>(reading.size())},
                    {"candidate_count", static_cast<uint64_t>(response_candidates.size())},
                    {"result", SafeLogText("ok")}});
      }
      bool notify_ui = false;
      {
        std::lock_guard<std::mutex> lock(candidates_mtx_);
        candidates_ = std::move(response_candidates);
        if (candidate_window_show_pending_) {
          if (candidates_.empty()) {
            candidate_window_show_pending_ = false;
          } else {
            notify_ui = true;
          }
        }
      }
      if (notify_ui) candidate_ui_.PostCandidatesReady();
    } else {
      RuntimeLog(azookey::logging::RuntimeLogLevel::Warn, "ipc_stale_query_result",
                 {{"request_id", req_id}});
    }

    ++next_id;
  }
}

void TextService::PostQueryCandidates(const std::string& reading) {
  std::lock_guard<std::mutex> lock(ipc_mtx_);
  ipc_pending_reading_ = reading;
  ipc_pending_raw_romaji_.clear();
  ipc_pending_batch_mode_.clear();
  ipc_pending_is_batch_ = false;
  ++ipc_pending_id_;
  ipc_has_request_ = true;
  ipc_cv_.notify_one();
}

void TextService::PostBatchConversion(const std::string& reading, const std::string& raw_romaji) {
  std::lock_guard<std::mutex> lock(ipc_mtx_);
  ipc_pending_reading_ = reading;
  ipc_pending_raw_romaji_ = raw_romaji;
  ipc_pending_batch_mode_ =
      batch_conversion_ai_cleanup_.load(std::memory_order_relaxed) ? "ai-cleanup" : "neural";
  ipc_pending_is_batch_ = true;
  ++ipc_pending_id_;
  ipc_has_request_ = true;
  ipc_cv_.notify_one();
}

std::string TextService::CurrentPreeditSurface() const {
  return preedit_kana_ + romaji_.PreviewPending();
}

std::string TextService::CurrentDisplayedPreeditSurface() const {
  if (candidate_ui_.IsShowing() && selected_candidate_idx_ >= 0 &&
      selected_candidate_idx_ < static_cast<int>(shown_candidates_.size())) {
    return shown_candidates_[static_cast<size_t>(selected_candidate_idx_)].surface;
  }
  return CurrentPreeditSurface();
}

void TextService::OnCandidatesReady(void* context) {
  if (!context) return;
  static_cast<TextService*>(context)->ShowCandidateWindowFromCache();
}

POINT TextService::CandidateAnchorPoint() {
  if (caret_pt_valid_) return caret_pt_;
  const CaretAnchor anchor = ResolveCaretAnchor(nullptr);
  caret_pt_ = anchor.point;
  caret_pt_valid_ = anchor.valid;
  return caret_pt_;
}

void TextService::ShowCandidateWindowFromCache() {
  if (CurrentPreeditSurface().empty() || candidate_ui_.IsShowing()) {
    std::lock_guard<std::mutex> lk(candidates_mtx_);
    candidate_window_show_pending_ = false;
    return;
  }

  std::vector<ipc::CandidateField> snapshot;
  {
    std::lock_guard<std::mutex> lk(candidates_mtx_);
    if (!candidate_window_show_pending_) return;
    candidate_window_show_pending_ = false;
    if (candidates_.empty()) return;
    shown_candidates_ = candidates_;
    snapshot = shown_candidates_;
  }

  std::vector<std::wstring> items;
  for (const auto& candidate : snapshot) items.push_back(Utf8ToWide(candidate.surface));
  if (items.empty()) return;

  batch_query_in_progress_ = false;
  selected_candidate_idx_ = 0;
  const POINT pt = CandidateAnchorPoint();
  const HRESULT begin_hr = candidate_ui_.BeginUI(thread_mgr_, pt, items, 0);
  if (FAILED(begin_hr)) {
    RuntimeLog(azookey::logging::RuntimeLogLevel::Warn, "candidate_ui_begin_failed");
    return;
  }

  const HRESULT update_hr = active_context_ ? RequestPreeditUpdate(active_context_) : E_UNEXPECTED;
  if (FAILED(update_hr)) {
    candidate_ui_.EndUI();
    RuntimeLog(azookey::logging::RuntimeLogLevel::Warn, "candidate_initial_preedit_update_failed");
  }
}

#ifdef AZOOKEY_TSF_TESTING
bool TextService::candidate_window_show_pending_for_test() {
  std::lock_guard<std::mutex> lk(candidates_mtx_);
  return candidate_window_show_pending_;
}

void TextService::set_cached_candidates_for_test(std::vector<ipc::CandidateField> candidates) {
  std::lock_guard<std::mutex> lk(candidates_mtx_);
  candidates_ = std::move(candidates);
}

std::vector<ipc::CandidateField> TextService::cached_candidates_for_test() {
  std::lock_guard<std::mutex> lk(candidates_mtx_);
  return candidates_;
}

void TextService::show_candidate_window_from_cache_for_test() { ShowCandidateWindowFromCache(); }

std::optional<ipc::CommitObservationRequest>
TextService::last_queued_commit_observation_for_test() {
  std::lock_guard<std::mutex> lock(ipc_mtx_);
  for (auto it = ipc_send_queue_.rbegin(); it != ipc_send_queue_.rend(); ++it) {
    if (it->type == ipc::MessageType::CommitObservation) {
      return ipc::ParseCommitObservationRequest(it->payload_json);
    }
  }
  return std::nullopt;
}

void TextService::set_batch_romaji_options_for_test(bool enabled, bool preview_romaji,
                                                    bool auto_punctuation) {
  batch_romaji_conversion_.store(enabled);
  batch_romaji_preview_romaji_.store(preview_romaji);
  batch_conversion_ai_cleanup_.store(false);
  batch_auto_punctuation_.store(auto_punctuation);
}

bool TextService::batch_query_in_progress_for_test() const { return batch_query_in_progress_; }

bool TextService::has_pending_ipc_query_for_test() {
  std::lock_guard<std::mutex> lock(ipc_mtx_);
  return ipc_has_request_;
}

bool TextService::pending_ipc_query_is_batch_for_test() {
  std::lock_guard<std::mutex> lock(ipc_mtx_);
  return ipc_pending_is_batch_;
}

uint64_t TextService::pending_ipc_request_id_for_test() {
  std::lock_guard<std::mutex> lock(ipc_mtx_);
  return ipc_pending_id_;
}

std::string TextService::pending_ipc_reading_for_test() {
  std::lock_guard<std::mutex> lock(ipc_mtx_);
  return ipc_pending_reading_;
}

std::string TextService::pending_ipc_raw_romaji_for_test() {
  std::lock_guard<std::mutex> lock(ipc_mtx_);
  return ipc_pending_raw_romaji_;
}

void TextService::set_ipc_pipe_name_for_test(std::string pipe_name) {
  ipc_pipe_name_for_test_ = std::move(pipe_name);
}

void TextService::start_ipc_worker_for_test() { StartIpcWorker(); }

void TextService::stop_ipc_worker_for_test() { StopIpcWorker(); }
#endif

bool TextService::BatchRomajiEnabled() const {
  return batch_romaji_conversion_.load(std::memory_order_relaxed);
}

std::string TextService::BatchPreviewSurface() const {
  if (batch_romaji_preview_romaji_.load(std::memory_order_relaxed)) {
    return ApplyDefaultCompositionPunctuation(batch_raw_romaji_);
  }
  return ApplyDefaultCompositionPunctuation(core::RomajiKanaConverter::Preview(batch_raw_romaji_));
}

std::string TextService::BatchReadingForConversion() const {
  return ApplyDefaultCompositionPunctuation(
      core::RomajiKanaConverter::ConvertForCommit(batch_raw_romaji_));
}

void TextService::RefreshBatchPreeditSurface() { preedit_kana_ = BatchPreviewSurface(); }

void TextService::ClearBatchState() {
  batch_raw_romaji_.clear();
  batch_query_in_progress_ = false;
}

void TextService::PostCommitObservation(const std::string& reading,
                                        const ipc::CandidateField& chosen,
                                        const std::vector<ipc::CandidateField>& shown) {
  using namespace std::chrono;
  const uint64_t now_ms = static_cast<uint64_t>(
      duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());

  ipc::CommitObservationRequest req;
  req.reading = reading;
  req.chosen = chosen;
  req.shown = shown;
  req.left_context = "";
  req.timestamp_ms = now_ms;

  PostIpcSend(ipc::MessageType::CommitObservation, ipc::BuildCommitObservationRequest(req), true);
}

void TextService::PostCancel(uint64_t target_request_id) {
  std::lock_guard<std::mutex> lock(ipc_mtx_);
  ipc_send_queue_.push_back(
      {ipc::MessageType::Cancel, ipc::BuildCancel({target_request_id}), false, target_request_id});
  ipc_cv_.notify_one();
}

void TextService::PostIpcSend(ipc::MessageType type, std::string payload, bool expects_response) {
  std::lock_guard<std::mutex> lock(ipc_mtx_);
  ipc_send_queue_.push_back({type, std::move(payload), expects_response, 0});
  ipc_cv_.notify_one();
}

// --- EditSession ---

EditSession::EditSession(TextService* service, ITfContext* context)
    : service_(service), context_(context) {
  if (context_) context_->AddRef();
}

STDMETHODIMP EditSession::QueryInterface(REFIID riid, void** ppvObj) {
  if (!ppvObj) return E_POINTER;
  *ppvObj = nullptr;
  if (riid == IID_IUnknown || riid == IID_ITfEditSession) {
    *ppvObj = static_cast<ITfEditSession*>(this);
    AddRef();
    return S_OK;
  }
  return E_NOINTERFACE;
}
STDMETHODIMP_(ULONG) EditSession::AddRef() {
  return static_cast<ULONG>(InterlockedIncrement(&ref_count_));
}
STDMETHODIMP_(ULONG) EditSession::Release() {
  const auto c = static_cast<ULONG>(InterlockedDecrement(&ref_count_));
  if (c == 0) {
    if (context_) context_->Release();
    delete this;
  }
  return c;
}

STDMETHODIMP EditSession::DoEditSession(TfEditCookie ec) {
  try {
#ifdef AZOOKEY_TSF_TESTING
    if (testing::ConsumeComBoundaryAllocationFailureForTest()) {
      throw std::bad_alloc();
    }
#endif
    // M5/M6: commit path — set final surface text and end composition.
    if (service_->committing_) {
      const std::string pending_commit_surface = service_->commit_surface_;
      const auto pending_commit_observation = service_->pending_commit_observation_;
      const std::wstring surface = Utf8ToWide(pending_commit_surface);
      service_->committing_ = false;
      service_->commit_surface_.clear();
      const auto restore_pending_commit = [&]() {
        service_->committing_ = true;
        service_->commit_surface_ = pending_commit_surface;
        service_->pending_commit_observation_ = pending_commit_observation;
      };
      const auto post_pending_commit_observation = [&]() {
        if (!pending_commit_observation) {
          service_->pending_commit_observation_.reset();
          return;
        }
        service_->pending_commit_observation_ = pending_commit_observation;
        service_->PostPendingCommitObservation();
      };

      if (service_->composition_) {
        ITfRange* committed_range = nullptr;
        if (!surface.empty()) {
          HRESULT text_hr = service_->composition_->GetRange(&committed_range);
          if (SUCCEEDED(text_hr) && committed_range) {
            text_hr =
                committed_range->SetText(ec, 0, surface.c_str(), static_cast<LONG>(surface.size()));
          }
          if (FAILED(text_hr) || !committed_range) {
            if (committed_range) committed_range->Release();
            restore_pending_commit();
            return FAILED(text_hr) ? text_hr : E_FAIL;
          }
        }
        // EndComposition finalizes the text in the document.
        ITfComposition* comp = service_->composition_;
        comp->AddRef();
        const HRESULT end_hr = comp->EndComposition(ec);
        if (SUCCEEDED(end_hr) && service_->composition_ == comp) {
          service_->composition_ = nullptr;
          comp->Release();
        }
        comp->Release();
        if (FAILED(end_hr)) {
          if (committed_range) committed_range->Release();
          restore_pending_commit();
          return end_hr;
        }
        if (committed_range) {
          TF_SELECTION sel{};
          sel.range = committed_range;
          if (SUCCEEDED(committed_range->Collapse(ec, TF_ANCHOR_END))) {
            context_->SetSelection(ec, 1, &sel);
          }
          committed_range->Release();
        }
      } else if (!surface.empty()) {
        // No active composition (e.g. commit triggered before the async preedit
        // session ran); insert the surface text directly at the cursor.
        TF_SELECTION sel{};
        ULONG fetched = 0;
        HRESULT text_hr = E_FAIL;
        if (SUCCEEDED(context_->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel, &fetched)) &&
            fetched > 0 && sel.range) {
          text_hr = sel.range->SetText(ec, 0, surface.c_str(), static_cast<LONG>(surface.size()));
          if (SUCCEEDED(text_hr) && SUCCEEDED(sel.range->Collapse(ec, TF_ANCHOR_END))) {
            context_->SetSelection(ec, 1, &sel);
          }
          sel.range->Release();
        }
        if (FAILED(text_hr)) {
          restore_pending_commit();
          return text_hr;
        }
      }
      post_pending_commit_observation();
      return S_OK;
    }

    // Normal preedit update.
    const std::wstring kana = Utf8ToWide(service_->CurrentDisplayedPreeditSurface());

    if (kana.empty()) {
      if (service_->composition_) {
        ITfRange* cleared_range = nullptr;
        HRESULT clear_hr = service_->composition_->GetRange(&cleared_range);
        if (SUCCEEDED(clear_hr) && cleared_range) {
          clear_hr = cleared_range->SetText(ec, 0, L"", 0);
        }
        const bool had_range = cleared_range != nullptr;
        if (cleared_range) cleared_range->Release();
        if (FAILED(clear_hr) || !had_range) {
          return FAILED(clear_hr) ? clear_hr : E_FAIL;
        }

        ITfComposition* comp = service_->composition_;
        comp->AddRef();
        const HRESULT end_hr = comp->EndComposition(ec);
        if (SUCCEEDED(end_hr) && service_->composition_ == comp) {
          service_->composition_ = nullptr;
          comp->Release();
        }
        comp->Release();
        if (FAILED(end_hr)) return end_hr;
      }
      return S_OK;
    }

    // Create composition if not active.
    if (!service_->composition_) {
      ITfContextComposition* pCtxComp = nullptr;
      if (FAILED(context_->QueryInterface(IID_ITfContextComposition,
                                          reinterpret_cast<void**>(&pCtxComp))) ||
          !pCtxComp)
        return E_FAIL;

      TF_SELECTION sel{};
      ULONG fetched = 0;
      context_->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel, &fetched);
      if (fetched == 0) {
        pCtxComp->Release();
        return E_FAIL;
      }

      sel.range->Collapse(ec, TF_ANCHOR_END);
      HRESULT hr = pCtxComp->StartComposition(ec, sel.range, service_, &service_->composition_);
      sel.range->Release();
      pCtxComp->Release();
      if (FAILED(hr) || !service_->composition_) return hr;
    }

    // Update composition text.
    ITfRange* pRange = nullptr;
    if (FAILED(service_->composition_->GetRange(&pRange)) || !pRange) return E_FAIL;

    const HRESULT set_text_hr =
        pRange->SetText(ec, 0, kana.c_str(), static_cast<LONG>(kana.size()));
    if (FAILED(set_text_hr)) {
      pRange->Release();
      return set_text_hr;
    }

    // Cache the caret screen position for the candidate window anchor (M5/M19).
    {
      const RECT* text_ext_rect = nullptr;
      HWND text_extent_window = nullptr;
      RECT rc{};
      ITfContextView* pView = nullptr;
      if (SUCCEEDED(context_->GetActiveView(&pView)) && pView) {
        BOOL clipped = FALSE;
        if (pView->GetTextExt(ec, pRange, &rc, &clipped) == S_OK) {
          text_ext_rect = &rc;
          HWND view_window = nullptr;
          if (SUCCEEDED(pView->GetWnd(&view_window))) {
            text_extent_window = view_window;
          }
        }
      }
      if (pView) pView->Release();
      const CaretAnchor anchor = ResolveCaretAnchor(text_ext_rect, text_extent_window);
      service_->caret_pt_ = anchor.point;
      service_->caret_pt_valid_ = anchor.valid;
    }

    // Apply underline display attribute via GUID_PROP_ATTRIBUTE.
    ITfProperty* pProp = nullptr;
    if (SUCCEEDED(context_->GetProperty(GUID_PROP_ATTRIBUTE, &pProp)) && pProp) {
      ITfCategoryMgr* pCatMgr = nullptr;
      if (SUCCEEDED(CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                                     IID_ITfCategoryMgr, reinterpret_cast<void**>(&pCatMgr))) &&
          pCatMgr) {
        TfGuidAtom atom = TF_INVALID_GUIDATOM;
        pCatMgr->RegisterGUID(kInputAttributeGuid, &atom);
        if (atom != TF_INVALID_GUIDATOM) {
          VARIANT var;
          var.vt = VT_I4;
          var.lVal = static_cast<LONG>(atom);
          const HRESULT attr_hr = pProp->SetValue(ec, pRange, &var);
          if (FAILED(attr_hr)) {
            RuntimeLog(azookey::logging::RuntimeLogLevel::Warn,
                       "preedit_display_attribute_set_failed");
          }
        }
        pCatMgr->Release();
      }
      pProp->Release();
    }

    // Keep the document caret at the end of the preedit without collapsing the
    // range used for the full-composition display attribute above.
    ITfRange* selection_range = nullptr;
    HRESULT selection_hr = pRange->Clone(&selection_range);
    if (SUCCEEDED(selection_hr) && !selection_range) selection_hr = E_UNEXPECTED;
    if (SUCCEEDED(selection_hr)) {
      selection_hr = selection_range->Collapse(ec, TF_ANCHOR_END);
      if (SUCCEEDED(selection_hr)) {
        TF_SELECTION selection{};
        selection.range = selection_range;
        selection.style.ase = TF_AE_NONE;
        selection.style.fInterimChar = FALSE;
        selection_hr = context_->SetSelection(ec, 1, &selection);
      }
      selection_range->Release();
    }
    if (FAILED(selection_hr)) {
      RuntimeLog(azookey::logging::RuntimeLogLevel::Warn, "preedit_selection_update_failed");
    }

    pRange->Release();
    return S_OK;
  } catch (const std::bad_alloc&) {
    return E_OUTOFMEMORY;
  } catch (...) {
    return E_FAIL;
  }
}

}  // namespace azookey::tsf
