---
kind: spec
milestones: [M48, M67]
canonical_for: ["アプリ別入力プロファイル", "入力欄の種類による切替"]
---

# アプリ別入力プロファイル 仕様

対象リポジトリ: dolquis/azooKey-Desktop
対応マイルストーン: M48（追加機能トラック）、M67（§13）
関連: `plans/windows-port-roadmap.md` M30 / M46、
      `docs/privacy-and-secure-input-spec.md`（M46）、
      `docs/learning-data-management-spec.md` §15（Persona）

## 0. 節とマイルストーンの対応

本書の節と、その範囲を定める `plans/windows-port-roadmap.md` のマイルストーンの対応を次に示す。

| 節 | マイルストーン |
|---|---|
| §1〜§2 | M48 |
| §3 | M46（ForegroundAppDetector の導入）。M48 が共用する |
| §4〜§5 | M48 |
| §6 | M48（`promptPrefixByApp` の移行。同キーは M30 の横断テーマ X-2-6） |
| §7 | M48（候補タグの定義は M52） |
| §8 | M30（設定アプリ） |
| §9 | M48 |
| §10 | M48（e2e は M50 の connect） |
| §11〜§12 | M48（§12 は M48 範囲外の将来拡張） |
| §13 | M67（入力欄の種類による切替。プロファイルの `inputScopeSwitching` フィールドを含む） |

## 1. 目的

前面アプリに応じて、予測 / 学習 / 文体 / AI backend / 候補タグ重みを
切り替える。VS Code では技術語を優先し、Outlook では敬語を優先し、
1Password ではセーフ入力に倒す、といったコンテキスト適応を実現する。

既存 `settings.promptPrefixByApp` の発展統合として位置づけ、`promptPrefix`
だけでなく学習・予測・タグ・backend をアプリ単位で切替可能にする。

## 2. 設計原則

- **既存 `promptPrefixByApp` と後方互換**: 移行期間中は両方読み、
  `profilesByApp` 優先
- **解決順は明示**: process_name → window_class → default → global
- **secure 優先**: M46 の secure mode が profile より優先される
- **キャッシュ**: 入力先のプロセス名は無効化までキャッシュし、ウィンドウクラスは取得ごとに更新

## 3. ForegroundAppDetector

M46 で導入した `tsf-tip/src/ForegroundAppDetector.cpp` を共用する。
**`ForegroundApp` 構造体・キャッシュ戦略・スレッド親和性・解決機構・
fail-closed の正準定義は `docs/privacy-and-secure-input-spec.md` §4.2 / §4.3**
とし、本 spec では再定義しない。M48 は同一インスタンスから `ForegroundApp`
（UTF-8 の `process_name` / `window_class` と `resolved`）を `Get()` で取得する。
プロセス名は無効化までキャッシュし、クラス名は取得ごとに更新する。
タイトル・タイトル hash は取得しない。詳細は §4.3 に従う。

`ForegroundApp.resolved == false`（入力先のプロセス名が解決不能）の場合、M48 は
プロファイル未適用＝`default` / グローバルで扱う（boost なし）。プライバシー軸の
fail-closed（`autoSecureInput` 有効時に解決不能を secure 扱い）は M46 §4.3 が担当し、プロファイル軸はそれに
従属する。

### 3.1 Host への伝達

IPC リクエスト `QueryCandidates` / `QueryPredictions` に任意フィールド `app`
（`ipc::AppIdentity`）を追記する:

```json
{
  "app": {
    "process_name": "code.exe",
    "window_class": "Chrome_WidgetWin_1"
  }
}
```

これは Host 向けの伝達契約である。検出器は IPC を送らず、タイトルやその hash も供給しない。

- `app` を送らない旧クライアント、`app` が object でない、または `process_name` と
  `window_class` がともに空の要求は「アプリ不明」として扱い、要求自体は拒否しない。
- `process_name` が空の要求は `ForegroundApp.resolved == false` と同じく、
  グローバル設定で処理する（§3）。
- Host は `app` をログ・警告に出さない（§9.1）。
- Host は Handshake 応答の `capabilities` で次の 2 つを広告する。
  - `app_profile`: `QueryCandidates.app` からプロファイルを解決し、§7 のタグ boost を
    掛ける。
  - `candidate_tag`: 応答の `CandidateField.tag` を埋める。
- TIP はこの 2 つを見て、`app` の送出とタグの表示を有効にする。広告しない旧 Host に
  `app` を送っても無視されるだけで、要求は失敗しない。

## 4. 設定スキーマ

`settings/mvp-settings.schema.json` の top-level key `profilesByApp` に保存する。
各プロファイルは `additionalProperties: false` とし、Host の `SettingsStore` と
設定アプリの `SettingsDocument` は共通 validator を使って読み取り・保存する。
未指定フィールドに schema の default を保存時点で補わず、解決時に下位層を継承する。

設定例（実際に書き込まれる JSON 値）:

```json
{
  "profilesByApp": {
    "default": {
      "profileName": "Default",
      "predictionEnabled": true,
      "sentenceCompletion": false,
      "learningEnabled": true,
      "aiBackend": "auto",
      "promptPrefix": "",
      "candidateTagBoosts": {}
    },
    "code.exe": {
      "profileName": "Code",
      "predictionEnabled": true,
      "sentenceCompletion": false,
      "learningEnabled": true,
      "aiBackend": "local-zenzai",
      "preferTechnicalTerms": true,
      "candidateTagBoosts": {
        "Technical": 1.5,
        "English": 1.3
      }
    },
    "outlook.exe": {
      "profileName": "Mail",
      "predictionEnabled": true,
      "sentenceCompletion": true,
      "style": "polite",
      "candidateTagBoosts": {
        "Polite": 1.4
      }
    },
    "1password.exe": {
      "profileName": "Secure",
      "privacyMode": "secure"
    }
  }
}
```

