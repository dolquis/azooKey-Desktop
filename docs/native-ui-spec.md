# Native UI 仕様（Phase 6-C）

本書は候補/予測ウィンドウ等のネイティブ UI をモダン化する仕様を定める。
`plans/windows-port-roadmap.md` の Phase 6 の M26 が本書を参照する。

## 1. Dark/Light テーマ自動追従

### 1.1 検出

テーマの判定は `tsf-tip/include/azookey/tsf/ThemeColors.h` の `ThemeMode`（`Light` / `Dark` /
`HighContrast`）で表す。
`CurrentThemeMode()` は次の順で決める。

1. `SystemParametersInfoW(SPI_GETHIGHCONTRAST)` で `HCF_HIGHCONTRASTON` が立っていれば `HighContrast`
2. HKCU `Software\Microsoft\Windows\CurrentVersion\Themes\Personalize` の
   `AppsUseLightTheme` が 0 なら `Dark`
3. 値が 1、または読めなければ `Light`

判定規則そのものは `ThemeModeFrom(high_contrast, apps_use_light_theme)` に分け、
レジストリと SPI を読まずにテストする。
C++/WinRT の `UISettings` は TIP DLL に WinRT 依存を持ち込むため使わない。

### 1.2 変化の購読

候補ウィンドウと予測候補ウィンドウは owner を持たない top-level の `WS_POPUP` なので、
`WM_SETTINGCHANGE` の broadcast を直接受け取る。
次のいずれかでテーマを読み直し、再描画する。

- `WM_SETTINGCHANGE` で wParam が 0 かつ lParam が `"ImmersiveColorSet"`
- `WM_SETTINGCHANGE` で wParam が `SPI_SETHIGHCONTRAST`
- `WM_SYSCOLORCHANGE`、`WM_THEMECHANGED`

`WM_SETTINGCHANGE` の判定は `IsThemeSettingChange(wParam, lParam)` が持つ。
TIP は他プロセスに読み込まれ、broadcast の lParam を検証できないため、lParam は wParam が 0 のときだけ
文字列として読み、`"ImmersiveColorSet"` の長さを超えて読まない。

テーマを読み直すときは、色テーブルを差し替え、`DwmSetWindowAttribute` の
`DWMWA_USE_IMMERSIVE_DARK_MODE`（属性値 20）で枠の明暗も合わせる。
属性値 20 が失敗したときは、Windows 10 1809〜1909 の値 19 を試す。
どちらも持たない OS では呼び出しが失敗するだけで、表示には影響しない。

### 1.3 色テーブル

`ThemeColors` は両ウィンドウが描く要素ごとの色を持つ。

| フィールド | 用途 | `kLightTheme` | `kDarkTheme` | ハイコントラスト |
|---|---|---|---|---|
| `background` | 背景 | RGB(255,255,255) | RGB(32,32,32) | `COLOR_WINDOW` |
| `text` | 候補 | RGB(0,0,0) | RGB(255,255,255) | `COLOR_WINDOWTEXT` |
| `sub_text` | 説明、無効なボタン | RGB(96,96,96) | RGB(160,160,160) | `COLOR_GRAYTEXT` |
| `selection` | 選択行の背景 | RGB(0,120,215) | RGB(76,194,255) | `COLOR_HIGHLIGHT` |
| `selection_text` | 選択行の文字 | RGB(255,255,255) | RGB(0,0,0) | `COLOR_HIGHLIGHTTEXT` |
| `border` | 予測候補ウィンドウの枠 | RGB(160,160,160) | RGB(80,80,80) | `COLOR_WINDOWTEXT` |
| `panel_background` / `panel_text` | 案内行 | RGB(243,243,243) / 黒 | RGB(45,45,45) / 白 | `COLOR_BTNFACE` / `COLOR_BTNTEXT` |
| `info_background` / `info_text` | secure toast、劣化の詳細 | RGB(255,255,225) / 黒 | RGB(56,56,40) / 白 | `COLOR_INFOBK` / `COLOR_INFOTEXT` |
| `banner_background` | 劣化バナー（文字は `info_text`） | RGB(255,249,225) | RGB(67,53,25) | `COLOR_INFOBK` |

`ResolveThemeColors(mode)` は Light / Dark では固定表を返し、ハイコントラストでは
`GetSysColor` から組み立てる（§5.1）。
選択色に Windows のアクセント色は使わない（`UISettings` を使わないため）。

