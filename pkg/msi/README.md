# 未署名 MSI のビルド

`azooKey.wixproj` は、`windows-release` の TIP と Inference Host を
`%ProgramFiles%\azooKey` へ配置する x64 の per-machine MSI を生成します。
WiX Toolset は MSBuild SDK として 5.0.2 に固定しているため、グローバルインストールは不要です。
クリーンな Windows 11 でも起動できるよう、Release バイナリが直接依存する
MSVC runtime 3 ファイルを app-local で同梱します。

```powershell
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$installPath = & $vswhere -latest -products * `
  -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
  -property installationPath
$redistVersion = Get-ChildItem "$installPath\VC\Redist\MSVC" -Directory |
  Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } |
  Sort-Object { [version]$_.Name } -Descending |
  Select-Object -First 1
$vcRuntimeDir = Get-ChildItem "$($redistVersion.FullName)\x64" -Directory |
  Where-Object { $_.Name -match '^Microsoft\.VC\d+\.CRT$' } |
  Select-Object -First 1

cmake --preset windows-release `
  -DAZOOKEY_FETCH_GOOGLETEST=ON `
  -DAZOOKEY_FETCH_WIL=ON `
  -DAZOOKEY_FETCH_LLAMA_CPP=ON
cmake --build --preset windows-release
dotnet build .\pkg\msi\azooKey.wixproj `
  --configuration Release `
  -p:ProductVersion=1.0.0 `
  "-p:VCRuntimeDir=$($vcRuntimeDir.FullName)"
```

出力は `pkg\msi\bin\Release\azooKey-1.0.0-x64.msi` です。
別の成果物を使う場合は、`TipDllPath` と `HostExePath` を MSBuild property で指定します。
`VCRuntimeDir` には、使用した MSVC toolset の x64 `Microsoft.VC*.CRT`
ディレクトリを指定します。MSI は `msvcp140.dll`、`vcruntime140.dll`、
`vcruntime140_1.dll` を TIP と Inference Host と同じディレクトリへ配置します。
本体の `LICENSE` と、同梱依存を記録した `THIRD_PARTY_LICENSES` も
テキストファイルとして配置します。

設定アプリの実行ファイルがある場合は `SettingsExePath` を指定します。
MSI は実行ファイルを同梱し、スタートメニューへ `azooKey Settings` を追加します。
現行のリリースビルドには設定アプリ target がないため、この property を省略した MSI には
設定アプリとショートカットが入りません。

インストール時は、ファイル配置後に管理者権限で `msiexec /y` を実行し、
TIP DLL の `DllRegisterServer` に COM クラス、TSF プロファイル、カテゴリの登録を委譲します。
アンインストール時はファイル削除前に `msiexec /z` を実行します。
どちらの経路にも、後続処理が失敗した場合の rollback action を設定しています。
64-bit TIP を確実に登録するため、カスタムアクションは
`%SystemRoot%\System32\msiexec.exe`（WiX の `System64Folder`）を使用します。

通常の Windows 11 では `%ProgramFiles%` から
`ALL APPLICATION PACKAGES`（SID `S-1-15-2-1`）の読み取り・実行 ACL を継承するため、
MSI は独自の ACL を追加しません。DEV-673 の実機ゲートでは、インストール先の継承 ACL と、
Microsoft Store / Edge など AppContainer アプリでの日本語入力を確認します。

GGUF モデルと CUDA ランタイムは base MSI に同梱しません。
モデルは初回取得経路、CUDA は optional add-on で扱います。

この MSI はコード署名しません。
インストール時には Windows Defender SmartScreen と UAC に「不明な発行元」と表示されます。
実機でのインストール、IME入力、アンインストール確認は、管理者権限が必要な人間ゲートです。
