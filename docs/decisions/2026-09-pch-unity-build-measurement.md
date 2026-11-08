# ビルド時間の内訳と PCH / unity build の実測

- 種別: 測定記録・決定記録
- 日付: 2026-09
- 関連 Issue: DEV-910、DEV-1002、DEV-1086、DEV-1087
- 結論の正典: [`dev-infrastructure-spec.md`](../dev-infrastructure-spec.md) §2.7・§11.8
- 移設元: `docs/dev-infrastructure-spec.md` §2.7（「M37 受け入れ条件」を除く）と §11.8（commit `c8d9a5b` 時点）。原文をそのまま移設した

§2.7 の clang-cl 計測は原記録に日付が無い。§11.8 の MSVC 計測（2026-09）の前提として書かれているため同じファイルに置いた。本文中の `§` は移設元の `docs/dev-infrastructure-spec.md` の節を指す。

### 2.7 ビルド時間の内訳（実測）

本節は M37 の受け入れ条件ではなく、ビルド時間短縮策の当てどころを決めるための計測
記録である。短縮策としては §4.7 の sccache のみが入っており、コンパイル 1 回あたりの
コストがどこに出ているかは未計測だった。

#### 測定条件

| 項目 | 値 |
|---|---|
| コンパイラ | clang-cl（LLVM 23.1.0、`/clang:-ftime-trace`） |
| 構成 | Debug、Ninja、`-j 4` |
| 対象 | 本体ターゲットのみ（`AZOOKEY_BUILD_TESTS=OFF` / `AZOOKEY_BUILD_BENCH=OFF`）、55 TU |
| キャッシュ | `AZOOKEY_USE_COMPILER_CACHE=OFF`（実コンパイルを測るため） |

再現手順は、MSVC 環境を初期化したうえで次を実行し、生成される TU ごとの
`*.cpp.json` を集計する。

```powershell
cmake -S . -B build/timetrace -G Ninja -DCMAKE_BUILD_TYPE=Debug `
  -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl `
  "-DCMAKE_CXX_FLAGS=/DWIN32 /D_WINDOWS /GR /EHsc /clang:-ftime-trace" `
  -DAZOOKEY_USE_COMPILER_CACHE=OFF -DAZOOKEY_BUILD_TESTS=OFF `
  -DAZOOKEY_BUILD_BENCH=OFF -DAZOOKEY_FETCH_WIL=ON
cmake --build build/timetrace
```

`CMAKE_CXX_FLAGS` を指定すると CMake の MSVC 既定フラグを上書きするため、
`/DWIN32 /D_WINDOWS /GR /EHsc` を明示的に含める。省くと例外が無効化され
`cannot use 'try' with exceptions disabled` でビルドが落ちる。

集計には clang が出力する `Total Source`（`.cpp` の直接 include をルートとする解析
時間。TU あたり 0〜15 件）と `Total InstantiateFunction` / `Total InstantiateClass` を
使う。個別の `Source` イベントは `-ftime-trace-granularity` の既定 500us で出力から
落ちるため、`Total` 側を読む。

**この 2 つは排他ではなく、重なる累積タイマーである。** テンプレート実体化は
ヘッダの解析中に起きるため、実体化の時間はヘッダ展開の時間にも含まれる。したがって
以下の「ヘッダ展開」「テンプレート実体化」はいずれも **inclusive** な値であり、
足しても total にならない（下表の `C011_shortcut_routing.cpp` は 84.8% + 26.2% =
111.0%）。内訳の分割ではなく、それぞれ独立に「どれだけの時間がそこを通ったか」を
表す指標として読むこと。

`Source` 同士の入れ子による二重計上は無い。55 TU すべてで `Total Source` が
`Total Frontend` を超えないこと（最大比 0.990）、`Total Source` の count が TU あたり
0〜15 件（`.cpp` の直接 include 数）に留まることを確認している。

#### 上位 10 TU

