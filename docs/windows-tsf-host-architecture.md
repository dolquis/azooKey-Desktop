# Windows TSF TIP + Inference Host 分離設計

## 構成

- `tsf-tip/`: in-proc COM DLL。キー処理、Composition、候補UI制御、EditSession要求のみ担当。
- `inference-host/`: per-user常駐EXE。モデル常駐、候補生成、学習再ランキング、CPU/CUDA切替。
- `ipc/`: Named Pipe向けのバージョン付きメッセージ定義（現実装はJSON + length-prefix）。
- `learning/`: 頻度 + 時間減衰の再ランキング永続化。

## なぜ分離するか

- TIPはアプリ内で動くため、GPU初期化や巨大モデルロードを持つとアプリ巻き込みクラッシュの危険が高い。
- Host分離で、推論クラッシュはHost側に閉じ込め、TIPは再接続できる。

## Host の起動と再起動

TIP は Host の起動と再起動を担当しない。
TIP は任意のアプリプロセスへ読み込まれるため、プロセス生成を行うと、複数の TIP インスタンスによる起動競合やアプリ終了への巻き込みが起きる。
TIP の IPC worker は per-user pipe へ無期限に再接続し、Host のプロセス管理は TIP の外側へ置く。

開発登録では、`scripts/register-dev.ps1` が実行ユーザーの HKCU `Run` に `scripts/host-supervisor.ps1` を登録し、現在のセッションでも同じ supervisor を起動する。
supervisor はユーザー SID ごとの mutex で単一化し、既存の per-user pipe が消えた後に Host を起動する。
Host が終了した場合は、上限付きの指数バックオフで再起動する。
`scripts/unregister-dev.ps1` は named event で supervisor を停止するが、実行中の Host は強制終了しない。

HKCU `Run` はスクリプトを実行したユーザーだけを provision する。
別の Windows アカウントで TIP を使う場合は、そのアカウントでも開発登録の per-user 手順を実行する必要がある。
配布パッケージも各ユーザーのログオン時に supervisor を起動する経路を用意し、machine-wide の TIP 登録だけで Host が利用可能になるとは扱わない。

### Host CLI 引数

Host の起動引数は `ParseHostArgs` が一括して解析する。
`--backend` は `cpu` または `cuda` だけを受け付け、不正値を exit code 2 の起動エラーにする。
値を取る option が末尾にある場合と未知の引数も exit code 2 とし、暗黙の既定値へ戻さない。
`--pipe` は省略可能な次トークンを pipe 名として扱うが、`--` で始まるトークンは別 option として残す。
`--pipe-name` は値を必須とする。
`--model`、`--learning`、`--user-dict`、`--mock-dict`、`--handshake-token` も値を必須とする。
`userdict` より後ろのトークンは user dictionary CLI へ、`lookup` より後ろのトークンは
読み取り専用 lookup CLI へそのまま渡す。

## TSF EditSessionルール

- テキスト更新は必ず `RequestEditSession` を経由。
- 非同期推論結果到着後、UIスレッドでEditSessionを再要求し、最新 `request_id` のみ反映。
- 古い `request_id` は破棄（ライブ変換での逆転防止）。
  - 実装: `TextService::IpcWorkerThread` で `ipc_pending_id_` と受信 `req_id` を
    比較する staleness check。
  - Handshake の `host_generation_id` が既知の値から変化した場合は request ID を更新し、
    旧 Host 世代の in-flight 応答を stale 化する。pending 要求と候補表示待機状態は維持して
    新世代へ再送し、旧候補キャッシュだけを破棄する。Handshake と応答処理は同一 IPC worker
    で直列化されるため、request ID の不一致が旧世代応答の実効的な破棄経路になる。
- 確定時は `shown_candidates_` をスナップショットし、in-flight `QueryCandidates`
  に `Cancel` を送ってから EditSession を要求する。
  Cancel は同じ Named Pipe への短命な control 接続を Handshake 済みにして送る。
  これにより、primary 接続が `QueryCandidates` 応答待ちで塞がっていても Host 側の
  共有 `RequestScheduler` に `target_request_id` が届く。TIP インスタンスは primary / control
  接続の Handshake に同じ `client_id` を付け、Host は `(client_id, request_id)` 単位で
  Cancel 状態を管理する。Host は非空の `client_id` ごとに接続数を追跡し、最後の接続が閉じた
  時点でその状態を破棄する。別アプリの TIP が同じ `request_id` を使っても相互にキャンセルしない。
  control 接続が使えない場合は primary 接続への best-effort 送信に戻り、到着した古い
  応答は staleness check で破棄する。
