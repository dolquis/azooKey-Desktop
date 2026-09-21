# プライバシー / セーフ入力モード 仕様

対象リポジトリ: dolquis/azooKey-Desktop
対応マイルストーン: M46（プライバシー / モデル管理 / 学習データ UI トラック）
関連: `plans/windows-port-roadmap.md` M7 / M16 / M34 / M48、
      `docs/dev-infrastructure-spec.md` §7 (M41 ログ)、
      `docs/typo-correction-learning-spec.md`、
      `docs/app-profile-spec.md`（M48）
作成日: 2026-05-27
位置づけ: Phase 5 直後の前倒し（M34 と並行）

## 1. 目的

AI 変換 / 学習 / 外部 API / ログが扱う情報をユーザーが制御できるように
し、パスワード欄や機密入力時に自動で安全側に倒す。IME は機密入力を扱う
ため、本機能は M16（Magic Conversion / OpenAI API）と M34（DPAPI 暗号化）
の **設計前提条件** として「ユーザーが AI / 学習を停止できる」契約を
確立する。実装順としては M16 着手前または同時期の投入を **推奨** する
（hard prerequisite ではない。`plans/windows-port-roadmap.md` の M46
「推奨実装時期」記述と整合）。M16 が単独で先行する場合は secure アプリ
向けの初期プライバシーギャップが生じるため、その期間の暫定的な抑止
方針を別途定める必要がある。

## 2. 設計原則

- **secure は最優先**: temporary mode（将来）/ app profile（M48）/
  M55 typo / M57 ModernBERT の全てに優先する
- **fail closed**: 判定不能なら safer 側へ
- **ローカル完結**: クラウド送信は明示同意なしに発生しない
- **可逆**: secure 状態は通常アプリへ復帰すれば自動解除される
- **透明性**: 現在のモードはユーザーが常に確認できる

## 3. モード定義

| モード | 学習 | 予測 | LLM / 外部 API | ログ | 用途 |
|---|---|---|---|---|---|
| `normal` | ON | ON | 設定に従う | 通常 | 普段使い |
| `private` | OFF | ON | local のみ | 最小 | 学習させたくない入力 |
| `secure` | OFF | OFF | OFF | エラーのみ | パスワード / 秘密情報 |
| `offline` | ON | ON | local のみ | 通常 | ネットワーク禁止 |
| `custom` | 個別指定 | 個別指定 | 個別指定 | 個別指定 | 上級者向け |

既定モードは `normal`。ユーザーが明示的に変更しない限り、自動 secure
判定（§4）でのみ一時的に `secure` へ落とす。

`custom`（個別指定）は固定プリセットを持たず、learning / prediction /
external-AI / AI-candidate / detailed-logging の 5 軸をユーザーが個別に
指定する上級者向けモードである。各軸を永続化する per-axis スキーマと
許可軸への解決は §5.2 / §7（`privacy.custom`）で定義する。

## 4. 自動 secure 判定

本章の自動 secure 判定は **`privacy.autoSecureInput`（§7、既定 `true`）が有効なときのみ**
動作する。`autoSecureInput=false` の場合、`secureApps` 一致・パスワード欄・URL パターンの
いずれの自動判定も行わず（§4.3 の解決不能時 fail-closed も含め auto-secure しない）、secure は
ユーザーが明示設定したときのみ有効になる。

以下のいずれかに該当した場合（`autoSecureInput` 有効時）、モードを一時的に `secure` とする:

| 判定 | 実装 | 優先 |
|---|---|---|
| `secureApps` リスト一致 | `ForegroundAppDetector` でプロセス名比較 | 1 |
| パスワード入力欄 | TSF context / UI Automation で取得可能なら | 2 |
| `secureUrlPatterns` 一致 | ブラウザの URL（取得可能なら） | 3 |

検出が難しい場合でも、**§4.1 の `secureApps` ベースの自動切替は必ず
実装する**。

### 4.1 `secureApps`（バンドル既定リスト + ユーザー追加）

`secureApps` の実効リストは **2 層**で構成する。両層とも実行ファイル
basename の **大文字小文字を無視**（`lower()` 正規化して比較）する。

1. **バンドル既定リスト** `kDefaultSecureApps`（コード内定数。アプリと
   一緒にバージョン管理され、リリースでのみ更新される）:

```json
[
  "keepass.exe",
  "keepassxc.exe",
  "1password.exe",
  "bitwarden.exe",
  "lastpass.exe",
  "credentialuibroker.exe",
  "lsass.exe"
]
```