schema fragment（`properties.profilesByApp` への追加）。プロファイル名は
プロセス名 / ウィンドウクラス / `default` のいずれかで、各プロファイル
オブジェクトは `additionalProperties: false`:

```json
{
  "profilesByApp": {
    "type": "object",
    "additionalProperties": {
      "type": "object",
      "additionalProperties": false,
      "properties": {
        "profileName": { "type": "string", "default": "" },
        "predictionEnabled": { "type": "boolean", "default": true },
        "sentenceCompletion": { "type": "boolean", "default": false },
        "learningEnabled": { "type": "boolean", "default": true },
        "aiBackend": {
          "type": "string",
          "enum": ["auto", "local-zenzai", "openai", "none"],
          "default": "auto"
        },
        "promptPrefix": { "type": "string", "default": "" },
        "style": {
          "type": "string",
          "enum": ["auto", "polite", "casual", "technical"],
          "default": "auto"
        },
        "preferTechnicalTerms": { "type": "boolean", "default": false },
        "candidateTagBoosts": {
          "type": "object",
          "additionalProperties": { "type": "number", "minimum": 1.0, "maximum": 3.0 },
          "default": {}
        },
        "privacyMode": {
          "type": "string",
          "enum": ["inherit", "normal", "private", "secure"],
          "default": "inherit"
        },
        "bracketPairing": {
          "type": "string",
          "enum": ["auto", "on", "off"],
          "default": "auto"
        },
        "inputScopeSwitching": { "type": "boolean", "default": true }
      }
    },
    "default": {}
  }
}
```

### 4.1 プロファイルフィールド

| キー | 型 | 既定 | 意味 |
|---|---|---|---|
| `profileName` | string | "" | UI 表示名 |
| `predictionEnabled` | bool | true | 予測候補ウィンドウを出すか |
| `sentenceCompletion` | bool | false | 文末補完を出すか |
| `learningEnabled` | bool | true | このアプリで学習するか |
| `aiBackend` | enum | "auto" | `auto` / `local-zenzai` / `openai` / `none` |
| `promptPrefix` | string | "" | Magic Conversion のプロンプト前置。空でなければ、TIP が窓を開いた入力先のアプリで解決し、送る指示の先頭に付けて改行で区切る。窓には見せない（`docs/legacy-parity-spec.md` §4.3） |
| `style` | enum | "auto" | `auto` / `polite` / `casual` / `technical`。`auto` 以外は対応する候補タグ（`Polite` / `Casual` / `Technical`）へ暗黙倍率 1.5 を与える（§7） |
| `preferTechnicalTerms` | bool | false | true で `Technical` タグへ暗黙倍率 1.5 を与える（§7） |
| `candidateTagBoosts` | map | {} | 候補タグ名 → 倍率（M52 ベンチで定義する候補タグ `Technical` / `Polite` / `English` 等。M53 の辞書エントリ category（`person_name` 等）に作用する `dictionary.categoryBoosts` とは **別 namespace**。詳細は `docs/auto-word-registration-spec.md` §14.5 を参照） |
| `privacyMode` | enum | "inherit" | `inherit` / `normal` / `private` / `secure` |
| `bracketPairing` | enum | "auto" | `auto` / `on` / `off`。`auto` はグローバルのアプリリスト判定に従い、`on` / `off` はそれを上書きする。root の boolean マスターが false の場合と前面アプリ解決失敗時は常に無効 |
| `inputScopeSwitching` | bool | true | 入力欄の種類（InputScope）による入口のモードと抑制を、このアプリで行うか（§13）。未指定はグローバルの `inputScopeSwitching` を継承する |

`bracketPairing` の未指定は下位プロファイルの値を継承する。上位で明示した `auto` は
下位の `on` / `off` を解除し、`bracketPairingApps` / `bracketPairingAppPolicy` の判定に戻す。
TIP は共通 resolver の不変スナップショットからこのフィールドを解決し、Host への接続なしで適用する。

`predictionEnabled` も TIP が同じスナップショットから入力先（§3）について解決し、予測を照会するか、届いた予測を
表示するかを決める。どの層も持たなければグローバル `predictionEnabled` を継承する。適用の規則は
`docs/legacy-parity-spec.md` §3.6、入力インジケーターのメニューでの表示は `docs/tsf-deep-integration-spec.md` §4.5 に従う。

### 4.2 フィールド制約と backend 優先順位

**`candidateTagBoosts` の値域**: 各倍率は `[1.0, 3.0]`（schema で `minimum: 1.0` /
`maximum: 3.0`）。schema の min/max は宣言であり、設定ローダが手動パースである以上、
手編集・移行で混入した範囲外値（例 `100`）には効かない。よって**ランタイム側でも必ず
`[1.0, 3.0]` にクランプ**する（§7 の `min(3.0, max(1.0, value))`）。下げ方向には使わない
（1.0 未満は 1.0 の no-op）。3.0 は暴走防止の上限（3× 倍率で実質最上位を占有するため
十分）。未知のタグ名は無視する（forward-compat。タグ namespace は M52 ベンチの
`Technical` / `English` / `Polite` / `Casual` / `NamedEntity` 等）。

