---
paths:
  - ".github/workflows/windows*.yml"
  - "CMakeLists.txt"
  - "**/CMakeLists.txt"
  - "CMakePresets.json"
  - "**/*.cmake"
  - "cmake/**"
  - "core/**"
  - "inference-host/**"
  - "ipc/**"
  - "learning/**"
  - "settings/**"
  - "scripts/**/*.ps1"
  - "tsf-tip/**"
  - "tests/**"
  - "justfile"
---

# Windows ビルド

- README の preset と Windows ホスト実行経路を使い、Full CTest は `azookey_check` を優先する。
- `.ninja_lock` は関連プロセスを確認するまで削除しない。
- TIP 登録、実機確認、管理者権限、署名などの Human Gate を CI 成功で代替しない。
