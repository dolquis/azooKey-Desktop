#include "azookey/tsf/CandidateWindow.h"

#include <ShellScalingApi.h>
#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <d3d11.h>
#include <dwrite.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>

#include "CandidateSelection.h"
#include "azookey/tsf/CaretRectResolver.h"
#include "azookey/tsf/DpiScaling.h"
#include "azookey/tsf/RenderingEngine.h"

namespace azookey::tsf {

using Microsoft::WRL::ComPtr;

// Text formats from the system message font at the window's DPI.
struct CandidateWindow::TextStyle {
  ComPtr<IDWriteFactory> factory;
  ComPtr<IDWriteTextFormat> line;      // One line, left aligned, cut with an ellipsis.
  ComPtr<IDWriteTextFormat> centered;  // Buttons and the lock.
  ComPtr<IDWriteTextFormat> wrapped;   // Banner and details text, from the top.
};

struct CandidateWindow::RenderState {
  RenderingEngine engine;
};

namespace {

constexpr wchar_t kClassName[] = L"azooKeyCandidateWnd";
constexpr wchar_t kDetailsClassName[] = L"azooKeyCandidateHealthDetailsWnd";
constexpr wchar_t kSecureIndicatorText[] = L"🔒";
constexpr wchar_t kSecureToastText[] = L"🔒 セーフ入力中: 学習・AI・予測は停止しています";

const wchar_t* HealthBannerText(CandidateHealthState state) {
  switch (state) {
    case CandidateHealthState::DegradedSimple:
      return L"⚠️ 変換エンジンに接続できないため、かな・カタカナ候補で継続しています";
    case CandidateHealthState::DegradedModel:
      return L"⚠️ Zenzai を使えないため、簡易変換で継続しています";
    case CandidateHealthState::SafeMode:
      return L"⚠️ 異常終了が続いたため、安全モードで動作しています（AI と学習を停止中）";
    case CandidateHealthState::Healthy:
      return L"";
  }
  return L"";
}

const wchar_t* HealthDetailsText(CandidateHealthState state) {
  switch (state) {
    case CandidateHealthState::DegradedSimple:
      return L"変換エンジンとの接続を確認してください。詳しく調べるには、コマンドプロンプトで "
             L"azookey_diag.exe --json を実行してください。診断 ZIP は azookey_diag.exe --collect "
             L"--output azookey-diag.zip で作成できます。";
    case CandidateHealthState::DegradedModel:
      return L"Zenzai "
             L"モデルを利用できません。モデル設定を確認し、再試行してください。詳しく調べるには、コ"
             L"マンドプロンプトで azookey_diag.exe --json を実行してください。";
    case CandidateHealthState::SafeMode:
      return L"異常終了の原因（直前に変更したモデルや backend "
             L"など）を取り除き、%LOCALAPPDATA%\\azooKey\\config\\settings.json の "
             L"safeMode.enabled を false "
             L"にしてください。その後サインアウトして入り直すか、設定アプリで任意の設定を保存して "
             L"UpdateConfig を送ってください。詳しくは azookey_diag.exe --json で確認できます。";
    case CandidateHealthState::Healthy:
      return L"";
  }
  return L"";
}

HMODULE GetTipModuleHandle() {
  HMODULE module = nullptr;
  if (GetModuleHandleExW(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCWSTR>(&GetTipModuleHandle), &module)) {
    return module;
  }
  return nullptr;
}

#ifdef AZOOKEY_TSF_TESTING
UINT g_monitor_dpi_for_test = 0;
#endif

// The GPU was reset or removed; the whole device stack has to be rebuilt.
bool IsDeviceLost(HRESULT hr) {
  return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET ||
         hr == D2DERR_RECREATE_TARGET;
}

D2D1_COLOR_F ToColorF(COLORREF color) {
  return D2D1::ColorF(GetRValue(color) / 255.0f, GetGValue(color) / 255.0f,
                      GetBValue(color) / 255.0f, 1.0f);
}

D2D1_RECT_F ToRectF(const RECT& rect) {
  return D2D1::RectF(static_cast<FLOAT>(rect.left), static_cast<FLOAT>(rect.top),
                     static_cast<FLOAT>(rect.right), static_cast<FLOAT>(rect.bottom));
}

HRESULT CreateFormat(IDWriteFactory* factory, UINT dpi, DWRITE_WORD_WRAPPING wrapping,
                     DWRITE_TEXT_ALIGNMENT text_alignment,
                     DWRITE_PARAGRAPH_ALIGNMENT paragraph_alignment, bool ellipsis,
                     ComPtr<IDWriteTextFormat>& format) {
  HRESULT hr = CreateMessageTextFormat(factory, dpi, &format);
  if (FAILED(hr) || FAILED(hr = format->SetWordWrapping(wrapping)) ||
      FAILED(hr = format->SetTextAlignment(text_alignment)) ||
      FAILED(hr = format->SetParagraphAlignment(paragraph_alignment)))
    return hr;
  if (!ellipsis) return S_OK;
  ComPtr<IDWriteInlineObject> sign;
  if (FAILED(hr = factory->CreateEllipsisTrimmingSign(format.Get(), &sign))) return hr;
  const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
  return format->SetTrimming(&trimming, sign.Get());
}

// One drawing pass: fills, a frame, and text with color fonts (native-ui-spec §3.4).
class Painter {
 public:
  Painter(ID2D1DeviceContext* context, IDWriteFactory* factory)
      : context_(context), factory_(factory) {
    context_->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f), &brush_);
  }

