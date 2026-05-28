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
`ForegroundApp` 構造体（process_name / window_class / window_title_hash）
を取得し、500ms TTL でキャッシュ。フォーカス変更（`WinEventHookProc`
監視）で invalidate する。

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

`settings/mvp-settings.schema.json` の既定 `additionalProperties: false`
制約下で、新規 top-level key `profilesByApp` を追加する。schema 追加と
Host 側の読み書き実装は同一 PR でまとめ、schema 不在のまま
`profilesByApp` を書き込む不整合状態を作らない。

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
          "additionalProperties": { "type": "number" },
          "default": {}
        },
        "privacyMode": {
          "type": "string",
          "enum": ["inherit", "normal", "private", "secure"],
          "default": "inherit"
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

`privacyMode` のみ特殊扱い: `inherit` の場合だけ下位層を継承し、
明示値（`normal` / `private` / `secure`）は下位を上書きする
（プライバシー設定を意図せず緩める方向に継承しない方針）。

`privacyMode` が `inherit` 以外（`normal` / `private` / `secure`）の
プロファイルは、解決後に M46 `PrivacyGate` へ通知する。理由文字列は
モードごとに以下:

- `secure` → `auto_secure_app`（学習・外部 AI 完全 OFF）
- `private` → `auto_private_app`（外部 AI OFF / 学習は context_hash のみ）
- `normal` → `auto_normal_app`（グローバル既定に戻す。上位プロファイルから
  `private` / `secure` を継承していた場合に明示解除する用途）

`inherit` の場合は通知せず、グローバル設定（`settings.privacyMode` 等）を
そのまま使う。`PrivacyGate` 側は同一ユーザーアクション内で複数通知を
受けた場合、最も厳しいモード（`secure > private > normal`）を採用する。

## 6. 既存 `promptPrefixByApp` との統合

既存設定キー `promptPrefixByApp` は値 `{"<process>": "<prefix>"}` の
map（M30 横断テーマ X-2-6）で、key は大文字小文字混在の実プロセス名
（例: `Code.exe`, `OUTLOOK.EXE`）で書かれている既存ユーザー設定がある。
M48 の resolver は `profilesByApp[process_name.lower()]` で lookup する
ため、legacy 側も **読み込み時に key を `lower()` 正規化**して同じ規約に
揃える。M48 では以下の移行戦略をとる:

1. `profilesByApp[process.lower()].promptPrefix` を優先
2. それが未設定なら `promptPrefixByApp[process.lower()]` を読む
   （`SettingsManager` 読み込み時に大文字混在キーは `lower()` 正規化済み）
3. 設定アプリでの編集は `profilesByApp` 側に書く（`promptPrefixByApp`
   は read-only legacy 扱い）
4. M48 リリース後 3 マイナーバージョンで `promptPrefixByApp` 削除予定
   （deprecation warning を CHANGELOG に記載）

`SettingsManager` で読み込み時に統合し、内部表現は `profilesByApp`
ベースに統一する。lower 化で衝突した既存キー（例: `Code.exe` と
`code.exe` の両方）は最後に読み込んだ値を採用し、warning を `azookey_diag.exe`
に記録する。

## 7. 候補タグ Boost

`candidateTagBoosts` は M52 ベンチで定義する候補タグ（`Technical` /
`English` / `Polite` / `Casual` / `NamedEntity` 等）への倍率。各候補の
`final_score` に以下を掛ける:

```
boost = max(1.0, profile.candidateTagBoosts.get(tag, 1.0))
candidate.final_score *= boost
```

`max(1.0, ...)` で実効的に「下げる」ことはせず、boost のみ許可する。
逆方向の調整はタグ別の score weight 設定で行う（M11 範疇）。

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

`inference-host/src/AppProfileResolver.cpp`（新規）として実装する:

```cpp
struct ResolvedProfile {
  std::string profile_name;
  bool prediction_enabled;
  bool sentence_completion;
  bool learning_enabled;
  std::string ai_backend;
  std::string prompt_prefix;
  std::string style;
  bool prefer_technical_terms;
  std::map<std::string, double> candidate_tag_boosts;
  PrivacyMode privacy_mode;
};

class AppProfileResolver {
public:
  ResolvedProfile Resolve(const AppContext& app);
private:
  void LoadFromSettings(const Settings& s);
};
```

Dispatcher は IPC ハンドラの先頭で `Resolve` を呼び、その結果に基づいて
候補生成 / rerank / external AI を切り替える。

## 10. テスト

- unit: 解決順（process_name → window_class → default → global）の網羅
- unit: `promptPrefixByApp` legacy 読み込み + `profilesByApp` 優先
- unit: `privacyMode = secure` 時に M46 PrivacyGate へ通知
- integration: `code.exe` 検出 → 技術語タグ boost
- integration: `outlook.exe` 検出 → polite タグ boost
- e2e（M50 connect）: アプリ切替 1 秒以内にプロファイル反映

## 11. M48 受け入れ条件

- VS Code（`code.exe`）で技術語タグの候補順位が上がる
- Outlook（`outlook.exe`）で polite タグの候補順位が上がる
- secure 指定アプリ（`profile.privacyMode = secure`）で学習・外部 AI が
  停止する（M46 と整合）
- アプリ切替後 1 秒以内にプロファイルが反映される
- 既存 `promptPrefixByApp` 値が新 `profilesByApp[].promptPrefix` として
  読み込まれる（後方互換）

## 12. 将来拡張（M48 範囲外）

- ウィンドウタイトル単位の細分化（プライバシー上、本実装では非対応）
- アプリ自動検出のクラウド辞書（プライバシー上、本実装では非対応）
