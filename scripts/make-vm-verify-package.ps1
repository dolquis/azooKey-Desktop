#requires -Version 5.1
param(
  [ValidatePattern("^[A-Za-z0-9._-]+$")]
  [string]$Preset = "windows-release",
  [string]$OutputDirectory = "",
  [string]$MockDictionaryPath = "",
  [string]$ModelPath = "",
  [switch]$AllowNoModel,
  [switch]$IncludeCompat,
  [string]$RuntimeInstallerPath = ""
)

$ErrorActionPreference = "Stop"

function Get-VmVerifyAbsolutePath {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path
  )

  return $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
}

function Get-VmVerifyPresetConfiguration {
  param(
    [Parameter(Mandatory = $true)]
    [string]$PresetsPath,
    [Parameter(Mandatory = $true)]
    [ValidatePattern("^[A-Za-z0-9._-]+$")]
    [string]$PresetName,
    [Parameter(Mandatory = $true)]
    [string]$RepositoryRoot
  )

  $document = Get-Content -Raw -LiteralPath $PresetsPath | ConvertFrom-Json
  $presets = @($document.configurePresets)
  $visited = @{}

  function Resolve-VmVerifyPreset {
    param(
      [Parameter(Mandatory = $true)]
      [string]$Name
    )

    if ($visited.ContainsKey($Name)) {
      throw "CMake preset inheritance cycle detected at '$Name'."
    }
    $visited[$Name] = $true

    $node = $presets | Where-Object { $_.name -eq $Name } | Select-Object -First 1
    if (-not $node) {
      throw "CMake configure preset not found: $Name"
    }

    $resolved = @{
      BinaryDir = ""
      CacheVariables = @{}
    }

    $parents = @($node.inherits)
    # CMake gives the first parent in an inherits array higher precedence.
    # Apply parents in reverse order so earlier parents overwrite later ones.
    for ($parentIndex = $parents.Count - 1; $parentIndex -ge 0; $parentIndex--) {
      $parentName = $parents[$parentIndex]
      if (-not $parentName) {
        continue
      }
      $parent = Resolve-VmVerifyPreset -Name $parentName
      if ($parent.BinaryDir) {
        $resolved.BinaryDir = $parent.BinaryDir
      }
      foreach ($entry in $parent.CacheVariables.GetEnumerator()) {
        $resolved.CacheVariables[$entry.Key] = $entry.Value
      }
    }

    if ($node.binaryDir) {
      $resolved.BinaryDir = [string]$node.binaryDir
    }
    if ($node.cacheVariables) {
      foreach ($property in $node.cacheVariables.PSObject.Properties) {
        $value = $property.Value
        if ($value -isnot [string] -and $value.PSObject.Properties["value"]) {
          $value = $value.value
        }
        $resolved.CacheVariables[$property.Name] = [string]$value
      }
    }

    $visited.Remove($Name)
    return $resolved
  }

  $configuration = Resolve-VmVerifyPreset -Name $PresetName
  if (-not $configuration.BinaryDir) {
    throw "CMake configure preset '$PresetName' does not define binaryDir."
  }

  $configuration.BinaryDir = $configuration.BinaryDir.Replace(
    '${sourceDir}',
    $RepositoryRoot)
  $configuration.BinaryDir = Get-VmVerifyAbsolutePath -Path $configuration.BinaryDir
  return $configuration
}

function Get-VmVerifyCacheValue {
  param(
    [Parameter(Mandatory = $true)]
    [string]$CachePath,
    [Parameter(Mandatory = $true)]
    [string]$Name
  )

  $pattern = "^$([regex]::Escape($Name)):[^=]+=(.*)$"
  $match = Select-String -LiteralPath $CachePath -Pattern $pattern |
    Select-Object -First 1
  if (-not $match) {
    return ""
  }
  return $match.Matches[0].Groups[1].Value
}

