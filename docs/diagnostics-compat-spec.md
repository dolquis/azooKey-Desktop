# IME 診断・修復ウィザードとアプリ互換性テストハーネス 仕様（M44・M50）

対象リポジトリ: dolquis/azooKey-Desktop
対応マイルストーン: M44（§12）、M50（§13）
関連: `docs/dev-infrastructure-spec.md`（開発基盤の入口と転送表）、
      `docs/observability-spec.md`（構造化ログと redaction）、
      `docs/host-availability-spec.md`（Host の状態と `QueryDiagnostics` の入力）、
      `docs/handoff/windows-diagnostics-playbook.md`、
      `compat-test/README.md`、
      `plans/windows-port-roadmap.md` M44 / M50

本書は `docs/dev-infrastructure-spec.md` から §12・§13を切り出した文書である。節番号 §12.x・§13.x は旧 spec のまま保ち、旧 spec の転送表が§12・§13を本書へ解決する。本書内で、本書にない節（旧 spec の他の節）を指すときは、文書名を付けて書く（例: `observability-spec.md` §7.6）。

## 正典範囲

IME 診断・修復ウィザード（`azookey_diag`）の診断項目、判定基準、CLI、診断 ZIP、機密情報の扱い、`QueryDiagnostics` の IPC、UI（§12）と、
アプリ互換性テストハーネス（`compat-test/`）の対象アプリ、テストケース、実装、出力、CI 連携（§13）は本書が正典である。

## 節とマイルストーンの対応

| 節 | マイルストーン |
|---|---|
| §12 | M44 |
| §13 | M50 |

## 12. IME 診断・修復ウィザード（M44）

### 12.1 目的

azooKey が動作しない、候補が出ない、Zenzai が使われていない、学習が
反映されない、IME 登録が壊れているといった問題をユーザー自身または
開発者が短時間で切り分けられるようにする。IME は「入力できない」時点で
UX が即死しやすいため、本機能は配布前（Phase 4 ゲート）に優先実装する。

#### 12.1.1 M44 実装スコープ

M44 は `azookey_diag.exe --json`、`--repair`、`--collect` を提供し、
D-001〜D-015 を診断する。
Host が停止している場合もローカルで判定できる項目を継続し、全項目を
stable schema の `checks` 配列へ出力する。

Host の診断は §12.6 `QueryDiagnostics` を使う。`engine` は実効ランタイム tier
（R1 の `llama_cpp` / `mock`。R2 追加後は `winml`）を表し、`model_loaded` と
`loaded_model_path` は実際にロードされたモデルだけを表す。設定済みパスの存在だけで
モデルロード成功と判定してはならない。

診断 ZIP は `diag.json`、`settings.redacted.json`、`host-health.json`、
`ipc-ping.json`、`environment.txt`、`crash-summary.txt` で構成する。Release
ランタイムログが存在する場合は、直近 7 日の `host-*.jsonl` と `tip-*.jsonl` を
ローテート世代（`.jsonl.N`）も含め、redaction 後に `logs/` へ収集する。
収集は新しいファイルを優先し、1 ファイルあたり末尾 1 MiB、合計 8 MiB を上限とする。
切り詰めまたは除外が発生した場合は `logs/README.txt` に件数と上限を記録する。

設定アプリの「プライバシー」ペインの障害診断の節は follow-up とする。

### 12.2 診断項目

