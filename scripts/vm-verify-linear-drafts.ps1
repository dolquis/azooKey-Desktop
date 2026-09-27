#requires -Version 7.0
<#
.SYNOPSIS
  verification-summary.json とゲート ID 対応表から課題別の Linear 検証メモ下書きを作る。

.DESCRIPTION
  対応表は schemaVersion=1 と gates 配列を持つ JSON。各要素は source
  (bootstrap / diag / compat)、id、issueId (DEV-番号)、任意の targetId を持つ。
  compat の targetId を省いた場合は同じ case ID の全 target を集める。
  出力は issueId.md。summary の自由文、ユーザー入力本文、ローカル絶対パスは写さない。
  このスクリプトは合否を判定せず、Linear へ投稿しない。人間ゲートの合否判定と
  Linear への検証メモ投稿は人が行う。

.PARAMETER CheckpointName
  summary JSON には checkpoint 名がない。指定しない場合は空欄にする。
#>
param(
  [string]$SummaryPath = "",
  [string]$GateMapPath = "",
  [string]$OutputDirectory = "",
  [string]$CheckpointName = ""
)

$ErrorActionPreference = "Stop"

function Test-VmVerifyDraftToken {
  param([AllowNull()]$Value)
  return ([string]$Value -cmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$')
}

function Get-VmVerifyDraftData {
  param([string]$Path)
  if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
    throw "Required JSON file was not found."
  }
  try {
    $data = ConvertFrom-Json -InputObject (Get-Content -Raw -LiteralPath $Path) -NoEnumerate
  } catch {
    throw "Required file is not valid JSON."
  }
  if ($null -eq $data -or $data -isnot [System.Management.Automation.PSCustomObject]) {
    throw "Required JSON root must be an object."
  }
  return ,$data
}

function Get-VmVerifyDraftGate {
  param($Map)
  if ($Map.schemaVersion -ne 1 -or $null -eq $Map.gates -or
      $Map.gates -isnot [array] -or $Map.gates.Count -eq 0) {
    throw "Gate map must have schemaVersion 1 and a nonempty gates array."
  }
  $seen = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
  foreach ($gate in $Map.gates) {
    if ($null -eq $gate -or $gate.source -cnotin @("bootstrap", "diag", "compat") -or
        -not (Test-VmVerifyDraftToken $gate.id) -or
        [string]$gate.issueId -cnotmatch '^DEV-[1-9][0-9]*$' -or
        ($gate.targetId -and (-not (Test-VmVerifyDraftToken $gate.targetId) -or
          $gate.source -cne "compat"))) {
      throw "Gate map contains an invalid entry."
    }
    $key = "$($gate.source)|$($gate.id)|$($gate.targetId)|$($gate.issueId)"
    if (-not $seen.Add($key)) {
      throw "Gate map contains a duplicate entry."
    }
  }
  return @($Map.gates)
}

function Get-VmVerifyDraftStatus {
  param($Record)
  if ($null -eq $Record) { return "未取得" }
  if ($Record.status -in @("未取得", "未実行")) { return [string]$Record.status }
  if (-not (Test-VmVerifyDraftToken $Record.status)) { return "不明" }
  return [string]$Record.status
}

function Get-VmVerifyDraftRow {
  param($Summary, $Gate)
  $records = @()
  if ($Gate.source -eq "bootstrap" -and $Summary.bootstrap) {
    $records = @($Summary.bootstrap.checks | Where-Object { $_ -and $_.id -ceq $Gate.id })
    if ($Summary.bootstrap.packageCommitVsManifest -eq "mismatch") {
      $records = @($records | ForEach-Object {
          [pscustomobject]@{ targetId = ""; status = "excluded" }
        })
    }
  } elseif ($Gate.source -eq "diag" -and $Summary.diag) {
    $records = @($Summary.diag.checks | Where-Object { $_ -and $_.id -ceq $Gate.id })
  } elseif ($Gate.source -eq "compat") {
    foreach ($report in @($Summary.compat)) {
      if (-not $report -or ($Gate.targetId -and $report.targetId -cne $Gate.targetId)) {
        continue
      }
      $matching = @($report.results | Where-Object { $_ -and $_.id -ceq $Gate.id })
      foreach ($result in $matching) {
        $records += [pscustomobject]@{ targetId = $report.targetId; status = $result.status }
      }
      if ($matching.Count -eq 0) {
        $state = "未取得"
        if ($report.caseSelection -and @($report.caseSelection.excluded) -ccontains $Gate.id) {
          $state = "未実行"
        }
        $records += [pscustomobject]@{ targetId = $report.targetId; status = $state }
      }
    }
  }
  if ($records.Count -eq 0) {
    return @([pscustomobject]@{ targetId = $Gate.targetId; status = "未取得" })
  }
  return @($records)
}

