# アプリ別入力プロファイル 仕様

対象リポジトリ: dolquis/azooKey-Desktop
対応マイルストーン: M48（追加機能トラック）
関連: `plans/windows-port-roadmap.md` M30 / M46、
      `docs/privacy-and-secure-input-spec.md`（M46）、
      `docs/rich-features-spec.md`（X-2-6 promptPrefixByApp UI、X-2-7
      Persona）
作成日: 2026-05-27
位置づけ: 追加機能トラック（M35 / M36 と並列、M46 完了後）

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
- **キャッシュ**: ForegroundApp の検出は 500ms TTL でキャッシュ

## 3. ForegroundAppDetector

M46 で導入した `tsf-tip/src/ForegroundAppDetector.cpp` を共用する。
**`ForegroundApp` 構造体・キャッシュ戦略・スレッド親和性・解決機構・
fail-closed の正準定義は `docs/privacy-and-secure-input-spec.md` §4.2 / §4.3**
とし、本 spec では再定義しない。M48 は同一インスタンスから `ForegroundApp`
（`process_name` は `lower()` 正規化済み / `window_class` / `window_title_hash`）
を取得する。`window_title` 生値は検出器プロセス内専用で IPC には出さない（§3.1）。
フォーカス変更時の invalidate（`EVENT_SYSTEM_FOREGROUND`）と 500ms TTL も §4.3 に従う。

`ForegroundApp.resolved == false`（前面 / プロセス名が解決不能）の場合、M48 は
プロファイル未適用＝`default` / グローバルで扱う（boost なし）。プライバシー軸の
fail-closed（解決不能を secure 扱い）は M46 §4.3 が担当し、プロファイル軸はそれに
従属する。

### 3.1 Host への伝達

各 IPC リクエスト（`QueryCandidates` / `QueryPredictions` /
`TransformSelectedText`）に `app` フィールドを追加する:

```json
{
  "app": {
    "process_name": "code.exe",
    "window_class": "Chrome_WidgetWin_1",
    "window_title_hash": "0xabcd1234"
  }
}
```

`window_title` 本体は機密の可能性があるため `hash` のみを送る（M46 §8 と
整合）。

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
        }
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
| `promptPrefix` | string | "" | Magic Conversion のプロンプト前置 |
| `style` | enum | "auto" | `auto` / `polite` / `casual` / `technical` |
| `preferTechnicalTerms` | bool | false | 技術語辞書を boost |
| `candidateTagBoosts` | map | {} | 候補タグ名 → 倍率（M52 ベンチで定義する候補タグ `Technical` / `Polite` / `English` 等。M53 の辞書エントリ category（`person_name` 等）に作用する `dictionary.categoryBoosts` とは **別 namespace**。詳細は `docs/auto-word-registration-spec.md` §14.5 を参照） |
| `privacyMode` | enum | "inherit" | `inherit` / `normal` / `private` / `secure` |
| `bracketPairing` | enum | "auto" | `auto` / `on` / `off`。`auto` はグローバルのアプリリスト判定に従い、`on` / `off` はそれを上書きする。root の boolean マスターが false の場合と前面アプリ解決失敗時は常に無効 |

`bracketPairing` の未指定は下位プロファイルの値を継承する。上位で明示した `auto` は
下位の `on` / `off` を解除し、`bracketPairingApps` / `bracketPairingAppPolicy` の判定に戻す。
TIP は共通 resolver の不変スナップショットからこのフィールドを解決し、Host への接続なしで適用する。

### 4.2 フィールド制約と backend 優先順位（確定）

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
列挙ではなく M46 `PrivacyGate` の per-axis クエリ（§5.1。**§5 のグローバル floor 適用後**の
実効状態で評価し、per-app `normal` がグローバル `offline` / `secure` / `custom` を緩和した
後の値ではない）に従う。これにより `custom`（例: 外部 AI のみ無効化）を含め全モードを
統一的に扱う。優先順位を以下に確定する:

1. まず `auto` を `settings.aiBackend` へ展開する（`auto` のまま下の判定に渡さない。
   継承された `openai` を取りこぼさないため）。
2. `PrivacyGate::AiCandidateAllowed() == false`（`secure`、または `custom` で AI 候補生成を
   無効化）→ `aiBackend = none`（外部・ローカルとも AI を使わない。M46 §5 の抑止契約に従う）。
3. `PrivacyGate::ExternalAiAllowed() == false`（`private` / `offline` / `custom` で外部 AI
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

`privacyMode` が `inherit` 以外（`normal` / `private` / `secure`）の
プロファイルは、解決後に M46 `PrivacyGate` へ通知する。理由文字列は
モードごとに以下:

- `secure` → `auto_secure_app`（学習・外部 AI 完全 OFF）
- `private` → `auto_private_app`（外部 AI OFF / 学習は context_hash のみ）
- `normal` → `auto_normal_app`（上位 profile レイヤの `private` / `secure` を明示解除して
  **グローバル floor** に戻す。グローバルが `offline` / `private` / `secure` の場合はその
  floor を維持する）

