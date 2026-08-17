#Requires -Version 7.0
Set-StrictMode -Version Latest

<#
.SYNOPSIS
    Transactional Windows environment orchestration for the release runner.

.DESCRIPTION
    The PowerShell half of Wave D's environment orchestrator. The Windows reads and
    mutations themselves live in `exosnap-envctl.exe` (tools/envctl), which is a
    TEST-ONLY executable: it is never installed, never linked into exosnap.exe, and
    never started as a service or on login.

    That split is the point, and it is a trust boundary rather than a packaging
    detail:

        product truth      -> ExoSnap's own semantic automation (the control channel)
        environment truth  -> this orchestrator + exosnap-envctl
        physical truth     -> the operator, plus an automated check of the consequence
        secure truth       -> UAC, plus an automated check of before and after

    ExoSnap itself therefore never grows `windows.setHdr`, `windows.setRefreshRate`,
    `windows.setDefaultAudio`, `registry.set` or `shell.execute`. A recording
    application that can reconfigure the machine is a different product with a
    different threat model, and the release runner needing to set HDR is not a reason
    to ship one.

    Every mutation is a transaction. The mandatory sequence, enforced by envctl and
    surfaced here:

        snapshot exact original -> persist recovery journal -> validate desired ->
        apply minimal delta -> read back -> verify actual == desired -> [run the test] ->
        restore exact original -> read back -> verify actual == original -> close

    Two properties of that sequence matter more than the rest. There is no mutation
    before the journal is on disk, so a process kill at any point leaves behind what
    was originally there and what has already changed. And restore means "put back
    what this machine actually had", never "set the defaults" -- a machine that had
    HDR on gets HDR on again, not whatever Windows would pick.

    What this module cannot promise: instantaneous recovery from a power loss or an
    OS crash. Nothing running in user space can. The guarantee is the persistent
    journal plus a mandatory recovery pass on the next runner start, which refuses to
    begin a new mutating scenario while the environment is dirty.
#>

function Write-EnvctlJsonAtomic {
    <#
    .SYNOPSIS
        Temp-file-then-rename write for the desired-state handoff document.
    .DESCRIPTION
        Deliberately local rather than imported from LiveVerifyState.psm1. Importing
        that module here with -Force unloads it from whichever scope already had it --
        a nested -Force re-import rebinds the module to the nested scope and the
        caller silently loses every function it had imported. That failure looks like
        "New-LiveVerifyRun is not recognized" in a script that plainly imports it, so
        the eight lines are cheaper than the coupling.
    #>
    param([Parameter(Mandatory)] [string] $Path, [Parameter(Mandatory)] $Value)
    $directory = Split-Path -Parent $Path
    if (-not (Test-Path -LiteralPath $directory)) {
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
    }
    $temporary = "$Path.tmp"
    Set-Content -LiteralPath $temporary -Value ($Value | ConvertTo-Json -Depth 30) -Encoding utf8NoBOM
    Move-Item -LiteralPath $temporary -Destination $Path -Force
}

$script:EnvctlCandidates = @(
    'build/windows-x64-release/tools/envctl/Release/exosnap-envctl.exe',
    'build/windows-x64-debug/tools/envctl/Debug/exosnap-envctl.exe',
    'build/windows-x64-release/tools/envctl/exosnap-envctl.exe',
    'build/windows-x64-debug/tools/envctl/exosnap-envctl.exe'
)

