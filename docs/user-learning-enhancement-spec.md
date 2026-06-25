# ユーザー学習強化 仕様

対象リポジトリ: dolquis/azooKey-Desktop
対応マイルストーン: M54（変換品質トラック）
関連: `plans/windows-port-roadmap.md` M7 / M34 / M48 / M52 / M53 / M55、
      既存 `learning/src/LearningStore.cpp`、
      `docs/conversion-quality-benchmark-spec.md`（M52）、
      `docs/typo-correction-learning-spec.md`（M55）、
      `docs/app-profile-spec.md`（M48）
作成日: 2026-05-27
位置づけ: 変換品質トラック（M52 完了後、M53 / M55 と並行可能）

## 1. 目的

M7 で実装した既存 `LearningStore`（reading/surface 頻度 + 時間減衰）を
発展させ、**確定履歴 / 訂正履歴 / アプリ別傾向 / 打ち間違え採否**を
細粒度に学習し、個人適応を強化する。M52 ベンチで `user_adapt` カテゴリ
の改善を測定可能にする。

## 2. 設計原則

- **既存 TSV からの後方互換**: `learning.tsv` を自動マイグレーション
  し、既存ユーザーの学習を失わない
- **イベント単位の永続化**: 単純な頻度カウントから訂正イベントまで
  扱う
- **アプリ別 / 文脈別の学習**: M48 と連動した分離学習
- **保守的な学習**: 訂正の方が確定より強い負例
- **プライバシー**: 文脈は hash 保存を標準（M46 と整合）

## 3. ストレージ設計

### 3.1 既存 TSV からの移行

M7 の `learning.tsv` は `reading<TAB>surface<TAB>weight<SPACE>last_updated_epoch_sec`
形式（3 タブフィールド、weight と epoch_sec はスペース区切り）。`LearningStore::Save`
の実装に合わせること（`last_updated_epoch_sec` は epoch 秒、ミリ秒ではない）。
DEV-7 以降、`LearningStore::Save()` は先頭に
`# azookey-learning-tsv escaped=1` ヘッダーを書き、M7 形式の `reading` /
`surface` フィールドを TSV 境界でエスケープする。保存時は `\` → `\\`、
tab → `\t`、LF → `\n`、CR → `\r` の順で表現し、ヘッダー付きファイルの
読み込み時だけ復元する。これにより表記に tab / 改行 / バックスラッシュが
含まれても 1 レコード 1 行を維持する。ヘッダーのない旧 M7 TSV は
backslash を literal として扱い、`C:\temp` のような既存 surface を `\t`
escape と誤解しない。復旧不能な行は Host stderr に警告を出してスキップし、
正常行の読み込みを継続する。
M54 では以下のいずれかを選択する:

**Option A: TSV 拡張**（軽量・既定）
- 新規列 `app_name` `event_type` `context_hash` を tab 末尾に追加
- 旧形式は読み込み時に新列を空で補う
- 既存ユーザーの学習データを壊さない

**Option B: SQLite 化**（将来）
- M54 v2 で実施。本 M54 では Option A を採用し、v2 で SQLite に移行
  するパスを spec に残す

本 M54 では **Option A** を採用する。SQLite 化は将来の独立 M（M54-B
など）で扱う。

### 3.1.1 M7 現行 LearningStore の永続化運用（DEV-11）

M7 の現行 `LearningStore` は TSV 全体を書き出すため、Host は確定ごとに
同期 `Save()` を呼ばない。`InferenceEngine` は以下の設定で dirty な学習変更を
バッチ化し、上限と GC を適用する:

| 設定 | 既定 | 意味 |
|---|---:|---|
| `learning_flush_every_n` | 8 | 未保存の observation が N 件に達したら `Save()` |
| `learning_flush_interval_sec` | 5 | 最初の未保存 observation から T 秒経過したら timer で `Save()` |
| `learning_max_records` | 10000 | 保持する `(reading, surface)` レコード上限。0 は上限なし |
| `learning_min_weight` | 0.05 | `Score(now)` が閾値未満のレコードを GC。0 以下は閾値 GC 無効 |

Interval flush は次の observation を待たずに background timer で実行する。`FlushLearningStore()` は明示 flush API とし、Host の破棄時および `LoadModel`
境界で呼ぶ。`Save()` 失敗時は Host stderr に error ログを出し、dirty と
未保存件数を維持して次回 observation または明示 flush で再試行する。

### 3.2 TSV スキーマ（拡張）

```
# learning.tsv (v2)
# reading	surface	weight	last_updated_epoch_sec	commit_count	app_name	event_type	context_hash
にほんご	日本語	4.2	1780000000	12	code.exe	commit	0xabcd1234
こうしょう	交渉	2.8	1779999000	8	outlook.exe	commit	0xef567890
こうしょう	校章	0.0	1779990000	0	outlook.exe	correction_reject	0xef567890
```

| 列 | 内容 |
|---|---|
| `reading` | 読み（ひらがな） |
| `surface` | 表記 |
| `weight` | 重み（log(1 + commit_count) × recency × ...） |
| `last_updated_epoch_sec` | epoch 秒（`LearningStore.cpp` の実装と一致。ミリ秒ではない） |
| `commit_count` | 確定回数 |
| `app_name` | 確定時の前面アプリ（M48）。空文字列 = グローバル |
| `event_type` | `commit` / `correction_accept` / `correction_reject` / `typo_accept` / `typo_reject` |
| `context_hash` | 左文脈 hash（M46 と整合） |

TSV の text 列には §3.1 と同じエスケープ規約を適用する。`reading` /
`surface` だけでなく、将来追加する `app_name` などの文字列列も raw tab /
raw 改行を含めない。

### 3.3 SQLite 化（v2 ロードマップ）

将来 v2 で以下のテーブル分割を検討する（M54 範囲外）。`last_committed_at` /
`created_at` / `updated_at` はいずれも epoch 秒（INTEGER、`LearningStore.cpp`
の単位と一致させ、ミリ秒で保存しない）:

```sql
CREATE TABLE committed_candidates (
  id INTEGER PRIMARY KEY,
  reading TEXT NOT NULL,
  surface TEXT NOT NULL,
  left_context_hash TEXT,
  app_name TEXT,
  commit_count INTEGER DEFAULT 1,
  last_committed_at INTEGER NOT NULL,
  created_at INTEGER NOT NULL
);

