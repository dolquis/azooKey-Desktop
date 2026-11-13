# ニューラルリランカー仕様（Track B: NllScorer 型ニューラルリランク）

対象リポジトリ: dolquis/azooKey-Desktop
関連: `docs/neural-reranker-spec.md`（Track A: M56 Tiny Neural Reranker）、
      `docs/zenzai-inference-spec.md`（推論基盤）、
      `docs/conversion-quality-benchmark-spec.md`（M52）

本書は `docs/neural-reranker-spec.md` から Track B（旧 §B1〜§B12）を切り出した文書である。
章番号 `§B<n>` は旧 spec のまま保つ。両トラックは別モデル・別実装・別スケジュールで、
接点はスコア統合レイヤ（§B6）だけである。本書内の「Track A §N」は
`docs/neural-reranker-spec.md` の節を指す。

## 節とマイルストーンの対応

| 節 | マイルストーン |
|---|---|
| §B1〜§B5 | Track B（M 番号なし。前提は M8） |
| §B6 | Track B と Track A の接点（スコア統合） |
| §B7〜§B12 | Track B（M 番号なし。前提は M8） |

対応課題: DEV-413（実装）/ DEV-556（本章の設計確定）
参考実装（**設計思想のみ**。逐語移植しない）: karukan
`karukan-engine/src/kanji/llamacpp.rs`（`NllScorer`。コンテキストを
再利用して (reading, surface) ペアの文字あたり NLL を算出する）。本章の算出式・
正規化・合成規則は azooKey 側で独自に定めたものであり、参考実装の写しではない。

## B1. 目的とスコープ

**目的**: すでにロード済みの Zenzai モデルを使い、**辞書由来候補**の並び順を
系列尤度（NLL）で整える。生成は行わない。

- **入力**: 1 リクエスト内で確定した (左文脈, 読み) と、マージ済み候補列。
- **出力**: 対象候補への加算スコア（§B6）。候補の追加・削除・表層の書き換えは
  行わない。
- **前提**: M8（Zenzai 配線、`docs/zenzai-inference-spec.md`）のみ。M52 ベンチ・
  M53〜M55 を前提にしない（Track A と独立に着手できる）。
- **実装配置**: `inference-host/` 内で完結する。`ipc/` と `tsf-tip/` は変更しない
  （§B4.4）。
- **適用経路は `QueryCandidates` のみ**。`QueryPredictions`（次語予測）と
  `QueryCorrections`（訂正候補）には適用しない。前者は読みが確定していないため
  §B2.1 のプロンプト（`[IN]<input_katakana>[OUT]`）が成立せず、後者は候補集合が
  小さくレイテンシに対する利得が薄い。両者への拡張は将来課題とする。
- **`QueryCandidates` の中でも `live=true`（ライブ変換）は対象外**。ライブ変換は
  打鍵ごとに走る最小レイテンシ経路で top-1 のみを使う
  （`docs/zenzai-inference-spec.md` §6.1）ため、並べ替えの利得が無いまま
  §B7 の予算（既定 20ms）を毎打鍵に上乗せすることになる。NllScorer は
  **候補ウィンドウ要求（`live=false`）にのみ**適用する。
- **日英の判別には使わない**。NLL は文脈より `[IN]` に書いた形に強く従うため、日本語の表層と
  英字の表層を比べても判別の根拠にならない。日英混在入力（M63）の分割は Zenzai の外で決め、
  本トラックの適用範囲は変えない（`docs/mixed-english-auto-conversion-spec.md` §3）。

### B1.1 Track A（M56）との関係

- **排他ではない**。Track A は全ソース候補を手作り特徴量 + ONNX MLP で並べ替える層、
  Track B は辞書由来候補だけを LLM 尤度で整える層で、評価対象も信号源も異なる。
- 両方が有効な環境では、Track B の出力は Track A より**手前**に効く（§B6 の適用点）。
- Track B の `nll_per_char` を Track A の特徴量（`docs/neural-reranker-spec.md` §4）や `final_score`（同 §9）へ
  取り込むかは **M56 実装時の判断**とし、本章では決めない。取り込む場合も
  `features_v1` の 12 次元契約（同 §5.2）は Track B からは変更しない。

### B1.2 既存 Zenzai 生成との関係

Zenzai が生成した候補（`CandidateSource::Model`）は生成時点で系列 logprob を持ち、
`docs/zenzai-inference-spec.md` §6.5 で [0.3, 1.4] 帯へ写像済みである。これを
NllScorer で再評価すると**同じ信号を二重に掛ける**ため、生成候補は対象外とする
（§B5）。Track B が埋めるのは「Zenzai を通っていない辞書候補には尤度の情報が
一切無い」という空白である。

