# テスト一覧

CTest に登録されるテストと、CTest に載らない自動検査の一覧の正典。
`plans/windows-port-roadmap.md`「テスト体系」から参照される。
未解消のカバレッジギャップ（何をまだ検証できていないか）は同章が持つ。

## テストフレームワーク

テストフレームワークは **GoogleTest**、実行ランナーは **CTest** を併用する。
GoogleTest はまず `find_package` でシステムインストール版を探し、見つからず
かつ `-DAZOOKEY_FETCH_GOOGLETEST=ON` が指定されたときのみ `FetchContent` で
ダウンロードする（ネットワーク取得は明示オプトイン）。いずれでも入手できない
場合は警告を出してテストのみスキップし、ビルド自体は継続する（オフライン環境で
`cmake -S . -B build` が失敗しないようにするため）。
各テストは共通ヘルパ `azookey_discover_tests`（内部で `gtest_discover_tests` を呼び出し、
`DISCOVERY_TIMEOUT` は既定 60 秒）により **ケース単位**（`SuiteName.TestName`）で CTest に
登録されるため、下表の各実行ファイルは内部の `TEST()`/`TEST_F()` ごとに個別の CTest
エントリへ展開される。
共通ヘルパはケース単位の `TIMEOUT` と `LABELS` も付与でき、TSF/COM 境界に触る
テストは `tsf-com` label で診断用に抽出できる。
現行 preset では `ctest --preset windows-debug` または
`ctest --preset windows-release` で一括実行する。

## 現存テスト一覧

下表は CMake が登録するテストと 1 行ずつ対応し、`scripts/check_test_inventory.py` が
`CMakeLists.txt` 側の登録との差分を検査する（テストを追加・削除する PR は同じ PR で
本表を更新する）。登録の一部は構成に依存し、`tsf-tip` / `settings-app` / `diagnostics` /
`compat-test` と `host_http_downloader_tests` / `azookey_conversion_quality_smoke` は
`WIN32` のとき、`azookey_nll_real_model_*` と `azookey_zenzai_real_model_*` は
`AZOOKEY_WITH_LLAMA_CPP` かつ `AZOOKEY_ZENZAI_TEST_MODEL` を与えたとき、
`azookey_zenzai_bench_smoke` / `azookey_zenzai_bench_json_smoke` /
`azookey_zenzai_bench_rejects_degraded_mock` は llama.cpp を無効にしたときに登録される。
本表はいずれか一つの構成ではなく、登録されうるテスト全体を対象とする。

