# 自動カッコペアリング 仕様（追加機能 / M61）

本書は azooKey-Desktop Windows 版の「自動カッコペアリング（Bracket Auto-Pairing）」
機能を定める。`plans/windows-port-roadmap.md` の M61 が本書を参照する。本書は機能
仕様（InputState・ClientAction・TSF 操作・設定項目・ユーザー可視挙動）の正典であり、
進捗・状態は持たない（状態の正典は Linear。`AGENTS.md`「Linear 運用（管制塔）」参照）。

関連 spec:

- `docs/legacy-parity-spec.md` … 入力パイプライン（§1: UserAction / InputState /
  ClientAction）。本機能はこのパイプライン上に載る（§3）。
- `docs/tsf-deep-integration-spec.md` … 再変換（M20）・IME On/Off。確定済みカッコの
  再変換境界・モード遷移に影響する（§4.6）。
- `docs/app-profile-spec.md` … アプリ別入力プロファイル（M48）。自動ペアするエディタ
  での二重化回避（per-app 有効/無効）に再利用する（§4.5）。
- `docs/dynamic-punctuation-spec.md` … 動的自動句読点（M59）。記号列内部での句読点
  抑制と整合する（§4.7）。
- `docs/romaji-batch-conversion-spec.md` … ローマ字一括変換（M58）。蓄積中のカッコ
  入力の扱いを定める（§4.7）。

## 1. 目的（と背景）

iPhone の日本語入力キーボードのように、`（）` `「」` `[]` `{}` `""` などの開きカッコを
打鍵したとき、IME が対応する閉じカッコを自動で補い、**カーソルをカッコの内側に置く**。
これにより「開きカッコ → 閉じカッコ → カーソルを内側へ戻す」という 3 操作を 1 打鍵に
畳み込み、カッコ内への入力をそのまま続けられる。

加えて、対の中で文章を打ち終えて閉じカッコへ到達したときの自然な操作（既に在る閉じ
カッコを**飛び越える**＝スキップ）と、空のカッコ対を**まとめて消す**操作（Backspace）を
提供し、編集の手戻りを減らす。

本機能は **TIP ローカルかつ決定的**である。推論ホスト（`inference-host`）へ問い合わせず、
IPC も追加しない。したがって Host 未接続・劣化モードでも動作し、レイテンシは実質ゼロ
（ローカルなテキスト操作のみ）。これが本機能の重要な設計上の性質である（§7）。

## 1.1 非目標

- **既定 ON ではない。** 既定 OFF の実験的オプトイン機能であり、`bracketPairing == true`
  のときのみ動作する。OFF のときカッコ・記号キーの挙動は一切変えない（従来どおり
  リテラル入力としてアプリへ渡す。後方互換）。
- **構文補完・スマートインデントではない。** プログラミングエディタの言語対応自動補完
  （文字列内では括弧を補完しない等の構文解析）は範囲外。文字単位の対応関係のみを扱う。
- **アプリの自動ペア機能の置換ではない。** VS Code / IDE / ブラウザ devtools 等、
  アプリ自身がカッコを自動ペアする環境では二重化が起きうる。本機能はそれを**回避する
  仕組み**（per-app 無効化・既定 denylist。§4.5）を持つが、アプリ側挙動を制御はしない。
- **カッコ字種の変換・正規化ではない。** 半角→全角の自動変換などは行わない。打鍵で
  生じた字（入力モードに依存。§4.4）をそのまま対にする。
- **文中任意位置でのペア追跡の永続化ではない。** スキップ・空ペア削除は「**打鍵時点で
  カーソル隣接文字を読む**」決定的判定で実現し（§4.2・§4.3）、長寿命の位置追跡
  （`ITfRange` の永続保持）には依存しない。これが堅牢性の要（§4.8）。

## 2. 設計原則

- **TIP ローカル・決定的・無 IPC。** カッコ対応は固定表（§4.1）で決まる。推論ホストにも
  候補生成にも依存しない。Host 障害時も動く（§7）。
- **入力パイプライン（M13）の上に載せる。** `docs/legacy-parity-spec.md` §1 の
  UserAction → InputState → ClientAction → TSF 操作の枠組みに、カッコ分類ロジックと
  新 ClientAction を加える形で統合する。新しい `UserAction` enum 値は追加しない
  （`Input` が既に codepoint を持つため。§3.1）。
- **隣接文字を打鍵時に読む。** スキップ（§4.2）・空ペア削除（§4.3）は、長寿命の位置
  追跡ではなく「カーソルの左右 1 文字を読んで判定する」決定的方式にする。これにより
  外部編集・フォーカス遷移・他 IME 経由編集があっても破綻しない（§4.8）。
- **挿入は単一 SetText。** カッコ対は 1 回の `ITfRange::SetText` で挿入し、Undo 単位を
  1 つに保つ（§5.4）。カーソル移動は `SetSelection` のみで追加の編集単位を作らない。
