# ユーザー学習強化 仕様

対象リポジトリ: dolquis/azooKey-Desktop
対応マイルストーン: M54（変換品質トラック）、M15（§14 の予測供給）
関連: `plans/windows-port-roadmap.md` M7 / M15 / M34 / M48 / M52 / M53 / M55、
      既存 `learning/src/LearningStore.cpp`、
      `docs/conversion-quality-benchmark-spec.md`（M52）、
      `docs/typo-correction-learning-spec.md`（M55）、
      `docs/app-profile-spec.md`（M48）、
      `docs/legacy-parity-spec.md` §3（M15 予測候補ウィンドウ）
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
M54 は TSV を拡張する（§3.2）。SQLite 化は将来の独立 M（M54-B など）で扱う（§3.3）。

v2 は M7 のファイルを置き換えず、同じディレクトリの別ファイルに保存する。
`LearningStore` は M7 の位置（`learning.tsv`）で構築し、保存先は
`LearningStoreV2PathFor` が返す `learning.v2.tsv`（DPAPI 保存では
`learning.v2.tsv.enc`）とする。読み込みと移行の規則は次のとおり。

- v2 ファイルがあれば v2 ファイルだけを読む。M7 ファイルは参照しない。
- v2 ファイルが無ければ M7 ファイルを読み、各行を v2 の行へ変換する。
  変換した行は dirty として扱い、次の flush で v2 ファイルへ書く。
  通常の保存では M7 ファイルの中身を書き換えない。例外は 2 つある。平文の M7 ファイルを
  DPAPI 形式へ移す既存の移行（`learning-data-management-spec.md` の M34 の規約）と、
  忘却した組を M7 ファイルからも除く書き戻し（同 §4.2）である。
- M7 の行は `app_name`、`event_type`、`context_hash` を空、`accept_count` と
  `reject_count` を 0 で補う。`commit_count` は `max(1, round(weight / 0.8))`
  で推定する（0.8 は M7 の 1 確定あたりの weight）。weight が 0 の行は 0 とする。
  推定しないと §6 の `log(1 + commit_count)` が 0 になり、移行した学習の効果が消える。
- v2 ファイルが復号できないなど読み込みに失敗した場合は、空のストアで動作し、
  保存を止める。M7 ファイルにも v2 ファイルにも書き込まない。
- v2 ファイルに解析できない行があった場合は、読める行で動作を続ける。
  その行を黙って捨てないため、次の保存の前に、格納されているファイル（暗号化されたまま）を
  `<ファイル名>.corrupt-<epoch 秒>` へ一度だけ複写する。複写に失敗したら保存を止める。
  ログに出すのは行番号と件数だけで、行の内容は出さない。
- ファイルや import から読んだ値には上限を設ける（weight は 1e9、各回数は 1e9）。
  merge の加算も上限で止める。
- weight は、読み戻すと同じ double になる最短の表記で書く。保存と読み込みを繰り返しても値がずれない。

既知の制限は次のとおり。

- 学習ファイルの writer は Host だけで、同時に書くのは 1 つの writer という前提である。
  2 つの writer が保存すると、file lock により一方の完全な世代が残る（後勝ち）が、他方の変更は失われる。
- 解析のために、復号した平文を `std::istringstream` へ一度複写する。M7 から引き継いだ方式で、
  複写は関数を抜けると解放されるが、明示的に消去はしない。
- `context_hash` の NFC 正規化は Windows でだけ行う（§8.1）。

M7 ファイルを残すのは、v2 を読めない旧版の Host へ戻したときの保護である。
旧版は v2 の行を不正行として読み飛ばすため、同じファイルへ v2 を書くと、
旧版の次回保存で学習が空になる。別ファイルにしておけば、旧版は M7 ファイルを読み、
M54 導入後の学習だけを失う。忘却した組は M7 ファイルからも除くので、旧版へ戻しても現れない。
M7 ファイルの削除は別の Issue で扱う。

