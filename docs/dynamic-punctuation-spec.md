# 動的自動句読点 仕様（追加機能 / M59）

本書は azooKey-Desktop Windows 版の「動的自動句読点（Dynamic Auto
Punctuation）」機能を定める。`plans/windows-port-roadmap.md` の M59 が本書を
参照する。本書は機能仕様（InputState・IPC payload・設定項目・ユーザー可視挙動）の
正典であり、進捗・状態は持たない（状態の正典は Linear。`AGENTS.md`「Linear 運用
（管制塔）」参照）。

関連 spec:

- `docs/legacy-parity-spec.md` … ライブ変換（§2）。本機能はライブ変換経路を土台にする。
- `docs/rich-features-spec.md` … X-1（ライブ変換のリッチ化）・X-1-2（TypingTempoTracker）。
  挿入安定化に再利用する。X-3（誤変換訂正・Post-Commit Lint）とは目的が異なる（後述 §2）。
- `docs/romaji-batch-conversion-spec.md` … M58-C `batchAutoPunctuation`（一括変換 +
  ai-cleanup 限定の句読点挿入）。本機能（逐次ライブ変換中の動的挿入・削除）とは別経路
  （後述 §2）。
- `docs/tsf-deep-integration-spec.md` … 再変換（M20）・multi-segment。surface に
  読みを持たない句読点が混在するときの再変換境界に影響する（§5）。

## 1. 目的（と背景）

ローマ字でかな漢字変換しながら文章を書くとき、文の区切り（読点 `、`）や文末
（句点 `。`）をユーザーが明示的に打鍵しなくても、IME が**文章の適切な位置へ動的に
句読点を挿入し、文脈の変化に応じて削除・再配置する**。これにより「句読点キーを
意識して打つ」操作を減らし、入力の流れを保つ。

「動的」とは、ライブ変換が打鍵ごとに preedit 全体を再評価するのに合わせ、句読点も
**毎回再計算**することを指す。直前に挿入した読点が、続く入力で文節構造が変われば
自然に消える（= 削除）。

## 1.1 非目標

- **句読点キーの字種設定ではない。** legacy macOS の `Config.PunctuationStyle`
  （`legacy/Core/Sources/Core/Configs/PuntuationConfigItem.swift`）は「`,` / `.` キー
  押下時に `、。` と `，．` のどちらの字を出すか」を決めるキー入力マッピングであり、
  文中への自動挿入とは無関係。本機能はそれとは別物（字種は §6 で別途扱う）。
- **一括変換 + AI 整文（M58-C）ではない。** `batchAutoPunctuation`（M58-C）は
  `batchRomajiConversion` + `batchConversionMode=ai-cleanup` 時に host 側 AI が全文を
  整文する経路で、逐次変換中は動作しない。本機能は**通常のライブ変換中**に動作する
  別系統。両者は設定で独立に ON/OFF できる。
- **文法校正・敬語チェックではない。** 句読点の挿入・削除のみを扱う。より広い不自然さ
  検出は X-3（Post-Commit Lint / DetectAnomalies）の領域で、本書は扱わない。
- **既定 ON ではない。** 既定 OFF の実験機能であり、有効化時のみ動作する。OFF のとき
  ライブ変換の挙動は一切変えない。

## 2. 設計原則

- **ライブ変換の上に載せる。** 動的句読点は `liveConversion == true` を前提とする
  enhancement である。候補ウィンドウ方式（liveConversion OFF）では「流れる文の
  preedit」が存在しないため動作しない。`liveConversion == false` のときは本機能を
  無効化する（設定が ON でも適用しない）。
- **かなバッファが正典、句読点は派生物。** ユーザーが打鍵した内容（かな / 生ローマ字）
  だけが編集可能な正典バッファである。自動句読点は surface のレンダリング時に**毎回
  再計算される派生物**であり、ユーザーが直接編集・カーソル選択する対象にしない
  （§5）。これにより「削除」を特別なロジックなしに full-preedit 再計算で実現する。
- **決定的ベースライン + ニューラル品質向上の二層。** まず host 側の決定的な
  文節境界ヒューリスティック（`PunctuationInserter`）で挿入位置を決め、テスト可能な
  baseline とする。zenz が surface に句読点を直接出力できる構成では、それを上位品質
  レイヤとして使う（SimpleConverter → Zenzai と同じ段階導入方針）。