**`aiBackend` の `auto` センチネルと root enum の整合**: プロファイルの `aiBackend`
enum `["auto", "local-zenzai", "openai", "none"]` の **`auto` はプロファイル専用
センチネル**で「グローバル `settings.aiBackend` を継承」を意味する。root の
`settings.aiBackend` は具体値のみ（`["none", "openai", "local-zenzai"]`、`auto` を
持たない）。実装は `auto` を root へ書き戻さず、解決時に `settings.aiBackend` へ展開する。

**プライバシーが profile backend に優先（backend 制約）**: backend 解決はモード名の
列挙ではなく M46 プライバシー判定の per-axis クエリ（§5.1。**§5 のグローバル floor 適用後**の
実効状態で評価し、per-app `normal` がグローバル `offline` / `secure` / `custom` を緩和した
後の値ではない）に従う。これにより `custom`（例: 外部 AI のみ無効化）を含め全モードを
統一的に扱う。優先順位を以下に確定する:

1. まず `auto` を `settings.aiBackend` へ展開する（`auto` のまま下の判定に渡さない。
   継承された `openai` を取りこぼさないため）。
2. `AI 候補生成許可 == false`（`secure`、または `custom` で AI 候補生成を
   無効化）→ `aiBackend = none`（外部・ローカルとも AI を使わない。M46 §5 の抑止契約に従う）。
3. `外部 AI 許可 == false`（`private` / `offline` / `custom` で外部 AI
   無効）→ 外部 `openai` を禁止。展開後の値が `openai`（明示・`auto` 継承のいずれも）なら
   モデル搭載時 `local-zenzai` へ降格、未搭載なら `none`。`local-zenzai` は許可。
4. 上記いずれにも該当しない（外部 AI 許可）→ 展開後の `profile.aiBackend` を適用。

**`privacyMode` enum の範囲**: profile の `privacyMode` は
`["inherit", "normal", "private", "secure"]` とし、`offline` / `custom` を **per-app
では持たない**。`offline`（ネットワーク禁止）は端末全体のグローバル方針、`custom` は
上級者向けグローバル詳細指定であり、アプリ単位では意味が薄く誤設定リスクも高いため
M48 では除外する（グローバル `privacy.mode` で扱う）。

## 5. 解決順

`AppProfileResolver::Resolve(app)` は以下の優先順で値を **field 単位で
overlay マージ** する。下位層で見つかった field は上位層の値で上書きされ、
**未指定の field はそのまま下位層を引き継ぐ**:

1. `profilesByApp[process_name.lower()]`（最優先）
2. `profilesByApp[window_class]`
3. `profilesByApp["default"]`
4. グローバル設定（`settings.predictionEnabled` 等）— **base**

例: legacy `promptPrefixByApp` から移行した `profilesByApp[process]` が
`promptPrefix` のみを持つ場合、`predictionEnabled` / `learningEnabled` 等は
グローバル設定（base）の値を継承する。partial profile が無関係な機能を
意図せず再有効化することはない。

`privacyMode` のみ特殊扱い: `inherit` の場合だけ下位層を継承し、明示値
（`normal` / `private` / `secure`）は下位を上書きする（プライバシー設定を意図せず
緩める方向に継承しない方針）。ただしこの上書きは **profile レイヤ内**（process →
window_class → default）に限り、下記の **グローバル floor** を下回ることはできない。

**グローバル `privacy.mode` は floor（最小保証）**: profile レイヤ解決の結果は、
グローバル `privacy.mode` が与える保証を **下回ってはならない**。実効モードは
「グローバル `privacy.mode` の制約」と「profile 解決後モードの制約」を **各軸で厳しい方**
（union of restrictions）に取る。特に:

- グローバル `offline`（ネットワーク禁止 / 外部 AI 不可）は per-app `privacyMode = normal`
  や `profile.aiBackend` では **解除されない**。`offline` は端末全体のネットワーク方針で
  あり、アプリ単位で緩められない。
- グローバル `secure` も terminal で per-app では緩和できない。

したがって per-app `normal` は「**グローバル floor まで** 戻す」意味であり、上位 profile
レイヤが付けた `private` / `secure` を明示解除する用途に限る。グローバルが `offline` /
`private` / `secure` の場合、`normal` はその floor までしか戻らず、リテラルな `normal`
（保護なし）には落とさない。

`privacyMode` はプロファイルからの要求値であり、許可そのものではない。
消費側は解決結果とグローバル設定から各軸の制約を評価し、グローバル floor を維持する。
`inherit` はグローバル方針を継承する。この契約はモード遷移通知や理由文字列の
送信 API を定義しない。TIP と Host の責務は
`docs/privacy-and-secure-input-spec.md` §5.1.1 に従う。

## 6. 既存 `promptPrefixByApp` との統合

本節は `promptPrefixByApp` の解決と移行の正典である。前面アプリは `ForegroundAppDetector` が返す `process_name` で引く。

既存設定キー `promptPrefixByApp` は値 `{"<process>": "<prefix>"}` の
map（M30 横断テーマ X-2-6）で、key は大文字小文字混在の実プロセス名
（例: `Code.exe`, `OUTLOOK.EXE`）で書かれている既存ユーザー設定がある。
M48 の resolver は、プロセス名と legacy 側のキーを大文字小文字を区別せず比較する。
Windows では Unicode ordinal 比較を使い、設定ファイルの元のキー表記は保持する。
`window_class` のキーは大小文字を区別する完全一致で照合する。プロセス名とは照合規約が異なる。
以下の `process.lower()` はこの照合規約を表す。M48 では以下の移行戦略をとる:

