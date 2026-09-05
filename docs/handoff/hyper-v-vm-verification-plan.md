# Hyper-V VM 実機確認の推奨プランとエージェント補助の評価

Hyper-V の Windows 11 VM で azooKey TIP を実機確認するときの推奨構成と検証フロー、
および Claude Code / Codex CLI が VM 操作を補助できる範囲の評価をまとめる。

- 登録と bootstrap の個別手順の正典は [`hyper-v-tip-verification.md`](./hyper-v-tip-verification.md)、
  チェック項目の正典は [`dev32-verification-checklist.md`](./dev32-verification-checklist.md) と
  `compat-test/` であり、本書はそれらを重複記述しない。
- 進捗と状態の正典は Linear で、本書は状態を持たない。
  導入作業を進める場合は Linear に起票して追跡する。

## 1. 現在の検証資産

コードベースには、VM 実機確認を支える資産が 4 系統ある。

| 資産 | 役割 | 正典 |
|---|---|---|
| `scripts/make-vm-verify-package.ps1` | ホスト側で Release 成果物を manifest 付き検証 zip にする | `docs/dev-infrastructure-spec.md` §2.6 |
| `verify-bootstrap.ps1`（zip 同梱） | VM 内での導入と事前確認。`-Json` で機械可読な結果を返す | 同上 |
| `compat_test.exe`（`compat-test/`） | Notepad、VS Code、Edge の C-001〜C-012 を UI Automation + SendInput で自動実行し、`report.json` を出す | `docs/dev-infrastructure-spec.md` §13 |
| `azookey_diag.exe` | 登録、pipe、ログディレクトリの診断と `--repair` | `docs/handoff/windows-diagnostics-playbook.md` |

手動確認の動線はチェックリスト（コア A1〜A8、拡張 B1〜B7）が定義済みである。
本プランは、これらの資産を後述の 3 層に組み合わせる。

## 2. 推奨 VM 構成

- **OS**: 配布対象に合わせた Windows 11 の最新 GA ビルド。検証記録に `winver` のビルド番号を残す。
- **世代とリソース**: 第 2 世代、4 vCPU、メモリ 8 GB を起点にする。GGUF を検証する場合はモデルサイズ分を加算する。
- **統合サービス**: PowerShell Direct（`vmicvmsession`）は既定で有効。`Copy-VMFile` を使う場合だけ Guest Service Interface を有効化する（既定無効）。
- **アカウント**: ローカル管理者を 1 つ用意する。ゲスト内自動化（§4）を使う VM に限り、自動サインインを設定し、画面ロックとスクリーンセーバーを無効にする（SendInput と UI Automation は対話セッションがロックされていると失敗する）。
- **ランタイム**: `vc_redist.x64.exe`（VC++ 2015-2022 Redistributable x64）を導入したベースラインを作る。
- **checkpoint 運用**: 「クリーン + Redistributable + 上記設定」のベースライン checkpoint を 1 つ維持し、各検証はその上に検証用 checkpoint を取ってから始める。検証後は復元して残骸を持ち越さない。
- **セッション種別**: ファイル転送は拡張セッション、IME 検証と打鍵は基本セッション（runbook の安全装置に従う）。C-005 に限る例外は [`hyper-v-tip-verification.md`](./hyper-v-tip-verification.md)「拡張セッションの限定例外（C-005）」が定める。
- **保険**: Microsoft IME を削除しない。

## 3. 推奨検証フロー（1 サイクル 3 層）

1 回の検証サイクルを、自動化できる層から人間にしか判断できない層へ 3 層に分ける。

| 層 | 内容 | 実行者 |
|---|---|---|
| **層 1: 自動先行検証** | Release ビルド → `make-vm-verify-package.ps1` → 検証用 checkpoint 取得 → zip 転送 → `verify-bootstrap.ps1 -Json` → `compat_test.exe`（3 target）→ `report.json` とログの回収 → 判定サマリ作成 | エージェント補助可（§4） |
| **層 2: 半自動打鍵確認** | 基本セッションでチェックリスト A1〜A8 と B 系を打鍵。ログとスクリーンショットの整理、チェックリスト下書きはエージェントが補助 | 人間 + エージェント |
| **層 3: 人間ゲート** | 候補ウィンドウの見え方、DPI、切替の体感などの視覚判断。チェックリスト確定、Linear への検証メモ、`gate:human-required` の Done 判定。最後に checkpoint 復元 | 人間 |

