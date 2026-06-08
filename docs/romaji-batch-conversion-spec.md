# ローマ字一括変換 仕様（追加機能 / M58）

本書は azooKey-Desktop Windows 版の「ローマ字一括変換（Batch Romaji
Conversion）」機能を定める。`plans/windows-port-roadmap.md` の M58（M58-A /
M58-B / M58-C）が本書を参照する。本書は機能仕様（InputState・IPC payload・
設定項目・ユーザー可視挙動）の正典であり、進捗・状態は持たない（状態の正典は
Linear。`AGENTS.md`「Linear 運用（管制塔）」参照）。

関連 spec:

- `docs/legacy-parity-spec.md` … InputState 状態機械（§1.2）、キーバインド（§1.4）、
  ライブ変換（§2）。本機能はこれらを土台にする。
- `docs/rich-features-spec.md` … AI 変換・文単位予測。M58-C の AI 整文と接続。
- `docs/tsf-deep-integration-spec.md` … 再変換・multi-segment。M58-B の文節再変換と接続。

## 1. 目的（と背景）

「かな漢字変換のたびに思考と視線が中断される」問題を解消する。ユーザーは
ローマ字で文章を最後まで打ち切り、**最後に一度だけ一括変換**する。変換待ち・
候補選択・誤変換修正が入力の流れに割り込まないため、画面を注視せず（相手の目を
見たまま）連続入力できる。

参考: note 記事「ローマ字のまま入力して最後に AI 変換」
（https://note.com/fta7/n/nb8ccb733c425）、議論まとめ
（https://togetter.com/li/2702866）。これらが提起する入力スタイルを、決定的な
ニューラルかな漢字変換（既定）と、AI による誤字補正・整文（オプション）の
2 系統として IME 機能化する。

### 1.1 非目標

- 既定の逐次変換（liveConversion / 予測）の置き換えではない。`batchRomajiConversion`
  を有効化したときのみ動作する追加モードである（既定 OFF）。
- 上流のデータ生成・LLM ホスティングは範囲外。AI 整文は既存 `aiBackend`
  （local-zenzai / openai）に委譲する。

## 2. 設計原則

- **蓄積中は IPC を発火しない。** ローマ字→かな変換はローカル（`RomajiKanaConverter`、
  `core/src/RomajiKanaConverter.cpp`）で逐次行い、推論（候補・予測・ライブ変換）の
  クエリは一括変換トリガまで一切送らない。これが「中断ゼロ」の中核。
- **既定 OFF・後方互換。** OFF のときは従来の逐次変換・状態遷移を一切変えない。
- **段階導入。** M58-A（コア）→ M58-B（長文・文節再変換）→ M58-C（AI 整文）。

## 3. InputState への統合

`core/include/azookey/core/InputState.h`（`docs/legacy-parity-spec.md` §1.2 で
定義）の `InputStateKind` に状態を 1 つ追加する。

```cpp
enum class InputStateKind {
    Idle,
    Composing,
    Previewing,
    BatchAccumulating,   // 追加: ローマ字蓄積中（推論クエリを発火しない）
    BatchConverting,     // 追加: 一括変換リクエスト送信済み・応答待ち（候補未確定）
    Selecting,
    ReplaceSuggestion,
    UnicodeInput,
};
```

遷移表（`batchRomajiConversion == true` のとき。主要遷移のみ）:

| 現状 | 入力 | 次状態 | 副作用 |
|---|---|---|---|
| Idle | Input | BatchAccumulating | StartComposition / ローカルでかなバッファ更新 / Preedit 更新 |
| BatchAccumulating | Input | BatchAccumulating | かなバッファ更新 / Preedit 更新（**IPC 無し**） |
| BatchAccumulating | Backspace | BatchAccumulating / Idle | バッファ 1 単位削除（空になれば Idle） |
| BatchAccumulating | StartConversion (Space) | BatchConverting | `QueryBatchConversion` 送信（応答待ち。Preedit は蓄積内容のまま） |
| BatchConverting | 中間応答 (`partial:true`) | BatchConverting | `full_surface` を Preedit に漸進更新（**確定不可**。`segments` を蓄積） |
| BatchConverting | 最終応答 (`partial:false`) | Selecting | 完全な `full_surface` / `segments` を確定可能候補として表示 |
| BatchConverting | Commit (Enter) | BatchConverting | 無視（最終応答前は確定しない。部分結果を commit させない） |
| BatchConverting | Input | BatchAccumulating | in-flight を `Cancel` → 打鍵をバッファに追加 |
| BatchConverting | Cancel (Esc) | BatchAccumulating | in-flight を `Cancel` → 蓄積状態へ戻す |
| Selecting | NextCandidate / SelectByDigit | Selecting | 候補選択（M58-B では文節単位） |
| Selecting | Forward/Backward | Selecting | 文節カーソル移動（M58-B） |
| Selecting | Commit (Enter) | Idle | EndComposition + CommitObservation（全文確定） |
| Selecting | Cancel (Esc) | BatchAccumulating | 変換結果を破棄し蓄積状態へ戻す |
| BatchAccumulating | Cancel (Esc) | Idle | CancelComposition |

