---
kind: spec
milestones: [M66]
canonical_for: ["通常変換の文節の伸縮と部分確定"]
defers_to: [docs/legacy-parity-spec.md §1.4, docs/lattice-conversion-spec.md §7.3]
---

# 文節の伸縮と部分確定 仕様

対象リポジトリ: dolquis/azooKey-Desktop
対応マイルストーン: M66（追加機能）
関連: `docs/legacy-parity-spec.md`（§1.1 の `UserAction`、§1.2 の状態遷移、§1.4 のキー割当）、
      `docs/tsf-deep-integration-spec.md`（§3.6 の字種指定確定、§4.2・§4.3 の確定、§5 の文節ごとの表示属性）、
      `docs/lattice-conversion-spec.md`（§7.3 の区切りの制約）、
      `docs/romaji-batch-conversion-spec.md`（一括変換の文節）、`docs/macos-input-parity-audit-report.md`（P-09）

通常変換は、Space の後に読み全体の候補だけを出し、Enter で読み全体を確定する。長い読みの区切りを
変換器が誤ったとき、利用者は読みを消して短く打ち直すしかない。本書は、先頭の文節の長さを
Shift+← / Shift+→ で変え、その文節だけを確定して残りの読みの変換を続ける操作を定める。
legacy（macOS 版）の `SegmentsManager.editSegment` と、選択中の候補が覆う部分だけを確定する
`InputState` の遷移に相当する。

## 節とマイルストーンの対応

| 節 | マイルストーン |
|---|---|
| §1〜§11 | M66 |

## 正典範囲

本書が正典であるもの: 先頭文節の状態、伸縮と部分確定のキーごとの挙動、先頭文節の候補の要求と応答を待つ間の扱い、
部分確定の手順、伸縮中の表示属性の割り当て、部分確定の学習観測。

本書が委ねるもの:

| 主題 | 正典 |
|---|---|
| `UserAction` の値と、VK からの写像 | `docs/legacy-parity-spec.md` §1.1・§1.4 |
| 応答を待つ間に保留するキーの基本の規則、上限、期限 | `docs/legacy-parity-spec.md` §1.2 |
| 字種指定確定の字種、綴り、観測 | `docs/tsf-deep-integration-spec.md` §3.6 |
| 確定の edit session が拒否されたときの再試行 | `docs/tsf-deep-integration-spec.md` §2.4.1 |
| 表示属性の値と、属性を付けられないときの fallback | `docs/tsf-deep-integration-spec.md` §5.6 |
| 語の境目の制約付きの探索と、先頭の区間の候補の取り出し | `docs/lattice-conversion-spec.md` §7.3 |
| payload のフィールド名と wire key | `ipc/include/azookey/ipc/Payloads.h` |
| 受け入れ条件 | `plans/windows-port-roadmap.md` M66 |

## 1. 目的と範囲

目的は、変換の区切りを誤ったときに、読みを消さずに先頭の文節を縮めたり伸ばしたりして変換し、
その文節を確定して残りの読みへ進めるようにすることである。

範囲は、一括変換（`batchRomajiConversion`）が OFF の通常変換の候補選択中（core の `Selecting`）とする。

範囲に含めないもの:

- 変換した文を複数の文節に分けて並べ、注目する文節を移す操作と、確定した文節へ戻る操作。
  `legacy/segment-edit-upstream.md` の上流計画の残りで、roadmap の「スコープ外」が扱う。
- 一括変換の文節の伸縮。一括変換の文節は文末記号と長さで切ったまとまりで、文法上の文節ではない
  （`docs/romaji-batch-conversion-spec.md` §7）。
- 読み入力中（`Composing`）とライブ変換の表示中（`Previewing`）からの伸縮。
- 区切りの位置の学習。
- Space の候補列の先頭に、先頭文節だけを覆う候補を並べること（legacy の `firstClauseResults`）。

## 2. 用語

| 用語 | 意味 |
|---|---|
| 読み | `Selecting` が対象にする読み全体。未確定のローマ字をかなにした後の文字列で、auto_replace の補正（`docs/typo-correction-learning-spec.md` §5）を反映した後の読みである。長さを n（code point）とする |
| 先頭文節 | 読みの区間 `[0, e)` |
| e | 先頭文節の終わりの code point 位置。1 ≤ e ≤ n |
| 残りの読み | 読みの区間 `[e, n)` |
| 伸縮中 | e < n の状態 |
| 部分確定 | 先頭文節だけを確定し、残りの読みを composition に残すこと |

