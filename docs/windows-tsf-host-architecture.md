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
CPU 版と Vulkan 版のどちらの Host 実行体を起動するかの選択規則は、`docs/sideload-packaging-spec.md` §1.6.3 が正典である。

### Host CLI 引数

Host の起動引数は `ParseHostArgs` が一括して解析する。
`--backend` は `cpu`、`cuda`、`vulkan` だけを受け付け、不正値を exit code 2 の起動エラーにする。
値を取る option が末尾にある場合と未知の引数も exit code 2 とし、暗黙の既定値へ戻さない。
`--pipe` は省略可能な次トークンを pipe 名として扱うが、`--` で始まるトークンは別 option として残す。
`--pipe-name` は値を必須とする。
`--model`、`--learning`、`--user-dict`、`--data-root`、`--mock-dict`、`--handshake-token` も値を必須とする。
`--data-root` は絶対パスのみ受け付け、Host と CLI の config / data / logs / models を
その root 配下に解決する。結合テストは一時 root を指定して通常のユーザーデータから隔離する。
`--learning` と `--user-dict` の明示パスは `--data-root` 内の既定パスより優先する。
`userdict` より後ろのトークンは user dictionary CLI へ、`lookup` より後ろのトークンは
読み取り専用 lookup CLI へ、`newwords` より後ろのトークンは新語承認 CLI
（`docs/auto-word-registration-spec.md` §7-3）へそのまま渡す。

CLI 引数の文字列は UTF-8 バイト列とする。Windows の entry point は wide argv（`wmain`）で受け、
引数ごとに UTF-16 から UTF-8 へ変換する。対を成さない surrogate のように UTF-8 へ変換できない
引数は exit code 2 で拒否し、置換文字で代用しない。narrow `main` の argv は process の
active code page で符号化されるため、CLI の入力経路として使わない。この契約は `userdict`、
`lookup`、`newwords` へ渡すトークンにも同じく及ぶ。

`--learning`、`--user-dict`、`--data-root`、`--mock-dict`、`userdict import`、`userdict export` の明示パスは、
この UTF-8 バイト列から `azookey::core::Utf8Path` で `std::filesystem::path` を構築する。
narrow の `std::filesystem::path` コンストラクタは Windows で active code page として復号する
ため、CLI 由来のパスには使わない。path を文字列へ戻す表示と JSON 出力は
`azookey::core::PathToUtf8` を使う。`--model` の model path も同じ UTF-8 バイト列として
model 読み込み経路へ渡す。model path の文字コード契約は `docs/zenzai-inference-spec.md` §9.3 に置く。

application manifest の `activeCodePage="UTF-8"` は、この契約の実現手段として採らない。
manifest の埋め込みは MSVC かつ identity package 有効時に限られ、採用すると build 構成によって
CLI の文字コードが変わる。wide argv と明示的な UTF-8 変換は build 構成に依存しない。
本節が定義するのは CLI 引数と path の文字コードであり、console の表示文字コードは対象外とする。

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
  確定直後のテキストと新規入力が混ざらないようにする。消費したキーは確定の後に残し、
  再試行が成功したら打った順に処理する（`docs/tsf-deep-integration-spec.md` §2.4.1）。

## IPC メッセージ

各メッセージの payload フィールドの正典スキーマは `ipc/include/azookey/ipc/Payloads.h`
（および `ipc/src/Payloads.cpp` の serialize/deserialize）を参照。以下のフィールド列挙は
要約であり、全フィールドを網羅しない。`✅` は Payload codec と Dispatcher のハンドラまで
配線してある型、`⚠️` は `MessageType` の enum（`ipc/include/azookey/ipc/Messages.h`）にだけ
定義してあり、Dispatcher が `unsupported_message_type` を返す型を示す。設定アプリ向けの型と
M45 以降に足した型は「新規 IPC メッセージ」の表に載せる。配線の正典は `ipc/` と
`inference-host/src/Dispatcher.cpp` の実装であり、作業の状態は Linear が持つ。

