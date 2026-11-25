---
kind: spec
milestones: [M69]
canonical_for: ["確定の取り消し"]
defers_to: [docs/legacy-parity-spec.md §1.4, docs/user-learning-enhancement-spec.md §4.1]
---

# 確定の取り消し 仕様

対象リポジトリ: dolquis/azooKey-Desktop
対応マイルストーン: M69（追加機能）
関連: `docs/legacy-parity-spec.md`（§1.1 の `UserAction`、§1.2 の状態遷移、§1.4 のキー割当、§7 の忘却）、
      `docs/tsf-deep-integration-spec.md`（§1 の再変換、§2.4.1 の確定の再試行、§3.2 の無変換の巡回、§5.6 の表示属性）、
      `docs/user-learning-enhancement-spec.md`（§4.1 の `CommitCorrection`、§6.2 の `correction_penalty`）、
      `docs/segment-editing-spec.md`（§6 の部分確定）、`docs/typo-correction-learning-spec.md`（§4 の打ち間違えの検出）

誤って確定したとき、利用者は確定した文字を消して読みを打ち直すしかない。本書は、確定の直後に Ctrl+Backspace を
押すと、直前の確定を確定する前の読みの composition へ戻す操作を定める。戻した読みは、Space で変換し直すことも、
打ち足すことも、Esc で消すこともできる。

## 節とマイルストーンの対応

| 節 | マイルストーン |
|---|---|
| §1〜§11 | M69 |

## 正典範囲

本書が正典であるもの: 取り消せる確定と取り消しの有効な条件、Ctrl+Backspace を消費するかの判定、戻す読みと戻した後の
状態、取り消しの edit session の手順と失敗したときの扱い、取り消しの後の学習、secure の文脈での扱い、部分確定との関係。

本書が委ねるもの:

| 主題 | 正典 |
|---|---|
| `UserAction` の値と、VK からの写像 | `docs/legacy-parity-spec.md` §1.1・§1.4 |
| `CommitCorrection` の payload と Host の記録 | `ipc/include/azookey/ipc/Payloads.h`、`docs/user-learning-enhancement-spec.md` §4.1 |
| 忘却（Ctrl+Shift+Backspace）の対象と送信 | `docs/legacy-parity-spec.md` §7 |
| 部分確定の手順と観測 | `docs/segment-editing-spec.md` §6・§8 |
| secure と学習許可の判定 | `docs/privacy-and-secure-input-spec.md` §5 |
| 受け入れ条件 | `plans/windows-port-roadmap.md` M69 |

## 1. 目的と範囲

目的は、確定した直後に誤りに気づいたとき、確定した文字を消して読みを打ち直さずに、確定する前の読みへ戻して
変換し直せるようにすることである。

再変換（M20、`docs/tsf-deep-integration-spec.md` §1）は、選択範囲やキャレットの位置の文字列から読みを逆引きして候補を
出す操作で、どの確定済みの文字列にも使える。確定の取り消しは、直前の確定について TIP が覚えている読みへ戻す操作で、
逆引きを使わず、戻した後は読み入力中と同じに編集できる。

範囲に含めないもの:

- 2 回以上前の確定へ遡る取り消しと、取り消しのやり直し。
- 一括変換（`batchRomajiConversion`）の確定の取り消し。
- 確定した文節へ戻る編集（`docs/segment-editing-spec.md` §1 の範囲外と同じ）。
- 取り消した確定の学習観測を Host から取り除くこと（§7）。

## 2. 用語

| 用語 | 意味 |
|---|---|
| 取り消しの対象 | 直前の確定について TIP が持つ記録。戻す読み、生ローマ字、確定した表層、確定した context、確定した文字列の直後のキャレットの位置（`ITfRange`）、その確定が送った学習観測の組からなる |
| 取り消し | 対象の確定した表層を、戻す読みの composition に置き換えること |
| 戻した入力 | 取り消しで始めた composition。確定、Esc、フォーカスの喪失などで終わるまでを指す |

## 3. キー

