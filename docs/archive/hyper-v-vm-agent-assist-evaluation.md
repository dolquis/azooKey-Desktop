# Hyper-V VM 実機確認のエージェント補助の評価

> **アーカイブ済み**: `docs/handoff/hyper-v-vm-verification-plan.md` の §4.2〜§4.4（操作チャネル別の評価、スパイクで確認する未確認点、Claude Code と Codex CLI の比較）と §5（運用レベルの定義）を、評価時点の記録として移した。以後は更新しない。
> 節番号と本文中の § 参照は移設元のものである。移設元の §2・§3・§4.5 は `docs/handoff/hyper-v-tip-verification.md`「VM 構成」「検証フロー（3 層）」「エージェントへの委任」に、§4.1・§6 は `docs/handoff/hyper-v-vm-verification-plan.md` にある。

## 4. エージェント補助の評価

### 4.2 操作チャネル別の評価

| チャネル | できること | 評価 | 制約 |
|---|---|---|---|
| Hyper-V cmdlet（`Get-VM`、`Checkpoint-VM`、`Restore-VMSnapshot`） | checkpoint の取得、復元、VM 状態確認 | ◎ 実用 | ホスト側で Hyper-V 管理者権限が要る |
| PowerShell Direct（`Invoke-Command -VMName`、`New-PSSession -VMName` + `Copy-Item -ToSession/-FromSession`） | ネットワーク設定に依存しないゲスト内コマンド実行と双方向ファイル転送 | ◎ 実用 | ホストで管理者権限、ゲスト資格情報が要る。セッションは非対話（§4.3） |
| `Copy-VMFile` | ホスト→ゲストの片方向ファイル転送 | ◎ 実用 | Guest Service Interface が要る（既定無効、§2）。`vm-verify-session.ps1 -Prepare` の転送経路 |
| `Msvm_Keyboard` WMI（`TypeText`、`TypeKey`、`TypeScancodes`） | 仮想キーボードデバイスへの打鍵注入。基本セッションの物理打鍵と同じ入力経路を通る | △ 実験 | `TypeText` は ASCII 512 文字上限。観察手段（下記）とペアでないと判定できない |
| VMConnect 基本セッション + ホスト UI 自動化（windows-mcp または computer-use MCP） | ホスト UI 越しの打鍵と、VM 画面のスクリーンショット取得。エージェントは画像を直接読める | △ 実験 | 入力が VMConnect ウィンドウへ期待どおり届くか未確認。windows-mcp が接続できない環境では、Claude Code の computer-use MCP が同経路の代替になる |
| ゲスト内 `compat_test.exe` | compat ケースの UI Automation 自動実行と機械可読レポート | ◎ 実用 | 対話セッション必須。PowerShell Direct のセッションは対話セッションではないため、対話ユーザーのスケジュールタスク経由で起動する |

結論として、補助は可能である。
実用度が高い順に、(1) ホスト側オーケストレーション（パッケージ生成、checkpoint 管理、転送、回収）、(2) PowerShell Direct + スケジュールタスクによる `verify-bootstrap.ps1` と `compat_test.exe` の実行、(3) スクリーンショット読解を組み合わせた半自動打鍵確認、の 3 段になる。
打鍵注入（`Msvm_Keyboard`）は PoC の価値はあるが、判定の信頼性が観察手段に依存するため、人間ゲートの代替にはしない。

### 4.3 スパイクで確認する未確認点

次の 4 点は文書と API 仕様から確定できないため、スパイクで確認する対象として定義した。
各項目は Linear の個別課題であり、実施結果と判断は各課題側に残す。
L1 の成立可否を決めるのは DEV-730 と DEV-731 である。
層 2 の補助範囲を決める DEV-732 と DEV-733 は、後述「L2 スパイクを再実施しない理由と再評価の条件」の条件が成立するまで実施しない。

- 管理者資格の PowerShell Direct セッションで `verify-bootstrap.ps1` の管理者判定が真になるか（DEV-730）。
  bootstrap は非管理者のとき `Start-Process -Verb RunAs`（UAC ダイアログ）で昇格するが、非対話セッションでは同意ダイアログを表示できない。
  判定が真ならこの分岐を踏まず、そのまま実行できる。