## B2. NLL の定義

### B2.1 プロンプトと候補トークン列

プロンプト `P` は `docs/zenzai-inference-spec.md` §3.2 の zenz-v3 フォーマットと
**完全に同一**とする（別フォーマットを作らない）。

```
P = [CTX]<clean_left_context>[IN]<input_katakana>[OUT]
```

- 前処理（カタカナ化・左文脈 30 文字切り詰め）も同 §3.1 と同一。生成経路と
  同じ `BuildZenzaiPrompt` を共有する。
- 候補表層 `s` は `llama_tokenize` でトークン列 `t_1 … t_m` にする。
  **BOS を付与しない**（BOS は `P` 側のみ。同 §5 の `add_bos_token` 契約に従う）。
- **EOS をトークン列に含めない**（決定）。理由:
  1. GGUF が宣言する `tokenizer.ggml.eos_token_id` は pin モデルでずれており、
     `docs/zenzai-inference-spec.md` §9.2 の override 契約に依存する（DEV-743）。
     EOS 項を NLL に入れると、
     スコアが override の有無というモデルメタデータ都合に左右される。
  2. EOS の logprob は「ここで文が終わる自然さ」を測る別の量であり、読みが同一で
     表層長の異なる候補間で短い表層を系統的に有利にする。
  3. 辞書候補は**完結した表層**として与えられ、終端の判断は不要。

### B2.2 算出式

`log p(t_i | …)` は、`t_1 … t_{i-1}` まで decode した位置の logits に
log-softmax を掛けた値とする（数値安定化は生成経路の `CollectTokenChoices` と
同じく最大値減算で行う。top-K への絞り込みは行わず、対象トークン id の値のみ使う）。

```
NLL_total(s) = - Σ_{i=1..m} log p(t_i | P, t_1 … t_{i-1})
nll_per_char(s) = NLL_total(s) / max(1, C(s))
```

- `C(s)` = 表層 `s` の **UTF-8 コードポイント数**。
- 累算は `double`。

### B2.3 正規化単位をコードポイントとする理由（決定）

分母に**トークン数ではなくコードポイント数**を採る。

- トークン平均（`NLL_total / m`）は語彙依存が強い。同一読みの候補でも、語彙に
  1 トークンとして載っている表層（例「日本語」）と複数トークンに割れる表層
  （例「二本語」）でスケールが変わり、**語彙頻度バイアスがそのまま順位に乗る**。
- コードポイント数は候補固有かつモデル非依存の量で、同一読みの候補集合では
  ほぼ等長になる。したがって候補間比較は実質的に `NLL_total` の比較に近い意味を
  保ちつつ、長さの異なる候補（送りがな違い等）でも破綻しない。
- 書記素クラスタ単位（結合文字・異体字セレクタを 1 文字に畳む）には**しない**。
  決定性と実装単純性を優先する。日本語表層では両者の差はほぼ生じず、差が問題に
  なる表層が現れた時点で別課題として再検討する。

### B2.4 非有限値の扱い

`log p` に NaN / Inf が 1 つでも現れた候補は**スコア不能**として扱い、その候補
だけを対象集合から外す（元の score と順位を保つ）。他候補の評価は継続する。
全対象候補がスコア不能なら §B8 の `invalid_score` として何も適用しない。

## B3. コンテキスト再利用（prefix KV 共有）

1 リクエスト内で `P` は全候補に共通である。これを候補ごとに decode し直すと
コストが候補数に比例して跳ね上がるため、**prefix の KV を 1 回だけ作って
候補の作業 sequence へ共有する**。CPU・Vulkan とも、候補全体が高々 2 token
（suffix が高々 1 token）の連続候補を最大 16 sequence にまとめる。長い候補は
候補単位で decode し、従来の chunk 形状を維持する。batch の演算形状による数値差の
検査は §B11 に従い、prefix 共有・生成の同等性検査とは分ける。

```
1. 再利用できない範囲の KV を落とし、P の残りを decode する（prefix KV 確立、prefix_len トークン。§B3.4）
1'. prefix 最終位置の logits を**スナップショットへ複製**する（§B3.1）
2. 各候補 s について:
   a. t_1 の logprob は 1'. のスナップショットから読む
   b. sequence 0 の prefix を候補専用 sequence へ複製する
3. 上記の単位で t_1 … t_{m-1} を decode し、t_2 … t_m の logprob を読む
4. 候補の作業 sequence を解放し、sequence 0 の prefix と logits を保持する
5. 予算 / cancel は prefix decode、候補準備、suffix decode の前後と logits 集計中に確認する（§B7）
```

