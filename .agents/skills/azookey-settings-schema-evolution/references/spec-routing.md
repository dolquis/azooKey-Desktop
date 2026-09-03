# 設定スキーマ変更のルーティング

## 仕様、実装、テスト

| 変更領域 | 最初に読む仕様 | 主な実装とデータ | 優先テスト |
|---|---|---|---|
| runtime 設定キー | 対応する `docs/*-spec.md`, `settings/mvp-settings.schema.json` | `inference-host/src/SettingsStore.cpp`, 対応する header | `host_settings_store_tests` |
| 既定値サンプル | `settings/mvp-settings.schema.json` | `settings/default-settings.sample.json` | schema sample validation |
| ModelCatalog | `docs/model-management-spec.md` | `settings/model-catalog.schema.json`, `inference-host/src/ModelCatalog.cpp` | `host_model_catalog_tests` |
| 設定UI | `docs/native-ui-spec.md` と機能別仕様 | `settings/` のUI実装 | 関連UIテストと runtime テスト |
| migration と破損復旧 | `docs/dev-infrastructure-spec.md`, 対応する機能仕様 | SettingsStore / ModelCatalog の読込経路 | 旧版と破損fixtureを扱うテスト |
| credential と外部AI設定 | `docs/ai-backend-spec.md`, `docs/privacy-and-secure-input-spec.md` | secret storage と設定参照箇所 | credential を含めないfixtureとエラー経路 |

## 同期チェックリスト

- JSON Schema の `type`, `required`, `enum`, 数値範囲、`default`
- C++ 設定構造体、parser、validation、fallback、適用先
- `settings/default-settings.sample.json`
- 設定UIの入力制約、表示、初期値、保存値
- 対応する `docs/*-spec.md`
- pre-commit / GitHub Actions の schema validation
- `schemaVersion` と migration / quarantine

設定キーを削除またはrenameする場合は、既存ユーザーのファイルを unknown key として全面拒否するのか、migration するのかを明示する。

## 検証

```powershell
check-jsonschema --check-metaschema settings/mvp-settings.schema.json settings/model-catalog.schema.json
check-jsonschema --schemafile settings/mvp-settings.schema.json settings/default-settings.sample.json
```

CI と再現条件を合わせる場合は `check-jsonschema==0.37.3` を使う。

- runtime settings: `host_settings_store_tests`
- model catalog: `host_model_catalog_tests`
- 横断確認: `cmake --build --preset windows-debug --target azookey_check`

Windows の実ビルドと CTest は Windows Headless CMake Build 手順に従う。schema 検証成功だけで runtime への適用成功を代替しない。

## 実装後に失効する spec 記述の削除

spec-first で定義した設定キー（schema 未登録）を実装して schema へ追加したら、
対応する spec の「未登録」「実装時に追加」という記述を削除するか過去形に変える。
逆に、schema に既にあるキーを spec 側が「新設する」と書いているのも実体との
食い違いなので、実装 PR のついでに直す（詳細は `azookey-doc-governance` スキル）。
