# Shared AppContainer (UWP / Microsoft Store) access helpers for the TIP DLL.
#
# An AppContainer process is authorized through *two* sets of principals: the
# usual user / group SIDs and the AppContainer SIDs (package SID + capability
# SIDs). The effective access is the intersection of both, so a DLL the
# interactive user can read is still unreadable from an AppContainer unless the
# DACL also grants one of the AppContainer principals. Windows therefore ACLs
# the resources it wants reachable from *every* AppContainer with
# `ALL APPLICATION PACKAGES` (SID S-1-15-2-1). See
# https://learn.microsoft.com/windows/win32/secauthz/implementing-an-appcontainer#appcontainer-overview
#
# ctfmon loads the TIP DLL in-process inside the target application, so without
# that ACE Japanese input is unavailable in Microsoft Store / AppContainer apps.
# `%ProgramFiles%` carries the ACE by default, which is why the MSI
# (docs/sideload-packaging-spec.md §4) inherits it and adds no ACL of its own.
# Development registration loads the TIP straight out of a build tree under the
# user profile, which does not carry it — so register-dev.ps1 grants it
# explicitly and unregister-dev.ps1 takes it back. Design: same spec §1.7.
#
# Two rules keep the revoke side from damaging ACLs this repository does not
# own. First, only the ACEs a registration actually *added* are removed: each
# grant is recorded in a machine-wide ledger (see Get-AppContainerGrantLedger)
# and unregistration consults it, so an ACE that predated registration survives.
# Second, removal is exact (`RemoveAccessRuleSpecific`), so a hand-tuned ACE for
# the same SID with different rights or inheritance is never folded into ours.

function Get-AllApplicationPackagesIdentity {
  return New-Object Security.Principal.SecurityIdentifier("S-1-15-2-1")
}

