#include "azookey/tsf/CandidateWindow.h"

#include <ShellScalingApi.h>
#include <dwrite_2.h>
#include <icu.h>
#include <windowsx.h>
#include <wrl.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <map>
#include <string>

#include "CandidateSelection.h"
#include "azookey/tsf/CaretRectResolver.h"
#include "azookey/tsf/DpiScaling.h"

namespace azookey::tsf {

struct EmojiDrawingCache {
  Microsoft::WRL::ComPtr<IDWriteFactory2> factory;
  Microsoft::WRL::ComPtr<IDWriteGdiInterop> interop;
  Microsoft::WRL::ComPtr<IDWriteBitmapRenderTarget> target;
  Microsoft::WRL::ComPtr<IDWriteRenderingParams> params;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
  std::map<std::wstring, Microsoft::WRL::ComPtr<IDWriteTextLayout>> layouts;
  float font_size{};
};

bool CandidateWindow::NeedsColorEmoji(const std::wstring& text) {
  for (size_t i = 0; i < text.size(); ++i) {
    UChar32 cp = text[i];
    if (cp >= 0xd800 && cp <= 0xdbff) {
      if (i + 1 >= text.size() || text[i + 1] < 0xdc00 || text[i + 1] > 0xdfff) continue;
      cp = 0x10000 + ((cp - 0xd800) << 10) + (text[++i] - 0xdc00);
    }
    if (i + 1 < text.size() && text[i + 1] == 0xfe0e) continue;
    if (u_hasBinaryProperty(cp, UCHAR_EMOJI_PRESENTATION) ||
        (i + 1 < text.size() && text[i + 1] == 0xfe0f && u_hasBinaryProperty(cp, UCHAR_EMOJI)))
      return true;
  }
  return false;
}

namespace {
using Microsoft::WRL::ComPtr;

class ColorGlyphRenderer final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDWriteTextRenderer> {
 public:
  ColorGlyphRenderer(IDWriteFactory2* factory, IDWriteBitmapRenderTarget* target,
                     IDWriteRenderingParams* params, COLORREF color)
      : factory_(factory), target_(target), params_(params), color_(color) {}
  IFACEMETHODIMP IsPixelSnappingDisabled(void*, BOOL* value) override {
    if (!value) return E_POINTER;
    *value = FALSE;
    return S_OK;
  }
  IFACEMETHODIMP GetCurrentTransform(void*, DWRITE_MATRIX* value) override {
    return target_->GetCurrentTransform(value);
  }
  IFACEMETHODIMP GetPixelsPerDip(void*, FLOAT* value) override {
    if (!value) return E_POINTER;
    *value = target_->GetPixelsPerDip();
    return S_OK;
  }
  IFACEMETHODIMP DrawGlyphRun(void*, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE mode,
                              const DWRITE_GLYPH_RUN* run,
                              const DWRITE_GLYPH_RUN_DESCRIPTION* description, IUnknown*) override {
    ComPtr<IDWriteColorGlyphRunEnumerator> layers;
    const auto hr =
        factory_->TranslateColorGlyphRun(x, y, run, description, mode, nullptr, 0, &layers);
    if (hr == DWRITE_E_NOCOLOR)
      return target_->DrawGlyphRun(x, y, mode, run, params_.Get(), color_, nullptr);
    if (FAILED(hr)) return hr;
    BOOL more = FALSE;
    for (;;) {
      const auto next = layers->MoveNext(&more);
      if (FAILED(next)) return next;
      if (!more) return S_OK;
      const DWRITE_COLOR_GLYPH_RUN* layer = nullptr;
      const auto current = layers->GetCurrentRun(&layer);
      if (FAILED(current)) return current;
      const auto channel = [](float value) { return static_cast<BYTE>(value * 255.0f + 0.5f); };
      const auto color = layer->paletteIndex == 0xffff
                             ? color_
                             : RGB(channel(layer->runColor.r), channel(layer->runColor.g),
                                   channel(layer->runColor.b));
      const auto draw = target_->DrawGlyphRun(layer->baselineOriginX, layer->baselineOriginY, mode,
                                              &layer->glyphRun, params_.Get(), color, nullptr);
      if (FAILED(draw)) return draw;
    }
  }
  IFACEMETHODIMP DrawUnderline(void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) override {
    return S_OK;  // Candidate layouts never request decoration.
  }
  IFACEMETHODIMP DrawStrikethrough(void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*,
                                   IUnknown*) override {
    return S_OK;
  }
  IFACEMETHODIMP DrawInlineObject(void* context, FLOAT x, FLOAT y, IDWriteInlineObject* object,
                                  BOOL sideways, BOOL rtl, IUnknown* effect) override {
    return object->Draw(context, this, x, y, sideways, rtl, effect);
  }