1. `profilesByApp[process.lower()].promptPrefix` が**存在すれば**（明示的な空文字 `""`
   を含む）それを使う。`""` は「レガシー prefix をクリアする」上書き値であり、レガシーへ
   フォールスルーしない。
2. `promptPrefix` フィールドが**存在しない**（プロファイル自体が無い、または profile に
   `promptPrefix` キーが無い）場合のみ `promptPrefixByApp[process.lower()]` を読む（§5
   overlay と同じく「未指定 field のみ下位層を継承」。空 `""` は未指定ではない）。このため
   設定ローダは `promptPrefix` の**キー有無を保持**し、既定 `""` を overlay / legacy 解決前に
   先食いで適用しない（大文字混在キーも同じ照合規約で扱う）。
   `profilesByApp` にも `promptPrefixByApp` にも該当が無ければ、prefix を付けない（generic prompt）
3. 設定アプリでの編集は `profilesByApp` 側に書く（`promptPrefixByApp`
   は read-only legacy 扱い）
4. `promptPrefixByApp` は M48 を含むリリースから 3 マイナーバージョンの間は読み込み、
   その後のリリースで削除する（deprecation warning を CHANGELOG に記載）

共通 resolver の読み込み・解決処理に統合する。照合で衝突した既存キー（例: `Code.exe` と `code.exe` の
両方）は、**JSON のキー列挙順に依存しない決定的規則**として、衝突元キーを Unicode
コードポイント順で昇順ソートし**末尾（最大）を採用**する。
共通 validator は警告を返し、Host の `SettingsLoadResult.profile_warnings` と
設定アプリの保存結果で参照できる。`azookey_diag.exe` への表示接続は M48 の診断統合で行う。

## 7. 候補タグ Boost

`candidateTagBoosts` は M52 ベンチで定義する候補タグ（`Technical` /
`English` / `Polite` / `Casual` / `NamedEntity` 等）への倍率。各候補の
score（rerank 後の `core::Candidate::score`。以下の式では `final_score` と書く。Track A の導入後は
`docs/neural-reranker-spec.md` §9 の `final_score`）に以下を掛ける（`inference-host/src/CandidateTags.cpp`
の `ApplyTagBoosts`）:

```
raw   = profile.candidateTagBoosts.get(tag, 1.0)
boost = min(3.0, max(1.0, raw))   // [1.0, 3.0] にクランプ（手編集・移行値の暴走防止）
if candidate.final_score >= 0:
    candidate.final_score *= boost
else:
    candidate.final_score /= boost  // 負のスコアも上げる方向に動かす
```

`max(1.0, …)` で下げ方向には使わず、`min(3.0, …)` で上限もクランプする（§4.2 の
`[1.0, 3.0]` を実際に強制するのはこのランタイム式）。boost のみ許可し、逆方向の調整は
タグ別の score weight 設定で行う（M11 範疇）。

- **暗黙倍率**: `style` と `preferTechnicalTerms` は対応タグへ倍率 1.5 を与える。
  同じタグに `candidateTagBoosts` の明示値があれば、明示値と 1.5 の大きい方を使う。
- **負のスコア**: スコアは対数確率などで負になり得る。負の `final_score` は倍率で
  割り（`final_score /= boost`）、常に上げる方向に作用させる。
- **並べ替え**: boost を受けた候補だけを、新しいスコアより低い候補の前へ移す。
  boost を受けない候補どうしの相対順は変えない。
- **適用位置**: rerank の後、M35 の補正読み提案を先頭へ挿入する前に 1 回適用する。
  `live = true` の `QueryCandidates` も同じ経路を通る（`QueryLiveConversion` は
  `app` を持たないため boost しない）。
- **予測**: `QueryPredictions` は `app` を受け取るが、boost は適用しない。予測は
  学習・モデル・辞書を出所ごとに混ぜており、出所間でスコアを比較できないため。
- **タグの付与元**:
  - 出所が既知のタグ（辞書 category 由来の `Technical` など）は出所側が付与する。
    同一 surface の候補統合でも辞書由来タグを保持する規則は
    `docs/auto-word-registration-spec.md` の category → タグ写像に従う。
  - 既存タグは文体判定で上書きしない。未付与の候補には Host が surface の文末を
    `Polite` → `Casual` → `English` の順に判定し、最初に一致したタグを付与する。
    文体判定では末尾に連続する Unicode White_Space と `。！!？?` だけを除いてから、
    次の語尾へ一致するか調べる。元の surface、reading、score は変更しない。
    - `Polite`: `です`、`ます`、`でした`、`ました`、`ません`、`ませんでした`、
      `ございます`、`ございました`、`ください`。
    - `Casual`: `だよ`、`だね`、`だぞ`、`だぜ`、`だろ`、`じゃん`。
    裸の `だ`、文中の一致、語尾の後に続く `、,.…」` などは対象外とする。
    文体判定は形態素解析を伴わない保守的な語尾一致であり、`かます` などの語彙も
    `Polite` に一致し得る。不正 UTF-8 は文体タグの対象外とする。
  - 文体に一致しなかった未付与の候補には `English` を付与する。条件は、空白
    （ASCII と U+3000）を除くコードポイントの過半が ASCII で、ASCII 英字を 1 字以上含むこと
    （`docs/auto-word-registration-spec.md` の category → タグ写像）。
  - タグは `core::Candidate::tag` と IPC の `CandidateField.tag` で運ぶ。
