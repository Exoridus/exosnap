#Requires -Version 7.0
<#
.SYNOPSIS
    Tests for the host-wide locks that keep two verify runs from competing.

.DESCRIPTION
    Not Pester: the same homegrown harness the other script tests use.

    The contention being prevented is real and local: two worktrees each running a
    build and a test suite at full parallelism on one machine. The cases here hold a
    lock from a second process -- the only way a lock across sessions can be shown
    to be one -- and check what the first process does about it.
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptRoot = Split-Path -Parent $PSScriptRoot

# These cases measure real contention, so they take real locks from real second
# processes -- and under CTest this file runs inside a run-tests.ps1 that already
# holds the real device lock and has marked the environment for its children. On
# the real names the tests would be contending with the machine that is running
# them. So: a namespace of their own, set before the module is loaded and inherited
# by every holder they start, and the parent's inheritance marks cleared so the
# first lock taken here is a real one. The parent's hold on the real device lock
# still covers this process; nothing here touches the device.
$env:EXOSNAP_HOST_LOCK_NAMESPACE = "Tests.$PID"
foreach ($kind in 'build', 'device') {
    [Environment]::SetEnvironmentVariable("EXOSNAP_HOST_LOCK_$($kind.ToUpperInvariant())", $null)
}
Import-Module (Join-Path $scriptRoot 'lib/HostResourceLock.psm1') -Force

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

function Start-ForeignHolder {
    <#
    .SYNOPSIS
        A second process that takes one host lock and holds it until told to stop.
    .DESCRIPTION
        Signals through a file that the lock is held, so the test does not race the
        child's startup. Returns the process and the release file.
    #>
    param([Parameter(Mandatory)] [string] $Kind)
    $dir = Join-Path ([IO.Path]::GetTempPath()) "host-lock-tests/$([guid]::NewGuid().ToString('n'))"
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
    $held = Join-Path $dir 'held'
    $release = Join-Path $dir 'release'
    $module = (Join-Path $scriptRoot 'lib/HostResourceLock.psm1') -replace '\\', '/'

    $body = @"
Import-Module '$module' -Force
`$lock = Enter-HostLock -Kind '$Kind' -Holder 'foreign'
Set-Content -LiteralPath '$($held -replace '\\', '/')' -Value 'held'
while (-not (Test-Path -LiteralPath '$($release -replace '\\', '/')')) { Start-Sleep -Milliseconds 50 }
Exit-HostLock -Lock `$lock
"@
    $process = Start-Process -FilePath 'pwsh' -PassThru -WindowStyle Hidden `
        -ArgumentList @('-NoProfile', '-NonInteractive', '-Command', $body)

    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    while (-not (Test-Path -LiteralPath $held) -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 50 }
    if (-not (Test-Path -LiteralPath $held)) {
        $process.Kill()
        throw 'the foreign holder never reported that it took the lock'
    }
    return [pscustomobject]@{ Process = $process; Release = $release; Dir = $dir }
}

function Stop-ForeignHolder {
    param([Parameter(Mandatory)] $Holder)
    Set-Content -LiteralPath $Holder.Release -Value 'go'
    if (-not $Holder.Process.WaitForExit(15000)) { $Holder.Process.Kill() }
    Remove-Item -LiteralPath $Holder.Dir -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host 'host resource locks'

Test-Case 'a lock nobody holds is taken at once' {
    $lock = Enter-HostLock -Kind 'build' -Timeout ([TimeSpan]::FromSeconds(5))
    try {
        Assert-True (-not $lock.Waited) 'an uncontended lock reported a wait'
        Assert-True (Test-HostLockHeld -Kind 'build') 'the lock is held and the probe does not see it'
    }
    finally { Exit-HostLock -Lock $lock }
    Assert-True (-not (Test-HostLockHeld -Kind 'build')) 'the lock was released and the probe still sees it'
}

Test-Case 'a lock held by another process is seen from this one' {
    # The whole point: CTest RESOURCE_LOCK cannot see a second ctest. This can.
    $foreign = Start-ForeignHolder -Kind 'build'
    try {
        Assert-True (Test-HostLockHeld -Kind 'build') 'a lock held by a foreign process looked free'
    }
    finally { Stop-ForeignHolder -Holder $foreign }
}

Test-Case 'waiting on a foreign holder times out rather than proceeding' {
    # Proceeding unlocked after a long wait would be exactly the collision the lock
    # prevents, so the deadline is an error the caller has to handle.
    $foreign = Start-ForeignHolder -Kind 'build'
    try {
        $threw = $false
        $message = ''
        try { Enter-HostLock -Kind 'build' -Timeout ([TimeSpan]::FromMilliseconds(300)) | Out-Null }
        catch { $threw = $true; $message = $_.Exception.Message }
        Assert-True $threw 'a second run took a lock another process was holding'
        Assert-True ($message -match 'held by another run') 'the refusal did not say why'
    }
    finally { Stop-ForeignHolder -Holder $foreign }
}

Test-Case 'the second run gets the lock when the first releases it, and says it waited' {
    $foreign = Start-ForeignHolder -Kind 'device'
    try {
        # Release after a delay from a third party so the wait is real.
        $release = $foreign.Release
        $releaser = Start-Job -ScriptBlock {
            param($path)
            Start-Sleep -Milliseconds 2500
            Set-Content -LiteralPath $path -Value 'go'
        } -ArgumentList $release

        $lock = Enter-HostLock -Kind 'device' -Timeout ([TimeSpan]::FromSeconds(30))
        try {
            Assert-True $lock.Waited 'the run did not report waiting for the other holder'
            Assert-True ($lock.WaitedFor.TotalMilliseconds -ge 1500) `
                "waited only $([int]$lock.WaitedFor.TotalMilliseconds) ms; the holder was not really holding"
        }
        finally { Exit-HostLock -Lock $lock }
        Receive-Job -Job $releaser -Wait -AutoRemoveJob | Out-Null
    }
    finally { Stop-ForeignHolder -Holder $foreign }
}

