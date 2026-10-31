# Legacy Parity 仕様（Phase 5）

本書は macOS 版 (`legacy/azooKeyMac/`) で実装済みだった機能を Windows 版へ
復元する際の **正典仕様** である。`plans/windows-port-roadmap.md` の Phase 5
（M13〜M19）が本書を参照する。

Windows 版としての仕様判断は本書が正典であり、`legacy/` は参照資産である
（`AGENTS.md`「対象と正典」）。legacy との差異が生じた場合は、legacy に合わせるのでは
なく本書へ Windows 版の結論を書いて確定する。実装で挙動が変わる場合も本書を先に更新する。

## 1. 入力パイプライン (M13)

### 1.1 UserAction

レガシー `legacy/Core/Sources/Core/InputUtils/Actions/UserAction.swift` の
列挙を C++ `enum class` で再現する。

新規ヘッダ: `core/include/azookey/core/UserAction.h`

```cpp
enum class UserAction : uint16_t {
    // 文字入力
    Input,              // 通常の文字入力（VK_A 等）
    InputAlnum,         // 英数モード時の文字入力

    // 編集操作
    Backspace,
    Delete,
    Forward,            // Ctrl+F / Right
    Backward,           // Ctrl+B / Left
    Up,                 // Ctrl+P / Up
    Down,               // Ctrl+N / Down
    LineHead,           // Ctrl+A / Home
    LineEnd,            // Ctrl+E / End

    // 変換制御
    StartConversion,    // Space
    NextCandidate,      // Space (2回目以降)
    PrevCandidate,      // Shift+Space
    SelectByDigit,      // 1〜9
    Commit,             // Enter
    Cancel,             // Esc

    // モード切替
    ToggleHankaku,      // 半角/全角キー
    ToggleHiraKata,     // 無変換 (Hira ⇄ Kata)
    StartUnicodeInput,  // Ctrl+Shift+U
    StartAlnumDouble,   // 英数キーダブルタップ → Magic Conversion
    StartKanaDouble,    // かなキーダブルタップ → 選択テキストの和訳（§4.1）

    // 学習
    Forget,             // Ctrl+Shift+Backspace で直近確定を忘却

    // デバッグ
    ToggleDebugWindow,  // F10
};

struct UserActionEvent {
    UserAction action;
    char32_t   codepoint = 0;     // Input/InputAlnum 時のみ
    uint32_t   modifiers = 0;     // bit0=Shift bit1=Ctrl bit2=Alt bit3=Win
    int        digit     = 0;     // SelectByDigit 時の 1〜9
};
```

VK → UserAction マッピングは `tsf-tip/src/TextService.cpp::OnKeyDown` 内の
テーブルで実装する。テーブルは `core/src/UserActionMap.cpp` に切り出し、
TIP と将来の Linux ポート両方で再利用できる形にする。

> **参考（fkunn1326/azooKey-Windows, MIT）**: 先行 Windows 実装も独立に
> **UserAction（物理キー→意味）→ process_key（状態機械で ClientAction 列を生成）→
> handle_action（副作用実行）** の三段分離を採っており、本節の UserAction →（§1.2 InputState
> 状態機械）→ ClientAction 設計の妥当性を裏付ける。状態遷移を宣言的な表へ集約する思想は
> 参照価値が高い（ただし参考実装は `Selecting` 状態が未配線等の未完成があり、鵜呑みにしない）。

### 1.2 InputState

レガシー `legacy/Core/Sources/Core/InputUtils/InputState.swift` の状態機械を
C++ で再現する。

新規ヘッダ: `core/include/azookey/core/InputState.h`

```cpp
enum class InputStateKind {
    Idle,                // 入力中でない（composition 無し）
    Composing,           // ローマ字入力中 (preedit のみ)
    Previewing,          // ライブ変換中（preedit に最良候補が表示中）
    Selecting,           // 候補ウィンドウ表示中
    ReplaceSuggestion,   // 既存テキスト選択中の AI 置換待ち
    UnicodeInput,        // U+XXXX 入力モード
};
```

遷移表（主要遷移のみ）：

| 現状 | 入力 | 次状態 | 副作用 |
|---|---|---|---|
| Idle | Input | Composing（liveConv ON なら Previewing） | StartComposition、liveConv ON では QueryLiveConversion |
| Idle | Space / Shift+Space（ひらがなモード） | Idle | `fullWidthSpace = true` なら Space、false なら Shift+Space で全角空白を composition を作らずに挿入する。もう一方は半角空白としてアプリへ渡す（core を通さず TIP が判定する。`docs/tsf-deep-integration-spec.md` §3.7） |
| Composing (liveConv OFF) | Input（文字・数字・記号。§1.5.3） | Composing | 読みに足して Preedit 更新、打鍵ごとの `QueryCandidates` |
| Composing | StartConversion | Composing | 候補窓用の `QueryCandidates`（`live = false`）送信のみ。打鍵中の候補キャッシュは使わない。**`Selecting` へ遷移せず**応答到着で `Selecting`（§1.5.4。応答を待つ間のキーは TIP が保留し、窓が開いた後に再生する） |
| Composing / Previewing / Selecting | 字種指定確定（F6〜F10） | Idle | 読み全体を指定の字種に変えて確定する（`docs/tsf-deep-integration-spec.md` §3.6）。F7・F8 だけ ObserveCommit を送る |
| Composing (liveConv ON) | Input | Previewing | QueryLiveConversion + Preedit 更新（`punctuationConversion = commit` の `、` `。` `？` `！` は下の句読点の行が優先） |
| Previewing | Input | Previewing | QueryLiveConversion + Preedit 更新（`punctuationConversion = commit` の `、` `。` `？` `！` は下の句読点の行が優先） |
| Previewing | StartConversion | Previewing | Composing と同じく候補窓用の `QueryCandidates` を送り、応答到着で `Selecting`（最良候補にハイライト） |
| Selecting | NextCandidate | Selecting | 選択 index +1 |
| Selecting | Commit | Idle | EndComposition + CommitObservation |
| Selecting | Input（英字・記号・長音 `-`・句読点） | Composing（liveConv ON なら Previewing） | 選択中の候補を Commit と同じく確定して CommitObservation を送り、そのキーを Idle からの入力として新しい composition を始める。一括変換の複数文節は表示中の全文節を確定する。例外は `punctuationConversion = commit` の `、` `。` `？` `！`（下の句読点の行） |
| Selecting | Backspace | Composing（liveConv ON なら Previewing） | 候補窓を閉じ、reading の末尾 1 単位を消して再問い合わせ（選択中の候補は確定しない） |
| Composing / Previewing / Selecting | Input（`、` `。` `？` `！`、`punctuationConversion = commit`） | Idle | 第 1 候補（Selecting は選択中の候補）に句読点を続けて確定し、句読点を除いた読みと候補を観測する。条件と待ち方は `docs/tsf-deep-integration-spec.md` §3.5.1 |
| Composing / Previewing / Selecting / UnicodeInput | Cancel | Idle | 候補窓を閉じ（Selecting のとき）、CancelComposition。候補キャッシュは破棄し、学習観測は送らない。確定済みテキストには触れない |
| Idle | StartAlnumDouble | ReplaceSuggestion | GetSelection + Show Prompt |
| ReplaceSuggestion | Commit | Idle | TransformSelectedText IPC + ReplaceSelection |
| ReplaceSuggestion | Cancel | Idle | Hide Prompt |
| Idle | StartUnicodeInput | UnicodeInput | StartComposition + hex buffer |
| UnicodeInput | Commit | Idle | hex → UTF-32 → UTF-16 surrogate, EndComposition |

> **候補窓の取得**: `StartConversion` は打鍵中の候補キャッシュの有無によらず、候補窓用の
> `QueryCandidates`（`live = false`）を送って **`Composing` / `Previewing` を維持**し、応答到着時に
> `Selecting` へ遷移する。打鍵ごとの `live = true` の応答はモデル候補 1 件で、typo 補正の提案
> （M35 suggest）、英単語候補（M60）、記号・絵文字の rewriter を含まないため、候補窓には使わない。
> 応答が届く前に届いた別の候補通知（予測、ライブ変換、Host 状態の更新など）は待ちを終わらせない。
> これは TIP の前処理を通る Space の経路（英単語候補、記号・絵文字の rewriter）でも同じである。
> 応答が TIP の deadline（`dev-infrastructure-spec.md` §8.5.2 の候補窓用 750ms）に間に合わなかったときは、
> 打鍵中の応答を先頭に、TIP ローカル候補をその後ろに並べて窓を開く。打鍵中の応答は、ライブ変換 OFF
> なら `live = true` の `QueryCandidates` の候補、ON なら `QueryLiveConversion` の表層（句読点を
> 挿入していないもの）である。読み・左文脈・学習許可が Space の要求と一致し、secure でない context の、
> staleness 判定を通った応答だけを使う。auto_replace で読みを補正した応答は使わず、Host との接続を
> やり直すと捨てる。
> 候補スナップショット未確定のまま Enter / 数字 / 矢印が Selecting 挙動になる回帰を防ぐためで、
> 詳細は §1.5.4。

> **応答を待つ間のキーは保留して再生する（DEV-1813）**: `StartConversion` の応答を待つ間に来たキーのうち、
> `MapUserAction(VK, modifiers, Selecting)`（§1.5.3）が `nullopt` でないキー（Enter、Space、Shift+Space、↑↓、
> 変換、数字、Backspace、Ctrl+P/N/H、Ctrl+Shift+U など）、F6〜F10、`docs/tsf-deep-integration-spec.md` §3.5 で
> 取り込む文字・記号・数字は、TIP が消費して保留し、窓が開いた時点（応答の到着、上記の 750ms の deadline、
> Host 不通のローカル候補）で打った順に `Selecting` へ再生する。それ以外のキー（Tab、`Selecting` で割り当ての
> 無い ←→、Home など）は保留せず、その場でアプリへ渡す（保留中のキーはそのまま残す）。
> Esc と、IME を Off にするキー（`VK_IME_OFF`、半角/全角、`imeOnOff` の無変換）と英数モードへの切替（英数キー、
> 入力インジケーター）は保留せず、その場で処理して保留中のキーを捨てる。結果は legacy の同期処理と同じになる。Space→Enter は第 1 候補、
> Space→Space→Enter は第 2 候補を確定し、Space→`2` は 2 番目の候補を確定し、Space→文字は選択中の
> 候補を確定して次の入力を始める。2 回目以降の Space で候補窓用の要求を出し直さない。
> 保留は `docs/tsf-deep-integration-spec.md` §2.4.1 の確定の再試行を待つキーと同じキューを共有し、
> 打鍵時の Shift 状態を保って、両方の待ちを合わせて最大 16 個とする（17 個目以降は捨てる）。保留の上限は
> 750ms の deadline で、新しいタイマーは作らない。候補が 0 件で窓が開かずに待ちが終わったときは、
> `Composing`（`Previewing`）のまま再生する（Enter は読みの確定になる）。Esc はその場で入力を取り消し、
> IME を Off にするキーと英数モードへの切替はその場で表示中の文字列を確定する（`docs/tsf-deep-integration-spec.md`
> §4.2・§4.3）。フォーカス喪失、composition の終了、Deactivate も保留中のキーを捨てる。TIP の前処理を通る Space の経路
> （英単語候補、記号・絵文字の rewriter）も同じ規則に従う。一括変換の応答待ちは
> `docs/romaji-batch-conversion-spec.md` §3 に従い、この保留を当てる場合は同 spec に書く。
> `docs/dynamic-punctuation-spec.md` §7.3 の Enter の保留中に来たキーも同じ保留に載せる案があり、
> DEV-1794 の調査で決める。

> **ライブ変換の既定は OFF（意図した legacy との差）**: legacy の `LiveConversion` は既定 true だが、
> Windows 版の `liveConversion` は既定 false とする。Windows 版の利用者の多くはライブ変換の無い
> Microsoft IME から移ってくることと、打鍵ごとの推論が CPU だけの機械に負荷をかけることによる。
> ライブ変換 OFF の Space の段数も legacy と違う。legacy は 1 回目の Space で窓を出さずに先頭候補を
> 表示し、2 回目で窓を開く。Windows 版は 1 回目で窓を開くので、次候補へ進むのが 1 打早い。

> **Esc は 1 回で入力を取り消す（意図した legacy との差）**: legacy（macOS 版）は Selecting の Esc で
> Composing（ライブ変換 OFF では先頭候補プレビューを経由）へ戻り、さらに Esc で停止する 2〜3 段だった。
> Windows 版は 2026-10-17 のユーザー決定により、どの表示状態（候補窓・予測窓・ライブ変換の
> 候補窓）からでも Esc 1 回で入力中の読みを取り消す。読みへ戻る専用キーは設けない。候補窓から
> 読みの編集に戻るキーは Backspace だけである（文字入力は選択中の候補を確定して次の入力を始める）。
> 例外として、数字リライターの候補窓で押した `0`（候補番号ではない数字）は、窓を閉じて読みに足す。
> 例外は次の 2 つで、段階的に効く:
> 一括変換（`docs/romaji-batch-conversion-spec.md` §3）と絵文字検索（`docs/candidate-rewriter-spec.md`
> §11）。Unicode 入力と置換提案プロンプトの Esc は従来から 1 回で閉じる。