- ✅ `Handshake` — 要求 `(tip_version, protocol_version, capabilities, client_id?, handshake_token?)` /
  応答 `HandshakeResponse(host_version, protocol_version, accepted, model_loaded,
  host_generation_id?, capabilities[])`。欠落するcapabilitiesは空配列として受理し、
  `oob_cancel`と`commit_segments`の広告をもとに長文の制御と学習通知を選ぶ。
  `app_profile` と `candidate_tag` の広告は `docs/app-profile-spec.md` §3.1 に従う。
  `host_generation_id` は Host 起動ごとに生成し、同一プロセスの
  全接続で共有する UUID。省略する旧 Host は空文字として受理する。応答は TIP 向け runtime
  設定（下記「設定（SettingsStore）」）も載せる。
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
  SafeMode 中はロードせず `ok=false` / `error: "safe_mode"` を返す
  （`docs/dev-infrastructure-spec.md` §8.5.3）。
- ✅ `QueryCandidates` — 要求 `(reading, left_context, max_candidates, live, app?, raw_romaji?,
  english_candidates?)` /
  応答 `(candidates[], partial)`。各 candidate は `(surface, reading, score, source,
  description?, tag?)`。`app` と `tag` は `docs/app-profile-spec.md` §3.1 / §7、
  `raw_romaji` と `english_candidates` は `docs/inline-english-candidate-spec.md` §6.2 に従う。
  変換候補は `max_candidates` で切り詰める。応答にはその後で、rewriter（記号・絵文字）の
  追加分と M60 の英単語候補（辞書語 5 件・学習語 8 件・生ローマ字・6 形まで）が加わりうる。
- ✅ `QueryLiveConversion` — 要求 `(kana, context, auto_punctuation?, punctuation_style?,
  raw_romaji?, mixed_english?, secure?)` / 応答 `(surface, confidence, segments[]?)`。
  要求 ID は Envelope の `request_id`。`auto_punctuation`・`punctuation_style` と応答の
  `segments[]` は `docs/dynamic-punctuation-spec.md` §7 に従う。動作は
  `docs/legacy-parity-spec.md` §2 が定める。
- ✅ `QueryPredictions` — 要求 `(kana, leftSideContext, mode, app?, secure?)` /
  応答 `(predictions[], ok?, error?)`。要求 ID は Envelope の `request_id`。`secure` の
  欠落は secure として扱い、wire には `false` だけを載せる（`docs/privacy-and-secure-input-spec.md`
  §5.1.1）。動作は `docs/legacy-parity-spec.md` §3 と `docs/rich-features-spec.md` X-2 が定める。
- ✅ `ReverseConvert` — 要求 `(surface)` / 応答 `(reading, confidence)`。
  逆引きの順序は `docs/tsf-deep-integration-spec.md` §1.2 に従う。どこにも当たらない表層形では空の読みと信頼度 0 を返す。
- ✅ `QueryBatchConversion` — 要求 `(reading, raw_romaji, mode, auto_punctuation, max_candidates,
  english_candidates?)` /
  応答 `(segments[], full_surface, partial, canceled)`。各 segment は
  `(reading, candidates[])`。`QueryCandidates` と同じく `RequestScheduler` で
  cancel / latest を追跡し、キャンセル時は `canceled=true` と空の segments を返す。
  Host は読みを chunk に分けて segments を返し（`docs/romaji-batch-conversion-spec.md` §7）、
  `full_surface` は各 segment の先頭候補の surface を連結したものである。
  任意の `english_candidates` は `docs/inline-english-candidate-spec.md` §4.1.1・§6.7 に従う。
  segments が 1 要素のときは、`max_candidates` で切り詰めた後に M60 の英単語候補が加わりうる。
  `ai-cleanup` は共有 `AiBackend::Transform` へ委譲する。入力と許可の契約は
  `docs/romaji-batch-conversion-spec.md` §6 を参照する。
- ✅ `Cancel(target_request_id)`
- ✅ `CommitObservation(reading, chosen, shown, left_context, timestamp_ms, observation_id)` /
  応答 `CommitObservationResponse(ok)`。配送保証は at-least-once で、TIP は応答を受け取れ
  なかった観測を確定の順序を保ったまま再接続後に再送し、Host は `observation_id` で重複を
  捨てる。`observation_id` を省略する旧 TIP は空文字として受理し、冪等化の対象外とする。
  キーの生成規則、両側の上限、失われうる範囲は `docs/learning-data-management-spec.md` §12
  が正典。
