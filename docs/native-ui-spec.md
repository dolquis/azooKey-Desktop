# Native UI 仕様（Phase 6-C）

本書は候補/予測ウィンドウ等のネイティブ UI をモダン化する仕様を定める。
`plans/windows-port-roadmap.md` の Phase 6 の M26 が本書を参照する。

## 1. Dark/Light テーマ自動追従

### 1.1 検出

```cpp
bool ShouldAppsUseDarkMode() {
    HKEY hkey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        0, KEY_READ, &hkey) != ERROR_SUCCESS) return false;
    DWORD val = 0, size = sizeof(val);
    RegGetValueW(hkey, nullptr, L"AppsUseLightTheme", RRF_RT_REG_DWORD,
                 nullptr, &val, &size);
    RegCloseKey(hkey);
    return val == 0;  // 0=Dark, 1=Light
}
```

### 1.2 変化の購読

```cpp
case WM_SETTINGCHANGE:
    if (lParam && _wcsicmp((LPCWSTR)lParam, L"ImmersiveColorSet") == 0) {
        OnThemeChanged(ShouldAppsUseDarkMode());
    }
    break;
```

`OnThemeChanged(bool dark)`：
- 色テーブルを切替
- 既存ウィンドウを再描画
- DComp Visual Tree の backdrop を切替

### 1.3 色テーブル

`tsf-tip/src/ThemeColors.h`（新規）：

```cpp
struct ThemeColors {
    COLORREF background;
    COLORREF selection;
    COLORREF text;
    COLORREF sub_text;
    COLORREF underline;
    COLORREF shadow;
};

constexpr ThemeColors kLightTheme = {
    /*background*/  RGB(255, 255, 255),
    /*selection*/   RGB(0,   120, 215),
    /*text*/        RGB(0,   0,   0),
    /*sub_text*/    RGB(96,  96,  96),
    /*underline*/   RGB(0,   0,   0),
    /*shadow*/      RGB(0,   0,   0),
};

constexpr ThemeColors kDarkTheme = {
    /*background*/  RGB(32,  32,  32),
    /*selection*/   RGB(76,  194, 255),
    /*text*/        RGB(255, 255, 255),
    /*sub_text*/    RGB(160, 160, 160),
    /*underline*/   RGB(255, 255, 255),
    /*shadow*/      RGB(0,   0,   0),
};
```

選択色（selection）は Windows 11 のアクセント色を尊重する場合、
`UISettings::GetColorValue(UIColorType::Accent)`（C++/WinRT）から取得。

## 2. DirectComposition + Mica/Acrylic

### 2.1 Mica 適用

```cpp
#include <dwmapi.h>
#pragma comment(lib, "Dwmapi.lib")

void EnableMica(HWND hwnd, bool dark) {
    BOOL use_dark = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE,
                          &use_dark, sizeof(use_dark));
    DWM_SYSTEMBACKDROP_TYPE backdrop = DWMSBT_MAINWINDOW; // Mica
    DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE,
                          &backdrop, sizeof(backdrop));
}
```

サポート OS:
- Windows 11 22H2 以降: Mica
- それ以前: アクリル `DwmEnableBlurBehindWindow` フォールバック
- Windows 10 LTSC: 単色背景

OS バージョン判定：`RtlGetVersion` (`ntdll.dll`)。
Windows 11 22H2 = build 22621。

### 2.2 DComp Visual Tree

```
root visual
 ├─ backdrop visual (Mica/Acrylic surface)
 └─ content visual
     ├─ selection highlight visual
     └─ text visual (DirectWrite render target)
```

実装：

```cpp
ComPtr<IDCompositionDevice> device;
DCompositionCreateDevice(nullptr, IID_PPV_ARGS(&device));

ComPtr<IDCompositionTarget> target;
device->CreateTargetForHwnd(hwnd, FALSE, &target);

ComPtr<IDCompositionVisual> root, backdrop, content;
device->CreateVisual(&root);
device->CreateVisual(&backdrop);
device->CreateVisual(&content);
root->AddVisual(backdrop.Get(), TRUE, nullptr);
root->AddVisual(content.Get(), FALSE, nullptr);

target->SetRoot(root.Get());
device->Commit();
```

## 3. DirectWrite 描画

### 3.1 ファクトリと TextFormat

```cpp
ComPtr<IDWriteFactory7> dwrite;
DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                    __uuidof(IDWriteFactory7),
                    reinterpret_cast<IUnknown**>(dwrite.GetAddressOf()));

ComPtr<IDWriteTextFormat> text_format;
dwrite->CreateTextFormat(
    L"Yu Gothic UI",              // フォントファミリ
    nullptr,                      // システムフォントコレクション
    DWRITE_FONT_WEIGHT_REGULAR,
    DWRITE_FONT_STYLE_NORMAL,
    DWRITE_FONT_STRETCH_NORMAL,
    14.0f * dpi / 96.0f,          // ポイントサイズ
    L"ja-JP",
    &text_format);
```

フォール優先順：
1. `"Yu Gothic UI"`（Windows 8.1+）
2. `"Meiryo UI"`
3. `"MS UI Gothic"`

### 3.2 TextLayout