| ID | 項目 | チェック内容 | 失敗時の推奨修復 |
|---|---|---|---|
| D-001 | TIP DLL 存在 | 登録済み DLL パスが存在するか | 再登録を促す |
| D-002 | COM 登録 | CLSID / InprocServer32 / Profile GUID が正しいか | `scripts/register-dev.ps1` または MSIX 修復 |
| D-003 | 言語プロファイル | 日本語 `0x0411` の Profile があるか | Profile 再登録 |
| D-004 | Host 起動 | 現在のログオンセッションに Host プロセスが存在するか | Host 起動 |
| D-005 | IPC Handshake | Named Pipe 接続 + Handshake 成功 | Host 再起動 |
| D-006 | IPC Ping | Ping 往復 latency 測定 | pipe / firewall / Host 状態確認 |
| D-007 | モデルパス | 設定上のモデル（R1=`.gguf` ファイル / R2=ONNX GenAI ディレクトリ）が存在するか | モデル選択 UI（M45）へ誘導 |
| D-008 | モデル検証 | 形式別検証（R1=GGUF magic / version、R2=`genai_config.json` + 参照 ONNX） | 破損モデル扱い |
| D-009 | fallback 状態 | Zenzai / SimpleConverter / degraded を表示 | モデルロード再試行。`safe_mode` は `safeMode.enabled=false` に変更し、Host を再起動するか `UpdateConfig` を送る（`host-availability-spec.md` §8.5.3） |
| D-010 | learning store | 読み込み可能か、破損していないか、v2 と暗号化 M7 の併存 | 保存による M7 cleanup の再試行、破損時はバックアップ後に初期化 |
| D-011 | user dict | JSON 読み込み可能か | バックアップ後に修復 |
| D-012 | settings | schema validation 成功 | 不正値のリセット |
| D-013 | logs | `%LOCALAPPDATA%\azooKey\logs\` 書き込み可能 | ディレクトリ作成 |
| D-014 | DPAPI | 暗号化データを復号できるか | 再認証 / 再入力を促す |
| D-015 | app compatibility | 前面ウィンドウのプロセスアーキテクチャと AppContainer 状態から既知の非対応条件を判定する。対象アプリ内の TSF context 取得可否は CLI からは観測しない | 互換性情報表示（M50 result） |

D-013 は実効ログディレクトリ内に一時ファイルを作成して削除し、実際の書き込み可否を
判定する。プロセス終了などで残った診断用一時ファイルは、次回診断時に 24 時間を
超えていれば削除する。Host を一度も起動していない環境ではディレクトリが存在せず
`error` になりうる。この場合の `--repair` はログディレクトリを作成して再診断する。

D-012 の schema 正典は `settings/mvp-settings.schema.json` とし、CI / pre-commit は
同 schema の meta-schema 妥当性と代表サンプルの適合性を静的に検証する（`build-ci-spec.md` §4.3）。

### 12.2.1 判定基準（status 決定ルール）

各項目の `status`（§12.3 の `ok` / `warning` / `error`）は以下の閾値で確定する。
閾値は `host-availability-spec.md` §8.5.2 timeout 表・§12.6 `fallback_state` enum・§13.2 自動化レベルと
同一値を参照し、二重定義しない。`--repair` 列は roadmap M44 の受け入れ条件の
自動修復対象（D-001 / D-002 / D-003 / D-013）と一致する。

**評価順序（重複時の優先）**: 各行の `ok` / `warning` / `error` 述語が重複し
うる場合は、**`error` → `warning` → `ok` の順に評価し、最初に一致した（最も
厳しい）status を採用する**。これにより各行の status は一意に定まり、緩い
`ok` 述語が厳しい状態を隠さない。

| ID | ok | warning | error | `--repair` |
|---|---|---|---|---|
| D-001 | DLL パス存在かつ呼び出し元 bitness と一致 | — | パス不在 or bitness 不一致 | ✓ 再登録 |
| D-002 | CLSID / InprocServer32 / Profile GUID が全一致 | 任意キー欠落（動作には影響しない） | 必須キー欠落 or 値不一致 | ✓ `scripts/register-dev.ps1` / MSIX 修復 |
| D-003 | `0x0411` Profile 登録済み | — | 未登録 | ✓ Profile 再登録 |
| D-004 | Host プロセス存在 | — | プロセス不在 | ✗（起動案内のみ。自動起動は M42 / 起動経路の責務） |
| D-005 | 接続 + Handshake Ready | — | 接続不可 or Handshake 失敗 | ✗ |
| D-006 | Ping RTT ≤ 100ms | 100ms < RTT ≤ 500ms | RTT > 500ms or 無応答（`host-availability-spec.md` §8.5.2 Ping timeout） | ✗ |
| D-007 | `model.enabled=false`（Zenzai 無効 = 該当なし）、または enabled かつ `model.selectedPath` が存在（R1=`.gguf` ファイル / R2=`genai_config.json` を含む ONNX GenAI ディレクトリ） | enabled だがパス未設定（Zenzai ON だがモデル未選択） | enabled かつ設定済みパスが不在 | ✗（M45 モデル選択へ誘導） |
| D-008 | `model.enabled=false`、またはモデル未選択（`model.selectedPath` 空 = 検証対象なし。該当なしとし、未選択の warning は D-007 が担う）、または enabled かつ**選択済み**モデルが `valid` **かつ loaded**（R1=GGUF magic / version、R2=`genai_config.json` パース + 参照 ONNX 実在。model-management-spec §3.3 の format 別 `valid` を使う） | enabled かつ選択済みモデルが `valid` だが未ロード（fallback 動作中）、または Host の実効ランタイムが `mock`（§12.6 `engine`） | enabled かつ**選択済み**モデルの形式別検証に失敗（R1: magic 不一致 / version 非対応 / 破損、R2: config 不正 / 参照 ONNX 欠落） | ✗ |
| D-009 | `fallback_state == healthy`、または（`safe_mode` でない）`model.enabled=false`（SimpleConverter 固定が意図された設定） | `degraded_simple` / `degraded_model`（enabled 時の非意図的劣化）。Host に到達できず `QueryDiagnostics` を得られない場合は、`model.enabled` なら `degraded_simple`、そうでなければ `healthy` とみなす（`host-availability-spec.md` §8.5.1） | `safe_mode`（`model.enabled` に関わらず最優先） | ✗（復旧は D-005 / D-008 修復経由。`safe_mode` の解除は `host-availability-spec.md` §8.5.3） |
| D-010 | 読み込み成功・schema 妥当（空 / 新規を含む）かつ暗号化 M7 の cleanup 待ちでない | 旧 schema だが migration 可能、または可読な v2 と `learning.tsv.enc` が併存 | 読み込み不可 / 破損 / cleanup 対象の存在確認不可 | ✗（バックアップ後の初期化は手動確認） |
| D-011 | JSON 読み込み成功（空 / 新規・欠損ファイルは空として正常） | — | パース不可 / 破損 | ✗（バックアップ後の修復は手動確認） |
| D-012 | schema validation 成功、または `settings.json` が無い（既定値で動作） | 本節の登録済み旧形式に一致し、登録された移行経路で移行できる | 読み込み不可、JSON 不正、または validation 失敗（旧ベンチマーク履歴の例外を含む） | ✗（不正値リセットは確認後） |
| D-013 | logs ディレクトリ書き込み可 | — | ディレクトリ未作成、または書き込み不可 | ✓ ディレクトリ作成 |
| D-014 | OpenAI 鍵を要求する**実効バックエンド**が無い（global `aiBackend` と全 `profilesByApp.*` の app-profile §4.2 解決後の実効値がいずれも `none` / `local-zenzai`）、または OpenAI を要求する実効バックエンドがあり `openAiApiKey` が非空で有効（plaintext〔M16–M34 移行期。schema が plaintext を許容〕はそのまま有効、`dpapi:` prefix 付きは復号成功） | OpenAI を要求する実効バックエンド（global もしくは**いずれかの** `profilesByApp.*` が `build-ci-spec.md` §4.2 解決後に `openai`）があるが `openAiApiKey` が空（資格情報未設定で認証不可）、または settings の読み込み・schema 検証の失敗や DPAPI を使えないことで判定できない（`details.state` が `unavailable`。settings の不備そのものは D-012 が担う） | `dpapi:` prefix 付きの暗号化値が復号失敗 | ✗（再認証 / 再入力を促す） |
| D-015 | —（CLI は対象アプリ内の TSF context を直接観測しない） | x64 の通常プロセス、または前面ウィンドウ・プロセス情報・アーキテクチャ・トークンの取得不能で、既知の非対応条件を確定できない | x86、x64 以外のアーキテクチャ、または AppContainer プロセスを確認 | ✗（§13 互換性情報へ） |

D-002 の `warning` は任意の表示名だけが欠けた状態であり、`--repair` は登録を再実行しない。
必須登録に不備がある `error` の場合だけ再登録し、再診断で `warning` まで改善した場合は修復成功とする。

D-010 の `details.legacy_retained` は、v2 の検出時に `learning.tsv.enc` の存在を
確認した結果を真偽値で返す。存在確認に失敗した場合は `error` とする。
v2 が可読なら併存を cleanup 待ちの `warning` とし、v2 が読めない場合の `error` を
優先する。v2 がなく、可読で非空のヘッダなし旧 TSV を読んだ場合は migration の `warning` とする。
`escaped=1` の M7 ファイルだけがある場合は、それだけでは `warning` にしない。
診断はファイルを削除せず、v2 との併存判定では legacy の本文を復号・出力しない。削除と再試行の条件は
`docs/user-learning-enhancement-spec.md` §3.1 に従う。平文 M7、`.bak`、`.corrupt-*` の
残存だけでは cleanup 待ちと判定しない。

D-012 が `warning` とする旧形式は、本節に登録したものに限る。`settings.json` は版識別子を持たず、
`settings/mvp-settings.schema.json` は加算的に拡張する（`docs/settings-app-spec.md` §3.6
「拡張方針」）。破壊的変更は下記の登録を伴うため、登録済みの旧形式が無い限り、過去の schema に
適合した `settings.json` は現行 schema にも適合し、`ok` になる。ただし下記のベンチマーク履歴の例外を除く。
後方互換として受理する値形式、すなわち平文の `openAiApiKey`（D-014 が扱う）、root tier の
`backendPreference` / `epPreference`（`docs/model-management-spec.md` §5.2）、
`backendPreference` の `directml` / `npu`（同 §5.1）、`promptPrefixByApp`（`docs/app-profile-spec.md` §6）も
schema に適合するため D-012 では `ok` とし、非推奨であることを D-012 では案内しない。

旧形式の登録は、キーの改名・削除・enum 縮小を伴う schema 変更と同じ PR で行い、
旧形式の識別方法と、Host の `SettingsStore` または設定アプリの `settings-app/SettingsDocument.*` に
実装した移行経路を併記する。`docs/app-profile-spec.md` §6 が予定する `promptPrefixByApp` の削除や、
`directml` / `npu` を enum から外す変更は、この登録の対象になる。

厳密な履歴 schema を導入した `model.benchmarkHistory` は、上記の後方互換保証の例外とする。
旧設定アプリが読んだ `model` / `model_path` 形式、`completedAt` などの必須フィールド不足、
nullable な旧計測値を持つ要素は、過去の schema が履歴を配列として受理していても、
現行 schema に適合しなければ D-012 を `error` とする。旧応答の追加フィールドを持つ場合も同じで、
診断用コピーから旧行を除いて `warning` に緩和しない。
整理経路は `model-management-spec.md` §7.1 の Host 追記時整理であり、
利用者が次回ベンチマークを実行した際に不適合な旧要素を除き、新しい結果を追記する。
設定アプリの通常保存と診断は旧要素を保持し、診断も `--repair` もこの整理を実行しない。

登録済みの旧形式に一致しない schema 非適合は、旧形式らしく見えても（`schemaVersion` などの
未定義キーを含む場合も）旧形式と推定せず `error` とする。D-012 の判定は `settings.json` を
読むだけで書き換えない。移行と不正値のリセットは `--repair` の対象外とし、
上記の移行経路または利用者の確認を経て行う。

ARM64 ホストでは `IsWow64Process2` の結果だけで x64 エミュレーションと ARM64
ネイティブを区別できないため、`GetProcessInformation(ProcessMachineTypeInfo)` で
対象プロセスのアーキテクチャを確認する。同情報を取得できない場合は
`architecture_unknown` の `warning` とし、ARM64 を非対応と推定しない。
確認できた ARM64 ネイティブは、`docs/sideload-packaging-spec.md` §0.1 の
x64 入力対象スコープに従い
`unsupported_architecture` とする。

D-015 の `details.reason` は `context_unverified`、`no_foreground_window`、
`process_unavailable`、`architecture_unknown`、`container_unknown`、`x86`、
`unsupported_architecture`、`app_container` の固定 enum とする。`warning` は
「対応を確認済み」ではなく、プロセス条件からは TSF context の可否を決められないことを
意味する。アプリ別の実入力と既知の劣化は §13 の互換性ハーネスで判定する。
ウィンドウタイトル、プロセス名、入力本文は D-015 の JSON に含めない。

全体 `status` は §12.4 の規約どおり `checks[].status` の最悪値
（`error` > `warning` > `ok`）とする。通常の `warning` は「縮退しているが入力は
継続できる」を意味する。D-015 の `warning` だけは TSF context が未確認であり、
入力の成立を主張しない。`error` は「当該機能が成立しない」を意味し、UI（§12.7）の
アイコン（✅ / ⚠️ / ❌）に対応させる。任意データ・任意機密の「未設定」は
正常系として扱い、`warning` を出さない。具体的には、クリーンインストール
直後で学習・辞書が空（`UserDictionary::Load()` は欠損ファイルを空の成功
ロードとして扱う）の D-010 / D-011 と、OpenAI 鍵を要求する**実効バックエンド**が
無い構成（global および全 app-profile の `build-ci-spec.md` §4.2 実効 `aiBackend` が
`none` / `local-zenzai`）で復号対象の機密が無い D-014 は
`ok` とする。ただし global もしくはいずれかの app-profile の実効バックエンドが
`openai`（`build-ci-spec.md` §4.2 解決後）で `openAiApiKey` が空の場合は、
当該 AI backend が認証できないため D-014 を `warning` とする
（機能を選択したのに資格情報が欠落している状態）。`warning` / `error` は
migration 要・読み込み不可・破損・**選択中機能の資格情報欠落**、または
**設定済みの値が期待どおり復号 / 読込できない**場合に限る。

**D-014 の「実効バックエンド」の定義**: global `settings.aiBackend` と、設定された
全 `profilesByApp.*` プロファイルの `aiBackend` を `docs/app-profile-spec.md` §4.2 の
解決規則（`auto` を global へ展開 → プライバシー判定による降格を適用）で評価した実効値の
集合を指す。診断は前面アプリに依存しない**静的構成チェック**のため、その集合の
**いずれか 1 つでも** `openai` に解決されれば「OpenAI 鍵を要求する」とみなす。逆に
プロファイルが `aiBackend=openai` を宣言しても、privacy（`secure`、または `private` /
`offline` / `custom` の外部 AI 無効）で `build-ci-spec.md` §4.2 step 2–3 により `local-zenzai` / `none` へ
降格する実効値は鍵を要求しない（D-014 の判定対象に含めない）。`profilesByApp` が未導入
（M48 前）の構成では実効集合は global のみとなり、従来の D-014（global `aiBackend` のみで
判定）と等価で後方互換である。
同様に、`model.enabled=false`（SimpleConverter 固定。model-management-spec
§モデル設定）は Zenzai を使わない意図的構成のため、D-007 / D-008 / D-009 は
`ok`（該当なし）とし、モデル未選択・未ロード・`degraded_model` を `warning`
扱いしない。これらが `warning` になるのは `model.enabled=true` のときのみ。
ただし `safe_mode`（連続クラッシュによる安全モード）は `model.enabled` に
関わらず D-009 を `error` とし、この「該当なし」ショートカットより優先する。

### 12.3 `azookey_diag.exe` CLI

CLI は 3 サブコマンドを持つ。

```powershell
azookey_diag.exe --json                                  # 全項目を JSON で出力
azookey_diag.exe --repair                                # 自動修復可能なものを実行
azookey_diag.exe --collect --output azookey-diag.zip     # 診断 ZIP 生成
```

`--json` 出力例:

```json
{
  "status": "warning",
  "timestamp_ms": 1780000000000,
  "checks": [
    {
      "id": "D-008",
      "name": "model_validation",
      "status": "warning",
      "message": "Selected model is valid but is not loaded",
      "details": {
        "valid": true,
        "loaded": false,
        "engine": "llama_cpp"
      }
    }
  ]
}
```

`status` は `ok` / `warning` / `error` の 3 値。`--json` はテスト可能な
stable schema として固定する。
`--repair` は D-001、D-002、D-003、D-013 を修復した後に全項目を再診断し、
同じ root object へ次の `repairs` 配列を追加する。

```json
{
  "probe_failed": false,
  "repairs": [
    {
      "id": "D-001",
      "status": "succeeded | failed | not_needed | permission_denied",
      "before_status": "ok | warning | error",
      "after_status": "ok | warning | error | null",
      "message": "string"
    }
  ]
}
```

D-001〜D-003 の machine-wide COM / TSF 再登録は 1 回の操作にまとめる。
非昇格プロセスでは UAC を自動起動せず、`permission_denied` と昇格して再実行する手順を返す。
修復対象が `ok` の場合は操作せず `not_needed` を返す。
post-probe を実行できない場合は `probe_failed=true` とし、`after_status` を `null` にする。
修復前に `ok` だった項目は `not_needed` のまま保持し、修復を試みた項目は `failed` とする。
D-001〜D-003 の直接修復は AppContainer ACL を変更しないため、UWP または Microsoft Store
アプリで検証する場合は `scripts/register-dev.ps1` を実行して ACL を付与する。
再登録が失敗すると DLL 側の rollback により修復前の登録も失われる場合がある。
post-probe で状態悪化を検出した場合は `failed` とし、`scripts/register-dev.ps1` または
MSIX 修復を案内する。
診断の実行と JSON または ZIP の生成に成功した場合、`status` が `error` でも
プロセスの exit code は `0` とする。
引数の不正、probe の実行失敗、ZIP の生成失敗では exit code `2` を返す。
`--repair` で `failed` または `permission_denied` が残った場合は exit code `3` を返す。
自動化側は exit code ではなく、出力された `status` と `checks` で診断結果を判定する。

### 12.4 診断 ZIP 構成

```
azookey-diagnostics-YYYYMMDD-HHMMSS.zip
├── diag.json
├── settings.redacted.json
├── host-health.json
├── ipc-ping.json
├── logs/
│   ├── host-YYYYMMDD.jsonl   (直近 7 日)
│   ├── host-YYYYMMDD.jsonl.N (ローテート世代)
│   ├── tip-YYYYMMDD.jsonl    (直近 7 日)
│   ├── tip-YYYYMMDD.jsonl.N  (ローテート世代)
│   └── README.txt            (切り詰めまたは除外時のみ)
├── environment.txt           (OS バージョン / CPU / RAM / GPU)
└── crash-summary.txt         (azooKey 管理ダンプの一覧のみ、ダンプ本体は含めない)
```

`diag.json` は §12.3 `--json` 出力と同一の **stable schema** とし、以下を正典とする
（snapshot テストで固定。本文系フィールドは持たない）:

```json
{
  "status": "ok | warning | error",
  "timestamp_ms": 0,
  "checks": [
    {
      "id": "D-001 .. D-015",
      "name": "string (安定 ID。例 model_validation)",
      "status": "ok | warning | error",
      "message": "string (人間向け。本文・候補・prompt を含めない)",
      "details": { "...": "check 固有のメタ情報のみ（パス / bool / 件数 / hash / mtime）" }
    }
  ]
}
```

- `status` は `checks[].status` の最悪値（error > warning > ok）。
- 各 ZIP メンバの突き合わせは `observability-spec.md` §7.3 の相関 ID では行わず、診断は単発スナップショット。
  `host-health.json` は §12.6 `QueryDiagnostics` の payload を保存する。ただし
  文字列 field は §12.5 の `host-health.json` 行の規則で処理してから保存し、
  生の payload を**そのまま書き出す経路は持たない**。
- 新しい check は `D-0NN` を末尾追加（後方互換）。既存 ID の `name` 改名は破壊的変更扱い。

### 12.5 機密情報の取り扱い

診断 ZIP には以下を**含めない**:

- OpenAI API key（DPAPI 暗号化済みでも除外）
- 入力本文・候補本文
- Magic Conversion の prompt
- 変換前後の全文
- ユーザー辞書の実データ
- 学習 TSV / typo_corrections.tsv / auto_words.tsv の本文

必要な場合でも、**件数・サイズ・hash・mtime のみ**を記録する。
`settings.redacted.json` は API key 等の機密 field を `***redacted***`
に置換した copy。

ZIP メンバごとの redaction ルールを以下に固定する。本文系フィールドの
出力可否は `observability-spec.md` §7.6 redaction ポリシーを正典とし、診断 ZIP は `observability-spec.md` §7.6 と
**共通の redaction 関数**を用いて二重定義を作らない。

| ZIP メンバ | redaction ルール |
|---|---|
| `diag.json` | §12.4 stable schema の制約に従い `message` / `details` に本文・候補・prompt を含めない。パス中のユーザー名は `%LOCALAPPDATA%` 等の環境変数表記へ正規化する |
| `settings.redacted.json` | API key 等の機密 field に加え、Magic Conversion prompt 系 field（`promptPrefixByApp` の各値、および移行後の `profilesByApp.*.promptPrefix`〔`profilesByApp` は process / window class をキーとする map。配列ではなく全エントリ値が対象〕）を `***redacted***` に置換。さらに path 系 field（`model.selectedPath` / `model.directory` / `customRomajiTablePath` 等の絶対パス）は `diag.json` / `host-health.json` と同じくユーザー名を含む生パス（`C:\Users\...`）を残さないよう `%LOCALAPPDATA%` 等の環境変数表記へ正規化する。`observability-spec.md` §7.6 はログ本文の正典で settings の prompt / path field を対象に含めないため、診断 ZIP では本欄で明示的に処理し、§12.5 の「prompt を含めない」方針と整合させる |
| `host-health.json` | §12.6 `QueryDiagnostics` payload。構造化 field（bool / enum / 数値）はそのまま保存するが、文字列の `loaded_model_path` / `last_error` / `ep_last_error` は `core::RedactFreeText`（`observability-spec.md` §7.6）でパス正規化（`%LOCALAPPDATA%` 等）と資格情報の伏せ字を通してから保存し、生のユーザーパスを残さない。自由文字列から本文を後で検出することはできないため、Host は `last_error` / `ep_last_error` を作る時点で入力本文・候補・prompt を入れず、固定の理由語・HRESULT・backend や HTTP のステータスだけで構成する |
| `ipc-ping.json` | RTT / 成否のみ |
| `logs/*.jsonl*` | `observability-spec.md` §7.6 の共通 field 判定を用いてログとローテート世代を収集（Release 既定で本文なし）。Debug かつ `AZOOKEY_LOG_BODY=1` の本文入りログは収集時に再 redact する。新しいファイルから 1 ファイル末尾 1 MiB、合計 8 MiB まで収集し、切り詰めまたは除外は `logs/README.txt` に記録する |
| `environment.txt` | OS バージョン / CPU / RAM / GPU のみ。ホスト名・ユーザー名・シリアル番号を含めない |
| `crash-summary.txt` | azooKey 管理ダンプ（`docs/sideload-packaging-spec.md` §8.2 の保存先と命名規則に合う `.dmp`）の件数・合計サイズと、各ファイルのファイル名・サイズ・更新時刻だけを記録する。ファイル名は `<module>` / UTC 時刻 / PID だけから成り、ディレクトリ部分は書かない。ダンプを開かず、ダンプ本体・スタック上の文字列バッファを含めない。同意が `off` で保存先が無い場合と、ダンプが 0 件の場合はその旨を 1 行で記す。WER（OS）が保存したダンプは対象外とする |

### 12.6 IPC: QueryDiagnostics

Host 側の状態を取得する新規 IPC。MessageType enum 末尾に追加する
（M40 互換性ルール）。

エンベロープは既存 wire format `{version, request_id, type, trace_id, payload}`
に従う。

```
Request:
  { "version": 1, "request_id": 1, "type": "QueryDiagnostics",
    "trace_id": "...", "payload": {} }

Response:
  { "version": 1, "request_id": 1, "type": "QueryDiagnostics",
    "trace_id": "...",
    "payload": {
      "model_loaded": bool,
      "loaded_model_path": str (optional, model_loaded=true のときだけ存在),
      "engine": "llama_cpp" | "mock" | "winml",
      "backend": str, "rss_mb": int,
      "backend_selection": {                  (optional, 起動時の選択結果)
        "source": "cli" | "settings" | "auto",
        "auto_backend": str, "reason": str,
        "adapters_known": bool,
        "hw_gpu_count": uint64, "hw_ml_only_count": uint64,
        "vulkan_loader": bool, "cuda_runtime": bool,
        "os_build": uint32, "winml_os_eligible": bool, "on_battery": bool
      },
      "prompt_format": "zenz-v3" | "kanakanji-edge-v1"   (optional, ロード中モデルの値),
      "ep": str (optional, R2/winml 時の選択 EP 名),
      "ep_state": str (optional, "NotPresent"|"NotReady"|"Ready"|"Registered"|"Failed"),
      "ep_last_error": str (optional, EP 取得・登録失敗の HRESULT/診断文; §4.6),
      "learning_entries": int, "user_dict_entries": int,
      "fallback_state": "healthy" | "degraded_simple" |
                        "degraded_model" | "safe_mode",
      "last_error": str (optional),
      "neologd_layer": {                       (optional)
        "state": "not_requested" | "loading" | "ready" | "missing_pack" | "error",
        "reason": str (optional, state="error" のときの固定の分類名)
      }
    } }
```

v1 の reader は `rss_mb`、`learning_entries`、`user_dict_entries` の欠落を `0` として扱う。
`engine`、`backend`、`fallback_state` は必須であり、欠落時は payload 全体を不正とする。
`neologd_layer` は protocol v1 の追加フィールドで、欠落と未知の `state` はどちらも「無し」として
扱い、payload 全体は不正にしない。値の意味は `docs/azdic-physical-layer-spec.md` §15.14 が定める。
`reason` は `neologd_pack_load` ログと同じ固定の分類名だけで、パスやサーバの応答を含まない。

`backend_selection` は protocol v1 への省略可能な追加フィールドである。欠落、型違い、
内部フィールドの欠落や数値範囲外は「無し」として扱い、payload 全体は不正にしない。
未知の内部キーは無視する。値は Host 起動時の BackendSelector の結果で、選択規則と
`reason` の固定分類名は `docs/copilot-pc-backend-spec.md` §4.4 に従う。
`source` は起動時の要求元、`auto_backend` は一次フィルタの候補であり、明示指定や
ロード失敗後の実 backend は従来どおり `backend` が示す。Health の意味も変えない。
電源変化や `UpdateConfig` の後もこの起動時の値を保持する。
`adapters_known=false` は列挙不能または SafeMode による probe 省略、`os_build=0` は
OS build 不明を表す。SafeMode の probe 省略時は件数と bool も 0 / false になる。
アダプタ名・識別子・パス・自由文は含めず、分類名と非識別の事実のみを返す。
診断 CLI の JSON は `D-008` の `details.backend_selection` に同じ値を載せる。

`prompt_format` は protocol v1 への省略可能な追加フィールドで、ロード中モデルが使う
プロンプト形式（catalog の宣言、または語彙からの推定。`docs/zenzai-inference-spec.md`
§2.4.2）を返す。モデル未ロード、Zenzai 以外の変換器、mock runtime（`runtime_tier` が `mock`）、古い Host では省略する。
reader は欠落と未知の値を「無し」として扱い、payload 全体は不正にしない。値は
2 つの固定名だけで、パスや利用者の入力を含まない。診断 CLI の JSON は `D-008` の
`details.prompt_format` に同じ値を載せる（省略時は載せない）。

`--collect` 時はこの IPC で取得した値を `host-health.json` に保存する。
`last_error` / `ep_last_error` は失敗時にユーザーパスを含みうるため、保存前に
§12.5 の `host-health.json` 行の規則で処理する。Host がこれらに入力本文・候補・prompt を
入れないことも同行が定める。

### 12.7 UI

M44 v1（Phase 4 ゲート）は **CLI（`azookey_diag.exe`）+ 診断 ZIP** のみで
完結させる。設定アプリ `診断` タブは M30 完了後の follow-up として
v1.x で追加する位置づけ（M30 を M44 v1 の前提にはしない）。下記の GUI
要件は v1.x（M30 完了後）に適用する。Host 未起動でも GUI 単体で項目
D-001〜D-003 / D-007 / D-008 / D-011〜D-013 までは実行可能とする。

```
[azooKey 診断]

状態: 一部問題があります

[1] TIP 登録状態             ✅ 正常
[2] Host 起動状態            ✅ 起動中
[3] IPC 接続                 ✅ Ping 12ms
[4] Zenzai モデル            ⚠️ 未ロード。SimpleConverter fallback 中
[5] 学習データ               ✅ 正常
[6] ユーザー辞書             ✅ 正常
[7] 設定ファイル             ✅ 正常
[8] ログ出力                 ✅ 有効
[9] セキュリティ設定         ✅ API キーは DPAPI 暗号化済み

[再チェック] [自動修復] [診断 ZIP を作成] [ログフォルダを開く]
```

### M44 受け入れ条件

受け入れ条件の定義の正典は [`plans/windows-port-roadmap.md`](../plans/windows-port-roadmap.md)
の M44 節とする。本書は §12 で診断項目・判定基準・診断 ZIP の redaction を定義し、
受け入れ条件を複製しない。

診断 ZIP に秘密情報が含まれないことは snapshot テストで検証する。

## 13. アプリ互換性テストハーネス（M50）

### 13.1 目的

主要アプリで TSF composition / 候補ウィンドウ位置 / 確定 / キャンセル /
Unicode / 絵文字 / Undo / Redo が壊れないことを半自動で検証し、
アプリ互換性のベースラインを確保する。

### 13.2 対象アプリ

| 種別 | アプリ | 自動化レベル |
|---|---|---|
| 標準 | Notepad | full |
| 標準 | WordPad / Notepad 後継 | best-effort |
| ブラウザ | Edge | full |
| ブラウザ | Chrome | full |
| ブラウザ | Firefox | best-effort |
| Electron | VS Code | full |
| Electron | Discord | best-effort |
| Electron | Slack | best-effort |
| Office | Word | recorder |
| Office | Excel | recorder |
| Office | Outlook | recorder |
| Terminal | Windows Terminal | full |
| Terminal | PowerShell ISE | best-effort |
| UWP / WinUI | Windows Settings | full |
| UWP / WinUI | Store apps | best-effort |

`full` = UI Automation + SendInput + screenshot による完全自動。
`best-effort` = 自動化可能な範囲のみ、残りは checklist。
`recorder` = キーボード操作の記録・再生（Office は UI Automation の
信頼性が低い）。

M50 の自動ゲートは次の target 定義を使う。
runner は target ごとに新規ウィンドウと一時文書を作り、既存の文書やタブを操作しない。

| target | AppId | トップレベル window class | 優先する UI Automation control |
|---|---|---|---|
| Notepad | `Microsoft.WindowsNotepad_8wekyb3d8bbwe!App` | `Notepad` | Document |
| Edge | `MSEdge` | `Chrome_WidgetWin_1` | Edit + Name `azooKey compatibility editor` |
| VS Code | `Microsoft.VisualStudioCode` | `Chrome_WidgetWin_1` | Document |

Edge は ControlType だけで Edit を探索するとアドレスバーへ一致し得るため、テスト HTML の
`aria-label` が公開する UIA Name との AND 条件で textarea を特定する。

### 13.3 テストケース

| ID | ケース | 期待動作 |
|---|---|---|
| C-001 | `nihongo` → Space → Enter で確定 | 「日本語」が確定される |
| C-002 | Backspace で preedit が戻る | 1 文字分戻る |
| C-003 | ESC で composition 破棄 | preedit が消える |
| C-004 | 候補ウィンドウがキャレット付近に出る | キャレット下に出る |
| C-005 | マルチディスプレイ端で候補が画面外に出ない | 画面内にクランプ |
| C-006 | DPI 150% で候補位置がずれない | 正しくスケール |
| C-007 | 絵文字 / サロゲートペア入力 | 正しく入力される |
| C-008 | Undo / Redo が破綻しない | 元に戻る |
| C-009 | フォーカス移動時に composition が安全に処理される | crash しない |
| C-010 | Host kill 中も入力が固まらない | DegradedSimple で継続 |
| C-011 | `Ctrl+A/C/V/L/S`、Alt メニュー、Win キー併用を押す | TIP が食わずアプリ / OS へ通る |
| C-012 | `ja` / `ju` / `jo` / `jya` / `jyu` / `jyo` を入力 | 「じゃ」「じゅ」「じょ」として preedit / commit できる |
| C-013 | supervisor 配下の Host を一時停止して入力し、再開する | TIP の `Degraded` 中もかな候補で入力を続け、再開後 `Ready` と通常変換へ戻る |
| C-014 | ライブ変換 OFF で `kitto` / `syatu` / `siro` / `nn` を入力 | 各かなを preedit / commit できる |
| C-015 | 固定 2 候補で ↑↓ / Space / Shift+Space / Ctrl+N/P、数字 2、Esc 1 回 | 選択 surface、確定文字列、Esc 1 回で候補窓と preedit が消えることを確認する |
| C-016 | ライブ変換 OFF で `nihongo` + 即 Space を一括送出し、確定後も入力する | 候補・確定・後続入力を UIA で確認する |
| C-017 | preedit 中に Ctrl+H を押す | 1 単位が削除される |
| C-018 | ライブ変換 ON で入力・編集・確定する | UIA で preedit と確定文字列の遷移を確認する |
| C-019 | 予測候補を表示し Tab / Shift+Tab / Esc を押す | 候補窓の予測モード（窓プロパティ `azooKey.CandidateWindowMode` で候補と区別する）の位置、受理後の文字列変化、Esc 1 回で窓と preedit が消えることを確認する。Shift+Tab は予測モードに 2 件以上表示された場合に判定し、1 件の場合は failing-skip とする。表示候補の index は文字列から判定しない |

C-001〜C-012 は `full` アプリ（§13.2）で UI Automation により自動判定できる。
C-014〜C-019 は UI Automation と対象 process の HWND 列挙により自動判定する。
C-013 は UI Automation と TIP の構造化ログを組み合わせて判定する。
C-013 は DEV-1364 の追加ケースであり、M50 の C-001〜C-012 受け入れ条件は変えない。
予測候補の文字そのものと描画品質は UIA で判定しない。
ライブ変換が ON の C-014 / C-016、ライブ変換が OFF の C-018、予測候補が返らない場合、
固定 2 候補の入力配列が利用できない場合は
該当ケースを `failing-skip` として残す。
ただし M50 完了ゲートの**必須対象は Notepad / VS Code / Edge**（M50 受け入れ
条件）であり、その他の `full` アプリ（Chrome / Windows Terminal /
Windows Settings 等）は実行可能な範囲で自動判定する best-effort 対象とする。
`best-effort` / `recorder` アプリでは自動化できないケースを §13.3.1 の
手動チェックリストで補完する。
TSF の描画責務を見る実機の手動チェックリスト（M3 の表示属性、M5 の `pbShow`）は `compat-test/` に置き、
一覧は `compat-test/README.md`「手動チェックリスト」が持つ。

### 13.3.1 Office 手動チェックリスト（recorder 補完）

Office（Word / Excel / Outlook）は UI Automation の TSF テキストパターン
信頼性が低く（§13.2 `recorder`）、キー操作の記録・再生 + 目視チェック
リストで代替する。各行は「対象」列のアプリでのみ確認し、対象外アプリでは
N/A（`report.md` に `N/A` と記録し pass/fail 判定に含めない）とする。pass /
fail を手動記録して `report.md` の Office セクション（§13.5）へ転記する。

| ID | 対象 | チェック項目 | 期待 |
|---|---|---|---|
| O-01 | Word / Excel / Outlook | 本文（Word 段落 / Excel セル / Outlook 本文）で `nihongo`→Space→Enter | 「日本語」確定、文字化けなし |
| O-02 | Word / Excel / Outlook | 候補ウィンドウがキャレット付近に出る | キャレット下に出て、セル / 行移動に追従 |
| O-03 | Excel | セル編集（F2）と数式バーの双方で composition が成立 | 双方で確定できる |
| O-04 | Excel | 未確定中の矢印キーが preedit 内移動になる | preedit 内で動く（確定後はセル移動） |
| O-05 | Word | オートコレクト / オートフォーマットが composition を破壊しない | preedit 中は介入しない |
| O-06 | Outlook | 宛先（To / Cc）欄と本文の双方で入力できる | 双方で確定できる |
| O-07 | Word / Excel / Outlook | 絵文字 / サロゲートペア確定後の表示（C-007 相当） | 正しく表示される |
| O-08 | Word / Excel / Outlook | ESC で composition 破棄 / Backspace で 1 文字戻る（C-002 / C-003 相当） | preedit が消える / 1 文字戻る |
| O-09 | Word / Excel / Outlook | Host kill 中の入力（C-010 相当） | 固まらず DegradedSimple で継続 |
| O-10 | Word / Excel / Outlook | Release 既定で本文がログ / 成果物に残らない（`observability-spec.md` §7.6） | 残らない |

recorder スクリプトは各アプリの対象行（O-01〜O-09 のうち対象列に該当する
もの）のキー列を記録・再生し、結果は目視 + screenshot で判定する。

### 13.3.2 アプリ別 workaround / 既知の差異

`full` 自動化が成立しない、または TSF 実装差で挙動が分かれるアプリの
既知差異と対処方針を固定する。実装（§13.4）の target JSON にこの方針を
反映する。

| アプリ / 種別 | 既知の差異 | workaround |
|---|---|---|
| Chrome / Edge（Chromium） | display attribute（下線）が完全反映されない場合がある | 位置取得は `ITfContextView::GetTextExt` を優先。下線は IME 既定描画にフォールバック |
| Firefox | TSF サポートが best-effort。`GetTextExt` が空矩形を返すことがある | 空矩形時は直近 caret rect のキャッシュへフォールバック |
| VS Code / Electron | Monaco が composition 中に独自補完を出す | preedit 中は IME 候補を優先し、Electron 側 IME イベント順序に依存しない |
| Discord / Slack（Electron） | `contenteditable` で `GetTextExt` 精度が低い | best-effort。位置ずれ時はキャレット位置 fallback |
| Word / Excel / Outlook | UI Automation TextPattern の信頼性が低い | recorder + §13.3.1 チェックリスト。Excel はセル編集と数式バーで context が切り替わる点に注意 |
| Windows Terminal / PowerShell | conhost 系で TSF level 3 非対応の場面がある | level 1（最小 composition）で確定を優先、候補位置はキャレット概算 |
| UWP / WinUI（Store apps） | AppContainer ACL、Windows アプリ互換カテゴリ、第三者 IME の署名がロード前提 | `ALL APPLICATION PACKAGES`（S-1-15-2-1）RX、`GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT` 登録、署名済み TIP を揃える（正典は `docs/msix-packaging-spec.md` §1.7）。本ハーネスは Windows Settings を `full`、その他 Store apps を `best-effort` とし、署名済み成果物の確認は Human Gate で行う |

### 13.4 実装

ハーネスは `compat-test/` に置く。ディレクトリ構成とファイルの役割は
`compat-test/README.md`「ディレクトリ構成」が正典である。

各 target JSON は AppId / window class / 自動化レベルを定義する。

**クリップボード安全化（C-011）**: C-011 は `Ctrl+V` 貼り付けの透過を検証する
ため、実機・self-hosted runner の実クリップボードに依存すると利用者の私的な
クリップボード内容を対象アプリに貼り付け、§13.5 の screenshot / log artifact
として保存してしまうおそれがある。ハーネスは **C-011 実行直前に決定的な
テストデータでクリップボードを置換し、実行後に元の内容を復元する**こと
（復元失敗時も元データを artifact に残さない）。**クリップボード退避は
M50 ゲート対象（Notepad / VS Code / Edge）では前提条件**であり、退避が
不可能な runner では C-011 を pass にできない（gate 未達の failing skip 扱い。
自動ゲートから silent に除外しない）。手動確認（§13.3.1 相当）への振り替えは
best-effort / 非ゲート対象に限る。

退避時は `EmptyClipboard` より前に全formatを列挙し、遅延レンダリングを含む各handleを
即時複製する。復元後は退避した全formatが存在し、比較可能な `HGLOBAL` データのsizeと
hashが一致することを確認する。複製または一致確認ができない場合は復元成功として扱わない。

C-007 の `KEYEVENTF_UNICODE` によるサロゲートペア注入はTSFを通らないため、対象アプリの
UTF-16保持確認に限定する。保持できても `surrogate-pair-tip-path-unverified` の
`failing-skip` とし、azooKey候補からの絵文字確定は `gate:human-required` の実機検証で
補完する。

C-010 はPowerShell supervisorが起動したHostだけを停止対象とし、再起動プロセスに加えて
per-user named pipeへの接続を確認してから復帰と判定する。runnerはHostを代替起動しない。
Notepad targetでは、再接続待ちを後続ケースへ波及させないためC-010を最後に実行する。
C-013 は同じ supervisor 配下の Host を終了させずに一時停止する。別プロセスの
`compat_host_hang_watchdog.exe` が再開を担当し、runner の異常終了か 8 秒の期限でも
再開する。検証 zip には runner と同じディレクトリに watchdog を同梱する。
停止中の入力継続と再開後の通常変換を UI Automation で確認し、TIP の構造化ログで
`Degraded` → `Ready` の遷移を確認する。対象アプリが runner 起動プロセス自身なら
ログ環境変数の継承を確認できるため、遷移の欠落を fail とする。既存プロセスへの
ウィンドウ委譲などでログ設定を確認できず、対象 PID の遷移が無い場合は failing-skip とし、
観測条件を満たせない状態を pass にしない。watchdog が期限切れで Host を再開した
場合は再開失敗と区別して期限切れとして fail にする。

runner / C-001〜C-019 / unit test はトップ `CMakeLists.txt` から
`add_subdirectory(compat-test)` で Windows ビルドへ配線する。

runner は起動前後のトップレベルウィンドウを比較し、起動後に増えた対象 class の
ウィンドウだけを runner 所有として扱う。
この判定では、`CreateProcessW` が返した PID のウィンドウを優先し、対象 executable と
同名の既存 process が新しいウィンドウを生成した場合も、その新規ウィンドウを選択する。
runner 所有の新規ウィンドウは `WM_CLOSE` の対象にできるが、既存 process 自体は終了しない。
一時 document を編集した場合は、保存結果を確認できたときだけ新規ウィンドウを閉じる。

Modern Notepad は設定に応じて、指定された一時 document を既存ウィンドウの新規 tab として
開く場合がある。
Notepad target は `use_temporary_document` と
`allow_reused_window_for_temporary_document` を有効にし、UI Automation の選択中 `TabItem`
または一意な一時ファイル名を含む対象ウィンドウ title で今回の document を識別する。
識別できた場合、runner が所有するのは一時 document の tab だけであり、既存ウィンドウと
既存 process は所有しない。
既存ウィンドウを再利用する場合、editor探索は非表示でない候補が一つに定まる場合だけ成功とする。
入力前後にも一時 document が選択中であることを確認し、別tabへ切り替わった場合は入力を中止する。
この非表示判定は既存ウィンドウ内のtab再利用に限り、EdgeとVS Codeを含む新規ウィンドウの
editor探索には適用しない。

終了時は一時 document が選択中であることを再確認する。
`Ctrl+S` の入力注入だけでは保存成功としない。
一時ファイルの更新時刻が変化し、内容が空になったことを確認できた場合だけ `Ctrl+W` でそのtabを閉じる。
再利用を許可する target は `save_before_close` を無効にできない。
`require_new_window` と既存ウィンドウ再利用を同時に指定したtarget設定は無効とする。
所有権、保存、tabの終了を確認できない場合は既存ウィンドウを閉じず、一時ファイルを残して
そのパスを診断へ出力する。

runner が `TerminateProcess` の対象にできるのは、自身の `CreateProcessW` が返した process
handle に限る。
対象ウィンドウの PID が起動 PID と異なる場合、その対象 process を終了してはならない。
対象 class のウィンドウが生成されなければ `new-target-window-not-found`、既存ウィンドウを
対象 executable の既存ウィンドウを検出しても一時 document の所有権を一意に証明できなければ
`target-document-ownership-not-established` の `failing-skip` とする。
回帰テストは「起動 process の新規ウィンドウ」「既存 process が生成した新規ウィンドウ」
「既存ウィンドウ内の新規 tab」「ウィンドウ未生成」を区別する。

Edge と VS Code は `Chrome_WidgetWin_1` の新規ウィンドウだけを操作する。
Edge は外部通信を行わない一時 HTML の textarea、VS Code は拡張機能を無効にした
一時テキストファイルを使う。
3 target とも C-013、C-010 の順に最後に実行し、Host の停止を後続ケースへ波及させない。

### 13.5 出力

```
compat-report-YYYYMMDD-HHMMSS/
├── report.md         # 人間向けサマリ
├── report.json       # CI artifact 用
├── screenshots/
│   └── notepad_C-001_fail.png
└── failures/
    └── notepad_C-001_fail/
        ├── failure.log    # 固定 reason code・矩形・本文を含まない読み取り診断値のみ
        └── screenshot.png
```

スクリーンショットは失敗時だけ生成する。デスクトップ画素は取得せず、空の画像に
対象ウィンドウ・キャレット・候補ウィンドウの矩形だけを描画する。
`report.md` は target 固有の HTML コメント marker、全体結果、件数表、折りたたみ可能な
case 詳細表を持ち、そのまま PR コメント本文として使える形式にする。

runner は `--cases C-001,C-004` で実行するケースを選び、`--skip C-010` で除外できる。
両方を指定した場合は、`--cases` の選択から `--skip` を除く。
形式が `C-NNN` でない ID、重複、target の `cases` に無い ID、空になる選択は終了コード
`64` で拒否し、出力ディレクトリを作らない。
実行順は選択の順ではなく target の `cases` の順に従い、C-010 を最後に置く順序を保つ。
C-001 を前提とするケースを選び C-001 を選ばなかった場合は、C-001 を前提として追加する。
`--skip C-001` で明示的に除外した場合は追加せず、前提を持つケースを実行せずに
`baseline-case-excluded` の `failing-skip` とする。
除外したケースは `results` と件数に含めず、終了コードは実行したケースだけで決まる。

`report.json` は `schema_version` 1 のまま、次の `case_selection` を持つ。

| フィールド | 内容 |
|---|---|
| `executed` | `results` に載るケース。target の順 |
| `excluded` | target の `cases` のうち実行しなかったケース。target の順 |
| `prerequisites_added` | 前提として追加したケース（C-001） |
| `baseline_case_excluded` | C-001 を除外したまま前提を持つケースを選んだ場合に `true` |

### 13.6 CI 連携

`.github/workflows/compat.yml` に optional な `compat` ジョブを置く。
この workflow は `compat-test` ラベル付き PR の open / reopen / synchronize / labeled
イベント、または手動 dispatch でだけ runner を起動する。
通常の PR ではジョブが skip されるため、Windows runner 時間を消費しない。

```yaml
jobs:
  compat:
    if: >-
      github.event_name == 'workflow_dispatch' ||
      contains(github.event.pull_request.labels.*.name, 'compat-test')
```

ジョブは Release の `compat_test` を一度だけビルドし、Notepad、VS Code、Edge の
3 target を順に実行する。
各 target の非 0 終了を記録しても残りの target を続行し、`compat-report-*/`、
`compat-run-*.log`、`compat-summary.json` を
`compat-report-<run-id>-<run-attempt>` artifact としてアップロードする。
実機条件を満たせない target は `failing-skip` を report に残す。

GitHub-hosted runner では azooKey TIP を登録も選択もしない。
このジョブは runner のビルド、対象アプリの起動、診断レポートの収集経路を確認するため、
target の非 0 終了だけではジョブを失敗させない。
ジョブが成功しても M50 の互換性テストが通ったことを意味せず、M50 ゲートは azooKey TIP を
登録・選択した対話環境の `report.json` で判定する。

### M50 受け入れ条件

受け入れ条件の定義の正典は [`plans/windows-port-roadmap.md`](../plans/windows-port-roadmap.md)
の M50 節とする。本書は §13 で対象アプリ・テストケース・レポート形式を定義し、受け入れ条件を複製しない。
