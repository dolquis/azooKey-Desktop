# azooKey-Desktop / karukan 比較調査レポート

> **種別**: 調査・比較・移植候補整理（特定時点のスナップショット）
> **作成日**: 2026-06-30
> **対象**: 自リポジトリ `azooKey-Desktop`（Windows TSF / C++）と参考実装 `togatoga/karukan`（Rust / Linux + macOS）
> **位置づけ**: 本ファイルは時点依存の**調査レポート**であり、状態の正典ではない。
> - マイルストーン定義・受け入れ条件の正典 → `plans/windows-port-roadmap.md`（候補リライターは **M62**、karukan 参照対応は末尾「横断テーマ」節）
> - 進捗・状態・優先度の正典 → Linear（team `Dev`）
> - 機能仕様の正典 → `docs/*-spec.md`
> - karukan は**読み取り専用の参考実装**。コードは逐語移植せず設計思想のみ参照する。
> - 取り込みの追跡（Linear, team Dev）→ DEV-397（Tracking）/ DEV-398（M62-A 数値リライター）/ DEV-399（THIRD_PARTY_LICENSES）/ DEV-400（M13 設計レビュー）
>
> 調査方法: 両リポジトリを読み取り専用で 10 観点に分割し、各観点エージェントが実ファイルを
> 読んで file:line 付き所見を作成。高インパクト主張 6 件を別エージェントが再検証（confirmed/corrected）。
> `legacy/` は参照専用として除外。本調査ではコードを変更していない。

---

## 1. Executive Summary

1. **karukan は「azooKey が計画済みだが未実装」の領域の、よくテストされた動作参照実装である。**
   最重要は **M13（TSF 非依存 IM 状態機械: UserAction / InputState / ClientAction）**。決定的事実
   として、azooKey の `docs/legacy-parity-spec.md` §1.1 は **既に karukan の三段分離を M13 の設計
   根拠として明示的に引用している**。よって karukan の使い所はコード移植ではなく **M13 着手時の
   設計レビュー資産**（最大価値・最小コスト＝**P0/MVP前**）。

2. **唯一の「真のギャップ」（コードにもロードマップにも無い）は候補リライター**（数字異表記 /
   記号関連候補 / 絵文字 / 半角カタカナ / 英字幅ケース）。中でも **数字リライターは Mozc データ
   非依存の純アルゴリズム・無 IPC・TIP ローカル**で最も低リスク高価値 → **最初に作るべき具体機能
   （P1/MVP後）**。ロードマップに **M62** として定義済み。

3. **モデルのライフサイクル管理**（宣言的レジストリ + HF 自動 DL + プリフェッチ + pre-tokenizer
   上書き）は **M32/M45 の未実装部分に 1:1 対応**。v1.0 配布（MSIX 非同梱・初回 DL）の前提。
   ただし **zenz GGUF 配布ライセンス（Linear DEV-202）が未解決ブロッカー**。

4. **取り込まない方がよいもの**: karukan のプロセス結合トポロジ（macOS 子プロセス spawn /
   fcitx5 in-proc FFI）、適応 2 モデル常駐切替、HTTP サーバ+Web UI、HF 自動 DL のキャッシュ配置、
   `tracing` 採用、学習 score 式・非原子書込。いずれも azooKey の **TSF in-proc + クラッシュ隔離 /
   単一 Zenzai / 依存最小 / 既存の堅牢な学習**に劣るか衝突する。

5. **azooKey が既に優位な領域（取り込み不要）**: 学習の堅牢性（原子書込 `AtomicFile.h`・TSV
   エスケープ・負例学習 `ObserveCorrection`・時間減衰）、IPC の認証+version（`Envelope.version` /
   `Handshake`）、キャンセル/デッドラインの推論ループ貫通（`llama_set_abort_callback`）、
   degraded/error 三値 Health、設定の fail-soft（破損 quarantine）。

6. **重要な事実訂正（spec がコードより古い箇所あり）**: (a) `inference-host/src/ZenzaiModelConverter.cpp`
   は**既に llama.cpp beam search 本体を実装済み**（`docs/zenzai-inference-spec.md:28` の「fallback
   のみ」記述は古い）。(b) karukan の `number.rs` は Mozc 由来「データ」ではなく**自前テーブルの
   純コード**（説明文のみ Mozc 準拠）→ ライセンス的に最も安全。記号/絵文字のみ Mozc 由来データ
   （BSD-3）+ CLDR。

7. **azooKey に `THIRD_PARTY_LICENSES` 集約ファイルが不在**（glob 0 件）。記号/絵文字の Mozc 由来
   データや SudachiDict 辞書（M53）を採用する前に整備が必須。実装リスクゼロの先行整備。

**MVP（ロードマップ Phase 1-4）を壊す提案は無い。** 取り込み候補はすべて「M13 設計レビュー」か
「MVP 後の追加機能」に収まる。

---

## 2. 比較対象の概要

### azooKey-Desktop（5 層 + プロセス分割）

| 層 | 責務 | 主要シンボル |
|---|---|---|
| `tsf-tip/`（in-proc COM DLL） | キー処理・Composition・候補 UI・EditSession | `TextService.h:79-173` |
| `inference-host/`（別プロセス EXE） | モデル常駐・候補生成・学習再ランク・CPU/CUDA | `InferenceEngine.h:54-133`, `Dispatcher.h` |
| `core/`（OS非依存） | 変換コア（**状態機械ではない**） | `IConverter.h:29-42`, `RomajiKanaConverter`, `SimpleConverter` |
| `ipc/` | length-prefix JSON over Named Pipe + 認証 | `Messages.h:10-34`, `Payloads.h` |
| `learning/` | 頻度+時間減衰の再ランク・ユーザー辞書 | `LearningStore`, `Reranker`, `UserDictionary.h:11-22` |