> **候補窓での文字入力は確定して続ける（legacy と同じ、DEV-1735）**: legacy の Selecting は `.input` を
> `commitMarkedTextAndAppendPieceToMarkedText` として扱い、表示中のテキストを確定してから入力を
> Composing で続ける（Backspace は `removeLastMarkedText` で読みへ戻る）。Windows 版も同じにする。
> legacy 自身の注記のとおり、これは macOS 標準の IME（Selecting を Composing に戻して入力を続ける）とは
> 異なる。数字は候補番号、Space / ↑↓ / Enter / Esc は上の行のまま変えない。Space 後で候補応答が
> 届く前（§1.5.4、状態は Composing）の入力は、上の保留により窓が開いた後に同じ規則で扱う。
> ライブ変換の Previewing（候補窓なし）の入力は読みに足す。絵文字検索、Unicode 入力、
> 置換提案プロンプトは対象外である。

各遷移を `InputState::HandleEvent(UserActionEvent, const EditContextHint&)` →
`HandleResult`（`ClientAction` 列 + 次状態）を返す純粋関数として実装し、テストで網羅する
（`core/tests/input_state_test.cpp`）。シグネチャ・純粋性契約・条件遷移（レガシーの
`ClientActionCallback`）の扱いは §1.5.1 を正典とする。

### 1.3 ClientAction → TSF 翻訳

レガシーの `ClientAction`（`appendToMarkedText` 等）を Windows TIP の
TSF 操作に対応付ける。

| ClientAction | TSF 操作 |
|---|---|
| `appendToMarkedText(s)` | `ITfRange::SetText(s)` + `kInputAttributeGuid` Property |
| `replaceMarkedText(s)` | composition を `s` にする（前回と異なる末尾だけ `SetText`。`docs/tsf-deep-integration-spec.md` §5.6） |
| `commitMarkedText()` | `ITfComposition::EndComposition` |
| `cancelMarkedText()` | composition range を空文字に SetText → EndComposition |
| `showCandidateWindow(list)` | `CandidateWindow::Show(list, anchor)` |
| `hideCandidateWindow()` | `CandidateWindow::Hide()` |
| `showPredictionWindow(list)` | `PredictionWindow::Show(list, anchor)` |
| `hidePredictionWindow()` | `PredictionWindow::Hide()` |
| `replaceSelectedText(s)` | selection range に `SetText(s)`、composition 無し |
| `playBeep()` | `MessageBeep(MB_OK)` |

M3〜M10 の通常入力では、`TextService::ApplyInputStateResult` が `InputState` の返す
`ClientAction` 列を順に処理し、TSF 側の副作用を `TextService::ApplyClientAction` に渡す。
`ClientAction` は variant 型である。EditSession が必要な preedit 更新と確定は
`RequestPreeditUpdate` と `RequestCommitEditSession` を介して適用する。

C++ の `ClientAction` は `core/include/azookey/core/ClientAction.h` の `std::variant` で、
上表の各行に対応する struct（`AppendToMarkedText` / `ReplaceMarkedText` / `CommitMarkedText` /
`CancelMarkedText` / `ShowCandidateWindow` / `HideCandidateWindow` / `ShowPredictionWindow` /
`HidePredictionWindow` / `ReplaceSelectedText` / `PlayBeep`）に、状態機械が必要とする次の副作用を
append-only で加える（§1.5.2）。preedit の更新は romaji のかな化で既存文字が変わるため、
`AppendToMarkedText` ではなく `ReplaceMarkedText`（全体置換）で表す。

| ClientAction | 意味 | TSF / TIP 側の操作 |
|---|---|---|
| `QueryCandidates(reading)` | reading の候補を問い合わせる | `PostQueryCandidates`。request id と staleness は TIP が持つ（§1.5.4） |
| `UpdateCandidateSelection(index)` | 表示中の候補窓のハイライトだけを動かす | `CandidateUiCoordinator::MoveSelection`（§1.6.2 (B)） |
| `ObserveCommit(reading, surface)` | 候補確定を学習へ記録する | `PostCommitObservation` |
| `ToggleInputMode(Hankaku / HiraKata)` | 入力モードを切り替える | 入力モードの compartment 更新 |
| `ShowReplaceSuggestionPrompt(mode)` | 選択テキストを取得してプロンプトを出す（§4.2, §4.3） | Magic Conversion / Replace Suggestion のダイアログ |
| `SubmitReplaceSuggestion` | プロンプトを送信する | `TransformSelectedText` IPC。応答は `ReplaceSelectedText` で反映 |
| `HideReplaceSuggestionPrompt` | プロンプトを閉じる | ダイアログを閉じる |
| `ForgetLastCommit` | 直近確定を忘却する（§7） | 忘却 IPC |
| `ToggleDebugWindow` | デバッグ窓を開閉する（§8） | デバッグ窓の表示切替 |

候補確定は `[HideCandidateWindow, ReplaceMarkedText(surface), CommitMarkedText,
ObserveCommit(reading, surface)]`、as-is 確定は `[ReplaceMarkedText(reading), CommitMarkedText]` の
列で表す（as-is 確定は学習観測を出さない）。

### 1.4 キーバインドマッピング

`tsf-tip/src/TextService.cpp::OnKeyDown` 内の table で実装：

| VK | Modifier | UserAction |
|---|---|---|
| VK_A〜VK_Z | (none) | Input |
| VK_BACK | (none) | Backspace |
| VK_DELETE | (none) | Delete |
| VK_LEFT | (none) | Backward |
| VK_RIGHT | (none) | Forward |
| VK_UP | (none) | Up |
| VK_DOWN | (none) | Down |
| VK_HOME | (none) | LineHead |
| VK_END | (none) | LineEnd |
| VK_SPACE | (none) | StartConversion / NextCandidate |
| VK_SPACE | Shift | PrevCandidate |
| VK_RETURN | (none) | Commit |
| VK_ESCAPE | (none) | Cancel |
| VK_1〜VK_9、VK_NUMPAD1〜VK_NUMPAD9 | (none, Selecting) | SelectByDigit（テンキーも候補番号。legacy はテンキーの数字を文字入力として扱うので、テンキーだけ legacy との差） |
| VK_0、VK_NUMPAD0 | (none, Selecting) | Input（選択中の候補を確定して `0` で次の composition を始める。legacy と同じ。数字リライターの候補窓の例外は §1.2） |
| VK_0〜VK_9、VK_NUMPAD0〜VK_NUMPAD9 | (none, Composing / Previewing) | Input（半角数字を読みに足す。Idle は `numberRewriter` が OFF ならパススルー。§1.5.3） |
| VK_ADD / VK_SUBTRACT / VK_MULTIPLY / VK_DECIMAL / VK_DIVIDE | (none, Composing / Previewing / Selecting) | Input（`＋` `ー` `＊` と半角の `.` `/`。Idle はパススルー。`docs/tsf-deep-integration-spec.md` §3.5） |
| VK_1 | Shift | Input（`！`。Selecting でも候補番号にしない） |
| VK_0〜VK_9 | Shift | Input（生む文字が `docs/tsf-deep-integration-spec.md` §3.5 の表にあれば全角で取り込む。無ければパススルー） |
| VK_H | Ctrl | Backspace |
| VK_P | Ctrl | Up |
| VK_N | Ctrl | Down |
| VK_F | Ctrl | Forward |
| VK_B | Ctrl | Backward |
| VK_A | Ctrl | LineHead |
| VK_E | Ctrl | LineEnd |
| VK_U | Ctrl+Shift | StartUnicodeInput |
| VK_BACK | Ctrl+Shift | Forget |
| VK_KANJI | (none) | ToggleHankaku |
| VK_NONCONVERT | (none) | ToggleHiraKata |
| VK_F6 | (none) | 字種指定確定（ひらがな）。読みがある状態だけ |
| VK_F7 | (none) | 字種指定確定（全角カタカナ）。読みがある状態だけ |
| VK_F8 | (none) | 字種指定確定（半角カタカナ）。読みがある状態だけ |
| VK_F9 | (none) | 字種指定確定（全角英字）。読みがある状態だけ |
| VK_F10 | (none) | 読みがある状態は字種指定確定（半角英字）、Idle は ToggleDebugWindow |

「英数キー」「かな」のダブルタップは独自検出ロジック → §4.1（トリガ検出）参照。

状態ごとの解決（§1.5.3 第 1 層）は `core/src/UserActionMap.cpp` の
`MapUserAction(VK, modifiers, InputStateKind)` が持ち、表の補足として次を定める。

- composition 内カーソルを持たないため、`VK_DELETE` / `VK_HOME` / `VK_END` / Ctrl+A / Ctrl+E は
  どの状態でも `nullopt`（パススルー）とする。Ctrl+I / O / S は割り当てず `nullopt` とする。
- F6〜F10 は Composing / Previewing / Selecting（Space の応答待ちを含む）で字種指定確定になり、
  Idle では `nullopt` とする（F7 は Edge・Chrome のキャレットブラウズ、F10 は Win32 のメニューバーへ渡る）。
  例外として、Idle の F10 は `ToggleDebugWindow` とし、TIP が食うのはデバッグ窓を持つ Debug ビルドだけ
  である（§8.1）。字種と確定の規則は `docs/tsf-deep-integration-spec.md` §3.6。
- `VK_LEFT` / `VK_RIGHT` / Ctrl+B / Ctrl+F は Selecting でだけ `Backward` / `Forward` になる。
  TIP は batch 変換の文節移動にだけ使い、文節が無いときはパススルーする。
- Ctrl+H は reading（または Unicode 16 進バッファ）がある状態で `Backspace`、Ctrl+P / Ctrl+N は
  Selecting で `Up` / `Down` になる。
- Alt または Win を含む組合せは常に `nullopt` とする。
- 開閉キー（半角/全角、`VK_IME_ON` / `VK_IME_OFF`、ひらがなキー `VK_DBE_HIRAGANA`、`convertKeysAction` が
  `imeOnOff` のときの変換・無変換）は、TIP がこの写像より前に処理する（`docs/tsf-deep-integration-spec.md`
  §3.1・§3.1.1）。`VK_IME_ON` / `VK_IME_OFF` / `VK_DBE_HIRAGANA` はこの写像では `nullopt` である。
- TIP は `ClientAction` の効果を実装している `UserAction`（文字入力、Backspace、変換・候補操作、
  Commit、Cancel、Selecting 中の Forward / Backward）だけを消費する。モード切替・Unicode 入力・
  Forget・デバッグ窓の `UserAction` は、対応するマイルストーン（M16 / M18）が効果を実装するまで
  TIP でパススルーする。

> **字種指定のキーと legacy との差（DEV-1812）**: F6〜F10 は legacy と Microsoft IME に共通の操作として
> 割り当てる（roadmap M64）。legacy で F6〜F10 と同じ働きをする Ctrl+J / K / ; / L / : と、Microsoft IME の
> Ctrl+U / I / O / P / T は割り当てない（Ctrl+P は Selecting の `Up` に使う）。legacy の Ctrl+I / O（文節の
> 伸縮）と Ctrl+S（AI の提案）も割り当てない。Microsoft IME の F6〜F10 は確定せずに字種を巡回し、Enter で
> 確定するが、Windows 版は legacy と同じく 1 打で確定する。

**キー割当の legacy との差**: 上の表と補足は Windows 版の割当であり、legacy の
`UserAction.getUserAction` と `InputState` の写像そのものではない。legacy と違う割当は次のとおりで、
どれも意図した差である。F6〜F10 と Ctrl+I / O / S は上の引用が決めている。

| キー・場面 | legacy | Windows |
|---|---|---|
| Ctrl+P / Ctrl+N | 読み入力中と候補選択中に ↑ / ↓ にする。読み入力中の Ctrl+N は候補選択を開き、Ctrl+P は何もしない | Selecting でだけ `Up` / `Down`。それ以外はアプリへ渡す |
| Ctrl+F | → にする。読み入力中は何もせず、候補選択中は選択中の候補が覆う部分だけを確定して残りを続ける | Selecting でだけ `Forward`。TIP は一括変換の文節移動にだけ使い、文節が無いときはアプリへ渡す |
| Ctrl+B / Ctrl+A / Ctrl+E | 割当なし（未定義の Ctrl 組合せ） | Ctrl+B は Ctrl+F と対にして Selecting の `Backward` とする。Ctrl+A / Ctrl+E は割り当てない。表の `LineHead` / `LineEnd` は `UserAction` の名前で、この版は割り当てない |
| Ctrl+M | Enter と同じ | 割り当てない。アプリへ渡す |
| Tab | 読み入力中は予測の受理（予測が無ければ消費）。候補選択中は消費 | 予測窓があるときだけ第 1 候補を受理する。それ以外はアプリへ渡す |
| ↓ | 読み入力中に候補選択を開く | Space で開く。Selecting 以外の ↓ はアプリへ渡す |
| ← / → / ↑ | 読み入力中は消費する。候補選択中の → は選択中の候補が覆う部分の確定、↑ は前の候補 | Selecting 以外はアプリへ渡す。Selecting の ← / → は Ctrl+B / F と同じ、↑ は前の候補 |
| 未定義の Ctrl 組合せ | 読み入力中・候補選択中は消費する | アプリへ渡す |
| 英数キー | 英語への絶対指定。読みを残したまま英字を足す | `VK_OEM_ATTN` のトグル。読みがあれば確定してから切り替える（`docs/tsf-deep-integration-spec.md` §3.1） |
| 忘却 | Ctrl+Delete（Backspace）。候補選択中に選択中の候補の記憶を消す | Ctrl+Shift+Backspace。入力していないときに直近の確定を消す（§7.1） |
| Unicode 入力中の Space と hex 以外のキー | Space は確定してアプリへは渡さない。hex 以外は消費して入力を続ける | Space は確定したうえでアプリへ渡す。hex 以外は確定して通常入力に移る（§6.1） |

素通しにしたキーについて、TIP は composition を確定も取り消しもしない。キーを受けたアプリが
composition の内側と外側のどちらへ作用させるかはアプリ次第である。legacy は読み入力中にこれらを
消費するので、composition の扱いがアプリに左右されない。

