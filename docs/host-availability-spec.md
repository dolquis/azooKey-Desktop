# Host 可用性・再接続 仕様（M42・M47）

対象リポジトリ: dolquis/azooKey-Desktop
対応マイルストーン: M42（§8）、M47（§8.5）
関連: `docs/dev-infrastructure-spec.md`（開発基盤の入口と転送表）、
      `docs/ipc-persistence-hardening-spec.md`（IPC の堅牢化）、
      `docs/observability-spec.md`（構造化ログ）、
      `docs/windows-tsf-host-architecture.md`（Host の起動と再起動）、
      `plans/windows-port-roadmap.md` M42 / M47

本書は `docs/dev-infrastructure-spec.md` から §8を切り出した文書である。節番号 §8.x は旧 spec のまま保ち、旧 spec の転送表が§8を本書へ解決する。本書内で、本書にない節（旧 spec の他の節）を指すときは、文書名を付けて書く（例: `build-ci-spec.md` §4.3）。

## 正典範囲

TIP と Host の接続状態機械、再接続と劣化モード、Host 世代 ID（§8.1〜§8.4）、M47 の復旧 UX の状態機械と timeout 表（§8.5）、
推論 Worker のプロセス分離の設計案（§8.6）は本書が正典である。

## 節とマイルストーンの対応

| 節 | マイルストーン |
|---|---|
| §8（§8.5・§8.6 を除く） | M42 |
| §8.5 | M47 |
| §8.6 | M に属さない設計案（roadmap「リスクと不確実性」） |

## 8. Host 可用性・再接続（M42）

### 8.1 解決する問題

Host が停止した際の TIP 側再接続戦略が限定的で、入力中に候補更新が停止する
恐れがある。常駐方法（Run キー）だけではアップデート時・クラッシュ時の復旧
が弱い。

### 8.2 接続状態機械

TIP 側 IPC ワーカー（`TextService::IpcWorkerThread`）に
明示的な接続状態機械を導入する:

```
Disconnected ─→ Connecting ─→ Handshaking ─→ Ready
     ↑              │              │           │
     └──────────────┴──────────────┴───────────┘ (失敗・切断時)
Ready ─→ Degraded （deadline 超過の連続時。§8.3）
Degraded ─→ Ready （応答の再開時）
Degraded ─→ Disconnected （切断時）
```

各状態遷移はログ（`observability-spec.md` §7）に記録する。

遷移は次の 13 本に限る（実装は `tsf-tip/src/IpcConnectionState.cpp` の遷移表）。
表に無い (状態, イベント) の組は拒否し、状態を変えない。自己遷移は持たない。

| From | イベント | To |
|---|---|---|
| `Disconnected` | `connect_started` | `Connecting` |
| `Connecting` | `pipe_connected` | `Handshaking` |
| `Connecting` | `connect_failed` / `stopped` | `Disconnected` |
| `Handshaking` | `handshake_accepted` | `Ready` |
| `Handshaking` | `handshake_failed` / `stopped` | `Disconnected` |
| `Ready` | `connection_lost` / `stopped` | `Disconnected` |
| `Ready` | `response_deadline_exceeded` | `Degraded` |
| `Degraded` | `response_restored` | `Ready` |
| `Degraded` | `connection_lost` / `stopped` | `Disconnected` |

状態機械に載せるのは primary 接続だけである。out-of-band Cancel 用の短命な control 接続は
遷移を起こさない。設定変更時に確立済み接続の上で再実行する Handshake は `Connecting` /
`Handshaking` を経由せず、成功は応答として扱い（§8.3。`Degraded` なら `response_restored`）、
拒否は `connection_lost` とする。

遷移ログのイベント名は `ipc_connection_state_transition` で、フィールドは `from` / `to` /
`transition_event`（上表の固定語）、`attempt`（直近の `Ready` 以降の何回目の接続試行か。1 始まり）、
`process_id`（TIP をロードしたアプリの OS プロセス ID）だけとする。
入力本文を載せるフィールドは持たない。Host 停止中や Handshake を拒否され続ける間は、backoff ごとに
`Disconnected` / `Connecting` / `Handshaking` の間の遷移を繰り返す。この 3 状態の間の遷移は
`attempt` が 1 または 2 のべき乗のときだけ記録し、同じ試行の遷移はまとめて記録するか省く。
`Ready` / `Degraded` に出入りする遷移は毎回記録する。
表に無い組を受けたときは `ipc_connection_state_rejected` を記録する。

Handshake 応答には protocol v1 の省略可能フィールド
`host_generation_id: string` を含める。Host はプロセス起動時にランダムな UUID を
1 回生成し、同じ Host プロセスが受け付ける primary / control を含む全接続へ同じ値を返す。
旧 Host がこのフィールドを省略した場合、TIP は空文字として扱い、接続自体は拒否しない。

