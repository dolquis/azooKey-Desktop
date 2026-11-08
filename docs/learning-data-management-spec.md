# 学習データ可視化・バックアップ 仕様

対象リポジトリ: dolquis/azooKey-Desktop
対応マイルストーン: M49（プライバシー / モデル管理 / 学習データ UI トラック）
関連: `plans/windows-port-roadmap.md` M7 / M9 / M30 / M34 / M35 / M36-A / M55、
      `docs/typo-correction-learning-spec.md`、
      `docs/auto-word-registration-spec.md`、
      `docs/privacy-and-secure-input-spec.md`

## 1. 目的

ユーザーが azooKey の学習内容を **確認・削除・バックアップ・復元** できる
ようにする。透明性をユーザー体験として提供し、学習機能への信頼を担保
する。

## 2. 対象データ

| データ | 既定パス | 由来 M | 暗号化 |
|---|---|---|---|
| `learning.tsv` | `%LOCALAPPDATA%\azooKey\data\learning.tsv` | M7 | M34 で `.enc` |
| `learning.v2.tsv` | `%LOCALAPPDATA%\azooKey\data\learning.v2.tsv`（M54 の v2 形式。暗号化された移行元 `learning.tsv.enc` は v2 保存・読み戻し検証後に削除する） | M54 | M34 と同じ `.enc` |
| `user_dict.json` | `%LOCALAPPDATA%\azooKey\data\user_dict.json` | M9 | M34 で `.enc` |
| `typo_corrections.tsv` | `%LOCALAPPDATA%\azooKey\data\typo_corrections.tsv` | M35 / M55 | M34 で `.enc` |
| `auto_words.tsv` | `%LOCALAPPDATA%\azooKey\data\auto_words.tsv` | M36-A | M34 で `.enc` |
| `english_learning.tsv` | `%LOCALAPPDATA%\azooKey\data\english_learning.tsv` | M60（英単語候補の確定。`docs/inline-english-candidate-spec.md` §5） | `learning.tsv` と同じ `.enc` |
| `english_learning.v2.tsv` | `%LOCALAPPDATA%\azooKey\data\english_learning.v2.tsv`（`learning.v2.tsv` と同じ v2 形式。移行と M7 形式の削除の規則も同じ） | M60 | `learning.tsv` と同じ `.enc` |

プロセス終了時にこれらのデータが保存される範囲は §11 で定める。

## 3. UI（設定アプリ 学習データタブ）

```
[学習データ]

タブ:
  [学習候補] [ユーザー辞書] [タイプミス補正] [新語候補] [バックアップ]

検索: [__________________]

読み        候補       重み   最終使用       タグ     操作
にほんご    日本語     4.2    2026-05-27    [学]     [忘却]
ぜんざい    Zenzai     3.1    2026-05-26    [辞]     [忘却]
こうしょう  交渉       2.8    2026-05-25    [典]     [忘却]
```

操作:

- **検索**: reading / surface の部分一致
- **忘却**: 個別エントリを忘れる（ストアごとの規則は §4.2）
- **エクスポート**: 各ファイルを DPAPI で暗号化した ZIP 出力（§5）
- **インポート**: 別環境からの復元（§6）
- **全削除**: 当該タブのデータを全消去（DPAPI 鍵は維持）

設定アプリの「学習」ペイン（`sideload-packaging-spec.md` §3.2）は、上の図の
タブのうちストア別の 4 つ（学習候補 / ユーザー辞書 / タイプミス補正 / 新語候補）と「バックアップ」を持つ。

- ストアのタブ: 検索（`ListLearningEntries` の `query`）、100 件ずつのページ送り、
  読み・候補・重み（小数 1 桁）・最終使用（ローカル日付）・タグの表示、個別の忘却を持つ。
  忘却は確認ダイアログを経てから `ForgetLearningEntry` を `id` で送る。
- ユーザー辞書のタブだけは、上の操作に加えて「読み」「語」の 2 欄と「追加」ボタンを持つ
  （`sideload-packaging-spec.md` §3.7）。両欄の前後の空白（全角を含む）を除き、読みのカタカナを
  ひらがなに直してから `AddUserWord` を送る。どちらかが空なら送らない。成功したら追加した読みで
  一覧を検索し直す。失敗は応答の `error`（`busy` / `lock_failed` / `load_failed` / `save_failed` /
  `unavailable` / `invalid_request` / `not_authenticated`）ごとの文言で示す。利用者が明示的に行う編集なので、
  確定観測の secure・学習可否の判定は掛けない。設定の `dictionary.userDictionaryEnabled` が偽でも
  追加と忘却はでき、変換に使うかどうかだけがその設定に従う。
- 「バックアップ」タブ: 対象ストアの選択、暗号化の切替、エクスポート、取り込み時の衝突解決
  （`merge` / `overwrite` / `keep_both`）の選択、インポートを持つ。暗号化をオフにしたときは警告を出し、
  書き出す前に確認する。インポートは確認ダイアログを経てから送る。
- 「全削除」は確認ダイアログを経てから `ResetLearningStore`（§4.6、capability `learning_reset`）で
  ストア単位に送る。

## 4. IPC

設定アプリは学習データファイルを直接開かず、一覧、忘却、エクスポート、インポート、全削除、
ユーザー辞書への追加（`AddUserWord`）をすべて Host への IPC として発行する（writer 責務の正典は
§14）。
`user_dict.json` を含む本節の対象ストアはすべてこの経路に従う。

`MessageType` enum 末尾に append（M40 互換性）。新規 type 名は
`ListLearningEntries` / `ForgetLearningEntry` / `ExportLearningData` /
`ImportLearningData`。エンベロープは `ipc/src/Messages.cpp` の既存
wire format `{version, request_id, type, trace_id, payload}` に従い、
request と response は同一 `type` を共有して payload schema で区別する。

### 4.1 ListLearningEntries

Request（settings-app → host）:

```json
{
  "version": 1,
  "request_id": 100,
  "type": "ListLearningEntries",
  "trace_id": "018fd2c2-...",
  "payload": {
    "store": "learning",
    "query": "にほん",
    "limit": 100,
    "offset": 0
  }
}
```

`store` の取り得る値: `learning` / `user_dict` / `typo` / `auto_word`。

Response（host → settings-app）:

```json
{
  "version": 1,
  "request_id": 100,
  "type": "ListLearningEntries",
  "trace_id": "018fd2c2-...",
  "payload": {
    "total": 1234,
    "entries": [
      {
        "id": "0123456789abcdef",
        "channel": "kana",
        "reading": "にほんご",
        "surface": "日本語",
        "weight": 4.2,
        "last_updated_epoch_sec": 1780000000,
        "tags": ["learned"],
        "metadata": {}
      }
    ]
  }
}
```

ページネーション（`limit` / `offset`）必須。エントリ数が多くなる学習
ストアでも UI 応答を保つ。`limit` は 500 で頭打ちにし、`total` は検索に一致した全件数を返す。

- **並び**: 学習チャネル、読み、表記の昇順。
- **学習チャネル**: `learning` ストアはかな（`kana`）と英語（`english`、DEV-1196）の
  2 つの `LearningStore` を列挙し、各 entry にチャネル名を持たせる。英語のチャネルには、M63 の
  負の観測の行（読みが `¬` で始まり、重みは 0。`docs/mixed-english-auto-conversion-spec.md` §11）も
  ほかの行と同じく出る。その忘却は英字の抑制を解く。
- **学習の entry**: 1 つの `(reading, surface)` を 1 entry とし、app 別の行
  （`user-learning-enhancement-spec.md` §3.2）を合算する。`weight` は合計、
  最終使用は最大、`tags` は app 名、`metadata` は `commit_count` / `accept_count` / `reject_count`。
  忘却済みの組は出さない。