### 1.5 C++ 移植境界（純粋性契約・拡張点・VK 表所有権・非回帰戦略）

§1.1〜§1.3 はレガシー（macOS）の UserAction / InputState / ClientAction を定義する。§1.4 は Windows 版の
VK 表であり、レガシーと違う割当は §1.4 の「キー割当の legacy との差」が持つ。
本節は、それを C++ へ移植する際の **境界判断**（実装着手前に固定すべき責務分界・拡張点・
純粋性契約・非回帰戦略）を確定する。M13 の状態機械の上には後続マイルストーン
（M14 / M15 / M16 / M17 / M58-A / M59 / M61-A）がすべて載るため、各自が `InputState` /
`ClientAction` / `UserAction` を場当たり拡張すると enum 破壊・二重実装・回帰を招く。
後続はいずれも本節を正典として拡張すること。

#### 1.5.1 純粋性契約（core は文書を読まない）

`InputState::HandleEvent` は **純粋関数**である。同一の (現在状態, イベント, ヒント) からは
同一の (`ClientAction` 列, 次状態) のみを返し、TSF / エディタバッファ / IPC / ファイル /
グローバル状態に依存しない。

- **シグネチャ**（M13 で確定。M61-A `docs/bracket-pairing-spec.md` §3.1.1 が前提とする
  `EditContextHint` 引数を最初から含め、後続が引数を増やさずに済むようにする）:
  ```cpp
  // core/include/azookey/core/InputState.h（純粋・TSF 非依存）
  struct HandleResult {
      std::vector<ClientAction> actions;  // TIP が順に適用する副作用指示
      InputState                next;     // 適用後の論理状態（値）
  };

  // 文書事実は hint 経由でのみ core へ渡る。既定は空（全 nullopt）。
  HandleResult HandleEvent(const UserActionEvent& event,
                           const EditContextHint& hint = {}) const;
  ```
- **`EditContextHint`**（正典定義。`core/include/azookey/core/EditContextHint.h`。`docs/bracket-pairing-spec.md` §3.1.1 は本定義を参照する）:
  ```cpp
  struct EditContextHint {
      std::optional<char32_t> char_before;  // キャレット直前 1 文字（無ければ nullopt）
      std::optional<char32_t> char_after;   // キャレット直後 1 文字
      std::optional<bool> selection_collapsed;  // true=collapsed / false=範囲選択 / nullopt=不明
  };
  ```
- **責務分界**: core は文書（`ITfContext` / `ITfRange` / 選択範囲 / 周辺文字）を
  **直接読まない**。隣接文字・選択状態が必要な判定（M61-A のスキップ / 空ペア削除など）は、
  TIP が同期読取専用 EditSession（`docs/bracket-pairing-spec.md` §5.3）で `EditContextHint` を
  埋め、その hint を付けて `HandleEvent` を呼ぶ。読取が拒否されたら hint は空
  （全フィールド `nullopt`）で渡し、core は安全側（リテラル挿入 / 通常 Backspace）へ分岐する。
- **選択状態は三値**: `selection_collapsed` は `std::optional<bool>` とし、`true`=collapsed 確定 /
  `false`=範囲選択確定 / `nullopt`=不明（読取失敗）を区別する。**`true` のときだけ collapsed 前提の
  経路**（M61-A の開きカッコ→`insertBracketPair` 等）を採り、`false`・`nullopt` はいずれも安全側
  （M61-A 既定はリテラル挿入。M61-B の `bracketWrapSelection` は `false` 確定時のみ選択を囲む）へ
  倒す。plain `bool`（既定 `true`）にすると空 hint が「collapsed 確定」と区別できず、範囲選択中の
  読取失敗が誤ってペア挿入に倒れるため避ける（`docs/bracket-pairing-spec.md` §3.3）。実装は
  collapsed 判定を必ず `hint.selection_collapsed == true`（または `.value_or(false)`）で書く。
  `std::optional<bool>` は engaged な `false` も真と評価されるため、`if (hint.selection_collapsed)`
  の素の真偽評価は使わない（`docs/bracket-pairing-spec.md` §3.3 / §5.3 の collapsed 条件も同様）。
- **遡及適用**: この純粋性契約は M13 本体にも適用する（M61-A だけでなく、M13 の
  Composing / Selecting / Commit 経路も文書非依存に保つ）。現行 `TextService.cpp` の OnKeyDown は
  EditSession 内で `GetSelection` を呼ぶが、これは **TIP 側のレンダリング / EditSession 都合**で
  あり、状態遷移の判断材料ではない。状態遷移に必要な文書事実は `EditContextHint` のみに限定する。

> **型の整理（レガシーとの差異）**: レガシー Swift は
> `event(...) -> (ClientAction, ClientActionCallback)` で、遷移を `ClientActionCallback`
> （`transition` / `basedOnBackspace` / `basedOnSubmitCandidate` 等の条件分岐）として返す。
> C++ 版は **core が論理 composition バッファ（reading）と候補・選択 index を所有する**ため、
> `basedOnBackspace`（削除後に空か）・`basedOnSubmitCandidate`（確定後に残りがあるか）は
> **core 内部で決定的に解決**できる。したがって `ClientActionCallback` は移植せず、
> `HandleResult.next` に解決済みの次状態を載せる。文書依存の分岐のみ `EditContextHint`
> から解決する。

#### 1.5.2 拡張点ポリシー（どの enum を後続が拡張してよいか）

3 つの enum は安定性のティアが異なる。新機能は以下に従って拡張する。

- **`UserAction`（最安定・原則追加しない）**: 新しい文字・記号は `Input` / `InputAlnum` を
  再利用し、`UserActionEvent.codepoint` に載せる（M61-A のカッコ入力もこの方式で新 enum 値
  なし。`docs/bracket-pairing-spec.md` §3.1）。新 case の追加は「新しい *意味的キー意図*」
  （既存のどの UserAction でも表現できない操作）に限る。追加時は末尾に append し、
  `UserActionMap` と全 consumer を同時更新する。
- **`InputStateKind`（append-only・追加的拡張可）**: 新状態は **末尾に追加**し、既存値の
  並び替え・削除・renumber をしない。実例: M14 の `Previewing`（§1.2 に既出）、M58-A の
  `BatchAccumulating` / `BatchConverting`（`docs/romaji-batch-conversion-spec.md` §3）。
  **既存状態 + `EditContextHint` で表現できる機能は状態を追加しない**（M61-A は状態追加ゼロで
  `Idle` を再利用）。状態追加は「IPC 発火条件・確定可否・preedit の意味が既存と質的に異なる
  ライフサイクル」（batch の無 IPC 蓄積など）に限る。
- **`ClientAction`（拡張の主戦場・append-only variant）**: 新しい副作用は ClientAction を
  末尾追加する。パラメータ付きは payload を持つ（`insertBracketPair(open, close)` 等。
  variant 型）。**複合アクションを作らない**: レガシーの `commitMarkedTextAndAppendToMarkedText`
  相当は `[commitMarkedText, appendToMarkedText(s)]` のように **細粒度アクションの列**で表現する
  （組合せ爆発を防ぐ）。

**状態データ（associated value）の扱い**: C++ `enum class InputStateKind` は値を持てない。
レガシーの `unicodeInput(String)`（16 進バッファ）・`attachDiacritic(String)`（デッドキー）に
相当する状態データは、`InputState` 値オブジェクトのフィールド（`kind` でスイッチして使う）
として保持する。`InputState` が持つもの: `kind`、reading バッファ
（`core::RomajiKanaConverter` + 確定済みかな）、候補スナップショット + 選択 index、
Unicode 16 進バッファ。

- **予約状態**: レガシーの `attachDiacritic` は §1.2 の表に未掲載だが、M16（Magic）/
  M17（カスタムローマ字・デッドキー）で必要になりうる。M13 では実装せず、`InputStateKind` の
  **将来追加候補として予約**する（M17 着手時に末尾追加）。M13 のスコープは §1.2 に列挙した
  6 状態 + M3〜M10 parity に限定する。

#### 1.5.3 VK→UserAction 表の所有権（2 層分界）

VK→UserAction マッピング（§1.4）は 2 層に分け、責務を分界する。

- **第 1 層 — core `UserActionMap`（純粋・モード非依存・状態は明示引数）**: `(VK, modifiers,
  現在の InputStateKind)` → `std::optional<UserActionEvent>` を返す純粋関数（`nullopt` = その状態で
  IME が消費しない＝アプリへパススルー。`OnTestKeyDown` の eaten=FALSE に対応）。戻り値は enum 単体
  ではなく **`UserActionEvent` 全体**で、`action` / `modifiers` / `digit`（`SelectByDigit` の 1〜9 を
  VK から決定）を埋める（`codepoint` は第 2 層で TIP が解決した値を載せる）。これにより数字キーは
  index 付きで一意に識別でき、TIP が VK から digit を再導出する二重マッピングを不要にする。状態依存の解決
  （`Space` は Composing / Previewing で `StartConversion`、Selecting で `NextCandidate`、
  `Shift+Space` で `PrevCandidate` / 数字は Selecting の `1`〜`9` で `SelectByDigit`、Selecting の `0` と
  Composing / Previewing で `Input`、**Idle では `nullopt`＝パススルー**）は、**現在状態を引数として明示的に受け取って**決める。
  ある状態でその IME が扱わないキーは `nullopt` を返す（例: Idle の数字）。**hint 依存の例外**: Idle の Backspace は既定 `nullopt`
  （パススルー）だが、M61-A（`bracketPairing` ON）では TIP が §5.3 の同期読取で空ペアを確認した
  ときだけ Backspace を core の純粋関数 `EvaluateBracketBackspace(…, hint, …)` へ回し、返った
  `deleteBracketPair` を `BracketEditSession::Apply` で適用して eaten=TRUE にする
  （`docs/bracket-pairing-spec.md` §3.3・§5.3）。すなわち **hint に依存する eaten 判定は map
  （VK, mod, kind）ではなく OnKeyDown / OnTestKeyDown 層で hint を用いて上書き**する。文書・TSF・IO に触れないため純粋関数のままで、`kind` を
  注入して `tsf-tip/tests/keymap_test.cpp` で網羅テストできる。キーボードレイアウト・入力モードには依存しない
  （codepoint は解決しない＝第 2 層の責務）。
- **数字キー（DEV-1811）**: 主キーの `0`〜`9` とテンキーの `0`〜`9` は、Composing / Previewing で `Input`
  とし、読みに半角数字として足す。`numberRewriter`（`docs/candidate-rewriter-spec.md` §3）は候補を足すだけで、
  読みがあるときの取り込みの可否を変えない。Selecting では `1`〜`9`（テンキーを含む）が `SelectByDigit`、
  `0`（テンキーを含む）が `Input` である（§1.4）。Idle の数字は `nullopt` とし、`numberRewriter` が OFF のときは
  表計算などへ数字を直接入れる操作を変えない（legacy は Idle の数字でも入力を始めるので、意図した legacy
  との差である）。`numberRewriter` が ON で一括変換ではないときは、従来どおり TIP の前処理が Idle の数字を
  取り込んで composition を始める。Space の応答待ちの数字は §1.2 の保留で窓が開いた後に `SelectByDigit` として効く。
- **記号キー（DEV-1811）**: 記号を生む OEM キー（`VK_OEM_1`〜`VK_OEM_8`、`VK_OEM_102`、`VK_OEM_MINUS`、
  `VK_OEM_PLUS`、`VK_OEM_COMMA`、`VK_OEM_PERIOD`）と Shift 付きの数字キーは、Idle / Composing / Previewing /
  Selecting で `Input` とする。どの文字を生むか、全角化の表に載るかは第 2 層が `ToUnicodeEx` の結果で決め、
  表に無い文字ならキーをアプリへ渡す。表と取り込み方は `docs/tsf-deep-integration-spec.md` §3.5。
  core が `InputState` を所有するため本写像も **core 側**に置き、TIP は `(VK, modifiers, 解決済み codepoint)`
  を core へ渡す。`docs/bracket-pairing-spec.md` §3.1 のカッコ補完は、このキーを TIP が写像より前に処理する。
  テンキーの記号キー（`VK_ADD`、`VK_SUBTRACT`、`VK_MULTIPLY`、`VK_DECIMAL`、`VK_DIVIDE`）は Composing /
  Previewing / Selecting で `Input`、Idle で `nullopt` とする（§1.4）。文字は全角化の表ではなく VK で決める
  （同 §3.5）。legacy はテンキーの記号でも Idle から入力を始めるので、Idle の素通しは意図した legacy との差である。
- **第 2 層 — TIP `TextService.cpp::OnKeyDown`（プラットフォーム・モード依存）**:
  `Input` / `InputAlnum` の **codepoint** を、`ToUnicodeEx` + 現在の入力モード
  （ひらがな / カタカナ / 英数）と記号写像（`hiragana` で `[`→`「` 等）から解決し、
  `UserActionEvent.codepoint` に載せる。キーボードレイアウト・IME モードの関心を所有する。
- **状態を翻訳境界に含める根拠（無状態 map にしない）**: `Space` は Selecting で次候補・他で変換
  開始、数字は Selecting の `1`〜`9` で候補選択・Selecting の `0` と読み入力中で文字入力・Idle でパススルーと、**同一キーの意味が現在状態に依存する**。
  無状態 `(VK, modifiers)` 写像ではこれを表現できず、§1.1 に raw `Space` / `Digit` 変種を増やすと
  状態解決を状態機械の外へ二重化してしまう。そこで **`InputStateKind` を写像の引数に含める**
  （round-2 レビュー提示の選択肢「状態を翻訳境界に含める」）。これは状態の二重保持ではない:
  写像は「キー＋状態 → どの `UserAction` か」の宣言的解決だけを担い、**遷移と副作用は引き続き
  状態機械（`HandleEvent`）が単独で所有**する。`UserActionMap` は状態を保持せず引数として受ける
  純粋関数なので、テスト容易性と Linux 再利用性は保たれる。§1.1 の `StartConversion` /
  `NextCandidate` / `PrevCandidate` / `SelectByDigit` は本写像が返す値であり、別途の raw 変種を
  §1.1 へ追加しない（append-only 方針を維持）。