function Assert-VmVerifyBuildReady {
  param(
    [Parameter(Mandatory = $true)]
    [string]$BuildDirectory,
    [switch]$IncludeBench,
    [switch]$IncludeCompat,
    [AllowNull()]
    [pscustomobject]$CommandResult = $null
  )

  if ($null -eq $CommandResult) {
    $targets = @("azookey_tsf_tip", "azookey_inference_host", "azookey_diag")
    if ($IncludeBench) {
      $targets += "azookey_zenzai_bench"
    }
    if ($IncludeCompat) {
      $targets += "compat_test"
    }
    $previousNinjaStatus = $env:NINJA_STATUS
    try {
      # Keep the exact one-action check independent of the caller's display format.
      $env:NINJA_STATUS = '[%f/%t] '
      $output = & cmake --build $BuildDirectory --target $targets -- -n 2>&1
      $exitCode = $LASTEXITCODE
    } finally {
      $env:NINJA_STATUS = $previousNinjaStatus
    }
  } else {
    $output = $CommandResult.Output
    $exitCode = [int]$CommandResult.ExitCode
  }
  $outputText = ($output | Out-String).Trim()
  if ($exitCode -ne 0) {
    throw "Build freshness check failed for '$BuildDirectory' (exit $exitCode): $outputText"
  }
  $ready = $outputText -match '^ninja:\s+no work to do\.$'
  if ($IncludeBench -and $outputText -ceq '[1/1] Refreshing benchmark commit header') {
    # This description is shared with bench/CMakeLists.txt's custom target COMMENT.
    # The only allowed work is the write-if-different commit header generator.
    # A changed HEAD/override must still force a real build of its consumers.
    $cachePath = Join-Path $BuildDirectory "CMakeCache.txt"
    $source = Get-VmVerifyCacheValue -CachePath $cachePath -Name "CMAKE_HOME_DIRECTORY"
    $commit = Get-VmVerifyCacheValue -CachePath $cachePath -Name "AZOOKEY_BENCH_GIT_COMMIT"
    if (-not $commit -or $commit -match '^(?i:0|OFF|NO|FALSE|N|IGNORE|NOTFOUND)$' -or
        $commit -match '(?i)-NOTFOUND$') {
      $commit = Get-VmVerifyGitCommit -RepositoryRoot $source
    }
    $header = Join-Path $BuildDirectory "bench\generated\BenchmarkCommit.h"
    $expected = "#pragma once`n#define AZOOKEY_BENCH_COMMIT `"$commit`"`n"
    $ready = (Test-Path -LiteralPath $header -PathType Leaf) -and
      ([System.IO.File]::ReadAllText($header).Replace("`r`n", "`n") -ceq $expected)
  }
  if (-not $ready) {
    throw ("Build artifacts are stale for '$BuildDirectory'. " +
      "Run the preset build before packaging. Dry-run output: $outputText")
  }
}

function Get-VmVerifyGitCommit {
  param(
    [Parameter(Mandatory = $true)]
    [string]$RepositoryRoot,
    [AllowNull()]
    [pscustomobject]$CommandResult = $null
  )

  if ($null -eq $CommandResult) {
    $commit = (& git -C $RepositoryRoot rev-parse HEAD 2>&1 | Out-String).Trim()
    $exitCode = $LASTEXITCODE
  } else {
    $commit = ($CommandResult.Output | Out-String).Trim()
    $exitCode = [int]$CommandResult.ExitCode
  }
  if ($exitCode -ne 0 -or $commit -notmatch '^[0-9a-fA-F]{40}$') {
    throw "Unable to resolve the repository commit: $commit"
  }
  return $commit.ToLowerInvariant()
}

function Assert-VmVerifyWorktreeClean {
  param(
    [Parameter(Mandatory = $true)]
    [string]$RepositoryRoot,
    [AllowNull()]
    [pscustomobject]$CommandResult = $null
  )

  if ($null -eq $CommandResult) {
    $status = (& git -C $RepositoryRoot status --porcelain --untracked-files=all 2>&1 |
      Out-String).Trim()
    $exitCode = $LASTEXITCODE
  } else {
    $status = ($CommandResult.Output | Out-String).Trim()
    $exitCode = [int]$CommandResult.ExitCode
  }
  if ($exitCode -ne 0) {
    throw "Unable to inspect the repository worktree: $status"
  }
  if ($status) {
    throw ("The repository worktree is not clean. Commit or remove local changes " +
      "before creating a commit-addressed VM verification package.")
  }
}

