#requires -Version 7.0
<#
.SYNOPSIS
  1 回の VM 検証セッションが出した機械可読な成果物を 1 つの検証サマリへ束ねる。

.DESCRIPTION
  検証パッケージの manifest.json（commit / preset）を軸に、次の入力を
  verification-summary.json と verification-summary.md へ集約する。

    -BootstrapJsonPath : verify-bootstrap.ps1 -Json の出力
    -DiagJsonPath      : azookey_diag.exe --json の出力
    -CompatReportPath  : compat_test.exe の report.json（target ごとに複数可）
    -OsBuild           : 検証 VM の OS ビルド番号（VM 内の winver の値。例: 26100.4061）

  サマリは観測値の集約であり、人間ゲートの合否を表さない。全体の合否欄は持たず、
  pass / fail / failing-skip / 人間待ち / warning / 対象外 / 不明を分けて数える。
  指定されなかった入力、見つからない入力、読めない入力は欠落として明示する。

  VM から回収した成果物をホスト側でまとめる用途のため PowerShell 7 で実行する。
  Windows PowerShell 5.1 は BOM なし UTF-8 の日本語文字列を読めない。
  OS ビルド番号はホストのものと取り違えないよう自動取得せず、明示指定だけを受け付ける。

  入力本文・ログ本文・ユーザー名を含みうる絶対パスはサマリへ出さない。
  各入力からは既知のフィールドだけを拾い、自由文の message はパスを [path] へ
  置換して短く切る。azookey_diag の details と bootstrap の hostBinary のパスは
  拾わない。
#>
param(
  [string]$ManifestPath = "",
  [string]$BootstrapJsonPath = "",
  [string]$DiagJsonPath = "",
  [string[]]$CompatReportPath = @(),
  [string]$OsBuild = "",
  [string]$OutputDirectory = ""
)

$ErrorActionPreference = "Stop"

$script:VmVerifySummaryNotice =
  "Aggregated observations only. This summary does not decide any human gate."

# 各系統の status を共通の分類へ写す。未知の値は unrecognized として別に数え、
# pass 側へ丸めない。
$script:VmVerifySummaryStatusMap = @{
  bootstrap = @{
    "pass" = "pass"
    "fail" = "fail"
    "manual_required" = "manualRequired"
    "not_applicable" = "notApplicable"
  }
  diag = @{
    "ok" = "pass"
    "error" = "fail"
    "warning" = "warning"
  }
  compat = @{
    "pass" = "pass"
    "fail" = "fail"
    "failing-skip" = "failingSkip"
  }
}

$script:VmVerifySummaryCountKeys = @(
  "pass", "fail", "failingSkip", "manualRequired", "warning", "notApplicable", "unrecognized")

function Get-VmVerifySummaryEmptyCount {
  $counts = [ordered]@{}
  foreach ($key in $script:VmVerifySummaryCountKeys) {
    $counts[$key] = 0
  }
  return $counts
}

function Add-VmVerifySummaryCount {
  param(
    [Parameter(Mandatory = $true)]
    $Target,
    [Parameter(Mandatory = $true)]
    $Source
  )

  foreach ($key in $script:VmVerifySummaryCountKeys) {
    $Target[$key] += $Source.$key
  }
}

function Get-VmVerifySummaryCategory {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Source,
    [AllowEmptyString()]
    [string]$Status
  )

  $map = $script:VmVerifySummaryStatusMap[$Source]
  if ($Status -and $map.ContainsKey($Status)) {
    return $map[$Status]
  }
  return "unrecognized"
}

# 識別子として出す値（check ID、status、target ID など）は固定の文字種に限る。
# 外れた値は中身を出さず redacted に置き換える。
function ConvertTo-VmVerifySummaryToken {
  param(
    [AllowNull()]
    $Value,
    [int]$MaxLength = 64
  )

  $text = [string]$Value
  if (-not $text) {
    return ""
  }
  if ($text.Length -le $MaxLength -and $text -match '^[A-Za-z0-9][A-Za-z0-9._-]*$') {
    return $text
  }
  return "redacted"
}