CREATE TABLE correction_events (
  id INTEGER PRIMARY KEY,
  reading TEXT NOT NULL,
  rejected_surface TEXT,
  accepted_surface TEXT,
  left_context_hash TEXT,
  app_name TEXT,
  event_type TEXT,
  created_at INTEGER NOT NULL
);

CREATE TABLE app_profiles_learning (
  app_name TEXT PRIMARY KEY,
  total_commits INTEGER DEFAULT 0,
  style_polite_ratio REAL,
  style_technical_ratio REAL,
  updated_at INTEGER NOT NULL
);
```

これらは v2 で実施し、M54 v1 では TSV 拡張で完結させる。

## 4. 学習イベント

`Dispatcher` は以下のイベントを `LearningStore::Observe` 経由で記録
する:

| イベント | 学習内容 |
|---|---|
| 候補確定（`commit`） | `commit_count += 1`、weight 更新 |
| 即 Backspace（`commit` → 直後 `backspace`） | 直前候補を `correction_reject` で記録 |
| 再変換（`recommit`） | 変更前を負例、変更後を正例 |
| ユーザー辞書登録 | weight を最優先（M9） |
| アプリ別確定 | `app_name` フィールドに記録 |
| typo 補正候補の採用（M55） | `typo_accept` イベント、pattern 信頼度 +0.25 |
| typo 補正候補の拒否（M55） | `typo_reject` イベント、pattern 信頼度 -0.45 |

## 5. 時間減衰

```
days_since_last_commit = max(0, (now_epoch_sec - last_updated_epoch_sec) / 86400)
recency_score          = exp(-ln(2) * days_since_last_commit / half_life_days)
```

`now_epoch_sec` / `last_updated_epoch_sec` はいずれも epoch 秒（§3.1。ミリ秒
ではない）。`half_life_days` は**真の半減期**であり、`days_since_last_commit ==
half_life_days` のとき `recency_score = 0.5` になる（`ln(2)` 係数で正規化する。
係数を省くと e-folding 時定数になり半減しないので注意）。時計の巻き戻りや未来の
`last_updated`（NTP 補正・手動時刻変更）で差が負になった場合は
`days_since_last_commit = 0` にクランプし、`recency_score = 1.0`（減衰なし）
として扱う。`recency_score` の値域は `(0, 1]`。

| 用途 | half_life | 根拠（なぜこの値か） |
|---|---:|---|
| 一般語 | 30 日 | 日々の話題に追従して入れ替わる語彙。約 1 か月で半減し、月単位の作業サイクルを 1 周期として古い確定を緩やかに忘れる。 |
| 固有名詞 | 90 日 | 人名・地名・組織名は一度関わると数か月は再出現しうる。一般語の 3 倍残し、四半期スパンの再利用を拾う。 |
| 技術語 | 120 日 | ドメイン語彙は語彙交替が遅く安定。最長の half_life を与え、長期プロジェクト中の専門用語を維持する。 |
| 一時的な話題語 | 14 日 | ニュース・イベント由来の語は陳腐化が速い。約 2 週間で半減させ、話題が去れば速やかに順位を下げる。 |
| 打ち間違えパターン | 60 日 | 打鍵の癖は安定して持続する一方、ユーザーが癖を直せばフェードすべき。一般語より長く技術語より短い中庸値とする。 |

half_life は category（M53 辞書層から取得）で切り替える。category が不明な
場合は 30 日（一般語）を既定とする。1 レコードが複数 category を取りうる場合
（辞書で多義）は **最長の half_life** を採用する（保守的に長く保持し、誤って
速く忘れない）。

これらの half_life は初期値であり、M52 `user_adapt` カテゴリの学習前後比較
（§11・`conversion-quality-benchmark-spec.md`）で校正する対象である（§13）。
ただし「一般語 < 固有名詞・打ち間違え < 技術語」「一時話題が最短」という
**大小関係（順序）は設計上の不変条件**として固定し、校正で順序を反転させない。

## 6. ユーザー学習スコア

```
user_score =
  log(1 + commit_count)
  × recency_score
  × app_profile_weight
  × correction_penalty