候補群の suffix 合計が共有 KV の空き容量を超える場合は、収まる候補群に分けて
3.・4. を繰り返す。単独候補の context 上限は変えず、batch の token 数は
`min(32, llama_n_batch)` 以下に分割する。長い候補は従来の 32 token 区切りを維持する。
各 chunk の decode 前後で `ShouldAbort` を確認し、次の chunk へ進む前に中断する。
1 token の候補には suffix decode が不要である。

### B3.1 prefix logits のスナップショット契約

**prefix 最終位置の logits は、prefix を decode した経路（生成・評価のいずれか）の直後に
複製して全候補で再利用する。**

`llama_get_logits*` が返すのは**直近の `llama_decode()` の出力**である。KV を
`prefix_len` へロールバックしても logits バッファは巻き戻らないため、候補 1 の
decode 後に読める「最終位置の logits」は prefix のものではなく**候補 1 の最終
出力**である。スナップショットを取らずに §B3 の 2.a を実装すると、2 候補目以降の
`t_1` の logprob が誤る（あるいは候補ごとに prefix を decode し直すことになり、
本節の共有契約そのものが失われる）。

- 複製するのは prefix 最終位置の logits 行（`vocab_size` 個）。候補ごとに `t_1` が
  異なるため、特定トークンの値だけでは足りない。
- log-softmax の分母（§B2.2 の最大値減算を含む正規化項）は**スナップショットから
  1 回だけ算出**し、全候補で使い回す。
- スナップショットは prefix キャッシュと寿命を共にする。キャッシュが指す prompt と
  一致する限り再利用し、prompt が変われば、あるいはキャッシュを無効化すれば
  取り直す（§B3.4）。

回帰テストで固定する（§B11）: 候補を 2 件以上与え、候補 2 の `t_1` の logprob が
**prefix 由来**であること（候補 1 の出力由来になっていないこと）。

### B3.2 off-by-one 契約

**`t_i` の logprob は `t_1 … t_{i-1}` まで decode した位置の logits から読む。**
したがって:

- `t_1` の logprob の出所は**候補トークンではなく prefix の最終位置**である。
- `t_m` 自身を decode する必要は無い（`t_m` の出力 logits は使わない）。
  候補あたり実際に decode するのは `m-1` トークンである。

この 1 ずれは実装で最も壊れやすい箇所であり、テストで固定する（§B11）。

### B3.3 既存 `DecodeTokens` を流用しない

`inference-host/src/ZenzaiModelConverter.cpp` の `DecodeTokens` は
NllScorer にそのまま使えない。`llama_batch_get_one` で組んだ batch は
**最終位置の logits しか出さない**ためである。NLL には位置ごとの logits が要る。

NllScorer 専用の decode ヘルパを設け、(a) KV をクリアしない、(b) 位置ごとに
logits 出力を要求する batch を組む、の 2 点を満たす。

`llama_batch_init` で batch を確保し、`pos` と `seq_id` を明示する。
候補の位置ごとに `logits[]` を立て、`llama_get_logits_ith` で対応する行を読む。
`llama_memory_seq_cp` で sequence 0 の prefix を作業 sequence へ共有する。
suffix の行は次の decode より前に logprob へ集計し、候補ごとの全 vocab 行の
コピーは保持しない。集計後に `llama_memory_seq_rm` で作業 sequence を全て解放する。
pin の正典は `CMakeLists.txt` の
`AZOOKEY_LLAMA_CPP_GIT_TAG` とする。

### B3.4 `llama_context` の共有とスレッド

- 生成経路と**同一の `llama_context`** を使う。モデルを 2 つ常駐させない。
- NllScorer は `Convert` の**完了後**、`converter_call_mutex_` を保持して走る。
  decode 中は `state_mutex_` を保持せず、Health とモデル交換の応答性を保つ。
- `Convert` と NllScorer は同じ prompt prefix を KV キャッシュで共有する（§B12）。
  評価は beam の作業シーケンスを解放し、sequence 0 を再利用できる prefix 長へ
  切り詰めてから、残りのトークンだけを decode する。prompt が生成時のものと一致し、
  その最終トークンの logits を保持していれば decode するトークンは 0 になる。
- 生成も prompt token 列がキャッシュと完全一致し、その末尾の logits を保持していれば、
  prompt 全体を再利用する。beam の初手はそのスナップショットを読む。部分一致時は
  必要な suffix を decode してスナップショットを更新する。suffix が空でも cancel と
  deadline を確認する。
- sequence 0 は prompt 専用とし、生成の beam は 1〜4、NLL は最大 1〜16 を
  作業用に使う。長い候補は候補ごとに sequence 1 を再利用する。
  `n_seq_max` は 17、`kv_unified` は true とし、KV 容量は共有 prefix と同時に保持する
  suffix の合計で管理する。両経路は排他実行で、開始時に作業 sequence を解放する。
