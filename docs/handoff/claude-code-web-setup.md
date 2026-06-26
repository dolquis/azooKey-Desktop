# Claude Code on the web（Cloud実行環境）セットアップ手順

このリポジトリを [Claude Code on the web](https://code.claude.com/docs/en/claude-code-on-the-web)（claude.ai/code の Ubuntu 24.04 サンドボックス）で動かすためのセットアップ。**既定では `.claude/settings.json` の SessionStart フックから自動実行**され、追加のWeb UI操作なしで Cloud セッションのツールチェーン導入とビルドが走る。

## 仕組み（既定: SessionStart フック方式）

`.claude/settings.json` の `SessionStart` フックが、セッション開始ごとに [`scripts/cloud-setup.sh`](../../scripts/cloud-setup.sh) を呼ぶ:

```jsonc
"hooks": {
  "SessionStart": [
    { "matcher": "startup|resume",
      "hooks": [ { "type": "command", "command": "bash scripts/cloud-setup.sh" } ] }
  ]
}
```

このフックは**ローカルCLIでも読み込まれて実行される**が、スクリプト冒頭の
`CLAUDE_CODE_REMOTE` ガードにより、**Cloud 以外では即座に no-op で抜ける**
（スキップ通知は stdout ではなく stderr に出すため、セッションのコンテキストを汚さない）。
Cloud（`CLAUDE_CODE_REMOTE=true`）でのみ apt 導入とビルドが走る。

| 設定 | 保存先 | ローカルCLI | Cloud |
|---|---|---|---|
| SessionStart フック（`.claude/settings.json`） | Git | ✅ 読み込む（ガードで no-op） | ✅ 実行 |
| [`scripts/cloud-setup.sh`](../../scripts/cloud-setup.sh) | Git | ガード経由で no-op | フックから実行され apt+ビルド |
| ネットワーク許可リスト | **Web コンソール**（Environment 設定） | ❌（ローカルに概念なし） | ✅ |
| `.mcp.json` / `CLAUDE.md` / `.claude/rules\|skills\|agents` | Git | ✅ | ✅ |

> ローカルでフックを動かすには `bash` が PATH 上にあること（Windows は Git Bash。
> 本リポジトリは Windows ビルドを Bash 経由で回す前提のため既に満たす）。`bash` が
> 無い環境では SessionStart フックが警告を出すが、フック失敗は非ブロッキングで
> セッションは続行する。

## 必要な手動設定（Web コンソール）

リポジトリ側は配線済みなので、Web コンソールでは**ネットワークだけ**設定する:

1. claude.ai/code でこのリポジトリの **Environment（環境）設定**を開く。
2. **Network access** を **Trusted**（既定の許可リスト。パッケージレジストリと GitHub を含む）にする。
   - apt / FetchContent が遮断される場合のみ、Custom 許可リストに以下を追加:
     ```
     archive.ubuntu.com
     security.ubuntu.com
     github.com
     objects.githubusercontent.com
     ```
   - GitHub の git 操作は専用プロキシ経由のため、Network access が None でも認証付きで動作する。

> **代替（Web UI Setup script 方式）**: 重い導入をセッション前に一度だけ走らせ
> キャッシュしたい場合は、SessionStart フックの代わりに Web コンソールの
> **Setup script** 欄へ `bash scripts/cloud-setup.sh` を貼ってもよい。その場合は
> `.claude/settings.json` の SessionStart フックを外す（二重実行を避ける）。

## Cloud でビルドされる範囲

`tsf-tip/` は `if(WIN32)` ガードで Linux では自動スキップされる。Cloud（Linux）でビルド・テストできるのは:

- `core/` `ipc/` `learning/` `inference-host/` `bench/` と各 `tests/`

これは `.github/workflows/windows.yml` の `linux-build` ジョブと同じ範囲で、CI と挙動が揃う。Windows 専用（TSF TIP・MSIX・register）の検証はローカル／実機で行う。

## ローカル/Cloud の判別

スクリプトやフックを条件分岐したいときは環境変数を使う:

```bash
if [ "${CLAUDE_CODE_REMOTE:-}" = "true" ]; then
  # Cloud セッションでのみ実行
fi
```

[`scripts/cloud-setup.sh`](../../scripts/cloud-setup.sh) はこのガードを内蔵しており、Cloud 以外で呼ばれても安全に no-op で抜ける。
