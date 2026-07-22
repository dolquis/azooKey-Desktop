---
name: tsf-ipc-protocol
description: ipc/ 配下の Envelope、MessageType、Payload codec、Named Pipe transport、TIP・settings・inference-host 間のハンドシェイク、キャンセル、再接続、互換性を変更・レビュー・デバッグするときに使用する。
---

# IPC プロトコル運用ガイド

## 最初に確認する

1. `docs/windows-tsf-host-architecture.md` と変更対象に対応する `docs/*-spec.md` を読む。
2. wire形式、必須フィールド、接続回復を扱う場合は `references/frame-format.md` を読む。
3. 次の実装を突き合わせ、enumに存在するだけの型を「利用可能」とみなさない。
   - `ipc/include/azookey/ipc/Messages.h` と `ipc/src/Messages.cpp`
   - `ipc/include/azookey/ipc/Payloads.h` と `ipc/src/Payloads.cpp`
   - `inference-host/src/Dispatcher.cpp`
   - TIPクライアントは `tsf-tip/src/TextService.cpp`
4. 仕様、実装、テストが食い違う場合は黙って一方へ合わせず、差異を報告して正典を同期する。

## 不変条件

- 4-byte little-endian length prefixとUTF-8 JSON `Envelope`を維持する。
- `request_id`、`trace_id`、`type`のwireフィールド存在を必須とする。`trace_id`は空文字を
  許容できるが、フィールド自体を省略しない。
- 既存`type`の意味を変更しない。新しい`MessageType`は`Unknown`の前へ追加し、
  `TypeToString`と`TypeFromString`を同時に更新する。
- v1への追加フィールドには省略時の既定値を定義する。削除、型変更、意味変更では
  `Envelope.version`のbumpと移行経路を設計する。
- payloadの必須・任意判定を推測せず、対応する`Parse*`実装とテストで確認する。
- 1MiB上限、per-user SIDのDACL、remote client拒否、最大32接続を弱めない。

## 接続断とハンドシェイク

- TIPはprimary接続と短命なCancel用control接続の両方でHandshakeを完了してから
  通常メッセージを送る。同一TIPの両接続では同じ`client_id`を使う。
- Hostに`handshake_token`が設定された場合だけ、Handshake成立前の通常メッセージを拒否する。
  token未設定でもprotocol versionの不一致はHandshake拒否となる。
- Host不在時を「無入力」や全面的なkey pass-throughと表現しない。現在のTIPは対応キーを
  local preeditへ取り込み、最新の候補要求を保留して再接続後に再送する。Host応答が戻るまでは
  候補生成だけが利用できない。
- TIPからHostを起動・再起動しない。IPC workerは250msから最大3000msまで指数バックオフし、
  接続断では終了せず`Deactivate`の停止通知まで再接続する。
- 接続断やHandshake失敗でモーダルUIを表示しない。

## 変更手順

1. typeごとに「enum」「payload codec」「Host dispatcher」「送信クライアント」「テスト」の
   配線状態を確認する。
2. wire変更を`Messages.*`と`Payloads.*`へ実装し、未知・欠落・境界値もテストする。
3. Host dispatcherとTIPまたはsettings側クライアントを同じ変更で同期する。
4. retry、Cancel、stale response破棄を壊していないか確認する。最新要求を再送するときに
   古い`request_id`の結果をUIへ反映しない。
5. `ipc_tests`、`ipc_payloads_tests`、必要に応じて`ipc_named_pipe_transport_tests`と
   `ipc_tip_client_tests`を実行する。最終確認は`azookey_check`へ含める。
6. 変更した契約に対応するspecと本スキルのreferenceを同期する。

## 禁止事項

- length prefixを省いてJSONだけ送信しない。
- TIPのTSF/UIスレッドで接続、送受信、応答待ちを行わない。
- `Unknown`へ変換される未対応typeを、Hostが処理できるものとして扱わない。
- security fallbackやframe上限をテスト都合で本番経路へ持ち込まない。