- ✅ `CommitSegmentsObservation` — 応答は `CommitObservationResponse(ok)`。全文節を検証し、
  観測 ID 単位で再送を抑止する。payload と旧 Host への fallback は
  `docs/romaji-batch-conversion-spec.md` §6.4 を正典とする。
- ✅ `ObserveTypo(wrong_reading, correct_reading, timestamp_ms, secure, learning_allowed)` —
  fire-and-forget で、Host は応答を返さない。学習ゲートは `docs/privacy-and-secure-input-spec.md`
  §5.1.1、payload の契約は `docs/typo-correction-learning-spec.md` §7 / §12.13 に従う。
- ✅ `ListNewWordCandidates(state_filter, max_items)` / 応答 `(ok, error?, items[])`、
  `ResolveNewWord(surface, reading, action)` / 応答 `(ok, changed, error?)` — 新語候補（M36-A）の
  一覧と確定・却下。契約は `docs/auto-word-registration-spec.md` §7-1 / §7-2 に従う。
- ✅ `AddUserWord` / `RemoveUserWord` — `InferenceEngine` の状態ロック下で
  `UserDictionary` を更新し、永続化に成功した場合だけ `ok=true` を返す。
  永続化に失敗した場合は直前の辞書状態へ戻し、`Health` の `last_error` に反映する。
  応答は `(ok, generated_id?, error?)`（`RemoveUserWord` は `(ok, error?)`）。`ok=false` のとき `error` に
  `busy` / `lock_failed` / `load_failed` / `save_failed` / `not_found` / `unavailable` /
  `invalid_request` / `not_authenticated` のいずれかを載せる（前の 6 つは `UserDictionaryEditResultName` と同じ語）。
  `error` を持たない古い Host の応答も読める。
- ✅ `UpdateConfig` — 要求 payload は空オブジェクト（設定内容は wire に載せない。Host が
  `SettingsStore::Reload()` で settings.json を読み直す） / 応答 `(ok, error?)`。
  再読込は `update_config_mutex` で直列化し、runtime settings を engine config へ適用してから
  モデルを再ロードする。settings.json が invalid な場合は engine を触らず `ok=false` を返す。
- ✅ `QueryDiagnostics` — 要求 payload は空オブジェクト / 応答
  `(model_loaded, loaded_model_path?, engine, backend, rss_mb, ep?, ep_state?, ep_last_error?,
  learning_entries, user_dict_entries, fallback_state, last_error?, neologd_layer?, backend_selection?,
  prompt_format?)`。
  `neologd_layer` は `neologd_lexicon` 層の状態で、値の意味は `docs/auto-word-registration-spec.md` §15.14、
  wire の形は `docs/dev-infrastructure-spec.md` §12.6 が定める。
  `fallback_state` は `healthy` / `degraded_simple` / `degraded_model` / `safe_mode` のいずれかで、
  `safe_mode`（`docs/dev-infrastructure-spec.md` §8.5.3）は他のどの判定より優先する。
  送信側は診断 CLI（`diagnostics/` の `azookey_diag` ターゲット）で、
  `Diagnostics.cpp` の IPC プローブが Handshake → Ping に続けて `request_id=3` /
  `trace_id="diag-query-diagnostics"` / payload `{}` で送り、応答を `ParseQueryDiagnostics`
  して診断スナップショットへ格納する。TIP が送る契機と、応答のうち `fallback_state` だけを
  使う規則は `docs/dev-infrastructure-spec.md` §8.5.1 が定める。
- ⚠️ `UpdateUserWord` / `QueryCorrections` — enum にだけ定義し、Payload codec も Dispatcher の
  ハンドラも持たない。受信時は次項の `unsupported_message_type` 応答になる。
- 認証済み Dispatcher がハンドラ未対応の型または `Unknown` を受け取った場合は、
  request の型と相関 ID を保った `{"ok":false,"error":"unsupported_message_type"}`
  応答を返す。無応答は fire-and-forget の `Cancel` と `ObserveTypo` に限る。

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
- 永続フォーマットは TSV で、DPAPI で暗号化して拡張子 `.enc` を付けたファイルに保存する
  （`learning/include/azookey/learning/LearningStore.h`）。未指定時は
  `%LOCALAPPDATA%\azooKey\data\learning.tsv` を M7 形式の基準パスとし
  （`inference-host/src/UserDataPaths.cpp`）、保存先は M54 の v2 形式の
  `learning.v2.tsv.enc` である。M7 形式の `learning.tsv.enc` と旧平文の `learning.tsv` は
  保存先ではない。v2 ファイルが無い間は M7 形式が読み込み元であり、保存の前に中断した移行を
  片付ける。v2 の保存を検証した後に M7 形式のファイルを削除する
  （`learning/src/LearningStore.cpp` の `Save`、`docs/user-learning-enhancement-spec.md` §3.1、
  `docs/learning-data-management-spec.md` §14）。