層 1 で `overallStatus=fail` または `compat_test` の新規 fail が出た場合は、層 2 に進まず修正へ戻る。
層 1 の pass は層 2 と層 3 の合格を意味しない（前段の検証成功を後段の成功に読み替えない）。

## 4. エージェント補助の評価

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

| チャネル | できること | 評価 | 制約 |
|---|---|---|---|
| Hyper-V cmdlet（`Get-VM`、`Checkpoint-VM`、`Restore-VMSnapshot`） | checkpoint の取得、復元、VM 状態確認 | ◎ 実用 | ホスト側で Hyper-V 管理者権限が要る |
| PowerShell Direct（`Invoke-Command -VMName`、`New-PSSession -VMName` + `Copy-Item -ToSession/-FromSession`） | ネットワーク設定に依存しないゲスト内コマンド実行と双方向ファイル転送 | ◎ 実用 | ホストで管理者権限、ゲスト資格情報が要る。セッションは非対話（§4.3） |
| `Copy-VMFile` | ホスト→ゲストの片方向ファイル転送 | ○ 代替 | Guest Service Interface が既定無効。PowerShell Direct の転送で足りる |
| `Msvm_Keyboard` WMI（`TypeText`、`TypeKey`、`TypeScancodes`） | 仮想キーボードデバイスへの打鍵注入。基本セッションの物理打鍵と同じ入力経路を通る | △ 実験 | `TypeText` は ASCII 512 文字上限。観察手段（下記）とペアでないと判定できない |
| VMConnect 基本セッション + ホスト UI 自動化（windows-mcp または computer-use MCP） | ホスト UI 越しの打鍵と、VM 画面のスクリーンショット取得。エージェントは画像を直接読める | △ 実験 | 入力が VMConnect ウィンドウへ期待どおり届くか未確認。windows-mcp が接続できない環境では、Claude Code の computer-use MCP が同経路の代替になる |
| ゲスト内 `compat_test.exe` | C-001〜C-012 の UI Automation 自動実行と機械可読レポート | ◎ 実用 | 対話セッション必須。PowerShell Direct のセッションは対話セッションではないため、対話ユーザーのスケジュールタスク経由で起動する |

結論として、補助は可能である。
実用度が高い順に、(1) ホスト側オーケストレーション（パッケージ生成、checkpoint 管理、転送、回収）、(2) PowerShell Direct + スケジュールタスクによる `verify-bootstrap.ps1` と `compat_test.exe` の実行、(3) スクリーンショット読解を組み合わせた半自動打鍵確認、の 3 段になる。
打鍵注入（`Msvm_Keyboard`）は PoC の価値はあるが、判定の信頼性が観察手段に依存するため、人間ゲートの代替にはしない。

### 4.3 スパイクで確認する未確認点

次の 4 点は文書と API 仕様から確定できないため、L1（§5）の導入前にスパイクで確認する。
各項目は Linear に個別課題として起票済みで、実施結果と判断は各課題側に残す。
L1 の成立可否を決める DEV-730 と DEV-731 を先に実施し、層 2 の補助範囲を決める DEV-732 と DEV-733 はその後でよい。

- 管理者資格の PowerShell Direct セッションで `verify-bootstrap.ps1` の管理者判定が真になるか（DEV-730）。
  bootstrap は非管理者のとき `Start-Process -Verb RunAs`（UAC ダイアログ）で昇格するが、非対話セッションでは同意ダイアログを表示できない。
  判定が真ならこの分岐を踏まず、そのまま実行できる。
- PowerShell Direct セッションで構成した per-user host pipe に、対話セッション側の TIP が接続できるか（DEV-731）。
  named pipe の名前空間はセッションをまたぐが、HKCU の自動起動設定が効くのは次回の対話ログオンからである。
- windows-mcp の入力送出が VMConnect 基本セッションウィンドウへ届くか（DEV-732）。
  ホスト UI 自動化の実装は windows-mcp に限らず、Claude Code の computer-use MCP でも同じ問いを検証できる。
- `Msvm_Keyboard` 注入時の候補ウィンドウ挙動を、スクリーンショットと画像読解だけで判定できるか（DEV-733）。

### 4.4 Claude Code と Codex CLI の比較

