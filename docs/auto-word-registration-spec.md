# 新語自動取得・辞書追加 仕様（Auto Word Registration）

本書は「Google 日本語入力のように、新しい単語を自動で取得して辞書に追加する」
機能の仕様を定める。対象は **Windows 版（C++ 移植: `core/` `learning/` `ipc/`
`inference-host/` `tsf-tip/`）** のみ。macOS 版 `legacy/` は対象外。

対応マイルストーン: `plans/windows-port-roadmap.md` M36-A（マイニング） /
M36-B（トレンド）+ M53（辞書層全体再設計）。
姉妹機能: `docs/typo-correction-learning-spec.md`（M35 + M55 統合補正）、
`docs/conversion-quality-benchmark-spec.md`（M52）。

> **本仕様の段差**: §1〜§13 は M36-A / M36-B の正典。
> §14 以降は M53 の追補章で、`AutoWordStore` を **多層 DictionaryStore**
> の 1 レイヤとして統合し、辞書層全体（base / sudachi / neologd /
> named_entity / technical_terms / user / app_specific）を再設計する。
> M36-A / M36-B の挙動は M53 でも下位互換として維持する。

## 1. 背景と目的

- 現状の辞書は 3 層: `SimpleConverter`（内蔵辞書 + `LoadFromTsv`）/
  `learning::UserDictionary`（**手動登録**の JSON 辞書）/ `learning::LearningStore`
  （確定履歴の重み付け再ランキング）。いずれも「新語を能動的に取り込む」機構を
  持たない。
- 本機能は、ユーザーがよく使うのに辞書に無い語（未知語＝OOV）や、世間で新しく
  使われ始めた語（トレンド語）を自動的に収集し、変換候補に反映する。
- 手動登録の `UserDictionary` とは独立した専用ストアで管理し、誤登録の一括削除・
  頻度管理・承認状態の管理をしやすくする。

## 2. 取得元（2 系統）

| 系統 | 取得元 | ネットワーク | 対応 MS |
|---|---|---|---|
| マイニング | ユーザー自身の入力・確定履歴から OOV を検出 | 不要（オフライン完結） | M36-A |
| トレンド取得 | プロジェクトがホストする整形済みトレンド語リストを定期 DL | DL のみ | M36-B |

## 3. 新ストア `learning::AutoWordStore`

### 3-1. データモデル

```cpp
namespace azookey::learning {

enum class AutoWordSource { Mining, Trending };
enum class AutoWordState  { Pending, Confirmed, Rejected };

struct AutoWord {
  std::string     surface;            // 表面形
  std::string     reading;            // かな読み（マイニングは確定時の読み）
  AutoWordSource  source{AutoWordSource::Mining};
  AutoWordState   state{AutoWordState::Pending};
  uint32_t        count{0};           // 観測頻度（mining）/ rank 由来初期値（trending）
  uint64_t        first_seen_epoch{0};
  uint64_t        last_seen_epoch{0};
  double          score{0.0};         // 候補注入時スコア
};
// (surface, reading) で一意。

}  // namespace azookey::learning
```

### 3-2. 永続化形式

TSV（`LearningStore` の区切り規約に準拠、`#` コメント可）。先頭にバージョン
ヘッダ行を置く。

```
# azookey-auto-word-store v1
surface	reading	source	state	count	first_seen_epoch	last_seen_epoch	score
```

- `source`: `mining` | `trending`、`state`: `pending` | `confirmed` | `rejected`
- 既定パス: `%LOCALAPPDATA%\azooKey\data\auto_words.tsv`
  （`learning.tsv` / `user_dict.json` と同じ `data\` 配下。host CLI `--auto-word-store` で上書き。
  §14.10 の非配布データ配置と一致させる）
- パース失敗行はスキップ。ファイル無しは空ストアで成功扱い（`UserDictionary::Load`
  の挙動に合わせる）。

### 3-3. API

```cpp
class AutoWordStore {
 public:
  explicit AutoWordStore(std::string path);

  bool Load();
  bool Save() const;
  void Reset();

  // マイニング観測。新規は pending 追加、既存は count++。
  // rejected キーは count も更新せず無視（再提示抑止）。
  // auto_promote かつ count >= promote_threshold で即 Confirmed 昇格。
  // 戻り値: Confirmed へ昇格したら true。
  bool Observe(const std::string& surface, const std::string& reading,
               uint64_t now_epoch, uint32_t promote_threshold, bool auto_promote);

  // トレンドリストの一括取り込み。rejected キーは skip。
  // 既存 mining 語と衝突したら source は mining 優先（ローカル観測実績を尊重）。
  void IngestTrending(const std::vector<AutoWord>& batch,
                      uint64_t now_epoch, bool auto_promote);

  // 承認フロー
  std::vector<AutoWord> ListByState(AutoWordState state) const;
  bool Confirm(const std::string& surface, const std::string& reading);
  bool Reject(const std::string& surface, const std::string& reading);

  // QueryCandidates 用: reading でルックアップし Confirmed のみ返す。
  std::vector<AutoWord> LookupConfirmed(const std::string& reading) const;

  // 古い pending（既定 90 日未昇格）を掃除。host 起動時に 1 回呼ぶ。
  size_t PrunePending(uint64_t now_epoch, uint64_t max_age_sec);

  size_t Size() const;
};
```

- 責務は「保存・状態遷移・ルックアップ」に限定。OOV 判定・しきい値・DL は持たない
  （`InferenceEngine` と `TrendingWordFetcher` が担う）。
- 却下語は削除せず `state=rejected` で永続化し、再観測しても pending に戻さない。
- `TrendingWordFetcher` のワーカースレッドから変更されるため、**内部に mutex を
  持ちスレッド安全**にする。

## 4. マイニング検出（M36-A）

### 4-1. 配線 — 新 IPC は不要

既存の `CommitObservation` を再利用する。`CommitObservationRequest{reading,
chosen, shown, left_context, timestamp_ms}` が確定読み・確定 surface を含み、
`Dispatcher::HandleCommitObservation` が `engine_->CommitObservation(reading,
chosen.surface, now)` を呼ぶ。この `InferenceEngine::CommitObservation` 内に
OOV 検出を追加する。

```cpp
void InferenceEngine::CommitObservation(reading, surface, now) {
  // ... 既存の store_->Observe / converter_->Commit ...
  if (auto_word_store_ && config_.auto_word_mining_enabled
      && IsMiningCandidate(reading, surface)) {
    auto_word_store_->Observe(surface, reading, now,
                              config_.auto_word_min_count,
                              config_.auto_word_auto_register);
    auto_word_store_->Save();
  }
}
```

### 4-2. OOV 判定 `IsMiningCandidate(reading, surface)`

確定 surface が「実辞書エントリ」に存在しないことを基準とする。

- `user_dict_->Lookup(reading)` に同一 surface があれば既知語 → 除外。
- `SimpleConverter` の実辞書バケット（`dictionary_[reading]`）に同一 surface が
  あれば既知語 → 除外。
- `auto_word_store_` で既に confirmed なら新規ではない（`Observe` 内の count++
  のみで足りる）。

> **実装上の落とし穴**: `SimpleConverter::Convert` は辞書ヒットが無くても
> 必ず identity（`{kana, kana}`）・長音・引用符などのヒューリスティック候補を
> 返す。よって「`Convert` の出力に surface が含まれるか」で既知判定をしては
> **いけない**。実辞書エントリ（dictionary バケット）の membership を直接確認
> すること。推奨は `IConverter` に `bool Contains(reading, surface)` を追加し、
> Zenzai（M8）導入後も統一的に判定できるようにする。当面は内蔵辞書バケットの
> 直接参照でよい。

### 4-3. 誤検出フィルタ

`IsMiningCandidate` で以下を除外する:

| ケース | 理由 |
|---|---|
| surface が記号・約物のみ | ノイズ |
| surface が ASCII 英数のみ | URL・コード片の混入防止 |
| surface が 1 文字（UTF-8 コードポイント単位） | 単漢字・誤確定ノイズ |
| surface が長すぎる（既定 20 コードポイント超） | 文章の誤確定 |
| reading が空 | 候補注入には reading 必須 |
| reading == surface | 変換していない（かな入力そのまま） |
| reading が非かな（ひらがな/カタカナ以外）を含む | 読みとして不正 |
| surface に数字を含む | 保守的に除外 |

しきい値（最大長など）は当面 `InferenceEngine` 側の定数とし、必要になれば設定化。

### 4-4. エッジケース方針

- mining 語の reading は確定時の読み。空になるケースは §4-3 で除外する。
- `converter_->Convert` を `CommitObservation` 内で呼ぶコストは確定時 1 回のため
  許容。Zenzai 導入後は内蔵辞書直接ルックアップへ最適化する余地をコメントで残す。
- trending と mining の同一語衝突は `IngestTrending` で mining を source 優先。

## 5. トレンド語取得（M36-B）

### 5-1. `host::TrendingWordFetcher`

- 新規 `inference-host/include/azookey/host/TrendingWordFetcher.h` /
  `src/TrendingWordFetcher.cpp`（WinHTTP 使用）。
- 起動時 1 回 + `interval_hours`（既定 24h）周期で `FetchOnce` を実行:
  DL → SHA256 検証 → JSON パース → `AutoWordStore::IngestTrending`。
- 検証・ネットワーク失敗時はストアを変更しない。`ETag` / `generated_at` 比較で
  無変更時はスキップ。

### 5-2. 取得元 — プロジェクトホストの静的アセット

クライアント（IME）は **Google Trends を直接スクレイピングしない**。代わりに
プロジェクトがホストする整形済みの静的アセットを定期 DL する。

- 取得元例: `https://github.com/<org>/azooKey-Desktop/releases/download/
  trending-latest/trending-words.json`（GitHub Releases の固定タグ、または
  HuggingFace データセット）。
- 併置の `trending-words.json.sha256` でハッシュ検証。将来は detached 署名
  （`.sig`）対応の余地を残す。
- 検証通過後、ローカルキャッシュへアトミック書き込み（macOS 版
  `legacy/Core/Sources/Core/InputUtils/DebugTypoCorrectionWeights.swift` の
  アトミック展開パターンを踏襲）。
- **上流のデータ生成パイプライン**（Google Trends 等からの収集・整形・
  スコアリング）は別リポジトリ / CI ジョブで運用し、**本書はクライアントの
  DL・検証・取り込みのみを規定する**。

### 5-3. アセットフォーマット

```json
{
  "version": 1,
  "generated_at": "2026-05-20T00:00:00Z",
  "words": [
    { "surface": "推し活", "reading": "おしかつ", "rank": 1 }
  ]
}
```

- `reading` は上流で付与済み（クライアントで読み推定はしない）。reading 欠落
  エントリは取り込みスキップ。
- `rank` から初期 `count` / `score` を導出
  （例: `score = base + (max_rank - rank) / max_rank * k`）。

### 5-4. M32 WinHTTP 基盤との関係

M32（自動更新）の `UpdateChecker.cpp` と HTTP DL + SHA256 検証パターンが共通。
共通ヘルパ `inference-host/src/HttpDownloader.{h,cpp}` を `UpdateChecker`（M32）と
`TrendingWordFetcher`（M36-B）で共有する。**`HttpDownloader` は M32 が新設し
（M32 は自動更新 §6 に加え `docs/sideload-packaging-spec.md` §1.6.1 (b) の初回
モデル DL でも利用）、M36-B はそれを再利用する**（依存順は M32 → M36-B。
`plans/windows-port-roadmap.md` の M32 / M36-B と整合）。

## 6. 適用（`QueryCandidates` への注入）

`InferenceEngine::QueryCandidates`（`InferenceEngine.cpp` 102-136 行）の
`user_dict_` ルックアップ直後、`converter_->Convert` の前に追加する。

```cpp
if (auto_word_store_) {
  for (const auto& w : auto_word_store_->LookupConfirmed(kana)) {  // Confirmed のみ
    core::Candidate c;
    c.surface = w.surface;
    c.reading = w.reading;
    c.score   = w.score;
    c.source  = core::CandidateSource::UserDictionary;
    c.debug_info = "auto-word";
    merged.push_back(std::move(c));
  }
}
```

- **pending 語は注入しない**（`LookupConfirmed` が Confirmed 限定）。承認後に
  初めて変換へ反映される。
- `EngineConfig` に `auto_word_default_score` を追加し、user_dict と内蔵辞書の
  中間程度のスコアを既定にする（誤候補の過剰な昇格を防ぐ）。

## 7. 承認フロー

### 7-1. 新 IPC メッセージ

- `ipc/include/azookey/ipc/Messages.h`: `MessageType` に
  `ListNewWordCandidates`、`ResolveNewWord` を追加。
- `ipc/src/Messages.cpp`: `TypeToString` / `TypeFromString` に対応追加。
- `ipc/include/azookey/ipc/Payloads.h` / `src/Payloads.cpp`:

```cpp
struct ListNewWordCandidatesRequest { std::string state_filter; uint32_t max_items{50}; };
struct NewWordField {
  std::string surface, reading, source, state;
  uint32_t count{0};
  uint64_t last_seen_epoch{0};
};
struct ListNewWordCandidatesResponse { std::vector<NewWordField> items; };