# 自由文は 1 行へ畳み、パスを [path] へ置換し、長さを切る。ログ本文が紛れ込んでも
# サマリを膨らませない。ユーザー名は空白を含みうるため、パスを空白で区切って
# 判定しない。引用符内のパス、プロファイル配下の残り全体、区切り文字を含む語を順に潰す。
function ConvertTo-VmVerifySummarySafeText {
  param(
    [AllowNull()]
    $Value,
    [int]$MaxLength = 200
  )

  $text = [string]$Value
  if (-not $text) {
    return ""
  }
  $text = $text -replace '[\r\n\t]+', ' '
  $text = $text -replace '(''|")[^''"]*[\\/][^''"]*\1', '$1[path]$1'
  $text = $text -replace '(?i)(?:[A-Z]:)?[\\/](?:Users|Documents and Settings)[\\/][^''"<>|]*', '[path]'
  $text = $text -replace '(?i)\\\\\?\\[^\s"''<>|]*', '[path]'
  $text = $text -replace '(?i)(?<![A-Za-z0-9])[A-Z]:[\\/][^\s"''<>|]*', '[path]'
  $text = $text -replace '\\\\[^\s"''<>|]+', '[path]'
  $text = $text -replace '[^\s"''<>|]*\\[^\s"''<>|]*', '[path]'
  $text = $text.Trim()
  if ($text.Length -gt $MaxLength) {
    $text = $text.Substring(0, $MaxLength) + "..."
  }
  return $text
}

function Get-VmVerifySummarySha256 {
  param(
    [AllowNull()]
    $Value
  )

  $text = [string]$Value
  if ($text -match '^[0-9A-Fa-f]{64}$') {
    return $text.ToLowerInvariant()
  }
  return ""
}

# 系統ごとの必須キー。別系統の JSON や途中で切れた出力を、件数 0 の「取得」として
# 扱わないために確認する。
$script:VmVerifySummaryRequiredKeys = @{
  bootstrap = @("overallStatus", "checks")
  diag = @("status", "checks")
  compat = @("target", "results")
}

# 入力ファイルを読む。未指定・不在・解析不能を区別して返し、例外にしない。
# PowerShell 5.1 の > リダイレクトは UTF-16 で書くため、BOM 判定のある
# Get-Content -Raw で読む。
function Read-VmVerifySummaryInput {
  param(
    [AllowEmptyString()]
    [string]$Path,
    [Parameter(Mandatory = $true)]
    [string]$Source
  )

  if (-not $Path) {
    return [pscustomobject][ordered]@{ State = "missing"; Reason = "not provided"; Data = $null }
  }
  if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
    return [pscustomobject][ordered]@{ State = "missing"; Reason = "file not found"; Data = $null }
  }
  try {
    $data = ConvertFrom-Json -InputObject (Get-Content -Raw -LiteralPath $Path) -NoEnumerate
  } catch {
    return [pscustomobject][ordered]@{ State = "invalid"; Reason = "not valid JSON"; Data = $null }
  }
  if ($null -eq $data -or $data -isnot [System.Management.Automation.PSCustomObject]) {
    return [pscustomobject][ordered]@{ State = "invalid"; Reason = "not a JSON object"; Data = $null }
  }
  $names = @($data.PSObject.Properties.Name)
  foreach ($key in $script:VmVerifySummaryRequiredKeys[$Source]) {
    if ($names -notcontains $key) {
      return [pscustomobject][ordered]@{ State = "invalid"; Reason = "unexpected shape"; Data = $null }
    }
  }
  return [pscustomobject][ordered]@{ State = "present"; Reason = ""; Data = $data }
}