| TU | total (ms) | frontend | backend | ヘッダ展開 (incl.) | テンプレート実体化 (incl.) | ヘッダ% | テンプレート% |
|---|---|---|---|---|---|---|---|
| `NamedPipeTransport.cpp` | 5315 | 5128 | 112 | 4479 | 533 | 84.3% | 10.0% |
| `Diagnostics.cpp` | 2711 | 2213 | 310 | 1236 | 630 | 45.6% | 23.2% |
| `TextService.cpp` | 2360 | 2140 | 138 | 1651 | 530 | 69.9% | 22.5% |
| `main.cpp` | 1961 | 1702 | 155 | 1255 | 516 | 64.0% | 26.3% |
| `CompatRunner.cpp` | 1960 | 1710 | 158 | 1310 | 496 | 66.8% | 25.3% |
| `ReportWriter.cpp` | 1812 | 1540 | 164 | 1377 | 446 | 76.0% | 24.6% |
| `Dispatcher.cpp` | 1811 | 1700 | 66 | 1170 | 424 | 64.6% | 23.4% |
| `UserDictCli.cpp` | 1790 | 1517 | 153 | 1299 | 474 | 72.5% | 26.5% |
| `C011_shortcut_routing.cpp` | 1763 | 1676 | 44 | 1494 | 462 | 84.8% | 26.2% |
| `TargetConfigLoader.cpp` | 1756 | 1577 | 102 | 1375 | 445 | 78.3% | 25.3% |

TU 別コンパイル時間の総和は 74.2 秒（並列実行のため wall-clock とは異なる）。
上位 10 TU が総和の 31.3% を占める。

#### 全体の内訳と読み取り

ビルド全体（55 TU）では **frontend 91.2%、ヘッダ展開 76.4%（inclusive）、
テンプレート実体化 22.2%（inclusive）**。後の 2 つは重なるため、合計は内訳にならない。
排他な分割は frontend と backend の間にだけ成り立つ。

* **コストはヘッダ展開に偏っている。** backend（最適化・コード生成）は 1 割に満たず、
  frontend の中でもヘッダの解析が支配的である。`NamedPipeTransport.cpp` は単一 TU で
  最も重く、その 84.3% がヘッダ展開である。
* したがって**当てどころは PCH** である。PCH はまさにヘッダ展開の繰り返しを削る手段で、
  この分布と一致する。
* **unity build もこの内訳だけでは決まらない。** unity build が削るのは TU 数に比例する
  固定費だけではない。複数の `.cpp` を同じ TU へまとめると、include guard や
  `#pragma once` の効く共通ヘッダは unity batch 内で 1 度しか解析されない。つまり
  ここで支配項と測った「TU ごとに繰り返すヘッダ展開」そのものが削減対象になる。
  内訳から不採用を導くことはできないため、unity on/off も実測の対象とした。
  結論は §11.8 にある。
* 重量 TU は `tsf-tip` に限らない。`ipc` / `diagnostics` / `compat-test` にも同程度の
  ヘッダ展開コストが出ており、PCH の適用対象を `tsf-tip` と `settings-app` に限定する
  前提は、実測では支持されない。

#### この計測が示さないこと

* **絶対値を MSVC へ一般化できない。** 実際の CI と開発ループが使うのは `cl.exe` で
  あり、本計測は clang-cl のもの。内訳の傾向（ヘッダ展開が支配的）は構造的なもので
  移りやすいが、短縮幅の見積もりには使えない。
* **PCH の採否もこの計測だけでは決まらない。** §4.7 の sccache と PCH は相互作用し、
  MSVC の PCH（`/Yc` `/Yu`）を挟むとキャッシュのヒット率が落ちて正味で悪化しうる。
  そのため採否は「PCH 有無 × sccache hit/miss」の 4 条件を MSVC で実測して決めた。
  結論は §11.8 にある。
* **ヘッダ展開のうち「純粋な解析」と「実体化」の比率は出せない。** 上記のとおり
  両者は重なる指標であり、exclusive time は取っていない（個別イベントが粒度で
  落ちているため、算出には粒度を下げた再計測が要る）。「ヘッダ展開が支配的」と
  いう結論はこの制約の影響を受けないが、内訳をさらに割る用途には使えない。
* テスト・bench ターゲットと `settings-app` は対象外。Debug 構成のみ。

### 11.8 PCH — 不採用 / unity build — 見送り（DEV-910 で確定 / 2026-09）

