#requires -Version 5.1
<#
.SYNOPSIS
  配布バイナリの PE import に現れる MSVC ランタイム DLL を、MSI が実際に同梱する
  ファイル一覧と突き合わせる。

.DESCRIPTION
  MSI は VC++ Redistributable を要求せず、必要な MSVC ランタイムを
  %ProgramFiles%\azooKey へ app-local 配置する（docs/sideload-packaging-spec.md §4.1）。
  同梱一覧は `pkg/msi/Package.wxs` の手書き `<File>` なので、ビルド構成の変更で
  新しいランタイム依存が増えても静かに漏れる。DEV-1137 では ggml の OpenMP 既定
  有効化により Host が VCOMP140.DLL を暗黙インポートし、未同梱のまま配布された
  結果、クリーン環境で 0xC0000135 終了した。

  照合先は redist ディレクトリではなく MSI の同梱一覧である。redist ディレクトリは
  `msvcp140_1.dll` や `concrt140.dll` のように MSI が同梱しないファイルも持つため、
  そちらと突き合わせると「取得元にはあるが MSI に入らない」依存を見逃す。

  同梱一覧は 2 つの経路から集める。

  - `Package.wxs` の `<File Source="...">`（`Name` があればそれを優先する）
  - `-PayloadDirectory` に渡したディレクトリのファイル。`Package.wxs` が
    `<Files Include="...\**">` でツリーごと harvest する設定アプリの payload が
    これに当たる

  判定対象は MSVC ランタイムの命名規則に一致する import だけで、OS が常に提供する
  DLL は対象にしない。allowlist を持ち込まないための制限である。

  Release workflow は MSI をビルドする前にこれを実行する。dumpbin は MSVC 開発者
  環境の PATH から解決する。
#>
param(
  [string[]]$BinaryPath = @(),
  [string]$PackagePath = "",
  [string[]]$PayloadDirectory = @()
)

$ErrorActionPreference = "Stop"

# msvcp140 / msvcp140_1 / msvcp140_atomic_wait / vcruntime140_threads /
# vccorlib140 / vcomp140 / concrt140 など、Visual C++ の再頒布可能ランタイムが
# 取りうる名前。接尾辞は数字だけではないため、語尾を数字に限定しない。
$script:AppLocalRuntimePattern =
  '^(?:msvcp|msvcr|vcruntime|vcomp|concrt|vccorlib)\d+(?:_[A-Za-z0-9_]+)?\.dll$'

function Get-AppLocalRuntimeImport {
  <#
    .SYNOPSIS
      dumpbin /dependents の出力から import DLL 名を取り出す。
    .DESCRIPTION
      dumpbin は通常の依存も delay load 依存も、字下げした 1 行 1 件で並べる。
      Summary 節はセクション名とサイズなので .dll で終わらず、混入しない。
  #>
  param(
    [Parameter(Mandatory = $true)]
    [AllowEmptyCollection()]
    [AllowEmptyString()]
    [string[]]$DependencyOutput
  )

  $names = @()
  foreach ($line in $DependencyOutput) {
    if ($line -match '^\s+([A-Za-z0-9_.+-]+\.(?:dll|DLL))\s*$') {
      $names += $Matches[1]
    }
  }
  return $names
}

function Get-AppLocalRuntimeDependencyOutput {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path
  )

  $output = & dumpbin /nologo /dependents $Path 2>&1
  if ($LASTEXITCODE -ne 0) {
    throw ("dumpbin failed for '$Path' (exit $LASTEXITCODE): " +
      ($output | Out-String).Trim())
  }
  return @($output | ForEach-Object { [string]$_ })
}