struct ResolveNewWordRequest { std::string surface, reading, action; }; // action: confirm|reject
struct ResolveNewWordResponse { bool ok{false}; };
```

### 7-2. Dispatcher 配線

- `Dispatcher` コンストラクタに `learning::AutoWordStore* auto_word_store` を追加
  （`user_dict` の隣）。
- `Dispatch` の switch に 2 ケース追加。
- `HandleListNewWordCandidates`: `auto_word_store_->ListByState(...)` →
  `NewWordField` に変換。
- `HandleResolveNewWord`: action に応じ `Confirm` / `Reject` → `Save()`。

### 7-3. UI と MVP 暫定

- **本命 UI**: M30 WinUI3 設定アプリの辞書ペインに「新語候補」リストを追加。
  pending を `ListNewWordCandidates` で取得、各行に登録/却下ボタン →
  `ResolveNewWord`。M36 の承認 UI は M30 に依存する。
- **M30 完成前の MVP 暫定**（いずれか／併用）:
  1. **auto モード限定運用** — 承認 UI が無い間は `registrationMode=auto` を
     推奨設定とし、pending を経ず即 confirmed。
  2. **host のデバッグ CLI** — `--list-new-words` /
     `--confirm-new-word <surface> <reading>` のワンショットサブコマンドを
     `inference-host` に追加（`AutoWordStore` を直接操作。検証・dogfooding 用）。

## 8. 設定

`settings/mvp-settings.schema.json` に追加する（**M36 実装着手時に追加**。
本仕様書では定義のみ記載）。

```json
"autoWordRegistration": {
  "type": "object",
  "description": "M36: 新語の自動取得・辞書追加",
  "properties": {
    "miningEnabled":   { "type": "boolean", "default": true },
    "trendingEnabled": { "type": "boolean", "default": false },
    "registrationMode": { "type": "string", "enum": ["confirm", "auto"], "default": "confirm" },
    "miningMinCount":  { "type": "integer", "default": 3, "minimum": 1 },
    "trendingIntervalHours": { "type": "integer", "default": 24, "minimum": 1 }
  },
  "additionalProperties": false
}
```

host 側 `SettingsStore` は導入済みだが、本機能の設定キー追加・runtime 反映は
M36 実装範囲のため、当面の実効値は host CLI 引数 / 環境変数で受ける（M35 と同じ運用）:
`--auto-word-mining on|off` / `--auto-word-trending on|off` /
`--auto-word-mode confirm|auto` / `--auto-word-min-count N` /
`--auto-word-store <path>` / `--trending-url <url>`
（環境変数 `AZOOKEY_AUTOWORD_*` フォールバック）。`EngineConfig` には
`auto_word_mining_enabled` / `auto_word_trending_enabled` /
`auto_word_auto_register` / `auto_word_min_count` / `auto_word_default_score`
を追加する。

## 9. プライバシー方針

- **マイニングはオフライン完結** — OOV 判定・`auto_words.tsv` への保存はすべて
  ローカルの host プロセス内処理。ユーザーの入力語・確定履歴を外部へ送信しない。
  新たな外部通信を一切追加しない。
- **トレンド取得はダウンロードのみ** — `TrendingWordFetcher` は公開静的アセットを
  GET するだけ。ユーザー語・入力統計・端末情報をリクエストに含めない
  （User-Agent はバージョン文字列のみ、M32 と同様）。アップロードは行わない。
- **DPAPI 暗号化（M34）対象に新ストアを含める** — `auto_words.tsv` はユーザーの
  未知語（固有名詞・個人情報を含みうる）を蓄積するため、`learning.tsv` /
  `user_dict.json` と同等の機微情報として M34 の暗号化対象に追加する。
  公開アセットである trending-words のローカルキャッシュは暗号化不要。

## 10. マイルストーン分割

| MS | 内容 | 前提 |
|---|---|---|
| M36-A | ユーザー入力マイニングによる新語自動登録 | M6 のみ。HTTP 非依存で独立着手可 |
| M36-B | リモートトレンド語取得 | M36-A 完了 + M32 の WinHTTP 基盤（`HttpDownloader`） |

## 11. 変更対象ファイル一覧

| 区分 | ファイル | MS |
|---|---|---|
| 新規 | `learning/include/azookey/learning/AutoWordStore.h` / `src/AutoWordStore.cpp` | A |
| 新規 | `learning/tests/auto_word_store_test.cpp` | A |
| 新規 | `inference-host/include/azookey/host/TrendingWordFetcher.h` / `src/TrendingWordFetcher.cpp` | B |
| 再利用 | `inference-host/include/azookey/host/HttpDownloader.h` / `src/HttpDownloader.cpp`（M32 で新設。§5-4） | B |
| 編集 | `ipc/include/azookey/ipc/Messages.h`, `ipc/src/Messages.cpp` | A |
| 編集 | `ipc/include/azookey/ipc/Payloads.h`, `ipc/src/Payloads.cpp` | A |
| 編集 | `inference-host/include/azookey/host/InferenceEngine.h`, `src/InferenceEngine.cpp` | A |
| 編集 | `inference-host/include/azookey/host/Dispatcher.h`, `src/Dispatcher.cpp` | A |
| 編集 | `inference-host/src/main.cpp` | A / B |
| 編集 | `settings/mvp-settings.schema.json` | A |
| 編集 | `learning/CMakeLists.txt`, `learning/tests/CMakeLists.txt` | A |
| 編集 | `inference-host/CMakeLists.txt`, `inference-host/tests/CMakeLists.txt` | B |

## 12. テスト計画

- 新規 `learning/tests/auto_word_store_test.cpp`:
  `Observe` の pending 追加 → count++ → しきい値で Confirmed 昇格 /
  `auto_promote` 即昇格 / Reject 後の再 `Observe` が無視される /
  `LookupConfirmed` が pending を返さない / `IngestTrending` の rejected skip・
  mining 優先 / Save→Load 往復 / `PrunePending` の経過判定 /
  キー一意性（reading 空 vs 非空）。
- `ipc/tests/payloads_test.cpp`: `ListNewWordCandidates` / `ResolveNewWord` の
  Build→Parse 往復、不正 action・欠損フィールドのパース失敗。
- `ipc/tests/messages_test.cpp`: 新 `MessageType` の `TypeToString` /
  `TypeFromString` 往復。
- `inference-host/tests/engine_test.cpp`: `CommitObservation` で OOV が記録 /
  既知語（user_dict・内蔵辞書）は記録されない / 誤検出フィルタで除外 /
  Confirmed 語が `QueryCandidates` に `debug_info="auto-word"` で注入 /
  pending 語は注入されない。
- `inference-host/tests/dispatcher_test.cpp`: `HandleListNewWordCandidates` が
  pending 一覧を返す / `HandleResolveNewWord` confirm/reject の反映 /
  `auto_word_store_=nullptr` でクラッシュしない。
- 新規 `inference-host/tests/trending_word_fetcher_test.cpp`（M36-B）:
  SHA256 検証成功/失敗の分岐。`HttpDownloader` をインターフェース化し
  テストダブルを注入（HTTP はモック / ローカルファイル経由）。

## 13. 検証手順（実装後）

1. ビルド: `cmake --preset windows-debug -DAZOOKEY_FETCH_GOOGLETEST=ON && cmake --build --preset windows-debug`
2. テスト: `ctest --preset windows-debug --output-on-failure`
   （`auto_word_store_tests` / `payloads_test` / `engine_test` /
   `dispatcher_test`、M36-B では `trending_word_fetcher_tests` が green）。
3. host を `--auto-word-mining on --auto-word-mode auto` で stdio 起動し、
   辞書に無い `(reading, surface)` の `CommitObservation` を `miningMinCount`
   回送ってから同じ reading の `QueryCandidates` を投げ、`auto-word` マーク付き
   候補が注入されることを確認。`confirm` モードでは `ListNewWordCandidates` /
   `ResolveNewWord` で承認後に注入されることを確認。
4. M36-B: 正規アセットとハッシュ不一致アセットの両方で `FetchOnce` を実行し、
   検証通過時のみ取り込まれることを確認。

---

## 14. M53 追補: 辞書層全体の再設計

本章は M53 で導入する追補仕様。M36-A / M36-B の `AutoWordStore` を
保持しつつ、辞書層全体を多層 `DictionaryStore` として整理する。Zenzai
が苦手な固有名詞・新語・技術語・地名・人名・製品名を辞書層で補強する
ことを目的とする。

### 14.1 アーキテクチャ

```
DictionaryStore
  ├─ base_lexicon            (SimpleConverter 内蔵, bundled)
  ├─ sudachi_lexicon         (bundled。配布判定 §14.9 / §14.10)
  ├─ neologd_lexicon         (別 pack DL・同梱不可, follow-up で取り込み。§14.9/§14.10)
  ├─ named_entity_lexicon    (bundled curated)
  ├─ technical_terms_lexicon (bundled curated)
  ├─ user_dictionary         (M9 の UserDictionary, local-only)
  ├─ auto_words              (M36-A の AutoWordStore, local-only)
  └─ app_specific_dictionary (M48 アプリ別, local-only)
