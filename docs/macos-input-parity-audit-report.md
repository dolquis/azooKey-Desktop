<!-- lint:allow-file line-ref,status -->
<!-- 本ファイルは作成日時点の監査スナップショットであり、更新しない前提の記録である。 -->

# macOS 版との入力体験パリティ監査レポート

> **種別**: 監査・比較（特定時点のスナップショット）
> **作成日**: 2026-10-31
> **対象**: Windows 版 `dolquis/azooKey-Desktop` の `624e029a`（origin/main）と、macOS 版 `legacy/`（upstream `azooKey/azooKey-Desktop` の `51a4167b`、2026-05-16 相当）。upstream のその後の入力関連の変更も確認した（§7）。
> **位置づけ**: 本ファイルは時点依存の**監査記録**であり、正典ではない。
> - Windows 版の仕様の正典 → `docs/*-spec.md`。legacy との差は spec に「Windows 版の結論」として書く方針である（`docs/legacy-parity-spec.md` 冒頭）。
> - マイルストーン定義と受け入れ条件の正典 → `plans/windows-port-roadmap.md`
> - 状態・優先度・対応の追跡 → Linear（DEV-1810 と、そこから起票する課題）
> - `legacy/` は参照資産であり、本レポートは「Mac 版に合わせるべき」とは決めない。差ごとに「spec に Windows 版の結論を書く」か「Mac 版に合わせる」かを選べる形で示す。
> - 本レポートの重大度と対応案は作成日時点の見立てであり、本ファイルは更新しない。
>
> 調査方法: 両者を読み取り専用で 8 領域に分け、領域ごとのエージェントが実ファイルを読んで、file と symbol の根拠を付けた対応表を作った。重大度が高・中の指摘は親が実ファイルで確認し、そのうち主な 12 件は別のエージェントが反証する立場で再検証した。重大度が低の指摘は、領域ごとの監査者が示した file と symbol の根拠に依る。build・test・実機の打鍵はしていない。根拠は file と symbol で示し、行番号は書かない（調べたのは上記の snapshot）。

---

## 1. 要約

1. **基本の打鍵は同等か、spec に記録した意図した差に収まる。** ローマ字からかなへの変換、ライブ変換 ON での Space による変換、候補窓での Space・↑↓・数字 1〜9 による選択、Enter での確定、候補選択中の文字入力（確定して続ける）、Backspace、Unicode 入力の基本操作（16 進の入力・Backspace・Enter・Esc）は Mac 版と同じ結果になる。Esc 1 回での取り消しは、2026-10-17 の決定に基づく意図した差である。
2. **Mac 版の利用者が日常の打鍵で当たる差が 4 件ある（重大度 高）。**
   - 読み入力中の数字が読みに入らず、アプリへ渡る（P-01）。
   - 読み入力中の括弧や `@ : ;` などの記号が全角化されず、読みに入らない（P-02）。
   - Space の直後、候補が届く前に Enter を押すと、ひらがなのまま確定する（P-03。高速な機械では中、CPU だけの機械やバッテリ駆動で高）。
   - F6〜F10 と Ctrl+J/K/L/;/: による字種変換が無く、1 キーで字種を変えて確定する手段が無い（P-04）。
3. **既定値の差が、最初に触ったときの差を大きくしている。** ライブ変換が Mac 版は ON、Windows 版は OFF である。日本語モードの Space は、Mac 版の既定が全角、Windows 版は半角である。予測窓は Mac 版では開発中の機能として OFF、Windows 版は ON である（P-05、P-06、P-16）。
4. **spec と roadmap に関する指摘が 10 件ある**（§6）。spec の中の食い違い、spec と実装の不一致、legacy との差の書き漏れを含む。代表は、ひらがなキー（`VK_DBE_HIRAGANA`）の処理が spec にあって実装に無いこと、OEM 記号キーを core の表に入れるという記述が実装と合わないこと、設定キー `inputMode` が表示名どおりに効かないことである。
5. **`docs/legacy-parity-spec.md` §1.4 は「レガシーの VK 表」と呼ばれているが、legacy の割当と一致しない。** Windows 版の挙動を spec に書いてはいても、legacy との差として書いていない項目が多い。§1.4 に legacy との差を 1 行ずつ足せば、本レポートの「記録のない差」の多くは「意図した差」になる。
6. **打鍵の取りこぼしや順序の逆転は、通常の打鍵では起きない構造になっている。** キーを消費するかの判定と読みの更新は TIP の中で同期的に行い、Host に依存するのは候補と表示だけである。upstream の Mac 版が #343・#345 で直した入力漏れに当たるリスクは見つからなかった（§7）。差は、応答を待つ間に押したキーの意味（P-03、P-08）と、遅い Host での表示（P-15）に集中している。

## 2. 比較の基準と方法

### 2.1 基準

| 側 | 基準 | 補足 |
|---|---|---|
| macOS 版 | `legacy/`（upstream `51a4167b`） | `docs/legacy-parity-spec.md` が参照する版。upstream のその後の commit のうち、#343・#345・#346・#373・#386・#394・#396 を追加で読んだ |
| Windows 版 | `624e029a`（origin/main） | DEV-1806（ライブ変換の表示を応答待ちにひらがなへ戻さない）を含む |

Mac 版のローマ字表、AZIK 表、かな表、`firstClauseResults`、`forgetMemory` の中身は依存パッケージ AzooKeyKanaKanjiConverter の中にあり、repo に無い。これらに依存する Mac 側の挙動は「推測」と明記した。

### 2.2 監査領域

| 領域 | 範囲 |
|---|---|
| A. キーの割当 | Mac `UserAction.getUserAction` と Windows `core/src/UserActionMap.cpp`・`TextService::OnTestKeyDown` / `OnKeyDown` の前処理 |
| B. 状態遷移 | Mac `InputState.event` の全セルと Windows `core/src/InputState.cpp` |
| C. 文字入力 | ローマ字表、記号・数字・スペース、入力方式、キー配列、dead key |
| D. 変換と候補の操作 | Space、候補の選択と並び、確定と学習、ライブ変換、字種変換、文節、予測、忘却 |
| E. 表示 | 入力中の文字の属性、候補窓、予測窓、位置、入力モードの表示、UI-less |
| F. 選択テキストの変換 | 起動キー、プロンプト UI、結果の反映（AI の品質は対象外） |
| G. 応答の遅れと取りこぼし | 打鍵の順序、応答前に押したキー、遅い Host、表示のちらつき、デッドロック |
| H. 設定と既定値 | Mac の設定項目と Windows の設定キー、既定値、設定 UI |

対象外: Windows 独自機能そのものの評価（カッコ補完、リライター、日英混在入力、一括変換など）、配布、モデル管理、プライバシー、学習データの形式。

