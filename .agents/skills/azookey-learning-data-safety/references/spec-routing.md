# 学習データ変更のルーティング

## 仕様、実装、テスト

| 変更領域 | 最初に読む仕様 | 主な実装 | 優先テスト |
|---|---|---|---|
| 学習データの保存、読込、破損復旧 | `docs/learning-data-management-spec.md`, `docs/user-learning-enhancement-spec.md` | `learning/src/LearningStore.cpp` | `learning_tests` |
| atomic write と file lock | `docs/learning-data-management-spec.md` | `learning/src/AtomicFile.h`, `learning/include/azookey/learning/FileLock.h` | `atomic_file_tests` |
| ユーザー辞書、import/export、CLI | `docs/learning-data-management-spec.md` | `learning/src/UserDictionary.cpp`, `inference-host/src/UserDictCli.cpp` | `user_dictionary_tests`, `host_userdict_cli_tests` |
| reranker と候補順位への学習反映 | `docs/user-learning-enhancement-spec.md` | `learning/src/Reranker.cpp` | `reranker_tests` |
| secure input、ログ、外部送信 | `docs/privacy-and-secure-input-spec.md` | `inference-host/src/InferenceEngine.cpp`, `inference-host/src/Dispatcher.cpp` | `host_engine_tests`, `host_dispatcher_tests` |
| 自動単語学習 | `docs/auto-word-registration-spec.md` | CodeGraph で現在の入口を確認する | 関連する learning / host テスト |
| typo correction と学習の相互作用 | `docs/typo-correction-learning-spec.md` | CodeGraph で現在の入口を確認する | 関連する converter / learning テスト |

## 検証コマンドの選び方

- 学習ライブラリ: `learning_tests`, `user_dictionary_tests`, `reranker_tests`, `atomic_file_tests`
- Host 経由の辞書操作: `host_userdict_cli_tests`
- secure input や request lifecycle: `host_engine_tests`, `host_dispatcher_tests`
- 影響が横断的な場合: `cmake --build --preset windows-debug --target azookey_check`

Windows の実ビルドと CTest はリポジトリ指定の Windows Headless CMake Build 手順で実行する。対象 target が最新か確認せず、古い実行ファイルへ `ctest` だけを実行しない。

## レビュー観点

- 保存先がユーザー単位であり、repo 配下やパッケージ資産へ入らないか。
- temp file、rename/replace、flush、lock の順序が失敗時にも一貫するか。
- 破損、旧形式、部分書込みでデータを黙って失わないか。
- import/export が入力検証、重複、サイズ上限、文字コード、途中失敗を扱うか。
- secure input の情報が学習、候補履歴、テレメトリ、ログへ残らないか。
- migration が再実行可能で、旧版からのロールフォワードと失敗時回復を説明できるか。