# PowerShell 7 の ConvertFrom-Json は ISO 8601 の文字列をローカル時刻の DateTime へ
# 変換する。UTC の ISO 8601 へ戻してから出す。
function ConvertTo-VmVerifySummaryUtcText {
  param(
    [AllowNull()]
    $Value
  )

  if ($Value -is [datetime]) {
    return $Value.ToUniversalTime().ToString("o")
  }
  if ($Value -is [DateTimeOffset]) {
    return $Value.UtcDateTime.ToString("o")
  }
  return ConvertTo-VmVerifySummarySafeText -Value $Value -MaxLength 40
}

function Get-VmVerifySummaryInputState {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Name,
    [Parameter(Mandatory = $true)]
    [string]$State,
    [AllowEmptyString()]
    [string]$Reason = ""
  )

  return [pscustomobject][ordered]@{
    name = $Name
    state = $State
    reason = $Reason
  }
}

function Get-VmVerifySummaryManifest {
  param(
    [Parameter(Mandatory = $true)]
    [string]$ManifestPath
  )

  if (-not (Test-Path -LiteralPath $ManifestPath -PathType Leaf)) {
    throw "Package manifest was not found: $ManifestPath"
  }
  $manifest = Get-Content -Raw -LiteralPath $ManifestPath | ConvertFrom-Json
  if (-not $manifest.commit -or $manifest.commit -notmatch '^[0-9a-f]{40}$') {
    throw "Package manifest does not carry a full commit hash: $ManifestPath"
  }
  $preset = ConvertTo-VmVerifySummaryToken -Value $manifest.preset
  if (-not $preset -or $preset -eq "redacted") {
    throw "Package manifest does not carry a valid preset name: $ManifestPath"
  }

  # manifest の path は zip 内の相対パスだが、ファイル名だけを出す。
  $model = ""
  $roles = @()
  foreach ($file in @($manifest.files)) {
    if (-not $file) {
      continue
    }
    $roles += [string]$file.role
    if ($file.role -eq "gguf-model" -and -not $model) {
      $leaf = ([string]$file.path) -split '[\\/]' | Select-Object -Last 1
      $model = ConvertTo-VmVerifySummaryToken -Value $leaf -MaxLength 128
    }
  }

  return [pscustomobject][ordered]@{
    commit = [string]$manifest.commit
    preset = $preset
    buildType = ConvertTo-VmVerifySummaryToken -Value $manifest.buildType
    generatedAtUtc = ConvertTo-VmVerifySummaryUtcText -Value $manifest.generatedAtUtc
    bundledModel = $model
    compatBundled = ($roles -contains "compat-runner")
  }
}

function Get-VmVerifySummaryOsBuild {
  param(
    [AllowEmptyString()]
    [string]$OsBuild = ""
  )

  if (-not $OsBuild) {
    return [pscustomobject][ordered]@{ State = "missing"; Reason = "not provided"; Build = "" }
  }
  $build = $OsBuild.Trim()
  if ($build -notmatch '^\d{4,6}(\.\d{1,6})?$') {
    return [pscustomobject][ordered]@{ State = "invalid"; Reason = "not a Windows build number"; Build = "" }
  }
  return [pscustomobject][ordered]@{ State = "present"; Reason = ""; Build = $build }
}

