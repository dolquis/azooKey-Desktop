---
name: azookey-settings-schema-evolution
description: azooKey Desktop の設定キー追加、変更、削除、JSON Schema、default sample、SettingsStore、ModelCatalog、schemaVersion、migration、credential、設定UIを変更またはレビューするときに使う。schema、runtime既定値、parser、sample、UI、CIの同期、旧版互換性、破損時fallback、秘密情報の扱いを確認する。
---

# azooKey 設定スキーマ進化

設定変更を JSON Schema だけで終わらせず、runtime、既定値、migration、UI、CI まで一つの契約として扱う。

## 手順

1. `references/spec-routing.md` で対象が MVP settings か model catalog かを判定する。
2. key owner、型、既定値、範囲、欠落時、unknown key、invalid value、旧 `schemaVersion` の挙動を決める。
3. `.codegraph/` があり CodeGraph が利用可能なら読込先と適用先を確認する。Serena が利用可能なら active project と languages を確認してから parser、設定構造体、参照元を確認する。利用できない場合は `rg` と実ファイルで同じ範囲を確認する。
4. schema、runtime default、parse/apply、sample、UI、仕様文書、CI を必要な範囲で同時に更新する。
5. metaschema、sample validation、focused test を実行し、必要なら `azookey_check` へ広げる。

## 必須ガードレール

- root の `additionalProperties: false` など、未知キー拒否の既存方針を意図なく緩めない。
- schema default、sample、runtime default、UI default を一致させる。
- `schemaVersion` を変える場合は、旧版の検出、migration、再実行、失敗時回復を定義する。
- 破損、型不正、範囲外では、既存の quarantine と安全な fallback を無言のデータ破棄へ変えない。
- API key、token、credential を schema default、sample、fixture、ログ、PR本文へ入れない。
- `mvp-settings.schema.json` と `model-catalog.schema.json` の責務を混ぜない。
- 将来予約の設定キーを、実装と仕様がないまま先行追加しない。

## 完了条件

- schema、runtime、sample、UI、docs の既定値と制約が一致している。
- metaschema、sample validation、関連する SettingsStore / ModelCatalog テストが通る。
- 旧版、欠落、unknown、invalid、破損ケースと、実行できなかった検証を報告する。