- **既定 OFF・後方互換。** OFF のとき従来のライブ変換・状態遷移・学習を一切変えない。

## 3. InputState への統合

新しい状態は追加しない。`docs/legacy-parity-spec.md` §1.2 の `Composing` /
`Previewing`（ライブ変換中）の中で動作する。`Previewing` の preedit 更新時に、
surface へ自動句読点を含めて表示する。

遷移への影響（`dynamicPunctuation == true` かつ `liveConversion == true` のとき）:

| 現状 | 入力 | 次状態 | 副作用 |
|---|---|---|---|
| Composing/Previewing | Input | Previewing | `QueryLiveConversion(auto_punctuation=true)` 送信。応答 surface に句読点を含めて Preedit 全体を差し替え（句読点は再計算で増減しうる） |
| Previewing | Backspace | Previewing/Composing | **かなバッファを 1 単位削除**（自動句読点は削除単位に数えない。§5）。再 `QueryLiveConversion` |
| Previewing | Commit (Enter) | Idle | 句読点を含む全文を確定。`CommitObservation` は自動句読点を分離して観測（§5・§7） |
| Previewing | Cancel (Esc) | Idle | CancelComposition |

`dynamicPunctuation == false` または `liveConversion == false` のときは、
`QueryLiveConversion` に `auto_punctuation=false` を載せ、従来どおり句読点を挿入しない。

## 4. 挿入・削除の判定

### 4.1 挿入位置

host 側（`inference-host`）で判定する。二層構成:

- **決定的レイヤ（既定・M59 コア）**: `PunctuationInserter`（新規）が、変換後 surface と
  **文節境界（live 変換応答の `segments[]`、§7.2）**を入力に、文節境界での読点 `、` と
  文末の句点 `。` を挿入する。形態素解析器を持たないため、各文節末尾の**表層サフィックス
  一致**で節境界を判定する。規則表（既定。bench M52・実機で調整。本表が決定的レイヤの
  正典）:

  **読点 `、` 挿入（文節 i と i+1 の境界。直前文節 i の末尾表層で判定）**

  | 分類 | トリガ（直前文節末尾の表層パターン） | 例 | 動作 |
  |---|---|---|---|
  | 接続助詞(順接/理由) | 「ので」「から」「ため」 | 雨なので→ | 境界に読点 |
  | 接続助詞(逆接) | 「が」「けど」「けれど」「のに」「ても」「でも」 | 行ったが→ | 境界に読点 |
  | 連用中止/並列 | 「て」「で」(連用中止)、「し」、「たり」 | 食べて→ / 安いし→ | 境界に読点 |
  | 引用/条件 | 「と」(引用)、「ば」「なら」「たら」 | 〜すれば→ | 条件節境界に読点 |
  | 副詞節 | 「とき」「ところ」「場合」「うえで」 | 〜の場合→ | 境界に読点 |
  | 接続詞(文頭) | 文節頭が「しかし」「だから」「また」「そして」「ただし」「つまり」 | 〜。しかし→ | 接続詞**直後**に読点 |

  **句点 `。` 挿入**

  | 分類 | トリガ | 動作 |
  |---|---|---|
  | 終止 | 末尾文節が終止形/丁寧形（「です」「ます」「だ」「である」「した」「ない」等）で終わり、かつ確定直前 or `onPause` idle | 文末に句点 |
  | 疑問 | 末尾が疑問終助詞「か」 | 文末に句点（`？` 化は将来・設定） |

  **抑制（挿入しない）規則**

  | 条件 | 理由 |
  |---|---|
  | 直前/直後が既に句読点 | 連続句読点防止 |
  | 文頭（バッファ先頭） | 行頭読点防止 |
  | 直近 K=6 文字以内に既に読点がある | 読点過密防止 |
  | 全体が N=8 文字未満かつ単一文節 | 短文への過剰挿入防止 |
  | 英数字列・記号列・URL 様パターンの内部 | 誤挿入防止（M60 英単語候補・記号との干渉回避） |
  | 当該文節境界スコアが `segmentBoundaryConfidence`（既定 0.5）未満 | ライブ変換中の不安定境界への挿入を防ぐ |
  | タイピング中（`onPause` 設定時） | §4.3 安定化 |