## 3. 状態

`InputState` は `Selecting` の状態データに次を持つ。新しい `InputStateKind` は足さない
（`docs/legacy-parity-spec.md` §1.5.2）。

- e。`Selecting` に入ると e = n になる。
- この `Selecting` で伸縮したかどうか。
- `Selecting` に入ったときの読み全体の候補のスナップショット。
- 先頭文節の候補を待っているかどうか（§5.2）。

伸縮は `UserAction` の `ShrinkSegment`（Shift+←）と `ExtendSegment`（Shift+→）で受ける（`docs/legacy-parity-spec.md`
§1.1・§1.4）。候補のスナップショットと選択は、伸縮中は先頭文節の候補、e = n のときは読み全体の候補を指す。
e = n の間、`Selecting` の挙動は `docs/legacy-parity-spec.md` §1.2・§1.4 のとおりで、本書の規則は
`ShrinkSegment` と `ExtendSegment` だけに掛かる。

一括変換の文節を表示している候補選択は e を持たず、本書の対象外とする。

## 4. キー操作

伸縮は、読みの code point 1 つを単位とする。拗音の小書きも 1 つと数える。

| キー | e = n のとき | e < n のとき |
|---|---|---|
| Shift+← | n ≥ 2 なら e = n − 1。n = 1 なら消費して何もしない | e ≥ 2 なら e − 1。e = 1 なら消費して何もしない |
| Shift+→ | まだ伸縮しておらず n ≥ 2 なら e = 1。伸縮した後か n = 1 なら消費して何もしない | e + 1。e + 1 = n なら読み全体に戻る |
| → / Ctrl+F | `docs/legacy-parity-spec.md` §1.4 のとおりアプリへ渡す | 部分確定（§6） |
| ← / Ctrl+B | `docs/legacy-parity-spec.md` §1.4 のとおりアプリへ渡す | 消費して何もしない |
| Enter | 読み全体の確定（`docs/legacy-parity-spec.md` §1.2） | 部分確定 |
| 数字（候補番号） | 番号の候補で読み全体を確定 | 番号の候補で部分確定 |
| 候補窓の行のクリック（`docs/native-ui-spec.md` §4.1）、UI-less の候補 UI からの確定（`docs/tsf-deep-integration-spec.md` §2.3 の `Finalize`） | 選んだ候補で読み全体を確定 | 選んだ候補で部分確定 |
| F6〜F10 | 読み全体の字種指定確定（`docs/tsf-deep-integration-spec.md` §3.6） | 先頭文節の読みの字種指定確定で部分確定（同 §3.6） |
| Space / Shift+Space / ↑↓ / Ctrl+P / Ctrl+N / 変換キー / UI-less の `SetSelection` | 候補を移る | 先頭文節の候補を移る |
| Backspace | `docs/legacy-parity-spec.md` §1.2 のとおり読みへ戻る | e を捨てて読み全体に戻し、同じく読みへ戻る |
| Esc | 入力を取り消す | 同じく入力を取り消す |

表に無いキー（Delete、Home、End、Tab、PageUp、PageDown、Ctrl+Z、Ctrl+Y、Ctrl+Shift+Backspace など）は e を保ち、
e = n のときと同じに扱う（`docs/legacy-parity-spec.md` §1.4。Ctrl+Shift+Backspace は消費して何もしない）。

Shift+→ を伸縮の前の読み全体から押して e = 1 にするのは、legacy と同じく、短い先頭文節から伸ばし始められる
ようにするためである。一度伸縮した後は、legacy と同じく e = n で止まる。

e が n より小さい値に変わると、先頭文節の候補を取り直す（§5）。e が n に戻ると候補を取り直さず、`Selecting` に
入ったときの読み全体の候補に戻し、先頭の候補を選ぶ。

次の操作は、伸縮中には表示中の文字列（先頭文節の選択中の候補と、残りの読み）を確定する。残りの読みは
読みのまま確定する。

- 文字・記号・長音の入力（`docs/legacy-parity-spec.md` §1.2 の `Selecting` の `Input`）。確定した後、
  そのキーで次の入力を始める。
