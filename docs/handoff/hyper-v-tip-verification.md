# Hyper-V Win11 VM での TIP 実機検証 runbook（DEV-32）

azooKey TIP（`tsf-tip`）と inference-host を Hyper-V の Windows 11 VM 上で実機検証するための手順。
最初の対象は **DEV-32「M2–M4 実機 Win11 動線検証（打鍵→preedit→候補→確定）」** だが、以後の TIP 実機検証にも再利用できる。

- 進捗・状態の正典は Linear（team `Dev` / Project `azooKey Desktop / Windows IME MVP`）。本 runbook は手順のみを扱い、達成状態は持たない。
- ビルド/登録の標準手順は `README.md`、登録の責務境界は `docs/sideload-packaging-spec.md` を参照。
- 検証サイクル全体の推奨構成と、エージェント（Claude Code / Codex）による VM 操作補助の評価は
  [`hyper-v-vm-verification-plan.md`](./hyper-v-vm-verification-plan.md) を参照。

## なぜ VM で検証するか

TIP は全プロセスにロードされる IME であり、machine-wide 登録（HKLM COM + TSF profile）を伴う。
不具合があるとホスト OS の文字入力に波及し得るため、隔離された VM で検証し、チェックポイントで即座に戻せるようにする。

## 安全装置（先に押さえる4点）

| ⚠️ | 要点 |
|---|---|
| **基本セッションで検証** | Hyper-V の拡張セッション（VMConnect = RDP ベース）では IME 入力がリダイレクトされ、VM 内 TIP の挙動が正しく見えないことがある。**ファイルコピーは拡張セッション、IME 検証は基本セッション**に切り替えて行う。 |
| **検証前にチェックポイント** | 登録で入力が壊れた場合に戻せるよう、**登録の直前にチェックポイントを取得**する。 |
| **VC++ 2015-2022 Redistributable (x64) 必須** | Release 版は `VCRUNTIME140.dll` / `MSVCP140.dll` に依存（`api-ms-win-crt-*` は OS 同梱の UCRT）。`vc_redist.x64.exe` を VM に導入すれば動く。 |
| **MS-IME を残す** | azooKey に切替後に不具合で戻れなくなった場合の保険。標準の Microsoft IME は削除しない。 |

## 成果物の持ち込み方式

**Release ビルドをコピーする方式**を採る。理由:

- Debug ビルド（`windows-debug`）は再頒布不可の Debug CRT（`vcruntime140d.dll` / `msvcp140d.dll` / `ucrtbased.dll`）に依存し、VS 未導入のクリーンな VM では動作しない。
- Release ビルド（`windows-release`）の依存は `VCRUNTIME140.dll` / `MSVCP140.dll` のみで、VC++ Redistributable (x64) で充足できる（配布形態に近い）。
- CMake は CRT を明示設定しておらず既定の動的 `/MD`（Release）/ `/MDd`（Debug）。

Release ビルドでも、`AZOOKEY_LOG=1` を設定すると TIP / Host の JSON Lines ログを取得できる。通常の一次調査ではこの方式を使い、VM 内への開発環境構築は不要である。デバッガ接続や修正後の再ビルドが必要な調査では、末尾の **付録: Debug ビルド + デバッガ方式** を使う。

## ホスト側の準備

```powershell
# リポジトリ直下
cmake --preset windows-release -DAZOOKEY_FETCH_GOOGLETEST=ON
cmake --build --preset windows-release

# VC++ Redistributable を同梱する場合だけ、ローカルの installer を明示する。
.\scripts\make-vm-verify-package.ps1 `
  -Preset windows-release `
  -OutputDirectory .\build\vm-verify-packages `
  -RuntimeInstallerPath C:\path\to\vc_redist.x64.exe