function Get-VmVerifySummaryBootstrap {
  param(
    [Parameter(Mandatory = $true)]
    $Data,
    [Parameter(Mandatory = $true)]
    [string]$ManifestCommit
  )

  $counts = Get-VmVerifySummaryEmptyCount
  $checks = @()
  foreach ($check in @($Data.checks)) {
    if (-not $check) {
      continue
    }
    $status = [string]$check.status
    $category = Get-VmVerifySummaryCategory -Source "bootstrap" -Status $status
    $counts[$category] += 1
    $checks += [pscustomobject][ordered]@{
      id = ConvertTo-VmVerifySummaryToken -Value $check.id
      status = ConvertTo-VmVerifySummaryToken -Value $status
      category = $category
      message = ConvertTo-VmVerifySummarySafeText -Value $check.message
    }
  }

  # 取り違えた zip の結果を束ねないよう、bootstrap が読んだ manifest の commit と
  # 突き合わせる。判定はせず、一致・不一致・記録なしを観測として残す。
  $packageCommit = [string]$Data.package.commit
  $commitMatch = "unknown"
  if ($packageCommit) {
    $commitMatch = $(if ($packageCommit -eq $ManifestCommit) { "match" } else { "mismatch" })
  }

  $hostBinary = $null
  if ($Data.hostBinary) {
    $hostBinary = [pscustomobject][ordered]@{
      status = ConvertTo-VmVerifySummaryToken -Value $Data.hostBinary.status
      runningSha256 = Get-VmVerifySummarySha256 -Value $Data.hostBinary.runningSha256
      expectedSha256 = Get-VmVerifySummarySha256 -Value $Data.hostBinary.expectedSha256
    }
  }

  return [pscustomobject][ordered]@{
    overallStatus = ConvertTo-VmVerifySummaryToken -Value $Data.overallStatus
    packageCommitVsManifest = $commitMatch
    hostBinary = $hostBinary
    counts = [pscustomobject]$counts
    checks = @($checks)
  }
}

function Get-VmVerifySummaryDiag {
  param(
    [Parameter(Mandatory = $true)]
    $Data
  )

  $counts = Get-VmVerifySummaryEmptyCount
  $checks = @()
  foreach ($check in @($Data.checks)) {
    if (-not $check) {
      continue
    }
    $status = [string]$check.status
    $category = Get-VmVerifySummaryCategory -Source "diag" -Status $status
    $counts[$category] += 1
    # details はパスや設定値を含むので拾わない。
    $checks += [pscustomobject][ordered]@{
      id = ConvertTo-VmVerifySummaryToken -Value $check.id
      name = ConvertTo-VmVerifySummaryToken -Value $check.name
      status = ConvertTo-VmVerifySummaryToken -Value $status
      category = $category
      message = ConvertTo-VmVerifySummarySafeText -Value $check.message
    }
  }

  return [pscustomobject][ordered]@{
    status = ConvertTo-VmVerifySummaryToken -Value $Data.status
    counts = [pscustomobject]$counts
    checks = @($checks)
  }
}

function Get-VmVerifySummaryCompatReport {
  param(
    [Parameter(Mandatory = $true)]
    $Data
  )

  # report.json の summary は信用せず results から数え直す。
  $counts = Get-VmVerifySummaryEmptyCount
  $results = @()
  foreach ($result in @($Data.results)) {
    if (-not $result) {
      continue
    }
    $status = [string]$result.status
    $category = Get-VmVerifySummaryCategory -Source "compat" -Status $status
    $counts[$category] += 1

    $artifact = [string]$result.artifact
    if ($artifact -notmatch '^[A-Za-z0-9._-]+(/[A-Za-z0-9._-]+)*$' -or $artifact -match '(^|/)\.\.(/|$)') {
      $artifact = ""
    }
    $duration = 0
    if ($null -ne $result.duration_ms) {
      [void][long]::TryParse([string]$result.duration_ms, [ref]$duration)
    }
    $results += [pscustomobject][ordered]@{
      id = ConvertTo-VmVerifySummaryToken -Value $result.id
      status = ConvertTo-VmVerifySummaryToken -Value $status
      category = $category
      reasonCode = ConvertTo-VmVerifySummaryToken -Value $result.reason_code
      durationMs = $duration
      artifact = $artifact
    }
  }

  # runner が書いた summary と数え直した件数の食い違いは、観測値として残す。
  $reportedSummaryMatches = $null
  if ($Data.summary) {
    $reportedSummaryMatches = (
      [string]$Data.summary.pass -eq [string]$counts["pass"] -and
      [string]$Data.summary.fail -eq [string]$counts["fail"] -and
      [string]$Data.summary.failing_skip -eq [string]$counts["failingSkip"])
  }

  return [pscustomobject][ordered]@{
    targetId = ConvertTo-VmVerifySummaryToken -Value $Data.target.id
    displayName = ConvertTo-VmVerifySummarySafeText -Value $Data.target.display_name -MaxLength 80
    automationLevel = ConvertTo-VmVerifySummaryToken -Value $Data.target.automation_level
    reportedSummaryMatches = $reportedSummaryMatches
    counts = [pscustomobject]$counts
    results = @($results)
  }
}