```

各層は独立にロード / 無効化 / 更新可能。`DictionaryCandidateProvider`
が全層を query して候補を merge する。

### 14.2 辞書エントリ形式

```json
{
  "surface": "TensorRT",
  "reading": "てんそるあーるてぃー",
  "normalized_reading": "てんそるあーるてぃー",
  "pos": "名詞-固有名詞",
  "category": ["technical", "proper_noun"],
  "cost": 4200,
  "frequency": 0.72,
  "source": "technical_terms",
  "priority": 0.85,
  "created_at": "2026-05-27",
  "updated_at": "2026-05-27"
}
```

| フィールド | 内容 |
|---|---|
| `surface` | 表記 |
| `reading` | 読み（ひらがな） |
| `normalized_reading` | 正規化後（§14.3） |
| `pos` | 品詞 |
| `category` | カテゴリ配列（§14.5） |
| `cost` | mecab 互換 cost |
| `frequency` | 0.0〜1.0 |
| `source` | どの層由来か |
| `priority` | 0.0〜1.0、scoring 用 |

### 14.3 読み正規化

| 入力 | 正規化 |
|---|---|
| カタカナ | ひらがなへ |
| 全角英数 | 半角英数へ |
| 長音「ー」 | 保持。ただし一致判定では緩く扱う |
| ヴァ / バ揺れ | alias として保持（双方ヒット） |
| づ / ず、ぢ / じ | alias として保持 |

完全一致 → alias 一致 → 緩い長音一致の順で評価する。

### 14.4 カテゴリ

| category | 例 |
|---|---|
| `general` | 一般語 |
| `person_name` | 山田太郎 |
| `place_name` | 三河八橋、豊田市 |
| `station_name` | 名古屋駅、知立駅 |
| `product_name` | iPhone 16 Pro、RTX 4070 |
| `software` | TensorRT、DirectML、azooKey |
| `anime_game` | 作品名・キャラ名 |
| `company_org` | OpenAI、SB Intuitions |
| `technical` | 技術用語 |
| `neologism` | 新語 |

M54 の time-decay half_life もこの category で切り替える（一般語 30 日、
固有名詞 90 日、技術語 120 日）。

### 14.5 dictionary_score

```
dictionary_score =
  base_frequency
  + source_priority
  + exact_reading_bonus
  + category_bonus
  - obsolete_penalty
```

| 因子 | 説明 |
|---|---|
| `base_frequency` | エントリの frequency（0.0〜1.0） |
| `source_priority` | 層 priority（user > technical > neologd > base） |
| `exact_reading_bonus` | 完全一致 +0.10、alias 一致 +0.05 |
| `category_bonus` | 辞書エントリ category（§14.4 の `person_name` / `place_name` / `technical` 等）に対する `settings.dictionary.categoryBoosts` 適用。M48 と独立した辞書層側のスコア因子 |
| `obsolete_penalty` | 最終使用が古いエントリに減点 |

**M48 の `candidateTagBoosts`（候補タグ boost）は `dictionary_score` に含めない。**
M48 タグ boost は `docs/app-profile-spec.md` §7 が候補の `final_score` に対して
**1 回だけ**乗算的に適用する正典機構であり、DictionaryStore 由来候補にも同一
経路で（§14.12 が付与する候補タグに基づき）作用する。`dictionary_score` 側で
重ねて加算すると二重適用になるため、ここでは扱わない。`category_bonus`（辞書
category への `dictionary.categoryBoosts` 適用）は M48 タグ boost とは別 setting
key・別 namespace の辞書層内因子であり、二重適用しない。

各因子の確定係数（`source_priority` 表・bonus/penalty の式と既定値）は §14.11、
辞書 category から候補タグへの写像と source tagging 戦略は §14.12 を参照。

### 14.6 辞書更新パイプライン

| 種類 | 更新方法 | M |
|---|---|---|
| bundled dictionary | アプリ更新時に同梱 | M28 / M53 |
| neologism pack（`neologd_lexicon`） | 任意更新（SHA256 検証 DL。M36-B/M32 の `HttpDownloader` 基盤を再利用） | neologd pack follow-up（§14.10） |
| technical pack | 任意更新（bundled or download） | M53 |
| user dictionary | 即時反映 | M9 |
| auto_words | 即時反映（M36-A の confirm / auto） | M36-A |
| app-specific dictionary | 設定画面で ON / OFF | M48 |

各層のライセンス可否・同梱/別 pack/非配布の判定は §14.9、パッケージング方式と
第三者帰属（ThirdPartyNotices）は §14.10 を正典とする。

### 14.7 DictionaryStore 実装

`learning/src/DictionaryStore.cpp`（新規）として実装:

```cpp
class DictionaryStore {
public:
  // 全層 query
  std::vector<DictionaryEntry> Lookup(
      std::string_view reading,
      const LookupContext& ctx);

  // 個別層の有効化
  void EnableLayer(LayerId layer, bool enabled);

  // 層別 priority
  double LayerPriority(LayerId layer) const;

private:
  std::vector<std::unique_ptr<IDictionaryLayer>> layers_;
};
```

各 layer は `IDictionaryLayer` を実装。既存 `UserDictionary` /
`AutoWordStore` も layer として wrap する（後方互換）。

物理層（読みの索引構造・直列化形式・マルチソースビルド）は §15 を正典とする。
`IDictionaryLayer` の検索インターフェースと 2 方向の前方一致（common-prefix / predictive）の契約、
静的層の `.azdic` アーティファクトと mutable 層の `std::map` の使い分けは §15.8 / §15.7。

### 14.8 設定スキーマ拡張

`mvp-settings.schema.json` に追加:

```json
{
  "dictionary": {
    "sudachiEnabled": true,
    "neologdEnabled": false,
    "namedEntityEnabled": true,
    "technicalTermsEnabled": true,
    "userDictionaryEnabled": true,
    "autoWordsEnabled": true,
    "appSpecificDictionaryEnabled": true,
    "verifyOnLoad": false,
    "categoryBoosts": {
      "person_name": 1.1,
      "place_name": 1.1,
      "station_name": 1.1,
      "product_name": 1.1,
      "company_org": 1.1,
      "software": 1.0,
      "anime_game": 1.0,
      "technical": 1.0,
      "neologism": 1.0,
      "named_entity": 1.1
    }
  }
}
```

`neologdEnabled` の既定は **`false`**（opt-in）。`neologd_lexicon` は MSIX 非同梱の
別 DL pack（§14.9 / §14.10）であり、pack が未ダウンロードの状態で `true` にしても
当該 layer は無効（missing-pack）として扱う。bundled 層（`sudachi` / `named_entity` /
`technical_terms`）の既定は `true`。

`verifyOnLoad` の既定は **`false`**。真にすると、静的層アーティファクトのロード時に
`.azdic` の全バイトハッシュ検証（§15.5）を行う。既定でオフなのは起動レイテンシに
乗せないためであり、オフでもヘッダとセクション範囲の検証は常に行う。

`categoryBoosts` は **boost-only** とし、各値は schema 上 **[1.0, 1.2] に検証
（clamp）** する（1.0 未満による降格を禁止し、上限を `category_bonus` の宣言レンジ
0.20 に対応させる。§14.11 の `category_bonus = clamp(categoryBoosts − 1.0, 0.00,
0.20)` と整合。`candidateTagBoosts` の boost-only 契約（`docs/app-profile-spec.md`
§7）と同方針）。

`categoryBoosts` の key は §14.4 で定義する具体 category（`person_name`,
`place_name`, `station_name`, `product_name`, `company_org`, `software`,
`anime_game`, `technical`, `neologism`）と一致させる。`named_entity` は
固有名詞系（`person_name` / `place_name` / `station_name` / `product_name` /
`company_org`）の umbrella tag として scorer 側で同義扱いし、エントリ category
が具体名（例: `person_name`）でも `categoryBoosts.named_entity` の
値が当該カテゴリへ加算される（具体値が同時に設定されている場合は具体値が優先、
umbrella は加算しない）。これにより M52 `named_entity_recall_at_5` の
target を達成可能な scoring 経路を確保する。

### 14.9 辞書ソースのライセンスと配布判定

各辞書層のソース・ライセンス・配布可否。**配布判定の正典は本節**であり、
MSIX 同梱物（`docs/sideload-packaging-spec.md` §1）はこの判定に従う。ライセンスは
外部上流の変更があり得るため、各ソースの一次情報（下記 URL）を **取り込み時に
再検証**する（最終確認: 2026-06-11）。

| layer | 採用ソース | ライセンス | 配布判定 | 条件・注記 |
|---|---|---|---|---|
| `base_lexicon` | azooKey_dictionary_storage | Apache-2.0 | **同梱可**（既存） | azooKey 内蔵辞書。LICENSE / NOTICE を ThirdPartyNotices に保持 |
| `sudachi_lexicon` | SudachiDict（`core` 版） | Apache-2.0（内包: UniDic=BSD-3-Clause / NEologd 由来データ） | **同梱可** | LEGAL に基づき配布物全体が Apache-2.0。UniDic の BSD-3 著作権表示に加え、SudachiDict NOTICE（内包 NEologd 由来データの Hatena / 郵便 / 駅名 / 人名 帰属）も帰属に伝播（§14.10）。サイズの観点で `full` ではなく `core` を採用 |
| `neologd_lexicon` | mecab-ipadic-NEologd | Apache-2.0（ただし上流データに個別条件: Hatena キーワード=はてな社条件・要帰属 / 駅名 / 人名 / 郵便 等） | **別 pack DL（同梱不可）** | サイズ大 + 上流データの provenance が個別条件付き。MSIX に含めず SHA256 検証 DL（既定無効）。取り込み経路は §14.10 の neologd pack follow-up（M36-B とは別作業）。DL 時に上流ライセンス/帰属を提示 |
| `named_entity_lexicon` | Wikidata（CC0）+ GeoNames（CC-BY-4.0）+ 日本郵便 郵便番号データ | CC0 / CC-BY-4.0 / 権利主張なし | **同梱可**（curated 派生） | Wikidata=CC0（人名/組織/製品 + 読み）。GeoNames=CC-BY-4.0（**帰属必須**）。CC-BY-SA の Wikipedia 本文は **不使用**（share-alike 回避）。郵便データは権利主張なし（帰属歓迎） |
| `technical_terms_lexicon` | プロジェクト自作 + CC0/CC-BY 上流 | Apache-2.0（自作分）/ 上流に従う | **同梱可** | リポジトリ内で手入れ。外部由来分は上流ライセンス・帰属を ThirdPartyNotices に記載 |
| `user_dictionary` / `auto_words` / `app_specific_dictionary` | ユーザー生成 | N/A（ユーザーデータ） | **非配布**（ローカルのみ） | 配布物に含めない。`%LOCALAPPDATA%` に保存（`docs/sideload-packaging-spec.md` §1.4 / §9） |

一次情報（取り込み時に再検証する URL）:

- SudachiDict LEGAL: `https://github.com/WorksApplications/SudachiDict/blob/develop/LEGAL`
- mecab-ipadic-NEologd COPYING / README: `https://github.com/neologd/mecab-ipadic-neologd`
- azooKey_dictionary_storage: `https://github.com/ensan-hcl/azooKey_dictionary_storage`（Apache-2.0）
- Wikidata licensing（CC0）: `https://www.wikidata.org/wiki/Wikidata:Licensing`
- GeoNames（CC-BY-4.0）: `https://www.geonames.org/about.html`

