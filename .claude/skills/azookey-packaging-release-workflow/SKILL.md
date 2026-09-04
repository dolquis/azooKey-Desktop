---
name: azookey-packaging-release-workflow
description: azooKey Desktop の MSI、WiX、MSIX、AppxManifest、external-location identity package、コード署名、証明書、TIP 登録、VM 検証、配布成果物、GitHub Release workflow を変更、レビュー、デバッグするときに使う。配布チャネルを区別し、秘密鍵と Publisher identity を保護し、自動検証と管理者権限、実機確認の人間ゲートを分離する。
---

# azooKey 配布とリリース運用

配布チャネルを混同せず、パッケージ生成、署名、登録、検証、リリースまでを一つの変更経路として扱う。

## 手順

1. `references/spec-routing.md` を読み、対象を MVP 直接配布 MSI、Microsoft Store MSIX、スタンドアロン MSIX サイドロードのいずれかに分類する。
2. 対象チャネルの正典、実装、既存テストを確認し、未検証 PoC と canonical 経路を区別する。
3. C++ の package identity、TIP 登録、AppContainer 境界へ影響する場合は、`.codegraph/` があり CodeGraph が利用可能なら経路を確認する。Serena が利用可能なら active project と languages を確認してから対象シンボルと参照元を確認し、利用できない場合は `rg` と実ファイルで同じ範囲を確認する。
4. Context-Mode が利用可能なら、XML、PowerShell、workflow、長い diff やログを整理する。利用できない場合は必要な範囲を絞って確認する。
5. 変更に最も近い静的整合テストと PowerShell 品質検証を実行し、必要な場合だけ MSI / MSIX 生成、署名、VM smoke へ検証を広げる。
6. Windows CMake / Ninja / MSVC の実ビルドは Windows Headless CMake Build 手順に従う。
7. Microsoft の API、manifest、署名要件は Microsoft Learn の現行資料で確認する。
8. 配布形式、署名、更新、登録 lifecycle、観測、ユーザー可視挙動が変わる場合は、`docs/sideload-packaging-spec.md` と関連文書を同期する。

## 必須ガードレール

- `docs/sideload-packaging-spec.md` §0 の配布方針を先に確認し、MSI、Store MSIX、スタンドアロン MSIX の要件を混同しない。
- `.pfx`、秘密鍵、証明書パスワード、token をリポジトリ、ログ、コマンド履歴、PR 本文へ含めない。
- MSIX の `Identity@Publisher` と署名証明書 Subject の一致を確認し、実在しない Publisher や証明書値を推測で設定しない。
- machine-wide TIP 登録、証明書ストア変更、署名済み package の登録、IME 実入力、アンインストール確認は、明示された管理者権限、実機確認、人間ゲートとして扱う。
- 管理者権限や実機を要する検証を、エージェント単独で完了扱いにしない。
- Pester の静的整合テスト、package 生成、署名、インストール、TIP 実動作を別の検証段階として報告する。
- 前段の検証成功を、後段の検証成功に読み替えない。
- `pkg/msix/Package.wapproj` や未検証 MSIX 経路を、仕様と実機検証なしに canonical または production-ready と扱わない。
- `gate:human-required` 課題は検証メモ前に Done とせず、GitHub 参照でも自動 close を避ける。
- package に同梱するバイナリ、VC runtime、ライセンス、manifest、version、architecture の整合を確認する。

## 完了条件

- 対象チャネルと canonical 経路を明記する。
- 実施した静的テスト、PowerShell 品質検証、実ビルド、package 生成、署名、登録、VM / 実機確認を段階別に報告する。
- 未実施の管理者権限、実機確認、Store 提出検証と、その影響範囲を明記する。
- Documentation impact を判定し、更新しなかった場合も理由を示す。
