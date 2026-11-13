# 新語自動取得・辞書追加 仕様（Auto Word Registration）

本書は「Google 日本語入力のように、新しい単語を自動で取得して辞書に追加する」
機能の仕様を定める。対象は **Windows 版（C++ 移植: `core/` `learning/` `ipc/`
`inference-host/` `tsf-tip/`）** のみ。macOS 版 `legacy/` は対象外。

姉妹機能: `docs/typo-correction-learning-spec.md`（M35 + M55 統合補正）、
`docs/conversion-quality-benchmark-spec.md`（M52）。

## 0. 節とマイルストーンの対応

本書の節と、その範囲を定める `plans/windows-port-roadmap.md` のマイルストーンの対応を次に示す。

| 節 | マイルストーン |
|---|---|
| §1〜§2、§10〜§13 | M36-A・M36-B 共通 |
| §3〜§4、§6〜§9 | M36-A |
| §5 | M36-B |
| §14 | M53（辞書層の論理層） |
| §15 | M53（system 辞書の物理層。`docs/azdic-physical-layer-spec.md` へ移動。転送表を参照） |

§14 以降は M53 の追補章で、`AutoWordStore` を **多層 DictionaryStore**
の 1 レイヤとして統合し、辞書層全体（base / sudachi / neologd /
named_entity / technical_terms / user / app_specific）を再設計する。
M36-A / M36-B の挙動は M53 でも下位互換として維持する。

## 1. 背景と目的

- `SimpleConverter`（内蔵辞書 + `LoadFromTsv`）/
  `learning::UserDictionary`（**手動登録**の JSON 辞書）/ `learning::LearningStore`
  （確定履歴の重み付け再ランキング）の 3 層は、いずれも「新語を能動的に取り込む」機構を
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

型の正典は `learning/include/azookey/learning/AutoWordStore.h` の `AutoWord`（と `AutoWordSource` /
`AutoWordState`）である。1 語は `(surface, reading)` で一意とし、各フィールドは次の意味を持つ。

| フィールド | 意味 | 既定 |
|---|---|---|
| `surface` | 表面形 | — |
| `reading` | かな読み。マイニングでは確定時の読み | — |
| `source` | 取得元。`Mining`（§4）または `Trending`（§5） | `Mining` |
| `state` | 承認状態。`Pending` / `Confirmed` / `Rejected`（§7） | `Pending` |
| `count` | マイニングでは観測頻度、トレンドでは rank 由来の初期値 | 0 |
| `first_seen_epoch` / `last_seen_epoch` | 初回・最終の観測時刻（epoch 秒） | 0 |
| `score` | 候補へ注入するときのスコア。0 なら §6 の既定スコアを使う | 0.0 |

### 3-2. 永続化形式

TSV（`LearningStore` の区切り規約に準拠、`#` コメント可）。先頭にバージョン
ヘッダ行を置く。

```
# azookey-auto-word-store v1
# surface	reading	source	state	count	first_seen_epoch	last_seen_epoch	score
```

列名行は `#` 付きのコメントとして書く。`#` が無いと、列名行とレコード行を
読み手が区別できない。フィールド中の `\`・タブ・改行・先頭の `#` は
`LearningStore` と同じ規約でエスケープする。

- `source`: `mining` | `trending`、`state`: `pending` | `confirmed` | `rejected`
- 既定パス: `%LOCALAPPDATA%\azooKey\data\auto_words.tsv`
  （`learning.tsv` / `user_dict.json` と同じ `data\` 配下。host CLI で `--learning <path>` を
  明示すると、その親ディレクトリの `auto_words.tsv` を使う。§14.10 の非配布データ配置と
  一致させる）
- `score` は、読み戻したときに同じ `double` になる最短の 10 進表現で書く。
- パース失敗行はスキップ。ファイル無しは空ストアで成功扱い（`UserDictionary::Load`
  の挙動に合わせる）。

### 3-3. API

```cpp
class AutoWordStore {
 public:
  explicit AutoWordStore(std::filesystem::path path);