- **ニューラルレイヤ（任意・品質向上）**: zenz が surface に句読点を直接出力できる構成
  では、その出力を採用する。zenz 出力と決定的レイヤの整合は実装時に定める（既定は
  決定的レイヤ、zenz 句読点が利用可能なら上書き）。

### 4.2 削除・再配置

ライブ変換は打鍵ごとに preedit 全体を再評価し置き換える（M14）。したがって、ある
位置の読点は、続く入力で文節構造が変わって `PunctuationInserter` がそこを境界と
判定しなくなれば**自然に消える**。明示的な削除ロジックは持たず、full-preedit 再計算で
挿入・削除・再配置を一括して扱う。

### 4.3 安定化（ちらつき抑制）

打鍵ごとに句読点が出現・消滅すると視覚的に不安定になる。安定化規則:

- 設定 `dynamicPunctuationStability`:
  - `onPause`（既定）: タイピング中（`TypingTempoTracker::IsTyping()` = X-1-2 を再利用、
    平均打鍵間隔 < 閾値）は句読点を**挿入しない**。入力が止まった（idle）瞬間に再評価して
    挿入する。連続入力中のちらつきを防ぐ。
  - `eager`: 打鍵ごとに常に再評価して挿入する（反応は速いがちらつきうる）。
- どちらでも、確定（Enter）直前には必ず最終評価を行い、文末句点を補う。

## 5. 読み↔surface の非対称と編集

自動句読点は**ユーザーが打鍵していない**ため、対応する読みを持たない。これが
surface と読みの 1:1 対応を崩す。各サブシステムの扱いを以下に固定する。

- **Backspace の削除単位**: Backspace は**かな / 生ローマ字バッファの 1 単位**を削除し、
  自動句読点は削除単位に数えない。句読点は再計算で増減する派生物のため、ユーザーが
  Backspace で句読点だけを消そうとしても、消えるのは直近のかな単位であり、句読点の
  有無は再評価で決まる。
- **カーソル/キャレット**: M59 コアは**バッファ末尾編集**（末尾への追記・末尾からの削除）を
  対象とする。preedit 途中へのキャレット移動編集（文中挿入）は、読み↔surface の
  オフセット写像（自動句読点ぶんのズレ）を要するため M59 コアの対象外とし、M20
  （再変換 / multi-segment）統合時に扱う。
- **学習（CommitObservation）**: 学習ストアは `(reading, surface)` ペアを観測する。
  surface に読みを持たない句読点が混じると学習が汚染される。確定時は応答の `segments`
  情報を使って**自動句読点スパンを分離**し、各文節の `(reading, surface)` を句読点抜きで
  観測する（§7）。自動句読点そのものは学習対象にしない。
- **再変換（M20）**: 確定済みテキストに含まれる自動句読点は読みを持たないため、
  再変換時は**文節境界 / リテラル**として扱い、読みへ逆変換しない。詳細は M20 統合時に
  `docs/tsf-deep-integration-spec.md` 側で確定する。

### 5.1 データ構造

正典バッファは**読み（かな）+ 生ローマ字**であり、句読点込み surface は派生物。
TIP 側 composition 状態:

```cpp
// TIP（tsf-tip）side composition state（概念。既存 TextService に統合）
std::string kana_buffer_;        // 打鍵由来の読み。編集の正典
std::string raw_romaji_;         // 生ローマ字（M58/M60 と共有）
std::string rendered_surface_;   // 句読点込み表示文字列。毎回再計算で全置換
std::vector<LiveSegment> segments_;  // host 応答（§7.2）。auto_punctuation マーカ付き
```

`LiveSegment`（host 応答 = §7.2 の各要素）:

```cpp
struct LiveSegment {
  uint32_t start_char;       // rendered_surface_ 上の開始（UTF-16 code unit）
  uint32_t end_char;         // 同 終了（排他）
  double   score;            // 文節境界/変換信頼度
  bool     auto_punctuation; // true: 読みを持たない自動挿入句読点
  std::string reading;       // 文節読み（auto_punctuation=true は空）
};
```

不変条件: `auto_punctuation == true` の要素は `reading.empty()` かつ
`kana_buffer_` に対応文字を持たない（純粋な派生物）。

### 5.2 Backspace（削除単位）