操作チャネルの実体はどちらも PowerShell であり、`.mcp.json` と `.codex/config.toml` は同じ `powershell` / `windows-mcp` MCP サーバーを定義している。
したがって層 1 の自動化はどちらのハーネスでも成立し、能力差は小さい。

検証補助に限れば Claude Code を推奨する。
理由は 2 つで、スクリーンショット画像を直接読解できること（層 2 の補助で効く）、および UAC や管理者 PowerShell を挟む往復を PowerShell.MCP の共有コンソールで人間と分担する運用が本リポジトリで整備済みであることによる。
役割分担の規約（Claude = 設計とレビュー、Codex = 実装）とも整合する。

### 4.5 任せる作業と人間に残す作業

| 任せられる（エージェント） | 人間に残す |
|---|---|
| 検証 zip の生成と鮮度確認 | VM 内での TIP 登録コマンドの実行判断（下記） |
| checkpoint の取得、存在確認、復元 | 候補ウィンドウの見え方、DPI、体感などの視覚判断 |
| zip 転送、ログと `report.json` の回収 | チェックリストの確定と合否判定 |
| `-Json` 結果と report の判定サマリ作成 | `gate:human-required` の Done 判定と Linear への検証メモ |
| チェックリスト下書き、Linear 起票の下書き | 規約改訂の判断（VM 内登録の委任可否を含む） |

現行規約（`AGENTS.md`、`azookey-packaging-release-workflow` スキル）は、TIP 登録をエージェントが単独で完了させることと、実機確認をエージェント単独で完了扱いにすることを禁じており、この文言は VM 内かどうかを区別していない。
VM 内の登録は checkpoint 復元で可逆なため委任の余地はあるが、それには `AGENTS.md` への例外追記（VM 内に限る委任）が要る。
追記するまでは、登録コマンドは人間が VM 内で実行し、エージェントは前後の確認と記録を担う。
いずれの場合も、エージェントの実行結果は「先行検証」であり、人間ゲートの合格そのものにはならない。

## 5. 運用レベルの定義

導入は次の 3 レベルで段階化する。
レベルは運用形態の定義であり、達成状態は Linear で追跡する。

どのレベルでも、初回に次のホスト側セットアップを済ませる（人間の一回作業）。

1. 実行ユーザーの `Hyper-V Administrators` への追加と再ログオン（§4.1）。
2. §2 の構成での VM 作成と、ベースライン checkpoint の取得。
3. ゲスト資格情報の準備（`Get-Credential` での対話取得、または SecretManagement。§6）。
4. エージェント側 MCP（`powershell` / `windows-mcp`）の接続確認。環境起因の失敗は `just doctor --fix-hints` で切り分ける。

- **L0 ホスト側支援**: 追加実装なしで今すぐ使える。層 1 のうちゲスト内実行を除く全部（パッケージ生成、checkpoint 管理、転送、回収、サマリ作成）と、層 2 のログ整理、起票支援。
- **L1 ゲスト内自動検証**: §4.3 のスパイクを先に潰した上で、PowerShell Direct 経由の bootstrap 実行と、対話セッションのスケジュールタスク経由の `compat_test.exe` 実行を自動化する。
- **L2 打鍵注入の実験**: `Msvm_Keyboard` または VMConnect + ホスト UI 自動化（windows-mcp / computer-use MCP）で A 系チェックの一部を半自動化する PoC。結果は参考情報にとどめ、人間ゲートを置き換えない。

## 6. リスクと制約

- ゲスト資格情報をスクリプト、ログ、リポジトリ、コマンド履歴に残さない。`Get-Credential` で対話取得するか、SecretManagement モジュールで保管する。
- エージェントのシェルに Hyper-V 管理者権限を与えることになる。`Remove-VM` や `Remove-VMSnapshot` などの破壊的 cmdlet は許可リストに入れず、都度確認にする。
- 拡張セッションでは IME 検証をしない。打鍵注入も、拡張セッション経由になる経路（RDP ベース）を使わない。C-005 だけは限定例外とし、`hyper-v-tip-verification.md` が定める前提確認を満たした場合に認める。
- VM 内の machine-wide 登録は checkpoint 復元で巻き戻す。復元せずに検証を重ねると、前回の残骸が次の結果を汚す。
- 自動サインインとロック無効は検証専用 VM に限定し、ホストや常用環境に適用しない。
