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
| `core_tests` | `core/tests/simple_converter_test.cpp` | 固定辞書、TSV ロード、prefix fallback、静的 bigram コンテキスト表（suffix/最長一致）、`Correct`、`Learn`、`Contains` が実辞書エントリと `Learn` 由来の確定履歴を区別すること |
| `core_tests` | `core/tests/utf8_test.cpp` | UTF-8 デコード/エンコードの符号位置境界と埋め込み NUL、不正シーケンスの 1 バイト消費、suffix の境界保持 |
| `core_tests` | `core/tests/command_line_test.cpp` | wide 引数の UTF-8 変換、非 ASCII パス引数、未対 surrogate と null 引数の reject、パス境界のバイト保持 |
| `core_tests` | `core/tests/redaction_test.cpp` | ユーザープロファイルパスの区切り・ドライブ差を跨ぐ正規化、既知 credential prefix の伏せ字化 |
| `core_tests` | `core/tests/live_conversion_chunker_test.cpp` | ライブ変換の日本語スパンと逐語スパンの分割、UTF-8 encoded surrogate のバイト単位扱い、新規/追記/中間挿入/削除のプラン算出 |
| `core_tests` | `core/tests/number_rewriter_test.cpp` | 数字読みの漢数字・大字・丸数字・ローマ数字展開と範囲制限、混在・不正読みの非展開、uint64 overflow 時の全候補 skip |
| `core_tests` | `core/tests/katakana_rewriter_test.cpp` | 全角→半角カタカナの展開順、濁点・半濁点の分解と長音・ヲの写像、非対応かな混在時の半角抑止、不正 UTF-8 の reject |
| `core_tests` | `core/tests/character_form_cycle_test.cpp` | ひらがなからカタカナ・半角カタカナ・全角英数・ASCII へ進む文字種循環と非対応入力の判定 |
| `core_tests` | `core/tests/symbol_rewriter_test.cpp` | 記号 seed の固定順展開と family 非回転、ペア記号と組み込みペアリングの一致、候補マージ時の枠配分と重複除去 |
| `core_tests` | `core/tests/rewriter_index_test.cpp` | リライターデータの互換かな正規化、読み完全一致の順位付けと上限、BOM/CRLF 受理、不正行の skip と sequence 保持 |
| `core_tests` | `core/tests/input_state_test.cpp` | 入力状態機械の全状態 × 全 `UserAction` の遷移表、候補の往復（表示・巡回・数字選択・確定と学習観測）、cache miss 時に `Selecting` へ入らず応答到着で遷移すること、応答待ち中の Enter の as-is 確定、Backspace の削除単位、モード切替での reading 保持、Unicode 入力の範囲検査と桁数上限、`WithComposition` / `Reset` による状態受け渡し、Selecting 中の通常入力で Composing に戻ること 、単独の明示句読点による composition 開始と Backspace、ライブ変換 ON の `Previewing` 遷移と候補選択からの復帰 |
| `core_tests` | `core/tests/m59_input_state_test.cpp` | 自動句読点を読みバッファに含めず、Backspace がかなを 1 単位削除すること |
| `core_tests` | `core/tests/punctuation_rules_test.cpp` | M59 の組み込み句読点規則、TSV 上書き・無効化、不正行スキップ、guard 構文と Unknown の判定 |
| `core_tests` | `core/tests/bracket_pairing_test.cpp` | 括弧テーブルの上書き・追加・無効化、アプリ別 allow/deny ポリシーの重ね合わせ、挿入・skip・空ペア Backspace 削除、対称引用符の境界判定 |
| `core_tests` | `core/tests/app_profile_resolver_test.cpp` | アプリ別プロファイルの部分上書き、`auto`/inherit の解決順、プロセス名・ウィンドウクラス照合と決定的衝突報告、不正値の inherit |
| `runtime_logger_tests` | `core/tests/runtime_logger_test.cpp` | 構造化ログの JSON 行 schema 固定、機微本文の伏せ字化、環境変数 opt-in と level、Debug・本文 opt-in・非secure・詳細ログ許可の積による本文出力、Release の強制 redaction、イベント間・並行呼出し間の許可分離、書込不能先での非 throw、世代ローテーションと保持期間 |
| `core_tests` | `core/tests/crash_retention_test.cpp` | クラッシュ診断の個数・容量・保存期間制限、リンクと無関係なファイルの保護 |
| `core_tests` | `core/tests/etw_logger_test.cpp` | ETW イベントの固定長 payload、数値フィールド、要求の対応付けと終了結果 |
| `crash_reporting_tests` | `core/tests/crash_reporting_test.cpp` | 子プロセスのクラッシュ収集、main / worker の実スタックオーバーフローと同意 off、保存不能時の fallback、許可 stream と本文非混入 |
| `ipc_tests` | `ipc/tests/messages_test.cpp` | Envelope シリアライズ、length-prefix フレーミング、`MessageType` mapping（`ObserveTypo` / `ListNewWordCandidates` / `ResolveNewWord` / `ReverseConvert` の名前往復を含む）、`QueryLiveConversion` / `QueryPredictions` の型名往復 |
| `ipc_json_tests` | `ipc/tests/json_test.cpp` | JSON パーサの int64/uint64 精度、深度・入力長上限、Unicode escape、不正入力、round-trip |
| `ipc_payloads_tests` | `ipc/tests/payloads_test.cpp` | Handshake/Ping/Health/LoadModel/QueryCandidates/QueryBatchConversion/Cancel/Commit/UserWord/ReverseConvert の build/parse + malformed reject、`ObserveTypo` と `QueryCandidatesResponse.corrected_reading` の往復と欠如時の後方互換、`QueryCandidates` と学習イベントの privacy フラグの往復および欠落・型不正時の安全側の既定値、`ListNewWordCandidates` / `ResolveNewWord` の往復と不正 `state_filter`・`max_items`・`action` の reject、両応答の `ok` / `changed` / `error` の往復と欠如時の後方互換 、M59 の `auto_punctuation` / `punctuation_style` と `segments[]` の往復・欠落時既定値、ライブ変換要求・応答の往復と不正値拒否、予測 Word mode の要求・応答と欠落・型不正値の拒否 |
| `ipc_named_pipe_transport_tests` | `ipc/tests/named_pipe_transport_test.cpp` | サーバ起動 → クライアント接続 → Handshake/Ping ラウンドトリップ、overlapped 即時完了エラー保持、accept churn 下での複数クライアント同時接続（`ConcurrentClientsConnectDuringAcceptChurn`）、短いヘッダー・本文不足・ゼロ長の固定バイナリ fixture の切断と期限内終了 |
| `ipc_handshake_token_tests` | `ipc/tests/handshake_token_test.cpp` | 暗号乱数 token の生成、原子的な更新と読取、不正・欠落値の拒否、ファイル ACL |
| `ipc_tip_client_tests` | `ipc/tests/tip_client_ipc_test.cpp` | TIP-client 経路（StartDebugIpcProbe 相当）の Handshake → Ping → QueryCandidates、Host 停止 → 再起動をまたぐ client 再接続（`ClientReconnectsAfterHostRestart`） |
| `learning_tests` | `learning/tests/learning_test.cpp` | `LearningStore::Observe/ObserveCorrection/Score`、`Reranker::Apply` 間接テスト |
| `dpapi_crypto_tests` | `learning/tests/dpapi_crypto_test.cpp` | ユーザースコープ DPAPI の往復、暗号化ファイル移行、復号失敗時の平文 fallback 拒否、モック暗号境界、非 Windows での暗号利用不可時の拒否 |
| `user_dictionary_tests` | `learning/tests/user_dictionary_test.cpp` | Add/Lookup/Remove、Save/Load round trip、missing file、malformed JSON |
| `reranker_tests` | `learning/tests/reranker_test.cpp` | null-store、空 candidates、stable sort、時間減衰、学習ブースト、correction downweight |
| `typo_correction_store_tests` | `learning/tests/typo_correction_store_test.cpp` | M35 打ち間違えペアの頻度カウントとしきい値境界、UTF-8 コードポイント単位の編集距離と長さ・同一・空読みフィルタ、`last_updated` 経過による無視、Save/Load ラウンドトリップとタブ・改行のエスケープ、ファイル無し・破損行の読み飛ばし、`Reset` |
| `auto_word_store_tests` | `learning/tests/auto_word_store_test.cpp` | M36-A pending / confirmed / rejected の状態遷移、`confirm` モードでの非自動昇格と `auto` モードの閾値昇格、却下語の再提示抑止、`IngestTrending` の rejected skip と mining ソース優先、`PrunePending` の対象限定、`SetState` の直前状態の返却と `CompareAndSetState` の条件付き変更、Save/Load ラウンドトリップとエスケープと score の値保存、ファイル無し・破損行の読み飛ばし、`Reset` |
| `atomic_file_tests` | `learning/tests/atomic_file_test.cpp` | 原子的書き込み後の一時ファイル残置なし、親ディレクトリ不正・ディレクトリ衝突時の既存ファイル保護、同一パス並行書き込みとファイルロック |
| `host_engine_tests` | `inference-host/tests/punctuation_inserter_test.cpp` | M59 の文節境界・文末句読点、抑制、字種切替と `Unknown` 品詞の表層フォールバック |
| `host_engine_tests` | `inference-host/tests/engine_test.cpp` | 学習ブースト、user-dict 注入、cancel 早期 return、legacy overload、`LoadModel` の GGUF 実プローブ（最小ヘッダ受理・不正 GGUF reject・CPU backend ロード成功・path 空時の MVP fallback）、`--backend cuda` 指定時の CPU フォールバック、M35 suggest のマーク付き候補注入と閾値未満での非適用・`auto_replace` の `corrected_reading`・`off` の無変化と非学習、M36-A の OOV マイニング蓄積と `confirm` / `auto` モード差、既知語・記号・数字・ASCII・非かな読みの除外、mining 無効時と store 未設定時の no-op、observation_id 重複の非二重計上、Zenzai 利用時もフォールバック変換器の辞書語を既知語とすること、confirmed 語の `auto-word` 注入と既定スコア・pending 語の非注入・`confirm` モードで承認まで注入しないこと、M47 のモデルロード結果による `DegradedModel` / `RecoveringModel` / `Healthy` の遷移と差し替え失敗の非劣化扱い、`SafeMode` がモデル系イベントで抜けずロード要求を `safe_mode` で拒否すること、軽量ライブ変換の最良候補 |
| `host_engine_tests` | `inference-host/tests/nll_scorer_test.cpp` | NLL 再スコアの prefix スナップショットと次トークン位置、log-softmax の数値安定性、Unicode スカラ単位の正規化、timeout/cancel 時の候補不変とサーキット開放、モデル未ロード時の既定維持 |
| `host_engine_tests` | `inference-host/tests/rewriter_test.cpp` | リライターの遅延ロードと一度きりロード、データ欠落時の非リトライと不正行 skip、手動クエリのみのマージと通常候補数の保持、同梱データの round-trip と index 予算 |
| `host_dispatcher_tests` | `inference-host/tests/dispatcher_test.cpp` | Handshake/Ping/QueryCandidates/QueryBatchConversion/ReverseConvert/Cancel/Commit/AddUserWord/RemoveUserWord/Health の主要ハンドラ、`UpdateConfig` を待たずに Handshake が更新後の settings.json から TIP 向け設定を返し、その読み取りが runtime 設定を置き換えないこと、学習イベント自身の privacy と接続単位の `secure_flag` による拒否・再Handshake時の能力リセット・拒否時の重複排除ID保持、モデル再ロードの設定ロック保持中も学習要求が応答する競合回帰、Host secure 設定による学習とマイニングの抑止、`ObserveTypo` の無応答とストア更新、`QueryCandidates` 応答の `corrected_reading`、`ListNewWordCandidates` / `ResolveNewWord` の承認フローと store 未設定時の応答、同じ操作の繰り返しの `changed=false`、各 `error` コード、保存失敗時の状態の巻き戻し、承認後にはじめて候補へ注入されること、実行中の変換へ out-of-band `Cancel` が届いて打ち切られること、`SafeMode` の `fallback_state=safe_mode` 優先・`LoadModel` 拒否・`UpdateConfig` での解除、ライブ変換の要求 ID、キャンセル、信頼度の飽和回避、Host capability、予測 Word mode の相関 ID・認証・キャンセル・古い応答抑止・未対応 mode |
| `host_dispatcher_tests` | `inference-host/tests/ai_backend_test.cpp` | AI 整文バックエンドの privacy 既定と secure/disabled 時の非送信、retry 上限と cancel 後の遅延成功の非公開、応答の UTF-8・NUL・サイズ検証 |
| `host_dispatcher_tests` | `inference-host/tests/host_etw_test.cpp` | Host の要求・推論・学習フェーズの ETW 対応付けと結果 |
| `host_health_state_tests` | `inference-host/tests/health_state_machine_test.cpp` | M47 の健康状態機械の全遷移と表外の組の拒否・自己遷移なし、model 復旧中は transport の回復で `Healthy` に戻らないこと、`SafeMode` が解除イベント以外で抜けないこと、連続クラッシュ履歴の 60 秒窓・正常終了でのリセット・時計の巻き戻り、履歴ファイルの往復と不正内容の破棄、自プロセスの印の判定と PID 再利用の区別、`enteredAt` の RFC 3339 形式 |
| `host_scheduler_tests` | `inference-host/tests/scheduler_test.cpp` | `NextRequestId` 連番、`Cancel`/`IsCanceled`、`MarkLatest`/`IsLatest`、thread-safety smoke |
| `host_args_tests` | `inference-host/tests/args_test.cpp` | `--backend` 別名と未対応値の reject、`--pipe` のオプション非消費、handshake・パス系オプション、非 ASCII パスの UTF-8 保持、`userdict` / `lookup` / `newwords` サブコマンドの残り引数の受け渡し |
| `host_args_tests` | `inference-host/tests/startup_test.cpp` | supervisor 未指定時の寿命非制限、不正 supervisor の fail-closed、保持ハンドル経由のプロセス終了監視、mock host が Vulkan を広告しないこと |
| `host_user_data_paths_tests` | `inference-host/tests/user_data_paths_test.cpp` | `UserDataPaths` のパス解決（root/config/data/logs/models、`learning.tsv`/`user_dict.json`、`typo_corrections.tsv`/`auto_words.tsv`）、`--learning-path` 明示時に新ストアが同階層へ解決されること |
| `host_model_catalog_tests` | `inference-host/tests/model_catalog_test.cpp` | モデルカタログの既定補完と明示既定 id、不正・重複 id と未知既定の reject、`models\zenzai\` 配下での解決、ローカル欠落の報告 |
| `host_http_downloader_tests` | `inference-host/tests/http_downloader_test.cpp` | SHA256 不一致時の `.part` 非昇格、Range 再開・Range 無視サーバでの再取得、上限超過応答の非昇格、非 loopback 平文 HTTP の接続前 reject |
| `host_userdict_cli_tests` | `inference-host/tests/userdict_cli_test.cpp` | `userdict` CLI の add/list/remove ラウンドトリップ、dry-run、稼働中 Host 優先と直接編集の使い分け、import/export と非 ASCII パス保持 |
| `host_lookup_cli_tests` | `inference-host/tests/lookup_cli_test.cpp` | `lookup` CLI の読み完全一致・読み前置一致・表記一致、ロック取得不可時の失敗、TSV/JSON の列、破損ユーザー辞書を隔離も変更もしないこと |
| `host_newwords_cli_tests` | `inference-host/tests/newwords_cli_test.cpp` | M36-A `newwords` CLI の引数検証、`list` の並びと JSON/TSV 出力、`--offline` の直接編集と冪等な再実行、Host 不在時にファイルを変更しないこと、稼働中の Dispatcher 経由の confirm で pending → confirmed → 候補注入まで通ることと Host の `error` の伝達 |
| `host_settings_store_tests` | `inference-host/tests/settings_store_test.cpp` | 設定の既定値補完とクランプ、model ブロックによる backend 上書き、推論スレッド数の電源プロファイル追従、不正 JSON の隔離、一般設定の維持、privacy 許可のリセットと正常設定での復旧、専用 mutex による policy 公開と学習判定・書込みの直列化、読み込み済みより新しいファイルだけを隔離せずに読む Handshake 用の先読み、privacy の secure・詳細ログ許可・欠落と不正値の安全側解決、`typoCorrectionMode`/`typoMinCount`/`autoWordRegistration.*` の既定値・範囲外値の既定復帰・`EngineConfig` への反映、`safeMode.*` の解釈と SafeMode 中の AI・学習・モデルの強制停止、SafeMode の記録が他のキーを保ち欠落ファイルを作り解釈できないファイルを書き換えないこと 、M59 の 6 設定キーの既定・有効・範囲外値と EngineConfig 反映 |
| `host_cli_unicode_argv` | `azookey_inference_host` | Windows の実プロセス argv 境界で非 ASCII 引数が UTF-8 のまま CLI に届くこと |
| `dictionary_tests` | `dictbuild/tests/dictionary_test.cpp` | 辞書 trie の探索方向と最短優先の上限、破損検出、参照失敗時の該当レイヤのみ無効化、静的辞書と可変辞書の独立、ユーザー変更の追跡、表層形の完全一致逆引きとレイヤ・ユーザー語の優先順 |
| `dictbuild_python_tests` | `dictbuild/tests/test_dictbuild.py` | オフライン辞書ビルダ（Python）の単体テスト |
| `diagnostics_tests` | `diagnostics/tests/diagnostics_test.cpp` | 診断 JSON schema の固定、機微本文の除外とランタイムログのバイト上限、zip 収集物の限定、D-014 の実効 OpenAI backend と DPAPI 状態、D-015 の既知非対応・未確認理由、`--repair` の冪等性と失敗時の非成功報告 |
| `diagnostics_cli_rejects_help_with_json` | `azookey_diag` | `--help` と `--json` の併用を非 0 終了で拒否 |
| `diagnostics_cli_rejects_repair_with_json` | `azookey_diag` | `--repair` と `--json` の併用を非 0 終了で拒否 |
| `tsf_tip_com_smoke_tests` | `tsf-tip/tests/com_smoke_test.cpp` | DLL `DllGetClassObject` → `IClassFactory::CreateInstance(IID_IUnknown)`、`ActivateEx` の sink advise / unadvise。登録 round-trip（`RegisterPublishesProfileAndUnregisterRemovesIt`、`FailedCategoryRegistrationRollsBackAndRetrySucceeds`）は opt-in 環境変数 `AZOOKEY_RUN_REGISTRATION_SMOKE` + 昇格時のみ実行で、CI では走らない（roadmap「既知のテストギャップ」1） |
| `tsf_tip_onkeydown_preedit_tests` | `tsf-tip/tests/onkeydown_preedit_test.cpp` | `OnKeyDown`/`OnTestKeyDown` で romaji→kana preedit 蓄積、Backspace（pending romaji / UTF-8 単位 kana 削除）、Escape クリア、Space で pending flush、preedit 無し時の制御キー非消費、InputState 経由の数字候補確定で表示スナップショットと学習 metadata が一致すること、`TsfTipBracketTest` によるカッコペアリング（ペア挿入・skip・空ペア削除・選択囲み・composition の確定と取消・per-app 有効範囲）の `OnKeyDown` 経由の結合検証、`TsfTipSecureInputTest` による M46 secure 抑止（バンドル既定 / ユーザー追加の secureApps 一致、入力先アプリ解決不能、明示 secure 設定、neural バッチ確定、`CommitObservation` / `CommitSegmentsObservation` 抑止と secure ↔ normal 復帰、要求ごとの wire フラグと再送時の保持、`secure_flag` 広告）、`TsfTipOnKeyDownPreeditTest.PrivacyProbeSyntheticLatencyAndCallsPerKey` による同期 scope probe の 1 回/key 検証と `AZOOKEY_TSF_PRIVACY_BENCH=1` での opt-in synthetic latency 測定（実アプリの遅延保証には用いない）、予測候補の読みの非拡張候補除外と追記 suffix、実 Named Pipe の fake Host に対する IPC ワーカーの接続状態遷移（Handshake 拒否中は `Ready` に入らない、Host 停止で `Ready` を離れ再起動で戻る）と idle 時の `Health` 監視（応答中は `Ready` 維持、無応答が閾値回続くと `Degraded`・応答再開で `Ready`、入力し続けて idle にならない間も `QueryCandidates` の deadline 超過で `Degraded` に入り応答で戻る、無応答 Host への監視待ちが `QueryCandidates` を遅らせない）、Host 不在時の TIP 内ローカルフォールバック（通常入力・一括変換とも Space で待たずにかな・カタカナ候補を表示し、確定したかなが `CommitObservation` に載らない。一括変換の応答待ち中に Host が落ちても Space と Enter が塞がれない、`Degraded` では一括変換を一度 Host へ送り次の Space でローカル候補へ切り替える）、外部 composition 終了とフォーカス離脱の順序別確定（既存文字の重複回避・空範囲の復元・入力先による置換の尊重・Esc 取消し）、設定変更による同一接続の再 Handshake（残留 Health 応答の読み飛ばし・次の要求への batch 設定反映・拒否時の観測キュー再送と失効した Cancel の破棄） 、単独 `、。` と半角候補の Space・Enter・数字確定、Backspace・Esc 整合、専用 IPC、従来 Host fallback、preedit 表示、古い応答、Backspace・Esc、secure 入力での抑止、表示と確定の一致、従来リライター経路の候補照会とライブ表示 |
| `tsf_tip_bracket_edit_session_tests` | `tsf-tip/tests/bracket_edit_session_test.cpp` | `BracketEditSession` を `OnKeyDown` を介さず直接呼ぶ単体契約。`RunSync` の 同期セッション契約（context 無し・実行されなかったセッション・遅延実行されたセッションの不発）、`ReadHint` の隣接 1 文字読取と読取失敗時に空ではなく unknown な hint を返すこと、immediate ペア挿入が単一 SetText と対の内側への collapsed なキャレット設定になること、書込ロックの要求、キャレット操作が失敗しても 挿入済みテキストを再適用しないこと（skip は逆に applied を立てないこと）、composition の開始・範囲取得が失敗したときの後始末、SetText 後の range が両端どちらで collapsed になっても、また片方向の shift を拒否されても対の内側へキャレットが入ること、どの経路も確認できないときはキャレットを動かさず、隣接文字を読めないときは末尾基準の位置に留めること |
| `tsf_tip_character_form_edit_session_tests` | `tsf-tip/tests/character_form_edit_session_test.cpp` | 選択範囲と直近確定範囲の文字種循環、選択・caret・context の再検証、非対応入力と失敗時の非適用、置換後の二重編集防止 |
| `tsf_tip_reconversion_function_tests` | `tsf-tip/tests/reconversion_function_test.cpp` | `ITfFnReconversion` の範囲判定・候補一覧・選択結果適用の COM 契約、部分読取時の置換防止、変換キーから fake Host の逆引き・候補取得までの往復、cache の context・privacy ガード |
| `tsf_tip_ai_input_guard_tests` | `tsf-tip/tests/ai_input_guard_test.cpp` | AI 整文と M46 secure 抑止が共有する privacy gate。`ClassifyInputScope` の password / PIN scope 検出、scope 0 件・`GUID_PROP_INPUTSCOPE` 未提供・`GetValue` 失敗・`VT_UNKNOWN` でない・`GetInputScopes` 失敗・`GetSelection` 失敗・編集セッションの非同期実行と拒否がいずれも `Unknown`（AI は fail closed で拒否、secure は誤検出しない）になること、`ClassifyFocusWindow` の `ES_PASSWORD` 検出、`kDefaultSecureApps` が spec §4.1 の 7 件と一致すること、実効リストがバンドル既定とユーザー追加の和集合で大文字小文字を無視すること、`privacy.secureApps` の不正値が既定のみへ縮退すること |
| `tsf_tip_display_attribute_tests` | `tsf-tip/tests/display_attribute_test.cpp` | `ITfDisplayAttributeProvider`（`GetDisplayAttributeInfo`/`EnumDisplayAttributeInfo`）と `InputDisplayAttributeInfo`（GUID/説明/下線属性、`Next`/`Reset`/`Skip`/`Clone`、null 引数 reject） |
| `tsf_tip_activate_uiless_tests` | `tsf-tip/tests/activate_uiless_test.cpp` | `ActivateEx` が `ITfThreadMgrEx::GetActiveFlags`（`dwFlags` ではなく）から UI-less 状態を導出すること、キーボード開閉 compartment の初期読込・通知・解除と IME Off 時の入力状態クリア |
| `tsf_tip_staleness_tests` | `tsf-tip/tests/staleness_test.cpp` | 連続応答のうち最新のみ受理、より新しいリクエストが queue 済みの応答の破棄、commit で無効化された応答の破棄 |
| `tsf_tip_ipc_reconnect_backoff_tests` | `tsf-tip/tests/ipc_reconnect_backoff_test.cpp` | IPC ワーカーの再接続待機（spec §8.3）。注入した乱数源に渡る範囲が `[250ms, 前回の 3 倍]` で上限 3s に切られること、範囲外の値が下限・上限へ丸められること、`Reset` 後は下限の窓から数え直すこと、seed 固定の乱数で待機が範囲内に分散すること、既定の乱数源を持つ 2 インスタンスが同じ系列にならないこと |
| `tsf_tip_ipc_connection_state_tests` | `tsf-tip/tests/ipc_connection_state_test.cpp` | IPC ワーカーの接続状態機械（spec §8.2）。表に載る全遷移の遷移先、載らない (状態, イベント) の拒否、自己遷移が無いこと、`Disconnected` から全状態へ到達できること、ログ用の状態名・イベント名が固定語彙で一意なこと、接続試行中（`Disconnected` / `Connecting` / `Handshaking` の間）の遷移ログが 1 回目と 2 のべき乗回目の試行だけに間引かれ、同じ試行の遷移が同じ判定になること |
| `tsf_tip_caret_position_tests` | `tsf-tip/tests/caret_position_test.cpp` | text extent 優先のキャレット位置決定と物理座標正規化、変換失敗時の座標保持、GUI スレッドキャレットと物理カーソルへの段階的 fallback、フォーカス喪失・コンテキスト push でのキャッシュ破棄 |
| `tsf_tip_candidate_ui_coordinator_tests` | `tsf-tip/tests/candidate_ui_coordinator_test.cpp` | 候補 UI の app-drawn / TIP 描画切替、`BeginUIElement` 失敗の HRESULT 報告、UI-less 時の `ITfUIElementMgr` 要求と欠落時 fallback、選択移動の wrap |
| `tsf_tip_candidate_window_dpi_tests` | `tsf-tip/tests/candidate_window_dpi_test.cpp` | 候補ウィンドウのレイアウト metrics の DPI スケール、DPI 0 の既定 fallback、絵文字判定が漢字・文字記号を巻き込まないこと、description 有無での列構成 |
| `tsf_tip_prediction_window_tests` | `tsf-tip/tests/prediction_window_test.cpp` | 予測ウィンドウのキャレット右側配置、画面端での左右・上下反転、最大 5 件、モニタ矩形に収まる座標計算 |
| `tsf_tip_query_interface_contract_tests` | `tsf-tip/tests/query_interface_contract_test.cpp` | `QueryInterface` の null out-param と未対応 IID の契約、`ITfFnConfigure`／`ITfFunction` 公開、`Show` からのプロファイル付き設定アプリ起動と失敗時 HRESULT |
| `tsf_tip_keymap_tests` | `tsf-tip/tests/keymap_test.cpp` | VK → `UserAction` 写像（第 1 層）の全エントリを状態ごとに検証、変換・無変換・半角/全角・英数キー、数字キーの `digit`、Alt / Win 組合せと表に無いキーのパススルー、core の VK 定数と `VK_*` の一致 |
| `tsf_tip_local_settings_tests` | `tsf-tip/tests/local_settings_test.cpp` | Host 非依存で共有設定ファイルを読む TIP ローカル設定、ローマ字テーブル変更の監視と再読み込み、Unicode パス・再作成ディレクトリへの再バインド、不正・過大ファイルでの既定復帰、privacy の secure ↔ normal 再読込と不正設定での拒否、監視の再 arm に失敗しても以後の保存を取りこぼさないこと、内容が変わった保存だけを観測者へ通知すること、`liveConversion` の既定 OFF・`predictionEnabled` の既定 ON と再読込 |
| `azookey_settings_launch_arguments_tests` | `settings-app/tests/launch_arguments_test.cpp` | 設定アプリ起動引数の round-trip、値欠落・不正 LangId / プロファイルの reject、未指定と空指定の区別、重複・未知オプションの reject |
| `azookey_settings_persistence_tests` | `settings-app/tests/settings_document_test.cpp` | 設定ドキュメントの既定値と隠しキー保持、不正エントリの除去、ロック・読み取り失敗時の既存ファイル不変、不正文書の隔離と原子的保存による復旧、privacy 学習・ログ軸の保持と不正値の secure 制限、API キーの DPAPI 保護・旧平文移行・復号不能値の保全、`typoCorrectionMode`/`typoMinCount`/`autoWordRegistration.*` の保存時保持と不正値の削除 、M59 の 6 設定キーの保存保持 |
| `azookey_settings_persistence_tests` | `settings-app/tests/settings_ipc_client_test.cpp` | 設定アプリから Host への Handshake と `UpdateConfig` 送信 |
| `compat_test_unit_tests` | `compat-test/tests/compat_test_unit_tests.cpp` | 互換ハーネスの target 定義検証（自動化契約・既知回避策・一時文書の所有）、レポート schema と非信頼テキストの伏せ字化、クリップボード復元、ウィンドウ所有権判定 |
| `temporary_learning_file_tests` | `bench/temporary_learning_file_test.cpp` | bench 用一時学習ファイルの並行予約時の独立性、他所有者への非干渉、巻き戻し時の後始末と想定外ファイルの保全 |
| `benchmark_result_tests` | `bench/benchmark_result_test.cpp` | bench JSON schema の固定、baseline 比較の閾値と絶対ノイズ床、baseline 欠落・非互換時の非回帰扱い、UTF-8 出力パス |
| `conversion_quality_tests` | `bench/conversion_quality_test.cpp` | 変換品質の符号位置単位 CER、canonical / acceptable 一致の区別、raw と NFKC の独立集計、不正 UTF-8 の reject、データセットハッシュの改行正規化 |
| `azookey_bench_smoke` | `azookey_bench` | CPU `SimpleConverter` 経路の p50/p95/p99 出力、p95 < 50ms |
| `azookey_rich_features_bench_smoke` | `azookey_rich_features_bench` | M14 軽量ライブ変換の Host 推論 p95 が 30ms 以下で、JSON 出力が schema に一致すること |
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
| `.github/workflows/windows.yml` の `windows-coverage` | GitHub Actions（build 対象の PR / `main` push / 手動 dispatch） | OpenCppCoverage 0.9.9.0 で Windows Debug の CTest 子プロセスを計測し、HTML と Cobertura を `windows-opencppcoverage` artifact に 14 日保持。クラッシュ注入と大量ログ境界の計測非互換テストは除外し、通常の Windows Debug ジョブで実行。Linux LLVM coverage と別系列の informational ジョブで、数値閾値による合否判定はしない（`docs/dev-infrastructure-spec.md` §4.3 / §10） |
| `.github/workflows/docs.yml` の `docs-lint` | GitHub Actions（PR / `main` push） | `scripts/docs-lint.py` の文書ドリフト検査（DECISIVE はベースライン 0 件で凍結）、`scripts/check_agent_instruction_size.py` の `AGENTS.md` バイト予算、`scripts/check_test_inventory.py` による「現存テスト一覧」と `CMakeLists.txt` 登録の突合、`scripts/check_skill_references.py` による repo 固有 Skill（`.claude/skills/MANIFEST.md` の区分 `repo`）の routing 表とパス・CTest target の突合、`scripts/check_agent_definitions.py` による repo 固有 agent（`.claude/agents/MANIFEST.md` の区分 `repo`）の Claude / Codex 本文・権限の突合と `scripts/tests/test_agent_readonly_guard.py` の read-only guard 表 |
| `scripts/tests/msix-identity-consistency.Tests.ps1` | Pester（CI） | MSIX identity manifest と `kTextServiceClsid` / `kTextServiceProfileGuid` / `kJapaneseLangId` の静的整合、Option A の不変条件、ビルド埋め込み配線 |
| `scripts/tests/test_benchmark_commit_freshness.py` | Python unittest（手動。CMake / Ninja / Git が必要） | commit ヘッダーと consumer の依存追跡。ソース・ヘッダー・HEAD・override・branch・worktree・packed ref の変更後の再ビルドと、ビルド直後の dry-run を検証 |
| `scripts/doctor.ps1`（`just doctor`） | 開発者・エージェントの手元 | 不足ツール・未初期化 dev shell・未取得依存の診断（`docs/dev-infrastructure-spec.md` §2.5。§12 の `azookey_diag.exe` とは別物） |
