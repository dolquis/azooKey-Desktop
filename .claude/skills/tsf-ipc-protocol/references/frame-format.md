# IPC フレームフォーマット (TIP ⇔ Inference Host)

## トランスポート

- Windows Named Pipe (`\\.\pipe\azookey-<sid>`)
  - パイプ名は `ipc::DefaultPipeName()` が現在のプロセストークンの SID から
    導出する (`ipc/include/azookey/ipc/NamedPipeTransport.h`)。
- Pipe モード: `PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE`
- DACL: 現在のユーザ SID のみに RW 許可
  (`NamedPipeServer` Windows 実装で設定)
- 1 サーバが複数クライアント (TIP + 設定 UI 等) を許容する。同時接続
  インスタンス上限は実装値 `kMaxPipeInstances = 32`
  (`ipc/include/azookey/ipc/Limits.h`)。設計意図は小数 (TIP + 設定 UI 程度) で
  あり、上限値を 4 へ絞るか 32 のままとするかは Issue #37 で検討する。
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
- length-prefix は **必ず** payload と同じ 1 回の `Send` で書き出す。
- 最大 payload サイズは 1MiB (`ipc::kMaxFrameSize`)。JSON パーサの最大入力長
  と同じ値に揃え、超過フレームは送受信時に拒否する。
- バイトオーダーが LE な理由: Windows ネイティブが LE のため zero-copy で
  処理できる。

## Envelope 構造 (`ipc::Envelope`)

| フィールド      | 型        | 意味                                                |
|----------------|----------|-----------------------------------------------------|
| `version`      | `int`    | プロトコル世代 (現状 `1`)                            |
| `request_id`   | `uint64` | 呼び出し ID。レスポンスはこの ID をエコーバックする   |
| `trace_id`     | `string` | 任意のトレース文字列 (ロギング / 追跡用)             |
| `type`         | `enum`   | `MessageType` (下記)                                |
| `payload_json` | `string` | type 毎の payload (UTF-8 JSON)                      |

`ipc::Serialize` / `ipc::Deserialize` は `Envelope` ⇔ JSON 文字列を変換する。
payload 本体は型ごとに `Build*Request/Response` / `Parse*Request/Response` で
扱う (`ipc/include/azookey/ipc/Payloads.h`)。

## メッセージ種別 (`ipc::MessageType`)

`ipc/include/azookey/ipc/Messages.h` で定義。文字列変換は
`TypeToString` / `TypeFromString` を経由する。

| 種別                  | 役割                                                |
|----------------------|-----------------------------------------------------|
| `Handshake`           | 起動時のバージョン交渉 + capability 共有             |
| `LoadModel`           | Inference Host へのモデル読込指示 (M8)              |
| `QueryCandidates`     | 入力中文字列に対する変換候補要求                     |
| `QueryPredictions`    | 予測変換要求                                          |
| `QueryCorrections`    | typo 補正候補要求                                     |
| `Cancel`              | in-flight な要求のキャンセル (M10)                  |
| `CommitObservation`   | 確定操作の学習用フィードバック (M6)                  |
| `CommitCorrection`    | 補正候補の確定通知                                    |
| `AddUserWord`         | ユーザ辞書追加                                        |
| `UpdateUserWord`      | ユーザ辞書更新                                        |
| `RemoveUserWord`      | ユーザ辞書削除                                        |
| `Ping` / `Health`     | 死活監視 / Inference Host 状態取得                  |

各メッセージの payload スキーマは `ipc/include/azookey/ipc/Payloads.h` 内の
struct (例: `HandshakeRequest`, `QueryCandidatesRequest`, `CandidateField`,
`CommitObservationRequest`) を参照。

`HandshakeRequest` は任意の `handshake_token` を持つ。Host 側に token が設定
されている場合、protocol version と token の両方が一致したときだけ
`HandshakeResponse.accepted=true` になる。pipe mode の Host は
`AZOOKEY_IPC_HANDSHAKE_TOKEN` / `--handshake-token` を優先する。未指定時は
per-user pipe ACL のみで動作し、token 検証は無効。手動で token を使う場合は
Host / TIP の両プロセスに同じ `AZOOKEY_IPC_HANDSHAKE_TOKEN` を明示設定する。

## 既知の制約

- `Receive()` はブロッキング、`ReceiveWithTimeout(timeout_ms)` は送信キューを
  drain したい呼び出し側のためのノンブロッキング版。

## 変更時の同期チェックリスト

1. `MessageType` に新種別を追加した場合は `TypeToString` / `TypeFromString` を
   両方更新する。
2. `Payloads.h` に新規 struct を追加した場合は同じ場所に `Build*` / `Parse*` を
   両方宣言し、`ipc/src/Payloads.cpp` に実装を追加する。
3. TIP 側 (`tsf-tip/src/TextService.cpp` の IPC ワーカー
   `IpcWorkerThread` / `PostIpcSend`) と Host 側
   (`inference-host/src/Dispatcher.cpp`) のディスパッチを両方更新する。
4. `ipc/tests/` にラウンドトリップテスト (`payloads_test.cpp` /
   `messages_test.cpp` パターン) を追加する。
5. プロトコル意味の変更は `Envelope.version` の bump を検討する。
