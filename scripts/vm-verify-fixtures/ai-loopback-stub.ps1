#requires -Version 5.1
<#
.SYNOPSIS
  Loopback HTTP stub that makes the AI cleanup backend fail in a chosen way.

.DESCRIPTION
  VM guest script for the human gate that checks the AI cleanup error notices.
  Point openAiApiEndpoint at http://127.0.0.1:<port>/v1 and pick the failure with
  -Mode. The stub answers every request the same way until it is stopped; restart
  it with another -Mode to switch.

    -Mode auth       : 401
    -Mode ratelimit  : 429 with "Retry-After: <RetryAfterSeconds>"
    -Mode delay      : waits DelaySeconds, then answers 504. Set DelaySeconds above
                       the client timeout so the client gives up first.

  The stub binds a TcpListener to 127.0.0.1 only, so it needs neither elevation nor
  a URL ACL and is unreachable from outside the guest. Use the literal 127.0.0.1 in
  the endpoint: the client rejects plain HTTP for "localhost".

  The request body and headers are read and dropped. They are never logged, echoed
  or stored, because the body is the text being typed and the Authorization header
  is the API key. One line per request is written to the console (and to -LogPath
  when given): UTC time, method, path without the query string, body length and the
  status sent.

  Stop with Ctrl+C, or pass -MaxRequests to exit after that many requests.

  Keep this file ASCII-only. It must run on Windows PowerShell 5.1 (the Windows 11
  default shell), which reads BOM-less UTF-8 as the ANSI code page.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File .\ai-loopback-stub.ps1 -Mode ratelimit -RetryAfterSeconds 5
#>
param(
  [ValidateSet("auth", "ratelimit", "delay")]
  [string]$Mode = "",
  [ValidateRange(1, 65535)]
  [int]$Port = 18089,
  [ValidateRange(0, 3600)]
  [int]$RetryAfterSeconds = 5,
  [ValidateRange(0, 3600)]
  [int]$DelaySeconds = 60,
  [ValidateRange(0, 100000)]
  [int]$MaxRequests = 0,
  [string]$LogPath = ""
)

$ErrorActionPreference = "Stop"

$script:AiStubMaxHeaderBytes = 65536
$script:AiStubMaxBodyBytes = 16777216
$script:AiStubReadTimeoutMs = 5000

function Get-AiStubResponse {
  param([string]$Mode, [int]$RetryAfterSeconds)
  switch ($Mode) {
    "auth" { return [pscustomobject]@{ Status = 401; Reason = "Unauthorized"; RetryAfter = $null } }
    "ratelimit" {
      return [pscustomobject]@{ Status = 429; Reason = "Too Many Requests"; RetryAfter = $RetryAfterSeconds }
    }
    "delay" { return [pscustomobject]@{ Status = 504; Reason = "Gateway Timeout"; RetryAfter = $null } }
    default { throw "Unsupported mode." }
  }
}

function Read-AiStubRequest {
  # Returns only the method, the path and the body length. Headers and body are dropped here.
  param([System.IO.Stream]$Stream)
  $header = New-Object System.IO.MemoryStream
  $one = New-Object byte[] 1
  $terminated = $false
  while ($header.Length -lt $script:AiStubMaxHeaderBytes) {
    if ($Stream.Read($one, 0, 1) -le 0) { break }
    $header.WriteByte($one[0])
    $length = $header.Length
    if ($length -ge 4) {
      $bytes = $header.GetBuffer()
      if ($bytes[$length - 4] -eq 13 -and $bytes[$length - 3] -eq 10 -and
          $bytes[$length - 2] -eq 13 -and $bytes[$length - 1] -eq 10) {
        $terminated = $true
        break
      }
    }
  }
  if (-not $terminated) { return $null }

  $lines = [System.Text.Encoding]::ASCII.GetString($header.ToArray()) -split "`r`n"
  $method = "-"
  $path = "-"
  if ($lines[0] -match '^([A-Z]{1,10}) (\S{1,2048}) HTTP/1\.[01]$') {
    $method = $Matches[1]
    $path = ($Matches[2] -split '\?', 2)[0]
    if ($path -notmatch '^[A-Za-z0-9/._~-]{1,200}$') { $path = "redacted" }
  }
  $contentLength = 0
  foreach ($line in $lines) {
    if ($line -match '^Content-Length:\s*(\d{1,9})\s*$') { $contentLength = [int]$Matches[1] }
  }
  if ($contentLength -gt $script:AiStubMaxBodyBytes) { return $null }

  $buffer = New-Object byte[] 8192
  $remaining = $contentLength
  while ($remaining -gt 0) {
    $read = $Stream.Read($buffer, 0, [Math]::Min($buffer.Length, $remaining))
    if ($read -le 0) { break }
    $remaining -= $read
  }
  [Array]::Clear($buffer, 0, $buffer.Length)
  return [pscustomobject]@{ Method = $method; Path = $path; BodyLength = $contentLength - $remaining }
}

