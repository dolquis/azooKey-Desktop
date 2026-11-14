---
kind: spec
milestones: [M3, M5, M20, M21, M22, M23, M64]
canonical_for: ["TSF 深部統合"]
---

# TSF Deep Integration 仕様（Phase 6-A）

本書は Microsoft IME（MS-IME）と同等の TSF 統合を実現するための仕様を定める。
`plans/windows-port-roadmap.md` の Phase 6 の M20〜M23 が本書を参照する。

## 0. 節とマイルストーンの対応

本書の節と、その範囲を定める `plans/windows-port-roadmap.md` のマイルストーンの対応を次に示す。

| 節 | マイルストーン |
|---|---|
| §1 | M20 |
| §2（§2.0・§2.8〜§2.11 を除く） | M21 |
| §2.0、§2.8〜§2.11 | M5（UI-less の最小契約）。M21 がこれを拡張する |
| §3（§3.6 を除く） | M22 |
| §3.6 | M64 |
| §4 | M21 |
| §5（§5.6・§5.7 を除く） | M23 |
| §5.6・§5.7 | M3（単一 DisplayAttribute）。§5.6 は M23 との対応も定める |
| §6 | M11（設定アプリの起動）・M30 |
| §7 | M23 |
| §8 | M15・M21 |
| §9〜§10 | 共通 |

## 1. ITfReconversion / ITfFnReconversion (M20)

### 1.1 目的

既に確定済みのテキストに対して、「再変換」操作を提供する。
ユーザーが「明日」を「あした」と再変換したい、等。

> **参考（fkunn1326/azooKey-Windows, MIT）**: 周辺テキスト取得には、transitory context
> （検索ボックス等）の親コンテキストを `GUID_COMPARTMENT_TRANSITORYEXTENSION_PARENT` で
> 辿る手法が有効（先行実装の `surrounded_text` が mozc `tip_surrounding_text.cc` を参照して
> 実装）。再変換時の周辺文字列取得や、Zenzai への前方文脈供給（`SetContext`）に流用できる。
> 原典は mozc（C++）であり、実装時は原典に当たること。

### 1.2 インターフェース実装

`ReconversionFunction`（`tsf-tip/include/azookey/tsf/ReconversionFunction.h` /
`tsf-tip/src/ReconversionFunction.cpp`）が `ITfFnReconversion` を実装する。宣言の正典はヘッダで、
本書はコードを写さない。`ITfFunction::GetDisplayName` と、`ITfFnReconversion` の `QueryRange` /
`GetReconversion` / `Reconvert` を持ち、変換キーの入口と、アプリのコールバックの外から候補を
非同期に作る入口（下記 `GetReconversion` の edit session）を TIP 向けに加える。

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

IPC は `ReverseConvert` を使う。payload の正典は `ipc/include/azookey/ipc/Payloads.h` の
`ReverseConvertRequest` / `ReverseConvertResponse` である。本節は
`docs/windows-tsf-host-architecture.md`「IPC メッセージ」の索引から参照され、次の意味を定める。

| フィールド | 意味 |
|---|---|
| 要求 `surface` | 逆引きする表層（再変換の範囲のテキスト） |
| 応答 `reading` | 上の順序で得た最尤の読み。どこにも当たらなければ空 |
| 応答 `confidence` | 読みの信頼度。`reading` が空のときは 0 |

#### Reconvert

- 元の選択文字列と同じ候補を確定した場合、その文字列を変更しない
- Host 候補の確定は、対象の選択範囲と context が一致する場合に限って置換する
- 候補選択には azooKey の候補 UI を使う
- 候補 UI での確定（Enter・数字キー・クリック）は `TF_ES_ASYNCDONTCARE` の edit session で置換する。
  クリックはアプリのコールバックの外に届き、同期ロックを拒むアプリ（Word）では後から実行されるため、
  その間の次の確定は受け付けず、実行時に選択・context・候補 UI が変わっていれば置換しない
- 置換に失敗したら候補 UI を開いたままにし、強調を Enter が確定する候補（TIP の選択位置）へ戻す

### 1.3 FunctionProvider 公開

`TextService` は `ITfFunctionProvider` を公開し、`ActivateEx` 時に
`ITfSourceSingle::AdviseSingleSink` で thread manager に登録する。
`GetFunction` から `ITfFnReconversion` を返し、`Deactivate` 時に
`UnadviseSingleSink` で解除する。Windows SDK に
`GUID_TFCAT_TIP_RECONVERSION` という category はないため、category 登録は使わない。

### 1.4 受け入れ条件

受け入れ条件の定義の正典は [`plans/windows-port-roadmap.md`](../plans/windows-port-roadmap.md)
の M20 節とする。本節は受け入れ条件を複製しない。

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
検索統合 API が無くてもスタート検索で活性化・入力は可能だが、統合表示は得られず composition 完了
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

`CandidateListUIElement`（`tsf-tip/include/azookey/tsf/CandidateListUIElement.h` /
`tsf-tip/src/CandidateListUIElement.cpp`）が `ITfCandidateListUIElement`（`ITfUIElement` を継承する）を
実装する。宣言の正典はヘッダで、本書はコードを写さない。

- `ITfUIElement`: `GetDescription` / `GetGUID` / `Show` / `IsShown`。
- `ITfCandidateListUIElement`: `GetUpdatedFlags` / `GetDocumentMgr` / `GetCount` / `GetSelection` /
  `GetString` / `GetPageIndex` / `SetPageIndex` / `GetCurrentPage`。
- `ITfCandidateListUIElementBehavior`（`SetSelection` / `Finalize` / `Abort`）は M21 の full 実装で
  加える（§2.8「M5 と M21 の役割分担」）。

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

Backspace で候補表示を閉じるときは、表示用 preedit を正典の reading
へ戻してから次の編集を反映する（ライブ変換 ON で応答を引き継いでいれば、`docs/legacy-parity-spec.md` §2.3.1 の表示にする）。追加入力（英字・記号・長音・句読点）は、表示中 `surface` を
Enter と同じく確定（学習観測も Enter と同じ）してから、そのキーで新しい composition を始める。
一括変換の複数文節では、表示中の全文節を確定する。確定の edit session が拒否されて再試行を待つ間は、
Enter の確定と同じく、打ったキーを消費して確定の後に残す。再試行を待つキーは次の規則で扱う。

- 確定を起こしたキーと、待つ間に TIP が消費したキーは、打鍵時のキー状態（修飾キー、OEM 記号と数字の解釈）と
  ともに、打った順に最大 16 個まで残す。それより後のキーは捨てる。このキューは Space の応答待ちの保留
  （`docs/legacy-parity-spec.md` §1.2「応答を待つ間のキーは保留して再生する」）と共有し、上限の 16 個は両方の
  待ちで残したキーを合わせた数とする。
- 次のキーが確定を再試行して成功したら、残したキーを打った順に処理し、最後に再試行したキー自身を処理する。
  Enter の確定、`VK_IME_OFF` などのオフ（§4.2）、候補窓からの打鍵の確定のどれでも同じに扱う。
- core が持つ読みや候補窓（`docs/legacy-parity-spec.md` §6）の表示中の Ctrl+Shift+U は、表示中の文字列の確定の後に `U+` を始める。その確定が拒否されたときも、
  このキーを待つキーとして残す。再試行の成功後、Idle で Ctrl・Shift を押した状態に戻して処理するので、
  `U+` が始まり、その後に待っていたキーが続く。再試行が成功しないまま確定待ちが終わったときは、
  このキーも捨てるので Unicode 入力は始まらない。