function Get-VmVerifySummary {
  param(
    [Parameter(Mandatory = $true)]
    [string]$ManifestPath,
    [AllowEmptyString()]
    [string]$BootstrapJsonPath = "",
    [AllowEmptyString()]
    [string]$DiagJsonPath = "",
    [AllowEmptyCollection()]
    [string[]]$CompatReportPath = @(),
    [AllowEmptyString()]
    [string]$OsBuild = ""
  )

  $package = Get-VmVerifySummaryManifest -ManifestPath $ManifestPath
  $os = Get-VmVerifySummaryOsBuild -OsBuild $OsBuild
  $inputs = @(Get-VmVerifySummaryInputState -Name "manifest" -State "present")
  $inputs += Get-VmVerifySummaryInputState -Name "osBuild" -State $os.State -Reason $os.Reason

  $total = Get-VmVerifySummaryEmptyCount

  $bootstrapInput = Read-VmVerifySummaryInput -Path $BootstrapJsonPath -Source "bootstrap"
  $inputs += Get-VmVerifySummaryInputState `
    -Name "bootstrap" -State $bootstrapInput.State -Reason $bootstrapInput.Reason
  $bootstrap = $null
  if ($bootstrapInput.State -eq "present") {
    $bootstrap = Get-VmVerifySummaryBootstrap -Data $bootstrapInput.Data -ManifestCommit $package.commit
    # 別 commit のパッケージで得た結果は、この manifest を軸にした合計へ混ぜない。
    # 系統ごとの件数と不一致の事実は bootstrap 側に残す。
    if ($bootstrap.packageCommitVsManifest -ne "mismatch") {
      Add-VmVerifySummaryCount -Target $total -Source $bootstrap.counts
    }
  }

  $diagInput = Read-VmVerifySummaryInput -Path $DiagJsonPath -Source "diag"
  $inputs += Get-VmVerifySummaryInputState -Name "diag" -State $diagInput.State -Reason $diagInput.Reason
  $diag = $null
  if ($diagInput.State -eq "present") {
    $diag = Get-VmVerifySummaryDiag -Data $diagInput.Data
    Add-VmVerifySummaryCount -Target $total -Source $diag.counts
  }

  $compat = @()
  $compatPaths = @($CompatReportPath | Where-Object { $_ })
  if ($compatPaths.Count -eq 0) {
    $inputs += Get-VmVerifySummaryInputState -Name "compat" -State "missing" -Reason "not provided"
  }
  for ($index = 0; $index -lt $compatPaths.Count; $index++) {
    $compatInput = Read-VmVerifySummaryInput -Path $compatPaths[$index] -Source "compat"
    $inputs += Get-VmVerifySummaryInputState `
      -Name "compat[$index]" -State $compatInput.State -Reason $compatInput.Reason
    if ($compatInput.State -eq "present") {
      $report = Get-VmVerifySummaryCompatReport -Data $compatInput.Data
      Add-VmVerifySummaryCount -Target $total -Source $report.counts
      $compat += $report
    }
  }

  $missing = @($inputs | Where-Object { $_.state -ne "present" } | ForEach-Object { $_.name })
  $total["missingInputs"] = $missing.Count

  return [pscustomobject][ordered]@{
    schemaVersion = 1
    generatedAtUtc = [DateTimeOffset]::UtcNow.ToString("o")
    notice = $script:VmVerifySummaryNotice
    package = $package
    os = [pscustomobject][ordered]@{
      build = $os.Build
    }
    inputs = @($inputs)
    missingInputs = @($missing)
    counts = [pscustomobject]$total
    bootstrap = $bootstrap
    diag = $diag
    compat = @($compat)
  }
}

