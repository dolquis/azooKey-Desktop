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

M7 の `learning.tsv` は `reading\tsurface\tweight\tlast_used` 形式
（タブ区切り）。M54 では以下のいずれかを選択する:

**Option A: TSV 拡張**（軽量・既定）
- 新規列 `app_name` `event_type` `context_hash` を tab 末尾に追加
- 旧形式は読み込み時に新列を空で補う
- 既存ユーザーの学習データを壊さない

**Option B: SQLite 化**（将来）
- M54 v2 で実施。本 M54 では Option A を採用し、v2 で SQLite に移行
  するパスを spec に残す

本 M54 では **Option A** を採用する。SQLite 化は将来の独立 M（M54-B
など）で扱う。

### 3.2 TSV スキーマ（拡張）

```
# learning.tsv (v2)
# reading	surface	weight	last_used_at	commit_count	app_name	event_type	context_hash
にほんご	日本語	4.2	1780000000000	12	code.exe	commit	0xabcd1234
こうしょう	交渉	2.8	1779999000000	8	outlook.exe	commit	0xef567890
こうしょう	校章	0.0	1779990000000	0	outlook.exe	correction_reject	0xef567890
```

| 列 | 内容 |
|---|---|
| `reading` | 読み（ひらがな） |
| `surface` | 表記 |
| `weight` | 重み（log(1 + commit_count) × recency × ...） |
| `last_used_at` | UNIX ms |
| `commit_count` | 確定回数 |
| `app_name` | 確定時の前面アプリ（M48）。空文字列 = グローバル |
| `event_type` | `commit` / `correction_accept` / `correction_reject` / `typo_accept` / `typo_reject` |
| `context_hash` | 左文脈 hash（M46 と整合） |

### 3.3 SQLite 化（v2 ロードマップ）

将来 v2 で以下のテーブル分割を検討する（M54 範囲外）:

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
recency_score = exp(-days_since_last_commit / half_life_days)
```

| 用途 | half_life |
|---|---:|
| 一般語 | 30 日 |
| 固有名詞 | 90 日 |
| 技術語 | 120 日 |
| 一時的な話題語 | 14 日 |
| 打ち間違えパターン | 60 日 |

half_life は category（M53 辞書層から取得）で切り替える。category が
不明な場合は 30 日（一般語）を既定とする。

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
| `app_profile_weight` | M48 のプロファイル一致度（同 app: 1.2、別 app: 0.8） |
| `correction_penalty` | `correction_reject` 件数に応じ減衰（0.0〜1.0） |

`correction_penalty = max(0, 1 - 0.3 × reject_count)` を初期値とする。
詳細は M52 ベンチで校正する。

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
- `context_hash` は SHA-256 の上位 4 bytes のみを使う（衝突容認）
- ログ出力時は `reading` / `surface` を M41 §7.6 の方針で扱う
- 学習データの export / import は M49 が担当

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
- M46 secure 中は学習が発生しない
- M48 アプリ別プロファイルが `app_profile_weight` に反映される
- `event_type` 列が拡張可能な enum として実装され、`commit` /
  `correction_accept` / `correction_reject` を記録できる。M55 完了後は
  同じ列で `typo_accept` / `typo_reject` も扱えること（M55 未完了時は
  enum 値を予約しておくだけで可）

## 12. 将来拡張（M54 範囲外）

- SQLite 化（テーブル分割、index、複雑クエリ）
- 学習データのクラウド同期 → ポリシー違反のため恒久的に非採用
