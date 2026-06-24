# Legacy Parity 仕様（Phase 5）

本書は macOS 版 (`legacy/azooKeyMac/`) で実装済みだった機能を Windows 版へ
復元する際の **正典仕様** である。`plans/windows-port-roadmap.md` の Phase 5
（M13〜M19）が本書を参照する。

実装着手前にこの仕様を読み、レガシー実装と差異がある場合は本書を更新してから
コードに反映すること。実装後に挙動が変わった場合も本書を先に更新する。

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
    StartKanaDouble,    // かなキーダブルタップ → Replace Suggestion

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
| Idle | Input | Composing | StartComposition |
| Composing | StartConversion（候補キャッシュ有） | Selecting | Show CandidateWindow（§1.5.4） |
| Composing | StartConversion（候補キャッシュ無＝cache miss） | Composing | `QueryCandidates` 送信のみ。**`Selecting` へ遷移せず**応答到着で `Selecting`（§1.5.4。未到着中は Enter=確定 as-is・数字パススルー） |
| Composing (liveConv ON) | Input | Previewing | QueryLiveConversion + Preedit 更新 |
| Previewing | Input | Previewing | QueryLiveConversion + Preedit 更新 |
| Previewing | StartConversion | Selecting | Show CandidateWindow（最良候補にハイライト） |
| Selecting | NextCandidate | Selecting | 選択 index +1 |
| Selecting | Commit | Idle | EndComposition + CommitObservation |
| 任意 | Cancel | Idle | CancelComposition |
| Idle | StartAlnumDouble | ReplaceSuggestion | GetSelection + Show Prompt |
| ReplaceSuggestion | Commit | Idle | TransformSelectedText IPC + ReplaceSelection |
| Idle | StartUnicodeInput | UnicodeInput | StartComposition + hex buffer |
| UnicodeInput | Commit | Idle | hex → UTF-32 → UTF-16 surrogate, EndComposition |

> **候補表示の cache hit / miss 分岐**: `StartConversion` での `Selecting` 遷移は候補キャッシュ有の
> ときのみ即時。キャッシュ無（cache miss）は `QueryCandidates` を送って **`Composing` を維持**し、応答
> 到着時に `Selecting` へ遷移する。`Previewing | StartConversion` も未キャッシュなら同様に応答到着まで
> `Selecting` に入らない。候補スナップショット未確定のまま Enter / 数字 / 矢印が Selecting 挙動になる
> 回帰を防ぐためで、詳細は §1.5.4。

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
| `replaceMarkedText(s)` | composition range 全体に `SetText(s)` |
| `commitMarkedText()` | `ITfComposition::EndComposition` |
| `cancelMarkedText()` | composition range を空文字に SetText → EndComposition |
| `showCandidateWindow(list)` | `CandidateWindow::Show(list, anchor)` |
| `hideCandidateWindow()` | `CandidateWindow::Hide()` |
| `showPredictionWindow(list)` | `PredictionWindow::Show(list, anchor)` |
| `hidePredictionWindow()` | `PredictionWindow::Hide()` |
| `replaceSelectedText(s)` | selection range に `SetText(s)`、composition 無し |
| `playBeep()` | `MessageBeep(MB_OK)` |

実装は `tsf-tip/src/TextService.cpp::ApplyClientAction(const ClientAction&)`
として 1 メソッドに集約。引数は variant 型。EditSession が必要なものは
queue に積み、UI スレッドで `RequestEditSession` を呼ぶ。

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
| VK_1〜VK_9 | (none, Selecting時のみ) | SelectByDigit |
| VK_H | Ctrl | Backspace |
| VK_P | Ctrl | Up |
| VK_N | Ctrl | Down |
| VK_F | Ctrl | Forward |
| VK_B | Ctrl | Backward |
| VK_A | Ctrl | LineHead |
| VK_E | Ctrl | LineEnd |
| VK_I | Ctrl | (将来：カタカナ変換) |
| VK_O | Ctrl | (将来：英字変換) |
| VK_S | Ctrl | (将来：半角変換) |
| VK_U | Ctrl+Shift | StartUnicodeInput |
| VK_BACK | Ctrl+Shift | Forget |
| VK_KANJI | (none) | ToggleHankaku |
| VK_NONCONVERT | (none) | ToggleHiraKata |
| VK_F10 | (none) | ToggleDebugWindow |