- **常に入力を失わない。** 読み取り用 EditSession が拒否される / 選択取得に失敗する等の
  異常時は、ペアリングをあきらめ**リテラル 1 文字挿入**にフォールバックする（§4.8）。
- **既定 OFF・後方互換。** OFF のとき従来の状態遷移・確定・候補・学習を一切変えない。

## 3. InputState への統合（M13）

### 3.1 UserAction（新 enum 値なし・VK マッピングは拡張が必要）

`docs/legacy-parity-spec.md` §1.1 の `UserAction::Input`（および英数モードの
`InputAlnum`）は `UserActionEvent.codepoint` に打鍵由来の Unicode コードポイントを
持つ。本機能は **codepoint を分類する層**を InputState に足すだけでよく、新しい
`UserAction` enum 値は**追加しない**（`Input` / `InputAlnum` を再利用する）。

ただし **VK→UserAction マッピング（§1.4）は拡張が必要**である。M13 §1.4 の表は現状
`VK_A〜VK_Z` のみを `Input` に写し、カッコ・記号を生む OEM キー（`VK_OEM_4`=`[`、
`VK_OEM_6`=`]`、`VK_OEM_1` 等、および JIS 配列で `「」` 等を生むキー）は写していない。
このままだとブラケットキーが `UserActionEvent.codepoint` を生まず、InputState が開き/
閉じカッコを分類できないため**本機能が発火しない**。したがって M61-A は次を行う:

- **ブラケット/記号を生む VK を `Input` / `InputAlnum` へ写すエントリを M13 §1.4 の
  VK→UserAction 表（`tsf-tip/src/TextService.cpp::OnKeyDown` 内テーブル /
  `core/src/UserActionMap.cpp`）へ追加する。** codepoint は**入力モード依存**で、
  `ToUnicode`（キーボードレイアウト + シフト状態）または IME の現在モード
  （`hiragana` で `[`→`「` 等）から解決し、`UserActionEvent.codepoint` に載せる（§4.4）。
- 分類自体は codepoint ベース（下記）なので、表に載せる VK 集合は §4.1 の対応表を包含
  すれば十分（OEM_4/6/1/7/さらに丸括弧を生む Shift+8/9 等。実装時に対象 VK を確定）。
- `bracketPairing == false` のときは、これら VK は従来どおりアプリへパススルー（あるいは
  IME の通常文字処理）であり、新マッピングが既存挙動を変えないようガードする（§5.3 の
  `OnTestKeyDown` eaten 判定も `bracketPairing` ＆有効モードでのみ TRUE）。

> このマッピング拡張は M13（入力パイプライン）の VK 表を「不変」とはしないが、`UserAction`
> 列挙・InputState 種別・ClientAction 枠組みには破壊的変更を加えない（純粋追加）。M13 が
> 既に OEM キーを `Input` へ写すよう実装済みなら、本機能は分類層の追加のみで足りる。

分類（`bracketPairing == true` かつ現在の入力モードで有効なとき。§4.4）:

- codepoint が**開きカッコ**（§4.1 の `open` 集合）→ ペア挿入経路（§4.1）。
- codepoint が**閉じカッコ**（§4.1 の `close` 集合）→ スキップ判定経路（§4.2）。
- `UserAction::Backspace` → カーソルが空ペアの内側なら一括削除経路（§4.3）。