```

`make-vm-verify-package.ps1` は preset と CMake cache の build type を照合し、
Ninja dry-run で対象バイナリが最新と確認できた場合だけ zip を生成する。
成果物は `azookey-verify-<commit>-<preset>.zip` と同名の
`.manifest.json` で、zip 内にも `manifest.json` が入る。
出力先は worktree 外、または `.gitignore` 対象の `build/` 配下に置く。
それ以外のリポジトリ内ディレクトリへ出力すると、次回の clean 判定で成果物が
未追跡ファイルとして検出される。

`vc_redist.x64.exe` は自動取得しない。
クリーンな VM へ持ち込む場合は、Microsoft の配布元から別途取得した installer を
`-RuntimeInstallerPath` で明示する。
mock dictionary は `-MockDictionaryPath`、GGUF は `-ModelPath` で追加できる。
GGUF を指定する場合は llama.cpp 対応 preset をビルドしておく。
生成スクリプトは同じ build directory の `azookey_zenzai_bench.exe` も収集し、
VM 側の bootstrap は manifest から GGUF、bench、mock dictionary を自動検出する。
いずれもローカルファイルだけを収集し、ネットワークには接続しない。

## 手順

### 1. チェックポイントと VM への転送（ホスト側・1 コマンド）

```powershell
# ホスト側。VM は起動しておく。
.\scripts\vm-verify-session.ps1 -Prepare -VMName "<VM名>"
```

チェックポイント取得と zip の転送を通しで行い、転送先の guest パスを出力する。

* **チェックポイント名は自動で決まる**。パッケージの `manifest.json` にある commit と
  preset から `pre-azookey-<preset>-<commit12>` を生成するため、同じパッケージで
  取り直しても同じ名前を指し、別コミットの検証と取り違えない。名前を明示したい場合は
  `-CheckpointName` を渡す。
* パッケージは既定で `build\vm-verify-packages` の最新を選ぶ。明示するなら `-PackagePath`。
* 転送先は既定で `C:\azookey-verify`。変えるなら `-GuestDestination`。
* **同名のチェックポイントが既にある場合は停止する**。前回の検証で VM が汚れている
  可能性があり、その状態を「検証前」として保存すると基準を失うため。`-Restore` で
  戻すか、`Remove-VMSnapshot` で消してからやり直す。

転送は `Copy-VMFile` を使うため、VM 側で **Guest Service Interface** が有効である必要が
ある。無効な場合はスクリプトが有効化コマンドと代替経路を案内して**非ゼロ終了する**
（黙って転送をスキップしない）。

### 2. 転送できない場合の代替経路

Guest Service Interface が使えない VM では、手順 1 が停止する。その場合は zip を手で
持ち込む。

* VMConnect を拡張セッションで開き、生成した zip を VM の `C:\azookey-verify\` へコピーする
* または、ホストのフォルダ共有経由でコピーする

いずれの場合も、チェックポイントが取得済みかを
`Get-VMSnapshot -VMName "<VM名>"` で確認する。取得されていなければ手で取る。

### 3. bootstrap と事前確認（VM・対話ユーザーの PowerShell）

転送した zip を guest 側で展開してから実行する。手順 1 の出力も、この後の入力検証を
**基本セッション**で行うことを再掲する（切り替えは対話操作のため自動化しない）。

```powershell
cd C:\azookey-verify
powershell -ExecutionPolicy Bypass -File .\verify-bootstrap.ps1 `
  -CheckpointConfirmed

# 機械可読な結果が必要な場合
powershell -ExecutionPolicy Bypass -File .\verify-bootstrap.ps1 `
  -Json `
  -CheckpointConfirmed
```

- bootstrap は同梱時だけ VC++ Redistributable を
  `/install /quiet /norestart` で導入し、TIP 登録と per-user host pipe を確認する。
- bootstrap 自体は対話ユーザーの通常 PowerShell で実行する。
  VC++ Redistributable と machine-wide TIP 登録が必要な箇所だけ UAC で昇格するため、
  HKCU の自動起動設定と per-user pipe は対話ユーザーに紐づく。
- 期待する DLL が登録済みなら `register-dev.ps1` を再実行しない。
  pipe が存在する場合は、`GetNamedPipeServerProcessId` で serving 中 Host の PID を特定し、
  その実行ファイルパスと SHA-256 を同梱 Host および `manifest.json` の
  `inference-host` と照合する。両方が一致すれば
  既存 Host を再利用し、
  不一致なら既存 supervisor と Host を停止して同梱 Host で再起動する。
  `-Json` の `hostBinary.status` は `reused` / `restarted` / `started` /
  `unverified` / `not_applicable` を返す。実行ファイルパスや hash を取得できない場合は
  黙って再利用せず warning とし、`hostBinary.reason` に理由を記録する。
- manifest に mock dictionary または GGUF が含まれる場合は bootstrap が自動で host
  引数へ渡す。別ファイルを使う場合だけ `-MockDictionaryPath` または `-ModelPath` を
  明示する。
- `overallStatus=fail` は実機検証へ進まない。
  `warning` は checkpoint または DebugView の手動確認が残っている状態である。
- `-CheckpointConfirmed` は checkpoint を作成するオプションではない。
  ホスト側で取得済みと確認した事実だけを bootstrap へ渡す。

### 4. 実機検証（★基本セッションに切替えて）

VMConnect を基本セッションに切替（拡張セッションをオフ）。メモ帳で DEV-32 チェックリスト:

1. **Win+Space** で **azooKey** を選択できる（DEV-157 修正で言語一覧に出る）
2. `ka` → 「か」がアンダーライン付き **preedit**（M3）
3. **Backspace** で1文字戻る / **ESC** で composition クリア（M3）
4. `watashi`（組込辞書語）→ **Space** で「私」等の漢字候補（M4/M5）。※ `にほんご` 等の辞書外語は `--mock-dict` か学習が無いと漢字化されない（下記 ⚠️ 参照）
5. **↑↓** 選択・**Enter/数字** で確定、確定テキストがアプリに入る（M5/M6）
6. 候補が出れば IPC 往復成立（= Host 由来）。詳細ログが必要な場合は、検証前に次を実行してサインアウト／サインインし、Host と検証対象アプリを新しい環境で起動する
   ```powershell
   [Environment]::SetEnvironmentVariable('AZOOKEY_LOG', '1', 'User')
   [Environment]::SetEnvironmentVariable('AZOOKEY_LOG_LEVEL', 'info', 'User')
   ```
   ログは `%LOCALAPPDATA%\azooKey\logs\tip-YYYYMMDD.jsonl` と
   `host-YYYYMMDD.jsonl` に出力される。取得後は環境変数を削除し、Host と対象アプリを
   再起動する。

> ⚠️ **変換能力の前提（重要）**: 現状 Zenzai 推論は未実装（`ZenzaiModelConverter` は gguf を probe するのみで `SimpleConverter` へ委譲。DEV-190）。そのため **辞書外の語は漢字に変換されない**（SimpleConverter の静的辞書＝わたし/にほん/とうきょう 等＋学習語のみ）。一般のかな漢字変換を確認するには、パッケージ生成時に `-MockDictionaryPath <TSV>` を指定するか学習済み語を使う。実機検証 DEV-32 でも「にほんご」（辞書外語）が漢字化されないことを確認済み（DEV-190。チェックリストでは任意の A5-opt で確認し、コア A5 は組込辞書語 `watashi` で評価する）。Zenzai 推論本体は M8/M9 系で未完。

### 5. 記録と後始末

- **記録**: `winver` で OS ビルドを控え、手順・結果・スクショを **DEV-32 にコメント**（→ DEV-5 の human gate も同時クローズ可）。
- **後始末（どちらか）**:
  - クリーンに戻す（推奨）: チェックポイントに復元
    ```powershell
    # ホスト側。手順 1 と同じ規則でチェックポイント名を解決する。
    .\scripts\vm-verify-session.ps1 -Restore -VMName "<VM名>"
    ```
    対象のチェックポイントが存在しない場合は、名前を表示して非ゼロ終了する。
    別名で取った場合は `-CheckpointName` を渡す。
  - スクリプトで解除:
    ```powershell
    Stop-Process -Name azookey_inference_host -Force -ErrorAction SilentlyContinue
    powershell -ExecutionPolicy Bypass -File .\unregister-dev.ps1 -TipDllPath .\azookey_tsf_tip.dll
    ```
    `unregister-dev.ps1` は HKCU の host 自動起動解除 + `regsvr32 /u`（`DllUnregisterServer`）+ HKLM CLSID / `CTF\TIP`（native / WOW6432Node 両方）の残骸削除を行う。

## トラブルシュート