「英数キー」「かな」のダブルタップは独自検出ロジック → §4.1（トリガ検出）参照。

### 1.5 C++ 移植境界（純粋性契約・拡張点・VK 表所有権・非回帰戦略）

§1.1〜§1.4 はレガシー（macOS）の UserAction / InputState / ClientAction / VK 表を定義する。
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
- **`EditContextHint`**（正典定義。`docs/bracket-pairing-spec.md` §3.1.1 は本定義を参照する）:
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
  `Shift+Space` で `PrevCandidate` / 数字は Selecting で `SelectByDigit`、**それ以外は `nullopt`＝
  パススルー（`Input` にしない）**）は、**現在状態を引数として明示的に受け取って**決める。ある状態でその IME が扱わないキーは `nullopt` を返す（例: Selecting 以外の
  数字）。これは現行 `OnTestKeyDown`/`OnKeyDown` の eaten 判定（数字は `candidate_ui_.IsShowing()`
  時のみ消費）と一致し、M10 を保存する。**hint 依存の例外**: Idle の Backspace は既定 `nullopt`
  （パススルー）だが、M61-A（`bracketPairing` ON）では TIP が §5.3 の同期読取で空ペアを確認した
  ときだけ Backspace を core へ回し（`HandleEvent(Backspace, hint)` → `deleteBracketPair`）eaten=TRUE
  にする（`docs/bracket-pairing-spec.md` §3.3・§5.3）。すなわち **hint に依存する eaten 判定は map
  （VK, mod, kind）ではなく OnKeyDown / OnTestKeyDown 層で hint を用いて上書き**する。文書・TSF・IO に触れないため純粋関数のままで、`kind` を
  注入して `tsf-tip/tests/keymap_test.cpp` で網羅テストできる。キーボードレイアウト・入力モードには依存しない
  （codepoint は解決しない＝第 2 層の責務）。M61-A の OEM キー（`VK_OEM_4`=`[`、`VK_OEM_6`=`]`、
  `VK_OEM_1` 等、および JIS 配列で `「」` を生むキー）は **本表へ `Input` として追加**する
  （`docs/bracket-pairing-spec.md` §3.1）。core が `InputState` を所有するため本写像も **core 側**に
  置き、TIP は `(VK, modifiers, 解決済み codepoint)` を core へ渡す。
- **第 2 層 — TIP `TextService.cpp::OnKeyDown`（プラットフォーム・モード依存）**:
  `Input` / `InputAlnum` の **codepoint** を、`ToUnicodeEx` + 現在の入力モード
  （ひらがな / カタカナ / 英数）と記号写像（`hiragana` で `[`→`「` 等）から解決し、
  `UserActionEvent.codepoint` に載せる。キーボードレイアウト・IME モードの関心を所有する。
- **状態を翻訳境界に含める根拠（無状態 map にしない）**: `Space` は Selecting で次候補・他で変換
  開始、数字は Selecting で候補選択・他で文字入力と、**同一キーの意味が現在状態に依存する**。
  無状態 `(VK, modifiers)` 写像ではこれを表現できず、§1.1 に raw `Space` / `Digit` 変種を増やすと
  状態解決を状態機械の外へ二重化してしまう。そこで **`InputStateKind` を写像の引数に含める**
  （round-2 レビュー提示の選択肢「状態を翻訳境界に含める」）。これは状態の二重保持ではない:
  写像は「キー＋状態 → どの `UserAction` か」の宣言的解決だけを担い、**遷移と副作用は引き続き
  状態機械（`HandleEvent`）が単独で所有**する。`UserActionMap` は状態を保持せず引数として受ける
  純粋関数なので、テスト容易性と Linux 再利用性は保たれる。§1.1 の `StartConversion` /
  `NextCandidate` / `PrevCandidate` / `SelectByDigit` は本写像が返す値であり、別途の raw 変種を
  §1.1 へ追加しない（append-only 方針を維持）。
- **分界規則**: **core は「キー（＋現在状態）がどの `UserAction`（種別・`digit` 等。消費しないなら `nullopt`）か」を決め、TIP は「どの文字を
  生むか（codepoint）」を決める**。これにより M61-A の OEM 追加は「core 表への `Input` 追加」＋
  「TIP の codepoint 解決追加」で済み、`UserAction` enum を不変に保てる。