補足:

- **SudachiDict を同梱の主力**とし、固有名詞・新語の相当部分を Apache-2.0 の
  クリーンな単一ソースで賄う。これにより NEologd を同梱しなくても M53 v1 の
  named_entity / neologism 改善が成立する（NEologd は更なる新語のための
  optional pack に限定）。
- **UniDic**（SudachiDict 内包）は GPL-2.0 / LGPL-2.1 / 修正 BSD のトリプル
  ライセンス。SudachiDict と同様に **BSD-3 オプション**で取り扱う（GPL 伝播なし）。
- **mozc 辞書**（BSD-3-Clause）は採用しない（base + SudachiDict で充足）。将来
  base 強化が必要になった場合の代替候補としてのみ記録する。
- 判定理由の核心は「**Apache-2.0 / BSD-3-Clause（SudachiDict 内包 UniDic）/
  CC0 / CC-BY / 権利主張なし（パブリックドメイン相当）のクリーンソースのみを
  同梱し、上流データに個別条件が付く NEologd は同梱せず opt-in の別 pack DL に
  分離する**」。
  これにより「ライセンス未確認のまま同梱した場合の配布差し止め」リスクを回避する。

### 14.10 パッケージング方式と第三者帰属（ThirdPartyNotices）

| 方式 | 対象 layer | 配置 | 更新 |
|---|---|---|---|
| **同梱（bundled）** | base / sudachi(core) / named_entity / technical_terms | MSIX 内 read-only データ（`docs/sideload-packaging-spec.md` §1） | アプリ更新時（§14.6） |
| **別 pack DL（optional）** | neologd | SHA256 検証 DL → `%LOCALAPPDATA%\azooKey\packs\` | neologd pack follow-up・既定無効（下記） |
| **非配布（local-only）** | user / auto_words / app_specific | `%LOCALAPPDATA%\azooKey\data\` | ランタイム |

**`neologd_lexicon` pack の取り込み経路（follow-up。M36-B とは別作業）**:
M36-B（§5）は `trending-words.json` を WinHTTP で DL → SHA256 検証 →
`AutoWordStore` へ取り込む経路であり、**mecab-ipadic-NEologd を `DictionaryStore`
の `neologd_lexicon` 層へ入れる経路ではない**。`neologd_lexicon` pack は次を要する
**別 follow-up**であり、M53 v1 / M36-B の成果物には含めない（追跡は Linear）:

- **pack 形式**: mecab-ipadic-NEologd を §14.2 のエントリ形式へ変換した
  コンパイル済み DictionaryStore 層アーティファクト（生 NEologd ソースではない。
  ファイル名/マニフェストは `neologd_lexicon.*` 識別子）。実体の形式は
  §15.5 の `.azdic` v1 であり、同梱層と同一のリーダで読む。
- **DL/検証**: M36-B / M32 が共有する `HttpDownloader`（§5-4）+ SHA256 検証
  基盤を**再利用**して `%LOCALAPPDATA%\azooKey\packs\` へ取得（既定無効・opt-in）。
- **ローダ**: pack を `neologd_lexicon` 層としてロードする `DictionaryStore`
  ローダ（§14.7）。M36-B の `AutoWordStore::IngestTrending` とは別経路。

帰属（**ThirdPartyNotices**）:

- 同梱辞書の全ライセンス（Apache-2.0 LICENSE + NOTICE、UniDic BSD-3 著作権表示、
  GeoNames CC-BY-4.0 クレジット、郵便データ出典）を 1 つの
  `ThirdPartyNotices.txt` に集約して MSIX に同梱し、設定アプリのライセンス画面
  （`docs/sideload-packaging-spec.md` §3.2 のバージョン/ライセンス導線）から参照
  可能にする。
- **bundled の SudachiDict(core) は NEologd 由来データを内包する**（LEGAL が
  `core_lex.csv` / `notcore_lex.csv` の由来として明記）。Apache-2.0 の
  NOTICE 伝播の一部として、**SudachiDict LEGAL / NOTICE が列挙する内包データの
  帰属（Hatena キーワード / 日本郵便 / 駅名 / 人名 等）を ThirdPartyNotices に
  含める**。standalone の mecab-ipadic-NEologd 単体パックを同梱しないこと
  （下記配布ガード）と、SudachiDict 内包データの帰属を載せることは両立する。
- **GeoNames を含む場合は CC-BY-4.0 の帰属表示が必須**（リンク付きクレジット）。
- standalone の mecab-ipadic-NEologd 単体パックは同梱しないため、その**単体配布
  としての** notices は ThirdPartyNotices に不要（別 pack DL 時に上流ライセンス/
  帰属を DL 画面で提示する）。ただし上記のとおり SudachiDict 内包の NEologd 由来
  データの帰属は同梱物の一部として含める。
- **配布ガード**（受け入れ条件 §14.13）: MSIX 構築時に同梱アセットへ
  **standalone の mecab-ipadic-NEologd パック（`neologd_lexicon` 層アセット）**
  が混入しないことを CI でチェックする。ガードの対象は NEologd 単体パックで
  あり、**SudachiDict(core) が Apache-2.0 で内包する NEologd 由来データは対象外**
  （SudachiDict は配布物全体が Apache-2.0 であり同梱可、§14.9）。判定は
  ファイル名/マニフェスト（`neologd_lexicon.*` 等の pack 識別子）で行い、
  「NEologd 由来の語彙が含まれるか」ではなく「NEologd 単体パックが同梱物に
  あるか」で評価する。

### 14.11 レイヤ優先度と dictionary_score 係数の確定

§14.5 の各因子の確定係数。`dictionary_score` は **DictionaryStore 内候補のランク
付け**と、Zenzai n-best への **merge 時のブースト量**に用いる（Zenzai 候補との
最終統合ランキングは M56 Tiny Reranker の責務であり本節の対象外）。

`source_priority`（層別の加算定数。大きいほど優先）:

| layer | source_priority |
|---|---|
| `user_dictionary` | 1.00 |
| `auto_words`（confirm 済み） | 0.85 |
| `app_specific_dictionary` | 0.70 |
| `auto_words`（auto mined） | 0.55 |
| `technical_terms_lexicon` | 0.50 |
| `named_entity_lexicon` | 0.45 |
| `neologd_lexicon` | 0.35 |
| `sudachi_lexicon` | 0.30 |
| `base_lexicon` | 0.20 |

その他の因子:

| 因子 | 確定式 / 既定値 | 範囲 |
|---|---|---|
| `base_frequency` | エントリ `frequency`（§14.2） | 0.00–1.00 |
| `exact_reading_bonus` | 完全一致 +0.10 / alias 一致 +0.05 / 緩い長音一致 +0.02（§14.3 の評価順） | 0.00–0.10 |
| `category_bonus` | `clamp(max_c(resolve(c)) − 1.0, 0.00, 0.20)`。エントリの `category[]`（§14.2。§14.12 dedup で union）に対し各 `c` を `resolve(c)` へ解決し**最大値**を採る（集約規則）。**boost-only**: `categoryBoosts` は [1.0, 1.2] に検証し降格に使わない | 0.00–0.20 |
| `obsolete_penalty` | `0.10 × (1 − 2^(−Δdays / half_life))`。Δdays = 最終使用からの日数。half_life は §14.4 category（一般 30 / 固有名詞 90 / 技術 120 / 新語 60 日）。**usage timestamp を持つ層（user / auto_words / app_specific）のみ**適用。静的同梱層は 0 | 0.00–0.10 |

`category_bonus` の **集約規則**: エントリが複数 category を持つ場合
（§14.2 の `category[]` 配列、§14.12 の dedup union）、各 category `c` を
`resolve(c)` = `categoryBoosts[c]`（**§14.8 の具体値優先**。未設定かつ
named_entity 系（`person_name` / `place_name` / `station_name` / `product_name` /
`company_org`）なら umbrella `categoryBoosts.named_entity`、それ以外で未設定なら
`1.0`）へ解決し、その **最大値**を採る（first/sum ではなく max。決定的・再現可能）。
例: `category=["technical","proper_noun"]` で `categoryBoosts.technical=1.0`・
`proper_noun` 未設定（umbrella 非該当）→ `max(1.0, 1.0)=1.0` → `category_bonus=0.00`
（§14.11 worked example と整合）。

M48 の `candidateTagBoosts`（候補タグ boost）は `dictionary_score` の因子では
**ない**（§14.5）。M48 タグ boost は `docs/app-profile-spec.md` §7 が候補の
`final_score` に対し §7 正準の clamp 式
`final_score *= min(3.0, max(1.0, candidateTagBoosts[tag]))`（[1.0, 3.0]）を **1 回
だけ**適用する（DictionaryStore 由来候補にも §14.12 で付与した単一タグに基づき
同経路で作用。二重適用を避けるため `dictionary_score` には入れない）。

worked example（`code.exe` 前面・`candidateTagBoosts.Technical = 1.5`、エントリ
"TensorRT" / `technical_terms` / frequency 0.72 / 完全一致 / `categoryBoosts.technical = 1.0`）:

```
dictionary_score
  = 0.72 (base_frequency)
  + 0.50 (source_priority: technical_terms)
  + 0.10 (exact_reading_bonus: 完全一致)
  + 0.00 (category_bonus: clamp(1.0 − 1.0, 0, 0.20))
  − 0.00 (obsolete_penalty: 静的層)
  = 1.32