  bool ok() const { return brush_ != nullptr; }

  void Fill(const RECT& rect, COLORREF color) {
    brush_->SetColor(ToColorF(color));
    context_->FillRectangle(ToRectF(rect), brush_.Get());
  }

  // A one-pixel border along the inside of the client area.
  void Frame(int width, int height, COLORREF color) {
    brush_->SetColor(ToColorF(color));
    context_->DrawRectangle(D2D1::RectF(0.5f, 0.5f, static_cast<FLOAT>(width) - 0.5f,
                                        static_cast<FLOAT>(height) - 0.5f),
                            brush_.Get());
  }

  void Text(const std::wstring& text, IDWriteTextFormat* format, const RECT& rect, COLORREF color) {
    if (text.empty() || rect.right <= rect.left || rect.bottom <= rect.top) return;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(factory_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format,
                                          static_cast<FLOAT>(rect.right - rect.left),
                                          static_cast<FLOAT>(rect.bottom - rect.top), &layout)))
      return;
    brush_->SetColor(ToColorF(color));
    context_->DrawTextLayout(
        D2D1::Point2F(static_cast<FLOAT>(rect.left), static_cast<FLOAT>(rect.top)), layout.Get(),
        brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT | D2D1_DRAW_TEXT_OPTIONS_CLIP);
  }

 private:
  ID2D1DeviceContext* context_;
  IDWriteFactory* factory_;
  ComPtr<ID2D1SolidColorBrush> brush_;
};

}  // namespace

CandidateWindow::CandidateWindow() = default;

CandidateWindow::~CandidateWindow() { Destroy(); }

// static
int CandidateWindow::ScaleForDpi(int value, UINT dpi) { return tsf::ScaleForDpi(value, dpi); }

// static
CandidateWindow::LayoutMetrics CandidateWindow::ComputeLayoutMetrics(UINT dpi) {
  return {
      std::max(1, ScaleForDpi(kBaseItemHeight, dpi)),
      std::max(1, ScaleForDpi(kBaseHorzPad, dpi)),
      std::max(1, ScaleForDpi(kBaseMaxWidth, dpi)),
      std::max(1, ScaleForDpi(kBaseCaretGap, dpi)),
      std::max(1, ScaleForDpi(kBaseMinTextWidth, dpi)),
      std::max(1, ScaleForDpi(kBaseExtraWidth, dpi)),
  };
}

// static
CandidateWindow::ColumnLayout CandidateWindow::ComputeColumnLayout(int max_surface_width,
                                                                   int max_description_width,
                                                                   UINT dpi) {
  const bool has_description = max_description_width > 0;
  const int surface_width =
      has_description ? std::min(max_surface_width, ScaleForDpi(kBaseMaxSurfaceWidth, dpi)) : 0;
  const int column_gap = has_description ? ScaleForDpi(kBaseColumnGap, dpi) : 0;
  const int content_width =
      (has_description ? surface_width : max_surface_width) + column_gap + max_description_width;
  return {surface_width, column_gap, content_width};
}

// static
RECT CandidateWindow::ComputePlacement(POINT anchor, RECT work_area, int width, int height,
                                       int caret_gap) {
  width = std::max(width, 0);
  height = std::max(height, 0);
  if (work_area.right <= work_area.left || work_area.bottom <= work_area.top) {
    return {anchor.x, anchor.y, anchor.x + width, anchor.y + height};
  }
  width = std::min(width, static_cast<int>(work_area.right - work_area.left));
  int x = std::clamp(static_cast<int>(anchor.x), static_cast<int>(work_area.left),
                     static_cast<int>(work_area.right) - width);
  int y = anchor.y;
  // The anchor is the caret's bottom-left; the gap stands in for the caret height.
  if (y + height > work_area.bottom) y = anchor.y - height - caret_gap;
  y = std::max(y, static_cast<int>(work_area.top));
  return {x, y, x + width, y + height};
}

// static
RECT CandidateWindow::ComputeDetailsPlacement(RECT candidate, RECT work_area, int width,
                                              int height) {
  width = std::clamp(width, 0, static_cast<int>(std::max(0L, work_area.right - work_area.left)));
  height = std::clamp(height, 0, static_cast<int>(std::max(0L, work_area.bottom - work_area.top)));
  const int x = std::clamp(
      static_cast<int>(candidate.left), static_cast<int>(work_area.left),
      std::max(static_cast<int>(work_area.left), static_cast<int>(work_area.right) - width));
  int y = static_cast<int>(candidate.bottom);
  if (y + height > work_area.bottom) y = static_cast<int>(candidate.top) - height;
  y = std::clamp(
      y, static_cast<int>(work_area.top),
      std::max(static_cast<int>(work_area.top), static_cast<int>(work_area.bottom) - height));
  return {x, y, x + width, y + height};
}