`batchRomajiConversion == false` のときは従来どおり Composing / Previewing に
遷移し、本状態は使われない。ライブ変換（M14）・予測（M15）は
BatchAccumulating 中は抑制する。

`QueryBatchConversion` は非同期のため、Space 直後は `Selecting` ではなく
`BatchConverting`（応答待ち）に入る。応答前の追加打鍵 / Esc は in-flight を `Cancel`
して `BatchAccumulating` に戻す（空の候補集合で `Selecting` に入らない）。

M58-B の長文変換では `partial:true` の中間応答が複数届きうる。中間応答は
`BatchConverting` のまま `full_surface` を Preedit に漸進更新するだけで**確定不可**と
し、**最終応答 `partial:false` を受信してから初めて `Selecting`（確定可能）へ遷移**
する。これにより、最初のチャンクだけが確定して残りの蓄積入力が欠落する事故を防ぐ
（`BatchConverting` 中の Enter は無視する）。M58-A は単一の `partial:false` 応答で
即 `Selecting` へ遷移する。

## 4. 入力中の表示・トリガ・確定

### 4.1 蓄積中の Preedit 表示

設定 `batchRomajiPreviewStyle` で切替える（両対応）。

- `kana`（既定）: `RomajiKanaConverter` の出力かな（例: `nihongo` → `にほんご`）を
  アンダーライン付き Preedit に表示。漢字変換はしない。読みやすく既存 IME に近い。
- `romaji`: 入力された生ローマ字（半角英字、例 `nihongo`）をそのまま Preedit に
  表示。画面を見ない運用に最適。`TextService` は生ローマ字文字列を別途保持する。

いずれのモードでも内部ではかなバッファを保持し、一括変換時の読みとして使う。
長音は既存 `RomajiKanaConverter` の仕様どおり `-`（ハイフン）入力で `ー` になる
（例: `ko-` → `こー`）。`ii` / `oo` 等は `いい` / `おお` になり `ー` には正規化
されないため、本書の例も実際の出力に合わせる（`kana` 読みがそのまま `reading` と
して zenz に渡るため、例と実装の不一致は変換品質を損なう）。

### 4.2 一括変換トリガ

**Space（`StartConversion`）をバッファ全体に適用**する。日本語入力中は空白を
打たないため、`docs/legacy-parity-spec.md` §1.4 の既存キーバインドと衝突しない。
`QueryBatchConversion` を送信する際は、`batchRomajiPreviewStyle` の表示設定や
`batchConversionMode` に関わらず、**常に蓄積した全文かなを `reading` に入れる**。
生ローマ字は `reading` に入れず、`ai-cleanup` 用に `raw_romaji`（任意）へ別途
格納する（§6.1）。`reading` に生ローマ字を入れると `neural` 変換および
`ai-cleanup`→`neural` fallback が zenz にアルファベットを渡してしまい、変換が
成立しなくなるため。

### 4.3 確定・キャンセル

- Enter（`Commit`）で全文を確定（`EndComposition` + `CommitObservation`）。
- Esc（`Cancel`）は Selecting / BatchConverting からは蓄積状態へ戻し（後者は in-flight を
  `Cancel`）、BatchAccumulating からは破棄。
- M58-B では Selecting 中に ←/→ で文節カーソル移動、Space/数字キーで文節候補切替。

## 5. 一括変換モード（2 系統）

設定 `batchConversionMode` で選択する。

- `neural`（既定 / M58-A）: 蓄積かなを Zenzai/zenz でかな漢字変換する。誤字補正・
  句読点の自動挿入はしない素直な変換。
- `ai-cleanup`（M58-C）: `aiBackend`（local-zenzai / openai）に全文を渡し、
  誤字補正・句読点挿入・整文まで委譲する。非決定的でレイテンシが大きい。
  `includeContextInAITransform` の文脈付与方針と整合させる。`aiBackend == none`
  のときは `neural` に fallback する。

## 6. IPC プロトコル

新 `MessageType::QueryBatchConversion` を追加する（`ipc/include/azookey/ipc/Messages.h`
・`ipc/include/azookey/ipc/Payloads.h`）。

### 6.1 Request

```jsonc
{
  "reading": "全文かな",        // 必ず蓄積した全文かな（生ローマ字は入れない）
  "raw_romaji": "nihongo...",   // 任意。生ローマ字。ai-cleanup の誤字補正に使う
  "left_context": "",            // 直近確定文（任意）
  "mode": "neural",             // "neural" | "ai-cleanup"
  "max_candidates": 5            // 文節あたり候補数
}
```

### 6.2 Response

```jsonc
{
  "segments": [                  // 文節構造（M58-B で複数、M58-A は単一でも可）
    {
      "surface": "日本語",
      "reading": "にほんご",
      "candidates": [ { "surface": "...", "reading": "...", "score": 0.0 } ]
    }
  ],
  "full_surface": "日本語を入力する", // 全文の最良連結（Preedit 即時表示用）
  "partial": false               // チャンク逐次変換途中は true
}
```