- `reading` / `surface` の TSV エスケープとヘッダー、M7 形式から v2 への移行は
  `docs/user-learning-enhancement-spec.md` §3.1 が正典。
- ユーザー辞書は未指定時 `%LOCALAPPDATA%\azooKey\data\user_dict.json` を基準パスとし、
  JSON を DPAPI で暗号化した `user_dict.json.enc` に保存する
  （`learning/include/azookey/learning/UserDictionary.h`）。拡張子 `.enc` の無い
  `user_dict.json` は旧平文の移行元である。`--learning` / `--user-dict` 指定時は
  明示パスを基準パスとして優先する。
- `userdict` / `newwords` / `lookup` の CLI 契約（IPC 送信の待ち時間、Host の待ちの予算、拒否コードの表示、
  `persistent_edit` の記録）と、`user_dict.json` の排他・原子的な保存は
  `docs/learning-data-management-spec.md` §13 が正典。
- 観測の永続化の契機（`learning_flush_every_n` / `learning_flush_interval_sec`）、`Save()` 失敗時の
  再試行、GC（`learning_max_records` / `learning_min_weight`）は
  `docs/user-learning-enhancement-spec.md` §3.1.1、時間減衰は同 §5 が正典。
  終了経路ごとの flush の保証と失われうる量は `docs/learning-data-management-spec.md` §11 が持つ。
- 破損時はリセット可能（`LearningStore::Reset` or ファイル削除。IPC は
  `docs/learning-data-management-spec.md` §4.6）。

## 設定（SettingsStore）

- 設定の正典スキーマは `settings/mvp-settings.schema.json`。`inference-host` の
  `SettingsStore` は起動時に `%LOCALAPPDATA%\azooKey\config\settings.json` を読み、
  未指定キーを schema default にフォールバックして提供する。
- `settings.json` はファイル正典とし、設定アプリと TIP（モデルの再試行、入力インジケーターのメニュー）からの IPC `UpdateConfig` は payload 空の
  再読込トリガとして扱う。設定オブジェクトは IPC schema へ二重定義しない。
- `HandshakeResponse` が載せる TIP 向け設定（batch 変換モード、句読点方針、rewriter、
  `maxCandidates`）は、応答を作る直前に `settings.json` の更新時刻を見て、読み込み済みの
  設定より新しければファイルから直接読んで応答に載せる。設定アプリは保存してから
  `UpdateConfig` を送るため、保存を検知した TIP の再 Handshake が `UpdateConfig` を
  追い越しうる。この読み取りは runtime 設定を置き換えず、`UpdateConfig` の再読込・
  `EngineConfig` の適用・モデル再ロード・crash consent の適用は従来どおり `UpdateConfig`
  の責務に残す。
- 応答のためのこの読み取りは、`UpdateConfig` を直列化する mutex を取らない。`UpdateConfig`
  はモデル再ロードの間その mutex を保持するため、Handshake が同じ mutex を待つと TIP の
  handshake timeout を超えて全接続が切断・再接続を繰り返す。同じ理由で、この経路は不正な
  JSON を隔離せず、ファイルロックを長く待たず、読めない・壊れている・存在しないときは
  読み込み済みの設定をそのまま使う。更新時刻が同一 tick 内に収まる連続保存は 1 回として
  扱われ、後続の `UpdateConfig` が最終状態を適用する。
- TIP 側は、接続が健全なまま `settings.json` が変わった場合に同じ接続で Handshake を
  し直して上記の設定を取り直す。再接続を待たないため、既に開いている文書が旧設定のまま
  取り残されない。