- 句読点の確定（`punctuationConversion = commit`、`docs/tsf-deep-integration-spec.md` §3.5.1）。
  表示中の文字列に句読点を続けて確定する。
- Unicode 入力の開始（Ctrl+Shift+U）。確定した後、Unicode 入力を始める。
- IME を Off にするキー（`docs/tsf-deep-integration-spec.md` §4.2）。
- 英数モードへの切替、フォーカスの喪失、Deactivate、TIP の外からの Off（`docs/tsf-deep-integration-spec.md` §4.3）。

これらの観測は §8 に従う。

Ctrl+I / Ctrl+O は割り当てない（`docs/legacy-parity-spec.md` §1.4）。読み入力中とライブ変換の
表示中の Shift+← / Shift+→ はアプリへ渡す。ただし Space の応答を待つ間は、同 §1.2 の保留に入り、窓が開いた後に
伸縮として再生する（`Selecting` の写像で `ShrinkSegment` / `ExtendSegment` になるキーだからである）。
TIP の前処理が出す候補窓（数字リライター、英単語候補、記号・絵文字のリライター。同 §1.2）は本書の対象外とし、
Shift+← / Shift+→ は Shift の無い ← / → と同じに扱う。

## 5. 先頭文節の候補

### 5.1 要求

e が n より小さい値に変わるたびに、TIP は候補窓用の `QueryCandidates`（`live = false`）を送る。

- 読み（`reading`）は先頭文節の読みとする。
- 残りの読みを任意のフィールドで添える。伸縮中の残りの読みは空でない。フィールドの名前と wire key は
  `ipc/include/azookey/ipc/Payloads.h` が定める。名前には `reading` を含め、ログの伏せ字の判定
  （`core/src/RuntimeLogger.cpp` の `IsSensitiveRuntimeLogField`）に掛かるようにする。読みの本文を運ぶフィールドだからである。
  Host は、このフィールドを持つ要求を先頭文節の要求として扱う（§5.3）。
- 左文脈、`secure`、`learning_allowed`、`app`、`max_candidates` は、読み全体の要求と同じに決める。
- 生ローマ字（`raw_romaji`・`raw_keys`）、英単語候補、日英混在、`alt_readings`、絵文字検索の起点、自動句読点は
  送らない。読みの前方だけでは生ローマ字との対応が取れず、英字化や別の読みで読みが変わると、e と読みの
  対応が崩れるためである。
- 応答が `corrected_reading` を持つときは、TIP は読みを置き換えず、その応答の候補を使わない。候補の `reading` が
  要求の読みと違う候補も使わない。使える候補が残らないときは、§5.2 の期限に達したときと同じく、先頭文節の読みの
  ローカル候補にする。残りの読みのフィールドを知らない Host は先頭文節の読みを補正しうるので、補正した読みの候補を
  補正前の読みに当てて確定し、観測することを防ぐためである。

前の e の要求が届いていなければ取り消し、その応答は staleness 判定で捨てる。連打を間引く待ち時間は置かない。
Zenzai は decode のループで取り消しを確かめて止まる（`docs/zenzai-inference-spec.md` §9.2.2）。

### 5.2 応答を待つ間

先頭文節の候補を待つ間、`InputState` は `Selecting` のまま待っていることを示す。候補窓は閉じず、前の一覧を
残す。応答が届くと、`InputState` は先頭文節の候補を差し替えて先頭の候補を選ぶ。`Composing` から `Selecting` への
遷移（`docs/legacy-parity-spec.md` §1.5.4）とは別の、`Selecting` の中の差し替えである。待つ前の e の応答は使わない。

応答が届くまで、先頭文節は読みのまま描く（§7）。

待つ間のキーは、Space の応答待ちと同じく保留し、候補が差し替わった後に打った順に再生する。保留するキーの
基本の規則、上限、待つ期限（候補窓用の 750 ms）、Host を失ったときのローカル候補は `docs/legacy-parity-spec.md`
§1.2 に従い、ローカル候補は先頭文節の読みから作る。伸縮中は、← / → / Ctrl+B / Ctrl+F も保留する。
§4 のとおり、伸縮中のこれらのキーは部分確定するか消費するので、アプリへ渡さない。