// static
CandidateWindow::Hit CandidateWindow::HitTest(const HitLayout& layout, POINT point) {
  constexpr Hit kNone{HitTarget::None, -1};
  if (layout.health_banner_visible && point.y >= layout.health_banner_top) {
    if (PtInRect(&layout.details_button, point)) return {HitTarget::HealthDetailsButton, -1};
    if (layout.has_retry_button && PtInRect(&layout.retry_button, point))
      return {HitTarget::HealthRetryButton, -1};
    return {HitTarget::HealthBanner, -1};
  }
  // The lock is not a candidate.
  if (layout.secure_indicator_width > 0 && point.y < layout.item_height &&
      point.x >= layout.client_width - layout.secure_indicator_width)
    return {HitTarget::SecureIndicator, -1};
  if (layout.item_height <= 0) return kNone;
  const int index = point.y / layout.item_height;
  if (index >= 0 && index < layout.item_count) return {HitTarget::Candidate, index};
  return kNone;
}

// static
UINT CandidateWindow::DpiForMonitor(HMONITOR monitor, HWND fallback_hwnd) {
#ifdef AZOOKEY_TSF_TESTING
  if (g_monitor_dpi_for_test) return g_monitor_dpi_for_test;
#endif
  UINT dpi_x = 0;
  UINT dpi_y = 0;
  if (monitor && SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y)) &&
      dpi_x != 0) {
    return dpi_x;
  }
  if (fallback_hwnd) {
    UINT window_dpi = GetDpiForWindow(fallback_hwnd);
    if (window_dpi != 0) return window_dpi;
  }
  return kDefaultDpi;
}

#ifdef AZOOKEY_TSF_TESTING
// static
CandidateWindow::LayoutMetricsForTest CandidateWindow::ComputeLayoutMetricsForTest(UINT dpi) {
  const LayoutMetrics metrics = ComputeLayoutMetrics(dpi);
  return {metrics.item_height, metrics.horizontal_padding, metrics.max_width,
          metrics.caret_gap,   metrics.min_text_width,     metrics.extra_width};
}

// static
CandidateWindow::ColumnLayoutForTest CandidateWindow::ComputeColumnLayoutForTest(
    int max_surface_width, int max_description_width, UINT dpi) {
  const ColumnLayout layout = ComputeColumnLayout(max_surface_width, max_description_width, dpi);
  return {layout.surface_width, layout.column_gap, layout.content_width};
}

CandidateWindow::Hit CandidateWindow::HitTestForTest(POINT point) const {
  RECT client_rc{};
  if (hwnd_) GetClientRect(hwnd_, &client_rc);
  return HitTest(CurrentHitLayout(client_rc.right), point);
}

// static
void CandidateWindow::SetMonitorDpiForTest(UINT dpi) { g_monitor_dpi_for_test = dpi; }

bool CandidateWindow::RenderForTest() {
  RECT client_rc{};
  return hwnd_ && GetClientRect(hwnd_, &client_rc) && Render(client_rc.right, client_rc.bottom);
}
#endif

// static
std::unique_ptr<CandidateWindow::TextStyle> CandidateWindow::CreateTextStyle(UINT dpi) noexcept {
  try {
    auto style = std::make_unique<TextStyle>();
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(style->factory.GetAddressOf()))))
      return nullptr;
    IDWriteFactory* factory = style->factory.Get();
    if (FAILED(CreateFormat(factory, dpi, DWRITE_WORD_WRAPPING_NO_WRAP,
                            DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true,
                            style->line)) ||
        FAILED(CreateFormat(factory, dpi, DWRITE_WORD_WRAPPING_NO_WRAP,
                            DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false,
                            style->centered)) ||
        FAILED(CreateFormat(factory, dpi, DWRITE_WORD_WRAPPING_WRAP, DWRITE_TEXT_ALIGNMENT_LEADING,
                            DWRITE_PARAGRAPH_ALIGNMENT_NEAR, true, style->wrapped)))
      return nullptr;
    return style;
  } catch (...) {
    return nullptr;
  }
}

void CandidateWindow::UpdateTheme() {
  const ThemeMode mode = CurrentThemeMode();
  theme_ = ResolveThemeColors(mode);
  ApplyWindowFrameTheme(hwnd_, mode);
}

void CandidateWindow::UpdateDpi(UINT dpi) {
  dpi = NormalizeDpi(dpi);
  if (dpi == dpi_ && text_style_) return;

  auto next = CreateTextStyle(dpi);
  // Keep the metrics and the text at one DPI: a failed style keeps the old pair.
  if (!next && text_style_) return;
  metrics_ = ComputeLayoutMetrics(dpi);
  text_style_ = std::move(next);
  dpi_ = dpi;
}

// static
ATOM CandidateWindow::RegisterWindowClass() {
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW | CS_DROPSHADOW;
  wc.lpfnWndProc = WndProc;
  wc.hInstance = GetTipModuleHandle();
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = kClassName;
  ATOM a = RegisterClassExW(&wc);
  if (!a && GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
    // CreateWindowExW uses the class name, so a non-zero sentinel is enough to
    // distinguish "already registered" from real registration failure.
    a = 1;
  }
  return a;
}

