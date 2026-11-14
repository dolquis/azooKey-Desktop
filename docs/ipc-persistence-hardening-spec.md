# ユーザーデータ永続化・IPC/JSON 堅牢化 仕様（M39・M40）

対象リポジトリ: dolquis/azooKey-Desktop
対応マイルストーン: M39（§5）、M40（§6）
関連: `docs/dev-infrastructure-spec.md`（開発基盤の入口と転送表）、
      `docs/observability-spec.md`（構造化ログ）、
      `docs/host-availability-spec.md`（Host の再接続）、
      `plans/windows-port-roadmap.md` M39 / M40

本書は `docs/dev-infrastructure-spec.md` から §5・§6を切り出した文書である。節番号 §5.x・§6.x は旧 spec のまま保ち、旧 spec の転送表が§5・§6を本書へ解決する。本書内で、本書にない節（旧 spec の他の節）を指すときは、文書名を付けて書く（例: `build-ci-spec.md` §4.3）。

## 正典範囲

ユーザーデータの標準ディレクトリレイアウト、パス解決規約、原子的書き込み（§5）と、JSON パーサの強化要件、
協定外メッセージの扱い、Named Pipe のセキュリティ強化（§6）は本書が正典である。

## 節とマイルストーンの対応

| 節 | マイルストーン |
|---|---|
| §5 | M39 |
| §6 | M40 |

## 5. ユーザーデータ永続化の堅牢化（M39）

### 5.1 解決する問題

学習・辞書ファイルの既定パスを相対パス（`azookey_learning.tsv` /
`azookey_user_dict.json`）で持つと、Host の起動元ディレクトリによって保存先が
変わり、ユーザーデータが迷子になりやすい。

### 5.2 標準ディレクトリレイアウト

明示指定がない場合、`%LOCALAPPDATA%\azooKey\` 配下を既定とする:

```
%LOCALAPPDATA%\azooKey\
  config\   settings.json
  data\     learning.v2.tsv.enc / user_dict.json.enc / typo_corrections.tsv.enc（M35）/
            auto_words.tsv.enc（M36-A）/ english_learning.v2.tsv.enc（M60）/
            host_run_state.txt（M47）
  logs\     host-YYYYMMDD.jsonl / tip-YYYYMMDD.jsonl
  models\   zenzai\