待つ間の Shift+← / Shift+→ は、保留しているキーが無ければ、その場で e を変えて要求を出し直す。保留しているキーが
あれば同じく保留し、打った順に再生する。その場で e が n に戻ったときは、出している先頭文節の要求を取り消して待ちを
終え、読み全体の候補に戻す（保留しているキーは無い）。

待つ間の候補窓のクリックと、UI-less の `SetSelection` / `Finalize` は受けず、消費して何もしない。窓に残っている前の
一覧は、いまの先頭文節の候補ではないためである。

Esc、IME を Off にするキー、英数モードへの切替、フォーカスの喪失、Deactivate、TIP の外からの Off、composition の
終了は、待つ間も保留せず、`docs/legacy-parity-spec.md` §1.2 のとおりその場で処理して保留中のキーを捨てる。このとき
出している先頭文節の要求も取り消す。§4 で表示中の文字列を確定する操作のうち、待つ間も保留しないもの（IME を Off に
するキー、英数モードへの切替、フォーカスの喪失、Deactivate、TIP の外からの Off）は、先頭文節を読みのまま、残りの読みと
あわせて確定し、観測は送らない。文字・記号・長音の入力、句読点、Ctrl+Shift+U と Backspace は保留し、候補が差し替わった
後に §4 のとおり再生する。

### 5.3 Host 側の候補源

先頭文節の要求では、Host は auto_replace を含む読みの補正をせず、要求の読みのまま変換し、`corrected_reading` を
返さない。補正すると e と読みの対応が崩れるためである。Host の設定から決まる補正の強さを要求で弱める向きであり、
要求で補正を強めない規則（`docs/typo-correction-learning-spec.md` §12.13）とは矛盾しない。

Zenzai、辞書の完全一致、`SimpleConverter`、候補のリライターは、要求の読み（先頭文節の読み）だけを使い、
残りの読みを読まない。Zenzai は右の文脈を使わないので、先頭文節を縮めると、後ろの語で決まる同音異義語を
読み全体の変換より誤りやすい。

ラティス（`docs/lattice-conversion-spec.md` §3.2 の段階 1 の条件に当たる要求）は、先頭文節の読みと残りの読みを
つないだ読みでラティスを作り、位置 e に語の境目の制約を掛けて探索する。候補には、各経路のうち位置 e より前の語の
表層をつないだものを使う。取り出し、畳み方、順位と `score` は同 §7.3、`source`、`debug_info`、件数は同 §3.4・§3.5 に
従う。§3.4 の「要求の読み全体」は、先頭文節の要求では先頭文節の読みである。つないだ読みが 64 code point を
超えるときは、先頭文節の読みだけでラティスを作る。

先頭文節の要求でない要求には、本節を当てない。残りの読みのフィールドを知らない Host はそれを読まないので、
ラティスを先頭文節の読みだけで作り、読みを補正しうる。その応答の扱いは §5.1 が定める。

## 6. 部分確定

### 6.1 InputState

`InputState` は、Enter・→・Ctrl+F・数字（候補窓のクリックと UI-less の `Finalize` を含む。§4）による部分確定を、
次の細粒度の `ClientAction` の列で表す
（`docs/legacy-parity-spec.md` §1.5.2）。

1. 候補窓を閉じる。
2. composition の先頭文節を確定する候補の表層に置き換えて確定し、残りの読みを composition に残す。この action は
   確定する表層と残りの読みを持つ `CommitFirstSegment` で、`ClientAction` の末尾に足す。
3. `ObserveCommit`（先頭文節の読み、確定した表層）。
4. 残りの読みの候補窓用の `QueryCandidates`。

F6〜F10 による部分確定は、TIP が `docs/tsf-deep-integration-spec.md` §3.6 の確定の経路で、先頭文節の読みを字種を
変えて確定する。`InputState` は、その後に上の 1 と同じく候補窓を閉じ、上の 4 と同じく残りの読みで要求を出す。

部分確定の後、`InputState` は残りの読みを読みとし、`Composing`（ライブ変換 ON なら `Previewing`）で応答を待つ。
応答が届くと、e が残りの読みの長さの `Selecting` に入る。Space の応答待ち（`docs/legacy-parity-spec.md` §1.2）と
同じ規則で、キーを保留して再生する。この間の残りの読みは、読み入力中と同じ表示属性で描く
（`docs/tsf-deep-integration-spec.md` §5.6）。

