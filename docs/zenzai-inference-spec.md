# Zenzai 推論コントラクト仕様

対象リポジトリ: dolquis/azooKey-Desktop
対応マイルストーン: M8（Phase 3「実 Zenzai と辞書 UI」）
関連: `plans/windows-port-roadmap.md` M8 / M9 / M10、
      `docs/zenzai-gpu-route.md`（GPU 化ルート・バックエンド経路）、
      `docs/copilot-pc-backend-spec.md` §4（推論エンジン × アクセラレータ選定）、
      `docs/neural-reranker-spec.md`（M56 最終 score 統合）、
      `docs/model-management-spec.md`（M45 モデル配置）
作成日: 2026-06-15
位置づけ: M8 Zenzai 実推論（DEV-219、特に M8-2 = DEV-221）の **設計前提**。
          「どの実行系か」（backend 方針 = DEV-98、実証 = DEV-194）に対し、
          本書は「**どう推論して候補を作るか**」（推論コントラクト）を確定する。

> **正典範囲**: 本書は Zenzai 推論の入出力コントラクト（プロンプト整形・制約・
> トークナイズ・n-best・多ソース統合・性能予算）の正典。バックエンド経路は
> `docs/zenzai-gpu-route.md` / `docs/copilot-pc-backend-spec.md` が正典であり、
> 本書はそれらを前提とする。状態・進捗は Linear（DEV-228 ほか）が正典。

---

## 1. 目的とスコープ

### 1.1 解決する空白

実機検証（DEV-32）の **A5**: メモ帳で「にほんご」+Space すると候補窓は出るが
漢字「日本語」が出ず、かな「にほんご」のみが返る。直接原因は
`inference-host/src/ZenzaiModelConverter.cpp` の `Convert` が `fallback_`
（`SimpleConverter`）へ素通しで、**実推論が一切無い**こと
（現状コードは候補に `zenzai-gguf-loaded;fallback-converter` を付与するのみ）。

backend 方針（DEV-98）は「llama.cpp C-API + CPU」を確定し、DEV-194 はその実機性能と
Windows ML 用 model への変換可否を検証する。
その上で **GGUF をどう叩いて候補列を作るか** を定義した spec が存在しない。
本書がそれを埋める。

### 1.2 本書が確定する 6 項目

| # | 項目 | 章 |
|---|---|---|
| 1 | プロンプト整形（zenz-v3 入力フォーマット） | §3 |
| 2 | 制約デコード（読み一貫性の担保） | §4 |
| 3 | トークナイズ（かな↔サブワード・未知語・記号・英数） | §5 |
| 4 | n-best 抽出（探索方針・候補数・打ち切り・重複除去・logprob） | §6 |
| 5 | 多ソース候補統合（Zenzai × user_dict × reranker のマージ・正規化・順位） | §7 |
| 6 | 性能予算（レイテンシ目標・トークン上限・bench 整合） | §8 |

### 1.3 スコープ外（参照のみ）

- バックエンド実行系（CPU/CUDA/WinML）の選定 → `docs/copilot-pc-backend-spec.md`。
- llama.cpp の CMake 導入・リンク・GGUF ロード境界 → M8-1（DEV-220）。
- 個人化 n-gram LM（marisa）のブレンド → §9.4 で**将来拡張**として枠のみ定義。
  v1.0 / M8 では採用しない（legacy macOS の `PersonalizationMode` は参照のみ）。
- 本格的な辞書ラティス（M53）・Tiny Reranker（M56）・ModernBERT（M57）→ §7.5 で
  将来統合点のみ示す。

---

## 2. 上流 Zenzai（legacy macOS）の事実関係（参照のみ）

Windows 版の仕様判断は §3 以降で別途行う。本章は **根拠**として upstream の
事実を記録する（`legacy/` は保全資産であり、そのまま正解として扱わない —
`AGENTS.md`「現行対象と legacy の扱い」）。

### 2.1 呼び出し方（legacy）

- `legacy/Core/Sources/Core/InputUtils/SegmentsManager.swift` が
  `kanaKanjiConverter.requestCandidates(composingText, options:)` を呼ぶ。
  Zenzai は `ConvertRequestOptions.ZenzaiMode.on(...)` で有効化される。
- **重要**: legacy はプロンプト整形・制約デコード・n-best 抽出を**自前で行わない**。
  すべて Swift 依存ライブラリ `AzooKeyKanaKanjiConverter`（内部で llama.cpp + zenz を
  ラップ）の中で行われる。**Windows 版は llama.cpp C-API を直結するため、この内部
  ロジックを再実装する**必要がある — これが本書の存在理由。

### 2.2 legacy の `ZenzaiMode.on` フィールド（参照）

| フィールド | legacy 値 | Windows 版の扱い（本書） |
|---|---|---|
| `weight` | `ggml-model-Q5_K_M.gguf`（zenz-v3.x-small） | §3.1 モデル。`EngineConfig.model_path` |
| `inferenceLimit` | 既定 5 / 範囲 1〜50（`Config.ZenzaiInferenceLimit`） | §6.3 にマップ（探索予算） |
| `requestRichCandidates` | bool（候補選択時 true） | §6.1 n-best 要求フラグ |
| `personalizationMode` | base/personal n-gram + alpha | §9.4 将来拡張（M8 非対象） |
| `versionDependentMode` | `.v3(profile, leftSideContext, enableAlignmentSeparator: true)` | §3.2 プロンプト構築 |

- `leftSideContext`: 確定済み左文脈を**最大 30 文字**、改行で分割し最後の行、
  先頭空白を除去して渡す（`getCleanLeftSideContext(maxCount: 30)`）。
- `profile`: ユーザー任意文字列（例「田中太郎/高校生」、既定 空文字）。

### 2.3 モデルの素性（外部・要実装時再確認）

- アーキテクチャ: **GPT-2 系の条件付き言語モデル**。トークナイザは
  **character-level + byte-level BPE**。出典: AzooKeyKanaKanjiConverter
  `Docs/zenzai.md`、zenz モデルカード（Hugging Face `Miwa-Keita/zenz-*`）。
- メモリ: small モデルで概ね 150MB 級。
- 配布形式: GGUF（`docs/zenzai-gpu-route.md` と整合）。
- v1.0 採用モデル: 上流 `Miwa-Keita/zenz-v3.2-small-gguf`（roadmap M8 受け入れ条件の
  配置対象。v1.0 配布での取得・配置の正は `sideload-packaging-spec.md` §1.6.1）。
  プロンプトは **v3 系フォーマット**（§3）を用いる。