### 2.3 分類と重大度

| 分類 | 意味 |
|---|---|
| 同等 | 同じ打鍵で同じ結果・表示になる。Mac 固有のキーは、Windows で同じ働きをする操作に置き換わっていれば同等とする |
| 意図した差 | spec か roadmap に Windows 版の挙動と理由がある |
| 意図した差（legacy 差は未記載） | spec に Windows 版の挙動は書いてあるが、legacy との差としては書かれていない |
| 記録のない差 | spec にも roadmap にも記述が無い |
| spec と実装の不一致 | spec の記述と実装が食い違う。spec の中での食い違いも含める |
| 欠落 | roadmap 上は予定に入っているが、実装が無い |
| 記録のない予定 | spec に予定として書かれているが、それを実装範囲に含む milestone が roadmap に無い |

| 重大度 | 意味 |
|---|---|
| 高 | 日常の打鍵で頻繁に起き、入力が消える・誤る |
| 中 | 操作や表示が違い、Mac 版に慣れた人が戸惑う |
| 低 | まれに起きる、または別の操作で代わりがきく |

## 3. 指摘一覧

### 3.1 重大度 高

#### P-01 読み入力中の数字が読みに入らず、アプリへ渡る

- **分類**: 意図した差（legacy 差は未記載）
- **Mac**: `InputState.swift` の composing で `.number` を `appendPieceToMarkedText` にする。`kyouha2kai` は「きょうは2かい」になり、まとめて変換できる。
- **Windows**: `core/src/UserActionMap.cpp` の `IsDigitRow` 分岐は、Selecting 以外の数字を `nullopt`（アプリへ渡す）にする。TIP が数字を取り込むのは `numberRewriter` が true のときだけで（`TextService::OnTestKeyDown` の `decimal_digit && ... number_rewriter_` 分岐）、既定は false である。
- **spec**: `docs/candidate-rewriter-spec.md` §3 は「`numberRewriter == false` のとき、数字キーは従来どおりアプリへ渡す」と定める。`docs/legacy-parity-spec.md` §1.5.3 は「Selecting 以外の数字は nullopt」と定め、M10 の挙動の保存を理由にしている。`tsf-tip/tests/keymap_test.cpp` もこの挙動を固定している。どちらの spec も legacy との差には触れていない。§1.5.3 の根拠の段落には「数字は他で文字入力」とあり、同じ節の規定と食い違う。
- **ほかに数字を取り込む経路**: カスタムローマ字表（`inputStyle = custom`）と絵文字の `:trigger` だけで、どちらも既定 OFF である。TIP は素通りしたキーについて composition に何もしない（確定もしない）。
- **影響**: 「3時」「2回」「第1」のように数字を含む語を打つと、composition が開いたまま数字がアプリへ渡る。数字が composition の内側と外側のどちらに入るか、composition が確定されるかはアプリ次第で、静的には決まらない（実機確認 C-01）。Space の直後、候補が届く前に押した数字も同じくアプリへ渡る（Mac は候補番号として確定する）。
- **対応の選択肢**: (a) 読み入力中の数字を読みに取り込む（Mac に合わせる。`numberRewriter` の候補追加とは切り離す）。(b) `numberRewriter` を既定 ON にする。(c) 現状を Windows 版の結論として §1.5.3 に legacy との差を書く。

#### P-02 読み入力中の括弧・記号が全角化されず、読みに入らない

- **分類**: 記録のない差。括弧については spec と実装の不一致
- **Mac**: `KeyMap+hankaku2zenkaku.swift` の表で 30 字余りを全角にして読みに足す（`[`→「、`]`→」、`(`→（、`{`→『、`@`→＠、`:`→：、`;`→；、`~`→〜、`/`→・ など）。
- **Windows**: 読みに取り込む記号は `、。？！`、読み入力中の `-`（`ー` にする）、読み入力中の `/ < >`（ASCII のまま）、Shift+1 だけである（`TextService` の `IsCompositionSymbol` / `TranslateOemCompositionSymbol`、`docs/tsf-deep-integration-spec.md` §3.5）。括弧キーは `bracketPairing` が ON のときだけ `HandleBracketKey` が扱い、既定（OFF）では素通りする。ON のときも記号を読みに足さず、読みを確定してからペアを入れる。他の記号は半角のままアプリへ渡る。読みが無いときの `-` も素通りする（Mac は `ー` で入力を始める）。読み入力中の Shift+`-`（JIS の `=`、US の `_`）は `ー` になる（Mac は `＝` / `＿`。静的に読んだ結果で、テストは無い）。
- **spec**: `docs/legacy-parity-spec.md` §1.5.3 と `docs/bracket-pairing-spec.md` §3.1 は「M61-A の OEM キー（`VK_OEM_4` など）は `UserActionMap` へ `Input` として追加する」と書くが、`core/src/UserActionMap.cpp` は追加しておらず、`tsf-tip/tests/keymap_test.cpp` は `VK_OEM_4` を全状態で `nullopt` に固定している。実装では TIP の `HandleBracketKey` が `MapTipKey` より前に処理する。失効しているのは仕組みの記述で、カッコ補完そのものは動く。`docs/bracket-pairing-spec.md` §3.1 は OFF のときパススルーと定めるが、legacy との差に触れていない。`docs/tsf-deep-integration-spec.md` §3.5 は `/` を `/` のまま取り込むと定め、Mac の `・` に触れていない。
- **影響**: 「「」」「（）」を含む文、`ji:kan` のような読みを打つと、記号が半角でアプリへ渡り、P-01 と同じく composition との位置関係が崩れうる。
- **対応の選択肢**: (a) ひらがなモードで OEM 記号を全角化して読みに取り込む（Mac に合わせる。カッコ補完の ON/OFF とは切り離す）。(b) §1.5.3 の記述を実装に合わせて直し、legacy との差を書く。

#### P-03 Space の直後、候補が届く前に Enter を押すと、ひらがなのまま確定する