- PowerShell Direct セッションで構成した per-user host pipe に、対話セッション側の TIP が接続できるか（DEV-731）。
  named pipe の名前空間はセッションをまたぐが、HKCU の自動起動設定が効くのは次回の対話ログオンからである。
- windows-mcp の入力送出が VMConnect 基本セッションウィンドウへ届くか（DEV-732）。
  ホスト UI 自動化の実装は windows-mcp に限らず、Claude Code の computer-use MCP でも同じ問いを検証できる。
- `Msvm_Keyboard` 注入時の候補ウィンドウ挙動を、スクリーンショットと画像読解だけで判定できるか（DEV-733）。

#### L2 スパイクを再実施しない理由と再評価の条件

DEV-732 と DEV-733 の前提は、層 2 の打鍵をゲストの外から送り、結果を VM 画面のスクリーンショットで判定するしかない、というものである。
この前提は次の 2 点で成り立たなくなった。

- L1 のゲスト内自動検証（`scripts/vm-verify-session.ps1 -Run`）により、`compat_test.exe` がゲストの対話セッションで UI Automation と SendInput を使って compat ケースを実行する。打鍵結果を UI Automation でテキストとして読むため、スクリーンショットの画像読解より観測が確実である。
- [`tip-manual-verification-checklist.md`](../handoff/tip-manual-verification-checklist.md)「compat runner との分担」が、A 系・B 系の各行を機械判定、人間判定、補助に分けている。

ホスト側の打鍵手段（windows-mcp、computer-use MCP、`Msvm_Keyboard`）は、§4.2 が両スパイクの前提として挙げたものと同じであり、判断を変える差分にならない。

L2 の打鍵注入が代替しうるのは、分担表で人間判定または補助に残る行のうち、視覚や体感を要さず打鍵で駆動できる部分に限られる（A3 の追加の綴り、A6 の選択移動、A7 の数字キー確定、B2 の早打ちなど）。
これらはゲスト内の compat ケースとして追加すれば UI Automation で判定でき、ゲスト外の打鍵とスクリーンショットによる判定より信頼性が高い。
候補ウィンドウの見え方、下線、DPI、体感は、どちらの経路でも人間が判断する。
したがって L2 のスパイクは効率化の手段としてゲスト内 compat ケースの追加に劣り、再実施しない。

次のいずれかが成立したら、この判断を再評価する。

- 打鍵で駆動でき、かつゲスト内の UI Automation では観測できない行が、分担表の人間判定または補助に見つかった。
- SendInput の合成入力と、仮想キーボードデバイス経由の入力とで、TIP の挙動が異なる回帰が見つかった。この場合は `Msvm_Keyboard` 経由の打鍵に固有の価値が生じる。
- ゲスト内のスケジュールタスクから compat を実行できない VM 構成（コンソールの対話サインインを用意できない VM など）が検証の標準になった。

再評価で実施すると決めた場合も、判定基準は DEV-732 と DEV-733 の記述を土台にし、結果は参考情報にとどめて人間ゲートを置き換えない（§4.2 の結論）。

### 4.4 Claude Code と Codex CLI の比較

操作チャネルの実体はどちらも PowerShell であり、`.mcp.json` と `.codex/config.toml` は同じ `powershell` / `windows-mcp` MCP サーバーを定義している。
したがって層 1 の自動化はどちらのハーネスでも成立し、能力差は小さい。

検証補助に限れば Claude Code を推奨する。
理由は 2 つで、スクリーンショット画像を直接読解できること（層 2 の補助で効く）、および UAC や管理者 PowerShell を挟む往復を PowerShell.MCP の共有コンソールで人間と分担する運用が本リポジトリで整備済みであることによる。
役割分担の規約（Claude = 設計とレビュー、Codex = 実装）とも整合する。

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
- **L2 打鍵注入の実験**: `Msvm_Keyboard` または VMConnect + ホスト UI 自動化（windows-mcp / computer-use MCP）で A 系チェックの一部を半自動化する PoC。結果は参考情報にとどめ、人間ゲートを置き換えない。着手の可否は §4.3「L2 スパイクを再実施しない理由と再評価の条件」に従う。