- NLL は sequence 0 に suffix を追加しない。評価後は作業 sequence を解放し、
  キャッシュが指す prompt と logits を次の生成へ引き継ぐ。
- 途中の失敗や cancel では prefix キャッシュを無効化して KV を消去し、次の生成が
  古いキャッシュを参照しないようにする。
- NllScorer を別スレッドへ出さない（`llama_context` は共有できない）。

## B4. 責務境界

### B4.1 IPC 責務境界（決定: 新規メッセージ型を作らない）

**Track B は IPC を一切変更しない。** 新しい `MessageType` を追加せず、
`QueryCandidates` の request / response payload（`ipc/include/azookey/ipc/Payloads.h`）
も変更しない。

理由:

- 候補集合は **TIP から送られてこない**。現行アーキテクチャでは
  `InferenceEngine::QueryCandidates` が user_dict + converter の出力を Host 内部で
  合成し（`docs/zenzai-inference-spec.md` §7.1）、TIP は読みと文脈を送るだけである。
  「TIP からリランク要求として候補集合を送る」形は現行の責務分割に無い。
- リランクは Zenzai の `llama_context` を要する。これは Host が単独で所有する資源で、
  プロセス境界をまたがせる理由が無い。
- 追加ラウンドトリップは、1 変換あたりのレイテンシ予算
  （`docs/zenzai-inference-spec.md` §8）を理由なく食う。

したがって NllScorer は `QueryCandidates` の内部フェーズであり、**TIP からは
「候補の並びが変わる」以外に観測されない**。

### B4.2 外部から観測できるもの

| 経路 | 内容 |
|---|---|
| `Candidate::debug_info` | 対象候補に `nll=<nll_per_char>;nlld=<bonus>` を追記（既存の `dup:` 併記と同じ流儀。表層・読みは含めない） |
| `Health` | **runtime 失敗のみ** `model_runtime_error_` へ `nll-scorer:<reason>` をミラーし、既存の `last_error` フィールドで運ばれる（§B8）。payload の形は変わらない |
| 構造化ログ（`azookey::logging::RuntimeLogger`） | `nll_rerank` イベントの `reason`、`nll_targets`、`nll_applied`、`latency_ms`（全所要時間）。IPC 要求に紐付く場合は `request_id` も記録する。適用時は `reason=applied`。既定 OFF・live・cancel では出力しない（§B9）。フィールド名の正典は `docs/observability-spec.md` §7.2 |

**`QueryDiagnostics` へは出さない（決定）。** 現行 `QueryDiagnosticsPayload`
（`ipc/include/azookey/ipc/Payloads.h`）は `model_loaded` / `engine` / `backend` /
`rss_mb` / EP 情報 / entry 数 / `fallback_state` / `last_error` のみを持ち、
`reason`（正常系を含む）や対象候補数を表すフィールドが無い。ここに出す要件を残すと
optional field 追加が必要になり、§B4.1 の「IPC を変更しない」と両立しない。

正常系を含む `reason` の観測先は**構造化ログに一本化する**。これは Track A が
fallback reason を構造化ログへ出し、M52 ベンチの `fallback_rate` 集計に使う設計
（Track A §7.2）と同じ流儀であり、IPC を触らずに `fallback_rate` 相当の集計ができる。
`QueryDiagnostics` に載せたくなった場合は、後方互換な optional field 追加として
**別課題で IPC 互換性を含めて設計する**（Track B の範囲外）。

### B4.3 モジュール境界

- `core::IConverter` は変更しない。NLL 評価は変換ではなく、全 converter が
  持てるインターフェースではない。
- NllScorer は `ZenzaiModelConverter` が公開する評価 API（`llama_context` を
  持つ側のメンバ）として実装し、`InferenceEngine` は
  `MirrorModelRuntimeErrorLocked` と同じく `dynamic_cast<ZenzaiModelConverter*>` で
  到達する。Zenzai 以外が active な間は `model_not_loaded` で no-op になる。

### B4.4 実装配置

`inference-host/src/ZenzaiModelConverter.cpp`（NLL 評価 API・専用 decode ヘルパ）、
`inference-host/include/azookey/host/ZenzaiModelConverter.h`、
`inference-host/src/InferenceEngine.cpp`（適用点の挿入）、
`inference-host/include/azookey/host/InferenceEngine.h`（`EngineConfig` へ設定追加）、
`inference-host/tests/`、`settings/mvp-settings.schema.json`（§B9）。
NLL の数値計算・候補選別・予算制御は `NllScorer.h` / `NllScorer.cpp` に置く。
`SettingsStore` は設定を Host へ適用し、`settings-app/SettingsDocument.cpp` は
設定画面の保存時に有効な `reranker` キーを保持する。評価ベンチは `bench/nll_bench.cpp` とする。
`ipc/` `tsf-tip/` `core/` `learning/` は**変更しない**。