function Add-VmVerifyPayloadFile {
  param(
    [Parameter(Mandatory = $true)]
    [string]$SourcePath,
    [Parameter(Mandatory = $true)]
    [string]$ArchivePath,
    [Parameter(Mandatory = $true)]
    [string]$Role,
    [Parameter(Mandatory = $true)]
    [string]$StagingDirectory
  )

  if (-not (Test-Path -LiteralPath $SourcePath -PathType Leaf)) {
    throw "Required VM verification payload is missing ($Role): $SourcePath"
  }

  $destination = Join-Path $StagingDirectory ($ArchivePath.Replace("/", "\"))
  $parent = Split-Path -Parent $destination
  if ($parent) {
    New-Item -ItemType Directory -Path $parent -Force | Out-Null
  }
  Copy-Item -LiteralPath $SourcePath -Destination $destination -Force

  $item = Get-Item -LiteralPath $destination
  return [ordered]@{
    path = $ArchivePath
    role = $Role
    size = [int64]$item.Length
    sha256 = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
  }
}

function Compress-VmVerifyArchive {
  param(
    [Parameter(Mandatory = $true)]
    [string]$StagingDirectory,
    [Parameter(Mandatory = $true)]
    [string]$DestinationPath
  )

  Add-Type -AssemblyName System.IO.Compression
  Add-Type -AssemblyName System.IO.Compression.FileSystem
  $fileStream = [System.IO.File]::Open(
    $DestinationPath,
    [System.IO.FileMode]::Create,
    [System.IO.FileAccess]::ReadWrite,
    [System.IO.FileShare]::None)
  try {
    $archive = [System.IO.Compression.ZipArchive]::new(
      $fileStream,
      [System.IO.Compression.ZipArchiveMode]::Create,
      $false)
    try {
      foreach ($file in Get-ChildItem -LiteralPath $StagingDirectory -File -Recurse) {
        $entryName = $file.FullName.Substring($StagingDirectory.Length).
          TrimStart("\").Replace("\", "/")
        $compression = [System.IO.Compression.CompressionLevel]::Optimal
        if ($file.Extension -ieq ".gguf") {
          # GGUF is already dense and can exceed Compress-Archive's 2 GB limit.
          $compression = [System.IO.Compression.CompressionLevel]::NoCompression
        }
        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
          $archive,
          $file.FullName,
          $entryName,
          $compression) | Out-Null
      }
    } finally {
      $archive.Dispose()
    }
  } finally {
    $fileStream.Dispose()
  }
}

function Export-VmVerifyPackage {
  param(
    [Parameter(Mandatory = $true)]
    [string]$RepositoryRoot,
    [Parameter(Mandatory = $true)]
    [string]$PresetName,
    [Parameter(Mandatory = $true)]
    [string]$DestinationDirectory,
    [string]$MockDictionary = "",
    [string]$Model = "",
    [switch]$AllowNoModel,
    [switch]$IncludeCompat,
    [string]$RuntimeInstaller = ""
  )

  if (-not $Model -and -not $AllowNoModel) {
    throw "Specify -ModelPath with a GGUF model, or explicitly use -AllowNoModel for a model-free package."
  }

  $repository = (Resolve-Path -LiteralPath $RepositoryRoot).Path
  $presetsPath = Join-Path $repository "CMakePresets.json"
  $configuration = Get-VmVerifyPresetConfiguration `
    -PresetsPath $presetsPath `
    -PresetName $PresetName `
    -RepositoryRoot $repository

  $expectedBuildType = [string]$configuration.CacheVariables["CMAKE_BUILD_TYPE"]
  if (-not $expectedBuildType) {
    throw "CMake configure preset '$PresetName' does not define CMAKE_BUILD_TYPE."
  }

  $cachePath = Join-Path $configuration.BinaryDir "CMakeCache.txt"
  if (-not (Test-Path -LiteralPath $cachePath -PathType Leaf)) {
    throw "CMake cache is missing for preset '$PresetName': $cachePath"
  }

  $actualBuildType = Get-VmVerifyCacheValue -CachePath $cachePath -Name "CMAKE_BUILD_TYPE"
  if ($actualBuildType -cne $expectedBuildType) {
    throw ("Preset/build mismatch: preset '$PresetName' expects CMAKE_BUILD_TYPE=" +
      "'$expectedBuildType', but the cache contains '$actualBuildType'.")
  }

  $cacheSource = Get-VmVerifyCacheValue -CachePath $cachePath -Name "CMAKE_HOME_DIRECTORY"
  if (-not $cacheSource -or
      -not [string]::Equals(
        (Get-VmVerifyAbsolutePath -Path $cacheSource).TrimEnd("\"),
        $repository.TrimEnd("\"),
        [StringComparison]::OrdinalIgnoreCase)) {
    throw "CMake cache source mismatch: expected '$repository', found '$cacheSource'."
  }

  $tipDll = Join-Path $configuration.BinaryDir "tsf-tip\azookey_tsf_tip.dll"
  $buildPrerequisites = [ordered]@{
    AZOOKEY_FETCH_LLAMA_CPP = Get-VmVerifyCacheValue -CachePath $cachePath -Name "AZOOKEY_FETCH_LLAMA_CPP"
    AZOOKEY_LLAMA_CPP_SOURCE_DIR = Get-VmVerifyCacheValue -CachePath $cachePath -Name "AZOOKEY_LLAMA_CPP_SOURCE_DIR"
  }
  $llamaEnabled = $false
  foreach ($value in $buildPrerequisites.Values) {
    if ($value -and $value -notmatch '^(?i:0|OFF|NO|FALSE|N|IGNORE|NOTFOUND)$' -and
        $value -notmatch '(?i)-NOTFOUND$') {
      $llamaEnabled = $true
    }
  }
  if ($Model -and -not $llamaEnabled) {
    throw ("Inference host is not linked with llama.cpp according to CMakeCache.txt. " +
      "Reconfigure with cmake --preset $PresetName -DAZOOKEY_FETCH_LLAMA_CPP=ON " +
      "(or set AZOOKEY_LLAMA_CPP_SOURCE_DIR), then rebuild before packaging.")
  }
  $hostExe = Join-Path $configuration.BinaryDir "inference-host\azookey_inference_host.exe"
  $diagExe = Join-Path $configuration.BinaryDir "diagnostics\azookey_diag.exe"
  $benchExe = Join-Path $configuration.BinaryDir "bench\azookey_zenzai_bench.exe"
  $compatExe = Join-Path $configuration.BinaryDir "compat-test\compat_test.exe"
  $compatTargets = Join-Path $repository "compat-test\targets"
  $requiredArtifacts = @($tipDll, $hostExe, $diagExe)
  if ($Model) {
    $requiredArtifacts += $benchExe
  }
  if ($IncludeCompat) {
    $requiredArtifacts += $compatExe
    if (-not (Test-Path -LiteralPath $compatTargets -PathType Container) -or
        -not (Get-ChildItem -LiteralPath $compatTargets -File -Recurse)) {
      throw "Required compat targets are missing: $compatTargets"
    }
  }
  foreach ($artifact in $requiredArtifacts) {
    if (-not (Test-Path -LiteralPath $artifact -PathType Leaf)) {
      throw "Required build artifact is missing for preset '$PresetName': $artifact"
    }
  }
  Assert-VmVerifyBuildReady `
    -BuildDirectory $configuration.BinaryDir `
    -IncludeBench:([bool]$Model) `
    -IncludeCompat:$IncludeCompat

  foreach ($optionalPath in @($MockDictionary, $Model, $RuntimeInstaller)) {
    if ($optionalPath -and -not (Test-Path -LiteralPath $optionalPath -PathType Leaf)) {
      throw "Optional payload path does not exist: $optionalPath"
    }
  }
  if ($MockDictionary -and [System.IO.Path]::GetExtension($MockDictionary) -ine ".tsv") {
    throw "Mock dictionary must be a TSV file: $MockDictionary"
  }
  if ($Model -and [System.IO.Path]::GetExtension($Model) -ine ".gguf") {
    throw "Model must be a GGUF file: $Model"
  }

  Assert-VmVerifyWorktreeClean -RepositoryRoot $repository
  $commit = Get-VmVerifyGitCommit -RepositoryRoot $repository
  $shortCommit = $commit.Substring(0, 12)
  $destination = Get-VmVerifyAbsolutePath -Path $DestinationDirectory
  New-Item -ItemType Directory -Path $destination -Force | Out-Null

  $baseName = "azookey-verify-$shortCommit-$PresetName"
  $zipPath = Join-Path $destination "$baseName.zip"
  $manifestPath = Join-Path $destination "$baseName.manifest.json"
  $staging = Join-Path $destination (".$baseName-staging-$([guid]::NewGuid().ToString('N'))")
  New-Item -ItemType Directory -Path $staging | Out-Null

  try {
    $payloads = @(
      @{ Source = $tipDll; Archive = "azookey_tsf_tip.dll"; Role = "tip-dll" }
      @{ Source = $hostExe; Archive = "azookey_inference_host.exe"; Role = "inference-host" }
      @{ Source = $diagExe; Archive = "azookey_diag.exe"; Role = "runtime-diagnostics" }
      @{ Source = (Join-Path $repository "scripts\register-dev.ps1"); Archive = "register-dev.ps1"; Role = "registration-script" }
      @{ Source = (Join-Path $repository "scripts\unregister-dev.ps1"); Archive = "unregister-dev.ps1"; Role = "unregistration-script" }
      @{ Source = (Join-Path $repository "scripts\host-supervisor.ps1"); Archive = "host-supervisor.ps1"; Role = "host-supervisor-script" }
      @{ Source = (Join-Path $repository "scripts\AppContainerAcl.ps1"); Archive = "AppContainerAcl.ps1"; Role = "registration-dependency" }
      @{ Source = (Join-Path $repository "scripts\verify-bootstrap.ps1"); Archive = "verify-bootstrap.ps1"; Role = "vm-bootstrap-script" }
      @{ Source = (Join-Path $repository "docs\handoff\dev32-verification-checklist.md"); Archive = "dev32-verification-checklist.md"; Role = "verification-checklist" }
    )
    if ($IncludeCompat) {
      $payloads += @{ Source = $compatExe; Archive = "compat_test.exe"; Role = "compat-runner" }
      foreach ($target in Get-ChildItem -LiteralPath $compatTargets -File -Recurse | Sort-Object FullName) {
        $relative = $target.FullName.Substring($compatTargets.Length).TrimStart("\").Replace("\", "/")
        $payloads += @{ Source = $target.FullName; Archive = "targets/$relative"; Role = "compat-targets" }
      }
    }
    if ($MockDictionary) {
      $payloads += @{
        Source = $MockDictionary
        Archive = "data/$([System.IO.Path]::GetFileName($MockDictionary))"
        Role = "mock-dictionary"
      }
    }
    if ($Model) {
      $payloads += @{
        Source = $Model
        Archive = "models/$([System.IO.Path]::GetFileName($Model))"
        Role = "gguf-model"
      }
      $payloads += @{
        Source = $benchExe
        Archive = "azookey_zenzai_bench.exe"
        Role = "llama-preflight"
      }
    }
    if ($RuntimeInstaller) {
      $payloads += @{
        Source = $RuntimeInstaller
        Archive = "vc_redist.x64.exe"
        Role = "vc-redist"
      }
    }

    $files = @()
    foreach ($payload in $payloads) {
      $files += Add-VmVerifyPayloadFile `
        -SourcePath $payload.Source `
        -ArchivePath $payload.Archive `
        -Role $payload.Role `
        -StagingDirectory $staging
    }

    $manifest = [ordered]@{
      schemaVersion = 1
      commit = $commit
      preset = $PresetName
      buildType = $actualBuildType
      buildPrerequisites = $buildPrerequisites
      generatedAtUtc = [DateTimeOffset]::UtcNow.ToString("o")
      files = @($files | Sort-Object path)
    }
    $manifestJson = $manifest | ConvertTo-Json -Depth 6
    $utf8WithoutBom = [System.Text.UTF8Encoding]::new($false)
    [System.IO.File]::WriteAllText(
      (Join-Path $staging "manifest.json"),
      $manifestJson,
      $utf8WithoutBom)
    [System.IO.File]::WriteAllText($manifestPath, $manifestJson, $utf8WithoutBom)

    Compress-VmVerifyArchive -StagingDirectory $staging -DestinationPath $zipPath

    return [pscustomobject][ordered]@{
      ZipPath = $zipPath
      ManifestPath = $manifestPath
      Manifest = [pscustomobject]$manifest
    }
  } finally {
    if (Test-Path -LiteralPath $staging) {
      Remove-Item -LiteralPath $staging -Recurse -Force
    }
  }
}

if ($MyInvocation.InvocationName -ne ".") {
  $repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
  if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $repositoryRoot "build\vm-verify-packages"
  }

  $result = Export-VmVerifyPackage `
    -RepositoryRoot $repositoryRoot `
    -PresetName $Preset `
    -DestinationDirectory $OutputDirectory `
    -MockDictionary $MockDictionaryPath `
    -Model $ModelPath `
    -AllowNoModel:$AllowNoModel `
    -IncludeCompat:$IncludeCompat `
    -RuntimeInstaller $RuntimeInstallerPath

  Write-Output "VM verification package: $($result.ZipPath)"
  Write-Output "Manifest: $($result.ManifestPath)"
}
