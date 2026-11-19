# M56 / M57 の ONNX 推論ランタイムの取り込み方式

- 種別: 決定記録
- 日付: 2026-11-20（人間 lead が管制塔経由で承認した日）
- 関連 Issue: DEV-2146（設計）。関連: DEV-1768・DEV-1769・DEV-1720（R2）、DEV-98（R2 = Windows ML）、DEV-112（M56 の embedding）
- 結論の正典: [`neural-reranker-spec.md`](../neural-reranker-spec.md) §7.3（ランタイム、依存、配布、読み込み、fallback、BackendSelector、ライセンス、opset）、[`modernbert-ja-scoring-spec.md`](../modernbert-ja-scoring-spec.md) §6.3（M57 の実行 EP と CPU EP の計測ゲート）、[`plans/windows-port-roadmap.md`](../../plans/windows-port-roadmap.md) M57 の前提
- 移設元: なし（新規）

## 背景

M56 の `tiny_reranker.onnx` と M57 の `modernbert-ja-70m.onnx` は ONNX を前提にしていたが、repo には ONNX Runtime（ORT）の依存が無かった。
Zenzai の R2（ONNX Runtime GenAI + Windows ML）は、R1 と同じモデルを R2 でも使えるという再開条件が成り立っておらず止まっている（DEV-1768・1769・1720 は Backlog）。
M57 spec は backend を R2 `winml` と R1 `cuda` / `vulkan` に限り、CPU を「実用上時間に合わない」としていたが、この主張は計測を引いていなかった。

## 決めるときに確かめた事実

確認済み:

- MSI が同梱する第三者の DLL は MSVC runtime（`msvcp140.dll` など。`sideload-packaging-spec.md` §4.1）だけで、推論ライブラリの DLL は無かった。llama.cpp は `BUILD_SHARED_LIBS=OFF` で Host に静的リンクしている。
- `docs/build-ci-spec.md` §3.1 は、prebuilt バイナリを取得元の固定と SHA256 の pin で扱う。vcpkg は必須にしない。
- DirectML は maintenance mode で、新しい op や最適化は足されない（Microsoft Learn）。
- Windows ML は ORT のコピーを持つ。2026-10-26 の `Microsoft.Windows.AI.MachineLearning` 2.5.96 は ORT 1.28.3 を含む。CPU EP と DirectML EP は全版で使えるが、NPU / GPU の EP は Windows 11 24H2 以降にカタログから取得する。self-contained の構成は約 41 MB（`onnxruntime.dll` 約 20 MB と `DirectML.dll` 約 20 MB）。Windows App SDK の bootstrap が要る（Microsoft Learn）。
- NuGet の `Microsoft.ML.OnnxRuntime`（CPU）は Windows ARM64 に対応している（onnxruntime.ai）。
- ORT は、ORT の release が実装する ONNX の opset まで読める（onnxruntime.ai の compatibility）。
- 開発機（Windows build 26300）の `C:\Windows\System32\onnxruntime.dll` は版 1.17（10.6 MB）だった。名前だけで読み込むと、OS のこの DLL を取り違えうる。
- ORT に Vulkan の EP は無い。R1 の CUDA の実リンクは中止している（[`2026-08-04-cuda-real-link-cancellation.md`](2026-08-04-cuda-real-link-cancellation.md)）。

確認していない事実:

- GitHub release の win-arm64 の zip の有無（無ければ、ARM64 の取得元は NuGet の `Microsoft.ML.OnnxRuntime` の nupkg の URL にする。取得元は取り込む PR で確かめて固定する）
- 素の ORT の `onnxruntime.dll`（CPU 版）の実サイズ
- CPU EP での fp16 の速さ
- ORT の prebuilt に同梱される第三者表記の中身
- llama.cpp（ggml）で ModernBERT の MLM ヘッドを動かせるか

## 比べた候補と結論

| 論点 | 候補 | 結論 |
|---|---|---|
| M57 の backend（R2 が止まっている間） | (a) ORT の CPU EP + INT8 を計測 spike でゲートする / (b) R2 の再開を待つ / (c) DirectML EP / (d) R1 の ggml | (a)。spike が不合格なら (b)。(c) は maintenance mode のため採らない。(d) は MLM ヘッドへの対応が未確認 |
| ランタイム | (a) 素の ORT の CPU EP（C API） / (b) Windows ML（R2 と共有） / (c) 自前の MLP 推論 | (a)。R2 の再開後は、Windows ML の ORT を読める Host ではそれを使い、読めない Host（26100 未満、bootstrap の失敗）では同梱の素の ORT を使う。1 プロセスで読む ORT は 1 つとする。(b) は非パッケージの Host に bootstrap を今入れることになる。(c) は M57 に使えない |
| 依存の取り込み | (a) 公式 prebuilt を URL + SHA256 で固定し、opt-in の `AZOOKEY_FETCH_ONNXRUNTIME` で取る / (b) NuGet restore を CMake に足す / (c) ソースから build / (d) vcpkg | (a)。取得しない build と Linux は mock |
| x64 / ARM64 | — | 両方を固定する。ARM64 の同梱は M27 |
| 配布 | (a) base MSI に同梱 / (b) 別 MSI の add-on / (c) 初回にダウンロード | (a)。import せず、絶対パスでの `LoadLibraryExW`（`LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS`）と `GetProcAddress` の `OrtGetApiBase` で読む（承認の時点の「delay-load」を、管制塔のレビューを受けてこの形に改めた）。M57 のモデルはダウンロード |
| fallback | — | 既存の `reason` を流用し、原因の内訳はログだけに出す |
| BackendSelector | (a) 加えない / (b) 記録だけする / (c) 判定に加える | (a) |
| ライセンス | — | ORT は MIT。同梱の第三者表記は全文を引き継ぐ。表記の確定は Human Gate の DEV-2174 に分け、取り込む PR は DEV-2174 の確定の後に出す |
| opset と量子化 | — | opset は固定した ORT が読める範囲で 1 つに固定する。M56 は fp32。M57 は CPU EP で INT8 だけ、fp16 は GPU / NPU の EP だけ |

## 再評価の条件

- CPU EP の計測ゲート（`modernbert-ja-scoring-spec.md` §6.3）の結果が出たとき。
- M24 の R2 が再開したとき。reranker を Windows ML の ORT へ移す。
- Windows ML が、非パッケージの Win32 プロセスから bootstrap なしで使えるようになったとき。
