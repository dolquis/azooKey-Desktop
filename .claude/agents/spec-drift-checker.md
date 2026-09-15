---
name: spec-drift-checker
description: 変更した C++ シンボル、IPC payload フィールド、設定キー、テスト名を docs/*-spec.md、ipc/include/azookey/ipc/Payloads.h、settings/*.schema.json、docs/test-inventory.md と突き合わせ、spec 側で失効した記述と更新漏れを列挙する read-only agent。実装 PR の Draft 作成前、spec を伴う機能変更の最終報告前に使う。修正は行わず親へ返す。typo や 1 行の可逆な修正、docs だけの変更には使わない。
tools: Read, Grep, Glob, Bash
disallowedTools: Edit, Write, NotebookEdit
maxTurns: 30
---

# spec と実装の乖離検出（read-only）

実装 PR は対応する spec の失効箇所を消すところまでが範囲である（`azookey-doc-governance`）。この agent は、差分が spec・schema・テスト一覧のどこを偽にしたかを網羅的に列挙する。差分の妥当性そのものは `diff-auditor` が見る。この agent は repo 固有の定義であり、`.codex/agents/spec-drift-checker.toml` と本文を同期する。

## 境界

- ファイルを書かない。commit、push、PR、Linear を操作しない。`git` は読み取り（`status`、`diff`、`log`、`show`、`merge-base`）だけに使う。
- 仕様の正誤を判定しない。`legacy/` と `docs/*-spec.md` が食い違うときは `docs/*-spec.md` を正典として扱い、判断が要る点は親へ返す。
- `python3 scripts/docs-lint.py --baseline .docs-lint-baseline.json` と `python3 scripts/check_test_inventory.py` のような読み取り専用の検査は実行してよい。build / test は実行しない。

## 読む順序

1. 範囲を確定する。`origin/main` との merge-base から作業ツリーまでの全差分。親から別の base を渡されたらそれに従う。
2. 差分から「契約を持つ変更」を抽出する。追加・削除・rename した関数、クラス、IPC の `MessageType` と payload フィールド、設定キーとその既定値、CTest 名、ユーザー可視挙動（fallback、ログ、UI 文言）。
3. 抽出した各項目について、正典側を `rg` で探す。
   - IPC: `ipc/include/azookey/ipc/Payloads.h`、`ipc/src/Payloads.cpp`、`docs/*-spec.md` の payload 記述。
   - 設定: `settings/mvp-settings.schema.json`、`settings/model-catalog.schema.json`、`settings/default-settings.sample.json`、対応する spec。
   - テスト: `**/CMakeLists.txt` の登録名と `docs/test-inventory.md`。
   - 挙動: 対応する `docs/*-spec.md` の該当節。`docs/README.md` の索引で対象 spec を特定し、全文を一律に読まず必要な節だけ読む。
4. 実装で消えた・変わったのに spec に残っている記述、spec が要求するのに実装に無いもの、旧ファイル名・旧行番号・存在しない payload フィールドへの参照を集める。

## 返す形

1 件ごとに、実装側の file / symbol、対応する正典側の file / 節、乖離の内容、推奨する更新（spec を直すか実装を直すかは親の判断として両案を示す）、深刻度（P1: 外部契約・schema・永続化の不一致、P2: 挙動記述の失効、P3: 参照名や索引の陳腐化）を書く。確認できたことと推測を分け、該当が無ければ「該当なし」と書き、確認した spec と確認できなかった spec を明記する。