確定の取り消しは Ctrl+Backspace（修飾キーは Ctrl だけ）で行う。`UserAction` は `UndoCommit` とし、core の写像は
Idle の Ctrl+Backspace を `UndoCommit` に、ほかの状態の Ctrl+Backspace を `nullopt` にする（`docs/legacy-parity-spec.md`
§1.1・§1.4）。TIP は §4 の条件が揃うときだけキーを消費し、揃わなければアプリへ渡す。キーを消費するかを写像ではなく
TIP の判定で上書きする形は、忘却（`Forget`）と同じである（同 §1.5.3・§7.2）。

- Ctrl+Shift+Backspace は忘却のまま変えない（`docs/legacy-parity-spec.md` §7）。
- 読み入力中と候補選択中の Ctrl+Backspace は、アプリへ渡す（`docs/legacy-parity-spec.md` §1.4、
  `docs/tsf-deep-integration-spec.md` §2.4.1）。
- Alt または Win を含む組合せは扱わない。
- 取り消しに使った Ctrl+Backspace を押し続けたときのキーリピート（キーの前の状態が押下のもの）は、TIP が消費して
  何もしない。戻した入力の中の Ctrl+Backspace はアプリへ渡るので、リピートで戻した入力が消えるのを防ぐためである。
  キーを一度離した後の Ctrl+Backspace は、上のとおりアプリへ渡す。
- 設定キーは置かず、既定で働く。確定の直後の Ctrl+Backspace は、アプリの単語削除にならない。

Google 日本語入力（mozc）の Microsoft IME 互換のキー設定（`src/data/keymap/ms-ime.tsv`）は、入力していない状態の
Ctrl+Backspace を確定の取り消し（`Undo`）に割り当てている。Microsoft IME の公式のキー一覧（support.microsoft.com の
「Microsoft 日本語 IME」）には確定の取り消しの記載が無いので、Microsoft IME 自身の挙動は実機で比べる（§10）。

legacy（macOS 版）に確定の取り消しは無い。legacy の Ctrl+Delete（Backspace）は候補選択中の忘却で、Windows 版は
割り当てない（`docs/legacy-parity-spec.md` §7.1）。

## 4. 取り消せる確定と有効な条件

### 4.1 対象を記録する確定

TIP は、次の確定の edit session が済み、確定した文字列の直後にキャレットがあることを同じ確定の流れの中で
確かめられたときに、取り消しの対象を記録する。確かめ方は、無変換の巡回（M22）が直近の確定を記録する実装
`CharacterFormEditSession::CaptureRecentCommit` と同じで、選択が空であり、その直前の文字列が確定した表層と一致することを
確定の後の読み取りの edit session で確かめる。

- 候補の確定（Enter、数字、候補窓のクリック、UI-less の `Finalize`。Space の応答待ちに保留して再生したキーを含む）
- ライブ変換の表示中の Enter（表示中の文字列の確定）
- 読みのままの確定（`docs/legacy-parity-spec.md` §1.3 の as-is 確定）
- 字種指定確定（F6〜F10、`docs/tsf-deep-integration-spec.md` §3.6）
- 句読点の確定（`punctuationConversion = commit`、`docs/tsf-deep-integration-spec.md` §3.5.1）
- 部分確定の後に、残りの読みを確定して Idle に戻る確定（§8）

次の確定は対象を記録しない。直前に記録していた対象も捨てるので、それより前の確定には戻らない。

- 一括変換の確定、絵文字検索の確定、Unicode 入力（`docs/legacy-parity-spec.md` §6）、カッコ補完の挿入、全角空白の挿入、
  再変換の置換、無変換の巡回の置換、Magic Conversion の置換。
- TIP がフォーカスの喪失、Deactivate、IME を Off にするキー、英数モードへの切替、TIP の外からの Off、アプリによる
  composition の終了で行った確定（`docs/tsf-deep-integration-spec.md` §2.4.1・§4.2・§4.3）。利用者が確定の直後に
  押したキーではないためである。
- 確定の edit session が拒否され、次のキーで確定し直した確定（`docs/tsf-deep-integration-spec.md` §2.4.1）。
- 確定の直後に次の composition を始める確定（候補選択中の文字入力など。`docs/legacy-parity-spec.md` §1.2）と、
  部分確定そのもの。どちらも composition が残るので、§4.2 の条件 4 を満たさない。

### 4.2 有効な条件