function Resolve-EnvctlPath {
    <#
    .SYNOPSIS
        Locates exosnap-envctl.exe, or returns $null.
    .DESCRIPTION
        Deliberately returns $null rather than throwing. A machine without the tool
        can still run every scenario that needs no mutation; the scenarios that do
        need one report UNAVAILABLE with the reason, which is a true statement about
        this run. Failing the whole campaign because a test-only helper was not built
        would report a product problem that does not exist.
    #>
    param([string] $ExplicitPath)

    if (-not [string]::IsNullOrWhiteSpace($ExplicitPath)) {
        if (Test-Path -LiteralPath $ExplicitPath) { return (Get-Item -LiteralPath $ExplicitPath).FullName }
        throw "No exosnap-envctl at '$ExplicitPath'"
    }
    if ($env:EXOSNAP_ENVCTL -and (Test-Path -LiteralPath $env:EXOSNAP_ENVCTL)) {
        return (Get-Item -LiteralPath $env:EXOSNAP_ENVCTL).FullName
    }
    $root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
    foreach ($candidate in $script:EnvctlCandidates) {
        $path = Join-Path $root $candidate
        if (Test-Path -LiteralPath $path) { return (Get-Item -LiteralPath $path).FullName }
    }
    $command = Get-Command 'exosnap-envctl' -ErrorAction SilentlyContinue
    if ($null -ne $command) { return $command.Source }
    return $null
}

function Invoke-Envctl {
    <#
    .SYNOPSIS
        Runs one envctl subcommand and returns its parsed JSON.
    .DESCRIPTION
        stdout is JSON, stderr is diagnostics, the exit code is the verdict. Parsed
        strictly: a non-zero exit or unparseable stdout is an error here rather than
        a $null that a caller mistakes for "nothing to do".
    #>
    param(
        [Parameter(Mandatory)] [string] $EnvctlPath,
        [Parameter(Mandatory)] [string[]] $Arguments
    )
    $stdout = & $EnvctlPath @Arguments 2>$null
    $exit = $LASTEXITCODE
    $text = ($stdout | Out-String).Trim()
    if ([string]::IsNullOrWhiteSpace($text)) {
        if ($exit -ne 0) { throw "envctl $($Arguments -join ' ') failed (exit $exit) with no output" }
        return $null
    }
    try { $parsed = $text | ConvertFrom-Json }
    catch { throw "envctl $($Arguments -join ' ') produced unparseable output: $text" }
    if ($exit -ne 0) {
        $detail = if ($parsed.PSObject.Properties.Name -contains 'error') { $parsed.error } else { $text }
        throw "envctl $($Arguments -join ' ') failed (exit $exit): $detail"
    }
    return $parsed
}

function Invoke-EnvctlTolerant {
    <#
    .SYNOPSIS
        Like Invoke-Envctl, but returns the parsed body on a non-zero exit instead of
        throwing.
    .DESCRIPTION
        For the subcommands whose non-zero exit is a VERDICT rather than a fault --
        `resolve-aliases` exits 1 when an alias is unbound, which is the ordinary
        answer on a machine that does not own every device the catalogue can name.
        Throwing there would turn "this desk has no second monitor" into a runner
        crash. Unparseable output is still an error: that is a fault.
    #>
    param(
        [Parameter(Mandatory)] [string] $EnvctlPath,
        [Parameter(Mandatory)] [string[]] $Arguments
    )
    $stdout = & $EnvctlPath @Arguments 2>$null
    $text = ($stdout | Out-String).Trim()
    if ([string]::IsNullOrWhiteSpace($text)) { return $null }
    try { return $text | ConvertFrom-Json }
    catch { throw "envctl $($Arguments -join ' ') produced unparseable output: $text" }
}