- **id**: ストア名、チャネル、読み、表記をそれぞれ長さ付きで連結した文字列の
  SHA-256 の先頭 16 桁（小文字 16 進）。キーから決まるので、Host の再起動後も変わらず、
  採番状態を保存しない。

### 4.2 ForgetLearningEntry

Request:

```json
{
  "version": 1,
  "request_id": 101,
  "type": "ForgetLearningEntry",
  "trace_id": "018fd2c2-...",
  "payload": {
    "store": "learning",
    "id": "abc"
  }
}
```

Response:

```json
{
  "version": 1,
  "request_id": 101,
  "type": "ForgetLearningEntry",
  "trace_id": "018fd2c2-...",
  "payload": {
    "removed": true
  }
}
```

忘却の扱いはストアごとに次のとおり。

| ストア | 忘却 |
|---|---|
| `learning` | 組の全 app 行の weight と回数を 0 にする。次の保存でファイルから除く。再観測時は新規として扱い、過去の回数を引き継がない |
| `typo` | ペアを削除する |
| `user_dict` | 単語を削除する |
| `auto_word` | 状態を `rejected` にする。削除すると同じ語を再び候補として提案するため、行は残す |

`learning` の 0 化した行を保存で除くのは、忘れた語をディスクに残さないためである。
`learning_min_weight` の GC（`user-learning-enhancement-spec.md` §3.1.1）が無効でも除く。
忘却した組は、前方一致の `min_score` の値によらず予測に出さない。
変換器がメモリに持つ同じ組の確定履歴も消す。辞書由来の候補は元のスコアと属性に戻し、
確定だけで追加された候補は除く。ほかの組の履歴は保つ。

v2 の保存・読み戻し検証後は M7 ファイル（`learning.tsv.enc`）を削除する
（`user-learning-enhancement-spec.md` §3.1）。検証や削除に失敗して M7 ファイルが残っている場合は、
忘却を M7 ファイルにも及ぼし、同じ組の行を M7 の書式のまま除く。
M7 ファイルへの書き戻しは既存の atomic replace と file lock に従い、
失敗しても M7 ファイルを壊さない（失敗したら忘却を失敗として返す）。
通常の保存では M7 の行を書き換えず、検証済みの暗号化 M7 ファイルを削除する。

### 4.3 ExportLearningData

Request:

```json
{
  "version": 1,
  "request_id": 102,
  "type": "ExportLearningData",
  "trace_id": "018fd2c2-...",
  "payload": {
    "stores": ["learning", "user_dict", "typo", "auto_word"],
    "destination_path": "C:\\Users\\me\\Desktop\\azookey-backup.zip",
    "encrypt": true,
    "include_settings": false
  }
}
```

Response:

```json
{
  "version": 1,
  "request_id": 102,
  "type": "ExportLearningData",
  "trace_id": "018fd2c2-...",
  "payload": {
    "status": "success",
    "file_size_bytes": 12345,
    "encrypted": true,
    "items": [
      { "name": "learning", "file": "learning.tsv.enc", "count": 1234, "sha256": "..." }
    ]
  }
}
```

`encrypt = true` のとき各ファイルを DPAPI で暗号化する。`encrypt =
false` は明示的な選択時のみ許可し、UI で大きな警告を表示する。

### 4.4 ImportLearningData

Request:

```json
{
  "version": 1,
  "request_id": 103,
  "type": "ImportLearningData",
  "trace_id": "018fd2c2-...",
  "payload": {
    "source_path": "...",
    "conflict_resolution": "merge",
    "stores": ["learning", "user_dict"]
  }
}
```

`conflict_resolution` の取り得る値: `merge` / `overwrite` / `keep_both`。

Response:

```json
{
  "version": 1,
  "request_id": 103,
  "type": "ImportLearningData",
  "trace_id": "018fd2c2-...",
  "payload": {
    "status": "success",
    "imported_counts": { "learning": 1234, "user_dictionary": 42 },
    "skipped_counts": { "learning": 5 },
    "conflict_counts": { "learning": 3 }
  }
}
```

counts のキーは archive の item 名（§5.1）である。

### 4.5 共通の応答規則と配線

- `ListLearningEntries` と `ForgetLearningEntry` の応答は `ok` と、失敗時の `error` を持つ。
  `ExportLearningData` と `ImportLearningData` は `status`（`success` / `error`）と、失敗時の `error` を持つ。
  `error` は次のどれかである。
  - `invalid_request`: payload が不正、または未知のストア名、または未知の `conflict_resolution`。
  - `not_authenticated`: Handshake のトークン認証の前に送った。
  - `save_failed`: 忘却を保存できなかった。M7 ファイルからの除去の失敗も含む。
  - `unsupported`: `include_settings = true` を指定した。`settings.redacted.json` の export は未対応で、黙って無視しない。
  - archive の失敗は `invalid_path`、`destination_exists`、`decrypt_failed` などの名前で返す。
    いずれも `BackupErrorCode` の名前で、`bad_item` と `store_unavailable` を含む。
- `ForgetLearningEntry` は、`id` か、`reading` と `surface` の組のどちらか一方を受ける。
  組の形は TIP の Ctrl+Shift+Backspace（`legacy-parity-spec.md` §7.2）が使い、`store` は `learning`（かなのチャネル）に限る。
  一致する項目が無いときは `ok = true`、`removed = false` を返す。
- export の応答は `manifest` の代わりに `encrypted` と `items`（`name`、`file`、`count`、`sha256`）を返す。
- Host は Handshake の capabilities に `learning_data_management` を載せる。
- Host は一覧・忘却・エクスポート・インポート・全削除を `InferenceEngine` 上で行う。
  忘却・全削除は確定観測などの writer と専用の mutex で直列化し、ストアを短いロック区間で複写する。
  暗号化・保存・旧形式の書き戻し・再試行は状態と変換器のロックの外で行い、返る前に結果を反映する。
  保存中の問い合わせは直前に公開されたストアを参照できる。学習の反映と変換器の履歴更新は同じ区間で行う。
  定期 flush は操作中の snapshot を上書きせず、操作終了後に再開する。
  ユーザー辞書は、`AddUserWord` と同じくファイルロックを取り、ディスクの内容を読み直した複写へ変更する。
  `AddUserWord` / `RemoveUserWord` と import も、ファイルロックの待ちと読み直し・保存を状態のロックの外で行い、
  保存に成功したときだけ複写を公開する。import は複写への merge だけを状態のロックの中で行う。
  Host の中でユーザー辞書のファイルロックを取る経路はどれも、確定観測などの writer の mutex とは別の、ユーザー辞書の writer の mutex を持つ。
  忘却・全削除・import は writer の mutex、ユーザー辞書の writer の mutex、ユーザー辞書のファイルロック、状態のロックの順に取る。
  `AddUserWord` / `RemoveUserWord` は writer の mutex を取らず、ユーザー辞書の writer の mutex から同じ順に取る。
  逆の順に取る経路は無い。どの経路もファイルロックを公開まで持つので、同じ Host の編集どうしで変更は失われず、
  どの経路がファイルロックを待つ間も変換の問い合わせは待たされず、`AddUserWord` / `RemoveUserWord` が待つ間は確定観測も待たされない。
- export はロックを持ったままストアの内容を複写し、暗号化と ZIP の書き出しはロックの外で行う。
  import は archive の読み込み・検証・復号をロックの外で行い、解析と merge とユーザー辞書以外の保存だけをロックの中で行う。
  いずれも変換の経路を止める時間を短くするためである。
- import の後の保存に失敗したときは `error = "io"` を返す。ユーザー辞書は変更前に戻す。
  学習・typo・auto_word は merge した内容をメモリに残し、次の flush で保存を再試行する。
  設定アプリは、取り込みがまだディスクに無いことを利用者に伝える。