- 推論チューニング値は `inferenceThreads`、`maxCandidates`、`maxContextLength` を使う。
  `inferenceThreads` は 0 から 8 で、0 の場合は `powerProfile` に従う。
  `auto` は Windows の電源状態から AC 時 8、バッテリ時 2、取得失敗・不明時 4 を基準とする。
  `performance` は 8、`battery_saver` は 2 を基準とする。
  自動導出値は `std::thread::hardware_concurrency()` で上限化し、並列度が 0 の場合は 1 とする。
  `inferenceThreads=1..8` の明示指定は電源状態・並列度より優先する。
  電源状態は起動時と設定再読込時に取得し、決定した実効値をモデル再ロードでも使う。
  電源の変化でも設定し直し、ロード済みのモデルにも次の変換から効く（`docs/copilot-pc-backend-spec.md` §6.2）。
  `maxCandidates` は 1 から 32 で既定値 9、`maxContextLength` は 0 から 30 で既定値 30 とする。
  settings.json に明示で書かれた値は既定値より優先し、既定値が変わっても移行しない
  （settings-app は変更したキーだけを保存するので、明示値はユーザーが選んだ値である）。
  `maxContextLength` の単位は Unicode コードポイント数であり、0 の場合は左文脈を推論へ渡さない。
  TIP は非 secure 入力で同じ行の composition または選択範囲の開始位置直前から
  最大 30 コードポイントを読み、
  `maxContextLength` で短縮してローカルの TIP → Host IPC 候補要求へ渡す。読取失敗、
  選択状態が不明、secure 入力では空文字列を渡す。利用範囲は
  `docs/privacy-and-secure-input-spec.md` §5.3 に従う。
- parse に失敗した `settings.json` は `.invalid` suffix へ隔離する。起動時は default 設定で継続し、
  `UpdateConfig` 再読込時は error を返して現在の runtime 設定を維持する。隔離の条件と、設定アプリの
  保存との排他は `docs/learning-data-management-spec.md` §14「共有ユーザーデータの writer 責務」を正典とする（`FileLock.h` の共有ファイル
  単位ロックで read から rename までを直列化する）。
- 候補ウィンドウ位置更新（`update_pos` / `OnLayoutChange` 連動）の再入対策として、先行実装の
  「更新中は layout change を一定時間抑止する状態機械」を設計参照にできる（抑止値は環境依存）。

## 共有ユーザーデータの writer 責務