Ctrl+Backspace を押した時点で、次がすべて成り立つときに取り消す。

1. 対象を記録した確定の後で、Ctrl+Backspace が最初のキーである。修飾キー（Shift、Ctrl、Alt、Win）だけの押下は
   数えない。Ctrl を押してから Backspace を押すので、Ctrl の押下で対象を捨てない。
2. Ctrl+Backspace を受けた context が、確定した context と同じである。
3. IME が On で、英数モードではなく、アプリがキーボード入力を無効にした context（`docs/tsf-deep-integration-spec.md`
   §4.3）でもない。
4. composition、候補窓（候補モードと予測モード）、確定の再試行の待ち、Space の応答待ちのどれも無い。読み・未確定の
   ローマ字・一括変換の入力、Unicode 入力、再変換の候補一覧、絵文字検索、カッコ補完の composition、句読点の最終応答の
   待ちも、入力中の状態としてこれに含める。
5. 確定の edit の後に、選択とキャレットが動いていない。`OnEndEdit` で、確定した edit の後の選択の変化を見る
   （`docs/user-learning-enhancement-spec.md` §4.1 の `undo` の窓と同じ見方）。この変化を見られるよう、TSF の text edit
   sink が見ている context の確定だけを対象として記録する（同 §4.1 と同じ）。
6. 取り消しの edit session の中で読み直したとき、選択が空で、キャレットが記録した位置にあり、その直前の文字列が
   確定した表層と一致する。

時間の上限は置かない。確定の直後であることは、条件 1・5・6 が決める。

次のときは対象を捨てる。捨てた後の Ctrl+Backspace はアプリへ渡す。

- Ctrl+Backspace 以外のキー（修飾キーだけの押下を除く）。
- 確定の edit の後の選択とキャレットの変化（マウスのクリック、アプリによる編集）。
- フォーカスと context の切り替え（push / pop を含む）、Deactivate、IME の Off、英数モードへの切替。
- 取り消しそのもの（成功したときも、§6 で失敗したときも）。

## 5. 戻す読みと戻した後の状態

戻す読みは、確定の直前に core が持っていた読みとする。未確定のローマ字をかなにした後で、auto_replace の補正
（`docs/typo-correction-learning-spec.md` §5）を反映した後の読みである。

- typo 補正の提案、英単語候補、日英混在の候補を選んで確定したときも、候補の読みではなく composition の読みを戻す。
- 生ローマ字（`docs/inline-english-candidate-spec.md` §4.1 のバッファ）も、確定の直前の値に戻す。英単語候補と
  F10 の字種指定確定は、取り消す前と同じに出る。
- 句読点の確定は、読みに確定した句読点（`、` `。` `？` `！`）を足して戻す。
- 自動句読点（M59。Host が読みを持たずに挿入した句読点）は戻さない。
- 予測の受理（`docs/legacy-parity-spec.md` §3.4）で伸びた分は、確定の直前の読みに含まれる。
- 字種指定確定（F6〜F10）は、字種を変える前の読みを戻す。
- 部分確定の後の確定は、残りの読みだけを戻す（§8）。

戻した後の `InputState` は、戻す読みを読みとする `Composing`（ライブ変換 ON なら `Previewing`）である
（`docs/legacy-parity-spec.md` §1.2）。composition には読み（かな）を表示し、読み入力中と同じく打鍵ごとの要求
（ライブ変換 OFF は `QueryCandidates`、ON は `QueryLiveConversion`）を 1 回出す。その後のキーは読み入力中と同じに
働き、Space で候補窓を開き、Backspace で読みの末尾を消し、文字入力で読みに足す。Esc は戻した入力を取り消すので、
確定していた文字列は文書から消える。

core は、§6 の edit session が済んでから `UndoCommit` を受けて `Composing`（`Previewing`）に入る。戻す読みを core へ
渡す入口は `UserActionEvent` の `reading` で、`HandleEvent` の純粋性は `docs/legacy-parity-spec.md` §1.5.1 のとおり保つ（同 §1.1）。
composition の文字列は §6 の edit session が書き、core は書き直さない。

## 6. edit session