 private:
  ComPtr<IDWriteFactory2> factory_;
  ComPtr<IDWriteBitmapRenderTarget> target_;
  ComPtr<IDWriteRenderingParams> params_;
  COLORREF color_;
};

ComPtr<IDWriteTextLayout> EmojiLayout(EmojiDrawingCache& cache, HFONT font,
                                      const std::wstring& text, float width, float height) {
  if (!cache.factory) {
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory2),
                                   reinterpret_cast<IUnknown**>(cache.factory.GetAddressOf()))))
      return {};
  }
  if (!cache.interop && FAILED(cache.factory->GetGdiInterop(&cache.interop))) return {};
  if (!cache.params && FAILED(cache.factory->CreateRenderingParams(&cache.params))) return {};
  LOGFONTW logfont{};
  const float size = font && GetObjectW(font, sizeof(logfont), &logfont)
                         ? static_cast<float>(std::abs(logfont.lfHeight))
                         : 16.0f;
  // GDI font and layout dimensions are physical pixels; pixelsPerDip=1 deliberately
  // makes one layout unit one pixel, including when the message font is DPI-scaled.
  if (!cache.format || cache.font_size != size) {
    cache.format.Reset();
    cache.layouts.clear();
    cache.font_size = size;
    if (FAILED(cache.factory->CreateTextFormat(
            L"Segoe UI Emoji", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size, L"ja-JP", &cache.format)))
      return {};
    cache.format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    cache.format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    ComPtr<IDWriteInlineObject> ellipsis;
    if (SUCCEEDED(cache.factory->CreateEllipsisTrimmingSign(cache.format.Get(), &ellipsis))) {
      const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
      cache.format->SetTrimming(&trimming, ellipsis.Get());
    }
  }
  auto& layout = cache.layouts[text];
  if (!layout &&
      FAILED(cache.factory->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()),
                                             cache.format.Get(), width, height, &layout)))
    return {};
  layout->SetMaxWidth(width);
  layout->SetMaxHeight(height);
  return layout;
}

bool DrawColorEmoji(EmojiDrawingCache& cache, HDC hdc, HFONT font, const std::wstring& text,
                    const RECT& rect) {
  const auto width = rect.right - rect.left;
  const auto height = rect.bottom - rect.top;
  if (width <= 0 || height <= 0) return false;
  const auto layout =
      EmojiLayout(cache, font, text, static_cast<float>(width), static_cast<float>(height));
  if (!layout) return false;
  if (!cache.target) {
    if (FAILED(cache.interop->CreateBitmapRenderTarget(hdc, width, height, &cache.target)))
      return false;
    cache.target->SetPixelsPerDip(1.0f);
  } else {
    SIZE size{};
    if (FAILED(cache.target->GetSize(&size))) return false;
    if ((size.cx != width || size.cy != height) && FAILED(cache.target->Resize(width, height)))
      return false;
  }
  const auto memory_dc = cache.target->GetMemoryDC();
  if (!BitBlt(memory_dc, 0, 0, width, height, hdc, rect.left, rect.top, SRCCOPY)) return false;
  const auto renderer = Microsoft::WRL::Make<ColorGlyphRenderer>(
      cache.factory.Get(), cache.target.Get(), cache.params.Get(), GetTextColor(hdc));
  if (!renderer || FAILED(layout->Draw(nullptr, renderer.Get(), 0, 0))) return false;
  return BitBlt(hdc, rect.left, rect.top, width, height, memory_dc, 0, 0, SRCCOPY) != FALSE;
}