2. **ユーザー追加リスト** `privacy.secureApps`（設定。既定 `[]`）。
   ユーザーが独自に追加するアプリのみを保持し、**バンドル既定を再掲しない**
   （§7 schema の既定が `[]` なのはこのため）。

実効判定は `effectiveSecureApps = kDefaultSecureApps ∪ lower(privacy.secureApps)`。
`ForegroundApp.process_name`（§4.2、照合時に大文字小文字を無視）が実効集合に含まれる
場合、`autoSecureInput` 有効時（§4 前段）に自動 secure とする。`lsass.exe` は
Windows の UAC 認証ダイアログで前面に来ることがあるため保険として含める。

> **バンドル既定の無効化（将来）**: 特定のバンドル既定をユーザーが個別に
> 無効化する用途（`privacy.secureAppsDisabled` 等の減算）は M46 範囲外とし
> §12 の将来拡張で扱う。M46 では実効リストを「バンドル既定 ∪ ユーザー追加」
> とし、減算はサポートしない（プライバシーを緩める方向の操作は後送り）。

#### 4.1.1 バンドル既定リストの保守手順・更新元

- **更新元はリポジトリのみ**: `kDefaultSecureApps` はコード内の静的定数
  （`core/include/azookey/core/SecureApps.h`。TIP の照合と Host の設定パースが同じ
  ヘッダを共有するため、`tsf-tip/` ではなく `core/` に置く。実効リストの照合主体は
  TIP であり、Host は §5.1.1 のとおり前面アプリを解決しない）で定義し、
  **ネットワーク取得・テレメトリ駆動の自動更新は行わない**（§12 および
  `docs/app-profile-spec.md` §12「クラウド辞書はプライバシー上非対応」と整合）。
- **更新はアプリリリース経由**: エントリの追加・削除は通常の PR としてレビュー
  し、リリースに同梱して配布する。本 spec §4.1 のリストとコード定数は同一 PR で
  同期し、片方だけ更新しない。
- **掲載基準**: 広く認知された資格情報 / 秘密情報マネージャ、または OS の認証
  ブローカ（`CredentialUIBroker.exe` / `lsass.exe` 等）に限る。一般アプリは誤検出で
  ユーザーの学習機会を不必要に奪うため入れない。追加 PR には掲載基準を満たす
  根拠を記載する。
- **照合規約**: basename の完全一致（パス・引数を含めない）で `lower()` 比較する。
  ワイルドカード・正規表現は使わない（M46 範囲外）。

### 4.2 ForegroundAppDetector（共有・正準定義）

M46 が導入し M48（`docs/app-profile-spec.md`）と**共用する正準コンポーネント**。
入力先アプリにロードされた TIP の `tsf-tip/src/ForegroundAppDetector.cpp` に**単一
インスタンス**として実装し、自動 secure 判定（§4）と M48 の per-request
`app` フィールド（app-profile §3.1）の双方を、この 1 つの検出器から供給する
（二重実装を作らない。`docs/windows-tsf-host-architecture.md` のコンポーネント
一覧と整合）。app-profile §3 は本定義を参照し、再定義しない。

```cpp
struct ForegroundApp {                // core/include/azookey/core/AppProfileResolver.h
  std::string process_name;           // UTF-8 の実行ファイル basename
  std::string window_class;           // UTF-8 の同一プロセスのウィンドウクラス
  bool resolved{false};               // プロセス名の解決可否
};

class ForegroundAppDetector {
public:
  core::ForegroundApp Get();          // owner thread から同期取得
  void Invalidate();                  // キャッシュをクリア
};
```

型の正典は `core/include/azookey/core/AppProfileResolver.h`、検出器の宣言は
`tsf-tip/include/azookey/tsf/ForegroundAppDetector.h` に置く。プロセス名は照合側で
大文字小文字を無視して比較する。タイトル・タイトル hash は取得も保持もしない。

### 4.3 キャッシュ戦略・スレッド・解決機構・フェイルクローズ

`Get()` / `Invalidate()` は TIP の owner thread から呼ぶ。検出器は
`GetModuleFileNameW(nullptr, ...)` で入力を受け取る同一プロセスの実行ファイルを取得し、
UTF-8 の basename を `Invalidate()` までキャッシュする。各 `Get()` で
`GetFocus()` と `GetAncestor(..., GA_ROOT)` を使い、同一プロセスのウィンドウだけを
対象に `GetClassNameW` でクラス名を取得する。クラスを取得できなければ空文字列とする。
500ms TTL や前面ウィンドウの WinEventHook は設けない。