function Write-AiStubResponse {
  param([System.IO.Stream]$Stream, $Response)
  # The body is a fixed literal. Nothing from the request is echoed.
  $body = '{"error":{"message":"stub"}}'
  $head = "HTTP/1.1 $($Response.Status) $($Response.Reason)`r`n" +
    "Content-Type: application/json`r`n" +
    "Content-Length: $($body.Length)`r`n" +
    "Connection: close`r`n"
  if ($null -ne $Response.RetryAfter) { $head += "Retry-After: $($Response.RetryAfter)`r`n" }
  $bytes = [System.Text.Encoding]::ASCII.GetBytes($head + "`r`n" + $body)
  $Stream.Write($bytes, 0, $bytes.Length)
  $Stream.Flush()
}

function Write-AiStubLog {
  param([string]$Line, [string]$LogPath)
  Write-Host $Line
  if ($LogPath) { Add-Content -LiteralPath $LogPath -Value $Line -Encoding ASCII }
}

function Invoke-AiLoopbackStub {
  param([string]$Mode, [int]$Port, [int]$RetryAfterSeconds, [int]$DelaySeconds,
    [int]$MaxRequests = 0, [string]$LogPath = "")
  $response = Get-AiStubResponse -Mode $Mode -RetryAfterSeconds $RetryAfterSeconds
  $listener = New-Object System.Net.Sockets.TcpListener ([System.Net.IPAddress]::Loopback), $Port
  $listener.Start()
  $served = 0
  try {
    Write-AiStubLog -LogPath $LogPath -Line `
      "listening http://127.0.0.1:$Port mode=$Mode status=$($response.Status)"
    while ($MaxRequests -eq 0 -or $served -lt $MaxRequests) {
      # Poll so that Ctrl+C is honored; a blocking accept would ignore it.
      if (-not $listener.Pending()) {
        Start-Sleep -Milliseconds 50
        continue
      }
      $client = $listener.AcceptTcpClient()
      try {
        $client.ReceiveTimeout = $script:AiStubReadTimeoutMs
        $client.SendTimeout = $script:AiStubReadTimeoutMs
        $stream = $client.GetStream()
        $request = Read-AiStubRequest -Stream $stream
        $sent = "dropped"
        if ($null -ne $request) {
          if ($Mode -eq "delay") { Start-Sleep -Seconds $DelaySeconds }
          try {
            Write-AiStubResponse -Stream $stream -Response $response
            $sent = [string]$response.Status
          } catch [System.IO.IOException] {
            # The client gave up first. That is the expected outcome in delay mode.
            $sent = "client-closed"
          }
        } else {
          $request = [pscustomobject]@{ Method = "-"; Path = "-"; BodyLength = 0 }
        }
        $served++
        $time = [DateTime]::UtcNow.ToString("yyyy-MM-ddTHH:mm:ss.fffZ")
        Write-AiStubLog -LogPath $LogPath -Line `
          "$time $($request.Method) $($request.Path) body=$($request.BodyLength) sent=$sent"
      } catch [System.IO.IOException] {
        $served++
        Write-AiStubLog -LogPath $LogPath -Line "request aborted: read failed"
      } finally {
        $client.Close()
      }
    }
  } finally {
    $listener.Stop()
  }
  return $served
}

if ($MyInvocation.InvocationName -ne ".") {
  if (-not $Mode) { throw "-Mode is required (auth, ratelimit or delay)." }
  $count = Invoke-AiLoopbackStub -Mode $Mode -Port $Port -RetryAfterSeconds $RetryAfterSeconds `
    -DelaySeconds $DelaySeconds -MaxRequests $MaxRequests -LogPath $LogPath
  Write-Host "served $count request(s)"
}