## B5. 対象候補集合

| `CandidateSource` | 対象 | 理由 |
|---|---|---|
| `SystemDictionary` / `Heuristic` | **対象** | 尤度の情報を持たない辞書・ヒューリスティック候補。Track B が埋める空白 |
| `UserDictionary` | 対象外 | ユーザーが登録した語の順位を LLM 尤度で動かさない（`docs/zenzai-inference-spec.md` §7.3 / §7.6 のソース優先と整合） |
| `Model` | 対象外 | Zenzai 生成候補。生成時の系列 logprob と二重評価になる（§B1.2） |
| `Llm` | 対象外 | M16 クラウド整文用の予約。性質が異なる |

- 上限 `nllTopK`（既定 **8**、範囲 1〜16）。適用直前の score 降順で上位から採る。
  suffix をまとめる単位は §B3 に従う。コストは候補数に加え、suffix token の合計、
  KV と batch の容量による分割数に依存する。予算（§B7）はリクエスト全体へ適用する。
- 対象が 0 件のときは何もしない（§B8 の `no_target`。失敗ではない）。

### B5.1 選択集合の外に対する保証

対象外ソースの候補と、`nllTopK` からあふれた候補について保証するのは
**`score` と `debug_info` が不変であること**に限る。**相対順位の不変は保証しない。**

最終順位は候補集合全体に対する score 降順の `stable_sort`（§B6.1）で決まるため、
選択集合内の候補が減点されれば、選択集合外の候補が自分の score を変えないまま
相対的に浮上しうる。例: `nllTopK=2` で対象 A=1.00 / B=0.95、選択集合外 C=0.94 の
とき、B に -0.30 が適用されれば C は B を追い越す。これは全体ソートを持つ以上
避けられず、**並べ替えという目的そのもの**でもある。

ただし §B6.2 が減点のみであることから、次の一方向の性質は保証される:

> **選択集合外の候補が、NLL 補正によって順位を下げることはない。** 補正は
> 選択集合内の候補を下げるだけなので、集合外の候補の相対位置は同じか上がるかの
> いずれかである。

したがって `UserDictionary` 候補（§B5 で対象外）は、NLL によって順位を落とされない
という保証を持つ。§B11 のテストもこの粒度で書く。

## B6. スコア合成（既存 `Reranker` との関係）

### B6.1 適用点

```
QueryCandidates:
  user_dict lookup → converter Convert → merge → DedupMergedCandidates
    → ★ NllScorer（本章）
    → ApplyRerankerOrRaw（learning::Reranker: 学習加点 + score 降順 stable_sort）
```

`DedupMergedCandidates` の**後**、`ApplyRerankerOrRaw` の**前**に置く。

この順序が DEV-413 の「既存 `Reranker` とは排他でなく補完」の具体化である。
学習加点はユーザー個人の確定履歴という最も強い個別化信号であり、最後に載せて
最終順位を支配できるようにする。NLL は辞書候補の**事前順位**を整えるだけの層で、
学習された選好を上書きしない。

`learning::Reranker::Apply` の実装（加算 → `stable_sort`）と最終順位規則
（`docs/zenzai-inference-spec.md` §7.3）は**変更しない**。

### B6.2 合成形（減点のみの相対補正）

score を**置換せず加算項**として合成する。対象集合内の最尤候補を基準にする。

```
nll_min   = min over 対象候補 i of nll_per_char_i
delta_i   = nll_min - nll_per_char_i                  // ≤ 0
bonus_i   = max(delta_i, -kNllSpan) * nllWeight       // ≤ 0
score_i  += bonus_i
```

- `kNllSpan` = **2.0**（nats/char。コード内定数）、`nllWeight` 既定 **0.15**。
  したがって 1 候補あたりの補正幅は **[-0.30, 0]** に収まる。
- **加点しない（減点のみ）**のは意図的である。最尤候補は 0 のまま据え置き、
  劣る候補だけが尤度差に応じて下がる。これにより:
  - 対象集合内の並びは「元の score − 尤度ギャップ」で決まり、目的である
    辞書候補の並べ替えは達成される。
  - `docs/zenzai-inference-spec.md` §7.2 の **score 帯を上向きに壊さない**。
    辞書候補が user_dict 帯（1.5）や Zenzai 帯上限（1.4）を NLL 由来で
    追い越すことが構造的に起こらない。
  - 帯からの下方向のはみ出しは `kNllSpan * nllWeight`（既定 0.30）に上限が
    あり、帯幅を超えて沈むことはない。