モジュールパス取得失敗・バッファ不足・basename の UTF-8 変換失敗では
`resolved = false` とする。ウィンドウクラスの取得失敗だけでは unresolved にしない。
他プロセスを `OpenProcess` する設計ではなく、UIPI による他プロセス照会失敗を
この検出器の失敗条件として扱わない。

- **プライバシー軸**: `autoSecureInput = true` のとき、プロセス名の解決不能は secure 扱い。
- **プロファイル軸（M48）**: 解決不能時は `default` / グローバル設定を適用する。

**入力 scope 軸の扱い**: §4 表の優先 2（パスワード入力欄）は TSF の
`GUID_PROP_INPUTSCOPE` から判定する。scope が password / PIN と**積極的に判定
できたときだけ** secure とし、取得できない場合（property 未提供・同期読取セッション
の拒否・scope 0 件）は secure 側へ倒さない。前面アプリ軸の fail-closed が backstop
として働くうえ、`GUID_PROP_INPUTSCOPE` を提供しないアプリは珍しくなく、ここで倒すと
学習が無言で広範に停止するためである。AI 送信軸は §2 のとおり fail closed を保ち、
判定不能な scope では送信を抑止する。この二軸の非対称が `AiInputAllowed` と
`SecureInputDetected`（`tsf-tip/src/AiInputGuard.cpp`）の差である。

## 5. secure 中の挙動契約

TIP が `ResolvePrivacy` で secure と判定し、`secure_input_` が有効な間、以下を**強制抑止**する:

| 抑止対象 | 実装ポイント |
|---|---|
| `CommitObservation` IPC を送信しない | `tsf-tip/src/TextService.cpp::CommitSelected` が `ResolvePrivacy` で判定して `pending_commit_observation_` を捨て、`PostIpcSend` が `CommitObservation` / `CommitSegmentsObservation` を経路共通の choke point として落とす |
| `LearningStore::Observe` を呼ばない | `inference-host/src/Dispatcher.cpp` |
| `QueryPredictions` IPC を送信しない | `tsf-tip/src/TextService.cpp::PostIpcSend` が message type で落とす。学習・予測系の送出は `PostIpcSend` を通す規約とし、queue へ直接積まない |
| Magic Conversion を無効化 | `tsf-tip/src/TextService.cpp::OnDoubleTap` |
| OpenAI 等の外部 AI を `aiBackend=none` 強制 | `inference-host/src/AiBackend.cpp` |
| ログに `reading` / `surface` を含めない | M41 logger の redaction（`docs/dev-infrastructure-spec.md` §7.6 優先順位 1。Debug / `AZOOKEY_LOG_BODY=1` でも secure 中は出力しない） |
| 候補生成は内蔵変換 + 既存辞書のみ | `inference-host/src/Dispatcher.cpp` |
| M55 補正候補の学習・適用を停止 | `correction/TypoCorrectionEngine.cpp` |

### 5.1 判定の責務と許可軸

TIP は `TextService::ResolvePrivacy` で設定と入力先の検出結果を解決し、
`secure_input_` を使って送信・学習スナップショットを抑止する。Host は
`Dispatcher` の各要求処理で、その要求のフラグと設定を検査する。
共有するのは許可条件であり、状態を共有するゲートオブジェクトやモード遷移通知 API は設けない。

学習、予測、外部 AI、AI 候補生成、詳細ログは別の許可軸である。§3 / §5.2 は
各モードの意味を定義する。外部 AI の許可は AI 候補生成の許可を前提とし、
secure は全軸に優先する。これらの軸名は C++ の呼出 API を表さない。
private / custom の学習方針の適用は、secure 判定とは別の消費側の責務とする。

詳細ログの設定解決は `core::ParsePrivacyPolicy(settings)` が担い、
`RuntimeSettings.privacy_policy` に `core::PrivacyPolicy` を保持する。
その既定は `secure = true`、`detailed_logging_allowed = false` である。
本文の出力条件は `docs/dev-infrastructure-spec.md` §7.6 に従う。

#### 5.1.1 判定主体と二段ゲート

本節は `docs/typo-correction-learning-spec.md` §12.12.2 / §12.13、
`docs/ai-backend-spec.md` §8、`docs/app-profile-spec.md` §5 から参照される。

