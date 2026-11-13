# 校正ビュー（意味的訂正とバッチ訂正ビュー）仕様

対象リポジトリ: dolquis/azooKey-Desktop
関連:
- `docs/ai-backend-spec.md` §3.1（`Anomalies` タスク）・§8（secure ゲート連携）
- `docs/privacy-and-secure-input-spec.md` §5.1.1（event privacy の `secure` / `learning_allowed`）
- `docs/learning-data-management-spec.md` §15（Persona の算出と `QueryPersona`）
- `docs/user-learning-enhancement-spec.md` §4.1（`CommitCorrection`）
- `docs/tsf-deep-integration-spec.md`（再変換の候補窓、固定パス）
- `docs/settings-app-spec.md`（設定アプリの起動、校正ペインの位置づけ）
- `plans/windows-port-roadmap.md` M24 / M30
- `legacy/Core/Sources/Core/InputUtils/SegmentsManager.swift`（参照のみ。ライブ変換・予測候補・誤変換訂正の旧実装は、独立した `PredictionEngine.swift` / `CorrectionEngine.swift` ではなく `SegmentsManager` 内にある）

> 本書は、文書内の不自然な確定箇所を LLM で検出し、設定アプリの「校正」ペインに一覧して、
> アプリ側の該当範囲を選択・再変換するまでの契約を定める正典仕様である。
> 旧 `docs/rich-features-spec.md` の X-3-4 と X-3-6 を移した。
> 進捗・状態の正典は Linear であり、本書は状態を持たない。

---

## 0. 節とマイルストーンの対応

| 節 | マイルストーン |
|---|---|
| §1 | M24（横断項目。`DetectAnomalies` の検出） |
| §2 | M30（設定アプリの「校正」ペイン） |

## 1. 意味的訂正（検出の入口）

LLM で「不自然な確定箇所」を検出する API。Post-Commit Lint（`docs/ai-backend-spec.md` §4.3）と
似ているが、より長文（〜段落単位）を対象に、文法 / 敬語不一致 / 主述不一致までを検出する。

検出の入口は IPC `DetectAnomalies` で、所見は `start` / `length`（UTF-16 code unit）、`reason`、
`suggestions[]`、`confidence` を持つ。`docs/learning-data-management-spec.md` §15 の Persona を
文体の手がかりに使う。経路と制約は §2 が定める。

## 2. バッチ訂正ビュー

Ctrl+Shift+Space で「文書内誤変換候補一覧」を別ウィンドウに表示する。

- 起動: TSF の preserved key（`ITfKeystrokeMgr::PreserveKey`、Ctrl+Shift+Space）
- 表示: 設定アプリ内の「校正」タブ（Phase 7-M30）
- 動作:
  - フォアグラウンドアプリの選択位置の周りから文書を取得（`ITfContext` の range）
  - 設定アプリが Host に `DetectAnomalies` を投げる
  - 一覧表示、項目クリックでアプリ側の該当範囲を選択し、その範囲に再変換の候補を出す

### 2.1 ホットキー

TIP は有効化のたびに Ctrl+Shift+Space を preserved key として登録し、無効化で解除する。
`RegisterHotKey` は使わない。TIP は各アプリのプロセスに読み込まれるので、グローバルホットキーは最初に
登録した 1 プロセスにしか届かず、前面アプリの文脈を取れない。preserved key は azooKey が有効な入力文脈にだけ
届く。TIP がキーを取るのは、IME がオンで英数モードでなく、読みの入力中でも候補の表示中でもなく、文脈の
キーボードが無効（`GUID_COMPARTMENT_KEYBOARD_DISABLED` / `EMPTYCONTEXT`）でないときだけで、それ以外は
アプリへ渡す。IME オンの間は、Excel（列の選択）、Word（改行しないスペース）、VS Code（パラメーターヒント）
など Ctrl+Shift+Space を使うアプリの操作より TIP が優先する。

### 2.2 対象外のプロセス

昇格したプロセス、整合性レベルが medium 未満のプロセス、AppContainer の中の TIP は、
キーを登録せずアプリへ渡す。昇格したプロセスから起動した設定アプリは昇格したまま動き、低い整合性レベルの
プロセスは medium の設定アプリへ session pipe を提供できないため。

### 2.3 文書の取得