- **タグ名の照合**: ASCII の大文字小文字を区別しない（`Technical` と `technical`
  を同一視する）。

本節はタグ boost の clamp 式と適用位置の正典であり、`docs/auto-word-registration-spec.md`
§14.5・§14.11・§14.12 から参照される。

## 8. UI（設定アプリ）

設定アプリの「アプリ別」ペイン（`settings-app-spec.md` §3.2）は、次の画面でプロファイルを扱う。
前面アプリの検出表示と「全削除」は持たない。プロファイルの削除は 1 件ずつ行う:

```
[アプリ別設定]

検出中の前面アプリ: code.exe (Chrome_WidgetWin_1)
[このアプリのプロファイルを追加]

登録済みプロファイル:
  [default]            予測ON  学習ON  AI:auto       [編集]
  [code.exe]           予測ON  学習ON  AI:zenzai     技術語+50% [編集]
  [outlook.exe]        予測ON  学習ON  敬語タグ+40%  [編集]
  [1password.exe]      🔒 secure                       [編集]

[新規追加] [全削除]
```

編集ダイアログ:
- profileName / predictionEnabled / sentenceCompletion /
  learningEnabled / aiBackend / promptPrefix / style /
  preferTechnicalTerms / candidateTagBoosts / privacyMode / bracketPairing /
  inputScopeSwitching（§13）

各フィールドは「指定しない（継承）」を選べ、指定しないフィールドは `settings.json` に書かない（§4）。
`promptPrefix` は「指定する」を切り替え、指定して空にすると §6 の「明示的な空文字」になる。
`candidateTagBoosts` は、タグ名と倍率（1.0〜3.0）の行を足し引きして編集する。範囲外の倍率、
空または重複したタグ名、既にあるアプリ名（大文字小文字を区別しない）は、ダイアログを閉じられない。
保存は、読み込み時と異なるときだけ `profilesByApp` 全体を書く。
`promptPrefixByApp`（従来）は、プロファイル一覧の下に読み取り専用で並べ、保存しても消さない。
各行の「プロファイルへ移す」は、同じアプリ（大文字小文字を区別しない）のプロファイルの `promptPrefix` へ値をコピーする。
プロファイルが無ければ `promptPrefix` だけを持つプロファイルを、従来のキー表記のまま作る。
既に `promptPrefix` を持つプロファイルは上書きしない（§6-1 でそちらが優先される）。従来のキーは消さない。

## 9. AppProfileResolver

共通の選択処理は `core/src/AppProfileResolver.cpp` に置く。Host は
`RuntimeSettings::AppProfiles()` で不変の resolver を参照し、TIP も同じ resolver を使う。
単一フィールドの解決も、全フィールドと同じ overlay 処理を通す。

```cpp
class AppProfileResolver {
public:
  static AppProfileResolver FromSettings(const json::Value& settings,
                                        std::vector<std::string>* warnings = nullptr);
  json::Object Resolve(const ForegroundApp& app, AppNameEqual equal = EqualAppName) const;
  std::optional<json::Value> ResolveField(std::string_view field, const ForegroundApp& app,
                                        AppNameEqual equal = EqualAppName) const;
};
```

Host は resolver を `EngineConfig::app_profiles` として設定と一緒に公開する。
`QueryCandidates` のハンドラは、要求ごとに `engine_->config()` のスナップショットから
resolver を取り出して `Resolve` を呼ぶ。UpdateConfig のロックは取らない。
タグ boost 以外のフィールド（学習・external AI・予測の切替）は、プライバシー判定の
許可範囲内で消費側が個別に統合する。

### 9.1 共通基盤と機能への適用境界

resolver は設定フィールドを選ぶ純粋な処理であり、TSF 操作、IPC、ファイル I/O、
許可判定を行わない。`privacyMode` はプロファイルからの要求値であり、
グローバル floor 適用後の許可判定ではない。消費側は §4.2 / §5 の プライバシー判定制約を
満たしてから候補・学習・AI に適用する。共通基盤だけでは、これらの実動作は切り替わらない。
カッコ設定の適用境界は `bracket-pairing-spec.md` §4.5.0 とする。
カッコ設定を適用する in-process TIP は入力先の自プロセス名と自スレッドのウィンドウクラスを
使う。M46 / M48 も §3 の同じ入力先識別を共用する。

不正なプロファイル・未知フィールド・型や enum の不正は除外して下位層を継承し、
タグ倍率は読み込み時にも `[1.0, 3.0]` に制限する。未知のタグ名は保存時に保持し、
スコア適用時に既知タグだけを参照する。検証警告にアプリ名や設定値を含めない。
設定アプリでモデル設定を保存しても、プロファイルの有効フィールドと明示的な空文字を保持する。

## 10. テスト

- unit: 解決順（process_name → window_class → default → global）の網羅
- unit: `promptPrefixByApp` legacy 読み込み + `profilesByApp` 優先
- unit: `privacyMode = secure` の要求値とグローバル floor の解決
- integration: `code.exe` 検出 → 技術語タグ boost
- integration: `outlook.exe` 検出 → §7 の文末判定で付与した `Polite` タグを boost。
  別アプリと app 未指定では boost せず、IPC に同じタグと元の候補文字列を運ぶ。