### 3.1.1 M7 現行 LearningStore の永続化運用（DEV-11）

M7 の現行 `LearningStore` は TSV 全体を書き出すため、Host は確定ごとに
同期 `Save()` を呼ばない。`InferenceEngine` は以下の設定で dirty な学習変更を
バッチ化し、上限と GC を適用する:

| 設定 | 既定 | 意味 |
|---|---:|---|
| `learning_flush_every_n` | 8 | 未保存の observation が N 件に達したら `Save()` |
| `learning_flush_interval_sec` | 5 | 最初の未保存 observation から T 秒経過したら timer で `Save()` |
| `learning_max_records` | 10000 | 保持する `(reading, surface, app_name)` 行の上限。0 は上限なし |
| `learning_min_weight` | 0.05 | `Score(now)`（組の app 行の合計）が閾値未満の `(reading, surface)` を、全 app 行ごと GC。0 以下は閾値 GC 無効。`reject_count` の合計が `accept_count` の合計を上回る組は GC しない（§6.2 のペナルティを保つため。拒否で weight が 0 になった組ほど消えやすいので除外が要る）。`learning_max_records` の上限は除外しない |

Interval flush は次の observation を待たずに background timer で実行する。`FlushLearningStore()` は明示 flush API とし、Host の破棄時および `LoadModel`
境界で呼ぶ。`Save()` 失敗時は Host stderr に error ログを出し、dirty と
未保存件数を維持して次回 observation または明示 flush で再試行する。

上記 2 つの契機に加えて、未保存 observation が 0 件の状態で新しい
observation を受け、かつ直近の保存成功から `learning_flush_interval_sec`
秒以上が経過している場合は、その observation を含めて同期 `Save()` を実行する
（burst 先頭の同期 flush）。直近の保存から同秒数未満の場合は即時保存せず、
件数契機と時間契機に委ねる。このレート制限により、追加の書き込みは 1 つの
burst につき最大 1 回に収まり、確定ごとの同期保存にはならない。狙いは、
保存を保証しない終了経路で失われる量を抑えることであり、区分と損失上限の正典は
`docs/learning-data-management-spec.md` §11 に置く。

### 3.2 TSV スキーマ（拡張）

先頭行は `# azookey-learning-tsv escaped=1 version=2` とし、以降の 1 行が
`(reading, surface, app_name)` の 1 組を表す。列はすべて tab で区切る。

```
# azookey-learning-tsv escaped=1 version=2
にほんご	日本語	9.6	1780000000	12	0	0	code.exe	commit	0xabcd1234
こうしょう	交渉	6.4	1779999000	8	1	0	outlook.exe	correction_accept	0xef567890
こうしょう	校章	0	1779990000	0	0	2	outlook.exe	correction_reject	0xef567890
```

| 列 | 内容 |
|---|---|
| `reading` | 読み（ひらがな） |
| `surface` | 表記 |
| `weight` | §4 の各イベントが加減した値の累計。M7 の weight と同じ意味で、§14 の前方一致と M7 減衰の `Score()` が使う。`user_score`（§6）は保存せず、スコア時に回数列から求める |
| `last_updated_epoch_sec` | 最後のイベントの epoch 秒（ミリ秒ではない） |
| `commit_count` | 確定回数。`correction_accept` と `typo_accept` も確定として数える |
| `accept_count` | `correction_accept` と `typo_accept` の累計（§6.2） |
| `reject_count` | `correction_reject` と `typo_reject` の累計（§6.2） |
| `app_name` | 確定時の前面アプリ（M48）を §6.1 の規則で正規化した値。空文字列 = グローバル |
| `event_type` | 最後のイベント。`commit` / `correction_accept` / `correction_reject` / `typo_accept` / `typo_reject`。M7 から移行した行は空 |
| `context_hash` | 最後のイベントの左文脈 hash（§8.1）。M7 から移行した行は空 |

