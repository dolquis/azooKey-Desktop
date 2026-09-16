#requires -Version 5.1
<#
.SYNOPSIS
  配布バイナリの PE import に現れる MSVC ランタイム DLL が、app-local 同梱の
  取得元ディレクトリに揃っていることを確認する。

.DESCRIPTION
  MSI は VC++ Redistributable を要求せず、必要な MSVC ランタイムを
  %ProgramFiles%\azooKey へ app-local 配置する（docs/sideload-packaging-spec.md §4.1）。
  同梱一覧は手書きのため、ビルド構成の変更で新しいランタイム依存が増えても
  静かに漏れる。DEV-1137 では ggml の OpenMP 既定有効化により Host が
  VCOMP140.DLL を暗黙インポートし、未同梱のまま配布された結果、
  クリーン環境で 0xC0000135 終了した。

  このスクリプトは dumpbin /dependents の import 名を読み、MSVC ランタイムに
  該当する名前が同梱元ディレクトリのいずれかに実在するかを検査する。
  OS が常に提供する DLL（kernel32 等）は対象にしない。判定対象を MSVC
  ランタイムの命名規則に限定することで、allowlist の保守を持ち込まない。

  Release workflow は MSI をビルドする前にこれを実行する。dumpbin は MSVC
  開発者環境の PATH から解決する。
#>
param(
  [string[]]$BinaryPath = @(),
  [string[]]$RuntimeDirectory = @()
)

$ErrorActionPreference = "Stop"

# msvcp140 / msvcp140_1 / vcruntime140 / vcruntime140_1 / vcomp140 / concrt140 /
# msvcr120 など、Visual C++ の再頒布可能ランタイムが取りうる名前。
$script:AppLocalRuntimePattern = '^(?:msvcp|msvcr|vcruntime|vcomp|concrt)\d+(?:_\d+)?\.dll$'

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

function Get-AppLocalRuntimeGap {
  <#
    .SYNOPSIS
      import 名の一覧と同梱元ディレクトリを突き合わせ、欠落した DLL 名を返す。
  #>
  param(
    [Parameter(Mandatory = $true)]
    [AllowEmptyCollection()]
    [string[]]$ImportName,
    [Parameter(Mandatory = $true)]
    [string[]]$RuntimeDirectory
  )

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
    $found = $false
    foreach ($directory in $RuntimeDirectory) {
      if (Test-Path -LiteralPath (Join-Path $directory $name) -PathType Leaf) {
        $found = $true
        break
      }
    }
    if (-not $found) {
      $missing += $name
    }
  }
  return $missing
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
    [string[]]$RuntimeDirectory,
    [AllowNull()]
    [hashtable]$DependencyOutput = $null
  )

  foreach ($directory in $RuntimeDirectory) {
    if (-not (Test-Path -LiteralPath $directory -PathType Container)) {
      throw "App-local runtime directory does not exist: $directory"
    }
  }

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
    $missing = Get-AppLocalRuntimeGap `
      -ImportName $imports `
      -RuntimeDirectory $RuntimeDirectory
    if ($missing) {
      $failures += "$binary imports $($missing -join ', ')"
    }
    $inspected += [pscustomobject][ordered]@{
      path = $binary
      runtimeImports = @($imports | Where-Object { $_ -match $script:AppLocalRuntimePattern })
    }
  }

  if ($failures) {
    throw ("App-local MSVC runtime payload is incomplete. Add the missing files to " +
      "pkg/msi/Package.wxs and to the release workflow's runtime resolution, or drop " +
      "the dependency from the shipped build. " + ($failures -join "; ") +
      ". Searched: " + ($RuntimeDirectory -join ", "))
  }

  return $inspected
}

if ($MyInvocation.InvocationName -ne ".") {
  if (-not $BinaryPath) {
    throw "Specify -BinaryPath with at least one binary to inspect."
  }
  if (-not $RuntimeDirectory) {
    throw "Specify -RuntimeDirectory with the app-local runtime source directories."
  }

  $result = Assert-AppLocalRuntime `
    -BinaryPath $BinaryPath `
    -RuntimeDirectory $RuntimeDirectory
  foreach ($entry in $result) {
    $imports = if ($entry.runtimeImports) { $entry.runtimeImports -join ", " } else { "(none)" }
    Write-Output "$($entry.path): $imports"
  }
}