TIP は `OnKeyDown` の中で、同期の読み書きの edit session（`TF_ES_SYNC | TF_ES_READWRITE`）を要求し、次の順に操作する。

1. §4.2 の条件 6 を確かめ直す。
2. 確定した表層の範囲で composition を始める（`ITfContextComposition::StartComposition`）。
3. composition の文字列を戻す読みに置き換え、読み入力中の表示属性を付ける（`docs/tsf-deep-integration-spec.md` §5.6）。
4. キャレットを composition の末尾に置く。

失敗したときは次のとおりにする。

- 1 が成り立たないとき、同期の edit session が拒まれたとき、2 が失敗したときは、文書を変えずに対象を捨て、キーを
  アプリへ渡す。アプリは Ctrl+Backspace を単語削除などに使う。無変換の巡回が直近の確定を書き換えられなかったときと
  同じ扱いである（`CharacterFormEditSession::CycleRecentCommit`）。
- 2 の後で 3 が失敗したときは、composition を確定した表層のまま `EndComposition` で終え、対象を捨ててキーを消費する。
  文書の文字列は変わらない。キーを渡すと、アプリが残った composition に作用しうるためである。
- 取り消しの後にアプリが composition の文字列を変えたときは、読み入力中と同じく
  `docs/tsf-deep-integration-spec.md` §2.4.1 の規則に従う。

失敗は理由の名前だけを RuntimeLogger に残し、読み・表層・生ローマ字は残さない。イベントの名前は実装で決め、
`docs/observability-spec.md` の規則に従う。

## 7. 学習

取り消した時点では、学習のメッセージ（`CommitObservation`、`CommitCorrection`、`ForgetLearningEntry`）を Host へ
送らない。戻した入力の打鍵ごとの要求（§5）は、読み入力中と同じに送る。

取り消しの対象は、取り消した確定が送った学習観測の組（読みと表層）のうち、自動句読点の組（`is_auto_punctuation`。
`docs/dynamic-punctuation-spec.md` §7.4）を除いた最後の組を持つ。これを「取り消した観測」と呼ぶ。即 Backspace の
`undo` が確定の最後の文節を対象にするのに倣う（`docs/user-learning-enhancement-spec.md` §4.1）。
戻した入力が最初の確定で終わり、次の 2 つがともに成り立つときに限り、`CommitCorrection`（`kind: "undo"`、
`docs/user-learning-enhancement-spec.md` §4.1）を 1 回送る。`reading` と `rejected_surface` は、取り消した観測の
読みと表層とする。

1. その確定が部分確定ではなく、送った観測の組（自動句読点の組を除く）に、取り消した観測と同じ読みの組がある。
2. その組の表層が、取り消した観測の表層と違う。

比べるのは観測の組どうしなので、句読点の確定（観測は句読点を除いた組。`docs/tsf-deep-integration-spec.md` §3.5.1）、
ライブ変換の確定が自動句読点の組を添えて送る `CommitSegmentsObservation`、文節の伸縮中に表示中の文字列を確定した
確定（観測は先頭文節の組。`docs/segment-editing-spec.md` §8）も、同じ規則で決まる。

次のときは送らない。

- 取り消した確定が学習観測を送っていなかったとき。as-is 確定、F6・F9・F10、ローカル候補による確定、English タグの
  候補、secure と学習不可の文脈での確定が当たる。
- 取り消した確定が、日英混在の自動変換（M63）の区間ごとの観測を送った確定のとき（`docs/mixed-english-auto-conversion-spec.md`）。
  区間の区切りは戻した読みと対応しないので、どの組を拒否したかが決まらない。取り消しそのものは §4.1 のとおり働く。
  ライブ変換の確定などが複数の組を送ったときは、上のとおり自動句読点の組を除いた最後の組で比べる。
- 戻した入力の最初の確定が観測を送らなかったとき、または取り消した観測と同じ読みの組を送らなかったとき（打ち足し、
  読みが変わる Backspace、句読点を足した確定）。
- 取り消した観測と同じ表層を確定し直したとき。
- 部分確定したとき、Esc で取り消したとき、確定しないまま終わったとき（フォーカスの喪失など）。
- 戻した入力の確定より前に context の push / pop があったとき。保留していた `undo` はその時点で捨てる。

