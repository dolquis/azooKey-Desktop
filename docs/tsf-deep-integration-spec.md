# TSF Deep Integration 仕様（Phase 6-A）

本書は Microsoft IME（MS-IME）と同等の TSF 統合を実現するための仕様を定める。
`plans/windows-port-roadmap.md` の Phase 6 の M20〜M23 が本書を参照する。

## 1. ITfReconversion / ITfFnReconversion (M20)

### 1.1 目的

既に確定済みのテキストに対して、「再変換」操作を提供する。
ユーザーが「明日」を「あした」と再変換したい、等。

> **参考（fkunn1326/azooKey-Windows, MIT）**: 周辺テキスト取得には、transitory context
> （検索ボックス等）の親コンテキストを `GUID_COMPARTMENT_TRANSITORYEXTENSION_PARENT` で
> 辿る手法が有効（先行実装の `surrounded_text` が mozc `tip_surrounding_text.cc` を参照して
> 実装）。再変換時の周辺文字列取得や、Zenzai への前方文脈供給（`SetContext`）に流用できる。
> 原典は mozc（C++）であり、実装時は原典に当たること。

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
2. `ITfThreadMgrEx::GetActiveFlags` で `TF_TMF_UIELEMENTENABLEDONLY` を検出し
   `ui_less_mode_` を保持（フラグ系統の正確な扱いは §2.2・§2.10）
3. `ITfUIElementMgr::BeginUIElement` の戻り値 `pbShown` を見て自前 HWND を出すか
   抑制するかを切り替え（§2.4・§2.6 参照）

これらを満たさないと、Win11 スタート検索 / Office 365 / Edge 等で TIP が活性化
されないアプリが出る。**M5 受け入れ条件の暗黙の前提**として扱い、M21 で拡張
（`ITfIntegratableCandidateListUIElement` 等）するが、最小契約は v1.0 で要求さ
れる。