| ターゲット | テスト | 主要シナリオ |
|---|---|---|
| `core_tests` | `core/tests/romaji_kana_converter_test.cpp` | `Feed`/`Flush`/`Preview`/`ConvertForCommit`（小書きっ・ん・長音） |
| `core_tests` | `core/tests/simple_converter_test.cpp` | 固定辞書、TSV ロード、prefix fallback、静的 bigram コンテキスト表（suffix/最長一致）、`Correct`、`Learn` |
| `core_tests` | `core/tests/utf8_test.cpp` | UTF-8 デコード/エンコードの符号位置境界と埋め込み NUL、不正シーケンスの 1 バイト消費、suffix の境界保持 |
| `core_tests` | `core/tests/command_line_test.cpp` | wide 引数の UTF-8 変換、非 ASCII パス引数、未対 surrogate と null 引数の reject、パス境界のバイト保持 |
| `core_tests` | `core/tests/redaction_test.cpp` | ユーザープロファイルパスの区切り・ドライブ差を跨ぐ正規化、既知 credential prefix の伏せ字化 |
| `core_tests` | `core/tests/live_conversion_chunker_test.cpp` | ライブ変換の日本語スパンと逐語スパンの分割、UTF-8 encoded surrogate のバイト単位扱い、新規/追記/中間挿入/削除のプラン算出 |
| `core_tests` | `core/tests/number_rewriter_test.cpp` | 数字読みの漢数字・大字・丸数字・ローマ数字展開と範囲制限、混在・不正読みの非展開、uint64 overflow 時の全候補 skip |
| `core_tests` | `core/tests/katakana_rewriter_test.cpp` | 全角→半角カタカナの展開順、濁点・半濁点の分解と長音・ヲの写像、非対応かな混在時の半角抑止、不正 UTF-8 の reject |
| `core_tests` | `core/tests/symbol_rewriter_test.cpp` | 記号 seed の固定順展開と family 非回転、ペア記号と組み込みペアリングの一致、候補マージ時の枠配分と重複除去 |
| `core_tests` | `core/tests/rewriter_index_test.cpp` | リライターデータの互換かな正規化、読み完全一致の順位付けと上限、BOM/CRLF 受理、不正行の skip と sequence 保持 |
| `core_tests` | `core/tests/bracket_pairing_test.cpp` | 括弧テーブルの上書き・追加・無効化、アプリ別 allow/deny ポリシーの重ね合わせ、挿入・skip・空ペア Backspace 削除、対称引用符の境界判定 |
| `core_tests` | `core/tests/app_profile_resolver_test.cpp` | アプリ別プロファイルの部分上書き、`auto`/inherit の解決順、プロセス名・ウィンドウクラス照合と決定的衝突報告、不正値の inherit |
| `runtime_logger_tests` | `core/tests/runtime_logger_test.cpp` | 構造化ログの JSON 行 schema 固定、機微本文の伏せ字化、環境変数 opt-in と level、書込不能先での非 throw、世代ローテーションと保持期間 |
| `core_tests` | `core/tests/crash_retention_test.cpp` | クラッシュ診断の個数・容量・保存期間制限、リンクと無関係なファイルの保護 |
| `core_tests` | `core/tests/etw_logger_test.cpp` | ETW イベントの固定長 payload、数値フィールド、要求の対応付けと終了結果 |
| `crash_reporting_tests` | `core/tests/crash_reporting_test.cpp` | 子プロセスのクラッシュ収集、同意 off と保存不能時の fallback、許可 stream と本文非混入 |
| `ipc_tests` | `ipc/tests/messages_test.cpp` | Envelope シリアライズ、length-prefix フレーミング、`MessageType` mapping |
| `ipc_json_tests` | `ipc/tests/json_test.cpp` | JSON パーサの int64/uint64 精度、深度・入力長上限、Unicode escape、不正入力、round-trip |
| `ipc_payloads_tests` | `ipc/tests/payloads_test.cpp` | Handshake/Ping/Health/LoadModel/QueryCandidates/QueryBatchConversion/Cancel/Commit/UserWord の build/parse + malformed reject |
| `ipc_named_pipe_transport_tests` | `ipc/tests/named_pipe_transport_test.cpp` | サーバ起動 → クライアント接続 → Handshake/Ping ラウンドトリップ、overlapped 即時完了エラー保持、accept churn 下での複数クライアント同時接続（`ConcurrentClientsConnectDuringAcceptChurn`） |
| `ipc_tip_client_tests` | `ipc/tests/tip_client_ipc_test.cpp` | TIP-client 経路（StartDebugIpcProbe 相当）の Handshake → Ping → QueryCandidates、Host 停止 → 再起動をまたぐ client 再接続（`ClientReconnectsAfterHostRestart`） |
| `learning_tests` | `learning/tests/learning_test.cpp` | `LearningStore::Observe/ObserveCorrection/Score`、`Reranker::Apply` 間接テスト |
| `user_dictionary_tests` | `learning/tests/user_dictionary_test.cpp` | Add/Lookup/Remove、Save/Load round trip、missing file、malformed JSON |
| `reranker_tests` | `learning/tests/reranker_test.cpp` | null-store、空 candidates、stable sort、時間減衰、学習ブースト、correction downweight |
| `atomic_file_tests` | `learning/tests/atomic_file_test.cpp` | 原子的書き込み後の一時ファイル残置なし、親ディレクトリ不正・ディレクトリ衝突時の既存ファイル保護、同一パス並行書き込みとファイルロック |
| `host_engine_tests` | `inference-host/tests/engine_test.cpp` | 学習ブースト、user-dict 注入、cancel 早期 return、legacy overload、`LoadModel` の GGUF 実プローブ（最小ヘッダ受理・不正 GGUF reject・CPU backend ロード成功・path 空時の MVP fallback）、`--backend cuda` 指定時の CPU フォールバック |
| `host_engine_tests` | `inference-host/tests/nll_scorer_test.cpp` | NLL 再スコアの prefix スナップショットと次トークン位置、log-softmax の数値安定性、Unicode スカラ単位の正規化、timeout/cancel 時の候補不変とサーキット開放、モデル未ロード時の既定維持 |
| `host_engine_tests` | `inference-host/tests/rewriter_test.cpp` | リライターの遅延ロードと一度きりロード、データ欠落時の非リトライと不正行 skip、手動クエリのみのマージと通常候補数の保持、同梱データの round-trip と index 予算 |
| `host_dispatcher_tests` | `inference-host/tests/dispatcher_test.cpp` | Handshake/Ping/QueryCandidates/QueryBatchConversion/Cancel/Commit/AddUserWord/RemoveUserWord/Health の主要ハンドラ |
| `host_dispatcher_tests` | `inference-host/tests/ai_backend_test.cpp` | AI 整文バックエンドの privacy 既定と secure/disabled 時の非送信、retry 上限と cancel 後の遅延成功の非公開、応答の UTF-8・NUL・サイズ検証 |
| `host_dispatcher_tests` | `inference-host/tests/host_etw_test.cpp` | Host の要求・推論・学習フェーズの ETW 対応付けと結果 |
| `host_scheduler_tests` | `inference-host/tests/scheduler_test.cpp` | `NextRequestId` 連番、`Cancel`/`IsCanceled`、`MarkLatest`/`IsLatest`、thread-safety smoke |
| `host_args_tests` | `inference-host/tests/args_test.cpp` | `--backend` 別名と未対応値の reject、`--pipe` のオプション非消費、handshake・パス系オプション、非 ASCII パスの UTF-8 保持 |
| `host_args_tests` | `inference-host/tests/startup_test.cpp` | supervisor 未指定時の寿命非制限、不正 supervisor の fail-closed、保持ハンドル経由のプロセス終了監視、mock host が Vulkan を広告しないこと |
| `host_user_data_paths_tests` | `inference-host/tests/user_data_paths_test.cpp` | `UserDataPaths` のパス解決（root/config/data/logs/models、`learning.tsv`/`user_dict.json`） |
| `host_model_catalog_tests` | `inference-host/tests/model_catalog_test.cpp` | モデルカタログの既定補完と明示既定 id、不正・重複 id と未知既定の reject、`models\zenzai\` 配下での解決、ローカル欠落の報告 |
| `host_http_downloader_tests` | `inference-host/tests/http_downloader_test.cpp` | SHA256 不一致時の `.part` 非昇格、Range 再開・Range 無視サーバでの再取得、上限超過応答の非昇格、非 loopback 平文 HTTP の接続前 reject |
| `host_userdict_cli_tests` | `inference-host/tests/userdict_cli_test.cpp` | `userdict` CLI の add/list/remove ラウンドトリップ、dry-run、稼働中 Host 優先と直接編集の使い分け、import/export と非 ASCII パス保持 |
| `host_lookup_cli_tests` | `inference-host/tests/lookup_cli_test.cpp` | `lookup` CLI の読み完全一致・読み前置一致・表記一致、ロック取得不可時の失敗、TSV/JSON の列、破損ユーザー辞書を隔離も変更もしないこと |
| `host_settings_store_tests` | `inference-host/tests/settings_store_test.cpp` | 設定の既定値補完とクランプ、model ブロックによる backend 上書き、推論スレッド数の電源プロファイル追従、不正 JSON の隔離と現行設定の維持 |
| `host_cli_unicode_argv` | `azookey_inference_host` | 実プロセスの argv 境界で非 ASCII 引数が UTF-8 のまま CLI に届くこと |
| `dictionary_tests` | `dictbuild/tests/dictionary_test.cpp` | 辞書 trie の探索方向と最短優先の上限、破損検出、参照失敗時の該当レイヤのみ無効化、静的辞書と可変辞書の独立、ユーザー変更の追跡 |
| `dictbuild_python_tests` | `dictbuild/tests/test_dictbuild.py` | オフライン辞書ビルダ（Python）の単体テスト |
| `diagnostics_tests` | `diagnostics/tests/diagnostics_test.cpp` | 診断 JSON schema の固定、機微本文の除外とランタイムログのバイト上限、zip 収集物の限定、`--repair` の冪等性と失敗時の非成功報告 |
| `diagnostics_cli_rejects_help_with_json` | `azookey_diag` | `--help` と `--json` の併用を非 0 終了で拒否 |
| `diagnostics_cli_rejects_repair_with_json` | `azookey_diag` | `--repair` と `--json` の併用を非 0 終了で拒否 |
| `tsf_tip_com_smoke_tests` | `tsf-tip/tests/com_smoke_test.cpp` | DLL `DllGetClassObject` → `IClassFactory::CreateInstance(IID_IUnknown)`、`ActivateEx` の sink advise / unadvise。登録 round-trip（`RegisterPublishesProfileAndUnregisterRemovesIt`、`FailedCategoryRegistrationRollsBackAndRetrySucceeds`）は opt-in 環境変数 `AZOOKEY_RUN_REGISTRATION_SMOKE` + 昇格時のみ実行で、CI では走らない（roadmap「既知のテストギャップ」1） |
| `tsf_tip_onkeydown_preedit_tests` | `tsf-tip/tests/onkeydown_preedit_test.cpp` | `OnKeyDown`/`OnTestKeyDown` で romaji→kana preedit 蓄積、Backspace（pending romaji / UTF-8 単位 kana 削除）、Escape クリア、Space で pending flush、preedit 無し時の制御キー非消費 |
| `tsf_tip_display_attribute_tests` | `tsf-tip/tests/display_attribute_test.cpp` | `ITfDisplayAttributeProvider`（`GetDisplayAttributeInfo`/`EnumDisplayAttributeInfo`）と `InputDisplayAttributeInfo`（GUID/説明/下線属性、`Next`/`Reset`/`Skip`/`Clone`、null 引数 reject） |
| `tsf_tip_activate_uiless_tests` | `tsf-tip/tests/activate_uiless_test.cpp` | `ActivateEx` が `ITfThreadMgrEx::GetActiveFlags`（`dwFlags` ではなく）から UI-less 状態を導出する（spec §2.10） |
| `tsf_tip_staleness_tests` | `tsf-tip/tests/staleness_test.cpp` | 連続応答のうち最新のみ受理、より新しいリクエストが queue 済みの応答の破棄、commit で無効化された応答の破棄 |
| `tsf_tip_caret_position_tests` | `tsf-tip/tests/caret_position_test.cpp` | text extent 優先のキャレット位置決定と物理座標正規化、変換失敗時の座標保持、GUI スレッドキャレットと物理カーソルへの段階的 fallback、フォーカス喪失・コンテキスト push でのキャッシュ破棄 |
| `tsf_tip_candidate_ui_coordinator_tests` | `tsf-tip/tests/candidate_ui_coordinator_test.cpp` | 候補 UI の app-drawn / TIP 描画切替、`BeginUIElement` 失敗の HRESULT 報告、UI-less 時の `ITfUIElementMgr` 要求と欠落時 fallback、選択移動の wrap |
| `tsf_tip_candidate_window_dpi_tests` | `tsf-tip/tests/candidate_window_dpi_test.cpp` | 候補ウィンドウのレイアウト metrics の DPI スケール、DPI 0 の既定 fallback、絵文字判定が漢字・文字記号を巻き込まないこと、description 有無での列構成 |
| `tsf_tip_query_interface_contract_tests` | `tsf-tip/tests/query_interface_contract_test.cpp` | `QueryInterface` の null out-param と未対応 IID の契約、`ITfFnConfigure`／`ITfFunction` 公開、`Show` からのプロファイル付き設定アプリ起動と失敗時 HRESULT |
| `tsf_tip_local_settings_tests` | `tsf-tip/tests/local_settings_test.cpp` | Host 非依存で共有設定ファイルを読む TIP ローカル設定、ローマ字テーブル変更の監視と再読み込み、Unicode パス・再作成ディレクトリへの再バインド、不正・過大ファイルでの既定復帰 |
| `azookey_settings_launch_arguments_tests` | `settings-app/tests/launch_arguments_test.cpp` | 設定アプリ起動引数の round-trip、値欠落・不正 LangId / プロファイルの reject、未指定と空指定の区別、重複・未知オプションの reject |
| `azookey_settings_persistence_tests` | `settings-app/tests/settings_document_test.cpp` | 設定ドキュメントの既定値と隠しキー保持、不正エントリの除去、ロック・読み取り失敗時の既存ファイル不変、不正文書の隔離と原子的保存による復旧 |
| `azookey_settings_persistence_tests` | `settings-app/tests/settings_ipc_client_test.cpp` | 設定アプリから Host への Handshake と `UpdateConfig` 送信 |
| `compat_test_unit_tests` | `compat-test/tests/compat_test_unit_tests.cpp` | 互換ハーネスの target 定義検証（自動化契約・既知回避策・一時文書の所有）、レポート schema と非信頼テキストの伏せ字化、クリップボード復元、ウィンドウ所有権判定 |
| `temporary_learning_file_tests` | `bench/temporary_learning_file_test.cpp` | bench 用一時学習ファイルの並行予約時の独立性、他所有者への非干渉、巻き戻し時の後始末と想定外ファイルの保全 |
| `benchmark_result_tests` | `bench/benchmark_result_test.cpp` | bench JSON schema の固定、baseline 比較の閾値と絶対ノイズ床、baseline 欠落・非互換時の非回帰扱い、UTF-8 出力パス |
| `conversion_quality_tests` | `bench/conversion_quality_test.cpp` | 変換品質の符号位置単位 CER、canonical / acceptable 一致の区別、raw と NFKC の独立集計、不正 UTF-8 の reject、データセットハッシュの改行正規化 |
| `azookey_bench_smoke` | `azookey_bench` | CPU `SimpleConverter` 経路の p50/p95/p99 出力、p95 < 50ms |
| `azookey_bench_json_smoke` | `azookey_bench` | JSON 出力の schema 固定と `passed` 真、人間向け行の非混入 |
| `azookey_bench_ipc_smoke` | `azookey_bench` | `--ipc` のフェーズ別レイテンシ出力（serialize / framing / deserialize / pipe round-trip）とサンプル数 |
| `azookey_conversion_quality_smoke` | `azookey_bench` | 変換品質評価の集計出力・per-case 出力・baseline 比較（非互換 baseline を含む）の生成物検証 |
| `azookey_nll_fixture_smoke` | `bench/BuildNllFixture.cmake` | NLL 実モデルテストが要求する辞書 fixture の生成（fixture setup） |
| `azookey_zenzai_bench_missing_model_output` | `azookey_zenzai_bench` | モデル未指定時に degraded を JSON へ記録し、実行自体は落ちないこと |
| `azookey_zenzai_bench_smoke` | `azookey_zenzai_bench` | mock Zenzai 経路の実行（llama.cpp 無効ビルド） |
| `azookey_zenzai_bench_json_smoke` | `azookey_zenzai_bench` | mock Zenzai 経路の JSON schema 固定と `passed` 真、`status=ok` 行の非混入 |
| `azookey_zenzai_bench_rejects_degraded_mock` | `azookey_zenzai_bench` | `--require-zenzai` 指定時に degraded な mock 結果を失敗として扱うこと |
| `azookey_nll_real_model_context` | `azookey_nll_bench` | pin した実 Zenzai GGUF と fixture 辞書で、文脈ありの NLL 再スコア最上位を検証 |
| `azookey_nll_real_model_no_context` | `azookey_nll_bench` | 同じ pin モデルと fixture 辞書で、文脈なしの NLL 再スコア最上位を検証 |
| `azookey_zenzai_real_model_nihongo_smoke` | `azookey_zenzai_bench` | immutable revision + SHA256 で pin した実 Zenzai GGUF を upstream llama.cpp でロードし、`にほんご` → `日本語` の厳密一致、Zenzai 候補あり、`utf8-prefix-trimmed` 不在、参照実装と同じ prompt token ID 列を検証 |
| `azookey_zenzai_real_model_sentence_smoke` | `azookey_zenzai_bench` | 同じ pin モデルで `わたしはがくせいです` → `私は学生です` の厳密一致、Zenzai 候補あり、`utf8-prefix-trimmed` 不在、参照実装と同じ prompt token ID 列を検証 |
| `azookey_zenzai_real_model_json_smoke` | `azookey_zenzai_bench` | 同じ pin モデルでの JSON 出力の schema 固定と `passed` 真、`status=ok` 行の非混入 |

## CTest 以外の自動検査

CTest に載らない検査は次のとおり。CTest の一覧と混在させず、実行系統ごとに分けて把握する。

| 検査 | 実行系統 | 内容 |
|---|---|---|
| `.github/workflows/sanitizers.yml` | GitHub Actions（`cron: 17 2 * * 1` の週次 + 手動 dispatch） | `linux-asan-ubsan`（ASan + UBSan）で `core`/`ipc`/`learning`/`inference-host` を、`windows-asan`（MSVC ASan）でこれに `tsf-tip` を加えた全体を検査。頻度・対象・preset の内訳は `docs/dev-infrastructure-spec.md` §4.6 が正典 |
| `.github/workflows/secret-scan.yml` / `.github/workflows/windows.yml` の `quality` / `linux-no-tests` / `windows-no-tests` | GitHub Actions（PR / `main` push / 手動 dispatch） | path 除外なしの secret scan で PR commit range または作業ツリーを gitleaks 走査。pre-commit の actionlint / taplo（yamlfmt の既存 baseline は DEV-913）、`AZOOKEY_BUILD_TESTS=OFF` + bench 無効の Linux / Windows build を独立ジョブで検証（`docs/dev-infrastructure-spec.md` §4.3） |
| `.github/workflows/docs.yml` の `docs-lint` | GitHub Actions（PR / `main` push） | `scripts/docs-lint.py` の文書ドリフト検査（DECISIVE はベースライン 0 件で凍結）、`scripts/check_agent_instruction_size.py` の `AGENTS.md` バイト予算、`scripts/check_test_inventory.py` による「現存テスト一覧」と `CMakeLists.txt` 登録の突合 |
| `scripts/tests/msix-identity-consistency.Tests.ps1` | Pester（CI） | MSIX identity manifest と `kTextServiceClsid` / `kTextServiceProfileGuid` / `kJapaneseLangId` の静的整合、Option A の不変条件、ビルド埋め込み配線 |
| `scripts/doctor.ps1`（`just doctor`） | 開発者・エージェントの手元 | 不足ツール・未初期化 dev shell・未取得依存の診断（`docs/dev-infrastructure-spec.md` §2.5。§12 の `azookey_diag.exe` とは別物） |