function ConvertTo-VmVerifySummaryCell {
  param(
    [AllowNull()]
    $Value
  )

  # GitHub と Linear の Markdown は <...> をタグとして落とすため、実体参照にする。
  return ([string]$Value).Replace("|", "\|").Replace("<", "&lt;").Replace(">", "&gt;")
}

function Format-VmVerifySummaryCountRow {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Label,
    [Parameter(Mandatory = $true)]
    [AllowEmptyString()]
    [string]$State,
    $Counts
  )

  if ($null -eq $Counts) {
    return "| $Label | $State | - | - | - | - | - | - | - |"
  }
  return ("| {0} | {1} | {2} | {3} | {4} | {5} | {6} | {7} | {8} |" -f
    $Label, $State, $Counts.pass, $Counts.fail, $Counts.failingSkip, $Counts.manualRequired,
    $Counts.warning, $Counts.notApplicable, $Counts.unrecognized)
}

function Get-VmVerifySummaryInputLabel {
  param(
    [Parameter(Mandatory = $true)]
    $Summary,
    [Parameter(Mandatory = $true)]
    [string]$Name
  )

  $entry = @($Summary.inputs | Where-Object { $_.name -eq $Name }) | Select-Object -First 1
  if (-not $entry) {
    return "未取得"
  }
  switch ($entry.state) {
    "present" { return "取得" }
    "invalid" { return "読めない ($($entry.reason))" }
    default { return "未取得 ($($entry.reason))" }
  }
}