function New-EnvironmentOrchestrator {
    <#
    .SYNOPSIS
        Creates the orchestrator context for one release campaign.
    .DESCRIPTION
        Performs the mandatory startup recovery immediately: if a journal from a
        killed runner is on disk, it is restored and verified BEFORE this returns,
        and `Dirty` stays true if that restore did not succeed. A caller must not
        start a mutating scenario against a dirty environment -- Assert-EnvironmentClean
        is the gate that says so out loud.
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] [string] $RunId,
        [Parameter(Mandatory)] [string] $JournalDirectory,
        [string] $EnvctlPath,
        [string] $AliasProfile
    )

    $resolved = Resolve-EnvctlPath -ExplicitPath $EnvctlPath
    # One journal FILE per campaign, inside the run directory. envctl takes a file
    # path, not a directory: a transaction has exactly one journal, and the whole
    # dirty-startup gate is "does that file exist and is it unfinished".
    $journalPath = Join-Path $JournalDirectory 'env-journal.json'
    $context = [pscustomobject]@{
        RunId            = $RunId
        JournalDirectory = $JournalDirectory
        JournalPath      = $journalPath
        EnvctlPath       = $resolved
        AliasProfile     = $AliasProfile
        Available        = ($null -ne $resolved)
        Dirty            = $false
        DirtyDetail      = $null
        Recovered        = @()
    }
    if (-not $context.Available) { return $context }

    if (-not (Test-Path -LiteralPath $JournalDirectory)) {
        New-Item -ItemType Directory -Path $JournalDirectory -Force | Out-Null
    }

    $arguments = @('recover', '--journal', $journalPath)
    if (-not [string]::IsNullOrWhiteSpace($AliasProfile)) { $arguments += @('--profile', $AliasProfile) }
    try {
        $result = Invoke-Envctl -EnvctlPath $resolved -Arguments $arguments
        if ($null -ne $result) {
            # `mutationAllowed` is the gate envctl computes, and it is the only field
            # worth trusting here: a journal that was present and restored leaves it
            # true, and anything else -- unreadable journal, a restore that could not
            # verify, a device still missing -- leaves it false. Deriving "dirty" from
            # `journalPresent` instead would call a successfully recovered run dirty.
            if ($result.PSObject.Properties.Name -contains 'evidence' -and $null -ne $result.evidence -and
                $result.evidence.PSObject.Properties.Name -contains 'properties') {
                $context.Recovered = @($result.evidence.properties | ForEach-Object { $_.property })
            }
            if ($result.PSObject.Properties.Name -contains 'mutationAllowed' -and -not $result.mutationAllowed) {
                $context.Dirty = $true
                $context.DirtyDetail = "recovery left the environment in state '$($result.state)'"
                if ($result.PSObject.Properties.Name -contains 'error' -and $result.error) {
                    $context.DirtyDetail += ": $($result.error)"
                }
            }
        }
    }
    catch {
        # A recovery pass that itself failed is the strongest possible reason not to
        # mutate anything else. Recorded as dirty rather than rethrown, so the
        # campaign can still run its read-only and product-only scenarios and report
        # exactly why the mutating ones did not.
        $context.Dirty = $true
        $context.DirtyDetail = $_.Exception.Message
    }
    return $context
}

function Assert-EnvironmentClean {
    <#
    .SYNOPSIS
        Hard gate: no new mutating scenario while a previous transaction is unrestored.
    #>
    param([Parameter(Mandatory)] $Orchestrator)
    if ($Orchestrator.Dirty) {
        throw "The environment is dirty from an earlier run and was not restored: $($Orchestrator.DirtyDetail). " +
        'Restore it (scripts/release-verify.ps1 recover) before running a mutating scenario.'
    }
}

function Get-EnvironmentSnapshot {
    <#
    .SYNOPSIS
        Full read-only environment snapshot, keyed by stable Windows identifiers.
    #>
    param([Parameter(Mandatory)] $Orchestrator)
    if (-not $Orchestrator.Available) { return $null }
    $arguments = @('snapshot')
    if (-not [string]::IsNullOrWhiteSpace($Orchestrator.AliasProfile)) {
        $arguments += @('--profile', $Orchestrator.AliasProfile)
    }
    return Invoke-Envctl -EnvctlPath $Orchestrator.EnvctlPath -Arguments $arguments
}

