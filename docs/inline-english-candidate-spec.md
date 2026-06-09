# ローマ字入力中インライン英単語候補 仕様（追加機能 / M60）

本書は azooKey-Desktop Windows 版の「ローマ字入力中インライン英単語候補（Inline
English Candidate in Romaji Input）」機能を定める。`plans/windows-port-roadmap.md` の
M60 が本書を参照する。本書は機能仕様（IPC payload・設定項目・ユーザー可視挙動）の
正典であり、進捗・状態は持たない（状態の正典は Linear。`AGENTS.md`「Linear 運用
（管制塔）」参照）。

関連 spec:

- `docs/legacy-parity-spec.md` … 候補ウィンドウ（§1.3）、ライブ変換（§2）、
  カスタムローマ字テーブル（§5）。本機能は候補生成・確定動線を土台にする。
- `docs/rich-features-spec.md` … X-2-3（ラベル付き候補・`CandidateTag::English`）。
  英単語候補の識別・バッジ表示に再利用する。
- `docs/romaji-batch-conversion-spec.md` … §4.1（生ローマ字バッファの常時保持）。
  本機能の英単語候補は同じ生ローマ字バッファを基にする。

## 1. 目的（と背景）

日本語ローマ字入力中に、**英数モードへ切り替えることなく**英単語を入力できるように
する。ユーザーがローマ字で `apple` と打つと、かな漢字候補（あっぷる / アップル …）に
加えて英単語候補（`apple` / `Apple` …）を候補列へ注入し、ユーザーが選べば英単語が
そのまま確定する。

これは azooKey 本家 `ConvertRequestOptions.englishCandidateInRoman2KanaInput`
（`legacy/Core/Sources/Core/InputUtils/SegmentsManager.swift:179`、macOS 版では
`false` で無効）に相当する機能の Windows 版実装である。Windows 版は Swift の
`KanaKanjiConverterModule` を使わず独自 C++ core + Zenzai 構成のため、同等機能を
再実装する。

## 1.1 スコープと非目標

- **スコープ: 候補注入のみ（1 語単位）。** 生ローマ字バッファ 1 つ（語の区切りまで）に
  対して英単語候補を生成し、候補列へ注入する範囲を本書の対象とする。
- **非目標（将来課題）: 連続英文タイプ。** スペースを含む複数語の英文を Japanese モードの
  まま連続入力する体験（語間スペース処理・文単位英語予測）は本書の対象外。将来、
  `requireEnglishPrediction`（legacy）相当の英語予測や M58 一括変換との統合として
  別マイルストーンで検討する（§9）。
- **非目標: 自動モード切替・自動言語判定。** 英単語を検出して英数 IME へ自動で切り替える
  ことはしない。あくまで「追加候補」をユーザーが明示的に選ぶ方式（azooKey と同じ）。
- **非目標: 英語スペル訂正・補完。** 辞書ゲーティング（§4.2）を超えるスペル補正や
  単語補完は対象外（将来課題）。
- **既定 OFF。** 既定 OFF で、有効化時のみ候補を注入する。OFF のとき候補生成・確定の
  挙動は一切変えない。

## 2. 設計原則

- **生ローマ字バッファを基にする。** TIP はかなバッファとは別に**生ローマ字（半角英字）
  バッファを常時保持**する（`docs/romaji-batch-conversion-spec.md` §4.1 が確立した
  方針を共有・再利用）。英単語候補はこの生ローマ字を素材に生成する。
- **決定的ベースライン + 辞書品質向上の二層。** まず生ローマ字そのもの + 大文字化
  バリアント（決定的・辞書不要）を必ず提示できるベースラインとし、英単語辞書による
  ランキング・ゲーティングを上位品質レイヤとする（SimpleConverter → Zenzai と同じ
  段階導入方針）。辞書が無くても最低限の英単語入力が成立する。
- **日本語候補を妨げない。** 英単語候補は**追加候補**であり、明確な英語意図シグナルが
  ない限り日本語候補より上位に出さない。誤って英単語が第一候補を奪わない（§4.3）。
- **既定 OFF・後方互換。** OFF のとき候補生成・rerank・確定を一切変えない。

## 3. 候補生成の素材

`apple` を例に、生成しうる英単語候補:

| 種別 | 例 | 生成条件 |
|---|---|---|
| 生ローマ字そのもの | `apple` | 常時（決定的ベースライン） |
| 先頭大文字 | `Apple` | `inlineEnglishCaseVariants` ON |
| 全大文字 | `APPLE` | `inlineEnglishCaseVariants` ON |
| 全角ローマ字 | `ａｐｐｌｅ` | `fullWidthEnglishCandidate` ON（legacy `fullWidthRomanCandidate` 相当） |
| 辞書一致語 | `apple`（辞書ヒットで上位化） | `inlineEnglishDictionary` ON（品質レイヤ） |

全候補に `CandidateTag::English`（X-2-3）を付与し、候補 UI で `[英]` バッジを出せる
ようにする。

## 4. 注入・ランキング・ゲーティング

### 4.1 注入対象の経路

- **候補ウィンドウ（M5）**: `StartConversion`（Space）で候補列に英単語候補を注入する
  （主経路）。
- **ライブ変換（M14）/ 予測（M15）（任意）**: 実装時に判断。M60 コアは候補ウィンドウ
  注入を必須とし、ライブ/予測への注入は任意拡張とする。

### 4.2 ゲーティング（いつ出すか）

あらゆるローマ字に英単語候補を出すと雑音になる（例: `ko` で英語を出さない）。host 側で
以下のシグナルにより**注入可否と順位**を決める:

- 生ローマ字が英単語辞書にヒットする（品質レイヤ）。
- 生ローマ字がかなとして成立しにくい列を含む（例: `xyz`、日本語ローマ字で生成
  困難な子音連続）。
- 長さが `inlineEnglishMinLength`（既定 2）以上。
- 大文字を含む（Shift 併用で打鍵された）など、英語意図シグナルがある。

辞書が無いベースライン構成では、最低限「生ローマ字そのもの」を**下位の追加候補**として
常に提示しうる（順位は §4.3）。

### 4.3 順位（日本語を奪わない）

- 英語意図シグナルが弱いときは、英単語候補を**日本語上位候補より下**の安定した順位に
  置く（マッスルメモリのため順位を固定）。
- 英語意図シグナルが強い（辞書ヒット + かな不成立など）ときのみ上位化を許す。
- **自動選択はしない。** 第一候補を英単語に自動で差し替えない。ユーザーが明示選択した
  ときだけ確定する。

## 5. 確定と学習

- 英単語候補を確定したとき、surface = 選んだ英語形（`apple` / `Apple` …）。
- 学習（`CommitObservation`）の reading は**生ローマ字（半角英字、例 `apple`）**とする。
  再度同じローマ字を打ったときに学習で英単語候補が再提示されるようにするため。かな
  （`あっぷる`）を reading にすると、かな漢字学習と混線するため避ける。
- 学習ストアには `CandidateSource`（既存 `Candidate.source`）/ English タグで識別して
  記録し、かな漢字学習と区別できるようにする（具体的な格納先は実装時に決定。既存
  `LearningStore` に English チャネルを設けるか、source タグで区別する）。

## 6. IPC プロトコル

新 `MessageType` は追加しない。既存 `QueryCandidates`
（`docs/legacy-parity-spec.md` §1.2、`ipc/include/azookey/ipc/Messages.h`）を
オプションフィールドで拡張する。

### 6.1 Request

```jsonc
{
  "request_id": 0,
  "kana": "あっぷる",
  "raw_romaji": "apple",          // 追加: 生ローマ字（英単語候補の素材）
  "english_candidates": true,      // 追加: 英単語候補注入を有効化（inlineEnglishCandidates 伝搬）
  "context": ""
}
```

`raw_romaji` は本機能の核。`reading`/`kana` には生ローマ字を入れず、別フィールドに
分離する（かな漢字変換の入力を汚染しないため。M58 §4.2 と同じ原則）。
`english_candidates` は `inlineEnglishCandidates` 設定を host へ伝搬する。

### 6.2 Response

既存候補列（`surface` / `reading` / `score` / `source`）に、X-2-3 で計画済みの
`tag: uint8` を付与して English を識別する。新しい応答構造は要らない。

