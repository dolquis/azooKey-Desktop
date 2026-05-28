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
- 既定パス: `%LOCALAPPDATA%\azooKey\auto_words.tsv`
  （host CLI `--auto-word-store` で上書き）
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
共通ヘルパ `inference-host/src/HttpDownloader.{h,cpp}`（新規）に切り出し、
`UpdateChecker`（M32）と `TrendingWordFetcher`（M36-B）で共有する。M36-B 着手者が
`HttpDownloader` を新設し、M32 はそれを利用するよう調整する。

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

設定ローダーは未実装のため、当面の実効値は host CLI 引数 / 環境変数で受ける
（M35 と同じ運用）:
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
| 新規 | `inference-host/include/azookey/host/HttpDownloader.h` / `src/HttpDownloader.cpp` | B |
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
  ├─ base_lexicon            (SimpleConverter 内蔵)
  ├─ sudachi_lexicon         (optional pack)
  ├─ neologd_lexicon         (optional pack, M36-B が更新)
  ├─ named_entity_lexicon    (bundled curated)
  ├─ technical_terms_lexicon (bundled curated)
  ├─ user_dictionary         (M9 の UserDictionary)
  ├─ auto_words              (M36-A の AutoWordStore)
  └─ app_specific_dictionary (M48 アプリ別)
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
  + app_profile_bonus
  - obsolete_penalty
```

| 因子 | 説明 |
|---|---|
| `base_frequency` | エントリの frequency（0.0〜1.0） |
| `source_priority` | 層 priority（user > technical > neologd > base） |
| `exact_reading_bonus` | 完全一致 +0.10、alias 一致 +0.05 |
| `category_bonus` | 辞書エントリ category（§14.4 の `person_name` / `place_name` / `technical` 等）に対する `settings.dictionary.categoryBoosts` 適用。M48 と独立した辞書層側のスコア因子 |
| `app_profile_bonus` | M48 `profilesByApp[*].candidateTagBoosts` を **候補タグ**（M52 ベンチで定義する `Technical` / `Polite` / `English` 等）に適用。辞書エントリ category とは **別 namespace**（`category_bonus` と二重適用しない） |
| `obsolete_penalty` | 最終使用が古いエントリに減点 |

`category_bonus`（辞書 category への適用）と `app_profile_bonus`（候補タグ
への適用）は別 namespace のため独立した因子として加算する。M48 で `code.exe`
を開いていて `candidateTagBoosts.Technical = 1.5` が設定されていても、これは
候補タグ `Technical` が付与された候補に作用するのであって、辞書 category
`technical` への作用は `dictionary.categoryBoosts.technical` 側で制御する。

### 14.6 辞書更新パイプライン

| 種類 | 更新方法 | M |
|---|---|---|
| bundled dictionary | アプリ更新時に同梱 | M28 / M53 |
| neologism pack | 任意更新（M36-B が SHA256 検証） | M36-B |
| technical pack | 任意更新（bundled or download） | M53 |
| user dictionary | 即時反映 | M9 |
| auto_words | 即時反映（M36-A の confirm / auto） | M36-A |
| app-specific dictionary | 設定画面で ON / OFF | M48 |

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

### 14.8 設定スキーマ拡張

`mvp-settings.schema.json` に追加:

```json
{
  "dictionary": {
    "sudachiEnabled": true,
    "neologdEnabled": true,
    "namedEntityEnabled": true,
    "technicalTermsEnabled": true,
    "userDictionaryEnabled": true,
    "autoWordsEnabled": true,
    "appSpecificDictionaryEnabled": true,
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

`categoryBoosts` の key は §14.4 で定義する具体 category（`person_name`,
`place_name`, `station_name`, `product_name`, `company_org`, `software`,
`anime_game`, `technical`, `neologism`）と一致させる。`named_entity` は
固有名詞系（`person_name` / `place_name` / `station_name` / `product_name` /
`company_org`）の umbrella tag として scorer 側で同義扱いし、エントリ category
が具体名（例: `person_name`）でも `categoryBoosts.named_entity` の
値が当該カテゴリへ加算される（具体値が同時に設定されている場合は具体値が優先、
umbrella は加算しない）。これにより M52 `named_entity_recall_at_5` の
target を達成可能な scoring 経路を確保する。

### 14.9 M53 受け入れ条件

- M52 ベンチで `named_entity_recall_at_5` が 90% 以上
- M52 ベンチで `neologism` カテゴリの top5 が、**M53 v1 時点で bundled
  されている `neologd_lexicon`** の範囲で baseline 比改善。neologd 更新
  前提の追加改善（M36-B `neologism pack` の SHA256 検証付き再配布で得ら
  れる新語追加）は **M36-B 完了時のみ** 評価し、M36-B 未完了時は本項を
  bundled lexicon の範囲で測定する（M36-B 完了後の follow-up チェック
  とする。`plans/windows-port-roadmap.md` M53 entry と整合）
- 既存 M36-A `auto_words.tsv` が DictionaryStore の auto_words layer
  として読み込まれる（後方互換）
- 既存 M9 `user_dict.json` が user_dictionary layer として読み込まれる
- 任意の layer を ON / OFF できる
- `app_specific_dictionary` layer のデータ構造と読み込み経路は M53 で
  確立する（実際の boost 適用は M48 完了後の統合検証で確認）。M48 未完了時
  は layer を空（無効）として扱い、本受け入れ条件は M48 完了後の follow-up
  チェックとする