- そのとき TIP が受け取らないキー（確定後の空の preedit での Backspace や矢印など）は捨てる。app へは渡さない。
  再生の途中で Off になったとき（残したキーに半角/全角や `VK_IME_OFF` があったとき）、その後のキーも同じく捨てる。
- 上限を超えたキーや、キー状態を読めなかったキーを捨てたときは、待ちごとに 1 回だけ理由名をログに残す。
  キーの内容、数、時刻は残さない。
- キー以外で確定待ちが終わったとき（フォーカス喪失、Deactivate、§4.3 の lifecycle の確定、後から許可された
  edit session、入力インジケーターのクリックとメニューによる Off（§4.5））は、残したキーを捨てる。どの入力先にも表示されていない入力なので、別の入力先では再生しない。
- アプリが composition を終了した後に TIP がその入力を確定するとき（確定を待つ間の終了の後の再試行と、終了の後の
  フォーカス移動や次のキーによる lifecycle の確定のどちらでも）、確定は TIP が最後に書いた range へ書く。range に
  アプリが残した文字列がその時の表示と同じなら置き換え、アプリが書き換えた文字列は残し、range が空なら書き戻す。
  range が空でも、その直後に TIP が最後に書いた文字列が残っているとき（アプリが文字列を残したまま range だけが
  文字列の前端へ潰れたとき）は、その文字列を確定の文字列に置き換え、隣に挿入しない（DEV-1927。挿入すると同じ
  入力が 2 回入る）。見るのは range の直後だけで、range が文字列の後端へ潰れたとき（残った文字列が range の直前に
  あるとき）は検出せず、書き戻すので二重になりうる。直前の文字列はログ（`docs/debugging.md` の
  `key_path_terminated_commit`）のために読むだけで、書き換えない。
  アプリが range の文字列を本当に消し、その直後に同じ文字列がもともと書かれていた場合も、この規則で置き換わる
  （その文字列は残らない）。range の位置だけでは、この 2 つを見分けられない。
- 例外として、TIP が composition を表示したまま修飾付きの編集のキー（Shift+Delete、Shift+Insert、Ctrl+X /
  Ctrl+V / Ctrl+Shift+Z / Ctrl+Shift+Y / Ctrl+Backspace / Ctrl+Delete / Ctrl+Enter。Ctrl のキーは Shift を足した
  組合せを含む）をアプリへ渡し、次のキー、フォーカスの切り替え、composition の文字列を変えない選択の変化
  （クリックなど。TIP 自身の edit session が動かした caret は含めない）のどれよりも前にアプリが composition を
  終了したときは、range が空でも書き戻さない。アプリが
  そのキーで文字列を消したとみなし、入力を捨てる（DEV-1845）。修飾の無い Delete は、読みの末尾ではたいてい
  何もしないので含めない。range の直後に TIP が最後に書いた文字列が残っているときは、前項のとおり置き換える。
  確定を待つ間の終了は、直前のキーにかかわらず前項のとおり確定する。編集のキーがアプリで何もしなかった後に、
  キーを打たずにフォーカスを移し、アプリが選択とフォーカスの通知より前に range を空にして終了したときも、
  入力を捨てる。通知の順だけでは、この場合とアプリの編集を見分けられない。
- composition が終わらないまま、アプリが composition の文字列を変えたとき（`OnEndEdit` で、TIP が書いた直後に
  同じ edit session の中で読み戻した文字列と比べて検知する。文書が書き込みを切り詰めたり正規化したりしても、
  読み戻した文字列が基準になる）は、TIP は読み、候補窓（候補モードと予測モード）、保留中のキーを捨て、次の edit session で
  composition をアプリの文字列のまま終える。その後の Enter などは、消えた入力を確定しない。その edit session より
  前に打ったキーは、composition を終えた後の新しい入力になる。
Esc は読みへ戻らず入力を取り消す
（`docs/legacy-parity-spec.md` §1.2）。
候補の `surface` を reading バッファへ書き戻してはならない。

### 2.5 受け入れ条件

受け入れ条件の定義の正典は [`plans/windows-port-roadmap.md`](../plans/windows-port-roadmap.md)
の M21 節とする。本節は受け入れ条件を複製しない。

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

### 2.8 候補 UI 統合方針と `CandidateUiCoordinator`（v1.0 / M5）

両立方式を選んだ経緯は [`decisions/2026-06-11-candidate-ui-coexistence.md`](decisions/2026-06-11-candidate-ui-coexistence.md) に残す。

**設計判断**: 候補 UI は「`ITfCandidateListUIElement`（3-interface 契約）
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

#### API: `tsf-tip/include/azookey/tsf/CandidateUiCoordinator.h`

宣言の正典はヘッダで、本書はコードを写さない。描画責務に関わる入口の契約は次のとおり。

- `SetUiLessMode(bool)`: `ActivateEx` で得た UI-less フラグを保持する（§2.10）。
- `BeginUI(thread_mgr, pt, items, selected_idx, …)`: 候補表示を始める。内部で
  1) `ITfCandidateListUIElement` を生成して候補を持たせ、2) `ITfUIElementMgr::BeginUIElement(elem,
  &pbShow, &ui_element_id)` を呼び、3) `pbShow` で描画責務を確定する。4) `pbShow==FALSE` のときは
  続けて `UpdateUIElement(ui_element_id)` を即時に呼び、初回の候補内容をアプリへ通知する。アプリは
  `BeginUIElement` では内容を読まず、最初の `UpdateUIElement`（全 update フラグ立ち）で読み始めるため、
  これを省くと初回の候補が描画されない（§2.6 フローチャート）。`pt` は自前 HWND を出す場合の
  キャレット直下のスクリーン座標である。
- `UpdateUI(items, selected_idx)`: 候補リストか選択の変更。`pbShow==TRUE` 経路は自前 HWND を再描画し
  `UpdateUIElement` は呼ばない。`pbShow==FALSE` 経路は `UpdateUIElement(ui_element_id)` だけを呼ぶ。
- `EndUI()`: `pbShow` の値に関わらず HWND を隠し、`EndUIElement` を必ず呼ぶ。
- `IsShowing()`: 表示中かどうか。

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

#### M5 と M21 の役割分担

- **M5（v1.0）**: 上記 coordinator の骨格 + `pbShow==TRUE` 経路（自前 HWND）+
  `BeginUIElement`/`EndUIElement` 動線 + `ActivateEx` フラグ検出（§2.10）+
  カテゴリ登録（§2.9）。`pbShow==FALSE` 経路は `CandidateListUIElement` の最小実装
  （`GetCount`/`GetString`/`GetSelection`/`GetUpdatedFlags` + `UpdateUIElement`
  通知）まで。
- **M21**: `CandidateListUIElement` の full 実装（paging・`...Behavior`・
  `ITfIntegratableCandidateListUIElement` 等 §2.3/§2.7）、IME On/Off（§4）。予測
  は UIElement 化せず、自前の候補窓の予測モードで出す（§8）。M21 の役割は「UI-less mode の full 動作確認とリッチ
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