function Get-EnvironmentCapabilities {
    <#
    .SYNOPSIS
        The capability classification table: what is readable, what is safely mutable,
        and what only a human or a physical act can change on this machine.
    #>
    param([Parameter(Mandatory)] $Orchestrator)
    if (-not $Orchestrator.Available) { return $null }
    return Invoke-Envctl -EnvctlPath $Orchestrator.EnvctlPath -Arguments @('describe')
}

function Resolve-EnvironmentAliases {
    <#
    .SYNOPSIS
        Binds machine-local aliases (display.main-hdr, audio.render.44100-test, ...)
        to stable Windows identifiers.
    .DESCRIPTION
        Scenarios declare aliases, never friendly names -- "27GL850" is a fact about
        one desk, and a check that hardcodes it is untestable everywhere else. The
        friendly name survives only as a label for the human gates.

        Two failures are returned rather than guessed at:
          ambiguous_device -- more than one device matches; nothing is chosen.
          unbound_alias    -- the profile does not bind it; the message says how to.
    #>
    param([Parameter(Mandatory)] $Orchestrator)
    if (-not $Orchestrator.Available) { return $null }
    $arguments = @('resolve-aliases')
    if (-not [string]::IsNullOrWhiteSpace($Orchestrator.AliasProfile)) {
        $arguments += @('--profile', $Orchestrator.AliasProfile)
    }
    # resolve-aliases exits 1 when an alias is missing or ambiguous, which is a
    # perfectly ordinary answer for a machine that does not have every device a
    # catalogue mentions. The BODY is what carries the verdict, so the non-zero exit
    # is read rather than thrown on.
    try { return Invoke-Envctl -EnvctlPath $Orchestrator.EnvctlPath -Arguments $arguments }
    catch { return Invoke-EnvctlTolerant -EnvctlPath $Orchestrator.EnvctlPath -Arguments $arguments }
}

function Test-EnvironmentRequirement {
    <#
    .SYNOPSIS
        Can this machine satisfy a scenario's declared environment requirement?
    .OUTPUTS
        @{ Satisfied = $bool; Reason = 'why not' }
    .DESCRIPTION
        An unmet requirement is UNAVAILABLE, never FAIL. "This machine has no 240 Hz
        mode" is a statement about the desk, and reporting it as a product failure
        would make the release report unreadable exactly where it needs to be trusted.
    #>
    param(
        [Parameter(Mandatory)] $Orchestrator,
        [Parameter(Mandatory)] [hashtable] $Requirement,
        $Aliases
    )
    if (-not $Orchestrator.Available) {
        return @{ Satisfied = $false; Reason = 'exosnap-envctl is not built; environment requirements cannot be resolved' }
    }
    if ($null -eq $Aliases) { $Aliases = Resolve-EnvironmentAliases -Orchestrator $Orchestrator }

    foreach ($key in $Requirement.Keys) {
        $alias = $Requirement[$key]

        # An error carrying this alias wins over a binding row, because that is where
        # `unbound_alias` lands: an alias the profile never bound has no row at all.
        if ($null -ne $Aliases -and $Aliases.PSObject.Properties.Name -contains 'errors') {
            $failure = @($Aliases.errors) | Where-Object { $_.alias -eq $alias } | Select-Object -First 1
            if ($null -ne $failure) {
                return @{ Satisfied = $false; Reason = "$($failure.code): $($failure.message)" }
            }
        }

        $binding = $null
        if ($null -ne $Aliases -and $Aliases.PSObject.Properties.Name -contains 'bindings') {
            $binding = @($Aliases.bindings) | Where-Object { $_.alias -eq $alias } | Select-Object -First 1
        }
        if ($null -eq $binding) {
            return @{ Satisfied = $false
                Reason           = "unbound_alias: '$alias' is not bound on this machine. Bind it once with: " +
                "exosnap-envctl bind-alias --alias $alias --stable-id <id from resolve-aliases>"
            }
        }
        switch ("$($binding.status)") {
            'ok' { }
            # A friendly name that drifted is not a failure: the binding is by stable
            # id, and the name is a label. Reporting it as unmet would make a monitor
            # firmware update look like missing hardware.
            'friendly_name_changed' { }
            'ambiguous_device' {
                return @{ Satisfied = $false; Reason = "ambiguous_device: '$alias' matches more than one device; nothing was chosen" }
            }
            'device_not_present' {
                return @{ Satisfied = $false; Reason = "'$alias' is bound to $($binding.stableId) but that device is not present" }
            }
            default {
                return @{ Satisfied = $false; Reason = "'$alias' reports status '$($binding.status)'" }
            }
        }
    }
    return @{ Satisfied = $true; Reason = $null }
}