- 同点時は `stable_sort` により従前の順序（挿入順）が保たれる。NLL は独自の
  タイブレーク規則を導入しない。
- `debug_info` へ `nll=` / `nlld=` を追記する（§B4.2）。

### B6.3 Track A `final_score` との関係（インターフェース注記）

Track A §9 の `final_score` へ NLL を載せる場合は、`normalize(nll_score)` の
1 項として扱い、重みは M52 ベンチで校正する（§9 の他項と同じ扱い）。本章は
Track A の重み表を変更しない。§B6.2 の加算合成は、Track A が未導入の環境で
Track B が単独で成立するための形式である。

## B7. 予算・キャンセル・打ち切り

- リクエスト単位の予算 `nllBudgetMs`（既定 **20ms**、範囲 5〜60、範囲外は clamp）。
- 既存の deadline / cancel plumbing（`kModelConversionBudget`、
  `ConversionContext` の cancel、`docs/zenzai-inference-spec.md` §9.2.2）に載せる。
  NLL 用の別スレッド・別タイマーを作らない。
- **all-or-nothing**（決定）: 予算超過を検出した時点で、そのリクエストの NLL 適用を
  **全面的に破棄**し、NLL 適用前の score のまま続行する。部分適用はしない。
  - 理由: 部分適用では「先に評価された候補だけが減点される」ため、同じ入力でも
    実行時のゆらぎで順位が変わる。IME の決定性を優先する（Track A §7.2 が
    timeout に対して whole-request skip を選ぶのと同じ方針）。
  - 実装上は `Candidate::score` を直接書き換えず、bonus を別配列に貯め、
    **完走したときだけ**適用する。
- cancel は prefix decode と suffix batch decode の前後、候補準備時、候補群の境界、
  logits 集計中に確認する。
- **cancel は失敗ではなく中断**（決定）: 現行 `QueryCandidates` は cancel 観測時に
  候補列そのものを返さない（空を返す）。したがって cancel 時の NllScorer は
  「fallback で順序を保つ」対象ですらなく、単に処理を放棄して cancel 経路へ抜ける。
  §B8 の `reason` を記録せず、circuit breaker にも計上しない（下記）。

## B8. fallback と失敗理由

いずれの場合も**候補列は NLL 未適用のまま返し**、`learning::Reranker` は通常どおり
走る。これが DEV-413 の「既存 `Reranker` のみへ縮退する経路」である。

| `reason` | 条件 | breaker 計上 |
|---|---|---|
| `disabled` | `nllRerankEnabled=false`（既定） | なし |
| `model_not_loaded` | Zenzai 未ロード / active converter が Zenzai でない（degraded 中を含む） | なし |
| `no_target` | 対象候補 0 件（no-op。失敗ではない） | なし |
| `secure_mode` | M46 secure 中（§B10） | なし |
| `budget_exceeded` | §B7 の予算超過 | **する** |
| （cancel） | ユーザー操作による中断。`reason` を記録しない（§B7） | なし |
| `infer_error` | decode 例外・logits 取得失敗・トークナイズ失敗 | **する** |
| `invalid_score` | 全対象候補が非有限 NLL（§B2.4） | なし（入力起因） |
| `circuit_open` | breaker 作動中 | なし |

- **例外を `QueryCandidates` の外へ出さない**。`ApplyRerankerOrRaw` と同じ流儀で
  握り潰し、Zenzai 由来の失敗として `model_runtime_error_` に
  `nll-scorer:<reason>` を記録する（汎用 `last_error_` には混ぜない。
  `docs/zenzai-inference-spec.md` §9.2.1 と整合）。
- **連続失敗の circuit breaker**: runtime 失敗（`budget_exceeded` / `infer_error`）が
  連続 `nllFailureThreshold`（既定 **3**）回に達したらセッション内で NllScorer を
  無効化し、以降は `circuit_open` で no-op にする。モデル再ロード / 設定変更 /
  Host 再起動で再有効化する。ユーザー設定 `nllRerankEnabled` とは独立した内部
  フラグである（Track A §7.2・M57 §5.4 と同じ runtime パターン）。
- `model_not_loaded` と `no_target` は正常な非適用であり、失敗として記録しない
  （degraded / 短い候補列で breaker が無意味に開くのを防ぐ）。
- **cancel を breaker に計上しない**。cancel の発生率はスコアラの健全性ではなく
  ユーザーの打鍵速度に相関するため、速く打つユーザーが連続 cancel しただけで
  breaker が開き、セッション中 NLL が黙って無効化されてしまう。予算超過
  （`budget_exceeded`）は計上する — 遅いマシンで慢性的に予算を超えるなら無効化が
  正しい応答であり、Track A が timeout を計上するのと同じ理由による。