- **分界規則**: **core は「キー（＋現在状態）がどの `UserAction`（種別・`digit` 等。消費しないなら `nullopt`）か」を決め、TIP は「どの文字を
  生むか（codepoint）」を決める**。これにより OEM キーの取り込みは「core 表への `Input` 追加」＋
  「TIP の codepoint 解決追加」で済み、`UserAction` enum を不変に保てる。
- **テスト分界**: roadmap M13 受け入れの `tsf-tip/tests/keymap_test.cpp` は
  `UserActionMap(VK, modifiers, kind)` → `std::optional<UserActionEvent>` の全エントリ（状態依存解決・`digit` ペイロード・`nullopt` パススルーを含む）を各 `kind`
  注入で検証する。状態遷移・副作用は `core/tests/input_state_test.cpp`、codepoint 解決（モード依存）は
  TIP レベルのテストで検証する。

#### 1.5.4 非回帰移行戦略（M3〜M10 の挙動保存）

M13 の載せ替え対象は、M3〜M10 の通常入力における reading と候補選択の論理状態である。
`OnKeyDown` は入力を `InputState::HandleEvent` に渡し、非同期候補の鮮度を TIP 側で確認してから
`InputState::HandleCandidatesArrived` に渡す。状態機械への載せ替えは以下の戦略で、
挙動回帰なし（roadmap M13 受け入れ条件）を担保する。

- **特性化テスト先行（strangler 移行）**: リファクタ前に、現行 OnKeyDown の M3〜M10 挙動を
  状態遷移として `core/tests/input_state_test.cpp` に固定する。既存
  `tsf-tip/tests/onkeydown_preedit_test.cpp` は移行中も常に緑に保つ（等価性ゲート）。
- **状態所有の分割**: 現行の暗黙状態を以下に振り分ける。
  - **core `InputState`（論理状態）**: `kind`、reading（確定かなと pending romaji）、
    候補スナップショットと選択 index、Unicode 16 進バッファ。
  - **TIP 専有（プランビング・`TextService` に残す）**: `ITfComposition* composition_`、
    `ITfContext*`（active / commit context）、IPC の id 群（`ipc_pending_id_` /
    `ipc_inflight_id_` / `ipc_has_request_` — **staleness は TIP 側に残す**）、
    `CandidateUiCoordinator`（ウィンドウ）、`caret_pt_`、`committing_`（EditSession 再入ガード）、
    `ui_less_mode_`。
  - **分界の根拠**: IPC staleness と TSF オブジェクトは本質的に非同期・プラットフォーム依存。
    core は同期・純粋に保つ。候補の **データ + 選択 index** は core へ、候補 **ウィンドウ** は
    TIP に残す。
  - **TIP の mirror と例外経路**: `preedit_kana_` / `romaji_` は EditSession と例外経路の表示用、
    `shown_candidates_` / `selected_candidate_idx_` は候補 UI と IPC 学習 metadata 用の mirror とする。
    通常入力の正典は `input_state_` とし、batch、emoji、number rewriter、rewriter 有効時の
    Space は TIP の前処理を維持する。例外経路から通常入力へ戻す際は composition を core に
    取り込むか、reading が空なら core を初期化する。
- **非同期候補と staleness の保存**: 候補問い合わせは非同期のため、純粋 core は待てない。
  `HandleEvent(StartConversion)` を `Composing` で受けたら `[queryCandidates(reading)]` を出すが、
  **候補が未到着のうちは `Selecting` へ遷移しない**（候補スナップショット・窓が無い状態で Enter /
  数字 / 矢印が Selecting 挙動になると、現行の pending-candidate 経路を回帰させる）。core は `Composing`
  （または下記の pending 状態）に留まる。待つ間のキーは TIP が保留して core へ渡さず、`Selecting` へ
  遷移した後に打った順に渡す（§1.2「応答を待つ間のキーは保留して再生する」）。core の `Composing` が
  Enter を `CommitPreeditAsIs` として扱うのは、候補が 0 件で窓が開かなかった後の再生と、保留を通らない
  経路に限られる。`Selecting` への遷移は **候補が適用された時点**で起きる: 候補窓用の要求
  （`queryCandidates` の `for_window`）の応答到着を TIP が core へ供給するフィードバックイベント
  （`HandleCandidatesArrived(list)`）で。応答到着時の窓表示は TIP 側の既存機構
  （`candidate_window_show_pending_` + request_id staleness）が処理し、staleness を core へ
  移さないことで M10 を保存する。
  - **代替**: 「クエリ送信済み・応答待ち」を明示する pending 状態（M58-A の `BatchConverting`
    `docs/romaji-batch-conversion-spec.md` §3 と同型）を `InputStateKind` へ append しても良い。
    その場合も、待つ間のキーは TIP の保留を通し、pending 状態でキーを解釈しない。
- **挙動デルタの明示**（silent regression を避けるため、現行に無い挙動は「意図的追加」として
  区別する。下記は現行 OnKeyDown の inline 分岐に基づく区分であり、実装時に現行コードと
  突き合わせて確定する）:
  - **保存**（現行実装あり）: Space での romaji flush→候補表示 / 巡回(+1)、Up/Down 巡回、
    数字 1〜9 選択、Enter 確定（`CommitSelected` / `CommitPreeditAsIs`）、Esc、フォーカス喪失時の
    確定 / composition 終了、長音 `ー`（composing 中の `-` / `VK_SUBTRACT`）。
  - **意図的追加**（§1.4 は規定するが現行 OnKeyDown に未実装または部分的。M13 で新規・新テストで
    担保。M3〜M10 の回帰ではない）: Shift+Space の後退巡回（`PrevCandidate`）、
    Ctrl+H/P/N/F/B/A/E バインド、`VK_KANJI` / `VK_NONCONVERT` のモード切替、
    Ctrl+Shift+U（Unicode 入力）、Ctrl+Shift+Backspace（Forget）、F10（デバッグ窓）。
- **削除単位の契約（M59 連携）**: Backspace は **かな / 生ローマ字バッファの 1 単位**を削除し、
  自動句読点（M59）は削除単位に数えない（`docs/dynamic-punctuation-spec.md` §5.2）。reading
  バッファを core が所有することで、この「削除単位」を core 内部で定義でき、M59 は状態追加なしで
  載る。
- **等価性ゲート（roadmap M13 受け入れ条件に対応）**: (1) `onkeydown_preedit_test.cpp` が緑の
  まま、(2) `input_state_test.cpp` が全状態 × 全 UserAction の遷移を網羅（純粋 core なので
  `EditContextHint` 注入で文書なしに網羅）、(3) `keymap_test.cpp` が VK 表（第 1 層）の全
  エントリを検証。

### 1.6 karukan-im 設計参照（M13）

本節は、参考実装 `togatoga/karukan`（Rust / Linux + macOS）の状態機械層 `karukan-im` と
§1.1〜§1.5 を突き合わせた結論を確定する。分析のスナップショットは
`docs/karukan-comparison-report.md` 候補 1 / §5-1 にある。karukan は読み取り専用の参考実装で、
コードは逐語移植せず設計思想のみ参照する（**コード移植ではない**）。本節が正典として固定する
のは「突き合わせで判明した azooKey 側の欠け」と「azooKey 独自に再設計が必要な境界」であり、
レビューの経緯は本書に置かない。

karukan 側の参照点（`docs/karukan-comparison-report.md §9`。**外部リポジトリの
ため、実際の位置は M13 実装着手時に現物で確認する**）: `karukan-im/src/core/state.rs`
（`InputState`＝状態が値）、`karukan-im/src/core/engine/types.rs`（`EngineAction`
＝6 種の抽象出力アクション）、`karukan-im/src/core/engine/mod.rs`（`Engine` が状態でディスパッチ）、
`karukan-macos/.../KarukanInputController.swift`（薄いフロントエンドの到達点）。

#### 1.6.1 三段分離の対応（karukan の到達点 ↔ azooKey M13 の設計）

| 段 | karukan（到達済み・参照） | azooKey M13（本書の設計） |
|---|---|---|
| 物理キー → 意味 | XKB keysym → 内部キー抽象 | §1.5.3 第 1 層 `UserActionMap(VK, modifiers, kind) → optional<UserActionEvent>`（純粋・状態注入） |
| 状態機械（OS 非依存・アクション出力） | `InputState` enum（値）を保持し、`process_key(&mut self, ...)` が `self.state` でディスパッチして `EngineAction` 列を出力（**engine 状態を内包・可変。純粋関数ではない**。`engine/mod.rs`） | §1.5.1 `InputState::HandleEvent(event, hint) → HandleResult{actions, next}`（**純粋・文書非依存＝azooKey 独自の設計選択**） |
| 副作用の抽象出力 | `EngineAction` 6 種（`UpdatePreedit` / `ShowCandidates` / `HideCandidates` / `Commit` / `UpdateAuxText` / `HideAuxText`。`types.rs`） | §1.3 `ClientAction`（variant・append-only。§1.5.2） |
| 薄いフロントエンド | fcitx5 / macOS が抽象アクションを実行するだけ | `TextService.cpp::ApplyClientAction`（§1.3）が TSF 操作へ翻訳 |

karukan が「状態を単一の OS 非依存クレートに内包し、抽象アクションを出力してフロントエンドを
本当に薄く保つ」点が、azooKey の §1.5.3 2 層分界（状態機械を OS 非依存層へ閉じる）が目指す構造の
**動作実例**である。**ただし karukan の `process_key(&mut self, ...)` は `self.state` を可変に持ち
メトリクス更新や状態変更ヘルパを呼ぶステートフルなアクション発行器であり、純粋関数ではない**
（`engine/mod.rs`）。したがって azooKey の §1.5.1 純粋関数境界（値状態＋副作用なしの `HandleEvent`）は
**karukan が実証済みの性質ではなく azooKey 独自の設計選択**である。この純粋境界を karukan の既成
事実と誤認して API・テスト設計の根拠にしないこと（karukan から借りるのは「OS 非依存層に状態と
アクション出力を閉じる構造」であって、関数の純粋性ではない）。azooKey 側の出発点は、`tsf-tip/src/TextService.cpp`
の `OnTestKeyDown` / `OnKeyDown` に VK 別 if/else と暗黙フラグ（`preedit_kana_` /
`candidate_ui_.IsShowing()` / `committing_` の組合せ）で状態が集中している構造であり、
M13 はここから純粋状態機械層を切り出す（§1.5.4）。

#### 1.6.2 `EngineAction` ↔ `ClientAction` 突き合わせ（真の欠け / 設計オプションの区別）

karukan の抽象出力（`EngineAction` 6 種: `UpdatePreedit` / `ShowCandidates` / `HideCandidates` /
`Commit` / `UpdateAuxText` / `HideAuxText`。karukan `types.rs` / `conversion.rs`
に照合したもの）と azooKey の §1.3 `ClientAction→TSF` 表を突き合わせた。§1.3 は現状 10 行
（`appendToMarkedText` / `replaceMarkedText` / `commitMarkedText` / `cancelMarkedText` /
`showCandidateWindow` / `hideCandidateWindow` / `showPredictionWindow` / `hidePredictionWindow` /
`replaceSelectedText` / `playBeep`）である。突き合わせの結論を、**(A) karukan にも対応アクションが
在り azooKey に無い「真の欠け」**と、**(B) karukan にも対応アクションが無く azooKey 側で決める
「設計オプション」**に区別して確定する（区別を明示するのは、karukan 由来でない設計判断を
「karukan の欠け」と誤って正典化しないため）。追加はいずれも append-only 方針（§1.5.2）に従い、
最終決定は M13 実装 PR で行う（本節は enum を確定変更しない）。

**(A) 2 つの別チャネルに分離**: karukan は「候補ごとの説明」と「aux / ステータス
テキスト」を**別チャネル**で扱う。azooKey も 2 つに分離して確定する。両者を混同すると、
aux テキスト用 `ClientAction` を 1 つ足しただけで、M62 リライターが実際に必要とする候補説明
データ経路（候補データ構造・ビューモデル）が未拡張のまま残るため、明確に分ける。

- **(A1) 候補注釈（per-candidate description）データ経路**: karukan は
  `karukan-im/src/core/candidate.rs` の `Candidate.description` として**候補ごと**の右側コメントを
  運ぶ。azooKey も `core::Candidate`（`core/include/azookey/core/Candidate.h`）に `description` を
  持ち、候補ウィンドウがこれを描画する。**M62 候補リライター**が載る前提はこの**データ /
  ビューモデル**であって、§1.3 に `ClientAction` を 1 つ足すことでは満たせない。
  伝送経路は M62 のローカル / Host 分割に従い、**IPC 拡張を一律の前提にしない**
  （正典は `docs/candidate-rewriter-spec.md`）。
  - **TIP ローカルリライタ（M62-A 数字 / M62-B 半角カナ・英字。無 IPC・Host 非依存）**: description は
    **`core::Candidate` + 候補ウィンドウ view-model** で保持・表示する。`ipc::CandidateField`
    の変更は**不要**（M62-A を膨らませない）。
  - **Host 側データ駆動リライタ（M62-C 記号 / M62-D 絵文字）**: このときに限り `ipc::CandidateField`
    に `description` フィールドを append-only 追加して伝送する。
