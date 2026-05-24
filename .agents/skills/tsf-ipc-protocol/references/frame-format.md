# IPC フレームフォーマット (TIP ⇔ Inference Host)

## トランスポート

- Windows Named Pipe (`\\.\pipe\azookey-<sid>`)
  - パイプ名は `ipc::DefaultPipeName()` が現在のプロセストークンの SID から
    導出する (`ipc/include/azookey/ipc/NamedPipeTransport.h`)。
- Pipe モード: `PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE`
- DACL: 現在のユーザ SID のみに RW 許可
  (`NamedPipeServer` Windows 実装で設定)
- 1 サーバが複数クライアント (TIP + 設定 UI 等) を許容

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

## 既知の制約

- ペイロード最大サイズの明示的な上限はコードで強制していない。
  ただし `PIPE_TYPE_MESSAGE` のメッセージサイズは Windows 仕様に従う
  (実用上、変換候補リクエスト/レスポンスは数 KB 程度の想定)。
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