> ⚠️ **実装時検証点（DEV-221）**: 特殊トークンのコードポイント・BOS/EOS の
> 有無・トークナイザの正規化は、**配置する GGUF の `tokenizer` メタデータと
> モデルカードで必ず突き合わせる**こと。本書 §3/§5 は upstream ドキュメント
> 由来の既定値であり、モデル実体が優先する。

---

## 3. プロンプト整形（スコープ #1）

### 3.1 入力の前処理

`InferenceEngine::QueryCandidates(kana, context, ...)` から
`ZenzaiModelConverter::Convert(kana, ConversionContext)` へ渡る値:

- `kana`: 変換対象の読み。preedit 由来で **ひらがな**が標準。
- `ConversionContext.preceding_text`: 左文脈（確定済みテキスト）。
- `ConversionContext.preedit_text`: `kana` と同一（`BuildContext` 参照）。

zenz は **入力カタカナ**（`input_katakana`）を期待する。よって Convert 入口で:

1. **読みをカタカナへ正規化**: ひらがな U+3041–U+3096 を +0x60 してカタカナ化。
   長音「ー」、小書き、々等はそのまま。既存の記号・英数（半角/全角）は保持。
2. **左文脈の整形**: `preceding_text` を**末尾 30 文字**に切り詰め、改行で分割して
   最後の行を採用、先頭空白を除去（legacy `getCleanLeftSideContext` と同義）。
   空なら context フィールドを省く。

> 正規化はかな表記のみ対象。漢字交じりの preceding_text は文脈としてそのまま渡す。

### 3.2 zenz-v3 プロンプトフォーマット

特殊トークン（制御コードポイント、upstream `Docs/zenzai.md` 由来）。本書では
**実体の不可視 PUA 文字を直接書かず**、下表の可読ショートハンド（`[CTX]` 等）で表記する。
実装は対応コードポイントを使うこと。

| ショートハンド | コードポイント | 意味 |
|---|---|---|
| `[CTX]` | U+EE02 | 左文脈 `<context>` の前置 |
| `[IN]` | U+EE00 | `<input_katakana>` の開始 |
| `[OUT]` | U+EE01 | `<output>`（生成開始位置） |
| `[PROF]` | U+EE03 | `<profile>`（任意） |
| `[TOPIC]`/`[STYLE]`/`[SET]` | U+EE04 / U+EE05 / U+EE06 | 実験的（M8 非使用） |
| `[RCTX]` | U+EE07 | v3.2 の `<right_context>`（M8 非使用） |
| `[EOS]` | モデル EOS（`</s>` 相当） | 生成停止トークン。GGUF の `tokenizer.ggml.eos_token_id` が `</s>` を指していない場合の扱いは §9.2 |

**v3 フォーマット**（採用、`[PROF]` / `[CTX]` は任意）:

```
[PROF]<profile>[CTX]<context>[IN]<input_katakana>[OUT]<output>[EOS]
```

- 文脈・プロフィールが無い場合は対応セグメント（トークン + 値）ごと省略。最小形は
  `[IN]<input_katakana>[OUT]` を**プロンプトとして与え**、`[OUT]` 以降を生成。
- プロフィール使用時（将来。M8 既定は空）は `[PROF]<profile>` を文脈の前に置く
  （正確な配置順は実装時にモデルカードで確認 — §2.3 検証点）。
- 生成は `[OUT]` 直後から始め、`[EOS]` or トークン上限（§8）で停止。

**例**（`kana="にほんご"`, 左文脈 `"私は"`）:

```
プロンプト: [CTX]私は[IN]ニホンゴ[OUT]
期待生成  : 日本語[EOS]
```

**例**（最小・文脈なし、A5 再現ケース `kana="にほんご"`）:

```
プロンプト: [IN]ニホンゴ[OUT]
期待生成  : 日本語[EOS]
```

### 3.3 プロンプト構築の擬似コード

`TOK(x)` はショートハンド `x` のコードポイント（§3.2 表）を表す。

```cpp
std::string BuildZenzaiPrompt(const std::string& kana_input,
                              const std::string& preceding_text,
                              const std::string& profile /* 既定 "" */) {
  const std::string katakana = ToKatakana(kana_input);          // §3.1-1
  const std::string ctx = CleanLeftContext(preceding_text, 30); // §3.1-2
  std::string p;
  if (!profile.empty()) p += TOK("[PROF]") + profile;  // 将来（M8 既定は空）
  if (!ctx.empty())     p += TOK("[CTX]")  + ctx;
  p += TOK("[IN]") + katakana + TOK("[OUT]");
  return p;  // 生成は [OUT] の直後から
}
```

---

## 4. 制約デコード（スコープ #2）

### 4.1 要件

生成された表層 `<output>` の**読みが入力カタカナに一致**しなければならない
（「にほんご」→「日本語」は可、「にほんご」→「二本語」は読み不一致で不可）。
upstream Zenzai は `enableAlignmentSeparator: true` の v3 モードと
**lexically constrained beam search**（辞書ラティスを許容トークン源とする読み制約）で
これを担保する。

### 4.2 Windows 版の段階設計

Windows 版 MVP（M8）の時点では**本格辞書ラティスが無い**（`SimpleConverter` は
固定小テーブル、辞書層全体は M53）。よって 2 段で定義する。

#### 4.2.1 M8 MVP（DEV-221 が実装）— 生成 + 健全性チェック + 劣化フォールバック

- zenz は読み一貫の変換を行うよう**学習済み**。MVP では自由生成（greedy / 小ビーム）に
  zenz の学習を信頼し、**事後健全性チェック**のみを課す:
  - 生成が**非空**であること。
  - `[EOS]` に到達、またはトークン上限（§8）で打ち切り後に成立する**閉じた**表層。
  - 生成が入力カタカナをそのまま返すだけ（変換ゼロ）でも**有効な候補**として扱う
    （かな候補は SimpleConverter 帯でも出るため重複は §6.5 で除去）。
- **厳密な読み等価検証（surface→yomi 突合）は M8 では行わない**。理由: 漢字→読みの
  逆引きには辞書（M53）が要る。MVP は zenz の学習に委ね、破綻時はフォールバック。