- **(A2) aux / ステータステキスト表示アクションの欠落（M62 とは別チャネル）**: karukan の
  `EngineAction::UpdateAuxText` / `HideAuxText`（`engine/types.rs`）は、reading やモード表示など
  **候補に紐づかない補助 / ステータステキスト**のためのチャネルで、(A1) の候補ごとの説明とは別物。
  azooKey の §1.3 には対応する aux テキストアクションが無い。azooKey は reading を preedit、
  モードを別 UI で扱うため、このチャネルを持たないこと自体は設計として成立する。導入する場合は
  §1.3 に `showAuxText(s)` / `hideAuxText` を append-only 追加する。**(A1) とは独立**に扱い、
  aux テキストアクションの追加で (A1) を満たしたと誤認しないこと。

**(B) 設計オプション（karukan 由来の欠けではない）**: 以下は karukan にも専用アクションが無く、
azooKey が自分で決める設計判断である。karukan の欠けとして正典化せず、azooKey 側の選択肢として記す。

- **候補選択（ハイライト）更新の反映方式（azooKey 設計判断）**: §1.2 の遷移表は
  `Selecting | NextCandidate → 選択 index +1` を副作用として要求する。**karukan は候補移動専用の
  アクションを持たず**、`update_conversion_preedit`（`conversion.rs`）から `UpdatePreedit` +
  `ShowCandidates` を再発行して選択状態を更新する（＝再描画で反映）。よって「選択移動アクションの
  欠落」は karukan 由来の欠けではない。azooKey は次のいずれかを M13 で選ぶ: (a) karukan と同様に
  `showCandidateWindow(list, selected_index)` を再発行して反映する、(b) `CandidateWindow` の
  ハイライトのみ更新する `updateCandidateSelection(index)` を**最適化として**追加し窓の作り直しを
  避ける、(c) `HandleResult.next` の選択 index を TIP が読んで反映する契約に一本化する。レガシー
  Swift `ClientAction`（`legacy/.../Actions/ClientAction.swift`）は
  `selectNextCandidate` 系を持つが、これは Swift 実装の選択であって C++ 側の必須要件ではない。
  azooKey は (b) を採る（TSF では窓の再構築コストが実測課題になり得るため）。
  `UpdateCandidateSelection(index)` + `ReplaceMarkedText(surface)` の列で反映する（§1.3）。
- **候補ページング・スクロールの反映方式（azooKey 設計判断）**: 候補数が窓の表示件数を超えるときの
  ページ送りも karukan に専用アクションは無い（同様に `ShowCandidates` 再発行で表現しうる）。
  M13 スコープでは 1〜9 の数字選択と Up/Down 巡回のみを保存（§1.5.4）すれば足りる。候補窓が複数
  ページを持つ設計へ進む場合に限り、`pageCandidates(delta)` 相当を append-only で足すか
  `showCandidateWindow` 再発行で表現するかを選ぶ（`UserAction` は §1.5.2 に従い新規追加せず、
  既存 Up/Down の意味拡張で吸収可）。

- **取りこぼしでないもの（確認済み）**: レガシー Swift の複合アクション
  （`commitMarkedTextAndAppendToMarkedText` 等）は §1.5.2 の「複合アクションを作らない」方針
  どおり細粒度アクションの列 `[commitMarkedText, appendToMarkedText(s)]` で表現するため、
  §1.3 に複合行が無いのは**意図的**であり欠けではない。`consume` / `fallthrough` は
  §1.5.3 の `optional<UserActionEvent>`（`nullopt`＝パススルー）と eaten 判定に吸収され、
  ClientAction 化しないのが正しい。

#### 1.6.3 azooKey 独自に再設計が必要な 3 点（karukan を写経しない境界）

karukan は参考になるが、以下 3 点は **karukan に対応概念が無いか、TSF / Windows 固有のため
逐語移植不可**である。M13 は本書 §1 の設計（レガシー Swift + spec）を一次参照とし、karukan は
補助に留める。

1. **VK → UserAction 表（karukan は XKB keysym ベース）**: karukan のキー抽象は XKB keysym で
   あり、Windows の VK コード・修飾・IME モード（`VK_KANJI` / `VK_NONCONVERT` / `VK_DBE_*`、
   MS-IME 互換）とは写像が根本的に異なる。§1.4 の VK 表と §1.5.3 の 2 層分界（core が
   `(VK, modifiers, kind) → UserAction`、TIP が codepoint 解決）が azooKey の正典であり、
   karukan のキー層は**取り込まない**。
2. **IPC 非同期の cache hit / miss 分岐（karukan に対応概念なし）**: karukan は同期変換前提で
   状態表を組むため、「候補問い合わせを送って応答到着まで `Selecting` に遷移しない」という
   §1.5.4 の非同期契約に相当する概念を持たない。azooKey は候補生成が別プロセス
   （`inference-host`）への非同期 IPC で、候補窓用の要求の応答到着イベント
   （`HandleCandidatesArrived(list)`）で `Selecting` へ遷移する（§1.2 の StartConversion 行・
   §1.5.4）。staleness（`ipc_pending_id_` 等）は TIP 側に残し core を純粋に保つ。この非同期
   境界は azooKey 独自設計であり、karukan の同期状態表を**そのまま持ち込まない**。
3. **`ClientAction` → `ITfComposition` / `ITfRange` 翻訳層（karukan は preedit を自前 String 保持）**:
   karukan はフロントエンドが preedit を自前 `String` バッファで持つが、TSF はアプリの実テキストを
   `ITfComposition` / `ITfRange::SetText` 経由で編集する。§1.3 の `ClientAction→TSF` 対応表と
   `ApplyClientAction`（EditSession キューイング）が azooKey の翻訳層であり、毎キーの preedit
   自前保持・高頻度 `UpdatePreedit` は**取り込まない**（再描画・ちらつき要因。
   `docs/karukan-comparison-report.md §6`）。

#### 1.6.4 テスト様式の雛形（karukan tests → `input_state_test.cpp`）

karukan の状態機械テスト `karukan-im/src/core/engine/tests/{basic,cursor,mode_toggle}.rs` を、
§10 の `core/tests/input_state_test.cpp`（全状態 × 全 UserAction 網羅）の**様式雛形**として使う
（データの逐語移植ではなく、テスト設計の観点を借りる）。

| karukan テスト観点 | azooKey `input_state_test.cpp` での対応 |
|---|---|
| `basic.rs`: 入力 → composition 生成 → 確定の基本往復 | `Idle→Composing→(Selecting)→Commit→Idle` の基本経路。`HandleEvent` が返す `(actions, next)` を AAA で固定 |
| `cursor.rs`: カーソル移動・削除単位 | `Backward` / `Forward` / `Backspace` の削除単位（§1.5.4「かな/生ローマ字 1 単位・自動句読点は数えない」）を検証 |
| `mode_toggle.rs`: モード切替の状態保存 | `ToggleHiraKata` / `ToggleHankaku` / `VK_KANJI` / `VK_NONCONVERT`（§1.5.4「意図的追加」）の切替で reading バッファが壊れないこと |

雛形化の要点（§1.5.1 純粋性契約を活かす）: (a) `HandleEvent` は純粋関数なので TSF / Zenzai / IPC を
起動せず、`EditContextHint` を注入して文書なしに網羅できる。(b) 各ケースは
`(現在状態, UserActionEvent, hint) → (期待 ClientAction 列, 期待 next)` の表明に落とす。
(c) cache miss 経路は `HandleCandidatesArrived` 相当のフィードバックイベント注入で
`Composing→Selecting` 遷移を検証する。応答未到着中のキーは TIP が保留して core へ渡さない（§1.2・§1.5.4）
ので、core のテストは「待ちの間に `Selecting` 挙動にならないこと」と「候補 0 件で `Composing` のまま
再生されたときの Enter=as-is 確定」を固定し、保留と再生の順序は TIP のテストで固定する。

#### 1.6.5 取り込まない範囲（明示）

`docs/karukan-comparison-report.md §6`（取り込まない方がよいもの）のうち、M13（状態機械）に
関わる範囲を本節でも明示的に除外する。

- **プロセス結合トポロジ**: karukan の macOS 子プロセス spawn / fcitx5 in-proc FFI は取り込まない。
  azooKey は共有 per-user `inference-host` + Named Pipe + クラッシュ隔離が正典
  （`docs/windows-tsf-host-architecture.md`）。
- **preedit 自前 String 保持 + 毎キー UpdatePreedit**（§1.6.3-3 参照）。
- **XKB keysym キー抽象 / 一方向モードトグル**（§1.6.3-1 参照）。VK / MS-IME 互換が正典。
- **同期変換前提の状態表**（§1.6.3-2 参照）。IPC 非同期・staleness 契約が正典。

## 2. ライブ変換 (M14)

### 2.1 動作概要

`settings.liveConversion == true` のとき、Composing 状態では
**候補ウィンドウを表示せず、Preedit にライブ最良候補を表示** する。
数値・括弧など従来のリライター経路は候補一覧を保持するため、
`QueryCandidates` の先頭候補をライブ表示に使い、Space で候補一覧を開く。

### 2.2 IPC

新規 Payload: `QueryLiveConversion(request_id, kana, context)` →
`QueryLiveConversionResponse(request_id, surface, confidence)`
`request_id` は既存の IPC `Envelope` に置き、Payload は `kana/context` と
`surface/confidence` をそれぞれ持つ。M59 の動的句読点の任意フィールド（要求の
`auto_punctuation` / `punctuation_style`、応答の `segments[]`）は `docs/dynamic-punctuation-spec.md` §7 が定める。`confidence` は有限の 0.0..1.0 とする。
M14 の軽量経路では候補の順位スコアをゼロ基準の sigmoid で単調に写像する。
X-1 の多 pass 推論では上位 2 候補の差を使って信頼度を較正する。
Host は Handshake 応答で `query_live_conversion` capability を広告する。
旧 Host が広告しない場合、TIP は `QueryCandidates` の先頭候補を preedit に使う。

既存 `QueryCandidates` と分けるのは：
- 返却するのは「最良 1 件」だけで軽量
- バッチ周期が異なる（タイピング中は短く）
- Phase 5 末尾でリッチ化（信頼度返却）が入る

### 2.3 シーケンス

```
[ローマ字入力]
   ↓
RomajiKanaConverter::Feed (preedit kana 更新)
   ↓
PostQueryLiveConversion(req_id=N, kana, context)
   ↓
InferenceEngine: 軽量推論 (モデルがあれば live = true の Zenzai 1-pass、無ければ SimpleConverter)
   ↓
QueryLiveConversionResponse(req_id=N, surface, confidence)
   ↓
[TIP] req_id staleness check
   ↓
EditSession: Preedit を surface にする（前回の表示と異なる末尾だけ書き換える。
             docs/tsf-deep-integration-spec.md §5.6）
   ↓
DisplayAttribute: kInputAttributeGuid (Phase 5 は単一属性)
                  → Phase 5 末尾で 4 段階に拡張 (rich-features-spec X-1)
```

#### 2.3.1 応答待ちの表示（DEV-1806）

打鍵で読みが変わってから、その読みへの応答が届くまでの preedit は、読みをそのまま見せずに
同じ入力の間に表示した応答を引き継いで表示する。応答は別プロセスへの非同期 IPC で返るため、読み全体を見せると
打鍵ごとに「変換済みの表記 → ひらがな → 新しい表記」と表示が往復する。

- 引き継ぐのは、同じ入力の間に表示した応答のうち、未確定ローマ字を含まない読みへのものである。
  読みごとに最新の 1 件を、新しいものから 8 件まで持つ。`かk` への応答は `かき` の先頭にならないので
  引き継がない。
- 各応答は区切り（piece）に分けておく。`segments[]` の各文節（持たないときは表層全体を 1 文節とする）を、
  読みと共通するかな（ひらがな、ひらがなとして比べるカタカナ、長音符）とその間の他の文字の並びに分け、
  他の文字の並びにはそれぞれ 1 文字以上の読みを当てる。この当て方が 1 通りに決まるときだけ分け、
  決まらない文節や 64 文字を超える文節は分けない。`ヵ` と `ヶ` は読みが一つに決まらないので、かなとして
  扱わない。
- 表示は、引き継いだ応答のうち、現在の読みの先頭と一致する区切りを最も長く残せるもの（同じ長さなら
  新しいもの）を使う。残すのは、かなで終わる区切りか文節の末尾までとし、残りの読みはかなのまま続ける。
  漢字の並びの直後で切ると、送り仮名を落とした語（`書く` から `書`）になるので切らない。
  - 例: `か` → `蚊` の後に `き` を打つと、`かき` への応答が届くまで `蚊き` と表示する。
  - 例: `はなをみる` → `花を見る` から Backspace で `る` を消すと、応答が届くまで `花をみ` と表示する。
  - 例: `か` → `蚊`、`かき` → `柿` の後に Backspace で `き` を消すと、`蚊` と表示する。
- 残せる区切りがなければ読みをそのまま表示する。
- 残した部分の末尾に来る自動句読点（`docs/dynamic-punctuation-spec.md` §5.1）は外し、間にあるものは残す。
  末尾の句読点は次の応答が決め直す。句読点の挿入を求めた要求への応答で、読みと表層に一致する
  `segments[]` を持たないものは、末尾が自動句読点か区別できないので引き継がない。
- 現在の読みそのものへの応答（Backspace で読みが戻った場合を含む）は、ほかの応答より優先してその表層を
  表示する（引き継いだ応答では末尾の自動句読点を外す）。
- 未確定ローマ字は、表示ではそのままの文字で、Enter の確定では確定時の変換（`CommitAsIs` の flush）で
  並べる。表示で入力どおりに見せていた末尾だけを置き換え、変換済みとして見せていた部分は変えない。