function ConvertTo-RestoreResult {
    <#
    .SYNOPSIS
        Maps envctl's terminal transaction state onto the report's restore taxonomy.
    .DESCRIPTION
        Two vocabularies for one fact, deliberately kept apart: envctl speaks about a
        transaction's state machine, the release report speaks about whether the
        machine was put back. The mapping is total -- an unrecognised state becomes
        RESTORE_FAILED, never RESTORED, because "I do not know what happened" and
        "everything is fine" must not be the same answer.
    #>
    param([string] $State)
    switch ("$State") {
        'Restored' { return 'RESTORED' }
        'RestorePending' { return 'RESTORE_PENDING' }
        'RestorePendingDeviceUnavailable' { return 'RESTORE_PENDING_DEVICE_UNAVAILABLE' }
        'RestoreFailed' { return 'RESTORE_FAILED' }
        'Clean' { return 'RESTORED' }   # nothing was ever applied
        default { return 'RESTORE_FAILED' }
    }
}

function Invoke-EnvironmentTransaction {
    <#
    .SYNOPSIS
        Runs $Body with the environment mutated to $Desired, and restores it exactly
        afterwards -- whatever $Body did.
    .DESCRIPTION
        The restore is in a `finally`, and that placement is the contract. It has to
        survive an assertion failure, a product failure, a test bug, a timeout, an
        exception thrown from anywhere in the body, an operator answering FAIL or
        ABORT at a human gate, and a catchable Ctrl+C. In particular a human gate sits
        INSIDE the transaction: an operator who walks away from a gate must not leave
        the machine reconfigured.

        Returns:
          @{ Product = <whatever $Body returned>; RestoreResult = '...';
             Evidence = { property -> before/requested/applied/afterRestore };
             TransactionId = '...'; Error = '...' }

        The product verdict and the restore verdict are separate on purpose. A
        scenario can prove the product correct and still leave a display in the wrong
        mode, and one field cannot say both.
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] $Orchestrator,
        [Parameter(Mandatory)] [string] $Scenario,
        [Parameter(Mandatory)] [hashtable] $Desired,
        [Parameter(Mandatory)] [scriptblock] $Body
    )

    $outcome = @{
        Product        = $null
        RestoreResult  = 'NOT_APPLICABLE'
        Evidence       = $null
        TransactionId  = $null
        Error          = $null
        # Set when the environment could not be brought to the desired state at all,
        # so the scenario body never ran. Carried as a TYPED code rather than a thrown
        # message because the caller has to tell two very different things apart: a
        # display that does not offer the requested mode (UNAVAILABLE -- a fact about
        # this desk) from a setter that claimed success and read back wrong (FAIL -- a
        # fact about the mechanism).
        SetupError     = $null
        SetupErrorCode = $null
    }

    # A desired state with nothing in it mutates nothing, journals nothing and needs
    # no restore. Modelled explicitly rather than falling out of an empty loop, so a
    # scenario that only READS the environment is visibly a different thing from one
    # that mutated and happened to restore.
    if ($Desired.Count -eq 0) {
        $outcome.Product = & $Body $null
        return $outcome
    }

    Assert-EnvironmentClean -Orchestrator $Orchestrator
    if (-not $Orchestrator.Available) {
        throw 'exosnap-envctl is not built; a mutating scenario cannot run'
    }

    $desiredFile = Join-Path $Orchestrator.JournalDirectory "desired-$([guid]::NewGuid().ToString('n')).json"
    # The document envctl reads is { "desired": { "<alias>:<property>": "<value>" } }
    # and every value must be a string -- a JSON number here would be a type error at
    # the boundary rather than a refresh rate.
    Write-EnvctlJsonAtomic -Path $desiredFile -Value @{ desired = $Desired }

    $arguments = @('begin', '--scenario', $Scenario, '--run-id', $Orchestrator.RunId,
        '--journal', $Orchestrator.JournalPath, '--desired', $desiredFile)
    if (-not [string]::IsNullOrWhiteSpace($Orchestrator.AliasProfile)) {
        $arguments += @('--profile', $Orchestrator.AliasProfile)
    }

    $begun = Invoke-EnvctlTolerant -EnvctlPath $Orchestrator.EnvctlPath -Arguments $arguments
    Remove-Item -LiteralPath $desiredFile -Force -ErrorAction SilentlyContinue
    if ($null -eq $begun -or -not $begun.ok) {
        $outcome.SetupErrorCode = if ($null -ne $begun -and $begun.PSObject.Properties.Name -contains 'errorCode') {
            "$($begun.errorCode)"
        }
        else { 'begin_failed' }
        $outcome.SetupError = if ($null -ne $begun -and $begun.PSObject.Properties.Name -contains 'error') {
            "$($begun.error)"
        }
        else { 'envctl begin produced no usable answer' }
        # `begin` rolls back whatever it had already applied before it gave up, so
        # there is nothing to restore here -- and its own journal is gone. Reporting
        # RESTORED would be a claim about a transaction that never opened.
        return $outcome
    }
    $outcome.TransactionId = $begun.transactionId
    $journalPath = $begun.journalPath

    try {
        $outcome.Product = & $Body $begun
    }
    catch {
        # Recorded, not swallowed: the caller decides what a body failure means for
        # the product verdict. The restore below runs either way.
        $outcome.Error = $_.Exception.Message
    }
    finally {
        try {
            # `restore` exits 4 when the environment is still owed something, which is
            # a verdict this function has to REPORT rather than throw on -- the report
            # needs RESTORE_FAILED next to the product result, not a runner stack
            # trace instead of both.
            $restore = Invoke-EnvctlTolerant -EnvctlPath $Orchestrator.EnvctlPath `
                -Arguments @('restore', '--journal', $journalPath)
            $outcome.RestoreResult = ConvertTo-RestoreResult -State $(if ($null -ne $restore) { $restore.state } else { $null })
            if ($null -ne $restore -and $restore.PSObject.Properties.Name -contains 'evidence') {
                $outcome.Evidence = $restore.evidence
            }
            if ($outcome.RestoreResult -ne 'RESTORED') {
                $Orchestrator.Dirty = $true
                $Orchestrator.DirtyDetail = "scenario '$Scenario' ended in $($outcome.RestoreResult)"
            }
        }
        catch {
            $outcome.RestoreResult = 'RESTORE_FAILED'
            $Orchestrator.Dirty = $true
            $Orchestrator.DirtyDetail = $_.Exception.Message
        }
        Remove-Item -LiteralPath $desiredFile -Force -ErrorAction SilentlyContinue
    }

    return $outcome
}

Export-ModuleMember -Function Write-EnvctlJsonAtomic, Resolve-EnvctlPath, Invoke-Envctl,
Invoke-EnvctlTolerant, ConvertTo-RestoreResult, New-EnvironmentOrchestrator,
Assert-EnvironmentClean, Get-EnvironmentSnapshot, Get-EnvironmentCapabilities,
Resolve-EnvironmentAliases, Test-EnvironmentRequirement, Invoke-EnvironmentTransaction