自動句読点は `kana_buffer_` に存在しないため、削除は常に**読みバッファ**に対して行う。

```text
on Backspace in Previewing(dynamicPunctuation && liveConversion):
    if RomajiKanaConverter has pending:   # 例: "k" だけ打って未確定
        PopPending()                      # 生ローマ字を 1 つ戻す
    else:
        remove last 1 kana unit from kana_buffer_ (+ 対応 raw_romaji_ 末尾)
    re-issue live conversion (auto_punctuation=true)
    # rendered_surface_ と segments_ を新応答で全置換 → 句読点は再計算で増減
```

ユーザーには「Backspace で句読点が消えた」ように見える場合があるが、実際は読みが
変わって再計算で句読点が落ちただけ。「読点だけ残してかなを消す」「かなだけ残して
読点を消す」という分離編集は提供しない（句読点は読みに従属する派生物のため）。

### 5.3 確定と学習分離（CommitObservation）

```text
on Commit(Enter):
    final = re-evaluate(kana_buffer_, auto_punctuation=true)  # 文末句点を補う最終評価
    EndComposition(final.rendered_surface)                    # 句読点込みでアプリへ確定
    # 学習は auto_punctuation セグメントを除外し、文節ごとに観測
    for seg in final.segments where !seg.auto_punctuation:
        CommitObservation(reading = seg.reading,
                          chosen.surface = substr(final.rendered_surface, seg))
    # auto_punctuation セグメントは学習に渡さない（reading 無し）
```

**学習不変条件**: 自動句読点の文字は `CommitObservation` の `reading` にも
`chosen.surface` にも含めない。既存 `CommitObservationRequest`（単一 `reading` +
`chosen`）で表現するには 2 経路:

- (推奨) 文節ごとに複数回 `CommitObservation` を送る（`!auto_punctuation` のみ）。
- (縮退) 単一文節時は `chosen.surface` から自動句読点を除去した文字列を 1 回送る。

どちらも IPC payload の変更を要しない（§7.4）。multi-segment commit を専用 payload 化
する場合は M58-B の segments commit と統合する（将来）。

### 5.4 カーソル/オフセット写像

M59 コアは**末尾編集（末尾 append / 末尾 delete）のみ**を対象とするため、
`reading 末尾 ↔ surface 末尾` の対応で足り、フルオフセット写像は不要。
文中キャレット編集（preedit 途中挿入）は、`segments_` の
`(start_char, end_char, reading)` から surface↔reading 逆写像を構築する必要があり、
M20（再変換 / multi-segment）統合時に対応する。`auto_punctuation` セグメントは
reading 長 0 として写像に組み込む。

## 6. 表示と字種

- **字種**: 設定 `dynamicPunctuationStyle` で挿入する句読点の字種を選ぶ。
  - `ja`（既定）: 読点 `、` / 句点 `。`
  - `fullwidth_latin`: 全角カンマ `，` / 全角ピリオド `．`
  （legacy `Config.PunctuationStyle` の 4 値のうち、自動挿入で意味を持つ「読点字 / 句点字」の
  組のみを 2 値で表現する。キー押下時の字種設定とは別物。）
- **DisplayAttribute（任意）**: 自動挿入した句読点を、ユーザー打鍵文字とは異なる
  控えめな属性（例: X-1-1 の `kLiveAttrTentativeGuid` 相当の薄い下線）で描画し、
  「IME が補った・以後自動調整される」ことを示してよい。既定は周囲と同一属性で
  目立たせない。実装時に決定する。

## 7. IPC プロトコル

新 `MessageType` は追加しない。**実装の現状整合**: ライブ変換は現状
`QueryCandidatesRequest.live = true`（`ipc/include/azookey/ipc/Payloads.h`）で運ばれており、
`docs/legacy-parity-spec.md` §2.2 が提案した独立 `QueryLiveConversion` payload は未実装。
よって M59 は**現行 `QueryCandidates` 経路にオプションフィールドを追加**する。M14 が独立
`QueryLiveConversion` payload に分離する場合は、本節のフィールドを**そのまま新 payload へ
移設**する（フィールド定義・既定値・後方互換規約は不変）。

すべての追加フィールドは**任意・後方互換**とし、既存 `Payloads.cpp` の流儀
（Build = `o.emplace(...)`、Parse = `GetBool/GetString/GetUInt/...().value_or(既定)`）に従う。
省略時は既定値となり、旧 TIP / 旧 host と相互運用できる。