TIP は初めて非空の世代 ID を観測したときは保存だけを行う。同じ世代への再接続では
保留中の要求を維持する。既知の世代 ID から別の値（省略を含む）へ変化した場合は、
`Handshaking` から `Ready` へ遷移する前に request ID を更新して旧世代の in-flight 応答を
stale 化する。未送信または接続断後に再武装された pending 要求と候補表示待機状態は維持し、
新世代へ新しい request ID で再送する。候補キャッシュは世代変更時に破棄する。
現行の IPC worker は Handshake と応答処理を同一スレッドで直列化しているため、世代変更と
応答反映は並行せず、request ID の不一致が旧世代応答の実効的な破棄経路になる。
世代の初回観測と変化は構造化ログへ記録する。

### 8.3 再接続と劣化モード

- **Exponential backoff + jitter** で再接続を試行する。待機時間は decorrelated jitter で
  決め、各回を `[下限, min(前回の 3 倍, 上限)]` から一様に引く。下限は 250ms、上限は 3s とし、
  接続が確立したら次の停止は下限の窓から数え直す。乱数源は TIP インスタンスごとに別の
  seed を持ち、複数のホストアプリに読み込まれた TIP が同じ周期で再接続しないようにする。
- Host から一定時間応答がない場合（下記の deadline 超過が連続した場合）、TIP は
  `Degraded` 状態へ移行し、下記の TIP 内ローカル候補で入力継続を保証する。Host 復帰後は
  `Ready` へ戻す。
- TIP 内のローカルフォールバック候補は、TIP 自身がローマ字→かな変換した読みそのもの
  （ひらがな）と、`core::ExpandKatakanaCandidates` による全角・半角カタカナとする。
  いずれも `source` は `fallback` で、TIP 内で完結するため辞書や Host を要さない。
  通常入力の Space では、数字リライター等の TIP ローカルのリライターが有効ならその候補も後ろに並ぶ。
  `core::SimpleConverter` の組み込み辞書は試験用の数語しか持たないため TIP には載せない。
  - 次のいずれかの間、TIP は Host の候補を待たず Space でこの候補を即時に表示する。
    直近の接続・Handshake が失敗した、確立済みの接続が切れた、`Degraded` に入った。
    `Ready` へ戻るとこの扱いを解く。TIP 起動直後の初回接続前は Host の候補を待つ。
  - 通常入力では、保留中（未送出または再接続待ちで再武装済み）の `QueryCandidates` は
    取り消さず、Host 復帰後に送出して、応答が fresh なら候補キャッシュを更新する。送出済みの
    要求が deadline を超えた場合の `Cancel` は従来どおり送る。一括変換では、接続が無い間は Host 宛ての要求を出さず、読み全体を
    1 文節とするローカル候補を表示する。`Degraded`（pipe は生きている）では最初の Space で
    Host へ要求を送り、その応答で `Ready` に戻れるようにする。一括変換の応答を待つ間に Host が
    使えなくなった場合、または `Degraded` で応答を待っている場合は、次の Space で待機中の要求を
    取り消してローカル候補を表示し、Enter で読み（かな）をそのまま確定する。
  - `QueryCandidates` の deadline 超過、一括変換の失敗、Host が候補の無い文節を返した場合も
    同じ候補を使う。
  - 確定した候補の `source` が `fallback`（一括変換では文節のいずれか）なら、その確定の
    `CommitObservation` は送らない。Host が候補の無い文節に補う `fallback` も同じ扱いとする。
- 「Host から一定時間応答がない場合」には、pipe 接続自体は維持されているが
  `QueryCandidates` / `Health` の有効応答が deadline 内に返らない
  connected-but-silent 状態を含める。TIP は pipe 切断を待たず、処理種別ごとの
  deadline で劣化判定する。
- ヘルス監視は既存 `Health` メッセージを流用し、定期的に往復確認する。
  - primary 接続で送るものが無い状態が監視間隔（既定 5s）続いたときだけ `Health` を 1 回送る。
    入力中の要求とは同じ接続で直列に扱い、監視を割り込ませない。
  - 応答待ちは §8.5.2 の IPC Ping と同じ 500ms を上限とする。待機中に `QueryCandidates`、
    送信キューの項目、設定変更による Handshake の再実行要求が入ったら、待機を打ち切って
    そちらを先に処理する。打ち切った `Health` の遅れた
    応答は request ID 不一致として読み捨て、stale 応答のログは出さない。
  - 応答が上限内に返らなければ `ipc_health_timeout` を記録する。
  - `Health` の送信失敗や監視中の切断は `ipc_health_send_failed` を記録し、`connection_lost` として
    再接続へ戻す。これにより、idle 中に Host が停止した場合も次のキー入力を待たずに検知する。
- 劣化判定は処理種別ごとの deadline 超過を共通に数える。`Health` の無応答と
  `QueryCandidates` の deadline 超過（§8.5.2 の打鍵ごと・ライブ変換 150ms、候補窓用 750ms）は
  どちらも 1 回の超過として `ipc_host_deadline_missed` に記録し、連続 2 回で `Ready` から
  `Degraded` へ遷移する（`response_deadline_exceeded`）。入力し続けて idle にならない間も、
  この経路で劣化を判定する。TIP が `Cancel` した候補窓用の要求（新しい要求や確定による。§8.5.2）は、
  `Cancel` が main pipe 経由になって deadline まで待った場合も、この回数に含めない。
  一括変換の deadline 超過は接続を切って再接続へ戻すため、この回数には含めない。