プロファイルのアイコンは、TIP DLL のパスと `uIconIndex = 0`（DLL 内で先頭のアイコン）で登録する。
TIP DLL は `tsf-tip/resources/azookey_tsf_tip.rc` で、ICON リソースを `IDI_AZOOKEY_BRAND`（ID 1）の 1 つだけ持つ。
中身は設定アプリと同じブランドアイコン `settings-app/Assets/azookey-settings.ico` で、複製せず相対パスで参照する。
入力方式の一覧と切替フライアウトは、このアイコンを表示する。§4.5 のモードアイコンとは別物である。
アイコンを増やすときは、ブランドアイコンを先頭に保つ。保てない場合は `uIconIndex` を変えて再登録する。
`tsf-tip/tests/brand_icon_test.cpp` は、ビルドした DLL を登録せずに、アイコンが 1 つであることと、index 0 から大小のアイコンを取れることを確かめる。

検証は `tsf-tip/tests/com_smoke_test.cpp` の登録 round-trip smoke で行う。
通常経路では再登録、`GetProfile`、解除を確認する。Debug ビルドでは
`AZOOKEY_TEST_FAIL_CATEGORY_REGISTRATION` によってプロファイル登録後の失敗を注入し、
最初のカテゴリを登録した後で失敗させ、残骸がないことと、そのまま再登録できることを確認する。
対話的 TSF セッションを要するため、opt-in 環境変数 `AZOOKEY_RUN_REGISTRATION_SMOKE`
+ 昇格時のみ実行で、CI（headless）では走らない。

### 2.10 `ActivateEx` の最小実装

`TextService::ActivateEx` は `ITfThreadMgrEx` を
QI して `GetActiveFlags` を呼び、`TF_TMF_UIELEMENTENABLEDONLY` の有無を
`ui_less_mode_` に保持する（`tsf-tip/src/TextService.cpp`）。`dwFlags` は UIElement
ビットを公式に列挙しないため一次情報にしない。`ui_less_mode_` は
`CandidateUiCoordinator` に伝播し、§2.8 の `pbShow` 分岐で自前 HWND と
app-drawn UIElement 経路を切り替える。

v1.0（M5）の最小実装は次のとおりである（UI-less 判定の一次情報は §2.2 のとおり
`ITfThreadMgrEx::GetActiveFlags`）。

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
`GetActiveFlags` 経由で取得でき、M46 プライバシー / セーフ入力で扱う。この判定は
COM smoke テスト（モック `ITfThreadMgrEx::GetActiveFlags` が
`TF_TMF_UIELEMENTENABLEDONLY` を返すとき `ui_less_mode_` が true になる）で
検証する。

### 2.11 アプリ互換チェックリスト（実機 Win11・`gate:human-required`）

`pbShow` の戻り値とアプリの描画責務はアプリ実装に依存するため、代表アプリでの実測が
必要。**計測手順・対象アプリ・合格条件（安定仕様）は
`compat-test/m5_uiless_pbshow_checklist.md`** に定める。実機 Win11 が必要なため DEV-97 の子課題 DEV-153
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
| `VK_DBE_HIRAGANA` (0xF2) | ひらがな（JIS のカタカナひらがなキー） | `VK_IME_ON` と同じ（IME On、英数モードならひらがなへ） |

`VK_OEM_ATTN` と `VK_DBE_ALPHANUMERIC` は同じ VK 値の別名なので、
VK だけから区別しない。

本表は TIP の最終挙動である。core の `UserActionMap` が返す値（`VK_KANJI` → `ToggleHankaku`、
`VK_NONCONVERT` → `ToggleHiraKata` など）は `docs/legacy-parity-spec.md` §1.4 が定め、開閉キーは
TIP がその写像より前に処理する。

`VK_DBE_KATAKANA`（0xF1、Shift 付きのカタカナひらがなキー）は扱わず、アプリへ渡す。Windows 版には
カタカナ入力モードが無いためで、legacy にもカタカナキーは無い。Alt 付きのカタカナひらがなキー
（ローマ字入力とかな入力の切替）も扱わない。

英数キー（`VK_OEM_ATTN`）は英数モードのトグルとし、2 回押すとひらがなに戻る。legacy の英数キーは英語への
絶対指定で、読みを残したまま英字を足すので、意図した legacy との差である。英数モードへ切り替えるときは
入力中の読みを確定する（§4.3）。ひらがなへの絶対指定には、ひらがなキーか `VK_IME_ON` を使う。

開閉キー（半角/全角、`VK_IME_ON`、`VK_IME_OFF`、ひらがなキー、`imeOnOff` のときの変換・無変換）は、
修飾キー（Shift、Ctrl、Alt、Win）が押されていないときだけ働く。IME Off と英数モードでも
TIP がキーを受け取り、アプリへは渡さない。アプリがキーボード入力を無効にした context
（§4.3）では他のキーと同じく素通しする。`VK_IME_ON` と `VK_IME_OFF` は Microsoft IME と同じ
キーで、AutoHotKey（`Send {vk16}` / `Send {vk1A}`）や PowerToys Keyboard Manager から送れる。
Alt の単独押しを TIP が検出することはしない。

- `VK_IME_ON` とひらがなキー（`VK_DBE_HIRAGANA`、DEV-1816）: Off なら On にする。On なら compartment を
  書き換えない。英数モード中なら英数モードを解除し、ひらがな入力に戻す。
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

受け入れ条件の定義の正典は [`plans/windows-port-roadmap.md`](../plans/windows-port-roadmap.md)
の M22 節とする。本節は受け入れ条件を複製しない。

F6〜F10 の字種指定確定（§3.6）の受け入れ条件は roadmap の M64 節が持つ。

### 3.5 OEM 記号キーと明示句読点

`VK_OEM_COMMA`、`VK_OEM_PERIOD`、`VK_OEM_2` と Shift 付きの `VK_1` は、現在の keyboard layout と modifier
状態を使って文字へ変換する。
日本語入力 ON で変換結果が `,`、`.`、`?`、`!` のときは、composition の有無によらず
明示句読点 `、`、`。`、`？`、`！` として preedit へ取り込む（`？` `！` は DEV-1670。legacy の全角記号入力と揃える）。
preedit がない場合は句読点単独で composition を開始する。句読点単独の変換候補は全角と半角の順
（`、` と `,`、`。` と `.`、`？` と `?`、`！` と `!`）に表示し、Space で選び Enter または候補番号で確定する。
半角候補は Host と記号リライターの有無に依存しない。Esc は通常の候補窓と同じく
入力を取り消し（`docs/legacy-parity-spec.md` §1.2）、Backspace は句読点を削除する。
その他の記号は、legacy の `KeyMap+hankaku2zenkaku.swift` と同じ次の表で全角にして preedit へ取り込む
（DEV-1811）。キーは仮想キーではなく、現在の keyboard layout と modifier 状態で変換した文字で判定するので、
JIS 配列と US 配列の違い（`@` `:` `"` `(` などの位置）は表を引く前に吸収される。