**強み**: プロセス分離によるクラッシュ隔離 / IPC 認証+version / キャンセル・デッドラインの推論
ループ貫通 / 学習の原子書込・負例学習 / 設定 fail-soft / 仕様（`docs/*-spec.md` 34 本）と
ロードマップ（2212 行）が極めて広範で先回り設計済み。

**弱み**: IME 状態が TIP（OS 依存層）に集中し純粋状態機械層が未実装（M13 待ち）。候補リライター層
が皆無。モデル取得機構が無い（手動配置前提）。設定に推論性能の数値ノブ（スレッド数/候補数/
コンテキスト長）が欠落。`core` のユーザー辞書は `std::map`（trie 無し）。**Zenzai 一般変換は未配線**
（辞書/学習/mock のみ）。

### karukan（4 層）

| クレート | 責務 | azooKey 対応 |
|---|---|---|
| `karukan-engine/` | ローマ字→かな + llama.cpp 推論 + 辞書(trie) + 学習 + リライター | `core` 変換部 + `learning` + `inference-host` 推論部 |
| `karukan-im/` | **OS非依存ステートマシン**（`core/engine/*`）+ JSON-RPC server | azooKey が**新設予定**の `core/InputState.cpp`（M13） |
| `karukan-cli/` | 辞書ビルド・Sudachi 生成・HTTP サーバ・bench | `bench/` + `UserDictCli` + `scripts/` |
| `karukan-fcitx5/`, `karukan-macos/` | 薄いフロントエンド（C FFI / Swift） | `tsf-tip`（薄化後の姿） |

**強み**: 状態機械が単一の OS 非依存クレートに集約され `EngineAction`（6 種抽象アクション）を
返す純粋関数 / フロントエンドが本当に薄い / 候補リライター完備 / モデル自動 DL・レジストリ /
ライブ変換の増分チャンク / per-crate CI 分割。

**Windows 版にそのまま持ち込めない点**: Rust 実装（`yada` trie / `hf_hub` / `tokenizers` /
`LazyLock`）/ XKB keysym ベースのキー抽象 / 同期変換前提の状態表（azooKey は IPC 非同期）/
preedit 自前バッファ保持（TSF は `ITfComposition`/`ITfRange` 経由）/ プロセス結合トポロジ
（macOS 子プロセス・fcitx5 in-proc FFI）/ CPU 固定（`n_gpu_layers=0`）/ 外部 `tokenizer.json` 必須
（azooKey は GGUF 内蔵を正典）。

---

## 3. 機能比較マトリクス

| 領域 | azooKey-Desktop | karukan | karukanの優位点 | 取り込む価値 | 優先度 | 備考 |
|---|---|---|---|---|---|---|
| アーキテクチャ | 5層+プロセス分割。状態はTIPに集中 | 4層。状態はim層に集約 | 状態機械をOS非依存層に閉じる設計の動作実例 | 設計参照=high | P0 | legacy-parity-spec §1.1が既にkarukanを引用 |
| IME状態管理 | M13計画のみ。`TextService`の暗黙フラグ | `InputState`enum + `EngineAction`返り値 | 状態=値/遷移=純粋関数/出力=抽象アクション | 設計参照=high | P0/P1 | コード移植不可。VK表・IPC非同期は独自 |
| ローマ字かな変換 | `RomajiKanaConverter`（純粋・分離済） | `romaji/`(trie+rules) | 同等。trie実装は別アプローチ | low | — | azooKeyは既に分離済 |
| かな漢字変換 | `ZenzaiModelConverter`にbeam search実装済 | `kanji/`(greedy→beam) | 同等以上はazooKey側 | low（生成ループ） | 保留 | abort callbackはazooKeyが優位 |
| モデル管理 | 手動配置。DL機構なし | TOML registry+HF自動DL+prefetch | DL/レジストリ/プリフェッチ/pre-tok上書き | high | P1 | M32/M45に1:1対応。zenz license DEV-202がブロッカー |
| IPC/サーバー境界 | 認証+version付きNamed Pipe | 認証なしJSON-RPC | なし（azooKeyが上位） | low | — | 取り込み不要 |
| 学習 | 原子書込/負例/時間減衰/エスケープ | reading-keyed二層+prefix線形走査 | reading-keyed二層構造(前方一致可) | medium | P3 | score式・永続化はazooKeyが優位 |
| ユーザー辞書 | `std::map`+quarantine+atomic | trie+ディレクトリ自動読込 | trie前方一致・複数ファイルmerge | medium | P3 | azooKeyは堅牢性が優位 |
| システム辞書 | mock-dictのみ(Zenzai未配線) | yada trie+Sudachi/Mozc/JSONビルド | double-array trie+マルチソース | medium | P3 | M53と一緒に。Sudachi/UniDic/NEologdライセンス |
| **候補リライター** | **皆無（grep 0件）** | 数字/記号/絵文字/半角カナ/英字 | **真のギャップ。完全な異表記展開** | **high** | **P1** | M62。数字=純コード/記号絵文字=Mozc data(BSD-3) |
| 絵文字入力 | 皆無（タグ枠のみ） | かな読み+`:trigger`曖昧検索 | **真のギャップ** | high(価値)/P3(コスト) | P2/P3 | M62-D。emoji.yml=Mozc+CLDR、47k行 |
| ライブ変換 | M14計画のみ。staleness機構は実装済 | チャンク増分再変換+is_japanese | prefix/suffix増分・非日本語verbatim | high(設計) | P1/P2 | M14に効く。M58とは別レイヤ |
| 設定 | 機能フラグ広範。数値ノブ欠落 | TOML+merge+性能ノブ完備 | スレッド数/候補数/コンテキスト長/chunk長 | high | P2 | 形式(JSON)・パス層は変えず項目追加 |
| テスト | bench mock分離・OS層分離が成熟 | per-crate分割+#[ignore]モデル分離 | モデル依存テストの明示ゲート | medium | P2 | azooKeyのmock機構が既に同等 |
| CI | 単一windows.yml全ビルド | 5 workflow+paths+rust-cache | per-crate paths+ビルドキャッシュ | medium | P3 | sccache CI配線が唯一の純新規 |
| CLI | UserDictCli/bench/register.ps1 | dict viewer/server/sudachi/bench | 辞書ビューアCLI+評価CLI | medium | P2 | HTTP/Web UIは取り込まない |
| ドキュメント | spec 34本+roadmap 2212行が正典 | README+per-crate README | なし（azooKeyが圧倒的に詳細） | low | — | — |
| パッケージング | MSIX/署名/WinGet計画(M28-32) | cargo + Docker(fcitx5) | なし（OS固有） | low | — | 取り込み不可 |