function ConvertTo-VmVerifyLinearDraft {
  param($Summary, [string]$IssueId, [array]$Gates, [string]$CheckpointName)
  $commit = [string]$Summary.package.commit
  if ($commit -cnotmatch '^[0-9a-fA-F]{40}$') {
    throw "Summary package commit must be a full hash."
  }
  $osBuild = [string]$Summary.os.build
  if ($osBuild -notmatch '^\d{4,6}(\.\d{1,6})?$') { $osBuild = "" }
  $preset = [string]$Summary.package.preset
  if (-not (Test-VmVerifyDraftToken $preset)) { $preset = "" }

  $lines = [System.Collections.Generic.List[string]]::new()
  $lines.Add("# $IssueId 検証メモ（下書き）")
  $lines.Add("")
  $lines.Add("> 自動観測の転記です。人間ゲートの合否は判定していません。Linear への投稿前に人が確認します。")
  $lines.Add("")
  $lines.Add("## 検証環境")
  $lines.Add("- 検証日 / 検証者:")
  $lines.Add("- OS build (``winver``): $osBuild")
  $lines.Add("- source: branch / commit:  / $commit")
  $lines.Add("- 成果物: ☐ MSI (ファイル名 / SHA-256) ☐ 検証 zip (``manifest.json`` の commit): $commit / preset ``$preset``")
  $lines.Add("- VM: Hyper-V / セッション種別 = 基本セッション")
  $lines.Add("- 開始 checkpoint 名: $CheckpointName")
  $lines.Add("- バックエンド: ☐ CPU (SimpleConverter) ☐ zenz GGUF (ファイル名)")
  $lines.Add("")
  $lines.Add("## 自動判定の結果")
  $lines.Add("")
  $lines.Add("| 系統 | ゲート ID | target | status |")
  $lines.Add("|---|---|---|---|")
  foreach ($gate in $Gates) {
    foreach ($row in @(Get-VmVerifyDraftRow -Summary $Summary -Gate $gate)) {
      $target = [string]$row.targetId
      if ($target -and -not (Test-VmVerifyDraftToken $target)) { $target = "redacted" }
      $status = Get-VmVerifyDraftStatus -Record $row
      $lines.Add("| $($gate.source) | $($gate.id) | $target | $status |")
    }
  }
  if ($Summary.bootstrap -and $Summary.bootstrap.packageCommitVsManifest -eq "mismatch" -and
      @($Gates | Where-Object { $_.source -eq "bootstrap" }).Count -gt 0) {
    $lines.Add("")
    $lines.Add("- bootstrap の package.commit は manifest と不一致。bootstrap の観測結果を除外した。")
  }
  $lines.Add("")
  $lines.Add("## 人間待ち")
  foreach ($gate in $Gates) {
    foreach ($row in @(Get-VmVerifyDraftRow -Summary $Summary -Gate $gate)) {
      if ($row.status -eq "manual_required") {
        $target = [string]$row.targetId
        if ($target -and -not (Test-VmVerifyDraftToken $target)) { $target = "redacted" }
        $label = "$($gate.source) $($gate.id)"
        if ($target) { $label += " $target" }
        $lines.Add("- $label の人間確認:")
      }
    }
  }
  $lines.Add("- Part C の当該課題の確認項目と結果:")
  $lines.Add("- 実機の観測・証跡:")
  $lines.Add("- 人間ゲートの合否:")
  return ($lines -join "`n") + "`n"
}

function Invoke-VmVerifyLinearDraft {
  param([string]$SummaryPath, [string]$GateMapPath, [string]$OutputDirectory,
    [string]$CheckpointName = "")
  if ($CheckpointName -and ($CheckpointName.Length -gt 80 -or
      $CheckpointName -cnotmatch '^[\p{L}\p{N} _.-]+$')) {
    throw "Checkpoint name contains unsupported characters."
  }
  $summary = Get-VmVerifyDraftData -Path $SummaryPath
  if ($summary.schemaVersion -ne 1) { throw "Unsupported summary schemaVersion." }
  $gates = Get-VmVerifyDraftGate -Map (Get-VmVerifyDraftData -Path $GateMapPath)
  $output = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory)
  if (Test-Path -LiteralPath $output) {
    if (-not (Test-Path -LiteralPath $output -PathType Container)) {
      throw "Output directory must be a directory."
    }
    if (Get-ChildItem -LiteralPath $output -Force | Select-Object -First 1) {
      throw "Output directory must be empty for each run."
    }
  }
  New-Item -ItemType Directory -Path $output -Force | Out-Null
  $issues = @($gates | ForEach-Object { $_.issueId } | Sort-Object -Unique)
  foreach ($issue in $issues) {
    $issueGates = @($gates | Where-Object { $_.issueId -ceq $issue })
    $content = ConvertTo-VmVerifyLinearDraft -Summary $summary -IssueId $issue `
      -Gates $issueGates -CheckpointName $CheckpointName
    [System.IO.File]::WriteAllText((Join-Path $output "$issue.md"), $content,
      [System.Text.UTF8Encoding]::new($false))
  }
  return $issues
}

if ($MyInvocation.InvocationName -ne ".") {
  if (-not $SummaryPath -or -not $GateMapPath -or -not $OutputDirectory) {
    throw "-SummaryPath, -GateMapPath and -OutputDirectory are required."
  }
  foreach ($issue in @(Invoke-VmVerifyLinearDraft -SummaryPath $SummaryPath -GateMapPath $GateMapPath `
      -OutputDirectory $OutputDirectory -CheckpointName $CheckpointName)) {
    Write-Output "Draft: $issue.md"
  }
  Write-Output "Human review and Linear posting are required."
}