- `Health`、`QueryCandidates`、一括変換、設定再読込の Handshake のいずれかに応答が返った時点で、
  連続回数を 0 に戻し、`Degraded` なら `Ready` へ戻す（`response_restored`）。`Health` の
  `status` が `degraded` / `error` でも応答として扱う。

### 8.4 本マイルストーンの範囲

接続状態機械・backoff 再接続・ヘルス監視・劣化フォールバックまでを範囲と
する。Host の Windows サービス化や監視ランチャ導入は将来課題とし、本仕様
では扱わない。

### M42 受け入れ条件

受け入れ条件の定義の正典は [`plans/windows-port-roadmap.md`](../plans/windows-port-roadmap.md)
の M42 節とする。本書は §8.2〜§8.4 で接続状態機械・再接続・劣化モード・Host 世代 IDを定義し、
受け入れ条件を複製しない。

### 8.5 M47 拡張: ユーザー可視復旧 UX

M42 は IPC transport 層の状態機械と劣化フォールバックまでを範囲とした。
M47 はその上に乗る**ユーザー可視 UX レイヤ**を扱う。Zenzai モデル単位の
劣化（モデル未配置 / ロード失敗）、推論 timeout の要求単位の fallback、連続クラッシュ時の
SafeMode を導入する。

#### 8.5.1 拡張状態機械

M42 の `Ready` / `Degraded` を更に細分化する:

```
Healthy
  ↓ Host no response (M42 transport)
DegradedSimple   ← Host 不調。TIP 内ローカル候補（§8.3）で継続
  ↓ reconnect success
Recovering
  ↓ health ok
Healthy

Healthy
  ↓ model load failed
DegradedModel    ← Host は健康だが Zenzai 無効、辞書 + 学習 + Simple
  ↓ reload success
Healthy

Any
  ↓ repeated crash (Host process が N 回連続クラッシュ)
SafeMode         ← AI / 学習 / 外部 API を全停止、最小限の入力のみ
```

| 状態 | 説明 | ユーザー影響 |
|---|---|---|
| `Healthy` | Host + Zenzai 正常 | 通常 |
| `DegradedSimple` | Host 不調（pipe 切断または connected-but-silent）。TIP 内ローカル候補（§8.3）で継続 | 変換品質低下 |
| `DegradedModel` | Host は正常だが Zenzai 無効（モデルのロード失敗）。Host の辞書 + 学習 + SimpleConverter で継続 | 変換品質低下 |
| `Recovering` | 再接続 / 再ロード中 | 一時的に候補更新遅延 |
| `SafeMode` | 連続クラッシュにより AI / 学習を停止 | 安定優先 |

遷移トリガと駆動 timeout（§8.5.2）を一覧化する。`request_id` / `trace_id`
（`observability-spec.md` §7.3）で staleness を判定し、状態遷移は `observability-spec.md` §7 のログに記録する。`DegradedSimple` /
`RecoveringTransport` を通る行は、TIP が合成する表示状態（下記「ユーザー可視状態の合成」）の
遷移であり、Host の状態機械はこれらの行を通らない。

| From | To | トリガ | 駆動 timeout / 閾値 |
|---|---|---|---|
| `Healthy` | `DegradedSimple` | Host 無応答（pipe 切断 or connected-but-silent）。TIP が §8.3 の劣化判定で判定する。一括変換の deadline 超過は接続を切るため pipe 切断として扱う | `Health` 500ms / `QueryCandidates`（打鍵ごと 150ms、候補窓用 750ms）の deadline 超過が連続 2 回（§8.3） |
| `Healthy` | `DegradedModel` | Zenzai モデル load 失敗 | Model load 30s |
| `DegradedSimple` | `Recovering`（transport 復旧） | 再接続成功（pipe 再確立 + Handshake 受理） | exponential backoff（§8.3） |
| `DegradedModel` | `Recovering`（model 復旧） | `LoadModel` または `UpdateConfig` によるパス付きの再ロード開始（完了ではない） | Model load 30s |
| `Recovering`（transport 復旧） | `Healthy` | Handshake 受理後に TIP が Host の状態（`QueryDiagnostics.fallback_state`）を受け取る | `QueryDiagnostics` 500ms 以内 |
| `Recovering`（model 復旧） | `Healthy` | 再ロードが成功し `model_loaded` が真になる（Host の `InferenceEngine` が判定する。再ロードの受理や `LoadModelResponse.ok` だけでは遷移しない） | Model load 30s |
| `Recovering`（transport 復旧） | `DegradedSimple` | 再接続失敗 / 再 timeout | 同上 timeout 再超過 |
| `Recovering`（model 復旧） | `DegradedModel` | 再ロード失敗 / timeout / `model_loaded==false` | Model load 30s 超過 |
| `Any` | `SafeMode` | Host プロセスが 60s 以内に 3 回連続クラッシュ（§8.5.3） | crash カウンタ ≥3 / 60s |
| `SafeMode` | `Healthy` | ユーザーが手動解除（`settings.safeMode.enabled=false`） | 手動のみ（自動復帰しない） |

