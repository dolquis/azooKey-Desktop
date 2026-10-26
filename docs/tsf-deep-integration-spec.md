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

`tsf-tip/include/azookey/tsf/ReconversionFunction.h` と
`tsf-tip/src/ReconversionFunction.cpp`：

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

`ReverseConvert` は辞書層（静的層とユーザー辞書）を先に引く。見つからない表層には、
学習ストアでその表層に確定した読みのうち減衰後の重みが最大のものを返す。それも無く、
表層が平仮名・片仮名・長音符だけなら、片仮名を平仮名に揃えた表層を読みとする。
いずれにも当たらなければ読みは空で、TIP は候補 UI を出さずに再変換の範囲を解放する。

TIP は選択変更を `ITfTextEditSink::OnEndEdit` で検知し、UI メッセージで privacy を
確認してから IPC worker に逆引きと候補取得を先読みさせる。フォーカス中の context では、
`GetReconversion` と `Reconvert` は元の選択文字列を候補 1 件として直ちに返し、
候補選択を azooKey の候補 UI で行う。選択範囲と context が一致するキャッシュ済み候補が
あればその場で候補 UI を開き、キャッシュ未取得時は Host の応答を TSF/UI スレッド上で
待たずに、後から届いた Host 候補を候補 UI に表示する。この表示では範囲を
`TF_ES_ASYNCDONTCARE` の edit session で読み直し、表層が要求時と一致するときだけ
候補 UI を開く。アプリのコールバックの外では同期ロックを拒むアプリ（Word）があるため、
この session は後から実行されることがあり、その間に選択・context・TIP の状態が
変わっていれば候補を捨てる。フォーカス外の context には、
選択範囲と context が一致するキャッシュ済み候補をそのまま返す。選択範囲または
context が変わった後の応答は破棄し、元の選択範囲へ適用しない。
変換キーも選択範囲を保持して非同期取得し、候補 UI に表示する。

新規 IPC：

```
ReverseConvertRequest:
  surface: string

ReverseConvertResponse:
  reading: string         // 最尤読み
  confidence: float
```

#### Reconvert

- 元の選択文字列と同じ候補を確定した場合、その文字列を変更しない
- Host 候補の確定は、対象の選択範囲と context が一致する場合に限って置換する
- 候補選択には azooKey の候補 UI を使う

### 1.3 FunctionProvider 公開

`TextService` は `ITfFunctionProvider` を公開し、`ActivateEx` 時に
`ITfSourceSingle::AdviseSingleSink` で thread manager に登録する。
`GetFunction` から `ITfFnReconversion` を返し、`Deactivate` 時に
`UnadviseSingleSink` で解除する。Windows SDK に
`GUID_TFCAT_TIP_RECONVERSION` という category はないため、category 登録は使わない。

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

### 2.3 CandidateListUIElement 実装

`tsf-tip/include/azookey/tsf/CandidateListUIElement.h` / `tsf-tip/src/CandidateListUIElement.cpp`：

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

#### 2.4.1 候補選択と preedit の表示

候補表示中も、かな reading は変換要求と再編集に使う正典として保持する。
一方、候補ウィンドウを表示した時点と、上下キーまたは Space で候補選択が変わったときは、
選択中候補の `surface` を表示用 preedit として EditSession から composition range へ反映する。
Enter と数字キーは、この表示中 `surface` と同じ候補を確定する。

追加入力、Backspace で候補表示を閉じるときは、表示用 preedit を正典の reading
へ戻してから次の編集を反映する。Esc は読みへ戻らず入力を取り消す
（`docs/legacy-parity-spec.md` §1.2）。
候補の `surface` を reading バッファへ書き戻してはならない。

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
                  const std::vector<CandidateViewItem>& items, int selected_idx);

  // 候補リスト or 選択変更時。
  //   pbShow==TRUE 経路: 自前 HWND を再描画（UpdateUIElement は呼ばない）
  //   pbShow==FALSE 経路: ITfUIElementMgr::UpdateUIElement(ui_element_id_) のみ
  HRESULT UpdateUI(const std::vector<CandidateViewItem>& items, int selected_idx);

  // 終了。pbShow の値に関わらず HWND を Hide し EndUIElement を必ず呼ぶ。
  HRESULT EndUI();

  bool IsShowing() const;

 private:
  // BeginUIElement の戻り値を受けて描画経路を確定:
  //   true  → 自前 HWND を Show（OS/アプリは描画しない）
  //   false → 自前 HWND は出さず、即時 UpdateUIElement で初回内容をアプリへ委譲
  void OnPbShown(bool tip_draws);

  CandidateWindow own_window_;                  // pbShow==TRUE 経路
  wil::com_ptr_nothrow<CandidateListUIElement> ui_element_;  // pbShow==FALSE / UI-less 経路
  wil::com_ptr_nothrow<ITfUIElementMgr> ui_element_mgr_;
  DWORD ui_element_id_{0xFFFFFFFF};             // BeginUIElement が返す ID
  bool ui_less_mode_{false};                    // TF_TMF_UIELEMENTENABLEDONLY
  bool tip_draws_{true};                        // 直近 BeginUI の pbShow 結果
  bool showing_{false};
};

}  // namespace azookey::tsf
```

`ITfUIElementMgr::BeginUIElement(ITfUIElement* pElement, BOOL* pbShow,
DWORD* pdwUIElementId)` / `UpdateUIElement(DWORD)` / `EndUIElement(DWORD)` の各
シグネチャは msctf.h 準拠。`ui_element_mgr_` は `BeginUI` 時に必要なら
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

### 2.9 TSF capability カテゴリ登録要件

[UILess Mode Overview「UIElement Supporting TIP」](https://learn.microsoft.com/windows/win32/tsf/uiless-mode-overview): 「UIElement
をサポートする TIP は `GUID_TFCAT_TIPCAP_UIELEMENTENABLED` でカテゴリ登録されねば
ならない」。

**要件**: `tsf-tip/src/DllMain.cpp::DllRegisterServer` は `GUID_TFCAT_TIP_KEYBOARD`
（キーボード型 TIP の必須カテゴリ）と `GUID_TFCAT_DISPLAYATTRIBUTEPROVIDER` を
`RegisterCategory` する。`GUID_TFCAT_TIPCAP_UIELEMENTENABLED` は **§2.8 の
UIElement 公開実装（`ITfUIElementMgr` / `ITfCandidateListUIElement`）と同時に登録する**。
公開実装が無いまま本カテゴリだけ登録すると、UI-less-only ホスト（Windows 11 / Office）
が TIP 自前ウィンドウを抑制した上で候補が TSF 経由で公開されず、候補が消える /
選択不能になる。したがって本カテゴリ登録は M5 の最小 UIElement 公開実装に内包する。

[Custom input method editor requirements](https://learn.microsoft.com/windows/apps/develop/input/input-method-editor-requirements)
に従い、Windows アプリ互換性の宣言として
`GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT` も登録する。このカテゴリは UIElement 公開能力とは
独立したロード前提であり、`DllUnregisterServer` と登録失敗 rollback でも解除する。

**v1.0 で必要な追加（M5）**:

```cpp
// DllRegisterServer 内、DISPLAYATTRIBUTEPROVIDER 登録に続けて:
pCatMgr->RegisterCategory(kTextServiceClsid,
                          GUID_TFCAT_TIPCAP_UIELEMENTENABLED,
                          kTextServiceClsid);