- **分類**: 意図した差（利用者の体験としての判断は未記載）
- **Mac**: Space で候補を同期的に作るので、Space→Enter は先頭候補の確定になる。
- **Windows**: `core/src/InputState.cpp` の `StartConversion` は候補窓用の `QueryCandidates` を出して Composing（または Previewing）のまま応答を待つ。その間の Enter は `CommitAsIs` で読みを確定する。既定はライブ変換 OFF なので、ひらがなが確定する。
- **spec**: `docs/legacy-parity-spec.md` §1.2 と §1.5.4 は「候補が未到着のうちは Selecting へ遷移しない。Enter は `CommitPreeditAsIs`」と定め、理由を M10 の挙動の保存としている。誤確定になるという体験面の判断は書かれていない。
- **TIP 側の保留**: 無い。句読点の確定待ち（`BeginFinalPunctuationHold`）はライブ変換 OFF では効かない。応答の通知は PostMessage で届くので、応答が先に届いていれば後の Enter より先に処理される。誤確定が起きるのは、応答の到着か 750ms の期限の早いほうまでの間に Enter を押したときに限られる。
- **影響**: 「kanji → Space → Enter」を窓要求の往復時間より速く打つと、Mac は「漢字」、Windows は「かんじ」を確定する。待ち時間は「直前の打鍵の要求の残り（期限 150ms）＋窓要求そのもの」になる。窓要求に近い実測は `docs/zenzai-inference-spec.md` の i7-12700KF・CPU で p50 10〜40ms、p95 47〜66ms（候補 5 件、NLL なしの条件）で、設計目標は p50 < 80ms、p95 < 180ms（`docs/dev-infrastructure-spec.md`）である。`docs/dev-infrastructure-spec.md` §8.5.2 は、窓要求が CPU だけの機械やバッテリ駆動では 150ms を超えると書いている。
- **重大度の見立て**: 高速な機械では、画面を見ずに Space→Enter を続けて打つ人に限られるので中（推測）。CPU だけの機械やバッテリ駆動では頻度が上がるので高。
- **対応の選択肢**: (a) 応答待ちの Enter を保留し、応答の到着か期限切れで第 1 候補を確定する（句読点の確定待ち `BeginFinalPunctuationHold` が前例になる）。(b) 現状を維持し、§1.5.4 に体験面の判断と理由を書く。

#### P-04 F6〜F10 と Ctrl+J/K/L/;/: による字種変換が無い

- **分類**: 記録のない差（roadmap にも無い）
- **Mac**: F6（ひらがな）、F7（カタカナ）、F8（半角カナ）、F9（全角英字）、F10（半角英字）と、Ctrl+J/K/;/L/: が同じ働きをする（`UserAction.swift`、`InputState.swift` の `.function`、`azooKeyMacInputController` の `submit*Candidate`）。1 回で確定し、確定した語は学習される。候補選択中は、選択中の候補が覆う部分だけを変える。
- **Windows**: どれも割当が無く、composition を残したままアプリへ渡る。F10 は Debug ビルドだけデバッグ窓に使う（`docs/legacy-parity-spec.md` §8.1）。1 キーで字種を変えて確定する手段は無い。代わりの手段は 3 つある。Enter で確定した直後に無変換キーを押すと、直近の確定をカタカナ→半角カナ→全角英数→半角英数と巡回できる（`docs/tsf-deep-integration-spec.md` §3.2。キャレットが確定位置にあり、間に他のキーを打たないことが条件）。既定 OFF のカタカナリライターを ON にすると、候補窓に全角・半角カタカナが加わる。ひらがなは読みそのものの候補として選べる。読み入力中に無変換キーを押すと、選択が無いのでアプリへ渡る。
- **spec**: `docs/legacy-parity-spec.md` §1.4 は Ctrl+I/O/S を「将来: カタカナ・英字・半角変換」に予約しているが、legacy の Ctrl+I/O は文節の伸縮、Ctrl+S は AI の提案であり、legacy と食い違う。F6〜F10 の記述は無い。
- **影響**: 「アイパッド」「ID」のように字種を指定して 1 打で確定する操作が Windows ではできない。F7 を押すと Edge や Chrome のキャレットブラウズ、F10 を押すと Win32 アプリのメニューバーが動く（推測）。F6〜F10 は MS-IME とも共通の操作で、MS-IME の利用者も日常的に押す。
- **対応の選択肢**: (a) MS-IME と共通の F6〜F10 を spec と roadmap に新設する（Ctrl+J/K/L/;/: も含めるかは別に決める）。(b) 無変換キーの巡回を Windows 版の結論として §1.4 に書き、legacy との差を明記する。

### 3.2 重大度 中