bool CandidateWindow::Create() {
  static ATOM s_atom = RegisterWindowClass();
  if (!s_atom) return false;

  {
    const ScopedPerMonitorDpiAwareness dpi_context;
    // DirectComposition draws the whole window, the border included (native-ui-spec §4.1).
    hwnd_ = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP, kClassName,
        nullptr, WS_POPUP, 0, 0, 200, metrics_.item_height, nullptr, nullptr, GetTipModuleHandle(),
        this);
  }
  if (hwnd_) {
    UpdateDpi(GetDpiForWindow(hwnd_));
    UpdateTheme();
  }
  return hwnd_ != nullptr;
}

void CandidateWindow::Destroy() {
  HideDetails();
  if (hwnd_) {
    KillTimer(hwnd_, kHealthBannerTimer);
    KillTimer(hwnd_, kSecureToastTimer);
    DestroyWindow(hwnd_);
    // hwnd_ is cleared in WM_DESTROY handler.
  }
  render_.reset();
}

int CandidateWindow::MeasureText(const std::wstring& text) const {
  if (!text_style_ || text.empty()) return 0;
  ComPtr<IDWriteTextLayout> layout;
  DWRITE_TEXT_METRICS measured{};
  if (FAILED(text_style_->factory->CreateTextLayout(
          text.c_str(), static_cast<UINT32>(text.size()), text_style_->line.Get(), 100000.0f,
          static_cast<FLOAT>(metrics_.item_height), &layout)) ||
      FAILED(layout->GetMetrics(&measured)))
    return 0;
  return static_cast<int>(std::ceil(measured.widthIncludingTrailingWhitespace));
}

void CandidateWindow::Show(POINT pt, const std::vector<CandidateViewItem>& items, int selected_idx,
                           std::wstring notice) {
  if (!hwnd_ || items.empty()) return;
  last_anchor_ = pt;

  const ScopedPerMonitorDpiAwareness dpi_context;
  items_ = items;
  notice_ = std::move(notice);
  selected_idx_ = std::clamp(selected_idx, 0, static_cast<int>(items_.size()) - 1);
  const MonitorWorkArea monitor = ResolveMonitorWorkArea(DefaultMonitorWin32Api(), pt);
  UpdateDpi(DpiForMonitor(monitor.monitor, hwnd_));

  // DirectWrite measures the same strings GDI did; widths may differ by a few
  // pixels, while the rows, padding and columns keep their metrics.
  int max_surface_w = metrics_.min_text_width;
  int max_description_w = 0;
  for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
    const auto& item = items_[static_cast<size_t>(i)];
    max_surface_w =
        std::max(max_surface_w, MeasureText(std::to_wstring(i + 1) + L". " + item.surface));
    max_description_w = std::max(max_description_w, MeasureText(item.description));
  }
  int notice_width = MeasureText(notice_);
  if (secure_toast_visible_) notice_width = std::max(notice_width, MeasureText(kSecureToastText));

  const ColumnLayout columns = ComputeColumnLayout(max_surface_w, max_description_w, dpi_);
  surface_column_width_ = columns.surface_width;
  int width = std::min(columns.content_width + metrics_.horizontal_padding * 2 +
                           metrics_.extra_width + SecureIndicatorWidth(),
                       metrics_.max_width);
  width =
      std::min(std::max(width, notice_width + metrics_.horizontal_padding * 2), metrics_.max_width);
  int height = HealthBannerTop();
  if (health_banner_visible_) {
    width = std::max(width, ScaleForDpi(480, dpi_));
    height += HealthBannerHeight();
  }

  const RECT placement = ComputePlacement(pt, monitor.work_area, width, height, metrics_.caret_gap);
  const int placed_width = placement.right - placement.left;
  const int placed_height = placement.bottom - placement.top;
  showing_ = true;
  // A failed frame keeps the window and its click regions; failure_stage_ says why.
  Render(placed_width, placed_height);
  SetWindowPos(hwnd_, HWND_TOPMOST, placement.left, placement.top, placed_width, placed_height,
               SWP_SHOWWINDOW | SWP_NOACTIVATE);
  showing_ = false;
}

void CandidateWindow::Hide() {
  HideDetails();
  if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
  HideHealthBanner();
  HideSecureToast();
}

void CandidateWindow::SetSecureIndicator(bool visible) {
  if (secure_indicator_visible_ == visible) return;
  secure_indicator_visible_ = visible;
  if (IsVisible()) ResizeAtLastAnchor();
}

void CandidateWindow::ShowSecureToast() {
  if (!hwnd_) return;
  secure_toast_visible_ = true;
  secure_toast_until_ = GetTickCount64() + kSecureToastDurationMs;
  if (IsVisible()) ResizeAtLastAnchor();
  SetTimer(hwnd_, kSecureToastTimer, kSecureToastDurationMs, nullptr);
}

void CandidateWindow::ResumeSecureToast() {
  if (!hwnd_ || secure_toast_visible_) return;
  const ULONGLONG now = GetTickCount64();
  if (now >= secure_toast_until_) return;
  secure_toast_visible_ = true;
  if (IsVisible()) ResizeAtLastAnchor();
  SetTimer(hwnd_, kSecureToastTimer, static_cast<UINT>(secure_toast_until_ - now), nullptr);
}

void CandidateWindow::HideSecureToast() {
  if (hwnd_) KillTimer(hwnd_, kSecureToastTimer);
  if (!secure_toast_visible_) return;
  secure_toast_visible_ = false;
  if (IsVisible()) ResizeAtLastAnchor();
}