constexpr wchar_t kClassName[] = L"azooKeyCandidateWnd";
constexpr wchar_t kDetailsClassName[] = L"azooKeyCandidateHealthDetailsWnd";
constexpr wchar_t kFallbackFontFace[] = L"Yu Gothic UI";
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

// Owns a GDI brush for one paint pass.
class SolidBrush {
 public:
  explicit SolidBrush(COLORREF color) : brush_(CreateSolidBrush(color)) {}
  ~SolidBrush() {
    if (brush_) DeleteObject(brush_);
  }
  SolidBrush(const SolidBrush&) = delete;
  SolidBrush& operator=(const SolidBrush&) = delete;

  void Fill(HDC hdc, const RECT& rect) const {
    if (brush_) FillRect(hdc, &rect, brush_);
  }

 private:
  HBRUSH brush_;
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
UINT CandidateWindow::DpiForMonitor(HMONITOR monitor, HWND fallback_hwnd) {
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
#endif

// static
HFONT CandidateWindow::CreateMessageFont(UINT dpi) {
  dpi = NormalizeDpi(dpi);

  NONCLIENTMETRICSW nonclient_metrics{};
  nonclient_metrics.cbSize = sizeof(nonclient_metrics);
  LOGFONTW log_font{};
  if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(nonclient_metrics),
                                 &nonclient_metrics, 0, dpi)) {
    log_font = nonclient_metrics.lfMessageFont;
  } else {
    log_font.lfHeight = -MulDiv(9, static_cast<int>(dpi), 72);
    log_font.lfWeight = FW_NORMAL;
    lstrcpynW(log_font.lfFaceName, kFallbackFontFace, LF_FACESIZE);
  }
  return CreateFontIndirectW(&log_font);
}

void CandidateWindow::UpdateTheme() {
  const ThemeMode mode = CurrentThemeMode();
  theme_ = ResolveThemeColors(mode);
  ApplyWindowFrameTheme(hwnd_, mode);
}

void CandidateWindow::UpdateDpi(UINT dpi) {
  dpi = NormalizeDpi(dpi);
  if (dpi == dpi_ && font_) return;

  metrics_ = ComputeLayoutMetrics(dpi);
  HFONT next_font = CreateMessageFont(dpi);
  if (next_font) {
    if (font_) DeleteObject(font_);
    font_ = next_font;
  }
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
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
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
    hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClassName,
                            nullptr, WS_POPUP | WS_BORDER, 0, 0, 200, metrics_.item_height, nullptr,
                            nullptr, GetTipModuleHandle(), this);
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
  if (font_) {
    DeleteObject(font_);
    font_ = nullptr;
  }
}