`Recovering` は突入元（transport / model）を保持し、退出条件は元の劣化種別に対応する
ものだけを満たす。model 復旧中は pipe が生きていても（Ping OK）モデル未ロードのうちは
`DegradedModel` に留め、`degraded-model` UI（§8.5.4）を消さない。

遷移表は `inference-host/src/HealthStateMachine.cpp` が持つ。`Recovering` は突入元ごとに
`RecoveringTransport` / `RecoveringModel` の 2 値で表し、ログの状態名は `healthy` /
`degraded_simple` / `degraded_model` / `recovering_transport` / `recovering_model` /
`safe_mode` とする。表に無い (状態, イベント) の組は拒否して状態を変えず、自己遷移は持たない。
遷移は `health_state_transition`（`from` / `to` / `event`）、拒否は `health_state_rejected`、
設定の `safeMode.enabled` に合わせて `SafeMode` に置いた場合は `health_state_restored` として記録する。

状態機械の遷移のうち、Host が自分で観測できるものは Host が駆動する。

- model 系: `InferenceEngine` がモデルロードの結果で駆動する。モデルを持たない状態でロードに
  失敗すると `DegradedModel`、`DegradedModel` でパス付きのロードを始めると `RecoveringModel`、
  そのロードが成功して `model_loaded` が真になると `Healthy`、失敗すると `DegradedModel` へ戻る。
  別のモデルが動いたまま差し替えだけが失敗した場合は劣化として扱わない。
- 推論の失敗と打ち切りは、単発でも連続でも Host の状態機械を変えない（model 系の対象外）。
  ロード済みモデルの推論が 1 変換ハード予算（`docs/zenzai-inference-spec.md` §8.2 の 600ms。
  上限の設計値は §8.5.2 の 800ms）を超えた要求は、その時点の best-so-far beam があれば
  Zenzai の候補として返し、劣化として扱わない。例外・空生成・best-so-far の無い打ち切りで
  失敗した要求だけを SimpleConverter の候補へ落とす（`ZenzaiModelConverter` の fallback。
  同 §6.4 / §9.2.2）。次の要求は再び Zenzai で処理する。IPC の deadline 超過は transport 系
  （§8.3 の劣化判定）が扱い、本項の対象ではない。状態機械に載せない理由は次のとおり。
  - 打ち切りの多くは best-so-far を返す正常な打ち切りであり、回数を数えても
    品質の低下と対応しない。
  - `degraded_model` の文言（§8.5.4）は Zenzai を使えない状態を示し、[再試行] はモデルの
    再ロードである。モデルが動いていて遅いだけの状態には文言も対処も合わない。
  - `RecoveringModel` は再ロードを突入の契機とする。推論の成功で `Healthy` へ戻る経路を
    足すと、突入元ごとに退出条件を対応させる規則が崩れる。

  推論の hard failure は `model_runtime_error_` を通じて `Health.status=degraded` と
  `last_error` に出る（次の成功変換で消える）。best-so-far を返した打ち切りは
  `Health` にも `QueryDiagnostics` にも出ず、頻度は所要時間（M51 の trace の
  `model_inference`（`observability-spec.md` §7.7.2）、ETW の Converter phase）から推定する。`fallback_state` の導出（下記）は状態機械の状態と設定だけから決まるため、
  推論の失敗で `fallback_state` と状態機械が食い違うことはない。推論の遅延を利用者へ
  示す必要が生じた場合は、本項への状態や行の追加として新規課題で扱い、本項の判断を
  暗黙に覆さない。
- `SafeMode`: Host が起動時に §8.5.3 の条件で入り、`UpdateConfig` で
  `settings.safeMode.enabled=false` を読んだときに `Healthy` へ戻る。起動時や `UpdateConfig` で
  `enabled=true` を読んだときは、突入の経路によらずその値に従って `SafeMode` に置く
  （`health_state_restored`）。フラグが真である間の強制事項は §8.5.3 が定める。

transport 系（`Healthy` → `DegradedSimple` → `RecoveringTransport` → `Healthy`）は Host が
自分で観測できない。応答しない Host は自分の無応答を報告できず、1 つの Host に複数の TIP が
接続して観測が食い違いうるためである。したがって Host は transport 系のイベント
（`HostUnresponsive` / `TransportReconnected` / `HostHealthConfirmed` / `TransportRecoveryFailed`）を
発行せず、Host の状態機械が `DegradedSimple` / `RecoveringTransport` に入る経路は無い。
transport 系は M42 の接続状態機械（§8.2）が担い、TIP 側に `HealthStateMachine` を複製しない。

##### ユーザー可視状態の合成（TIP）

候補ウィンドウのインジケータ（§8.5.4）が表示する状態は、TIP が自分の接続状態と Host の報告から
次の順に合成する。上の行が成り立てば下の行は見ない。