開き・閉じ両用の対称デリミタ（`"` `'` `` ` `` 等、`open == close`）は特別扱いする
（§4.1.1。既定 OFF）。

### 3.2 ClientAction（新規 3 種）

`docs/legacy-parity-spec.md` §1.3 の ClientAction に以下を追加する。

| ClientAction | 意味 |
|---|---|
| `insertBracketPair(open, close)` | `open`+`close` を挿入し、カーソルを両者の間に置く（既定の immediate）。`composition` トリガ時は preedit/候補として提示し確定操作で確定（§4.0.1） |
| `skipOverClosing(close)` | カーソル直後の文字が `close` のとき、挿入せずカーソルを `close` の外（右）へ進める（§4.2） |
| `deleteBracketPair()` | カーソル左右が空のカッコ対のとき、開き・閉じ両方を削除する（§4.3） |

TSF への翻訳は §5。実装は既存 `tsf-tip/src/TextService.cpp::ApplyClientAction`
（`docs/legacy-parity-spec.md` §1.3）に分岐を足す。

### 3.3 状態遷移（追加分）

`docs/legacy-parity-spec.md` §1.2 の遷移表に、`bracketPairing == true` のときの分岐を
加える（既定 immediate トリガ。§4.0）。新しい InputState 種別は**追加しない**。

| 現状 | 入力 | 次状態 | 副作用 |
|---|---|---|---|
| Idle | Input(開きカッコ) | Idle | `insertBracketPair`（commit OC + カーソル内側）。composition は残らない |
| Composing/Previewing | Input(開きカッコ) | Idle | **現在の composition を先に確定**（候補窓表示中は `CommitSelected`、それ以外は `CommitPreeditAsIs` / ローマ字 flush）→ その後 `insertBracketPair` |
| Selecting | Input(開きカッコ) | Idle | `CommitSelected` → `insertBracketPair` |
| Idle | Input(閉じカッコ) | Idle | カーソル直後が同じ閉じカッコなら `skipOverClosing`、無ければリテラル 1 文字挿入（§4.2） |
| Idle | Backspace | Idle | カーソル左右が空ペアなら `deleteBracketPair`、無ければ通常 Backspace（§4.3） |
| UnicodeInput / ReplaceSuggestion | Input(カッコ) | （変更なし） | これらの特殊モード中はペアリングしない（§4.6） |

`composition` トリガ（`bracketPairingTrigger == "composition"`）の遷移は §4.0.1。

## 4. 動作詳細

### 4.0 挿入トリガ（既定 immediate）

`bracketPairingTrigger` で挿入・確定の方式を選ぶ。既定は **`immediate`**。

- **`immediate`（既定）**: 開きカッコを打鍵した瞬間に `open`+`close` を確定挿入し、
  カーソルを両者の間へ移動する（Enter 不要）。対は**確定済みテキスト**であり、内側への
  入力は新しい composition として始まる。
- **`composition`**: 開きカッコを打鍵すると preedit に `open`+`close` を表示し（カーソルは
  内側）、確定操作（Enter / 数字 / クリック / 別キーでの auto-commit）で確定する。確定後の
  カーソル位置は immediate と同じ（内側）。`docs/legacy-parity-spec.md` の確定経路を流用。

どちらの方式でも対は**ローカル固定生成**であり、推論ホスト・候補 IPC を介さない（§7）。

#### 4.0.1 `composition` トリガの詳細

- 開きカッコ打鍵で `Composing` 相当の composition を作り、surface=`open`+`close`、
  キャレットは `open` と `close` の間（`ITfRange` の内側境界）に置く。
- ライブ変換（M14）/ 候補窓は使わない（固定対のため候補を問い合わせない）。確定操作で
  `EndComposition` し、カーソルを内側に残す（§5.2）。
- 確定前に Esc → composition 破棄（カッコは挿入されない）。確定前に別の開きカッコ →
  現対を確定してから新対を挿入（ネスト。§4.1.2）。
- この方式は当初ユーザー記述「`「` → `」` → Enter」に対応するが、既定は immediate と
  する（設定で切替可能。§6）。

### 4.1 カッコ対応表（組み込み既定）

`open`→`close` の固定表。全角（日本語）と半角（ASCII）を既定で持つ。codepoint は
すべて BMP（サロゲートなし）であり、UTF-16 code unit 長は各 1（§4.8・§5.3）。

**非対称ペア（既定 ON。`open != close`）**

| open | close | 名称 |
|---|---|---|
| `（` U+FF08 | `）` U+FF09 | 全角丸括弧 |
| `「` U+300C | `」` U+300D | かぎ括弧 |
| `『` U+300E | `』` U+300F | 二重かぎ括弧 |
| `【` U+3010 | `】` U+3011 | 隅付き括弧 |
| `〔` U+3014 | `〕` U+3015 | 亀甲括弧 |
| `［` U+FF3B | `］` U+FF3D | 全角角括弧 |
| `｛` U+FF5B | `｝` U+FF5D | 全角波括弧 |
| `〈` U+3008 | `〉` U+3009 | 山括弧 |
| `《` U+300A | `》` U+300B | 二重山括弧 |
| `“` U+201C | `”` U+201D | 全角ダブルクォート（曲線） |
| `‘` U+2018 | `’` U+2019 | 全角シングルクォート（曲線） |
| `(` U+0028 | `)` U+0029 | 半角丸括弧 |
| `[` U+005B | `]` U+005D | 半角角括弧 |
| `{` U+007B | `}` U+007D | 半角波括弧 |

**対称デリミタ（既定 OFF。`open == close`。`bracketSymmetricQuotePairing` で有効化）**

| char | 備考 |
|---|---|
| `"` U+0022 | 半角ダブルクォート |
| `'` U+0027 | 半角アポストロフィ（誤爆しやすく既定 OFF） |
| `` ` `` U+0060 | バッククォート |

既定 OFF / 要検討の文字（誤爆リスク）:

- `<` U+003C / `>` U+003E（半角山括弧）: 比較演算子・タグと衝突するため**既定では
  ペア対象外**。`bracket-pairs.tsv`（§4.5・M61-B）で opt-in 可。

対称デリミタの扱い（`bracketSymmetricQuotePairing == true` のとき。M61-B）:

- 同じ文字が開き/閉じ兼用のため、文脈で判別する。**カーソル直後が同じ文字なら
  スキップ**（§4.2）。そうでなく、**カーソル直前が空 / 空白 / 別の開きカッコ**（語境界）の
  ときのみペア挿入する。語の途中（直前が文字）では単一文字挿入とし、アポストロフィの
  誤爆を抑える。

### 4.1.2 ネスト

開きカッコ内で更に開きカッコを打つと、内側に新しい対を作る（`「あ（|）」` 等）。対の
追跡を持たない決定的方式（§4.8）でも、挿入は常にカーソル位置への OC 挿入＋内側移動で
あるため、ネストは自然に成立する。スキップ・空ペア削除も隣接文字判定なので各層で動く。

### 4.2 スキップ（閉じカッコの飛び越え）

`bracketSkipOverClosing == true`（既定 ON）かつ閉じカッコを打鍵したとき:

1. 読み取り用 EditSession で**カーソル直後（右）1 code unit**を読む（§5.3）。
2. それが今打鍵した閉じカッコと**等しければ**、挿入せずカーソルを 1 つ右へ進める
   （`skipOverClosing`）。これにより `「あ|」` で `」` を打つと `「あ」|` になり、`」` が
   二重化しない。
3. 等しくなければ、**リテラルに閉じカッコ 1 文字を挿入**する（通常の記号入力）。

判定根拠と既知の割り切り（§4.8 も参照）:

- 「カーソル右の文字」を見るだけなので、自分が自動挿入した閉じカッコか、ユーザーが
  別途打った閉じカッコかは区別しない。**右隣が同じ閉じカッコなら飛び越える**という
  単純規則を採る（多くのエディタの「閉じ括弧を打つと飛び越える」設定と同等）。
- 厳密に「自動挿入した対のみ飛び越える」挙動が要るときは、直近挿入対のベストエフォート
  追跡（カーソル隣接性が保たれている間のみ有効なフラグ）を**任意の精緻化**として
  M61-B で足す。コアは隣接文字判定で十分かつ堅牢とする。

### 4.3 空ペアの一括削除（Backspace）

`bracketBackspaceDeletesPair == true`（既定 ON）かつ `UserAction::Backspace` で、かつ
TIP の composition が無い（カーソルが確定済みテキスト中にある）とき:

1. 選択が**collapsed**（範囲選択でない）であることを確認する。範囲選択中は通常 Backspace。
2. 読み取り用 EditSession で**カーソル直前（左）1 code unit と直後（右）1 code unit**を読む。
3. 左が開きカッコ `O`、右がその対応閉じカッコ `C`（§4.1 の同一ペア）で**空ペア**（間に
   文字が無い）なら、左右 2 文字をまとめて削除する（`deleteBracketPair`）。
4. それ以外は通常の Backspace（左 1 文字削除。サロゲートペアは 2 code unit を 1 単位として
   削除する一般規則に従う）。

composition 中の Backspace は従来どおり（ローマ字 pending を戻す / かな 1 単位削除。
`docs/legacy-parity-spec.md` §1 / 既存 `TextService::OnKeyDown` の VK_BACK 経路）であり、
本規則は確定済みテキスト上のカーソルにのみ適用する。

### 4.4 入力モード別の挙動

`docs/legacy-parity-spec.md` の入力モード（`inputMode`: `hiragana` / `alnum_half` /
`alnum_full`）に応じて、打鍵キーが生む文字は IME の既存文字入力ロジックで決まる。本機能は
**生じた codepoint を §4.1 表で分類するだけ**なので、モード対応は自然に従う。

- `hiragana`: JIS 配列で `[`/`]` キー等が `「`/`」` を生む等、全角カッコが対象。
- `alnum_half`（`bracketPairingInAlnumMode == true`、既定 ON）: `(` `[` `{` 等の半角
  カッコが対象。
- `alnum_full`: 全角 ASCII カッコ `（` `［` `｛` が対象（§4.1 全角行）。

`bracketPairingInAlnumMode == false` のときは英数モード（`alnum_half` / `alnum_full`）で
ペアリングを抑制する（エディタでの二重化が気になるユーザー向け）。既定はユーザー選択に
従い**英数モードでも有効（true）**。

### 4.5 アプリ互換（二重化回避）

エディタ・IDE（VS Code・Visual Studio・JetBrains 系等）やブラウザ devtools は、アプリ
自身がカッコを自動ペアする。本機能と重なると `()` や `「」」` の二重化が起きる。本機能は
既定 OFF・オプトインで一次的に緩和されるが、有効化ユーザー向けに per-app 制御を持つ。

- `bracketPairingAppPolicy`（enum: `denylist`（既定）/ `allowlist`）:
  - `denylist`: denylist 掲載アプリ以外で有効。
  - `allowlist`: allowlist 掲載アプリのみで有効。
- 前面アプリ判定は `promptPrefixByApp`（rich X-2-6）と同じ「前面プロセス実行ファイル名」
  解決基盤を再利用する（拡張子込みのファイル名、大文字小文字を区別しない）。

#### 4.5.0 アプリリストのシリアライズ（`bracketPairingApps`）

アプリの deny/allow リストは**カッコ対応表（§4.5.1 の `bracket-pairs.tsv`）とは別物**で、
プロセス名を encode する独自スキーマを持つ。`bracket-pairs.tsv` には**アプリ名を書かない**
（同 TSV は `<open>\t<close>\t<flags>` のカッコ対専用）。アプリリストは次の二経路で供給する:

- **設定 `bracketPairingApps`（配列、`string[]`、M61-B）**: 前面プロセスの実行ファイル名
  （例 `"Code.exe"` `"devenv.exe"`）の配列。大文字小文字を区別しない。`bracketPairingAppPolicy`
  に従って解釈する:
  - `denylist`: **組み込み既定 denylist（定数シード）∪ `bracketPairingApps`** に載るアプリで
    本機能を無効化、その他で有効。
  - `allowlist`: `bracketPairingApps` に載るアプリでのみ有効（組み込みシードは無視）。
- **M48 アプリ別入力プロファイル（`docs/app-profile-spec.md`）**: M48 完了後は per-app の
  有効/無効をプロファイルが持ち、**`bracketPairingApps` 設定より優先**する（プロファイルに
  当該アプリのエントリがあればそれを使う）。M48 未完了時は `bracketPairingApps` 単独で動作する。

組み込み既定 denylist（`denylist` ポリシー時のシード。定数。実機・フィードバックで調整）:
`Code.exe`（VS Code）/ `devenv.exe`（Visual Studio）/ `idea64.exe`・`pycharm64.exe` 等
JetBrains 系 / `sublime_text.exe`。これらはアプリ側自動ペアが既定で働くため。`allowlist`
ポリシーではこのシードを使わず `bracketPairingApps` のみを許可集合とする。

#### 4.5.1 カッコ対応表の外部化（TSV、M61-B）

§4.1 の**カッコ対応表**（アプリリストではない。アプリリストは §4.5.0）は組み込み既定として
持つが、再コンパイルせず調整できるよう **TSV で上書き・追加**できる。M17 カスタムローマ字
テーブル（`docs/legacy-parity-spec.md` §5）と同じファイル運用・ホットリロード基盤
（`ReadDirectoryChangesW`）を再利用する。

- 既定パス: `%LOCALAPPDATA%\azooKey\bracket-pairs.tsv`（`bracketPairsPath` で上書き）。
- 形式（UTF-8、BOM 許容、1 行 1 対）: `<open>\t<close>\t<flags>`。`open == close` の行は
  対称デリミタ（§4.1.1）。`flags` は任意（例 `off` で当該既定対を無効化＝打消し）。**本 TSV は
  カッコ対専用で、アプリ名（プロセス名）は書かない**（アプリ deny/allow は §4.5.0 の
  `bracketPairingApps` 設定 / M48 プロファイル）。
- マージ規則: 組み込み既定を常にロードし、TSV 行は `open` キーで上書き・新規追加、
  `off` フラグで無効化（M59 §4.1.4 と同方針）。
- ファイル無しなら組み込み既定のみで動作（後方互換）。不正行は warning ログでスキップ。
- ファイルが無くても**コア（M61-A）は組み込み既定の対応表で完全動作**する。TSV 外部化と
  per-app プロファイル統合は M61-B。

### 4.6 特殊モード・IME 状態での扱い

- **UnicodeInput（U+XXXX 入力中）/ ReplaceSuggestion（AI 置換待ち）**: これらの特殊
  composition 中はペアリングしない（カッコはそのモードの入力 / 通常文字として扱う）。
- **IME Off / 直接入力（パススルー）**: TIP が入力を能動処理していない状態では、カッコ
  キーはアプリへ直接渡る。ペアリングは TIP が有効モードで入力処理しているときのみ働く。
- **セキュア入力（パスワード欄 / secure アプリ）**: 本機能は**ローカルなテキスト挿入のみ**で
  外部送信・推論・ログ収集を伴わない（§7）。したがって M46（プライバシー / セーフ入力
  モード）のような外部送信ゲートは不要。挿入内容のログ記録もしない（記号の発生のみを
  debug ログ可）。secure 欄で IME がオーバーライド状態のときは通常のリテラル挿入として
  振る舞う。

### 4.7 他の追加機能との相互作用

- **動的自動句読点（M59）**: M59 は記号列・カッコ近傍での句読点挿入を抑制する
  （`docs/dynamic-punctuation-spec.md` §4.1 抑制規則「記号列の内部」）。本機能が挿入する
  カッコ境界に読点が割り込まないよう、両者は独立に動作しつつ整合する。M59 はライブ変換
  preedit 上で動き、本機能は確定済みテキスト上で動くため経路が分離している。
- **ローマ字一括変換（M58）**: `batchRomajiConversion == true` の蓄積中
  （`BatchAccumulating`）は、カッコもローマ字バッファに**リテラル蓄積**し、ペアリングは
  **抑制**する（一括変換のバッファ正典性を保つため）。一括確定後の `Idle` ではペアリングが
  通常どおり働く。これにより蓄積中にカッコ対が割り込んでバッファ意味論を崩さない。
- **再変換（M20）**: 自動挿入されたカッコは読みを持たない確定済みリテラルであり、
  再変換時は文節境界 / リテラルとして扱い読みへ逆変換しない（`docs/dynamic-punctuation-spec.md`
  §5 のカッコ版。詳細は M20 統合時に `docs/tsf-deep-integration-spec.md` 側で確定）。

### 4.8 堅牢性とフォールバック

- **隣接文字判定で位置追跡を持たない。** スキップ（§4.2）・空ペア削除（§4.3）は打鍵
  時点でカーソル左右を読むため、外部編集・フォーカス遷移・マウスによるカーソル移動が
  あっても状態不整合を起こさない。長寿命 `ITfRange` を保持しない。
- **BMP 前提の比較。** §4.1 のカッコ文字はすべて BMP（1 code unit）。隣接文字読取で
  サロゲート片が来ても、比較対象が BMP カッコ文字なので一致せず、安全に通常挙動へ落ちる。
- **読み取り EditSession 失敗時のフォールバック。** スキップ / 削除判定のための同期読取
  EditSession（§5.3）が拒否される、または `GetSelection` が失敗するときは、判定を諦め
  **リテラル挿入 / 通常 Backspace**にフォールバックする（入力を絶対に失わない）。
- **範囲選択中。** §4.2 / §4.3 は collapsed カーソル前提。範囲選択中の Backspace は通常
  削除。範囲選択中の開きカッコは、`bracketWrapSelection`（M61-B）が ON なら囲み（§ 下記）、
  OFF なら選択置換のリテラル挿入（アプリ既定）。

### 4.9 選択テキストの囲み（M61-B・既定 OFF）

`bracketWrapSelection == true`（M61-B、既定 OFF）かつ範囲選択中に開きカッコを打鍵すると、
選択範囲を `open`…`close` で囲む（例: 選択 `あ` + `「` → `「あ」`、カーソルは閉じカッコの
後ろ or 選択を維持。実装時に確定）。`GetSelection` で範囲テキストを取得し、`open`+選択+
`close` を 1 回の `SetText` で置換する。**コア（M61-A）には含めない**（ユーザー選択スコープ
外）。本節は将来挙動の定義であり、既定 OFF。

## 5. TSF 操作（ClientAction → TSF 翻訳）

`docs/legacy-parity-spec.md` §1.3 の表に以下を追加する。実装は既存の commit 経路
（`tsf-tip/src/TextService.cpp` の `EditSession::DoEditSession`、現状
`committed_range->SetText` → `Collapse(TF_ANCHOR_END)` → `SetSelection`）の変形で足りる。

| ClientAction | TSF 操作 |
|---|---|
| `insertBracketPair(O,C)`（immediate） | カーソル位置（選択 range）に `O`+`C` を 1 回 `SetText` → その range を `Collapse(TF_ANCHOR_START)` 後 `ShiftStart(+len(O))` 等で `O` と `C` の境界へ collapse → `SetSelection`（§5.2） |
| `insertBracketPair(O,C)`（composition） | composition を作り surface=`O`+`C`、内側境界へキャレット。確定操作で `EndComposition` し内側を維持 |
| `skipOverClosing(C)` | カーソル range を `ShiftEnd(+1)`（1 code unit）→ `Collapse(TF_ANCHOR_END)` → `SetSelection`（挿入はしない。§5.3） |
| `deleteBracketPair()` | カーソル range を左へ `ShiftStart(-1)`・右へ `ShiftEnd(+1)` し空文字 `SetText`（左右 2 code unit 削除。§5.3） |

### 5.1 既存実装の再利用点

既存 commit パスは `committed_range->SetText(ec, 0, surface, len)` のあと
`committed_range->Collapse(ec, TF_ANCHOR_END)` → `context_->SetSelection(ec, 1, &sel)` で
**末尾**にキャレットを置く（`tsf-tip/src/TextService.cpp` 参照）。本機能の immediate 挿入は
この「末尾へ collapse」を「**`O` と `C` の境界へ collapse**」に変えるだけである。

### 5.2 カーソルを内側へ置く（immediate）

`O`+`C` を SetText した range（先頭=`O` の前、末尾=`C` の後）に対し:

1. `range->Collapse(ec, TF_ANCHOR_START)` で `O` の前へ。
2. `range->ShiftStart(ec, +len(O), ...)`（`len(O)` は code unit。§4.1 より全カッコ 1）で
   `O` と `C` の境界へ。
3. `TF_SELECTION sel{ .range = range, ... }` を `context_->SetSelection(ec, 1, &sel)`。

代替実装: SetText 後の range を `Collapse(TF_ANCHOR_END)`（`C` の後）→ `ShiftStart(ec,
-len(C), ...)` で **start のみ**を `O` と `C` の境界へ戻し、**最後に `Collapse(TF_ANCHOR_START)`
で end を start に揃えて collapse する**。この最終 collapse を省くと range は start=境界・
end=`C` の後で**非 collapsed のまま `C` を選択範囲に含み**、`SetSelection` がキャレットでなく
`C` を選択してしまう（immediate では直後の入力が `C` を置換する）。最終 collapse まで行えば
主経路（手順 1〜3）と同値。実装時にどちらかへ確定。

### 5.3 隣接文字の読取（スキップ・削除判定）

判定にはカーソル隣接の 1 code unit を読む。`OnKeyDown` 内で**同期の読取専用
EditSession**（`TF_ES_SYNC | TF_ES_READ`。既存 lifecycle commit が `TF_ES_SYNC` を使う
前例あり）を要求し、次を行う:

- 右 1 文字: 選択 range を clone → `Collapse(TF_ANCHOR_END)` → `ShiftEnd(ec, +1, &shifted)` →
  `GetText` で 1 code unit 取得。
- 左 1 文字: 同様に `Collapse(TF_ANCHOR_START)` → `ShiftStart(ec, -1, &shifted)` → `GetText`。

読み取り結果で §4.2 / §4.3 の分岐を決め、続く読み書き EditSession（または同一 RW
セッション）で `skipOverClosing` / `deleteBracketPair` / リテラル挿入を適用する。同期
セッションが拒否されたらフォールバック（§4.8）。

> **OnTestKeyDown の eaten 判定**: `bracketPairing == true` かつ現モードで有効なとき、
> 開きカッコ / （スキップ対象の）閉じカッコ / 空ペア内 Backspace を `*eaten = TRUE` に
> する必要がある。閉じカッコ・Backspace の最終挙動は隣接文字に依存するが、`OnTestKeyDown`
> 時点で「本機能が処理しうる」キーは eaten 宣言し、`OnKeyDown` で隣接文字を読んで実際の
> 挙動（スキップ or リテラル）を決める。リテラル挿入になる場合も TIP が文字を挿入する
> （eaten のまま）ため、アプリへ素通ししない（二重入力防止）。

### 5.4 Undo 単位

カッコ対は 1 回の `SetText` で挿入するため、理想的にはアプリ側 Undo 1 回で対が消える。
`SetSelection`（カーソル移動）は編集単位を作らない。実アプリの Undo 粒度はアプリ依存
（TSF/アプリ実装による）であり、本機能は単一 SetText に保つことで最善を尽くすに留める
（互換性は §8 の実機チェックで確認）。

## 6. 設定スキーマ

実装時に `settings/mvp-settings.schema.json` へ以下を追加する
（`additionalProperties:false` を維持。`description` に対応 M を記載する既存流儀に
合わせる）。本書（spec）が設定キーの正典であり、実ファイルへの追加は M61 実装時に
行う（本セッションは設計確定のみでスキーマファイルは変更しない。`docs/dynamic-punctuation-spec.md`
§8 と同方針）。

| キー | 型 | 既定 | M | 説明 |
|---|---|---|---|---|
| `bracketPairing` | boolean | `false` | M61-A | マスタートグル。true で自動カッコペアリングを有効化 |
| `bracketPairingTrigger` | enum `immediate`/`composition` | `immediate` | M61-A | 挿入・確定の方式（即時挿入 / preedit→確定操作。§4.0） |
| `bracketSkipOverClosing` | boolean | `true` | M61-A | 閉じカッコ直前で同じ閉じカッコを打つと飛び越える（§4.2） |
| `bracketBackspaceDeletesPair` | boolean | `true` | M61-A | 空ペア内側の Backspace で開き・閉じを一括削除（§4.3） |
| `bracketPairingInAlnumMode` | boolean | `true` | M61-A | 半角 / 全角英数モードでもペアリングを有効化（§4.4） |
| `bracketSymmetricQuotePairing` | boolean | `false` | M61-B | 対称デリミタ（`"` `'` `` ` ``）のペアリングを有効化（§4.1.1） |
| `bracketWrapSelection` | boolean | `false` | M61-B | 範囲選択中の開きカッコで選択を囲む（§4.9） |
| `bracketPairingAppPolicy` | enum `denylist`/`allowlist` | `denylist` | M61-B | per-app 有効範囲ポリシー（§4.5・§4.5.0。M48 統合） |
| `bracketPairingApps` | array(string) | `[]` | M61-B | deny/allow 対象の前面プロセス実行ファイル名（例 `"Code.exe"`）。`bracketPairingAppPolicy` に従い解釈。大文字小文字を区別しない（§4.5.0）。M48 プロファイルがあればそちらが優先 |
| `bracketPairsPath` | string | `%LOCALAPPDATA%\azooKey\bracket-pairs.tsv` | M61-B | カッコ対応表 TSV のパス（カッコ対専用・アプリ名は含まない。§4.5.1）。無ければ組み込み既定のみ。ホットリロード対応 |

設定 UI（M30）完成までは host CLI / 環境変数 / settings.json（`DEV-203` の SettingsStore）
経由で実効値を受ける（M58〜M60 と同方針）。

## 7. IPC（追加なし）

**本機能は IPC メッセージを追加せず、推論ホスト（`inference-host`）にも依存しない。**
カッコ対応は §4.1 の固定表で決まる決定的処理であり、`QueryCandidates` も `Commit*` 観測も
発火しない（自動挿入カッコは学習対象にもしない）。

帰結:

- Host 未起動・切断・劣化モードでも本機能は完全動作する（`docs/dev-infrastructure-spec.md`
  §8 の劣化モードと整合）。
- `composition` トリガ（§4.0.1）でも対は TIP がローカル生成するため候補 IPC は不要。
- ベンチ（`bench/`）への追加計測も不要（ローカル編集のみ・推論レイテンシ無し）。

この「無 IPC・Host 非依存」は M58（一括変換）・M59（動的句読点）・M60（英単語候補）と
明確に異なる本機能固有の性質であり、実装・テストの単純さに直結する。

## 8. テスト計画

- **カッコ対応表 / 分類** (`core/tests/bracket_pairing_test.cpp` 新規 or
  `input_state_test.cpp` 拡張): §4.1 の各 open→(open,close) 写像、対称デリミタの
  既定 OFF、`<`/`>` の既定除外、BMP code unit 長。
- **InputState 遷移** (`core/tests/input_state_test.cpp`):
  - Idle + 開きカッコ → `insertBracketPair`（immediate、カーソル内側）。
  - Composing/Selecting + 開きカッコ → 確定（CommitSelected / CommitPreeditAsIs）後に挿入。
  - 閉じカッコ：右隣が同一閉じカッコ → `skipOverClosing` / そうでなければリテラル挿入。
  - Backspace：左右が空ペア → `deleteBracketPair` / そうでなければ通常 Backspace、範囲
    選択中は通常削除。
  - `bracketPairing == false` で全カッコ・Backspace が従来挙動（回帰：挙動不変）。
  - `bracketPairingInAlnumMode == false` の英数モードでリテラル挿入になること。
  - `composition` トリガで preedit に OC が出て確定でカーソル内側、Esc で破棄。
- **TSF レベル** (`tsf-tip/tests/`、既存 `onkeydown_preedit_test.cpp` 流儀のモック context):
  - immediate 挿入後の `SetSelection` が `O` と `C` の境界に来ること（§5.2）。
  - 隣接 1 文字読取（右・左）でスキップ / 空ペア削除が分岐すること（§5.3）。
  - 同期読取セッション拒否時にリテラル挿入 / 通常 Backspace へフォールバックすること（§4.8）。
  - `OnTestKeyDown` がカッコ・該当 Backspace を eaten 宣言し、アプリへ素通ししないこと（§5.3）。
- **TSV 外部化** (`core/tests/`、M61-B): TSV パース・`open` キー上書き・`off` 無効化・
  不正行スキップ・対称デリミタ行。
- **アプリ互換** (状態機械 or TIP、M61-B): denylist 掲載アプリで抑制、allowlist ポリシーの
  反転、前面アプリ解決（`promptPrefixByApp` 基盤再利用）。
- **手動 / 実機（Win11、`gate:human-required`）**: メモ帳 / WordPad / ブラウザのテキスト
  欄 / Office で、開きカッコ打鍵でカーソルが内側に来ること、閉じカッコ打鍵で飛び越える
  こと、空ペア内 Backspace で両方消えること、`hiragana` / `alnum_half` 双方で動くこと、
  Undo 1 回で対が消える（アプリ依存）こと。denylist 掲載エディタ（VS Code 等、M61-B）で
  二重化しないこと。`bracketPairing == false` で挙動が一切変わらないこと。

## 9. 受け入れ条件（M61）

M61-A / M61-B の受け入れ条件は `plans/windows-port-roadmap.md` の M61 に定義する
（本書は仕様、roadmap は受け入れ条件の「定義」、達成状態は Linear）。

## 10. 参照

- 入力パイプライン（UserAction / InputState / ClientAction）: `docs/legacy-parity-spec.md` §1
- 既存 commit + カーソル配置の実装: `tsf-tip/src/TextService.cpp`（`EditSession::DoEditSession`
  の `SetText` → `Collapse(TF_ANCHOR_END)` → `SetSelection` 経路）
- per-app 入力プロファイル（denylist 統合）: `docs/app-profile-spec.md`（M48）
- 再変換境界・IME On/Off: `docs/tsf-deep-integration-spec.md`
- 記号列での句読点抑制: `docs/dynamic-punctuation-spec.md` §4.1
- 蓄積中のリテラル扱い: `docs/romaji-batch-conversion-spec.md`（M58）
- カスタム TSV のファイル運用・ホットリロード前例: `docs/legacy-parity-spec.md` §5（M17）