void CandidateWindow::ShowHealthBanner(CandidateHealthState state) {
  if (state == CandidateHealthState::Healthy) {
    HideDetails();
    HideHealthBanner();
    health_state_ = state;
    health_banner_until_ = 0;
    return;
  }
  if (!hwnd_) return;
  if (state != health_state_) HideDetails();
  health_state_ = state;
  health_banner_visible_ = true;
  health_banner_until_ = GetTickCount64() + kHealthBannerDurationMs;
  if (IsVisible()) ResizeAtLastAnchor();
  SetTimer(hwnd_, kHealthBannerTimer, kHealthBannerDurationMs, nullptr);
}

void CandidateWindow::ResumeHealthBanner() {
  if (!hwnd_ || health_banner_visible_ || health_state_ == CandidateHealthState::Healthy) return;
  const ULONGLONG now = GetTickCount64();
  if (now >= health_banner_until_) return;
  health_banner_visible_ = true;
  if (IsVisible()) ResizeAtLastAnchor();
  SetTimer(hwnd_, kHealthBannerTimer, static_cast<UINT>(health_banner_until_ - now), nullptr);
}

void CandidateWindow::HideHealthBanner() {
  if (hwnd_) KillTimer(hwnd_, kHealthBannerTimer);
  if (!health_banner_visible_) return;
  health_banner_visible_ = false;
  if (IsVisible()) ResizeAtLastAnchor();
}

void CandidateWindow::SetRetryInFlight(bool in_flight) {
  retry_in_flight_ = in_flight;
  Repaint();
}

void CandidateWindow::ShowDetails() {
  if (!hwnd_ || health_state_ == CandidateHealthState::Healthy) return;
  HideDetails();
  static const ATOM details_class = []() -> ATOM {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DROPSHADOW;
    wc.lpfnWndProc = DetailsWndProc;
    wc.hInstance = GetTipModuleHandle();
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kDetailsClassName;
    ATOM atom = RegisterClassExW(&wc);
    if (atom) return atom;
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS ? ATOM{1} : ATOM{0};
  }();
  if (!details_class) return;

  RECT candidate_rc{};
  GetWindowRect(hwnd_, &candidate_rc);
  const MonitorWorkArea monitor = ResolveMonitorWorkArea(
      DefaultMonitorWin32Api(), {candidate_rc.left + (candidate_rc.right - candidate_rc.left) / 2,
                                 candidate_rc.top + (candidate_rc.bottom - candidate_rc.top) / 2});
  if (monitor.work_area.right <= monitor.work_area.left) return;
  const RECT placement = ComputeDetailsPlacement(candidate_rc, monitor.work_area,
                                                 ScaleForDpi(480, dpi_), ScaleForDpi(240, dpi_));
  details_hwnd_ = CreateWindowExW(
      WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
      kDetailsClassName, nullptr, WS_POPUP, placement.left, placement.top,
      placement.right - placement.left, placement.bottom - placement.top, hwnd_, nullptr,
      GetTipModuleHandle(), this);
  if (!details_hwnd_) return;
  RenderDetails();
  ShowWindow(details_hwnd_, SW_SHOWNOACTIVATE);
}

void CandidateWindow::HideDetails() {
  if (details_hwnd_) DestroyWindow(details_hwnd_);
  details_render_.reset();
}

void CandidateWindow::RenderDetails() {
  RECT client_rc{};
  if (!details_hwnd_ || !GetClientRect(details_hwnd_, &client_rc)) return;
  // The failure record belongs to the candidate window; the popup does not overwrite it.
  const char* const stage = failure_stage_;
  const HRESULT hr = failure_hr_;
  RenderTo(details_render_, details_hwnd_, client_rc.right, client_rc.bottom,
           &CandidateWindow::DrawDetails);
  failure_stage_ = stage;
  failure_hr_ = hr;
}

void CandidateWindow::DrawDetails(ID2D1DeviceContext* context, int width, int height) const {
  // Clear first so a failed brush still commits a defined frame.
  context->Clear(ToColorF(theme_.info_background));
  Painter paint(context, text_style_->factory.Get());
  if (!paint.ok()) return;
  RECT text_rc{0, 0, width, height};
  InflateRect(&text_rc, -metrics_.horizontal_padding, -metrics_.horizontal_padding);
  text_rc.bottom -= metrics_.item_height;
  paint.Text(HealthDetailsText(health_state_), text_style_->wrapped.Get(), text_rc,
             theme_.info_text);
  const RECT close_rc{width - ScaleForDpi(90, dpi_), height - metrics_.item_height, width, height};
  paint.Text(L"[閉じる]", text_style_->centered.Get(), close_rc, theme_.info_text);
  paint.Frame(width, height, theme_.border);
}