| 条件 | 表示する状態 |
|---|---|
| §8.3 のローカル候補を即時に表示する条件（直近の接続・Handshake の失敗、確立済み接続の切断、`Degraded`）が成り立つ | `degraded_simple` |
| `Ready` に戻ってから Host の状態を受け取るまで（`RecoveringTransport` に当たる） | 直前の表示を保つ |
| 上記以外 | Host が報告した `fallback_state` |

TIP 起動直後の初回接続前は §8.3 と同じく劣化扱いにせず、何も表示しない。

TIP は Host の状態を primary 接続の `QueryDiagnostics`（`diagnostics-compat-spec.md` §12.6）で取得し、`fallback_state`
だけを使う。IPC の payload は追加しない。送る契機は次の 2 つとする。

- Handshake が受理された直後。初回接続、再接続、設定変更による Handshake の再実行（§8.2）を含む。
  Host が `settings.json` へ `safeMode` を書くと TIP の設定監視が Handshake をやり直すため、
  SafeMode への突入もこの契機で観測できる。
- `Health`（§8.3）の応答の `status` または `model_loaded` が直前の応答から変わったとき。

`QueryDiagnostics` は入力中の要求と同じ接続で直列に扱い、応答待ちの上限と、入力の要求が
来たときの打ち切りは §8.3 の `Health` と同じ規則に従う。取得に失敗した場合と入力の要求で
打ち切った場合は表示を変えず、次に primary 接続が監視間隔（§8.3）の間 idle になったときに
送り直す。取得できるまで送り直しを続け、上の 2 つの契機を待たない。
応答の `loaded_model_path` / `last_error` などの文字列は、TIP の表示にもログにも使わない。

`HealthPayload.status` は Host が `last_error` から決める粗い値である。`last_error` が無ければ
`ok`、あってモデルがロード済みまたはパスが設定済みなら `degraded`、それ以外は `error` とする。
TIP はこの値を状態の取得契機にだけ使い、表示する状態を直接決めない。
この規則は `docs/zenzai-inference-spec.md` §7.4・§9.2.1 から参照される。

##### `fallback_state` の導出（Host）

Host は `QueryDiagnostics.fallback_state` を状態機械の状態と設定から次の順に決める。
上の行が成り立てば下の行は見ない。

| 条件 | `fallback_state` |
|---|---|
| 状態機械が `SafeMode` | `safe_mode` |
| `model.enabled=false`（SimpleConverter 固定が意図された設定） | `healthy` |
| 状態機械が `DegradedModel` / `RecoveringModel` | `degraded_model` |
| 状態機械が `Healthy` | `healthy` |

モデルの無効化は model 系の遷移を起こさない。状態機械が `DegradedModel` のまま無効化されても、
2 行目により `fallback_state` は `healthy` になる。`Healthy` のまま進むモデルのロード
（起動直後の初回ロードを含む）と、
GPU backend のロードに失敗して CPU backend で動いている状態は Zenzai が使えるか使える見込みがあるため
`healthy` とし、backend の違いは `QueryDiagnostics.backend` で示す。`degraded_simple` は Host が
自分で返す値ではなく、Host に到達できないときに client 側（TIP の合成、`azookey_diag` の D-009）が
使う値とする。

#### 8.5.2 timeout 表

`observability-spec.md` §7.5 の request レイヤ deadline を処理種別ごとに定める:

| 処理 | timeout |
|---|---:|
| IPC Ping / `Health`（TIP の監視は `Health` を使う。§8.3）/ TIP の `QueryDiagnostics`（§8.5.1） | 500ms |
| `QueryCandidates` fast / ライブ変換（`QueryLiveConversion`。M14 以前の Host へは `QueryCandidates` の `live=true` で代替する）/ `QueryPredictions` | 150ms |
| 候補窓用の `QueryCandidates`（`live = false`。Space の候補窓と絵文字の一覧） | 750ms |
| 再変換（`live=true` の `QueryCandidates` と読みの逆引き） | 750ms |
| 一括変換（`QueryBatchConversion`）の neural | 30s |
| 一括変換の ai-cleanup（外部 AI を使わない経路。`local-zenzai`、または openai でも外部不許可） | 35s（独立した 30s の全体期限 + 余裕 5s。`docs/ai-backend-spec.md` §7.1） |
| 外部 AI 呼び出し（M16 Magic Conversion / M58-C ai-cleanup の openai backend） | `openAiTimeoutMs`（既定 30s）+ 余裕（TIP は 5s を足す）。正典は `docs/ai-backend-spec.md` §7.1 |
| （設計値）ローカル Zenzai 1 変換のハード予算の上限 | 800ms。変換要求（`QueryCandidates` / 一括変換）の request deadline ではない（下記） |
| ローカルの非 Cleanup AI 変換（`local-zenzai` / 外部 AI 不許可 / Lint。Host の `AiBackend`） | 800ms（Host が強制する deadline。超過は `Timeout`） |
| ModernBERT scoring（M57） | 30〜50ms |
| Model load | 30s |

timeout は request 送信または backend 処理開始からの wall-clock deadline として扱う。
connected-but-silent Host でも、pipe 切断や blocking read の解除を待たない。