| 症状 | 原因・対処 |
|---|---|
| azooKey が言語一覧に出ない | `azookey_diag.exe --json` で D-001〜D-003 を確認する。`error` の場合は管理者 PowerShell で `azookey_diag.exe --repair` を実行する。`permission_denied` は非昇格で実行した状態、互換 DLL が見つからない場合は `register-dev.ps1 -TipDllPath <path>` または MSIX 修復が必要な状態を示す。`--repair` は AppContainer ACL を付与しないため、UWP または Microsoft Store アプリを検証する前に `register-dev.ps1 -TipDllPath <path>` を実行する。 |
| preedit は出るが候補が出ない | host 未起動。`Get-Process azookey_inference_host` → 無ければ手動 `--pipe` 起動（手順4）。 |
| `logs/` が作成されない、または診断 ZIP にログがない | `azookey_diag.exe --json` で D-013 を確認する。`error` の場合は `azookey_diag.exe --repair` で `%LOCALAPPDATA%\azooKey\logs\` を作成し、再診断結果を確認する。 |
| 入力が変・日本語に切替わらない | 拡張セッションのままになっている可能性 → 基本セッションへ（手順5）。 |
| 異常系で対象アプリが固まる | DEV-173 の残存（`tsf-tip/src/TextService.cpp:1001` の CommitObservation 応答待ちが無期限 `Receive()`）。正常系検証には影響なし。「Host 強制終了 → 即 IME 切替/アプリ終了」を叩く前に bounded 化すると安全。 |

## 付録: Debug ビルド + デバッガ方式（VM に開発環境がある場合）

VM に開発環境（VS2022・CMake・Ninja・Windows SDK・git）が揃っている場合は、リポジトリを取得して
**Debug ビルド**を行い、デバッガ接続や修正後の再ビルドを VM 内で繰り返せる。
Release の opt-in ファイルログで不足する場合に使う。

### ログの出力先

| 対象 | 出力先 | ビルド依存 | 取得方法 |
|---|---|---|---|
| TIP / Host | `%LOCALAPPDATA%\azooKey\logs\*-YYYYMMDD.jsonl` | Release / Debug、`AZOOKEY_LOG=1` のときだけ | ファイルを回収 |
| TIP（`tsf-tip`） | `OutputDebugStringA`（`[azooKey TIP] <event>`） | Debug のみ | DebugView でキャプチャ |
| host（`inference-host`） | `std::cerr`（info/warn/error）+ `std::cout`（IPC 応答 JSON） | 常時 | コンソール起動 |

TIP のイベント例: `ipc_connected` / `ipc_handshake_rejected` /
`ipc_query_timeout` / `ipc_worker_stopped`。

### 手順

0. 検証前にチェックポイント取得。コマンドは **"Developer PowerShell for VS 2022"** から実行（`cl` / `ninja` / `cmake` が PATH に乗る）。

1. Git 取得（private リポジトリなら `gh auth login` か PAT で認証）:
   ```powershell
   cd C:\dev
   git clone https://github.com/dolquis/azooKey-Desktop.git
   cd azooKey-Desktop
   git checkout <検証したいブランチ>
   ```

2. Debug ビルド（Debug CRT は VS が供給するため Redist 不要）:
   ```powershell
   cmake --preset windows-debug -DAZOOKEY_FETCH_GOOGLETEST=ON
   cmake --build --preset windows-debug
   ```

3. host を**コンソールで**起動（ログを見るため。このウィンドウは閉じない）:
   ```powershell
   .\build\windows-debug\inference-host\azookey_inference_host.exe --pipe --backend cpu
   # 永続ログも欲しい場合: ... --pipe --backend cpu 2> host.log
   ```

4. TIP 登録（別の管理者 PowerShell）:
   ```powershell
   cd C:\dev\azooKey-Desktop
   .\scripts\register-dev.ps1
   ```
   - **Debug 方式は `register-dev.ps1` の既定パスがそのまま `build\windows-debug\…` を指す**ため、`-TipDllPath` / `-HostExePath` の明示は不要（Release 方式と対照的）。
   - 手順3で host を先に起動済みなら、`register-dev.ps1` は per-user pipe を probe して既起動を検知し、二重起動しない。

5. TIP ログを DebugView でキャプチャ:
   - VM に [DebugView](https://learn.microsoft.com/sysinternals/downloads/debugview) を入れ、**管理者で起動 → Capture → "Capture Global Win32"**（TIP は各アプリのプロセス内で動くため Global 推奨）。
   - フィルタに `[azooKey TIP]`。

6. 検証（★基本セッション）: DEV-32 チェックリストを実施し、DebugView（TIP）とコンソール（host）を突き合わせて IPC 往復を確認。

7. 後始末: `Stop-Process -Name azookey_inference_host -Force` → `.\scripts\unregister-dev.ps1` → またはチェックポイント復元。

### Release 方式との使い分け

| | Debug + 開発環境 | Release + zip コピー |
|---|---|---|
| TIP / Host ファイルログ | opt-inで出る | opt-inで出る |
| DebugView | TIPイベントが出る | 出ない |
| host stderr | 出る | 出る |
| デバッガ・再ビルド | 回しやすい | ホスト再ビルド要 |
| 開発環境 | 必要 | 不要（Redist のみ） |
| 向き | デバッガを使う調査 | 配布形態の動作確認と一次調査 |

`OutputDebugString` はファイルに残らない。永続ログは `AZOOKEY_LOG=1` で取得する。

## 技術根拠（確認済み）

- Release 成果物の実依存（`dumpbin /dependents`）: `VCRUNTIME140.dll`, `MSVCP140.dll`, `api-ms-win-crt-*`, `ole32`, `KERNEL32`, `USER32`。Debug CRT（`*d.dll`）依存なし。
- `windows-release` ビルド・`ctest`（157件中 156 passed / 1 skipped = 登録 smoke は管理者要）はホストで緑を確認済み。
- TIP CLSID: `{71EE04FA-B35D-4EB8-87A1-582D44A9A58C}`。