- 学習の忘却は、v2 側に組が無くても M7 ファイルからの除去を試みる。M7 ファイルからの除去に失敗すると
  `save_failed` を返す（v2 側の忘却は保存済みでありうる）。このとき一覧からは組が消えて id では届かないので、
  TIP と同じ `{reading, surface}` の形で再試行すると、M7 ファイルの行を除いて `removed = true` を返す。
  移行前にだけ学習した組（M7 ファイルにだけある組）も、この形で除ける。一覧は v2 側の組だけを出す。
- 確定観測（`CommitObservation` / `CommitSegmentsObservation`）は任意の `app`
  （`process_name`、`window_class`）を持つ。Host は `process_name` を正規化して学習の app 行とし、
  `left_context` から `context_hash` を求める（`user-learning-enhancement-spec.md` §3.2、§8.1）。

### 4.6 ResetLearningStore

§3 の「全削除」は `ResetLearningStore` で Host へ送る。Host は Handshake の capabilities に
`learning_reset` を載せ、設定アプリはそれを告知した Host にだけ送る。

```json
{ "type": "ResetLearningStore", "payload": { "store": "learning" } }
```

応答は `{ "ok": bool, "error": str（失敗時） }` である。`error` は §4.5 の名前を使い、
`invalid_request`（`store` が無い、または未知）、`not_authenticated`、`store_unavailable`
（そのストアを Host が持っていない）、`save_failed` のいずれかである。

ストアごとの範囲は次のとおり。

| `store` | 消す範囲 |
|---|---|
| `learning` | かなと英語の学習チャネルの全行と、M7 ファイル（§4.2）の全行 |
| `user_dict` | ユーザー辞書の全単語 |
| `typo` | タイプミス補正の全ペア |
| `auto_word` | 新語候補の全行。`rejected` も消えるので、断った語も再び候補になりうる。確定から派生して英単語辞書に入った語（追加語の journal、overlay、コンパクション済みの base、退避した journal。`docs/inline-english-candidate-spec.md` §4.8） |

リセットは途中で失敗しても半端に消えない。Host は複写を空にして保存し、成功時だけ公開する。
保存に失敗したら公開済みの内容を保って `save_failed` を返す。`learning` は複数のファイルに
またがるので、次の順に処理する。

1. 読み込みに失敗して保存を止めているチャネルが 1 つでもあれば、何も変えずに `save_failed` を返す。
   保存できないファイルは空にできず、`ok` を返すと消えていないデータが次の起動で戻るためである。
2. 各チャネルを空にして v2 ファイルへ保存する。
3. M7 ファイルの全行を除く。v2 ファイルが先にあるので、M7 ファイルが学習の唯一の写しになる時点は無い。
4. 手順 2 か 3 が失敗したら、全チャネルを退避した内容へ戻して保存し直し、`save_failed` を返す。
   保存し直しにも失敗したチャネルは、戻した内容をメモリに持ち、次の flush で再び保存する。

このため `save_failed` の後も Host が提示する学習の内容は変わらない。
`learning` のリセット成功時は、変換器がメモリに持つ確定履歴も全て消す。
保存に失敗したリセットでは、変換器の履歴を保つ。
`auto_word` の忘却・全削除と新語の承認・却下は、writer の mutex を保持して複写を変更・保存する。
保存成功時だけ表を公開する。同時操作はその後に進むため、失敗時に承認・却下を上書きしない。
`auto_word` の全削除は、表を空にして保存した後、writer の mutex を放してから英単語辞書の追加語を消す
（手順と途中失敗の扱いは `docs/inline-english-candidate-spec.md` §4.8「学習データの全削除での消去」）。
英単語辞書のロックを取れない、または消去の途中で失敗したときは `save_failed` を返す。このとき新語候補の表は
すでに空で、英単語辞書には追加語の一部が残りうる。再び全削除すると同じ語を消す。
TSV が無く `.bin` だけの英単語辞書では、追加語の記録（journal）に残らないままコンパクションで base に入った語と、
解析できない journal にあった語は消せない（同 §4.8）。
`auto_word` の忘却（`rejected` にする）は、確定で英単語辞書に入った語を取り消す（同 §4.8「却下での取り消し」）。
英単語辞書への書込みは、どの経路も writer の mutex と状態のロックの外で、辞書の更新 mutex、`english-words.lock` の順に取る。
逆の順に取る経路は無い。
`user_dict` は `AddUserWord` と同じくファイルロックを取り、ディスクの内容を読み直してから空にする。

## 5. バックアップ形式

```
azookey-backup-YYYYMMDD-HHMMSS.zip
├── manifest.json
├── learning.tsv.enc       (DPAPI 暗号化。かなの学習)
├── learning_english.tsv.enc  (英語の学習)
├── user_dict.json.enc
├── typo_corrections.tsv.enc
├── auto_words.tsv.enc
├── english_learning.tsv.enc
└── settings.redacted.json  (include_settings=true 時のみ)
```

- **ZIP**: 圧縮しない STORE 方式だけを使う（§7）。読み込みも STORE 以外、ZIP64、
  暗号化フラグ、data descriptor を含む archive を拒否する。
- **entry の中身**: 各ストアが保存時に書く平文（学習は v2 形式）を、ファイルごとに暗号化したもの。
- **読み込みの制限**: Host は archive をディスクへ展開しない。manifest が挙げた
  固定名の entry だけをメモリ上で読む。上限は archive 全体 64 MiB、entry 16 件、
  1 entry 32 MiB で、読む前に検査する。