本表は Host 内の推論予算とも別レイヤである。Host の `InferenceEngine` は 1 変換ごとに
`docs/zenzai-inference-spec.md` §8.2 の 1 変換ハード予算（600ms）を設定し、超過時は
best-so-far を返す。表の「ローカル Zenzai 1 変換のハード予算の上限」800ms は、この予算の
上限を決める設計値であり、変換要求（`QueryCandidates` / 一括変換）に対しては実行時に強制される
request deadline ではない。ローカル Zenzai の変換が 800ms で client から打ち切られるわけではなく、client が待つ時間は要求の種別ごとの上の行
（fast / ライブ変換 150ms、候補窓用 750ms、再変換 750ms、一括変換 30s）が決める。600ms を上限に揃えない根拠は
同 §8.2 の上限と下限の制約である。

ライブ変換は `QueryCandidates` fast と同じ 150ms を TIP の 1 つの定数で待つ。
`QueryCandidates` fast は打鍵ごとの `live = true` の要求である。
Space で候補窓を開く `live = false` の要求（`legacy-parity-spec.md` §1.2）は、Zenzai の複数候補、
typo 提案、英単語候補、NLL rerank を含み、打鍵ごとの要求より重い。
150ms で打ち切ると、CPU のみの機械やバッテリ駆動時に窓が fallback 候補になり、超過の連続で
Host が生きていても `Degraded` に入る。そのため候補窓用の要求は再変換と同じ 750ms で待つ。
750ms は Host の 1 変換ハード予算（600ms）の外側にあり、正常な Host では超過しない。
したがって候補窓用の要求の超過も §8.3 の劣化判定に数える。
候補窓用の要求が in-flight の間に新しい変換要求（打鍵ごとの要求、ライブ変換、別の候補窓用の要求）を
出すときは、in-flight の候補窓用の要求に `Cancel`（`observability-spec.md` §7.5）を送る。その応答は新しい要求により
stale になっており、待ち続けると新しい要求が候補窓用の deadline まで遅れるためである。
in-flight の要求が打鍵ごとの要求、`QueryPredictions`、再変換のときは `Cancel` を送らない。
候補表示レイテンシの性能目標（`build-ci-spec.md` §10 の p50 < 80ms）は目標値であり、打ち切りの deadline ではない。

Host は `QueryCandidates` fast、候補窓用の `QueryCandidates`、ライブ変換の値を自分の deadline として持たない。
これらは client 側の deadline と Cancel（`observability-spec.md` §7.5）が打ち切る。
Host 側に置かない理由は次のとおり。

- 再変換も `live=true` の `QueryCandidates` を使い、client はそれをライブ変換より長く待つ。
  Host は payload から、再変換とライブ変換用の `live=true` の要求を区別できない。
- Host の予算は推論の開始から数え、client の deadline は送信から数える。同じ値を
  Host に置いても、client の打ち切りより先に効く場面は Host が負荷で遅れたときに限られる。
  その場面では、打ち切りが候補を途中で切った出力に変える。

本表は request レイヤの値であり、transport のフレームデッドライン（`ipc-persistence-hardening-spec` §6.4.7。
read 2000ms / write 5000ms）とは別レイヤである。本表の値を変えても `ipc-persistence-hardening-spec` §6.4.7 は
追従せず、逆も同様。フレームデッドラインには推論時間が算入されないため、
一括変換 30s や Model load 30s と比較して短いことは矛盾しない。
値は独立だが**優先関係はある**: client の受信で両者が同時に効く場合、本表の
deadline がフレームデッドラインより短ければ本表が勝つ（`ipc-persistence-hardening-spec.md` §6.4.8）。上記の
「blocking read の解除を待たない」を transport 側が破らないための規約である。

timeout 時は Cancel を送信し、古い結果は staleness check（M10）で
破棄する。`request_id` と `trace_id`（M51）で staleness を判定する。
Cancel / deadline は Dispatcher の応答抑止だけでなく、converter / reranker /
backend 推論処理まで伝播させる。重い処理が deadline 後も継続し、IME 側の
fallback や次リクエスト処理を妨げる設計は不可とする。

#### 8.5.3 SafeMode 突入条件

直近 60 秒以内に Host プロセスが 3 回連続でクラッシュした場合、Host は
`SafeMode` に入り、ユーザーが解除するまで AI / 学習を強制 OFF にする。突入はユーザーへ
通知して解除手順を案内する（通知の契機と表示は §8.5.4）。

