# Trending アセットの公開・更新手順

本書は [`auto-word-registration-spec.md`](../auto-word-registration-spec.md) §5-2 から参照する
恒常 runbook である。入力形式・上限・拒否条件は同仕様 §5-3、取得と保存の契約は §5-1〜§5-2 を正典とする。
本書は公開前の検証と手動の配信操作を扱う。データ収集元、外部サービス、schema、MSI / MSIX、署名方式は追加しない。

## 1. 公開対象と承認

配信先の候補は GitHub Releases の固定タグ `trending-latest` とし、アセット名は
`trending-words.json` と `trending-words.json.sha256` の組にする。
公開先リポジトリ、タグ、実際の取得 URL は公開前に人間が確認する。
製品の取得 URL は、配信先と実データの承認がそろうまで空のままにする。
空の取得先で無通信となる基盤の検証を、実データの提供と扱わない。

公開する JSON は、次の事項を人間がレビューした実データに限る。

- 内容の出典と利用許諾、配布に必要な帰属表示。
- 各語の表記・読み・順位と、公開対象として含める根拠。
- 公開するバイト列と `generated_at`、差し戻しに使う直前の承認済みアセットの組。

出典・許諾・レビュー結果は対応する課題と Release の説明から追跡できるようにする。
ユーザーの入力履歴や個人の学習ストアを公開データの材料にしない。
空の `words` 配列や、全要素の reading が欠落・空のデータは形式検証を通り得るが、
実データの提供を満たすものとして公開の受入判定をしない。

## 2. 元のバイト列の検証と SHA256 生成

リポジトリルートで、レビュー対象ファイルを指定して実行する。
以下のパスはレビュー済みファイルの絶対パスに置き換える。

```powershell
$assetPath = 'C:\reviewed-assets\trending-words.json'
$checksumPath = 'C:\reviewed-assets\trending-words.json.sha256'
python scripts/validate_trending_asset.py $assetPath --checksum-out $checksumPath
```

終了コードが成功であることを確認する。validator は runtime の形式制約に従って入力を検証し、
元ファイルのバイト列から SHA256 を計算する。JSON を再整形・再シリアライズした別ファイルの
ハッシュを代用しない。内容・改行・エンコーディングを変更した場合は、レビューと検証をやり直す。
checksum を生成しない検証には `--checksum-out` を省略する。

検証した JSON と生成した checksum を一組として保管し、以後の確認とアップロードにはこの組を使う。
checksum は完全性の確認に使い、データの利用許諾や配信元の正当性の確認を置き換えない。

## 3. ローカル HTTP と取得期限の確認

Windows のビルド環境を用意し、README の標準手順で対象テストをビルドしてから実行する。
エージェントによるビルドは Windows Headless CMake Build の手順に従う。

```powershell
cmake --build --preset windows-debug --target host_http_downloader_tests host_trending_word_fetcher_tests
ctest --preset windows-debug -R '^(HttpDownloaderTest|TrendingWordHttpIntegrationTest|TrendingWordFetcherTest)\.' --output-on-failure --no-tests=error
```

loopback HTTP の fixture を通じ、checksum 取得、JSON 取得、検証、キャッシュ置換、ストア保存までを確認する。
`TrendingWordHttpIntegrationTest` は正常な取得・保存、SHA 不一致、正しい SHA を持つ
不正アセットについて、旧キャッシュ・メモリ・保存済みストアの保持と一時ファイルの後始末を確認する。
非 loopback の平文 HTTP を使わず、テストを実施するために製品の取得 URL を変更しない。

取得全体の 15 秒の期限は、`TrendingFetchDependencies::steady_now` へ注入した時計で
直前・一致・超過を確認する。`DeadlineUsesInjectedClockAtAndAroundFifteenSeconds` は
checksum 取得と JSON 取得の各段階で 14999 / 15000 / 15001 ms を検証する。
ネットワーク処理の境界で期限を評価し、期限を迎えた結果をキャッシュやストアへ反映しない。
実際に 15 秒待つ方法を、境界条件の検証の代わりにしない。

validator 自身の回帰検証には `python scripts/tests/test_validate_trending_asset.py` を使う。
同じテストは Build workflow の `quality`（表示名 `Pre-commit`）ジョブでも実行する。

## 4. 手動公開と更新

1. 公開する JSON と checksum、出典・許諾・レビュー記録、ローカル検証結果をそろえる。
   更新では直前の承認済みの組も保存する。
2. 人間が対象リポジトリと固定タグを確認し、Release の公開またはアセット置換を承認する。
   初回は Release の下書きで二つのファイル名と内容を確認してから公開する。
3. 更新では `generated_at` に再発行ごとに異なる値を設定し、変更後の JSON を再検証して checksum を作る。
   同じ値は Host の直近の保存成功値と一致すると、再取り込みを省略される。
4. 人間が承認した二つのアセットを手動で公開または置換する。固定タグを使うためだけに
   既存の Git タグを別コミットへ移動しない。
5. 公開 URL から二つのアセットを取り直し、公開された JSON の元バイト列の SHA256 が
   公開 checksum と一致することを確認する。validator も公開された JSON に対して再実行する。
6. 公開した Release と検証結果を対応する課題へ記録する。製品の取得先を設定する変更は、
   確認した URL と公開内容の承認を根拠に別途レビューする。

二つのアセットは同時には置換できない。更新途中に JSON と checksum の組が食い違うと、
クライアントは SHA256 不一致として取得を拒否し、旧キャッシュとストアを保持する。
片方が一時的に取得できない場合も取得失敗となる。整合する組の公開を確認するまで、更新の受入としない。

## 5. データのロールバック

配信データを差し戻す場合は、直前の承認済み JSON を基に人間が内容を再確認し、
`generated_at` を差し戻しの再発行用の値へ変更する。§2 の validator と SHA256 生成、
§3 のローカル検証を経て、§4 の手動更新と公開後の照合を行う。
以前の checksum は、`generated_at` を変更した JSON に再利用しない。

アセットの差し戻しは、既に取り込まれた語を削除する操作ではない。
新しいアセットから語を除いても AutoWordStore の既存語は消えず、`rejected` は保持され、
同一語にローカルの `mining` 記録がある場合はその由来が優先される。
順位を戻しても既存語の score は低下せず、count・recency・自動昇格した Confirmed 状態も
公開前の値へ復元されない。配信データの差し戻しを学習状態の復元とみなさない。
必要な語の拒否や学習データ削除は、ユーザーの意思に基づく既存の管理操作として別に扱う。
配信ロールバックの一部として、学習ストアをリセットしたりファイルを削除したりしない。

## 6. 受入記録

対応する課題へ、公開対象の Release、`generated_at`、SHA256、内容と利用許諾のレビュー結果、
validator とローカル HTTP テストの結果、公開後に再取得した組の照合結果、
差し戻し用アセットの保管先を記録する。恒常文書へ検証日ごとの結果や測定件数を転記しない。