```

その後、M48 タグ boost は app-profile-spec §7 が `final_score` に対して別途
適用する（"TensorRT" は §14.12 で `Technical` タグが付くため
`final_score *= min(3.0, max(1.0, 1.5))`（= 1.5）。`dictionary_score` 内では二重適用しない）。
`dictionary_score` の正規化は不要（DictionaryStore 内の相対ランク + merge
ブースト量として使用）。本例（`dictionary_score = 1.32`）は単体テストの
期待値とする（§14.13）。

### 14.12 source tagging 戦略と M48 連携

**source tagging**:

- 各候補は `source`（由来 LayerId）と `category[]`（§14.4）を保持する（§14.2）。
- 層横断の **dedup**（同一 `normalized_reading` かつ同一 `surface`）では、
  `source_priority + base_frequency` が最大のエントリを残し、`category` 配列は
  **union**、寄与した全層を `sources[]` provenance として記録する（スコアは
  勝者層の `source_priority` を用いる）。
- `sources[]` provenance は debug probe（M9 / D-09）・ETW（M33）にのみ出力し、
  ユーザー可視 UI には出さない。

**category → 候補タグ マッピング**（候補に単一タグを **付与**し、M48
`candidateTagBoosts` を `final_score` 段で適用可能にするため。タグ boost 自体は
app-profile-spec §7 が 1 回適用し `dictionary_score` には入れない。§14.5）:

| 辞書 category（§14.4） / 条件 | 候補タグ（M52） |
|---|---|
| `software` / `technical` / `product_name` | `Technical` |
| surface が ASCII/ラテン文字主体（例 "TensorRT", "iPhone"） | `English` |
| その他（`person_name` / `place_name` / `station_name` / `company_org` / `anime_game` / `neologism` / `general`） | なし（既定） |

- **候補タグは単一（スカラ）**。候補モデルは `docs/rich-features-spec.md`
  X-2-3 の `CandidateTag tag`（IPC `tag: uint8`）でタグを 1 つだけ保持する。
  複数行に該当する候補（例 "TensorRT" = `Technical` かつ surface=ASCII）には
  **precedence で 1 つだけ付与**する: **`Technical` > `English` > なし**
  （辞書 category 由来タグを surface 形式由来タグより優先）。両タグの同時保持・
  同時 boost は行わない（multi-tag 化は X-2-3 / IPC のスキーマ変更を要し本仕様
  の前提外。将来 `CandidateTag` がリスト化されれば本 precedence を緩和できる）。
- 付与された単一タグに対する M48 boost は **app-profile-spec §7** が
  §7 正準の clamp 式 `final_score *= min(3.0, max(1.0, candidateTagBoosts[tag]))`（[1.0, 3.0]）として 1 回適用する
  （"TensorRT" は `Technical` が選ばれる）。`dictionary_score`（§14.11）には
  含めない（二重適用回避）。
- 候補タグの確定 taxonomy は M52 ベンチで定義する。上表は既知タグ
  （`Technical` / `English`）への写像であり、未知タグは「なし」とする。
- 固有名詞系のスコア寄与は **`category_bonus`（§14.8 の `categoryBoosts` +
  `named_entity` umbrella、`dictionary_score` 内）** で行い、候補タグ経由では
  ない。M48 タグ boost は候補タグ（前面アプリ文脈）専用で `final_score` 段
  （§7）に作用し、両者は別 setting key・別 namespace・別段（§14.5 と整合）。
- `app_specific_dictionary` 層自体は §14.11 の `source_priority`（0.70）で
  スコアされる DictionaryStore 層であり、M48 の `candidateTagBoosts`（§7 の
  `final_score` 乗算）とは独立に働く（層スコア + タグブーストの二経路）。

### 14.13 M53 受け入れ条件

受け入れ条件の定義の正典は [`plans/windows-port-roadmap.md`](../plans/windows-port-roadmap.md)
の M53 節とする。本書は辞書層の構成・同梱判定・スコア係数・帰属表示を定義し、受け入れ条件を
複製しない。物理層（`.azdic`）の受け入れ条件は §15.11 が定義し、roadmap M53 から参照される。

**実装上の留意点**:

- M53 v1 のベンチ対象は同梱の `sudachi_lexicon`（core 版。NEologd 由来データを Apache-2.0
  で内包）+ `base_lexicon` の範囲に限る。`neologd_lexicon`（別 DL pack）有効時の追加改善は
  §14.10 の neologd pack follow-up（M36-B とは別作業）完了時に、当該 pack を有効化した
  構成で評価する。
- §14.10 の配布ガードが除外するのは **standalone NEologd 単体パック**であり、SudachiDict が
  内包する NEologd 由来データは対象外。
- `app_specific_dictionary` layer は M48 未完了時に空（無効）として扱う。

## 15. M53 追補（DEV-412）: system 辞書の物理層

§14 は辞書層の**論理設計**（層構成・エントリ意味・スコア・ライセンス判定）を定める。
本章はその下にある**物理層**、すなわち読みの索引構造・ディスク上の直列化形式・
複数上流からのビルド手順を定める。DEV-412（system 辞書 double-array trie 化 +
マルチソースビルド）の実装はこの章を正典とする。

### 15.1 スコープと責務境界

| 対象 | 物理表現 | 本章の扱い |
|---|---|---|
| `base_lexicon` / `sudachi_lexicon` / `named_entity_lexicon` / `technical_terms_lexicon` | ビルド済み **`.azdic` アーティファクト**（double-array trie + エントリプール） | §15.2〜§15.6 |
| `neologd_lexicon`（別 pack DL） | 同じ `.azdic` 形式 | §15.5（§14.10 の「コンパイル済み層アーティファクト」は本形式を指す） |
| `user_dictionary` / `auto_words` / `app_specific_dictionary` | 既存の `std::map` + JSON / TSV を**維持** | §15.7 |

責務の置き場所は次のとおり。

- **`core/`**（`azookey_core`）: trie 構築・検索と `.azdic` の読み取り。辞書の意味論を
  持たない汎用データ構造として置く。`learning` は `core` に依存するため（`learning/CMakeLists.txt`）、
  この配置なら層の向きを崩さずに `DictionaryStore` から使える。
- **`learning/`**: `DictionaryStore`（§14.7）と各 `IDictionaryLayer` 実装。層の有効化・
  dedup・スコアは §14 の責務。
- **`dictbuild/`**（新規のホストツール）: 上流データから `.azdic` を生成するオフライン
  ビルダ。IME 実行時には存在しない。

ランタイムは `.azdic` を**読むだけ**であり、書かない。書き込みが要る層は §15.7 の
mutable 層に限る。

### 15.2 trie 実装の選定

**決定: `.azdic` 内の double-array trie は自前実装**（`core/src/DoubleArrayTrie.cpp`、
構築側は `dictbuild` と共有）とし、第三者 trie ライブラリはベンダリングしない。

| 候補 | 評価 |
|---|---|
| **自前 double-array**（採用） | 実行時は base / check 配列の走査のみで、追加の再配布義務が発生しない。構築側もキー整列済み入力に対する素直な配置で足りる |
| darts-clone | MeCab 系で実績のある単一ヘッダ実装。ただし採用しても §15.5 のコンテナ（セクション構成・整合性検証・mmap 境界）は自作になり、削減できるのは trie 本体に限られる |
| marisa-trie | LOUDS 系でサイズは有利だが、デュアルライセンス構成のため取り込み時にライセンス選択の判断が要る。サイズ予算（§15.9）が実測で超過した場合の再検討候補として記録する |
| yada（karukan が使用） | Rust 実装であり移植不可。設計思想の参照にとどめる |

外部 trie ライブラリを後から採用する場合は、コードもデータと同じく
`docs/licensing-policy.md` の採用ワークフロー（ライセンス互換性の判定 → 三層 attribution →
`THIRD_PARTY_LICENSES` 追記 → 配布ガード）に載せる。上流ライセンスは版によって
変わり得るため、判定時に一次情報（各上流リポジトリの `COPYING` / `LICENSE`）を参照して
確定させる。本節は候補の記録であり、ライセンス判定を先取りしない。

### 15.3 キー空間と読み正規化の適用点

trie のキーは §14.3 で正規化した `normalized_reading` の UTF-8 バイト列とする。
§14.3 の各規則を、ビルド時とクエリ時のどちらで効かせるかを次のとおり確定する。

| §14.3 の規則 | 適用点 | 理由 |
|---|---|---|
| カタカナ → ひらがな | **ビルド時**（キーを正規化） | 上流ごとの表記差をアーティファクト境界で吸収する |
| 全角英数 → 半角英数 | **ビルド時** | 同上 |
| ヴァ / バ、づ / ず、ぢ / じ の alias | **ビルド時にキーを展開** | クエリ時展開は入力長に対して指数的に候補キーが増え、順序の決定性も落ちる |
| 長音「ー」の緩い一致 | **クエリ時**（「ー」を除去した二次検索） | 長音の位置は任意個所に現れ、ビルド時展開ではキー数が発散する |

alias 展開の上限は 1 エントリあたり **8 キー**とする。超過するエントリは完全一致キーのみを
登録し、ビルドログに警告を出す（無音で語彙を落とさない）。展開されたキーには
`MatchKind::Alias` を、元のキー経由の参照には `MatchKind::Exact` を持たせ、§14.11 の
`exact_reading_bonus`（完全一致 +0.10 / alias 一致 +0.05 / 緩い長音一致 +0.02）を
検索結果から一意に決められるようにする。alias 展開された参照は元のキーと同じ終端に
合流しうるため、`MatchKind` はキー単位ではなく**キーからエントリへの参照**が持つ
（§15.5 の `EIDX`）。

クエリ側の評価順は §14.3 のとおり完全一致 → alias 一致 → 緩い長音一致とする。
alias キーは同じ trie に載るため、正規化済みキーでの 1 回の検索が `MatchKind::Exact` と
`MatchKind::Alias` の両方を返す。この検索が空を返したときに限り、長音を除去したキーで
再検索し、その結果に `MatchKind::LongVowelRelaxed` を付ける。評価順は結果の並べ替えで
表現し、上位の一致があっても下位の一致を捨てない（`exact_reading_bonus` の差として
スコアに現れる）。

エントリが保持する `reading`（§14.2）は正規化前の原表記であり、正規化は
`normalized_reading` 側にだけ効く。表示・エクスポートは `reading` を使う。

### 15.4 検索 API の契約

**探索の向きが逆な 2 つの検索を別 API に分ける。** 混同すると、同じモード名で静的層と
mutable 層が別の結果を返すことになる。

- **common-prefix 検索**: 登録キーが**入力の接頭辞**であるものを返す。入力
  「とうきょうと」に対し「とう」「とうきょう」が返る。変換対象の読みを辞書語で切り出す
  向きであり、完全一致（`ExactMatch`）はこの特殊形。
- **predictive 検索**: 登録キーが**入力で始まる**ものを返す。入力「とう」に対し
  「とうきょう」「とうきょうと」が返る。入力途中の読みを補完する向きであり、M15 予測候補
  ウィンドウが要求するのはこちら。

```cpp
namespace azookey::core {

enum class MatchKind : uint8_t { Exact = 0, Alias = 1, LongVowelRelaxed = 2 };

struct PrefixMatch {
  uint32_t key_length;      // マッチしたキーのバイト長（UTF-8 の文字境界で終わる）
  uint32_t key_id;          // KEYS セクションの index（§15.5）
  uint32_t entry_ref_off;   // EIDX セクション内のオフセット（§15.5）
  uint32_t entry_count;     // このキーに紐づくエントリ数
};

class DoubleArrayTrie {
 public:
  // 登録キーのうち key の接頭辞であるものを返す。max_results == 0 は無制限。
  void CommonPrefixSearch(std::string_view key, size_t max_results,
                          std::vector<PrefixMatch>& out) const;