| レイヤ | 判定入力 | 役割 |
|---|---|---|
| TIP | 設定、入力 scope、入力先アプリ | イベント時に secure を判定し、送信・保留観測を抑止する |
| Host | 当該要求の privacy フラグ、接続の受理済み capability、設定 | 学習要求を独立に検査する。入力先を自ら検出しない |

`ObserveTypo`、`CommitObservation`、`CommitSegmentsObservation` は
イベントごとの `secure` と `learning_allowed` を運ぶ。Host が学習を許可するのは、
受理済み接続が `secure_flag` に対応し、当該要求の `secure == false` かつ
`learning_allowed == true` の場合に限る。欠落・型不正はそれぞれ
`true` / `false` の安全側で扱い、直近の `QueryCandidates` から推定しない。
`QueryCandidates` にも `secure` を載せる。wire 契約は
`docs/typo-correction-learning-spec.md` §12.13 と payload 定義を参照する。

これは protocol v1 の加算的拡張である。古い TIP と新しい Host の組合せでは
学習を拒否し、新しい TIP と古い Host の組合せでは TIP の送信抑止を維持する。
`QueryPredictions` / `TransformSelectedText` の secure 由来の送信抑止は TIP が担う。
これらに学習観測用フラグの欠落判定を流用しない。

### 5.2 `custom` モードの per-axis 解決

`privacy.mode = custom` のとき、各許可軸は `privacy.custom`（§7）の対応フラグを
返す。`custom` は §3 の他モードのような固定プリセットを持たず、軸ごとにユーザーが許可・抑止
を指定する上級者向けモードである。

**クエリと per-axis フラグの対応**:

| 許可軸 | backing フラグ | 既定 |
|---|---|---|
| 学習許可 | `privacy.custom.learning` | `false` |
| 予測許可 | `privacy.custom.prediction` | `true` |
| AI 候補生成許可 | `privacy.custom.aiCandidate` | `true` |
| 外部 AI 許可 | `privacy.custom.externalAi ∧ privacy.custom.aiCandidate` | `false` |
| 詳細ログ許可 | `privacy.custom.detailedLogging ∧ ¬privacy.redactLogs` | `false`（`redactLogs` 既定 `true` のため） |

**解決順（precedence）**:

1. **secure が最優先**（§2）。自動 secure 判定（§4）または明示 secure の間は `custom` の
   per-axis フラグを無視し、§5 の secure 契約（全軸抑止・backend `none`・ログ redaction）を
   適用する。`custom` は secure を緩めない。
2. secure でないとき `mode = custom` なら、各クエリは上表の backing フラグをそのまま返す。
   未指定の軸は §7 schema の既定（= private 相当の安全側）で補完するため、欠落キーがあっても
   挙動は一意に定まる。
3. **不変条件の強制**: §5.1 の 外部 AI 許可 ⇒ AI 候補生成許可 を保つため、
   `aiCandidate = false` のときは `externalAi` の保存値によらず 外部 AI 不許可 に
   強制する（AI 候補生成を止めるなら外部送信も止まる、の安全側固定）。このとき backend は
   `none`（ローカル zenzai も外部 LLM も動かさない。§5.1 と整合）。
4. **`redactLogs` は詳細ログの floor**: 詳細ログ許可は per-axis フラグ単独では
   true にならず、`privacy.redactLogs = false`（既定 `true`）を併せて満たす場合のみ true になる。
   `privacy.custom.detailedLogging = true` でも `redactLogs = true` の間は本文系フィールドを
   redact し続ける（`docs/dev-infrastructure-spec.md` §7.6 の設定解決に従う）。
   これにより Debug + `AZOOKEY_LOG_BODY=1` であっても、`redactLogs` が有効な限り入力本文は出力されない。

**既定の意味**: per-axis 既定（learning OFF / prediction ON / aiCandidate ON / externalAi OFF /
detailedLogging OFF）は §2「fail closed」に沿った private 相当の安全側であり、軸を 1 つも
指定しない `custom` は private と同じ実効挙動になる。これにより DEV-121 の暫定 fallback
（`custom` → `private`）が後方互換に置換される。

**他設定との関係**: `disableLearningInPrivateMode` / `disableExternalAIInPrivateMode`（§7）は
`private` モード専用のトグルであり、`custom` には適用しない（`custom` では `privacy.custom.*`
が唯一の権威）。`docs/app-profile-spec.md` §4.2 の backend 解決は、`custom` でも
AI 候補生成許可 / 外部 AI 許可 の 2 クエリ経由で一貫して評価される。