- **テスト分界**: roadmap M13 受け入れの `tsf-tip/tests/keymap_test.cpp` は
  `UserActionMap(VK, modifiers, kind)` → `std::optional<UserActionEvent>` の全エントリ（状態依存解決・`digit` ペイロード・`nullopt` パススルーを含む）を各 `kind`
  注入で検証する。状態遷移・副作用は `core/tests/input_state_test.cpp`、codepoint 解決（モード依存）は
  TIP レベルのテストで検証する。

#### 1.5.4 非回帰移行戦略（M3〜M10 の挙動保存）

現行 `tsf-tip/src/TextService.cpp::OnKeyDown` は、状態を約 19 個のメンバ変数に分散させた
inline 分岐で M3〜M10 を実装済みである。状態機械への載せ替えは以下の戦略で、挙動回帰なし
（roadmap M13 受け入れ条件）を担保する。

- **特性化テスト先行（strangler 移行）**: リファクタ前に、現行 OnKeyDown の M3〜M10 挙動を
  状態遷移として `core/tests/input_state_test.cpp` に固定する。既存
  `tsf-tip/tests/onkeydown_preedit_test.cpp` は移行中も常に緑に保つ（等価性ゲート）。
- **状態所有の分割**: 現行の暗黙状態を以下に振り分ける。
  - **core `InputState`（論理状態）**: `kind`、reading バッファ（`preedit_kana_` + `romaji_` の
    ペンディング = `core::RomajiKanaConverter`）、候補スナップショット（`shown_candidates_`）+
    選択 index（`selected_candidate_idx_`）、Unicode 16 進バッファ。
  - **TIP 専有（プランビング・`TextService` に残す）**: `ITfComposition* composition_`、
    `ITfContext*`（active / commit context）、IPC の id 群（`ipc_pending_id_` /
    `ipc_inflight_id_` / `ipc_has_request_` — **staleness は TIP 側に残す**）、
    `CandidateUiCoordinator`（ウィンドウ）、`caret_pt_`、`committing_`（EditSession 再入ガード）、
    `ui_less_mode_`。
  - **分界の根拠**: IPC staleness と TSF オブジェクトは本質的に非同期・プラットフォーム依存。
    core は同期・純粋に保つ。候補の **データ + 選択 index** は core へ、候補 **ウィンドウ** は
    TIP に残す。
- **非同期候補と staleness の保存**: 候補問い合わせは非同期のため、純粋 core は待てない。
  `HandleEvent(StartConversion)` を `Composing` で受けたら `[queryCandidates(reading)]` を出すが、
  **候補が未到着のうちは `Selecting` へ遷移しない**（候補スナップショット・窓が無い状態で Enter /
  数字 / 矢印が Selecting 挙動になると、現行の pending-candidate 経路を回帰させる）。`Composing`
  （または下記の pending 状態）に留まり、Enter は `CommitPreeditAsIs`、数字は IME 非消費で
  アプリへパススルー（現行は候補窓表示時のみ消費。§1.5.3）のまま M10 挙動を保つ。`Selecting` への遷移は **候補が適用された時点**で起きる: キャッシュヒット時は
  同期に、cache miss 時は応答到着を TIP が core へ供給するフィードバックイベント
  （例 `HandleCandidatesArrived(list)`）で。応答到着時の窓表示は TIP 側の既存機構
  （`candidate_window_show_pending_` + request_id staleness）が処理し、staleness を core へ
  移さないことで M10 を保存する。
  - **代替**: 「クエリ送信済み・応答待ち」を明示する pending 状態（M58-A の `BatchConverting`
    `docs/romaji-batch-conversion-spec.md` §3 と同型）を `InputStateKind` へ append しても良い。
    その場合も確定可否・キー挙動は上記（Enter=as-is 確定・候補選択操作は不可）に一致させる。
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

## 2. ライブ変換 (M14)

### 2.1 動作概要

`settings.liveConversion == true` のとき、Composing 状態では
**候補ウィンドウを表示せず、Preedit にライブ最良候補を表示** する。

### 2.2 IPC

新規 Payload: `QueryLiveConversion(request_id, kana, context)` →
`QueryLiveConversionResponse(request_id, surface, confidence)`

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
InferenceEngine: 軽量推論 (SimpleConverter or Zenzai 1-pass)
   ↓
QueryLiveConversionResponse(req_id=N, surface, confidence)
   ↓
[TIP] req_id staleness check
   ↓