```

| 因子 | 意味 |
|---|---|
| `log(1 + commit_count)` | 確定回数の対数（飽和効果） |
| `recency_score` | §5 の時間減衰 |
| `app_profile_weight` | M48 のプロファイル一致度（§6.1） |
| `correction_penalty` | `correction_reject` 件数に応じ減衰（§6.2、値域 0.0〜1.0） |

`user_score` は乗算合成であり、各因子は **1.0 を中立（影響なし）** とする。
これにより M48（app）/ M46（context）/ M55（typo）いずれかが未完了の段階でも、
該当因子を 1.0 にすれば残りの因子だけで成立する（§11 受け入れ条件と整合）。

### 6.1 `app_profile_weight` の算出式

```
app_name_n(s) = lowercase(basename(s))   // "C:\...\Code.exe" → "code.exe"

app_profile_weight(e_app, ctx_app):
  if not app_aware_learning_enabled        → 1.0   // M48 未完了 / 設定 OFF
  else if e_app == "" (global record)      → 1.0   // app 非依存の確定
  else if ctx_app == "" (前面 app 不明)     → 1.0
  else if app_name_n(e_app) == app_name_n(ctx_app) → W_same   // 既定 1.2
  else                                     → W_diff           // 既定 0.8
```

- `e_app` は学習レコードの `app_name` 列（§3.2）、`ctx_app` は変換要求時の
  前面アプリ（M48 `ForegroundAppDetector`）。比較は **正規化後（小文字化 +
  パス除去した実行ファイル名）** で行う。
- 既定係数は `W_same = 1.2` / `W_diff = 0.8`。これは中立 1.0 に対する
  **±20% のナッジ**であり、`user_score` が乗算であることから、commit_count が
  2 倍以上開いた明確な頻度差（`log` 差）を覆さない程度に弱く、同程度の
  競合候補の並びだけを入れ替える強さに設定する。`W_same × W_diff = 0.96 ≈ 1`
  とし、同 app 加点と別 app 減点をほぼ対称にする。
- `W_same` / `W_diff` は初期値であり M52 / M48 統合検証で校正する（§13）。
  ただし `W_diff ≤ 1.0 ≤ W_same` の順序は不変条件として固定する。

### 6.2 `correction_penalty` の算出式

```
net_reject       = max(0, correction_reject_count - correction_accept_count)
correction_penalty = max(P_min, 1 - k_reject × net_reject)
```

- `correction_reject_count` / `correction_accept_count` は当該
  `(reading, surface[, app_name])` に対する `correction_reject` /
  `correction_accept` イベント（§4）の累積数。`net_reject` を使うことで、
  一度拒否された surface を後に再採用した場合（状況依存の拒否だった）に
  ペナルティが回復する。
- 既定係数は `k_reject = 0.3` / `P_min = 0.0`。`net_reject = 1` で 0.7、
  3〜4 回の純拒否で 0.0 に達し、繰り返し明示的に拒否された surface は
  候補から実質排除される（`P_min = 0.0` を許容する）。
- **訂正は確定より強い負例**（§2 設計原則）を、ペナルティが
  **乗算で効く**ことで表現する。`net_reject = 1` で `user_score` を 30% 下げ
  （0.7 倍）、純拒否が累積するほど倍率が下がる。`log(1 + commit_count)` の
  加法的増分と `k_reject` を直接比較はしない（次元が異なる）。`k_reject` /
  `P_min` は M52 で校正する（§13）。

## 7. UserLearningScorer

`inference-host/src/UserLearningScorer.cpp`（新規）として実装:

```cpp
class UserLearningScorer {
public:
  // candidate に user_score を付与
  void Score(const Context& ctx, std::span<Candidate> candidates);
private:
  double CalcUserScore(const LearningEntry& e, const Context& ctx);
  double RecencyScore(int64_t last_used_at, std::string_view category);
  double AppProfileWeight(std::string_view e_app, std::string_view ctx_app);
};
```

`Dispatcher` の rerank フェーズで呼び出す。失敗時（store 読み込み失敗等）
は user_score を 0 として続行（fallback 必須）。

## 8. プライバシー

- M46 secure mode 中は `Observe` を呼ばない（M46 §5 と整合）
- `context_hash` は §8.1 の算出式に従い、SHA-256 の上位 4 bytes のみを使う
  （衝突容認）
- ログ出力時は `reading` / `surface` を M41 §7.6 の方針で扱う
- 学習データの export / import は M49 が担当

### 8.1 `context_hash` の算出式と役割

```
left_ctx   = 確定対象 reading の直前にある確定済みテキストの
             末尾 K コードポイント（既定 K = 8。NFC 正規化後に切り出す）