- この表示中の Enter（`CommitAsIs`）とフォーカス喪失時の確定（`docs/tsf-deep-integration-spec.md`
  §4.3）は、表示している文字列を確定する。学習観測を送るのは、確定した文字列が現在の読みへの応答の
  表層であるときだけとする。引き継いだ応答から組み立てた表示は Host が返した変換ではないので、
  Backspace で読みが引き継いだ応答の読みに戻った場合も観測しない。表示中にライブ変換を OFF にしても、
  preedit を描き直すまでは表示している文字列を確定する。
- 句読点キーで第 1 候補と句読点を確定する経路（`docs/tsf-deep-integration-spec.md` §3.5.1）は、
  候補の表記を確定し、この表示を使わない。
- 引き継ぎは確定・Esc・secure 入力・フォーカスの移動で破棄し、次の入力へ持ち越さない。
  候補窓から Backspace で読みに戻ったときは同じ入力が続くので残す。ライブ変換 OFF では使わない。
- **legacy との差**: legacy は、Backspace の後のライブ変換の表示を必ず読み（ひらがな）に戻す
  （`SegmentsManager` のライブ変換の表示は、削除の後は常にひらがなを示す）。Windows 版は、上の規則で
  引き継いだ応答のうち現在の読みの先頭と一致する区切りを残し、現在の読みへの応答が届いたら差し替える。
  削除のたびに「変換済みの表記 → ひらがな → 新しい表記」と往復させないための差である。

### 2.4 キャンセル経路

- Esc: `ITfComposition::EndComposition` で composition 消去 + InputState=Idle
- Backspace: kana を 1 文字戻し、再度 `PostQueryLiveConversion`。応答までの表示は §2.3.1
- Secure 入力: M14 の Payload には secure フラグがないため、TIP は Host へ
  `QueryLiveConversion` を送らず、ローカルの kana を Preedit に保つ。
- 別のアプリへフォーカス移動: `OnEndEdit` で commit (現状の挙動を維持)

### 2.5 staleness check との併用

既存 M10 の `ipc_pending_id_` staleness check をそのまま利用。
QueryLiveConversion も同じ ID 空間。古い response は破棄。

確定（Enter）時は in-flight の QueryLiveConversion に `Cancel` を送ってから
`EndComposition`。

## 3. 予測候補ウィンドウ (M15)

### 3.1 候補ウィンドウとの分離

`tsf-tip/src/CandidateWindow.cpp` は **変換候補** 専用に保ち、新規
`tsf-tip/src/PredictionWindow.cpp` を追加する。両者は別 HWND
（`WS_POPUP | WS_CLIPSIBLINGS`）。

理由：
- 表示タイミングが異なる（候補=Space 後、予測=入力中常時）
- 表示の寿命と操作が異なる。2 つは同時に出さない。変換候補の表示中は予測を出さず、届いた予測の
  結果も捨てる（ライブ変換のプレビュー中は変換候補を出さないので、予測を出す）

### 3.2 配置アルゴリズム

`PredictionWindow::Show(candidates, caret_rect_screen)`:

1. anchor = composition 全体の text extent（`ITfContextView::GetTextExt`）。アプリが返さない
   ときはキャレット矩形で代える
2. 候補ウィンドウのサイズを計算（最大 5 件、フォント高 ×6 + padding）
3. 配置候補:
   - composition 開始位置の下に配置: `x = anchor.left`, `y = anchor.bottom`
   - 下が画面外なら上に: `y = anchor.top - height`
   - 作業領域の外に出る分は内側へ寄せる
4. `MonitorFromPoint(caret_center, MONITOR_DEFAULTTONEAREST)` で対象モニタを判定し、矩形外チェック

打鍵で動く anchor の辺は、1 行に収まる composition では右辺だけである。左辺と上下辺から位置を
決めるので、composition の間、窓は打鍵ごとに動かない。Space 後の変換候補ウィンドウと同じ点
（composition 開始位置の左下）を基準にする。

次の場合、窓は打鍵に合わせて動く。

- アプリが text extent を返さず、キャレット矩形で代えるとき（キャレットに追従する）
- composition が折り返して複数行になったとき（左辺と下辺が変わった打鍵で 1 回動く）

作業領域が狭く、composition の下にも上にも収まらないときは、作業領域の内側へ寄せる。この
場合、窓は composition に重なる。縦書きの配置は扱わない。

### 3.3 キャッシュ

`InferenceEngine::QueryPredictions` の応答を TIP 側で 1 秒キャッシュ
（既存レガシーの挙動）。同じ context で連続呼び出しを抑制。

キャッシュキー: `(kana, leftSideContext_hash)`。Phase 5 では単純な hash map。

キャッシュに無い読みでも、直前に問い合わせた読みと同じで 1 秒以内なら問い合わせ直さない。
この抑止は、その問い合わせの応答か、応答を落としたときの再試行が残っていることを前提とする。
Esc などで予測を消すと、問い合わせも再試行も捨てるので、抑止も解く。直後に同じ読みを打てば
問い合わせ直す。

1 秒の期限は、同じ読みの連続呼び出しの抑制と、別の読みから戻ったときや候補窓を閉じた後の
表示し直しに使う。いま窓に出ている読みの結果は、元のキャッシュ項目が期限を過ぎた後に
入力の更新や candidates-ready の通知で評価し直しても、同じ読みと context のあいだは窓に残し、問い合わせ直さない。設定の変更と、応答を落としたときの
再試行は、表示中の窓に頼らず、上のキャッシュと問い合わせの規則で選び直す。

### 3.4 操作

- Tab: 第一候補を受理（preedit に追記）
- Shift+Tab: 第二候補
- Esc: 入力中（preedit か romaji 保留がある）は予測窓を閉じて入力を取り消す（§1.2 の Cancel と同じ。
  1 回で終わる）。入力中でなく窓だけが残っている場合は窓を閉じるだけ（predictionEnabled=true なら
  次の入力更新で再表示）
- マウス左クリック: 該当候補を受理

### 3.5 IPC

`QueryPredictions` の Payload 契約は次のとおり。

```
Envelope:                  request_id
QueryPredictionsRequest:   kana, leftSideContext, mode, app?
QueryPredictionsResponse:  predictions[], ok?, error?
```

`predictions[]` の各要素は既存の `CandidateField`（`tag` を含む）とする。`app` は
`docs/app-profile-spec.md` §3.1 の前面アプリ識別で、予測には boost を掛けない（同 §7）。
`ok` の省略時は `true` とみなし、`false` の場合は `error` に失敗理由を載せる。
`mode` は X-2 で拡張（`word | phrase | sentence`）。Phase 5 では `word` のみ。

### 3.6 設定

`settings.predictionEnabled`（bool、default=true）。
OFF のときは `PredictionWindow` を生成しない。

既定 ON は意図した legacy との差である。legacy の予測（`DebugPredictiveTyping`）は開発中の機能として既定
OFF で、Tab は予測を受理しない。Windows 版は既定 ON で、予測窓があるときの Tab で第 1 候補を受理する
（§3.4）。Microsoft IME も予測入力が既定 ON で、Tab で予測を選ぶ操作が近い。

### 3.7 診断ログ

予測が窓に届くまでの経過は `AZOOKEY_LOG` の info ログ `prediction_state` で追える。
`reason` は止まった箇所または結果を表し、照会しない理由（`disabled` / `not_allowed` /
`no_context` / `keyboard_closed` / `alphanumeric_mode` / `candidate_window` / `not_composing` /
`secure_input`）、照会の経過（`queued` / `host_unsupported` / `response_dropped`）、
応答の破棄（`stale_reading` / `stale_context` / `stale_generation`）、
表示の結果（`no_acceptable_prediction` / `window_create_failed` / `window_show_failed` / `shown`。
1 秒キャッシュからの表示を含む）のいずれかをとる。
`window_create_failed` と `window_show_failed` は失敗した初期化・描画の段階 `stage` と `hresult` を持つ。
各 `reason` は composition ごとに 1 回だけ出し、打鍵ごとには出さない。
composition の開始と TIP の deactivate で記録済みの `reason` を戻すので、次の入力でも同じ `reason` を出す。
どの行も入力本文、予測候補、打鍵の回数を持たない。

## 4. Magic Conversion / Replace Suggestion (M16)

> AI 変換バックエンド（`AiBackend` / OpenAI 互換 API 呼び出し）の契約は
> **`docs/ai-backend-spec.md` が正典**である。本節 §4.4（IPC）/ §4.5（OpenAI 呼び出し）/
> §4.6（結果反映）は骨子であり、IPC payload・`mode`→system-prompt マッピング・
> HTTP 経路・エラー/リトライ/タイムアウト・secure ゲート（M46）・API キー保存（M34）・
> ストリーミング境界・M58-C / X-3-3 との共有契約は同書で確定する。

### 4.1 トリガ検出（ダブルタップ）

英数キーまたはかなキーを **0.5 秒（500ms）以内に 2 回押下** で起動する。間隔は legacy の
`doubleTapInterval`（0.5 秒）に合わせる。

- 英数キー: `VK_OEM_ATTN`（`VK_DBE_ALPHANUMERIC` と同じ 0xF0。`docs/tsf-deep-integration-spec.md` §3.1）。
  `VK_OEM_AUTO`（0xF3）は半角/全角キーの別名であり、英数キーではない。
- かなキー: ひらがなキー `VK_DBE_HIRAGANA`（0xF2）。

実装：`TextService::OnKeyDown` で各キーの最終押下時刻を保持し、
`GetTickCount64() - last_press_ms < 500` で判定する。

legacy の割当（`azooKeyMacInputController`）に合わせて、2 回目の押下で次のように動く。

- 英数ダブルタップ
  - 選択テキストがあれば、Magic Conversion のプロンプト窓を初期プロンプト `english`（英訳）で開く。
  - 選択が無く、入力中の読みがあれば、読みを半角英字で確定して英数モードにする。
- かなダブルタップ
  - 選択テキストがあれば、同じ窓を初期プロンプト `japanese`（和訳）で開く。
  - 選択が無ければ何もしない。

どちらも同じプロンプト窓（§4.3）を開き、初期プロンプトだけが違う。かなダブルタップを
「言い換え」の別機能（Replace Suggestion）にはしない。Replace Suggestion は、legacy では
Ctrl+S（AI の提案）から入る状態であり、このダブルタップとは別である。Windows 版は Ctrl+S を
割り当てない（§1.4）。

### 4.2 選択テキスト取得

```cpp
ITfContext* ctx = GetCurrentContext();
TF_SELECTION sel{};
ULONG fetched = 0;
ctx->GetSelection(read_cookie, TF_DEFAULT_SELECTION, 1, &sel, &fetched);
WCHAR buf[4096];
ULONG cch = 0;
sel.range->GetText(read_cookie, 0, buf, ARRAYSIZE(buf), &cch);
```

選択が空の場合は **キャレット位置の段落 or 文** を取得（`ITfRange::Clone` +
`ShiftStart/ShiftEnd` で改行境界まで拡張）。

### 4.3 プロンプト UI

Phase 5 は簡易 Win32 ダイアログ（`DialogBox` + EDIT control）。
WinUI 3 への移行は Phase 7-M30。

legacy のプロンプト窓は 2 段である。プロンプトを入力して変換結果のプレビューを取得し
（`getTransformationPreview`、選択テキストへは触れない）、利用者が結果を確かめてから適用する。
以下の 1 段のダイアログは、プレビューと適用を分けない簡易形であり、これが legacy との差である。
プレビューを挟む 2 段にするかは、M16 の UI の段階として `docs/ai-backend-spec.md` で決める。

レイアウト：
```
+------------------------------------------+
| プロンプトを入力                          |
| [____________________________________]   |
| 選択テキスト: "こんにちは、田中さん。"     |
|                                          |
|                    [キャンセル] [ 変換 ]  |
+------------------------------------------+
```

### 4.4 IPC

> 以下は骨子。確定版の payload（`max_results` / `results` / `error_class` 追加を含む）は
> `docs/ai-backend-spec.md` §4.1 が正典。

新規メッセージ：

```
TransformSelectedTextRequest:
  request_id: uint64
  prompt: string             (e.g. "もっと丁寧に")
  selection: string
  context: string            (前後 N 文字)
  mode: enum { rewrite, translate, summarize, free }

TransformSelectedTextResponse:
  request_id
  result: string
  error: optional<string>
```

### 4.5 OpenAI API 呼び出し

`inference-host/src/AiBackend.cpp` を新設。

- Bearer 認証：`Authorization: Bearer <openAiApiKey>`
- エンドポイント：`{openAiApiEndpoint}/chat/completions`
- モデル：`gpt-4o-mini`（設定で変更可）
- MVP: `stream=false`
- Phase 6 拡張: `stream=true`（ストリーミング → TIP へ push IPC）

Foundation Models は **macOS 専用なので Windows 版では実装しない**。
将来のローカル LLM は M24 (Zenzai 流用) で対応。

### 4.6 結果反映

`replaceSelectedText(result)` ClientAction を発行。
TIP 側で selection range に `SetText(result)` を実行（composition 無し）。

## 5. カスタムローマ字テーブル (M17)

### 5.1 TSV フォーマット

```
# 行頭 # はコメント
# 空行は無視
# 各行は <input>\t<output>\t<consume> （consume は省略可で default=全消費）
xa	ぁ
xi	ぃ
sha	しゃ
@@	@
```

仕様詳細：
- 文字コード: UTF-8（BOM 許容）
- セパレータ: タブ文字 `\t`
- `<input>`: ASCII 1〜8 文字
- `<input>` の ASCII 英字は大文字・小文字を区別しない
- 記号・数字を含む規則は、通常入力と batch 入力の双方でキーの ASCII 文字を照合する。
  一致しない記号は従来の句読点・記号表示に従う。