## 6. UI 表示

候補ウィンドウ下部 or 設定アプリで現在のプライバシー状態を確認できる。

```
🔒 セーフ入力中: 学習・AI・予測は停止しています
```

毎回ラベルを出すと邪魔になるため、初回のみ toast / 小さなインジケータ
表示とする:

- secure 突入直後 1 回 toast 表示（5 秒で自動消滅）
- 候補ウィンドウ右端に小さな 🔒 アイコンを常時表示
- 通常モード復帰時は toast なし

## 7. 設定スキーマ

`settings/mvp-settings.schema.json` の既定 `additionalProperties: false`
制約下で、新規 top-level key `privacy` を追加する。schema 追加と Host
側の読み書き実装は同一 PR でまとめ、schema 不在のまま `privacy.*` を
書き込む不整合状態を作らない。

設定例（実際に書き込まれる JSON 値）:

```json
{
  "privacy": {
    "mode": "normal",
    "custom": {
      "learning": false,
      "prediction": true,
      "externalAi": false,
      "aiCandidate": true,
      "detailedLogging": false
    },
    "autoSecureInput": true,
    "disableLearningInPrivateMode": true,
    "disableExternalAIInPrivateMode": true,
    "redactLogs": true,
    "crashReportConsent": "off",
    "secureApps": [],
    "secureUrlPatterns": [],
    "privateApps": [],
    "showSecureIndicator": true
  }
}
```

`secureApps` には §4.1 のバンドル既定を再掲せず、ユーザー追加分のみを保存する
（既定 `[]`）。実効リストは §4.1 のとおりバンドル既定との和集合で評価する。

`settings/mvp-settings.schema.json` の `privacy` が持つキーは `mode`・`crashReportConsent`・
`custom.aiCandidate`・`custom.externalAi`・`custom.detailedLogging`・`redactLogs`・
`secureApps`・`showSecureIndicator` であり、
`inference-host/src/SettingsStore.cpp` と `settings-app/SettingsDocument.cpp` の許可キーも
これに一致する。schema が持たない軸（`autoSecureInput`・`secureUrlPatterns`・`privateApps`・
`disableLearningInPrivateMode`・`disableExternalAIInPrivateMode`・`custom` の
learning / prediction）は書き込めない。`additionalProperties: false` が
schema 検証で弾き、`settings-app/SettingsDocument.cpp` の許可キー判定は未知の `privacy`
フィールドを含む object を `{"mode": "secure"}` へ潰す。実行時はこれらの軸の既定値が
適用され、`autoSecureInput` は `true` 固定として §4 の自動 secure 判定が常に働く。
§4 前段が定めるユーザー側の無効化手段は、当該キーが schema へ入るまで存在しない。

`privacy.custom` は `mode = custom` のときのみ参照する（他モードでは無視する）。
各軸の既定は §5.2 の private 相当の安全側に揃え、欠落キーは schema 既定で補完される。
`custom` の解決順・不変条件（`aiCandidate = false` で `externalAi` を強制 OFF）は §5.2 を正典とする。

`crashReportConsent`（M33）は azooKey 管理クラッシュダンプの収集同意を表す。本キーは
schema をここで正典として定義し、値の意味（`off` / `local` の挙動・ダンプ最小化・
保持運用）は `docs/sideload-packaging-spec.md` §8.3 を正典とする。既定は `off` で、
明示的な `local` 選択時のみ、本文・スタック・レジスタなどのメモリを含まない
メタデータをローカル保存する。自動送信は行わない。
**M33 の enum は `off` / `local` のみ**とし、送信（upload）は schema 化しない。送信経路は
将来 M で実装する際に、**バージョン / タイムスタンプ付きの明示同意レコード**として別途導入する
（bare な `"upload"` 値を先行して永続化しない。理由は §8.3）。ローダは欠落・未知値・型不正を
`off` に正規化し、設定ファイルの読み取り・解析失敗時も収集を停止する。
**導入 M の区別**: 共有AI基盤では`mode`と`custom.aiCandidate` / `custom.externalAi`を
先行導入する。TIPの入力scopeがpassword/PIN、または判定不能ならAI送信を抑止する。
hostはリクエストの許可と設定の許可の積を取り、TIPから許可を引き上げられない。
他の学習・予測・ログの各軸とM46のUIはこのAI用判定とは別範囲とする。
`crashReportConsent`はM33（ETW/WER）で、M33では`privacy` objectに
`crashReportConsent` を既定値付きで加算的に追加する（`docs/sideload-packaging-spec.md` §3.6
「予定済み top-level 拡張」と整合）。