```

Windows アプリ互換宣言は M5 の UIElement 公開実装とは独立して登録する。次は責務境界を
示す簡略例であり、実装では全必須カテゴリを共有リストから登録する。

```cpp
// UIElement 公開実装に依存しない Windows アプリ互換宣言:
pCatMgr->RegisterCategory(kTextServiceClsid,
                          GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT,
                          kTextServiceClsid);
```

`DllUnregisterServer` でも対応する `UnregisterCategory` を呼ぶ。machine-wide 登録は
管理者権限で HKLM へ書き込む前提のため、カテゴリ登録失敗は `SELFREG_E_CLASS` を返す
致命エラーとして扱う。`DllRegisterServer` は最初の HKLM 書き込みから成功確定まで
rollback guard を保持し、後続処理が失敗した場合は `DllUnregisterServer` 相当の
best-effort cleanup で COM CLSID、TSF プロファイル、カテゴリを削除する。
同じプロファイルを再登録する呼び出しは成功し、失敗後も明示的な unregister なしで
再試行できなければならない。再登録中に失敗した場合も、以前から存在した登録を復元せず、
部分状態を残さない完全な未登録状態へ収束させる。

検証は `tsf-tip/tests/com_smoke_test.cpp` の登録 round-trip smoke で行う。
通常経路では再登録、`GetProfile`、解除を確認する。Debug ビルドでは
`AZOOKEY_TEST_FAIL_CATEGORY_REGISTRATION` によってプロファイル登録後の失敗を注入し、
最初のカテゴリを登録した後で失敗させ、残骸がないことと、そのまま再登録できることを確認する。
対話的 TSF セッションを要するため、opt-in 環境変数 `AZOOKEY_RUN_REGISTRATION_SMOKE`
+ 昇格時のみ実行で、CI（headless）では走らない。

### 2.10 `ActivateEx` の最小実装と契約ギャップ

**実装済み（DEV-157 / DEV-154）**: `TextService::ActivateEx` は `ITfThreadMgrEx` を
QI して `GetActiveFlags` を呼び、`TF_TMF_UIELEMENTENABLEDONLY` の有無を
`ui_less_mode_` に保持する（`tsf-tip/src/TextService.cpp`）。`dwFlags` は UIElement
ビットを公式に列挙しないため一次情報にしない。`ui_less_mode_` は
`CandidateUiCoordinator` に伝播し、§2.8 の `pbShow` 分岐で自前 HWND と
app-drawn UIElement 経路を切り替える。

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
| `VK_OEM_ENLW` (0xF4) | 同上（一部 HW） | 同上 |
| `VK_IME_ON` (0x16) | IME オン | IME On（トグルしない） |
| `VK_IME_OFF` (0x1A) | IME オフ | IME Off（トグルしない） |
| `VK_NONCONVERT` (0x1D) | 無変換 | 確定済み or 選択を平仮名/カタカナ/英字 と巡回変換。`convertKeysAction` が `imeOnOff` なら IME Off |
| `VK_CONVERT` (0x1C) | 変換 | 確定済み or 選択を再変換 (M20 と同経路)。`convertKeysAction` が `imeOnOff` なら IME On |
| `VK_OEM_ATTN` / `VK_DBE_ALPHANUMERIC` (0xF0) | Caps / 英数 | alphanumeric モードトグル |
| `VK_DBE_HIRAGANA` (0xF2) | ひらがな | input mode = hiragana |
| `VK_DBE_KATAKANA` (0xF1) | カタカナ | input mode = katakana |

`VK_OEM_ATTN` と `VK_DBE_ALPHANUMERIC` は同じ VK 値の別名なので、
VK だけから区別しない。`VK_DBE_HIRAGANA` / `VK_DBE_KATAKANA` の
入力モード直接指定は、DEV-1205 の半角/全角・無変換・変換・Caps の4キーとは別の拡張である。

開閉キー（半角/全角、`VK_IME_ON`、`VK_IME_OFF`、`imeOnOff` のときの変換・無変換）は、
修飾キー（Shift、Ctrl、Alt、Win）が押されていないときだけ働く。IME Off と英数モードでも
TIP がキーを受け取り、アプリへは渡さない。アプリがキーボード入力を無効にした context
（§4.3）では他のキーと同じく素通しする。`VK_IME_ON` と `VK_IME_OFF` は Microsoft IME と同じ
キーで、AutoHotKey（`Send {vk16}` / `Send {vk1A}`）や PowerToys Keyboard Manager から送れる。
Alt の単独押しを TIP が検出することはしない。

- `VK_IME_ON`: Off なら On にする。On なら compartment を書き換えない。英数モード中なら英数モードを
  解除し、ひらがな入力に戻す。
- `VK_IME_OFF`: On なら Off にする。Off なら何もしない。入力途中（読み、ライブ変換の表示、候補の選択中）
  なら、表示中の文字列を Enter と同じに確定してから Off にする（§4.2）。

### 3.1.1 変換・無変換キーの役割（`convertKeysAction`）

設定 `convertKeysAction`（enum `default` / `imeOnOff`、既定 `default`、DEV-1736）は変換キーと無変換キーの
役割を決める。

- `default`: §3.2 の巡回と §3.3 の再変換。
- `imeOnOff`: 変換キーは `VK_IME_ON`、無変換キーは `VK_IME_OFF` と同じに働く。再変換と字種の巡回には使わない。
  英字キーボードで Google 日本語入力と同じ「Alt → 変換/無変換」の割り当てを使うための設定である。

TIP は `settings.json` の変更を読み直して反映する。キーが無い、または `default` / `imeOnOff` 以外の値は
`default` とする。Host はこの設定を読まない。

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

### 3.5 OEM 記号キーと明示句読点

`VK_OEM_COMMA`、`VK_OEM_PERIOD`、`VK_OEM_2` は、現在の keyboard layout と modifier
状態を使って文字へ変換する。
日本語入力 ON で変換結果が `,` または `.` のときは、composition の有無によらず
明示句読点 `、` または `。` として preedit へ取り込む。preedit がない場合は
句読点単独で composition を開始する。句読点単独の変換候補は `、` と `,`、または
`。` と `.` の順に表示し、Space で選び Enter または候補番号で確定する。
半角候補は Host と記号リライターの有無に依存しない。Esc は通常の候補窓と同じく
入力を取り消し（`docs/legacy-parity-spec.md` §1.2）、Backspace は句読点を削除する。
`/` と Shift 付きの `<`、`>`、`?` は、その文字を preedit へ取り込む。
未確定ローマ字が残っている場合は先にかなへ確定し、記号を同じ composition の末尾へ追加する。
batch 変換の raw 入力には ASCII の `,` と `.` を保持するが、かな preview と raw romaji
preview の表示では明示句読点として `、。` へ変換する。

preedit がない場合、`/` と Shift 付きの `<`、`>`、`?` はアプリへパススルーする。
preedit に取り込んだ記号は通常の Backspace と同じ削除対象とし、アプリ本文と IME 内部に
編集対象を分裂させない。

`docs/dynamic-punctuation-spec.md` の自動句読点は、ユーザーが明示的に押したキーの
字種と状態遷移を扱わないため、本節の契約とは別機能である。
テンキーの `VK_DECIMAL` と `VK_DIVIDE` は本節の対象外とし、現段階では composition へ
取り込まない。

#### 3.5.1 句読点キーで変換して確定する（`punctuationConversion`）

設定 `punctuationConversion`（enum `off` / `commit`、既定 `off`、DEV-1638）が `commit` のとき、
`、` または `。` のキーは、直前の読みを変換して句読点と一緒に確定する。
`off` では §3.5 の挙動を変えない。

- 確定する文字列は、読みの第 1 候補に句読点を続けたものとする。
  第 1 候補は次の順に決める。
  - ライブ変換 ON で、表示中の surface がその読みのものなら、その surface。
    末尾が自動句読点（`docs/dynamic-punctuation-spec.md` §5.1）なら、その文節を外して押した句読点に置き換える。
    末尾以外の自動句読点は表示どおり残す。
  - それ以外は、その読みに対する打鍵中の応答（`QueryCandidates` の live）の先頭候補。
- 候補窓の表示中（`Selecting`）は、選択中の候補に句読点を続けて確定する。
- 押した時点で第 1 候補が無い場合（未確定ローマ字を確定して読みが変わった、応答がまだ届いていない）は、
  その読みで live の `QueryCandidates` を送り、`docs/dynamic-punctuation-spec.md` §7.3 と同じ上限（既定 200ms）まで待つ。
  待つ間はキーを消費し、応答の到着、上限の経過、次のキーのいずれかで処理する。
  上限を過ぎた場合、Host が使えない場合、Space の応答待ちの場合は `off` と同じく句読点を読みに足す。
  未変換の読みを強制確定することはない。
  フォーカス喪失、composition の終了、Deactivate では、Enter の保留（`docs/dynamic-punctuation-spec.md` §7.3）と同じく、待っている句読点キーを捨てる。
- 次の場合は変換せず、`off` と同じく句読点を読みに足す。
  - 読みが空、または `、` `。` だけ（§3.5 の半角候補を残す）。
  - `Selecting` 以外で、読みの末尾が数字（`3.14`、`1,000`）。
  - 一括変換（`docs/romaji-batch-conversion-spec.md`）の蓄積中、絵文字検索中、括弧入力中。
- 数字リライターが ON でも、`Selecting` 以外の句読点キーは core の状態機械が扱う。
  数字リライターの候補窓（legacy 経路）の表示中は対象外とする。
- 学習観測は、句読点を除いた読みと候補だけを送る（`docs/privacy-and-secure-input-spec.md` §5 の抑止に従う）。
  ライブ変換の文節観測では、外した末尾の自動句読点を含めない。
  secure と private の文脈でも変換と確定は行い、観測は既存の抑止で送らない。
- その打鍵では半角の `,` `.` を候補から選べない。
  確定の直後に無変換キーで字種を切り替える機能（確定文字列を読みの字種へ置き換える）は、押した句読点まで置き換えるため、句読点キーの確定には働かない。
  `、` `。` 以外（`？` `！` など）は対象外とする（follow-up: DEV-1670）。
- Enter だけが待つ最終要求（`docs/dynamic-punctuation-spec.md` §7.3）は、句読点キーでは送らない。

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

有効化時の初期状態は On とする。azooKey を選んだ直後から日本語入力で始まり、
Microsoft IME と旧 macOS 版に揃う。`ActivateEx` は sink を購読する前に compartment を読み、
`VT_I4` の 0、値が未設定（`VT_I4` 以外）、または読み出しに失敗したときは
`SetValue(client_id, 1)` で On にする。既に On なら書き込まない。`SetValue` が失敗しても
有効化は失敗させず、`keyboard_open_compartment_set_failed` を警告ログに出して、
読み直した値に従う。読み直した値が `VT_I4` の 0 なら Off、それ以外は On とする。
`ITfCompartmentMgr` を持たないホストでは compartment を扱わず、On として動く。

新しい入力先も On で始まる。OPENCLOSE はスレッドの compartment だが、IMM32 互換層は
入力コンテキスト（HIMC）ごとの開閉状態を持ち、フォーカスの移った先の状態で compartment を
書き直す。新しい HIMC の既定は閉じた状態のため、有効化時に On にしても、最初の入力先へ
フォーカスが移ると Off に戻りうる。そこで `ITfThreadMgrEventSink::OnSetFocus` で初めて
フォーカスを受けた `ITfDocumentMgr`（有効化時に既にフォーカスを持つものを含む）にも、
有効化時と同じ規則で On を適用する。この初回フォーカスから 1 秒以内に同じ document manager の
compartment が 0 へ変わったときは、入力コンテキストの復元とみなして 1 度だけ On に戻す。
TIP が処理した開閉キー（半角/全角、`VK_IME_ON`、`VK_IME_OFF`、`imeOnOff` のときの変換・無変換、英数、ひらがな）と
入力インジケーターの左クリック（§4.5）でユーザーが開閉したら、その document manager では戻さない。言語バーやアプリの `ImmSetOpenStatus` など TIP を経由しない Off は区別できないため、
この 1 秒以内に届いたものは 1 度だけ On に戻る。
一度フォーカスを受けた document manager へ戻ったときは TIP は書き込まず、compartment の値に従う。
入力先ごとの開閉の復元は IMM32 互換層が行い、TIP は入力先ごとの開閉を記録しない。
document manager の記録は `OnUninitDocumentMgr` と `Deactivate` で消す。

開閉の経緯は `AZOOKEY_LOG` の info ログで追える。`keyboard_open_initial`（On にする判定、
`source` は `activate` / `first_focus` / `reopen_after_restore`）、`keyboard_open_focus_first` と
`keyboard_open_focus`（フォーカス受信）、`keyboard_open_refresh`（値の反映、`source` は
`activate` / `compartment_change` / `toggle_key`（半角/全角）/ `ime_on_key` / `ime_off_key` /
`convert_key` / `nonconvert_key`（`imeOnOff` のときの変換・無変換）/ `langbar_click`）を出す。
開閉キーで状態が変わらなかったとき（On での `VK_IME_ON` など）も、その `source` の `keyboard_open_refresh` を 1 行出す。
TIP が compartment を書き換えると、TSF が同期に呼ぶ `OnChange` の `compartment_change` の行が先に値の変化を記録し、
その直後にキーやクリックの `source` の行が続く。どの操作で開閉したかは、後者の行で追う。これらの行は compartment の値、
反映前後の開閉、英数モード、フォーカスウィンドウの HIMC の開閉（`imm_open`）、
`GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION` の値、スレッド ID を持つ。
`key_passthrough` は Off または英数モードでキーを素通ししたことを、開閉と英数モードの組が
変わるたびに 1 行だけ出す。打鍵ごとには出さない。どの行も入力本文、キーコード、打鍵の回数を持たない。

### 4.2 状態遷移

| 旧 | 新 | 動作 |
|---|---|---|
| Off | On | composition / candidate なし、入力モードを `hiragana` に初期化 |
| On | Off | composition があれば確定 (auto-commit) し、内部状態をクリア |
| On（英数モード） | On | `VK_IME_ON` と `imeOnOff` の変換キーだけ。英数モードを解除して `hiragana` にする。compartment は書き換えない |

On → Off で何を確定するかは、Off にした操作で決まる。

- `VK_IME_OFF`、`imeOnOff` の無変換、入力インジケーターの左クリック（§4.5）: 画面に表示中の文字列
  （ライブ変換の表示、選択中の候補、読み）を Enter と同じ経路で確定してから compartment を 0 にする。
  Enter と同じく確定を学習観測に送り、secure と学習不可の文脈では送らない。`dynamicPunctuation` の
  onPause で Enter が行う最終要求の待ち（`docs/dynamic-punctuation-spec.md` §7.3）はせず、表示中の文字列を
  その場で確定する。確定の edit session が拒否されたとき、または Enter が確定せずに待つとき（一括変換の応答待ち）は
  Off にせず On のまま残し、キーは消費する。拒否された確定は次のキーで確定し直す。Enter がキーを受け取らない状態
  （Enter に割り当てのない入力の途中）では、次の項目と同じ経路で確定して Off にする。
- 半角/全角、言語バーや IMM など TIP の外からの Off: フォーカス喪失と同じ確定の経路で、読み（未確定ローマ字を含む）を
  確定する。TSF が編集を拒んだときは composition を残し、後の edit session で確定し直す。

### 4.3 Win+Space / アプリ切替時

- Win+Space (言語切替) は OS が処理 → `Deactivate` が呼ばれる
- アプリ切替 (`OnSetFocus`) では現在の composition を **確定** してから前面アプリへ
  フォーカスを譲る（MS-IME 互換）
- 同じアプリ内の入力先からのフォーカス離脱も確定する。入力先が先に composition を
  終了した場合は、最後に表示した範囲を旧入力先で確認する。文字が残っていれば再挿入せず、
  保留中の更新で確定文字が変わっていれば旧表示文字の範囲を最新文字へ置換する。
  範囲が空になっていればその範囲だけを復元する。入力先が別の文字へ変更した場合は
  上書きしない。同期編集が拒否された場合は旧入力先と範囲を保持して再試行し、
  範囲を特定できない場合は現在の選択位置へ推測で挿入しない。明示的な Esc 取消しでは
  復元しない。
- 同じアプリ内で document manager が切り替わった時点で composition がまだ無い入力
  （打鍵の preedit の edit session が走る前の保留中の入力）は、どの入力先にも表示されて
  いないので旧入力先へ確定せず、切替先の document で続ける。Excel が未編集のセルへの
  最初の打鍵でセル編集用の document へフォーカスを移す場合がこれに当たる。
  - 入力欄自体が secure（パスワードの入力 scope やフォーカス窓、アプリがキーボード入力を
    無効にした context）なら、旧入力欄でも切替先でも持ち越さずに捨てる。
  - 持ち越した入力は、切替の時点で切替先の document に表示を要求する。composition として
    表示されるまでは、切替時と打鍵ごとに、その context が切替先の document のもので、
    IME が開いていて英数モードでなく、入力欄自体が secure でないときだけ続け、それ以外では
    捨てる。表示される前に再びフォーカスが移った場合、切替先の document が破棄された場合、
    Deactivate でも捨てる。
  - 旧 document に積まれていた edit session が後から走っても、TIP がその context を再び
    編集するまでは、持ち越した入力を旧 document に表示も確定もしない。
  - アプリの外へフォーカスが移った場合（切替先の document manager が無い）は持ち越さない。
  - 確定時の secure・学習の判定は、確定した入力先の context で行う。

### 4.4 受け入れ条件

- azooKey を選んだ直後、直前の compartment が Off でも IME On で始まる
- 新しく開いたウィンドウ（新しい入力先）は IME On で始まる
- 開いたことのある入力先へ戻ったとき、TIP は開閉を書き込まない（IMM32 互換層が入力先ごとの状態を戻すアプリでは、Off にした入力先は Off のまま）
- 半角/全角キーで IME On/Off が切り替わり、Status を反映
- `VK_IME_ON` で Off から On になり、On のままなら変わらない。英数モード中の `VK_IME_ON` はひらがなに戻る
- `VK_IME_OFF` で On から Off になり、Off のままなら変わらない。読み入力中・候補表示中は表示中の文字列を確定してから Off になる
- 初回フォーカスから 1 秒以内でも、開閉キーと入力インジケーターのクリックによる Off は On に戻らない
- `convertKeysAction` が `default` のとき変換・無変換は §3.2・§3.3 のとおり。`imeOnOff` のとき変換で On、無変換で Off になり、再変換と巡回は起きない
- 入力インジケーターが On・英数モード・Off を「あ」「_A」「A」で表示し、左クリックで On/Off が切り替わる（§4.5）
- Win+Space で別言語に切替時、composition が確定される
- アプリ切替時に composition が確定される

### 4.5 入力インジケーター（言語バー項目 `GUID_LBI_INPUTMODE`）

TIP は有効化のときに言語バー項目（`ITfLangBarItemButton`）を 1 つ追加し、無効化で削除する。
Windows 8 以降のタスクバーの入力インジケーターは、`TF_LANGBARITEMINFO.guidItem` が `GUID_LBI_INPUTMODE`
の項目だけを IME のモードアイコンとして表示する（Microsoft Learn「Custom Input Method Editor (IME)
requirements」「TF_LANGBARITEMINFO」）。Windows の「デスクトップ言語バーを使用する」を有効にすると、
同じ項目が浮動言語バーにも出る。別のツールバーやトレイ常駐アプリは作らない。

- 項目: `clsidService` は TIP の CLSID、`guidItem` は `GUID_LBI_INPUTMODE`、スタイルは
  `TF_LBI_STYLE_BTN_BUTTON | TF_LBI_STYLE_SHOWNINTRAY`。本体は `tsf-tip/src/LangBarInputModeItem.cpp`。
- 追加: `ActivateEx` で compartment を購読した後、thread manager の `ITfLangBarItemMgr::AddItem` に渡す。
  `ITfLangBarItemMgr` を持たないホストと `AddItem` の失敗では項目を出さず、有効化は失敗させない
  （失敗は `langbar_item_add_failed` を警告ログに出す）。
- 削除: `Deactivate` で `RemoveItem` を呼び、TIP の参照を解放する。言語バーが項目を持ち続けても、
  無効化後のクリックは何もしない。
- TIP 登録（HKLM のカテゴリ）は変えない。`GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT` を登録した TIP は、
  `GUID_TFCAT_TIPCAP_SYSTRAYSUPPORT` なしでもトレイの表示対象になる。`GUID_TFCAT_TIPCAP_INPUTMODECOMPARTMENT`
  は登録しないので、OS が conversion mode から別のモード表示を出すことはない。

表示の規則:

| 状態 | アイコンと `GetText` | ツールチップ |
|---|---|---|
| On、ひらがな入力 | あ | `azooKey: ひらがな` |
| On、英数モード | _A（A に下線） | `azooKey: 半角英数` |
| Off | A | `azooKey: 直接入力` |

英数モードは IME On のままキーを素通しする状態なので、Off と見分けられるよう下線を付ける。

- アイコンはリソースを持たず、`GetIcon` のたびに GDI で文字を描いて作る。大きさは 96 DPI で 16x16 とし、
  システムの DPI に合わせて拡大する。背景は透明で、文字の色はタスクバーのテーマに合わせる。
  HKCU `Software\Microsoft\Windows\CurrentVersion\Themes\Personalize` の `SystemUsesLightTheme` が
  0 以外なら黒、0・値が無い・読めないときは白（ダークのタスクバー）とする。
- 更新の契機: OPENCLOSE compartment の変化、英数モードの切り替え、開閉キー、フォーカスの移動。表示が変わったときだけ
  `ITfLangBarItemSink::OnUpdate(TF_LBI_ICON | TF_LBI_TEXT | TF_LBI_TOOLTIP)` で通知する。
- 左クリック（`TF_LBI_CLK_LEFT`）: 半角/全角と同じく On/Off を切り替える。ユーザーの開閉として扱い、§4.1 の
  1 秒以内の On への戻しの対象にしない（`source` は `langbar_click`）。Off にするときは `VK_IME_OFF` と同じく表示中の文字列を確定する（§4.2）。
- 右クリック（`TF_LBI_CLK_RIGHT`）: TIP がポップアップメニューを出す（下記）。

右クリックのメニュー:

`TF_LBI_STYLE_BTN_MENU` の項目では言語バーが `InitMenu` / `OnMenuSelect` を呼び、`OnClick` を呼ばない
（Microsoft Learn「Language Bar (Text Services)」）。左クリックの On/Off を残すため、スタイルは
`TF_LBI_STYLE_BTN_BUTTON` のままとし、`OnClick(TF_LBI_CLK_RIGHT)` で TIP が `TrackPopupMenuEx`
（`TPM_RETURNCMD`）を呼ぶ。メニューは TIP のスレッドに作った非表示の窓を持ち主とし、クリック位置に、
項目の矩形を避けて出す。メニューの間は TIP のスレッドでモーダルループが回るので、閉じた後に TIP が
無効化されていたら選択は捨てる。

| 項目 | 表示 | 選んだとき |
|---|---|---|
| ひらがな | ラジオ。今の表示（上の表）が「あ」なら選択 | Off なら On にする。英数モードなら解除する |
| 半角英数 | ラジオ。「_A」なら選択 | Off なら On にしてから英数モードにする。英数キー（`ToggleAlnum`）と同じく、表示中の入力を確定できなかったら切り替えない |
| 直接入力 | ラジオ。「A」なら選択 | 左クリックの Off と同じく表示中の文字列を確定して Off にする（`source` は `langbar_menu`） |
| ライブ変換 | `liveConversion` の今の値でチェック | 値を反転して settings.json に書き、Host に `UpdateConfig` を送る |
| 予測 | `predictionEnabled` の今の値でチェック | 値を反転して settings.json に書く。`UpdateConfig` は送らない |
| 設定を開く | 常に有効 | §6 の `ITfFnConfigure::Show` と同じく `LaunchSettingsApplication` で設定アプリを開く（日本語の言語 ID と TIP のプロファイル） |

入力モードの 3 項目は設定ファイルに触れない。

- 今の値: ライブ変換と予測のチェックは、TIP が settings.json から読んでいる値（`TipLocalSettings`）を出す。
  アプリごとのプロファイル（`profilesByApp`）は表示にも書き込みにも使わない。
- 書き込み: settings.json の該当するトップレベルのキーだけを書き、他のキーはディスク上のまま残す。
  設定アプリと Host と同じファイルロック（`AcquireExclusiveFileLockForPath`）を取って読み、atomic に
  置き換える（`WriteTextFileAtomically`）。ファイルが無ければそのキーだけのファイルを作る。JSON の
  オブジェクトとして読めないファイルは書き換えず、隔離もしない（設定アプリが開いたときに扱う）。
  ロックは 500 ms 待って取れなければ書かない。結果は `langbar_menu_setting_write` にキーと結果の名前だけを出す。
- 反映: どの TIP インスタンスも settings.json の監視で新しい値を読む。Host が読むのは `liveConversion`
  （動的句読点の入口）だけなので、`UpdateConfig` はライブ変換を切り替えたときだけ、モデル再試行と同じ短命の
  control 接続で送る（Host は `UpdateConfig` でモデルも読み直す）。
- 設定アプリとの同時編集: 設定アプリは保存のとき、ユーザーが変えたキーだけをロックの下でディスク上の
  ファイルへ書く。設定アプリを開いたままメニューで切り替えても、その変更は設定アプリの保存で消えない。
  ただし開いている設定アプリの画面は古い値のままで、同じキーを設定アプリでも変えて保存すると設定アプリの値が残る。
- 書き込まない場面: 管理者・低 IL・AppContainer のプロセス（一括校正の画面と同じ判定、rich-features-spec
  X-3-6）と、settings.json の場所が分からないときは、ライブ変換と予測をグレーアウトする。入力モードと
  「設定を開く」は使える。

アイコンの見え方（ライト／ダーク、DPI）、Windows 11 での表示、クリックの挙動、タスクバーの入力インジケーターが
右クリックを `OnClick` に渡すことと、メニューの表示位置と閉じ方は実機で確かめる（Human Gate）。

## 5. 複合 DisplayAttribute (M23)

### 5.1 文節ごとの色分け

変換中の preedit を、文節（segment）ごとに異なる属性で表示：

| 属性 | 用途 | 想定する描画 |
|---|---|---|
| Focused | 注目文節（カーソルがある文節） | 青背景（太い実線下線） |
| Converted | 変換済み文節 | 実線下線 |
| Unconverted | 未変換文節 | 点線下線 |

色は provider から指定しない（§5.6）。
「想定する描画」は、`bAttr` に応じてアプリが選ぶ配色の典型である。

### 5.2 GUID 定義

`tsf-tip/include/azookey/tsf/DisplayAttribute.h` に定義する：

```cpp
inline constexpr GUID kFocusedSegmentAttributeGuid     = {0x06c5c71c, ...};
inline constexpr GUID kConvertedSegmentAttributeGuid   = {0x74ebd680, ...};
inline constexpr GUID kUnconvertedSegmentAttributeGuid = {0xa46c0379, ...};
```

`DisplayAttributeKind`（`Input` / `FocusedSegment` / `ConvertedSegment` / `UnconvertedSegment`）が
GUID と `TF_DISPLAYATTRIBUTE` の対応を持つ。
`EnumDisplayAttributeInfo` はこの順で 4 エントリを返し、M3 の入力属性が先頭に来る（§5.6）。
`GetDisplayAttributeInfo` は 4 つの GUID のいずれにも応じる。

### 5.3 Property 設定

EditSession は、表示中の文節から `SegmentAttributeRange`（UTF-16 単位の開始位置、長さ、属性）
の列を作り、文節ごとに composition range を clone して `GUID_PROP_ATTRIBUTE` を設定する。

```cpp
for (const auto& segment : ranges) {
    ITfRange* range = nullptr;
    composition->Clone(&range);
    range->Collapse(ec, TF_ANCHOR_START);
    range->ShiftEnd(ec, segment.start + segment.length, &shifted, nullptr);
    range->ShiftStart(ec, segment.start, &shifted, nullptr);
    property->SetValue(ec, range, &atom_of(segment.kind));
}
```

属性の割り当ては、注目文節が Focused、それ以外は候補を表示している文節が Converted、
読みのまま表示している文節が Unconverted である。
長さ 0 の文節には range を作らない。

アトムは `ITfCategoryMgr::RegisterGUID` で 4 つまとめて取得し、TextService インスタンス内に保持する。

### 5.4 文節境界

文節は一括変換の結果（`ipc::BatchConversionSegment`）である。
segment は区間を持たないので、各文節が表示している文字列（選択中の候補の `surface`、
候補を選んでいなければ `reading`）の UTF-16 長を先頭から積み上げて境界を決める。
この文字列は preedit 全体の表示文字列を作る規則と同じであり、区間の和が composition の長さと
一致しないときは文節属性を使わず §5.6 の fallback にする。

### 5.5 受け入れ条件

- 「nihongo」入力 → Space で候補表示 → カーソル位置の文節が青背景
- 矢印キー（Left/Right）で文節を移動できる（カーソル位置によって色が動く）

provider は色を指定しないため、「青背景」はアプリが `TF_ATTR_TARGET_CONVERTED` を描く配色である。
アプリごとの見え方は実機チェックリストの D-11 で確認する（§5.7）。

### 5.6 M3 の単一 DisplayAttribute との対応関係

M3（`plans/windows-port-roadmap.md` M3）では TIP が公開する表示属性は 1 つで、
EditSession が composition 全体へ `GUID_PROP_ATTRIBUTE` を 1 回 `SetValue` する
（`tsf-tip/src/TextService.cpp`、属性定義は `tsf-tip/src/DisplayAttribute.cpp`）。

| 項目 | M3 の値 |
|---|---|
| GUID | `kInputAttributeGuid`（`tsf-tip/include/azookey/tsf/DisplayAttribute.h`） |
| `bAttr` | `TF_ATTR_INPUT`（未変換） |
| `lsStyle` / `fBoldLine` | `TF_LS_SOLID` / `FALSE` |
| `crText` / `crBk` / `crLine` | いずれも `TF_CT_NONE`（アプリ既定の描画に委ねる） |
| 適用単位 | composition 全体（文節を区別しない） |

通常の preedit 更新では、composition 全体へ `SetText` と `GUID_PROP_ATTRIBUTE` の
`SetValue` を適用した後、その range を `Clone` する。
clone した selection 用 range を `TF_ANCHOR_END` へ collapse し、`SetSelection` で
キャレットを末尾へ移す。
表示属性用 range 自体を collapse すると下線の適用範囲が失われるため、selection 用 range
と共有しない。

M23 の 3 属性（§5.1）と M3 の属性は次のように対応する。`bAttr` は
[`TF_DA_ATTR_INFO`](https://learn.microsoft.com/windows/win32/api/msctf/ne-msctf-tf_da_attr_info)
の定義（`TF_ATTR_TARGET_CONVERTED` = 選択された変換済み、`TF_ATTR_CONVERTED` =
変換済み、`TF_ATTR_INPUT` = 未変換）に従って割り当てる。

| M23 属性 | 用途 | `bAttr` | 描画（§5.1） | M3 での扱い |
|---|---|---|---|---|
| Focused | 注目文節 | `TF_ATTR_TARGET_CONVERTED` | 太い実線下線（アプリにより青背景） | 区別しない（全体が未変換扱い） |
| Converted | 変換済み文節 | `TF_ATTR_CONVERTED` | 実線下線 | 区別しない（同上） |
| Unconverted | 未変換文節 | `TF_ATTR_INPUT` | 点線下線 | M3 の属性が担う（実線・色指定なし） |

移行規則と fallback:

- `kInputAttributeGuid` は M23 実装後も残し、**文節情報が使えないときの fallback
  属性**として composition 全体に適用する。`EnumDisplayAttributeInfo` は M3 の 1
  エントリ + M23 の 3 エントリ = 4 エントリを返す。
- fallback を使う条件は、一括変換の文節を候補ウィンドウで表示していないこと
  （入力中の読み、ライブ変換の preedit、絵文字入力、core 経由の preedit を含む）、
  または文節の区間の和が composition の長さと一致しないこと。
  ライブ変換の preedit は文節情報を持たないため、常に fallback 属性で表示する。
- 文節ごとの `SetValue`（§5.3）に 1 つでも失敗したとき、または文節属性のアトムを
  取得できないときは、composition 全体を fallback 属性で塗り直し、色が文節間で
  不揃いなまま残らないようにする。

アプリ差についての前提: 多くのアプリは provider が指定した色をそのまま使わず、
`bAttr` を見て自前の配色で描く。したがって色指定は「無視されうる」前提で決め、
文節の区別は `bAttr` の選択で担保する。M3 が色を `TF_CT_NONE` にしているのは
この前提に沿った選択であり、M23 で色を足すときも `bAttr` だけで区別が付く
組み合わせを崩さない。
M23 の 3 属性も色を `TF_CT_NONE` とし、`bAttr` と下線の線種・太さだけで区別する。
Dark テーマやハイコントラストでの配色はアプリの描画に任せる。

| 属性 | `bAttr` | `lsStyle` | `fBoldLine` |
|---|---|---|---|
| Input（fallback） | `TF_ATTR_INPUT` | `TF_LS_SOLID` | `FALSE` |
| Focused | `TF_ATTR_TARGET_CONVERTED` | `TF_LS_SOLID` | `TRUE` |
| Converted | `TF_ATTR_CONVERTED` | `TF_LS_SOLID` | `FALSE` |
| Unconverted | `TF_ATTR_INPUT` | `TF_LS_DOT` | `FALSE` |

回帰テストは `tsf-tip/tests/display_attribute_test.cpp`（属性値の定義、
enumerator の `Next` / `Skip` / `Clone` 境界、clone の属性コピー、文節の区間と属性の割り当て、
文節ごとの `SetValue` と失敗時の fallback）が担う。
enumerator の境界テストは属性数（`kDisplayAttributeKindCount`）を基準に書く。

### 5.7 アプリ互換チェックリスト（M3・実機 Win11）

`bAttr` の解釈と下線の描画はアプリ実装に依存するため、代表アプリでの実測が要る。
**確認手順・対象アプリ・確認項目・合格条件（安定仕様）は
`compat-test/m3_display_attribute_checklist.md`** に定める。実機 Win11 が必要な
ため実走は DEV-365（`gate:human-required`）で行い、**アプリ別の実測結果（可変ログ）
は docs ではなく DEV-365（Linear）に記録する**（`AGENTS.md` の状態 Linear 一本化方針）。
M23 の文節ごとの属性とマウスクリックによる注目文節の移動も、同チェックリストの項目として確認する。

## 6. ITfFnConfigure (M30 連動)

### 6.1 目的

Windows 設定の「言語と地域 → IME → オプション → 詳細設定」から
設定アプリを起動できるようにする。

### 6.2 実装

`tsf-tip/include/azookey/tsf/TextService.h` / `tsf-tip/src/TextService.cpp` の `TextService` が `ITfFnConfigure` を直接実装する。
`GetDisplayName` は `azooKey Settings` を返し、`Show` は `langid` と `profile` を
`tsf-tip/src/SettingsLauncher.cpp::LaunchSettingsApplication` へ渡す。`TextService::QueryInterface`
は `IID_ITfFnConfigure` に加え、その基底インターフェイスである `IID_ITfFunction` にも同一の
COM identity を返す。

`LaunchSettingsApplication` は `--langid 0xNNNN --profile {GUID}` を生成し、§6.4 の固定パスを
`ShellExecuteExW` で非同期起動する。起動 API が失敗した場合は `GetLastError()` を HRESULT へ
変換し、エラー値が設定されていない場合も `E_FAIL` を返す。成功時に得た process handle は
直ちに閉じる。校正ビュー（`docs/rich-features-spec.md` X-3-6）は同じ固定パスを
`LaunchSettingsApplicationForProofread` で起動し、引数は `--proofread-session <token>` だけとする。

> **重要: `kTextServiceClsid` の COM オブジェクトが `ITfFnConfigure` を QI で返すこと**:
> 言語/IME 設定は `CoCreateInstance(kTextServiceClsid, IID_ITfFnConfigure)` でこの機能を取得する
> （Microsoft Learn: ITfFnConfigure は「`ITfInputProcessorProfiles::Register` に渡した CLSID」+
> `IID_ITfFnConfigure` で CoCreateInstance される）。したがって **`DllGetClassObject` / class
> factory が `kTextServiceClsid` 用に生成する TextService オブジェクト自身が `ITfFnConfigure`
> を実装し、`QueryInterface(IID_ITfFnConfigure)` で返す**必要がある。別クラスへ分離する場合も
> TextService の QI 経由で到達できなければならない。さもないと設定側が `E_NOINTERFACE` を
> 受け取り、「詳細設定」が開かない。本実装は TextService が `ITfTextInputProcessorEx` と併せて
> `ITfFnConfigure` を継承し、`Show` を直接実装する。

> **`Show` を非同期にする理由（`docs/sideload-packaging-spec.md` §3.5 と整合）**:
> `ITfFnConfigure::Show` の Remarks は「ダイアログを閉じるまで return しない」（短命な
> モーダル プロパティ シートを想定）だが、本 IME の設定 UI は WinUI 3 の独立した**長命**
> プロセスである。`Show` を `azookey_settings.exe` 終了までブロックすると、呼び出し元
> （言語/IME 設定 UI）をその間フリーズさせるため、**起動後ただちに `S_OK` を返す
> 非同期方式**を採る（`WaitForSingleObject` で終了待ちしない）。設定値の反映はプロパティ
> シートの OK/Apply ではなく `UpdateConfig` IPC（§3）で行う。多重起動を避けるため設定
> アプリは single-instance とし、既存インスタンスがあれば前面化する
> （`SEE_MASK_NOCLOSEPROCESS` で得たプロセスハンドルは起動成功を確認した後すぐ閉じ、
> 既存インスタンスの前面化は設定アプリ側の single-instance 処理に委ねる）。

### 6.3 追加 category は不要

`ITfFnConfigure` の公開に追加の TSF category 登録は不要である。
Windows は `ITfInputProcessorProfiles::Register` に登録された TextService の CLSID を
`IID_ITfFnConfigure` と組み合わせて `CoCreateInstance` する。
そのため、TextService の `QueryInterface` が `ITfFnConfigure` と基底の `ITfFunction` を返す
契約をテストする。

`GUID_TFCAT_TIP_PROPERTY_UI_TEXT_SERVICE` という識別子は Windows SDK の定義済み category に
存在しないため、登録処理へ追加しない。

### 6.4 設定アプリのパス解決

`tsf-tip/src/SettingsLauncher.cpp` は、ロード済み TIP DLL の絶対パスを基準に
`azookey_settings.exe` を解決する。MSI は両者を同じ install root へ配置するため、
ユーザー書き込み可能な `%LOCALAPPDATA%` や、呼び出し元プロセスの current directory を
含み得る `PATH` へ fallback しない。隣接ファイルが存在しなければ
`HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)` を返す。モジュールパス取得やファイル属性の
確認自体が失敗した場合は、その Windows error を HRESULT として保持し、アクセス拒否などを
「ファイル不在」へ潰さない。

### 6.5 起動ログ

`TextService::Show` は起動に成功したとき `settings_launch`（Info）、失敗したとき
`settings_launch_failed`（Warn）を TIP runtime log に記録する。両イベントは `hr` と `langid`
を含む。これにより、`Show` が呼ばれていない状態と、パス解決またはプロセス起動に失敗した状態を
ログ上で区別する。

## 7. ITfMouseSink (M23)

### 7.1 目的

Preedit テキスト上でマウスクリックされた箇所を「注目文節」に切替える。

### 7.2 実装

TextService は `ITfMouseSink` を実装する。
EditSession が composition の文字列を更新するたびに、対象 context を QI した
`ITfMouseTracker::AdviseMouseSink(composition range, this, &cookie)` で composition range を
監視し直す。
composition が終わったとき（確定、空にした更新、`OnCompositionTerminated`、フォーカス喪失などの
lifecycle 後始末）は `UnadviseMouseSink` で解除する。
`ITfMouseTracker` を返さない context では何もしない。

```cpp
STDMETHODIMP TextService::OnMouseEvent(ULONG uEdge, ULONG uQuadrant,
                                       DWORD dwBtnStatus, BOOL* pfEaten) {
    *pfEaten = FALSE;
    if (!(dwBtnStatus & MK_LBUTTON)) return S_OK;
    size_t index = 0;
    // 直前の EditSession が文節属性を当てた文節の並び（attributed_segments_）で判定する
    if (!SegmentIndexAtMouseEdge(attributed_segments_, uEdge, uQuadrant, &index))
        return S_OK;  // 一括変換の文節を表示していない
    *pfEaten = TRUE;
    if (index != focused) FocusBatchSegment(index);  // 矢印キーと同じ処理
    return S_OK;
}
```

`uEdge` は composition 先頭からの文字境界で、`uQuadrant` の 0 と 1 は境界の手前、2 と 3 は
境界の後ろを表す。
手前ならその境界の直前の文字、後ろなら直後の文字を含む文節を選ぶ。
composition の末尾より後ろは最後の文節とする。
`uEdge` は描画済みの composition に対する位置なので、判定には内部状態から作り直した文節ではなく、
直前の EditSession が属性を当てた文節の並びを使う。
`OnMouseEvent` から要求した preedit 更新が同期で走っても、配送中の sink を解除しないよう、
その EditSession では監視を張り直さない（注目の移動では表示文字列が変わらないため）。

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
| ReconversionFunction | `tsf-tip/tests/reconversion_function_test.cpp` | Windows 限定。QueryRange / GetReconversion / Reconvert |
| Configure 起動 | `tsf-tip/tests/configure_test.cpp` | Windows 限定。Show が EXE を起動するか |
| KeyMap 互換 | `tsf-tip/tests/keymap_test.cpp` | Windows 限定。VK_NONCONVERT 等の挙動。開閉キーの分類（§3.1、§3.1.1） |
| 開閉キーと入力インジケーター（§3.1、§4.1、§4.5） | `tsf-tip/tests/activate_uiless_test.cpp` | Windows 限定。`VK_IME_ON` / `VK_IME_OFF` と `imeOnOff` の変換・無変換、初回フォーカス直後の Off、言語バー項目の追加・削除・通知・クリック |
| 言語バー項目（§4.5） | `tsf-tip/tests/langbar_input_mode_test.cpp` | Windows 限定。表示の規則、文字の色、アイコンの大きさと透明度、sink への通知、クリックの受け渡し |
| CandidateListUIElement | `tsf-tip/tests/ui_element_test.cpp` | Windows 限定。GetString/GetCount/SetSelection |
| Key / composition / preedit | `tsf-tip/tests/onkeydown_preedit_test.cpp` | Windows 限定。OEM 句読点と記号、候補選択 surface、末尾 selection、Backspace と Esc の状態遷移 |
| DisplayAttribute（§5） | `tsf-tip/tests/display_attribute_test.cpp` | Windows 限定。4 属性の定義と enumerator の Next / Skip / Reset / Clone。clone が位置と属性定義を複製すること。文節の区間と属性の割り当て、文節ごとの `SetValue` と fallback |
| Preedit クリック（§7） | `tsf-tip/tests/onkeydown_preedit_test.cpp` | Windows 限定。クリック位置の文節への注目移動、左ボタン以外と文節がないときに食わないこと |

## 10. 参照

- TSF SDK: `Microsoft.Windows.SDK.Cpp` の `msctf.h`
- MS-IME 互換仕様（公式ドキュメント）: <https://learn.microsoft.com/windows/win32/tsf/>
- 旧 macOS 実装の対応箇所：`legacy/azooKeyMac/InputController/azooKeyMacInputController.swift`
- Phase 6-B: `docs/copilot-pc-backend-spec.md`
- Phase 6-C: `docs/native-ui-spec.md`