- 確定時の EditSession は同期要求で実行し、`SetText` / `EndComposition` が完了した
  場合だけ preedit を clear して `CommitObservation` を送る。
- EditSession が拒否または失敗したとき (`hr_session != S_OK`) は preedit と
  queued commit (`committing_` / `commit_surface_`) を保持し、確定済み観測も送らない。
- queued commit が残っている間、後続の通常キー入力は先頭で元の context に対する
  commit を再試行し、成功・再拒否のどちらでもそのキーを消費して stale preedit や
  確定直後のテキストと新規入力が混ざらないようにする。

## IPCメッセージ（実装済み = ✅）

各メッセージの payload フィールドの正典スキーマは `ipc/include/azookey/ipc/Payloads.h`
（および `ipc/src/Payloads.cpp` の serialize/deserialize）を参照。以下は現状の配線済み
フィールドの要約。

- ✅ `Handshake` — 要求 `(tip_version, protocol_version, capabilities, client_id?, handshake_token?)` /
  応答 `HandshakeResponse(host_version, protocol_version, accepted, model_loaded,
  host_generation_id?)`。`host_generation_id` は Host 起動ごとに生成し、同一プロセスの
  全接続で共有する UUID。省略する旧 Host は空文字として受理する。
  `client_id` は primary / control 接続間で Cancel 名前空間を共有するための TIP インスタンス ID、
  `handshake_token` は per-connection 認証ゲートに使う（後述）。
- ✅ `Ping` / `Health`
- ✅ `LoadModel(path, backend, n_gpu_layers?)` / `LoadModelResponse(ok, error?)`。
  Host は `ProbeZenzaiGgufModel` で GGUF を実プローブし、成功時は `ZenzaiModelConverter`
  を構築する（`InferenceEngine::LoadModelWithResult`）。CUDA backend は未リンクのため
  CPU に fallback し、`error` にその旨を入れて `ok=true` を返す。`path` 空文字時は MVP
  fallback converter を active にする。probe 失敗時は、まだモデル未ロードの初回ロードなら
  MVP fallback converter へ切り替える一方、既にモデルロード済みの再ロードでは直前にロード
  済みのモデルを active のまま維持する（`LoadModelFailureKeepsPreviouslyLoadedModel`）。
- ✅ `QueryCandidates` — 要求 `(reading, left_context, max_candidates, live)` /
  応答 `(candidates[], partial)`。各 candidate は `(surface, reading, score, source)`。
  応答前に `max_candidates` で件数を切り詰める。
- ✅ `QueryBatchConversion` — 要求 `(reading, raw_romaji, mode, auto_punctuation, max_candidates)` /
  応答 `(segments[], full_surface, partial, canceled)`。各 segment は
  `(reading, candidates[])`。`QueryCandidates` と同じく `RequestScheduler` で
  cancel / latest を追跡し、キャンセル時は `canceled=true` と空の segments を返す。
  現状の segments は 1 要素（文節分割は未実装）で、`full_surface` は先頭候補の surface。
- ✅ `Cancel(target_request_id)`
- ✅ `CommitObservation(reading, chosen, shown, left_context, timestamp_ms)`
- ✅ `AddUserWord` / `RemoveUserWord` — `InferenceEngine` の状態ロック下で
  `UserDictionary` を更新し、永続化に成功した場合だけ `ok=true` を返す。
  永続化に失敗した場合は直前の辞書状態へ戻し、`Health` の `last_error` に反映する。
- ✅ `UpdateConfig` — 要求 payload は空オブジェクト（設定内容は wire に載せない。Host が
  `SettingsStore::Reload()` で settings.json を読み直す） / 応答 `(ok, error?)`。
  再読込は `update_config_mutex` で直列化し、runtime settings を engine config へ適用してから
  モデルを再ロードする。settings.json が invalid な場合は engine を触らず `ok=false` を返す。