```cpp
ComPtr<IDWriteTextLayout> layout;
dwrite->CreateTextLayout(
    text.c_str(), text.size(),
    text_format.Get(),
    max_width, max_height,
    &layout);

// 候補番号と本体で別のフォントウェイト
DWRITE_TEXT_RANGE number_range = { 0, 2 };  // "1. "
layout->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, number_range);
```

### 3.3 描画

```cpp
ComPtr<ID2D1RenderTarget> rt;       // DComp surface から取得
ComPtr<ID2D1SolidColorBrush> brush;
rt->CreateSolidColorBrush(D2D1::ColorF(theme_.text), &brush);

rt->BeginDraw();
rt->Clear(D2D1::ColorF(theme_.background, 0.0f));  // backdrop が透ける
rt->DrawTextLayout({ pad_x, pad_y }, layout.Get(), brush.Get());
rt->EndDraw();
```

### 3.4 絵文字（カラーフォント）

`IDWriteTextRenderer` のカスタム実装または、`ID2D1DeviceContext4::DrawText`
（カラー絵文字をフルカラーで描画）。

```cpp
ComPtr<ID2D1DeviceContext4> dc4;
rt.As(&dc4);
dc4->DrawText(text.c_str(), text.size(),
              text_format.Get(), &layout_rect, brush.Get(),
              D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
```

「Segoe UI Emoji」が候補に含まれるとき自動的にカラー絵文字レンダリング。

## 4. 適用範囲

### 4.1 CandidateWindow.cpp

既存 GDI 実装を以下に置換：

- WS_POPUP HWND は維持
- 描画レイヤを GDI → DComp + D2D + DirectWrite に置換
- ヒットテスト・キャレット追従ロジックは変更なし

### 4.2 PredictionWindow.cpp（M15 新規）

最初から DComp + D2D + DirectWrite で実装。

### 4.3 デバッグウィンドウ（M18-3）

Phase 6-C で同じ仕組みに統一。Phase 5 では GDI で実装してよい。

### 4.4 Magic Conversion プロンプト（M16）

Phase 5 では Win32 標準ダイアログ（IDD_*）で実装。
Phase 7-M30 で設定アプリと統合して WinUI 3 に移行。

## 5. アクセシビリティ

### 5.1 ハイコントラスト対応

```cpp
HIGHCONTRASTW hc{ sizeof(hc) };
SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0);
bool high_contrast = (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
```

`high_contrast == true` のとき：
- Mica 無効化（不透明背景）
- システムテーマ色を `GetSysColor` で取得
- フォントを `GetThemeSysFont(SPI_GETICONTITLELOGFONT)` から

### 5.2 UI Automation

候補ウィンドウは `WS_EX_NOACTIVATE` のため、UIA Provider は不要（フォーカスを
取らない）。ただし候補内容を読み上げソフトに通知するため、
`UiaRaiseAutomationEvent` で `UIA_AsyncContentLoadedEventId` を発火。

Phase 6-C 末尾で追加実装。

## 6. アニメーション

### 6.1 候補ウィンドウのフェードイン

```cpp
ComPtr<IDCompositionAnimation> opacity_anim;
device->CreateAnimation(&opacity_anim);
opacity_anim->AddCubic(0.0,  0.0f, 0.0f, 0.0f, 0.0f);
opacity_anim->AddCubic(0.15, 1.0f, 0.0f, 0.0f, 0.0f);
opacity_anim->End(0.15, 1.0f);
content_visual->SetOpacity(opacity_anim.Get());
device->Commit();
```

150ms でフェードイン。

### 6.2 候補移動のアニメーション

選択ハイライト矩形の移動を 100ms cubic easing。Visual の Offset を
`SetOffsetX/Y` で animate。

### 6.3 配慮

`SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, ...)` で「アニメーション無効」
設定をチェックし、無効なら即座に最終状態へ。

## 7. テスト

| テスト | 場所 | 内容 |
|---|---|---|
| Theme 切替 | `tsf-tip/tests/theme_test.cpp` | Windows 限定。WM_SETTINGCHANGE で色テーブル切替 |
| DPI scaling | `tsf-tip/tests/dpi_test.cpp` | 96/144/192 DPI でフォントサイズ計算 |
| High contrast | `tsf-tip/tests/high_contrast_test.cpp` | Windows 限定。HCF_HIGHCONTRASTON 検出 |
| 描画 smoke | `tsf-tip/tests/render_smoke_test.cpp` | Windows 限定。DComp + D2D + DirectWrite で 1 フレーム描画 |

CI では `windows-2022` ランナーで実行。アーティファクトとしてスクリーンショットを
`bench/` で出力（Phase 6-C 完了時の見栄え確認用）。

## 8. 参照

- DirectComposition: <https://learn.microsoft.com/windows/win32/directcomp/>
- DirectWrite: <https://learn.microsoft.com/windows/win32/directwrite/>
- DWMWA_SYSTEMBACKDROP_TYPE: <https://learn.microsoft.com/windows/win32/api/dwmapi/ne-dwmapi-dwm_systembackdrop_type>
- Mica の利用ガイド: <https://learn.microsoft.com/windows/apps/design/style/mica>
- 既存実装：`tsf-tip/src/CandidateWindow.cpp`
- PredictionWindow 仕様：`docs/legacy-parity-spec.md` §3