EditSession: Preedit 全体を surface で差し替え
   ↓
DisplayAttribute: kInputAttributeGuid (Phase 5 は単一属性)
                  → Phase 5 末尾で 4 段階に拡張 (rich-features-spec X-1)
```

### 2.4 キャンセル経路

- Esc: `ITfComposition::EndComposition` で composition 消去 + InputState=Idle
- Backspace: kana を 1 文字戻し、再度 `PostQueryLiveConversion`
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
- 配置位置が異なる（候補=キャレット下、予測=キャレット右）
- 同時表示するケースがある（ライブ変換中の予測表示）

### 3.2 配置アルゴリズム

`PredictionWindow::Show(candidates, caret_rect_screen)`:

1. anchor = キャレット矩形の右辺
2. 候補ウィンドウのサイズを計算（最大 5 件、フォント高 ×6 + padding）
3. 配置候補:
   - 右側に配置: `x = caret.right + 4`, `y = caret.top`
   - 画面外なら左側: `x = caret.left - width - 4`
   - 下が画面外なら上に: `y = caret.bottom - height`
4. `MonitorFromPoint(caret_center, MONITOR_DEFAULTTONEAREST)` で対象モニタを判定し、矩形外チェック

### 3.3 キャッシュ

`InferenceEngine::QueryPredictions` の応答を TIP 側で 1 秒キャッシュ
（既存レガシーの挙動）。同じ context で連続呼び出しを抑制。

キャッシュキー: `(kana, leftSideContext_hash)`。Phase 5 では単純な hash map。

### 3.4 操作

- Tab: 第一候補を受理（preedit に追記）
- Shift+Tab: 第二候補
- Esc: ウィンドウを閉じる（predictionEnabled=true なら自動再表示）
- マウス左クリック: 該当候補を受理

### 3.5 IPC

既存 `QueryPredictions` (enum のみ) の Payload を本実装する。

```
QueryPredictionsRequest:  request_id, kana, leftSideContext, mode
QueryPredictionsResponse: request_id, predictions[]
```

`mode` は X-2 で拡張（`word | phrase | sentence`）。Phase 5 では `word` のみ。

### 3.6 設定

`settings.predictionEnabled`（bool、default=true）。
OFF のときは `PredictionWindow` を生成しない。

## 4. Magic Conversion / Replace Suggestion (M16)

> AI 変換バックエンド（`AiBackend` / OpenAI 互換 API 呼び出し）の契約は
> **`docs/ai-backend-spec.md` が正典**である。本節 §4.4（IPC）/ §4.5（OpenAI 呼び出し）/
> §4.6（結果反映）は骨子であり、IPC payload・`mode`→system-prompt マッピング・
> HTTP 経路・エラー/リトライ/タイムアウト・secure ゲート（M46）・API キー保存（M34）・
> ストリーミング境界・M58-C / X-3-3 との共有契約は同書で確定する。

### 4.1 トリガ検出（ダブルタップ）

英数キー（VK_OEM_AUTO もしくは VK_DBE_ALPHANUMERIC）または
かなキー（VK_DBE_HIRAGANA）を **300ms 以内に 2 回押下** で起動。

実装：`TextService::OnKeyDown` で各キーの最終押下時刻を保持し、
`GetTickCount64() - last_press_ms < 300` で判定。

- 英数ダブルタップ → Magic Conversion （AI が自由テキスト変換）
- かなダブルタップ → Replace Suggestion （AI が選択テキストを言い換え）

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
ファイル変更検出時は `RomajiKanaConverter::ReloadCustomTable()` を呼び、
新規入力からテーブルを差し替える（進行中の preedit は触らない）。

### 5.4 内蔵テーブルとの関係

`inputStyle == "default"`：内蔵テーブルのみ
`inputStyle == "custom"`：**内蔵テーブルを差し替え**（マージしない）

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

### 6.2 表示

Preedit に `U+XXXX` 形式で表示（例: 入力中 `30A1` なら `U+30A1`）。
DisplayAttribute は通常の入力下線。

### 6.3 範囲チェック

- `0x0` ～ `0x10FFFF`
- サロゲート領域 `0xD800`〜`0xDFFF` は拒否（beep）
- それ以外は `wchar_t[2]` でサロゲートペアを生成

## 7. 学習忘却 (M18-2)

### 7.1 トリガ

- Ctrl+Shift+Backspace: 直前の commit を忘却
- 設定アプリ（Phase 7）から指定エントリを削除

### 7.2 API

`learning/include/azookey/learning/LearningStore.h` に追加：

```cpp
class LearningStore {
public:
    // 既存
    void Observe(reading, surface, ...);
    double Score(reading, surface, now) const;