- e2e（M50 connect）: アプリ切替 1 秒以内にプロファイル反映

## 11. M48 受け入れ条件

受け入れ条件の定義の正典は [`plans/windows-port-roadmap.md`](../plans/windows-port-roadmap.md)
の M48 節とする。本書はプロファイル解決・スキーマ・スコア適用・後方互換の写像を定義し、
受け入れ条件を複製しない。

## 12. 将来拡張（M48 範囲外）

- ウィンドウタイトル単位の細分化（プライバシー上、本実装では非対応）
- アプリ自動検出のクラウド辞書（プライバシー上、本実装では非対応）

## 13. 入力欄の種類（InputScope）による切替

本節は M67 を定める。§1〜§12 がアプリ単位の切替を扱うのに対し、本節は入力欄単位の切替を扱う。

### 13.1 目的と根拠

アプリは入力欄の種類を、TSF の `GUID_PROP_INPUTSCOPE` で IME に伝える。TIP はこれを読み、URL・メール・数値の欄では
英数モードで入力を始め、カッコ補完と自動句読点を止める。検索欄ではひらがなのまま、自動句読点だけを止める。
利用者が欄ごとに英数モードへ手で切り替える手間を省くためである。

既定で有効にする（§13.6）根拠は次の 2 つである。

- Microsoft Learn の互換性の記事（「Calling ImmSetConversionStatus() or ImmGetConversionStatus() from Windows Store apps
  is not supported」）は、アプリが欄の入力 scope を指定して既定の IME モードを制御し、scope ごとのモードは各 IME が決める、
  と述べる。
- Chromium は、アドレス欄の入力の種類を URL にすると IME が英字モードを既定にして検索語を打ちにくくなるため、Windows で
  入力ロケールが CJK のときはアドレス欄を検索の種類として報告する（`chrome/browser/ui/views/omnibox/omnibox_view_views.cc` の
  `GetPreferredTextInputType`）。Web ページの欄は、`ui/base/ime/win/tsf_input_scope.cc` で `type=url` が `IS_URL`、
  `type=email` が `IS_EMAIL_SMTPEMAILADDRESS`、`type=number` が `IS_NUMBER`、`type=tel` が
  `IS_TELEPHONE_FULLTELEPHONENUMBER`、`type=search` が `IS_SEARCH` になる。

Microsoft IME の値ごとの挙動は、本節の根拠に含めない。実機での比較は Human Gate で行う（§13.8）。

パスワード・PIN・`IS_PRIVATE` の扱いは M46（`docs/privacy-and-secure-input-spec.md` §4.3）が定め、本節は変えない（§13.3）。

### 13.2 対象の値と挙動

| 群 | InputScope の値 | 入口のモード | カッコ補完 | 自動句読点 |
|---|---|---|---|---|
| URL | `IS_URL` | 英数 | 止める | 止める |
| メール | `IS_EMAIL_USERNAME`、`IS_EMAIL_SMTPEMAILADDRESS`、`IS_EMAILNAME_OR_ADDRESS` | 英数 | 止める | 止める |
| 数値・電話 | `IS_DIGITS`、`IS_NUMBER`、`IS_TELEPHONE_FULLTELEPHONENUMBER`、`IS_TELEPHONE_COUNTRYCODE`、`IS_TELEPHONE_AREACODE`、`IS_TELEPHONE_LOCALNUMBER` | 英数 | 止める | 止める |
| 半角英数 | `IS_ALPHANUMERIC_HALFWIDTH` | 英数 | 止める | 止める |
| 検索 | `IS_SEARCH`、`IS_SEARCH_INCREMENTAL` | 変えない | 変えない | 止める |

表の上の 4 群をまとめて「英数で始める群」と呼ぶ。

- **入口のモード**: 「英数」は、欄に入ったときに TIP の英数モード（`docs/tsf-deep-integration-spec.md` §4.2・§4.5。On のまま
  キーを素通しする状態）にすることを指す。IME の開閉（`GUID_COMPARTMENT_KEYBOARD_OPENCLOSE`）は書かない。IME が Off の
  ときは何もしない。
- **カッコ補完**: 「止める」は、その欄にいる間、入力モードによらずカッコ補完（`docs/bracket-pairing-spec.md`）を働かせない
  ことを指す。カッコのキーは、カッコ補完が無効のときと同じ扱いになる。
- **自動句読点**: 「止める」は、その欄にいる間、TIP が動的句読点（M59）について `dynamicPunctuation == false` のときと同じに
  振る舞うことを指す。要求の `auto_punctuation` を常に `false` で送り、idle のタイマーを張らず、Enter で最終要求を待たない
  （`docs/dynamic-punctuation-spec.md` §4.3.1・§7.1.1・§7.3）。一括変換の `batchAutoPunctuation`（M58-C）は対象に含めない。

次は変えない。

- 記号の全角化（`docs/tsf-deep-integration-spec.md` §3.5）。英数モードでは記号はもともと半角でアプリへ渡る。利用者がその欄で
  ひらがなへ切り替えた後は、通常どおり全角にする。
- 学習。英数モードは打鍵を素通しするので、観測が生じない。`IS_PRIVATE` による学習の停止は M46 が担う。
- 予測候補とライブ変換。

