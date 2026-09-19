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

    Three lock kinds, because the contention is different:

      tree    ONE build directory. Whoever holds it is the only cooperating entry
              point that may configure, build or judge that tree. Bound to the
              canonical path, so two independent build trees do not wait for each
              other. This is the lock that makes a test result evidence: a second
              entry point that rebuilt the tree between the build and the receipt
              would leave a result describing binaries nobody can name.
      build   compiler processes, host-wide. One build at a time on the host; the
              parallelism inside it is the build's own business. It bounds the
              machine, not the correctness of a result.
      device  the GPU and the interactive desktop. One test run, one live check or
              one VM campaign at a time -- a GPU test and a recording soak running
              together share an encoder neither of them measures.

    LOCK ORDER, where a step needs more than one: tree, then build, then device.
    Every caller acquires in that order and releases in reverse, so no two holders
    can wait on each other. Nothing may take `build` or `device` and then ask for
    `tree`.

    What the tree lock does NOT cover: a bare `cmake --build` typed into a shell, or
    any tool that does not use this module. The protection is a protocol between the
    repository's own entry points, not enforcement by the filesystem.

    A lock is a named Mutex in the Global namespace, so it is seen across sessions
    and released by the operating system if its holder dies -- a crashed build does
    not leave the host locked. Abandoned is treated as acquired for that reason.

    A holder passes the lock down to its children. A mutex is recursive for the
    thread that owns it and for nothing else, so a run-tests.ps1 that holds the
    device lock and then runs a test which itself starts run-tests.ps1 would wait on
    itself forever. The holder therefore marks the environment, and a child that
    finds the mark runs under its parent's hold instead of taking one of its own --
    the parent has the device, and the child is part of what it is doing with it.

    The tree marker is per tree, not per kind: a child that inherited the hold on one
    build directory is not thereby excused from locking a different one.
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

function Get-HostLockTreeKey {
    <#
    .SYNOPSIS
        The identity of one build tree: a short digest of its canonical path.
    .DESCRIPTION
        The directory need not exist yet -- a configure run creates it, and it has to
        be locked before it does. Case is folded because the paths compared here are
        Windows paths, where two spellings of one directory are one resource.
    #>
    [OutputType([string])]
    param([Parameter(Mandatory)] [string] $Path)

    if ([string]::IsNullOrWhiteSpace($Path)) { throw 'a tree lock needs the build directory it protects' }
    $full = [System.IO.Path]::GetFullPath($Path).TrimEnd([char]'\', [char]'/').ToLowerInvariant()

    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $digest = [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($full)))
    }
    finally { $sha.Dispose() }
    return $digest.Replace('-', '').Substring(0, 16).ToLowerInvariant()
}

function Get-HostLockName {
    <#
    .SYNOPSIS
        The operating-system name of one host lock. A tree lock needs its build directory.
    #>
    [OutputType([string])]
    param(
        [Parameter(Mandatory)] [ValidateSet('build', 'device', 'tree')] [string] $Kind,
        [string] $Path
    )
    if ($Kind -eq 'tree') { return "Global\$script:Namespace.Tree.$(Get-HostLockTreeKey -Path $Path)" }
    if (-not [string]::IsNullOrWhiteSpace($Path)) { throw "the '$Kind' lock is host-wide and takes no path" }
    return $script:LockNames[$Kind]
}

function Get-HostLockInheritVariable {
    <#
    .SYNOPSIS
        The environment variable a holder sets so its own children recognise its hold.
    #>
    [OutputType([string])]
    param(
        [Parameter(Mandatory)] [ValidateSet('build', 'device', 'tree')] [string] $Kind,
        [string] $Path
    )
    if ($Kind -eq 'tree') { return "EXOSNAP_HOST_LOCK_TREE_$(Get-HostLockTreeKey -Path $Path)" }
    return $script:InheritVariables[$Kind]
}

function Get-HostLockTimeout {
    <#
    .SYNOPSIS
        How long a caller waits for a lock before giving up.
    .DESCRIPTION
        EXOSNAP_HOST_LOCK_TIMEOUT_SECONDS, when set and positive, wins. It exists so a
        contention test can assert the refusal in seconds instead of half an hour; a
        real run has no reason to set it.
    #>
    [OutputType([TimeSpan])]
    param([string] $Override = $env:EXOSNAP_HOST_LOCK_TIMEOUT_SECONDS)
    $parsed = 0.0
    if (-not [string]::IsNullOrWhiteSpace($Override) -and
        [double]::TryParse($Override, [Globalization.NumberStyles]::Float,
            [Globalization.CultureInfo]::InvariantCulture, [ref]$parsed) -and $parsed -gt 0) {
        return [TimeSpan]::FromSeconds($parsed)
    }
    return [TimeSpan]::FromMinutes(30)
}