- 破綻（例外 / 空生成 / 文字化け / **best-so-far の無い**タイムアウト）時は `SimpleConverter`
  へ劣化（§7.4）。例外は握り潰さず `last_error_` / log に残す（DEV-177 と同方針）。
- **deadline 超過でも best-so-far beam があれば**それを返す（§6.4）＝正常出力であり劣化扱い
  しない（fallback も `degraded` もしない。§9.2.2 と整合）。

#### 4.2.2 目標形（将来 M53 以降）— 辞書ラティス制約付き協調デコード

- 辞書（M53）が `読みセグメント → 表層候補` のラティスを供給し、zenz はラティス上の
  経路を logprob でスコア/枝刈り。読み一貫性は**構成上保証**、未知語・記号は辞書の
  identity エントリで吸収。`inferenceLimit` を探索予算（LLM 評価回数）に対応させる。
- 本形への移行時に §6（n-best）と §7（統合）の契約は不変に保つ（score 帯・debug_info
  形式・source を変えない）ことで、DEV-221 実装を壊さず差し替え可能とする。

> 設計判断（Windows 版）: M8 は**生成 + 事後チェック**に限定し、辞書ラティス制約は
> M53 完了後に重ねる。これにより A5 解消（「日本語」が出る）を最短で達成しつつ、
> 将来の厳密制約への移行口を確保する。

---

## 5. トークナイズ（スコープ #3）

- zenz のトークナイザ（character + byte-level BPE）を**そのまま使用**する。llama.cpp の
  `llama_tokenize` / `llama_token_to_piece` を用い、独自トークナイズは行わない。
- **特殊トークン**（`[CTX]`/`[IN]`/`[OUT]`/`[PROF]` … = U+EE0x、`[EOS]`）は、GGUF の
  `tokenizer` メタデータに added/special token として登録されていれば `special=true` で
  トークナイズする。登録が無い場合は UTF-8 バイト列として BPE に通る
  （character/byte-level のためラウンドトリップは保たれる）。**いずれか実装時に確認**
  （§2.3 検証点）。
- **未知語・記号・英数**: byte-level BPE のため未知バイトは生じない。入力カタカナに
  含まれる記号・数字・空白・英字はそのまま渡す（zenz-v2.5 データセットの `input` も
  記号・数字・空白を含む前提）。読みのカタカナ化（§3.1-1）は**かな範囲のみ**に適用し、
  英数記号は変換しない。
- **BOS**: GGUF メタデータの `add_bos_token` に従う。プロンプトは §3.2 の制御トークンで
  境界を表すため、BOS の二重付与に注意（`llama_tokenize(..., add_special=...)` の扱いを
  モデルメタデータと一致させる）。

---

## 6. n-best 抽出（スコープ #4）

### 6.1 要求モードと候補数

- 既定（ライブ変換・1 候補要求）: **top-1** を greedy で取得（最小レイテンシ）。
- 候補ウィンドウ表示時（legacy `requestRichCandidates: true` 相当）: **n-best** を取得。
  IPC/TIP は `QueryCandidates` で常に複数候補を期待するため、本書は **n-best を既定経路**
  とし、ライブ変換中の 1 候補要求は n-best の先頭を使う実装でよい。
- 出力候補数の上限 `N_zenzai`: **既定 4**（後続 reranker 入力上限 30 と整合、
  `neural-reranker-spec` §7）。設定で 1〜8 に可変（§9.3）。

### 6.2 探索方針（M8 MVP）

- **ビームサーチ**（beam width = `B`、既定 4）を採用。サンプリング（temperature>0）は
  IME の決定性を損なうため**既定では使わない**（再現性・テスト容易性を優先）。
- 各ビームは系列 logprob（トークン logprob の総和）を保持。`[EOS]` / トークン上限で確定。
- top-`N_zenzai` の確定系列を候補化する。

### 6.3 inferenceLimit のマッピング

- legacy の `inferenceLimit`（既定 5 / 範囲 1〜50）は upstream では「LLM 推論ループ回数
  （探索予算）」。MVP（単純ビーム）では **ビーム幅 `B` と n-best 抽出数の上限**に対応
  させる: `B = clamp(inferenceLimit, 1, 8)`、既定 4。
- §4.2.2 の辞書ラティス制約へ移行した時点で、本パラメータを**探索ループ回数**の本来
  意味へ戻す（契約 §6.5 の出力形は不変）。

### 6.4 打ち切り

- トークン上限 `max_new_tokens`（§8）到達で強制停止。打ち切り系列も、閉じた表層なら
  候補化可（健全性チェック §4.2.1）。
- 1 変換の総予算（§8 のレイテンシ予算）超過時は、その時点の最良ビームのみ返す。

### 6.5 重複除去と logprob 正規化

- **重複除去（converter 内）**: `ZenzaiModelConverter` は自身の n-best を表層 `surface`
  完全一致で dedup（先に出た高 logprob を残す）。
- **クロスソース重複除去**: user_dict / SimpleConverter 由来の同一表層との重複は、
  converter 内 dedup では消えない（別ソースのため）。`QueryCandidates` のマージ経路で
  dedup する（**§7.6**）。
- **logprob 正規化**（候補 score への写像）: 系列長で割った**平均トークン logprob**の
  指数を取り、加算スケールへ写す:

  ```
  avg_lp   = total_logprob / num_output_tokens     // ≤ 0
  prob_geo = exp(avg_lp)                            // (0, 1]  幾何平均トークン確率
  score    = Z_FLOOR + (Z_CEIL - Z_FLOOR) * prob_geo
  ```

  既定 `Z_FLOOR = 0.3`, `Z_CEIL = 1.4`。
  - 単調性: logprob 大 → score 大（n-best 内順位を保存）。
  - 帯設計: Zenzai score ∈ [0.3, 1.4]。**user_dict 既定 1.5 より下**に収め（§7.3）、
    SimpleConverter の identity（0.6）より上に best を置く。
  - **生の logprob は `debug_info` に保持**（`zenzai;lp=-1.82;avg=-0.30`）。M56 Tiny
    Reranker は raw `zenzai_score`（例 -1.82、`neural-reranker-spec` §4）を後段で使う。

---

## 7. 多ソース候補統合（スコープ #5）

### 7.1 現行の合成順（`InferenceEngine::QueryCandidates`）

統合は **converter ではなく `QueryCandidates` 側**で行われる（現行コード）:

```
1. user_dict_->Lookup(kana)   → score = value or user_word_default_score(1.5), debug="user-dict"〔source は §7.2 上 UserDictionary だが現状コード未設定。下記注〕
2. active_converter_->Convert(kana, ctx)  → Zenzai or SimpleConverter の候補を末尾連結
3. Reranker::Apply → 各候補 score += LearningStore::Score(reading, surface, now)  → score 降順 stable_sort
```

> ⚠️ **現状コードの不整合（DEV-221 が修正）**: 手順 1 の user_dict 候補は現行
> `QueryCandidates` で `c.surface`/`reading`/`score`/`debug_info` のみ設定し、**`c.source` を
> 設定していない**ため既定 `CandidateSource::Heuristic` のまま（`core/Candidate.h`）。§7.2 の
> source 帯・source 依存のマージ/dedup（§7.6）を正しく効かせるには、マージ経路で
> **`c.source = core::CandidateSource::UserDictionary` を明示設定する**（1 行修正、本契約に含む）。

`ZenzaiModelConverter::Convert` は **手順 2 の候補列を返すだけ**であり、user_dict や
reranker を意識しない。本書の Zenzai 契約（§6.5 の score / source / debug_info）が
この合成にそのまま乗る。

### 7.2 各ソースの score 帯（正規化の正典）

| ソース | source enum | score 帯 | 備考 |
|---|---|---|---|
| user_dict（読み完全一致） | `UserDictionary` | 既定 **1.5**（`user_word_default_score`）/ entry value | 最優先帯 |
| Zenzai | **`Model`** | **[0.3, 1.4]**（§6.5） | local model 変換。`Llm` は M16 クラウド用に予約 |
| SimpleConverter（辞書/heuristic） | `SystemDictionary` / `Heuristic` | 0.1〜1.2 | fallback / 劣化時 |
| 学習加点（reranker） | （加算） | weight×decay（≈0.8/確定、半減期≈4.6日） | 全ソースに加算 |

> **設計判断**: Zenzai を `CandidateSource::Model` とし、`CandidateSource::Llm` は
> M16「Magic Conversion」（OpenAI 等クラウド LLM）に予約する。両者は性質
> （local 変換 vs クラウド整文）が異なるため分離する。

### 7.3 順位規則

- 最終順位は `score + 学習加点` の**降順 stable_sort**（現行 reranker のまま）。
- **user_dict 最優先**（roadmap M9）は **既定 value（無指定＝1.5）** の場合、Zenzai 帯上限
  （1.4）より上に収まるため**スコア単独キーのまま自然に担保**される（合成順・reranker の
  ソートキーは変更不要）。
- **明示 value の扱い**: user_dict の score は `w.value.value_or(1.5)`。user が**明示的に低い/
  負の value** を設定した場合（`AddUserWord` 任意値）はその値で順位付けされる＝user の意図的な
  格下げを尊重する（M9「最優先」は既定登録語が対象）。ただし**重複表層の dedup** では value に
  関わらず user_dict を保持する（§7.6 のソース優先）。
- 例外: ユーザーが Zenzai 候補を繰り返し確定すると学習加点でその候補が 1.5 を超え
  user_dict 既定を上回り得る — これは**学習された選好の反映**として正しい挙動。
- 同点時は stable_sort により**挿入順**（user_dict → Zenzai → Simple）が保たれる。

### 7.4 劣化モード（degraded）の順位

- Zenzai 推論が例外 / 空生成 / **best-so-far beam の無い**タイムアウトのとき、
  `ZenzaiModelConverter::Convert` は `fallback_->Convert`（SimpleConverter）の結果を返す
  （現行委譲を維持）。この時の候補 source は SimpleConverter 由来（`SystemDictionary`/
  `Heuristic`、0.1〜1.2 帯）。
- **deadline 超過でも best-so-far beam があれば** Zenzai のその beam を返す（§6.4/§9.2.2）＝
  正常出力であり degraded 扱いしない（fallback も `model_runtime_error_` もセットしない）。
- `debug_info` に `zenzai-degraded;<理由>` を付与。degraded を `/Health` に出すための
  converter→engine→Health 吸い上げ機構は **§9.2.1**（converter 自前 `last_error_` を engine が
  同ロック内で**専用フィールド `model_runtime_error_` にミラー**し `effective_last_error()`
  経由で Health に反映。汎用 `last_error_`〔load/learning〕には**混ぜない**ので成功変換で確実に
  クリアできる）。
- user_dict は degraded でも手順 1 でそのまま最優先帯に残る（Host は落ちない）。

### 7.5 将来の score 統合（参照）

M52 ベンチ後、`neural-reranker-spec` §9 の `final_score`
（`normalize(zenzai_score)*0.35 + …`）へ移行する。その際 Zenzai は**生 logprob**を
供給する（§6.5 で `debug_info` に保持済み）。本書の [0.3,1.4] 写像は**それまでの
暫定加算統合**であり、M56 で学習重みへ置換される。`Candidate` 構造体に raw score 用の
数値フィールドを足すかは M56 で判断（M8 では `debug_info` 文字列で前方互換）。

### 7.6 クロスソース重複除去（QueryCandidates マージ経路）

§7.1 の合成では user_dict と converter（Zenzai/Simple）が**独立に同一表層を出し得る**
（例: user_dict「日本語」と Zenzai「日本語」）。現行 `QueryCandidates` は両者を連結し
reranker でソートするのみで**クロスソース dedup をしない**ため、UI に重複候補が出る。
これを防ぐため**マージ経路に表層 dedup を追加する**（M8 統合 = DEV-221 スコープの小改修）。

規則:

- キーは表層 `surface`（同一読み `kana` 内での合成のため読みは共通）。
- **ソース優先で残す**: 重複集合に user_dict（`source==UserDictionary`）候補があれば
  **score の高低に関わらず user_dict を残す**（user が登録した表層はその値ごと user の
  エントリで代表させる）。`w.value` が明示的に低い/負（`AddUserWord` は任意値を受理、既存
  テストは -3.0 を送る）でも user_dict を保持する — §7.2 の帯依存（1.5>1.4）では保証
  できないため **source 優先で担保**する。
- user_dict を含まない重複（Zenzai × Simple 等）は**最高 score の 1 件**を残す。残す側の
  `debug_info` に脱落側 source を併記（例 `user-dict;dup:zenzai`）して証跡を保つ。