表に無い値は対象外とし、従来どおり扱う。`IS_DEFAULT`、ファイル名、ログオン名、人名、住所、日付、時刻、
`IS_FORMULA`、`IS_FORMULA_NUMBER`、`IS_YOMI` などがこれに当たる。ファイル名と人名には日本語が入りうる。
`IS_HIRAGANA`、`IS_KATAKANA_HALFWIDTH`、`IS_KATAKANA_FULLWIDTH`、`IS_ALPHANUMERIC_FULLWIDTH`、`IS_NUMBER_FULLWIDTH` も
対象外である。azooKey にはこれらに対応する入力モードが無い。

### 13.3 欄の種類の判定と M46 との関係

欄の種類は、M46 のパスワード欄の判定と同じ `GUID_PROP_INPUTSCOPE` の読み取りの処理（`docs/privacy-and-secure-input-spec.md`
§4.3、`tsf-tip/src/AiInputGuard.cpp` の `ProbeInputScope`）が返す scope の値の列から求める。読み取るのは §13.4 の時機であり、
M46 の判定は従来どおり入力イベントごとに行う。本節は M46 の判定の時機も結果も変えない。

M46 の判定を先に確定させ、欄の種類はその結果を変えない。secure、学習の許可、AI 送信の許可は、欄の種類によらず M46 の規則で
決まる。本節が切り替えるかを決めるときの secure には、同じ時点の M46 の判定を使う。

- secure と判定した入力欄（パスワード・PIN の scope、パスワード形式のフォーカス窓、キーボード入力を無効にした context、
  secure のアプリやモード）では、本節の切替を行わない。M46 の扱いだけが働く。
- `IS_PRIVATE` と表の値が同時にあるときは、表の値で切り替え、`IS_PRIVATE` による学習と AI 送信の停止もそのまま働く。
  Chromium は InPrivate のウィンドウでは欄の種類によらず `IS_PRIVATE` だけを返すので、そこでは切り替えない。

アプリが複数の値を返したときは、返した順に見て、§13.2 の表に最初に当たった値で欄の種類を決める。

次のときは欄の種類なしとし、本節の切替を行わない（§13.7）。

- `GUID_PROP_INPUTSCOPE` を提供しない。
- 値が 0 件である。
- 表に当たる値が無い。

読み取りに失敗したとき（edit session の拒否などで値の列を得られないとき）の扱いは §13.4 が定める。

### 13.4 切替の時機と戻し

欄の種類は次の 2 つの時機に判定する。

- document manager へフォーカスが移ったとき（`ITfThreadMgrEventSink::OnSetFocus`）。このとき前の欄を離れたものとして扱う。
  `docs/tsf-deep-integration-spec.md` §4.1 の初回フォーカスで TIP が IME を On にする処理の後に判定する。
- composition が無い状態でキーを処理する前。同じ document manager のまま scope だけが変わるアプリでも、欄の移動を拾うためである。
  同じキーの `OnTestKeyDown` と `OnKeyDown` は同じ判定結果を使う。

判定した欄の種類が前回と同じなら何もしない。変わったときは、前の欄を離れ、新しい欄に入ったものとして扱う。
読み取りに失敗したときは、前回の判定を保ち、次の時機に読み直す。フォーカスの移動の時点で失敗したときは、前回の判定が無いので
欄の種類なしとして扱う。

英数で始める群の欄に入ったとき、IME が On で英数モードでなければ、英数モードにする。このとき、入る前のモードを覚えておく。
すでに英数モードなら何もしない。

英数で始める群の欄にいる間に、利用者の操作によらずに IME が On になったときも、英数モードにする。TIP の外からの On と、
`docs/tsf-deep-integration-spec.md` §4.1 で TIP が行う On（初回フォーカスと 1 秒以内の戻し）がこれに当たる。開閉の状態が
変わると英数モードは解けるため、適用し直さなければひらがなで始まる。利用者が開閉キーや入力インジケーターで On にしたときは、
下の利用者の切替として扱う。

次のときは切り替えない。

- composition がある（通常はフォーカスの移動で確定済みである。`docs/tsf-deep-integration-spec.md` §4.3）。
- 持ち越した入力がある（同 §4.3）。英数モードの切替先は持ち越した入力を捨てるため、切り替えると打鍵が失われる。
- IME が Off である。

利用者がその欄で入力モードを切り替えたとき（英数キー、ひらがなキー、`VK_IME_ON`、入力インジケーター、Off から On への
切替（`docs/tsf-deep-integration-spec.md` §4.2））は、その欄にいる間は利用者の選択を保ち、本節は切り替え直さない。
利用者の操作によらない On での英数モードの適用し直しも、その後は行わない。

本節が英数モードにした欄から離れたときは、入る前のモードへ戻す。その欄で利用者が切り替えていても戻す。
欄での切替はその欄の中だけで効く。英数で始める群の別の欄へ直接移ったときは英数モードのまま、覚えたモードを引き継ぐ。
戻すときに IME が Off なら何もしない（On にしたときの規則は `docs/tsf-deep-integration-spec.md` §4.2 に従う）。
欄ごとの選択は保存しない。同じ欄へ戻ると、もう一度英数モードから始まる。

モードの切替は、入力インジケーターの表示（`docs/tsf-deep-integration-spec.md` §4.5）を更新する。判定した欄の種類は、M46 の判定と同じ info ログ
`input_gate`（`docs/privacy-and-secure-input-spec.md` §4.3）に、値が変わったときだけ記録する。入力本文は含めない。

### 13.5 優先順位

上ほど優先する。