// static
LRESULT CALLBACK CandidateWindow::DetailsWndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                                 LPARAM lParam) {
  CandidateWindow* self = nullptr;
  if (msg == WM_NCCREATE) {
    auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
    self = static_cast<CandidateWindow*>(create->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  } else {
    self = reinterpret_cast<CandidateWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }
  switch (msg) {
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_PAINT: {
      // DirectComposition keeps the last frame; RenderDetails draws on change.
      PAINTSTRUCT paint{};
      BeginPaint(hwnd, &paint);
      EndPaint(hwnd, &paint);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_LBUTTONDOWN:
      if (self) self->HideDetails();
      return 0;
    case WM_DESTROY:
      if (self) {
        self->details_hwnd_ = nullptr;
        self->details_render_.reset();
      }
      return 0;
    default:
      return DefWindowProcW(hwnd, msg, wParam, lParam);
  }
}

void CandidateWindow::ResizeAtLastAnchor() {
  if (!items_.empty()) Show(last_anchor_, items_, selected_idx_, notice_);
}

int CandidateWindow::HealthBannerHeight() const { return metrics_.item_height * 3; }

int CandidateWindow::FooterTop() const {
  return metrics_.item_height * (static_cast<int>(items_.size()) + (notice_.empty() ? 0 : 1));
}

int CandidateWindow::HealthBannerTop() const {
  return FooterTop() + (secure_toast_visible_ ? metrics_.item_height : 0);
}

int CandidateWindow::SecureIndicatorWidth() const {
  return secure_indicator_visible_ ? ScaleForDpi(kBaseSecureIndicatorWidth, dpi_) : 0;
}

RECT CandidateWindow::HealthDetailsButtonRect(int width) const {
  const int top = HealthBannerTop() + metrics_.item_height * 2;
  const int button_width = ScaleForDpi(52, dpi_);
  const int right =
      width - metrics_.horizontal_padding -
      (health_state_ == CandidateHealthState::DegradedModel ? ScaleForDpi(68, dpi_) : 0);
  return {right - button_width, top, right, top + metrics_.item_height};
}

RECT CandidateWindow::HealthRetryButtonRect(int width) const {
  const int top = HealthBannerTop() + metrics_.item_height * 2;
  const int right = width - metrics_.horizontal_padding;
  return {right - ScaleForDpi(60, dpi_), top, right, top + metrics_.item_height};
}

CandidateWindow::HitLayout CandidateWindow::CurrentHitLayout(int client_width) const {
  return {metrics_.item_height,
          static_cast<int>(items_.size()),
          client_width,
          SecureIndicatorWidth(),
          health_banner_visible_,
          HealthBannerTop(),
          HealthDetailsButtonRect(client_width),
          health_state_ == CandidateHealthState::DegradedModel,
          HealthRetryButtonRect(client_width)};
}

void CandidateWindow::PostCandidatesReady() {
  if (hwnd_) PostMessageW(hwnd_, kCandidatesReadyMessage, 0, 0);
}

bool CandidateWindow::ScheduleCandidatesReady(UINT delay_ms) {
  return hwnd_ && SetTimer(hwnd_, kCandidatesReadyTimer, delay_ms, nullptr) != 0;
}

void CandidateWindow::CancelScheduledCandidatesReady() {
  if (hwnd_) KillTimer(hwnd_, kCandidatesReadyTimer);
}

bool CandidateWindow::IsVisible() const { return hwnd_ && IsWindowVisible(hwnd_); }

void CandidateWindow::MoveSelection(int delta) {
  if (items_.empty()) return;
  int n = static_cast<int>(items_.size());
  selected_idx_ = internal::WrapCandidateSelectionIndex(selected_idx_, delta, n);
  Repaint();
}

void CandidateWindow::SetSelected(int idx) {
  if (items_.empty()) return;
  selected_idx_ = std::clamp(idx, 0, static_cast<int>(items_.size()) - 1);
  Repaint();
}

void CandidateWindow::Repaint() {
  RECT client_rc{};
  if (!IsVisible() || !GetClientRect(hwnd_, &client_rc)) return;
  Render(client_rc.right, client_rc.bottom);
}

bool CandidateWindow::Fail(const char* stage, HRESULT hr) {
  failure_stage_ = stage;
  failure_hr_ = hr;
  return false;
}

bool CandidateWindow::Render(int width, int height) {
  return RenderTo(render_, hwnd_, width, height, &CandidateWindow::DrawContent);
}

bool CandidateWindow::RenderTo(std::unique_ptr<RenderState>& state, HWND hwnd, int width,
                               int height, DrawFn draw) noexcept {
  try {
    if (!hwnd || width <= 0 || height <= 0) return Fail("size", E_FAIL);
    if (!text_style_) return Fail("text_style", E_FAIL);
    failure_stage_ = "";
    failure_hr_ = S_OK;
    // A lost device is rebuilt once; any other failure waits for the next frame.
    for (int attempt = 0; attempt < 2; ++attempt) {
      if (!state) {
        auto next = std::make_unique<RenderState>();
        if (!next->engine.Initialize(hwnd, SurfaceAlpha::Opaque))
          return Fail(next->engine.failure_stage(), next->engine.failure_hr());
        state = std::move(next);
      }
      RenderingEngine& engine = state->engine;
      bool drawn = engine.ResizeSurface(width, height);
      if (drawn) {
        ID2D1DeviceContext* context = engine.BeginDraw();
        drawn = context != nullptr;
        if (drawn) {
          (this->*draw)(context, width, height);
          drawn = engine.EndDraw();
        }
      }
      if (drawn) return true;
      Fail(engine.failure_stage(), engine.failure_hr());
      if (!IsDeviceLost(failure_hr_)) return false;
      state.reset();
    }
    return false;
  } catch (...) {
    // Never unwind into a window procedure or the TSF caller.
    state.reset();
    return Fail("exception", E_OUTOFMEMORY);
  }
}

void CandidateWindow::DrawContent(ID2D1DeviceContext* context, int width, int height) const {
  // Clear first so a failed brush still commits a defined frame.
  context->Clear(ToColorF(theme_.background));
  Painter paint(context, text_style_->factory.Get());
  if (!paint.ok()) return;
  const TextStyle& style = *text_style_;
  // Candidate text stops short of the secure indicator column.
  const int content_right = width - SecureIndicatorWidth();

  for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
    const RECT row_rc = {0, i * metrics_.item_height, width, (i + 1) * metrics_.item_height};
    const bool selected = i == selected_idx_;
    if (selected) paint.Fill(row_rc, theme_.selection);
    const auto& item = items_[static_cast<size_t>(i)];
    RECT surface_rc = row_rc;
    surface_rc.left += metrics_.horizontal_padding;
    surface_rc.right = surface_column_width_ > 0
                           ? std::min(content_right - metrics_.horizontal_padding,
                                      static_cast<int>(surface_rc.left) + surface_column_width_)
                           : content_right - metrics_.horizontal_padding;
    // Font fallback picks Segoe UI Emoji for emoji, drawn in color (native-ui-spec §3.4).
    paint.Text(std::to_wstring(i + 1) + L". " + item.surface, style.line.Get(), surface_rc,
               selected ? theme_.selection_text : theme_.text);
    if (!item.description.empty()) {
      RECT description_rc = row_rc;
      description_rc.left = surface_rc.right + ScaleForDpi(kBaseColumnGap, dpi_);
      description_rc.right = content_right - metrics_.horizontal_padding;
      paint.Text(item.description, style.line.Get(), description_rc,
                 selected ? theme_.selection_text : theme_.sub_text);
    }
  }
  if (secure_indicator_visible_ && !items_.empty()) {
    // Top-right corner of the first row; drawn over that row's background.
    const RECT icon_rc{content_right, 0, width, metrics_.item_height};
    paint.Text(kSecureIndicatorText, style.centered.Get(), icon_rc,
               selected_idx_ == 0 ? theme_.selection_text : theme_.text);
  }
  const int notice_top = metrics_.item_height * static_cast<int>(items_.size());
  if (!notice_.empty()) {
    RECT notice_rc = {0, notice_top, width, notice_top + metrics_.item_height};
    paint.Fill(notice_rc, theme_.panel_background);
    InflateRect(&notice_rc, -metrics_.horizontal_padding, 0);
    paint.Text(notice_, style.line.Get(), notice_rc, theme_.panel_text);
  }
  if (secure_toast_visible_) {
    const int toast_top = FooterTop();
    RECT toast_rc{0, toast_top, width, toast_top + metrics_.item_height};
    paint.Fill(toast_rc, theme_.info_background);
    InflateRect(&toast_rc, -metrics_.horizontal_padding, 0);
    paint.Text(kSecureToastText, style.line.Get(), toast_rc, theme_.info_text);
  }
  if (health_banner_visible_) {
    const RECT banner_rc{0, HealthBannerTop(), width, height};
    paint.Fill(banner_rc, theme_.banner_background);
    RECT text_rc = banner_rc;
    text_rc.left += metrics_.horizontal_padding;
    text_rc.right -= metrics_.horizontal_padding;
    text_rc.top += ScaleForDpi(3, dpi_);
    text_rc.bottom = banner_rc.bottom - metrics_.item_height;
    paint.Text(HealthBannerText(health_state_), style.wrapped.Get(), text_rc, theme_.info_text);
    paint.Text(L"[詳細]", style.centered.Get(), HealthDetailsButtonRect(width), theme_.info_text);
    if (health_state_ == CandidateHealthState::DegradedModel) {
      paint.Text(L"[再試行]", style.centered.Get(), HealthRetryButtonRect(width),
                 retry_in_flight_ ? theme_.sub_text : theme_.info_text);
    }
  }
  paint.Frame(width, height, theme_.border);
}

#ifdef AZOOKEY_TSF_TESTING
bool CandidateWindow::RenderPixelsForTest(std::vector<COLORREF>* pixels, int* width,
                                          int* height) const {
  RECT client_rc{};
  if (!pixels || !width || !height || !hwnd_ || !text_style_ || !GetClientRect(hwnd_, &client_rc) ||
      client_rc.right <= 0 || client_rc.bottom <= 0)
    return false;
  const auto w = static_cast<UINT32>(client_rc.right);
  const auto h = static_cast<UINT32>(client_rc.bottom);

  ComPtr<ID3D11Device> d3d_device;
  ComPtr<IDXGIDevice> dxgi_device;
  ComPtr<ID2D1Factory1> factory;
  ComPtr<ID2D1Device> device;
  ComPtr<ID2D1DeviceContext> context;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                               D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                               &d3d_device, nullptr, nullptr)) ||
      FAILED(d3d_device.As(&dxgi_device)) ||
      FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr,
                               reinterpret_cast<void**>(factory.GetAddressOf()))) ||
      FAILED(factory->CreateDevice(dxgi_device.Get(), &device)) ||
      FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context)))
    return false;

  const D2D1_PIXEL_FORMAT format =
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE);
  ComPtr<ID2D1Bitmap1> target;
  ComPtr<ID2D1Bitmap1> readback;
  if (FAILED(context->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0,
                                   D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, format),
                                   &target)) ||
      FAILED(context->CreateBitmap(
          D2D1::SizeU(w, h), nullptr, 0,
          D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                  format),
          &readback)))
    return false;
  context->SetTarget(target.Get());
  context->SetDpi(96.0f, 96.0f);
  // Grayscale text keeps every non-emoji pixel neutral, so color means a color font.
  context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
  context->BeginDraw();
  DrawContent(context.Get(), client_rc.right, client_rc.bottom);
  if (FAILED(context->EndDraw()) ||
      FAILED(readback->CopyFromBitmap(nullptr, target.Get(), nullptr)))
    return false;

  D2D1_MAPPED_RECT mapped{};
  if (FAILED(readback->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return false;
  pixels->assign(static_cast<size_t>(w) * h, 0);
  for (UINT32 y = 0; y < h; ++y) {
    const BYTE* row = mapped.bits + static_cast<size_t>(y) * mapped.pitch;
    for (UINT32 x = 0; x < w; ++x) {
      const BYTE* bgra = row + static_cast<size_t>(x) * 4;
      (*pixels)[static_cast<size_t>(y) * w + x] = RGB(bgra[2], bgra[1], bgra[0]);
    }
  }
  readback->Unmap();
  *width = client_rc.right;
  *height = client_rc.bottom;
  return true;
}
#endif

// static
LRESULT CALLBACK CandidateWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  CandidateWindow* self = nullptr;
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
    self = static_cast<CandidateWindow*>(cs->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    self->hwnd_ = hwnd;
  } else {
    self = reinterpret_cast<CandidateWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }
  // DirectComposition draws the whole client area. This is handled here, not
  // as a case in HandleMessage: clang-cl 19 for ARM64 fails with "assembler
  // label '' can not be undefined" when that switch gains it.
  if (msg == WM_ERASEBKGND) return 1;
  if (self) return self->HandleMessage(hwnd, msg, wParam, lParam);
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CandidateWindow::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;

    case WM_PAINT: {
      // DirectComposition keeps the last frame; Render draws on every change.
      PAINTSTRUCT ps;
      BeginPaint(hwnd, &ps);
      EndPaint(hwnd, &ps);
      return 0;
    }

    case WM_LBUTTONDOWN: {
      RECT client_rc{};
      GetClientRect(hwnd, &client_rc);
      const Hit hit =
          HitTest(CurrentHitLayout(client_rc.right), {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
      switch (hit.target) {
        case HitTarget::HealthDetailsButton:
          ShowDetails();
          break;
        case HitTarget::HealthRetryButton:
          if (retry_in_flight_ || !on_retry_) break;
          retry_in_flight_ = true;
          Repaint();
          try {
            on_retry_();
          } catch (...) {
            // Never unwind through a Win32 window procedure.
            retry_in_flight_ = false;
            Repaint();
          }
          break;
        case HitTarget::Candidate:
          selected_idx_ = hit.index;
          Repaint();
          if (on_click_) on_click_(hit.index);
          break;
        case HitTarget::None:
        case HitTarget::SecureIndicator:
        case HitTarget::HealthBanner:
          break;
      }
      return 0;
    }

    case WM_SETTINGCHANGE:
    case WM_SYSCOLORCHANGE:
    case WM_THEMECHANGED:
      if (msg != WM_SETTINGCHANGE || IsThemeSettingChange(wParam, lParam)) {
        UpdateTheme();
        Repaint();
        RenderDetails();
      }
      return DefWindowProcW(hwnd, msg, wParam, lParam);

    case kCandidatesReadyMessage:
      if (on_candidates_ready_) on_candidates_ready_(on_candidates_ready_context_);
      return 0;

    case WM_TIMER:
      if (wParam == kHealthBannerTimer) {
        HideHealthBanner();
        return 0;
      }
      if (wParam == kSecureToastTimer) {
        HideSecureToast();
        return 0;
      }
      if (wParam == kCandidatesReadyTimer) {
        CancelScheduledCandidatesReady();
        if (on_candidates_ready_) on_candidates_ready_(on_candidates_ready_context_);
        return 0;
      }
      return DefWindowProcW(hwnd, msg, wParam, lParam);

    case WM_DPICHANGED:
      // The suggested rect is ignored: the surface size and the placement both
      // come from Show, which measures for the anchor's monitor. A change that
      // Show's own SetWindowPos causes is already measured for.
      if (showing_) return 0;
      UpdateDpi(LOWORD(wParam));
      if (IsVisible()) ResizeAtLastAnchor();
      return 0;

    case WM_DESTROY:
      HideDetails();
      // The device stack is bound to this HWND; a later Create makes a new one.
      render_.reset();
      hwnd_ = nullptr;
      return 0;

    default:
      // Use the hwnd parameter from WndProc, not the hwnd_ member: after
      // WM_DESTROY sets hwnd_ to nullptr, trailing messages (e.g. WM_NCDESTROY)
      // would otherwise forward with a null handle.
      return DefWindowProcW(hwnd, msg, wParam, lParam);
  }
}

}  // namespace azookey::tsf