---

## 4. 取り込み候補 Top 10

### 候補 1: M13「TSF非依存 IM 状態機械」の設計参照として karukan-im を併置
- **概要**: コード移植ではなく、M13（UserAction/InputState/ClientAction）着手時の設計レビュー資産として karukan の `InputState`/`EngineAction`/`process_key` を「達成済みの到達点」として並置する。
- **karukan 側**: `karukan-im/src/core/state.rs:10-30`（`InputState` enum＝状態が値）、`engine/types.rs:11-25`（`EngineAction` 6 種＝抽象出力）、`engine/mod.rs:482-486`（状態でディスパッチ）、`KarukanInputController.swift:4-9`（薄いフロントエンドの到達点）。
- **azooKey 側**: 現状 `tsf-tip/src/TextService.cpp:307-640`（VK 別 if/else・`preedit_kana_`/`IsShowing()`/`committing_` の暗黙組合せ）。計画は M13、新設予定 `core/include/azookey/core/InputState.h` 等。
- **メリット**: M13 の enum 設計・純粋性契約・受け入れ条件（全状態×全 UserAction 網羅テスト）の質を直接底上げ。新規実装コストほぼゼロ。
- **リスク**: 低。Rust の代数的データ型を C++ に機械翻訳しないこと（spec は既に C++ 流 `HandleResult{actions,next}` で設計済み）。
- **ライセンス**: 設計思想参照のみ。義務なし。
- **触るファイル**: なし（M13 設計工程内）。突き合わせ対象は `docs/legacy-parity-spec.md §1`。
- **最小実装案**: M13 着手前レビューで ① karukan の `EngineAction` 6 種と azooKey の `ClientAction` 表を突き合わせ候補ページング/aux テキストの欠けを検出、② `tests/{basic,cursor,mode_toggle}.rs` をテスト様式の雛形化。
- **テスト方針**: `core/tests/input_state_test.cpp` で `(状態, UserAction, ヒント)→(ClientAction列, 次状態)` を純粋関数テスト（TSF/Zenzai/IPC 非起動）。
- **優先度**: **P0** / **MVP前**（M13 と同時）

### 候補 2: 数字リライター（漢数字/大字/ローマ数字/丸数字/2・8・16 進数）— M62-A
- **概要**: 純十進入力に対し異表記候補を生成する純アルゴリズムを `core/` に新設。**真のギャップ**で最も低リスク。
- **karukan 側**: `karukan-engine/src/rewriter/number.rs:174-227`（自前テーブル `KANJI_DIGITS` 等 `:32-51`、`descriptions_match_mozc` `:335` は説明文のみ Mozc 準拠）。
- **azooKey 側**: 該当機能なし（`core/tsf-tip/inference-host` を grep して 0 件）。`docs/rich-features-spec.md` も未定義。
- **メリット**: 辞書・データ・IPC・Mozc データ依存ゼロの決定的処理。実利用頻度が高く UX 価値最大。M61 が確立した「TIP ローカル・無 IPC・Host 非依存」モデルに完全に乗る。
- **リスク**: 低。OS 非依存 `core/` の純追加で TSF/IPC/UI スレッドに無干渉。
- **ライセンス**: **最も安全**。Mozc 由来「データ」を使わず、アルゴリズムは一般的で創作性が低い。
- **触るファイル**: `core/include/azookey/core/NumberRewriter.h`（新規）, `core/src/NumberRewriter.cpp`（新規）, `core/tests/number_rewriter_test.cpp`（新規）, `core/CMakeLists.txt`。
- **最小実装案**: `ExpandNumberCandidates(reading)->vector<Candidate{surface,description}>`。inference-host への配線は別 PR。
- **テスト方針**: `number.rs:280-368` を**期待値表として**移植（逐語コピーでなく）。`123→百二十三/壱百弐拾参/0x7b/0b1111011`（丸数字/ローマは範囲内のみ・例 `12→⑫`）、`0` 特例、上限超スキップ、混在（`20世紀`）除外。AAA 構造。
- **優先度**: **P1** / **MVP後**（M62-A）

