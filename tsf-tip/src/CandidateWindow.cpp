#include "azookey/tsf/CandidateWindow.h"

#include <ShellScalingApi.h>
#include <dwrite_2.h>
#include <icu.h>
#include <wrl.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <map>
#include <string>

#include "CandidateSelection.h"

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
constexpr wchar_t kFallbackFontFace[] = L"Yu Gothic UI";

HMODULE GetTipModuleHandle() {
  HMODULE module = nullptr;
  if (GetModuleHandleExW(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCWSTR>(&GetTipModuleHandle), &module)) {
    return module;
  }
  return nullptr;
}

UINT NormalizeDpi(UINT dpi) { return dpi == 0 ? USER_DEFAULT_SCREEN_DPI : dpi; }

class ScopedThreadDpiAwarenessContext {
 public:
  explicit ScopedThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT context)
      : previous_context_(SetThreadDpiAwarenessContext(context)) {}

  ~ScopedThreadDpiAwarenessContext() {
    if (previous_context_) {
      SetThreadDpiAwarenessContext(previous_context_);
    }
  }

  ScopedThreadDpiAwarenessContext(const ScopedThreadDpiAwarenessContext&) = delete;
  ScopedThreadDpiAwarenessContext& operator=(const ScopedThreadDpiAwarenessContext&) = delete;

 private:
  DPI_AWARENESS_CONTEXT previous_context_;
};
}  // namespace

CandidateWindow::CandidateWindow() = default;

CandidateWindow::~CandidateWindow() { Destroy(); }

// static
int CandidateWindow::ScaleForDpi(int value, UINT dpi) {
  return MulDiv(value, static_cast<int>(NormalizeDpi(dpi)), static_cast<int>(kDefaultDpi));
}

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
    const ScopedThreadDpiAwarenessContext dpi_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClassName,
                            nullptr, WS_POPUP | WS_BORDER, 0, 0, 200, metrics_.item_height, nullptr,
                            nullptr, GetTipModuleHandle(), this);
  }
  if (hwnd_) {
    UpdateDpi(GetDpiForWindow(hwnd_));
  }
  return hwnd_ != nullptr;
}

void CandidateWindow::Destroy() {
  if (hwnd_) {
    DestroyWindow(hwnd_);
    // hwnd_ is cleared in WM_DESTROY handler.
  }
  if (font_) {
    DeleteObject(font_);
    font_ = nullptr;
  }
}

void CandidateWindow::Show(POINT pt, const std::vector<CandidateViewItem>& items,
                           int selected_idx) {
  if (!hwnd_ || items.empty()) return;

  const ScopedThreadDpiAwarenessContext dpi_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  items_ = items;
  if (!emoji_cache_) emoji_cache_ = std::make_unique<EmojiDrawingCache>();
  emoji_cache_->layouts.clear();
  selected_idx_ = std::clamp(selected_idx, 0, static_cast<int>(items_.size()) - 1);
  HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
  UpdateDpi(DpiForMonitor(mon, hwnd_));

  // Measure maximum text width using the window's DC.
  HDC hdc = GetDC(hwnd_);
  HGDIOBJ old_font = nullptr;
  if (hdc && font_) old_font = SelectObject(hdc, font_);
  int max_surface_w = metrics_.min_text_width;
  int max_description_w = 0;
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
  }
  if (hdc) {
    if (old_font) SelectObject(hdc, old_font);
    ReleaseDC(hwnd_, hdc);
  }

  const ColumnLayout columns = ComputeColumnLayout(max_surface_w, max_description_w, dpi_);
  surface_column_width_ = columns.surface_width;
  int width =
      std::min(columns.content_width + metrics_.horizontal_padding * 2 + metrics_.extra_width,
               metrics_.max_width);
  int height = metrics_.item_height * static_cast<int>(items_.size());

  // Keep window on-screen: flip above caret if it would overflow below.
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  GetMonitorInfoW(mon, &mi);
  if (pt.x + width > mi.rcWork.right) pt.x = mi.rcWork.right - width;
  if (pt.x < mi.rcWork.left) pt.x = mi.rcWork.left;
  if (pt.y + height > mi.rcWork.bottom) {
    // Estimate caret height and flip to open upward when it would overflow.
    pt.y = pt.y - height - metrics_.caret_gap;
  }
  if (pt.y < mi.rcWork.top) pt.y = mi.rcWork.top;

  SetWindowPos(hwnd_, HWND_TOPMOST, pt.x, pt.y, width, height, SWP_SHOWWINDOW | SWP_NOACTIVATE);
  InvalidateRect(hwnd_, nullptr, TRUE);
}

void CandidateWindow::Hide() {
  if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
}

void CandidateWindow::PostCandidatesReady() {
  if (hwnd_) PostMessageW(hwnd_, kCandidatesReadyMessage, 0, 0);
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

      for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        RECT row_rc = {0, i * metrics_.item_height, client_rc.right,
                       (i + 1) * metrics_.item_height};
        if (i == selected_idx_) {
          FillRect(hdc, &row_rc,
                   reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_HIGHLIGHT + 1)));
          SetTextColor(hdc, GetSysColor(COLOR_HIGHLIGHTTEXT));
        } else {
          FillRect(hdc, &row_rc, reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_WINDOW + 1)));
          SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));
        }
        const auto& item = items_[static_cast<size_t>(i)];
        std::wstring label = std::to_wstring(i + 1) + L". " + item.surface;
        RECT surface_rc = row_rc;
        surface_rc.left += metrics_.horizontal_padding;
        surface_rc.right = surface_column_width_ > 0
                               ? std::min(surface_rc.right - metrics_.horizontal_padding,
                                          surface_rc.left + surface_column_width_)
                               : surface_rc.right - metrics_.horizontal_padding;
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
          if (i != selected_idx_) SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
          RECT description_rc = row_rc;
          description_rc.left = surface_rc.right + ScaleForDpi(kBaseColumnGap, dpi_);
          description_rc.right -= metrics_.horizontal_padding;
          if (description_rc.left < description_rc.right) {
            DrawTextW(hdc, item.description.c_str(), static_cast<int>(item.description.size()),
                      &description_rc,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
          }
        }
      }
      if (old_font) SelectObject(hdc, old_font);
      EndPaint(hwnd, &ps);
      return 0;
    }

    case WM_LBUTTONDOWN: {
      int y = static_cast<int>(HIWORD(lParam));
      int idx = y / metrics_.item_height;
      if (idx >= 0 && idx < static_cast<int>(items_.size())) {
        selected_idx_ = idx;
        Repaint();
        if (on_click_) on_click_(idx);
      }
      return 0;
    }

    case kCandidatesReadyMessage:
      if (on_candidates_ready_) on_candidates_ready_(on_candidates_ready_context_);
      return 0;

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
