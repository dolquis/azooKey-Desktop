# 32 bit (x86) プロセスを MVP の入力対象から外す

- 種別: 決定記録
- 日付: 2026-08
- 関連 Issue: DEV-160
- 結論の正典: [`sideload-packaging-spec.md`](../sideload-packaging-spec.md) §0.1「32 bit (x86) プロセスを MVP 対象外とする理由」
- 移設元: `docs/sideload-packaging-spec.md` §0.1 の同名小節（commit `c8d9a5b` 時点）。原文をそのまま移設した

本文中の `§` は移設元の `docs/sideload-packaging-spec.md` の節を指す。

#### 32 bit (x86) プロセスを MVP 対象外とする理由（DEV-160 で確定 / 2026-08）

入力対象にはもう一つ、対象プロセスの bitness という境界がある。
Win32 か UWP かという上記の軸とは独立しており、MVP が保証するのは **Win32 デスクトップアプリであり、かつ x64 プロセス**である。
32 bit (x86) プロセスでの入力は v1.0 の対象外とし、既知の制限として扱う。

TSF と ctfmon は対象プロセスの bitness に一致する TIP をロードする。
本リポジトリのビルドは x64 のみであり、`tsf-tip/src/DllMain.cpp` の `DllRegisterServer` が書くのも 64 bit ビューの `HKLM\Software\Classes\CLSID` と、そこから `ITfInputProcessorProfiles` が登録する `CTF\TIP` だけである。
`scripts/register-dev.ps1` が呼ぶのも 64 bit の `regsvr32` である。
したがって 32 bit プロセスからは TIP が解決されず、azooKey を選べない。
古い Office や一部のレガシーアプリ、組み込みアプリがこれに該当する。

64 bit アプリ（Notepad、Chrome、VS Code、最新 Office、設定アプリ）は影響を受けない。
DEV-32 の実機検証動線も 64 bit アプリで成立するため、この制限は v1.0 のリリースゲートを塞がない。
`docs/handoff/dev32-verification-checklist.md` の B7 が 32 bit アプリを「既知(DEV-160)」として扱うのも、この境界に沿う。

将来 32 bit 対応を入れる場合に必要になる作業は次のとおりである。

* `tsf-tip/` に x86 ターゲットを追加し、x86 TIP DLL をビルドする
* 32 bit の `regsvr32` から `DllRegisterServer` を呼び、レジストリの 32 bit ビュー（`WOW6432Node`）へ CLSID と `CTF\TIP` を登録する
* §1.7 の `ALL APPLICATION PACKAGES` RX 付与規則を x86 DLL にも適用する（同節の付与対象は現時点で x64 TIP DLL 1 個を前提としている）
* MSIX（§1）と WiX/MSI（§4）へ x86 成果物を同梱し、アンインストール時に 32 bit ビューの残骸も消えることを確認する

再評価のトリガは 2 つある。
実利用者から 32 bit プロセスでの入力要望が挙がること、または M27（ARM64 ビルド）で複数アーキテクチャのビルドと登録の経路が整備され、x86 追加の限界コストが下がることである。
それまでは本節を既知の制限の正典とする。
