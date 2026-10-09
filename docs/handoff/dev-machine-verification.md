# 実機検証の環境振り分けと開発機での実施手順

`gate:human-required` の実機検証を、checkpoint で巻き戻せる Hyper-V VM で行うか、Windows の開発機（普段使いの物理機）で行うかを決める基準と、開発機で行うときの前提・手順・後始末を定める。

- VM での手順の正典は [`hyper-v-tip-verification.md`](./hyper-v-tip-verification.md)、VM セッションでの一括消化は [`human-gate-batch-runbook.md`](./human-gate-batch-runbook.md) であり、本書はそれらを再掲しない。
- どの課題をどちらで行うかは、Linear のラベル `env:vm` / `env:dev-machine` が正典である。ラベルの定義は [`../linear-conventions.md`](../linear-conventions.md) §13 にある。本書は判定基準だけを持ち、課題ごとの割り当てと状態を持たない。
- 進捗と合否の正典は Linear の各課題である。

## 1. 振り分けの原則

迷ったら VM で行う。
開発機は checkpoint で戻せないため、壊したときに戻す手段が登録解除と手作業の片付けしかない。
VM なら失敗しても復元で済む。
開発機を選ぶのは、課題のすべての手順が §2 のどれにも当たらない場合に限る。

課題に、実施環境についての人の判断が記録されていれば、それに従う。

1 つの課題の中に VM を要する手順が 1 つでもあれば、その課題全体を `env:vm` とする。
開発機でできる手順だけを先に済ませたい場合は、課題を分割してから行う（分割の規格は [`../linear-conventions.md`](../linear-conventions.md) §7.1）。

実機検証でない人間ゲート（方針判断、リリース操作、GitHub 設定、別ハードウェアでの計測）にはどちらのラベルも付けない。

## 2. VM で行う条件

次のどれかに当たる手順を含む課題は `env:vm` とする。

| 記号 | 条件 | 理由 |
|---|---|---|
| V1 | TIP の COM / TSF 登録を、通常の登録と解除以外の方法で変える（失敗注入、部分登録、ロールバック、残骸の確認） | HKLM に途中状態が残ったときに、登録解除で戻る保証がない |
| V2 | MSI / MSIX の導入、修復、アップグレード、アンインストールを行う。またはクリーン OS や VC++ Redistributable 未導入を前提とする | machine-wide に配置と登録を行い、再起動を要求しうる。クリーン前提は開発機では作れない |
| V3 | OS 全体の診断・検証機構を設定する（Application Verifier と Image File Execution Options、ETW manifest の登録、WPR の採取） | 解除し忘れると、以後の開発機のすべてのプロセスが影響を受ける |
| V4 | OS ユーザーの追加、別ユーザーでのサインイン、ログオン経路の観測を行う | 開発機のアカウント構成を変える |
| V5 | 学習データ、ユーザー辞書、資格情報を不可逆に変える（平文ストアから DPAPI 形式への移行、破損させた fixture の配置、手動で解除するまで残る SafeMode の誘発） | 開発機の実データは checkpoint で戻せない |
| V6 | `compat_test.exe` による自動打鍵（SendInput）を走らせる | 実行中は対話セッションを占有し、開発機で開いている別のウィンドウへ打鍵が届きうる |

## 3. 開発機で行える条件

§2 のどれにも当たらず、次の範囲に収まる課題は `env:dev-machine` としてよい。

- TIP の登録は、人が `register-dev.ps1` で行う開発登録だけを使う。
- 操作は、打鍵と目視、設定アプリでの保存、`settings.json` の可逆な書き換え、azooKey の Host と supervisor の停止・一時停止・再起動、`AZOOKEY_LOG` のログと `azookey_diag.exe` の出力の採取にとどまる。ETW の採取は V3 に当たる。
- 表示スケールの変更と再サインイン、マルチディスプレイ、物理 JIS キーボード、VS Code や Office での確認を含んでよい。これらは開発機のほうが条件をそろえやすい。
- TIP を登録しない Windows CLI / コンソールの検証は、合成データを別のデータルートに置いて実データと分けるなら、開発機で行ってよい。この場合 §5 の手順 1・2 と §6 の手順 1・2 は要らない。

