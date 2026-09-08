# Windows 設定アプリのアイコン

`azookey-settings.ico` は青いキーキャップに小文字の a を配置した、この製品用の新規意匠です。
`legacy/` の資産やフォントは使用していません。リポジトリの LICENSE に従います。
16 / 20 / 24 / 32 / 40 / 48 / 64 / 128 / 256 px の画像を含みます。

再生成は `render-icon.py` 冒頭のコマンドを使用します。ICO は配布用のソース資産として管理し、
通常のビルドに Python や Pillow は不要です。EXE の resource と MSI の Icon table は同じ ICO を使用します。

EXE の VERSIONINFO は CMake の `AZOOKEY_PRODUCT_VERSION`（直接 MSBuild する場合は
`AzooKeyProductVersion`）から作ります。既定値は開発版の `0.0.0` です。
リリース CI は検証済みタグの版を MSI と共通で渡し、EXE では末尾に `.0` を付けます。