クラッシュは Host 自身が次の起動時に検知する。TIP は Host の終了コードを観測できない
ためである。pipe モードの Host は起動時に `%LOCALAPPDATA%\azooKey\data\host_run_state.txt`
へ実行中の印（自プロセスの PID と作成時刻）を書き、正常な終了経路（停止要求、supervisor の
終了、pipe の listen 失敗）で印を消す。起動時に印が残っていれば、前回の Host は終了経路に
到達せずに終わったものとして、その検知時刻を 1 回のクラッシュとして記録する。記録した
PID と作成時刻の両方が一致するプロセスがまだ生きている場合は、別の Host が pipe を持っている
重複起動であり、クラッシュとして数えず印にも触れない（PID だけが一致するのは無関係な
プロセスによる再利用であり、前回の Host は終わっている）。印の読み取りから書き込みまでと、
終了時の消去は、ファイルロックの下で行う。終了時は印が自プロセスのものであるときだけ消す。
ログオフやシャットダウンで Host が終了経路に達する前に止められた場合も 1 回と数える。
pipe を取れない側の Host が先に印を書いた同時起動では、pipe を持つ側の Host に印が残らず、
その Host の異常終了は数えない。通常の起動は supervisor が 1 つに絞るため、この組み合わせは
supervisor を介さない手動起動に限られる。解除の手順は `docs/debugging.md`「典型トラブル」に置く。正常終了は記録を空にするので、数えるのは直近の
正常終了以降の連続クラッシュだけとなる。60 秒の窓に 3 回目が入った起動で `SafeMode` に入る。
前回のクラッシュを検知した起動は `host_previous_run_crashed`（`recent_crashes`）、突入は
`safe_mode_entered`（`result` / `crash_count`）、印の書き込み失敗は `host_run_history_write_failed`
として記録する。

`SafeMode` の間、Host は `settings.json` の内容にかかわらず次を強制する。モデルを
ロードしない（`model.enabled` と `--model` の指定を無視し、`LoadModel` は
`error: "safe_mode"` で拒否する）、NLL rerank・`llmMagicConversion`・`aiBackend`
（`none` 扱い）・`ai-cleanup` の一括変換・外部 AI を止める、学習の書き込みを止める
（privacy の `secure` と同じ扱い）、`typoCorrectionMode` を `off`、自動単語登録の
抽出を止める。辞書・既存の学習結果による変換は続ける。

M47 v1（M30 未完了時）は TIP の候補ウィンドウ下部バナーで通知する。設定アプリの通知バナーへの統合は
M30 完了後の follow-up とし、M30 を M47 v1 の前提にはしない。
SafeMode は `settings.safeMode.enabled = true` フラグとして永続化し、
ユーザーが手動で解除するまで継続する。解除は `safeMode.enabled=false` への書き換えと、
Host の再起動または `UpdateConfig`（§8.5.1）で行う。解除しても `enteredAt` / `lastCrashCount` は
消さず、直近の突入の記録として残し、次の突入で上書きする。`host_run_state.txt` の
クラッシュ記録も解除では消さず、これまでどおり正常終了で空にする。キーは他の設定と同じ camelCase とし、
`settings/mvp-settings.schema.json` に次の形で持つ:

```json
"safeMode": {
  "type": "object",
  "properties": {
    "enabled": { "type": "boolean", "default": false },
    "enteredAt": { "type": "string", "description": "RFC 3339 タイムスタンプ（UTC）" },
    "lastCrashCount": { "type": "integer", "default": 0, "minimum": 0 }
  },
  "additionalProperties": false
}
```

Host は `SafeMode` に入るとき、`settings.json` のファイルロックを取り、ディスク上の内容へ
`safeMode` オブジェクトだけを合成して atomic に書き戻す（他のキーは保つ）。ファイルが
存在するのに読めない、または JSON として解釈できない場合は書き換えず、その起動では
`SafeMode` に入らない（`safe_mode_entered` を `result: "error"` で記録する）。書き込みは
TIP の設定監視にも届き、TIP は通常の設定変更と同じく Handshake をやり直す。
スキーマと Host 側の読み書きパスは同じ PR / コミットで揃え、永続化されない不整合状態を
作らない。

#### 8.5.4 UI 通知

候補ウィンドウ下部の控えめインジケータで、§8.5.1 で TIP が合成した表示状態を示す。
表示する状態ごとの文言とボタンは次のとおりとする。`healthy` は表示しない。
M46 の secure 表示と同時に出すときの並びは
[`privacy-and-secure-input-spec.md`](privacy-and-secure-input-spec.md) §6 が定める。

| 表示状態 | 文言 | ボタン |
|---|---|---|
| `degraded_simple` | ⚠️ 変換エンジンに接続できないため、かな・カタカナ候補で継続しています | [詳細] |
| `degraded_model` | ⚠️ Zenzai を使えないため、簡易変換で継続しています | [詳細] [再試行] |
| `safe_mode` | ⚠️ 異常終了が続いたため、セーフモードで動作しています（AI と学習を停止中） | [詳細] |

毎回ラベルを出すと邪魔になるため:

- 表示状態が変わった直後のみ 1 回表示し、5 秒経つか候補ウィンドウを閉じた時点で消す。
  変わったときに候補ウィンドウが出ていなければ、次に候補ウィンドウを出したときに表示する。
  文節移動などで候補一覧を出し直すことは閉じることに当たらず、インジケータは 5 秒の残りの間
  表示を続ける
- `safe_mode` は、TIP が接続後に初めて観測したときも「変わった直後」として扱う。Host は起動時に
  SafeMode へ入るため、これが §8.5.3 の「突入の通知」に当たる。同じ Host 世代
  （`host_generation_id`、§8.2）の間、TIP インスタンスごとに 1 回だけ表示する
