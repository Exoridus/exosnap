#Requires -Version 7.0
<#
.SYNOPSIS
    Host-wide locks for the work that two worktrees must not do at the same time.

.DESCRIPTION
    Two verify runs on one machine -- a pre-commit in one worktree while a pre-push
    runs in another, or an agent's gate beside the developer's -- each start a build
    at full parallelism, a test run at full parallelism and a clang-tidy at full
    parallelism. None of them knows about the others. The machine then runs several
    times its core count in compiler processes, the timing-sensitive tests flake, and
    every run is slower than it would have been in sequence.

    CTest's RESOURCE_LOCK serialises tests within ONE ctest; it cannot see a second
    one. This is the layer above it: a named lock on the host, held for the duration
    of one heavy step, so a second run of the same kind waits instead of competing.

    Two locks, because the contention is different:

      build   compiler processes. One build at a time on the host; the parallelism
              inside it is the build's own business.
      device  the GPU and the interactive desktop. One test run, one live check or
              one VM campaign at a time -- a GPU test and a recording soak running
              together share an encoder neither of them measures.

    A lock is a named Mutex in the Global namespace, so it is seen across sessions
    and released by the operating system if its holder dies -- a crashed build does
    not leave the host locked. Abandoned is treated as acquired for that reason.

    A holder passes the lock down to its children. A mutex is recursive for the
    thread that owns it and for nothing else, so a run-tests.ps1 that holds the
    device lock and then runs a test which itself starts run-tests.ps1 would wait on
    itself forever. The holder therefore marks the environment, and a child that
    finds the mark runs under its parent's hold instead of taking one of its own --
    the parent has the device, and the child is part of what it is doing with it.
#>

Set-StrictMode -Version Latest

# EXOSNAP_HOST_LOCK_NAMESPACE moves every lock to a different set of names. Only
# the module's own tests set it: they measure real contention by holding real
# locks from a second process, and under CTest they run inside a run-tests.ps1
# that already holds the real device lock -- the tests would then be contending
# with the machine that is running them. A namespace of their own is the same
# mechanism on locks nothing else is holding.
$script:Namespace = if ([string]::IsNullOrWhiteSpace($env:EXOSNAP_HOST_LOCK_NAMESPACE)) { 'ExoSnap.Host' }
else { "ExoSnap.Host.$($env:EXOSNAP_HOST_LOCK_NAMESPACE)" }

$script:LockNames = @{
    build  = "Global\$script:Namespace.Build"
    device = "Global\$script:Namespace.Device"
}

# The variable a holder sets for its children, per kind. The value is the holder
# name, which is only for the message: any non-empty value means "already held
# above you".
$script:InheritVariables = @{
    build  = 'EXOSNAP_HOST_LOCK_BUILD'
    device = 'EXOSNAP_HOST_LOCK_DEVICE'
}

function Test-HostLockInherited {
    <#
    .SYNOPSIS
        Whether a process above this one already holds a host lock for it.
    #>
    [OutputType([bool])]
    param([Parameter(Mandatory)] [ValidateSet('build', 'device')] [string] $Kind)
    $value = [Environment]::GetEnvironmentVariable($script:InheritVariables[$Kind])
    return -not [string]::IsNullOrWhiteSpace($value)
}

function Get-HostLockName {
    <#
    .SYNOPSIS
        The operating-system name of one host lock.
    #>
    [OutputType([string])]
    param([Parameter(Mandatory)] [ValidateSet('build', 'device')] [string] $Kind)
    return $script:LockNames[$Kind]
}

function Enter-HostLock {
    <#
    .SYNOPSIS
        Acquires one host lock, waiting up to a deadline for another holder to let go.
    .DESCRIPTION
        Returns a handle to pass to Exit-HostLock, or throws when the deadline passed.
        Throwing rather than proceeding unlocked: a step that ran anyway because the
        wait was long would be exactly the collision the lock exists to prevent, and
        a caller that would rather skip can catch.

        The wait is reported, so a run that sat behind another for a minute says so
        instead of looking slow.
    #>
    param(
        [Parameter(Mandatory)] [ValidateSet('build', 'device')] [string] $Kind,
        [TimeSpan] $Timeout = [TimeSpan]::FromMinutes(30),
        # Who is waiting, for the message. A hook names its worktree; a campaign its run id.
        [string] $Holder = "$PID"
    )
    # Already held above this process: this run is part of what the holder is
    # doing with the resource, and waiting on it would be waiting on ourselves.
    if (Test-HostLockInherited -Kind $Kind) {
        return [pscustomobject]@{
            Kind      = $Kind
            Holder    = $Holder
            Mutex     = $null
            Inherited = $true
            WaitedFor = [TimeSpan]::Zero
            Waited    = $false
        }
    }

    $mutex = [System.Threading.Mutex]::new($false, (Get-HostLockName -Kind $Kind))
    $started = [DateTime]::UtcNow
    $acquired = $false
    try {
        $acquired = $mutex.WaitOne($Timeout)
    }
    catch [System.Threading.AbandonedMutexException] {
        # The previous holder died without releasing. The lock is ours and nothing
        # about the resource is inconsistent -- a build or a test run leaves no
        # shared state behind that a new one could misread.
        $acquired = $true
    }

    if (-not $acquired) {
        $mutex.Dispose()
        throw ("host lock '$Kind' is held by another run and was not released within " +
            "$([int]$Timeout.TotalSeconds) s; a second $Kind step on this machine would compete " +
            "with it for the same cores or the same device")
    }

    # Mark it for the children started while it is held.
    [Environment]::SetEnvironmentVariable($script:InheritVariables[$Kind], $Holder)

    $waited = [DateTime]::UtcNow - $started
    return [pscustomobject]@{
        Kind      = $Kind
        Holder    = $Holder
        Mutex     = $mutex
        Inherited = $false
        WaitedFor = $waited
        Waited    = $waited -ge [TimeSpan]::FromSeconds(2)
    }
}

