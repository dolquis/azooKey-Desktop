---
name: azookey-observability-diagnostics
description: azooKey Desktop の構造化ログ（RuntimeLogger、AZOOKEY_LOG）、ETW（EtwLogger、AzooKey.man、wprp）、redaction、crash 収集と dump 保持（CrashReporting、CrashRetention）、diagnostics/ の azookey_diag CLI（--json / --repair / --collect）、QueryDiagnostics IPC、Windows 診断 playbook を変更、レビュー、利用するときに使う。「ログを増やしたい」「crash を採取したい」「Host が固まる原因を調べたい」のような依頼でも、ログ・ETW・診断成果物に触れるなら必ずこのスキルを使う。
---

# azooKey 可観測性と診断

ログと診断は、ユーザーの入力本文を扱う IME で最も漏洩しやすい経路である。
このスキルは「何を記録するか」より先に「何を記録してはいけないか」を確認する。

## 手順

1. `references/spec-routing.md` を読み、対象がランタイムログ、ETW、crash、診断 CLI の
   どれかを決め、対応する仕様の節と実装を確認する。
2. ツール選択・初期化は AGENTS.md「調査と実装」に従い、ログ呼び出し元と redaction の
   通過点（`core/src/Redaction.cpp`、各 logger の payload 構築）を参照元まで確認する。
3. 記録するフィールドごとに、入力本文・候補本文・prompt・window title・ユーザーパス・
   credential のいずれかを含みうるかを判定し、含みうるものは伏せ字化か削除にする。
4. 最小差分で実装し、schema を固定するテスト（JSON 行の形、ETW payload の固定長）を更新する。
5. 対象テストを実行し、Host の `QueryDiagnostics` 応答に触れたら `host_dispatcher_tests` へ広げる。
6. ログ形式、保持期間、環境変数、CLI の出力 schema が変わる場合は
   `docs/dev-infrastructure-spec.md` §7 / §12 と `docs/debugging.md` を同じ変更で更新する。

## 必須ガードレール

- ETW には文字列を載せない。`EtwLogger` に string を取る overload を足さない。
- Release ビルドのランタイムログに入力本文、候補本文、`prompt`、`window_title` の生値を出さない。
  redaction ポリシーの正典は `docs/dev-infrastructure-spec.md` §7.6 と
  `docs/privacy-and-secure-input-spec.md` §8 である。
- ログは環境変数 `AZOOKEY_LOG` の明示 opt-in でだけ出力し、既定ではファイルを作らない。
  出力先、ローテーション（5 MiB・3 世代・7 日）は `docs/debugging.md`「ログ収集」が定義する。
- `CrashReporting` を TIP DLL に組み込まない。同意（`CrashConsent`）が Off なら何も書かない。
- `PruneCrashDumps` は命名規則に合う dmp だけを管理し、symlink を辿らず、ディレクトリを作らない。
- secure input 中はログ・ETW・診断 zip に入力の痕跡を残さない。
- `azookey_diag --collect` の zip に含めるファイルは spec §12 の一覧に限定し、
  ランタイムログはバイト上限で切る。`--repair` は冪等に保つ。
- 診断や採取の手順で管理者権限、実機、WPR / ProcDump の実行が要る部分は Human Gate として
  分け、エージェント単独で完了扱いにしない。

## 完了条件

- 追加・変更したフィールドについて、機微情報を含みうるかの判定と対処を報告する。
- schema 固定テスト、redaction テスト、保持ポリシーのテストの実行結果を報告する。
- 実機採取（`docs/handoff/windows-diagnostics-playbook.md`）が必要な場合は、その旨と
  未実施の範囲を明記する。