- 詳細クリックの遷移先は M30 / M44 の進捗で分岐する。M47 v1（M30 未完了
  かつ M44 v1 完了時）は `azookey_diag.exe` の起動方法を案内する TIP 内
  ポップアップを表示する。M30 完了と M44（`diagnostics-compat-spec.md` §12.7）の診断タブ統合（M44
  follow-up）の両方が揃った後は、設定アプリの「プライバシー」ペインの障害診断の節を直接開く動作に
  切り替える
- 「再試行」クリックで TIP は `UpdateConfig` を送る。Host は `settings.json` を読み直し、
  選択中のモデルと backend で再ロードする（§8.5.1 の `RecoveringModel`）。TIP はモデルの
  パスも backend も持たないため、`LoadModel` は使わない。再ロードは完了まで応答しないため、
  IPC ワーカーとは別のスレッドから、Cancel と同じく Handshake を済ませた短命な control 接続
  （`observability-spec.md` §7.3）で送り、primary 接続の入力処理を待たせない。応答は control 接続で待ってから閉じ、
  待ちの上限は §8.5.2 の Model load とする。送信中は同じ TIP インスタンスから重ねて送らず、
  [再試行] を押せない状態にする。入力インジケーターのメニューでライブ変換を切り替えたときも同じ経路で送り
  （`docs/tsf-deep-integration-spec.md` §4.5）、送信中なら重ねて送らない。複数の TIP から届いた `UpdateConfig` は Host が直列に処理する。
  結果は §8.5.1 の契機で取得する `fallback_state` で観測する
- `safe_mode` では再ロードが `error: "safe_mode"` で拒否されるため [再試行] を出さず、
  [詳細] で `docs/debugging.md`「典型トラブル」の解除手順を案内する

#### 8.5.5 適応モデル切替の再解釈

karukan は `max_latency_ms` を超えたら main（90M）から light（26M）へ切り替える適応
モデル切替を持つ（`docs/karukan-comparison-report.md` §6）。azooKey はこれを
**取り込まない**。2 モデル常駐を前提とする設計であり、単一 Zenzai モデル構成と衝突する
ためである。ParallelBeam も同じ理由で採らない。

その上で、この機構が解こうとしている問題、すなわち「遅いときは軽い経路へ落として応答を
返す」ことは、**M47 の要求単位の fallback が担う**。§8.5.1 のとおり、推論が 1 変換
ハード予算を超えた要求は、best-so-far beam があればそれを返し、無ければその要求だけを
SimpleConverter の候補へ落とす。いずれの場合も状態機械は遷移させない。
この予算は `docs/zenzai-inference-spec.md` §8.2 の 600ms（上限の設計値は §8.5.2 の 800ms）であり、これが karukan の `max_latency_ms` に対応する。落ちる先は軽量モデルではなく、辞書 + 学習 +
SimpleConverter の経路であり、モデルをもう 1 つ常駐させずに同じ目的を果たす。backend の切り替えもモデル差し替えとは別の軸にあり、ロード失敗時に
CPU backend へ再試行し、それも失敗したら SimpleConverter へ落とす経路は
`docs/model-management-spec.md` §5.3 が定めている。

したがって本項の結論は「再解釈済み。追加機構は設けない」である。v1.0 後にレイテンシ起因の
切替を新たに検討する余地が残るのは、モデルとメモリ予算が確定し、**軽量モデルを常駐させる
費用が正当化できると分かった場合**に限る。その検討は §8.5 の状態機械への追加として新規課題
で扱い、本項の判断を暗黙に覆さない。

### M47 受け入れ条件

受け入れ条件の定義の正典は [`plans/windows-port-roadmap.md`](../plans/windows-port-roadmap.md)
の M47 節とする。本書は §8.5 で復旧 UX の状態機械と timeout 表を定義し、
受け入れ条件を複製しない。

### 8.6 推論 Worker プロセス分離（Broker/Worker, MVP後）

inference-host は TIP から見て out-of-proc だが、`Dispatcher` /
`InferenceEngine` / `SettingsStore` / `UserDataPaths` を同一プロセス・同一
ユニットに同居させている。この構成では、モデル runtime（llama.cpp / ggml /
将来の NPU runtime / driver）起因のメモリ破損・例外が、設定・辞書・IPC 認証まで
巻き込む blast radius を持つ。

将来の堅牢化として、inference-host を **Broker（設定・辞書・学習・IPC 認証・
Worker 監視/再起動）** と **Worker（モデル runtime のみ）** に分離し、モデル
backend のクラッシュを「落としてよい側」へ封じ込める方向を定義する。これは
v1.0 の必須範囲ではなく **MVP 後の投資**であり、着手時は `observability-spec.md` §7 の相関 ID・§8 の
再接続状態機械を土台に、`request_id` を軸とした Cancel / staleness / sequencing
の E2E を最優先で担保する（分離後に取りこぼしが出やすい箇所のため）。設計スパイクは
Linear で追跡する（2026-07 開発基盤ツール導入 第2弾）。`plans/windows-port-roadmap.md`
の「リスクと不確実性」と相互参照する。