§2.7 は、ビルド時間の支配項が「TU ごとに繰り返すヘッダ展開」であることを clang-cl の
`-ftime-trace` で示した。その削減手段として PCH と unity build を MSVC で実測し、
**PCH は不採用、unity build は見送りとする。**

#### 測定条件

| 項目 | 値 |
|---|---|
| コンパイラ | MSVC（`cl.exe`、CI と開発ループが実際に使うもの） |
| 構成 | Debug、Ninja、`-j 20`、20 論理プロセッサ |
| 対象 | `AZOOKEY_BUILD_TESTS=ON` / `AZOOKEY_BUILD_BENCH=ON`、241 TU |
| キャッシュ | `AZOOKEY_USE_COMPILER_CACHE=ON`（`/Z7` Embedded が効く条件） |
| 計測範囲 | `cmake --build` のみ。configure は含めない |
| 試行 | 各条件 3 回、中央値（最小-最大） |

`SCCACHE_DIR` は計測専用ディレクトリへ向け、cold 条件はキャッシュディレクトリの
作り直しで作る。GoogleTest は事前取得した v1.17.0 のソースツリーを指定し、
configure ごとのダウンロードを計測から排除する。

**configure 時に `VSLANG=1033` を与えること。** これが無いと日本語 UI ホストでは ninja が
header dependency を 1 件も記録せず、さらに PCH の有無で `/showIncludes` の未消費出力量が
変わるため、計測にバイアスが乗る。この不具合そのものは DEV-1002 で扱う。

#### 4 条件の結果

| 条件 | PCH | sccache | 中央値 | 最小-最大 |
|---|---|---|---|---|
| 1 | 無 | cold | 130.3 s | 93.2-137.8 |
| 2 | 無 | warm | 29.4 s | 21.9-30.4 |
| 3 | 有 | cold | 70.3 s | 62.4-73.7 |
| 4 | 有 | warm | 42.1 s | 32.9-252.6 |

PCH は cold を 46.1% 短縮し、warm を 43.0% 悪化させる。

条件 4 の最大値 252.6 s は他の 2 回（42.1 s / 32.9 s）で再現しなかった単発の外れ値だが、
除外せず範囲に残す。cold 条件は同一セル内でも 93-138 s とばらつくため、
この幅を超える差だけを有意と読む。

#### sccache のヒット率がどう変わるか

`sccache --show-stats` の実測値。warm 条件、ninja がコンパイルしたオブジェクト数を分母とする。

| | コンパイル対象 | cache hit | hit 率 |
|---|---|---|---|
| PCH 無 | 241 | 241 | 100% |
| PCH 有 | 254 | 73 | 28.7% |

**残る 71.3% は cache miss ではなく、構造的にキャッシュ不能である。** sccache が挙げる
non-cacheable の理由は `/Fp` が 168 件、`/Yc` が 13 件で、PCH を使う全 TU と PCH 生成
そのものに一致する。sccache は PCH 関連フラグを持つコンパイルをキャッシュ対象から外すため、
これらは cache が warm でも毎回コンパイルされる。PCH がヘッダ解析を削る利得と、
キャッシュを失う損失が相殺し、warm では損失が上回る。

#### 不採用の根拠

CI と日常の開発ループでは cache が warm な条件が支配的である。CI は sccache を GitHub
Actions のキャッシュサービスで run 間再利用し（§4.7）、ローカルでも初回ビルド以降は warm
になる。**PCH が改善するのは、頻度の低い cold 条件だけである。**

ローカルの増分ビルドでは PCH に利得がある（ヘッダ 1 本の変更で 42.7 s → 17.5 s、
単一 `.cpp` の変更で 10.0 s → 7.5 s）。これを取りに行く案として、PCH をローカル専用
preset に限る構成も検討したが採らない。ビルド構成が二重化し、CI が通らない経路が
できるうえ、ローカルでもブランチ切り替え後のフルビルドは warm 条件に当たって
43% の悪化を受けるためである。PCH の適用には DEV-1086 の解消も前提となる。

#### unity build を見送る理由

unity build は性能だけを見れば採用に足る。cold 71.5 s（-45.1%）、warm 16.5 s（-43.9%）で
**PCH と違い warm でも改善し、全 TU がキャッシュ可能なまま**である（hit 率 100%）。
unity ビルドの成果物で CTest は全件成功する。