ただし最小契約が満たすのは **UI-less ホストでの「活性化 + 基本 UI-less 描画」まで**
である。Win11 スタート検索の**統合インライン検索**体験（候補が検索ボックスに統合
表示される）は、UI-less mode に加えて検索統合 API（`ITfFnSearchCandidateProvider`
+ `ITfIntegratableCandidateListUIElement`）の実装を要し（[IME search integration
requirements](https://learn.microsoft.com/windows/apps/develop/input/input-method-editor-requirements#ime-search-integration)）、これは §2.7 のとおり **M21 スコープ**。
未実装でもスタート検索で活性化・入力は可能だが、統合表示は得られず composition 完了
後にのみクエリが渡る劣化モードになる。したがって **M5 の受け入れに「スタート検索の
統合表示」を含めない**（活性化・入力のみを範囲とする）。

最小契約を満たすための具体的な API 設計（自前 HWND と TSF UI element の「両立」
方式 = `CandidateUiCoordinator`）・カテゴリ登録要件・`ActivateEx` 最小実装・実機
互換チェックリストは §2.8〜§2.11 に定める。これらが DEV-97（D-01）で確定した
v1.0 方針であり、`plans/windows-port-roadmap.md`「リスクと不確実性」の未決事項
（候補 UI を `ITfCandidateListUIElement` か自前 HWND かプロトタイプ後に決める）を
解消する。

### 2.1 目的（M21 拡張）

Windows 11 / Office アプリ等で OS 側が候補ウィンドウを描画するモード
（TF_TMF_UIELEMENTENABLEDONLY）に対応する。
TIP 自前の `CandidateWindow` を抑制し、`ITfCandidateListUIElement` 経由で
OS 側 Suggestion UI に候補を渡す。

### 2.2 ActivateEx の検出（フラグ系統と一次情報）

TSF のアクティベーションフラグには 2 系統あり（いずれも msctf.h 定義）、混同し
やすいので明確化する:

| 系統 | 用途 | 例 |
|---|---|---|
| `TF_TMAE_*` | アプリが `ITfThreadMgr2/Ex::ActivateEx` に**渡す入力**フラグ | `TF_TMAE_UIELEMENTENABLEDONLY`, `TF_TMAE_SECUREMODE` |
| `TF_TMF_*` | `ITfThreadMgr2/Ex::GetActiveFlags` が**返す現在状態** | `TF_TMF_UIELEMENTENABLEDONLY`, `TF_TMF_SECUREMODE` |

重要な注意（[`ITfTextInputProcessorEx::ActivateEx`](https://learn.microsoft.com/windows/win32/api/msctf/nf-msctf-itftextinputprocessorex-activateex) /
[`ITfThreadMgrEx::GetActiveFlags`](https://learn.microsoft.com/windows/win32/api/msctf/nf-msctf-itfthreadmgrex-getactiveflags)）:
`ITfTextInputProcessorEx::ActivateEx` の `dwFlags` は公式リファレンス上
`TF_TMAE_SECUREMODE` / `TF_TMAE_COMLESS` / `TF_TMAE_WOW16` / `TF_TMAE_CONSOLE` のみを
列挙し、**UIElement 用ビットを明示していない**。したがって UI-less 判定を
`ActivateEx` の `dwFlags` に依存させるのは脆い。**一次情報として
`ITfThreadMgrEx::GetActiveFlags` を使い `TF_TMF_UIELEMENTENABLEDONLY` を確認する**
方式を採る。

```cpp
HRESULT TextService::ActivateEx(ITfThreadMgr* pThreadMgr,
                                TfClientId tid,
                                DWORD dwFlags) {
    // dwFlags は UIElement ビットを公式に列挙しないため GetActiveFlags を一次情報に。
    ui_less_mode_ = false;
    ITfThreadMgrEx* tmex = nullptr;
    if (SUCCEEDED(pThreadMgr->QueryInterface(IID_PPV_ARGS(&tmex))) && tmex) {
        DWORD active = 0;
        if (SUCCEEDED(tmex->GetActiveFlags(&active)))
            ui_less_mode_ = (active & TF_TMF_UIELEMENTENABLEDONLY) != 0;
        tmex->Release();
    }
    // ...
}
```

> **現状ギャップ（M5 で解消）**: 現行 `TextService::ActivateEx` は
> `UNREFERENCED_PARAMETER(dwFlags);` でフラグを破棄しており、`ui_less_mode_` を
> 保持していない（`tsf-tip/src/TextService.cpp`）。最小実装の手順とテスト方針は
> §2.10 に詳述する。

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

> **改訂（§2.8 両立方式に統合）**: 描画分岐は `ui_less_mode_` ではなく
> `BeginUIElement` の戻り値 `pbShow` で行う。TIP は UI-less / 通常モードに関わらず
> **常に `BeginUIElement(elem, &pbShow, &uiElementId)` を呼ぶ**（UIElement 対応 TIP
> の要件。§2.8・§2.9）。`ui_less_mode_`（`GetActiveFlags`）は冗長な自前 UI を事前に
> 抑制するためのヒントに留め、最終的な描画責務は per-call の `pbShow` で確定する。
> 通常スレッドでもアプリが `ITfUIElementSink` を advise していれば `pbShow ==
> FALSE` を返し得るため、`ui_less_mode_ == false` で `BeginUIElement` をバイパスして
> はならない。実装は §2.8 の `CandidateUiCoordinator` に集約する。

`BeginUIElement(elem, &pbShow, &uiElementId)` の 3 引数は順に `pElement`（UI 要素）/
`pbShow`（アプリが TIP UI を許可するかの返却。§2.6 参照）/ `pdwUIElementId`（OS 側
UI 要素 ID 返却）。`pbShow` の値による分岐は次の通り（§2.6 の表と整合）:

- **`pbShow == TRUE`（アプリは描画しない / TIP に自前 UI を許可）**: TIP が自前
  HWND を populate / update する。`ITfUIElement::Show(true)` で表示を開始し、以降
  は候補リストの差し替えや選択変更を TIP 自身が HWND に直接書く。OS / アプリは
  描画 / ポーリングしない。
- **`pbShow == FALSE`（アプリが代替描画する）**: 自前 HWND は出さず、TIP が
  `UpdateUIElement` で OS に更新通知する（初回は `BeginUIElement` 直後に即時呼び
  出す。§2.6・§2.8）。アプリは `ITfCandidateListUIElement::GetString` 等を QI 経由
  で呼んで候補を取得し、自身で描画する。

### 2.5 受け入れ条件

- Windows 11 / Office (TF_TMF_UIELEMENTENABLEDONLY) で TIP の自前ウィンドウが
  出ず、Windows 11 標準の候補 UI に候補が表示される
- `pbShow == TRUE` を返すアプリ（UI element sink を持たない大多数のレガシー Win32
  等）では従来通り `CandidateWindow` が出る

### 2.6 `pbShow` の per-call 切替

`ITfUIElementMgr::BeginUIElement(IUnknown* pElement, BOOL* pbShow, DWORD* pdwUIElementId)`
は呼び出しごとに `pbShow` の値が変わり得る。アプリ側が
`ITfUIElementSink::BeginUIElement` の `pbShow` で **TIP に描かせるか / アプリが
描くか** を返すため、TIP は **per-call** で次のように切り替える（[`ITfUIElementSink::BeginUIElement`](https://learn.microsoft.com/windows/win32/api/msctf/nf-msctf-itfuielementsink-beginuielement) パラメータ定義より）:

| `pbShow` 戻り値 | アプリ側の意思 | TIP 側挙動 |
|---|---|---|
| `TRUE`（既定 / アプリが描画しない） | アプリは UI を描かない。TIP に自前 UI を出してよい | 自前 HWND を表示し、以降は更新が無くてもよい（OS は `UpdateUIElement` を要求しない） |
| `FALSE`（アプリが代替描画する） | アプリが `ITfCandidateListUIElement` 等を QI して自前で描画する | 自前 HWND は出さない。`UpdateUIElement` を呼んでアプリに更新を通知 |

`UpdateUIElement` は `pbShow == FALSE` の場合に呼ぶ（[フローチャート](https://learn.microsoft.com/windows/win32/tsf/uiless-mode-overview#the-flow-chart-of-uilessmode)：「TIP must calls UpdateUIElement() after BeginUIElement() returns FALSE in *pbShow」）。

`EndUIElement` は `pbShow` の値にかかわらず呼ぶ（UI 要素のライフサイクル管理用）。

なお Microsoft Learn では `pbShow` と `pbShown` の表記が混在する。`ITfUIElementSink::
BeginUIElement` の正式パラメータ名は `pbShow`（API リファレンス）、`ITfUIElementMgr::
BeginUIElement` のフローチャート解説では `*pbShown` と書かれる。本書では `pbShow` で
統一する。

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

### 2.8 候補 UI 統合方針と `CandidateUiCoordinator`（v1.0 / M5・DEV-97 確定）

**設計判断（確定）**: 候補 UI は「`ITfCandidateListUIElement`（3-interface 契約）
か自前 `WS_POPUP` HWND か」の二者択一ではなく、**両方を常に実装し `BeginUIElement`
の戻り値 `pbShow` で per-call に描画責務を切り替える「両立 (coexistence)」方式**を
v1.0 方針として確定する。

両立が必要な根拠（[UILess Mode Overview](https://learn.microsoft.com/windows/win32/tsf/uiless-mode-overview)）:

- UI-less aware TIP は可視 UI を出す前に必ず `ITfUIElementMgr::BeginUIElement` を
  呼ぶ義務がある。通常モード（非 UI-less スレッド）でも、アプリが
  `ITfUIElementSink` を advise していれば `pbShow == FALSE` を返し得る。よって
  自前 HWND だけの実装では「アプリが自分で描画したい」ケースを満たせない。
- 逆に `ITfCandidateListUIElement` だけでは、UI element sink を持たない大多数の
  レガシー Win32 アプリ（メモ帳等）で OS もアプリも候補を描画しないため、自前
  HWND が必須。

→ v1.0 では **自前 HWND と TSF UI element の両方を 1 つの調整役（coordinator）に
集約**し、`pbShow` で振り分ける。`TextService` は `CandidateWindow` /
`ITfCandidateListUIElement` を直接操作せず、本 coordinator のみを呼ぶ。

#### API: `tsf-tip/include/azookey/tsf/CandidateUiCoordinator.h`（新規）

> 注: DEV-97 起票時の表記 `azookey/tip/CandidateUiCoordinator.h` は、既存 include
> 規約 `azookey/tsf/`（`CandidateWindow.h` 等と同階層）に合わせ
> **`azookey/tsf/CandidateUiCoordinator.h`** に統一する。

```cpp
namespace azookey::tsf {

// 候補 UI の単一統合点。pbShow に応じて自前 HWND と TSF UI element を振り分ける。
class CandidateUiCoordinator {
 public:
  // ActivateEx で受け取った UI-less フラグを保持（§2.10）。
  void SetUiLessMode(bool ui_less);

  // 候補表示を開始する。内部で:
  //   1) ITfCandidateListUIElement インスタンスを生成（候補を保持）
  //   2) ITfUIElementMgr::BeginUIElement(elem, &pbShow, &ui_element_id_)
  //   3) OnPbShown(pbShow) で描画責務を確定
  //   4) pbShow==FALSE のときは続けて UpdateUIElement(ui_element_id_) を即時に
  //      呼び初回候補内容をアプリへ通知する。アプリは BeginUIElement では内容を
  //      読まず最初の UpdateUIElement（全 update フラグ立ち）で読み始めるため、
  //      これを省くと初回候補が描画されない（§2.6 フローチャート）。
  // pt は自前 HWND を出す場合のキャレット直下スクリーン座標。
  HRESULT BeginUI(ITfThreadMgr* thread_mgr, POINT pt,
                  const std::vector<std::wstring>& items, int selected_idx);

  // 候補リスト or 選択変更時。
  //   pbShow==TRUE 経路: 自前 HWND を再描画（UpdateUIElement は呼ばない）
  //   pbShow==FALSE 経路: ITfUIElementMgr::UpdateUIElement(ui_element_id_) のみ
  HRESULT UpdateUI(const std::vector<std::wstring>& items, int selected_idx);

  // 終了。pbShow の値に関わらず HWND を Hide し EndUIElement を必ず呼ぶ。
  HRESULT EndUI();

  bool IsShowing() const;

 private:
  // BeginUIElement の戻り値を受けて描画経路を確定:
  //   true  → 自前 HWND を Show（OS/アプリは描画しない）
  //   false → 自前 HWND は出さず、即時 UpdateUIElement で初回内容をアプリへ委譲
  void OnPbShown(bool tip_draws);

  CandidateWindow own_window_;                  // pbShow==TRUE 経路
  ComPtr<CandidateListUIElement> ui_element_;   // pbShow==FALSE / UI-less 経路
  ITfUIElementMgr* ui_element_mgr_{nullptr};
  DWORD ui_element_id_{0xFFFFFFFF};             // BeginUIElement が返す ID
  bool ui_less_mode_{false};                    // TF_TMF_UIELEMENTENABLEDONLY
  bool tip_draws_{true};                        // 直近 BeginUI の pbShow 結果
  bool showing_{false};
};

}  // namespace azookey::tsf
```

`ITfUIElementMgr::BeginUIElement(ITfUIElement* pElement, BOOL* pbShow,
DWORD* pdwUIElementId)` / `UpdateUIElement(DWORD)` / `EndUIElement(DWORD)` の各
シグネチャは msctf.h 準拠。`ui_element_mgr_` は `ActivateEx` 時に
`thread_mgr->QueryInterface(IID_ITfUIElementMgr, ...)` で取得しキャッシュする。

#### 状態遷移

| 状態 | `BeginUI` | `UpdateUI` | `EndUI` |
|---|---|---|---|
| Hidden | `BeginUIElement` → `pbShow` 判定 → Showing(TIP) / Showing(App)。FALSE のときは続けて即時 `UpdateUIElement`（初回内容通知） | (no-op) | (no-op) |
| Showing(TIP)（`pbShow==TRUE`） | （再 `BeginUI` は `EndUI` 後） | 自前 HWND 再描画のみ | HWND Hide + `EndUIElement` |
| Showing(App)（`pbShow==FALSE`） | 同上 | `UpdateUIElement` のみ | `EndUIElement`（HWND は元から非表示） |

`ITfUIElement::Show(FALSE)`（アプリが途中で UI-less に移行し TIP UI を隠す要求）を
受けた場合は Showing(TIP) → Showing(App) へ遷移し、以後 `UpdateUIElement` 経路に
切り替える（[UILess Mode Overview「Show/Hide status of UIElement」](https://learn.microsoft.com/windows/win32/tsf/uiless-mode-overview)）。

#### M5 と M21 の役割分担（改訂）

- **M5（v1.0）**: 上記 coordinator の骨格 + `pbShow==TRUE` 経路（自前 HWND）+
  `BeginUIElement`/`EndUIElement` 動線 + `ActivateEx` フラグ検出（§2.10）+
  カテゴリ登録（§2.9）。`pbShow==FALSE` 経路は `CandidateListUIElement` の最小実装
  （`GetCount`/`GetString`/`GetSelection`/`GetUpdatedFlags` + `UpdateUIElement`
  通知）まで。
- **M21**: `CandidateListUIElement` の full 実装（paging・`...Behavior`・
  `ITfIntegratableCandidateListUIElement` 等 §2.3/§2.7）、IME On/Off（§4）、予測
  候補の UIElement 化（§8）。M21 の役割は「UI-less mode の full 動作確認とリッチ
  化」に縮小し、**契約自体（3-interface 雛形 + BeginUIElement 動線）は v1.0 で実装**
  する。

### 2.9 カテゴリ登録要件（`GUID_TFCAT_TIPCAP_UIELEMENTENABLED`）

[UILess Mode Overview「UIElement Supporting TIP」](https://learn.microsoft.com/windows/win32/tsf/uiless-mode-overview): 「UIElement
をサポートする TIP は `GUID_TFCAT_TIPCAP_UIELEMENTENABLED` でカテゴリ登録されねば
ならない」。

**要件**: `tsf-tip/src/DllMain.cpp::DllRegisterServer` は `GUID_TFCAT_TIP_KEYBOARD`
（キーボード型 TIP の必須カテゴリ）と `GUID_TFCAT_DISPLAYATTRIBUTEPROVIDER` を
`RegisterCategory` する（DEV-157 で実装済み）。`GUID_TFCAT_TIPCAP_UIELEMENTENABLED`
は **§2.8 の UIElement 公開実装（`ITfUIElementMgr` / `ITfCandidateListUIElement`）と
同時に登録する**。公開実装が無いまま本カテゴリだけ登録すると、UI-less-only ホスト
（Windows 11 / Office）が TIP 自前ウィンドウを抑制した上で候補が TSF 経由で公開されず、
候補が消える / 選択不能になる。したがって本カテゴリ登録は §2.8 / M21 の最小 UIElement
公開実装に内包し、DEV-157 では登録しない（`ui_less_mode_` 検出のみ先行 = §2.10）。

**v1.0 で必要な追加（M5）**:

```cpp
// DllRegisterServer 内、DISPLAYATTRIBUTEPROVIDER 登録に続けて:
pCatMgr->RegisterCategory(kTextServiceClsid,
                          GUID_TFCAT_TIPCAP_UIELEMENTENABLED,
                          kTextServiceClsid);
```

`DllUnregisterServer` でも対応する `UnregisterCategory` を呼ぶ。machine-wide 登録は
管理者権限で HKLM へ書き込む前提のため、カテゴリ登録失敗は `SELFREG_E_CLASS` を返す
致命エラーとして扱う。検証は `tsf-tip/tests/com_smoke_test.cpp` の登録 round-trip
smoke（`RegisterProfile` / カテゴリ登録 → `GetProfile` で確認 → 解除）で covered。
対話的 TSF セッションを要するため、opt-in 環境変数 `AZOOKEY_RUN_REGISTRATION_SMOKE`
+ 昇格時のみ実行で、CI（headless）では走らない。

### 2.10 `ActivateEx` の最小実装と現状ギャップ

**実装済み（DEV-157）**: `TextService::ActivateEx` は `ITfThreadMgrEx` を QI して
`GetActiveFlags` を呼び、`TF_TMF_UIELEMENTENABLEDONLY` の有無を `ui_less_mode_` に
保持する（`tsf-tip/src/TextService.cpp`）。`dwFlags` は UIElement ビットを公式に
列挙しないため一次情報にしない。`ui_less_mode_` を消費して自前候補ウィンドウを
抑制する `pbShow` 分岐（§2.8）は M21 で実装する。

**v1.0 で必要な最小実装（M5）**: UI-less 判定は §2.2 のとおり
`ITfThreadMgrEx::GetActiveFlags` を一次情報とする（`ActivateEx` の `dwFlags` は
UIElement ビットを公式に列挙しないため）。

```cpp
STDMETHODIMP TextService::ActivateEx(ITfThreadMgr* ptim, TfClientId tid, DWORD dwFlags) {
  ui_less_mode_ = false;
  ITfThreadMgrEx* tmex = nullptr;
  if (SUCCEEDED(ptim->QueryInterface(IID_PPV_ARGS(&tmex))) && tmex) {
    DWORD active = 0;
    if (SUCCEEDED(tmex->GetActiveFlags(&active)))
      ui_less_mode_ = (active & TF_TMF_UIELEMENTENABLEDONLY) != 0;
    tmex->Release();
  }
  candidate_ui_.SetUiLessMode(ui_less_mode_);
  // 既存の sink advise / candidate UI 生成 / IPC worker 起動はそのまま
}
```

`TF_TMF_SECUREMODE`（パスワード欄等のセキュア入力）等の他フラグも同じ
`GetActiveFlags` 経由で取得でき、M46 プライバシー / セーフ入力で扱う。本変更は
COM smoke テスト（モック `ITfThreadMgrEx::GetActiveFlags` が
`TF_TMF_UIELEMENTENABLEDONLY` を返すとき `ui_less_mode_` が true になる）で
covered。

### 2.11 アプリ互換チェックリスト（実機 Win11・`gate:human-required`）

`pbShow` の戻り値とアプリの描画責務はアプリ実装に依存するため、代表アプリでの実測が
必要。**計測手順・対象アプリ・合格条件（安定仕様）は `docs/legacy-parity-spec.md`
§12** に定める。実機 Win11 が必要なため DEV-97 の子課題 DEV-153
（`gate:human-required`）で M5 着手時に実施し、**アプリ別の実測結果（可変ログ）は
docs ではなく DEV-153（Linear）に記録する**（`AGENTS.md` の状態 Linear 一本化方針）。

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
        // 選択中の言語プロファイルを引数として設定アプリへ渡す
        // （複数プロファイル時に既定ページではなく該当プロファイルを初期表示するため）
        wchar_t profile[64] = {};
        StringFromGUID2(rguidProfile, profile, ARRAYSIZE(profile));
        wchar_t args[128] = {};
        swprintf_s(args, L"--langid 0x%04X --profile %s", langid, profile);

        // 設定アプリ EXE を非同期起動（終了待ちしない。理由は下記注記）
        SHELLEXECUTEINFOW sei{ sizeof(sei) };
        sei.lpFile       = L"azookey_settings.exe"; // 実体は §6.4 GetInstalledExePath で解決
        sei.lpParameters = args;
        sei.hwnd         = hwndParent;
        sei.nShow        = SW_SHOW;
        sei.fMask        = SEE_MASK_NOCLOSEPROCESS;
        ShellExecuteExW(&sei);
        return S_OK;
    }
};
```

> **`Show` を非同期にする理由（`docs/sideload-packaging-spec.md` §3.5 と整合）**:
> `ITfFnConfigure::Show` の Remarks は「ダイアログを閉じるまで return しない」（短命な
> モーダル プロパティ シートを想定）だが、本 IME の設定 UI は WinUI 3 の独立した**長命**
> プロセスである。`Show` を `azookey_settings.exe` 終了までブロックすると、呼び出し元
> （言語/IME 設定 UI）をその間フリーズさせるため、**起動後ただちに `S_OK` を返す
> 非同期方式**を採る（`WaitForSingleObject` で終了待ちしない）。設定値の反映はプロパティ
> シートの OK/Apply ではなく `UpdateSettings` IPC（§3）で行う。多重起動を避けるため設定
> アプリは single-instance とし、既存インスタンスがあれば前面化する
> （`SEE_MASK_NOCLOSEPROCESS` で得たプロセスハンドルは前面化・監視用途に使い、
> 終了待ちには使わない）。

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