void CandidateWindow::Show(POINT pt, const std::vector<CandidateViewItem>& items, int selected_idx,
                           std::wstring notice) {
  if (!hwnd_ || items.empty()) return;
  last_anchor_ = pt;

  const ScopedPerMonitorDpiAwareness dpi_context;
  items_ = items;
  notice_ = std::move(notice);
  if (!emoji_cache_) emoji_cache_ = std::make_unique<EmojiDrawingCache>();
  emoji_cache_->layouts.clear();
  selected_idx_ = std::clamp(selected_idx, 0, static_cast<int>(items_.size()) - 1);
  const MonitorWorkArea monitor = ResolveMonitorWorkArea(DefaultMonitorWin32Api(), pt);
  UpdateDpi(DpiForMonitor(monitor.monitor, hwnd_));

  // Measure maximum text width using the window's DC.
  HDC hdc = GetDC(hwnd_);
  HGDIOBJ old_font = nullptr;
  if (hdc && font_) old_font = SelectObject(hdc, font_);
  int max_surface_w = metrics_.min_text_width;
  int max_description_w = 0;
  int notice_width = 0;
  if (hdc) {
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
      const auto& item = items_[static_cast<size_t>(i)];
      std::wstring label = std::to_wstring(i + 1) + L". " + item.surface;
      SIZE sz{};
      GetTextExtentPoint32W(hdc, label.c_str(), static_cast<int>(label.size()), &sz);
      if (NeedsColorEmoji(item.surface)) {
        const auto layout = EmojiLayout(*emoji_cache_, font_, item.surface, 100000.0f,
                                        static_cast<float>(metrics_.item_height));
        DWRITE_TEXT_METRICS measured{};
        if (layout && SUCCEEDED(layout->GetMetrics(&measured))) {
          const auto prefix = std::to_wstring(i + 1) + L". ";
          GetTextExtentPoint32W(hdc, prefix.data(), static_cast<int>(prefix.size()), &sz);
          sz.cx += static_cast<LONG>(std::ceil(measured.widthIncludingTrailingWhitespace));
        }
      }
      max_surface_w = std::max(max_surface_w, static_cast<int>(sz.cx));
      if (!item.description.empty()) {
        GetTextExtentPoint32W(hdc, item.description.c_str(),
                              static_cast<int>(item.description.size()), &sz);
        max_description_w = std::max(max_description_w, static_cast<int>(sz.cx));
      }
    }
    if (!notice_.empty()) {
      SIZE size{};
      GetTextExtentPoint32W(hdc, notice_.c_str(), static_cast<int>(notice_.size()), &size);
      notice_width = size.cx;
    }
    if (secure_toast_visible_) {
      SIZE size{};
      GetTextExtentPoint32W(hdc, kSecureToastText, static_cast<int>(wcslen(kSecureToastText)),
                            &size);
      notice_width = std::max(notice_width, static_cast<int>(size.cx));
    }
  }
  if (hdc) {
    if (old_font) SelectObject(hdc, old_font);
    ReleaseDC(hwnd_, hdc);
  }

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
  SetWindowPos(hwnd_, HWND_TOPMOST, placement.left, placement.top, placement.right - placement.left,
               placement.bottom - placement.top, SWP_SHOWWINDOW | SWP_NOACTIVATE);
  InvalidateRect(hwnd_, nullptr, TRUE);
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
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_INFOBK + 1);
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
      WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kDetailsClassName, nullptr,
      WS_POPUP | WS_BORDER, placement.left, placement.top, placement.right - placement.left,
      placement.bottom - placement.top, hwnd_, nullptr, GetTipModuleHandle(), this);
  if (details_hwnd_) ShowWindow(details_hwnd_, SW_SHOWNOACTIVATE);
}