それでも見送るのは、性能ではなく順序の問題による。

* 241 から 76 へ減った edge のうち 80、**削減量の 48% は `tsf_tip_*_tests` 8 target が
  同じ 11 ソースを重複コンパイルしている分**である（DEV-1087）。この重複は unity build の
  意味論的リスクを負わずに構造的に除去でき、Issue が挙げる「テストターゲット限定の適用」も
  そちらで実現される。
* 重複を除いた後に unity build がどれだけ上積みするかは測っていない。
* DEV-910 の計測時には除外が 2 件必要だった（`inference-host/src/UserDictCli.cpp` の無名 namespace 衝突、
  `bench/BenchmarkResult.cpp` の `GetObject` マクロ衝突）。加えて unity batch は
  ソースの並び順で決まるため、ソースを 1 本足すと無関係な batch の内容が変わり、
  それまで通っていた組み合わせが壊れうる。
* 単一 `.cpp` の変更に対する増分ビルドは 10.0 s → 12.4 s と 24% 悪化する。
  batch 全体が再コンパイルされるためで、最も頻度の高い操作が遅くなる。

再評価は本節と同じ 4 条件で行う。CMake の再評価用対策として、Win32 `max` マクロとの
衝突を core target の `NOMINMAX` で防ぐ。無名 namespace の同名 helper を持つ
学習ストア、Host CLI / モデル関連ソース、core / Host のテストソースは、該当 target の
`CMakeLists.txt` で衝突する組から必要なソースだけを unity batch から除外する。
同じ契約の helper は内部共通 helper へ集約し、意味が異なる helper はソース固有の名前にする。
これで衝突を解消できる場合は、unity batch からの除外を不要とする。
`CrashReporting.cpp` と `ThreadStackGuarantee.cpp` も別 object にして、TIP DLL が
クラッシュフィルタ本体を取り込まないリンク境界を保つ。これらは unity build の既定採用を
意味しない。
`UNITY_BUILD_BATCH_SIZE` は既定値のまま測っており、調整の余地を残している。

#### この判断が対象としないもの

`settings-app` は 4 条件の対象外である。MSBuild が `azookey_settings.vcxproj` を
ビルドする独立経路にあり、CMake の compiler launcher が及ばないため sccache とは
相互作用しない。PCH は `pch.h` と `/Yu` で既に使っている。Issue が当初挙げた
「対象は `tsf-tip/` と `settings-app/`」という前提は、この 2 点で成り立たない。
`tsf-tip` 以外にも重いヘッダ展開が出ている点は §2.7 のとおりである。

#### 再現手順

計測に使った CMake 側の足場は commit `a52527a` にある。`AZOOKEY_USE_PCH`（既定 OFF、
TU 数が 8 以上の repo target 13 個 / 168 TU へ `target_precompile_headers` を適用）、
`cmake/azookey_pch.h`（過半の TU が使う標準ヘッダと `<windows.h>`、および
`GetObject` の `#undef`）、`CMAKE_UNITY_BUILD` 時の除外 2 件からなる。
結論を反映した次の commit で取り除いてあるため、再現するときはこの commit の
CMake 変更を復元する。

```powershell
$env:VSLANG = "1033"
$env:SCCACHE_DIR = "<計測専用ディレクトリ>"
cmake -S . -B build/pch -G Ninja -DCMAKE_BUILD_TYPE=Debug `
  -DAZOOKEY_BUILD_TESTS=ON -DAZOOKEY_BUILD_BENCH=ON `
  -DAZOOKEY_USE_COMPILER_CACHE=ON -DAZOOKEY_USE_PCH=ON `
  -DAZOOKEY_FETCH_GOOGLETEST=ON `
  -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST="<取得済み googletest>"
sccache --zero-stats
cmake --build build/pch -j 20
sccache --show-stats
```

cold は `SCCACHE_DIR` を作り直してから、warm は同じキャッシュのまま build ディレクトリを
作り直してから測る。unity build は `-DCMAKE_UNITY_BUILD=ON` を渡す。
衝突を洗い出すときは ninja の `-k 0` を使う。既定の `-k 1` は最初の失敗で
スケジューリングを止めるため、衝突件数を過小に見積もる。