### 7.1 Request（`QueryCandidatesRequest` 拡張）

既存フィールド: `reading` / `left_context` / `max_candidates` / `live`。追加:

| field | 型 | 既定 | 説明 |
|---|---|---|---|
| `auto_punctuation` | bool | `false` | `dynamicPunctuation` を伝搬。host が ON/OFF を判別できるよう必須伝搬 |
| `punctuation_style` | string | `"ja"` | `"ja"`（`、。`）/ `"fullwidth_latin"`（`，．`） |

```jsonc
{
  "reading": "きょうはいいてんきです",
  "left_context": "",
  "max_candidates": 10,
  "live": true,                  // ライブ変換経路（既存）
  "auto_punctuation": true,       // 追加
  "punctuation_style": "ja"       // 追加
}
```

Build/Parse 追記（規約例）:

```cpp
// Build
o.emplace("auto_punctuation", j::Value(p.auto_punctuation));
o.emplace("punctuation_style", j::Value(p.punctuation_style));
// Parse（後方互換）
p.auto_punctuation  = v->GetBool("auto_punctuation").value_or(false);
p.punctuation_style = v->GetString("punctuation_style").value_or("ja");
```

`auto_punctuation` は `live == true` のときのみ意味を持つ（host は `live==false` では無視）。

### 7.2 Response（`QueryCandidatesResponse` に `segments[]` を追加）

現状 `QueryCandidatesResponse` は `candidates[]` + `partial` のみで文節構造を持たない。
M59 は**任意配列 `segments[]` を新規追加**する（X-1-1 の segments と整合）。`segments` を
省略した応答は従来どおり（句読点なし・文節情報なし）に解釈される。

各 segment フィールド:

| field | 型 | 必須 | 説明 |
|---|---|---|---|
| `start_char` | uint32 | ○ | 最良 surface 上の開始オフセット（**UTF-16 code unit**。TSF `ITfRange` と整合） |
| `end_char` | uint32 | ○ | 同 終了（排他） |
| `score` | number | ○ | 文節境界/変換信頼度（抑制判定 `segmentBoundaryConfidence` に使用） |
| `auto_punctuation` | bool | 既定 false | true = 読みを持たない自動挿入句読点スパン |
| `reading` | string | 既定 "" | 文節読み（`auto_punctuation=true` は空） |

最良 surface は既存 `candidates[0].surface`（句読点込み）を用い、`segments[]` がその
内部構造を与える（重複した `surface` フィールドは追加しない）。

```jsonc
{
  "candidates": [
    { "surface": "今日はいい天気です。", "reading": "きょうはいいてんきです", "score": 0.93, "source": "model" }
  ],
  "partial": false,
  "segments": [
    { "start_char": 0,  "end_char": 3,  "score": 0.95, "auto_punctuation": false, "reading": "きょうは" },
    { "start_char": 3,  "end_char": 9,  "score": 0.90, "auto_punctuation": false, "reading": "いいてんきです" },
    { "start_char": 9,  "end_char": 10, "score": 0.0,  "auto_punctuation": true,  "reading": "" }
  ]
}
```

> `start_char`/`end_char` を UTF-16 code unit に固定する理由: TIP は `ITfRange::SetText` /
> Property を UTF-16 オフセットで操作するため、UTF-8 codepoint で返すと TIP 側で再計算が
> 必要になり境界ズレの温床になる。host は UTF-16 単位でオフセットを計算して返す。

### 7.3 キャンセル・staleness

ライブ変換経路のため、既存 M10 の `ipc_pending_id_` staleness check と `Cancel`
（`CancelPayload.target_request_id`）をそのまま使う（`docs/legacy-parity-spec.md` §2.5）。
古い応答は破棄。確定時は in-flight のライブ変換リクエストに `Cancel` を送ってから
`EndComposition`。

### 7.4 CommitObservation 整合（学習分離）

既存 `CommitObservationRequest`（`reading` + `chosen:CandidateField` + `shown[]` +
`left_context` + `timestamp_ms`）を**変更せず**、§5.3 の不変条件（自動句読点を `reading` /
`chosen.surface` に含めない）を満たす。文節ごとに複数回 `CommitObservation` を送るか
（推奨）、単一文節時は句読点除去済み `chosen.surface` を 1 回送る（縮退）。multi-segment
commit を専用 payload にするのは M58-B segments commit との統合（将来）に委ねる。