function Test-HostLockInherited {
    <#
    .SYNOPSIS
        Whether a process above this one already holds a host lock for it.
    #>
    [OutputType([bool])]
    param(
        [Parameter(Mandatory)] [ValidateSet('build', 'device', 'tree')] [string] $Kind,
        [string] $Path
    )
    $value = [Environment]::GetEnvironmentVariable((Get-HostLockInheritVariable -Kind $Kind -Path $Path))
    return -not [string]::IsNullOrWhiteSpace($value)
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
        [Parameter(Mandatory)] [ValidateSet('build', 'device', 'tree')] [string] $Kind,
        # The build directory a 'tree' lock protects. Rejected for the other kinds.
        [string] $Path,
        [TimeSpan] $Timeout = [TimeSpan]::Zero,
        # Who is waiting, for the message. A hook names its worktree; a campaign its run id.
        [string] $Holder = "$PID"
    )
    if ($Timeout -eq [TimeSpan]::Zero) { $Timeout = Get-HostLockTimeout }
    $name = Get-HostLockName -Kind $Kind -Path $Path
    $inheritVariable = Get-HostLockInheritVariable -Kind $Kind -Path $Path

    # Already held above this process: this run is part of what the holder is
    # doing with the resource, and waiting on it would be waiting on ourselves.
    if (Test-HostLockInherited -Kind $Kind -Path $Path) {
        return [pscustomobject]@{
            Kind            = $Kind
            Path            = $Path
            Holder          = $Holder
            Mutex           = $null
            InheritVariable = $inheritVariable
            Inherited       = $true
            WaitedFor       = [TimeSpan]::Zero
            Waited          = $false
        }
    }

    $mutex = [System.Threading.Mutex]::new($false, $name)
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
        $contended = if ($Kind -eq 'tree') { "build tree '$Path'" } else { "$Kind step on this machine" }
        throw ("host lock '$Kind' is held by another run and was not released within " +
            "$([int]$Timeout.TotalSeconds) s; a second $contended would compete with it " +
            'for the same cores, the same device or the same binaries')
    }

    # Mark it for the children started while it is held.
    [Environment]::SetEnvironmentVariable($inheritVariable, $Holder)

    $waited = [DateTime]::UtcNow - $started
    return [pscustomobject]@{
        Kind            = $Kind
        Path            = $Path
        Holder          = $Holder
        Mutex           = $mutex
        InheritVariable = $inheritVariable
        Inherited       = $false
        WaitedFor       = $waited
        Waited          = $waited -ge [TimeSpan]::FromSeconds(2)
    }
}

function Exit-HostLock {
    <#
    .SYNOPSIS
        Releases a lock returned by Enter-HostLock. An inherited one is not ours to release.
    #>
    param([Parameter(Mandatory)] $Lock)
    if ($Lock.Inherited) { return }
    [Environment]::SetEnvironmentVariable($Lock.InheritVariable, $null)
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
    param(
        [Parameter(Mandatory)] [ValidateSet('build', 'device', 'tree')] [string] $Kind,
        [string] $Path
    )
    $name = Get-HostLockName -Kind $Kind -Path $Path
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
        [Parameter(Mandatory)] [ValidateSet('build', 'device', 'tree')] [string] $Kind,
        [Parameter(Mandatory)] [scriptblock] $Body,
        [string] $Path,
        [TimeSpan] $Timeout = [TimeSpan]::Zero,
        [string] $Holder = "$PID"
    )
    $lock = Enter-HostLock -Kind $Kind -Path $Path -Timeout $Timeout -Holder $Holder
    try {
        if ($lock.Waited) {
            $what = if ($Kind -eq 'tree') { "tree lock on $Path" } else { "host $Kind lock" }
            Write-Host ("  waited {0:0}s for the {1} held by another run" -f $lock.WaitedFor.TotalSeconds, $what) `
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

Export-ModuleMember -Function Get-HostLockName, Get-HostLockTreeKey, Get-HostLockInheritVariable,
    Get-HostLockTimeout, Enter-HostLock, Exit-HostLock, Test-HostLockHeld,
    Test-HostLockInherited, Invoke-WithHostLock, Get-HostJobBudget