| 打鍵した文字 | 取り込む文字 | 打鍵した文字 | 取り込む文字 | 打鍵した文字 | 取り込む文字 |
|---|---|---|---|---|---|
| `"` | `”` | `#` | `＃` | `$` | `＄` |
| `%` | `％` | `&` | `＆` | `'` | `’` |
| `(` | `（` | `)` | `）` | `=` | `＝` |
| `~` | `〜` | `\|` | `｜` | `` ` `` | `｀` |
| `{` | `『` | `}` | `』` | `+` | `＋` |
| `*` | `＊` | `<` | `＜` | `>` | `＞` |
| `_` | `＿` | `-` | `ー` | `^` | `＾` |
| `\` | `＼` | `¥` | `￥` | `@` | `＠` |
| `[` | `「` | `]` | `」` | `;` | `；` |
| `:` | `：` | `/` | `・` | | |

- 読みがあるときは同じ composition の末尾へ足す。未確定ローマ字が残っている場合は先にかなへ確定する。
- 読みが無いとき（Idle）は、その記号 1 文字で composition を始める（`-` は `ー` で始まる）。半角の候補を
  足すのは上の `、。？！` だけで、他の記号の候補は読みそのものと Host の候補である。
- 候補窓の表示中は、他の文字入力と同じく選択中の候補を確定してから記号で次の composition を始める（§2.4.1）。
- 表に無い文字を生むキーと、英数モード・IME Off の記号キーは、半角のままアプリへ渡す。
- 次の処理はこの表より先に、この順で行い、取り込まれた文字はその処理に従う。
  1. 絵文字検索の `:` トリガ（`docs/candidate-rewriter-spec.md` §11）。
  2. カッコ補完が有効なアプリのカッコ（`docs/bracket-pairing-spec.md` §3.1）。ただし、未確定の `n` の後の `'` は
     4 へ譲る。
  3. カスタムローマ字表（`inputStyle = custom`）が続けられる文字。
  4. `n` の後のアポストロフィ（下記）。
- 一括変換（`docs/romaji-batch-conversion-spec.md`）の raw 入力には打鍵した ASCII の文字を保持し、
  かな preview と変換の読みでは表の全角文字にする（句読点と同じ）。raw romaji preview の表示で全角にするのは
  下の `、。？！` だけで、他の記号は打鍵した ASCII のまま見せる。全角化はローマ字表を通した後の文字列に掛けるので、
  カスタムローマ字表の出力に含まれる表の記号も全角になる。raw 入力は ASCII しか持たないため、テンキーの `.` `/`
  も主キーと同じく `。` `・` になり、ASCII の形を持たない `¥` は一括変換が有効なときはアプリへ渡す。
- JIS 配列の ¥ キーは `ToUnicodeEx` で `\`（U+005C）を生むが、legacy と同じく `￥` として取り込む。ろキー
  （`VK_OEM_102`）と US 配列の `\` キーも同じ文字を生むので、¥ キーは `VK_OEM_5` かつスキャンコード `0x7D`
  （キーメッセージに無いときは現在の配列で `VK_OEM_5` を写したもの）で見分け、それ以外の `\` は `＼` とする。
- 読みに取り込んだ数字と記号は、`-`（ー）と `、。？！` を除いて M60 の打鍵のローマ字に数えないので、それらを
  含む読みでは `raw_romaji` を送らない（`docs/inline-english-candidate-spec.md` §4.1・§6.4。テンキーの `.` `/`
  を含む）。
- Shift で数字を生む配列（AZERTY など）でも、Shift 付きの数字キーは数字として取り込まない。

batch 変換の raw 入力には ASCII の `,`、`.`、`?`、`!` を保持するが、かな preview と raw romaji
preview の表示と変換の読みでは明示句読点として `、。？！` へ変換する。
この変換はカスタムローマ字表の出力に含まれる `,` `.` `?` `!` にも同じく掛かる。

Shift 付きの数字キーは、候補窓の表示中も候補番号にしない（Shift なしの `1`〜`9` だけが候補番号）。
Shift 付きの `VK_1` は `!` を生む配列でだけ上記の `！` として取り込み、他の Shift 付き数字キーは
生んだ文字が上の表にあれば全角で取り込み、無ければアプリへパススルーする。

Shift なしの数字キー（主キーとテンキー）は、読みがあるとき半角数字として読みに足す。`numberRewriter` の
有無はこの取り込みを変えない。読みが無いときの数字キーは、`numberRewriter` が ON で一括変換ではないときだけ
従来どおり composition を始め、それ以外はアプリへ渡す（`docs/legacy-parity-spec.md` §1.5.3、
`docs/candidate-rewriter-spec.md` §3）。
候補窓の表示中の数字キーは §1.4 の表（`docs/legacy-parity-spec.md`）に従う。

テンキーの記号キーは、読みがあるときだけ取り込み、読みが無いときは数字キーと同じくアプリへ渡す（表計算の
式の入力を変えない。legacy は Idle からも入力を始めるので、legacy との差である）。テンキーの VK は上の表より
前に VK で判定し、表は引かない。取り込む文字は legacy と同じく、`VK_ADD` は `＋`、`VK_SUBTRACT` は `ー`、
`VK_MULTIPLY` は `＊`、`VK_DECIMAL` は半角の `.`、`VK_DIVIDE` は半角の `/` とする（`3.14` のような数字の小数点と
区切りを全角にしないため）。英数モードと IME Off ではアプリへ渡す。
preedit に取り込んだ記号は通常の Backspace と同じ削除対象とし、アプリ本文と IME 内部に
編集対象を分裂させない。

アポストロフィ `'` は、内蔵ローマ字表で未確定ローマ字が `n` か `nn` のときだけ composition へ取り込み、
「ん」の区切りとして消費する（`docs/legacy-parity-spec.md` §5.4）。キーは仮想キーではなく、
現在の keyboard layout と modifier 状態で変換した文字が `'` かどうかで判定する（US 配列は
`VK_OEM_7`、JIS 配列は Shift 付きの `VK_7`）。この判定は `docs/bracket-pairing-spec.md` の
対称引用符の対より先に行う。未確定の `n` が無いとき、composition が無いとき、カスタムローマ字表の
ときは、`'` を区切りに使わない。その場合は上の処理順のとおり、カッコ補完の対称引用符が有効ならその対を入れ、
そうでなくカスタム表が続けられればその規則に従い、どちらでもなければ上の表で `’` として読みに取り込む
（英数モードではアプリへ渡す）。
batch 変換では raw 入力に `'` を保持し、表示と変換の読みは同じ規則で「ん」に区切る。

`docs/dynamic-punctuation-spec.md` の自動句読点は、ユーザーが明示的に押したキーの
字種と状態遷移を扱わないため、本節の契約とは別機能である。

#### 3.5.1 句読点キーで変換して確定する（`punctuationConversion`）

設定 `punctuationConversion`（enum `off` / `commit`、既定 `off`、DEV-1638）が `commit` のとき、
`、`、`。`、`？`、`！` のキー（§3.5）は、直前の読みを変換して句読点と一緒に確定する（`？` `！` は DEV-1670）。
`off` では §3.5 の挙動を変えない。
候補窓の表示中に `off` でこれらの句読点を押したときは、他の文字入力と同じく選択中の候補を確定し、
句読点で新しい composition を始める（§2.4.1、`docs/legacy-parity-spec.md` §1.2）。

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
  上限を過ぎた場合と Host が使えない場合は `off` と同じく句読点を読みに足す。
  未変換の読みを強制確定することはない。
- Space の応答待ち（`docs/legacy-parity-spec.md` §1.2）に押した句読点は、この待ちに入らず、Space の応答待ちの
  保留に積む。窓が開いた後に `Selecting` へ再生され、上の候補窓の表示中の規則で確定する。候補が 0 件で窓が
  開かなかったときは、再生した時点の規則で扱う。
  フォーカス喪失、composition の終了、Deactivate では、Enter の保留（`docs/dynamic-punctuation-spec.md` §7.3）と同じく、待っている句読点キーを捨てる。
- 次の場合は変換せず、`off` と同じく句読点を読みに足す。
  - 読みが空、または上記の句読点だけ（§3.5 の半角候補を残す）。
    その候補窓の表示中に押した句読点は、選択中の候補を確定して新しい composition を始める（§2.4.1）。
  - `Selecting` 以外で、読みの末尾が数字（`3.14`、`1,000`）。
  - 一括変換（`docs/romaji-batch-conversion-spec.md`）の蓄積中、絵文字検索中、括弧入力中。