```jsonc
{
  "request_id": 0,
  "candidates": [
    { "surface": "アップル", "reading": "あっぷる", "score": 0.0, "tag": 0 },
    { "surface": "apple",   "reading": "apple",   "score": 0.0, "tag": 4 },  // tag=English
    { "surface": "Apple",   "reading": "apple",   "score": 0.0, "tag": 4 }
  ]
}
```

`tag` 値は `docs/rich-features-spec.md` X-2-3 の `CandidateTag`（`English = 4`）に
合わせる。

### 6.3 staleness・Cancel

候補生成経路のため、既存 M10 の staleness / Cancel をそのまま使う。追加経路は無い。

## 7. 設定スキーマ

実装時に `settings/mvp-settings.schema.json` へ以下を追加する
（`additionalProperties:false` を維持。`description` に対応 M を記載する既存流儀に
合わせる）。本書（spec）が JSON schema の正典であり、実ファイルへの追加は M60 実装時に
行う（本セッションは設計確定のみでスキーマファイルは変更しない）。

| キー | 型 | 既定 | 説明 |
|---|---|---|---|
| `inlineEnglishCandidates` | boolean | `false` | M60: 日本語ローマ字入力中に英単語候補を候補列へ注入する |
| `inlineEnglishCaseVariants` | boolean | `true` | M60: 先頭大文字 / 全大文字バリアントも候補に出す |
| `fullWidthEnglishCandidate` | boolean | `false` | M60: 全角ローマ字候補も出す（legacy `fullWidthRomanCandidate` 相当） |
| `inlineEnglishMinLength` | integer (≥1) | `2` | M60: 英単語候補を出す生ローマ字の最小長 |
| `inlineEnglishDictionary` | boolean | `false` | M60: 英単語辞書によるランキング・ゲーティングを有効化（品質レイヤ。OFF でも生ローマ字 + 大文字化は出せる） |

## 8. テスト計画

- **候補生成** (`core/tests` or host テスト): 大文字化バリアント生成（`apple` →
  `Apple` / `APPLE`）、全角ローマ字生成、最小長ゲーティング、英語意図ヒューリスティック
  （`xyz` 等の非日本語ローマ字判定）。
- **順位** (host テスト): 弱シグナル時に英単語候補が日本語上位候補より下に来ること、
  自動選択されないこと、強シグナル時のみ上位化されること。
- **IPC** (`ipc/tests/payloads_test.cpp`): `QueryCandidates` の `raw_romaji` /
  `english_candidates` フィールド、候補 `tag` の build/parse 往復。
- **学習** (`learning/tests`): 英単語確定で reading=生ローマ字として記録され、かな漢字
  学習と混線しないこと。再度同じローマ字で英単語候補が再提示されること。
- **手動 / 実機（Win11、`gate:human-required`）**: Japanese モードのまま `apple` を打つと
  候補列に `apple` / `Apple` が現れ、選択すると英数モード切替なしで英単語が確定する。
  `ko` 等では英単語が第一候補を奪わない。OFF で英単語候補が一切出ない。

## 9. 将来課題（本書の対象外）

- **連続英文タイプ**: スペースを含む複数語の英文を Japanese モードのまま連続入力する
  体験。語間スペース処理・文単位英語予測（`requireEnglishPrediction` 相当）・M58
  一括変換との統合が必要。
- **英語スペル補正・補完**: 辞書ゲーティングを超える補正・補完。
- **アプリ別の英語優先度**: M48（アプリ別入力プロファイル）で、コードエディタ等では
  英単語タグを boost する（`docs/app-profile-spec.md` の候補タグ重みと接続）。

## 10. 受け入れ条件（M60）

M60 の受け入れ条件は `plans/windows-port-roadmap.md` の M60 に定義する（本書は仕様、
roadmap は受け入れ条件の「定義」、達成状態は Linear）。

## 11. 参照

- 英語候補オプション 旧実装: `legacy/Core/Sources/Core/InputUtils/SegmentsManager.swift`
  （`englishCandidateInRoman2KanaInput` / `fullWidthRomanCandidate` / `requireEnglishPrediction`）
- 候補生成・確定動線: `docs/legacy-parity-spec.md` §1.2・§1.3
- ラベル付き候補（English タグ）: `docs/rich-features-spec.md` X-2-3
- 生ローマ字バッファ保持: `docs/romaji-batch-conversion-spec.md` §4.1