TIP は既存の AI 経路と同じ判定（event privacy の `secure` / `learning_allowed` と AI の可否）で、
secure の文脈、学習を許さない文脈、外部 AI を使えない文脈（入力欄の種類を判定できない、前面アプリを
特定できない、アプリ別プロファイルの `aiBackend` が `local-zenzai` など）では本文を一切読まない。それ以外では、選択の開始位置（選択が無ければ文書の先頭）から前後に
UTF-16 で `kMaxAnomalyTextBytes` ずつまでを読み取り専用の edit session で読み、選択位置を中心に UTF-8 で
`kMaxAnomalyTextBytes` 以下の窓を切り出す。窓はサロゲートペアを割らず、対になっていないサロゲートは
U+FFFD（3 バイト）として数える。読めた本文が空のアプリ、edit session や `GetText` を拒むアプリでは、本文を
取得できないものとして扱う。

### 2.4 session

session（`ipc/include/azookey/ipc/ProofreadSession.h`）。TIP は押下のたびに使い捨ての named pipe
`\\.\pipe\azookey-proofread-<token>` を作り、設定アプリを `--proofread-session <token>` だけを引数に起動する。
token はシステムの乱数から作る 128 bit（小文字 16 進 32 桁）で、本文はコマンドライン、ディスク、ログのいずれにも
出さない。pipe は 1 instance、`FILE_FLAG_FIRST_PIPE_INSTANCE`、`PIPE_REJECT_REMOTE_CLIENTS` で、DACL は現在の
logon SID だけを許し、強制ラベル（medium、no-read-up / no-write-up / no-execute-up）で整合性レベルの低い
プロセスからの接続を止める。TIP は接続してきたプロセスの実行ファイルが TIP と同じフォルダの
`azookey_settings.exe` であることを確かめ、違えば切断する（8 回で session を終える）。
この pipe は使い捨てであり、`MessageType` に含めない。

| メッセージ | 向き | フィールド |
|---|---|---|
| `document` | TIP → 設定アプリ（接続直後に 1 回） | `status`（`captured` / `blocked` / `unavailable`）、`text`（`captured` のときだけ非空の UTF-8）、`secure` / `learning_allowed`、`window`（前面へ戻すアプリのウィンドウ） |
| `select` | 設定アプリ → TIP | `start` / `length`（`text` 上の UTF-16 code unit）、`reconvert`（省略時と bool 以外は `false` として扱い、session は終えない。真なら選択した範囲に再変換の候補を出す）、`suggestions`（省略可能な文字列の配列。その項目の `suggestions[]` を候補に混ぜる。§2.7） |
| `select_result` | TIP → 設定アプリ | `result`（`ok` / `document_changed` / `context_gone` / `invalid_range` / `failed`） |

各メッセージは 4 バイト little-endian の長さに続く JSON で、`document` は 512 KiB、ほかは 4 KiB を上限とする。
上限を超える、または解釈できないメッセージを受けたら TIP は session を終える。`blocked` と `unavailable` の
session は本文を持たず、設定アプリはその理由だけを表示する。

session は、同じ TIP で再び押されたとき（新しい session に置き換える）、設定アプリが切断したとき、文書の
document manager が破棄されたとき、TIP が無効化されたとき、10 分間 `select` が来なかったときに終わる。

### 2.5 範囲の選択

TIP は窓の range と、窓の本文の UTF-16 長とハッシュだけを保持する。送った本文の最終バッファ
（読み取った本文、送信用の JSON と frame）は送信の成否によらず消去する。JSON ライブラリ内部などの中間の
コピーは解放だけで、消去はしない。
`select` を受けると、範囲が窓に収まらなければ `invalid_range` を返す。収まれば read-write の edit session で
窓を読み直し、長さとハッシュが取得時と同じときだけ、境界がサロゲートペアの途中でないことを確かめて
その範囲を選択する。文書が変わっていれば `document_changed`、本文を持たない session や文書が閉じられた
session では `context_gone`、読みの入力中は `failed` を返す。設定アプリは `select` を送る前に `window` を
前面へ戻す。

### 2.6 候補の提示

`reconvert` が真で選択が `ok` のとき、TIP は選択と同じ edit session の中で event privacy を
確かめ、secure の文脈では選択だけで候補を出さない。候補は edit session を抜けた後に TIP の UI スレッドで、
変換キーの再変換と同じ経路で開く。開く前に、session が続いていること、その文脈に focus があること、読みの
入力中でないこと、secure でないこと、選択がまだ同じ範囲と本文であることを確かめ、どれかが外れれば候補を
出さず、理由はログにだけ残す。日本語を含まない範囲と長すぎる範囲は、変換キーの再変換と同じく候補を出さない（`suggestions` があっても出さない）。`select_result` は選択の結果だけを表し、候補を出せたかどうかを含まない。
`reconvert` を知らない TIP は選択だけを行う。候補を選ぶと、変換キーの再変換と同じく置き換えと訂正の学習を行う。