function Exit-HostLock {
    <#
    .SYNOPSIS
        Releases a lock returned by Enter-HostLock. An inherited one is not ours to release.
    #>
    param([Parameter(Mandatory)] $Lock)
    if ($Lock.Inherited) { return }
    [Environment]::SetEnvironmentVariable($script:InheritVariables[$Lock.Kind], $null)
    try { $Lock.Mutex.ReleaseMutex() } catch [System.ApplicationException] { }
    $Lock.Mutex.Dispose()
}

function Test-HostLockHeld {
    <#
    .SYNOPSIS
        Whether ANOTHER process currently holds a host lock. Read-only: does not acquire.
    .DESCRIPTION
        Asked from a separate process on purpose, which is exactly the question: would a
        second run on this machine have to wait. A Windows mutex is recursive for the
        thread that owns it, so a probe on the owning thread is granted the lock it is
        asking about and reports it free -- the one answer that cannot be right while
        this process holds it. A PowerShell script block cannot run on a bare second
        thread either, so the separate questioner is a child pwsh.
    #>
    [OutputType([bool])]
    param([Parameter(Mandatory)] [ValidateSet('build', 'device')] [string] $Kind)
    $name = Get-HostLockName -Kind $Kind
    $probe = @"
`$m = [System.Threading.Mutex]::new(`$false, '$name')
try {
    `$got = `$false
    try { `$got = `$m.WaitOne(0) } catch [System.Threading.AbandonedMutexException] { `$got = `$true }
    if (`$got) { `$m.ReleaseMutex(); exit 0 }
    exit 3
}
finally { `$m.Dispose() }
"@
    & pwsh -NoProfile -NonInteractive -Command $probe *> $null
    return $LASTEXITCODE -eq 3
}

function Invoke-WithHostLock {
    <#
    .SYNOPSIS
        Runs a script block while holding one host lock, and always releases it.
    #>
    param(
        [Parameter(Mandatory)] [ValidateSet('build', 'device')] [string] $Kind,
        [Parameter(Mandatory)] [scriptblock] $Body,
        [TimeSpan] $Timeout = [TimeSpan]::FromMinutes(30),
        [string] $Holder = "$PID"
    )
    $lock = Enter-HostLock -Kind $Kind -Timeout $Timeout -Holder $Holder
    try {
        if ($lock.Waited) {
            Write-Host ("  waited {0:0}s for the host {1} lock held by another run" -f $lock.WaitedFor.TotalSeconds, $Kind) `
                -ForegroundColor DarkYellow
        }
        return & $Body
    }
    finally { Exit-HostLock -Lock $lock }
}

function Get-HostJobBudget {
    <#
    .SYNOPSIS
        How many parallel jobs one heavy step may use on this host.
    .DESCRIPTION
        The lock serialises runs; this bounds the one that got through. Without it a
        single build still takes every core and the developer's machine stops
        answering for the duration. EXOSNAP_VERIFY_JOBS, when set and positive, is the
        operator's answer and wins; otherwise all cores but two, floor one, so the
        shell and the editor keep a core each while a gate runs.
    #>
    [OutputType([int])]
    param([string] $Override = $env:EXOSNAP_VERIFY_JOBS)
    $parsed = 0
    if (-not [string]::IsNullOrWhiteSpace($Override) -and [int]::TryParse($Override, [ref]$parsed) -and $parsed -gt 0) {
        return $parsed
    }
    return [Math]::Max(1, [Environment]::ProcessorCount - 2)
}

Export-ModuleMember -Function Get-HostLockName, Enter-HostLock, Exit-HostLock, Test-HostLockHeld,
    Test-HostLockInherited, Invoke-WithHostLock, Get-HostJobBudget
