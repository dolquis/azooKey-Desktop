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

## 4. 自動 secure 判定

以下のいずれかに該当した場合、モードを一時的に `secure` とする:

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
`ForegroundApp.process_name`（§4.2 で `lower()` 済み）が実効集合に含まれる
場合に自動 secure とする。`lsass.exe` は Windows の UAC 認証ダイアログで
前面に来ることがあるため保険として含める。

> **バンドル既定の無効化（将来）**: 特定のバンドル既定をユーザーが個別に
> 無効化する用途（`privacy.secureAppsDisabled` 等の減算）は M46 範囲外とし
> §12 の将来拡張で扱う。M46 では実効リストを「バンドル既定 ∪ ユーザー追加」
> とし、減算はサポートしない（プライバシーを緩める方向の操作は後送り）。

#### 4.1.1 バンドル既定リストの保守手順・更新元

- **更新元はリポジトリのみ**: `kDefaultSecureApps` はコード内の静的定数
  （`tsf-tip/src/ForegroundAppDetector.cpp` に隣接する単一ヘッダ等）で定義し、
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
TIP プロセス側 `tsf-tip/src/ForegroundAppDetector.cpp`（新規）に**単一
インスタンス**として実装し、自動 secure 判定（§4）と M48 の per-request
`app` フィールド（app-profile §3.1）の双方を、この 1 つの検出器から供給する
（二重実装を作らない。`docs/windows-tsf-host-architecture.md` のコンポーネント
一覧と整合）。app-profile §3 は本定義を参照し、再定義しない。

```cpp
struct ForegroundApp {
  bool         resolved = false;       // 解決可否（false = 前面/プロセス取得不可）
  std::wstring process_name;           // "keepass.exe"（basename, lower() 正規化）
  std::wstring window_class;           // "Notepad" / "Chrome_WidgetWin_1"
  uint32_t     window_title_hash = 0;  // 学習・IPC 用 hash（FNV-1a 等）

  // 検出器プロセス内専用。IPC payload にもログにも載せない（§8 / dev-infra §7.6）。
  // hash 計算と将来のパスワード欄ヒューリスティック（§12）のためだけに保持する。
  std::wstring window_title;
};

class ForegroundAppDetector {
public:
  const ForegroundApp& Current();    // 500ms TTL キャッシュ（§4.3）
  void Invalidate();                  // フォーカス変更時に次回 Current() を強制再解決
};
```

`process_name` は検出器境界で `lower()` 正規化し、全消費側（`secureApps` 照合 /
M48 `AppProfileResolver` lookup）が小文字で比較できるようにする。`window_title`
生値は機密の可能性があるため、Host へ送る IPC payload には `window_title_hash`
のみを含める（§8 / `docs/dev-infrastructure-spec.md` §7.6 と整合）。

### 4.3 キャッシュ戦略・スレッド・解決機構・フェイルクローズ

**スレッド親和性**: `Current()` / `Invalidate()` は TIP の STA スレッド（TSF
コールバックスレッド）から呼ぶ。検出器は IPC を行わずブロッキングしない
（per-keystroke 呼び出しに耐える軽量同期処理）。

**キャッシュ（500ms TTL + イベント無効化）**:

- `Current()` は前回解決から 500ms 以内ならキャッシュ値を返し、超過時のみ
  再解決する。500ms TTL はイベント取りこぼし（同一ウィンドウのタイトル変更など）
  に対するバックストップ。
- フォーカス変更は `SetWinEventHook(EVENT_SYSTEM_FOREGROUND, …,
  WINEVENT_OUTOFCONTEXT)` で監視し、コールバックで `Invalidate()` を呼ぶ。次回
  `Current()` は TTL を待たず即再解決する。
- 優先順位: **イベント無効化が TTL に優先**（無効化されたら必ず再解決）、TTL は
  イベント間の上限。フック登録は検出器生成時、解除は破棄時に行いリークさせない。

