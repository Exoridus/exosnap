#requires -Version 7.0
[CmdletBinding()]
param(
    # The exosnap.exe under test. Bound explicitly: an acceptance run that picked
    # "whatever build was on PATH" is evidence about an unknown artifact.
    [Parameter(Mandatory)]
    [string] $AppPath,

    [string] $EvidencePath,

    [int] $TimeoutSeconds = 60
)

<#
.SYNOPSIS
    Proves that a setting written over the control channel takes the product's
    own path all the way to disk, and survives a restart.

.DESCRIPTION
        settings.describe        -> the key table, with types and accepted values
        settings.get             -> the original values (restored at the end)
        settings.set             -> written through the SettingsAdapter setter
                                    the QML control writes to
        settings.get             -> the RECONCILED value, which is not always the
                                    requested one
        the application exits
        a second launch          -> the same values, read from the settings file
        settings.set (restore)   -> back to the originals
        settings.get             -> the restore verified

    The whole run uses an isolated EXOSNAP_CONFIG_DIR under the system temp
    directory, which is removed afterwards: nothing here can reach the
    developer's own configuration, and the restore step is belt-and-braces on top
    of that rather than the only protection.

    One assertion is the point of the exercise and is easy to lose: a write is
    reconciled by the PRODUCT. Asking for MP4 while the codecs are AV1 + Opus
    moves both codecs (ADR 0010), and the run asserts that it did -- because a
    settings path that accepted the container without reconciling would be a
    path no user takes.

    No Start-Sleep anywhere. Every wait is a blocking pipe connect (the endpoint
    does not exist until the server has started) or a real process-handle wait.

.EXAMPLE
    ./live-verify-settings-persistence.ps1 -AppPath build/windows-x64-debug/app/exosnap.exe
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Import-Module (Join-Path $PSScriptRoot 'lib/LiveVerifyClient.psm1') -Force

$steps = [System.Collections.Generic.List[object]]::new()
function Add-Step([string] $Name, [bool] $Pass, [object] $Detail) {
    $steps.Add([ordered]@{ step = $Name; pass = $Pass; detail = $Detail })
    $status = if ($Pass) { 'PASS' } else { 'FAIL' }
    Write-Host ("{0,-4} {1}" -f $status, $Name)
    if (-not $Pass) { Write-Host ("     {0}" -f ($Detail | ConvertTo-Json -Compress -Depth 6)) }
}

function Get-SettingValue($Connection, [string] $Key) {
    $response = Invoke-LiveVerifyCommand -Connection $Connection -Command 'settings.get' -Parameters @{ key = $Key }
    if (-not $response.ok) { throw "settings.get $Key refused: $($response.error.code) - $($response.error.message)" }
    return $response.result.values.$Key
}

