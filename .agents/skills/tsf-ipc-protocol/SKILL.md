---
name: tsf-ipc-protocol
description: ipc/ 配下の IPC 定義、TIP と inference-host 間の Named Pipe / JSON プロトコル、メッセージスキーマ、ハンドシェイク、エラー回復を扱うときに使用する。
---

# IPC プロトコル運用ガイド

## 基本仕様

- トランスポート：Windows Named Pipe(`\\.\pipe\azookey-*`)
- フレーミング：**4-byte little-endian length prefix + UTF-8 JSON payload**
- 双方向：TIP → Host(リクエスト)、Host → TIP(レスポンス + 非同期イベント)

詳細は `references/frame-format.md` を参照。

## 変更時に必ず守ること

1. **後方互換を壊す変更を禁止**。フィールド追加は許容、削除・型変更は要バージョン bump。
2. JSON スキーマを変更したら：
   - `ipc/` のヘッダ／シリアライザを更新
   - TIP 側(`tsf-tip/`)の呼び出しコードを更新
   - Host 側(`inference-host/`)のハンドラを更新
   - GoogleTest を追加／更新
   - `docs/` の関連仕様 md を更新
3. プロトコルバージョンフィールド(あれば)を必ず確認。

## ハンドシェイク失敗時の挙動

- TIP は **無入力フォールバック** に切り替える(変換せずパススルー)。
- Host プロセスが落ちている場合でも、TIP は **Host を起動・再起動しない**
  (TIP 側に `CreateProcess` 等は持たない)。Host は別経路
  (`scripts/register.ps1` の HKCU `Run` キー等)で起動される前提。
- TIP の IPC worker は **パイプへ指数バックオフで無期限に再接続**する
  (250ms→最大 3000ms で頭打ち。回数上限は無い)。worker は `Deactivate`
  (`ipc_stop_`) のときのみ終了し、接続断では終了しない
  (`tsf-tip/src/TextService.cpp` `IpcWorkerThread`、DEV-168)。
- ユーザーに見えるエラー UI は出さない(TIP からモーダル禁止)。

## やってはいけない

- 既存メッセージ ID(type フィールド)の意味を変更する。
- length prefix を含めずに JSON だけ送る実装を書く。
- 同期ブロッキング呼び出しを TIP のメインスレッドで行う(必ず非同期化)。
