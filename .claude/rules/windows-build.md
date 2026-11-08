---
paths:
  - ".github/workflows/benchmarks.yml"
  - ".github/workflows/compat.yml"
  - ".github/workflows/release.yml"
  - ".github/workflows/sanitizers.yml"
  - ".github/workflows/windows*.yml"
  - "CMakeLists.txt"
  - "**/CMakeLists.txt"
  - "CMakePresets.json"
  - "**/*.cmake"
  - "cmake/**"
  - "bench/**"
  - "compat-test/**"
  - "core/**"
  - "diagnostics/**"
  - "inference-host/**"
  - "ipc/**"
  - "learning/**"
  - "pkg/**"
  - "settings/**"
  - "settings-app/**"
  - "scripts/**/*.ps1"
  - "tsf-tip/**"
  - "tests/**"
  - "justfile"
---

# Windows ビルド

- Windows CMake / Ninja / MSVC の実ビルドと実テストは、README の preset と利用可能な Windows ホスト実行経路を使う。
- Full CTest は `cmake --build --preset windows-debug --target azookey_check` を優先する。詳細と停止時の切り分けは `README.md` と `docs/debugging.md` を参照する。
- `.ninja_lock` は関連する `ninja`、`cmake`、`cl`、`link`、`ctest` のプロセスを確認するまで削除しない。
- `CreateProcessAsUserW failed: 5` は、ツールが存在しないと断定する前に最小 probe を適切な Windows ホスト実行経路で再確認する。
- TIP 登録と Human Gate の扱いは `AGENTS.md`「最優先の安全規則」に従う。
- エージェント用 MCP、プラグイン、ホスト前提、doctor は `docs/handoff/agent-tooling-setup.md` を参照する。