- 数字リライターが ON でも、`Selecting` 以外の句読点キーは core の状態機械が扱う。
  数字リライターの候補窓（legacy 経路）の表示中は対象外とする。
- 学習観測は、句読点を除いた読みと候補だけを送る（`docs/privacy-and-secure-input-spec.md` §5 の抑止に従う）。
  ライブ変換の文節観測では、外した末尾の自動句読点を含めない。
  secure と private の文脈でも変換と確定は行い、観測は既存の抑止で送らない。
- その打鍵では半角の `,` `.` `?` `!` を候補から選べない。
  確定の直後に無変換キーで字種を切り替える機能（確定文字列を読みの字種へ置き換える）は、押した句読点まで置き換えるため、句読点キーの確定には働かない。
- Enter だけが待つ最終要求（`docs/dynamic-punctuation-spec.md` §7.3）は、句読点キーでは送らない。

### 3.6 字種指定確定（F6〜F10、M64）

読みがある状態（Composing、Previewing、Selecting、Space の応答待ち）で F6〜F10 を押すと、読み全体を次の字種に
変えて 1 打で確定する（DEV-1812）。修飾キー付きは対象外とする。

| キー | 確定する文字列 | 読みの数字・記号（§3.5 で取り込んだもの） | `dai3kai` の例 |
|---|---|---|---|
| F6 | ひらがな（読みそのもの） | 読みのまま | だい3かい |
| F7 | 全角カタカナ | 読みのまま | ダイ3カイ |
| F8 | 半角カタカナ | 半角形のある文字だけ半角にする（下記）。半角数字はそのまま | ﾀﾞｲ3ｶｲ |
| F9 | 全角英字 | 英字の綴り（下記）を全角にする（`3`→`３`、`[`→`［`） | ｄａｉ３ｋａｉ |
| F10 | 半角英字 | 英字の綴り（下記）のまま | dai3kai |

F8 で半角にする文字は、Unicode に半角形がある文字に限る。全角英数記号（U+FF01〜U+FF5E。`＠`→`@`、`：`→`:`、
`＊`→`*`）、`￥`→`¥`、`「」`→`｢｣`、`、。`→`､｡`、`・`→`･`、`ー`→`ｰ`、カタカナ（濁点・半濁点は分ける）を
変える。半角形の無い `’` `”` `〜` `『』` はそのまま残す。この対応は core の固定表（`BuildCommitForm`、
`core/src/CharacterFormCycle.cpp`）で持ち、`LCMapString` の `LCMAP_HALFWIDTH` の結果には依存しない。legacy（ICU の Fullwidth-Halfwidth 変換）と同じ対応を意図しており、
食い違う文字が見つかれば legacy に合わせる。

F6〜F8 は読みの字種だけを変え、F9・F10 は英字の綴りを全角・半角にする。数字と記号の扱いはこの点で legacy の
`submitHiraganaCandidate` 〜 `submitHalfWidthRomanCandidate` と同じである。ただし legacy の候補選択中の F6〜F8 は
選択中の候補が覆う部分の読み（ruby）だけを変えて確定するのに対し、Windows 版は読み全体を変える（意図した
legacy との差。Windows 版の通常変換は読み全体を確定するため）。

- 対象は表示中の候補ではなく読み全体である。未確定ローマ字は先にかなへ確定する。一括変換の文節表示中は、
  全文節の読みをつないだものを対象にする。ライブ変換の表示中も、表示中の変換結果ではなく読みを変える。
- F9・F10 の英字の綴りは、M60 の英単語候補が使う打鍵どおりの生ローマ字（大小も打鍵どおり）とする。
  一括変換では一括変換の raw 入力から取る。生ローマ字のバッファと、それが読みと対応するかの判定は
  `docs/inline-english-candidate-spec.md` §4.1 の契約をそのまま使い、この機能のために変えない。
- §4.1 の判定で生ローマ字が読みと一致しないとき（読みに数字・記号を含む、カスタムローマ字表、Backspace の後で
  対応が崩れた、読みの補正など）は、§3.2 の巡回と同じくかなから作り直したヘボン式の綴りにする。このとき
  大文字にせず小文字とし、読みに取り込んだ数字はそのまま、記号は §3.5 の表を逆に引いて打鍵した ASCII に戻す
  （`dai3kai` → `dai3kai`、`Kakko[` → `kakko[`）。legacy は数字・記号を含む読みでも打鍵どおりの綴りを出すので、
  この場合の大小（`Kakko[` → legacy は `Kakko[`）とヘボン式に直る綴り（`si` → `shi`）は legacy との差である。
  カスタムローマ字表の composition では、§4.1 の判定が一致しても生ローマ字を使わずヘボン式にする。ヘボン式で
  綴れない並び（`ふぁ` のような表に無い拗音、末尾の `っ`）を含むときは、数字・記号で区切られたかなの連続ごと
  かな 1 文字ずつ綴り、小書きのかなと `っ` には `x` を前置する（`ふぁ` → `fuxa`、`がっこうふぁん` →
  `gaxtukoufuxan`）。
- 一括変換の蓄積中は、F6〜F8 は raw 入力のかな（未確定のローマ字はかなへ確定したもの）を、F9・F10 は raw 入力を
  対象にする。一括変換ではカスタムローマ字表の composition でも raw 入力を使う。
- Unicode 入力中と絵文字検索（`docs/candidate-rewriter-spec.md` §11）の Collecting・Listing では、F6〜F10 を
  消費して何もしない。
- 学習観測は F7・F8 の確定だけを、読みと確定した文字列の組で送る（`docs/privacy-and-secure-input-spec.md` §5 の
  抑止に従う）。F6 は読みの as-is 確定と同じく送らない。F9・F10 は、英字がかなの読みの候補に混ざらないよう
  送らない。legacy はすべての字種を学習するので、意図した legacy との差である。
- 読みが無いときの F6〜F10 はアプリへ渡す。Idle の F10 は Debug ビルドだけデバッグ窓を開閉する
  （`docs/legacy-parity-spec.md` §8.1）。F10 の preserved key は読みがある間はデバッグ窓に使わず、キーを
  字種指定確定へ回す。
- F10 はアプリによってシステムキー（`WM_SYSKEYDOWN`、メニューバーの起動）として扱われ、TSF のキーイベント
  シンクに届かないホストがありうる。製品ビルドは F10 の preserved key を登録しないので、そのホストでは読みが
  ある間の F10 が効かない（制限）。preserved key の登録は変えない。
- 英数モードと IME Off では扱わない。
- Microsoft IME は F6〜F10 で確定せずに字種を巡回するが、Windows 版は legacy と同じく 1 打で確定する。
  確定した後に字種を変えるには §3.2 の無変換キーを使う。F6〜F10 で確定した後の巡回は、確定した字種の次から
  始まる（F7 の「カンジ」の後の無変換は「ｶﾝｼﾞ」、F10 の後はひらがな）。F6〜F10 以外の確定の後の巡回は変えない。
- 確定は TIP が行い、core の経路（`InputState`）と TIP の前処理の経路（一括変換、数字リライター、リライターの
  Space の候補窓）で同じ処理を通す。`InputState` は F6〜F10 の `UserAction` を扱わず、確定の後に Idle へ戻される。

### 3.7 読みが無いときの Space（`fullWidthSpace`）

設定 `fullWidthSpace`（bool、既定 `true`、DEV-1814）は、ひらがなモードで読みが無いときの Space の幅を決める。