# human-gate-batch-runbook.md Part C の環境ブロックへ機械で埋まる欄だけを入れる。
# 検証日・検証者・branch・MSI・checkpoint・バックエンドの選択は人が埋める。
function ConvertTo-VmVerifySummaryMarkdown {
  param(
    [Parameter(Mandatory = $true)]
    $Summary
  )

  $package = $Summary.package
  $shortCommit = $package.commit.Substring(0, 12)
  $osLine = "（未取得）"
  if ($Summary.os.build) {
    $osLine = $Summary.os.build
  }
  $modelNote = "manifest 同梱モデルなし"
  if ($package.bundledModel) {
    $modelNote = "manifest 同梱モデル: ``$($package.bundledModel)``"
  }

  $lines = [System.Collections.Generic.List[string]]::new()
  $lines.Add("<!-- azookey-verification-summary:$shortCommit-$($package.preset) -->")
  $lines.Add("## 検証環境")
  $lines.Add("- 検証日 / 検証者:")
  $lines.Add("- OS build (``winver``): $osLine")
  $lines.Add("- source: branch / commit:  / $($package.commit)")
  $lines.Add("- 成果物: ☐ MSI (ファイル名 / SHA-256) ☐ 検証 zip (``manifest.json`` の commit): " +
    "$($package.commit) / preset ``$($package.preset)`` / $($package.buildType)")
  $lines.Add("- VM: Hyper-V / セッション種別 = 基本セッション")
  $lines.Add("- 開始 checkpoint 名: ☐ vc_redist 未導入のクリーン（レーン 1） " +
    "☐ plan §2 のベースライン（レーン 2）/ 名前:")
  $lines.Add("- バックエンド: ☐ CPU (SimpleConverter) ☐ zenz GGUF (ファイル名)（$modelNote）")
  $lines.Add("")
  $lines.Add("## 自動観測サマリ")
  $lines.Add("")
  $lines.Add("> 観測値の集約であり、人間ゲートの合否を表さない。" +
    "failing-skip・人間待ち・warning・欠落は pass に含めない。")
  $lines.Add("")
  $lines.Add("| 系統 | 入力 | pass | fail | failing-skip | 人間待ち | warning | 対象外 | 不明 |")
  $lines.Add("|---|---|---:|---:|---:|---:|---:|---:|---:|")
  $bootstrapCounts = $null
  $bootstrapLabel = "bootstrap (``verify-bootstrap.ps1 -Json``)"
  if ($Summary.bootstrap) {
    $bootstrapCounts = $Summary.bootstrap.counts
    if ($Summary.bootstrap.packageCommitVsManifest -eq "mismatch") {
      $bootstrapLabel = "$bootstrapLabel（commit 不一致のため合計に含めない）"
    }
  }
  $lines.Add((Format-VmVerifySummaryCountRow `
    -Label $bootstrapLabel `
    -State (Get-VmVerifySummaryInputLabel -Summary $Summary -Name "bootstrap") `
    -Counts $bootstrapCounts))
  $diagCounts = $null
  if ($Summary.diag) {
    $diagCounts = $Summary.diag.counts
  }
  $lines.Add((Format-VmVerifySummaryCountRow `
    -Label "diag (``azookey_diag.exe --json``)" `
    -State (Get-VmVerifySummaryInputLabel -Summary $Summary -Name "diag") `
    -Counts $diagCounts))
  if (@($Summary.compat).Count -eq 0) {
    $compatInputs = @($Summary.inputs | Where-Object { $_.name -like "compat*" })
    $state = "未取得"
    if ($compatInputs.Count -gt 0) {
      $state = Get-VmVerifySummaryInputLabel -Summary $Summary -Name $compatInputs[0].name
    }
    $lines.Add((Format-VmVerifySummaryCountRow -Label "compat" -State $state -Counts $null))
  }
  foreach ($report in @($Summary.compat)) {
    $compatLabel = "compat ``$($report.targetId)``"
    if ($report.reportedSummaryMatches -eq $false) {
      $compatLabel = "$compatLabel（report.json の summary と results が食い違う）"
    }
    $lines.Add((Format-VmVerifySummaryCountRow `
      -Label $compatLabel -State "取得" -Counts $report.counts))
  }
  $lines.Add((Format-VmVerifySummaryCountRow -Label "**合計**" -State "" -Counts $Summary.counts))
  $lines.Add("")

  $missingLabels = @($Summary.inputs | Where-Object { $_.state -ne "present" } | ForEach-Object {
      "$($_.name) ($($_.state): $($_.reason))"
    })
  if ($missingLabels.Count -gt 0) {
    $lines.Add("- 欠落した入力: " + ($missingLabels -join ", "))
  } else {
    $lines.Add("- 欠落した入力: なし")
  }
  if ($Summary.bootstrap) {
    $commitLabel = switch ($Summary.bootstrap.packageCommitVsManifest) {
      "match" { "一致" }
      "mismatch" { "**不一致**" }
      default { "bootstrap 側に記録なし" }
    }
    $lines.Add("- bootstrap の package.commit と manifest の commit: $commitLabel")
    $lines.Add("- bootstrap overallStatus: ``$($Summary.bootstrap.overallStatus)``")
    if ($Summary.bootstrap.hostBinary) {
      $lines.Add("- bootstrap hostBinary.status: ``$($Summary.bootstrap.hostBinary.status)``")
    }
  }
  if ($Summary.diag) {
    $lines.Add("- diag status: ``$($Summary.diag.status)``")
  }

  if ($Summary.bootstrap -and @($Summary.bootstrap.checks).Count -gt 0) {
    $lines.Add("")
    $lines.Add("<details>")
    $lines.Add("<summary>bootstrap checks</summary>")
    $lines.Add("")
    $lines.Add("| ID | Status | Message |")
    $lines.Add("|---|---|---|")
    foreach ($check in @($Summary.bootstrap.checks)) {
      $lines.Add("| $($check.id) | $($check.status) | $(ConvertTo-VmVerifySummaryCell $check.message) |")
    }
    $lines.Add("")
    $lines.Add("</details>")
  }
  if ($Summary.diag -and @($Summary.diag.checks).Count -gt 0) {
    $lines.Add("")
    $lines.Add("<details>")
    $lines.Add("<summary>diag checks</summary>")
    $lines.Add("")
    $lines.Add("| ID | Name | Status | Message |")
    $lines.Add("|---|---|---|---|")
    foreach ($check in @($Summary.diag.checks)) {
      $lines.Add(("| {0} | {1} | {2} | {3} |" -f $check.id, $check.name, $check.status,
        (ConvertTo-VmVerifySummaryCell $check.message)))
    }
    $lines.Add("")
    $lines.Add("</details>")
  }
  foreach ($report in @($Summary.compat)) {
    if (@($report.results).Count -eq 0) {
      continue
    }
    $lines.Add("")
    $lines.Add("<details>")
    $lines.Add("<summary>compat $($report.targetId) cases</summary>")
    $lines.Add("")
    $lines.Add("| Case | Result | Reason | Duration (ms) | Artifact |")
    $lines.Add("|---|---|---|---:|---|")
    foreach ($result in @($report.results)) {
      $artifact = ""
      if ($result.artifact) {
        $artifact = "``$($result.artifact)``"
      }
      $lines.Add(("| {0} | {1} | {2} | {3} | {4} |" -f
        $result.id, $result.status, $result.reasonCode, $result.durationMs, $artifact))
    }
    $lines.Add("")
    $lines.Add("</details>")
  }

  return ($lines -join "`n") + "`n"
}

function Write-VmVerifySummaryFile {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path,
    [Parameter(Mandatory = $true)]
    [string]$Content
  )

  [System.IO.File]::WriteAllText($Path, $Content, (New-Object System.Text.UTF8Encoding($false)))
}

function Invoke-VmVerifySummary {
  param(
    [Parameter(Mandatory = $true)]
    [string]$ManifestPath,
    [AllowEmptyString()]
    [string]$BootstrapJsonPath = "",
    [AllowEmptyString()]
    [string]$DiagJsonPath = "",
    [AllowEmptyCollection()]
    [string[]]$CompatReportPath = @(),
    [AllowEmptyString()]
    [string]$OsBuild = "",
    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory
  )

  $summary = Get-VmVerifySummary `
    -ManifestPath $ManifestPath `
    -BootstrapJsonPath $BootstrapJsonPath `
    -DiagJsonPath $DiagJsonPath `
    -CompatReportPath $CompatReportPath `
    -OsBuild $OsBuild

  $directory = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory)
  New-Item -ItemType Directory -Path $directory -Force | Out-Null
  $jsonPath = Join-Path $directory "verification-summary.json"
  $markdownPath = Join-Path $directory "verification-summary.md"
  Write-VmVerifySummaryFile -Path $jsonPath -Content (($summary | ConvertTo-Json -Depth 8) + "`n")
  Write-VmVerifySummaryFile -Path $markdownPath -Content (ConvertTo-VmVerifySummaryMarkdown -Summary $summary)

  return [pscustomobject][ordered]@{
    JsonPath = $jsonPath
    MarkdownPath = $markdownPath
    Summary = $summary
  }
}

if ($MyInvocation.InvocationName -ne ".") {
  if (-not $ManifestPath) {
    throw "-ManifestPath is required. Pass the package manifest.json (or <zip basename>.manifest.json)."
  }
  if (-not $OutputDirectory) {
    throw "-OutputDirectory is required."
  }

  $result = Invoke-VmVerifySummary `
    -ManifestPath $ManifestPath `
    -BootstrapJsonPath $BootstrapJsonPath `
    -DiagJsonPath $DiagJsonPath `
    -CompatReportPath $CompatReportPath `
    -OsBuild $OsBuild `
    -OutputDirectory $OutputDirectory

  Write-Output "Summary JSON: $($result.JsonPath)"
  Write-Output "Summary Markdown: $($result.MarkdownPath)"
  if ($result.Summary.missingInputs.Count -gt 0) {
    Write-Output ("Missing inputs: " + ($result.Summary.missingInputs -join ", "))
  }
  Write-Output "This summary aggregates observations only. It does not decide any human gate."
}