TSV の text 列（`reading`、`surface`、`app_name`）には §3.1 と同じエスケープ規約を適用する。
列数が 10 でない行、数値列が非負の有限値でない行、未知の `event_type`、
`0x` と小文字 16 進 8 桁以外の `context_hash`、空の `reading` または `surface` は不正行として読み飛ばす。
同じ組の重複行は先勝ちとする。

`weight`、`commit_count`、`accept_count`、`reject_count` がすべて 0 の行は忘却済み（§7、
`legacy-parity-spec.md` §7）を表し、保存時に書き出さない。

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

Host は以下のイベントを `LearningStore::ObserveEvent` で記録する。
`Observe(reading, surface, alpha, now)` は app と文脈の無い `commit`、
`ObserveCorrection` は選んだ表記の `correction_accept` と拒否した表記の
`correction_reject` の組であり、M7 の呼び出し元はそのまま使える。
`alpha` は確定 1 回の weight（`learning_alpha`、既定 0.8）である。

| イベント | 記録する event | weight | 回数 |
|---|---|---|---|
| 候補確定 | `commit` | `+alpha` | `commit_count += 1` |
| 即 Backspace（確定の直後の取り消し） | 直前候補の `correction_reject` | `-alpha`（0 で止める） | `reject_count += 1` |
| 再変換（`recommit`） | 変更前の `correction_reject` と変更後の `correction_accept` | `-alpha` / `+alpha` | `reject_count` / `commit_count` と `accept_count` |
| typo 補正候補の採用（M55） | `typo_accept` | `+0.25` | `commit_count` と `accept_count` |
| typo 補正候補の拒否（M55） | `typo_reject` | `-0.45`（0 で止める） | `reject_count += 1` |
| アプリ別確定 | 上記の各 event を `app_name` の行に記録 | — | — |

ユーザー辞書登録は `LearningStore` に記録しない。登録語は辞書層の優先度（M9）で
候補の最上位に出る。どの event も `last_updated_epoch_sec`、`event_type`、
`context_hash` をそのイベントの値で上書きする。

### 4.1 訂正の IPC（`CommitCorrection`）

即 Backspace と再変換は、TIP が `CommitCorrection` で Host へ送る。Host は Handshake の
capability に `commit_correction` を載せ、TIP は Host がそれを告知したときだけ送る。
応答は `CommitObservation` と同じ `{ "ok": bool }` である。

| フィールド | 型 | 意味 |
|---|---|---|
| `kind` | `"undo"` \| `"reconvert"` | `undo` は即 Backspace、`reconvert` は再変換 |
| `reading` | str（非空） | 訂正した文節の読み |
| `rejected_surface` | str（非空） | 取り消した、または置き換えた確定表記 |
| `selected_surface` | str（任意） | `reconvert` で選び直した表記。`undo` では送らない |
| `left_context` | str | 文節より前の確定済みテキスト。`context_hash` の入力 |
| `timestamp_ms` | uint | 送信時刻 |
| `observation_id` | str | 再送の重複排除キー。訂正イベントごとに固有の値とし、訂正対象の確定に付けた id を再利用しない（Host は `CommitObservation` と同じ集合で重複を判定する） |
| `secure` / `learning_allowed` | bool | `CommitObservation` と同じ event privacy。欠落時は `true` / `false` |
| `app` | `{process_name, window_class}`（任意） | 前面アプリ。欠落時は大域行に記録する |

`kind` と `selected_surface` は一致しなければならない。`undo` に `selected_surface` がある要求、
`reconvert` に `selected_surface` が無い、空、または `rejected_surface` と同じ要求は不正とし、
Host は何も記録せず `ok: false` を返す。

Host は `CommitObservation` と同じ判定（Handshake の `secure_flag`、要求の `secure` /
`learning_allowed`、Host の privacy 設定）で学習を許すかを決め、許さないときは `ok: false` を返す。
許すときは、`observation_id` が既に適用済みなら記録せずに `ok: true` を返す。それ以外は、
`app` の行と `left_context` の `context_hash` で次を記録する。