## B9. 有効化フラグと既定 OFF 時の挙動不変

`settings/mvp-settings.schema.json` の `reranker` ブロックで次を定義する:

```json
{
  "reranker": {
    "nllRerankEnabled": false,
    "nllTopK": 8,
    "nllWeight": 0.15,
    "nllBudgetMs": 20,
    "nllFailureThreshold": 3
  }
}
```

- 既定は **OFF**（`nllRerankEnabled=false`）。
- **既定 OFF のとき、prefix decode を含め Zenzai への追加呼び出しを一切行わない。**
  候補列・score・`debug_info`・ログ出力・レイテンシは Track B 導入前と等価であること
  （回帰テストで固定する。§B11）。
- 範囲: `nllTopK` 1〜16、`nllWeight` 0.0〜1.0、`nllBudgetMs` 5〜60、
  `nllFailureThreshold` 1〜10。範囲外は clamp（Track A §8 と同じ扱い）。
- `SettingsStore` から `EngineConfig::nll` へ適用する。欠落・型不正は既定値を使う。
  専用 UI は設けず、設定ファイルで有効化する。
- 既定値（特に `nllWeight` / `nllTopK`）は M52 ベンチで校正する（§B12）。

## B10. プライバシー

- **M46 secure 中は NllScorer を実行しない**（`reason=secure_mode`）。
  `docs/privacy-and-secure-input-spec.md` §5 は secure 中の候補生成を
  「内蔵変換 + 既存辞書のみ」に限定しており、LLM を追加で走らせる本層はその契約に
  反する。ユーザー設定 OFF の `disabled` とは区別して記録し、プライバシー監査で
  識別できるようにする。判定の責務は M46 の
  `privacy-and-secure-input-spec.md` §5.1.1 に従い、本層の消費側で適用する。
- NllScorer は**学習データを書かない**。`LearningStore` にも Track A の露出トレース
  （Track A §6.1）にも寄与しない。読み取り専用の評価層である。
- ログに `reading` / `surface` を出さない。`debug_info` に載せるのは数値のみで、
  redaction 方針（`docs/observability-spec.md` §7.6）に抵触しない。
- 外部送信は無い（ローカルモデルのみ）。

## B11. テスト契約

**unit（決定的）**

NLL の集約・正規化（§B2.2）と合成（§B6.2）、対象集合の選別（§B5）は
**llama.cpp に依存しない純関数へ切り出す**。これらは `AZOOKEY_WITH_LLAMA_CPP=OFF`
のビルドでも、固定 logprob 表を入力とする mock フィクスチャで検証できる。
decode 経路そのもの（§B3）は llama 有りビルドの test に置く。

- `NLL_total` と `nll_per_char` の算出: 固定 logprob 表を与え、期待値と一致する。
- **off-by-one**（§B3.2）: 位置ごとに異なる固定 logits を与え、`t_1` の logprob が
  prefix 最終位置由来であることを検証する。1 ずれれば落ちるフィクスチャにする。
- EOS を含めない（§B2.1）: 候補トークン列に EOS が混入していないこと。
- 合成（§B6.2）: 最尤候補の bonus が 0、他が負、絶対値が `kNllSpan * nllWeight` を
  超えない。
- **prefix logits スナップショット**（§B3.1）: 候補を 2 件以上与え、候補 2 の
  `t_1` の logprob が prefix 由来であること（候補 1 の出力由来だと落ちる
  フィクスチャにする）。
- 選択集合外の保証（§B5.1）: 対象外ソース（`UserDictionary` / `Model` / `Llm`）と
  `nllTopK` からあふれた候補の **`score` と `debug_info` が不変**であること。
  併せて、それらの候補が補正によって**順位を下げない**こと（減点のみの帰結）。
  順位そのものの不変は検証しない（全体ソートを持つ以上成立しないため）。
- `live=true` のリクエストで NllScorer が走らない（§B1）。
- fallback: §B8 の各 `reason` で、候補列が NLL 未適用の結果と完全一致する。
- all-or-nothing（§B7）: 途中で予算超過させたとき、部分的な減点が残らない。
- circuit breaker: 連続 3 失敗で無効化され、モデル再ロードで復帰する。
- **既定 OFF の回帰ゲート**: `nllRerankEnabled=false` で候補列が Track B 導入前と
  完全一致する（§B9）。

**integration（real model。pin モデルを要する `gate:human-required` 相当）**

- DEV-743 で締めた流儀に合わせ、合格条件は代表入力の **top candidate 完全一致**
  とする（「含む」では壊れた出力が通過するため）。