function Get-NormalizedAclPath {
  param(
    [Parameter(Mandatory=$true)]
    [string]$Path
  )

  return [System.IO.Path]::GetFullPath($Path).TrimEnd([char]'\')
}

# Paths whose ACLs this repository never rewrites. `%ProgramFiles%` and
# `%SystemRoot%` already grant ALL APPLICATION PACKAGES by inheritance, so the
# grant is a no-op there; the guard exists so the *revoke* side can never strip
# an ACE that Windows (or the MSI install location) owns.
function Test-ProtectedSystemPath {
  param(
    [Parameter(Mandatory=$true)]
    [string]$Path
  )

  $candidate = Get-NormalizedAclPath -Path $Path
  $roots = @($env:SystemRoot, $env:ProgramFiles, ${env:ProgramFiles(x86)}, ${env:ProgramW6432})
  foreach ($root in $roots) {
    if (-not $root) {
      continue
    }

    $normalizedRoot = Get-NormalizedAclPath -Path $root
    if ($candidate.Equals($normalizedRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
      return $true
    }
    if ($candidate.StartsWith($normalizedRoot + [char]'\', [System.StringComparison]::OrdinalIgnoreCase)) {
      return $true
    }
  }

  return $false
}

# The exact ACE a grant installs on a path. Directory grants are inheritable: a
# rebuild replaces the TIP DLL with a brand new file that picks up the
# directory's inheritable ACEs, which is what keeps the grant alive across
# `cmake --build` without a post-build hook. Revocation rebuilds the same rule
# and removes only that, so nothing else for the same SID is disturbed.
function Get-AppContainerAccessRule {
  param(
    [Parameter(Mandatory=$true)]
    [string]$Path
  )

  $inheritance = [System.Security.AccessControl.InheritanceFlags]::None
  if (Test-Path -LiteralPath $Path -PathType Container) {
    $inheritance = [System.Security.AccessControl.InheritanceFlags]::ContainerInherit -bor
      [System.Security.AccessControl.InheritanceFlags]::ObjectInherit
  }

  return New-Object System.Security.AccessControl.FileSystemAccessRule(
    (Get-AllApplicationPackagesIdentity),
    [System.Security.AccessControl.FileSystemRights]::ReadAndExecute,
    $inheritance,
    [System.Security.AccessControl.PropagationFlags]::None,
    [System.Security.AccessControl.AccessControlType]::Allow)
}

# `FileSystemAccessRule` folds Synchronize into every allow mask, so a rule that
# was constructed in memory and the same rule read back from a DACL compare
# unequal unless both sides are normalized.
function Test-SameAccessMask {
  param(
    [Parameter(Mandatory=$true)]
    [System.Security.AccessControl.FileSystemRights]$Left,
    [Parameter(Mandatory=$true)]
    [System.Security.AccessControl.FileSystemRights]$Right
  )

  $synchronize = [int][System.Security.AccessControl.FileSystemRights]::Synchronize
  return (([int]$Left -bor $synchronize) -eq ([int]$Right -bor $synchronize))
}

# Returns "Allowed", "Denied" or "Missing" for ALL APPLICATION PACKAGES against
# the requested rights. A deny ACE that overlaps the requested rights wins, even
# partially: Windows evaluates deny first, so a partial deny still breaks the
# load and must be surfaced rather than papered over with another allow ACE.
#
# `-RequireObjectInherit` asks the stricter question a directory needs: not "can
# an AppContainer read this directory" but "will a file created here inherit the
# access". An explicit non-inheritable ACE satisfies the former and not the
# latter, so a directory checked without this switch would be reported as
# already covered and the next rebuild would lose access.
function Get-AppContainerAccessState {
  param(
    [Parameter(Mandatory=$true)]
    [System.Security.AccessControl.FileSystemSecurity]$Acl,
    [Parameter(Mandatory=$true)]
    [System.Security.AccessControl.FileSystemRights]$Rights,
    [switch]$RequireObjectInherit
  )

  $identity = (Get-AllApplicationPackagesIdentity).Value
  $wanted = [int]$Rights
  $objectInherit = [int][System.Security.AccessControl.InheritanceFlags]::ObjectInherit
  $allowed = $false

  foreach ($rule in $Acl.GetAccessRules($true, $true, [Security.Principal.SecurityIdentifier])) {
    if ($rule.IdentityReference.Value -ne $identity) {
      continue
    }
    if (([int]$rule.FileSystemRights -band $wanted) -eq 0) {
      continue
    }
    if ($rule.AccessControlType -eq [System.Security.AccessControl.AccessControlType]::Deny) {
      return "Denied"
    }
    if (([int]$rule.FileSystemRights -band $wanted) -ne $wanted) {
      continue
    }
    if ($RequireObjectInherit -and (([int]$rule.InheritanceFlags -band $objectInherit) -eq 0)) {
      continue
    }
    $allowed = $true
  }

  if ($allowed) {
    return "Allowed"
  }

  return "Missing"
}

# Grants ALL APPLICATION PACKAGES read + execute on a single file or directory.
# Returns $true when the DACL was rewritten, $false when the access was already
# effective (including by inheritance) so callers can report — and record — only
# the ACEs they actually added.
function Grant-AppContainerReadExecute {
  param(
    [Parameter(Mandatory=$true)]
    [string]$Path
  )

  if (-not (Test-Path -LiteralPath $Path)) {
    throw "Cannot grant AppContainer access to a missing path: $Path"
  }

  $isContainer = Test-Path -LiteralPath $Path -PathType Container
  $rights = [System.Security.AccessControl.FileSystemRights]::ReadAndExecute
  $acl = Get-Acl -LiteralPath $Path
  $state = Get-AppContainerAccessState -Acl $acl -Rights $rights -RequireObjectInherit:$isContainer
  if ($state -eq "Denied") {
    throw "A deny ACE blocks ALL APPLICATION PACKAGES on $Path. Remove it before registering the TIP for AppContainer apps."
  }
  if ($state -eq "Allowed") {
    return $false
  }

  $acl.AddAccessRule((Get-AppContainerAccessRule -Path $Path))
  Set-Acl -LiteralPath $Path -AclObject $acl

  return $true
}

# Removes the one ACE a grant would have installed on this path — same rights,
# same inheritance, non-inherited — and nothing else. Inherited ACEs belong to a
# parent this script does not own, ACEs with other rights or inheritance belong
# to whoever set them, and protected system paths are skipped outright.
function Revoke-AppContainerReadExecute {
  param(
    [Parameter(Mandatory=$true)]
    [string]$Path
  )

  if (Test-ProtectedSystemPath -Path $Path) {
    return $false
  }
  if (-not (Test-Path -LiteralPath $Path)) {
    return $false
  }

  $acl = Get-Acl -LiteralPath $Path
  $granted = Get-AppContainerAccessRule -Path $Path
  $identity = (Get-AllApplicationPackagesIdentity).Value
  $present = $false

  foreach ($rule in $acl.GetAccessRules($true, $false, [Security.Principal.SecurityIdentifier])) {
    if ($rule.IdentityReference.Value -ne $identity) {
      continue
    }
    if ($rule.AccessControlType -ne $granted.AccessControlType) {
      continue
    }
    if ($rule.InheritanceFlags -ne $granted.InheritanceFlags) {
      continue
    }
    if ($rule.PropagationFlags -ne $granted.PropagationFlags) {
      continue
    }
    if (-not (Test-SameAccessMask -Left $rule.FileSystemRights -Right $granted.FileSystemRights)) {
      continue
    }
    $present = $true
  }

  if (-not $present) {
    return $false
  }

  $acl.RemoveAccessRuleSpecific($granted)
  Set-Acl -LiteralPath $Path -AclObject $acl

  return $true
}

# Machine-wide ledger of the paths a registration granted. Registration and
# unregistration are separate elevated processes, so "did *we* add this ACE"
# cannot be answered from the DACL alone — an ACE the developer set by hand is
# byte-identical to ours. The ledger is the record that makes the revoke side
# precise, and it is deleted along with the last entry.
function Get-AppContainerGrantLedger {
  param(
    [Parameter(Mandatory=$true)]
    [string]$LedgerKey
  )

  $entry = Get-ItemProperty -Path $LedgerKey -Name "AppContainerAclGrants" -ErrorAction SilentlyContinue
  if (-not $entry) {
    return @()
  }

  return @($entry.AppContainerAclGrants)
}

function Save-AppContainerGrantLedger {
  param(
    [Parameter(Mandatory=$true)]
    [string]$LedgerKey,
    [string[]]$Entry = @()
  )

  if ($Entry.Count -gt 0) {
    if (-not (Test-Path -LiteralPath $LedgerKey)) {
      New-Item -Path $LedgerKey -Force | Out-Null
    }
    New-ItemProperty -Path $LedgerKey -Name "AppContainerAclGrants" -Value $Entry `
      -PropertyType MultiString -Force | Out-Null
    return
  }

  if (-not (Test-Path -LiteralPath $LedgerKey)) {
    return
  }

  Remove-ItemProperty -Path $LedgerKey -Name "AppContainerAclGrants" -ErrorAction SilentlyContinue

  # Leave no empty shell behind: drop the ledger key, then its parent, once the
  # last entry is gone and nothing else has moved in. Bounded to those two
  # levels, and never applied to a hive root such as `HKLM:\Software`.
  foreach ($key in @($LedgerKey, (Split-Path -Parent $LedgerKey))) {
    if (-not $key -or ($key -split '\\').Count -le 2) {
      break
    }
    if (-not (Test-Path -LiteralPath $key)) {
      continue
    }

    # Release the key handle before deleting it: an open RegistryKey can block
    # its own removal.
    $item = Get-Item -LiteralPath $key
    $occupied = ($item.SubKeyCount -gt 0 -or $item.ValueCount -gt 0)
    $item.Dispose()
    if ($occupied) {
      break
    }
    Remove-Item -LiteralPath $key -Force -ErrorAction SilentlyContinue
  }
}

function Test-AppContainerGrantLedgerEntry {
  param(
    [Parameter(Mandatory=$true)]
    [AllowEmptyCollection()]
    [string[]]$Ledger,
    [Parameter(Mandatory=$true)]
    [string]$Path
  )

  $candidate = Get-NormalizedAclPath -Path $Path
  foreach ($entry in $Ledger) {
    if (-not $entry) {
      continue
    }
    if ((Get-NormalizedAclPath -Path $entry).Equals($candidate, [System.StringComparison]::OrdinalIgnoreCase)) {
      return $true
    }
  }

  return $false
}

# Registration-time grant. Covers the DLL itself (the file the loader opens) and
# its containing directory (so a rebuilt DLL inherits the ACE). Both calls are
# idempotent, so an install under `%ProgramFiles%` reports no change at all —
# and records nothing, which is what keeps unregistration off ACLs it did not
# create.
function Grant-TipAppContainerAccess {
  param(
    [Parameter(Mandatory=$true)]
    [string]$TipDllPath,
    [string]$LedgerKey = "HKLM:\Software\azooKey\DevRegistration"
  )

  $tipDirectory = Split-Path -Parent $TipDllPath
  $changed = @()

  # The directory grant is the durability half (it survives rebuilds) and the
  # DLL grant is the correctness half (it covers the file being registered right
  # now). A failure on the directory must not cost us the DLL, so they are
  # attempted independently.
  if ($tipDirectory) {
    try {
      if (Grant-AppContainerReadExecute -Path $tipDirectory) {
        $changed += $tipDirectory
      }
    } catch {
      Write-Warning "Could not grant AppContainer access to $tipDirectory`: $_"
      Write-Warning "A rebuild will drop the grant on the TIP DLL; rerun registration after rebuilding."
    }
  }

  if (Grant-AppContainerReadExecute -Path $TipDllPath) {
    $changed += $TipDllPath
  }

  if ($changed.Count -eq 0) {
    Write-Host "ALL APPLICATION PACKAGES already has read+execute on the TIP DLL; no ACL change needed."
    return
  }

  Write-Host "Granted ALL APPLICATION PACKAGES (S-1-15-2-1) read+execute for AppContainer apps:"
  foreach ($path in $changed) {
    Write-Host "  $path"
  }

  $ledger = @(Get-AppContainerGrantLedger -LedgerKey $LedgerKey)
  foreach ($path in $changed) {
    if (-not (Test-AppContainerGrantLedgerEntry -Ledger $ledger -Path $path)) {
      $ledger += (Get-NormalizedAclPath -Path $path)
    }
  }

  try {
    Save-AppContainerGrantLedger -LedgerKey $LedgerKey -Entry $ledger
  } catch {
    Write-Warning "Could not record the AppContainer grant in $LedgerKey`: $_"
    Write-Warning "Unregistration will leave the grant in place; remove it by hand if that matters."
  }
}

# Unregistration-time revoke, mirroring Grant-TipAppContainerAccess. Only paths
# the ledger says this repository granted are touched, and only the exact ACE a
# grant installs is removed — so an ACE that predated registration, or one a
# developer set by hand with different rights, survives untouched.
function Revoke-TipAppContainerAccess {
  param(
    [Parameter(Mandatory=$true)]
    [string]$TipDllPath,
    [string]$LedgerKey = "HKLM:\Software\azooKey\DevRegistration"
  )

  $tipDirectory = Split-Path -Parent $TipDllPath
  $ledger = @(Get-AppContainerGrantLedger -LedgerKey $LedgerKey)
  if ($ledger.Count -eq 0) {
    return
  }

  $targets = @($TipDllPath)
  if ($tipDirectory) {
    $targets += $tipDirectory
  }

  $changed = @()
  $retired = @()
  foreach ($target in $targets) {
    if (-not (Test-AppContainerGrantLedgerEntry -Ledger $ledger -Path $target)) {
      continue
    }

    try {
      if (Revoke-AppContainerReadExecute -Path $target) {
        $changed += $target
      }
      # Retired only on a clean outcome — the ACE was removed, or it is
      # verifiably no longer there. A failure keeps the entry so the next
      # unregistration retries instead of orphaning the ACE with no record of it.
      $retired += (Get-NormalizedAclPath -Path $target)
    } catch {
      Write-Warning "Could not remove the AppContainer grant from $target`: $_"
      Write-Warning "Keeping $target in the grant ledger so unregistration can retry."
    }
  }

  if ($retired.Count -eq 0) {
    return
  }

  if ($changed.Count -gt 0) {
    Write-Host "Removed the ALL APPLICATION PACKAGES grant added at registration from:"
    foreach ($path in $changed) {
      Write-Host "  $path"
    }
  }

  $remaining = @($ledger | Where-Object {
    $_ -and -not (Test-AppContainerGrantLedgerEntry -Ledger $retired -Path $_)
  })

  try {
    Save-AppContainerGrantLedger -LedgerKey $LedgerKey -Entry $remaining
  } catch {
    Write-Warning "Could not update the AppContainer grant ledger in $LedgerKey`: $_"
  }
}
