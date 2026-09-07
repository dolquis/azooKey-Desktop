#requires -Version 5.1
# Shared read-only validation for doctor and prepare-clangd. Never executes DB commands.
function Test-ClangdDatabase {
  param(
    [Parameter(Mandatory=$true)][string]$RepoRoot,
    [string[]]$RequiredSources = @(
      "core/src/RewriterIndex.cpp", "ipc/src/Json.cpp", "tsf-tip/src/TextService.cpp"
    )
  )
  $root = [IO.Path]::GetFullPath($RepoRoot).TrimEnd('\', '/')
  $build = Join-Path $root "build/clangd"
  $database = Join-Path $build "compile_commands.json"
  try {
    if (-not (Test-Path -LiteralPath $database -PathType Leaf)) {
      throw "Missing compilation database: $database"
    }
    $raw = Get-Content -Raw -LiteralPath $database
    if (-not $raw.TrimStart().StartsWith('[')) { throw "Database must be a JSON array." }
    $entries = @($raw | ConvertFrom-Json)
    if ($entries.Count -eq 0) { throw "Compilation database is empty." }
    $sources = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in $entries) {
      if (-not $entry.directory -or -not $entry.file -or
          (-not $entry.command -and -not $entry.arguments)) {
        throw "Invalid compilation database entry."
      }
      if (-not [IO.Path]::IsPathRooted($entry.directory) -or
          [IO.Path]::GetFullPath($entry.directory).TrimEnd('\', '/') -ne
          [IO.Path]::GetFullPath($build).TrimEnd('\', '/')) {
        throw "Database belongs to a different build directory: $($entry.directory)"
      }
      $source = if ([IO.Path]::IsPathRooted($entry.file)) { $entry.file } else {
        Join-Path $entry.directory $entry.file
      }
      $source = [IO.Path]::GetFullPath($source)
      if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "Source no longer exists: $source"
      }
      $null = $sources.Add($source)
    }
    foreach ($relative in $RequiredSources) {
      if (-not $sources.Contains([IO.Path]::GetFullPath((Join-Path $root $relative)))) {
        throw "Required source missing from database: $relative"
      }
    }
    # Reconfigure after build configuration changes; timestamps are local to this checkout.
    $dbTime = (Get-Item -LiteralPath $database).LastWriteTimeUtc
    $inputs = @(Get-Item -LiteralPath (Join-Path $root "CMakePresets.json") -ErrorAction SilentlyContinue)
    foreach ($directory in @(".", "core", "ipc", "learning", "inference-host", "tsf-tip")) {
      $inputs += Get-Item -LiteralPath (Join-Path (Join-Path $root $directory) "CMakeLists.txt") -ErrorAction SilentlyContinue
    }
    if (@($inputs | Where-Object LastWriteTimeUtc -gt $dbTime).Count -gt 0) {
      throw "Build configuration is newer than the compilation database."
    }
    [pscustomobject]@{ ok = $true; details = "Validated $($entries.Count) entries: $database" }
  } catch {
    [pscustomobject]@{ ok = $false; details = $_.Exception.Message }
  }
}