| ID | 指摘 | 分類 | Mac | Windows | spec | 対応の選択肢 |
|---|---|---|---|---|---|---|
| P-05 | ライブ変換の既定が違い、ライブ OFF での Space の段数も違う。Mac はライブ OFF の 1 回目の Space で窓を出さずに先頭候補を表示し、2 回目で窓を開く。Windows は 1 回目で窓を開くので、次候補へ進むのが 1 打早い | 既定値は意図した差（legacy 差は未記載）。Space の段数は記録のない差 | `BoolConfigItem.swift` `LiveConversion` 既定 true、`InputState.swift` composing の `.space` → `enterFirstCandidatePreviewMode` | `settings/mvp-settings.schema.json` `liveConversion` 既定 false、`InputState::StartConversion` | 既定値は sideload spec にあるが Mac と違う理由は無い | 既定を true にする / false の理由と Space の段数を §1.2 に書く |
| P-06 | 日本語モードで読みが無いときの Space が半角になる。Mac の既定は全角で、Shift+Space は半角、`TypeHalfSpace` で反転する。Shift+Space の半角は Mac と同じで、違うのは Shift なしの Space だけ。Windows には全角空白を入れる経路が無い | 記録のない差 | `UserAction.swift` の Space、`TypeHalfSpace` 既定 false | `UserActionMap.cpp` の `kSpace` は Idle で `nullopt` | なし | 全角スペースの設定を足して既定を決める（MS-IME の既定も全角） / 結論を spec に書く |
| P-07 | 読み入力中・候補選択中に、未定義の Ctrl 組合せ（Ctrl+A/C/V/X/Z など）、予測窓が無いときの Tab、↓、→、候補選択中の `0`、PageUp/PageDown が、composition を残したままアプリへ渡る。TIP は素通りした後に composition を確定しない。Mac は入力中これらを消費するか動作に使う（未定義の Ctrl は消費、Tab は予測の受理か消費、↓ は窓を開く、→ ← ↑ は消費、`0` は確定して入力を続ける）。Mac の Ctrl+S は AI が無効（既定）ならアプリへ渡るので、ここでは差にならない | 記録のない差（素通しは spec にあるが、そのとき composition をどうするかは書かれていない） | `InputState.swift` の `.control` + `.unknown` の consume、composing の `.tab` / `.navigation`、selecting の `.number(.zero)` | `TextService::OnTestKeyDown` の Ctrl 判定と Tab 判定、`UserActionMap.cpp` の矢印・数字 | `legacy-parity-spec.md` §1.4 の補足と §1.5.3 | 読み入力中は消費する / §1.4 に legacy との差と composition の扱いを書く |
| P-08 | Space を押して候補を待つ間にもう一度 Space を押すと、要求をやり直す。Mac の「2 回目の Space で次候補」はどの経路でも消える。1 回目の窓要求が送信済みなら、窓が出るのも遅れる。一括変換とリライター経路には再送抑止があるが、2 回目の Space を消費するだけで次候補へは進まない。core 経路には再送抑止が無い。Space、Space、Enter と速く打つと、やり直しの待ち中の Enter になり P-03 と重なる | 記録のない差 | 同期処理 | `InputState::StartConversion` の再実行、`TextService::PostQueryCandidates` → `CancelInflightWindowQueryLocked` | `dev-infrastructure-spec.md` §8.5.2 は窓要求の Cancel だけを定める | 待ち中の Next/Prev を数えて応答時に選択位置を進める / 最低でも再送をやめる |
| P-09 | 部分確定と文節の伸縮が通常変換に無い。Mac は候補列の先頭に先頭文節の候補を並べ、Enter や → で選択中の候補が覆う部分だけを確定して残りを続ける。Shift+←→ と Ctrl+I/O で区切りを動かせる。Windows の通常変換は読み全体を確定し、文節を扱えるのは既定 OFF の一括変換だけ | 意図した差（根拠は弱い） | `SegmentsManager.swift` の候補の並びと `editSegment`、`InputState.swift` の `basedOnSubmitCandidate` | `InputState.cpp` の Selecting の Commit、`TextService` の文節移動は一括変換のみ | roadmap のスコープ外「文節エディット機能（上流計画）」。legacy に既にある部分確定と `editSegment` は名指ししていない | 「通常変換は読み全体、文節は一括変換」と spec に結論を書く / 部分確定を入れる |
| P-10 | 候補の並びと補助候補が違う。Mac は先頭で ↑ を押すと英数・全角英数・半角カナ・カタカナ・ひらがなの候補を出す。Windows の既定では TIP 側の補助候補が無い（カタカナはリライター ON のときだけ末尾、英字は M60 で opt-in） | 記録のない差 | `SegmentsManager.swift` の補助候補 | `TextService` の候補の組立て | `candidate-rewriter-spec.md` §18.5 は Windows 側だけを定める | 補助候補の既定を決めて spec に書く |
| P-11 | 候補窓は全候補を 1 枚に並べ、スクロールもページ送りも無い。PageUp/PageDown はアプリへ渡る。数字で選べるのは 1〜9 件目だけで、10 件目以降は Space か ↓ で辿る。TIP の上限は 9 件だが、既定でも「nn＋母音」の曖昧な読み（DEV-1734）では最大 12 件ほどになる（Host の結合処理を静的に読んだ結果）。リライターや英単語候補を ON にするとさらに増える。幅は 400px で頭打ちになり、長い候補は省略記号で切れる。Mac は 9 行表示とスクロールで、数字は表示中の 9 行の中で選ぶ。窓は候補に合わせて広がる。PageUp/PageDown のページ移動は upstream の #394 で入ったもので、`legacy/` には無い | ページ送りは意図した差（根拠は弱い）、数字で選べない候補と幅は記録のない差 | `CandidateView.swift`、upstream `9406e344` | `CandidateWindow.cpp`、`TextService.h` の `max_candidates_`、`inference-host/src/InferenceEngine.cpp` の `QueryAmbiguousCandidatesEx` | `legacy-parity-spec.md` §1.6.2 は karukan との比較でページ送りを先送りし、legacy のスクロールには触れていない。`native-ui-spec.md` §4.1 は幅を定める | ページ送りかスクロールを入れる / 件数と幅の上限と数字選択の範囲を結論として書く |
| P-12 | 英数キーがトグルになっている。Mac の英数は英語への絶対指定で、読みを残したまま英字を足す。Windows は 2 回押すとひらがなに戻り、入力中の読みは学習観測なしで確定される。Mac のかなキーに当たるひらがなキー（`VK_DBE_HIRAGANA`、0xF2）とカタカナキー（0xF1）は処理が無い。英数モード中は全キーが素通りするので、このキーで英数モードを抜けられない。Windows 版にはカタカナ入力モード自体が無い | 英数は意図した差（legacy 差は未記載）。ひらがなキーは spec と実装の不一致 | `InputState.swift` の `.英数` / `.かな` | `TextService::OnKeyDown` の `VK_OEM_ATTN` 分岐。`VK_DBE_HIRAGANA` / `VK_DBE_KATAKANA` の処理は無い | `tsf-deep-integration-spec.md` §3.1 は `VK_DBE_HIRAGANA` を「input mode = hiragana」と定める | ひらがなキーを実装する（`VK_IME_ON` と同じ処理） / spec から外す。英数キーの方式を §3.1 に legacy との差として書く |
| P-14 | ライブ変換 OFF の打鍵中に、Mac が出す 1 行の先頭候補プレビュー窓が Windows に無い。Mac は予測窓をその右に並べる | 記録のない差 | `SegmentsManager.getCurrentCandidateWindow`、`azooKeyMacInputController` の 1 行窓 | Space の後にしか窓を開かない | `legacy-parity-spec.md` §3.1 は予測窓だけを定める | プレビュー窓を足す / 不採用を §1.2・§3.1 に書く |
| P-15 | ライブ変換の表示の差。Backspace の後、Mac は必ずひらがなに戻すが、Windows は変換済みの表示を引き継ぐ。打ち続けている間、Windows は最後に表示した応答のまま変換済みの部分が止まり、残りはかなで表示され、手を止めた時点で全体が切り替わる | Backspace は意図した差（legacy 差は未記載）。打鍵中の停滞は意図した差 | `SegmentsManager.swift` のライブ変換の表示 | `TextService::LiveShownSurface`、`tsf-tip/src/LiveCarry.cpp`、鮮度判定 | `legacy-parity-spec.md` §2.3.1・§2.5 | §2.3.1 に legacy との差を書く。停滞は、読みの先頭が一致する stale 応答を carry の候補に使う案がある |
| P-16 | 予測窓の既定が違う。Mac は開発中の機能として既定 OFF で、Tab は予測を受理しない。Windows は既定 ON で、予測窓があるときの Tab で第 1 候補を受理する | 記録のない差 | `DebugPredictiveTyping` 既定 false | `predictionEnabled` 既定 true | `legacy-parity-spec.md` §3.6 は既定 true と書くだけ | §3.6 に legacy との差を書く |
| P-18 | 入力方式の選択肢が少ない。Mac は AZIK、かな入力（JIS / US）、カスタム表を選べる。Windows は内蔵表とカスタム表だけで、AZIK とかな入力は roadmap にも無い。カスタム表は英字を小文字にするので Shift を区別できず、内蔵表の促音や `-` の処理を通らない。Mac にある GUI エディタ、プリセット、書き出しも無い | 記録のない差 | `CustomCodableConfigItem.swift` `InputStyle`、`RomajiTableEditorWindow.swift` | `inputStyle`（default / custom）、`CustomRomajiLoader.cpp` | `legacy-parity-spec.md` §5 | かな入力と AZIK を roadmap に置くか決める。カスタム表の制約を §5 に書く |
| P-19 | 句読点の字種（`、。` / `，．` / `、．` / `，。`）を選べない。Windows は `、。` に固定されている | 記録のない差 | `PuntuationConfigItem.swift` | `TextService` の句読点の取り込み | `dynamic-punctuation-spec.md` §1.1 は別機能とだけ書く | 設定を足す / 結論を spec に書く |
| P-20 | 設定キー `inputMode`（hiragana / alnum_half / alnum_full）が文字入力に効かない。カッコ補完と `:` トリガの判定にしか使われず、alnum_full にしても英字は全角にならず、起動時の入力モードにもならない。設定アプリの表示名は「起動時の入力モード」で、利用者に誤った期待を与える。Host も値を読むが使わない | spec と実装の不一致 | 相当なし | `TextService` の `alnum_mode_` は実行時のトグルでだけ変わる | `bracket-pairing-spec.md` §4.4 は定義を legacy-parity-spec に委ねるが、同 spec に `inputMode` は無い。§4.7 は用途をカッコに限る。`tsf-deep-integration-spec.md` §4.2 はオフ→オンでひらがなに初期化すると定める | キーの名前と表示名をカッコ補完用に直す / 起動時のモードとして実装する |
| P-21 | 選択テキストの AI 変換（英数・かなのダブルタップ）が TIP に無い。core の状態機械はあるが、ダブルタップの検出も `TransformSelectedText` の IPC も無い。spec §4 の割当も Mac と違う（Mac のダブルタップは同じ窓で初期プロンプトだけを変える。spec はかなを「言い換え」の別機能とする。spec の英数キー `VK_OEM_AUTO` は半角/全角キーで、判定間隔は Mac の 0.5 秒に対して 300ms）。Mac のプロンプト窓はプレビューを確かめてから適用する 2 段だが、spec のダイアログは 1 段 | 欠落（M16）＋ spec が Mac と食い違う | `azooKeyMacInputController+SelectedTextTransform.swift`、`PromptInput/*` | `TextService::MapTipKey` が `StartAlnumDouble` / `StartKanaDouble` を落とす | `legacy-parity-spec.md` §4、roadmap M16 | M16 の着手前に §4 の割当・キー・間隔・UI の段階を決め直す |
| P-22 | 学習の忘却のキー・状態・対象が違う。Mac は Ctrl+Delete（Backspace）で、候補選択中に選択中の候補の記憶を消す。Windows は Ctrl+Shift+Backspace で、入力していないときに直近に確定した組を消す。確定前の候補を忘れさせる手段が無い | 意図した差（legacy 差は未記載） | `UserAction.swift` の Delete、`SegmentsManager.forgetMemory` | `UserActionMap.cpp` の Ctrl+Shift+Back、`TextService::ForgetLastCommit` | `legacy-parity-spec.md` §7 | §7 に legacy との差を書く |
| P-23 | ユーザー辞書を設定アプリから追加できない。Mac は GUI で追加・編集・削除・書き出しができ、既定で「あずーきー → azooKey」が入っている。Windows の追加手段は開発者向け CLI だけで、v1.0 の「ユーザー辞書を編集」ボタンも設定アプリに無い | v1.0 のボタンは spec と実装の不一致。フル GUI は欠落（v1.x） | `UserDictionaryEditorWindow.swift` | `settings-app/` の画面 | `sideload-packaging-spec.md` §3.7 | ボタンを置く / spec の v1.0 範囲を直す |
| P-24 | 字種の巡回の出力が違う。Mac は打った綴りそのものを出す（大小も打鍵どおり）。Windows はかなからヘボン式の綴りを作り直して大文字にする | 意図した差 | `SegmentsManager.swift` の英数候補 | `core/src/CharacterFormCycle.cpp` | `tsf-deep-integration-spec.md` §3.4 | なし（記録どおり） |
| P-25 | Shift+英字を日本語モードで打つと、Windows はかなにする（`Apple` →「あっpぇ」。大文字は英語候補のためにだけ保持する）。Mac は表に規則が無ければ大文字のまま残す（推測。表が repo 外） | 記録のない差（Mac 側は推測） | `UserAction.swift` の keyMap | `core/src/RomajiKanaConverter.cpp` | なし | Mac 側を実機で確かめてから決める |