- **survivor 選択はソース優先を一次キーとする**（score-based first-wins より前）: 表層グループ
  ごとに ①user_dict があれば user_dict、②無ければ最高 score、を survivor に選び他を除去する。
  最終順位（§7.3 降順 stable_sort）は survivor 確定後の集合に適用する。**sort 後の単純 first-wins は
  使わない**（低 value user_dict が高 score Zenzai に落とされ source 優先と矛盾するため、survivor
  選択を sort と独立に行う）。

---

## 8. 性能予算（スコープ #6）

| 指標 | 目標 | 根拠 / 計測 |
|---|---|---|
| CPU fallback（SimpleConverter）p95 | < 50ms | 既存ゲート（`zenzai-gpu-route` §計測ゲート、roadmap §テスト） |
| Zenzai CPU 1 変換 p50 | ≤ 150ms（small/Q5_K_M, 短文） | M8-3（DEV-222）で実測・更新 |
| Zenzai CPU 1 変換 p95 | ≤ 300ms（short reading ≤ 8 文字） | 同上。超過時は §6.4 打ち切り |
| `max_new_tokens` | reading 文字数 × 2 + 8（既定上限 64） | 表層は読みより長くならない前提 + 余白 |
| 初回ロード時間 | 計測のみ（ゲート無し、M8-3 で baseline） | mmap ロード（`copilot-pc-backend-spec` §5） |

- レイテンシ予算超過時は §6.4 の通り最良ビームを返し、IME を止めない。
- 上記の Zenzai 実測値は **M8-3（DEV-222）が `bench/` で計測し本表を更新**する。
  本書記載は設計時の目標であり、計測結果が正典化されたら置換する。

---

## 9. 実装契約（DEV-221 が即着手できる粒度）

### 9.1 `ZenzaiModelConverter::Convert` の擬似コード

```cpp
std::vector<core::Candidate> ZenzaiModelConverter::Convert(
    const std::string& kana, const core::ConversionContext& ctx) {
  if (!model_ready_) return DegradeToFallback(kana, ctx, "model-not-ready");

  try {
    const std::string prompt = BuildZenzaiPrompt(kana, ctx.preceding_text, profile_); // §3.3
    const auto beams = BeamSearch(prompt, /*B=*/beam_width_, /*max_new=*/MaxNewTokens(kana),
                                  deadline_, cancel_);  // §4.2.1 §6.2 §8 ＋ §9.2.2（decode 中も cancel/deadline をポーリング）
    if (Canceled(cancel_)) return {};                 // §9.2.2 キャンセルは fallback せず空（M10。stale 候補を出さない）
    if (beams.empty()) return DegradeToFallback(kana, ctx, "empty-generation");

    std::vector<core::Candidate> out;
    for (const auto& b : TopN(beams, n_best_)) {        // §6.1
      if (!IsSane(b.text)) continue;                    // §4.2.1 健全性
      core::Candidate c;
      c.surface    = b.text;
      c.reading    = kana;                              // 入力読みを保持
      c.score      = NormalizeLogprob(b.total_logprob, b.num_tokens); // §6.5
      c.source     = core::CandidateSource::Model;      // §7.2
      c.debug_info = "zenzai;lp=" + fmt(b.total_logprob) + ";avg=" + fmt(b.avg_lp);
      out.push_back(std::move(c));
    }
    DedupBySurface(out);                                // §6.5
    if (out.empty()) return DegradeToFallback(kana, ctx, "no-sane-candidate");
    return out;
  } catch (const std::exception& e) {
    return DegradeToFallback(kana, ctx, std::string("exception:") + e.what()); // §7.4
  }
}
```

- `DegradeToFallback` は `fallback_->Convert` の結果に `zenzai-degraded;<reason>` を付与し、
  **converter 自身の** `last_error_` / `degraded_` を設定する（**engine の private
  `last_error_` を直接触らない** — §9.2.1 で engine が吸い上げる）。例外は**握り潰さない**。
- `PredictNext` / `Correct` は当面 fallback 委譲のまま（zenz 対応要否は将来 spec 判断、
  DEV-221 は `Convert` を優先 — DEV-221 スコープと一致）。

### 9.2 ライフサイクルと所有権

- GGUF の `tokenizer.ggml.pre` が `gpt2-small-japanese-char` の場合、モデルロード時だけ
  llama.cpp の KV override で `gpt-2` に置き換え、upstream llama.cpp の既知
  pre-tokenizer 名としてロードする。その他の pre-tokenizer には override を適用しない。
  この置換が想定するトークン化との同一性は、`ku-nlp/gpt2-small-japanese-char` revision
  `f1623eb5e26cee239d8fc5a661c48482811b3dbb` の `AutoTokenizer`（`add_special_tokens=false`）を
  参照実装として確認する。文脈なしの `[IN]ニホンゴ[OUT]` は
  `172,120,202,444,677,259,796,172,120,203`、`[IN]ワタシハガクセイデス[OUT]` は
  `172,120,202,628,327,330,623,643,299,492,280,406,271,172,120,203` と一致しなければならない。
  real-model smoke と DEV-225 の実機ゲートは、この token ID 列を差分確認する。
- GGUF が宣言する `tokenizer.ggml.eos_token_id` が、語彙上その id に対応する piece から見て
  終端トークンでない場合（開始トークン `<s>` を指している等）、モデルロード時だけ KV override で
  語彙中の終端トークン（`</s>`）の id へ置き換える。**id を定数で書かず語彙から解決する**。
  eos が正しく宣言されている GGUF には override を適用しない。