部分確定は 1 回ごとに独立した確定として扱う。確定の取り消し（`docs/commit-undo-spec.md` §8）は、部分確定そのものを
取り消しの対象にせず、残りの読みを確定して Idle に戻ったときの直前の確定（残りの読みの分）だけを読みへ戻す。
先に部分確定した先頭文節へは遡らない。

### 6.2 TIP の edit session

部分確定の edit session は、次の順に操作する。

1. composition の文字列を、確定する表層と残りの読みをつないだものにする。
2. 確定する表層の範囲の `GUID_PROP_ATTRIBUTE` を `ITfProperty::Clear` で消す。
3. `ITfComposition::ShiftStart` で composition の先頭を確定する表層の後ろへ移す。composition から外れた文字列は
   `GUID_PROP_COMPOSING` を失い、確定した文字列になる。
4. 残りの読みの属性を付け直し、キャレットを composition の末尾に置く。

`ShiftStart` が失敗したときは、composition を確定する表層だけにして `EndComposition` で確定し、その直後で
残りの読みの composition を始め直す。属性を消せないときの扱いは、確定で composition を終えるときと同じとする
（`docs/tsf-deep-integration-spec.md` §5.6）。

edit session が拒否されたときは、読み全体の確定と同じく確定を待つものとして残し、次のキーで確定し直す
（`docs/tsf-deep-integration-spec.md` §2.4.1）。残りの読みの要求は、確定が済んでから出す。

残りの読みの要求の左文脈は、確定が済んだ後に文書から読む。確定した表層は左文脈に入る。

## 7. 表示

伸縮中は、先頭文節を Focused、残りの読みを Converted で描く。先頭文節の表示は、応答を待つ間は読み、応答の後は
選択中の候補である。e = n のときは、`docs/tsf-deep-integration-spec.md` §5.5 のとおり composition 全体を Focused で描く。
属性の値と、属性を付けられないときの fallback は同 §5.6 に従う。preedit をクリックしても注目する文節は移さない
（同 §7）。

候補窓は先頭文節の候補を出す。先頭文節は composition の先頭にあるので、窓の位置の規則は読み全体の候補と同じである。
部分確定の後は、残りの読みの composition の先頭に合わせる。アプリが候補 UI を描く UI-less の構成では、
`CandidateListUIElement` が先頭文節の候補を返す（`docs/tsf-deep-integration-spec.md` §2）。

## 8. 学習

Enter・→・Ctrl+F・数字（候補窓のクリックと UI-less の `Finalize` を含む）による部分確定のたびに、単発の
`CommitObservation` を送る。読みは先頭文節の読み、`chosen` は確定した候補、`shown` は先頭文節の候補一覧である。
観測するかどうかの条件（候補の `source`、`secure`、`learning_allowed`）は、読み全体の確定と同じにする。ローカル候補
（§5.1・§5.2）による部分確定は、読み全体の確定と同じく観測しない。文節ごとに確定する時点が違うので、`CommitSegmentsObservation` は
使わない。F6〜F10 による部分確定の観測は、`docs/tsf-deep-integration-spec.md` §3.6 の字種ごとの規則を、先頭文節の
読みと確定した文字列に当てる。

候補は先頭文節の読みを変換したものなので、観測する読みと表層の対は対応する。新語の判定
（`docs/auto-word-registration-spec.md` の M36-A）も、この対で行う。

§4 で伸縮中に表示中の文字列を確定する操作は、その操作が読み全体の確定で観測を送るときに限り、先頭文節だけを
観測する。文字入力、句読点の確定、Unicode 入力の開始、IME を Off にするキーは観測を送り、英数モードへの切替、
フォーカスの喪失、Deactivate、TIP の外からの Off は送らない（`docs/legacy-parity-spec.md` §1.2、
`docs/tsf-deep-integration-spec.md` §3.5.1・§4.2・§4.3）。読みのまま確定した残りの読みは、as-is 確定
（`docs/legacy-parity-spec.md` §1.3）と同じく観測しない。

区切りの位置は学習しない。次に同じ読みを打っても、先頭文節は読み全体から始まる。

## 9. legacy との差