- `true`: Space は全角空白 `　`、Shift+Space は半角空白を入れる。
- `false`: Space は半角空白、Shift+Space は全角空白を入れる。
- 全角空白は composition を作らずに挿入し、学習観測には送らない。半角空白はキーをアプリへ渡して入れる。
  キーを食うかはこの設定と Shift で決まるので、TIP が `UserActionMap` の写像
  （`docs/legacy-parity-spec.md` §1.5.3。Idle の Space は `nullopt`）より前に判定する。
- 次の Space は対象外とする: 読みがあるとき（変換と次候補）、英数モード、IME Off、Ctrl・Alt・Win 付き、
  Unicode 入力中（`docs/legacy-parity-spec.md` §6.1）、再変換の候補一覧の表示中、カッコ補完の composition 中、
  確定の再試行を待つ入力があるとき（§2.4.1）、文書が読み取り専用のとき（`ITfContext::GetStatus` の `TF_SD_READONLY`。
  Space はアプリの動作に任せる）。Unicode 入力を終えた Space は、終えた後に Idle と同じに扱うので、
  既定では文字を確定してから全角空白を入れる。
- アプリがキーボード入力を無効にした context（§4.3）では、他のキーと同じくキーを食わずに渡す。
- secure の文脈でも全角空白を入れる。学習観測は、どの文脈でも送らない。
- 全角空白を挿入する edit session の失敗は 2 種類に分ける。
  - 読み取り専用（`TS_E_READONLY`）、選択位置が取れない、`SetText` の失敗など、session が書けなかったときは、
    全角空白を捨てる。後のキーは待たせず、通常どおり処理する。
  - ロックが得られず session が実行されなかったとき（`TF_E_SYNCHRONOUS`、または `S_OK` で実行されなかったとき）だけ、
    全角空白を残す。次のキー（`OnTestKeyDown` が先に呼ばれればそこ）で 1 回だけ挿入し直してから、そのキーを
    通常どおり処理する。入れ直しも失敗したら全角空白を捨てる。次のキーが別の context のもの、またはキーボード入力が
    無効な context のものなら、入れ直さずに捨てる。フォーカス喪失と Deactivate でも捨てる。
  - `TF_S_ASYNC` など、後から実行されうる結果のときは、入れ直すと 2 つ入るので入れ直さない（後から実行されれば
    1 つ入る）。`TF_E_DISCONNECTED` などのその他の失敗では捨てる。
  - 捨てたときと待つときは、理由名だけを警告ログに 1 行残す（`full_width_space_dropped` /
    `full_width_space_waits_for_lock`）。キーの内容と時刻は残さない。
- legacy の `TypeHalfSpace`（既定 false＝全角）と Microsoft IME の既定（入力モードに従って全角）に揃える。

TIP は `settings.json` の変更を読み直して反映する。キーが無い、または bool 以外の値は `true` とする。
Host はこの設定を読まない。既定を全角にしたので、このキーを書いていない既存の `settings.json` を使う人も
全角空白になる（設定アプリは既定と同じ値をファイルに書かない。`docs/settings-app-spec.md` §3.6）。
移行の処理は設けない。

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
入力インジケーターの左クリックとメニュー（§4.5）でユーザーが開閉したら、その document manager では戻さない。言語バーやアプリの `ImmSetOpenStatus` など TIP を経由しない Off は区別できないため、
この 1 秒以内に届いたものは 1 度だけ On に戻る。
一度フォーカスを受けた document manager へ戻ったときは TIP は書き込まず、compartment の値に従う。
入力先ごとの開閉の復元は IMM32 互換層が行い、TIP は入力先ごとの開閉を記録しない。
document manager の記録は `OnUninitDocumentMgr` と `Deactivate` で消す。

開閉の経緯は `AZOOKEY_LOG` の info ログで追える。`keyboard_open_initial`（On にする判定、
`source` は `activate` / `first_focus` / `reopen_after_restore`）、`keyboard_open_focus_first` と
`keyboard_open_focus`（フォーカス受信）、`keyboard_open_refresh`（値の反映、`source` は
`activate` / `compartment_change` / `toggle_key`（半角/全角）/ `ime_on_key`（ひらがなキーを含む）/ `ime_off_key` /
`convert_key` / `nonconvert_key`（`imeOnOff` のときの変換・無変換）/ `langbar_click` / `langbar_menu`
（入力インジケーターのメニュー））を出す。
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
| On（英数モード） | On | `VK_IME_ON`、ひらがなキー（§3.1）、`imeOnOff` の変換キーだけ。英数モードを解除して `hiragana` にする。compartment は書き換えない |

On → Off で何を確定するかは、Off にした操作で決まる。

- 半角/全角、`VK_IME_OFF`、`imeOnOff` の無変換、入力インジケーターの左クリックとメニューの「直接入力」（§4.5）:
  画面に表示中の文字列（ライブ変換の表示、応答待ちに引き継いだ表示、選択中の候補、読み）を Enter と同じ経路で確定してから compartment を 0 にする。
  Enter と同じく確定を学習観測に送り（引き継いだ表示は送らない。`docs/legacy-parity-spec.md` §2.3.1）、secure と学習不可の文脈では送らない。`dynamicPunctuation` の
  onPause で Enter が行う最終要求の待ち（`docs/dynamic-punctuation-spec.md` §7.3）はせず、表示中の文字列を
  その場で確定する。確定の edit session が拒否されたとき、または Enter が確定せずに待つとき（一括変換の応答待ち）は
  Off にせず On のまま残し、キーは消費する。拒否された確定は次のキーで確定し直し、そのキーは確定の後に処理する
  （§2.4.1）。次のキーがオフのキーなら、その時点で Off になる。Enter がキーを受け取らない状態
  （Enter に割り当てのない入力の途中）では、次の項目と同じ経路で確定して Off にする。
- 言語バーや IMM など TIP の外からの Off: フォーカス喪失と同じ確定の経路（§4.3）で、表示中の文字列を確定する。
  学習観測には送らない。TSF が編集を拒んだときは composition を残し、後の edit session で確定し直す。

### 4.3 Win+Space / アプリ切替時

- Win+Space (言語切替) は OS が処理 → `Deactivate` が呼ばれる
- アプリ切替 (`OnSetFocus`) では現在の composition を **確定** してから前面アプリへ
  フォーカスを譲る（MS-IME 互換）