- 上記 2 つの override をどう立てるかの判断は、llama.cpp の型に依存しない純ロジック
  （`BuildZenzaiKvOverrides`）に置き、GGUF メタデータから override 一覧（キー・型・値）を
  決める。llama.cpp 境界側はその一覧を `llama_model_kv_override` 配列へ写すだけとし、
  配列末尾には llama.cpp が終端とみなす空キーのエントリを必ず残す。

  この分離で llama.cpp 無効ビルドの unit が検証できるのは、**override 記述子
  （どのキーに・どの型で・どの値を立てるか）を決める判断まで**である。記述子を
  `llama_model_kv_override` の `tag` / `val_str` / `val_i64` へ写す境界変換そのものは
  llama.cpp 有効ブロック内に残るため unit の対象外で、**実モデル smoke
  （`zenzai-real-model`）が担保する**。pin モデルの pre-tokenizer は
  `gpt2-small-japanese-char`・eos は誤宣言済みで両 override が実際に立つため、
  境界変換が誤った tag / 値を書けば smoke の変換出力が一致しなくなる。
  unit の合格だけを DEV-441 の受け入れ証拠として扱わない。

  pin モデル `Miwa-Keita/zenz-v3.2-small-gguf` は `eos_token_id = 2` を宣言するが、id 2 の
  piece は `<s>`、モデルが実際に出す終端は id 3 の `</s>` である（bos / eos が語彙文字列に
  対して 1 つずれており、本来は `bos=2` / `eos=3` / `pad=1`）。この状態では
  `llama_vocab_is_eog` が実際の終端で真にならず、生成が §8 のトークン上限まで走って
  終端後のゴミを surface へ積む。`にほんご` が `日本語日本語日本語` になる、
  `わたしはがくせいです` が未変換のまま返る、といった症状はいずれもこれに由来する
  （DEV-743 で原因確定、DEV-753 で実装）。
- `bos_token_id` も同じずれを持つが、pin モデルは `tokenizer.ggml.add_bos_token = false` の
  ため生成経路に影響しない。bos を override する場合はプロンプト契約（§3.2）への影響を
  別途評価する。
- llama.cpp の `llama_model` / `llama_context` ハンドルは M8-1（DEV-220）が保持。
  本 converter はそれを**非所有参照**で受け取り、推論ごとに `llama_decode` を回す。
- C 文字列の所有権・解放規約に注意（`docs/zenzai-gpu-route.md` の先行実装
  「`strdup` 戻り値未解放＝リーク」反面教師。C-API 直結のため FFI 文字列リークは無いが、
  `llama_token_to_piece` バッファの寿命管理を明示する）。
- CPU の llama.cpp 経路は、変換ごとにプロンプトを 1 回だけ decode する。
  各 beam の評価前に sequence 0 のプロンプト以降を削除し、beam 固有の生成 token だけを
  decode する。KV キャッシュ全体の clear は次の変換のプロンプト開始時に限る。
- `azookey_zenzai_bench` は、プロンプトと beam の decode 時間、decode token 数、
  beam 評価回数を schema v1 の `decodePhases` と text 出力へ記録する。
  phase 時間は性能原因の切り分けに使い、候補の機能検証や全体レイテンシの代替にはしない。

### 9.2.1 Host 可観測なエラーシンク（degraded → Health）

**問題**: `ZenzaiModelConverter` は現状 `(ZenzaiModelInfo, IConverter* fallback)` だけで
構築され、`InferenceEngine::last_error_`（private）への経路を持たない。一方 `Health`
（`Dispatcher::HandleHealth`）は `engine_->last_error()` のみで `ok` / `degraded` / `error`
を決める。`Convert` 内で例外/タイムアウト/空結果を握って fallback 候補を返すと、
**実推論が劣化しているのに `/Health` が `ok` を返す**（サイレント劣化）。

**契約**（DEV-221 が満たす）— *poll-after-call*（engine ロック内で吸い上げ）方式:

1. `ZenzaiModelConverter` に**直近 `Convert` の結果を反映する**エラー状態を持たせる:
   `std::optional<std::string> last_error_` + `bool degraded_`、非消費アクセサ
   `last_error()` / `degraded()`。**`Convert` の冒頭で必ず clear** し、`DegradeToFallback`
   時にセットする（⇒ 成功変換後は `last_error()` が nullopt を返す）。
2. `InferenceEngine` は runtime 変換劣化を**専用フィールド** `model_runtime_error_`
   （`std::optional<std::string>`）に持つ。これは load 時 / learning 由来の汎用
   `last_error_`（例 `failed to save learning store`〔learning〕）**とは別管理**。
3. `QueryCandidates` は **既に `state_mutex_` を保持**したまま `active_converter_->Convert(...)`
   を呼ぶ。その**直後（同ロック内）に毎回**、active が model converter なら
   `model_runtime_error_ = model_converter_->last_error();` で**ミラーする**
   （degrade 時はセット、**成功時は nullopt で上書き＝クリア**）。これが「成功で
   stuck degraded を解除する」明示パス。**コールバック push は `Convert` 内からの
   engine 再ロックで `state_mutex_` 再入デッドロックになる**ため採らない。
4. 防御として `QueryCandidates` の `Convert` 呼び出しを `try/catch` で囲む
   （`ApplyRerankerOrRaw` の reranker 例外境界と同型）。converter が万一再 throw した
   場合は engine 側で捕捉して `model_runtime_error_` を設定し、fallback または空を返す
   （Host は落とさない）。
5. `Health` の status は**既存の `model_loaded` 3 値分岐を保つ**。engine は `last_error_`
   （load/learning）と `model_runtime_error_`（runtime 変換）を畳んだ `effective_last_error()`
   を公開し、`Dispatcher::HandleHealth` はそれを使う（現行の `engine_->last_error()` 単独参照を
   置換）。status マッピング:
   - `effective_last_error()` 空 → **`ok`**。
   - 設定あり **かつ `model_loaded()`** → **`degraded`**（recoverable: runtime 変換劣化や
     loaded-model 警告）。
   - 設定あり **かつ `!model_loaded()`** → **`error`**（GGUF 欠落/不正等の hard load 失敗。
     既存 `HandleHealth` の error 分類を維持し、**degraded へ格下げしない**。`model_runtime_error_`
     は model loaded 時のみ立つので、この枝に来るのは load 失敗の `last_error_` のみ）。
   これにより:
   - runtime 劣化 → `model_runtime_error_` 立つ → `degraded`。
   - **その後の成功変換** → `model_runtime_error_` が nullopt に上書き → （他要因が無ければ）
     `ok` に**復帰**（§10 の Health テストと整合。stuck degraded を回避）。
   - 成功変換は `model_runtime_error_` のみクリアし、**load/learning 由来の `last_error_` は
     消さない**（別フィールドのため誤って健全化しない）。
   - **既存 API セマンティクスを壊さない**: CUDA 要求→CPU フォールバックは**成功 LoadModel**
     であり、警告は `ModelLoadResult.error` に返すのみ。`engine->last_error()` は空＝`Health=ok`
     のまま（既存テスト `LoadModelCudaFallsBackToCpuForNow` / `LoadModelCudaFallbackKeepsHealthOk`
     が assert）。`model_runtime_error_` は **runtime 変換劣化専用**であり、backend フォールバック
     警告を degraded に**再分類しない**（`docs/zenzai-gpu-route.md` の旧「degraded」表現は
     engine テストの実挙動が優先）。