- ✅ `QueryDiagnostics` — 要求 payload は空オブジェクト / 応答
  `(model_loaded, loaded_model_path?, engine, backend, rss_mb, ep?, ep_state?, ep_last_error?,
  learning_entries, user_dict_entries, fallback_state, last_error?)`。
  `fallback_state` は `healthy` / `degraded_simple` / `degraded_model` のいずれか。
  送信側は診断 CLI（`diagnostics/` の `azookey_diag` ターゲット）で、
  `Diagnostics.cpp` の IPC プローブが Handshake → Ping に続けて `request_id=3` /
  `trace_id="diag-query-diagnostics"` / payload `{}` で送り、応答を `ParseQueryDiagnostics`
  して診断スナップショットへ格納する。TIP からは送らない。
- ⚠️ enum のみ定義済み、Payload/Dispatcher 未実装:
  - `QueryPredictions` `QueryCorrections` `CommitCorrection` `UpdateUserWord`
  - `InferenceEngine` 側には既に `QueryPredictions/QueryCorrections/CommitCorrection`
    関数があるため、Payload と Dispatcher ハンドラを追加すれば配線可能。
  - 認証済み Dispatcher がこれらの型または `Unknown` を受け取った場合は、request の型と
    相関 ID を保った `{"ok":false,"error":"unsupported_message_type"}` 応答を返す。
    無応答は fire-and-forget の `Cancel` に限る。

### Handshake 認証ゲート

- Windows transport は logon SID に限定した DACL、最初の pipe instance の排他取得、
  接続後の peer process の user SID と logon SID の照合を行う。
  TIP 側の pipe handle は `SecurityIdentification` に制限し、接続先 server が
  TIP を impersonate できないようにする。
- Host 設定 `handshake_token` が非空のとき、`Dispatcher::RequiresAuthenticatedSession`
  が有効化され、Handshake 成立（`protocol_version` 一致 + `handshake_token` 一致）まで
  後続メッセージを拒否する。token 未設定（空）の場合は認証不要で全メッセージを受け付ける。
- Handshake の成否は接続ごとの `authenticated_` 状態に反映され、未認証時は Health が
  `last_error="not authenticated"` を返す。
- `client_id` を送る現行 TIP は RequestScheduler の Cancel / latest 状態を TIP インスタンス単位に
  分離する。旧クライアントが `client_id` を省略した場合は空の legacy 名前空間として受理し、
  protocol version 1 の後方互換を保つ。legacy 名前空間は接続終了時の個別破棄の対象外である。

## 学習

- モデル重みは更新しない（安全性優先）。
- `learning.db` 相当の永続層へ観測を保存し、再ランキングで反映。
- 永続フォーマットは現状 TSV。未指定時は
  `%LOCALAPPDATA%\azooKey\data\learning.tsv` に保存する。