1. M46（`docs/privacy-and-secure-input-spec.md` §4.3）。secure の欄では本節の切替を行わず、本節は M46 の判定を変えない（§13.3）。
2. その欄での利用者の切替（§13.4）。
3. アプリ別プロファイルの `inputScopeSwitching`（§5 の解決順で継承する）。
4. グローバルの `inputScopeSwitching`。

`inputScopeSwitching` が実効で `false` のアプリでは、本節の入口のモードと抑制をすべて行わない。

カッコ補完の抑制は、プロファイルの `bracketPairing: "on"`（§4.1、`docs/bracket-pairing-spec.md` §4.5.0）より優先する。欄の
種類のほうがアプリより具体的だからである。マスターの `bracketPairing == false` はこれまでどおり全体を無効にする。
URL などの欄でもカッコ補完を使いたいアプリでは、そのアプリの `inputScopeSwitching` を `false` にする。

自動句読点の抑制も、グローバルの `dynamicPunctuation` より優先する。

### 13.6 設定キー

`settings/mvp-settings.schema.json` のトップレベルへ次を追加する。

| キー | 型 | 既定 | 説明 |
|---|---|---|---|
| `inputScopeSwitching` | boolean | `true` | M67: 入力欄の種類（§13.2）に応じて、入口のモードを英数にし、カッコ補完と自動句読点を止める |

`profilesByApp` の各プロファイルにも、同名の boolean フィールド `inputScopeSwitching` を追加する（§4、§4.1）。
未指定は §5 の解決順で下位層を継承し、どの層も持たなければグローバルの値に従う。
旧版の resolver は未知のフィールドだけを除き、同じプロファイルの他のフィールドを保つ（§9.1）。

既定を `true` にする根拠は §13.1 にある。影響は次の範囲に限られる。

- `GUID_PROP_INPUTSCOPE` を返さないアプリと、表に無い値の欄は変わらない（§13.7）。
- Chromium 系ブラウザのアドレス欄は、日本語の入力ロケールでは検索欄として報告されるので、ひらがなのまま始まる。
- 英数モードになった欄でも、英数キーやひらがなキーで 1 打で戻せる。全体または特定のアプリで `false` にできる。

供給経路は `bracketPairing` と同じで、TIP が settings.json から読み、Host を経由しない（`docs/bracket-pairing-spec.md` §6.1）。
schema、`settings/default-settings.sample.json`、settings-app の許可キーと画面（グローバルの項目と §8 の編集ダイアログ）、
TIP の設定スナップショットを同じ変更でそろえる。

### 13.7 InputScope を返さないアプリ

§13.3 で欄の種類なしになった入力欄では、本節の切替を行わない。入口のモード、カッコ補完、自動句読点は、本節の導入前と
同じである。M46 の判定もそのまま働く。

### 13.8 テストと実機確認

単体テストと TIP のテストで、次を確かめる。

| 対象 | 検証内容 | 層 |
|---|---|---|
| 分類（§13.2、§13.3） | 表の各値が対応する群になること。複数の値で最初に当たった値が決めること。`IS_DEFAULT`、表に無い値、0 件、読み取りの失敗で種類なしになること。パスワード・PIN・`IS_PRIVATE` が群に入らないこと | tsf-tip |
| M46 との関係（§13.3） | secure の欄で切り替えないこと。`IS_URL` と `IS_PRIVATE` の組で英数モードになり、学習と AI 送信の停止が保たれること。M46 の既存の試験が変わらず通ること | tsf-tip |
| 時機（§13.4） | 欄に入ると英数モードになり、離れると入る前のモードへ戻ること。利用者の切替がその欄で保たれ、離れると戻ること。composition、持ち越した入力、IME Off で切り替えないこと。同じ document manager のまま scope が変わったとき、composition の無い次のキーの前に判定し直すこと。読み取りの失敗で前回の判定が保たれること | tsf-tip |
| 開閉との関係（§13.4） | 新しい document の初回フォーカスで TIP が On にした後と、1 秒以内の戻しの後に英数モードになること。TIP の外からの On の後も英数モードになること。利用者の開閉キーと入力インジケーターでの On ではひらがなになり、その欄では適用し直さないこと | tsf-tip |
| 抑制（§13.2、§13.5） | 英数で始める群の欄で、英数モードでもひらがなモードでもカッコ補完が働かないこと。プロファイルの `bracketPairing: "on"` でも働かないこと。検索欄と英数で始める群の欄で `auto_punctuation` が `false` になり、idle のタイマーを張らず、Enter が最終要求を待たないこと | tsf-tip |
| 設定（§13.5、§13.6） | グローバルとプロファイルの `inputScopeSwitching: false` で切替と抑制をすべて行わないこと。プロファイルの未指定が継承されること | core / tsf-tip |
| 返さないアプリ（§13.7） | 欄の種類なしで、入口のモード、カッコ補完、自動句読点が変わらないこと | tsf-tip |

実機では次を確かめる（`gate:human-required`）。

- Edge と Chrome の `type=url`・`email`・`number`・`tel`・`search` の欄と `inputmode` 属性で、欄の種類どおりに切り替わること。
- Edge と Chrome のアドレス欄が、日本語の入力ロケールでひらがなのまま始まること。
- InPrivate のウィンドウで切り替わらないこと。
- Firefox の同じ欄、Windows の検索欄、メモ帳（scope を返さない）、Excel の最初の打鍵（持ち越し）。
- 入力インジケーターが `_A` を出すこと。
- 同じ欄での Microsoft IME の入口のモードとの比較。