- `undo`: `rejected_surface` の `correction_reject` だけを記録する。変換器には何も教えない。
- `reconvert`: `selected_surface` の `correction_accept` と `rejected_surface` の
  `correction_reject` を記録し、`selected_surface` を確定として変換器へ渡す。

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

M53 の `category_mask`（ビットの並びは `auto-word-registration-spec.md` §14.4 の表の順）から
half_life への対応は `HalfLifeDaysForCategoryMask` が持ち、M53 の obsolete_penalty と同じ表を使う。

| category_mask | half_life |
|---|---:|
| `technical` を含む | 120 日 |
| `person_name`、`place_name`、`station_name`、`product_name`、`software`、`anime_game`、`company_org` のいずれかを含む | 90 日 |
| `neologism` だけ | 60 日 |
| それ以外（`general` と不明） | 30 日 |

`event_type` が `typo_accept` / `typo_reject` の行は打ち間違えパターンとして扱い、
category の half_life と 60 日の長い方を使う。一時的な話題語（14 日）に対応する
category は現在の M53 の表に無く、値だけを予約する。

本番の減衰は M52 の校正が済むまで M7 の `exp(-0.15 × days)` のままとする
（§14.4）。§5 の half_life による減衰は `UserLearningScorer` の
`LearningDecayMode::CategoryHalfLife` で有効になり、既定は `LearningDecayMode::Legacy` である（§7）。

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

1 つの `(reading, surface)` が複数の `app_name` 行を持つ場合、`user_score` は
行ごとにこの式で求めた値の和とする。

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
  `(reading, surface, app_name)` 行の `reject_count` / `accept_count` 列（§3.2）であり、
  `correction_*` と `typo_*` のイベント（§4）の累積数である。`net_reject` を使うことで、
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

`inference-host/src/UserLearningScorer.cpp` に実装する:

```cpp
class UserLearningScorer {
 public:
  UserLearningScorer(const learning::LearningStore* store, CategoryLookup category_lookup,
                     UserLearningScorerConfig config = {});
  // 各候補の score に user_score を足し、score の降順に安定ソートする
  void Score(const UserLearningContext& context, std::span<core::Candidate> candidates) const;
  double CalcUserScore(const UserLearningContext& context, const std::string& surface) const;
  double RecencyScore(const learning::LearningRecord& record, uint16_t category_mask,
                      uint64_t now_epoch_sec) const;
  double AppProfileWeight(std::string_view record_app, std::string_view current_app) const;
  double CorrectionPenalty(const learning::LearningRecord& record) const;
};
```

- `UserLearningContext` は読み、前面アプリのプロセス名、現在時刻を持つ。
- `CategoryLookup` は `(reading, surface)` の M53 `category_mask` を返す関数で、
  Host は辞書層を引いて渡す。
- `UserLearningScorerConfig` は減衰モード（§5）と §13 の係数を持つ。既定の減衰は `Legacy`。
- 呼び出し位置は InferenceEngine の rerank フェーズ（`ApplyRerankerOrRaw`）である。
  内部の切り替え `EngineConfig::user_learning_scorer_enabled`（既定 false、`settings.json` のキーにはしない）が
  true のときだけ `UserLearningScorer` で並べ、false のときは M7 の `Reranker` のまま並べる。
  既定では順位も M52 の baseline も変わらない。M52 の校正の後に既定を切り替える。
  切り替えをオンにしても、現時点の rerank は `UserLearningContext` の app と文脈を空にして呼ぶ。
  変換要求の経路はまだ前面アプリを rerank まで運ばないためで、app 行は §6.1 の規則どおり重み 1.0 で合算される。
  context 因子は v1 では使わない（§8.1）。app を運ぶのは、既定を切り替える前の M52 校正の作業で行う。
  `store_` が無いときは user_score を 0 として元の順で返す。
