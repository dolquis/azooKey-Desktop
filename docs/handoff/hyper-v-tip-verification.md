# Hyper-V Win11 VM での TIP 実機検証 runbook（DEV-32）

azooKey TIP（`tsf-tip`）と inference-host を Hyper-V の Windows 11 VM 上で実機検証するための手順。
最初の対象は **DEV-32「M2–M4 実機 Win11 動線検証（打鍵→preedit→候補→確定）」** だが、以後の TIP 実機検証にも再利用できる。

- 進捗・状態の正典は Linear（team `Dev` / Project `azooKey Desktop / Windows IME MVP`）。本 runbook は手順のみを扱い、達成状態は持たない。
- ビルド/登録の標準手順は `README.md`、登録の責務境界は `docs/sideload-packaging-spec.md` を参照。

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

繰り返しコード修正→再検証する場合は、代替として VM 内にビルド環境（VS2022 Build Tools + CMake + Ninja + git）を構築し VM 内ビルドする方式もある。

## ホスト側の準備

```powershell
# リポジトリ直下
cmake --preset windows-release -DAZOOKEY_FETCH_GOOGLETEST=ON
cmake --build --preset windows-release
```

VM へ渡す一式（`scripts/make-vm-verify-package`（任意）または手動で zip 化）:

| ファイル | 取得元 |
|---|---|
| `azookey_tsf_tip.dll` | `build/windows-release/tsf-tip/` |
| `azookey_inference_host.exe` | `build/windows-release/inference-host/` |
| `register.ps1` / `unregister.ps1` | `scripts/` |
| `vc_redist.x64.exe` | https://aka.ms/vs/17/release/vc_redist.x64.exe |

## 手順

### 1. チェックポイント（検証前スナップショット）

```powershell
# ホスト側
Checkpoint-VM -Name "<VM名>" -SnapshotName "pre-azookey-DEV-32"
```

### 2. VM へ転送（拡張セッション）

VMConnect を拡張セッションで開き、上記一式を VM の `C:\azookey-verify\` にコピーする
（拡張セッションが使えなければホストのフォルダ共有経由でも可）。

### 3. VC++ ランタイム導入（VM）

```powershell
C:\azookey-verify\vc_redist.x64.exe /install /quiet /norestart
```

### 4. 登録（VM・管理者 PowerShell）

```powershell
cd C:\azookey-verify
powershell -ExecutionPolicy Bypass -File .\register.ps1 `
  -TipDllPath .\azookey_tsf_tip.dll `
  -HostExePath .\azookey_inference_host.exe
```

- **`-TipDllPath` / `-HostExePath` の明示は必須**（スクリプト既定は `..\build\windows-debug\...` を指すため）。
- `register.ps1` の動作: 非昇格で host を HKCU `Run` に自動起動登録 + 現セッションで host 起動（per-user pipe `\\.\pipe\azookey-<SID>` を probe）→ 昇格して `regsvr32 /s`（`DllRegisterServer` = HKLM COM + TSF profile + keyboard/display-attribute/UI-element category 登録）。非管理者から実行すると UAC 自動昇格する。
- 「TSF TIP registration complete (machine-wide).」が出れば成功。
- host 稼働確認: `Get-Process azookey_inference_host`。出ない場合は `Start-Process .\azookey_inference_host.exe -ArgumentList "--pipe" -WindowStyle Hidden`。

### 5. 実機検証（★基本セッションに切替えて）

VMConnect を基本セッションに切替（拡張セッションをオフ）。メモ帳で DEV-32 チェックリスト:

1. **Win+Space** で **azooKey** を選択できる（DEV-157 修正で言語一覧に出る）
2. `ka` → 「か」がアンダーライン付き **preedit**（M3）
3. **Backspace** で1文字戻る / **ESC** で composition クリア（M3）
4. `nihongo` → **Space** で候補ウィンドウに「日本語」等（M4/M5）
5. **↑↓** 選択・**Enter/数字** で確定、確定テキストがアプリに入る（M5/M6）
6. 候補が出れば IPC 往復成立（= Host 由来）。詳細ログは VM に DebugView を入れると `IPC: connected to host ...` 等が見える

> モデル（gguf）は不要。CPU `SimpleConverter` で基本変換が出る（Zenzai 高品質変換のみ gguf 要、MVP 動線確認には不要）。

### 6. 記録と後始末

- **記録**: `winver` で OS ビルドを控え、手順・結果・スクショを **DEV-32 にコメント**（→ DEV-5 の human gate も同時クローズ可）。
- **後始末（どちらか）**:
  - クリーンに戻す（推奨）: チェックポイントに復元
    ```powershell
    Restore-VMSnapshot -VMName "<VM名>" -Name "pre-azookey-DEV-32" -Confirm:$false
    ```
  - スクリプトで解除:
    ```powershell
    Stop-Process -Name azookey_inference_host -Force -ErrorAction SilentlyContinue
    powershell -ExecutionPolicy Bypass -File .\unregister.ps1 -TipDllPath .\azookey_tsf_tip.dll
    ```
    `unregister.ps1` は HKCU の host 自動起動解除 + `regsvr32 /u`（`DllUnregisterServer`）+ HKLM CLSID / `CTF\TIP`（native / WOW6432Node 両方）の残骸削除を行う。

## トラブルシュート

| 症状 | 原因・対処 |
|---|---|
| azooKey が言語一覧に出ない | 管理者で登録したか。診断: `regsvr32 azookey_tsf_tip.dll`（`/s` 無し）。`0x8007007E`（モジュール無し）なら VC++ Redist 未導入（手順3）。 |
| preedit は出るが候補が出ない | host 未起動。`Get-Process azookey_inference_host` → 無ければ手動 `--pipe` 起動（手順4）。 |
| 入力が変・日本語に切替わらない | 拡張セッションのままになっている可能性 → 基本セッションへ（手順5）。 |
| 異常系で対象アプリが固まる | DEV-173 の残存（`tsf-tip/src/TextService.cpp:1001` の CommitObservation 応答待ちが無期限 `Receive()`）。正常系検証には影響なし。「Host 強制終了 → 即 IME 切替/アプリ終了」を叩く前に bounded 化すると安全。 |

## 技術根拠（確認済み）

- Release 成果物の実依存（`dumpbin /dependents`）: `VCRUNTIME140.dll`, `MSVCP140.dll`, `api-ms-win-crt-*`, `ole32`, `KERNEL32`, `USER32`。Debug CRT（`*d.dll`）依存なし。
- `windows-release` ビルド・`ctest`（157件中 156 passed / 1 skipped = 登録 smoke は管理者要）はホストで緑を確認済み。
- TIP CLSID: `{71EE04FA-B35D-4EB8-87A1-582D44A9A58C}`。