- **パス**: `destination_path` / `source_path` は絶対パスで `..` を含まず、拡張子は `.zip` とする。
  ファイル名に `:` を含むパス（NTFS の代替データストリーム）は拒否する。
  UNC（`\\host\share`）とデバイス名前空間（`\\?\`、`\\.\`）、予約デバイス名
  （`CON`、`PRN`、`AUX`、`NUL`、`COM1`〜`COM9`、`LPT1`〜`LPT9` など。拡張子が付いていても同じ）も拒否する。
  `source_path` がシンボリックリンクなら、たどらずに拒否する。
- **上書きしない確定**: export は同じディレクトリの一時ファイルへ書いてから、置換しない移動
  （Windows では `MOVEFILE_REPLACE_EXISTING` なしの `MoveFileEx`）で確定する。
  検査の後に別のプロセスが `destination_path` を作っても、上書きせずに `destination_exists` で失敗する。
  `destination_path` は既存ファイルを上書きせず、親ディレクトリが存在すること。
  `source_path` は存在する通常ファイルであること。

### 5.1 manifest.json

```json
{
  "version": 1,
  "created_at": "2026-05-27T00:00:00+09:00",
  "app_version": "0.1.0",
  "host_version": "0.1.0",
  "encrypted": true,
  "encryption_method": "dpapi-user-scope",
  "items": [
    { "name": "learning", "file": "learning.tsv.enc",
      "count": 1234, "sha256": "..." },
    { "name": "user_dictionary", "file": "user_dict.json.enc",
      "count": 42, "sha256": "..." },
    { "name": "typo_corrections", "file": "typo_corrections.tsv.enc",
      "count": 89, "sha256": "..." },
    { "name": "auto_words", "file": "auto_words.tsv.enc",
      "count": 17, "sha256": "..." },
    { "name": "english_learning", "file": "english_learning.tsv.enc",
      "count": 5, "sha256": "..." }
  ]
}
```

各 entry に SHA-256 を含めて整合性を検証する。SHA-256 は ZIP に格納したバイト列
（暗号化後）に対して計算し、すべての entry を検証してから復号する。
平文の SHA-256 は書かない。短い学習データは平文のハッシュから推測されうるためである。
平文 export の `encryption_method` は `"none"` とする。

### 5.2 暗号化

DPAPI ユーザースコープ（`CryptProtectData`）で各ファイルを暗号化する
（M34 と同じ）。**他ユーザー / 他マシンでは復号できない**ことを設計の
前提とする。

明示エクスポート（平文）時のみ、ユーザーが自己責任で平文 ZIP を出力
できる。この場合 manifest の `encrypted = false` とし、設定アプリで
ユーザーに「秘密情報が平文で保存されます」と警告する。

## 6. インポートの衝突解決

| 衝突パターン | `merge` | `overwrite` | `keep_both` |
|---|---|---|---|
| learning で同 reading/surface/app_name | weight と各回数を加算、最終使用と最後のイベントは新しい方 | 上書き | 既存維持 |
| user_dict で同 reading/word | 既存維持 | 上書き | 既存維持 |
| typo で同 wrong/correct | count 加算（上限で飽和）、最終使用は新しい方 | 上書き | 既存維持 |
| auto_word で同 surface/reading | count 加算、状態は既存維持 | 上書き | 既存維持 |
| 形式バージョン違い | migration 実行 | migration 実行 | migration 実行 |
| 暗号化データで復号失敗 | error 返却 | error 返却 | error 返却 |

`merge` を既定とする。UI で選択可能にする。

- 衝突しないキーは、どの方式でも追加する。
- 各ストアのキーは一意なので、`keep_both` は同じキーに 2 つの値を持てず、既存を維持する。
- auto_word の `merge` で状態を既存のまま残すのは、import によってユーザーが却下した語が
  候補に戻るのを防ぐためである。
- archive の読み込み、checksum、復号、各ストアの解析がすべて成功してから、
  ストアを変更する。どれかが失敗した場合はどのストアも変更しない。
  解析できない行を 1 行でも含む item は、Host が書いたものではないとみなし、
  import 全体を `bad_item` で失敗させる（一部だけ取り込んで成功を返さない）。
- 選んだストアのどれかが読み込みに失敗して保存を止めている場合、export と import は
  `store_unavailable` で失敗する。空のストアを正常なバックアップとして書き出したり、
  保存されない merge を成功として返したりしないためである。
- export の manifest の `count` は item に実際に書いた行数（学習では忘却した組を除く）とする。
- learning の migration は v2 の読み込み規則（`user-learning-enhancement-spec.md` §3.1）による。
  M7 形式の entry も v2 の行として取り込む。

## 7. BackupArchive 実装

実装は 3 層に分ける。

| ファイル | 責務 |
|---|---|
| `inference-host/src/StoreOnlyZip.cpp` | STORE 方式だけの ZIP の書き出しと検証付き読み込み（CRC-32 を含む） |
| `inference-host/src/BackupArchive.cpp` | manifest、entry ごとの暗号化と SHA-256、パスとサイズの検査（`WriteBackupArchive` / `ReadBackupArchive`） |
| `inference-host/src/LearningDataManager.cpp` | ストアの一覧・忘却・export・import と衝突解決（§4、§6） |

暗号は `learning::ByteCrypto` を注入して使う。本番は DPAPI、テストはモック鍵である。
ZIP ライブラリ（miniz など）は依存に加えない。DPAPI で暗号化した entry は圧縮が効かず、
STORE 方式で足りるためである。平文 export は圧縮されない。
外部の ZIP ツールで圧縮し直した archive は、STORE 以外の方式を含むので import できない。

## 8. プライバシー

- M46 secure mode 中はエクスポート / インポート操作を blocking する
  （secure または学習不許可のとき。消費側の判定責務は
  `docs/privacy-and-secure-input-spec.md` §5.1.1 に従う）
- バックアップ ZIP には API キー / OpenAI 関連設定を含めない（`settings`
  含める場合は M44 §12.5 と同じ redaction）

## 9. テスト

- unit: `BackupArchive` の暗号化 / 復号 round-trip
- unit: manifest SHA-256 検証
- unit: 衝突解決 3 種
- unit: export → 別の鍵（別 DPAPI 鍵に相当するモック鍵）で import 失敗、ストアは不変
- unit: 同じ鍵での export → import で全ストアの内容が一致
- unit: 不正な archive（ZIP の破損、manifest の不正、checksum 不一致）で失敗し、ストアは不変
- 同じ入力から同じバイト列の archive を書くこと（manifest のキー集合の固定を兼ねる）

テストの対応は `docs/test-inventory.md` の `host_store_only_zip_tests`、
`host_backup_archive_tests`、`host_learning_data_manager_tests` を参照する。
実 DPAPI での他ユーザーでの復号失敗は Human Gate（M34）である。

## 10. M49 受け入れ条件

受け入れ条件の定義の正典は [`plans/windows-port-roadmap.md`](../plans/windows-port-roadmap.md)
の M49 節とする。本書は可視化・忘却・export / import の形式と DPAPI ユーザースコープの
制約を定義し、受け入れ条件を複製しない。クロス環境復元が必要な場合は §5.2 の明示的な
平文エクスポートを使う。

## 11. プロセス終了時の flush 保証境界

学習データは確定のたびに同期保存せず、未保存の観測が一定件数に達するか一定時間が経過した時点でまとめて書き出す（`docs/user-learning-enhancement-spec.md` §3.1.1）。
そのため、プロセスが終了する瞬間には未保存の観測が残りうる。
本節は、どの終了経路まで保存を保証するか、保証しない経路で最悪どれだけ失われるかを定める。
本節の対象は Host が書く `learning.tsv` であり、`user_dict.json` のように操作のたびに保存するストアは含まない。
本節は終了経路ごとの flush の保証、burst 先頭の同期 flush、失われうる量の正典であり、
`docs/user-learning-enhancement-spec.md` §3.1.1 と `docs/windows-tsf-host-architecture.md`「学習」から参照される。

### 11.1 終了経路の区分

終了経路を次の 3 区分で扱う。

- **保証**：未保存の観測を書き出してから終了する。失われる観測は 0 件。
- **best-effort**：終了処理へ到達できれば書き出すが、OS が与える猶予に依存するため成立を約束しない。失われる量は §11.3 の上限に従う。
- **対象外**：終了処理が走らない。失われる量は §11.3 の上限に従う。

| 終了経路 | 区分 |
|---|---|
| `FlushLearningStore()` の明示呼び出し、モデルロード境界 | 保証 |
| `SIGINT` / `SIGTERM`、`CTRL_C_EVENT` / `CTRL_BREAK_EVENT` | 保証 |
| stdio モードで stdin が EOF になったときの正常終了 | 保証 |
| stdio モードで stdout の応答書込みに失敗したとき（broken pipe を含む）の正常終了 | 保証 |
| pipe モードで停止要求を検出したあとの正常終了 | 保証 |
| コンソールウィンドウの `×`（`CTRL_CLOSE_EVENT`） | best-effort |
| ログオフとシステムシャットダウン（`CTRL_LOGOFF_EVENT` / `CTRL_SHUTDOWN_EVENT`、およびセッション終了に伴うプロセス終了） | 対象外 |
| `TerminateProcess` 相当（タスクマネージャの強制終了、外部からの kill、クラッシュ） | 対象外 |

保存には `WriteTextFileAtomically` を使い、Windows では flush 用ファイルのオープンと
原子的置換で生じた一時的な共有違反・ロック違反を、各操作につき最大 500 ms、10 ms 間隔で再試行する。
再試行するのは `FlushLearningStore()` の明示呼び出しと Host の終了時 flush である。
観測の確定を契機とする flush、定期 flush、モデルロード前の flush は、候補照会と同じ Host 内の状態ロックを
保持したまま走るため再試行しない。1 回の試行で失敗したら dirty 状態のまま次の flush へ持ち越す。
置換では `ERROR_ACCESS_DENIED` も再試行対象に含める。読取ハンドルが削除共有を許可しない場合にも
このエラーが返るためであり、アクセス権を変更して回避することはしない。
期限までに成功しなければ一時ファイルの除去を試み、置換前の保存内容を維持して失敗を返す。
一時ファイル自体の削除も拒否された場合は残置しうる。
DPAPI の失敗や移行元の不整合を再試行で無視したり、平文へ fallback したりしない。

Host の終了時 flush は初回の保存に加え、失敗時に 50 ms 間隔で 2 回再試行する。
再試行を尽くしても保存できない場合は dirty 状態を維持し、stderr に失敗を記録する。
Windows Host の stderr が pipe の場合、`std::cerr` に出す診断は上限付き queue から専用 thread が書き込む。
Host が登録する llama のログ callback も同じ経路を使う。
読取側が止まっていても `std::cerr` の配送待ちによって main と学習 worker を止めず、
終了時の取消は診断 thread の書込みに限定し、
学習ファイル I/O を取り消さない。改行までの1行を組み立ててから queue に入れ、
queue と組立中の行はそれぞれ 64 KiB を上限とする。残容量に収まらない行は行全体を破棄し、
1行の上限を超えた場合は次の改行まで破棄する。終了時の未完了行と、100 ms の排出猶予を
超えて残った診断も破棄しうるため、stderr への配送は best-effort とする。
外部ライブラリが C stdio で stderr に直接書く診断は、この queue と取消の対象外とする。
上表の保証は書込み可能な保存先を前提とし、恒久的なアクセス拒否やディスク障害による保存失敗まで
成功扱いにはしない。失敗診断は固定の `stage`、エラーの `category` と数値 `code` に限定し、
入力本文、暗号文、ファイルパス、OS のエラーメッセージを含めない。

`CTRL_CLOSE_EVENT` を保証に含めないのは、ハンドラへ与えられる猶予が有限で、しかも環境に依存するためである。
ConPTY を使う Windows Terminal では classic `conhost.exe` より短くなりうる。
Windows 11 build 22621 の実測では、classic `conhost.exe` の `×` で最終 flush が成立し、Windows Terminal の `×` では成立しなかった（DEV-791）。
`SetConsoleCtrlHandler` によるハンドラ登録は DEV-178 で入っており、登録漏れではない。
保証をこの経路の成立に賭けない。

ログオフとシャットダウンを対象外に置くのは、現行の実装がこれらのイベントで停止処理を行わないためである。
`inference-host/src/main.cpp` のコンソール制御ハンドラは `CTRL_C_EVENT` と `CTRL_BREAK_EVENT` と `CTRL_CLOSE_EVENT` だけを処理し、残りを既定ハンドラへ渡す。

製品構成の終了も対象外に入る。
`scripts/host-supervisor.ps1` は Host をウィンドウ非表示で起動し、停止要求を受けても稼働中の Host を落とさずに監督だけをやめる。
Host はログオンセッションの終了とともに終了する。
したがって製品構成で学習データを守るのは、終了時の書き出しではなく §11.3 の損失上限である。

### 11.2 失う量を減らす手段

現行の debounce では、静止した状態から 1 件だけ確定して即座に終了すると、その 1 件が丸ごと失われる。
終了経路をどれだけ網羅しても対象外の経路は救えないため、保存の契機を前へ倒す。

**burst 先頭の同期 flush** を導入する。

- 契機：未保存の観測が 0 件の状態で新しい観測を受けたとき。
- 条件：直近の保存成功から `learning_flush_interval_sec` 秒以上が経過していること。
- 動作：その観測を含めて `Save()` を同期実行し、`CommitObservation` の応答を返す前に永続化を終える。
- レート制限：直近の保存から `learning_flush_interval_sec` 秒未満なら即時保存せず、既存の件数契機と時間契機に委ねる。

レート制限を置かないと、保存のたびに未保存件数が 0 へ戻るため次の 1 件が常に burst 先頭になり、確定ごとの同期保存に退化する。
これは DEV-11 が性能上の理由で退けた形である。
レート制限のもとでは追加の書き込みは 1 つの burst につき最大 1 回であり、時間契機が同じ burst に対して行う書き込みと同じ回数に収まる。

判定には既存の `learning_flush_interval_sec` を使い、新しい設定キーを追加しない。
経過時間の判定は直近の保存成功時刻を steady clock で保持して行う。
起動直後は保存の実績が無いため、最初の観測を即時保存の対象とする。
DEV-791 が再現させた「Host を起動し 1 件だけ確定して閉じる」操作は、この規定によって終了経路によらず保存される。

通常の観測時に `Save()` が失敗したときの扱いは変えない。
dirty と未保存件数を保ったまま stderr へ error を出し、次の観測または明示 flush で再試行する（`docs/user-learning-enhancement-spec.md` §3.1.1）。
即時保存の失敗を理由に `CommitObservation` を失敗させない。
Host の破棄時に flush が失敗した場合は 50 ms 間隔で最大 2 回再試行する。
再試行も失敗した場合は error を出したまま終了し、保存成功として扱わない。

### 11.3 失われうる最大量

`EngineConfig` の `learning_flush_every_n` と `learning_flush_interval_sec` の既定値（8 と 5）は
`docs/user-learning-enhancement-spec.md` §3.1.1 が正典である。どちらも `settings.json` には露出せず、
ビルド時の既定値として持つ。

この既定値のもとで、best-effort の経路と対象外の経路で失われうる観測は次の範囲に収まる。

- 件数：直近の保存以降に受けた観測のうち最大 7 件（8 件目で件数契機の保存が走るため）。
- 時間：未保存の観測列の先頭から最大 5 秒ぶん。
- 静止状態から再開した最初の 1 件は、§11.2 の即時保存により失われない。

ユーザーへ見せる表現としては「保存を保証しない終了では、直前 5 秒以内に連続して行った確定のうち最大 7 件ぶんの学習が失われうる」と書ける。

### 11.4 DEV-791 が実装する範囲

- §11.2 の burst 先頭の同期 flush を `InferenceEngine::NoteLearningMutationLocked` に実装する。設定キーは追加しない。
- `CTRL_CLOSE_EVENT` の経路は現行のまま best-effort として維持する。ConPTY で最終 flush が成立しないことは、本節のもとでは欠陥ではない。原因切り分けのための診断ログ追加は任意とする。
- `host_engine_tests` に次の 2 件を追加する。静止後の 1 件が明示 flush なしで永続化されること。`learning_flush_interval_sec` 未満の間隔で続く観測が即時保存を繰り返さないこと。

### 11.5 実機検証との対応

DEV-759（人間ゲート）の検証項目は本節の区分に従う。
保証の経路は保持を必須とし、best-effort と対象外の経路は §11.3 の上限内であれば未達として記録しない。
実行手順と判定の区分は `docs/handoff/human-gate-batch-runbook.md`「Windows CLI / コンソールの別枠」に置き、記録項目は人間ゲートの課題本文が持つ。

## 12. CommitObservation の配送保証

本節は `docs/windows-tsf-host-architecture.md` の IPC メッセージ一覧から `CommitObservation` の
配送保証と冪等化の正典として参照される。

§11 は Host の中で観測が永続化されるまでの境界を定める。
本節はその手前、TIP から Host へ観測が届くまでの境界を定める。

### 12.1 配送保証の水準

`CommitObservation` の配送保証は **at-least-once** とする。
TIP は応答を待つ送信としてこれを扱い、応答を受け取れなかった観測を再送する。
Host は同じ観測を二度適用しないための冪等化を持つ。

exactly-once は採らない。
送信済みで応答を受け取れなかった観測について、TIP は Host が適用したかどうかを知る手段を持たない。
その状態で再送しない設計は確定 1 件ぶんの学習をまるごと失い、再送する設計は最悪でも重みの二重計上に留まる。
学習は頻度と時間減衰による重み付けであり、1 件の二重計上は候補順序を大きく動かさない。

### 12.2 冪等化キー

`CommitObservationRequest` は `observation_id` を持つ。
定義は `ipc/include/azookey/ipc/Payloads.h` にある。

- TIP は確定ごとに 1 つの `observation_id` を生成し、その観測を再送する間は同じ値を使う。
- 値は TIP インスタンス ID（Handshake の `client_id` と同じ値）で名前空間を切り、インスタンス内の連番を続ける。同一 Host に複数の TIP インスタンスが接続しても衝突しない。
- Envelope の `request_id` は冪等化キーに使わない。TIP は接続ごとに `request_id` を振り直すため、再接続をまたいで同じ観測を指せない。
- TIP インスタンス ID を生成できなかった場合、TIP は `observation_id` を空にする。衝突しうるキーを送るより、冪等化を諦めるほうが安全である。

`observation_id` を持たない payload は、本フィールド以前の TIP からの要求として空文字で受理する。
空の `observation_id` は冪等化の対象にせず、Host はそのまま適用する。

### 12.3 Host 側の冪等化

Host は適用済みの `observation_id` を上限つきの ring で保持し、既に適用した ID の要求を学習ストアにも converter にも渡さない。
重複と判定した要求にも `ok=true` を返す。TIP に再送をやめさせるのが目的であり、失敗として返すと再送が続く。

ring は永続化しない。
`Save()` の後、応答を返す前に Host が落ちた場合、再接続後の再送は新しい観測として適用され、その 1 件が二重計上される。
この窓を塞ぐには適用済み ID を学習データと同じ原子性で永続化する必要があり、§12.1 の判断のもとでは割に合わない。

### 12.4 TIP 側の再送

TIP は送信に失敗した観測と、送信したが応答を受け取れなかった観測を、確定の順序を保ったまま送信キューへ戻し、再接続後の接続で再送する。
応答を待たない `Cancel` は戻さない。切断とともに消えた `request_id` を指しているためである。

再送には 2 つの上限を置く。

- **キュー長の上限**：Host の停止が続いても TIP のメモリが際限なく伸びないようにする。上限を超えたぶんは古い側から捨てる。
- **1 件あたりの試行回数の上限**：Host が必ず失敗させる観測が 1 件あっても、`QueryCandidates` が飢えないようにする。送信キューは各周回で候補要求より先に処理されるため、この上限が無いと再送ループが変換そのものを止める。

キュー長の上限は、送信キューを伸ばすすべての経路で適用する。
Host が到達不能な間、worker は接続の再試行と backoff を繰り返すだけで送信キューを処理する経路へ入らない。
再送のときだけ切り詰める実装では、上限が守るべき停止区間においてキューが伸び続けるため、上限を置いた意味が無くなる。

いずれの上限も、上限に達して観測を捨てたことを runtime log の warn に残す。
試行回数の上限は `tsf-tip/src/TextService.cpp` の定数として持ち、設定キーには露出しない。

キュー長の上限は `ipc/include/azookey/ipc/Limits.h` に置き、Host と共有する。
Host 側 ring の深さは、この上限に transport が同時に受け付ける接続数の上限を掛けた値とする。
`InferenceEngine` は全接続で共有される単一インスタンスであり、Host 再起動後は複数の TIP インスタンスが同時に backlog を再送する。
ring を 1 インスタンスぶんの上限で切ると、ある TIP の適用済み ID がその TIP の再送より先に他の TIP の再送で追い出され、`observation_id` が防ぐはずの二重計上が起きる。

### 12.5 テスト

- `ipc_payloads_tests`：`observation_id` の round-trip と、フィールドを持たない payload が空文字で parse されること。
- `host_engine_tests`：同じ `observation_id` の二度目が学習ストアへ反映されないこと。空の `observation_id` が毎回適用されること。ring 深さを超えた ID が再び適用されること。他の TIP インスタンスが backlog を再送しきっても適用済み ID が追い出されないこと。
- `host_dispatcher_tests`：再送に `ok=true` を返し、かつ学習ストアを二重に更新しないこと。
- `tsf_tip_onkeydown_preedit_tests`：応答を返さずに落ちた Host の後、再接続先の Host が同じ `observation_id` を受け取ること。worker が動いていない間の enqueue でキュー長の上限が守られ、古い側から捨てられること。

## 13. ユーザー辞書・新語・lookup の CLI 契約

本節は `azookey_inference_host.exe` の `userdict` / `newwords` / `lookup` サブコマンドの契約と、
`user_dict.json` の排他・原子的な保存の正典であり、`docs/windows-tsf-host-architecture.md`「学習」と
`docs/auto-word-registration-spec.md` §7-3 から参照される。

- Debug CLI（M9 移行措置）として
  `azookey_inference_host.exe [--user-dict <path>] userdict ...` を提供する。
  `add` / `remove` は既定で起動中 Host へ IPC 送信し、未起動時や検証用の直接編集では
  `--offline` を付ける。`list` / `import <path-to-tsv>` / `export <path-to-json>` は
  永続ファイルを直接読み書きする。`list` / `export` は読み取り専用で読み込み、平文の
  `user_dict.json` の移行（`.bak` / `.enc` の作成）も、解釈できない JSON の隔離
  （`.corrupt.<stamp>`）も行わない。解釈できないファイルは `lookup` と同じく読み込み失敗
  として終了コード 1 を返す。`--dry-run` を付けた add / remove / import / export も
  読み取り専用で読み込み、辞書や export 先を変更せず、移行・隔離を行わない。
  通常の直接 writer（`--offline` の add / remove、`import`）は旧平文を `.bak` に保全して
  `.enc` へ移行する。解釈できない平文は `.corrupt.<stamp>` へ隔離し、終了コード 1 を返す。
- `userdict import` の TSV は
  `reading<TAB>surface<TAB>cid<TAB>mid<TAB>weight` の 5 列（後ろ 3 列は空可）を受け付ける。
  不正行は skip 件数として報告し、同一 `(surface, reading)` は既存 `Add` と同じく後勝ちで
  置換する。`userdict export` は既存 JSON schema
  `{ "version": 1, "entries": [...] }` で書き出す。
- 新語候補（M36-A）の承認には
  `azookey_inference_host.exe [--learning <path>] newwords <list|confirm|reject> ...` を使う。
  `confirm` / `reject` は `userdict add` と同じく既定で起動中 Host へ IPC 送信し、
  直接編集は `--offline` を付けたときだけ行う。構文と出力は
  `docs/auto-word-registration-spec.md` §7-3 に従う。
- `userdict` / `newwords` の IPC 送信は、Host の pipe が無ければ約 100 ms で接続失敗とする。
  pipe はあるが空いた待ち受けが無い（Host が 1 本ずつ受け付けている途中の）ときは、
  呼び出しから合計 2 秒まで接続を待つ。待っている間に pipe が無くなれば、そこから約 100 ms で
  失敗する。複数の CLI を同時に起動しても、Host が稼働していれば順に接続できる。
- 接続後、handshake の応答は 2 秒まで待つ。Host の handshake は状態のロックを取らないので、
  ユーザー辞書の保存がファイルロックを待っていても遅れない。`userdict add` / `remove` と `newwords confirm` / `reject` の
  応答は、Host がその保存に使う上限（待ちの予算 5 秒と、flush・置換それぞれの一時競合の再試行予算）に
  1 秒を足した時間（7 秒）まで待つ。busy 接続と handshake の待ちを合わせると、失敗を表示するまで最悪約 11 秒かかる。
- Host の `AddUserWord` / `RemoveUserWord` は、要求の受信から数えて 5 秒を待ちの予算とする。予算は、同じ Host の
  他のユーザー辞書の編集の終わりを待つ時間と、ユーザー辞書のファイルロックを待つ時間を合わせて数える。
  予算を使い切ったとき、またはロックを取り読み直して保存を始める前の時点で予算を過ぎていたときは、保存せずに `ok=false` を返し、
  Health の `last_error` に `user dictionary busy` を残し、応答の `error` を `busy` にする。保存を始めた後は打ち切らない。
- CLI は Host の拒否を `running host rejected add: <error> (<説明>)`（`remove`）と表示する。
  `busy` の説明は、何も保存していないことと再試行を促す。`lock_failed` / `load_failed` は何も保存していないこと、`save_failed` は
  辞書を変えていないこと、`not_found` は語が辞書に無いこと、`unavailable` は Host がユーザー辞書を持たないことを示す。
  `invalid_request` と `not_authenticated` にも説明を添える。CLI の知らないコードは表示せず、
  `running host rejected add (unrecognized reason)` とする。`error` を持たない古い Host の応答では
  `running host rejected add`（`remove`）とだけ表示する。
  JSON 行の `error` はこの表示文で、`reason` に Host のコードを入れる（空白・制御文字・非 ASCII を除き 64 バイトまで。旧 Host の応答では省く）。
  `newwords confirm` / `reject` は、稼働中 Host が拒否したとき `ResolveNewWordResponse` の `error` のコードを
  JSON 行の `error` にそのまま載せる。`userdict` では同じ役割を `reason` が持つ。
- Host はファイルロックを取った後、ディスクのユーザー辞書を読み直してから編集する。暗号化ファイルの大きさ・最終書込み時刻・ファイル識別子（Windows では 128bit のファイル ID とボリュームのシリアル番号、Linux では inode）が、この Host が前回読み書きしたときから変わらず、
  その後メモリ上の辞書を変えていなければ、読み直しを省く（`load_skipped`）。他の writer は一時ファイルからの置換で書くので、識別子が変わる。
- したがって `userdict add` / `remove` で CLI が応答待ちを諦めた後に Host の編集が成功するのは、要求が Host に届くまでの時間と、
  読み直しと保存（DPAPI と flush・置換）に合わせて 2 秒より長くかかったときだけである。新語の確定・却下には待ちの予算が無い。保存の中では暗号化ファイル自体のロックも最大 5 秒待つ。待たされるのは外側のロックを取らない古いプロセスと競合したときだけだが、この待ちは CLI の 7 秒の計算に入っていない。
  CLI の時間切れの表示は、変更が後から成功しうることと、`userdict list`（`newwords list`）で確かめることを示す。
- Host は、ユーザー辞書の追加・削除と新語の確定・却下を 1 件ずつ RuntimeLogger のイベント `persistent_edit` に記録する。
  項目は `trace_id`、`request_id`、`op`（`user_dict_add` / `user_dict_remove` / `new_word_resolve`）、`result`、
  同じ Host の編集を待った時間 `writer_wait_ms`、ファイルロックを待った時間 `lock_wait_ms`、`load_ms`、`load_skipped`、
  `save_ms` と、受信からの経過時間 `total_ms` である。受信時刻はレコードの時刻から `total_ms` を引いて求める。
  単語と読みは記録しない。新語の確定・却下はファイルロックを取らないので、待ちと保存を合わせて `save_ms` に入れる。
- M11 / M30 の設定アプリ完成後も、`userdict` サブコマンドは v1.x の診断・移行用
  CLI として併存させる。GUI が通常操作面になった後も、CI やサポート手順から再現できる
  低レベル操作面として削除しない。
- 辞書と学習履歴の診断には、Host を起動しない読み取り専用 CLI を使う。
  構文は `azookey_inference_host.exe [--learning <path>] [--user-dict <path>] lookup`
  `--mode <exact|prefix|surface> --query <text> [--format <json|tsv>]` とする。
  `exact` と `prefix` は reading を、`surface` は表層形を比較する。
  比較は UTF-8 バイト列の完全一致または前方一致とし、正規化しない。
  `--query` は空文字列を許可しない。
  CLI 引数の文字コードは `docs/windows-tsf-host-architecture.md`「Host CLI 引数」の契約に従う。
  JSON は一致ごとに `op`、`ok`、`mode`、`query`、`source`、`reading`、`surface` と
  source 固有の値を 1 オブジェクトずつ出力する。
  JSON オブジェクトのキー順は規定しない。
  `user_dict` は存在する `cid`、`mid`、`weight` を、`learning` は `weight` と
  `last_updated_epoch_sec` を source 固有の値として出力する。
  該当なしの場合は `count: 0` の JSON を 1 行出力し、TSV では何も出力しない。
  TSV の列は `source`、`reading`、`surface`、`cid`、`mid`、`weight`、
  `last_updated_epoch_sec` の順とする。
  結果は reading、surface、source の昇順で固定する。
  この CLI はディレクトリ作成、legacy file migration、quarantine、保存を行わない。
  `user_dict.json` の読み取りは `AcquireExclusiveFileLockForPath` の保持中に行い、
  ロックを取得できない場合は exit code 1 とする。
  どちらかの入力ファイルを読み込めない場合は部分結果を返さず、exit code 1 とする。
  したがって破損ファイルの調査でも入力ファイルを変更しない。
- `user_dict.json` の直接編集経路は、共有ファイル単位の排他ロック
  (`learning/include/azookey/learning/FileLock.h`) を取得してから
  read-modify-write を行う。Host の `AddUserWord` / `RemoveUserWord` も同じロック下で、
  ディスク上の辞書ファイルが存在する場合は再読込してから更新し、Host メモリ上の stale な辞書で
  CLI / Settings 側の変更を上書きしない。
- 保存時は一時ファイルへ書き込んでから replace し、書き込み中クラッシュによる
  既存ファイル破損を避ける（`learning/src/AtomicFile.h`）。一時ファイルは
  `FlushFileBuffers` / `fsync` で flush 後、`MoveFileExW(MOVEFILE_REPLACE_EXISTING |
  MOVEFILE_WRITE_THROUGH)` で原子的に置換し、置換途中のクラッシュでも既存ファイルを
  無傷に保つ。

## 14. 共有ユーザーデータの writer 責務

本節は `docs/windows-tsf-host-architecture.md`「設定（SettingsStore）」「共有ユーザーデータの writer 責務」、
`docs/sideload-packaging-spec.md` §3.3・§3.7、`docs/bracket-pairing-spec.md`、
`docs/handoff/human-gate-batch-runbook.md`、`plans/windows-port-roadmap.md` M11 から参照される。
対象には学習データ以外の共有ファイル（`settings.json`、`host_run_state.txt`）も含む。
writer を 1 か所で列挙しないと、ロックを取らない書き手を見落とすためである。


`%LOCALAPPDATA%\azooKey\` 配下の共有ファイルは、直接書き込めるプロセスをファイルごとに固定する。
`FileLock.h` の排他は、書き手全員がロックを取ることを前提にしているため、誰が writer になり得るかを決めていないと、取り忘れた側の上書きが last-writer-wins で通ってしまう。
writer には内容を書き換える操作だけでなく、対象ファイルを rename / 削除する操作も含める。

| ファイル | 直接書き込むプロセス | 読み取り | 排他 |
|---|---|---|---|
| `user_dict.json.enc` | Host（`AddUserWord` / `RemoveUserWord`）、`userdict` CLI（`--offline` の add / remove、`import`） | Host、`userdict` CLI（`list` / `export`）、`lookup` CLI | `AcquireExclusiveFileLockForPath` + atomic replace |
| `settings.json` | 設定アプリ（保存、保存前の parse 失敗時の quarantine rename）、Host（parse 失敗時の quarantine rename、SafeMode 突入時の `safeMode` 記録。`docs/dev-infrastructure-spec.md` §8.5.3）、TIP（入力インジケーターのメニューによる `liveConversion` / `predictionEnabled` の 2 キーだけ。`docs/tsf-deep-integration-spec.md` §4.5） | Host（`SettingsStore::Load` / `Reload`） | `AcquireExclusiveFileLockForPath` + atomic replace（保存側）／同一ロック区間内の read → parse → rename（設定アプリと Host。下記） |
| `learning.v2.tsv.enc`（v2 移行前は `learning.tsv.enc` を読むだけ） | Host のみ | Host、`lookup` CLI | Host 内で直列化（debounce flush、`docs/user-learning-enhancement-spec.md` §3.1.1）し、保存時に `AcquireExclusiveFileLockForPath` + atomic replace |
| `english_learning.v2.tsv.enc`（v2 移行前は `english_learning.tsv.enc` を読むだけ） | Host のみ（M60 の英単語確定。`docs/inline-english-candidate-spec.md` §5） | Host | Host 内で直列化し、確定のたびに `AcquireExclusiveFileLockForPath` + atomic replace。失敗時は終了時に再試行する |
| `data\host_run_state.txt` | pipe モードの Host のみ（起動時の実行中の印、正常終了時の消去。`docs/dev-infrastructure-spec.md` §8.5.3） | 同じ Host（次の起動時） | `AcquireExclusiveFileLockForPath` + atomic replace。印を書いた Host が生きている間は重複起動側が触れず、終了時は自分の印だけを消す |
| `auto_words.tsv.enc` | Host（マイニング、`ResolveNewWord`、起動時の `PrunePending`）、`newwords` CLI（`--offline` の confirm / reject） | Host、`newwords` CLI（`list`） | `AcquireExclusiveFileLockForPath` + atomic replace。`--offline` は Host 停止中に限る（`docs/auto-word-registration-spec.md` §7-3） |

拡張子 `.enc` のない旧平文ファイルは移行元であり、移行時は `.bak` を保全する。

- 設定アプリは `user_dict.json.enc` を直接開かない。v1.0 の学習ペインのユーザー辞書タブ（`docs/sideload-packaging-spec.md` §3.7）も M30 / M49 の辞書 GUI も、Host への IPC（`AddUserWord` / `RemoveUserWord` と §4 のストア操作）を経由する。
  この制約は版によらない。辞書 GUI が完成しても、設定アプリは `user_dict.json.enc` の直接 writer にはならない。
- したがって `user_dict.json.enc` に対する独立した直接 writer は Host と `userdict` CLI の二つであり、「Host と設定アプリ」という組み合わせは設計上存在しない。
  プロセス間ロックの実機確認は、稼働中 Host への IPC 経由 `userdict add` と、別プロセスの `userdict add --offline` を重ねて行う（Human Gate は DEV-758、手順は `docs/handoff/human-gate-batch-runbook.md`）。
- `userdict export` は読み出した内容を引数のパスへ書くだけで、`user_dict.json.enc` 自体は変更しない。`user_dict.json.enc` に対する writer 操作は `--offline` の add / remove と `import` である。
- `settings.json` を保存するのは設定アプリと、SafeMode 記録・ベンチマーク履歴追記を行う Host と、入力インジケーターのメニューでライブ変換・予測を切り替える TIP である。TIP も Host と同じく、同じファイルロックの下で対象の 1 キーだけを合成して atomic replace し、読めない・解釈できないファイルは書き換えず隔離もしない。ロックが取れなければ書かない。Host は同じファイルロックの下でディスク上の内容へ対象キーだけを合成して atomic replace し、読めない・解釈できないファイルは書き換えない。設定アプリの保存は `safeMode` と `model.benchmarkHistory` を保つ。履歴の形式・保持規則は `model-management-spec.md` §7.1 に従う。
  どちらも mutator である。
  設定アプリは保存前の read-modify-write で JSON の parse に失敗したとき、Host は `SettingsStore::Load` / `Reload` で parse に失敗したときに、`settings.json` を `.invalid*` へ rename する。
  ロックを取らずに読むと、Host が破損した内容を読んでから rename するまでの間に設定アプリが atomic replace で正常なファイルを置いたとき、Host はその新しいファイルを quarantine し、保存した設定が消える。
  したがって「設定アプリだけが書くので競合しない」とは扱わない。この競合は、保存と Host の read から quarantine までを同一の named mutex 下で直列化し、quarantine の条件を内容の不正だけに絞ることで解消する（DEV-806 で確定。実装は DEV-808、GitHub Issue #278 はその mirror）。契約は次の 4 点である。

  1. **単一ロック区間** — Host は `settings.json` の存在確認・読み取り・parse・quarantine rename を、`AcquireExclusiveFileLockForPath`（`FileLock.h`）で取得した一つのロック保持区間の中で行う。設定アプリの保存が取るのと同じ named mutex（`MutexNameForPath` が正規化した絶対パスから導出）である。
     読み取りと rename を別々のロック区間に分けたり、ロック外で読んでから再検証して rename する（double-check）形は採らない。ロック保持区間は数 KB の JSON の読み取りと parse で閉じるため、保存側が待たされる時間は問題にならない。
  2. **quarantine は parse 失敗時だけ** — 設定アプリと Host は、ファイルを開けなかったときや読み取りに失敗したときは rename しない。
     open の失敗は破損を意味しない（他プロセスの共有違反、ウイルス対策ソフトやバックアップによる一時的なロックでも起きる）。読み取り失敗で rename すると、破損していない `settings.json` を退避してしまう。
  3. **ロックを取れなければ rename しない** — timeout までにロックを取得できなかったときは quarantine を行わない。ロックを持たない rename は禁止する。
  4. **fallback の意味** — quarantine の有無によらず、`Load`（Host 起動時）は既定値で継続し、`Reload`（`UpdateConfig`）は error を返して現在の runtime 設定を維持する。「維持する」は `SettingsStore::settings()` の値を含む（`EngineConfig` だけではない）。
- quarantine を廃して破損した `settings.json` をそのまま残す案は採らない。rename には、次回の保存で正常なファイルが復帰し、破損した内容は `.invalid*` として残り、ユーザーが手で消さなくてよいという利点がある。
  残す案でも `azookey_diag` の D-012 は不正を検出できるが、破損したファイルが残る限り Host は既定値で動き続け、復帰には設定アプリでの保存か手動削除が要る。上の 4 点は、この利点を保ったまま、正常なファイルを消す 2 つの経路（読み取り失敗での rename と、read から rename までの競合）だけを塞ぐ。
- 設定アプリの保存経路が満たす契約は次の 3 点である。一時ファイルへ書いて flush してから原子的に置換し、途中経過を `settings.json` として残さないこと。`user_dict.json` と同じく、正規化した絶対パスから導出した named mutex で read-modify-write 全体を排他すること。
  `WriteTextFileAtomically` は `learning/include/azookey/learning/AtomicFile.h` の公開 header とし、設定アプリと Host 側コンポーネントが同じ atomic replace 実装を使う。設定アプリは公開 include path を参照し、`settings.json` の実パスに対する共有ロックを取得してから helper を呼ぶ。
  保存前の parse に失敗した場合は、同じロック区間内で破損ファイルを `.invalid*` へ退避してから新しい設定を書き、退避したことを設定 UI に警告として表示する。退避に失敗した場合は保存せず、元のファイルを残す。
- `learning.tsv` の writer は Host だけで、supervisor が per-user mutex で Host を単一化する（`docs/windows-tsf-host-architecture.md`「Host の起動と再起動」）。
  ファイル単位ロックを取らないため、同一ユーザーで Host を二重に起動した状態は writer が二つある状態であり、想定しない。
- named mutex 名は正規化した絶対パスから導出する（`FileLock.h` の `MutexNameForPath`）。
  二つの writer が排他されるのは同じ実パスを解決したときに限るため、`--user-dict` で別パスを与えた検証はロックの確認にならない。