**解決機構**:

1. `GetForegroundWindow()` → `GetWindowThreadProcessId()`
2. `OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, …)`
3. `QueryFullProcessImageNameW` で実行ファイルパス → basename → `lower()`
   （`docs/rich-features-spec.md` §X-4-4 の `K32GetModuleFileNameExW` 経路は本定義で
   置換する。UWP / 保護プロセスでの取得性が高い方を正典とする）
4. `RealGetWindowClassW` で `window_class`
5. `window_title`（内部専用）→ `window_title_hash`

**フェイルクローズ（fail closed）**: 前面ウィンドウ / プロセス名が解決できない
場合（HWND が null、`OpenProcess` が UIPI で拒否される＝TIP 非昇格で**昇格アプリ**が
前面、など）、`ForegroundApp{ resolved = false }` を返す。§2「fail closed」原則に従い:

- **プライバシー軸**: `autoSecureInput = true`（既定）のとき、解決不能な前面は
  **secure 扱い**にする（アプリが見えない＝機密の可能性があるため安全側へ）。
- **プロファイル軸（M48）**: 解決不能時はプロファイル未適用＝`default` / グローバルで
  扱う（boost なし）。プロファイルは非プライバシー軸のため fail-closed の対象外。

この非対称により、検出不能でも学習・外部 AI が秘密入力へ漏れない一方、通常の
候補生成は素の挙動を維持する。

## 5. secure 中の挙動契約

`PrivacyGate::IsSecure() == true` の間、以下を**強制抑止**する:

| 抑止対象 | 実装ポイント |
|---|---|
| `CommitObservation` IPC を送信しない | `tsf-tip/src/TextService.cpp::Commit` |
| `LearningStore::Observe` を呼ばない | `inference-host/src/Dispatcher.cpp` |
| `QueryPredictions` IPC を送信しない | `tsf-tip/src/PredictionWindow.cpp` |
| Magic Conversion を無効化 | `tsf-tip/src/TextService.cpp::OnDoubleTap` |
| OpenAI 等の外部 AI を `aiBackend=none` 強制 | `inference-host/src/AiBackend.cpp` |
| ログに `reading` / `surface` を含めない | M41 logger の redaction（`docs/dev-infrastructure-spec.md` §7.6 優先順位 1。Debug / `AZOOKEY_LOG_BODY=1` でも secure 中は出力しない） |
| 候補生成は内蔵変換 + 既存辞書のみ | `inference-host/src/Dispatcher.cpp` |
| M55 補正候補の学習・適用を停止 | `correction/TypoCorrectionEngine.cpp` |

### 5.1 PrivacyGate 実装

`inference-host/src/PrivacyGate.cpp`（新規）として以下を実装する:

```cpp
class PrivacyGate {
public:
  enum class Mode { Normal, Private, Secure, Offline, Custom };

  Mode CurrentMode() const;

  // 動作可否クエリ
  bool LearningAllowed() const;
  bool PredictionAllowed() const;
  bool ExternalAiAllowed() const;
  bool DetailedLoggingAllowed() const;

  // モード遷移
  void EnterSecureFor(std::wstring_view app);
  void ExitSecure();
  void SetExplicitMode(Mode mode);
};
```