ctx_bytes  = UTF-8(left_ctx)
digest     = SHA-256(ctx_bytes)
context_hash = uint32(digest[0..3])          // 先頭 4 bytes をビッグエンディアンで
TSV 表記    = "0x%08x"                        // 例 0xabcd1234
```

- **入力**: 直前の確定済みテキスト（surface）の末尾 K コードポイント。読み
  （reading）ではなく確定表記を使う。`left_ctx` が空（行頭・文書先頭）の場合は
  予約値 `0x00000000` とする。K は左文脈の語数を概ね 1〜2 文節に収める長さで、
  プライバシー（長い原文を復元させない）と弁別性のバランスから既定 8。
- **正規化**: NFC 正規化のみ行い、大文字小文字や字種はそのまま保つ（日本語
  文脈が主対象のため lowercase 化はしない）。
- **役割**: `context_hash` は `user_score`（§6）の連続因子ではなく、学習
  レコードの **検索・バケット用キー**である。同一 `context_hash` のレコードに
  限定して優先採用する将来拡張（context 一致ボーナス `W_ctx_match`）の
  フックとして列を確定させるが、**M54 v1 では `user_score` に context 因子を
  加えない**（中立 1.0 相当）。これにより §6 の 4 因子構成を保ったまま、
  スキーマだけ前方互換に確定する。
- **衝突容認**: 4 bytes（32 bit）は左文脈の弱いシグネチャであり衝突しうる。
  context_hash 一致は「同じ文脈で確定された可能性が高い」程度の弱い手掛かり
  として扱い、一致だけで候補を確定しない（誤バケットの影響を限定する）。

## 9. 既存 M7 との関係

- 既存 `LearningStore::Save` / `Load` は維持（TSV 列追加で後方互換）
- 既存 `LearningStore::Observe` シグネチャは互換維持。新列は
  `Observe(reading, surface, OptionalContext)` のように追加引数で対応
- 既存 `Reranker` を `UserLearningScorer` にリネーム（または並存）。
  旧 `Reranker` 名を 1 マイナーバージョン deprecation 後に削除

## 10. テスト

- unit: 旧 `learning.tsv` 読み込み → 新形式で書き出し（migration）
- unit: 5 イベント種類の score 寄与
- unit: half_life 切替（category 別）
- unit: M46 secure 中の `Observe` 抑止
- integration: M52 `user_adapt` カテゴリで学習前後の改善測定
- integration: M48 別 app での `app_profile_weight` 効果

## 11. M54 受け入れ条件

- 同じ入力を複数回確定すると、次回以降候補順位が上がる
- M52 ベンチで `user_adapt` カテゴリが学習前後で +3% 以上改善
- 既存 `learning.tsv`（M7 形式）から自動マイグレートできる
- M46 完了済みの環境では secure 中に `Observe` が呼ばれない（M46 が
  未完了の段階で M54 を実装する場合は、`PrivacyGate` フックポイントを
  no-op stub で残しておき、M46 完了時に本条件を検証する follow-up とする）
- `app_name` 列がイベントに記録され、`app_profile_weight` 計算経路が
  実装されている（M48 完了後の統合検証で実 boost を確認）。M48 未完了時
  は `app_profile_weight = 1.0` を返すデフォルト挙動で受け入れ可
- `event_type` 列が拡張可能な enum として実装され、`commit` /
  `correction_accept` / `correction_reject` を記録できる。M55 完了後は
  同じ列で `typo_accept` / `typo_reject` も扱えること（M55 未完了時は
  enum 値を予約しておくだけで可）

## 12. 将来拡張（M54 範囲外）

- SQLite 化（テーブル分割、index、複雑クエリ）
- 学習データのクラウド同期 → ポリシー違反のため恒久的に非採用

## 13. スコア定数と M52 校正の対応

§5〜§6 のスコア定数は **式の形（functional form）を本書で確定し、係数の値は
初期値として与える**。係数は M52 ベンチ（`conversion-quality-benchmark-spec.md`）
の `user_adapt` カテゴリ（学習前後比較・60 ケース）で校正する。各定数が
どの不変条件のもとで、どの指標を目標に動かされるかを次に固定する。

| 定数 | 既定 | 不変条件（校正で破ってはならない） | 校正の目標 |
|---|---:|---|---|
| `half_life`（§5） | 30/90/120/14/60 日 | 一時話題 < 一般語 < 固有名詞・typo < 技術語 の順序 | `user_adapt` 改善を最大化しつつ古い確定の過保持を避ける |
| `W_same`（§6.1） | 1.2 | `W_diff ≤ 1.0 ≤ W_same` | M48 別 app 検証で同 app 候補の正答率向上 |
| `W_diff`（§6.1） | 0.8 | `W_same × W_diff ≈ 1`（対称） | 別 app 由来の誤った boost を抑制 |
| `k_reject`（§6.2） | 0.3 | `0 < k_reject ≤ 1`（純拒否 1 回で倍率を 30% 下げる乗算ペナルティ） | 拒否 surface の再浮上を防ぐ |
| `P_min`（§6.2） | 0.0 | `0 ≤ P_min < 1` | 繰り返し拒否された surface を排除 |
| `K`（§8.1, context_hash 長） | 8 コードポイント | 復元不能な短さを保つ（プライバシー） | 文脈弁別性とのバランス |
| `W_ctx_match`（§8.1, v1 では未使用） | 1.0（中立） | `W_ctx_match ≥ 1.0` | 将来 context バケット導入時に校正 |

- **受け入れ判定（§11）に用いる指標**: `user_adapt` カテゴリが学習前後で
  +3% 以上改善（`conversion-quality-benchmark-spec.md` の閾値表と整合）。
- 校正は係数の数値のみを動かし、本表の「不変条件」列と §6 の 4 因子構成
  （乗算合成・各因子の中立 1.0）は固定する。スキーマ（§3.2 の TSV 列）を
  変える校正結果が出た場合は M54 範囲外とし、別 M（§12）で扱う。