### 候補 3: モデルのライフサイクル管理（DL + SHA256 + カタログ + プリフェッチ + pre-tokenizer 上書き）
- **概要**: 推論ループは azooKey 既存を温存し、karukan の「モデル取得・宣言・トークナイザ堅牢化」設計のみ移植。
- **karukan 側**: `karukan-engine/models.toml:1-19`、`kanji/hf_download.rs:23-44`/`:77-89`、`kanji/model_config.rs:11-88`、`kanji/llamacpp.rs:100-136`（pre-tokenizer KV override）。
- **azooKey 側**: `SettingsStore.h:18-27`・`InferenceEngine.cpp:165-229` はあるが **DL 機構なし**（`docs/model-management-spec.md:27`「DL は M45 範囲外」）。
- **メリット**: M32（`.part`→SHA256→rename）/ M45 の未実装部分に 1:1 対応。`copilot-pc-backend-spec.md:258`「MSIX 非同梱・初回 DL」は v1.0 配布の前提。
- **リスク**: 中。**ブロッカー: zenz GGUF 配布ライセンス（Linear DEV-202）**。慎重領域（永続化・外部連携・schema）で Linear 起票 + spec 更新を伴う。
- **ライセンス**: karukan の `togatogah/jinen-v1-*`（GPT-2 系）は azooKey の `zenz-v3` とは別モデル → 機構のみ参照。`hf_hub` は使わず WinHTTP + `%LOCALAPPDATA%\azooKey\models\zenzai\`。
- **触るファイル**: `inference-host/`（カタログ JSON + DL/検証関数 + `--prefetch-models` CLI）, `SettingsStore.*`, `docs/model-management-spec.md`/`docs/ai-backend-spec.md`。
- **最小実装案**: ① カタログ宣言（JSON）② GGUF DL + SHA256 検証 ③ プリフェッチ（`autoLoadOnHostStart` と組合せ）④ pre-tokenizer KV override（低コスト高価値）。
- **テスト方針**: SHA256 ミスマッチで `.part` を rename しない / レジューム / ネット不通で既存継続（ローカル HTTP モック + 小フィクスチャ GGUF）。実モデル要は `gate:human-required` で分離。
- **優先度**: **P1** / **MVP後**（M8 配線後、ライセンス確定が先行条件）

### 候補 4: 非日本語チャンクの verbatim 通過（`is_japanese` グルーピング）
- **概要**: 数字・記号・英字・句読点の連続をモデルに送らず原文のまま通す。ライブ変換の正確性とレイテンシを同時に底上げ。
- **karukan 側**: `karukan-im/src/core/engine/chunk.rs:47-85`（`is_japanese`/`group_chunks`）, `:254-267`, テスト `tests/chunks.rs:56-67`。
- **azooKey 側**: 該当なし。M14/M58 のチャンカは未実装。
- **メリット**: digit/記号の mangling 回避 + レイテンシ削減 + IPC 往復削減。純粋関数で `core/` テスト可。
- **リスク**: 低。
- **ライセンス**: 問題なし。
- **触るファイル**: M14/M58 着手時の `core/` チャンカ純粋関数 + テスト。
- **テスト方針**: `tests/chunks.rs` 同型の純関数テスト（モデル非起動）。
- **優先度**: **P1** / **MVP後**

### 候補 5: 記号リライター（variant chain + かな読み引き）— M62-C
- **概要**: `「`→`『【〔（` の関連括弧チェーンと `かぎかっこ`→`「」` のかな読み引き。日本語入力の基本機能で**真のギャップ**。
- **karukan 側**: `karukan-engine/src/rewriter/symbol.rs:181-218`、データは `karukan-engine/data/symbols.yml`（Mozc `symbol.tsv` 由来）。
- **azooKey 側**: 該当なし。M61 ブラケットペアリングは「打鍵時の自動ペア挿入」で別物（補完関係）。
- **メリット**: 日本語入力の基本機能。`Rewriter` trait + chain の疎結合設計が azooKey の段階導入方針と親和。
- **リスク**: 中。**`symbols.yml` は Mozc 由来データ（BSD-3）** → 仕様参考 + データ再ポート + 表記整備が前提。
- **ライセンス**: ロジックは独自実装可。データを使うなら Mozc BSD-3 保持必須 → `THIRD_PARTY_LICENSES`（候補 7）整備が前提。
- **触るファイル**: `core/`（`IRewriter` + symbol rewriter）, データファイル（再ポート）, `docs/candidate-rewriter-spec.md`（新規）, `THIRD_PARTY_LICENSES`。読み引きは Host 側推奨。
- **テスト方針**: round-trip（`「→『【〔（`、`かぎかっこ→「」`）+ データ欠損フォールバック。
- **優先度**: **P1（機能価値）/ P2（データ整備）** / **MVP後 → v1.0前**（M62-C）

### 候補 6: 推論チューニング数値ノブを settings schema に追加
- **概要**: `inferenceThreads` / `maxCandidates` / `maxContextLength`（将来 `liveConversionChunkLen`）を追加。形式（JSON）・パーサ・パス層は変えず項目追加のみ。
- **karukan 側**: `karukan-im/src/config/settings.rs:45-73`（`num_candidates`=9, `max_context_length`=10, `n_threads`=4, `composing_chunk_len`=30, `max_latency_ms`=100）。
- **azooKey 側**: `settings/mvp-settings.schema.json` は機能フラグ・model.* は広範だが**性能ノブが構造的に欠落**。`max_candidates` は per-call IPC 引数のみ（`InferenceEngine.h:76`）。
- **メリット**: `ReadInt32` 既存パターンに乗る低リスク追加。
- **リスク**: 低。**スレッド既定は `powerProfile` 連動が必要**（全コア既定は in-proc TIP のもたつき要因）。
- **ライセンス**: 設定項目の設計思想参照のみ。TOML 形式・`ProjectDirs` パス層・jinen モデル名は採らない。
- **触るファイル**: `settings/mvp-settings.schema.json`, `inference-host/src/SettingsStore.cpp`, `InferenceEngine.h`（EngineConfig）。
- **テスト方針**: 欠落→既定/型違い→既定/負値→クランプ、`additionalProperties:false` の未知キー拒否、integration で反映。
- **優先度**: **P2** / **MVP後**

### 候補 7: `THIRD_PARTY_LICENSES` / NOTICE 集約ファイルの新設
- **概要**: 将来の Mozc 由来データ（記号/絵文字）や SudachiDict 辞書取り込みに備え、attribution 集約ファイルとデータヘッダ規約を整備。
- **karukan 側**: `THIRD_PARTY_LICENSES`（Mozc BSD-3 全文 + derived ファイル対応）, `karukan-cli/docs/LEGAL`（Apache-2.0 + UniDic + NEologd + はてな/郵便/駅名/人名連鎖）, 各 yml ヘッダ。
- **azooKey 側**: **集約ファイル不在**（glob 0 件）。本体は MIT。
- **メリット**: 候補 5/絵文字/M53 辞書強化の前提整備。三層 attribution（ファイルヘッダ + 集約 + 再生成注記）の完成形テンプレート。
- **リスク**: 極低（文書のみ）。
- **触るファイル**: `THIRD_PARTY_LICENSES`（新規・空雛形+規約）, `docs/`（規約説明）。README は肥大化させずリンクのみ。
- **優先度**: **P2** / **Phase A（今すぐ可）**

### 候補 8: 辞書/学習を引く読み取り専用 lookup CLI（Host 非経由）
- **概要**: 完全一致/前方一致/表層形で辞書を引く CLI。karukan-dict view の CLI モードのみ思想移植（Web/HTTP は採らない）。
- **karukan 側**: `karukan-cli/src/bin/dict.rs:248-294`（CLI 検索モード）。
- **azooKey 側**: 辞書ビューア相当なし（ユーザー辞書は list/export のみ）。
- **メリット**: 開発・サポート双方に効く。`UserDictCli` と同じ薄 lib + JSON/TSV 二形式で依存ゼロ・オフライン・OS 非依存。
- **リスク**: 低。
- **ライセンス**: CLI 思想移植のみ。HTTP/HuggingFace 自動 DL/tracing は依存最小・供給源 pin 方針と衝突するため採らない。
- **触るファイル**: `inference-host/`（lookup サブコマンド、UserDictCli と同流儀）。
- **テスト方針**: 完全一致/前方一致/表層形/該当なし/空辞書を AAA 固定（Host 非経由で IPC モック不要）。
- **優先度**: **P2** / **MVP後**（M9/M53 内包の可能性あり、要確認）

### 候補 9: ChunkPlan 相当の増分再チャンク純粋関数（ライブ変換土台）
- **概要**: prefix/suffix 差分で「変わった中間スパンだけ再変換」する純粋関数を `core/` に移植。M14 の per-keystroke 全文再変換を回避。
- **karukan 側**: `karukan-im/src/core/engine/chunk.rs:107-158`（`ChunkPlan::compute`）, テスト `tests/chunks.rs:285-307`。
- **azooKey 側**: staleness/cancel は実装済（`RequestScheduler.cpp` / `Dispatcher.cpp:236-263`）だが増分再チャンクは未実装。
- **メリット**: engine/model 非依存の純粋関数で単体テスト可能。M14 のレイテンシ制御土台。
- **リスク**: 中。**M58（文境界事前分割 + 1req1resp、`docs/romaji-batch-conversion-spec.md §6.3.5`）とは粒度が別**。per-keystroke 増分は M14 限定とし M58 と混同しないこと。
- **ライセンス**: ロジック移植（逐語コピー時のみ MIT/Apache 表記）。
- **触るファイル**: M14 着手時の `core/` 純粋関数 + テスト。
- **テスト方針**: fresh/append/middle-insert/delete の各ケース（モデル不要）。
- **優先度**: **P2** / **MVP後**

### 候補 10: 変換品質評価 CLI（Exact Match 率 + CER, NFKC 二重評価）
- **概要**: M52（変換品質ベンチ）の出力フォーマット雛形として karukan の `ajimee_bench` を参照。
- **karukan 側**: `karukan-cli/src/bin/ajimee_bench.rs:75-84`（Metrics）, `:219-231`（素/NFKC 二重評価）, `:99-135`（Wagner-Fischer CER）。
- **azooKey 側**: M52 で計画済み（`docs/conversion-quality-benchmark-spec.md`）だが現物なし。`bench/` は p50/95/99 のみ。
- **メリット**: 指標算出ロジック（CER/Exact/NFKC）が出力雛形になる。
- **リスク**: 中。**評価データは `conversion-quality-benchmark-spec §13`（CC0/PD/authored 限定、NEologd/UniDic/JMdict EXCLUDE）に従い自前作成必須**。karukan の AJIMEE/Sudachi 由来データは複製不可。
- **ライセンス**: 指標ロジックは移植可、評価データは複製不可。
- **触るファイル**: `bench/`（評価 CLI）, `docs/conversion-quality-benchmark-spec.md`。
- **テスト方針**: 既知ペアで CER 単体、Exact/NFKC 境界（全半角差）固定、小コーパス CTest smoke。
- **優先度**: **P2** / **v1.0後**（M52 起点）

---

## 5. 最優先で検討すべき設計改善（深掘り）

1. **TSF非依存 IM state machine 層（M13）を作るべきか → はい。ただし karukan は「設計参照」、一次参照は legacy + spec。** `legacy-parity-spec.md §1.1` が既に karukan を引用済み。**azooKey 独自に再設計が必要な 3 点**: ① VK→UserAction 表（karukan は XKB keysym）② IPC 非同期の cache hit/miss 分岐（`spec §1.5.4`、karukan は同期前提で対応概念なし）③ `ClientAction`→`ITfComposition`/`ITfRange` 翻訳層（karukan は preedit を自前 String 保持）。プロセス結合トポロジは**取り込まない**。

2. **学習 + 前方一致予測の強化 → データ構造の形のみ medium 価値。** azooKey の `LearningStore` は `reading\tsurface` フラットキーで**前方一致が構造的に不可能**。karukan の reading-keyed 二層 + `prefix_lookup` は M15 予測供給源を補完しうる。**ただし karukan の全 reading 線形走査（`learning.rs:84`）はそのまま不可** → trie か `std::map::lower_bound` が前提。score 式は azooKey の `exp(-0.15*days)` 減衰 + 負例学習を維持。

3. **候補リライターの段階導入 → 数字から（M62-A → B → C → D）。** `core` に `IRewriter` + `RewriterChain` を新設し `Candidate.h` に description 注釈フィールドを追加。**TIP ローカルな決定的リライト（数字/英字/カナ）と Host 側のデータ駆動リライト（記号/絵文字）を分割**するのが TSF 的に妥当。

4. **システム辞書 / ユーザー辞書の設計 → P3/保留。** azooKey のユーザー辞書（`std::map` + quarantine + atomic + `cid`/`mid` 品詞 ID）は堅牢性で優位。trie 化は M53 と一緒に。SudachiDict を採るなら Sudachi(Apache-2.0)/UniDic(BSD系)/NEologd(Apache-2.0+はてな条件) を個別精査。

5. **ライブ変換を MVP に入れるべきか → いいえ（MVP後 P2）。** karukan の prefix/suffix 増分再チャンクは優秀だが、TSF では ① `ITfComposition` 毎キー書き換えの更新頻度がちらつき/再描画コスト ② ライブ表示と Space 明示変換の候補一貫性 ③ チャンク境界と確定/再変換境界（M20）の整合、という制約が乗る。**MVP 前は settings 定義のみ先に固め（既に `liveConversion`/`batchRomajiConversion` は schema 済み）、実処理は M14 で。**

6. **モデル管理・設定スキーマの整理 → 候補 3 + 候補 6。** **適応 2 モデル切替（`max_latency_ms`→light）は取り込まない**（main 90M + light 26M 2 モデル常駐前提が単一 Zenzai 構成と衝突）→ v1.0 後に M47 DegradedModel と整合させ「レイテンシ閾値で degraded/CPU/Simple へ落とす」方向に再解釈。

7. **CI / テスト分割の改善 → 純新規は sccache CI 配線のみ。** azooKey の bench mock 分離・OS 層分離（`if(WIN32)`）・PR 診断コメントは既に成熟。lint/format ゲートは M37/M38 で計画済み（追認）。**per-module paths 分割は密結合 CMake ツリーでは効果限定的** → まず docs-only スキップのみ。

---

## 6. 取り込まない方がよいもの

| 分類 | 項目 | 理由 |
|---|---|---|
| TSF相性 | preedit 自前 String 保持 + 毎キー UpdatePreedit | TSF は `ITfComposition`/`ITfRange::SetText` でアプリ実テキスト編集。毎キー高頻度更新は再描画/ちらつき要因 |
| TSF相性 | プロセス結合トポロジ（macOS 子プロセス spawn / fcitx5 in-proc FFI） | azooKey の共有 per-user Host + Named Pipe + クラッシュ隔離が正しい |
| macOS/fcitx5固有 | XKB keysym キー抽象 + Right Alt/Super 一方向トグル | Windows は VK_KANJI/VK_NONCONVERT/VK_CONVERT/VK_DBE_*（MS-IME 互換）が正典 |
| MVPスコープ破壊 | 適応 2 モデル常駐切替 / ParallelBeam | main 90M + light 26M 2 モデル常駐前提。v1.0 後に M47 と整合 |
| ライセンスリスク | symbols.yml/emoji.yml の Mozc 由来データ丸ごとコピー / jinen モデル再配布 | BSD-3（Google 名前条項）+ CLDR 表記義務を新規に背負う。jinen は azooKey の zenz と別系統 |
| 費用対効果 | HTTP サーバ常駐 + Web UI / HF 自動 DL キャッシュ配置 / `tracing` 採用 | in-proc TIP に HTTP listener は UI スレッド占有。HF 自動 DL は供給源 pin 原則と相反。tracing は M41 自前 JSONL 方針と衝突 |
| 費用対効果 | 学習 score 式 / TSV 生 split・非原子書込 | azooKey は `exp` 減衰 + 負例学習 + TSV エスケープ + `MoveFileExW` 原子書込で既に堅牢 |
| 取り込み不要 | JSON-RPC（認証なし）/ ローマ字 trie / beam search 本体 | azooKey の認証+version IPC・分離済み `RomajiKanaConverter`・abort callback 付き beam が同等以上 |

---

## 7. 推奨ロードマップ

### Phase A: すぐできる低リスク改善
- 候補 7: `THIRD_PARTY_LICENSES` / NOTICE 空雛形 + ヘッダ規約の新設
- CI: docs-only（`docs/**`, `*.md`）を build job からスキップする最小 paths フィルタ

### Phase B: MVP前に入れる価値がある改善
- 候補 1: M13 着手時に karukan を設計レビュー資産として併置（**P0**）
- 位置単位の spec 固定（IPC payload と core 境界で UTF-16 / Unicode scalar / byte のどれかを明記）

### Phase C: MVP後に入れる大型改善
- 候補 2: 数字リライターを `core/` に純アルゴリズム移植（M62-A、最初の具体機能）
- 候補 4: `is_japanese` 非日本語チャンク verbatim 通過を M14/M58 チャンカに採用
- 候補 5: 記号リライター（M62-C、Mozc データ整備を伴う）
- 候補 6: 推論チューニング数値ノブを schema へ追加
- 候補 8: 辞書/学習の読み取り専用 lookup CLI（Host 非経由）
- 候補 3: モデル DL + SHA256 + カタログ + プリフェッチ（zenz ライセンス DEV-202 確定後）
- 候補 9: ChunkPlan 相当の増分再チャンク純粋関数（M14 限定、M58 と分離）
- 半角カタカナ + 英字リライター（M62-B、英字は M60 へ統合）

### Phase D: 長期検討
- 候補 10: 変換品質評価 CLI（M52）
- 絵文字リライター（M62-D、emoji.yml 再ポート + ライセンス + Host 配線 + 候補窓 UX）
- 学習データ構造の reading-keyed 二層化 + 前方一致 index（M15 予測供給）
- 適応モデル切替を M47 DegradedModel と整合させ再解釈
- `NllScorer` 型ニューラル・リランク（`docs/neural-reranker-spec.md`）
- CI sccache 配線 / double-array trie システム辞書（M53）

---

## 8. 次に実装するなら（最初の PR 案）

### PR 案 1: `feat(core): 数値候補リライター（異表記展開）を OS 非依存純アルゴリズムとして追加`（M62-A）
- **目的**: reading が純十進数字のとき漢数字/大字/ローマ数字/丸数字/進数の異表記候補を生成する純関数を `core/` に新設。
- **変更ファイル**: `core/include/azookey/core/NumberRewriter.h`（新規）, `core/src/NumberRewriter.cpp`（新規）, `core/tests/number_rewriter_test.cpp`（新規）, `core/CMakeLists.txt`
- **実装手順**: ① `number.rs:32-60` のテーブル/説明文を C++ 定数として自前定義 ② 4 桁セグメント + 大位の漢数字アルゴリズム ③ ローマ/丸数字は値インデックス、範囲外は空 ④ 混在は素通し ⑤ inference-host 配線は別 PR
- **テスト手順**: GoogleTest で `123→百二十三/壱百弐拾参/0x7b/0b1111011`（丸数字/ローマは範囲内のみ・例 `12→⑫`）、`0` 特例、上限超スキップ、混在素通しを AAA で固定
- **ロールバック容易性**: 高（新規ファイル追加のみ、既存シンボル不変、未配線で本体挙動に影響なし）
- **想定リスク**: 低（OS 非依存 `core/` の純追加で TSF/IPC/UI スレッドに無干渉）
- **規約注記**: AGENTS.md に従い M62-A の Linear 起票 + `docs/candidate-rewriter-spec.md` 新設

### PR 案 2: `chore(licensing): THIRD_PARTY_LICENSES 雛形とデータヘッダ規約を新設`（候補 7）
- **目的**: 将来の Mozc 由来データや辞書取込に備え、不在の attribution 集約ファイルとヘッダ規約を整備
- **変更ファイル**: `THIRD_PARTY_LICENSES`（新規）, `docs/licensing-policy.md` または既存 docs への追記
- **実装手順**: ① 「現状第三者同梱データ無し」明記 ② データ採用時の追記手順（ヘッダ規約、BSD-3 名前条項、CLDR/UniDic/NEologd 連鎖） ③ README はリンクのみ
- **ロールバック容易性**: 極高（文書追加のみ）
- **想定リスク**: 極低

### PR 案 3（任意・小）: `chore(ci): docs-only 変更を Windows build job からスキップ`
- **目的**: `docs/**`, `*.md` のみの変更で全ツリー Debug+Release ビルドが回るのを回避
- **変更ファイル**: `.github/workflows/windows.yml`（`paths-ignore` または job レベル `paths`）
- **想定リスク**: 低（回帰取りこぼし回避のため docs-only に限定）

---

## 9. 参照ファイル一覧

### azooKey-Desktop 側
- `README.md`, `AGENTS.md`, `CMakeLists.txt`, `plans/windows-port-roadmap.md`（M0-M62）, `settings/mvp-settings.schema.json`
- `core/include/azookey/core/`（`IConverter.h`, `Candidate.h`, `RomajiKanaConverter.h`, `SimpleConverter.h`）, `core/data/mock-dictionary.tsv`
- `tsf-tip/`（`TextService.h/.cpp`, `CandidateSelection.h`, `CandidateUiCoordinator.cpp`）
- `inference-host/`（`InferenceEngine.h/.cpp`, `ZenzaiModelConverter.cpp`, `Dispatcher.cpp`, `RequestScheduler.cpp`, `SettingsStore.*`, `UserDataPaths.cpp`, `UserDictCli.cpp`）
- `ipc/`（`Messages.h`, `Payloads.h`, `Limits.h`）, `learning/`（`LearningStore.*`, `Reranker.cpp`, `UserDictionary.*`, `AtomicFile.h`）, `bench/live_bench.cpp`
- `docs/`: `legacy-parity-spec.md`(M13), `windows-tsf-host-architecture.md`, `tsf-deep-integration-spec.md`, `zenzai-inference-spec.md`, `ai-backend-spec.md`, `model-management-spec.md`, `copilot-pc-backend-spec.md`, `zenzai-gpu-route.md`, `learning-data-management-spec.md`, `user-learning-enhancement-spec.md`, `neural-reranker-spec.md`, `rich-features-spec.md`, `inline-english-candidate-spec.md`, `dynamic-punctuation-spec.md`, `bracket-pairing-spec.md`, `romaji-batch-conversion-spec.md`, `conversion-quality-benchmark-spec.md`, `dev-infrastructure-spec.md`
- `.github/workflows/windows.yml`, `release.yml`

### karukan 側
- `README.md`, `Cargo.toml`, `THIRD_PARTY_LICENSES`, `LICENSE-MIT`, `karukan-cli/docs/LEGAL`
- `karukan-engine/src/`: `lib.rs`, `dict.rs`, `learning.rs`, `kanji/`（`mod.rs`, `backend.rs`, `llamacpp.rs`, `model_config.rs`, `hf_download.rs`, `error.rs`）, `rewriter/`（`mod.rs`, `number.rs`, `alphabet.rs`, `half_katakana.rs`, `symbol.rs`, `emoji.rs`）, `romaji/`, `models.toml`, `data/symbols.yml`
- `karukan-im/src/`: `lib.rs`, `core/state.rs`, `core/keycode.rs`, `core/engine/`（`mod.rs`, `input.rs`, `input_buffer.rs`, `mode.rs`, `cursor.rs`, `conversion.rs`, `chunk.rs`, `strategy.rs`, `types.rs`, `tests/*`）, `config/settings.rs`, `config/default.toml`, `server/mod.rs`, `server/protocol.rs`
- `karukan-cli/src/bin/`（`dict.rs`, `server.rs`, `sudachi_dict.rs`, `ajimee_bench.rs`）
- `karukan-macos/Sources/KarukanIME/KarukanInputController.swift`, `karukan-fcitx5/src/ffi/`
- `.github/workflows/`（`karukan-engine-ci.yml` 他 5 本）, `scripts/emoji_porter.py`

---

## 付録: ジャンル別・優先度順 整理（取り込み実行用バックログ）

### A. 設計参照候補（コードは移植せず、設計レビュー資産として使う）

| 優先度 | 候補 | 内容 | 鍵・前提 |
|---|---|---|---|
| P0/MVP前 | M13 状態機械の設計参照 | `InputState`(値)/`EngineAction`(抽象出力)/純粋関数を M13 の到達点として併置 | `legacy-parity-spec §1.1` が既に引用。独自再設計=VK表/IPC非同期/`ITfRange`翻訳 |
| P1/MVP前 | 位置単位の spec 固定 | IPC payload と core 境界で UTF-16/Unicode scalar/byte を明記 | `protocol.rs:24-26` を範に。M13 と独立 |

### B. 基盤整備候補（低リスク・先行すべき前提）

| 優先度 | 候補 | 内容 | 鍵・前提 |
|---|---|---|---|
| 今すぐ(Phase A) | `THIRD_PARTY_LICENSES` 雛形 | attribution 集約ファイル + データヘッダ規約 | azooKey に不在。記号/絵文字/辞書取込の**前提** |
| 今すぐ(Phase A) | CI: docs-only スキップ | `docs/**`,`*.md` のみで全ビルドを回さない | `core` 変更は全 job 保証 |
| P3/低リスク | CI: sccache 配線 | ローカル配線済を CI へ拡大 | per-module paths 分割は後回し |

### C. 新規機能の取り込み候補（コードを書く・原則 MVP後）

| 優先度 | 候補 | 内容 | 鍵・前提（M / ライセンス / リスク） |
|---|---|---|---|
| P1/MVP後 ★最初 | 数字リライター | 漢数字/大字/ローマ/丸数字/進数を `core/` に純アルゴリズム | **M62-A**。Mozcデータ非依存=**最安全**・無IPC・TIPローカル |
| P1/MVP後 | is_japanese verbatim | 非日本語チャンクを推論に送らず原文通過 | M14/M58。純粋関数で `core/` テスト可 |
| P1/MVP後 | モデルライフサイクル管理 | レジストリ+DL+SHA256+プリフェッチ+pre-tok上書き | M32/M45。**ブロッカー: zenz配布ライセンス DEV-202** |
| P1機能/P2データ | 記号リライター | variant chain + かな読み引き | **M62-C**。データはMozc由来BSD-3 → 再ポート+B前提 |
| P2/MVP後 | 設定 数値ノブ | `inferenceThreads`/`maxCandidates`/`maxContextLength` | 形式/パス層は変えず項目追加。スレッド既定は `powerProfile` 連動 |
| P2/MVP後 | 辞書/学習 lookup CLI | 完全一致/前方一致/表層形(Host非経由) | DXギャップ。Web/HTTP/tracingは採らない |
| P2/MVP後 | ChunkPlan 増分再チャンク | prefix/suffix差分で中間スパンだけ再変換 | M14**限定**(M58とは別レイヤ) |
| P2/MVP後 | 半角カタカナ + 英字 | 全/半角カタカナ・英字大小/全半角 | **M62-B**。英字はM60へ統合 |
| P2/v1.0後 | 変換品質評価CLI | Exact Match率 + CER + NFKC二重評価 | M52。**評価データは§13準拠で自前作成** |
| P2価値/P3コスト | 絵文字リライター | かな読み→絵文字 + `:trigger`曖昧検索 | **M62-D**。emoji.yml=Mozc+CLDR・Host配線・UX再設計 |

### D. 発展・長期候補（参考にとどめ、基盤が固まってから）

| 優先度 | 候補 | 内容 | 鍵・前提 |
|---|---|---|---|
| P3/v1.0後 | 学習 reading-keyed 二層 + 前方一致 | `reading→surfaces` 構造化で M15 予測供給 | **線形走査は不可** → trie/`lower_bound`前提。score式は維持 |
| P3/保留 | システム辞書 double-array trie | 前方一致+マルチソースビルド | M53と一緒に。Sudachi/UniDic/NEologdライセンス |
| P3/v1.0後 | NllScorer ニューラルリランク | コンテキスト再利用で辞書候補を Zenzai リランク | `docs/neural-reranker-spec.md` |
| v1.0後/要再解釈 | 適応モデル切替 | `max_latency_ms`→light の遅延降格 | **2モデル常駐は採らない** → M47 と整合させ再解釈 |

### E. 取り込まない（参考・衝突 or 劣位）

プロセス結合トポロジ（macOS子プロセス / fcitx5 in-proc FFI）／ HTTPサーバ+Web UI ／ HF自動DLキャッシュ配置 ／ `tracing`採用 ／ 学習score式・非原子書込 ／ JSON-RPC認証なし ／ XKB keysymキー抽象 ／ beam search本体（azooKeyに実装済）。

---

## 結論

- **今すぐ取り込むべきもの**: ① M13 設計参照として karukan-im を併置（P0、新規コスト最小） ② `THIRD_PARTY_LICENSES` 雛形整備（Phase A、リスクゼロ）
- **次に調査すべきもの**: ① zenz GGUF 配布ライセンス（Linear DEV-202、候補 3 のブロッカー） ② 配置 zenz GGUF の tokenizer 内蔵メタデータの実態 ③ 候補リライターの M62 位置づけ確定
- **後回しでよいもの**: ライブ変換の増分チャンク（候補 9/M14）、モデル DL 機構（候補 3/ライセンス確定後）、評価 CLI（候補 10/M52）、絵文字リライター、学習前方一致、システム辞書 trie 化
- **取り込まない方がよいもの**: プロセス結合トポロジ、適応 2 モデル常駐切替、HTTP サーバ+Web UI、HF 自動 DL キャッシュ配置、tracing 採用、学習 score 式・非原子書込、JSON-RPC 認証なし
- **最初に作るべき PR**: `feat(core): 数値候補リライター`（M62-A、真のギャップ・純コード・無 IPC・TIP ローカル・ライセンス最小・テスト容易）。次点で `chore(licensing): THIRD_PARTY_LICENSES 雛形`