  // 登録キーのうち key で始まるものを返す（key 自身を含む）。
  void PredictiveSearch(std::string_view key, size_t max_results,
                        std::vector<PrefixMatch>& out) const;

  // key 全体に一致する登録キーがあれば true。CommonPrefixSearch の
  // key_length == key.size() の項と同義。
  bool ExactMatch(std::string_view key, PrefixMatch& out) const;
};

}  // namespace azookey::core
```

共通の契約:

- **入力**は §15.3 で正規化済みの UTF-8 バイト列とする。正規化は呼び出し側の責務であり、
  `core` が提供する `NormalizeReading()` を通すこと。未正規化の入力を渡した場合、
  結果が空になることはあっても未定義動作にはならない。
- **出力順序**は `key_length` の昇順、同一長では `key_id` の昇順とする。同じ
  アーティファクトと同じ入力に対して常に同じ列を返す（決定的）。`PrefixMatch` は
  一致種別を持たない。同じキーに `Exact` と `Alias` のエントリが混在しうるため、
  `MatchKind` はエントリ単位で `EIDX` が持つ（§15.5）。
- **エントリの取り出し**は `EIDX[entry_ref_off .. entry_ref_off + entry_count)` が指す
  `ENTS` レコードを読む（§15.5）。`PrefixMatch` 自体はエントリの中身を持たない。
- **UTF-8 境界**: マッチは必ず文字境界で終わる。ビルド時に登録するキーが文字境界で
  終わるため、バイト単位走査でもこの性質が保たれる。不正な UTF-8 を含む入力は、
  一致しなかったものとして扱い、例外を投げない。
- **空入力**・欠落アーティファクトに対しては空の結果を返す。例外・アサートで落とさない。
- **`max_results` に達した場合**は `key_length` の短いものから順に埋めた時点で打ち切る。
  戻り値からは打ち切りの有無を区別できないため、上限は用途ごとに十分な値を呼び出し側が
  与える（§15.8）。

検索ごとに異なる点:

| | `CommonPrefixSearch` | `PredictiveSearch` |
|---|---|---|
| 返すキー | 入力の接頭辞である登録キー | 入力で始まる登録キー |
| 計算量 | 入力長 `n`、返却件数 `m` に対して O(n + m) | 入力長 `n`、返却件数 `m`、走査ノード数 `k` に対して O(n + k)。`k` は `max_results` で抑える |
| 打ち切り | 実用上ほぼ起きない（`m ≤ n`） | 起きうる。M15 は候補窓の表示件数に見合う上限を与える |

`PredictiveSearch` は入力に対応するノードまで遷移したあと、そのノードの部分木を
たどって終端を集める。double-array では、ノード `s` の子は `check[base[s] + c] == s`
を満たすバイト `c` として得られる。走査は `c` の昇順（深さ優先）で行い、これにより
出力がキーのバイト辞書順になる。`max_results` に達した時点で走査を打ち切る。

呼び出し側は次の 3 つを想定する。

1. `DictionaryStore::Lookup`（§14.7）の完全一致経路。`ExactMatch` を使う。
2. M15 予測候補ウィンドウへの補完供給。`PredictiveSearch` を使う。
3. 将来のラティス分割（複合語の切り出し）。`CommonPrefixSearch` を使う。M53 の範囲外
   であり、契約だけを満たしておく。

### 15.5 直列化フォーマット `.azdic` v1

単一ファイル、リトルエンディアン固定、全セクション 8 バイト境界、メモリマップ可能とする。
Windows は `CreateFileMappingW` / `MapViewOfFile`、テスト用の POSIX 経路は `mmap` を使い、
マップに失敗した場合は全読みにフォールバックする。

ヘッダ（64 バイト固定）:

| offset | size | field | 内容 |
|---|---|---|---|
| 0 | 8 | `magic` | `"AZDIC1\0\0"` |
| 8 | 2 | `format_version` | u16。v1 は `1` |
| 10 | 2 | `header_size` | u16 = 64 |
| 12 | 4 | `flags` | u32。bit0 = alias キー展開済み（§15.3）。未定義ビットは 0 |
| 16 | 4 | `layer_id` | u32。§14.1 の層識別子 |
| 20 | 4 | `entry_count` | u32 |
| 24 | 4 | `key_count` | u32 |
| 28 | 4 | `section_count` | u32 |
| 32 | 8 | `content_hash` | u64。先頭 `header_size` バイトを除く全バイト（セクションテーブルを含む）の FNV-1a 64 |
| 40 | 24 | `reserved` | 0 埋め |

セクションテーブルは offset 64 から `section_count` 個、1 エントリ 24 バイト
（`u32 type` / `u32 reserved` / `u64 offset` / `u64 size`）。

| type | 内容 |
|---|---|
| `TRIE` | double-array の base / check 配列。受理されたキーの値は `key_id`（u32） |
| `KEYS` | キーレコード列（`key_count` 個、8 バイト固定）。下記 |
| `EIDX` | エントリ参照レコード列（8 バイト固定）。キーからエントリへの間接参照 |
| `ENTS` | 固定長エントリレコード列（下表） |
| `STRS` | 文字列プール（UTF-8 の連結。終端子なし） |
| `META` | UTF-8 JSON。由来・ライセンス・ビルドレシピ（§15.6） |

**キーからエントリへの対応**。trie は受理したキーに対して `key_id` だけを返す。
`KEYS[key_id]` は 8 バイト固定で `u32 entry_ref_off` / `u32 entry_count` を持ち、実エントリは
`EIDX[entry_ref_off .. entry_ref_off + entry_count)` が指す `ENTS` のレコードである。
`EIDX` の 1 要素も 8 バイト固定で、`u32 entry_index`（`ENTS` の index）/ `u8 kind`
（`MatchKind`。ビルド時に確定するのは `Exact` か `Alias`）/ `u8 reserved[3]` を持つ。

`EIDX` を挟むのは、alias キー（§15.3）がエントリの複製なしに同じ `ENTS` レコードを
指せるようにするためである。`ENTS` は `(normalized_key, surface, source)` の昇順に
並ぶため、1 つの alias キーに集まるエントリは連続しない場合がある。`EIDX` があれば
任意の集合を 1 つの範囲として表現できる。ヘッダの `entry_count` は `ENTS` の
レコード数（論理エントリ数）であり、キー展開による重複を含まない。`EIDX` 内の並びは
`entry_index` の昇順とする（決定的）。

**`kind` をキー単位ではなくエントリ単位で持つ**理由は、同じ正規化キーに一致種別の異なる
エントリが集まるためである。「ば」を元キーとする語と、「ヴァ」から alias 展開されて「ば」に
なった語は同じ終端（同じ `key_id`）に集まるが、前者は完全一致 +0.10、後者は alias 一致
+0.05 で採点する（§14.11）。キー単位の `kind` ではこの区別ができない。

長音緩和の二次検索（§15.3）が返す一致は、`EIDX` の `kind` が何であっても
`MatchKind::LongVowelRelaxed` として報告する。二次検索は緩い一致であり、
`exact_reading_bonus`（§14.11）は最も弱い値を採るのが正しいためである。

エントリレコード（32 バイト固定）:

| offset | size | field |
|---|---|---|
| 0 | 4 | `surface_off`（`STRS` 内オフセット） |
| 4 | 4 | `surface_len` |
| 8 | 4 | `reading_off`（正規化前の原表記） |
| 12 | 4 | `reading_len` |
| 16 | 2 | `pos_id`（`META.pos_table` の index） |
| 18 | 2 | `category_mask`（§14.4 のカテゴリのビットマスク） |
| 20 | 2 | `cost`（i16、mecab 互換） |
| 22 | 2 | `frequency_q`（u16。`frequency × 65535` の量子化） |
| 24 | 1 | `source`（層識別子） |
| 25 | 1 | `priority_q`（u8。`priority × 255`） |
| 26 | 6 | `reserved` |

§14.2 の `created_at` / `updated_at` は `.azdic` に持たない。静的層は
`obsolete_penalty` が常に 0（§14.11）で日付を必要とせず、日付を埋めるとビルドの
再現性（§15.6）を壊すためである。アーティファクトの世代は日付ではなく、`META` が持つ
上流リビジョンとビルダ版で識別する。

`priority`（§14.2）は §14.11 のスコア式に現れない。v1 のビルダは層固定値
（当該層の `source_priority`）を `priority_q` に書き込むが、**スコア計算には使わない**。
`source_priority` を層から引くのと二重に加算すると §14.11 の確定係数が崩れるためで、
`priority_q` は debug probe 用の informational フィールドとして扱う。

検証は 3 段に分ける。**どの段でも、違反を検出したらクランプや切り詰めで続行せず拒否する。**
範囲外のオフセットを有効範囲へ丸めると、別のエントリや別の文字列を正しい値として読んで
しまい、破損を検出せずに誤った候補を出すことになる。

**(1) 構造検証（ロード時に必ず実行、O(1)）**。以下をすべて満たさなければ拒否する。
算術はすべて 64 bit で行い、加算前に残余を比較して overflow を避ける
（`off > file_size || size > file_size - off` の形で判定し、`off + size` を作らない）。

- `magic` 一致、`format_version` が既知、`header_size == 64`、`flags` に未知ビットが無い。
- セクションテーブル全体（`64 + section_count × 24` バイト）がファイル長に収まる。
- 必須セクション（`TRIE` / `KEYS` / `EIDX` / `ENTS` / `STRS` / `META`）がそれぞれ
  **ちょうど 1 個**存在する。重複した `type` は拒否する。未知の `type` は無視してよい
  （前方互換）が、重複判定の対象からは外さない。
- 各セクションの `offset` が 8 の倍数で、`offset` と `size` がファイル長に収まり、
  他のセクションおよびヘッダとセクションテーブルの領域と**重ならない**。
- 固定長レコードのセクションは `size` がレコード長の倍数であり、件数がヘッダと一致する:
  `KEYS.size == key_count × 8`、`EIDX.size % 8 == 0`、`ENTS.size == entry_count × 32`。
- `TRIE` の `size` が double-array の要素長の倍数である。

**(2) 参照検証（逆参照の時点、O(1)）**。内部参照は全件走査せず、実際に読む瞬間に検査する。
違反を 1 件でも検出した層は、その場で無効化して以後は空を返す。検査項目は次のとおり。

- trie が返す `key_id < key_count`。
- `KEYS[key_id]` について `entry_ref_off ≤ EIDX.count` かつ
  `entry_count ≤ EIDX.count − entry_ref_off`。
- `EIDX[i].entry_index < entry_count`、`EIDX[i].kind` が既知の `MatchKind`。
- `ENTS[j]` について `surface_off ≤ STRS.size` かつ `surface_len ≤ STRS.size − surface_off`。
  `reading_off` / `reading_len` も同様。`pos_id < META.pos_table` の要素数、
  `source` が既知の層識別子。

全件を先に走査しないのは、`ENTS` が数十万件規模になり、ロード予算（§15.9 の 20 ms）を
超えるためである。逆参照時の検査は分岐 2 つで済み、検索のレイテンシ予算には影響しない。

**(3) 全体検証（明示的に要求されたときのみ、O(N)）**。`content_hash` の照合と、(2) の
参照検証の全件走査を行う。実行するのは pack の DL 直後、`dictbuild --verify`、および設定
`dictionary.verifyOnLoad` が真のときとする。

いずれの段でも、検出した層**のみを無効化**し、他層はそのまま動作を続ける。無効化は
§14.8 の missing-pack と同じ扱い（layer が空として振る舞う）とし、検出した検証条件を
ログに残す。

`.azdic` は §14.10 の pack 配布経路でもそのまま使う。pack 全体の SHA256 検証は
ダウンローダ側（M36-B / M32 の `HttpDownloader` 基盤）の責務であり、`content_hash` は
アーティファクト内部の自己整合性チェックとして独立に持つ。

### 15.6 マルチソースビルドパイプライン

`dictbuild` は次の段を順に通す。CMake からは既定 OFF のオプション
（`AZOOKEY_BUILD_DICT_TOOLS`）で構築し、IME の配布物には含めない。

1. **extract**: 上流データを中間 TSV（`*.lex.tsv`、UTF-8 / LF）へ変換する。上流ごとに
   スクリプトが分かれる。中間 TSV の先頭には `docs/licensing-policy.md` の
   ファイルヘッダ層として、由来元・ライセンス・`THIRD_PARTY_LICENSES` への参照を
   コメント行で置く。
2. **normalize**: §15.3 の規則で `normalized_reading` を作り、alias キーを展開する。
3. **score**: `cost` と `frequency` を確定する（下記）。
4. **merge**: 層内で重複を解決する（下記）。dedup は確定済みの `frequency` を見るため、
   score より後に置く。
5. **build**: `ENTS` を `(normalized_key, surface, source)` の昇順に確定させたうえで、
   キーを整列して double-array を構築し、`KEYS` / `EIDX` を作って `.azdic` を書き出す。
6. **verify**: 書き出したアーティファクトを読み直し、全キーの `ExactMatch` が入力と
   一致すること、`content_hash` が一致することを確認する。

**層内 dedup**（ビルド時）は、§14.12 の層をまたぐ実行時 dedup とは別の規則を使う。
キーは `(normalized_reading, surface)` とし、衝突したときは `frequency` が最大の
エントリを残す。`category` は union、`cost` は最小値（強いほう）を採る。`frequency` が
同点のときは入力ファイル名の昇順、次いで行番号の昇順で勝者を決める（決定的）。

**`cost` と `frequency` の写像**。上流が mecab 系コストを持つ層（SudachiDict 等）は、
上流 `cost` を `[-32768, 32767]` にクランプしてそのまま格納する。コストを持たない
curated 層（`named_entity` / `technical_terms`）の既定は `5000` とする。
`frequency` は層内統計に依存しない固定スケールで導く。

```
frequency = clamp((8000 - cost) / 10000.0, 0.0, 1.0)
```

`cost >= 8000` は 0.0、`cost <= -2000` は 1.0 に飽和する。層内の分布で正規化しないのは、
上流の更新でスケールが動くとアーティファクトの再現性と §14.11 の worked example が
崩れるためである。中間 TSV が `frequency` を明示している場合はその値を優先し、上式は
適用しない（curated 層で人手の重み付けを効かせるため）。

**再現性**。同じ入力と同じビルダ版からはバイト単位で同一の `.azdic` が出る。キーの整列は
`(normalized_key, surface, source)` のバイト昇順、タイムスタンプ・絶対パス・並列実行順序は
アーティファクトに入れない。CI は同一入力で 2 回ビルドし、`content_hash` の一致を確認する。

**上流データの取得**は `AZOOKEY_DICT_SOURCE_DIR` で与えるローカルツリーを基本とし、CI では
SHA256 でピンした取得を使う。生の上流データは配布物に含めず、同梱するのは変換後の
`.azdic` だけである（§14.10 の同梱判定はアーティファクトに対して適用する）。

**帰属**は `META` セクションに持たせる。各アーティファクトは寄与した上流ごとに次を保持し、
`ThirdPartyNotices.txt` はこの `META` から生成する。

| フィールド | 内容 |
|---|---|
| `source_id` | 上流の識別子（`sudachidict-core` など） |
| `spdx` | SPDX 識別子。共通ライセンス本文の引き当てに使う |
| `upstream_url` | 一次情報の URL |
| `upstream_revision` | 取り込んだ版（タグ・コミット・リリース名） |
| `transform` | 変換スクリプトの識別子（再生成注記） |
| `copyright` | 上流固有の著作権表示（逐語） |
| `notice_ids` | notice catalog のエントリ ID 配列。空配列は「付随 notice 無し」を意味する |

SPDX 識別子だけでは `ThirdPartyNotices.txt` を作れない。共通ライセンス本文は引けても、
上流固有の著作権表示、Apache-2.0 の `NOTICE`、SudachiDict LEGAL が列挙する内包データの
帰属（§14.10）、GeoNames の CC-BY-4.0 クレジット文は復元できないためである。これらは
リポジトリ内の **notice catalog**（`dictbuild/notices/<notice_id>.txt`、逐語のテキスト）に
置き、`META` は `notice_ids` でそれを参照する。カタログ本文はリポジトリで版管理され、
アーティファクトのビルドとは独立に差分がレビューできる。

生成の失敗条件を次のとおり定める。`ThirdPartyNotices.txt` の生成は、`META` に
`source_id` / `spdx` / `upstream_url` / `upstream_revision` / `copyright` のいずれかを欠く
上流がある場合、`notice_ids` が参照する ID が catalog に存在しない場合、いずれもエラーで
停止する（欠落を空欄として出力しない）。生成が失敗した状態のアーティファクトは配布物に
入れられない（§14.10 の配布ガードで検出する）。

`docs/licensing-policy.md` の三層 attribution のうち、テキストファイルではファイルヘッダが
担う層を、バイナリでは `META` が担う。集約ファイル層は生成された `ThirdPartyNotices.txt` と
ルート `THIRD_PARTY_LICENSES`、再生成注記層は `transform` が満たす。

### 15.7 mutable 層との併存

`user_dictionary`（M9）/ `auto_words`（M36-A）/ `app_specific_dictionary`（M48）は
`.azdic` へ移さない。既存の `user_dict.json`（`UserDictionary` の version 1 スキーマ）と
`auto_words.tsv` をそのまま使い、§14.13 の後方互換条件を維持する。

double-array は構築が全件走査であり、1 語の追加ごとに再構築するには向かない。これらの層は
即時反映・部分更新・quarantine つき atomic write という要件を持ち、`std::map` を基盤とする
現行実装のほうが要件に合う。

2 方向の検索（§15.4）はデータ構造を変えずに供給する。`std::map` はキー昇順であるため、
**predictive 検索**は `lower_bound(input)` から走査し、キーが `input` で始まらなくなった
時点で止めれば O(log N + 一致数) で得られる。**common-prefix 検索**は `input` の各接頭辞を
短いものから順に `find` すれば、読みの文字数 `n` に対して O(n log N) で得られる。どちらも
静的層と同じ意味論を返す（§15.8 の `LookupMode` は層の物理表現によらず同じ結果集合を指す）。

`PrefixMatch`（§15.4）はアーティファクトのエントリプールを指す索引であり mutable 層には
存在しないため、層の境界で交換するのは `DictionaryEntry` である（§15.8）。結果の並びは
静的層と同じく読みの短いものから先に並べる。

読みの正規化（§14.3）は mutable 層にも効かせる。各層はロードと挿入の時点で
`normalized_reading` を算出してそれをキーにし、ユーザーが入力した原表記（`UserWord::ruby`）は
レコード側に保持する。alias と長音緩和は、静的層のようにキーを展開せず、クエリ時に
候補キーを生成して引く。層の規模が小さく、展開したキーを永続化すると既存の
`user_dict.json` スキーマを変えることになるためである。

想定規模はいずれの層も数千から数万件である。この範囲では走査の実測差が候補提示の
レイテンシ予算（§15.9）に現れない。エントリ数が 10 万件を超えた層には警告ログを出し、
物理表現の見直しを別課題として扱う。

### 15.8 層インターフェースとロード失敗時の挙動

§14.7 の `IDictionaryLayer` に、物理表現を隠す検索インターフェースを置く。

```cpp
enum class LookupMode {
  Exact,             // キーが入力と完全一致
  CommonPrefix,      // キーが入力の接頭辞（変換対象の切り出し）
  PredictivePrefix,  // キーが入力で始まる（入力途中の補完）
};