6. **モデル lifecycle で `model_runtime_error_` をクリアする**: `LoadModelWithResult` の
   **成功**（リロード含む）・モデルの**無効化 / アンロード**時、既存の `last_error_.reset()` と
   **同じ箇所で `model_runtime_error_.reset()` も行う**。これを怠ると、直近 `Convert` が degraded を
   立てた後にリロード/無効化しても次の `QueryCandidates` まで stale runtime エラーが残り、リロード
   直後の Health が新しい健全状態（または fallback-only 状態）を degraded/error と誤報告する
   （poll-after-call は次回 `Convert` でしか上書きしないため、lifecycle 境界での明示クリアが要る）。

> 設計判断: converter の劣化は **engine が毎回ミラーして Health に出す**のが正典。converter は
> 自分の per-call `last_error_` を持つだけで engine の private には触れない（責務分離 +
> ロック安全）。runtime 劣化は **専用フィールド**に隔離し、成功で確実にクリアしつつ
> load/learning エラーと混ざらないようにする。

### 9.2.2 キャンセル / デッドラインの decode ループ反映（M10 整合）

**問題**: `QueryCandidates` は `const std::atomic<bool>* cancel` を `Convert` の**前後でのみ**
チェックし、かつ `state_mutex_` を保持したまま `Convert` を呼ぶ。`IConverter::Convert` は
cancel を受け取らないため、長い Zenzai decode は途中中断できず、stale な推論が token/latency
予算まで走って**新規クエリをブロック**する（roadmap M10 の host 側早期中断と乖離）。

**契約**（DEV-221、M10＝DEV-106 と協調）:

1. `BeamSearch` の decode ループは**毎反復**（または数トークンごと）に (a) `cancel`
   （QueryCandidates の `atomic<bool>*`）と (b) §8 由来の wall-clock **deadline** をポーリング:
   - cancel 観測 → 即中断。**`Convert` は cancel を空生成（empty-generation）と区別し、
     `DegradeToFallback` を経由せず `{}` を返す**（§9.1 で BeamSearch 後に `Canceled(cancel_)` を
     empty チェックより**先に**判定）。fallback を返すと stale preedit に対する SimpleConverter
     候補が TIP に表示され M10 早期中断に反するため。QueryCandidates 側の `{}` 返却と一致。
   - deadline 超過 → cancel とは別扱い。その時点の best-so-far を返す（§6.4。候補は出す）。
2. `IConverter::Convert` は cancel を引数に持たないため、engine は `Convert` 呼び出し前
   （`state_mutex_` 内）に converter へ **cancel ポインタ + deadline を設定**する
   （converter-local setter。`Convert` 後にクリア）。最小実装として **deadline 強制のみ**でも
   stale work を bound できる（hard mid-decode cancel の完全対応は M10 / DEV-106 と協調）。
3. これにより long decode が `state_mutex_` を保持して後続クエリを待たせる時間を、§8 の
   p95 予算で上限化する。

### 9.3 設定項目（`EngineConfig` / settings.json）

Zenzai 推論パラメータは **既存の `model.*` 名前空間**（`settings/mvp-settings.schema.json` の
`model` オブジェクト、`docs/model-management-spec.md` §5/§7）に載せる。**`zenzai.*` の新規 root
キーは作らない** — schema は root も `model` も `additionalProperties: false` のため、未知 root
キー（例 `zenzai.*`）は M11 / DEV-203 の設定検証で reject される。

**既存キーを再利用**（schema 変更不要）:

| キー | 既定 | 対応 |
|---|---|---|
| `model.enabled` | true | Zenzai 有効化（`model.selectedPath` 有 + probe 成功で実効） |
| `model.selectedPath` | "" | GGUF パス（`EngineConfig.model_path`） |
| `model.backendPreference` / `model.nGpuLayers` | auto / -1 | backend 選択（§1.3、`copilot-pc-backend-spec`） |
| `model.fallbackToSimpleConverter` | true | 劣化モードの SimpleConverter フォールバック（§7.4） |

**新規に追加する推論整形キー**（`model` オブジェクトへ追加。`model` は
`additionalProperties: false` のため **schema 拡張が必要**。拡張は `area:settings` /
M11 / DEV-203 が `docs/model-management-spec.md` と整合のうえ反映する。本 docs PR では
schema 自体は変更しない）:

| キー | 既定 | 範囲 | 対応 |
|---|---|---|---|
| `model.inferenceLimit` | 4 | 1〜8（MVP）/ 1〜50（将来） | §6.3 ビーム幅 |
| `model.nBest` | 4 | 1〜8 | §6.1 出力上限 |
| `model.profile` | "" | 文字列 | §3.2（将来。M8 は空既定） |
| `model.maxNewTokens` | 64 | 16〜256 | §8 |

> M8 受け入れ条件（roadmap）は「配置時 LoadModel 成功 / 未配置でも落ちない / CPU・GPU
> 切替が効く」。本 spec の設定は既存 `EngineConfig`（backend / model_path / n_gpu_layers）に
> 上記 Zenzai パラメータを足す形で、IPC payload の互換を壊さない（`tsf-ipc-protocol`）。新規
> `model.*` キーの schema 反映は M11 / DEV-203 のスコープ。

### 9.4 個人化（将来拡張・M8 非対象）

legacy の `PersonalizationMode`（base/personal n-gram marisa + alpha 0.0/0.5/1.0/1.5）は
**M8 では実装しない**。将来 M45（モデル管理）/ M54（学習強化）で扱う。本書は枠のみ予約し、
Zenzai score 帯（§6.5）に personalization 加点を**後段で**足せるよう、§7.5 の reranker
統合点へ寄せる設計とする。

---

## 10. テスト計画

ネット非依存・GGUF 非依存（モック or 小フィクスチャ）を原則とする
（`inference-host/tests/`、roadmap M8 受け入れ条件 / DEV-190 整合）。