- フォーカス喪失、Deactivate、TIP の外からの Off（§4.2）、英数モードへの切替（英数キー、入力インジケーターの
  メニューの半角英数）の確定は同じ経路を通り、composition が表示中の文字列を確定する。ライブ変換の表示
  （応答待ちに引き継いだ表示を含む。`docs/legacy-parity-spec.md` §2.3.1）、
  選択中の候補、一括変換の各文節が表示中ならそれを、そうでなければ読み（未確定ローマ字を含む）を確定する。
  - 表示中の文字列は、候補の状態と `dynamicPunctuation` の最終要求の待ちを片付ける前に取る。待っていた
    句読点は確定に含めない。
  - ユーザーが選んで確定したとは限らないので、学習観測には送らない。Enter で確定待ちになっていた確定は、
    その確定の観測のまま送る。
  - TSF が編集を拒んだときは、表示中の文字列が読みと違えば、それを確定待ちとして残す。後の edit session か
    次のキーで、読みではなく表示中の文字列を確定する。読みのままなら、composition と読みを残して後で確定し直す。
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
  - 表示済みの composition でも、入力が最初の 1 打だけで、その打鍵から 500 ms 以内の切替なら、
    旧 document では確定せずに取り消して持ち越す。Excel がその打鍵の preedit を表示した後に
    切り替える場合がこれに当たる。2 打目以降の入力、候補窓（候補モードか予測モード）の表示中、読みと違うライブ
    変換の表示、上限より後の切替、旧入力欄自体が secure な場合、切替先が持ち越した入力を
    捨てる場合（IME が閉じている、英数モード、切替先の入力欄自体が secure）は、上の確定の
    規則に従う。旧 document が同期の edit session で取り消しを受け付けなければ持ち越さず、
    同じく確定の経路に戻る。取り消した位置にキャレットを置く。
  - 切替先にまだ context が無いときも旧 document から取り消して持ち越し、切替先での扱いは
    次の打鍵で判定する（下の箇条）。
  - こうして表示済みから持ち越した入力が切替先で表示される前に、下の箇条で捨てる場面（再び
    フォーカスが移った、切替先の document が破棄された、Deactivate、打鍵での判定）に当たった
    ときは、捨てずに元の document へ確定する（持ち越さなかった場合と同じ結果）。元の document が
    既に破棄されている、入力欄自体が secure になった、IME が閉じているか英数モードのとき、
    または元の document が確定を受け付けないときは捨てる。
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

受け入れ条件の定義の正典は [`plans/windows-port-roadmap.md`](../plans/windows-port-roadmap.md)
の M21 節とする。本節は受け入れ条件を複製しない。

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

- モードアイコンはリソースを持たず、`GetIcon` のたびに GDI で文字を描いて作る（TIP DLL の ICON リソースは、§2.9 で述べるプロファイル用のブランドアイコンだけである）。大きさは 96 DPI で 16x16 とし、
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
| 予測変換 | `predictionEnabled` の今の値でチェック | 値を反転して settings.json に書く。`UpdateConfig` は送らない |
| 設定を開く | 常に有効 | §6 の `ITfFnConfigure::Show` と同じく `LaunchSettingsApplication` で設定アプリを開く（日本語の言語 ID と TIP のプロファイルに加え、`--pane input` で「入力」ペインを開く。`docs/settings-app-spec.md` §3.2） |

入力モードの 3 項目は設定ファイルに触れない。

- 今の値: ライブ変換と予測変換のチェックは、TIP が settings.json から読んでいる全体設定の値（`TipLocalSettings`）を出す。
  アプリごとのプロファイル（`profilesByApp`）はチェックにも書き込みにも使わない。フォーカス中のアプリのプロファイルが
  予測変換を指定している（`app-profile-spec.md` §4。§5 の解決順でそのアプリに当たる層のどれかが、全体設定と同じ値でも `predictionEnabled` を持つ）ときは、チェックは全体設定のままにし、ラベルを
  「予測変換（このアプリはアプリ別の設定）」とする。選んだときに書くのは全体設定の `predictionEnabled` である。
- 表記: 項目の名前は設定アプリと同じ語にする（`docs/settings-app-spec.md` §3.2「表記規則」）。
- 書き込み: settings.json の該当するトップレベルのキーだけを書き、他のキーはディスク上のまま残す。
  設定アプリと Host と同じファイルロック（`AcquireExclusiveFileLockForPath`）を取って読み、atomic に
  置き換える（`WriteTextFileAtomically`）。ファイルが無ければそのキーだけのファイルを作る。JSON の
  オブジェクトとして読めないファイルは書き換えず、隔離もしない（設定アプリが開いたときに扱う）。
  ロックは 500 ms 待って取れなければ書かない（利用者への表示は無く、ログだけが残る）。置き換えの flush と replace はそれぞれ一時的な共有違反を
  最大 500 ms まで再試行するので、書き込みは UI スレッドを最悪で約 1.5 秒止める。結果は
  `langbar_menu_setting_write` にキー（`setting`）と結果（`result`）の名前だけを出す。`result` は
  `written` / `lock_unavailable` / `read_failed` / `invalid_file` / `write_failed` の固定値で、共通の値域（`docs/observability-spec.md` §7.2）に加えてこのイベントだけが使う。
- 反映: どの TIP インスタンスも settings.json の監視で新しい値を読む。Host が読むのは `liveConversion`
  （動的句読点の入口）だけなので、`UpdateConfig` はライブ変換を切り替えたときだけ、モデル再試行と同じ短命の
  control 接続で送る（Host は `UpdateConfig` でモデルも読み直すので、ライブ変換の切り替えのたびにモデルの再ロードが走る）。
  モデル再試行の送信が進行中なら、進行中の `UpdateConfig` が書き込みより先にファイルを読んだかもしれないので、
  その送信が終わったときにもう 1 回だけ送る（`langbar_menu_host_reload_deferred` を出す）。進行中に TIP が無効化
  されたら、もう 1 回は送らない。
- 切り替える値: メニューを開いたときに見せたチェックの逆を書く。メニューを開いている間にファイルが変わっても、
  見た表示と逆向きには書かない。
- 設定アプリとの同時編集: 設定アプリは保存のとき、ライブ変換・予測変換を含む一般の設定値については、ユーザーが
  変えたキーだけをロックの下でディスク上のファイルへ書く。設定アプリを開いたままメニューで切り替えても、その変更は設定アプリの保存で消えない。
  ただし開いている設定アプリの画面は古い値のままで、同じキーを設定アプリでも変えて保存すると設定アプリの値が残る。
- 書き込まない場面: 管理者・低 IL・AppContainer のプロセス（一括校正の画面と同じ判定、`docs/proofread-view-spec.md`
  §2.2）と、settings.json の場所が分からないときは、ライブ変換と予測変換をグレーアウトし、ラベルに
  「（このアプリでは変更できません）」を添える（予測変換では、アプリ別の設定の注記よりこちらを出す）。入力モードと「設定を開く」は使える。

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
一括変換が OFF の候補選択中（§5.5）は、preedit 全体を Focused で表示する。

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

一括変換が OFF の候補選択中（core の `Selecting`、§5.5）は、composition 全体を覆う Focused の
range を 1 つ作る。この range は文節ではないので、preedit のクリックで注目を移す対象にしない（§7）。

アトムは `ITfCategoryMgr::RegisterGUID` で 4 つまとめて取得し、TextService インスタンス内に保持する。

### 5.4 文節境界

文節は一括変換の結果（`ipc::BatchConversionSegment`）である。
segment は区間を持たないので、各文節が表示している文字列（選択中の候補の `surface`、
候補を選んでいなければ `reading`）の UTF-16 長を先頭から積み上げて境界を決める。
この文字列は preedit 全体の表示文字列を作る規則と同じであり、区間の和が composition の長さと
一致しないときは文節属性を使わず §5.6 の fallback にする。

### 5.5 受け入れ条件

受け入れ条件の定義の正典は [`plans/windows-port-roadmap.md`](../plans/windows-port-roadmap.md)
の M23 節とする。本節は受け入れ条件を複製しない。

roadmap M23 の「青背景」は、provider が色を指定しないため、アプリが `TF_ATTR_TARGET_CONVERTED` を
描く配色である。アプリごとの見え方は実機チェックリストの D-11 で確認する（§5.7）。

