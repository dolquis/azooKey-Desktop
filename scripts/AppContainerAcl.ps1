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

function Get-AllApplicationPackagesIdentity {
  return New-Object Security.Principal.SecurityIdentifier("S-1-15-2-1")
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

  $candidate = [System.IO.Path]::GetFullPath($Path).TrimEnd([char]'\')
  $roots = @($env:SystemRoot, $env:ProgramFiles, ${env:ProgramFiles(x86)}, ${env:ProgramW6432})
  foreach ($root in $roots) {
    if (-not $root) {
      continue
    }

    $normalizedRoot = [System.IO.Path]::GetFullPath($root).TrimEnd([char]'\')
    if ($candidate.Equals($normalizedRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
      return $true
    }
    if ($candidate.StartsWith($normalizedRoot + [char]'\', [System.StringComparison]::OrdinalIgnoreCase)) {
      return $true
    }
  }

  return $false
}

# Returns "Allowed", "Denied" or "Missing" for ALL APPLICATION PACKAGES against
# the requested rights. A deny ACE that overlaps the requested rights wins, even
# partially: Windows evaluates deny first, so a partial deny still breaks the
# load and must be surfaced rather than papered over with another allow ACE.
function Get-AppContainerAccessState {
  param(
    [Parameter(Mandatory=$true)]
    [System.Security.AccessControl.FileSystemSecurity]$Acl,
    [Parameter(Mandatory=$true)]
    [System.Security.AccessControl.FileSystemRights]$Rights
  )

  $identity = (Get-AllApplicationPackagesIdentity).Value
  $wanted = [int]$Rights
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
    if (([int]$rule.FileSystemRights -band $wanted) -eq $wanted) {
      $allowed = $true
    }
  }

  if ($allowed) {
    return "Allowed"
  }

  return "Missing"
}

# Grants ALL APPLICATION PACKAGES read + execute on a single file or directory.
# Returns $true when the DACL was rewritten, $false when the access was already
# effective (including by inheritance) so callers can report only real changes.
# Directory grants are made inheritable: a rebuild replaces the TIP DLL with a
# brand new file that picks up the directory's inheritable ACEs, which is what
# keeps the grant alive across `cmake --build` without a post-build hook.
function Grant-AppContainerReadExecute {
  param(
    [Parameter(Mandatory=$true)]
    [string]$Path
  )

  if (-not (Test-Path -LiteralPath $Path)) {
    throw "Cannot grant AppContainer access to a missing path: $Path"
  }

  $rights = [System.Security.AccessControl.FileSystemRights]::ReadAndExecute
  $acl = Get-Acl -LiteralPath $Path
  $state = Get-AppContainerAccessState -Acl $acl -Rights $rights
  if ($state -eq "Denied") {
    throw "A deny ACE blocks ALL APPLICATION PACKAGES on $Path. Remove it before registering the TIP for AppContainer apps."
  }
  if ($state -eq "Allowed") {
    return $false
  }

  $inheritance = [System.Security.AccessControl.InheritanceFlags]::None
  if (Test-Path -LiteralPath $Path -PathType Container) {
    $inheritance = [System.Security.AccessControl.InheritanceFlags]::ContainerInherit -bor
      [System.Security.AccessControl.InheritanceFlags]::ObjectInherit
  }

  $rule = New-Object System.Security.AccessControl.FileSystemAccessRule(
    (Get-AllApplicationPackagesIdentity),
    $rights,
    $inheritance,
    [System.Security.AccessControl.PropagationFlags]::None,
    [System.Security.AccessControl.AccessControlType]::Allow)
  $acl.AddAccessRule($rule)
  Set-Acl -LiteralPath $Path -AclObject $acl

  return $true
}

# Removes the *explicit* ALL APPLICATION PACKAGES allow ACEs from a file or
# directory. Inherited ACEs are left alone — they belong to a parent this script
# does not own — and protected system paths are skipped outright.
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
  $identity = (Get-AllApplicationPackagesIdentity).Value
  $removed = $false

  foreach ($rule in @($acl.GetAccessRules($true, $false, [Security.Principal.SecurityIdentifier]))) {
    if ($rule.IdentityReference.Value -ne $identity) {
      continue
    }
    if ($rule.AccessControlType -ne [System.Security.AccessControl.AccessControlType]::Allow) {
      continue
    }
    if ($acl.RemoveAccessRule($rule)) {
      $removed = $true
    }
  }

  if ($removed) {
    Set-Acl -LiteralPath $Path -AclObject $acl
  }

  return $removed
}

# Registration-time grant. Covers the DLL itself (the file the loader opens) and
# its containing directory (so a rebuilt DLL inherits the ACE). Both calls are
# idempotent, so an install under `%ProgramFiles%` reports no change at all.
function Grant-TipAppContainerAccess {
  param(
    [Parameter(Mandatory=$true)]
    [string]$TipDllPath
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

  if ($changed.Count -gt 0) {
    Write-Host "Granted ALL APPLICATION PACKAGES (S-1-15-2-1) read+execute for AppContainer apps:"
    foreach ($path in $changed) {
      Write-Host "  $path"
    }
  } else {
    Write-Host "ALL APPLICATION PACKAGES already has read+execute on the TIP DLL; no ACL change needed."
  }
}

# Unregistration-time revoke, mirroring Grant-TipAppContainerAccess. Only the
# explicit ACEs the registration step could have added are removed, so an MSI
# install location keeps its inherited ACE.
function Revoke-TipAppContainerAccess {
  param(
    [Parameter(Mandatory=$true)]
    [string]$TipDllPath
  )

  $tipDirectory = Split-Path -Parent $TipDllPath
  $changed = @()

  try {
    if (Revoke-AppContainerReadExecute -Path $TipDllPath) {
      $changed += $TipDllPath
    }
  } catch {
    Write-Warning "Could not remove the AppContainer grant from $TipDllPath`: $_"
  }

  if ($tipDirectory) {
    try {
      if (Revoke-AppContainerReadExecute -Path $tipDirectory) {
        $changed += $tipDirectory
      }
    } catch {
      Write-Warning "Could not remove the AppContainer grant from $tipDirectory`: $_"
    }
  }

  if ($changed.Count -gt 0) {
    Write-Host "Removed the explicit ALL APPLICATION PACKAGES grant from:"
    foreach ($path in $changed) {
      Write-Host "  $path"
    }
  }
}