- 失敗時（store が無い、category の参照で例外が出た、値が非有限になった）は
  その候補の user_score を 0 として続行する。score が非有限の候補は並びの末尾に置く。

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
  文脈が主対象のため lowercase 化はしない）。NFC は Windows の `NormalizeString`
  で行う。Windows 以外のビルドはテスト用であり、入力をそのまま使う
  （Host が受け取る文字列は Windows の TIP から来る）。不正な UTF-8 は正規化せずに使う。
- **実装**: `learning/src/ContextHash.cpp` の `ContextHash`。SHA-256 は OS に依存しない
  `learning/src/Sha256.cpp` を使う。
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

- `LearningStore::Save` / `Load` の呼び出し方は変えない。保存先と移行は §3.1 に従う。
- `LearningStore::Observe` / `ObserveCorrection` のシグネチャは互換を保つ。
  app と文脈を持つイベントは `ObserveEvent(LearningObservation, alpha, now)` で記録する（§4）。
- `Reranker` は `UserLearningScorer` と並存させる。InferenceEngine の rerank を
  `UserLearningScorer` に切り替えた後、`Reranker` を 1 マイナーバージョンの deprecation を経て削除する。

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
- secure 中に `Observe` が呼ばれない。TIP と Host の判定責務は
  `privacy-and-secure-input-spec.md` §5.1.1 に従う。
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

## 14. 学習ストアの reading-keyed 二層化と前方一致予測（M15 供給）

対応マイルストーン: M15（予測候補ウィンドウ）/ M54。

M7 の `LearningStore` は `reading<TAB>surface` を 1 本の文字列キーへ連結し、
`std::unordered_map` で保持する。
このキー設計では読みの前方一致で履歴を引けない。
ハッシュ表は順序を持たず、連結キーのどこまでが読みかも走査せずには判定できないためである。

本節は索引を **reading-keyed 二層構造**（読みから表記への 2 段の写像）へ組み替え、
読みの前方一致で過去の確定履歴を引く lookup と、M15 の予測候補ウィンドウへの供給契約を確定する。
確定するのは索引構造と lookup 契約と供給契約であり、スコアの式（§5 / §6）と
永続化の行スキーマ（§3.1 / §3.2）は変えない。
§3〜§13 の M54 本体とは独立に着手できる。

### 14.1 二層化は永続化形式を変えない

二層化はメモリ内の索引だけに適用し、行形式には持ち込まない。
永続化形式の版と移行は M54 の §3.1 / §3.2 だけが決める。

読みごとのブロックへ入れ子化しない理由は 2 つある。
1 行 1 レコードという形は §3.1 の破損耐性を支えており、入れ子にすると 1 行の破損が
同じ読みの全表記を巻き込む。
また §3.2 の列追加（`commit_count` / `app_name` / `event_type` / `context_hash`）は
同じ行スキーマ上の拡張であり、入れ子形式へ替えると M54 v1 の拡張と衝突する。

二層化に固有の移行処理は発生しない。

**書き出し順**: `Save()` の出力順は、エスケープ後の読み、TAB、表記、TAB、`app_name` を連結した
バイト列の昇順とする。
多層構造から書き出す実装でも、この連結キーの昇順を再現する。
各列を独立に比較して並べると、制御文字を含む読みで順序が入れ替わりうるため、
比較対象は連結後のキーで固定する。

**重複行**: 同一の `(reading, surface, app_name)` が複数行ある場合、先に現れた行を採用して
後続を捨てる（`Load()` の `emplace` による先勝ち）。

### 14.2 メモリ内データ構造

```cpp
// reading -> surface -> app_name -> record
std::map<std::string, std::map<std::string, std::map<std::string, LearningRecord>>> table_;
```

M54 の `app_name` 行（§3.2）は表記の下の 3 層目に置く。前方一致（§14.3）、
`Score()`、`ReverseLookup()` は表記ごとに app 行の減衰後 weight を合計した値を使う。