`%LOCALAPPDATA%\azooKey\` 配下の共有ファイルを直接書き込めるプロセスの固定、`settings.json` の
quarantine と保存の排他の契約は `docs/learning-data-management-spec.md` §14 が正典。

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

## 新規モジュール（Phase 5〜7）

下記は v1.0 以降の Phase が追加するモジュールの配置である。正典仕様は各 spec を参照。
「予定」と付けたファイルはリポジトリにまだ無く、括弧内の roadmap / spec が定義を持つ。

### Phase 5（レガシー parity、`docs/legacy-parity-spec.md`）

- `core/include/azookey/core/UserAction.h` — VK → 抽象アクション enum
- `core/include/azookey/core/InputState.h` — 入力状態機械
- `core/src/UserActionMap.cpp` — VK → UserAction 変換テーブル
- `core/src/CustomRomajiLoader.cpp` — TSV パース + ホットリロード
- `core/src/InputState.cpp` — Ctrl+Shift+U hex バッファ（Unicode 入力状態）
- `tsf-tip/src/DebugLogBuffer.cpp` — F10 デバッグウィンドウのログ保持と本文ゲート
- `tsf-tip/src/PredictionWindow.cpp` — 予測候補 HWND
- `tsf-tip/src/PromptDialog.cpp` — Magic Conversion プロンプト（予定。roadmap M16、`docs/legacy-parity-spec.md` §4.3）
- `tsf-tip/src/DebugWindow.cpp` — F10 デバッグウィンドウ
- `tsf-tip/src/CaretRectResolver.cpp` — `GetTextExt` / `GetGUIThreadInfo` 3 段
  フォールバックとモニタ作業領域の解決（`docs/legacy-parity-spec.md` §9）
- `inference-host/src/AiBackend.cpp` — OpenAI 互換 API クライアント

### Phase 5〜6 横断（`docs/rich-features-spec.md`）

- `tsf-tip/src/ContextTracker.cpp` — 段落 / 直前文 / アプリ別履歴（予定。`docs/rich-features-spec.md` X-4-3）
- `tsf-tip/src/ForegroundAppDetector.cpp` — 前面アプリ実行ファイル名取得
- `tsf-tip/src/TypingTempoTracker.h` — タイピング速度推定（予定。roadmap M24 の横断、`docs/rich-features-spec.md` X-1-2）
- `core/src/QwertyAdjacency.cpp` — FuzzyMatch 用 QWERTY 隣接表（予定。roadmap M17 の横断、`docs/rich-features-spec.md` X-3-1）
- `bench/rich_features_bench.cpp` — リッチ化機能のレイテンシ計測

### Phase 6-A（TSF 深部、`docs/tsf-deep-integration-spec.md`）

- `tsf-tip/src/ReconversionFunction.cpp` — `ITfFnReconversion`
- `tsf-tip/src/CandidateListUIElement.cpp` — UI-less Mode 用
- `tsf-tip/src/TextService.cpp` — `ITfFnConfigure` / `ITfFunction`
- `tsf-tip/src/SettingsLauncher.cpp` — 設定アプリ EXE 解決・起動

### Phase 6-B（Copilot+ PC / NPU、`docs/copilot-pc-backend-spec.md`）

- `inference-host/include/azookey/host/BackendSelector.h` / `src/BackendSelector.cpp` — DXCore 列挙・ランタイム DLL 判定による `auto` の解決（優先順位の適用は `src/main.cpp`）
- `inference-host/src/MmapModelLoader.cpp` — `CreateFileMapping` + `MapViewOfFile`
- `inference-host/src/PowerStatusWatcher.cpp` — AC/バッテリ検知

### Phase 6-C（UI モダン化、`docs/native-ui-spec.md`）

- `tsf-tip/src/ThemeColors.cpp` — Light/Dark/ハイコントラストの判定と色テーブル
- `tsf-tip/include/azookey/tsf/DpiScaling.h` — DPI 換算と PMv2 の一時切替
- `tsf-tip/src/RenderingEngine.cpp` — DComp + D2D + DirectWrite 共通

### Phase 7（サイドロード配信、`docs/sideload-packaging-spec.md`）

- `settings-app/` — C++/WinRT WinUI 3 設定アプリ（**最小版は M11 / v1.0** で導入し
  settings.json 書き込み + `UpdateConfig` 再読込まで含む。フル UI は M30。roadmap M11 / M30、
  `docs/sideload-packaging-spec.md` §3.0）
- `core/src/EtwLogger.cpp` — ETW Provider ラッパ
- `inference-host/src/UpdateChecker.cpp` — GitHub Releases ベース更新確認（予定。roadmap M32、`docs/sideload-packaging-spec.md` §6）
- `core/src/CrashReporting.cpp` — `SetUnhandledExceptionFilter` による同意制の例外メタデータ収集（Host と設定アプリ。TIP DLL には入れない）
- `learning/src/DpapiCrypto.cpp` — DPAPI ラッパ
- `pkg/msix/AppxManifest.xml` — MSIX マニフェスト
- `pkg/msi/Package.wxs` — WiX インストーラ

## 新規 IPC メッセージ

以下は「IPC メッセージ」の一覧に載せない、設定アプリ向けの型と M45 以降に足した型の一覧である。
表の型はどれも Payload codec と Dispatcher のハンドラまで配線してある。フィールドの正典は
`ipc/include/azookey/ipc/Payloads.h` である。

> 注: 利用可能な型は `Messages.h`、`Payloads.h`/`.cpp`、`Dispatcher.cpp` の
> 3 点で確認する。enum に存在するだけの型を利用可能とみなさない。
> 新メッセージ型は **`Unknown` sentinel の前に挿入**する。

| メッセージ | 方向 | 導入 Phase | 参照 |
|---|---|---|---|
| `ListModels` / `BenchmarkModel` | 設定アプリ → Host | M45、配線済み。capabilities `list_models` / `benchmark_model`。`ListModels` の要求の `include_catalog` で、応答に既定の catalog の `catalog[]` と各 GGUF の `catalog_id` が付く（DEV-1821） | model-management §4.1 / §4.2 |
| `FetchCatalogModel` / `QueryCatalogFetch` | 設定アプリ → Host | DEV-1821。capability `fetch_catalog_model`。開始はすぐ応答し、進捗と結果は問い合わせで得る | model-management §4.4 |
| `CancelCatalogFetch` | 設定アプリ → Host | DEV-1840。capability `cancel_catalog_fetch`。実行中のその entry の取得に止まるよう求め、すぐ応答する | model-management §4.4 |
| `ListLearningEntries` / `ForgetLearningEntry` / `ExportLearningData` / `ImportLearningData` | 設定アプリ → Host（`ForgetLearningEntry` の `reading` + `surface` 形は TIP → Host） | M49、capability `learning_data_management`。payload codec は `Payloads.h` の M49 節 | learning-data-management §4 |
| `ResetLearningStore` | 設定アプリ → Host | M30、capability `learning_reset` | learning-data-management §4.6 |
| `QueryPersona` | 設定アプリ → Host | M30、capability `persona` | rich X-2-7 |
| `DetectAnomalies` | 設定アプリ → Host | M30、capability `detect_anomalies` | rich X-3-6 |
| `CommitCorrection` Payload | TIP → Host | M54。TIP は確定直後の Backspace（`undo`）と azooKey UI での再変換（`reconvert`）で送る。capability `commit_correction` | user-learning-enhancement §4.1 |

`MessageType` の enum にまだ無く、spec が定義を持つ予定の型は次のとおり。enum へ足すまで
上の一覧にも表にも載せない。

| メッセージ | 方向 | 予定 | 参照 |
|---|---|---|---|
| `TransformSelectedText` / `Response` | TIP → Host | 予定（M16 Magic Conversion / Replace Suggestion） | legacy-parity §4.4、ai-backend §4.1 |
| `RequestPostCommitLint` / `Response` | TIP → Host | 予定（roadmap M16 の横断 X-3-3） | rich X-3-3、ai-backend §4.3 |

校正ビューで TIP が設定アプリへ本文を渡し、範囲の選択を受ける session（`ipc/include/azookey/ipc/ProofreadSession.h`）は
Host の IPC ではなく、TIP が押下ごとに作る使い捨ての pipe で、本表の `MessageType` には含めない（rich X-3-6）。

`UpdateConfig`（Settings → Host）は「新規追加」ではないため本表から外しているが、
M30 で完了するわけではない。M11 最小（v1.0）相当の settings.json 再読込は既に配線済みで、
現行の要求 payload は空オブジェクトである。roadmap M30 は変更対象に `ipc/src/Payloads.cpp`
を挙げて「M11 の最小 `UpdateConfig` を拡張」と定めており、payload 側の拡張が M30 に残る
（`docs/sideload-packaging-spec.md` §3 + roadmap M11 / M30）。実装済みの範囲と M30 で残る
範囲は次のとおり。

| 区分 | 内容 |
|---|---|
| 実装済み（M11 相当） | 要求 payload 空オブジェクト、`SettingsStore::Reload()` による settings.json 再読込、engine config 適用とモデル再ロード、応答 `(ok, error?)` |
| M30 に残る | 要求 payload の拡張（横断項目・バッチ訂正等をフル設定 UI から渡すためのフィールド追加）と、対応する `SettingsStore`（`inference-host/src/SettingsStore.cpp`）側の拡張 |

## TIP プロセス HWND 構成

TIP DLL がアプリプロセスにロードされた状態で、最大以下の HWND を保持する：

| HWND | 用途 | 親 | 生存期間 |
|---|---|---|---|
| `CandidateWindow` | 変換候補 | デスクトップ | composition 中のみ表示 |
| `PredictionWindow` | 予測候補（M15 で追加） | デスクトップ | composition 中常時表示（設定 ON 時） |
| `PromptDialog` | Magic Conversion / Replace Suggestion（予定。M16 が追加する。`docs/legacy-parity-spec.md` §4.3） | フォアグラウンド | モーダル表示中のみ |
| `DebugWindow` | F10 デバッグ（M18-3） | デスクトップ | トグル表示 |

設定アプリは **別プロセス**（`azookey_settings.exe`）で、IPC 経由で Host 設定を変更する。
最小の設定永続化・反映（settings.json 書き込み + `UpdateConfig` 再読込）は **M11**（v1.0）で
導入し、フル設定 UI は **M30**（post-v1.0）で本格化する（roadmap M11 / M30、
`docs/sideload-packaging-spec.md` §3）。TIP プロセスからは `ITfFnConfigure::Show` で
起動するのみ。