```

注: `typo_corrections.tsv.enc`（M35）、`auto_words.tsv.enc`（M36-A）、`english_learning.v2.tsv.enc`（M60）は
`learning.v2.tsv.enc` / `user_dict.json.enc` と同じ `data\` 配下に置く（`UserDataPaths`
の `data_dir` 規約に合わせる）。学習の 2 チャネルは、M7 形式の `learning.tsv.enc` /
`english_learning.tsv.enc` を移行元として同じ配下に残しうる。M49 backup（`docs/learning-data-management-spec.md`
§2）の対象範囲は本レイアウトを正典とする。ただし `host_run_state.txt`（`host-availability-spec.md` §8.5.3 の
クラッシュ検知用の印）はユーザーデータではなく、backup の対象に含めない。
拡張子 `.enc` のない旧ファイルは移行元で、移行時の `.bak` は保全する。

`%LOCALAPPDATA%` は `SHGetKnownFolderPath(FOLDERID_LocalAppData, ...)` で
取得する（WIL 導入後は `wil::unique_cotaskmem_string` で受ける）。必要な
サブディレクトリは起動時に自動作成する。

### 5.3 パス解決規約

- `--learning` / `--user-dict` が指定された場合は、従来どおり**明示パスを
  優先**する（既存テスト・CI・開発フローを壊さない）。
- `--data-root` を指定した場合は、その絶対パスを config / data / logs / models の
  root とする。`--learning` / `--user-dict` の明示パスはこの root より優先する。
- `--data-root` も指定がない場合は `%LOCALAPPDATA%\azooKey\data\...` を使う。
- 保存先決定ロジックは純粋関数として切り出し、unit test で
  「明示指定優先 / 既定パス生成 / 環境変数欠落時の挙動」を検証する。

### 5.4 原子的書き込み

学習・辞書ファイルの保存は「一時ファイルへ書き込み → flush →
`MoveFileEx`（`MOVEFILE_REPLACE_EXISTING` + `MOVEFILE_WRITE_THROUGH`）で
rename」で原子的に行い、書き込み中クラッシュによる破損を防ぐ。

Host は SIGINT / SIGTERM と Windows の CTRL+C / CTRL+Break / コンソール終了を
停止要求として処理し、`InferenceEngine` のデストラクタが未保存の学習観測を
flush してから終了する。
Windows のコンソール終了ハンドラは停止要求だけを通知し、通常の終了処理が
完了するまで OS の制限時間内で待機する。
コンソールウィンドウの終了（`CTRL_CLOSE_EVENT`）は OS の制限時間が有限で
ConPTY では更に短くなりうるため、この経路の最終 flush は best-effort である。
保証する終了経路と失われうる最大量の正典は
`docs/learning-data-management-spec.md` §11 に置く。

### 5.5 本マイルストーンの範囲と将来課題

本マイルストーンの範囲は **保存先統一・ディレクトリ自動作成・原子的書き込み・
パス解決の unit test** まで。次の項目は将来課題として扱い、ここでは仕様化
しない（対応マイルストーンを明記）:

- ファイルロック（単一 writer 保証） — M39 で実装可だが、Host は単一
  プロセス前提のため多重起動検出として軽量に扱う
- 破損時のバックアップ世代・起動時検証 — 将来課題
- DPAPI 暗号化 — **M34** の範囲（重複実装しない）

### M39 受け入れ条件

受け入れ条件の定義の正典は [`plans/windows-port-roadmap.md`](../plans/windows-port-roadmap.md)
の M39 節とする。本書は §5 で保存先の決定規約と原子的書き込みを定義し、受け入れ条件を複製しない。

## 6. IPC/JSON 堅牢化（M40）

### 6.1 方針

レビューは自前 JSON パーサ（`ipc/src/Json.cpp`）の `nlohmann-json` 置換
（提案 A）と、自前を残す場合の fuzz/malformed テスト追加（提案 B）を挙げる。
本仕様は**提案 B を主**とする。即時の全置換はしない（理由は `build-ci-spec.md` §11）。自前
パーサを残したまま境界堅牢性を上げる。外部ライブラリへの移行は v1.0 の範囲では
行わないことを `build-ci-spec.md` §11.2.1 で確定した。再評価の条件も同節が正典とする。

Envelope の送信は payload の JSON を検証してから連結し、受信は文書全体を
検証した parser が特定した最初のトップレベル `payload` の範囲をコピーする。
payload の再文字列化は行わず、キー順序・空白・数値・escape の表記を保持する。
型別 payload の Parse は引き続き行う。文字列の同一性を JSON の意味的同一性の
判定に使わない。欠落 payload の `{}`、重複キーの先勝ち、深度・入力長・不正値の
拒否規則は維持する。性能の確認には `build-ci-spec.md` §4.5 の IPC 計測を使う。

### 6.2 JSON パーサ強化要件

`ipc/src/Json.cpp` は次の防御を備える。各上限値は**本節（spec）を正典**とする
（AGENTS.md「対象と正典」: IPC payload / JSON schema / 挙動の決定は
`docs/*-spec.md`）。`ipc/include/azookey/ipc/Limits.h` の定数は本節の値に
一致させ、齟齬が出た場合は spec を正として Limits.h を修正する。コード側の
上限変更は wire contract と受け入れ条件に影響するため、spec 更新（レビュー）と
セットで行うこと。定数と spec 値の同期は `static_assert` / テストで担保する
（code-only な変更で wire contract が無断で変わる drift を防ぐ）。

- **ネスト深度上限 = 64**（`kMaxJsonNestDepth`） — 配列/オブジェクトのネストが
  64 を超えたらパース失敗を返す（再帰によるスタック枯渇防止）。`ParseValue` は
  depth を持ち回り、`ParseObject` / `ParseArray` が超過を検査する。
- **最大入力長 = 1 MiB**（`kMaxJsonInputBytes = 1024 * 1024`） — `ParseDocument`
  冒頭で入力バイト長を検査し、超過は即失敗。§6.4.3 の最大フレームサイズと同値で
  揃える（フレームを読み切れた時点で入力長上限も満たされる）。
- **サロゲートペア結合** — `\uXXXX` の上位（U+D800–U+DBFF）に続く下位
  （U+DC00–U+DFFF）を結合し、単独サロゲート（上位のみ / 下位のみ）は不正として
  拒否する。
- **不正 UTF-8 / 制御文字の拒否** — 文字列内の生の制御文字（U+0000–U+001F）を
  拒否し、生 UTF-8 バイト列は長さ・継続バイト・overlong（C0/C1、E0<A0、F0<90）・
  範囲外（F4>8F、> U+10FFFF）を検査して不正シーケンスを拒否する。
- **末尾ゴミの拒否** — 値の後ろに空白以外が残る入力を拒否する
  （`ParseDocument` が `pos_ == size` を要求）。
- **数値の安全な扱い** — `0` 始まりの多桁・小数点後桁なし・指数部桁なし等の
  不正形を拒否し、`1e9999` 等は `std::isfinite` で弾く。
- **uint64 精度** — **plain 整数 token**（符号なし数字のみ）の抽出
  （`GetInt` / `GetUInt`）は元トークン文字列から `std::stoll` /
  `std::stoull` で復元し、double 経由の精度欠落を避ける。
  `uint64_t` 全域 `18446744073709551615` まで正しく取り出せ、桁あふれは
  `nullopt` とする。小数・指数形などの非 plain 数値形は double 経路に入るため、
  `int64_t` / `uint64_t` の変換前に exclusive upper bound を検査し、範囲外 double
  を整数へキャストしてはならない。直列化側は `request_id` / `Ping.nonce` /
  `t_ms` / `Cancel.target_request_id` / `CommitObservation.timestamp_ms` 等の
  uint64 フィールドを token 保持版 `Value(uint64_t)` に渡し、2^53 超の値でも
  wire 上で丸めない。
- **数値 codec の locale 非依存** — 数値の parse / stringify は
  `std::from_chars` / `std::to_chars` で行い、ホストプロセス（TIP は任意アプリ内
  in-proc、Host も CRT locale を変える可能性）が非 C ロケール（小数点が `,` 等）
  を設定しても wire 表現を壊さない。overflow は `nullopt` で拒否し、
  underflow は JSON 数値として受け入れて 0.0（符号付きゼロを含む）へ丸める。

### 6.3 追加テストと協定外メッセージの扱い

`ipc/tests/` に境界・malformed テストを置く。JSON パーサ単体の境界は
`json_test.cpp` の `JsonTest` スイートで扱い、Envelope と length-prefix framing は
`messages_test.cpp` に置く。v1.0 の堅牢性バーは「**決定的な境界コーパス** + **有界な擬似乱数
スモーク**」で満たす。

探索的検証には `ipc_json_fuzzer` を使う。`AZOOKEY_BUILD_FUZZERS` は既定 `OFF` とし、
Clang と libFuzzer を利用できる構成でのみ有効化する。ハーネスは任意バイト列を
`ipc::json::Parse` へ渡し、`linux-asan-ubsan` の週次 schedule と手動 dispatch で
`ipc/testdata/` を seed corpus として 60 秒実行する。通常のオフラインビルド、MSVC
ビルド、PR の required check には含めない。

クラッシュ検体は sanitizer workflow の artifact に保存する。検体を再現して最小化した後、
JSON parser の入力なら `ipc/tests/json_test.cpp`、Envelope または frame の入力なら
`ipc/tests/messages_test.cpp` へ決定的な回帰ケースとして追加する。回帰テストが単独で失敗し、
修正後に通ることを確認してから、同じ検体を seed corpus に残すかを判断する。

- 深すぎるネスト・最大長超過を拒否する（`kMaxJsonNestDepth + 1` /
  `kMaxJsonInputBytes + 1`）
- 末尾ゴミ・`0` 始まり・指数部欠落を拒否する
- 巨大数（`1e9999`）・uint64 桁あふれ（`...616`）を安全に拒否する
- 不正 Unicode escape（単独サロゲート `\uD800` / `\uDC00`）を拒否する
- 文字列内の生制御文字（`\x01`）・overlong UTF-8（`C0 AF`、`E0 80 80`）を拒否する
- 擬似乱数バイト列を `Parse` してもクラッシュせず失敗を返す（有界回数スモーク）
- 自前 JSON パーサの直接単体テスト（int64/uint64 精度・深度・unicode escape・
  round-trip）を `ipc_json_tests`（`json_test.cpp`）に置く
- 最大フレーム超過の length-prefix を `Decode/EncodeLengthPrefixed` が拒否する
- `Payloads.cpp` 側で必須キー欠損・型不一致を安全に拒否する

**Dispatcher のハンドラを持たない MessageType**（`QueryCorrections` / `UpdateUserWord`。
`docs/windows-tsf-host-architecture.md`「IPC メッセージ」の索引）を受信した際、Dispatcher は黙って無視せず**明示的なエラー応答**を返す。
応答待ちの blocking client をハングさせないため、既存ワイヤ形式の envelope に
「未対応 type」を表す `{"ok":false,"error":"unsupported_message_type"}` を載せ、
request の `type`、`request_id`、`trace_id` を応答 envelope に維持する。
`Cancel` など fire-and-forget の type のみ無応答を許す。
認証済み Dispatcher の未配線 type と `Unknown` はこの応答へ統一する（DEV-266）。
併せて MessageType 列挙 ↔ `Payloads` codec の網羅
整合をテスト / CI で検査し、列挙追加時の codec 取りこぼしを防ぐ。

### 6.4 Named Pipe セキュリティ強化

現行 `ipc/src/NamedPipeTransport.cpp`（per-user pipe 名 + DACL +
length-prefix フレーミング + `kMaxFrameSize`）を基盤に強化する:

- **6.4.1 Release で fail-closed** — SID 取得失敗時、現在は fallback pipe
  名 / 既定セキュリティに落ちる。Debug/test ではこの fallback を許可し、
  Release/production では SID 取得失敗時に Host 起動を失敗させる
  （ビルド構成で分岐）。Debug/test の restricted-token 実行環境では current-user
  の logon SID ACE だけでは同一プロセス client の write open が拒否されるため、
  Release 以外に限り restricted-token 互換 ACE を追加して transport tests を
  実行可能にする
- **6.4.1a remote client rejection** — Named Pipe はローカル IME ↔ Host 専用
  であり、server 作成時に `PIPE_REJECT_REMOTE_CLIENTS` を指定して remote client
  接続を OS レベルで拒否する。これは DACL / per-user pipe 名の補助防御であり、
  本項は remote client 拒否に限る。**同一ユーザー内の別プロセスを秘密だけで
  区別する認証は不可能**であり（§6.4.4）、Handshake トークンは同一ユーザー
  なりすまし対策の認証ではなく多層防御として §6.4.4 で扱う
- **6.4.1b logon 境界と pipe 名の占有** — DACL の許可先には user SID ではなく
  logon SID を使い、同じ Windows アカウントの別ログオンセッションを拒否する。
  許可権限は `GENERIC_ALL` ではなく、duplex pipe の read/write と属性操作、
  複数 instance 作成に必要な個別権限へ限定する。`FILE_APPEND_DATA` と
  `FILE_CREATE_PIPE_INSTANCE` は同じ値であり、現行の複数 instance server では
  後者を除外できない。この残余権限に対して、最初の instance に
  `FILE_FLAG_FIRST_PIPE_INSTANCE` を指定し、先行プロセスが同名 pipe を作成済みなら
  Host 起動を失敗させる。接続後は server と client の双方が OS の pipe peer PID
  から user SID と logon SID の一致を確認し、不一致ならその接続を閉じる。
  client は `SECURITY_IDENTIFICATION` を指定して pipe を開き、偽 server が
  client のセキュリティコンテキストを impersonate できないようにする
- **6.4.1c pipe DACL の対応アプリ範囲（AppContainer 非対応）** — pipe DACL
  （`ipc/src/NamedPipeTransport.cpp`）には capability ACE
  （`ALL APPLICATION PACKAGES` / パッケージ SID）を**意図的に含めない**。
  AppContainer トークンは dual-principal であり、許可は user / logon SID と
  Package / Capability SID の intersection で決まる。logon SID しか許可しない
  現行 DACL では intersection が空になり、**AppContainer プロセスに in-proc
  ロードされた TIP は pipe へ接続できない**（`ERROR_ACCESS_DENIED`）。preedit は
  TIP 内ローカルで出るが、Host 経由の候補は来ない。
  これは実装の欠落ではなく**スコープ判断**である。MVP が入力先として保証するのは
  Win32 デスクトップアプリであり、パッケージ化された UWP / Microsoft Store アプリは
  対象外と確定している（`docs/sideload-packaging-spec.md` §0.1 / DEV-783）。
  除外の境界は AppContainer を使うかどうかではなくパッケージ化された UWP / Store
  アプリかどうかで引くため、**Edge は対象内**である（renderer は AppContainer だが、
  入力欄をホストするのは Win32 プロセス）。
  capability ACE の設計と、それに伴う接続元の信頼境界の見直しは、Store / UWP 入力を
  実装する v1.0 以降のトラックで扱う（DEV-555）。**MVP では追加しない。**
  なお §6.4.4 の Handshake トークンも、格納先 `%LOCALAPPDATA%` を読めない
  AppContainer プロセスを結果的に排除する。本項の DACL による排除とは独立した
  別レイヤであり、どちらか一方を緩めても AppContainer 入力は成立しない
- **6.4.2 接続インスタンス上限** — `PIPE_UNLIMITED_INSTANCES` を使わず、
  TIP と設定 UI などの同時接続を許容する有界な上限
  (`kMaxPipeInstances = 32`) を設ける
- **6.4.2a クライアントごとの未完了要求上限** — `RequestScheduler` は
  `client_id` ごとに未完了要求とキャンセル状態をそれぞれ最大64件まで保持する
  （`kMaxPendingRequestsPerClient`）。同一IDの並行要求も未完了件数に加算する。
  空の `client_id` は protocol-v1 の共有名前空間として同じ上限を適用する。
  上限到達時の新規要求は推論を開始せず、`QueryCandidates` は空候補、
  `QueryBatchConversion` は `canceled=true` で応答する。拒否した要求は
  latest ID と既存要求の完了状態を変更しない。
  状態数が上限に達し到着要求のIDが未登録なら、IDの大小にかかわらず非実行中の
  Cancel を回収してから受付を判定する。追跡中の要求と到着要求に一致する
  先行 Cancel は維持する。保持済みIDへの Cancel は常に反映し、状態数が上限に達した
  ときの未知IDへの先行 Cancel は保持しない。要求完了と最終接続の切断で
  対応する状態を回収する。
- **6.4.3 最大フレームサイズ = 1 MiB** — `kMaxFrameSize`（`= kMaxJsonInputBytes`、
  `Limits.h`）。4-byte little-endian length-prefix の値が 0 または本上限超過の
  フレームは `ReadEnvelope` / `DecodeLengthPrefixed` が拒否する。候補問い合わせ
  （数 KB）には十分で、長文一括変換（5,000 文字 ≒ 15 KB）も収まる。5 万文字級の
  超長文は分割前提（M58-B、`docs/romaji-batch-conversion-spec.md` と整合）。
  §6.2 の JSON 最大入力長と**同値に固定**し、片方だけ広げない
- **6.4.4 Handshake トークン** — Handshake で共有秘密トークンを検証する
  （`HandshakeRequest.handshake_token` ↔ Host の
  `DispatcherConfig.handshake_token`）。トークンが設定されている場合、Host は
  Handshake 成立まで他メッセージを未認証として type 別エラーで弾く
  （`Dispatcher::RequiresAuthenticatedSession`）。

  **脅威モデルと到達限界（重要）**: 本トークンは**多層防御（defense-in-depth）**で
  あり、**悪意ある同一ユーザープロセスからのなりすましを防ぐ認証ではない**。
  TIP は TSF DLL として任意のアプリプロセスに in-proc ロードされるため、Host と
  「正規の TIP プロセス」を結ぶ事前の信頼チャネルが存在しない。共有秘密を
  どのチャネルで配っても、同一ユーザー権限で動く悪意あるプロセスは正規 TIP と
  同じ経路でその秘密を入手でき（ファイルなら読み取り、env なら環境継承）、同一
  ユーザー内のプロセス同士を秘密だけで区別することは Windows のセキュリティ
  モデル上できない。**同一ユーザー境界の主防御は per-user pipe ACL + SID
  fail-closed（§6.4.1）+ remote 拒否（§6.4.1a）**であり、「どのプロセスが正規 TIP か」
  の信頼境界は TSF 自体が担う。トークンに認証を期待しない。

  **トークンが実際に与える価値**:
  - 非悪意の誤接続（バージョン違い / 別ビルドの Host・TIP / 残存プロセス）を弾く
  - トークン格納先（`%LOCALAPPDATA%`）を**読めない**同一ユーザープロセス、
    具体的には **capability 分離された AppContainer プロセス**（ユーザー通常 SID で
    `%LOCALAPPDATA%` を読めず、パッケージ SID / capability で隔離。DEV-204 関連）を
    結果的に排除する。**低 IL（low integrity）プロセスは排除できない** — Windows の
    整合性レベルは通常 write-up のみ制限し read-up は許すため、低 IL でも同一ユーザー
    DACL のトークンファイルを読めてしまう。この価値は AppContainer 等の capability
    隔離ケースに限る
  - 攻撃の手数を一段増やす speed bump

  **トークンの配布チャネル（v1.0 決定）**: env 事前共有（`AZOOKEY_IPC_HANDSHAKE_TOKEN`）
  は in-proc TIP が Host 起動時の環境を共有しないため production では成立しない。
  そこで Host が起動時に暗号論的乱数 16 byte（hex 32 文字）を生成し、現在ユーザー
  のみが読める `%LOCALAPPDATA%\azooKey\config\ipc-token`（継承を保護した明示 DACL で
  現在ユーザー、SYSTEM、Administrators のみアクセス可能）へ §5.4 の write-then-rename で
  原子的に書き出す。TIP は Handshake
  直前に同ファイルを読みトークンを得る。Host 再起動時はファイルを新トークンで
  上書きし、TIP は**再 Handshake のたびにファイルを読み直す**（M42 再接続時も同様）。
  環境変数 `AZOOKEY_IPC_HANDSHAKE_TOKEN` は開発・テスト用の上書き経路として残す。
  本チャネルは上記の defense-in-depth 価値に見合うものであり、認証の保証は与えない。

  **トークン取得失敗時の拒否**: Host はトークンの生成・書き出し、または token
  ファイルの所有ロック取得に失敗した場合、pipe の受付開始前に error で終了する。
  TIP と各 IPC クライアントは環境変数も token ファイルも使えない場合、Handshake
  を送らず接続を失敗として扱う。per-logon pipe ACL だけへの縮退は行わない。
  トークンが設定されている場合の比較は、値の一致位置を処理時間へ反映しない
  定数時間比較を使う。
- **6.4.5 client cleanup** — 切断済み client を Stop まで保持せず、
  切断検出時に解放する（長時間稼働でのリーク様の蓄積を防ぐ）
- **6.4.6 length-prefix read/write hardening** — Named Pipe は
  `PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT` を使う。server は
  `FILE_FLAG_OVERLAPPED` 付きで instance を作成し、`ConnectNamedPipe` の
  pending accept を `OVERLAPPED` event で待つ。`Stop()` 時は accept / client I/O
  を cancel して、listen 中の thread が無期限に残らないようにする。4-byte
  little-endian length-prefix によってフレーム境界を復元し、`ERROR_MORE_DATA`
  を扱って指定長まで読み切る。フレーム途中の一時的な `ERROR_NO_DATA` は bounded
  retry に留め、フレーム開始前の no-data / zero-byte read は切断扱いにする。
  write 側も `ERROR_PIPE_BUSY` / zero-byte write を bounded retry に留め、
  読まない peer による client thread 滞留を防ぐため blocking flush に依存しない。
  64KiB を超えるフレームが pipe write 単位で分割されても往復できる。
  ただし retry 回数の上限だけでは、pend したまま進まない I/O を縛れない。
  時間の上限は §6.4.7 が担う
- **6.4.7 フレームデッドライン** — retry 回数とは別に、**転送が始まった 1
  フレーム**に monotonic な時間上限を課す。

  **計時の開始点**: 接続が要求と要求の間で idle している間は計時しない
  （TIP は打鍵の合間に分単位で idle し得るため、idle に上限を置くと正常な接続を
  誤って切る）。最初の 1 chunk が 1 byte 以上を転送した時点で arm し、以降は
  ヘッダと本文を通じた 1 つの絶対デッドラインとして働く。chunk より細かい粒度は
  観測できない（pending I/O の内部進行は取得できない）が、絶対デッドラインで
  あるため本文を細切れに送り続ける peer も縛れる。

  **値**: ソフト 500ms。ハードは read 2000ms / write 5000ms。write を緩くするのは、
  64KiB の pipe buffer が埋まった後は読まない peer が正当に write を止め得るため
  （TIP がモーダルループ中・デバッガ attach 中など、悪意なしに起こる）。ローカル
  Named Pipe の 1 MiB 転送は健全な peer なら ms オーダーであり、いずれも十分な余裕を持つ。

  **ハード超過時**: 当該接続のみを切断する。部分データからの再開は行わない
  （キャンセルした message-mode read の残りは pipe に残留し、フレーム同期を
  回復する手段がないため）。他のクライアントには波及しない。

  **ソフト超過時**: フレームは破棄せず、遅い接続として計上するだけに留める
  （`NamedPipeServer::SoftDeadlineExceededCount()`）。計上の目的は、閾値に観測可能な
  効果を持たせて回帰テスト可能にすることである。

  **`host-availability-spec.md` §8.5.2 の timeout 表との関係**: 別レイヤであり、値を一致させない。
  `host-availability-spec.md` §8.5.2 は「要求を出してから応答が返るまで」（推論時間を含む）を縛る request
  レイヤの規約で、本項は「動き始めた 1 フレームを転送し切る」ことだけを縛る
  transport レイヤの規約である。推論に要した時間は本デッドラインに算入されない
  （write の計時は handler が応答を返した後に始まる）。ただし**両者が同時に効く
  場面（client の受信）では短いほうが勝つ**。本項のデッドラインが `host-availability-spec.md` §8.5.2 の
  wall-clock deadline を延長することはない（優先関係は §6.4.8）。

  **適用範囲**: server 側と `NamedPipeClient` の双方に適用する。どちらも
  `FILE_FLAG_OVERLAPPED` 付きのハンドルを使い、本項の計時規約（idle は計時せず、
  最初の 1 byte で arm し、ハード超過で当該接続を切る）を共有する。client 側は
  受信 API が引数でタイムアウトを取るため、その引数と本項のデッドラインの
  役割分担を §6.4.8 で定める。

  **残余リスクと評価結果（DEV-644 で確定 / 2026-08）**: 何も送らない idle 接続を
  `kMaxPipeInstances`（32）本張って枯渇させる DoS は本項では防げない。評価の結論は
  **対処しない**である。理由は 2 つある。第一に、これは同一ユーザー権限で動くプロセスを
  前提とし、§6.4.4 の脅威モデルでは per-user pipe ACL の主防御対象外に置いた範囲そのもの
  である。第二に、idle に上限を置かない判断は本項の計時規約と同じ根拠に立つ。TIP は打鍵の
  合間に分単位で idle し得るため、idle を縛る keepalive デッドラインは正常接続の誤切断
  リスクだけを増やす。

  将来この判断を覆すなら、**接続後 Handshake 未了の猶予**（例 5s で切断）を第一候補とする。
  §6.4.4 のトークンが未設定でも Handshake 自体は必須であり、idle 全体ではなく「まだ
  Handshake していない接続」だけを縛れるため、正常な idle を誤って切らない。
  一方、`kMaxPipeInstances` の一定数を Handshake 済み接続に予約する案は採らない。
  未 Handshake 枠が埋まれば正規 TIP の新規接続も同じように阻まれ、枯渇の窓が狭まるだけで
  塞がらないためである。
#### 6.4.8 client 受信の二相セマンティクスと deadline の優先関係

`NamedPipeClient` の受信は性質の異なる 2 つの待ちを 1 回の呼び出しに含む。
相ごとに上限を分けるが、**どちらの相も request レイヤ（`host-availability-spec.md` §8.5.2）の
wall-clock deadline を超えて待ってはならない**。

**スライスと request deadline は別物**: TIP の応答待ちループは
50ms 程度のスライスで受信 API を繰り返し呼びながら、その外側で
150ms（QueryCandidates fast）のような絶対 deadline を持つ
（`tsf-tip/src/TextService.cpp`）。スライス幅は Cancel 転送や停止確認を
差し挟むための刻みであって、要求 1 件の許容時間ではない。したがって
受信 API は **スライス幅と request の残デッドラインを別の引数として受け取る**
（1 引数形は「両者が等しい単発呼び出し」と定義する。実装形は DEV-744）。
呼び出し側が絶対 deadline を持たない場合は「なし」を渡せるものとし、その場合の
相 B は read ハードのみで縛る。`host-availability-spec.md` §8.5.2 に値を持つ要求（Ping・`Health`・
QueryCandidates / ライブ変換・再変換・一括変換・外部 AI 呼び出し）と M58-B の `T_sub` は、
いずれも request deadline として渡す。

**相 A（フレーム到着待ち）**: 1 byte も届いていない間の待ち。上限はスライス幅。
超過時は `nullopt` を返し、接続は維持し、pipe からは何も取り出さない。
呼び出し側は同じ応答を次の呼び出しで受け取れる。相 A のタイムアウトは異常では
なく通常の制御フローであり、`docs/romaji-batch-conversion-spec.md` の
stale 応答 drain も「タイムアウトは応答を消費しない」というこの性質に依存する。

**相 B（フレーム転送中）**: 最初の 1 byte が到着した時点で相 B に入る。上限は
**`min(request の残デッドライン, §6.4.7 の read ハード)`**。スライス幅では
縛らない — 1 MiB までのフレーム（`kMaxFrameSize`）が 64KiB の chunk
（`kPipeBufferSize`）に分割されて届き得るため、スライス境界とフレーム境界は
一致せず、スライスで打ち切ると健全な応答を運んでいる接続を切ることになる。
一方 request の残デッドラインで縛るのは打ち切って正しい: その時刻を過ぎた
応答は `host-availability-spec.md` §8.5.2 の契約上すでに失敗であり、呼び出し側は Cancel と fallback へ
進む。read ハードは「壊れた peer に対する上限」であって request deadline の
延長を許す根拠にはならない。

**相 B の超過**: §6.4.7 と同じく当該接続を切断して `nullopt` を返す
（以後 `IsConnected()` は false）。部分的に読み出したフレームからは同期を
回復できないため、接続を維持したままの再開はしない。**したがって request
deadline 超過のうち、フレームが転送途中だった場合だけは接続断（→ M42 の
再接続 / 劣化モード）を伴う。** 送信が始まってもいない遅い Host は相 A で
タイムアウトするため接続は維持され、従来どおり stale drain で処理できる。

**有界性**: 受信 1 回の所要は `min(request の残デッドライン, read ハード)` を
上限に有界。request deadline を渡さない呼び出しでも read ハードで有界。

**タイムアウトを取らない API**: `Receive()` は相 A を無期限に待ち（idle な
接続は正常）、相 B は §6.4.7 の read ハードデッドラインで縛る。`Send()` は
同項の write ハードデッドラインで縛る。

### M40 受け入れ条件

受け入れ条件の定義の正典は [`plans/windows-port-roadmap.md`](../plans/windows-port-roadmap.md)
の M40 節とする。本書は §6 で JSON パーサと Named Pipe transport の境界値・タイムアウトを定義し、
受け入れ条件を複製しない。