- `<output>`: 任意 Unicode（UTF-16 で 8 code unit まで）
- `<consume>`: 整数 1〜len(input)。`<input>` の何文字を消費するか。
  省略時は `len(input)`。例: `nn\tん\t2`、`n\tん\t1`（n 単独入力時）
- 重複定義はファイル末尾の定義が勝つ
- パース失敗行は warning ログを残してスキップ

### 5.2 配置と検出

- 既定パス: `%LOCALAPPDATA%\azooKey\custom-romaji.tsv`
- 設定 `customRomajiTablePath` で上書き可
- 設定 `inputStyle == "custom"` のときのみロード

### 5.3 ホットリロード

`ReadDirectoryChangesW` で TSV のディレクトリを監視。
ファイル変更検出時は表を再読込し、新規入力からテーブルを差し替える。
各入力セッションは開始時の表を保持し、進行中の preedit は触らない。

### 5.4 内蔵テーブルとの関係

`inputStyle == "default"`：内蔵テーブルのみ
`inputStyle == "custom"`：**内蔵テーブルを差し替え**（マージしない）

内蔵テーブルは、レガシーが依存する AzooKeyKanaKanjiConverter の `defaultRomanToKanaMap`
（`legacy/Core/Package.swift` で固定した revision）と同じ表規則を持つ。小書き（`xtu`、`la` など）、
`wi`、`ve` などの拡張綴りもこれに含まれる。子音の重ね打ち（`kk` → `っk`）、`nn`、`n` の後に
子音が続く場合の「ん」は、表ではなく `RomajiKanaConverter::Feed` で処理し、レガシーと同一の
挙動は求めない。例えば `nn` の後に母音か `y` が続くと、`n` を次の音節へ持ち越す（`konnichiha` →
「こんにちは」）。レガシーの表は `nn` → 「ん」で 2 文字とも消費するので、`genninn` はレガシーでは
「げんいん」、Windows 版では「げんにん」になる。

未確定ローマ字が `n` か `nn` のときのアポストロフィ（`n'`、`nn'`）は、Mozc の慣習に合わせて「ん」で
区切り、アポストロフィを消費する（`gen'inn` → 「げんいん」、`kann'a` → 「かんあ」）。レガシーの表に
この規則は無い。前に `n` が無いアポストロフィは従来どおり文字として残る。カスタム表はこの処理を
通らない。キーの割り当ては `docs/tsf-deep-integration-spec.md` §3.5 が持つ。

この `nn` + 母音の曖昧さは、ローマ字の規則を変えずに変換の段階で解決する方針とする。その素材として、
`RomajiKanaConverter::AmbiguousReadings` は生ローマ字を受け取り、各 `nn` + 母音・`y` を
「ん」+ な行（既定）と「ん」+ あ行・や行の両方で読んだ読みの一覧を返す。

- 先頭は既定の読みである。
- 変える位置は先頭から 3 か所までとし、それより後ろは既定の読みに固定する。
- `nnn` + 母音（「ん」+ な行を明示した綴り）、`n'` / `nn'`（「ん」で区切ることを明示した綴り）と
  末尾の `nn` は、曖昧な位置に数えない。
- その位置だけを「ん」+ あ行・や行で読むと、未変換の英字の数が既定の読みと変わる位置（末尾の
  `nny`、`nnyi` など）は変えない。
- カスタム表は `Feed` の `nn` 処理を通らないので、読みは 1 つだけになる。

曖昧な位置は reading（かな）からは復元できないため、この関数は生ローマ字を入力に取る。

この一覧は、次の経路で変換窓の読みを選ぶのに使う。

- TIP は、打鍵した生ローマ字が今の読みを綴っている間だけ、先頭の曖昧な位置を変えた読みを 1 つ
  `alt_readings` として送る。生ローマ字そのものは送らない。カスタム表では送らない。
  - 送るのは変換窓の要求（Space）だけで、打鍵ごとの候補要求とライブ変換の要求には付けない。
    secure の要求にも付けない（secure では TIP が `corrected_reading` を捨てるため）。Host も
    secure の要求では比べない。
    曖昧な位置があるときの Space は、打鍵ごとの候補キャッシュではなく変換窓の要求で候補を得る。
- Host（`InferenceEngine::QueryAmbiguousCandidatesEx`）は、既定の読みと `alt_readings` の
  先頭の読みをそれぞれ変換する。M35 が既定の読みを補正したとき、既定の読みの変換が期限で
  打ち切られたとき、M63 の分割で変換する要求では比べない（2 回目の変換をしない）。
  - 先に学習ストアの確定で決める。読みごとに、その読みの候補の表示文字列について
    `LearningStore::Score`（app ごとの行の減衰つき重みの合計）を引き、その最大値を比べる。
    どちらかの読みに確定があれば、もう一方の読みの最大値が既定の読みのものを上回るときだけ
    もう一方の読みを選び、同点は既定の読みにする。どちらにも確定が無いときだけ、次のモデルスコアで比べる。
    - もう一方の読みを選んだとき、両方の読みのモデルの候補があり、どちらの変換も期限で打ち切られて
      いなければ、モデルスコアで切り替えたときと同じ形で返す。それ以外は既定の読みの候補を
      すべて残し、もう一方の読みで確定のある候補をその読みのまま先頭へ移す。同じ表示文字列の
      候補は、もう一方の読みの 1 件にまとめる。`corrected_reading` は設定しない。
    - 先頭へ移した分だけ候補は増える。記号・絵文字の rewriter が有効なときは、rewriter の候補を
      足すときの上限で既定の読みの末尾の候補が落ちることがある。上限は広げない。
    - 確定は候補の読みで記録されるので、`san'in` と打って確定した「山陰」も さんいん の確定に
      数える。さんにん の確定が無いと、次の `sannin` では「山陰」が先頭になる。そこで「三人」を
      1 回確定すれば、新しいほうの確定が勝つか同点になり、既定の読みに戻る。
    - 学習ストアは、既存の reranker と同じく読み取るだけで、要求の `learning_allowed` を見ない。
      `learning_allowed` が止めるのは学習の書き込みである（`docs/privacy-and-secure-input-spec.md`
      §5.1.1）。secure の要求は比べないので、学習も引かない。この比較は読みも語もログに出さない。
  - モデルの候補が両方にあるとき、もう一方の読みの最良のモデルスコア（正規化 logprob）が
    既定の読みのものを一定の差（`kAmbiguousReadingModelMargin`）を超えて上回れば、もう一方の
    読みの候補を返し、`corrected_reading` でその読みを示す。差が小さいとき、どちらかの変換が
    期限で打ち切られたときは、既定の読みの候補を返す。
  - もう一方の読みに切り替えたときも、既定の読みの上位の候補（`kAmbiguousReadingTypedKept` 件）の
    うち表示文字列が重ならないものを、その読みのまま候補の後ろに足す。モデルのスコアは、意図と違う読みを選ぶことがある
    （`sennenn` の先頭が「千年」ではなく「遷延」、`tennin` の先頭が「転任」ではなく「転院」に
    なる）。そのときも、既定の読みの語は変換窓から選べる。
  - モデルが無いとき（fallback converter だけのとき）は、学習ストアの確定が無ければ選ばない。既定の読みの候補の後ろに、
    もう一方の読みの辞書候補のうち表示文字列が重ならないものを、その読みのまま足す。
- TIP は、送った `alt_readings` に含まれる `corrected_reading` を M35 の補正とは区別する。
  候補の読みには使うが、preedit のかなと入力状態は書き換えない。この差し替えは打ち間違い学習の
  観測にならない。もう一方の読みの候補を確定したときの学習の観測と undo の組は、その候補の読み
  （`genninn` で「原因」なら げんいん → 原因）で記録する。M35 の補正読みで確定したときに、補正読みで
  記録するのと同じ扱いである。
- ライブ変換では比べない。ライブ変換の表示は既定の読みの変換になり、Space の変換窓で直る。
- 打鍵中のかな preedit は既定の読みのままである。`genninn` の preedit は「げんにん」と表示され、
  ひらがなのまま確定すると「げんにん」になる。「げんいん」側が効くのは変換窓の候補とその確定である。

カスタム TSV が存在しない、読み込めない、または有効な規則が 0 件のときは
内蔵テーブルを使う。

カスタム表（`inputStyle == "custom"`）は、`RomajiKanaConverter::Feed` が内蔵表に対して行う処理を
通らない（`Feed` は `custom_table_` があると `ConvertCustomPending` だけで返す）。

- 打鍵した英字は小文字にして表を引く。表の規則は大文字と小文字を書き分けられず、Shift の有無を
  区別できない。legacy のカスタム表が大文字小文字を区別するかは、`legacy/` の実装（表の定義は
  外部パッケージ）から確かめられない。
- 子音の重ね打ちによる促音、`nn` と `n` の処理、`-` の長音符への変換、`n'` の処理は適用しない。
  これらが要るときは、規則を表に書く。

レガシー macOS 版が「差し替え」方式なのでそれを踏襲。マージ方式は混乱しやすい。
ユーザーが既定 + α を望む場合は内蔵テーブルを書き出して編集するスクリプトを
別途用意する（Phase 7 設定アプリで実装）。

## 6. Unicode 入力モード (M18-1)

### 6.1 起動と終了

- 起動: Ctrl+Shift+U （Composing でなくても可）
- 入力受理: `[0-9a-fA-F]` を最大 8 桁
- 終了:
  - Enter: hex → コードポイント → UTF-16 サロゲートペアで commit
  - Esc: キャンセル
  - 非 hex キー: 直前の hex を確定して通常入力へ移行

legacy との差: legacy は Space を確定として扱いアプリへ渡さず、hex 以外のキーは消費して Unicode 入力を
続ける。範囲外のコードポイントは何も入力せずに閉じる（beep しない）。Windows 版は Space も非 hex キーの
一つとして確定してからアプリへ渡し、範囲外は beep して取り消す（§6.3）。

### 6.2 表示

Preedit に `U+XXXX` 形式で表示（例: 入力中 `30A1` なら `U+30A1`）。
DisplayAttribute は通常の入力下線。

### 6.3 範囲チェック

- `0x0` ～ `0x10FFFF`
- サロゲート領域 `0xD800`〜`0xDFFF` は拒否（beep）
- それ以外は `wchar_t[2]` でサロゲートペアを生成

### 6.4 実装の配置

hex バッファと範囲チェックは `core::InputState` の Unicode 入力状態（`HandleUnicodeInput`）が持ち、
確定は UTF-8 の `ReplaceMarkedText` と `CommitMarkedText` で出す。サロゲートペアは TIP が
UTF-8 を UTF-16 へ変換するときに生じる。範囲外の beep は `PlayBeep` の `ClientAction` であり、
TIP は `MessageBeep(MB_OK)` で鳴らす。

TIP は Unicode 入力状態の間、キーを状態 `UnicodeInput` として第 1 層へ渡し、hex 数字（Shift なしの主キーと
テンキー、A〜F）をキーボード配列によらずコードポイントにして core へ渡す。Shift+数字は hex 数字ではなく、
入力された記号として §6.1 の非 hex キーに当たる。Unicode 入力のキーは数値リライターや一括変換が
有効でも TIP 側の前処理を通さない。Ctrl+Shift+U で入れるのは、読みが無いときと、core が持つ読みや
候補ウィンドウがあるときである。絵文字検索・カッコペア・一括変換の蓄積など TIP が持つ composition の
途中では入らず、キーはアプリへ渡す。

hex 数字・Backspace・Enter・Esc 以外のキー（core が割り当てない Space や矢印も含む）は、TIP がキーを
分類する前に Unicode 入力を終える。値がスカラー値なら確定し、空なら取り消し、範囲外なら beep して取り消す。
その後のキーは Idle から押したときと同じに扱う。Idle で素通しするキーはアプリへ渡し、一括変換や数値
リライターが有効なら、その前処理を通る。TSF は `OnTestKeyDown` が食わないキーを `OnKeyDown` へ渡さないため、
この終了は `OnTestKeyDown` で行う。

core の 1 回の結果が確定（`CommitMarkedText` と続く `ObserveCommit`）の後に新しい marked text を出すことがある
（読みや候補ウィンドウからの Ctrl+Shift+U）。TIP は確定する文字列と学習観測を確定の時点で固定し、
後続の操作は確定の後の preedit として適用する。

## 7. 学習忘却 (M18-2)

### 7.1 トリガ

- Ctrl+Shift+Backspace: 直前の commit を忘却
- 設定アプリ（Phase 7）から指定エントリを削除

legacy との差: legacy の忘却は Ctrl+Delete（Backspace）で、候補選択中に選択中の候補の記憶を消す
（`SegmentsManager.forgetMemory`）。読み入力中の忘却は何もしない。Windows 版は、キーを
Ctrl+Shift+Backspace とし、入力していないときだけ働いて直近に確定した組を消す（§7.2）。
確定前の候補を忘れさせるキーは無い。

### 7.2 API

`learning/include/azookey/learning/LearningStore.h`:

```cpp
class LearningStore {
public:
    // 組の全 app 行の weight と回数を 0 にする。行が無ければ false
    bool Forget(const std::string& reading, const std::string& surface);
    size_t size() const;
};
```

- `Forget(reading, surface)`: 組の weight と回数（`user-learning-enhancement-spec.md` §3.2）を
  0 にする。同じ組を再 commit すると初回として扱う。`Score()` は 0 を返し、前方一致にも出ない。