schema fragment（`properties.privacy` への追加）:

```json
{
  "privacy": {
    "type": "object",
    "additionalProperties": false,
    "properties": {
      "mode": {
        "type": "string",
        "enum": ["normal", "private", "secure", "offline", "custom"],
        "default": "normal"
      },
      "custom": {
        "type": "object",
        "additionalProperties": false,
        "properties": {
          "learning": { "type": "boolean", "default": false },
          "prediction": { "type": "boolean", "default": true },
          "externalAi": { "type": "boolean", "default": false },
          "aiCandidate": { "type": "boolean", "default": true },
          "detailedLogging": { "type": "boolean", "default": false }
        },
        "default": {
          "learning": false,
          "prediction": true,
          "externalAi": false,
          "aiCandidate": true,
          "detailedLogging": false
        }
      },
      "autoSecureInput": { "type": "boolean", "default": true },
      "disableLearningInPrivateMode": { "type": "boolean", "default": true },
      "disableExternalAIInPrivateMode": { "type": "boolean", "default": true },
      "redactLogs": { "type": "boolean", "default": true },
      "crashReportConsent": {
        "type": "string",
        "enum": ["off", "local"],
        "default": "off"
      },
      "secureApps": {
        "type": "array",
        "items": { "type": "string" },
        "default": []
      },
      "secureUrlPatterns": {
        "type": "array",
        "items": { "type": "string" },
        "default": []
      },
      "privateApps": {
        "type": "array",
        "items": { "type": "string" },
        "default": []
      },
      "showSecureIndicator": { "type": "boolean", "default": true }
    }
  }
}
```

## 8. ログ redaction

`docs/dev-infrastructure-spec.md` §7.6 の redaction ポリシー正典に従う。secure は
同表の **優先順位 1**（最優先）であり、Release・Debug いずれでも、また
`AZOOKEY_LOG_BODY=1` が設定されていても本文系フィールドを出力しない。
具体的には secure 中は以下を Release・Debug 双方で抑止する:

- `reading`, `surface`, `candidate.text` を `***redacted***` に置換
- `window_title` を redact（タイトル hash も生成しない）
- M16 Magic Conversion の prompt と応答を一切ログしない
- M55 typo の `raw_keys` / `observed_reading` を hash 化

これらは M44 診断 ZIP（`docs/dev-infrastructure-spec.md` §12.5）の
redaction ポリシーと共通の関数で処理する。

## 9. 設定反映とイベント単位の判定

明示モードは設定 `privacy.mode` から読み取る。TIP は入力イベント時に
`ResolvePrivacy` で入力先と設定を評価し、Host は各要求のフラグを評価する。
アプリ解決結果をモード遷移 IPC や理由通知へ変換して共有状態を更新する契約は設けない。
学習イベントの wire 契約は §5.1.1 に従う。

## 10. テスト

- unit: TIP の secure 判定、Host の要求ごとの学習可否、詳細ログの設定解決
- unit: `ForegroundAppDetector` のキャッシュ動作
- integration: `secureApps` 指定で `Observe` / `QueryPredictions` /
  Magic Conversion が抑止される
- integration: secure 中の log redaction（reading / surface が
  `***redacted***` に置換）
- e2e（M50 connect）: KeePass 起動 → secure 表示 → 通常アプリ復帰 →
  normal 復帰

## 11. M46 受け入れ条件

受け入れ条件の定義の正典は [`plans/windows-port-roadmap.md`](../plans/windows-port-roadmap.md)
の M46 節とする。本書は secure 判定・プライバシー判定の責務境界・ログ redaction・設定キーを
定義し、受け入れ条件を複製しない。

## 12. 将来拡張

- ブラウザの URL パターン判定（Edge / Chrome の UI Automation）
- RDP / VM 内での自動 secure
- アプリ別の `mode` 切替（`custom` モードの GUI 編集）
- パスワード欄判定の UI Automation 経路（§4 表の優先 2 は TSF の
  `GUID_PROP_INPUTSCOPE` と `ES_PASSWORD` まで。UIA へは広げない）

M46 の範囲は `secureApps` ベースの自動切替と §4.3「入力 scope 軸の扱い」の
パスワード欄判定までとし、上記は将来 M に分離する。