Test-Case 'the two locks are independent' {
    # A build in one worktree must not stop a test run in another from starting
    # its OWN wait on the device lock; only the same kind contends.
    $foreign = Start-ForeignHolder -Kind 'build'
    try {
        $lock = Enter-HostLock -Kind 'device' -Timeout ([TimeSpan]::FromSeconds(5))
        try { Assert-True (-not $lock.Waited) 'the device lock waited on a build holder' }
        finally { Exit-HostLock -Lock $lock }
    }
    finally { Stop-ForeignHolder -Holder $foreign }
}

Test-Case 'a holder that died releases the lock to the next run' {
    # A crashed build must not leave the host locked. Killed rather than released:
    # that is the case being tested.
    $foreign = Start-ForeignHolder -Kind 'build'
    $foreign.Process.Kill()
    $foreign.Process.WaitForExit(5000) | Out-Null
    Remove-Item -LiteralPath $foreign.Dir -Recurse -Force -ErrorAction SilentlyContinue

    $lock = Enter-HostLock -Kind 'build' -Timeout ([TimeSpan]::FromSeconds(5))
    try { Assert-True ($null -ne $lock) 'the lock of a dead holder could not be taken' }
    finally { Exit-HostLock -Lock $lock }
}

Test-Case 'the body runs under the lock and the lock is released afterwards, also on a throw' {
    $ran = $false
    Invoke-WithHostLock -Kind 'build' -Body {
        $script:ranInside = Test-HostLockHeld -Kind 'build'
    } | Out-Null
    Assert-True $script:ranInside 'the body did not run with the lock held'
    Assert-True (-not (Test-HostLockHeld -Kind 'build')) 'the lock outlived the body'

    $threw = $false
    try { Invoke-WithHostLock -Kind 'build' -Body { throw 'step failed' } | Out-Null } catch { $threw = $true }
    Assert-True $threw 'the body exception was swallowed'
    Assert-True (-not (Test-HostLockHeld -Kind 'build')) 'a throwing body left the lock held'
    [void]$ran
}

Test-Case 'a child of the holder runs under the parent hold instead of waiting on itself' {
    # The deadlock this exists to prevent: run-tests.ps1 holds the device lock and
    # runs a test that itself starts run-tests.ps1. A mutex is recursive per thread
    # and for nothing else, so without the inheritance the child would wait forever
    # on a lock its own ancestor holds.
    $lock = Enter-HostLock -Kind 'device' -Timeout ([TimeSpan]::FromSeconds(5))
    try {
        $module = (Join-Path $scriptRoot 'lib/HostResourceLock.psm1') -replace '\\', '/'
        $body = "Import-Module '$module' -Force; " +
            "`$l = Enter-HostLock -Kind 'device' -Timeout ([TimeSpan]::FromMilliseconds(500)); " +
            "if (`$l.Inherited) { exit 0 } else { exit 4 }"
        $child = Start-Process -FilePath 'pwsh' -PassThru -Wait -WindowStyle Hidden `
            -ArgumentList @('-NoProfile', '-NonInteractive', '-Command', $body)
        Assert-True ($child.ExitCode -eq 0) "the child did not inherit the hold (exit $($child.ExitCode))"
    }
    finally { Exit-HostLock -Lock $lock }

    # And the mark does not outlive the hold: a process started AFTER the release
    # takes its own lock.
    Assert-True (-not (Test-HostLockInherited -Kind 'device')) 'the inheritance mark outlived the lock'
}

Test-Case 'an inherited lock is never released by the child' {
    # Otherwise the child would release its parent's hold on exit and the next
    # test in the parent's run would collide with a foreign run after all.
    $lock = Enter-HostLock -Kind 'build' -Timeout ([TimeSpan]::FromSeconds(5))
    try {
        $module = (Join-Path $scriptRoot 'lib/HostResourceLock.psm1') -replace '\\', '/'
        $body = "Import-Module '$module' -Force; " +
            "`$l = Enter-HostLock -Kind 'build'; Exit-HostLock -Lock `$l; exit 0"
        Start-Process -FilePath 'pwsh' -Wait -WindowStyle Hidden `
            -ArgumentList @('-NoProfile', '-NonInteractive', '-Command', $body)
        Assert-True (Test-HostLockHeld -Kind 'build') 'the child released a hold that was not its own'
    }
    finally { Exit-HostLock -Lock $lock }
}

Test-Case 'the job budget leaves the machine usable and the operator wins' {
    $cores = [Environment]::ProcessorCount
    $default = Get-HostJobBudget -Override ''
    Assert-True ($default -ge 1) 'a budget below one job runs nothing'
    Assert-True ($default -le [Math]::Max(1, $cores - 2)) `
        "the default budget $default takes every core of $cores; the shell and the editor get none"

    Assert-True ((Get-HostJobBudget -Override '3') -eq 3) 'an explicit operator value was not honoured'
    Assert-True ((Get-HostJobBudget -Override 'lots') -eq $default) 'an unparseable value was not treated as unset'
    Assert-True ((Get-HostJobBudget -Override '0') -eq $default) 'zero was not treated as unset'
}

Write-Host ''
Write-Host "$($script:Passed) passed, $($script:Failed) failed."
if ($script:Failed -gt 0) { exit 1 }
exit 0
