# コア変換変更のルーティング

## 仕様、実装、テスト

| 変更領域 | 最初に読む仕様 | 主な実装 | 優先テスト |
|---|---|---|---|
| ローマ字かな変換（Feed / Flush / Preview） | `docs/romaji-batch-conversion-spec.md` §3〜§5、`docs/legacy-parity-spec.md` | `core/src/RomajiKanaConverter.cpp` | `core_tests`（`core/tests/romaji_kana_converter_test.cpp`） |
| 固定辞書変換と bigram 文脈 | `docs/conversion-quality-benchmark-spec.md`、`docs/model-management-spec.md` | `core/src/SimpleConverter.cpp`、`core/include/azookey/core/IConverter.h` | `core_tests`（`core/tests/simple_converter_test.cpp`）、`host_engine_tests` |
| 辞書 trie とレイヤ（静的・可変・ユーザー語） | `docs/auto-word-registration-spec.md` | `core/src/DoubleArrayTrie.cpp`、`learning/src/DictionaryStore.cpp`、`inference-host/src/DictionaryCandidateProvider.cpp` | `dictionary_tests`、`host_engine_tests` |
| オフライン辞書ビルド | `docs/auto-word-registration-spec.md`、`docs/nll-release-measurement.md` | `dictbuild/dictbuild.py` | `dictbuild_python_tests` |
| live 変換の span 分割 | `docs/rich-features-spec.md`、`docs/inline-english-candidate-spec.md` | `core/src/LiveConversionChunker.cpp` | `core_tests`（`core/tests/live_conversion_chunker_test.cpp`） |
| 一括変換の chunk 境界 | `docs/romaji-batch-conversion-spec.md` §5〜§7 | `core/include/azookey/core/BatchConversionChunker.h` | `core_tests`、`ipc_payloads_tests` |
| 数字・半角カタカナ・記号・絵文字リライター | `docs/candidate-rewriter-spec.md` §3、§6〜§9、§18、§19 | `core/src/NumberRewriter.cpp`、`core/src/KatakanaRewriter.cpp`、`core/src/SymbolRewriter.cpp`、`core/src/SymbolChainTable.cpp`、`core/src/RewriterIndex.cpp`、`core/src/RewriterNormalization.cpp`、`inference-host/src/RewriterData.cpp` | `core_tests`（`number_rewriter`、`katakana_rewriter`、`symbol_rewriter`、`rewriter_index`） |
| 括弧ペアリングの判定 | `docs/bracket-pairing-spec.md` §3、§4、§6 | `core/src/BracketPairing.cpp`、`core/src/BracketTable.cpp`、`core/src/BracketSettings.cpp` | `core_tests`（`core/tests/bracket_pairing_test.cpp`）。TSF への翻訳は `tsf-tip-development` |
| 動的句読点 | `docs/dynamic-punctuation-spec.md` | 対応する spec の「InputState への統合」節が指す実装 | 関連する `core_tests` と TIP テスト |
| アプリ別プロファイル解決 | `docs/app-profile-spec.md` §5、§9 | `core/src/AppProfileResolver.cpp`。前面アプリ判定は `tsf-tip/src/ForegroundAppDetector.cpp` | `core_tests`（`core/tests/app_profile_resolver_test.cpp`） |
| UTF-8 / コマンドライン / パス | `docs/windows-tsf-host-architecture.md` の CLI エンコーディング境界 | `core/src/Utf8.cpp`、`core/src/CommandLine.cpp`、`core/src/PlatformPaths.cpp` | `core_tests`（`utf8`、`command_line`）、`host_args_tests` |
| AI 変換のプライバシー判定 | `docs/privacy-and-secure-input-spec.md` | `core/include/azookey/core/AiPrivacy.h`、`tsf-tip/src/AiInputGuard.cpp` | `host_engine_tests`、TIP テスト |

ログ、redaction、crash 収集も `core/` にあるが、`azookey-observability-diagnostics` が扱う。

## 検証コマンドの選び方

- core だけの変更: `core_tests`。Linux でも `cmake --preset linux-debug` で構成し、
  `ctest --preset linux-debug -R core_tests` を実行できる。
- 辞書レイヤ: `dictionary_tests`、`dictbuild_python_tests`
- Host の候補生成に影響: `host_engine_tests`
- 横断確認: `cmake --build --preset windows-debug --target azookey_check`

上の target 名は入口であり網羅ではない。CTest 一覧の正典は `docs/test-inventory.md` で、
target の追加・削除はそちらと `scripts/check_test_inventory.py` が追う。

## 実装後に失効する spec 記述の削除

InputState の状態遷移やリライターの範囲を実装したら、対応する spec の「実装時に決める」
「M-N で追加する」といった記述を、確定した仕様の定義文へ書き換える
（詳細は `azookey-doc-governance` スキル）。`docs/legacy-parity-spec.md` の差分表に
影響する場合は、同じ変更で該当行を直す。