void CandidateWindow::HideDetails() {
  if (details_hwnd_) DestroyWindow(details_hwnd_);
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
      PAINTSTRUCT paint{};
      HDC hdc = BeginPaint(hwnd, &paint);
      RECT rect{};
      GetClientRect(hwnd, &rect);
      const ThemeColors theme = self ? self->theme_ : kLightTheme;
      SolidBrush(theme.info_background).Fill(hdc, rect);
      HGDIOBJ old_font = self && self->font_ ? SelectObject(hdc, self->font_) : nullptr;
      SetBkMode(hdc, TRANSPARENT);
      SetTextColor(hdc, theme.info_text);
      const int pad = self ? self->metrics_.horizontal_padding : 8;
      InflateRect(&rect, -pad, -pad);
      rect.bottom -= self ? self->metrics_.item_height : 24;
      if (self) {
        DrawTextW(hdc, HealthDetailsText(self->health_state_), -1, &rect,
                  DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
      }
      RECT close_rect{};
      GetClientRect(hwnd, &close_rect);
      close_rect.left = close_rect.right - (self ? ScaleForDpi(90, self->dpi_) : 90);
      close_rect.top = close_rect.bottom - (self ? self->metrics_.item_height : 24);
      DrawTextW(hdc, L"[閉じる]", -1, &close_rect,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      if (old_font) SelectObject(hdc, old_font);
      EndPaint(hwnd, &paint);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;  // WM_PAINT fills the whole client area.
    case WM_LBUTTONDOWN:
      if (self) self->HideDetails();
      return 0;
    case WM_DESTROY:
      if (self) self->details_hwnd_ = nullptr;
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

void CandidateWindow::Repaint() const {
  if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

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
  // WM_PAINT fills the whole client area with the theme background. This is
  // handled here, not as a case in HandleMessage: clang-cl 19 for ARM64 fails
  // with "assembler label '' can not be undefined" when that switch gains it.
  if (msg == WM_ERASEBKGND) return 1;
  if (self) return self->HandleMessage(hwnd, msg, wParam, lParam);
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CandidateWindow::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;

    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC hdc = BeginPaint(hwnd, &ps);
      UpdateDpi(GetDpiForWindow(hwnd));
      HGDIOBJ old_font = nullptr;
      if (font_) old_font = SelectObject(hdc, font_);
      SetBkMode(hdc, TRANSPARENT);

      RECT client_rc{};
      GetClientRect(hwnd, &client_rc);
      SolidBrush(theme_.background).Fill(hdc, client_rc);
      const SolidBrush selection_brush(theme_.selection);
      // Candidate text stops short of the secure indicator column.
      const LONG content_right = client_rc.right - SecureIndicatorWidth();

      for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        RECT row_rc = {0, i * metrics_.item_height, client_rc.right,
                       (i + 1) * metrics_.item_height};
        if (i == selected_idx_) {
          selection_brush.Fill(hdc, row_rc);
          SetTextColor(hdc, theme_.selection_text);
        } else {
          SetTextColor(hdc, theme_.text);
        }
        const auto& item = items_[static_cast<size_t>(i)];
        std::wstring label = std::to_wstring(i + 1) + L". " + item.surface;
        RECT surface_rc = row_rc;
        surface_rc.left += metrics_.horizontal_padding;
        surface_rc.right = surface_column_width_ > 0
                               ? std::min(content_right - metrics_.horizontal_padding,
                                          surface_rc.left + surface_column_width_)
                               : content_right - metrics_.horizontal_padding;
        bool color_drawn = false;
        if (NeedsColorEmoji(item.surface) && emoji_cache_) {
          const auto prefix = std::to_wstring(i + 1) + L". ";
          SIZE prefix_size{};
          GetTextExtentPoint32W(hdc, prefix.data(), static_cast<int>(prefix.size()), &prefix_size);
          RECT emoji_rect = surface_rc;
          emoji_rect.left += prefix_size.cx;
          color_drawn = DrawColorEmoji(*emoji_cache_, hdc, font_, item.surface, emoji_rect);
          if (color_drawn) {
            RECT prefix_rect = surface_rc;
            prefix_rect.right = emoji_rect.left;
            DrawTextW(hdc, prefix.data(), static_cast<int>(prefix.size()), &prefix_rect,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
          }
        }
        if (!color_drawn)
          DrawTextW(hdc, label.c_str(), static_cast<int>(label.size()), &surface_rc,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        if (!item.description.empty()) {
          if (i != selected_idx_) SetTextColor(hdc, theme_.sub_text);
          RECT description_rc = row_rc;
          description_rc.left = surface_rc.right + ScaleForDpi(kBaseColumnGap, dpi_);
          description_rc.right = content_right - metrics_.horizontal_padding;
          if (description_rc.left < description_rc.right) {
            DrawTextW(hdc, item.description.c_str(), static_cast<int>(item.description.size()),
                      &description_rc,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
          }
        }
      }
      if (secure_indicator_visible_ && !items_.empty()) {
        // Top-right corner of the first row; drawn over that row's background.
        RECT icon_rc{content_right, 0, client_rc.right, metrics_.item_height};
        SetTextColor(hdc, selected_idx_ == 0 ? theme_.selection_text : theme_.text);
        const std::wstring icon = kSecureIndicatorText;
        if (!emoji_cache_ || !DrawColorEmoji(*emoji_cache_, hdc, font_, icon, icon_rc)) {
          DrawTextW(hdc, icon.c_str(), static_cast<int>(icon.size()), &icon_rc,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
      }
      const int notice_top = metrics_.item_height * static_cast<int>(items_.size());
      if (!notice_.empty()) {
        RECT notice_rc = {0, notice_top, client_rc.right, notice_top + metrics_.item_height};
        SolidBrush(theme_.panel_background).Fill(hdc, notice_rc);
        SetTextColor(hdc, theme_.panel_text);
        notice_rc.left += metrics_.horizontal_padding;
        notice_rc.right -= metrics_.horizontal_padding;
        DrawTextW(hdc, notice_.c_str(), static_cast<int>(notice_.size()), &notice_rc,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
      }
      if (secure_toast_visible_) {
        const int toast_top = FooterTop();
        RECT toast_rc{0, toast_top, client_rc.right, toast_top + metrics_.item_height};
        SolidBrush(theme_.info_background).Fill(hdc, toast_rc);
        SetTextColor(hdc, theme_.info_text);
        toast_rc.left += metrics_.horizontal_padding;
        toast_rc.right -= metrics_.horizontal_padding;
        const std::wstring toast = kSecureToastText;
        if (!emoji_cache_ || !DrawColorEmoji(*emoji_cache_, hdc, font_, toast, toast_rc)) {
          DrawTextW(hdc, toast.c_str(), static_cast<int>(toast.size()), &toast_rc,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        }
      }
      if (health_banner_visible_) {
        RECT banner_rc{0, HealthBannerTop(), client_rc.right, client_rc.bottom};
        SolidBrush(theme_.banner_background).Fill(hdc, banner_rc);
        SetTextColor(hdc, theme_.info_text);
        RECT text_rc = banner_rc;
        text_rc.left += metrics_.horizontal_padding;
        text_rc.right -= metrics_.horizontal_padding;
        text_rc.top += ScaleForDpi(3, dpi_);
        text_rc.bottom = banner_rc.bottom - metrics_.item_height;
        DrawTextW(hdc, HealthBannerText(health_state_), -1, &text_rc,
                  DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);
        RECT details_rc = HealthDetailsButtonRect(client_rc.right);
        DrawTextW(hdc, L"[詳細]", -1, &details_rc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (health_state_ == CandidateHealthState::DegradedModel) {
          RECT retry_rc = HealthRetryButtonRect(client_rc.right);
          SetTextColor(hdc, retry_in_flight_ ? theme_.sub_text : theme_.info_text);
          DrawTextW(hdc, L"[再試行]", -1, &retry_rc,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
      }
      if (old_font) SelectObject(hdc, old_font);
      EndPaint(hwnd, &ps);
      return 0;
    }

    case WM_LBUTTONDOWN: {
      int x = GET_X_LPARAM(lParam);
      int y = GET_Y_LPARAM(lParam);
      if (health_banner_visible_ && y >= HealthBannerTop()) {
        RECT client_rc{};
        GetClientRect(hwnd, &client_rc);
        POINT click{x, y};
        const RECT details_rc = HealthDetailsButtonRect(client_rc.right);
        if (PtInRect(&details_rc, click)) {
          ShowDetails();
        } else if (health_state_ == CandidateHealthState::DegradedModel && !retry_in_flight_) {
          const RECT retry_rc = HealthRetryButtonRect(client_rc.right);
          if (PtInRect(&retry_rc, click) && on_retry_) {
            retry_in_flight_ = true;
            Repaint();
            try {
              on_retry_();
            } catch (...) {
              // Never unwind through a Win32 window procedure.
              retry_in_flight_ = false;
              Repaint();
            }
          }
        }
        return 0;
      }
      if (secure_indicator_visible_ && y < metrics_.item_height) {
        RECT client_rc{};
        GetClientRect(hwnd, &client_rc);
        // The lock is not a candidate.
        if (x >= client_rc.right - SecureIndicatorWidth()) return 0;
      }
      int idx = y / metrics_.item_height;
      if (idx >= 0 && idx < static_cast<int>(items_.size())) {
        selected_idx_ = idx;
        Repaint();
        if (on_click_) on_click_(idx);
      }
      return 0;
    }

    case WM_SETTINGCHANGE:
    case WM_SYSCOLORCHANGE:
    case WM_THEMECHANGED:
      if (msg != WM_SETTINGCHANGE || IsThemeSettingChange(wParam, lParam)) {
        UpdateTheme();
        Repaint();
        if (details_hwnd_) InvalidateRect(details_hwnd_, nullptr, FALSE);
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

    case WM_DPICHANGED: {
      UpdateDpi(LOWORD(wParam));
      RECT* suggested = reinterpret_cast<RECT*>(lParam);
      if (suggested) {
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
      }
      InvalidateRect(hwnd, nullptr, TRUE);
      return 0;
    }

    case WM_DESTROY:
      HideDetails();
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