function Get-MsiShippedFileName {
  <#
    .SYNOPSIS
      Package.wxs が個別に列挙する `<File>` の配置名を返す。
    .DESCRIPTION
      `Name` があればインストール後の名前はそれであり、無ければ `Source` の
      末尾要素になる。`$(Var)` のまま解決できない Source はその文字列を返すが、
      import 名と一致しないので判定へ影響しない。
  #>
  param(
    [Parameter(Mandatory = $true)]
    [string]$PackagePath
  )

  if (-not (Test-Path -LiteralPath $PackagePath -PathType Leaf)) {
    throw "WiX package authoring not found: $PackagePath"
  }
  [xml]$document = Get-Content -Raw -LiteralPath $PackagePath
  $namespaces = [System.Xml.XmlNamespaceManager]::new($document.NameTable)
  $namespaces.AddNamespace("w", "http://wixtoolset.org/schemas/v4/wxs")
  $nodes = $document.SelectNodes("//w:File", $namespaces)
  if (-not $nodes -or $nodes.Count -eq 0) {
    throw "No <File> elements were found in '$PackagePath'."
  }

  $names = @()
  foreach ($node in $nodes) {
    # XmlNode.Name はノード名（"File"）を返すため、属性は GetAttribute で読む。
    $name = $node.GetAttribute("Name")
    if (-not $name) {
      $name = [System.IO.Path]::GetFileName($node.GetAttribute("Source").Replace("/", "\"))
    }
    if ($name) {
      $names += $name
    }
  }
  return $names
}

function Get-AppLocalRuntimeGap {
  <#
    .SYNOPSIS
      import 名の一覧と同梱一覧を突き合わせ、同梱されない MSVC ランタイム名を返す。
  #>
  param(
    [Parameter(Mandatory = $true)]
    [AllowEmptyCollection()]
    [AllowEmptyString()]
    [string[]]$ImportName,
    [Parameter(Mandatory = $true)]
    [AllowEmptyCollection()]
    [string[]]$ShippedName
  )

  $shipped = @{}
  foreach ($name in $ShippedName) {
    $shipped[$name.ToLowerInvariant()] = $true
  }

  $missing = @()
  $seen = @{}
  foreach ($name in $ImportName) {
    if ($name -notmatch $script:AppLocalRuntimePattern) {
      continue
    }
    $key = $name.ToLowerInvariant()
    if ($seen.ContainsKey($key)) {
      continue
    }
    $seen[$key] = $true
    if (-not $shipped.ContainsKey($key)) {
      $missing += $name
    }
  }
  return $missing
}

function Get-AppLocalPayloadFileName {
  <#
    .SYNOPSIS
      ツリーごと harvest されるディレクトリの配置名を返す。
  #>
  param(
    [Parameter(Mandatory = $true)]
    [AllowEmptyCollection()]
    [string[]]$PayloadDirectory
  )

  $names = @()
  foreach ($directory in $PayloadDirectory) {
    if (-not (Test-Path -LiteralPath $directory -PathType Container)) {
      throw "Harvested payload directory does not exist: $directory"
    }
    $names += Get-ChildItem -LiteralPath $directory -File -Recurse |
      ForEach-Object { $_.Name }
  }
  return $names
}

function Assert-AppLocalRuntime {
  <#
    .SYNOPSIS
      すべての配布バイナリについて、MSVC ランタイム依存の同梱漏れを検査する。
    .PARAMETER DependencyOutput
      バイナリのパスをキー、dumpbin の出力行を値とするテーブル。
      指定した場合は dumpbin を起動せず、テストから経路を差し替えられる。
  #>
  param(
    [Parameter(Mandatory = $true)]
    [string[]]$BinaryPath,
    [Parameter(Mandatory = $true)]
    [AllowEmptyCollection()]
    [string[]]$ShippedName,
    [AllowNull()]
    [hashtable]$DependencyOutput = $null
  )

  $failures = @()
  $inspected = @()
  foreach ($binary in $BinaryPath) {
    if (-not (Test-Path -LiteralPath $binary -PathType Leaf)) {
      throw "Binary to inspect does not exist: $binary"
    }
    if ($null -ne $DependencyOutput) {
      if (-not $DependencyOutput.ContainsKey($binary)) {
        throw "No dumpbin output was supplied for '$binary'."
      }
      $output = @($DependencyOutput[$binary])
    } else {
      $output = Get-AppLocalRuntimeDependencyOutput -Path $binary
    }
    $imports = Get-AppLocalRuntimeImport -DependencyOutput $output
    if (-not $imports) {
      throw "No imported DLL names were found for '$binary'."
    }
    $missing = Get-AppLocalRuntimeGap -ImportName $imports -ShippedName $ShippedName
    if ($missing) {
      $failures += "$binary imports $($missing -join ', ')"
    }
    $inspected += [pscustomobject][ordered]@{
      path = $binary
      runtimeImports = @($imports | Where-Object { $_ -match $script:AppLocalRuntimePattern })
    }
  }

  if ($failures) {
    throw ("The MSI does not ship every MSVC runtime the release binaries import. " +
      "Add the missing files to pkg/msi/Package.wxs and to the release workflow's " +
      "runtime resolution, or drop the dependency from the shipped build. " +
      ($failures -join "; "))
  }

  return $inspected
}

if ($MyInvocation.InvocationName -ne ".") {
  if (-not $BinaryPath) {
    throw "Specify -BinaryPath with at least one binary to inspect."
  }
  if (-not $PackagePath) {
    throw "Specify -PackagePath with the WiX package authoring (pkg/msi/Package.wxs)."
  }

  $shipped = @(Get-MsiShippedFileName -PackagePath $PackagePath)
  $shipped += @(Get-AppLocalPayloadFileName -PayloadDirectory $PayloadDirectory)

  $result = Assert-AppLocalRuntime -BinaryPath $BinaryPath -ShippedName $shipped
  foreach ($entry in $result) {
    $imports = if ($entry.runtimeImports) { $entry.runtimeImports -join ", " } else { "(none)" }
    Write-Output "$($entry.path): $imports"
  }
}