### 7.5 payloads_test 期待値（`ipc/tests/payloads_test.cpp`）

- `QueryCandidatesRequest` round-trip で `auto_punctuation` / `punctuation_style` が保存される。
- これらを欠く JSON のパースで `false` / `"ja"` の既定になる（後方互換）。
- `QueryCandidatesResponse` round-trip で `segments[]`（`auto_punctuation` マーカ・`reading`・
  UTF-16 オフセット）が保存される。`segments` を欠く JSON は空 `segments` にパースされ、
  従来応答として解釈できる。

## 8. 設定スキーマ

実装時に `settings/mvp-settings.schema.json` へ以下を追加する
（`additionalProperties:false` を維持。`description` に対応 M を記載する既存流儀に
合わせる）。本書（spec）が JSON schema の正典であり、実ファイルへの追加は M59 実装時に
行う（本セッションは設計確定のみでスキーマファイルは変更しない）。

| キー | 型 | 既定 | 説明 |
|---|---|---|---|
| `dynamicPunctuation` | boolean | `false` | M59: ライブ変換中に句読点を動的挿入・削除する（`liveConversion=true` のときのみ有効） |
| `dynamicPunctuationStyle` | enum `ja`/`fullwidth_latin` | `ja` | M59: 自動挿入する句読点の字種（`、。` / `，．`） |
| `dynamicPunctuationStability` | enum `onPause`/`eager` | `onPause` | M59: 挿入タイミング（入力停止時のみ / 打鍵ごと） |
| `segmentBoundaryConfidence` | number (0.0–1.0) | `0.5` | M59: この境界スコア未満の文節境界には読点を挿入しない（§4.1 抑制規則） |

## 9. テスト計画

- **句読点挿入ロジック** (`core/tests/punctuation_inserter_test.cpp` 新規 or
  host テスト): 文節境界・接続表現での読点挿入、文末句点、連続読点の抑制、字種切替
  （`ja` / `fullwidth_latin`）。
- **状態機械** (`core/tests/input_state_test.cpp`): `dynamicPunctuation` ON で
  Backspace がかな単位を削除し自動句読点を削除単位に数えないこと、入力変化で句読点が
  再配置・削除されること、`liveConversion=false` で本機能が無効化され従来遷移が不変で
  あること、OFF で従来遷移が不変であること。
- **IPC** (`ipc/tests/payloads_test.cpp`): `QueryLiveConversion` の `auto_punctuation` /
  `punctuation_style` フィールド、`segments[].auto_punctuation` の build/parse 往復。
- **学習分離** (`learning/tests`): 自動句読点を含む確定で、句読点が `(reading, surface)`
  観測に混入しないこと。
- **安定化** (host テスト or 状態機械): `onPause` でタイピング中は句読点が出ず、idle で
  挿入されること（`TypingTempoTracker` をモックした時間注入）。
- **手動 / 実機（Win11、`gate:human-required`）**: ライブ変換 ON + `dynamicPunctuation`
  ON で文を打つと節境界・文末に句読点が現れ、続けて打つと再配置・削除され、Enter で
  妥当な句読点付き日本語が確定する。OFF で句読点が一切自動挿入されない。学習が汚染
  されない（直後に同じ読みを打って句読点なしの素直な候補が出る）。

## 10. 受け入れ条件（M59）

M59 の受け入れ条件は `plans/windows-port-roadmap.md` の M59 に定義する（本書は仕様、
roadmap は受け入れ条件の「定義」、達成状態は Linear）。

## 11. 参照

- ライブ変換 旧実装: `legacy/Core/Sources/Core/InputUtils/SegmentsManager.swift`
- 句読点字種設定（本機能とは別物）: `legacy/Core/Sources/Core/Configs/PuntuationConfigItem.swift`
- ライブ変換仕様: `docs/legacy-parity-spec.md` §2
- リッチ化（信頼度 segments・TypingTempoTracker）: `docs/rich-features-spec.md` X-1
- 一括 AI 整文の句読点（別経路）: `docs/romaji-batch-conversion-spec.md` §5・M58-C
