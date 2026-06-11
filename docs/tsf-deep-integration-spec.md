# TSF Deep Integration 仕様（Phase 6-A）

本書は Microsoft IME（MS-IME）と同等の TSF 統合を実現するための仕様を定める。
`plans/windows-port-roadmap.md` の Phase 6 の M20〜M23 が本書を参照する。

## 1. ITfReconversion / ITfFnReconversion (M20)

### 1.1 目的

既に確定済みのテキストに対して、「再変換」操作を提供する。
ユーザーが「明日」を「あした」と再変換したい、等。

### 1.2 インターフェース実装スケルトン

`tsf-tip/src/ReconversionFunction.h` / `.cpp`（新規）：

```cpp
class ReconversionFunction : public IUnknown
                           , public ITfFnReconversion {
public:
    // ITfFunction
    STDMETHODIMP GetDisplayName(BSTR* pbstrName) override;

    // ITfFnReconversion
    STDMETHODIMP QueryRange(ITfRange* pRange,
                            ITfRange** ppNewRange,
                            BOOL* pfConvertable) override;
    STDMETHODIMP GetReconversion(ITfRange* pRange,
                                 ITfCandidateList** ppCandidateList) override;
    STDMETHODIMP Reconvert(ITfRange* pRange) override;

    // IUnknown (略)
};
```

#### QueryRange

- 入力 `pRange`: 選択範囲 or キャレット位置
- 動作: 範囲が空なら現在のキャレットを含む語/文節境界まで拡張
- 出力 `ppNewRange`: 再変換対象として拡張された範囲
- 出力 `pfConvertable`: 再変換可能なら TRUE

実装：

1. `pRange->GetText` でテキスト取得
2. 空なら `ShiftStart`/`ShiftEnd` で前後 8 文字まで拡張
3. テキストを漢字/かな単位で語境界に snap（簡易ヒューリスティック）
4. `pfConvertable = (テキストが日本語を含む) ? TRUE : FALSE`

#### GetReconversion

- 入力範囲のテキストを `InferenceEngine::ReverseConvert(surface) → reading` で
  読み（reading）を逆引き
- その reading で `QueryCandidates` を実行
- 結果を `ITfCandidateList` ラッパーで返却

新規 IPC：

```
ReverseConvertRequest:
  surface: string

ReverseConvertResponse:
  reading: string         // 最尤読み
  confidence: float
```

#### Reconvert

- `GetReconversion` の結果から先頭候補で即座に置換
- ユーザーが候補を選び直したい場合は別途 `ITfCandidateListUIElement` を介する

### 1.3 Category 登録

`tsf-tip/src/DllMain.cpp::DllRegisterServer` に追加：

```cpp
ITfCategoryMgr* mgr = ...;
mgr->RegisterCategory(kTextServiceClsid,
                      GUID_TFCAT_TIP_RECONVERSION,
                      kTextServiceClsid);
```

### 1.4 受け入れ条件

- メモ帳で「明日」を選択 → 右クリック「再変換」（or 変換キー押下）→ 「あした」
  「アシタ」「Ashita」等の候補が出る
- 候補選択で範囲が置換される

## 2. UI-less Mode (M21・基本契約は M5 で必須)

### 2.0 v1.0 (M5) でカバーする最小契約

UI-less mode は本文 §2.1〜§2.5 で扱う full な実装 (M21) と独立に、**基本契約
のみ v1.0 (M5 候補ウィンドウ) で要求される**。具体的には:

1. `ITfTextInputProcessorEx` を実装する（`ITfTextInputProcessor` だけだと UI-less
   mode スレッドで TIP が **activate されない**。Microsoft Learn [UILess Mode
   Overview](https://learn.microsoft.com/windows/win32/tsf/uiless-mode-overview#how-to-create-uilessmode)）
2. `ActivateEx` の `dwFlags` で `TF_TMF_UIELEMENTENABLEDONLY` を検出し
   `ui_less_mode_` を保持
3. `ITfUIElementMgr::BeginUIElement` の戻り値 `pbShown` を見て自前 HWND を出すか
   抑制するかを切り替え（§2.4・§2.6 参照）

これらを満たさないと、Win11 スタート検索 / Office 365 / Edge 等で TIP が活性化
されないアプリが出る。**M5 受け入れ条件の暗黙の前提**として扱い、M21 で拡張
（`ITfIntegratableCandidateListUIElement` 等）するが、最小契約は v1.0 で要求さ
れる。

### 2.1 目的（M21 拡張）

Windows 11 / Office アプリ等で OS 側が候補ウィンドウを描画するモード
（TF_TMF_UIELEMENTENABLEDONLY）に対応する。
TIP 自前の `CandidateWindow` を抑制し、`ITfCandidateListUIElement` 経由で
OS 側 Suggestion UI に候補を渡す。

### 2.2 ActivateEx の検出

```cpp
HRESULT TextService::ActivateEx(ITfThreadMgr* pThreadMgr,
                                TfClientId tid,
                                DWORD dwFlags) {
    ui_less_mode_ = (dwFlags & TF_TMF_UIELEMENTENABLEDONLY) != 0;
    // ...
}
```

### 2.3 CandidateListUIElement 実装

`tsf-tip/src/CandidateListUIElement.h` / `.cpp`（新規）：

```cpp
class CandidateListUIElement
    : public IUnknown
    , public ITfUIElement
    , public ITfCandidateListUIElement
    , public ITfCandidateListUIElementBehavior {
public:
    // ITfUIElement
    STDMETHODIMP GetDescription(BSTR*) override;
    STDMETHODIMP GetGUID(GUID*) override;
    STDMETHODIMP Show(BOOL) override;
    STDMETHODIMP IsShown(BOOL*) override;

    // ITfCandidateListUIElement
    STDMETHODIMP GetUpdatedFlags(DWORD*) override;
    STDMETHODIMP GetDocumentMgr(ITfDocumentMgr**) override;
    STDMETHODIMP GetCount(UINT*) override;
    STDMETHODIMP GetSelection(UINT*) override;
    STDMETHODIMP GetString(UINT index, BSTR*) override;
    STDMETHODIMP GetPageIndex(UINT* pIndex, UINT uSize, UINT* puPageCnt) override;
    STDMETHODIMP SetPageIndex(UINT*, UINT) override;
    STDMETHODIMP GetCurrentPage(UINT*) override;

    // ITfCandidateListUIElementBehavior
    STDMETHODIMP SetSelection(UINT) override;
    STDMETHODIMP Finalize() override;
    STDMETHODIMP Abort() override;

private:
    std::vector<std::wstring> candidates_;
    UINT                      selection_  = 0;
};
```

### 2.4 動作分岐

- `ui_less_mode_ == false`: 既存の自前 `CandidateWindow::Show` を呼ぶ
- `ui_less_mode_ == true`: `BeginUIElement(elem, &pbShown)` を呼び、OS に通知

UI 要素 ID は ITfUIElementMgr に格納。`Show(true)` 時に OS が `GetString` 等を
ポーリングして自前 UI を描画する。

### 2.5 受け入れ条件

- Windows 11 / Office (TF_TMF_UIELEMENTENABLEDONLY) で TIP の自前ウィンドウが
  出ず、Windows 11 標準の候補 UI に候補が表示される
- それ以外のアプリでは従来通り `CandidateWindow` が出る

### 2.6 `pbShown` の per-call 切替

`ITfUIElementMgr::BeginUIElement(IUnknown* pElement, BOOL* pbShown, DWORD* pdwUIElementId)`
は呼び出しごとに `pbShown` の値が変わり得る。アプリ側が `ITfUIElementSink::
BeginUIElement` で UI 表示可否を選択するため、TIP は **per-call** で:

| `pbShown` 戻り値 | TIP 側挙動 |
|---|---|
| `TRUE`（アプリが描画する） | 自前 HWND を出さない。`ITfCandidateListUIElement::GetString` 等の問い合わせに応答 |
| `FALSE`（アプリが拒否、TIP が描画） | `UpdateUIElement` で OS にも更新通知しつつ、自前 HWND を表示 |

を切り替える。`UpdateUIElement` は `pbShown == FALSE` の場合のみ呼ぶ（[フローチャート](https://learn.microsoft.com/windows/win32/tsf/uiless-mode-overview#the-flow-chart-of-uilessmode)）。

`EndUIElement` は `pbShown` の値にかかわらず呼ぶ（UI 要素のライフサイクル管理用）。

### 2.7 `ITfIntegratableCandidateListUIElement`（Win11 Search 統合・任意）

検索ボックスや軽量入力フィールド（Windows 8 Search box、Windows 11 Start メニュー
の検索など）に統合された IME 体験を提供したい場合、`ITfIntegratableCandidateList
UIElement`（[ctffunc.h](https://learn.microsoft.com/windows/win32/api/ctffunc/nn-ctffunc-itfintegratablecandidatelistuielement)）を上記 3 つの interface と同じ
クラスに実装する。

これを実装し、かつ `ITfFnSearchCandidateProvider` も実装すると IME 検索統合
（[IME search integration](https://learn.microsoft.com/windows/apps/develop/input/input-method-editor-requirements#ime-search-integration)）の要件を満たす。

実装する場合は次の追加メソッド:

| メソッド | 用途 |
|---|---|
| `FinalizeExactCompositionString` | 現在の composition を「表示中の値」で確定 |
| `GetSelectionStyle` | 選択スタイル取得 |
| `OnKeyDown` | キー押下処理（搭載アプリ側から呼ばれる） |
| `SetIntegrationStyle` | 統合スタイル設定 |
| `ShowCandidateNumbers` | 候補番号の表示制御 |

任意であり、v1.0 では実装しない方針。M21 着手時に検索統合スコープ判断を行う。

## 3. 半角全角・無変換・変換・Caps (M22)

### 3.1 VK 対応表（MS-IME 互換）

| VK | キー名 | 動作 |
|---|---|---|
| `VK_KANJI` (0x19) | 半角/全角 | IME On/Off トグル |
| `VK_OEM_AUTO` (0xF3) | 同上（一部 HW） | 同上 |
| `VK_NONCONVERT` (0x1D) | 無変換 | 確定済み or 選択を平仮名/カタカナ/英字 と巡回変換 |
| `VK_CONVERT` (0x1C) | 変換 | 確定済み or 選択を再変換 (M20 と同経路) |
| `VK_OEM_ATTN` (0xF0) | Caps (英語キーボード) | alphanumeric モードトグル |
| `VK_DBE_HIRAGANA` (0xF2) | ひらがな | input mode = hiragana |
| `VK_DBE_KATAKANA` (0xF1) | カタカナ | input mode = katakana |
| `VK_DBE_ALPHANUMERIC` (0xF0) | 英数 | input mode = alphanumeric |

### 3.2 無変換キーの巡回

選択範囲がある場合：

1. 平仮名 → カタカナ
2. カタカナ → 半角カタカナ
3. 半角カタカナ → 全角英数
4. 全角英数 → 半角英数
5. 半角英数 → 平仮名（に戻る）

確定済み直近 commit が対象範囲（範囲がない場合）：

- 直近 commit から逆引きで `reading` を取り、同じ巡回を適用

### 3.3 変換キー

- composition 中: `StartConversion`（候補ウィンドウ表示）
- composition なし、選択あり: M20 `ITfFnReconversion::Reconvert` を呼ぶ

### 3.4 受け入れ条件

- メモ帳で半角/全角キー → IME のオン/オフがトグル
- 「あした」を選択して無変換キー → 「アシタ」→「ｱｼﾀ」→「ＡＳＨＩＴＡ」→「ASHITA」と巡回
- 「あした」を選択して変換キー → 「明日」候補が出る

## 4. IME On/Off 状態管理 (M21 と統合)

### 4.1 ITfKeyboardOpenCloseCompartment 購読

```cpp
// Activate 時
ITfCompartmentMgr* compMgr = nullptr;
pThreadMgr->QueryInterface(IID_PPV_ARGS(&compMgr));
ITfCompartment* comp = nullptr;
compMgr->GetCompartment(GUID_COMPARTMENT_KEYBOARD_OPENCLOSE, &comp);

ITfSource* src = nullptr;
comp->QueryInterface(IID_PPV_ARGS(&src));
src->AdviseSink(IID_ITfCompartmentEventSink, this, &keyboard_open_cookie_);
```

`ITfCompartmentEventSink::OnChange(REFGUID rguid)` で IME On/Off を検知。
`rguid == GUID_COMPARTMENT_KEYBOARD_OPENCLOSE` のとき値を読み出す。

### 4.2 状態遷移

| 旧 | 新 | 動作 |
|---|---|---|
| Off | On | composition / candidate なし、入力モードを `hiragana` に初期化 |
| On | Off | composition があれば確定 (auto-commit) し、内部状態をクリア |

### 4.3 Win+Space / アプリ切替時

- Win+Space (言語切替) は OS が処理 → `Deactivate` が呼ばれる
- アプリ切替 (`OnSetFocus`) では現在の composition を **確定** してから前面アプリへ
  フォーカスを譲る（MS-IME 互換）

### 4.4 受け入れ条件

- 半角/全角キーで IME On/Off が切り替わり、Status を反映
- Win+Space で別言語に切替時、composition が確定される
- アプリ切替時に composition が確定される

## 5. 複合 DisplayAttribute (M23)

### 5.1 文節ごとの色分け

変換中の preedit を、文節（segment）ごとに異なる属性で表示：

| 属性 | 用途 | 色 |
|---|---|---|
| Focused | 注目文節（カーソルがある文節） | 青背景 |
| Converted | 変換済み文節 | 黒 + 下線 |
| Unconverted | 未変換文節 | グレー + 点線下線 |

### 5.2 GUID 定義

`tsf-tip/src/DllMain.cpp` に追加：

```cpp
constexpr GUID kFocusedSegmentGuid     = { 0xBBC0..., ... };
constexpr GUID kConvertedSegmentGuid   = { 0xBBC1..., ... };
constexpr GUID kUnconvertedSegmentGuid = { 0xBBC2..., ... };
```

`EnumDisplayAttributeInfo` で 3 新規エントリを返す。

### 5.3 Property 設定

EditSession 内で、文節ごとに `ITfRange` を分割：

```cpp
for (const auto& seg : segments) {
    ITfRange* segRange = ...; // composition range を seg.start..seg.end でクローン
    VARIANT v;
    v.vt   = VT_I4;
    v.lVal = display_attr_provider_->GuidToAtom(
        seg.is_focused ? kFocusedSegmentGuid :
        seg.is_converted ? kConvertedSegmentGuid : kUnconvertedSegmentGuid);
    attr_prop->SetValue(write_cookie, segRange, &v);
}
```

`GuidToAtom` は `ITfCategoryMgr::RegisterGUID` 経由で取得するアトム。
キャッシュは TextService インスタンス内に保持。

### 5.4 文節境界

Phase 5 では「変換候補 1 件 = 1 文節」として扱う。
Phase 6 で Zenzai が複数文節を返すようになったら、`Candidate.segments[]` を活用。

### 5.5 受け入れ条件

- 「nihongo」入力 → Space で候補表示 → カーソル位置の文節が青背景
- 矢印キー（Left/Right）で文節を移動できる（カーソル位置によって色が動く）

## 6. ITfFnConfigure (M30 連動)

### 6.1 目的

Windows 設定の「言語と地域 → IME → オプション → 詳細設定」から
設定アプリを起動できるようにする。

### 6.2 実装

`tsf-tip/src/ConfigureFunction.h` / `.cpp`（新規）：

```cpp
class ConfigureFunction : public IUnknown, public ITfFnConfigure {
public:
    STDMETHODIMP GetDisplayName(BSTR* pbstrName) override {
        *pbstrName = SysAllocString(L"azooKey 設定");
        return S_OK;
    }
    STDMETHODIMP Show(HWND hwndParent, LANGID langid, REFGUID rguidProfile) override {
        // 設定アプリ EXE を起動
        SHELLEXECUTEINFOW sei{ sizeof(sei) };
        sei.lpFile  = L"azookey_settings.exe";
        sei.hwnd    = hwndParent;
        sei.nShow   = SW_SHOW;
        sei.fMask   = SEE_MASK_NOCLOSEPROCESS;
        ShellExecuteExW(&sei);
        return S_OK;
    }
};
```

### 6.3 Category 登録

```cpp
mgr->RegisterCategory(kTextServiceClsid,
                      GUID_TFCAT_TIP_PROPERTY_UI_TEXT_SERVICE,
                      kTextServiceClsid);
```

### 6.4 設定アプリのパス解決

`tsf-tip/src/InstalledPath.cpp`（新規）：

```cpp
std::wstring GetInstalledExePath(const wchar_t* exe_name) {
    // 1) TIP DLL と同じディレクトリを優先
    HMODULE hm = GetModuleHandleW(L"azookey_tsf_tip.dll");
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(hm, buf, MAX_PATH);
    PathRemoveFileSpecW(buf);
    std::wstring p = std::wstring(buf) + L"\\" + exe_name;
    if (PathFileExistsW(p.c_str())) return p;
    // 2) %LOCALAPPDATA%\azooKey\
    // 3) PATH
    return std::wstring(exe_name);
}
```

## 7. ITfMouseSink (M23)

### 7.1 目的

Preedit テキスト上でマウスクリックされた箇所を「注目文節」に切替える。

### 7.2 実装

`TextService::OnCompositionStart` で `ITfRange::AdviseSink(IID_ITfMouseSink, this, ...)` を呼ぶ。

```cpp
STDMETHODIMP TextService::OnMouseEvent(ULONG uEdge,
                                        ULONG uQuadrant,
                                        DWORD dwBtnStatus,
                                        BOOL* pfEaten) {
    if (dwBtnStatus & MK_LBUTTON) {
        // uEdge: クリック位置に最も近い range 境界
        size_t segment_index = FindSegmentByEdge(uEdge);
        SetFocusedSegment(segment_index);
        *pfEaten = TRUE;
    }
    return S_OK;
}
```

### 7.3 受け入れ条件

- 変換中 preedit の途中をマウスでクリック → 該当文節が注目に切り替わる
- 候補ウィンドウが該当文節の候補を表示し直す

## 8. TSF Suggestion UIElement（Windows 11 標準 Suggestion UI 統合）

UI-less Mode (M21) と同じ仕組みで、Windows 11 OS の Suggestion UI に
予測候補を渡す。

### 8.1 実装

`tsf-tip/src/PredictionListUIElement.h` / `.cpp`（新規）：

```cpp
class PredictionListUIElement
    : public IUnknown
    , public ITfUIElement
    , public ITfReadingInformationUIElement {
public:
    STDMETHODIMP GetReadingInformation(...) override;
    // ...
};
```

Phase 6-A 末尾で、`ui_less_mode_ == true` のときは予測候補も自前ウィンドウ
ではなく OS 側 Suggestion UI に渡す。

### 8.2 受け入れ条件

- Windows 11 / Office で予測候補が OS 標準の Suggestion UI に表示される
- それ以外のアプリでは自前 `PredictionWindow` が出る

## 9. テスト

| テスト | 場所 | 内容 |
|---|---|---|
| ReconversionFunction | `tsf-tip/tests/reconversion_test.cpp` | Windows 限定。QueryRange / GetReconversion / Reconvert |
| Configure 起動 | `tsf-tip/tests/configure_test.cpp` | Windows 限定。Show が EXE を起動するか |
| KeyMap 互換 | `tsf-tip/tests/keymap_msime_compat_test.cpp` | Windows 限定。VK_NONCONVERT 等の挙動 |
| CandidateListUIElement | `tsf-tip/tests/ui_element_test.cpp` | Windows 限定。GetString/GetCount/SetSelection |
| Segment DisplayAttribute | `tsf-tip/tests/segment_attr_test.cpp` | Windows 限定。3 文節の attribute 設定 |

## 10. 参照

- TSF SDK: `Microsoft.Windows.SDK.Cpp` の `msctf.h`
- MS-IME 互換仕様（公式ドキュメント）: <https://learn.microsoft.com/windows/win32/tsf/>
- 旧 macOS 実装の対応箇所：`legacy/azooKeyMac/InputController/azooKeyMacInputController.swift`
- Phase 6-B: `docs/copilot-pc-backend-spec.md`
- Phase 6-C: `docs/native-ui-spec.md`