| 場面 | legacy | Windows |
|---|---|---|
| 伸縮を始める状態 | 読み入力中、先頭候補の表示中、候補選択中 | 候補選択中だけ |
| 伸縮のキー | Shift+← / Shift+→、Ctrl+I / Ctrl+O | Shift+← / Shift+→ |
| Enter・数字で部分確定した後 | 残りの読みの先頭候補を窓なしで表示する | 残りの読みの候補窓を開く。ライブ変換 OFF の Space で 1 回目から窓を開くのと揃える（`docs/legacy-parity-spec.md` §1.2） |
| → で読み全体の候補を確定 | 読み全体を確定する | e = n の → はアプリへ渡す |
| Space の候補列 | 伸縮する前は、先頭文節だけを覆う候補を先頭に並べる | 読み全体の候補だけを並べる |

## 10. テスト計画

| 対象 | 試験 |
|---|---|
| core の状態機械 | `core/tests/input_state_test.cpp`: §4 の各キーが e と状態を表のとおりに変える。e が n より小さくなるたびに先頭文節の読みの候補窓用の要求を出し、e が n に戻ると読み全体の候補に戻す。先頭文節の候補を待つ間に届いた応答で候補が差し替わり、待つ前の e の応答は使われない。部分確定が §6.1 の列を返し、残りの読みで応答を待つ。待つ間にその場で e が n に戻ると、要求を取り消して読み全体の候補に戻る。伸縮中の Esc が入力を取り消し、Ctrl+Shift+U・句読点・文字入力が表示中の文字列を確定する。表に無いキーが e を保つ。e = n の各キーの結果が `docs/legacy-parity-spec.md` §1.2・§1.4 のとおり |
| キーの写像 | `tsf-tip/tests/keymap_test.cpp` と `core/tests/input_state_test.cpp`: Shift+← / Shift+→ が `Selecting` でだけ `ShrinkSegment` / `ExtendSegment` になり、Shift の無い ← / → と区別される。Ctrl+I / Ctrl+O は割り当てない |
| TIP | `tsf-tip/tests/onkeydown_preedit_test.cpp`（`DocumentPreeditHarness`）: 先頭文節の要求が §5.1 のフィールドを止め、残りの読みを添える。`corrected_reading` を持つ応答と、読みの違う候補を使わず、ローカル候補に落とす。待つ前の e の応答を使わない。待つ間のキー（伸縮中の ← / → と Backspace を含む）が保留され、候補が差し替わった後に再生される。待つ間のクリックを受けない。待つ間の Esc・IME を Off にするキー・英数モードへの切替・フォーカスの喪失が要求を取り消し、Off・英数モードへの切替・フォーカスの喪失は先頭文節を読みのまま確定して観測しない。待つ間の文字入力が保留され、候補が差し替わった後に表示中の文字列を確定する。750 ms の期限と Host の喪失で、先頭文節の読みのローカル候補になる。Space の応答を待つ間の Shift+← / Shift+→ が、窓が開いた後に伸縮として再生される。TIP の前処理の候補窓では伸縮しない。伸縮中の候補窓のクリックで部分確定する。部分確定で確定した表層の属性が消え、残りの読みが composition に残る。`ShiftStart` が失敗したときに composition を始め直す。確定が拒否されたときに残りの読みの要求を出さない。観測が §8 のとおりになり、残りの読みの要求の左文脈に確定した表層が入る。F6〜F10 の部分確定が先頭文節だけを字種で確定する。一括変換の候補選択の挙動が変わらない |
| 表示属性 | `tsf-tip/tests/display_attribute_test.cpp`: 伸縮中に Focused と Converted の 2 区間を作る |
| IPC | `ipc/tests/payloads_test.cpp`: 残りの読みのフィールドの往復と、フィールドの無い JSON を空として読むこと |
| Host | `inference-host/tests/dispatcher_test.cpp`: 先頭文節の要求で読みを補正せず `corrected_reading` を返さないこと、残りの読みがエンジンへ渡ること。`inference-host/tests/engine_lattice_test.cpp`: 残りの読みを添えた要求で、ラティスの候補が位置 e より前の表層になる。先頭文節の要求でない要求とモデルを通る要求には、§5.3 の規則が掛からない |

ラティスの語の境目の制約と先頭の区間の取り出しは、`docs/lattice-conversion-spec.md` §11 の core のラティスの部品の
試験が受け持つ。試験の一覧の正典は `docs/test-inventory.md` である。

## 11. 受け入れ条件

M66 の受け入れ条件は `plans/windows-port-roadmap.md` M66 が定める。