- **直近 commit の忘却**: Ctrl+Shift+Backspace は直近 commit の組の weight を 0 にする。
  `Observe` の逆操作（weight から 1 回分を引くこと）ではない。
  Host は複数の TIP から確定を受けるため、「直近」は TIP ごとに決まる。
  そこで TIP は、直近の確定が学習観測を送った確定であるときだけ、その `(reading, surface)` の組を保持する。
  Ctrl+Shift+Backspace では、その組を `ForgetLearningEntry`（`learning-data-management-spec.md` §4.2）で
  Host へ送り、組を捨てる。
  確定（候補・読み・Unicode 入力・カッコ挿入・再変換）のたびに保持した組を捨て、学習観測を送ったときだけ
  保持し直す。したがって学習観測を送らなかった確定（`learning_allowed` が false の確定を含む）の後は
  忘却の対象が無く、それより前の組へは遡らない。対象が無いとき Ctrl+Shift+Backspace はアプリへ渡す。
  保持した組はフォーカスを跨がない。フォーカスの移動、context の push / pop、Deactivate で捨てる。
  secure の文脈では `ForgetLearningEntry` を送らず、組だけを捨てる（M46 の学習系メッセージと同じ扱い）。
  このとき忘却の対象は無いので、Ctrl+Shift+Backspace はアプリへ渡す。
  English タグの組は保持しない。組の形式で指定できるのはかなチャネルだけだからである。
  `LearningStore` は直近の commit を持たず、`ForgetMostRecent` は置かない。
- 変換器の学習バケットと自動単語登録（M36-A）への観測は取り消さない。

### 7.3 永続化

`Forget` した組は次の保存で学習ファイルから除く
（`learning-data-management-spec.md` §4.2）。ファイルロックは既存の `Save()` フローに従う。

## 8. デバッグウィンドウ (M18-3)

### 8.1 起動と表示

- 起動: F10（トグル）。**Debug ビルド（`_DEBUG`）だけ**で有効にする。F10 は多くのアプリでメニューバーを
  開くキーであり、製品ビルドが取るとアプリの F10 が使えなくなるためである。製品ビルドでは preserved key を
  登録せず、`OnKeyDown` でも F10 を食わずにアプリへ渡す。
  F10 は `WM_SYSKEYDOWN` として届き、キーイベントシンクに来ないホストがあるため、
  TIP は F10 を TSF の preserved key としても登録する。登録できたときは preserved key の通知だけで切り替え、
  `OnKeyDown` は F10 を食うだけにして 1 回の押下で 2 回切り替えない。IME が閉じているとき、英数モード、
  アプリがキーボード入力を無効にした文脈では、どちらの経路でも F10 をアプリへ渡す
- ウィンドウ: `WS_POPUP`（枠は描画で付ける）、半透明、サイズ 600×400。初回の表示で UI スレッドに作る
- 内容:
  - 直近 50 件の IPC ログ（QueryCandidates / QueryLiveConversion /
    QueryPredictions の req_id, kana, 応答候補上位 3 件, latency_ms）
  - InputState の遷移ログ
  - staleness で破棄された応答の表示

### 8.2 実装

- TIP プロセス内で circular buffer に保持（`tsf-tip/src/DebugLogBuffer.cpp`。
  IPC ログと状態遷移ログをそれぞれ直近 50 件。Windows API に依存しない）
- `WM_PAINT` で `RenderingEngine` により描画（`tsf-tip/src/DebugWindow.cpp`。`native-ui-spec.md` §4.3）
- ログ取得は `OutputDebugString` と二重出力（既存の DebugView 経路も維持）。
  二重出力するのは §8.3 のゲートを通した後の行だけである

### 8.3 セキュリティ

- 入力中のキー押下や preedit 文字は表示しない（ログに漏れない）
- 表示してよい本文は Reading と候補テキストだけ。Magic Conversion のプロンプトは表示しない
- Reading と候補テキストも、M41 の本文ログゲート（`core::BodyLoggingAllowed`。
  `dev-infrastructure-spec.md` の本文ログ規則）が開いているときだけ表示する。
  ゲートは Debug ビルド、`AZOOKEY_LOG_BODY=1`、非 secure、詳細ログ許可のすべてを要する。
- ゲートが閉じているときは本文を `<redacted len=N>`（N はコードポイント数）に置き換える。
  ゲートの判定はログを記録する時点で行い、閉じていれば本文を保持しない。
  後でゲートが開いても、secure 中の本文は表示されない。
  記録時の判定には、そのリクエストを出したときのプライバシー判定（secure と詳細ログ許可）を使う。
  描画時の判定には、直近のプライバシー判定を使う。

## 9. マルチディスプレイ / カーソル追従 (M19)

### 9.1 キャレット矩形取得

優先順:

1. `ITfContextView::GetTextExt(range, &rect, &clipped)` を使う
2. `S_OK` でも `rect` が空の場合、`GetGUIThreadInfo` で `gti.rcCaret` を取得
3. それも失敗（rcCaret が 0,0,0,0）の場合、`GetPhysicalCursorPos` を `pt` として 1×16 論理 px の矩形を仮定

(2) は Chromium / Electron アプリで必要。(3) は最終フォールバック。

### 9.2 モニタ判定

キャレットの解決（§9.1、§9.3）とモニタ作業領域の解決は `tsf-tip/src/CaretRectResolver.cpp` が持つ。
Win32 API は関数ポインタの組（`CaretWin32Api`、`MonitorWin32Api`）で受け取り、テストでは偽物に差し替える。

```cpp
MonitorWorkArea m = ResolveMonitorWorkArea(DefaultMonitorWin32Api(), point);
// MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST) のモニタの rcWork（タスクバー除外）。
// GetMonitorInfo が失敗したときは仮想スクリーン全体を使う。
```

候補ウィンドウ、その詳細ポップアップ、予測候補ウィンドウはすべてこの解決を通す。

モニタを選ぶ点はウィンドウごとに異なる。

- 候補ウィンドウ: キャレット左下の anchor 点。
- 詳細ポップアップ: 候補ウィンドウの矩形の中心。
- 予測候補ウィンドウ: キャレット矩形の中心。

候補/予測ウィンドウは作業領域内に収まるよう配置する。
配置の計算は Win32 を呼ばない純粋関数（`CandidateWindow::ComputePlacement`、`CandidateWindow::ComputeDetailsPlacement`、`PredictionWindow::ComputePlacement`）とする。

- 候補ウィンドウは anchor の下に開き、下端からはみ出すときはキャレットの上へ反転する。幅は作業領域の幅を上限とする。
- 予測候補ウィンドウは composition 開始位置の下に開き、下端からはみ出すときは composition の上へ反転し、右端からはみ出す分は内側へ寄せる（§3.2）。

### 9.3 DPI 対応（Phase 6-B M26 と分担）

候補ウィンドウは、`WS_POPUP` の生成時と表示位置の計測、配置時に、スレッドの DPI awareness
context を `DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2` に切り替える。
各処理の完了後は、元の context へ戻す。

`ITfContextView::GetTextExt` から得た screen 座標は、`ITfContextView::GetWnd` の文書 HWND を
`LogicalToPhysicalPointForPerMonitorDPI` に渡して物理 screen 座標へ正規化してからキャッシュする。
これにより、DPI 非対応または system-DPI-aware のホストと PMv2 の候補ウィンドウで
キャレット座標の取得から `SetWindowPos` まで物理 screen 座標系を一致させる。
文書 HWND がない windowless context、または変換 API が失敗した場合は、
利用可能な `GetTextExt` 座標を保持する。

`GetGUIThreadInfo` の `rcCaret` は `hwndCaret` 基準の論理 client 座標として扱う。
`ClientToScreen` で screen 座標へ変換した後、同じ `hwndCaret` を
`LogicalToPhysicalPointForPerMonitorDPI` に渡して物理座標へ正規化する。
変換 API が失敗した場合は、`ClientToScreen` で得た座標を保持する。
最終フォールバックでは、参照 HWND を推測せず `GetPhysicalCursorPos` から物理 screen 座標を直接取得する。
キャレット高の 16 論理 px は、カーソル位置のモニター拡大率を使って物理 px へ換算する。
拡大率を取得できない場合は、16 物理 px を使う。
予測候補ウィンドウが `GetTextExt` の矩形を得られずに anchor 点から矩形を作るときも、同じ換算でキャレット高を決める。

候補ウィンドウは `WM_DPICHANGED` と表示先モニタの DPI に応じて、
配置範囲、行高、余白、フォントを更新する。

## 10. テスト戦略

| テスト | 場所 | 内容 |
|---|---|---|
| UserAction VK マッピング | `tsf-tip/tests/keymap_test.cpp` | Windows 限定。テーブル全エントリ |
| InputState 遷移 | `core/tests/input_state_test.cpp` | 全状態 × 全 UserAction の遷移網羅 |
| ライブ変換 IPC | `ipc/tests/payloads_test.cpp` | `QueryLiveConversion` の build/parse |
| カスタムローマ字 TSV | `core/tests/custom_romaji_test.cpp` | パース、重複、コメント、不正行 |
| LearningStore::Forget | `learning/tests/learning_store_v2_test.cpp` | Forget 後の Score=0、保存で行が除かれること、再観測が初回扱いになること |
| Unicode 入力 | `core/tests/input_state_test.cpp` | 範囲チェック、桁数上限、`30A1` の確定 |
| 候補ウィンドウのレイアウト、DPI 換算、配置 | `tsf-tip/tests/candidate_window_dpi_test.cpp` | Windows 限定。行高・余白・最大幅の DPI 換算、作業領域内への配置と上への反転、負座標のモニタ（§9.2） |
| キャレット座標の取得と正規化 | `tsf-tip/tests/caret_position_test.cpp` | Windows 限定。物理 screen 座標への正規化とフォールバック段位（§9.3）、モニタ作業領域の解決と仮想スクリーンへの fallback（§9.2） |
| 予測候補ウィンドウ配置（§3.2） | `tsf-tip/tests/prediction_window_test.cpp` | Windows 限定。composition 開始位置の下への配置と、composition が伸びても位置が変わらないこと、モニタ矩形と配置候補の切替、DirectComposition 初期化を含む `Create` と `Show` で窓が可視になること |

## 11. 参照

- 旧 macOS 実装：`legacy/azooKeyMac/InputController/azooKeyMacInputController.swift`
- 入力状態機械：`legacy/Core/Sources/Core/InputUtils/InputState.swift`
- UserAction：`legacy/Core/Sources/Core/InputUtils/Actions/UserAction.swift`
- カスタムテーブル：`legacy/Core/Sources/Core/Configs/CustomInputTableStore.swift`
- AI バックエンド：`legacy/Core/Sources/Core/MagicConversion/AIBackend.swift`
- ウィンドウ配置：`legacy/Core/Sources/Core/Windows/WindowPositioning.swift`
- 横断リッチ化：`docs/rich-features-spec.md`

## 12. UI-less / `pbShow` アプリ互換チェック（M5・実機 Win11）

> 本節は `docs/tsf-deep-integration-spec.md` §2.8〜§2.11 の候補 UI「両立
> (coexistence)」方式に対する**実機 Win11 確認の手順・対象・合格条件**（安定仕様）
> のみを定める。**アプリ別の実測 pass/fail 結果（可変ログ）は本ドキュメントに記入
> しない。** 実機検証は DEV-97（D-01）の子課題 DEV-153（`gate:human-required`）で
> 実施し、実測結果・スクリーンショット等は当該 Linear 課題のコメントに記録する
> （`AGENTS.md`「進捗・状態を README/docs/roadmap に置かず Linear に一本化」）。

**計測手順**: 各対象アプリで `nihongo` → Space で候補表示し、(a) TIP が activate
されるか、(b) `BeginUIElement` の `pbShow` 戻り値、(c) 自前 HWND が出るか / OS・アプリ
側 UI に乗るか、を確認する。`pbShow` はデバッグウィンドウ（§8）かログ（`docs/
dev-infrastructure-spec.md` の構造化ログ）に出力して確認する。

**対象アプリと期待挙動**（pass 基準の定義。実測値は DEV-153 に記録）:

| アプリ | 区分 | 期待挙動 |
|---|---|---|
| メモ帳 (Notepad) | レガシー Win32 | activate / `pbShow==TRUE` / 自前 HWND |
| Edge（アドレスバー / テキストエリア） | Chromium | activate（`pbShow` はホスト依存） |
| Chrome | Chromium | activate（`pbShow` はホスト依存） |
| VS Code | Electron | activate（`pbShow` はホスト依存） |
| Windows ターミナル | Win32 | activate / 自前 HWND |
| Win11 スタート検索 | UI-less | activate + 入力（統合インライン検索表示は M21） |
| Office 365（Word） | UI-less / アプリ描画 | activate / `pbShow==FALSE` / OS・アプリ UI に候補 |

> Win11 スタート検索の**統合インライン検索**体験（候補が検索ボックス直下に統合表示
> される）は検索統合 API（`ITfIntegratableCandidateListUIElement` +
> `ITfFnSearchCandidateProvider`）を要し M21 スコープ（`docs/tsf-deep-integration-spec.md`
> §2.7、[IME search integration requirements](https://learn.microsoft.com/windows/apps/develop/input/input-method-editor-requirements#ime-search-integration)）。
> M5 ではスタート検索で TIP が activate され入力できること（統合表示なしの劣化モード
> 可）までを範囲とする。

**合格条件**:

- UI-less / アプリ描画ホスト（Office 等、`pbShow == FALSE` を返すアプリ）で TIP が
  activate され、自前 HWND が出ず OS/アプリ UI に候補が乗る。
- レガシー Win32（メモ帳等）で従来通り自前 HWND が出る（`pbShow == TRUE` 経路）。
- Win11 スタート検索で TIP が activate され入力できる（統合インライン検索表示は M21
  で検証。上記注記参照）。
- いずれのアプリでも TIP が activate されない事象が出ないこと（`ActivateEx` /
  `ITfTextInputProcessorEx` / カテゴリ登録の不備の早期検出）。