一括変換が OFF の既定では、Space の後の preedit は core 経由で候補の表記に置き換わり、文節を持たない。
選択中の候補が preedit 全体を占めるので、候補選択中（core の `Selecting`）は composition 全体を
Focused で表示し、入力中（`Composing`）の fallback 属性（§5.6）と見分けられるようにする。
候補を移っても Focused のままである。Backspace で読みへ戻ると fallback 属性に戻る。
Esc は入力ごと取り消して composition を終えるので、属性を付ける文字列が残らない。
legacy が選択中の候補を `kTSMHiliteSelectedConvertedText` で描くのと同じ扱いである。
legacy は残りの読みを `kTSMHiliteConvertedText` で描く。候補が読みの前方だけを覆う形になったときは、
残りの読みを Converted にする。

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

通常の preedit 更新では、composition の文字列を書き換え、composition 全体へ
`GUID_PROP_ATTRIBUTE` の `SetValue` を適用した後、その range を `Clone` する。
文字列は、composition が前回書いた文字列をそのまま保っているときに限り、前回と異なる最初の文字から
末尾までだけを `SetText` で書き換え、同じ文字列なら書き込まない（DEV-1806。ライブ変換で
変換結果が変わらない文字を描き直さない）。書き換える区間は空にせず、composition の末尾まで含める。
区間の先頭が surrogate pair を割るときは 1 文字前から書き換える。末尾への純粋な挿入は anchor の
gravity 次第で composition に入らないことがあるため、文字を置き換える形にする。それでも composition が
書き込みに追随するかはアプリの text store に依るので、書き込み後に composition の文字列を読み直す。
一致しないときは composition の末尾を書き換えた区間の末尾まで `ITfComposition::ShiftEnd` で広げ
（広げられなければ書き換えた区間を空にし）、composition 全体を `SetText` で書き直す。前回の文字列を
読み取れないときも composition 全体を書き直す。
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
- fallback を使う条件は、一括変換の文節を候補ウィンドウで表示しておらず、core の候補選択中
  （§5.5）でもないこと（入力中の読み、ライブ変換の preedit、絵文字入力、候補選択中以外の
  core 経由の preedit を含む）、または文節の区間の和が composition の長さと一致しないこと。
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
打鍵から `GUID_PROP_ATTRIBUTE` に設定される範囲と属性（読み、候補選択中、Backspace での読みへの戻り、Esc での取り消し、一括変換の文節）は
`tsf-tip/tests/onkeydown_preedit_test.cpp` の `TsfTipSelectingDisplayAttributeTest` が担う。

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
直ちに閉じる。校正ビュー（`docs/proofread-view-spec.md` §2.4）は同じ固定パスを
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

> **`Show` を非同期にする理由（`docs/settings-app-spec.md` §3.5 と整合）**:
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
ログ上で区別する。§4.5 の入力インジケーターのメニューの「設定を開く」も同じ 2 つのイベントを同じフィールドで出すので、
`settings_launch` の行だけでは呼び出し元は分からない。

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

受け入れ条件の定義の正典は [`plans/windows-port-roadmap.md`](../plans/windows-port-roadmap.md)
の M23 節とする。本節は受け入れ条件を複製しない。

## 8. 予測モードと UI-less Mode の関係

UI-less Mode（M21）の環境でも、読み入力中の予測は TIP 自前の候補窓（`CandidateWindow`）の予測モード
（`docs/legacy-parity-spec.md` §3）で表示する。候補モード（`CandidateWindow` または OS 側の候補 UI）の
表示中は予測モードを出さない。OS の Suggestion UI へ予測候補を渡す統合
（`ITfReadingInformationUIElement` による公開）はこの仕様の範囲に含めない。

アプリが候補を描く環境では、アプリの UI と TIP の予測モードの窓が重なりうる。legacy の macOS 版は
候補の描画をすべて自前の窓で行うので、この重なりは Windows 固有である。

Suggestion UI へ渡す統合を範囲に含めるときは、roadmap にマイルストーンを置いてから本節に定義を書く。

### 8.1 受け入れ条件

受け入れ条件の定義の正典は [`plans/windows-port-roadmap.md`](../plans/windows-port-roadmap.md)
の M21 節とする。本節は受け入れ条件を複製しない。

## 9. テスト

| テスト | 場所 | 内容 |
|---|---|---|
| ReconversionFunction | `tsf-tip/tests/reconversion_function_test.cpp` | Windows 限定。QueryRange / GetReconversion / Reconvert |
| Configure 起動 | `tsf-tip/tests/query_interface_contract_test.cpp` | Windows 限定。`ITfFnConfigure` / `ITfFunction` の公開、`Show` がプロファイル付きで設定アプリを起動するか、起動失敗時の HRESULT |
| KeyMap 互換 | `tsf-tip/tests/keymap_test.cpp` | Windows 限定。VK_NONCONVERT 等の挙動。開閉キーの分類（§3.1、§3.1.1） |
| 開閉キーと入力インジケーター（§3.1、§4.1、§4.5） | `tsf-tip/tests/activate_uiless_test.cpp` | Windows 限定。`VK_IME_ON` / `VK_IME_OFF` と `imeOnOff` の変換・無変換、初回フォーカス直後の Off、言語バー項目の追加・削除・通知・クリック |
| 言語バー項目（§4.5） | `tsf-tip/tests/langbar_input_mode_test.cpp` | Windows 限定。表示の規則、文字の色、アイコンの大きさと透明度、sink への通知、クリックの受け渡し、右クリックのメニューの構成と項目の名前・注記、settings.json の 1 キーの書き込み（`TipSettingsWriter`） |
| 入力インジケーターのメニュー（§4.5） | `tsf-tip/tests/onkeydown_preedit_test.cpp` | Windows 限定。`TsfTipLangBarMenuTest`: 入力モードの切り替え、ライブ変換・予測変換の書き込みと `UpdateConfig` の有無、グレーアウト、アプリ別の設定の注記、無効化後の選択の破棄、設定アプリの起動（`--pane input`） |
| CandidateListUIElement | `tsf-tip/tests/candidate_ui_coordinator_test.cpp` | Windows 限定。UI-less 時の `GetCount` / `GetString` / `GetSelection`、app-drawn と TIP 描画の切替、`ITfUIElementMgr` 欠落時の fallback |
| Key / composition / preedit | `tsf-tip/tests/onkeydown_preedit_test.cpp` | Windows 限定。OEM 句読点と記号、候補選択 surface、末尾 selection、Backspace と Esc の状態遷移 |
| DisplayAttribute（§5） | `tsf-tip/tests/display_attribute_test.cpp` | Windows 限定。4 属性の定義と enumerator の Next / Skip / Reset / Clone。clone が位置と属性定義を複製すること。文節の区間と属性の割り当て、文節ごとの `SetValue` と fallback |
| preedit の属性（§5.3、§5.5） | `tsf-tip/tests/onkeydown_preedit_test.cpp` | Windows 限定。`TsfTipSelectingDisplayAttributeTest`: 読みは入力属性、一括変換 OFF の候補選択中は全体が注目属性、Backspace で読みへ戻ると入力属性、Esc の後に属性が残らないこと、一括変換の文節属性 |
| Preedit クリック（§7） | `tsf-tip/tests/onkeydown_preedit_test.cpp` | Windows 限定。クリック位置の文節への注目移動、左ボタン以外と文節がないときに食わないこと |

## 10. 参照

- TSF SDK: `Microsoft.Windows.SDK.Cpp` の `msctf.h`
- MS-IME 互換仕様（公式ドキュメント）: <https://learn.microsoft.com/windows/win32/tsf/>
- 旧 macOS 実装の対応箇所：`legacy/azooKeyMac/InputController/azooKeyMacInputController.swift`
- Phase 6-B: `docs/copilot-pc-backend-spec.md`
- Phase 6-C: `docs/native-ui-spec.md`