取り消した時点で `undo` を送らないのは、打ち足すための取り消しや選び直しで、正しい表層に減点を残さないためである。
`correction_reject` は `reject_count` を増やし、`correction_penalty` の `net_reject` は accept でしか減らない
（`docs/user-learning-enhancement-spec.md` §4・§6.2）。

戻した入力の確定そのものの観測は、通常の確定と同じに送る。

取り消した確定の観測は取り消さない。変換器の学習バケットと自動単語登録（M36-A）の観測はそのまま残る。
`LearningStore` の weight と回数も、選び直したとき（次の 1 つ目）を除いてそのまま残る。取り消した確定の観測を
Host から取り除く IPC は置かない。理由は次のとおり。

- 選び直したときは、上の `undo` が取り消した観測の組の weight を観測 1 回分戻す（その確定で新しく作られた組なら
  0 になる）。その組の `reject_count` は 1 増える。確定の観測は `accept_count` を増やさないので、その確定で新しく
  作られた組は拒否の回数が受け入れの回数を上回り、weight 0 のまま GC の対象にならずに残る
  （`docs/user-learning-enhancement-spec.md` §3.1.1）。ほかの文節の組と変換器の学習バケットは残る。
  選び直し以外で終えたときに残るのは、確定した候補を観測 1 回分強めた学習である。
- 選び直し以外で終えたとき、取り消した確定だけで作られた組は、時間減衰で `learning_min_weight` を下回ると GC で
  消える（`docs/user-learning-enhancement-spec.md` §3.1.1。本番の減衰は同 §14.4 の M7 の式で、観測 1 回分の組は
  既定値ではおよそ 19 日で GC の対象になる）。変換器の学習バケットは Host のメモリにだけあり、Host を再起動すると消える。
- 残った組は、設定アプリの学習データタブでの忘却と全削除（`docs/learning-data-management-spec.md` §3・§4.2・§4.6）で
  利用者が消せる。Ctrl+Shift+Backspace の忘却（`docs/legacy-parity-spec.md` §7.2）は、取り消しで忘却の対象を捨てる
  （下記）ので、同じ表層を確定し直した後にだけその組に届く。
- Host から取り除くには、TIP が取り消す観測を id で指し、Host がその観測で変えた内容を覚えておく必要がある。
  Host には観測 1 回分を戻す API が無く、忘却も観測の逆操作ではない。

Esc で戻した読みを消したときも、取り消した確定の組は学習に残る。即 Backspace の `undo` の窓
（`docs/user-learning-enhancement-spec.md` §4.1）が閉じた後に、確定した文字を Backspace で消したときと同じ扱いである。

mozc は確定の取り消しで、文節の学習からその確定が新しく加えたキーを消す（既にあったキーの更新は戻さない）。本書は
これに当たる処理を持たない。M69 の実機確認（DEV-2123）で、取り消した確定の学習が後の変換を誤らせる例が見つかったときは、
次の 2 案を比べて見直す。

- Host に、観測の id を指して、その観測が新しく作った行と項目だけを消すメッセージを置く（mozc と同じ範囲）。
- TIP が、取り消しの窓が閉じるまで観測の送信を保留し、取り消したら送らない（IPC は変えない）。

取り消しで次を捨てる。

- 忘却（Ctrl+Shift+Backspace）の対象の組（`docs/legacy-parity-spec.md` §7.2）。取り消した確定はもう文書に無い。
- 即 Backspace の `undo` の窓（`docs/user-learning-enhancement-spec.md` §4.1）。Ctrl+Backspace は修飾キーを伴うので、
  この窓をもともと閉じる。
- 無変換の巡回の直近の確定（`docs/tsf-deep-integration-spec.md` §3.2）。

typo 学習の確定直後の打ち直し（`docs/typo-correction-learning-spec.md` §4-2）の窓は Ctrl+Backspace で閉じ、取り消した
確定の読みを訂正前の読みにしない。戻した入力の中で Backspace によって読みを直して確定したときは、読み入力中と
同じく同 §4-1 の対象になる。

送るかどうかは送信の時点で判定する（secure と学習許可。`docs/user-learning-enhancement-spec.md` §4.1）。

## 8. 部分確定との関係

