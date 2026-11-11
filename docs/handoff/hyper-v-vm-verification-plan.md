---
kind: runbook
canonical_for: ["VM 検証資産の一覧と、エージェントが Hyper-V VM 操作を補助する前提（ホスト側の権限）とリスク"]
---

# Hyper-V VM 実機確認の検証資産とエージェント補助の前提

Hyper-V の Windows 11 VM で azooKey TIP を実機確認するときの検証資産と、
Claude Code / Codex CLI が VM 操作を補助するときの前提とリスクをまとめる。

- VM 構成、3 層の検証フロー、エージェントへの委任の条件と範囲、登録と bootstrap の個別手順の正典は
  [`hyper-v-tip-verification.md`](./hyper-v-tip-verification.md)、
  チェック項目の正典は [`tip-manual-verification-checklist.md`](./tip-manual-verification-checklist.md) と
  `compat-test/` であり、本書はそれらを重複記述しない。
- 操作チャネル別の評価、スパイクの未確認点、ハーネスの比較、運用レベル（L0〜L2）の定義は、
  評価時点の記録として [`../archive/hyper-v-vm-agent-assist-evaluation.md`](../archive/hyper-v-vm-agent-assist-evaluation.md) にある。
- 進捗と状態の正典は Linear で、本書は状態を持たない。

## 1. 検証資産

コードベースには、VM 実機確認を支える資産が 4 系統ある。

| 資産 | 役割 | 正典 |
|---|---|---|
| `scripts/make-vm-verify-package.ps1` | ホスト側で Release 成果物を manifest 付き検証 zip にする | `docs/dev-infrastructure-spec.md` §2.6 |
| `verify-bootstrap.ps1`（zip 同梱） | VM 内での導入と事前確認。`-Json` で機械可読な結果を返す | 同上 |
| `compat_test.exe` と `compat_host_hang_watchdog.exe`（`compat-test/`） | Notepad、VS Code、Edge の compat ケース（範囲は `docs/dev-infrastructure-spec.md` §13.3）を UI Automation + SendInput で自動実行し、`report.json` を出す。watchdog は C-013 の Host 再開を担当する | `docs/dev-infrastructure-spec.md` §13 |
| `azookey_diag.exe` | 登録、pipe、ログディレクトリの診断と `--repair` | `docs/handoff/windows-diagnostics-playbook.md` |

手動確認の動線はチェックリスト（コア A1〜A8、拡張 B1〜B7）が定める。
これらの資産を組み合わせる 3 層の検証フローは [`hyper-v-tip-verification.md`](./hyper-v-tip-verification.md)「検証フロー（3 層）」が定める。

## 2. 推奨 VM 構成

[`hyper-v-tip-verification.md`](./hyper-v-tip-verification.md)「VM 構成」へ移した。

## 3. 推奨検証フロー（1 サイクル 3 層）

[`hyper-v-tip-verification.md`](./hyper-v-tip-verification.md)「検証フロー（3 層）」へ移した。

## 4. エージェント補助

### 4.1 前提: エージェントは Hyper-V ホストで動かす

VM 操作を補助できるのは、Claude Code または Codex CLI を Hyper-V ホストの Windows 上（または `powershell.exe` を呼べる WSL 上）で起動した場合に限る。
クラウド実行（Claude Code on the web など）はホストの Hyper-V に到達できないため、本節の対象外である。

起動場所に加えて、Hyper-V cmdlet を実行できる権限が要る。
エージェントのシェルは通常非昇格で起動されるため、実行ユーザーが `Hyper-V Administrators` グループに所属していない限り `Get-VM` はアクセス拒否になる（`Administrators` 所属でも、UAC が非昇格トークンから管理者権限を外すため）。
解消は次のどちらかで行う。

- **推奨**: 実行ユーザーを `Hyper-V Administrators` へ追加し、再ログオンする。
  非昇格シェルのまま `Get-VM` や `Checkpoint-VM` が通るため、エージェントへ管理者コンソールを常時渡さずに済み、§6 の方針（破壊的 cmdlet を許可リストに入れない）とも整合する。
- 代替: 管理者起動した PowerShell.MCP 共有コンソールを人間と共用する。
  管理者権限が要る操作を人間の実行判断に寄せたい場合はこちらを選ぶ。

### 4.2 操作チャネル別の評価

§4.2〜§4.4 は評価時点の記録として [`../archive/hyper-v-vm-agent-assist-evaluation.md`](../archive/hyper-v-vm-agent-assist-evaluation.md) へ移した。

### 4.3 スパイクで確認する未確認点

§4.2 と同じく archive へ移した。

### 4.4 Claude Code と Codex CLI の比較

§4.2 と同じく archive へ移した。

### 4.5 任せる作業と人間に残す作業

[`hyper-v-tip-verification.md`](./hyper-v-tip-verification.md)「エージェントへの委任」へ移した。VM 内の TIP 登録委任の条件と範囲の正典は同節である。

## 5. 運用レベルの定義

評価時点の記録として [`../archive/hyper-v-vm-agent-assist-evaluation.md`](../archive/hyper-v-vm-agent-assist-evaluation.md) へ移した。

## 6. リスクと制約

本節は [`hyper-v-tip-verification.md`](./hyper-v-tip-verification.md)「エージェントへの委任」から参照される。

- ゲスト資格情報をスクリプト、ログ、リポジトリ、コマンド履歴に残さない。`Get-Credential` で対話取得するか、SecretManagement モジュールで保管する。
- エージェントのシェルに Hyper-V 管理者権限を与えることになる。`Remove-VM` や `Remove-VMSnapshot` などの破壊的 cmdlet は許可リストに入れず、都度確認にする。
- 拡張セッションでは IME 検証をしない。打鍵注入も、拡張セッション経由になる経路（RDP ベース）を使わない。C-005 だけは限定例外とし、`hyper-v-tip-verification.md` が定める前提確認を満たした場合に認める。
- VM 内の machine-wide 登録は checkpoint 復元で巻き戻す。復元せずに検証を重ねると、前回の残骸が次の結果を汚す。
- 自動サインインとロック無効は検証専用 VM に限定し、ホストや常用環境に適用しない。