  bool Load();
  bool Save() const;
  void Reset();
  // writer を直列化し、保存成功後に変更を公開する。
  bool ResetAndSave();
  AutoWordSaveOutcome SetStateAndSave(const std::string& surface, const std::string& reading,
                                     AutoWordState state);

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
  // メモリ上の状態を設定し、直前の状態を返す（キーが無ければ nullopt）。保存はしない。
  std::optional<AutoWordState> SetState(const std::string& surface, const std::string& reading,
                                        AutoWordState state);
  // 現在の状態が expected のときだけ desired へ変える。保存はしない。
  bool CompareAndSetState(const std::string& surface, const std::string& reading,
                          AutoWordState expected, AutoWordState desired);

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
- `TrendingWordFetcher` のワーカースレッドから変更されるため、内部で writer を直列化する。
  表の読み取りには別の mutex を使い、暗号化・保存中も直前に公開された表を参照できる。
- `ResolveNewWord` と学習データ管理の忘却は `SetStateAndSave`、全削除は `ResetAndSave` を使う。
  writer の mutex を保持して複写を変更・保存し、成功したときだけ短い表のロック区間で公開する。
  保存失敗時は公開済みの表を変更せず、同時操作の更新を上書きしない。
  `AutoWordSaveOutcome` は変更・保存成功を `Changed`、同じ状態を `Unchanged`、
  キー無しを `NotFound`、保存失敗を `SaveFailed` として返す。同じ状態は保存し直さない。

## 4. マイニング検出（M36-A）

### 4-1. 配線 — 新 IPC は不要

既存の `CommitObservation` を再利用する。Host は受理済み接続の
`secure_flag` と、当該イベントの `secure == false` /
`learning_allowed == true` を確認してから学習へ渡す。欠落・型不正は拒否する。
同じ契約を `CommitSegmentsObservation` にも適用し、直近の候補要求から推定しない。
詳細は `docs/privacy-and-secure-input-spec.md` §5.1.1 に従う。

`CommitObservationRequest{reading,
chosen, shown, left_context, timestamp_ms, secure, learning_allowed}` が確定読み・確定 surface を含み、
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
> すること。`IConverter` に `bool Contains(reading, surface)` を追加し、
> Zenzai（M8）導入後も統一的に判定できるようにする。
>
> `Contains` は **`Learn()` 由来の確定履歴を既知語として扱わない**。
> `SimpleConverter::Commit` は `Learn()` を呼んで確定語を辞書バケットへ入れる
> ため、バケットの membership をそのまま見ると新語が初回確定で「既知」になり、
> 2 回目以降を観測できず `miningMinCount` に到達しない。
>
> `Contains` の既定実装は `false` を返す。`ZenzaiModelConverter::Contains` は
> フォールバック変換器（`SimpleConverter`）の `Contains` へ委譲する。モデル
> バックエンド利用時も、既知語判定はユーザー辞書・静的辞書レイヤ・フォール
> バック変換器の実辞書の 3 つを見る。Zenzai モデル自身の語彙は既知語判定の
> 対象外とする。
> 静的層の収録語は、候補向けの ON/OFF と独立に既知語として扱う（§14.8）。
>
> 対象外とする根拠は 2 つある。1 つ目は、モデルが語の単位の語彙を持たないこと
> である。Zenzai の GGUF は文字単位の GPT-2 系モデル（`tokenizer.ggml.pre` が
> `gpt2-small-japanese-char`）で、語彙 6000 のうち複数文字のトークンは特殊記号
> `[UNK]` / `[PAD]` / `<s>` / `</s>` の 4 つだけである。残りは 1 文字のトークン
> と、語彙に無い文字を表す byte fallback 用の部分バイトである。よって
> `(reading, surface)` で引ける索引は作れない。surface をトークナイズしても、
> トークン数は辞書の有無に関係なく文字数と同じになり、未知語トークンも出ない
> ため、トークン列からも既知語は判定できない。モデルは kana から surface を
> 自由生成するので、モデルが出せる語を列挙する手段も無い。
>
> 2 つ目は設計上の理由である。モデル生成候補の確定を既知語として扱うと、
> その語は `AutoWordStore` に入らない。すると、モデル未ロード・tier 降格・
> degraded mode でフォールバック変換器へ切り替わったときに、その語が候補から
> 消える。auto-word はバックエンドをまたいで語を持ち越す層として扱い、モデルが
> 出せる語も記録する。モデルが既に出せる語が候補に上がる誤検出は、§4-3 の
> フィルタと `miningMinCount` による回数の gate を通った後に限られ、承認制
> （§7）では人が承認待ちの段階で弾ける。

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

本節のログ契約は `dev-infrastructure-spec.md` §7 と `debugging.md`「ログ収集」から参照する。

- 新規 `inference-host/include/azookey/host/TrendingWordFetcher.h` /
  `src/TrendingWordFetcher.cpp`（WinHTTP 使用）。
- 起動時 1 回 + `interval_hours`（既定 24h）周期で `FetchOnce` を実行:
  DL → SHA256 検証 → JSON パース → `AutoWordStore::IngestTrending`。
- 検証・ネットワーク失敗時はストアを変更しない。`ETag` / `generated_at` 比較で
  無変更時はスキップ。

worker は `Start` / `Stop` で所有し、終了時に join してストアより先に破棄する。
`UpdateSettings` は取得の設定世代を更新する。無効化・間隔変更・登録モード変更で
進行中の取得をキャンセルし、古い世代の結果は取り込まない。設定更新は通信の終了も
キャッシュ・ストアの書込みも待たず、再有効化時には直ちに取得する。同じ設定の再適用では
周期をリセットしない。取り込みの線形化点は、engine の writer mutex の下で設定世代を
再確認する時点とする。この確認を通った取り込みは設定更新より前に起きたものとして扱い、
`UpdateSettings` が戻った後に保存を終えることがある。確認の時点で世代が変わっていれば
ストアを変更しない。したがって、無効化の後に忘却・全削除を行うと、旧世代の語は残らない。
キャンセルされた世代でも、公開アセットのキャッシュ `trending-words.json` は更新されうる。
`Stop` は実行中の取り込みの終了を待つので、`Stop` の後にストアを破棄してよい。
起動時から無効な場合は、HTTP セッションもソケットも生成しない。

チェックサムは 1 KiB、JSON は共有 JSON parser と同じ 1 MiB を上限とする。各 WinHTTP 操作の timeout は
1 秒、取得全体のキャンセル期限は 15 秒。停止と期限は、操作の間に加えて、WinHTTP の
接続・応答・本文の待ちの間にも確認する（`HttpDownloader` は WinHTTP を非同期で使う）。
`generated_at` は保存に成功した直近の値と比較し、同じ値なら再取り込みと recency 更新を
省略する。この比較値は Host 内で保持し、再起動後の初回取得では再検証・取り込みを行う。

Host は `TrendingFetchDependencies::ingest_and_save` を
`InferenceEngine::IngestTrendingWords` へ接続する。同メソッドは writer 専用の
`learning_history_mutex_` を保持して設定世代の確認から取り込み・保存まで行い、忘却・全削除と直列化する。
保存中は `state_mutex_` を保持しない。
Fetcher の設定 mutex はキャッシュ・ストアの書込みをまたいで保持しない。取り込みと保存は
Fetcher の取得 mutex → engine の writer mutex → store の writer mutex の順で取得する。
`Dispatcher::HandleUpdateConfig` は `ApplyConfig` が戻り state mutex が解放された後に
Fetcher の設定 callback を呼ぶ。`Stop` / join も engine のロック外で行う。

診断イベント `trending_word_fetch` は固定の `reason`（`disabled`、
`source_unavailable`、`unchanged`、`ingested`、`failed`、`cancelled`）だけで結果を区別する。
URL、取得本文、ユーザー語は記録しない。失敗と取得先未設定は warning とし、
Host の変換処理を継続する。

### 5-2. 取得元 — プロジェクトホストの静的アセット

公開・更新・ロールバックの運用手順は
[`handoff/trending-asset-publication.md`](handoff/trending-asset-publication.md) を正典とする。

クライアント（IME）は **Google Trends を直接スクレイピングしない**。代わりに
プロジェクトがホストする整形済みの静的アセットを定期 DL する。

- 取得元例: `https://github.com/<org>/azooKey-Desktop/releases/download/
  trending-latest/trending-words.json`（GitHub Releases の固定タグ、または
  HuggingFace データセット）。
- 併置の `trending-words.json.sha256` でハッシュ検証。将来は detached 署名
  （`.sig`）対応の余地を残す。
  checksum は取得したアセットの完全性を確認するもので、配信元の正当性を保証しない。
- 検証通過後、ローカルキャッシュへアトミック書き込み（macOS 版
  `legacy/Core/Sources/Core/InputUtils/DebugTypoCorrectionWeights.swift` の
  アトミック展開パターンを踏襲）。
- キャッシュは解決済みユーザーデータの `data/trending-words.json` とする。
  専用 staging に SHA256 検証付きで取得する。staging から一度読み込んだ同じバイト列を
  SHA256 検証・JSON 検証・キャッシュ書込みに使い、既存キャッシュを atomic replace
  する。通信・検証・キャッシュ書込みの失敗時は旧キャッシュを保持し、staging と
  `.part` を削除する。ストアの保存失敗時は検証済みキャッシュとメモリ上の語を保持し、
  保存成功の比較値を更新せず次回取得で再試行する。
  取得先が空なら通信を開始しない。上記の例示 URL を暗黙の配信先として使わない。
  本番の取得先は配信契約の承認を受けて設定する。取得先が空の無通信構成での
  取得基盤の受入と、アセットの実配信の受入は区別する。
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

- `reading` は上流で付与済み（クライアントで読み推定はしない）。reading 欠落・空の
  エントリは取り込みスキップ。
- `rank` から初期 `count` / `score` を導出
  （例: `score = base + (max_rank - rank) / max_rank * k`）。
  v1 では正の `uint32_t` rank を使い、`count = max_rank - rank + 1`、
  `score = 1.0 + 0.2 * (max_rank - rank) / max_rank` とする。
  `max_rank` は reading のある取り込み対象の最大 rank とする。
  version は 1、`generated_at` は空でない 64 バイト以下の文字列、`words` は配列を必須とする。
  reading 欠落・空のエントリ以外の形式不正はアセット全体を拒否する。

入力の上限と文字制約は次のとおりとする。JSON 全体の上限は §5-1 の 1 MiB を維持する。

- `words` 配列は最大 10000 件。reading が欠落・空で取り込みをスキップする要素も数える。
- `surface` と `reading` は、それぞれ JSON のエスケープを解釈した後の UTF-8 で
  最大 256 バイトとし、妥当な UTF-8 であることを要求する。`surface` は空文字列を許可しない。
- 両文字列で C0（U+0000〜U+001F）、C1 と DEL（U+007F〜U+009F）、
  双方向制御文字（U+061C、U+200E〜U+200F、U+202A〜U+202E、U+2066〜U+2069）を拒否する。
- `reading` の文字は U+3041〜U+309F または U+30A0〜U+30FF に限る。
  全角の長音符・結合濁点・中点は許可範囲に含め、半角カナは拒否する。

reading 欠落・空のエントリをスキップする場合を除き、上限超過・文字制約違反・型不正は
アセット全体の拒否とする。検証を通過するまで取り込みやキャッシュの置換を行わず、
拒否時は既存のストアとキャッシュを保持する。

### 5-4. M32 WinHTTP 基盤との関係

M32（自動更新）の `UpdateChecker.cpp` と HTTP DL + SHA256 検証パターンが共通。
共通ヘルパ `inference-host/include/azookey/host/HttpDownloader.h` と
`inference-host/src/HttpDownloader.cpp` を `UpdateChecker`（M32）と
`TrendingWordFetcher`（M36-B）で共有する。**`HttpDownloader` は M32 が新設し
（M32 は自動更新 §6 に加え `docs/msix-packaging-spec.md` §1.6.1 (b) の初回
モデル DL でも利用）、M36-B はそれを再利用する**（依存順は M32 → M36-B。
`plans/windows-port-roadmap.md` の M32 / M36-B と整合）。

## 6. 適用（`QueryCandidates` への注入）

`InferenceEngine::QueryCandidatesEx` の `user_dict_` ルックアップ直後、
`converter_->Convert` の前に追加する。読みは typo 補正後の `kana` を使う。

```cpp
if (auto_word_store) {  // state_mutex_ の下で auto_word_store_ を写したもの
  for (auto& w : auto_word_store->LookupConfirmed(kana)) {  // Confirmed のみ
    core::Candidate c;
    c.surface = std::move(w.surface);
    c.reading = std::move(w.reading);
    c.score   = w.score > 0.0 ? w.score : config.auto_word_default_score;
    c.source  = core::CandidateSource::UserDictionary;
    c.debug_info = "auto-word";
    merged.push_back(std::move(c));
  }
}
```

- **pending 語は注入しない**（`LookupConfirmed` が Confirmed 限定）。承認後に
  初めて変換へ反映される。
- `LookupConfirmed` は `state_mutex_` の外で呼ぶ。表を複写する短い区間だけロックし、
  writer が `Save()` の暗号化・flush を行う間も、公開済みの確定語を読み取れる。
- `EngineConfig::auto_word_default_score`（既定 1.2）は、スコアを持たない語
  （マイニング由来の語はすべて `score == 0`）に使う。user_dict の既定値
  `user_word_default_score`（1.5）より低いので、同じ surface のユーザー辞書語が
  マージで勝つ。静的辞書の一般的な完全一致（frequency + base 層の優先度 0.20 +
  完全一致 0.10）よりは高い。設定キーは持たない。
- `debug_info` は識別用の印である。重複除去で他の候補とまとめられると、
  `;dup:` 付きで連結される。
- Confirmed になった ASCII 英字の語は、M60 の英単語辞書にも入る（`docs/inline-english-candidate-spec.md` §4.8）。
  Host は起動時に、まだ辞書に無い Confirmed の語も取り込む。Rejected に戻した語（`ResolveNewWord` の reject、
  学習データの忘却）は、確定で辞書に足した分だけ辞書から取り消す。新語候補の全削除は、確定で辞書に足した語も消す。

## 7. 承認フロー

### 7-1. 新 IPC メッセージ

- `ipc/include/azookey/ipc/Messages.h`: `MessageType` に
  `ListNewWordCandidates`、`ResolveNewWord` を追加。
- `ipc/src/Messages.cpp`: `TypeToString` / `TypeFromString` に対応追加。
- payload の正典は `ipc/include/azookey/ipc/Payloads.h` の `ListNewWordCandidatesRequest` /
  `NewWordField` / `ListNewWordCandidatesResponse` / `ResolveNewWordRequest` /
  `ResolveNewWordResponse` と、`ipc/src/Payloads.cpp` の codec である。wire key は C++ のメンバ名と同じ。

本節と §7-2 は `docs/windows-tsf-host-architecture.md`「IPC メッセージ」の索引から参照され、
フィールドの意味を次のとおり定める。

| payload | フィールド | 意味 | 欠落時 |
|---|---|---|---|
| `ListNewWordCandidatesRequest` | `state_filter` | 一覧する状態（下記の受理条件） | `pending` |
| 同上 | `max_items` | 返す件数の上限（下記の受理条件） | 50 |
| `NewWordField` | `surface` / `reading` / `count` / `last_seen_epoch` | §3-1 の同名フィールド | — |
| 同上 | `source` / `state` | §3-2 の TSV と同じ小文字の名前（`mining` / `trending`、`pending` / `confirmed` / `rejected`） | — |
| `ListNewWordCandidatesResponse` | `ok` / `error` / `items` | 下記「応答の意味」 | `ok=true` |
| `ResolveNewWordRequest` | `surface` / `reading` | 対象の語 | — |
| 同上 | `action` | `confirm` または `reject` | — |
| `ResolveNewWordResponse` | `ok` / `changed` / `error` | 下記「応答の意味」 | `changed=false` |

parser の受理条件:

- `state_filter` は `pending` / `confirmed` / `rejected` のいずれか。未知の値は
  パース失敗とする。全件表示へ広げると、利用者が却下した語を再び見せることになる。
- `max_items` は 1 以上 `kMaxNewWordCandidates`（500、`ipc/include/azookey/ipc/Limits.h`）
  以下。範囲外はパース失敗とする。上限が無いと 1 応答へストア全件を詰めさせられる。
- `ResolveNewWordRequest` は `surface` / `reading` が空、または `action` が
  `confirm` / `reject` 以外ならパース失敗とする。

応答の意味:

- `ListNewWordCandidatesResponse.ok` が `true` なら、`items` は指定状態の語を
  `max_items` 件まで並べたものである。空配列は「その状態の語が無い」を意味する。
  一覧を作れなかった場合は `ok=false` と `error` を返し、`items` は空にする。
- `ResolveNewWordResponse.ok` は「呼び出し後に対象語が要求した状態にあり、
  その状態がファイルに保存されている」を表す。`changed` はこの呼び出しで
  状態が変わった場合だけ `true` になる。既に同じ状態の語への同じ操作は
  `ok=true, changed=false` を返し、ファイルを書き直さない。
- `ok=false` のときは `error` に次のいずれかを入れる。

| `error` | 意味 |
|---|---|
| `invalid_request` | payload の parse に失敗した（上記の受理条件を満たさない） |
| `store_unavailable` | Host に `AutoWordStore` が設定されていない |
| `not_authenticated` | Handshake 未成立の接続からの要求 |
| `not_found` | `(surface, reading)` の語がストアに無い（Resolve のみ） |
| `save_failed` | 状態変更を保存できなかった（Resolve のみ）。メモリ上の状態は元に戻す |

`ok` / `changed` / `error` は protocol v1 への追加フィールドである。
`ListNewWordCandidatesResponse` で `ok` が無い応答は `ok=true`、
`ResolveNewWordResponse` で `changed` が無い応答は `changed=false` として読む。
同じ操作の繰り返しを `ok=true` とする `ok` の意味は `Envelope.version` 1 のまま
適用し、繰り返しを `ok=false` とする旧い意味は互換のために残さない。

### 7-2. Dispatcher 配線

- `Dispatcher` コンストラクタに `learning::AutoWordStore* auto_word_store` を追加
  （`user_dict` の隣）。
- `Dispatch` の switch に 2 ケース追加。
- `HandleListNewWordCandidates`: `auto_word_store_->ListByState(...)` の結果を
  `last_seen_epoch` の新しい順に並べ、`max_items` 件で切って `NewWordField` に変換。
- `HandleResolveNewWord`: action に応じた目標状態を `AutoWordStore::SetStateAndSave`
  へ渡す。`Changed` は `ok=true, changed=true`、`Unchanged` は保存せず
  `ok=true, changed=false` を返す。`NotFound` は `not_found`、`SaveFailed` は
  `save_failed` を返す。保存中は writer の mutex で別の接続の承認・却下を待たせ、
  成功時だけ表を公開する。失敗時の表は操作前のままである。

### 7-3. 承認 UI と代替経路

- **承認 UI**: M30 WinUI3 設定アプリの辞書ペインに「新語候補」リストを置く。
  pending を `ListNewWordCandidates` で取得、各行に登録/却下ボタン →
  `ResolveNewWord`。M36 の承認 UI は M30 に依存する。
- **承認 UI を使わない経路**（いずれか／併用）:
  1. **auto モード** — `registrationMode=auto` では pending を経ず即 confirmed
     になる。承認 UI を使えない環境の推奨設定とする。
  2. **host の `newwords` サブコマンド** — 検証・dogfooding 用のワンショット
     CLI。`userdict` / `lookup` と同じく host の引数に `newwords` を置き、
     それ以降をサブコマンドの引数として読む。