キーはエスケープ前の生の文字列とする。
エスケープは §3.1 のとおり入出力の境界だけで行い、索引のキーには持ち込まない。
外側を順序付き `std::map` にするのは、前方一致の対象が `lower_bound(prefix)` から始まる
連続した区間になるためである。
内側も `std::map` とし、同一読みの表記列挙を決定的にする。
1 つの読みが持つ表記は数個程度で、順序付きコンテナの定数倍は問題にならない。

**double-array trie は採らない**。
M53 の system 辞書はビルド時に構築して以後変更しない静的な索引である
（`auto-word-registration-spec.md` §15.2）。
学習ストアは確定のたびに `Observe` が走る mutable なストアであり、
挿入のたびに再配置を伴う double-array は向かない。
レコード数は `learning_max_records`（既定 10000、§3.1.1）で上限が付いており、
`std::map` の探索コストで足りる。

### 14.3 前方一致 lookup の契約

```cpp
struct PrefixMatch {
  std::string reading;
  std::string surface;
  double score;   // §14.4 の減衰後スコア
};

struct PrefixLookupResult {
  std::vector<PrefixMatch> matches;   // §14.5 の順序規則
  size_t visited_readings{};          // 外側 map で訪問した読みの数（走査範囲の検証用）
  size_t scanned_records{};           // 範囲内で見たレコード数（min_score 除外分を含む）
};

PrefixLookupResult LookupPrefix(const std::string& reading_prefix, size_t limit,
                                double min_score, uint64_t now_epoch_sec) const;
```

| 項目 | 規定 |
|---|---|
| 空の prefix | 常に空の結果を返す（全件走査を構造的に禁止する） |
| 走査範囲 | `lower_bound(prefix)` から、読みが prefix で始まらない最初の要素の手前まで。範囲を出た時点で打ち切る |
| バイト境界 | prefix はバイト列として比較する。UTF-8 の途中で切れた prefix もバイト前方一致としてヒットしうる |
| 除外 | `score < min_score` のレコードを結果に含めない |
| 上限 | `limit` 件。`limit == 0` は空を返す |
| `visited_readings` | 外側 map で訪問した読みの数。`lower_bound` の結果から範囲外を検出して打ち切るまでの反復回数 |
| `scanned_records` | 訪問した読みが持つレコードのうち、読みが prefix で始まるものの数（`min_score` で除外した分を含み、範囲外を含まない） |

`min_score` の既定は `learning_min_weight`（0.05、§3.1.1）と同じ値とする。
これにより `ObserveCorrection` で 0 まで落ちた負例と、
`learning-data-management-spec.md` §4.2 の忘却で 0 化されたエントリは予測に現れない。

**バイト境界の扱い**: 比較はバイト列で行うため、`あ`（`E3 81 82`）に対する `E3 81` のように
文字の途中で切れた prefix も前方一致としてヒットする。
lookup 側では UTF-8 の scalar 境界を検証せず、エラーにもしない。
呼び出し側は composition の読み（常に文字単位で確定した文字列）を渡すため、
実運用では文字の途中で切れた prefix は発生しない。
バイト前方一致で一致することを契約とし、境界テストで固定する（§14.8）。

**計算量**: 読みの種類数を R、prefix に一致する読みの数を P、
prefix に一致するレコード数を K、`limit` を L とすると `O(log R + K + K log L)` とする。
K に比例する走査だけを許し、R に比例する走査（karukan `learning.rs` の全読み走査）を禁じる。

`visited_readings` は「実際に何件の読みを見たか」を数える。
`lower_bound` から始めて範囲外の読みに達した時点で打ち切るなら
`visited_readings ≤ P + 1`（打ち切り判定で範囲外の 1 件を見る場合がある）であり、
先頭から全件を走査して一致だけを数える実装では R に近い値になる。
一致したレコードだけを数える `scanned_records` は、走査の始点と打ち切りを区別できないため、
全走査の検出には `visited_readings` を使う（§14.8 / §14.9）。

### 14.4 スコア式の維持

前方一致の順位付けには、既存 `LearningStore::Score()` が返す減衰後スコアをそのまま使う。