- 同音異義の代表ケース（左文脈あり / なし）で、NLL 適用前後の 1 位が期待どおりに
  入れ替わること、および対象外ソースの score・debug_info が不変で順位が下がらないこと。

**bench**

- `bench/` に `K = nllTopK` での追加レイテンシを計測する case を足し、§B7 の予算内に
  収まることを示す。生成と共有したあとの prefix コスト（§B3.4）と、その共有で
  decode せずに済んだ prompt トークン数を内訳として分離する。
- `azookey_nll_bench --model <GGUF> --iterations 10 --top-k 8 --threads 8` は
  固定の「こうせい」候補集合で生成後の追加評価を測る。warm-up は 2 回とし、
  prefix 平均と p95、prefix 再利用トークン数の最小値、全評価 p95、20ms 超過件数と
  割合、各反復の内訳を出す。
  生成前（prefix を全 decode）と生成後（prefix を共有）で NLL スコアが一致すること、
  NLL 評価の前後での生成候補順序と件数の一致も検証する。独立にロードしたモデルの
  cold 生成を基準に、warm 生成と cold NLL 評価後の生成が同じ候補順序・件数・スコアを
  返すことを必須とする。生成スコアの最大絶対差は 0 とし、NLL 評価後や再利用後の
  反復でも検証して `generation_max_score_delta` に記録する。
  `--context` と `--expect-top` で文脈と最上位候補の完全一致を指定できる。
  batch 評価は、計測区間外で各候補の単独評価と、評価順序を逆転して元の候補位置へ
  戻した評価と比較する。全経路で件数とスコアの有限性を検査し、CPU は絶対誤差
  `1e-5` 以下、Vulkan は最上位を含む全候補順位の一致を `batch_invariant` として検証する。
  Vulkan の異なる表層の同点は元の候補位置順で扱い、同じ表層同士の入れ替えは許容する。
  16 候補、80 token の長い候補、短長混在候補にも同じ検査を適用し、
  `extended_batch_invariant` と比較方式 `batch_comparison`、最大絶対差
  `batch_max_score_delta` を出力する。この比較方式は prefix 共有の絶対誤差 `1e-5`
  以下の検査や、生成候補の同等性契約を変更しない。
  cancel、期限切れ、空の候補による失敗後は、次の評価で prefix を再利用せず、cold 評価と
  同じスコアへ絶対誤差 `1e-5` 以下で復帰することを計測区間外で確認し、
  `cache_invalidation` として出力する。
  計測は 10 秒の診断用 deadline を使い、通常要求の 20ms 予算とは区別する。
- `--backend cpu|vulkan` で実行経路を指定する。Vulkan は対応ビルドとデバイスを必要とし、
  CPU への暗黙の代替を測定成功としない。
- `azookey_nll_bench --self-test` はモデルを使わず、比較器が最上位・下位の行入れ替え、
  非有限値、件数不一致を拒否し、CPU の絶対誤差、同点、重複表層を扱うことを検査する。
- `--query-dictionary <azdic>` は合成の TechnicalTerms 辞書を読み込み、NLL OFF と ON の
  `QueryCandidates` をそれぞれ 2 回の OFF warm-up 後に連続計測する。
  `bench/data/nll_fixture.lex.tsv` と同名の metadata JSON が固定入力である。
  各要求の時間、prompt 再利用トークン数、予算超過、返却候補への適用数を記録する。
  ON の途中で circuit が開いてもリセットせず、NLL が中断または失敗した直後の
  prefix 再計算も測定に含める。
  全件破棄は候補統合による生成スコア差と混同せず、`RerankNll` の入力境界で検証する。

## B12. 校正と性能検証

- prefix を生成と共有した NllScorer の評価が §B7 の予算（既定 20ms）に収まるか。
  共有は §B3.4 のとおり実装内部で完結し、本章の契約は変えない。
- 測定は固定モデル・入力・threads・warmup・反復を揃え、CPU と、専有条件を確保できる
  GPU で行う。GPU では測定中の利用率と電力状態も記録する。専有・アイドル条件を
  確保できない測定から GPU の優劣は結論しない。
- `nllWeight` / `nllTopK` / `nllBudgetMs` の既定値校正。M52 ベンチ
  （`docs/conversion-quality-benchmark-spec.md`）が整備された時点で実測に置き換える。
- 構造化ログのフィールドは §B4.2、ログ形式の正典は
  `docs/observability-spec.md` §7 に従う。
- CPU / Vulkan の Release 測定の条件と結果は
  [`decisions/2026-09-08-nll-release-measurement.md`](decisions/2026-09-08-nll-release-measurement.md) にある。