     ```
     azookey_inference_host newwords list [--state pending|confirmed|rejected] [--format json|tsv]
     azookey_inference_host newwords confirm --reading <reading> --surface <surface> [--offline]
     azookey_inference_host newwords reject  --reading <reading> --surface <surface> [--offline]
     ```

     - `list` は `AutoWordStore` から `auto_words.tsv.enc` を復号して読む
       （旧平文 `auto_words.tsv` は移行元。既定は `--state pending`）。host は
       観測と承認のたびに保存するので、ファイルが共有の参照元になる。並びは
       `ListNewWordCandidates` と同じ `last_seen_epoch` の新しい順とする。
       ストアを読めない場合は標準出力へ何も出さず、標準エラーへ理由を出して
       終了コード 1 で終わる。
     - `confirm` / `reject` は既定で稼働中の host へ Named Pipe で接続し、
       Handshake の後に `ResolveNewWord` を送る。host はストアをメモリ上に持ち、
       次の保存でファイル全体を書き直す。稼働中の host の裏でファイルを編集すると
       変更が失われるため、接続できない場合は失敗とする（接続の待ち時間は
       `docs/learning-data-management-spec.md` §13 に従う）。`--offline` を付けた
       ときだけファイルを直接編集する。直接編集は host と同じ `Load` / `Save`
       を使うので、読めなかった破損行は書き直しで失われる（§3-2 のスキップ規則）。
     - JSON 出力は 1 行 1 オブジェクトとする。`list` は語ごとに `op` / `ok` /
       `reading` / `surface` / `source` / `state` / `count` / `last_seen_epoch`
       を出し、該当語が無ければ `{"ok":true,"op":"list"}` の 1 行を出す。TSV は
       `reading` / `surface` / `source` / `state` / `count` / `last_seen_epoch`
       の順に並べる。`confirm` / `reject` は `op` / `ok` / `changed` / `reading` /
       `surface` / `via`（`ipc` / `file`）と、失敗時の `error`（§7-1 のコード、
       または接続・Handshake の失敗理由）を出す。
     - 終了コードは `ok=true`（`changed=false` を含む）で 0、失敗で 1、引数の
       誤りで 2 とする。

## 8. 設定

型・既定値・範囲の正典は `settings/mvp-settings.schema.json` の `autoWordRegistration` である
（schema の `description` が本節を指す）。本節は各キーの意味と反映先を定める。

| キー | 意味 | `EngineConfig` の反映先 |
|---|---|---|
| `miningEnabled` | ユーザー入力マイニング（§4）を行う | `auto_word_mining_enabled` |
| `trendingEnabled` | トレンド語の取得（§5）を行う | `auto_word_trending_enabled` |
| `registrationMode` | `confirm` は pending を経て承認（§7）、`auto` はしきい値到達で即 confirmed | `auto_word_auto_register`（`auto` のとき true） |
| `miningMinCount` | マイニングで昇格に要る観測回数 | `auto_word_min_count` |
| `trendingIntervalHours` | トレンド語を取得する間隔（時間） | `auto_word_trending_interval_hours` |

実効値の経路は `settings.json` の `autoWordRegistration.*` とする。host の
`SettingsStore` が読み、`ApplyRuntimeSettingsToEngineConfig` が上表の `EngineConfig` へ反映する。

`trendingIntervalHours` の上限は 8760（1 年）とする。`Dispatcher::HandleUpdateConfig` はモデルの再ロード前に Fetcher へ設定を通知する。
SafeMode または `privacy.mode=offline` では `trendingEnabled` の実効値を false とする。
offline による抑止は保存された `trendingEnabled` を変更せず、通常モードへ戻ると元の設定を使う。
`auto_word_default_score` は §6 の注入が使う `EngineConfig` の内部値であり、
設定キーを持たない。

## 9. プライバシー方針

- **マイニングはオフライン完結** — OOV 判定・`auto_words.tsv` への保存はすべて
  ローカルの host プロセス内処理。ユーザーの入力語・確定履歴を外部へ送信しない。
  新たな外部通信を一切追加しない。
- **トレンド取得はダウンロードのみ** — `TrendingWordFetcher` は公開静的アセットを
  GET するだけ。ユーザー語・入力統計・端末情報をリクエストに含めない
  （User-Agent はバージョン文字列のみ、M32 と同様）。アップロードは行わない。
  `privacy.mode=offline` は公開アセットの取得も禁止する。起動時は通信せず、実行中に
  offline へ切り替えると設定世代を更新し、進行中の取得をキャンセルする。
  neologd pack にも同じ禁止を適用する（§15.14）。
- **DPAPI 暗号化（M34）対象に新ストアを含める** — `auto_words.tsv` はユーザーの
  未知語（固有名詞・個人情報を含みうる）を蓄積するため、`learning.tsv` /
  `user_dict.json` と同等の機微情報として M34 の暗号化対象に追加する。
  公開アセットである trending-words のローカルキャッシュは暗号化不要。

## 10. マイルストーン分割

本節は削除した（M36-A と M36-B の範囲と前提は roadmap の M36-A・M36-B 節が持つ）。

## 11. 変更対象ファイル一覧

本節は削除した（変更対象は roadmap の M 節、テストは `docs/test-inventory.md` が持つ）。

## 12. テスト計画

- `learning/tests/auto_word_store_test.cpp`:
  `Observe` の pending 追加 → count++ → しきい値で Confirmed 昇格 /
  `auto_promote` 即昇格 / Reject 後の再 `Observe` が無視される /
  `LookupConfirmed` が pending を返さない / `IngestTrending` の rejected skip・
  mining 優先 / Save→Load 往復（score が値を変えずに往復すること） /
  `SetState` が直前の状態を返すこと / `CompareAndSetState` が期待状態のときだけ
  変えること / `PrunePending` の経過判定 /
  キー一意性（reading 空 vs 非空）。
- `ipc/tests/payloads_test.cpp`: `ListNewWordCandidates` / `ResolveNewWord` の
  Build→Parse 往復、不正 action・欠損フィールドのパース失敗、`ok` / `changed` /
  `error` の往復と、これらを欠く応答の既定値。
- `ipc/tests/messages_test.cpp`: 新 `MessageType` の `TypeToString` /
  `TypeFromString` 往復。
- `inference-host/tests/engine_test.cpp`: `CommitObservation` で OOV が記録 /
  既知語（user_dict・内蔵辞書）は記録されない / 誤検出フィルタで除外 /
  Zenzai 利用時もフォールバック変換器の辞書語は記録されない /
  Confirmed 語が `QueryCandidates` に `debug_info="auto-word"` と
  `auto_word_default_score` で注入 / pending 語は注入されない /
  `registrationMode=confirm` では承認するまで注入されない。
- `inference-host/tests/dispatcher_test.cpp`: `HandleListNewWordCandidates` が
  pending 一覧を返す / `HandleResolveNewWord` confirm/reject の反映 /
  同じ操作の繰り返しが `ok=true, changed=false` / `not_found`・`invalid_request`・
  `store_unavailable` の各 `error` / 保存失敗時に `save_failed` を返し状態を戻す /
  承認後にはじめて `QueryCandidates` へ注入される。
- `inference-host/tests/newwords_cli_test.cpp`: 引数検証 / `list` の並びと
  JSON・TSV 出力 / `--offline` での直接編集 / host 不在時にファイルを触らず
  失敗すること / 稼働中の Dispatcher 経由の confirm で pending → confirmed →
  注入まで通ること。
- `inference-host/tests/trending_word_fetcher_test.cpp`（M36-B）:
  `TrendingFetchDependencies` から取得関数と時刻を注入し、ローカル fixture の SHA256
  検証・取り込み・永続化、無効時の通信入口未呼出し、通信・検証失敗時の保持、
  保存失敗後の再試行、設定世代によるキャンセル、取り込み中の設定更新が書込みを待たないこと、
  `Stop` が取り込みの終了を待つこと、worker 終了、仮想時刻での周期再取得を検証する。

## 13. 検証手順（実装後）

本節は削除した（ビルドとテストの手順は `README.md`、テスト一覧は `docs/test-inventory.md` が持つ）。

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

auto-word の既知語判定（§4-2）が見るのは、この辞書層とフォールバック変換器の
実辞書だけである。Zenzai モデル自身の語彙は M53 でも判定に含めない。モデルは
文字単位で語の語彙を持たず、モデルが出せる語を辞書層に索引として持ち込む層も
上の構成に置かないためである（根拠は §4-2）。辞書層に語を足すと既知語判定の
範囲も同じだけ広がるので、モデルが出せる語の誤検出は辞書層の拡充で減らす。

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

`.azdic` の category bit は表の順に 0〜9 とする。C++ の共有定義は
`learning/include/azookey/learning/DictionaryCategory.h` に置き、Python ビルダの
`CATEGORIES` との一致を `dictbuild_python_tests` で検査する。

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

`learning/src/DictionaryStore.cpp` として実装:

```cpp
class DictionaryStore {
public:
  // 全層 query
  std::vector<DictionaryEntry> Lookup(
      std::string_view reading,
      const LookupContext& ctx);

  // マイニング専用の完全一致判定。静的層の有効フラグは無視する。
  bool ContainsKnownWord(std::string_view reading, std::string_view surface) const;

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

設定全体の拡張形を次に示す。`mvp-settings.schema.json`、Host、設定アプリの保存対象は
このうち7つの `*Enabled` キーとする。`verifyOnLoad` と `categoryBoosts` は後続の
拡張用であり、以下の契約を実装する段階で schema と runtime に追加する。

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
当該 layer は無効（missing-pack）として扱う。pack の取得とロードは Host の起動時だけ
行う（§15.14）。セッション中に `true` へ変えた場合、層は次の起動まで missing-pack のまま
である。bundled 層（`sudachi` / `named_entity` / `technical_terms`）の既定は `true`。

7つのキーは boolean のみを受け付ける。欠落、型不正、`dictionary` 自体の型不正は
キーの既定値へ戻し、未知のキーは runtime では無視する。schema は未知キーを拒否する。
設定アプリは表示していない有効なキーを保存時にも保持し、未知または型不正の値は除去する。
設定の再読込では Host の `EngineConfig` と `DictionaryStore` の有効層を更新する。
OFF の層は変換候補、読み前置一致による予測、表層形の逆引きへ寄与しない。
ユーザー辞書と confirmed 自動語の独立した候補注入にも同じ切替を適用する。
静的層の OFF は auto-word の既知語判定（§4-2）を変えない。ロードと検証を通過した
静的層は有効フラグにかかわらず専用経路で `(reading, surface)` を確認し、収録済みの語を
再マイニングしない。候補の取得経路にはこの有効フラグを無視する判定を使わない。
Base の切替キーは設けない。`appSpecificDictionaryEnabled` は供給元が空なら候補を増やさない。

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

本節と §14.10 は、設定アプリのライセンス導線（`native-ui-spec.md` §4.6）から参照される。
実機検証パッケージ（`dev-infrastructure-spec.md` §2.6）の辞書同梱も本節と §14.10 に従う。

各辞書層のソース・ライセンス・配布可否。**配布判定の正典は本節**であり、
配布物（`docs/sideload-packaging-spec.md` §4 の base MSI）の同梱物はこの判定に従う。ライセンスは
外部上流の変更があり得るため、各ソースの一次情報（下記 URL）を **取り込み時に
再検証**する（最終確認: 2026-06-11。SudachiDict は 2026-10-05 に v20260723 の LEGAL で再確認）。

| layer | 採用ソース | ライセンス | 配布判定 | 条件・注記 |
|---|---|---|---|---|
| `base_lexicon` | azooKey_dictionary_storage | Apache-2.0 | **同梱可**（既存） | azooKey 内蔵辞書。LICENSE / NOTICE を ThirdPartyNotices に保持 |
| `sudachi_lexicon` | SudachiDict（`core` 版） | Apache-2.0（内包: UniDic=BSD-3-Clause / NEologd 由来データ） | **同梱可** | LEGAL に基づき配布物全体が Apache-2.0。UniDic の BSD-3 著作権表示に加え、SudachiDict LEGAL（内包 NEologd 由来データの Hatena / 郵便 / 駅名 / 人名 帰属）も帰属に伝播（§14.10）。サイズの観点で `full` ではなく `core` を採用 |
| `neologd_lexicon` | mecab-ipadic-NEologd | Apache-2.0（ただし上流データに個別条件: Hatena キーワード=はてな社条件・要帰属 / 駅名 / 人名 / 郵便 等） | **別 pack DL（同梱不可）** | サイズ大 + 上流データの provenance が個別条件付き。MSIX に含めず SHA256 検証 DL（既定無効）。取り込み経路は §14.10 の neologd pack follow-up（M36-B とは別作業）。DL 時に上流ライセンス/帰属を提示 |
| `named_entity_lexicon` | Wikidata（CC0）+ GeoNames（CC-BY-4.0）+ 日本郵便 郵便番号データ | CC0 / CC-BY-4.0 / 権利主張なし | **同梱可**（curated 派生） | Wikidata=CC0（人名/組織/製品 + 読み）。GeoNames=CC-BY-4.0（**帰属必須**）。CC-BY-SA の Wikipedia 本文は **不使用**（share-alike 回避）。郵便データは権利主張なし（帰属歓迎） |
| `technical_terms_lexicon` | プロジェクト自作 + CC0/CC-BY 上流 | Apache-2.0（自作分）/ 上流に従う | **同梱可** | リポジトリ内で手入れ。外部由来分は上流ライセンス・帰属を ThirdPartyNotices に記載 |
| `user_dictionary` / `auto_words` / `app_specific_dictionary` | ユーザー生成 | N/A（ユーザーデータ） | **非配布**（ローカルのみ） | 配布物に含めない。`%LOCALAPPDATA%` に保存（`docs/msix-packaging-spec.md` §1.4 / `docs/sideload-packaging-spec.md` §9） |

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
| **同梱（bundled）** | base / sudachi(core) / named_entity / technical_terms | base MSI がインストール先の `dict\` へ置く read-only データ（`docs/sideload-packaging-spec.md` §4） | アプリ更新時（§14.6） |
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

pack のファイル名、マニフェスト、生成ツール、検証順序は §15.14 を正典とする。

帰属（**ThirdPartyNotices**）:

- 同梱辞書の全ライセンス（Apache-2.0 LICENSE + NOTICE、UniDic BSD-3 著作権表示、
  GeoNames CC-BY-4.0 クレジット、郵便データ出典）を 1 つの
  `ThirdPartyNotices.txt` に集約して配布物に同梱し、設定アプリのライセンス画面
  （`docs/settings-app-spec.md` §3.2 のバージョン/ライセンス導線）から参照
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
- **配布ガード**（roadmap M53 の受け入れ条件）: MSI 構築前に同梱アセットへ
  **standalone の mecab-ipadic-NEologd パック（`neologd_lexicon` 層アセット）**
  が混入しないことを release workflow の `dictbuild/check_bundle.py` でチェックする。ガードの対象は NEologd 単体パックで
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
**ない**（§14.5）。M48 タグ boost は候補の score に **1 回だけ**掛ける（clamp 式と適用位置は
`docs/app-profile-spec.md` §7 が正典）。DictionaryStore 由来候補にも §14.12 で付与した単一タグに
基づき同経路で作用し、二重適用を避けるため `dictionary_score` には入れない。

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

その後、M48 タグ boost は app-profile-spec §7 の式で候補の score に別途掛かる
（"TensorRT" は §14.12 で `Technical` タグが付くので倍率 1.5。`dictionary_score` 内では二重適用しない）。
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
| 未付与かつ app-profile-spec §7 の文体判定に一致せず、surface の空白（ASCII と U+3000）を除くコードポイントの過半が ASCII で、ASCII 英字を 1 字以上含む（例 "TensorRT", "iPhone"） | `English` |
| その他（`person_name` / `place_name` / `station_name` / `company_org` / `anime_game` / `neologism` / `general`） | category 由来タグなし。未付与候補は app-profile-spec §7 の文体判定・English fallback へ進む |

- category は `DictionaryStore` が同一エントリのカテゴリを union した結果から写像する。
  Host が同一 surface の候補を統合するときも、辞書由来の `Technical` を無タグ・
  `English` の勝者へ引き継ぐ。採用する score / reading / source と予測の出所別枠は変えず、
  表示上限や辞書追加枠の外にある取得済み辞書候補からもタグを補完する。
- **候補タグは単一（スカラ）**。候補モデルは `docs/legacy-parity-spec.md`
  §3.9 の `CandidateTag tag`（IPC `tag: uint8`）でタグを 1 つだけ保持する。
  複数行に該当する候補（例 "TensorRT" = `Technical` かつ surface=ASCII）には
  **辞書 category から付与した `Technical` を保持**する。既存タグの保持と
  未付与候補の文体判定・English fallback は **app-profile-spec §7** を正典とする。
  両タグの同時保持・
  同時 boost は行わない（multi-tag 化は X-2-3 / IPC のスキーマ変更を要し本仕様
  の前提外。将来 `CandidateTag` がリスト化されれば本 precedence を緩和できる）。
- 付与された単一タグに対する M48 boost は **app-profile-spec §7** の clamp 式で 1 回適用する
  （"TensorRT" は `Technical` が選ばれる）。`dictionary_score`（§14.11）には
  含めない（二重適用回避）。
- 候補タグの確定 taxonomy は M52 ベンチで定義する。辞書 category からの
  `Technical` への写像は本表、文体と surface からのタグ判定は app-profile-spec §7 が持つ。
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

## 転送表

§15（system 辞書の物理層）は `docs/azdic-physical-layer-spec.md` へ移した。節番号は保つ。

| 旧節 | 移動先 |
|---|---|
| §15 | `docs/azdic-physical-layer-spec.md` |