class IDictionaryLayer {
 public:
  virtual LayerId Id() const = 0;
  // アーティファクト欠落・検証失敗・pack 未取得のとき false。
  virtual bool IsAvailable() const = 0;
  virtual void Lookup(std::string_view normalized_reading, LookupMode mode,
                      size_t max_results,
                      std::vector<DictionaryEntry>& out) const = 0;
};
```

- `.azdic` を持つ静的層は §15.4 の trie を、mutable 層は §15.7 の `std::map` 走査を
  使う。`DictionaryStore` からは区別しない。同じ `LookupMode` に対し、どの層も同じ
  意味論の結果集合を返す。
- `DictionaryStore::Lookup`（§14.7）は `LookupMode::Exact`、M15 の予測候補ウィンドウは
  `LookupMode::PredictivePrefix` を使う。`LookupMode::CommonPrefix` は将来のラティス
  分割向けであり、M53 では呼び出し側を持たない。
- `max_results` は層ごとに上限を掛ける。全層の結果は §14.12 の dedup を通したあと、
  §14.11 の `dictionary_score` の降順に並べる。
- `IsAvailable()` が false の層は、`EnableLayer` で有効にされていても結果に寄与しない。
  設定上は有効だがアーティファクトが無い状態（未 DL の pack、破損したアーティファクト）は、
  §14.8 の missing-pack と同じユーザー可視挙動になる。

### 15.9 サイズとレイテンシの予算

| 項目 | 予算 |
|---|---|
| 同梱 `.azdic` 合計（MSIX 圧縮前） | 120 MB 以下 |
| 常駐 RSS 増分（mmap 前提、trie イメージのページインのみ） | 40 MB 以下 |
| 層 1 つのロード（mmap + ヘッダ検証） | 20 ms 以下 |
| `CommonPrefixSearch` の p95（読み 16 文字以内、warm） | 0.2 ms 以下 |
| `PredictiveSearch` の p95（読み 4 文字、`max_results` = 32、warm） | 0.5 ms 以下 |

これらは設計上の予算であり、上流の実語彙数によって達成可能性が変わる。SudachiDict
（core 版）を取り込む段階で実語彙数とアーティファクトサイズを実測し、超過する場合は
`sudachi_lexicon` の絞り込み（品詞・頻度による足切り）か、§15.2 の LOUDS 系実装への
切り替えのどちらを採るかを判断する。

trie 単体の検索レイテンシとロード時間は `bench/` 配下のマイクロベンチで測る。
`docs/conversion-quality-benchmark-spec.md` §6.3 の `latency_*` / `memory_peak_mb` は
変換全体の指標であり、辞書層の寄与はその差分として観測する。M52 ベンチの指標定義に
辞書層専用の項目は足さない。

### 15.10 テスト計画

| 種別 | 内容 |
|---|---|
| ゴールデン round-trip | 固定の小さな入力セットから `.azdic` を生成し、既知の `content_hash` と一致することを確認する。ビルドの再現性（§15.6）を回帰から守る |
| 参照実装との一致 | ランダム生成した読みの集合について、`CommonPrefixSearch` と `PredictiveSearch` の結果が `std::map` ベースの素朴な実装と順序込みで完全一致することを確認する。両者が探索の向きを取り違えていないこと（入力「とう」に対し predictive は「とうきょう」を返し、common-prefix は返さない）を個別のケースで確認する |
| 層をまたぐ意味論の一致 | 同じ語彙を静的層と mutable 層の双方に入れ、`LookupMode` ごとに両者が同じ結果集合を返すことを確認する（§15.8） |
| 正規化と alias | §15.3 の各規則について、ビルド時展開されたキーとクエリ時の長音緩和が、期待する `MatchKind` を返すことを確認する。alias キーが `EIDX` 経由で元エントリと同じ `ENTS` レコードを指し、`entry_count`（ヘッダ）が重複を含まないことも確認する |
| 破損耐性（構造） | `magic` 破壊 / `format_version` 不一致 / 未知 `flags` ビット / 必須セクションの欠落と重複 / 8 バイト境界違反 / セクションの重なり / 固定長セクションの `size` がレコード長の倍数でない / ヘッダの件数と `size` の不一致 / `offset` と `size` の範囲外（overflow を誘う値を含む）/ ファイル末尾の切り詰め / `content_hash` 不一致 |
| 破損耐性（内部参照） | `key_id ≥ key_count` / `entry_ref_off + entry_count > EIDX.count` / `EIDX[i].entry_index ≥ entry_count` / `EIDX[i].kind` が未知 / `surface_off + surface_len > STRS.size` / `reading_off + reading_len > STRS.size` / `pos_id` が `META.pos_table` の範囲外 / `source` が未知の層識別子 の各ケース |
| 破損時の縮退 | 上記いずれのケースでも、当該層のみが無効化されて空を返し、値がクランプされて別のエントリや文字列として読まれないこと、プロセスが落ちず他層が機能することを確認する |
| 後方互換 | 既存 `user_dict.json` / `auto_words.tsv` が層として読み込まれる（§14.13 の再掲） |
| 帰属生成 | `META` から生成した `ThirdPartyNotices.txt` に、寄与した全上流の SPDX と帰属が含まれることを確認する |

### 15.11 DEV-412 受け入れ条件

- `.azdic` v1 のリーダとビルダが往復し、同一入力から 2 回ビルドしたアーティファクトが
  バイト単位で一致する。
- `CommonPrefixSearch` と `PredictiveSearch` が §15.4 の契約（探索の向き・順序・UTF-8 境界・
  計算量・非例外）を満たし、参照実装との一致テストが緑である。静的層と mutable 層が同じ
  `LookupMode` に対して同じ結果集合を返す。
- 破損した `.azdic` を与えたとき、当該層だけが無効化され、他層の候補生成が継続する。
- `user_dictionary` / `auto_words` / `app_specific_dictionary` が既存の永続化形式のまま
  層として読み込まれ、前方一致を返す（§15.7）。
- 同梱アーティファクトの `META` が寄与した上流のライセンスと帰属を保持し、
  `ThirdPartyNotices.txt` に反映される（§14.10 / `docs/licensing-policy.md`）。
- 同梱 `.azdic` の合計サイズと各検索の p95 が §15.9 の予算に収まる。
  超過する場合は §15.9 の判断（絞り込みか実装差し替えか）を記録したうえで予算を改訂する。

### 15.12 実装着手時に確定する事項

本章で確定しないものを明示する。DEV-412 の実装で決め、決めた内容は本章へ反映する。

- SudachiDict（core 版）の取得経路。CI でのピン付きダウンロードか、リリースビルド用の
  事前生成アーティファクトかを、CI 実行時間と再現性の兼ね合いで決める。
- `META.pos_table` の品詞体系。SudachiDict の品詞体系をそのまま持つか、§14.2 の `pos`
  文字列へ写像した簡約体系にするか。
- §15.9 の予算を超えた場合の `sudachi_lexicon` 足切り基準（品詞・頻度のしきい値）。