## 2. 背景と DirectComposition

### 2.1 背景効果

候補ウィンドウと予測候補ウィンドウの背景は、テーマ色の不透明な塗りとする。
Mica / Acrylic などのシステム背景効果は使わない。

- Mica（`DWMSBT_MAINWINDOW`）はアクティブなウィンドウ向けで、非アクティブなウィンドウでは
  単色になる。両ウィンドウは `WS_EX_NOACTIVATE` で常に非アクティブなので、効果が出ない。
- Acrylic（`DWMSBT_TRANSIENTWINDOW`）は popup 向けで非アクティブでも描かれるが、
  `DwmExtendFrameIntoClientArea` と透明なクリアが要る。
  GDI で描く候補ウィンドウは透明なクリアができないため、候補ウィンドウを DirectComposition に
  移すまで、両ウィンドウとも背景効果を付けない（統一を優先する）。
- `DwmEnableBlurBehindWindow` は Windows 8 以降では効果がなく、Acrylic の代わりにならない。

Acrylic の採用は、候補ウィンドウの DirectComposition 移行と同時に判断する（§4.1）。
採用するときは build 22621 未満で不透明のテーマ色へ落とす。

### 2.2 RenderingEngine

`tsf-tip/src/RenderingEngine.cpp` の `RenderingEngine` が、1 つの popup HWND に対する
D3D11 + Direct2D + DirectComposition + DirectWrite の組を持つ。
ウィンドウは `WS_EX_NOREDIRECTIONBITMAP` で作る。

| メソッド | 役割 |
|---|---|
| `Initialize(hwnd)` | D3D11 device（hardware、失敗時は WARP）、D2D factory / device、`IDCompositionDesktopDevice`、topmost の composition target、root visual、DWrite factory を作る |
| `ResizeSurface(width, height)` | 物理 px の `IDCompositionSurface` を作り直して visual に付ける（同じ大きさなら何もしない） |
| `BeginDraw()` | surface 全体の描画を始め、96 DPI・surface 原点基準の `ID2D1DeviceContext` を返す |
| `EndDraw()` | 描画を終えて composition を commit する |
| `failure_stage()` / `failure_hr()` | 直近の失敗段階と HRESULT（描画内容は含まない） |

描画中に `DXGI_ERROR_DEVICE_REMOVED`、`DXGI_ERROR_DEVICE_RESET`、`D2DERR_RECREATE_TARGET` を受けたら
デバイスの組を捨て、次の表示で `Initialize` からやり直す。

`DCompositionCreateDevice2` は `IDCompositionDevice2` を直接返さないため、
`IDCompositionDesktopDevice` で作ってから `IDCompositionDevice2` を QI する。
visual tree は root visual に surface を 1 枚載せるだけとし、背景用の visual は持たない（§2.1）。

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
// COLORREF は 0x00BBGGRR なので、D2D1::ColorF(UINT32) へ直接渡さず変換する
rt->CreateSolidColorBrush(ToColorF(theme_.text), &brush);

rt->BeginDraw();
rt->Clear(ToColorF(theme_.background));  // 不透明（§2.1）
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

GDI で描き、色は §1.3 の `ThemeColors` を使う。
`WM_ERASEBKGND` は何もせず、`WM_PAINT` がクライアント領域全体をテーマの背景で塗る。
DPI は `docs/copilot-pc-backend-spec.md` §7 の規則（`tsf-tip/include/azookey/tsf/DpiScaling.h`）に従う。

描画レイヤを GDI から `RenderingEngine` へ移すことは後続とする。
移すときも `WS_POPUP` の HWND、ヒットテスト、キャレット追従の配置計算
（`docs/legacy-parity-spec.md` §9.2）は変えない。
カラー絵文字は、移行後は `D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT` で描く（§3.4）。

### 4.2 PredictionWindow.cpp（M15 新規）

`RenderingEngine` で描き、色は §1.3 の `ThemeColors` を使う。
`WM_DPICHANGED` で行高・余白・フォントを作り直し、表示中なら最後のキャレット矩形で表示をやり直す
（測り直し、作業領域内への配置、再描画）。
`WM_DPICHANGED` の推奨矩形は使わない。
`Show` 自身の `SetWindowPos` が別 DPI のモニタへ動かして発生した通知は無視する（`Show` が移動先の DPI で測り終えているため）。
DPI の変化で表示をやり直すときのキャレット矩形は、直前の `Show` の値である。
ウィンドウの移動でキャレットの位置も変わっていた場合は、次の preedit 更新による `Show` で正しい位置へ戻る。
テーマの変更通知では、テーマを読み直したときだけ再描画し、描画に失敗したらウィンドウを隠す。