### 3.3 重大度 低

| ID | 指摘 | 分類 |
|---|---|---|
| P-13 | 既定設定で Space を押して候補窓が出ている間も、入力中の文字の下線が入力中と同じ（細い実線）である。Mac は選択中の候補を `kTSMHiliteSelectedConvertedText` で強調し、残りの読みを `kTSMHiliteConvertedText` で描く。Windows で文節の属性が付くのは一括変換の文節があるときだけで（`TextService::ShowsBatchSegments`）、core 経路には Focused 属性を付ける経路が無い。preedit は候補の文字列に置き換わり候補窓も出るので、変換中であることは分かる。差は下線の強調だけ | spec と実装の不一致（`tsf-deep-integration-spec.md` §5.5 と roadmap M23 の受け入れ条件「nihongo → Space → 注目文節が青背景」が、core 経路を fallback とする §5.4・§5.6 に追随していない） |
| P-17 | UI-less の環境（アプリが候補を描く）でも、読み入力中の予測窓は TIP 自前の窓として出る（候補窓の表示中は出ない）。アプリの UI と重なりうる | 記録のない予定（`tsf-deep-integration-spec.md` §8 は Suggestion UI へ渡す予定を書くが、§8 を実装範囲に含む milestone が roadmap に無い） |
| P-26 | Ctrl+P/N が Windows では候補選択中だけ ↑↓ になる（Mac は入力中も）。Ctrl+F は文節移動だけ。Ctrl+M（Enter）は割当が無い | 意図した差（legacy 差は未記載）／Ctrl+M は記録のない差 |
| P-27 | Ctrl+S（AI の提案・続き）が無い。spec は Ctrl+S を半角変換に予約していて legacy と食い違う。`TransformShortcut` とピン留めプロンプトのショートカットも無い | 記録のない差 |
| P-28 | `¥` と `\` の扱い（`TypeBackSlash`）、テンキーの `/ , .` が読みに入らないこと、Option の組合せ、dead key、Option 直接全角入力 | 記録のない差（Mac 固有の操作を含む） |
| P-29 | Windows の内蔵表に `z.` などの z 系記号が無い（zh/zj/zk/zl だけある）。Mac の表にあるかは repo 外で確かめられない | 記録のない差（Mac 側は推測） |
| P-30 | Unicode 入力中の Space は確定したうえで Space もアプリへ渡る。hex 以外のキーは確定して通常入力に移る。範囲外は beep（Mac は黙って閉じる） | 意図した差（legacy 差は未記載） |
| P-31 | 候補窓のクリックで確定できる（Windows のみ）。番号の書式、選択中の強調色、フォントと行の高さ（候補窓と予測窓で文字の大きさが違う）、角丸・半透明・行の罫線の有無 | 意図した差が中心 |
| P-32 | 予測窓は最大 5 行（Mac は 3 行）で、Tab の記号が無く、絵文字がカラーにならない（`native-ui-spec.md` §3.4 との関係が曖昧） | 意図した差／記録のない差 |
| P-33 | `native-ui-spec.md` のフェードイン、選択の移動アニメ、UIA イベント、ハイコントラスト用フォントの実装が見つからない | spec と実装の不一致 |
| P-34 | ライブ変換で 1 文字の読みも変換を表示する（Mac は 2 文字以上）。ライブ OFF の入力中に Mac が出す 1 行窓の位置関係（P-14）に伴い、予測窓の位置も違う | 記録のない差 |
| P-35 | 確定直後の最初の問い合わせが、CommitObservation の ACK 待ち（最大 3 秒）の後になる。Host が自動語取得の保存を同期で行うと遅れうる（推測） | 記録のない差 |
| P-36 | 候補を待つ間に Host が落ちると、窓は自動では開かず、もう一度 Space を押すとローカル候補が出る | 記録のない差 |
| P-37 | ZenzaiProfile、PersonalizationLevel、InferenceLimit、SystemUserDictionary に当たる設定が無い（Zenzai の 3 件は `zenzai-inference-spec.md` §9 に将来の設定として書かれている）。OpenAI のエンドポイントの書式が違う（Mac は `/chat/completions` まで、Windows は `/v1` まで） | 記録のない予定／意図した差（`ai-backend-spec.md` §5.1） |

## 4. 同等と確認できた範囲

- **キー**: Ctrl+H、Enter とテンキー Enter、読み入力中の Space と Shift+Space、候補選択中の Space・Shift+Space・↑↓、数字 1〜9 による選択、Ctrl+Shift+U、Delete・Home・End（どちらもアプリへ渡す）。
- **状態遷移**: 読み入力中の文字入力・Backspace・Esc・Enter、候補選択中の文字入力（確定して続ける。DEV-1735）、候補選択中の Backspace（読みへ戻る）、Unicode 入力の hex・Backspace・Enter・Esc。Esc 1 回での取り消しは意図した差（`legacy-parity-spec.md` §1.2）。
- **文字入力**: 拗音、促音、xtu/ltu、wi/we/whu などの綴り、読み入力中の `-` → ー、`, . ? !` → `、。？！`。nn と n' の扱いは意図した差（`legacy-parity-spec.md` §5.4）。英数モードでの半角の直接入力。
- **変換と候補**: ライブ変換 ON での 1 回目の Space、ライブ変換中の Enter（表示中の文字列を確定）、予測の受理（読みを伸ばして続ける。upstream #373 後の Mac と同じ）。
- **表示**: 注釈、ライト・ダーク、候補窓と予測窓の表示と非表示、キャレット基準の配置・画面端での反転・押し戻し、複数モニタ、DPI、入力モードの表示から得られる情報、Unicode 入力の表示。キャレットが取れないときの代替は Windows のほうが手厚い。
- **応答と取りこぼし**: 通常の打鍵の順序と取りこぼし、高速打鍵とキーリピート、再接続中の打鍵、UI スレッドで Host を待たないこと、遅れて届いた応答で表示が書き換わらないこと（staleness）。
- **設定**: AI バックエンドの既定（off / none）、`includeContextInAITransform`、Zenzai の有効化、学習の既定（ON）、キー配列（Windows は OS の配列に従う）。

## 5. 領域別の主な対応

§3 の指摘を、領域ごとの対応表から引けるように並べ直す。Mac 側の根拠は主に `legacy/Core/Sources/Core/InputUtils/` の `Actions/UserAction.swift`・`InputState.swift`・`SegmentsManager.swift`、Windows 側は `core/src/UserActionMap.cpp`・`core/src/InputState.cpp`・`tsf-tip/src/TextService.cpp` である。

### 5.1 キーの割当（A）

| Mac のキー | Mac の働き | Windows | 分類 | 指摘 |
|---|---|---|---|---|
| Ctrl+H | Backspace | 読みがあるとき Backspace（`UserActionMap.cpp` の `MapCtrl`） | 同等 | — |
| Ctrl+P / Ctrl+N | 入力中と候補選択中に ↑↓ | 候補選択中だけ | 意図した差（legacy 差は未記載） | P-26 |
| Ctrl+F | → | 候補選択中の文節移動だけ | 意図した差（legacy 差は未記載） | P-26 |
| Ctrl+M | Enter | 割当なし | 記録のない差 | P-26 |
| Ctrl+I / Ctrl+O、Shift+←→ | 文節の伸縮 | 割当なし（spec は別用途に予約） | 記録のない差 | P-09 |
| F6〜F10、Ctrl+J/K/;/L/: | 字種を変えて確定 | 割当なし | 記録のない差 | P-04 |
| Ctrl+S | AI の提案・続き | 予約で割当なし | 記録のない差 | P-27 |
| Ctrl+Delete | 選択中の候補の忘却 | Ctrl+Shift+Backspace で直近の確定の忘却 | 意図した差（legacy 差は未記載） | P-22 |
| 未定義の Ctrl 組合せ | 入力中は消費 | アプリへ渡す | 記録のない差 | P-07 |
| Tab | 入力中は消費 | 予測窓があるときだけ受理 | 記録のない差 | P-07 |
| Space（読みなし） | 全角空白 | アプリへ渡す（半角） | 記録のない差 | P-06 |
| 数字 | 読みに足す | 候補選択中の 1〜9 だけ消費 | 意図した差（legacy 差は未記載） | P-01 |
| 記号 | 全角化して読みに足す | `、。？！ー / < >` と Shift+1 だけ取り込む | 記録のない差 | P-02 |
| PageUp / PageDown（#394） | 候補のページ移動 | アプリへ渡す | 記録のない差 | P-11 |
| 英数 | 英語への絶対指定 | `VK_OEM_ATTN` のトグル | 意図した差（legacy 差は未記載） | P-12 |
| かな | 日本語への絶対指定 | ひらがなキーの処理なし。`VK_IME_ON` が代わりになる | spec と実装の不一致 | P-12 |
| 英数・かなのダブルタップ | AI 変換窓。英数は読みがあれば半角英字で確定 | 検出なし | 欠落（M16） | P-21 |
| Option の組合せ、dead key | 直接入力、ダイアクリティカル | Alt と Win は常にアプリへ | 意図した差（Mac 固有） | P-28 |

Windows 独自のキーとして、半角/全角、`VK_IME_ON` / `VK_IME_OFF`、`convertKeysAction = imeOnOff` のときの変換・無変換（開閉）、無変換（確定後や選択範囲の字種の巡回）、変換（選択範囲の再変換）、Ctrl+Shift+Space（校正ビュー）がある。

### 5.2 状態遷移（B）

| Mac の状態 | 主な差 | 指摘 |
|---|---|---|
| none | 数字・記号・Space が読みに入らない | P-01、P-02、P-06 |
| attachDiacritic | Windows に相当する状態が無い（dead key は `legacy-parity-spec.md` §1.5.2 で予約） | P-28 |
| composing | ↓・Tab・未定義の Ctrl が素通り。Space の後に応答を待つ間、Enter は読みを確定する | P-03、P-07 |
| previewing（Mac はライブ OFF の Space 後の先頭候補プレビュー） | Windows に相当する状態が無い。Windows の Previewing はライブ変換の意味 | P-05 |
| selecting | Enter と数字は読み全体を確定（Mac は選択中の候補が覆う部分だけ）。→ は一括変換の文節移動だけ。`0` は素通り | P-07、P-09 |
| replaceSuggestion | TIP から到達しない。spec はダブルタップのプロンプトとして定義し直している | P-21 |
| unicodeInput | Space と hex 以外のキーの扱い、範囲外の扱い | P-30 |

### 5.3 応答の遅れと取りこぼし（G）

| 場面 | Windows の挙動 | 分類 | 指摘 |
|---|---|---|---|
| 通常の打鍵 | OnKeyDown で同期的に消費し、読みを更新する。要求は最新 1 件の slot に合流する | 同等 | — |
| 確定をアプリが拒否した間の打鍵 | 16 個まで保持して再生する。17 個目以降とフォーカス喪失時は捨てる | 意図した差（`tsf-deep-integration-spec.md` §2.4.1） | — |
| Space の後の Enter・数字・文字 | Enter は読みを確定、数字は素通り、文字は読みに足す | 意図した差 | P-01、P-03 |
| Space の後の Space | 要求をやり直す | 記録のない差 | P-08 |
| 窓要求が 750ms を超える | 打鍵ごとの応答とローカル候補で窓を開き、遅れた応答は捨てる | 意図した差（`dev-infrastructure-spec.md` §8.5.2） | — |
| ライブ変換の連続打鍵 | 最後に表示した応答のまま変換済みの部分が止まる | 意図した差 | P-15 |
| UI スレッドの待ち、アプリへの同期問い合わせ | Host を UI スレッドで待たない。左文脈の読み取りは 1 打鍵 1 回 | 同等 | — |

### 5.4 設定と既定値（H）

| Mac の設定 | Mac の既定 | Windows の設定キー | Windows の既定 | 分類 | 指摘 |
|---|---|---|---|---|---|
| LiveConversion | true | `liveConversion` | false | 記録のない差 | P-05 |
| InputStyle | default（ほかに AZIK・かな JIS・かな US・custom） | `inputStyle` | default（ほかに custom） | 記録のない差 | P-18 |
| KeyboardLayout | qwerty | なし（OS の配列に従う） | — | 同等 | — |
| TypeBackSlash | false | なし | — | 記録のない差 | P-28 |
| PunctuationStyle | `、。` | なし（`、。` 固定） | — | 記録のない差 | P-19 |
| TypeHalfSpace | false（全角） | なし（半角） | — | 記録のない差 | P-06 |
| OptionDirectFullWidthInput | false | なし | — | 記録のない差（Mac 固有） | P-28 |
| Learning | inputAndOutput | `privacy.mode` など | normal（学習 ON） | 同等 | — |
| DebugPredictiveTyping | false | `predictionEnabled` | true | 記録のない差 | P-16 |
| DebugTypoCorrection | false | `typoCorrectionMode` | suggest | 意図した差（`typo-correction-learning-spec.md`） | — |
| UserDictionary | 1 件（あずーきー） | `user_dict.json` | 空（推測） | spec と実装の不一致（v1.0 のボタン） | P-23 |
| AIBackendPreference | off | `aiBackend` | none | 同等 | — |
| OpenAiApiEndpoint | `.../v1/chat/completions` | `openAiApiEndpoint` | `.../v1` | 意図した差（`ai-backend-spec.md` §5.1） | P-37 |
| ZenzaiProfile ほか 3 件 | — | なし | — | 記録のない予定 | P-37 |

Windows 独自の機能で、既定で ON のまま素の打鍵結果を変えるものは `predictionEnabled`（P-16）と `typoCorrectionMode = suggest`（同じ誤りを 3 回観測した後、補正読みの候補が混ざる）である。リライター、日英混在入力、カッコ補完、動的句読点、句読点の確定、一括変換は既定 OFF で影響しない。

## 6. spec と roadmap に関する指摘

| ID | 対象 | 内容 | 関連 |
|---|---|---|---|
| S-01 | `legacy-parity-spec.md` §1.4・§1.5 冒頭 | §1.5 冒頭は §1.1〜§1.4 を「レガシーの VK 表」と呼ぶが、§1.4 は legacy と一致しない（Ctrl+B/A/E は legacy に無い。Ctrl+I/O/S の用途、F6〜F10、Ctrl+J/K/L/;/:、Ctrl+M が食い違うか載っていない）。§1.4 に legacy との差を 1 行ずつ書くと「意図した差」の根拠になる | P-04、P-07、P-26、P-27 |
| S-02 | `legacy-parity-spec.md` §1.5.3 | OEM キーを core の表へ `Input` として追加すると書くが、実装とテストは追加していない | P-02 |
| S-03 | `tsf-deep-integration-spec.md` §3.1 | `VK_DBE_HIRAGANA` / `VK_DBE_KATAKANA` の処理を定めるが、実装が無い。「input mode = katakana」の行き先になるカタカナ入力モードも無い | P-12 |
| S-04 | `tsf-deep-integration-spec.md` §5.5、roadmap M23 | §5.4 は文節を一括変換の結果と定義し、§5.6 は core 経由の preedit を fallback とする。§5.5 と M23 の受け入れ条件（Space で注目文節が青背景）は一括変換に限ると書いておらず、§5.6 に追随していない | P-13 |
| S-05 | `tsf-deep-integration-spec.md` §8 | UI-less で予測を Suggestion UI へ渡す予定を書くが、§8 を実装範囲に含む milestone が roadmap に無い | P-17 |
| S-06 | `legacy-parity-spec.md` §4.1 | 英数キーを `VK_OEM_AUTO`（0xF3、`tsf-deep-integration-spec.md` §3.1 では半角/全角）とし、判定間隔を 300ms（Mac は 0.5 秒）、かなのダブルタップを言い換えの別機能とする。いずれも Mac と食い違う | P-21 |
| S-07 | `bracket-pairing-spec.md` §4.4・§4.7、設定アプリの表示名 | §4.4 は `inputMode` の定義を legacy-parity-spec に委ねるが、同 spec に定義が無い。設定アプリは「起動時の入力モード」と表示するが、文字入力にも起動時のモードにも効かない | P-20 |
| S-08 | `sideload-packaging-spec.md` §3.7 | v1.0 の「ユーザー辞書を編集」ボタンが設定アプリに無い | P-23 |
| S-09 | `native-ui-spec.md` §5.1・§5.2・§6 | フェードイン、選択の移動アニメ、UIA イベント、ハイコントラスト用フォントの実装が見つからない | P-33 |
| S-10 | `legacy-parity-spec.md` §5.4 | カスタム表の書き出しスクリプトの記述があるが、`scripts/` に無い（Phase 7 の予定として書かれている）。カスタム表が内蔵表の促音・`-` の処理を通らないことが書かれていない | P-18 |

## 7. upstream の Mac 版の後続変更

| commit | 内容 | Windows への示唆 |
|---|---|---|
| `35661496`（#343） | XPC の応答が無いとき `handle` が false を返し、キーがアプリへ素通りして生の英字が漏れていた。composition を IME プロセス内に戻し、キーの所有権を同期的に決めるよう直した | Windows は最初からキーの消費を TIP 内で同期的に決めており、同じ漏れは起きない |
| `3ae5a465`（#345） | thin client の順序付きキューで打鍵を失わないようにし、応答待ちの間は Command 以外のキーを消費した。Chromium のデッドロック回避のテストを保持した | Windows は要求を最新 1 件に合流させるので順序付きキューは要らない。UI スレッドで Host を待つ経路も無い |
| `7ed6b6d0`（#346） | converter を入力セッション間で共有し、学習データの永続化を debounce した | 確定直後の最初の問い合わせが ACK 待ちの後になる点（P-35）が近い |
| `c4b246c6`（#386） | XPC による分離を撤回し、プロセス内の同期変換へ戻した | Windows の別プロセス構成とは前提が違う |
| `b7ec0e4f`（#373） | 予測候補の受理に converter の新 API を使う | 受理後に読みを伸ばして続ける点は Windows と同等 |
| `9406e344`（#394） | 候補のページ移動（PageUp/PageDown で 9 行ずつ。先頭ページの PageUp で補助候補を出す） | Windows に無い（P-11） |
| `6b3f3095`（#396） | 入力開始時に不要な選択範囲の問い合わせをしない | Windows の左文脈の読み取りは 1 打鍵 1 回にテストで固定されている |

## 8. 実機確認チェックリスト

静的には決められない項目である。Windows 側は開発機で行える可逆な操作に限る。ログは `AZOOKEY_LOG=1` と `AZOOKEY_LOG_KEYS=1` で採り（`docs/debugging.md`）、終わったら環境変数を消す。Mac 側の確認には macOS 版の実機が要る。

| ID | 確かめること | 手順 | 関連 |
|---|---|---|---|
| C-01 | 素通りしたキーの入り方 | Notepad、Word、Edge、VS Code で `dai3kai`、`ji:kan`、`kakko[` を打ち、確定した文字列と順序、composition が残るかを記録する | P-01、P-02 |
| C-02 | 読み入力中に素通りするキー | 読み入力中に Tab、↓、→、F7、F10、Ctrl+K、Ctrl+V、Ctrl+Z を、候補選択中に `0`、PageDown、Ctrl+Shift+Backspace を押し、アプリの機能が動くか、読みがどうなるかを見る | P-04、P-07 |
| C-03 | Space 直後のキー | `kanji` → Space → Enter、`kanji` → Space → `2`、`kanji` → Space → Space → Enter を 50〜100ms 間隔で送る。Host に CPU 負荷をかけた場合（`ProcessorAffinity` を 1 にするなど。終わったら Host を再起動する）と、かけない場合で比べる | P-03、P-08 |
| C-04 | 日本語モードの Space | 読みが無いときの Space と Shift+Space が全角か半角か | P-06 |
| C-05 | 変換中の下線 | 既定設定で `nihongo` → Space の下線を入力中と比べる。一括変換 ON でも比べる | P-13 |
| C-06 | 候補窓の件数と幅 | 30 文字を超える文で Space を押し、省略を見る。`maxCandidates` を 32 にして画面下端の近くで変換し、窓の高さと「10.」以降の番号を見る | P-11 |
| C-07 | ひらがなキーと英数キー | JIS 配列のカタカナひらがなキーが出す VK と、アプリへ渡るか。英数キーを 2 回押したときのモード | P-12 |
| C-08 | UI-less の予測窓 | Word とスタート検索で `predictionEnabled = true` のまま打ち、TIP の予測窓がアプリの UI と重ならないか | P-17 |
| C-09 | `inputMode` | alnum_half / alnum_full にして再起動し、初期モードと英字の幅を見る | P-20 |
| C-10 | ライブ変換の表示 | ライブ変換 ON で Host に負荷をかけ、毎秒 8 打ほど打ち続けたときの表示と、Backspace の後の表示 | P-15 |
| C-11 | Mac 側の挙動 | Mac で Shift 付きの `Apple`、`z.` `z,` `z[`、「きょうはいいてんき」→ Space → Enter（先頭文節だけを確定するか）、ライブ OFF の Space 1 回目で窓が出ないこと、F7 で確定した語が学習されるか | P-05、P-09、P-25、P-29 |

## 9. 確認できなかった範囲

- AzooKeyKanaKanjiConverter の内部（ローマ字表、AZIK 表、かな表、大文字の扱い、`firstClauseResults`、`acceptPredictionCandidate` での読みの変化、`forgetMemory` の範囲）。repo の外にある。
- 実際の描画（下線の太さ、アプリごとの配色、Windows 11 が窓の角を丸めるか、影）と、Mac の `kTSMHilite*` の見え方。
- 素通りしたキーを各アプリが composition に対してどこに挿入するか。
- Host の実際の応答時間の分布と、P-03・P-08 が起きる頻度。
- inference-host の候補が読みの一部だけを覆うことがあるか（P-09 の実害）。
- 一括変換、絵文字、数字リライターなど、既定 OFF の前処理経路でのキーの差。
