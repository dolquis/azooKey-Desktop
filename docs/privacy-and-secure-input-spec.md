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
の前提として「ユーザーが AI / 学習を停止できる」契約を確立する。

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

### 4.1 `secureApps`（既定値）

```json
[
  "KeePass.exe",
  "KeePassXC.exe",
  "1Password.exe",
  "Bitwarden.exe",
  "LastPass.exe",
  "CredentialUIBroker.exe",
  "lsass.exe"
]
```

`lsass.exe` は Windows の UAC 認証ダイアログで前面に来ることがあるため
保険として含める。ユーザーは設定で追加・削除できる。

### 4.2 ForegroundAppDetector

M48 と共用するモジュール。`tsf-tip/src/ForegroundAppDetector.cpp`
（新規）として実装し、以下を提供する:

```cpp
struct ForegroundApp {
  std::wstring process_name;   // "KeePass.exe"
  std::wstring window_class;   // "Notepad" / "Chrome_WidgetWin_1"
  std::wstring window_title;   // best-effort, 機密の可能性あり
  uint32_t window_title_hash;  // 学習用 hash
};

class ForegroundAppDetector {
public:
  ForegroundApp Current();           // 500ms TTL キャッシュ
  void Invalidate();                  // フォーカス変更時
};
```

`window_title` 自体は機密の可能性があるため、Host へ送る IPC payload
には `window_title_hash` のみを含める（M41 §7.6 と整合）。

## 5. secure 中の挙動契約

`PrivacyGate::IsSecure() == true` の間、以下を**強制抑止**する:

| 抑止対象 | 実装ポイント |
|---|---|
| `CommitObservation` IPC を送信しない | `tsf-tip/src/TextService.cpp::Commit` |
| `LearningStore::Observe` を呼ばない | `inference-host/src/Dispatcher.cpp` |
| `QueryPredictions` IPC を送信しない | `tsf-tip/src/PredictionWindow.cpp` |
| Magic Conversion を無効化 | `tsf-tip/src/TextService.cpp::OnDoubleTap` |
| OpenAI 等の外部 AI を `aiBackend=none` 強制 | `inference-host/src/AiBackend.cpp` |
| ログに `reading` / `surface` を含めない | M41 logger の redaction フラグ |
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

`settings/mvp-settings.schema.json` に以下を追加:

```json
{
  "privacy": {
    "mode": "normal",
    "autoSecureInput": true,
    "disableLearningInPrivateMode": true,
    "disableExternalAIInPrivateMode": true,
    "redactLogs": true,
    "secureApps": [
      "KeePass.exe", "KeePassXC.exe",
      "1Password.exe", "Bitwarden.exe",
      "LastPass.exe", "CredentialUIBroker.exe",
      "lsass.exe"
    ],
    "secureUrlPatterns": [],
    "privateApps": [],
    "showSecureIndicator": true
  }
}
```

## 8. ログ redaction

M41 §7.6 のプライバシー配慮と整合する。secure 中は以下を Release ・
Debug 双方で抑止する:

- `reading`, `surface`, `candidate.text` を `***redacted***` に置換
- `window_title` を `window_title_hash` のみに置換
- M16 Magic Conversion の prompt と応答を一切ログしない
- M55 typo の `raw_keys` / `observed_reading` を hash 化

これらは M44 診断 ZIP（`docs/dev-infrastructure-spec.md` §12.5）の
redaction ポリシーと共通の関数で処理する。

## 9. UpdatePrivacyMode IPC

設定アプリから Host にモード変更を伝達する新規 IPC:

```json
{
  "message_type": "UpdatePrivacyModeRequest",
  "payload": {
    "mode": "secure",
    "reason": "user_explicit" | "auto_secure_app" | "auto_password_field"
  }
}
```

```json
{
  "message_type": "UpdatePrivacyModeResponse",
  "payload": {
    "applied_mode": "secure",
    "previous_mode": "normal"
  }
}
```

`MessageType` enum 末尾に append（M40 互換性）。

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
- M48 アプリ別プロファイルが secure 指定の場合、当該アプリで自動 secure
  扱いになる

## 12. 将来拡張

- パスワード欄の TSF 自動判定（`ITfContextView` から取得試行）
- ブラウザの URL パターン判定（Edge / Chrome の UI Automation）
- RDP / VM 内での自動 secure
- アプリ別の `mode` 切替（`custom` モードの GUI 編集）

これらは M46 の本範囲外。M46 では `secureApps` ベースの自動切替まで
実装し、上記は将来 M に分離する。