function Set-SettingValue($Connection, [string] $Key, $Value) {
    return Invoke-LiveVerifyCommand -Connection $Connection -Command 'settings.set' `
        -Parameters @{ key = $Key; value = $Value }
}

# Closes the application the way its own close button does, and falls back to a
# kill only if that is refused. Which one happened is recorded: a run that had to
# kill the process has NOT proven the graceful exit path, and saying so is the
# difference between evidence and a green tick.
function Stop-Application($Process, [int] $TimeoutMs) {
    $graceful = $false
    try { $graceful = $Process.CloseMainWindow() } catch { $graceful = $false }
    if ($graceful -and $Process.WaitForExit($TimeoutMs)) { return 'closed' }
    try { $Process.Kill() } catch { }
    [void]$Process.WaitForExit($TimeoutMs)
    return 'killed'
}

$appFull = (Resolve-Path -LiteralPath $AppPath).Path
$appSha = (Get-FileHash -LiteralPath $appFull -Algorithm SHA256).Hash.ToLowerInvariant()

$scratch = Join-Path ([System.IO.Path]::GetTempPath()) ("exosnap-settings-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $scratch -Force | Out-Null
$env:EXOSNAP_CONFIG_DIR = Join-Path $scratch 'config'
$env:EXOSNAP_OUTPUT_DIR = Join-Path $scratch 'output'
New-Item -ItemType Directory -Path $env:EXOSNAP_CONFIG_DIR -Force | Out-Null

# The three keys under test, chosen for what each one proves:
#   video.cq            a plain number that nothing reconciles
#   app.accent          an application setting, which persists through a
#                       different store than the recording configuration
#   video.container     the one that is NOT written through unchanged: MP4
#                       forces the codecs, and that is the whole point of
#                       writing through the product edge
$mutations = @(
    @{ key = 'video.cq'; value = 29 }
    @{ key = 'app.accent'; value = 'violet' }
    @{ key = 'video.container'; value = 'MP4' }
)

$evidence = [ordered]@{
    application = @{ path = $appFull; sha256 = $appSha }
    configDir   = $env:EXOSNAP_CONFIG_DIR
}
$exitCode = 1
$first = $null
$second = $null
$conn = $null
try {
    # -- first launch: describe, snapshot, mutate ---------------------------
    $runId = New-LiveVerifyRunId
    $first = Start-Process -FilePath $appFull -PassThru -ArgumentList @('--live-verify-control', $runId)
    $conn = Connect-LiveVerify -RunId $runId -ConnectTimeoutMs ($TimeoutSeconds * 1000)
    Add-Step 'application endpoint attached (artifact bound)' `
        ($conn.Identity.executableSha256 -eq $appSha) `
        @{ reported = $conn.Identity.executableSha256; measured = $appSha }

    $described = Invoke-LiveVerifyCommand -Connection $conn -Command 'settings.describe'
    $keyNames = @($described.result.keys | ForEach-Object { $_.key })
    Add-Step 'settings.describe publishes the key table before anything is written' `
        ($described.ok -and $described.result.count -gt 30 -and
         ($mutations | ForEach-Object { $keyNames -contains $_.key }) -notcontains $false) `
        @{ count = $described.result.count; writeSemantics = $described.result.writeSemantics }
    $evidence.describedKeyCount = $described.result.count

    $originals = @{}
    foreach ($m in $mutations) { $originals[$m.key] = Get-SettingValue $conn $m.key }
    $evidence.originals = $originals
    Add-Step 'original values captured for restore' $true $originals

    $codecBefore = Get-SettingValue $conn 'video.videoCodec'
    foreach ($m in $mutations) {
        $written = Set-SettingValue $conn $m.key $m.value
        Add-Step ("settings.set {0}" -f $m.key) ([bool]$written.ok) $written
        if (-not $written.ok) { throw "settings.set $($m.key) refused" }
    }

    $afterWrite = @{}
    foreach ($m in $mutations) { $afterWrite[$m.key] = Get-SettingValue $conn $m.key }
    $evidence.afterWrite = $afterWrite
    Add-Step 'every write is observable in the same process' `
        (($mutations | ForEach-Object { $afterWrite[$_.key] -eq $_.value }) -notcontains $false) `
        $afterWrite

    # The reconciliation assertion: the container write moved the video codec,
    # which only happens if the write really went through the product's own
    # intake rather than into the model behind it.
    $codecAfter = Get-SettingValue $conn 'video.videoCodec'
    Add-Step 'the container write was reconciled by the product (ADR 0010)' `
        ($codecBefore -eq 'AV1' -and $codecAfter -ne 'AV1') `
        @{ before = $codecBefore; after = $codecAfter }
    $evidence.reconciliation = @{ videoCodecBefore = $codecBefore; videoCodecAfter = $codecAfter }

    $conn.Close()
    $conn = $null
    $firstExit = Stop-Application $first ($TimeoutSeconds * 1000)
    Add-Step 'application exited' ($first.HasExited) @{ how = $firstExit; exitCode = $first.ExitCode }
    $evidence.firstExit = $firstExit

    # -- second launch: the same config directory ----------------------------
    $secondRunId = New-LiveVerifyRunId
    $second = Start-Process -FilePath $appFull -PassThru -ArgumentList @('--live-verify-control', $secondRunId)
    $conn = Connect-LiveVerify -RunId $secondRunId -ConnectTimeoutMs ($TimeoutSeconds * 1000)

    $afterRestart = @{}
    foreach ($m in $mutations) { $afterRestart[$m.key] = Get-SettingValue $conn $m.key }
    $evidence.afterRestart = $afterRestart
    Add-Step 'every written value survived the restart' `
        (($mutations | ForEach-Object { $afterRestart[$_.key] -eq $afterWrite[$_.key] }) -notcontains $false) `
        $afterRestart

    # The settings file exists and is the one this run wrote. Reported rather
    # than asserted on its contents: the protocol answer above is the product
    # fact, and parsing the ini here would be a second reader of it.
    $settingsFile = Join-Path $env:EXOSNAP_CONFIG_DIR 'settings.ini'
    Add-Step 'the isolated settings file was written' (Test-Path -LiteralPath $settingsFile) @{ path = $settingsFile }

    # -- restore ---------------------------------------------------------------
    # Ordered so the container goes back first: restoring it re-reconciles the
    # codecs, and a codec restored before that would be overwritten again.
    foreach ($key in @('video.container', 'video.cq', 'app.accent')) {
        $restored = Set-SettingValue $conn $key $originals[$key]
        if (-not $restored.ok) { throw "restore of $key refused: $($restored.error.message)" }
    }
    $afterRestore = @{}
    foreach ($m in $mutations) { $afterRestore[$m.key] = Get-SettingValue $conn $m.key }
    $evidence.afterRestore = $afterRestore
    Add-Step 'the originals are restored' `
        (($mutations | ForEach-Object { $afterRestore[$_.key] -eq $originals[$_.key] }) -notcontains $false) `
        $afterRestore

    $conn.Close()
    $conn = $null
    $secondExit = Stop-Application $second ($TimeoutSeconds * 1000)
    Add-Step 'second application exited' ($second.HasExited) @{ how = $secondExit }

    $exitCode = if (($steps | Where-Object { -not $_.pass }).Count -eq 0) { 0 } else { 1 }
}
finally {
    if ($null -ne $conn) { try { $conn.Close() } catch { } }
    foreach ($process in @($first, $second)) {
        if ($null -ne $process -and -not $process.HasExited) { try { $process.Kill() } catch { } }
    }
    # The isolated configuration goes with the run. Leaving it behind would put
    # a stray ExoSnap config tree in the temp directory after every check.
    Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue

    $evidence.steps = $steps
    $evidence.pass = ($steps | Where-Object { -not $_.pass }).Count -eq 0
    if ($EvidencePath) {
        $evidence | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $EvidencePath -Encoding utf8
        Write-Host "evidence: $EvidencePath"
    }
}

$verdict = if ($exitCode -eq 0) { 'PASS' } else { 'FAIL' }
Write-Host ("`n{0}: {1}/{2} steps passed" -f $verdict, ($steps | Where-Object { $_.pass }).Count, $steps.Count)
exit $exitCode
