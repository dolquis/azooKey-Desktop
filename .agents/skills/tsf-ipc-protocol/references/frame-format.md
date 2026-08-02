# IPC フレームフォーマット (TIP ⇔ Inference Host)

## 目次

- [トランスポート](#トランスポート)
- [フレーミング](#フレーミング)
- [Envelope](#envelope-構造-ipcenvelope)
- [メッセージ種別](#メッセージ種別-ipcmessagetype)
- [Handshake](#handshakeと接続認証)
- [接続回復](#timeoutcancel接続回復)
- [同期チェックリスト](#変更時の同期チェックリスト)

## トランスポート

- Windows Named Pipe (`\\.\pipe\azookey-<sid>`)
  - パイプ名は `ipc::DefaultPipeName()` が現在のプロセストークンの SID から
    導出する (`ipc/include/azookey/ipc/NamedPipeTransport.h`)。
- Pipe モード: `PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT`。server は
  生成時から `FILE_FLAG_OVERLAPPED` を付け、overlapped I/O でキャンセル可能な
  accept を実現する(`PIPE_NOWAIT` は使わない)。接続後のメッセージ I/O は
  ブロッキング(`PIPE_WAIT`)のまま扱う
  (`ipc/src/NamedPipeTransport.cpp`)。
- DACL: 現在のユーザ SID のみに RW 許可
  (`NamedPipeServer` Windows 実装で設定)。Debug/test の restricted-token 実行環境では
  Release 以外に限り互換 ACE を追加する。
- Remote client は `PIPE_REJECT_REMOTE_CLIENTS` で拒否し、ローカル IME ↔ Host
  専用 transport として扱う。
- 1 サーバが複数クライアント (TIP + 設定 UI 等) を許容する。同時接続
  インスタンス上限は実装値 `kMaxPipeInstances = 32`
  (`ipc/include/azookey/ipc/Limits.h`)。
- Release では SID 取得失敗時に per-user pipe 名 / DACL fallback を拒否する。

## フレーミング

```
┌────────────────────────┬─────────────────────────────────────┐
│ length prefix (4 byte) │ payload: UTF-8 JSON (Envelope)      │
│ little-endian uint32   │ length-prefix の値と一致する長さ    │
└────────────────────────┴─────────────────────────────────────┘
```

- 実装: `ipc::EncodeLengthPrefixed` / `ipc::DecodeLengthPrefixed`
  (`ipc/include/azookey/ipc/Messages.h`)
- length-prefix と payload は連続して書き出し、受信側は length-prefix の長さまで
  読み切って 1 フレームとして復元する。Named Pipe の message boundary や pipe
  write 分割に依存してはならない。
- 最大 payload サイズは 1MiB (`ipc::kMaxFrameSize`)。JSON パーサの最大入力長
  と同じ値に揃え、超過フレームは送受信時に拒否する。
- little-endianはwire契約である。`EncodeLengthPrefixed`は4 byteを明示的に書き込み、
  payloadを新しい`std::vector<uint8_t>`へコピーするため、zero-copyを前提にしない。

## Envelope 構造 (`ipc::Envelope`)

| C++フィールド | wire key | 型 | 契約 |
|---|---|---|---|
| `version` | `version` | `int` | プロトコル世代。serialize時は常に出力する。欠落時はv1、許容範囲は`1..kEnvelopeVersion` |
| `request_id` | `request_id` | `uint64` | 必須。レスポンスは要求IDをエコーバックする |
| `trace_id` | `trace_id` | `string` | 必須フィールド。ログ・追跡用で、空文字は許容する |
| `type` | `type` | `string` | 必須。`MessageType`との変換には`TypeToString` / `TypeFromString`を使う |
| `payload_json` | `payload` | JSON value | typeごとのpayload。欠落時は`{}`として復元する |

`ipc::Serialize` / `ipc::Deserialize` は `Envelope` ⇔ JSON 文字列を変換する。
payload 本体は型ごとに `Build*Request/Response` / `Parse*Request/Response` で
扱う (`ipc/include/azookey/ipc/Payloads.h`)。

- `Serialize` は `std::optional<std::string>` を返す。`payload_json` が空でなく
  かつ有効な JSON でない場合は `std::nullopt`(シリアライズ失敗)を返し、生文字列を
  黙って埋め込まない。`payload_json` は常に `Build*()` が生成した有効な JSON である
  ことを前提とする。
- `Deserialize` は `version` を検証する。対応世代 `kEnvelopeVersion`(現状 `1`)より
  大きい世代(将来の breaking change)や `1` 未満(不正値)は `std::nullopt` を返して
  フレームを破棄する。互換世代を誤解釈するより破棄する方針(transport の
  「解釈不能フレームは無視」と同じ)。`version` フィールド欠落時は現行世代とみなす。
- `request_id`、`type`、`trace_id`のいずれかが欠落した場合も`std::nullopt`を返す。
  未知のtype文字列は`MessageType::Unknown`へ変換されるが、dispatcherで処理可能とは限らない。

## メッセージ種別 (`ipc::MessageType`)

`ipc/include/azookey/ipc/Messages.h` で定義。文字列変換は
`TypeToString` / `TypeFromString` を経由する。

次表は現行実装のスナップショットである。変更前に`Messages.*`、`Payloads.*`、
`Dispatcher.cpp`を再確認する。

| 種別 | 現在の配線 | 役割 |
|---|---|---|
| `Handshake` | codec + Host | version、capability、client ID、tokenの交換 |
| `LoadModel` | codec + Host | モデル読込指示 |
| `QueryCandidates` | codec + Host + TIP | 入力中readingの候補要求 |
| `QueryBatchConversion` | codec + Host + TIP | batch romajiの一括変換要求 |
| `QueryPredictions` | enumのみ | 将来の予測変換用予約 |
| `QueryCorrections` | enumのみ | 将来のtypo補正用予約 |
| `Cancel` | codec + Host + TIP | in-flight要求の取消。Hostはレスポンスを返さない |
| `CommitObservation` | codec + Host + TIP | 確定操作の学習フィードバック |
| `CommitCorrection` | enumのみ | 将来の補正確定通知用予約 |
| `AddUserWord` | codec + Host | ユーザ辞書追加 |
| `UpdateUserWord` | enumのみ | 将来のユーザ辞書更新用予約 |
| `RemoveUserWord` | codec + Host | ユーザ辞書削除 |
| `UpdateConfig` | Host + response codec | settings再読込。要求payloadは空オブジェクト |
| `Ping` | codec + Host | 疎通確認 |
| `Health` | codec + Host | Host状態取得 |
| `Unknown` | sentinel | 未知type。通常メッセージとして送信しない |

各メッセージの payload スキーマは `ipc/include/azookey/ipc/Payloads.h` 内の
struct (例: `HandshakeRequest`, `QueryCandidatesRequest`, `CandidateField`,
`CommitObservationRequest`) を参照。

## Handshakeと接続認証

`HandshakeRequest`は`tip_version`を必須とし、`protocol_version`省略時はv1、
`capabilities`省略時は空配列として読む。`client_id`と`handshake_token`は任意である。
Host側にtokenが設定されている場合、protocol versionとtokenの両方が一致したときだけ
`HandshakeResponse.accepted=true`になる。pipe modeのHostは
`AZOOKEY_IPC_HANDSHAKE_TOKEN` / `--handshake-token` を優先する。未指定時は
per-user pipe ACL のみで動作し、token 検証は無効。手動で token を使う場合は
Host / TIP の両プロセスに同じ `AZOOKEY_IPC_HANDSHAKE_TOKEN` を明示設定する。

TIPのprimary接続とCancel用control接続はそれぞれHandshakeを行う。同じTIPインスタンスは
両接続に同じ非空`client_id`を送り、HostのCancel/latest状態を他プロセスと分離する。

`HandshakeResponse` の wire payload は次のとおり。既存フィールドの意味は変更せず、
`host_generation_id` は protocol v1 への省略可能な追加フィールドとして扱う。

| wire key | 型 | 契約 |
|---|---|---|
| `host_version` | `string` | 必須。Host バイナリの版数 |
| `protocol_version` | `int` | 省略時は v1 |
| `accepted` | `bool` | 省略時は `false` |
| `model_loaded` | `bool` | 省略時は `false` |
| `host_generation_id` | `string` | 省略時は空文字。Host プロセス起動時に生成する UUID で、同一プロセスの全接続に共通 |
| `batch_romaji_conversion` | `bool` | 省略時は `false` |
| `batch_romaji_preview_style` | `string` | 省略時は `kana` |
| `batch_conversion_mode` | `string` | 省略時は `neural` |
| `batch_auto_punctuation` | `bool` | 省略時は `false` |

TIP は初回の非空 `host_generation_id` を保存し、同じ値への再接続では pending 要求を
維持する。既知の値から別の値（省略を含む）へ変わった場合は、Handshake 成立後に
旧世代向けの pending / in-flight 要求と候補表示待機状態を破棄する。要求送信時の世代と
応答反映時の世代が一致しない場合も、その応答を stale として破棄する。

## timeout、Cancel、接続回復

- `Receive()`はブロッキングである。
- `ReceiveWithTimeout(timeout_ms)`は`PeekNamedPipe`を10ms間隔で確認する期限付きpollingであり、
  即時returnを保証するnon-blocking APIではない。timeoutまたはpipe errorで`std::nullopt`を返す。
- TIPのIPC workerはHost不在時に250msから最大3000msまで指数バックオフして再接続する。
  接続断直前に取り出した最新候補要求は再度pendingへ戻し、Handshake後に再送する。
- Cancelは応答待ちのprimary接続を塞がないよう、Handshake済みの短命control接続を優先する。
  control接続が使えなければprimary接続へbest-effortでqueueする。
- TIPのUI側は`request_id`でstale responseを破棄し、再接続前の古い結果を表示しない。
- TIPはHandshakeで得た`host_generation_id`も照合し、Host再起動前の世代に属する
  pending / in-flight要求と遅延応答を破棄する。

## 変更時の同期チェックリスト

1. `MessageType`、`TypeToString`、`TypeFromString`を同期する。
2. request/response struct、`Build*`、`Parse*`を同じ変更で追加し、必須・任意・既定値を固定する。
3. `Dispatcher::Dispatch`とhandler、TIPまたはsettings側の送信経路を同期する。
4. `messages_test.cpp`でEnvelope、version、type mapping、frame境界を検証する。
5. `payloads_test.cpp`でcodecのround-trip、欠落、型違い、境界値を検証する。
6. transport変更では`named_pipe_transport_test.cpp`、TIPフロー変更では
   `tip_client_ipc_test.cpp`と関連`tsf-tip/tests/`を検証する。
7. `docs/windows-tsf-host-architecture.md`と関連specの契約記述を同期する。