### 2.7 修正候補の混在

設定アプリは、選んだ項目の `suggestions[]` を `select` の `suggestions` に載せる。
`suggestions` はモデルの出力なので、設定アプリは送る前に、TIP は受けた後に、同じ規則で要素ごとに検査する。

- 要素ごとの検査は、JSON を decode した後の文字列に対して行う。文字列でない要素、空の要素、UTF-8 で 256 バイトを
  超える要素は、その要素だけを捨てる。要求全体は拒否しない。残すのは検査を通った要素の先頭 5 件までとする。
  配列でない `suggestions` は無いものとして扱い、session は終えない。JSON として読めない要求（生の不正な UTF-8、
  対になっていないサロゲートの escape を含む）は、要素ごとの検査に届かず、ほかの解釈できないメッセージと同じく
  session を終える。
- 制御文字（U+0000–001F、U+007F–009F。タブと改行を含む）、U+2028 / U+2029、双方向制御文字（U+061C、
  U+200E / U+200F、U+202A–202E、U+2066–2069）、U+FEFF、U+FFF9–FFFB、不可視の文字（U+200B、U+2060–2064、
  tag 文字 U+E0000–E007F）を含む要素は、置き換えずに要素ごと捨てる。絵文字の連結と漢字の異体字に使う
  U+200C / U+200D と異体字セレクタ（U+FE00–FE0F、U+E0100–E01EF）は捨てない。
  JSON の escape で書かれた文字も、decode した後の文字で判定する。表示幅の上限は別に設けない。
- 設定アプリは、項目の範囲の本文そのものと同じ要素も送らない。検査を通った要素だけを送るので、要求は 4 KiB に収まる。

TIP は `suggestions` を候補の表記と置き換えの文字列としてだけ使い、指示・マークアップ・コマンド・読みとして
解釈しない。Host へは送らず、候補の注記も付けず、ほかの候補と同じ平文で描画する。ログには件数だけを残す。
並びは Host の先頭の候補、`suggestions`、残りの Host の候補の順で、既定の選択は Host の先頭の候補のままにする。
選んだ範囲の元の表記と同じ要素、Host の候補と同じ要素（完全一致で比べ、正規化はしない）、重複した要素は捨て、
Host の候補は元の順位に残す。選んだ範囲の再変換が始まったのに Host が候補を返さないとき（読みが得られない、Host に届かない）は `suggestions` だけを出す。
同じ範囲の再変換がすでに開いているときは、開いている一覧をそのまま使い、`suggestions` を足さない。
`suggestions` は再変換の cache に入れないので、アプリの再変換メニューや後の変換キーの再変換には出ない。
`suggestions` から選んだ候補は置き換えだけを行い、`CommitCorrection` も確定の学習観測も送らない
（`docs/user-learning-enhancement-spec.md` §4.1）。secure の文脈で候補を出さない判定は、`suggestions` にもそのまま適用する。
`suggestions` を知らない TIP はこのフィールドを無視し、Host の候補だけを出す。

### 2.8 Host の検出

Host の検出は `inference-host/include/azookey/host/AnomalyDetector.h` が持つ。§1 の検出は、AI 整文
（`ai-cleanup`）と同じ AI バックエンド設定（`aiBackend` / `openAi*`）と AI の同意
（`privacy.ai` / `privacy.external`）の下で、外部バックエンドだけを使う。設定が読めないときは同意が無いものとして扱う。ローカル経路には段落の誤りを
判定できるモデルが無いので使わない。使えないときは結果を推測せず、`unsupported` を返す。
モデルには、誤りと見た箇所を位置ではなく本文の文字列（`quote`）で返させる。Host はその文字列を本文で
前から順に探して UTF-16 の位置に直し、見つからない項目と、すでに置いた範囲とまったく同じ範囲の項目は捨てる。
範囲が重なるだけの項目は、理由が別なので残す。本文に 2 回以上現れる文字列は位置を一つに決められないので、
3 文字（UTF-16）未満なら捨て、それ以上なら `confidence` を 0.3 以下に下げて曖昧であることを示す。
`reason` は 512 バイトで切り、空の修正候補、`quote` と同じ文字列の修正候補、256 バイトを超える修正候補は捨てる。`reason` と `suggestions` はモデルの出力を
そのまま返すので、設定アプリはテキストとして表示し、指示として解釈しない。
本文と一緒に、Persona の 4 比率（数値だけ）も外部バックエンドへ送られる。`docs/learning-data-management-spec.md` §15 の
Persona があり、`sample_count` が 0 でなければ、文体の手がかりとして指示に添える。