通知先は `docs/privacy-and-secure-input-spec.md` §5.1.1 の **host 側インスタンス**
である（プロファイル解決は `AppProfileResolver` が host 側で行うため）。

`inherit` の場合は通知せず、グローバル設定（`settings.privacyMode` 等）を
そのまま使う。`PrivacyGate` 側は同一ユーザーアクション内で複数通知を受けた場合、
各軸で最も厳しい制約を採り（`secure` > `private` / `offline` > `normal`。`private` と
`offline` は各軸の union）、かつグローバル `privacy.mode` を floor として下回らない。

## 6. 既存 `promptPrefixByApp` との統合

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
   先食いで適用しない（大文字混在キーも同じ照合規約で扱う）
3. 設定アプリでの編集は `profilesByApp` 側に書く（`promptPrefixByApp`
   は read-only legacy 扱い）
4. M48 リリース後 3 マイナーバージョンで `promptPrefixByApp` 削除予定
   （deprecation warning を CHANGELOG に記載）

共通 resolver の読み込み・解決処理に統合する。照合で衝突した既存キー（例: `Code.exe` と `code.exe` の
両方）は、**JSON のキー列挙順に依存しない決定的規則**として、衝突元キーを Unicode
コードポイント順で昇順ソートし**末尾（最大）を採用**する。
共通 validator は警告を返し、Host の `SettingsLoadResult.profile_warnings` と
設定アプリの保存結果で参照できる。`azookey_diag.exe` への表示接続は M48 の診断統合で行う。

## 7. 候補タグ Boost

`candidateTagBoosts` は M52 ベンチで定義する候補タグ（`Technical` /
`English` / `Polite` / `Casual` / `NamedEntity` 等）への倍率。各候補の
`final_score` に以下を掛ける:

```
raw   = profile.candidateTagBoosts.get(tag, 1.0)
boost = min(3.0, max(1.0, raw))   // [1.0, 3.0] にクランプ（手編集・移行値の暴走防止）
candidate.final_score *= boost
```

`max(1.0, …)` で下げ方向には使わず、`min(3.0, …)` で上限もクランプする（§4.2 の
`[1.0, 3.0]` を実際に強制するのはこのランタイム式）。boost のみ許可し、逆方向の調整は
タグ別の score weight 設定で行う（M11 範疇）。

## 8. UI（設定アプリ）

設定アプリに `アプリ別設定` タブを追加（M30 完了後の M48 着手時）:

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
  preferTechnicalTerms / candidateTagBoosts / privacyMode

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

Dispatcher への適用では、IPC ハンドラの先頭で `Resolve` を呼び、
PrivacyGate の許可範囲内で候補生成 / rerank / external AI を切り替える。
この消費側の統合は共通基盤とは別に実装する。

### 9.1 共通基盤と機能への適用境界

resolver は設定フィールドを選ぶ純粋な処理であり、TSF 操作、IPC、ファイル I/O、
PrivacyGate の通知を行わない。`privacyMode` はプロファイルからの要求値であり、
グローバル floor 適用後の許可判定ではない。消費側は §4.2 / §5 の PrivacyGate 制約を
満たしてから候補・学習・AI に適用する。共通基盤だけでは、これらの実動作は切り替わらない。
カッコ設定の適用境界は `bracket-pairing-spec.md` §4.5.0 とする。
カッコ設定を適用する in-process TIP は入力先の自プロセス名と自スレッドのウィンドウクラスを
使う。§3 の M46 / Host 向け前面アプリ・プライバシー検出を、このローカル識別で代替しない。

不正なプロファイル・未知フィールド・型や enum の不正は除外して下位層を継承し、
タグ倍率は読み込み時にも `[1.0, 3.0]` に制限する。未知のタグ名は保存時に保持し、
スコア適用時に既知タグだけを参照する。検証警告にアプリ名や設定値を含めない。
設定アプリでモデル設定を保存しても、プロファイルの有効フィールドと明示的な空文字を保持する。

## 10. テスト

- unit: 解決順（process_name → window_class → default → global）の網羅
- unit: `promptPrefixByApp` legacy 読み込み + `profilesByApp` 優先
- unit: `privacyMode = secure` 時に M46 PrivacyGate へ通知
- integration: `code.exe` 検出 → 技術語タグ boost
- integration: `outlook.exe` 検出 → polite タグ boost
- e2e（M50 connect）: アプリ切替 1 秒以内にプロファイル反映

## 11. M48 受け入れ条件

受け入れ条件の定義の正典は [`plans/windows-port-roadmap.md`](../plans/windows-port-roadmap.md)
の M48 節とする。本書はプロファイル解決・スキーマ・スコア適用・後方互換の写像を定義し、
受け入れ条件を複製しない。

## 12. 将来拡張（M48 範囲外）

- ウィンドウタイトル単位の細分化（プライバシー上、本実装では非対応）
- アプリ自動検出のクラウド辞書（プライバシー上、本実装では非対応）