## 4. 開発機での前提

`AGENTS.md`「最優先の安全規則」のとおり、エージェントが TIP を登録してよいのは検証専用の VM 内だけである（[`hyper-v-vm-verification-plan.md`](./hyper-v-vm-verification-plan.md) §4.5）。
開発機では、登録、解除、Host の停止などの操作はすべて人が実行する。
エージェントが受け持つのは、ログの整理、結果の照合、検証メモの下書きである。

始める前に次をそろえる。

1. **Microsoft IME を残す。** azooKey で入力できなくなったときの戻り先になる。
2. **秘密情報を azooKey で入力しない。** secure 入力の抑止そのものを確かめる課題でも、実在しないダミーの文字列だけを使う。パスワードや API キーは Microsoft IME か直接入力で入れる。
3. **成果物を Release 構成にそろえる。** [`hyper-v-tip-verification.md`](./hyper-v-tip-verification.md)「ホスト側の準備」と同じ手順で検証 zip を作り、開発機の作業ディレクトリへ展開する。課題が求める commit から作り、検証メモに commit を記録する。
4. **既存の登録を確かめる。** 普段の開発で `build\` 配下の TIP を登録している場合は、`unregister-dev.ps1` で先に解除する。2 つの登録を重ねると、どちらの DLL が効いているか判別できない（[`parallel-worktree-runbook.md`](./parallel-worktree-runbook.md)）。
5. **ユーザーデータを退避する。** `%LOCALAPPDATA%\azooKey` があれば、フォルダごと別の場所へコピーする。展開した検証 zip の `learning-data-snapshot.ps1 -Label before` で、開始時の状態も記録しておく。記録は作業ディレクトリの `learning-data-snapshots.json` に追記されるので、§6 でも同じディレクトリから実行する。

## 5. 開発機での手順

1. 人が、展開した検証 zip で `register-dev.ps1 -TipDllPath .\azookey_tsf_tip.dll -HostExePath .\azookey_inference_host.exe` を実行する。実モデルを使う課題では `-ModelPath` も渡す。
2. 展開した zip の `azookey_diag.exe --json` で D-001〜D-003 を確認する。`error` があれば打鍵に進まない。
3. 課題の確認手順に従って操作する。ログを採る場合は [`../debugging.md`](../debugging.md)「ログ収集」に従う。打鍵と目視の観点は [`dev32-verification-checklist.md`](./dev32-verification-checklist.md) を使う。
4. 次のどれかが起きたら直ちに中止し、Microsoft IME に切り替えてから §6 の後始末を行う。
   - 対象アプリ以外の入力が乱れる。
   - アプリが固まって入力を受け付けない。
   - Explorer やログオン画面に影響が出る。

   その課題は `env:vm` に付け替え、起きた事実を課題に記録する。

## 6. 後始末

1. 人が、展開した zip の `unregister-dev.ps1 -TipDllPath .\azookey_tsf_tip.dll` で解除する。
2. `azookey_diag.exe --json` で登録の残骸が無いことを確かめる。
3. `learning-data-snapshot.ps1 -Label after` を記録し、`-From before -To after` で学習データの変化を見る。
4. 課題の手順が書き込んだユーザーデータは、§4 で退避したコピーから元に戻す。普段の開発で使う登録があれば、元のとおり登録し直す。

## 7. 記録

検証メモの環境欄には「開発機（物理）」と書き、VM での結果と区別する。
OS のビルド番号、検証 zip の commit、azooKey と Microsoft IME 以外に入っている IME も書く。
メモの様式は [`human-gate-batch-runbook.md`](./human-gate-batch-runbook.md) Part C を使う。
ユーザー名を含むローカルの絶対パス、入力した本文、秘密情報は書かない。
