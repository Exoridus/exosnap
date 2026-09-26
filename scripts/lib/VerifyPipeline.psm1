#Requires -Version 7.0
<#
.SYNOPSIS
    Command-line batching and tool result caches for scripts/check-quality.ps1.

.DESCRIPTION
    What is left of the PowerShell verification pipeline after its orchestration
    moved to exo-dev (tools/exo-dev). The cache directory derivation is mirrored
    by exo-dev's clang-tidy step, so both address the same entries for the same
    toolchain.
#>

Set-StrictMode -Version Latest

function Split-VerifyCommandLineBatch {
    <#
    .SYNOPSIS
        Splits a file list into invocations that fit on a Windows command line.
    .DESCRIPTION
        CreateProcess accepts at most 32767 characters, and the failure mode when
        a caller exceeds it is not a diagnosable error: the process never starts,
        so a step wrapped in continue-on-error reports a clean pass over an
        analysis that did not happen. Every list-of-files invocation in this
        repository therefore goes through here rather than trusting a file count
        to stay small.

        Two independent limits. The budget is the hard one. The item cap is a
        throughput choice: batches run in parallel, so smaller ones spread more
        evenly across cores.
    .PARAMETER Item
        The per-invocation arguments to distribute, one file path each.
    .PARAMETER FixedArgument
        Arguments repeated on every invocation; they consume the same budget.
    .PARAMETER MaximumItem
        Upper bound on items per batch.
    .PARAMETER CommandLineBudget
        Characters available for the whole command line. The default leaves room
        under the 32767 limit for the executable path and for quoting the shell
        adds around arguments containing spaces.
    #>
    [OutputType([object[]])]
    param(
        [string[]] $Item = @(),
        [string[]] $FixedArgument = @(),
        [int] $MaximumItem = 25,
        [int] $CommandLineBudget = 30000
    )

    # +3 per argument: a separating space plus the pair of quotes a path with a
    # space in it acquires. Charged to every argument, so the estimate can only
    # be too large.
    $cost = { param([string] $Argument) $Argument.Length + 3 }

    $fixedCost = 0
    foreach ($argument in $FixedArgument) { $fixedCost += (& $cost $argument) }

    $batches = [System.Collections.Generic.List[object]]::new()
    $current = [System.Collections.Generic.List[string]]::new()
    $currentCost = $fixedCost

    foreach ($entry in $Item) {
        $entryCost = & $cost $entry
        if ($current.Count -gt 0 -and
            (($current.Count -ge $MaximumItem) -or ($currentCost + $entryCost -gt $CommandLineBudget))) {
            $batches.Add(@($current.ToArray()))
            $current.Clear()
            $currentCost = $fixedCost
        }
        $current.Add($entry)
        $currentCost += $entryCost
    }
    if ($current.Count -gt 0) { $batches.Add(@($current.ToArray())) }

    return , @($batches)
}

function Get-VerifyToolCacheDirectory {
    <#
    .SYNOPSIS
        Where one analysis tool keeps its reusable results on this machine.
    .DESCRIPTION
        Outside the repository and outside every build tree, on purpose: both are
        wiped by the operations that precede a slow run -- a fresh configure, a
        `git clean`, a new worktree -- which is exactly when replaying earlier
        results would have paid the most. One location instead means several
        worktrees of the same repository share the cache.

        The leaf is derived from the fingerprint, never spelled out by a caller,
        so a toolchain the fingerprint names cannot serve results produced by a
        different one: a changed compiler or tool version simply addresses a
        different directory rather than aging stale entries out of a shared one.
    .PARAMETER Tool
        The tool the cache belongs to; becomes a path segment.
    .PARAMETER Fingerprint
        Everything that identifies the toolchain the results are valid for.
        Empty entries are dropped, and an entirely empty fingerprint is recorded
        as such rather than silently sharing the unqualified directory.
    .PARAMETER Root
        Overrides the per-user cache root. For tests.
    #>
    [OutputType([string])]
    param(
        [Parameter(Mandatory)] [string] $Tool,
        [string[]] $Fingerprint = @(),
        [string] $Root
    )

    if (-not $Root) {
        $Root = if ($env:LOCALAPPDATA) {
            Join-Path $env:LOCALAPPDATA 'ExoSnap/tool-cache'
        }
        else {
            Join-Path ([System.IO.Path]::GetTempPath()) 'exosnap-tool-cache'
        }
    }

    $parts = @($Fingerprint | Where-Object { $_ })
    if ($parts.Count -eq 0) { $parts = @('unqualified-toolchain') }

    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $digest = $sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes(($parts -join "`n")))
    }
    finally {
        $sha.Dispose()
    }
    $key = [System.BitConverter]::ToString($digest).Replace('-', '').Substring(0, 16).ToLowerInvariant()

    return (Join-Path (Join-Path $Root $Tool) $key)
}

Export-ModuleMember -Function @(
    'Get-VerifyToolCacheDirectory',
    'Split-VerifyCommandLineBatch'
)