```
decayed_weight = weight * exp(-0.15 * days_since_last_update)
```

karukan の `recency * 10 + ln_1p(freq)` は採らない。
確定回数だけを見る `ln_1p(freq)` では、`ObserveCorrection` が weight の減算で表す負例を
表現できないためである。
現行 `Reranker::Apply` は候補スコアへ減衰後 weight を加算する合成であり、
式を替えると M52 の baseline も動く。

M54 本体が入ると、減衰は §5 の `recency_score`（category 別 half_life）へ置き換わる。
本節は減衰の式を二重に定義せず「ストアの `Score()` が返す値を使う」という形で契約するため、
置き換え後の lookup は自動的に §5 に従う。
なお現行の係数 0.15 は半減期に直すと約 4.6 日であり、§5 の既定 30 日とは一致しない。
この差は §13 の校正対象であり、本節では M7 の現行値を維持する。

### 14.5 順序規則

`matches` の並びは次の順で決める。

1. `score` の降順
2. 同点なら読みの昇順（エスケープ前のバイト列比較）
3. 同じ読みなら表記の昇順（同上）

同点の解決を読みと表記で固定するのは、ハッシュ順に依存した不定な並びを残さないためである。
`limit` による切り詰めは、この順序を確定させたあとに行う（`std::partial_sort` 相当）。

### 14.6 M15 予測候補への供給

供給点は `InferenceEngine::QueryPredictions` とする。
学習由来の前方一致ヒットと、変換器の `PredictNext` 由来の予測候補は、
スコアを合成せず**区分連結**で並べる。

1. 学習由来ヒットを §14.5 の順序で並べ、先頭 `prediction_learning_max_entries` 件（既定 3）を取る。
2. `PredictNext` の結果は現行どおり `ApplyRerankerOrRaw` を通す。
3. 1 の並びの後ろに 2 の並びを連結する。
4. 表記が重複する候補は先に現れた側を残す（学習由来が優先される）。
5. 応答全体を予測窓の表示上限（`legacy-parity-spec.md` §3.2 の最大 5 件）で切り詰める。

スコアを合成しないのは、学習 weight（`learning_alpha` の累積）と変換器のスコアが
次元の異なる量で、共通の尺度へ載せるには校正定数がもう 1 つ必要になるためである。
区分連結なら定数を増やさずに順序が決まる。
学習枠へ上限を置くのは、予測窓が過去の確定履歴だけで埋まると新しい入力への追従が落ちるためである。
表記で重複を落とすのは、次項のとおり学習由来と変換器由来で受理時の動作が違い、
同じラベルが 2 つ並ぶと同じものを選んだつもりで違う結果になるためである。

### 14.6.1 受理時の動作と完全一致の除外

`legacy-parity-spec.md` §3.4 は予測候補の受理を「preedit に追記」と規定する。
学習由来の候補は、この追記の対象を**読みの残り**とする。

- 応答の `CandidateField.reading` には学習レコードの読み全体（例 `にほんご`）を、
  `surface` には学習レコードの表記（例 `日本語`）を載せる。
- 予測窓が表示するのは `surface` である。
- 受理（Tab / Shift+Tab / クリック）では、`reading` から現在の composition の読みを取り除いた
  残り（例 `ご`）を preedit へ追記する。`surface` は preedit へ入れない。
- 受理後の preedit は学習レコードの読みと一致し、以後は通常の変換動線
  （Space での候補選択、ライブ変換）に戻る。学習した表記は §6 のスコアで上位に来る。
- TIP は受理の直前に `reading` が現在の composition の読みで始まることを確認し、
  始まらない場合は当該候補を捨てる（M10 の staleness check をすり抜けた応答への保険）。

`surface` を preedit へ追記しないのは、表記が読み全体に対応するためである。
`にほん` の preedit へ `日本語` を追記すれば `にほん日本語` になる。