### 4.3 デバッグウィンドウ（M18-3）

M18-3 では GDI で実装してよい。
`RenderingEngine` と `ThemeColors` へ載せるのは、候補ウィンドウの移行（§4.1）と同じ後続で扱う。

### 4.4 Magic Conversion プロンプト（M16）

Phase 5 では Win32 標準ダイアログ（IDD_*）で実装。
Phase 7-M30 で設定アプリと統合して WinUI 3 に移行。

### 4.5 設定アプリの API キー入力（M34）

設定アプリの OpenAI API キーは `PasswordBox` で伏字表示する。読み込み時は
`dpapi:` 値を現在の Windows ユーザーで復号し、復号できない値は表示せず警告する。
入力を変更して保存すると DPAPI で保護してから `settings.json` に書く。
削除ボタンは保存時に空文字を書き、その他の設定だけを保存した場合は既存の
暗号化値を保持する。旧形式の平文値は保存時に保護値へ移行する。

### 4.6 設定アプリの辞書ライセンス導線（M53）

設定アプリの「バージョン」ペインにある「ライセンス」欄は、同梱辞書のライセンスと帰属を表示する文書を開く。
参照先は設定アプリ実行ファイルと同じディレクトリの `ThirdPartyNotices.txt` とし、
現在の作業ディレクトリには依存しない。文書の欠落や起動の失敗は InfoBar で知らせる。
辞書ソースの配布判定と文書内容の正典は `docs/auto-word-registration-spec.md` §14.9 / §14.10、
配布先の正典は `docs/sideload-packaging-spec.md` §4.1 とする。

別配布の `neologd_lexicon` pack の上流ライセンスと帰属は `ThirdPartyNotices.txt` に含めず、
「辞書」ペインで pack を有効にするときに表示し、同意を得る。表示内容と同意の扱いの正典は
`docs/auto-word-registration-spec.md` §15.14「設定アプリでの提示と同意」とする。

## 5. アクセシビリティ

### 5.1 ハイコントラスト対応

```cpp
HIGHCONTRASTW hc{ sizeof(hc) };
SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0);
bool high_contrast = (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
```

`high_contrast == true` のとき：
- 背景効果を使わない（不透明背景。§2.1 により常に不透明）
- システムテーマ色を `GetSysColor` で取得（§1.3 のハイコントラスト列）
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
| テーマ判定と色テーブル | `tsf-tip/tests/theme_colors_test.cpp` | Windows 限定。ハイコントラスト優先と `AppsUseLightTheme` の解釈、Light / Dark の固定表、ハイコントラストのシステム色、`ImmersiveColorSet` の判定 |
| DPI scaling | `tsf-tip/tests/theme_colors_test.cpp`、`tsf-tip/tests/candidate_window_dpi_test.cpp` | 96/144/192 DPI での換算、PMv2 の一時切替と復元、候補ウィンドウのレイアウト metrics |
| 描画 smoke | `tsf-tip/tests/theme_colors_test.cpp`、`tsf-tip/tests/prediction_window_test.cpp` | Windows 限定。`RenderingEngine` で 1 フレーム描画して commit する。予測候補ウィンドウの作成と表示 |

実描画の見た目（Dark / Light の切替、DPI 切替、ハイコントラスト）は実機で確認する。

CI では `windows-2022` ランナーで実行。アーティファクトとしてスクリーンショットを
`bench/` で出力（Phase 6-C 完了時の見栄え確認用）。

## 8. 参照

- DirectComposition: <https://learn.microsoft.com/windows/win32/directcomp/>
- DirectWrite: <https://learn.microsoft.com/windows/win32/directwrite/>
- DWMWA_SYSTEMBACKDROP_TYPE: <https://learn.microsoft.com/windows/win32/api/dwmapi/ne-dwmapi-dwm_systembackdrop_type>
- Mica の利用ガイド: <https://learn.microsoft.com/windows/apps/design/style/mica>
- 既存実装：`tsf-tip/src/CandidateWindow.cpp`
- PredictionWindow 仕様：`docs/legacy-parity-spec.md` §3