- TSV の `reading` / `surface` は保存時に
  `# azookey-learning-tsv escaped=1` ヘッダーを付け、`\` → `\\`、
  tab → `\t`、LF → `\n`、CR → `\r` としてエスケープする。ヘッダーのない
  旧ファイルは backslash を literal として扱い、既存データを壊さない。
- ユーザー辞書は未指定時 `%LOCALAPPDATA%\azooKey\data\user_dict.json` に保存する。
  `--learning` / `--user-dict` 指定時は明示パスを優先する。
- Debug CLI（M9 移行措置）として
  `azookey_inference_host.exe [--user-dict <path>] userdict ...` を提供する。
  `add` / `remove` は既定で起動中 Host へ IPC 送信し、未起動時や検証用の直接編集では
  `--offline` を付ける。`list` / `import <path-to-tsv>` / `export <path-to-json>` は
  永続ファイルを直接読み書きする。
- `userdict import` の TSV は
  `reading<TAB>surface<TAB>cid<TAB>mid<TAB>weight` の 5 列（後ろ 3 列は空可）を受け付ける。
  不正行は skip 件数として報告し、同一 `(surface, reading)` は既存 `Add` と同じく後勝ちで
  置換する。`userdict export` は既存 JSON schema
  `{ "version": 1, "entries": [...] }` で書き出す。
- M11 / M30 の設定アプリ完成後も、`userdict` サブコマンドは v1.x の診断・移行用
  CLI として併存させる。GUI が通常操作面になった後も、CI やサポート手順から再現できる
  低レベル操作面として削除しない。
- 辞書と学習履歴の診断には、Host を起動しない読み取り専用 CLI を使う。
  構文は `azookey_inference_host.exe [--learning <path>] [--user-dict <path>] lookup`
  `--mode <exact|prefix|surface> --query <text> [--format <json|tsv>]` とする。
  `exact` と `prefix` は reading を、`surface` は表層形を比較する。
  比較は UTF-8 バイト列の完全一致または前方一致とし、正規化しない。
  `--query` は空文字列を許可しない。
  CLI 引数の文字列は UTF-8 バイト列を前提とする。
  Windows の narrow argv から UTF-8 へ変換する境界はこの CLI の契約外とし、
  Unicode argv 経路は DEV-962、実機検証は DEV-963 で追跡する。
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
- 確定・訂正ごとの観測はメモリ上で即時反映し、TSV への永続化は
  `learning_flush_every_n`（既定 8 件）または `learning_flush_interval_sec`
  （既定 5 秒）の background timer でデバウンスする。Host 破棄時と `LoadModel` 境界では
  `FlushLearningStore()` で明示 flush する。
- `Save()` 失敗時は Host stderr に error を出し、dirty 状態を維持して
  次回 observation または明示 flush で再試行する。
- `learning_max_records`（既定 10000）と `learning_min_weight`（既定 0.05）で
  低スコア・減衰済みレコードを GC し、TSV の無制限増大を防ぐ。
- 破損時はリセット可能（`LearningStore::Reset` or ファイル削除）。
- 時間減衰: `exp(-0.15 * days)` で `LearningStore::Score` 内で適用。

## 設定（SettingsStore）

- 設定の正典スキーマは `settings/mvp-settings.schema.json`。`inference-host` の
  `SettingsStore` は起動時に `%LOCALAPPDATA%\azooKey\config\settings.json` を読み、
  未指定キーを schema default にフォールバックして提供する。
- `settings.json` はファイル正典とし、設定アプリからの IPC `UpdateConfig` は payload 空の
  再読込トリガとして扱う。設定オブジェクトは IPC schema へ二重定義しない。
- 推論チューニング値は `inferenceThreads`、`maxCandidates`、`maxContextLength` を使う。
  `inferenceThreads` は 0 から 8 で、0 の場合は `powerProfile` に従う。
  Host による AC またはバッテリ状態の自動判定を実装するまでは、`auto=4` を暫定値とし、
  `performance=8`、`battery_saver=2` とする。
  `maxCandidates` は 1 から 32 で既定値 9、`maxContextLength` は 0 から 30 で既定値 10 とする。
  `maxContextLength` の単位は Unicode コードポイント数であり、0 の場合は左文脈を推論へ渡さない。
  現在の TIP は候補要求へ左文脈を送らないため、左文脈の送信経路を実装するまでは
  `maxContextLength` を変更しても変換結果に影響しない。
- parse に失敗した `settings.json` は `.invalid` suffix へ隔離する。起動時は default 設定で継続し、
  `UpdateConfig` 再読込時は error を返して現在の runtime 設定を維持する。隔離の条件と、設定アプリの
  保存との排他は下記「共有ユーザーデータの writer 責務」を正典とする（`FileLock.h` の共有ファイル
  単位ロックで read から rename までを直列化する）。
- 候補ウィンドウ位置更新（`update_pos` / `OnLayoutChange` 連動）の再入対策として、先行実装の
  「更新中は layout change を一定時間抑止する状態機械」を設計参照にできる（抑止値は環境依存）。

## 共有ユーザーデータの writer 責務

`%LOCALAPPDATA%\azooKey\` 配下の共有ファイルは、直接書き込めるプロセスをファイルごとに固定する。
`FileLock.h` の排他は、書き手全員がロックを取ることを前提にしているため、誰が writer になり得るかを決めていないと、取り忘れた側の上書きが last-writer-wins で通ってしまう。
writer には内容を書き換える操作だけでなく、対象ファイルを rename / 削除する操作も含める。

| ファイル | 直接書き込むプロセス | 読み取り | 排他 |
|---|---|---|---|
| `user_dict.json` | Host（`AddUserWord` / `RemoveUserWord`）、`userdict` CLI（`--offline` の add / remove、`import`） | Host、`userdict` CLI（`list` / `export`）、`lookup` CLI | `AcquireExclusiveFileLockForPath` + atomic replace |
| `settings.json` | 設定アプリ（保存、保存前の parse 失敗時の quarantine rename）、Host（parse 失敗時の quarantine rename） | Host（`SettingsStore::Load` / `Reload`） | `AcquireExclusiveFileLockForPath` + atomic replace（保存側）／同一ロック区間内の read → parse → rename（設定アプリと Host。下記） |
| `learning.tsv` | Host のみ | Host、`lookup` CLI | Host 内で直列化（debounce flush、上記「学習」）。ファイル単位ロックは取らない |

- 設定アプリは `user_dict.json` を直接開かない。v1.0 の「ユーザー辞書を編集」は `userdict` CLI の probe を起動し（`docs/sideload-packaging-spec.md` §3.7）、M30 / M49 の辞書 GUI は Host への IPC（`AddUserWord` / `RemoveUserWord` と `docs/learning-data-management-spec.md` §4 のストア操作）を経由する。
  この制約は版によらない。辞書 GUI が完成しても、設定アプリは `user_dict.json` の直接 writer にはならない。
- したがって `user_dict.json` に対する独立した直接 writer は Host と `userdict` CLI の二つであり、「Host と設定アプリ」という組み合わせは設計上存在しない。
  プロセス間ロックの実機確認は、稼働中 Host への IPC 経由 `userdict add` と、別プロセスの `userdict add --offline` を重ねて行う（Human Gate は DEV-758、手順は `docs/handoff/human-gate-batch-runbook.md`）。
- `userdict export` は読み出した内容を引数のパスへ書くだけで、`user_dict.json` 自体は変更しない。`user_dict.json` に対する writer 操作は `--offline` の add / remove と `import` である。
- `settings.json` を保存するのは設定アプリだけだが、設定アプリと Host はどちらも mutator である。
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
- `learning.tsv` の writer は Host だけで、supervisor が per-user mutex で Host を単一化する（上記「Host の起動と再起動」）。
  ファイル単位ロックを取らないため、同一ユーザーで Host を二重に起動した状態は writer が二つある状態であり、想定しない。
- named mutex 名は正規化した絶対パスから導出する（`FileLock.h` の `MutexNameForPath`）。
  二つの writer が排他されるのは同じ実パスを解決したときに限るため、`--user-dict` で別パスを与えた検証はロックの確認にならない。

## 実装ルール

### スレッドモデル

- COM apartment: TSF text service DLL は `InProcServer32` でロードされるため、
  ホストアプリ文脈に従う。
- TSF インターフェースは UI スレッド境界で扱う。推論・重い変換をワーカースレッドへ
  逃がす際は、TSF オブジェクトへ直接触れず、結果はメッセージ／キューで UI スレッドへ
  戻して反映する（実装: `IpcWorkerThread`）。
- `AddRef`/`Release`/`QueryInterface` を厳格実装する。COM ポインタは
  `wil::com_ptr` または `Microsoft::WRL::ComPtr` を用いる。
- Host の `InferenceEngine` は `state_mutex_` の保持中に active converter の
  `shared_ptr` と設定値をスナップショットし、変換中は `state_mutex_` を解放する。
  このため Health と軽量 accessor、およびモデル差し替えは長時間変換を待たない。
  converter の mutable decode state は専用 mutex で直列化し、モデル差し替え後も
  実行中の旧 converter は `shared_ptr` によって変換終了まで生存する。

### 例外・障害耐性

- COM 境界をまたぐ関数は**例外を外へ出さない**。失敗時は `HRESULT` で返却し、
  ログに詳細を書き込む。
- 最低ログ要件: 起動／終了、例外、キーイベント要約、変換失敗理由。
  `AZOOKEY_LOG=1` のときは `%LOCALAPPDATA%\azooKey\logs\` に JSON Lines で出力する。
  Debug ビルドの TIP は `OutputDebugStringA`、Host は stderr の既存経路も維持する。

### 互換性優先の実装ルール

- Notepad / Chrome / VSCode / Office の挙動差を前提に、未処理キーは極力食わない。
- Ctrl / Alt / Win 併用のキーは、明示的に実装済みの TIP 機能でない限りアプリまたは
  OS ショートカットへ pass-through する。Shift 単独は通常のローマ字入力として扱う。
- composition 状態の不整合時は安全側（キャンセル）で復帰する。

## 新規モジュール（Phase 5/6 で追加予定）

下記モジュールは v1.0 以降の Phase で追加予定。正典仕様は各 spec を参照。

### Phase 5（レガシー parity、`docs/legacy-parity-spec.md`）

- `core/include/azookey/core/UserAction.h` — VK → 抽象アクション enum
- `core/include/azookey/core/InputState.h` — 入力状態機械
- `core/src/UserActionMap.cpp` — VK → UserAction 変換テーブル
- `core/src/CustomRomajiLoader.cpp` — TSV パース + ホットリロード
- `core/src/UnicodeInputBuffer.cpp` — Ctrl+Shift+U hex バッファ
- `tsf-tip/src/PredictionWindow.cpp` — 予測候補 HWND
- `tsf-tip/src/PromptDialog.cpp` — Magic Conversion プロンプト
- `tsf-tip/src/DebugWindow.cpp` — F10 デバッグウィンドウ
- `tsf-tip/src/CaretRectResolver.cpp` — `GetTextExt` / `GetGUIThreadInfo` 3 段
  フォールバック
- `inference-host/src/AiBackend.cpp` — OpenAI 互換 API クライアント

### Phase 5〜6 横断（`docs/rich-features-spec.md`）

- `tsf-tip/src/ContextTracker.cpp` — 段落 / 直前文 / アプリ別履歴
- `tsf-tip/src/ForegroundAppDetector.cpp` — 前面アプリ実行ファイル名取得
- `tsf-tip/src/TypingTempoTracker.h` — タイピング速度推定
- `core/src/QwertyAdjacency.cpp` — FuzzyMatch 用 QWERTY 隣接表
- `bench/rich_features_bench.cpp` — リッチ化機能のレイテンシ計測

### Phase 6-A（TSF 深部、`docs/tsf-deep-integration-spec.md`）

- `tsf-tip/src/ReconversionFunction.cpp` — `ITfFnReconversion`
- `tsf-tip/src/CandidateListUIElement.cpp` — UI-less Mode 用
- `tsf-tip/src/PredictionListUIElement.cpp` — Suggestion UI 用
- `tsf-tip/src/TextService.cpp` — `ITfFnConfigure` / `ITfFunction`
- `tsf-tip/src/SettingsLauncher.cpp` — 設定アプリ EXE 解決・起動

### Phase 6-B（Copilot+ PC / NPU、`docs/copilot-pc-backend-spec.md`）

- `inference-host/src/BackendSelector.cpp` — DXCore 列挙 + 優先順
- `inference-host/src/MmapModelLoader.cpp` — `CreateFileMapping` + `MapViewOfFile`
- `inference-host/src/PowerStatusWatcher.cpp` — AC/バッテリ検知
- `inference-host/src/DirectMlBackend.cpp` — DirectML EP 経路
- `inference-host/src/QnnBackend.cpp` — Snapdragon X NPU 経路（M27 と合流）

### Phase 6-C（UI モダン化、`docs/native-ui-spec.md`）

- `tsf-tip/src/ThemeColors.h` — Light/Dark 色テーブル
- `tsf-tip/src/RenderingEngine.cpp` — DComp + D2D + DirectWrite 共通

### Phase 7（サイドロード配信、`docs/sideload-packaging-spec.md`）

- `settings-app/` — C++/WinRT WinUI 3 設定アプリ（**最小版は M11 / v1.0** で導入し
  settings.json 書き込み + `UpdateConfig` 再読込まで含む。フル UI は M30。roadmap M11 / M30、
  `docs/sideload-packaging-spec.md` §3.0）
- `core/src/EtwLogger.cpp` — ETW Provider ラッパ
- `inference-host/src/UpdateChecker.cpp` — GitHub Releases ベース更新確認
- `inference-host/src/CrashHandler.cpp` — `SetUnhandledExceptionFilter`
- `learning/src/DpapiCrypto.cpp` — DPAPI ラッパ
- `pkg/msix/AppxManifest.xml` — MSIX マニフェスト
- `pkg/msi/Package.wxs` — WiX インストーラ

### 長期

- `learning/EmbeddingIndex.h` — 文脈ベクトルベース再ランキング（仮）

## 新規 IPC メッセージ

既存の配線済み 12 種（Handshake / Ping / Health / QueryDiagnostics / LoadModel /
QueryCandidates / QueryBatchConversion / Cancel / CommitObservation / AddUserWord /
RemoveUserWord / UpdateConfig）に加え、以下を Phase 5〜6 で順次追加する。

> 注: `MessageType` enum は 16 の named 型 + `Unknown` sentinel = 17 entries
> （`ipc/include/azookey/ipc/Messages.h` が正典）。このうち Payload/Dispatcher まで
> 配線済みは上記 12 種で、残る 4 種（`QueryPredictions` / `QueryCorrections` /
> `CommitCorrection` / `UpdateUserWord`）は enum のみ。配線済み判定は
> `Messages.h`・`Payloads.h`/`.cpp`・`Dispatcher.cpp` の 3 点を突き合わせて行い、
> enum に存在するだけの型を「利用可能」とみなさない。新メッセージ型を enum に
> 追加する際は **`Unknown` sentinel の前に挿入**すること（末尾の `Unknown` の
> 後ろに追加しない）。

| メッセージ | 方向 | 導入 Phase | 参照 |
|---|---|---|---|
| `QueryLiveConversion` / `Response` | TIP → Host | Phase 5 (M14) | legacy-parity §2 |
| `QueryPredictions` / `Response` | TIP → Host | Phase 5 (M15) | legacy-parity §3 + rich X-2 |
| `TransformSelectedText` / `Response` | TIP → Host | Phase 5 (M16) | legacy-parity §4 |
| `RequestPostCommitLint` / `Response` | TIP → Host | Phase 5 末 (M16 拡張) | rich X-3-3 |
| `LintFinding` | データ型 | Phase 5 末 | rich X-3-3 |
| `PredictStreamChunk`（push） | Host → TIP | Phase 6 (M24) | rich X-2-5 |
| `ReverseConvert` / `Response` | TIP → Host | Phase 6-A (M20) | tsf-deep §1 |
| `QueryFullRecompute` / `Response` | TIP → Host | Phase 5 末 | rich X-1-3 |
| `UpdateUserWord` / `Response` | Settings → Host | Phase 7 (M30) | 既存 enum 配線 |
| `QueryCorrections` / `CommitCorrection` Payload | TIP → Host | Phase 5〜6 | 既存 enum 配線 |

`UpdateConfig`（Settings → Host）は「新規追加」ではないため本表から外しているが、
M30 で完了するわけではない。M11 最小（v1.0）相当の settings.json 再読込は既に配線済みで、
現行の要求 payload は空オブジェクトである。roadmap M30 は変更対象に `ipc/src/Payloads.cpp`
を挙げて「M11 の最小 `UpdateConfig` を拡張」と定めており、payload 側の拡張が M30 に残る
（`docs/sideload-packaging-spec.md` §3 + roadmap M11 / M30）。実装済みの範囲と M30 で残る
範囲は次のとおり。

| 区分 | 内容 |
|---|---|
| 実装済み（M11 相当） | 要求 payload 空オブジェクト、`SettingsStore::Reload()` による settings.json 再読込、engine config 適用とモデル再ロード、応答 `(ok, error?)` |
| M30 に残る | 要求 payload の拡張（横断項目・バッチ訂正等をフル設定 UI から渡すためのフィールド追加）と、対応する `SettingsManager` 側の拡張 |

Envelope に `push: bool` フラグを追加し、`server → client` 一方向通知に
対応する（`docs/rich-features-spec.md` X-4-2）。

## TIP プロセス HWND 構成

TIP DLL がアプリプロセスにロードされた状態で、最大以下の HWND を保持する：

| HWND | 用途 | 親 | 生存期間 |
|---|---|---|---|
| `CandidateWindow` | 変換候補（M5 で実装済） | デスクトップ | composition 中のみ表示 |
| `PredictionWindow` | 予測候補（M15 で追加） | デスクトップ | composition 中常時表示（設定 ON 時） |
| `PromptDialog` | Magic Conversion / Replace Suggestion（M16） | フォアグラウンド | モーダル表示中のみ |
| `DebugWindow` | F10 デバッグ（M18-3） | デスクトップ | トグル表示 |

設定アプリは **別プロセス**（`azookey_settings.exe`）で、IPC 経由で Host 設定を変更する。
最小の設定永続化・反映（settings.json 書き込み + `UpdateConfig` 再読込）は **M11**（v1.0）で
導入し、フル設定 UI は **M30**（post-v1.0）で本格化する（roadmap M11 / M30、
`docs/sideload-packaging-spec.md` §3）。TIP プロセスからは `ITfFnConfigure::Show` で
起動するのみ。