**読みが prefix と完全一致するレコードは供給から除外する**。
追記すべき残りが空で、受理しても preedit が変わらないためである。
除外しても失われる機能はない。
その読みで確定した表記は、Space による変換候補（`QueryCandidates`）で §6 のスコアにより
上位へ来る経路が既にある。

この規定は `legacy-parity-spec.md` §3.4 の受理動作を変えず、`CandidateField` にも欄を追加しない
（`reading` は既存の欄である）。
M15 payload に replacement や delete count を導入する必要もない。

セーフ入力中の抑止は、M46 が TIP 側で `QueryPredictions` を送らないことで成立する
（`privacy-and-secure-input-spec.md` §5）。
本節では新しいゲートを設けない。

### 14.7 既存 API と設定への影響

- `Observe` / `ObserveCorrection` / `Score` / `Prune` / `Save` / `Load` のシグネチャと
  意味を変えない。`LookupPrefix` を追加するだけとする。
- `Prune` の `max_records` は `(reading, surface, app_name)` の行数を数える。
  読みの種類数ではない。
- `CandidateSource`（`core/include/azookey/core/Candidate.h`）へ `Learning` を末尾追加し、
  `inference-host/src/Dispatcher.cpp` の `SourceToWire` で `"learning"` へ写す。
  `CandidateField.source` は既存の文字列欄で、増えるのは欄ではなく値であるため
  `kEnvelopeVersion` は上げない。未知の値を受け取った旧受信側は既定の分岐へ落とす。
- `prediction_learning_max_entries`（既定 3）と `prediction_learning_min_score`
  （既定は `learning_min_weight` と同値）は `EngineConfig`
  （`inference-host/include/azookey/host/InferenceEngine.h`）へ置く。
  `learning_max_records` などと同じく Host 内部の既定値であり、
  `settings/mvp-settings.schema.json` には露出させない。

### 14.8 テスト方針

- 複数の読みがヒットする前方一致で、結果が §14.5 の順序（スコア降順、同点は読み昇順から表記昇順）で返る。
- prefix の直後の 1 文字だけが異なる読みが結果に混ざらない。空 prefix で空が返る。
- 10000 件を投入し prefix 一致が数件のとき、`visited_readings ≤ P + 1`（P = 一致した読みの数）に
  収まる。先頭から全件を走査して一致だけを数える実装ではこの上限を超えるため、テストが落ちる。
- 同じ入力で `scanned_records` が前方一致範囲のレコード数と等しい
  （`min_score` で除外された分を含み、範囲外を含まない）。
- `あ` の先頭 2 バイトのように文字の途中で切れた prefix で、当該読みがバイト前方一致でヒットする
  （§14.3 のバイト境界の契約）。
- ヘッダー無しの M7 TSV を読み込んで保存すると、§14.1 の連結キーの昇順で v2 形式の行が並ぶ。
- 同一の `(reading, surface)` を含む重複行で先勝ちになる。
- `ObserveCorrection` で weight が 0 になったレコードが前方一致の結果に出ない。
- 予測供給で、学習枠の上限と表記重複の先勝ちと表示上限の切り詰めが順に効く。
- 読みが prefix と完全一致するレコードが供給に現れない（§14.6.1）。
- 供給された候補は `surface` で表示され、受理すると `reading` から composition の読みを除いた
  残りが preedit へ追記される。追記後の preedit が学習レコードの読みと一致する。
- `reading` が現在の composition の読みで始まらない候補は受理時に捨てられる。

### 14.9 受け入れ条件

- 読みの前方一致で学習履歴を引け、結果が §14.5 の順序で返る。
- 前方一致の走査が `lower_bound` の範囲に限られる（`visited_readings ≤ P + 1` で確認）。
- 二層化だけを理由に永続化形式が変わらない（形式の版は §3 が決める）。
- 予測候補ウィンドウへ学習由来の候補が §14.6 の区分連結で供給され、
  受理が §14.6.1 の追記動作で成立する。
- 減衰後スコアが `exp(-0.15 * days)` のまま維持されている。
