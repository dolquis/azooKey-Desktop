# 複数 worktree での並行作業 runbook

本書は、同じマシン上の複数の git worktree で同時に build、test、PR 作業を進めるときの恒常 runbook である。
対象は、管制塔セッションが子セッションを立てる形と、Claude Code の `isolation: worktree` で subagent を分ける形の両方である（`docs/handoff/agent-orchestration.md` の L4）。
誰が何を担当するかは `agent-orchestration.md` が持ち、本書は worktree をまたいで共有されてしまう資源と、その回避策だけを持つ。
ビルドとテストの標準手順は `README.md`、停止時の切り分けは `docs/debugging.md` が正典である。

## worktree ごとに分かれるもの

- build ディレクトリ。`CMakePresets.json` の `binaryDir` は `${sourceDir}/build/...` なので、worktree ごとに別の build ディレクトリになる。そのぶん、各 worktree で configure と full build をやり直す。
- submodule。`git worktree add` は submodule を初期化しないため、新しい worktree では `third_party/wil` が未初期化になる。README のとおり `git submodule update --init third_party/wil` で初期化するか、configure に `-DAZOOKEY_FETCH_WIL=ON` を付ける。
- FetchContent の取得物。リポジトリは `FETCHCONTENT_BASE_DIR` を設定していないので、`-DAZOOKEY_FETCH_GOOGLETEST=ON` などの取得は build ディレクトリごとにダウンロードし直す。テストを登録させるには、GoogleTest が入っていない環境ではこのフラグが要る（README「ビルド & テスト」）。
- テストが使う名前付きパイプと ETW セッション。名前にプロセス ID または時刻値を含むため、並行実行で衝突しない。
- COM 登録スモーク（`tsf-tip/tests/com_smoke_test.cpp`）。環境変数 `AZOOKEY_RUN_REGISTRATION_SMOKE` と昇格の両方がそろわないと SKIP するので、通常の並行 CTest では登録に触れない。
- Serena。`.serena/project.yml` が `.claude/worktrees` を除外しており、worktree ごとに別 project として扱う。

## worktree をまたいで共有されるもの

| 資源 | 何が起きるか | 回避策 |
|---|---|---|
| sccache のサーバとキャッシュ | 既定（`CMakeLists.txt` の `AZOOKEY_USE_COMPILER_CACHE=ON`）では全 worktree が同じキャッシュを使う。別 checkout のヘッダを指す依存が返り、ヘッダ変更が再コンパイルを起こさないことがある。仕組みと確認方法は `docs/debugging.md` の sccache の節 | `-DAZOOKEY_USE_COMPILER_CACHE=OFF` で configure する。使い続けるなら `SCCACHE_DIR` とサーバのポート（`SCCACHE_SERVER_PORT`）を worktree ごとに分ける |
| `%TEMP%` 配下の固定名ディレクトリ | 多くのテストが `std::filesystem::temp_directory_path()` の下に固定名のディレクトリを作り、作る前に `remove_all` する（例: `core/tests/runtime_logger_test.cpp` の `TestDirectory`）。並行する CTest が互いのファイルを消す | CTest を起動する前に、`TMP` と `TEMP` を worktree ごとのディレクトリへ向ける |
| ミューテックス `Local\azooKey-runtime-log-<component>` | `core/src/RuntimeLogger.cpp` が作る。`Local\` はログオンセッション単位なので、worktree をまたいで同じものになる。CTest の `RESOURCE_LOCK azookey-runtime-log` は 1 回の `ctest` の中でしか効かない | `RESOURCE_LOCK azookey-runtime-log` を持つログ系テストは、worktree をまたいで直列に流す |
| CPU とディスク | 同時ビルドの負荷で、待ち時間に上限を持つテストが偽の失敗になりうる。例は `ipc/tests/named_pipe_transport_test.cpp` の経過時間の上限アサート、inference-host の engine テストのロード期限、bench の smoke の `--max-p95-ms` | `AZOOKEY_CTEST_PARALLEL_JOBS`（`azookey_check` の並列数）や `ctest --parallel` を下げる。時間で落ちたテストは、他の build が止まった状態で単独に再実行してから判断する |

## マシン全体で 1 つしかないもの

次の資源は worktree の数によらず 1 つで、並行作業の対象にしない。

- 開発版 TIP の登録（HKLM の CLSID と `CTF\TIP`、HKCU の Run）。TIP 登録は Human Gate であり、エージェントはホスト上で実行しない（検証専用 Hyper-V VM 内の例外を含め `AGENTS.md`「最優先の安全規則」に従う）。
- 常駐 Host の既定パイプ `\\.\pipe\azookey-<SID>`（`ipc/src/NamedPipeTransport.cpp`）と、`%LOCALAPPDATA%\azooKey\` のユーザーデータ。worktree の build から Host を手で起動して確かめるときは、`--pipe-name` と `--data-root` を指定して常駐 Host と実データから切り離す。
- compat-test の runner。実アプリを操作するため、同時に 1 つだけ走らせる。

## CodeGraph

`.codegraph/` の index は本体の checkout にだけある。worktree から CodeGraph を呼ぶと本体 checkout の index が返り、worktree の branch での変更は反映されない。
worktree での調査では CodeGraph の結果を正とせず、`rg` と実ファイルで確かめる（`docs/handoff/agent-tooling-setup.md` の「別 checkout の index を対象ソースの代用にしない」）。index の作成はユーザーが判断する。

## マージ衝突と main の取り込み

`main` の branch ruleset は `Require branches to be up to date` を要求しない（`docs/dev-infrastructure-spec.md`「ブランチ保護と必須チェック」）。
そのため、並行する PR は古い `main` のまま CI を通ってマージされうる。
後続の PR は、先行 PR がマージされるたびに `origin/main` を merge で取り込み、`scripts/check_test_inventory.py` と docs-lint を通し直す。
push 済みの branch は rebase しない。rebase すると force push が要るからである。

並行する PR で衝突しやすいファイルは次のとおり。同じ時期に 2 つの塊へ割り当てないか、割り当てるなら取り込み時の手動解消を見込む。

- `docs/test-inventory.md`。テストや workflow job を足す PR のほとんどが行を追加する。
- `tsf-tip/src/TextService.cpp` と `TextService.h`、`tsf-tip/tests/onkeydown_preedit_test.cpp`。
- `inference-host/src/Dispatcher.cpp` と `inference-host/src/InferenceEngine.cpp`。
- `.github/workflows/windows.yml`。job を足すときは集約 job `ci-gate` の `needs` にも加える。`scripts/check_test_inventory.py` は job ごとの inventory 行を検査するが、`ci-gate` の `needs` への追加漏れは検査しないので、取り込み後に目で確かめる。

## worktree の片付け

子セッションの worktree は `.claude/worktrees/` の下に作られ、PR のマージ後も locked のまま build ディレクトリごと残る。
削除はユーザーの承認を得てから行い、エージェントが判断して `git worktree remove` や `--force` を使わない。

## 権限まわりの変更

`.claude/settings*.json`、`.claude/hooks/`、権限設定の変更は、子セッションや subagent に任せない。
ハーネスがエージェント自身による権限の書き換えを拒否することがあり、迂回すると安全境界が崩れる。
必要になったら作業を止め、ユーザー（管制塔経由の場合は管制塔）に判断を返す。