| 種別 | 内容 |
|---|---|
| unit | `BuildZenzaiPrompt`: かな→カタカナ正規化、文脈 30 文字切詰、空文脈/空profile の省略、特殊トークン配置 |
| unit | `NormalizeLogprob`: 単調性、帯 [0.3,1.4] クランプ、num_tokens=0 ガード |
| unit | `DedupBySurface`（converter 内）: 同一表層で高 logprob 残存 |
| unit | マージ経路の source 設定（§7.1 注）: user_dict 候補が `CandidateSource::UserDictionary`（既定 Heuristic のままにしない） |
| unit | クロスソース dedup（§7.6）: user_dict と converter が同一表層のとき**ソース優先で user_dict を保持**（明示 value が低い/負 例 -3.0 でも Zenzai に落とされない）。user_dict を含まない重複は最高 score を残す |
| unit | 劣化モード（hard failure）: 例外/空生成/**usable beam が無い**ケースで `DegradeToFallback` され候補ゼロにならない。converter の `last_error()` 非空 → engine が `model_runtime_error_` にミラー＝`degraded`（§9.2.1） |
| unit | deadline 超過で **best-so-far beam あり**（§6.4/§9.2.2）: Zenzai の best-so-far を返し、`DegradeToFallback` を経由しない・valid な Zenzai 出力を捨てない・`degraded` にしない（normal budget expiry を hard failure と区別） |
| unit | モデル lifecycle クリア（§9.2.1）: degraded 変換後に LoadModel 成功/無効化/アンロードすると `model_runtime_error_` がクリアされ、直後の Health が stale な degraded/error を報告しない |
| unit | Health 反映（§9.2.1）: ①劣化変換後 `Health=degraded`、②**その後の成功変換で `model_runtime_error_` がクリアされ `ok` に復帰**（stuck degraded を回避）、③load/learning 由来の `last_error_` は成功変換で消えない（別フィールド隔離） |
| unit | CUDA→CPU フォールバック（§9.2.1）: LoadModel 成功・`last_error()` 空・`Health=ok`（既存 `LoadModelCudaFallsBackToCpuForNow` を回帰させない）。backend 警告は `model_runtime_error_` に立てない |
| unit | Health status 3 値（§9.2.1）: `effective_last_error` 空→`ok` / 設定あり＋`model_loaded`→`degraded` / 設定あり＋`!model_loaded`（GGUF 欠落・不正の hard load 失敗）→`error`（degraded に格下げしない） |
| unit | キャンセル/deadline（§9.2.2）: decode 中の cancel で即中断・**`{}` 返却（`DegradeToFallback` を経由せず stale な SimpleConverter 候補を出さない）**、deadline 超過は別扱いで best-so-far を返す。long decode が後続クエリを §8 予算超で待たせない |
| unit | source = `Model`、`debug_info` に `lp=`/`avg=` 痕跡 |
| unit | pre-tokenizer override 写像（§9.2）: `gpt2-small-japanese-char` のときだけ `gpt-2` へ写像し、他の pre-tokenizer には適用しない |
| unit | KV override **記述子の判断**（§9.2）: GGUF の tokenizer メタデータから override 記述子（キー・型・値）の一覧を決める純ロジックを、llama.cpp 無効ビルドで検証する。`gpt2-small-japanese-char` では `tokenizer.ggml.pre` の文字列記述子が 1 件だけ立ち、上流 pre-tokenizer・pre-tokenizer 宣言なしでは立たない。pre-tokenizer と eos の両方が該当する GGUF では pre-tokenizer → eos の順に 2 件並ぶ（llama.cpp へ渡す順序を固定する）。**記述子を `llama_model_kv_override` へ写す境界変換は本 unit の対象外**（同 §9.2。実モデル smoke が担保する） |
| unit | eos override 写像（§9.2）: 宣言された `eos_token_id` の piece が終端でない（開始トークンを指す）GGUF では語彙中の `</s>` の id へ解決し、**正しい eos を宣言する GGUF には override を適用しない**。id を定数で決め打ちしない（語彙から解決していることを、`</s>` の id が異なる語彙でも正しく解決できることで確認する） |
| integration（モデル有・任意/手動） | 上流 `Miwa-Keita/zenz-v3.2-small-gguf` の Zenzai GGUF 配置時、**host 入力 `にほんご`（かな）**→ **最上位候補が `日本語` に完全一致**する（**A5 解消**）。「`日本語` を含む」を合格条件にしてはならない — EOS override が欠けた状態の `日本語日本語日本語` が通過してしまうため（§9.2 / DEV-743）。あわせて `わたしはがくせいです` → `私は学生です`、および `top_debug_info` に `utf8-prefix-trimmed` が出ないことを確認する。代表入力について、override 適用後の token ID 列が `gpt2-small-japanese-char` の参照実装と一致することも差分確認する。romaji `nihongo` は TIP のキーストローク→かな経路（RomajiKanaConverter）の e2e 表現であり、host/converter テスト入力には使わない（§3.1）。DEV-221 受け入れ条件 / DEV-225 実機ゲート / DEV-753 |
| 順位 | user_dict 候補が Zenzai 候補より上（帯設計 §7.3）。学習加点で逆転し得ることの確認 |

---

## 11. 受け入れ条件（DEV-228）

- §1.2 の 6 項目が本 spec で確定し、DEV-221 が**追加の設計判断なしに**着手できる
  （プロンプト例 §3.2、擬似コード §9.1、候補合成規則 §7、score 写像 §6.5 が揃う）。
- 既存 spec と相互参照: `docs/zenzai-gpu-route.md`（本書へのリンク追加）、
  `docs/copilot-pc-backend-spec.md` §4、`docs/neural-reranker-spec.md` §9。
- `docs/README.md` 索引に本書を追加（Phase 3 / M8）。

---

## 12. Documentation impact

- [ ] Roadmap updated
- [X] Spec docs updated（新規 `docs/zenzai-inference-spec.md`、`docs/zenzai-gpu-route.md` と
  `docs/README.md` に相互参照を追加）
- [X] README update not needed
- [ ] No documentation impact

Reason: Zenzai 推論コントラクトを新規 spec として確定する設計タスク。マイルストーン
定義（roadmap M8）・受け入れ条件（定義）は不変のため roadmap は更新しない。設定キーは既存
`model.*` 名前空間（§9.3）に載せ、`settings/mvp-settings.schema.json` の拡張（`model` への
推論整形キー追加）は `area:settings` / M11 / DEV-203 のスコープとして本 PR では変更しない。
状態・進捗は Linear（DEV-228）が正典。
