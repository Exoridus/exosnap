#Requires -Version 7.0
<#
.SYNOPSIS
    Guards the containment contract of scripts/lib/SyntheticLoad.psm1.

.DESCRIPTION
    Not Pester: the same homegrown harness the other script tests use.

    The module exists because a leaked load worker is worse than no load at all
    -- it keeps consuming the machine and silently invalidates every measurement
    taken afterwards. Each of the three containment guarantees is asserted here
    by making it the only one that can act:

      - the worker's own deadline, with nothing stopping it;
      - the job object's kill-on-close, with the owning process killed outright
        so no `finally` can run;
      - the ordinary Stop path.

    The utilization check is asserted too, because the failure it catches (a
    worker that starts and does nothing) is invisible in every other signal.
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$modulePath = Join-Path $repoRoot 'scripts/lib/SyntheticLoad.psm1'

$script:Passed = 0
$script:Failed = 0

function Test-Case {
    param([Parameter(Mandatory)] [string] $Name, [Parameter(Mandatory)] [scriptblock] $Body)
    try {
        & $Body
        Write-Host "  PASS  $Name" -ForegroundColor Green
        $script:Passed++
    }
    catch {
        Write-Host "  FAIL  $Name" -ForegroundColor Red
        Write-Host "        $($_.Exception.Message)" -ForegroundColor Red
        $script:Failed++
    }
}

function Assert-True { param($Condition, [string] $Message) if (-not $Condition) { throw $Message } }

function Test-AnyAlive {
    param([int[]] $ProcessId)
    foreach ($id in $ProcessId) {
        if ($null -ne (Get-Process -Id $id -ErrorAction SilentlyContinue)) { return $true }
    }
    return $false
}

function Wait-AllGone {
    param([int[]] $ProcessId, [int] $TimeoutSeconds)
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        if (-not (Test-AnyAlive -ProcessId $ProcessId)) { return $true }
        Start-Sleep -Milliseconds 250
    }
    return -not (Test-AnyAlive -ProcessId $ProcessId)
}

Import-Module $modulePath -Force

Write-Host ''
Write-Host 'Synthetic load containment'

Test-Case 'the module exports the three entry points' {
    $exported = (Get-Module SyntheticLoad).ExportedFunctions.Keys
    foreach ($name in @('Start-SyntheticLoad', 'Stop-SyntheticLoad', 'Invoke-WithSyntheticLoad')) {
        Assert-True ($exported -contains $name) "missing exported function $name"
    }
}

Test-Case 'asking for no workers at all is refused' {
    $threw = $false
    try { Start-SyntheticLoad -MaxSeconds 5 -CpuWorkers 0 -DiskWorkers 0 | Out-Null }
    catch { $threw = $true }
    Assert-True $threw 'a load with zero workers was accepted'
}

Test-Case 'the load runs, is measured, and leaves nothing behind' {
    $load = Start-SyntheticLoad -MaxSeconds 30 -CpuWorkers 2
    $pids = $load.ProcessIds
    Assert-True ($pids.Count -eq 2) "expected 2 workers, got $($pids.Count)"
    Assert-True (Test-AnyAlive -ProcessId $pids) 'no worker was running after the ramp'
    $report = Stop-SyntheticLoad -Load $load
    Assert-True ($report.CoresBusy -gt 0.5) `
    "the workers only reached $($report.CoresBusy) cores busy, so the load was not real"
    Assert-True ($report.Survivors.Count -eq 0) 'Stop-SyntheticLoad left a worker running'
    Assert-True (Wait-AllGone -ProcessId $pids -TimeoutSeconds 10) 'a worker outlived the stop'
}

Test-Case 'stopping twice is harmless' {
    $load = Start-SyntheticLoad -MaxSeconds 20 -CpuWorkers 1
    [void](Stop-SyntheticLoad -Load $load)
    $second = Stop-SyntheticLoad -Load $load
    Assert-True ($null -eq $second) 'the second stop reported work it did not do'
}

Test-Case 'a worker exits on its own deadline with nothing stopping it' {
    # Guarantee 1 in isolation: the handle is deliberately abandoned, so neither
    # a Stop call nor a closing handle can be what ends the worker.
    $load = Start-SyntheticLoad -MaxSeconds 6 -CpuWorkers 1 -SkipUtilizationCheck
    $pids = $load.ProcessIds
    Assert-True (Wait-AllGone -ProcessId $pids -TimeoutSeconds 25) `
        'the worker outlived its own deadline'
    [void](Stop-SyntheticLoad -Load $load)
}

Test-Case 'killing the owner kills the workers' {
    # Guarantee 3 in isolation: the owner is terminated, so no `finally` and no
    # deliberate handle close can run. Only the job object can act here.
    $child = Join-Path ([IO.Path]::GetTempPath()) "exosnap-load-owner-$PID.ps1"
    $log = Join-Path ([IO.Path]::GetTempPath()) "exosnap-load-owner-$PID.log"
    @"
Import-Module '$modulePath' -Force
`$load = Start-SyntheticLoad -MaxSeconds 300 -CpuWorkers 1
`$load.ProcessIds -join ',' | Set-Content -LiteralPath '$log'
Start-Sleep -Seconds 300
"@ | Set-Content -LiteralPath $child

    $owner = Start-Process -FilePath (Get-Process -Id $PID).Path -PassThru -WindowStyle Hidden `
        -ArgumentList @('-NoProfile', '-NonInteractive', '-File', $child)
    try {
        $deadline = (Get-Date).AddSeconds(60)
        $workerIds = @()
        while ((Get-Date) -lt $deadline -and $workerIds.Count -eq 0) {
            if (Test-Path -LiteralPath $log) {
                $raw = (Get-Content -LiteralPath $log -Raw).Trim()
                if ($raw) { $workerIds = @($raw -split ',' | ForEach-Object { [int]$_ }) }
            }
            if ($workerIds.Count -eq 0) { Start-Sleep -Milliseconds 250 }
        }
        Assert-True ($workerIds.Count -eq 1) 'the owner never reported a worker'
        Assert-True (Test-AnyAlive -ProcessId $workerIds) 'the worker was not running before the kill'

        $owner | Stop-Process -Force
        Assert-True (Wait-AllGone -ProcessId $workerIds -TimeoutSeconds 20) `
            'the worker survived the death of the process that owned the job'
    }
    finally {
        $owner.Refresh()
        if (-not $owner.HasExited) { $owner | Stop-Process -Force -ErrorAction SilentlyContinue }
        Remove-Item -LiteralPath $child, $log -Force -ErrorAction SilentlyContinue
    }
}

Write-Host ''
Write-Host "$script:Passed/$($script:Passed + $script:Failed) passed"
if ($script:Failed -gt 0) { exit 1 }
exit 0