    // 新規
    void Forget(const std::string& reading,
                const std::string& surface);
    void ForgetMostRecent();  // 直近 commit を忘却
    size_t Size() const;
};
```

実装方針：
- `Forget(reading, surface)`: エントリの weight を 0 にリセット
  （エントリ自体は残す → 同じ reading/surface を再 commit した時の挙動を
  「初回扱い」に揃える）
- `ForgetMostRecent()`: 直近の Observe（メモリ内ログ）を逆操作

`Reranker::Apply` は weight=0 を「LearningStore に存在しない」と同等に扱う
ので、追加の修正は不要。

### 7.3 TSV 永続化

既存の学習 TSV（未指定時は `%LOCALAPPDATA%\azooKey\data\learning.tsv`）の
該当行を weight=0 で書き戻す。
ファイルロックは既存の `Save()` フローに従う。

## 8. デバッグウィンドウ (M18-3)

### 8.1 起動と表示

- 起動: F10（トグル）
- ウィンドウ: `WS_POPUP | WS_BORDER`、半透明、サイズ 600×400
- 内容:
  - 直近 50 件の IPC ログ（QueryCandidates / QueryLiveConversion /
    QueryPredictions の req_id, kana, 応答候補上位 3 件, latency_ms）
  - InputState の遷移ログ
  - staleness で破棄された応答の表示

### 8.2 実装

- TIP プロセス内で circular buffer に保持
- `WM_PAINT` で GDI 描画（Phase 6-C で DirectWrite に置換）
- ログ取得は `OutputDebugString` と二重出力（既存の DebugView 経路も維持）

### 8.3 セキュリティ

- 入力中のキー押下や preedit 文字は表示しない（ログに漏れない）
- Reading と候補テキストのみ。Magic Conversion のプロンプトは表示しない

## 9. マルチディスプレイ / カーソル追従 (M19)

### 9.1 キャレット矩形取得

優先順:

1. `ITfContextView::GetTextExt(range, &rect, &clipped)` を使う
2. `S_OK` でも `rect` が空の場合、`GetGUIThreadInfo` で `gti.rcCaret` を取得
3. それも失敗（rcCaret が 0,0,0,0）の場合、`GetCursorPos` を `pt` として 1×16 の矩形を仮定

(2) は Chromium / Electron アプリで必要。(3) は最終フォールバック。

### 9.2 モニタ判定

```cpp
HMONITOR mon = MonitorFromPoint({rect.left, rect.top}, MONITOR_DEFAULTTONEAREST);
MONITORINFO mi{ sizeof(mi) };
GetMonitorInfo(mon, &mi);
// mi.rcWork が work area（タスクバー除外）
```

候補/予測ウィンドウは `mi.rcWork` 内に収まるよう配置。

### 9.3 DPI 対応（Phase 6-B M26 と分担）

Phase 5 では `WS_POPUP` 生成時に `SetProcessDpiAwarenessContext(
DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)` のみ。
`WM_DPICHANGED` ハンドリング・フォントスケーリングは Phase 6-B で。

## 10. テスト戦略

| テスト | 場所 | 内容 |
|---|---|---|
| UserAction VK マッピング | `tsf-tip/tests/keymap_test.cpp` | Windows 限定。テーブル全エントリ |
| InputState 遷移 | `core/tests/input_state_test.cpp` | 全状態 × 全 UserAction の遷移網羅 |
| ライブ変換 IPC | `ipc/tests/payloads_test.cpp` | `QueryLiveConversion` の build/parse |
| カスタムローマ字 TSV | `core/tests/custom_romaji_test.cpp` | パース、重複、コメント、不正行 |
| LearningStore::Forget | `learning/tests/learning_test.cpp` | Forget 後の Score=0、ForgetMostRecent |
| Unicode 入力 | `core/tests/unicode_input_test.cpp` | 範囲チェック、サロゲートペア生成 |
| 配置アルゴリズム | `tsf-tip/tests/window_positioning_test.cpp` | Windows 限定。モニタ矩形 vs 配置 |

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
