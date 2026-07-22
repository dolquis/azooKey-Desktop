---
name: azookey-inference-model-workflow
description: azooKey Desktop の inference-host、Zenzai、GGUF、llama.cpp、ModelCatalog、モデル変換、ロード、runtime tier、health、degraded mode、fallback、実モデル検証、推論benchを変更またはレビューするときに使う。mock、最小GGUF、完全GGUFの検証レベルを区別し、ロード失敗時の安全な挙動と再現可能な検証条件を確認する。
---

# azooKey 推論モデル運用

モデル関連の変更を、変換、発見、選択、ロード、health、推論、fallback までの運用の流れとして扱う。

## 手順

1. `references/spec-routing.md` を読み、対象機能の正典と必要な runtime tier を決める。
2. CodeGraph でモデル発見から推論応答までの経路を確認し、Serena で対象シンボルと参照元を確認する。
3. load、health、handshake、degraded mode、fallback の既存契約を列挙する。
4. 変更に最も近い host テストを実行し、必要なら実モデルラベル、bench、`azookey_check` へ広げる。
5. モデル形式、catalog schema、fallback、設定項目、ユーザー可視挙動が変わる場合は対応する仕様文書も更新する。

## 必須ガードレール

- llama.cpp 無効ビルドは制御経路と mock の検証であり、実推論成功とは扱わない。
- 最小GGUF probe はパーサーやエラー経路の検証であり、モデル品質の証明には使わない。
- 完全GGUFによる実モデル検証では、モデル必須オプションと Zenzai 必須条件を明示し、skip を成功に数えない。
- 任意のGGUFを暗黙に自動選択しない。catalog、設定、明示パスの優先順位を仕様どおりに保つ。
- モデル不在、破損、非対応、OOM、初期化失敗時の fallback と degraded 状態を壊さない。
- モデルバイナリ、ライセンス制限物、secret、ユーザー入力をコミットやログへ含めない。
- IPC payload や TIP / Host の責務境界を変える場合は `tsf-ipc-protocol` も使用する。

## 完了条件

- 使用 preset、`AZOOKEY_WITH_LLAMA_CPP`、モデル入手元、モデル識別情報、skip 条件を報告する。
- 変更に対応する unit / integration / real-model 検証のどこまで実施したかを区別する。
- bench はハードウェア、モデル、入力、反復回数、warm-up を揃え、機能テストの代替にしない。