`Dispatcher` は各 IPC ハンドラの先頭で `PrivacyGate` に問い合わせ、
許可されない処理は早期 return + ログ記録（`result = "blocked"`）と
する。

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
    "autoSecureInput": true,
    "disableLearningInPrivateMode": true,
    "disableExternalAIInPrivateMode": true,
    "redactLogs": true,
    "secureApps": [],
    "secureUrlPatterns": [],
    "privateApps": [],
    "showSecureIndicator": true
  }
}
```

`secureApps` には §4.1 のバンドル既定を再掲せず、ユーザー追加分のみを保存する
（既定 `[]`）。実効リストは §4.1 のとおりバンドル既定との和集合で評価する。

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
      "autoSecureInput": { "type": "boolean", "default": true },
      "disableLearningInPrivateMode": { "type": "boolean", "default": true },
      "disableExternalAIInPrivateMode": { "type": "boolean", "default": true },
      "redactLogs": { "type": "boolean", "default": true },
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
- `window_title` を `window_title_hash` のみに置換
- M16 Magic Conversion の prompt と応答を一切ログしない
- M55 typo の `raw_keys` / `observed_reading` を hash 化

これらは M44 診断 ZIP（`docs/dev-infrastructure-spec.md` §12.5）の
redaction ポリシーと共通の関数で処理する。

## 9. UpdatePrivacyMode IPC

設定アプリ / TIP から Host にモード変更を伝達する新規 IPC。エンベロープは
既存 wire format `{version, request_id, type, trace_id, payload}` に従う。

Request:

```json
{
  "version": 1,
  "request_id": 200,
  "type": "UpdatePrivacyMode",
  "trace_id": "018fd2c2-...",
  "payload": {
    "mode": "secure",
    "reason": "user_explicit"
  }
}
```

`reason` の取り得る値:

| 値 | 送信元 | 用途 |
|---|---|---|
| `user_explicit` | 設定アプリ / ショートカット | ユーザーが明示的に切替 |
| `auto_secure_app` | M48 AppProfileResolver | `privacyMode = secure` プロファイル解決時 |
| `auto_private_app` | M48 AppProfileResolver | `privacyMode = private` プロファイル解決時 |
| `auto_normal_app` | M48 AppProfileResolver | `privacyMode = normal` プロファイル解決時（厳格モードからの明示解除） |
| `auto_password_field` | M46 secure 検出 | パスワード入力欄に focus した時 |

`reason` 列は forward-compatible とし、未知の値は host 側で
`unknown_reason` 扱いとしつつ `mode` だけ適用する（M40 互換性ルール）。

Response:

```json
{
  "version": 1,
  "request_id": 200,
  "type": "UpdatePrivacyMode",
  "trace_id": "018fd2c2-...",
  "payload": {
    "applied_mode": "secure",
    "previous_mode": "normal"
  }
}
```

`MessageType` enum 末尾に `UpdatePrivacyMode` を append（M40 互換性）。

## 10. テスト

- unit: `PrivacyGate` の各モードでの動作可否
- unit: `ForegroundAppDetector` のキャッシュ動作
- integration: `secureApps` 指定で `Observe` / `QueryPredictions` /
  Magic Conversion が抑止される
- integration: secure 中の log redaction（reading / surface が
  `***redacted***` に置換）
- e2e（M50 connect）: KeePass 起動 → secure 表示 → 通常アプリ復帰 →
  normal 復帰

## 11. M46 受け入れ条件

- `secureApps` 指定アプリで `LearningStore::Observe` が一切呼ばれない
- secure 中は OpenAI API 呼び出しが発生しない
- 構造化ログに入力本文が残らない（Release / Debug 双方）
- 通常アプリに戻ると元のモードに復帰する
- 候補ウィンドウに secure インジケータが表示される（`showSecureIndicator
  = true` 時）
- M48 完了後の follow-up: `profile.privacyMode = secure` のプロファイル
  検出で当該アプリが自動 secure 扱いになる（M48 未完了時は M46 単独の
  `secureApps` リストで判定し、本受け入れ条件は M48 統合時に検証する）

## 12. 将来拡張

- パスワード欄の TSF 自動判定（`ITfContextView` から取得試行）
- ブラウザの URL パターン判定（Edge / Chrome の UI Automation）
- RDP / VM 内での自動 secure
- アプリ別の `mode` 切替（`custom` モードの GUI 編集）

これらは M46 の本範囲外。M46 では `secureApps` ベースの自動切替まで
実装し、上記は将来 M に分離する。