部分確定（`docs/segment-editing-spec.md` §6）は 1 回ごとに独立した確定として扱う。部分確定そのものは取り消しの対象を
記録しない。部分確定の後は残りの読みが composition に残り、§4.2 の条件 4 を満たさないためである。残りの読みがある間の
Ctrl+Backspace は、読み入力中・候補選択中と同じくアプリへ渡す。

残りの読みを確定して Idle に戻ったときは、その確定が取り消しの対象になる。戻すのはその確定の読み（残りの読みの分）
だけで、先に部分確定した先頭文節へは遡らない。確定した文節へ戻る編集は `docs/segment-editing-spec.md` §1 の範囲外で
あり、本書もそれに揃える。

文節の伸縮中に表示中の文字列を確定する操作（文字入力、句読点の確定など。同 §4）は §4.1 の規則に従う。文字入力は
次の composition を始めるので対象にならない。句読点の確定は、表示中の文字列の読み（先頭文節の読みと残りの読み）に
句読点を足して戻す。

## 9. secure の文脈

secure の文脈（`docs/privacy-and-secure-input-spec.md` §5）でも、取り消しは働く。取り消しは文書の編集だけで、
学習のメッセージを Host へ送らない。§7 の `CommitCorrection` は送信の時点の判定で落ちる。戻した入力の打鍵ごとの
要求は、secure の読み入力中と同じに送る（同 §5 の制限を受ける）。

取り消しの対象の読み・表層・生ローマ字は TIP の中だけに持ち、ログには出さない。§4.2 の契機で捨てる。

## 10. テスト計画

| 対象 | 試験 |
|---|---|
| キーの写像 | `tsf-tip/tests/keymap_test.cpp`: Idle の Ctrl+Backspace が `UndoCommit` になり、ほかの状態の Ctrl+Backspace は `nullopt` になる。Ctrl+Shift+Backspace は `Forget` のまま、Alt・Win を含む組合せは `nullopt` |
| core の状態機械 | `core/tests/input_state_test.cpp`: `UndoCommit` で、渡した読みを読みとする `Composing`（ライブ変換 ON は `Previewing`）に入り、打鍵ごとの要求を出す。その後の Space・Backspace・文字入力・Esc が読み入力中と同じに働く |
| TIP | `tsf-tip/tests/onkeydown_preedit_test.cpp`（`DocumentPreeditHarness`）: §4.1 の各確定の直後の Ctrl+Backspace で、確定した表層が戻す読みの composition に置き換わる。対象を記録しない確定の後と、§4.2 の条件が欠けるとき（別のキーの後、キャレットの移動、別の context、Off、英数モード、composition がある間）は、キーがアプリへ渡る。修飾キーだけの押下で対象を捨てない。取り消しに使った Ctrl+Backspace のキーリピートが戻した入力を消さない。確かめ直しの失敗と edit session の拒否で、文書が変わらずキーがアプリへ渡る。composition を始めた後に読みへ置き換えられないとき（§6 の手順 3 の失敗）は、確定した表層のまま composition を終えてキーを消費する。戻す読み（typo 補正、英単語、句読点、F6〜F10、生ローマ字）が §5 のとおりになり、自動句読点は戻らない。部分確定の後は残りの読みだけが戻る。§7 の `undo` が送られる場合と送られない場合（自動句読点の組を添えた確定、M63 の区間の観測を含む）。取り消しで、忘却の対象、即 Backspace の窓、無変換の巡回の直近の確定を捨て、typo の確定直後の打ち直しの窓を閉じること。text edit sink の無い context の確定では対象を記録しないこと。secure で送らないこと |
| 実機（Human Gate） | Notepad、Word、Edge と Chrome（textarea と contenteditable）、VS Code、Excel のセル編集、スタート検索で、二重入力と位置のずれが無いこと、Esc で消えること、別の候補で確定し直せること、アプリの Ctrl+Z の単位、条件外の Ctrl+Backspace がアプリの単語削除になること。Microsoft IME の確定の取り消しとの比較 |

試験の一覧の正典は `docs/test-inventory.md` である。

## 11. 受け入れ条件

M69 の受け入れ条件は `plans/windows-port-roadmap.md` M69 が定める。