### 2.9 IPC `DetectAnomalies`

capability は `detect_anomalies`。設定アプリが送る。

| 要求フィールド | 型 | 意味 |
|---|---|---|
| `text` | str（非空、`kMaxAnomalyTextBytes` = 60 KiB 以下） | 検査する本文（UTF-8）。AI バックエンドの要求上限 64 KiB に指示文と共に収まる大きさ |
| `max_findings` | uint（既定 20、1〜50 に丸める） | 返す件数の上限 |
| `secure` / `learning_allowed` | bool（既定 `true` / `false`） | 本文を取った文脈の event privacy |

secure の文脈、または学習を許さない文脈の本文は送らない。Host も、要求の `secure` が真か
`learning_allowed` が偽のとき、または Host の privacy 設定が学習を止めているときは、AI へ渡さずに
`blocked` を返す。利用者が設定アプリへ貼り付けた本文は、`secure=false`、`learning_allowed=true` で送る。

| 応答フィールド | 型 | 意味 |
|---|---|---|
| `ok` / `error` | bool / str | `error` は `invalid_request` / `not_authenticated` / `unsupported`（使える AI バックエンド・API キー・同意が無い）/ `blocked` / `backend_failed`（認証・通信・応答形式の失敗） |
| `findings[]` | 配列（50 件以下、`start` 順） | 各項目は `start` / `length`（本文上の UTF-16 code unit）、`reason`、`suggestions[]`（5 件以下）、`confidence`（0〜1。モデルが返さなければ 0.5） |

### 2.10 設定アプリの「校正」ペイン

- 本文は外部の AI サービスへ送られる（§2.8 のとおり AI 整文と同じ設定と同意の下で使う）ので、
  ペインは常に、送信先が外部であること・必要な設定を、検査の操作より上に表示する。確認ダイアログは出さない。
- 空の本文と、60 KiB（UTF-8）を超える本文は送らず、その旨を表示する。入力欄の下にバイト数を示す。
- 要求は `secure=false`、`learning_allowed=true`、`max_findings` は既定（20）で送る。
- `unsupported` は、外部の AI バックエンド・API キー・AI の同意のいずれかが無いことを示す文で表示する。
  capability `detect_anomalies` を告知しない Host には送らず、「この Host は未対応」と表示する。
- 各項目は、本文上の該当箇所（UTF-16 code unit の範囲を本文から切り出し、長いものは切り詰める）、`reason`、
  `suggestions[]`、`confidence` を並べる。`reason` と `suggestions[]` はモデルの出力そのものなので、
  テキストとしてだけ表示し、リンク・マークアップ・コマンドとして解釈しない。
- 範囲は重なりうる（同じ範囲の項目は Host が 1 件にまとめている）。各項目の「該当箇所を選択」は、検査したときの
  本文のまま編集されていないときだけ、その範囲を入力欄で選択する。範囲が本文に収まらない項目や、
  サロゲートペアの途中を指す項目では、箇所を示さず選択も出さない。
- `--proofread-session <token>` で起動されたとき（起動済みの設定アプリへ渡されたときも同じ）は、ペインを
  開いて TIP の session に接続する。pipe を提供しているプロセスが自分と同じユーザー SID と logon SID を持ち、
  整合性レベルが medium 以上であることを確かめ、確かめられなければ何も読まずに接続を捨てる。さらに、
  そのプロセスが `document` の `window` を持つプロセスでなければ捨てる（アプリが view のウィンドウを返さず
  `window` が 0 の session では、この確認だけをしない）。続けて起動されたときは最後の起動の session を採る。`captured` の本文は入力欄に入れ、「検査する」を押したときに送る
  （自動では送らない）。入力欄が session の本文のままなら、要求の `secure` / `learning_allowed` は
  `document` の値をそのまま使い、本文は入力欄ではなく `document` の `text` を送る（入力欄は改行を変えるため、
  一覧の位置は `document` の本文で数える）。編集した本文は貼り付けた本文として扱う。
- `blocked` と `unavailable` の session では、入力欄を変えずに理由だけを表示する。`blocked` は理由を
  分けず、保護された入力欄、学習や AI の利用を許さない設定、種類を判定できない入力欄のいずれかであるため
  送れないことを示す。`unavailable` はアプリから本文を読めなかったことを示す。
- session の本文を検査した一覧では、「該当箇所を選択」は `window` を前面へ戻してから `reconvert` を真にした `select` を送り、
  `select_result` を文で表示する（`ok` 以外は選択しなかった理由）。session が応答しなくなったら session を
  閉じ、もう一度 Ctrl+Shift+Space を押すよう表示する。