既存 `QueryCandidates`（単一読み・単一候補列）と分離する理由: 一括変換は
**複数文節（segments）構造**を返す必要があり、既存 `Candidate`
（`core/include/azookey/core/Candidate.h`、segments フィールド無し）では表現
できない。multi-segment は `docs/tsf-deep-integration-spec.md` で将来課題と
されていた領域であり、本機能で初めて必須化する。

### 6.3 キャンセル・長文

- 進行中の一括変換は既存 `CancelPayload`（`target_request_id`）でキャンセルできる。
  `BatchConverting`（応答待ち）中に追加打鍵 / Esc があれば in-flight リクエストを
  破棄して `BatchAccumulating` へ戻す（§3）。
- 1 リクエストは `ipc/include/azookey/ipc/Limits.h` の `kMaxJsonInputBytes` /
  `kMaxFrameSize`（ともに 1 MB）以内に収める必要がある。IPC フレーミング/パーサが
  この上限を超えるフレームを `inference-host` 到達前に拒否するため、フレーム上限を
  超えうる蓄積バッファは **TIP 側で文境界により複数リクエストへ事前分割**してから
  送る（M58-B）。host 側分割は到達済みリクエストにしか効かず、フレーム上限超は救済
  できない。
- これとは別に、フレーム上限内のリクエストでも zenz のコンテキスト長に収めるため、
  `inference-host` 側でさらに文境界でチャンク分割して逐次変換する（§7）。

## 7. 長文・性能・失敗時（主に M58-B）

- **TIP 側事前分割（フレーム上限対策）**: 蓄積バッファが IPC フレーム上限
  （`kMaxFrameSize` = 1 MB）を超えうる場合、TIP は送信前に文境界で複数の
  `QueryBatchConversion` リクエスト（各々上限未満）へ分割して送る。フレーム上限を
  超える単一リクエストは IPC 層で拒否されるため、host 側分割では救済できない。
- **host 側チャンク分割（モデルコンテキスト対策）**: フレーム上限内の 1 リクエストでも、
  `inference-host` 側で文境界（句点・改行・推定文節境界）により分割し、zenz の
  コンテキスト長制限内で逐次変換 → 結合する。途中経過は `partial: true` で返し、
  Preedit を漸進更新する。
- **蓄積長上限**: バッファ長の上限と超過時の挙動（強制分割 / 警告ビープ）を実装で
  規定する。
- **非同期・キャンセル可能**: 一括変換は UI スレッドをブロックしない。
- **fallback 連鎖**: `ai-cleanup` 失敗時は `neural` へ、`neural` 失敗時はかなのまま
  確定可能にする（入力を失わない）。

## 8. 設定スキーマ

`settings/mvp-settings.schema.json` に以下を追加する（`additionalProperties:false`
を維持。`description` に対応 M を記載する既存流儀に合わせる）。

| キー | 型 | 既定 | 説明 |
|---|---|---|---|
| `batchRomajiConversion` | boolean | `false` | M58-A: ローマ字を蓄積し最後に一括変換するモードを有効化 |
| `batchRomajiPreviewStyle` | enum `kana`/`romaji` | `kana` | M58-A: 蓄積中 Preedit の表示（かなプレビュー / 生ローマ字） |
| `batchConversionMode` | enum `neural`/`ai-cleanup` | `neural` | M58-A/M58-C: 一括変換の処理系（zenz かな漢字変換 / AI 整文） |
| `batchAutoPunctuation` | boolean | `false` | M58-C: `ai-cleanup` 時に句読点を自動挿入 |

## 9. テスト計画

- **状態機械** (`core/tests/input_state_test.cpp`): `batchRomajiConversion` ON で
  Idle→BatchAccumulating→（Space）→BatchConverting→（`partial:false`）→Selecting→
  （Enter）→Idle、`partial:true` 中間応答では BatchConverting に留まり Enter が
  確定しないこと、BatchConverting 中の追加打鍵 / Esc で in-flight Cancel →
  BatchAccumulating 復帰、Esc の戻り、OFF で従来遷移が不変であることを網羅。
- **IPC** (`ipc/tests/payloads_test.cpp`): `QueryBatchConversion` の build/parse、
  segments 構造の往復、`partial` フラグ、Cancel の ID 整合。
- **変換** (`inference-host/tests`): 固定かな全文 → 妥当な segments / full_surface。
  チャンク分割（句点を含む長文）・キャンセルの動作。
- **手動 / 実機（Win11、`gate:human-required`）**: `kyouhaiitenkidesu`（→ きょうは
  いいてんきです → 今日はいい天気です）等を蓄積 → Space で一括変換 → Enter 確定で
  妥当な日本語が入る。設定 OFF で従来逐次動作が不変。`batchRomajiPreviewStyle`
  切替が反映される。

## 10. 受け入れ条件（M58 全体）

M58-A / M58-B / M58-C ごとの受け入れ条件は `plans/windows-port-roadmap.md` の
各マイルストーンに定義する（本書は仕様、roadmap は受け入れ条件の「定義」、
達成状態は Linear）。
