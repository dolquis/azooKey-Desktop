# Windows TSF + Inference Host デバッグ

## Build

Windows (推奨):

```powershell
cmake --preset windows-debug -DAZOOKEY_FETCH_GOOGLETEST=ON
cmake --build --preset windows-debug
ctest --preset windows-debug --no-tests=error
```

Release 構成:

```powershell
cmake --preset windows-release -DAZOOKEY_FETCH_GOOGLETEST=ON
cmake --build --preset windows-release
ctest --preset windows-release --no-tests=error
```

Linux/macOS では `tsf-tip/` 配下は自動スキップ。`core/` `ipc/` `learning/`
`inference-host/` `bench/` のみ単体検証可能。

```bash
cmake --preset linux-debug -DAZOOKEY_FETCH_GOOGLETEST=ON
cmake --build --preset linux-debug
ctest --preset linux-debug --no-tests=error
```

## Bench

```powershell
./build/windows-debug/bench/azookey_bench.exe
```

出力例: `p50_ms=... p95_ms=... p99_ms=...`。`azookey_bench_smoke` は
CTest に登録され、p95 が 50ms 以上なら失敗する。

## 手動確認（Windows）

1. `build/windows-debug/inference-host/azookey_inference_host.exe --pipe` で Host を起動。
   pipe mode では `AZOOKEY_IPC_HANDSHAKE_TOKEN` / `--handshake-token` が
   設定されている場合だけ Handshake token を検証する。手動で token を使う場合は
   `AZOOKEY_IPC_HANDSHAKE_TOKEN=<token>` を Host / TIP の両プロセスに設定する。
   Host 側だけ `--handshake-token <token>` で指定してもよいが、TIP 側には同じ
   環境変数を渡す。
   - `--cpu` / `--backend cpu` を明示すれば CPU バックエンド（既定）。
   - `--mock-dict <path.tsv>` で固定辞書を追加可能。
   - `--learning <path.tsv>` / `--user-dict <path.json>` を指定すると明示パスを
     優先する。未指定時は `%LOCALAPPDATA%\azooKey\data\` 配下を使う。
2. **登録方法は 2 通り**:
   - `regsvr32 build/windows-debug/tsf-tip/azookey_tsf_tip.dll`（**管理者権限が必要**）で
     `DllRegisterServer` が machine-wide COM 登録（HKLM）+ TSF プロファイル登録
     （`RegisterProfile`）+ キーボード / DisplayAttribute / UIElement カテゴリ登録を行う。
   - `scripts/register.ps1` は上記 `regsvr32` 呼び出しに加えて Host EXE の
     Run キー登録（自動起動・HKCU）まで行う。非管理者で起動すると自動で UAC 昇格する。
     MSIX 化までは PS1 経由を推奨。
3. Notepad でローマ字入力しプレエディット表示を確認（アンダーライン付き）。
4. Space で候補、↑↓ で選択、Enter or 1〜9 で確定、Esc でキャンセル。
5. 候補確定後、同じ reading を再変換し、確定済み候補が上位に来る（学習効果）。
6. Chrome / VSCode / Office でも基本操作確認。

## ログ収集

現状:

- TIP 側: `OutputDebugStringA`（DebugView または WinDbg で観測）。
  - `[azooKey TIP] IPC: <msg>` フォーマット。
- Host 側: stderr。`--pipe` 起動時は `named pipe listening: <name>` 表示後に
  Dispatcher 経路を待ち受け。

予定 (Phase 4 / M11):

- TIP/Host とも `%LOCALAPPDATA%\azooKey\logs\tip.log` / `host.log` に
  JSON Lines 形式で出力。

## CI

`.github/workflows/windows.yml` で `windows-2022` + msvc-dev-cmd + Ninja:

1. Debug / Release matrix を `windows-debug` / `windows-release` preset で実行。
2. configure / build / CTest のログを config ごとに artifact 化。
3. Release の `.pdb` を artifact 化。
4. PR には config ごとの configure / build / test tail を github-script で自動コメント。
5. Linux Debug 補助ジョブで非 Windows target を `linux-debug` preset で検証。

ローカルで CI と同じ流れを再現:

```powershell
cmake --preset windows-debug -DAZOOKEY_FETCH_GOOGLETEST=ON
cmake --build --preset windows-debug
ctest --preset windows-debug --no-tests=error
```

## 典型トラブル

- **TIP は動くが候補が遅延**: Host 未起動 / 名前付きパイプ接続失敗を疑う。
  TIP は Activate 後 5 秒間（250ms slice）リトライする。
  Host stderr に `named pipe listening: \\.\pipe\azookey-host` が出ているか確認。
- **候補が反転する（古い候補が上書きされる）**: `ipc_pending_id_` の比較で
  staleness check しているはず（`tsf-tip/src/TextService.cpp:717`）。
  DebugView で `IPC: stale response for req_id=N, discarding` が出るか確認。
- **確定時に空文字が入る**: `shown_candidates_` がスナップショットされる前に
  TSF EditSession が拒否（lock denial）された可能性。
  DebugView で `[azooKey TIP]` のフォローログ確認。
- **`DllRegisterServer` 失敗（`SELFREG_E_CLASS`）**: 多くは **非管理者実行**が原因。
  machine-wide 登録は HKLM と CTF\TIP プロファイルに書くため昇格が必須。管理者
  PowerShell（または自動昇格した `register.ps1`）で再実行する。
- **学習が反映されない**: 未指定時は
  `%LOCALAPPDATA%\azooKey\data\learning.tsv`、`--learning` 指定時はその明示パスを確認。
  CommitObservation 受信は Host stderr / Dispatcher テストで確認。
- **学習暴走**: `learning_alpha` を下げる（既定 0.8）。`LearningStore::Reset` または
  `%LOCALAPPDATA%\azooKey\data\learning.tsv` を削除して再起動。
