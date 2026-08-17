#Requires -Version 7.0
<#
.SYNOPSIS
    The v0.9 release verification campaign: one runner for every release gate.

.DESCRIPTION
    Wave D's D3. Where `live-verify.ps1` accepts a build, this accepts a RELEASE: it
    walks the whole v0.9 gate matrix, prepares the Windows environment each scenario
    needs, drives ExoSnap through its own semantic automation, validates the output
    with an independent tool, and stops for a human only where a human is genuinely
    irreducible.

    Four boundaries, and the campaign is honest about which side of each it is on:

        product truth      ExoSnap's control channel. Automated, always.
        environment truth  exosnap-envctl transactions. Automated where Windows
                           exposes a documented, restorable mechanism.
        physical truth     the operator unplugs the cable; the runner verifies the
                           consequence itself before calling it a pass.
        secure truth       UAC. The operator clicks it; the runner observes before
                           and after. Never scripted, never simulated.

    None of those boundaries is crossed with a pixel click, a SendKeys macro or a
    registry write to make a number go up. A gate that would need one is reported as
    manual, which is a true statement, rather than automated, which would not be.

    Three rules the report depends on:

    1. NOTHING IS A PASS WITHOUT EVIDENCE, and a human gate is not a pass until the
       runner has independently observed the consequence the operator was asked to
       cause. "The operator said done" is not an observation.
    2. A SCENARIO HAS TWO VERDICTS. The product verdict and the environment-restore
       verdict are recorded separately, because a scenario can prove the product
       correct and still leave the machine misconfigured, and a release report that
       merges them hides the second.
    3. AN UNMET REQUIREMENT IS NOT A FAILURE. No HDR display, no 240 Hz mode, no
       second monitor: UNAVAILABLE. No interactive terminal for a gate: DEFERRED.
       Neither is the product's fault and neither may be recorded as if it were.

.PARAMETER Command
    prepare   start a campaign against an explicitly named artifact
    run       run every runnable scenario
    resume    re-fingerprint, mark stale, and continue
    status    print the current state
    report    write release-verification.json + .md + junit.xml
    recover   restore a dirty environment left by a killed runner, and nothing else
    list      print the scenario catalog with its layers and requirements

.PARAMETER ExePath
    The exosnap.exe under test. MANDATORY for `prepare`: a release gate binds its
    verdict to a specific set of bytes, so there is deliberately no default
    resolution here. `live-verify.ps1` may guess at a local build; this may not.

.EXAMPLE
    pwsh scripts/release-verify.ps1 prepare -ExePath C:\rc\exosnap.exe -Tag v0.9.0-rc10
    pwsh scripts/release-verify.ps1 run
    pwsh scripts/release-verify.ps1 report
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet('prepare', 'run', 'resume', 'status', 'report', 'recover', 'list')]
    [string] $Command = 'status',

    [string] $ExePath,
    [string] $Tag,
    [string] $RunId,
    [string[]] $Only,
    [string] $AliasProfile,
    [string] $EnvctlPath,
    # Skips every gate that needs a human instead of offering it. The gates become
    # DEFERRED, never PASS and never FAIL -- nobody was asked.
    [switch] $NonInteractive,
    # Opts a scenario class in explicitly. Long-running and physically disruptive
    # classes (the 30-60 minute mixed-clock run, the unplug scenarios) are not part
    # of a default sweep.
    [string[]] $IncludeClass
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$runsRoot = Join-Path $repositoryRoot '.workspace/release-verify'

Import-Module (Join-Path $PSScriptRoot 'lib/LiveVerifyState.psm1') -Force -DisableNameChecking
Import-Module (Join-Path $PSScriptRoot 'lib/LiveVerifyClient.psm1') -Force -DisableNameChecking
Import-Module (Join-Path $PSScriptRoot 'lib/EnvironmentOrchestrator.psm1') -Force -DisableNameChecking
. (Join-Path $PSScriptRoot 'lib/LiveVerifyChecks.ps1')
. (Join-Path $PSScriptRoot 'lib/ReleaseScenarios.ps1')

# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------

function Write-Heading { param([string] $Text) Write-Host ''; Write-Host "== $Text" -ForegroundColor Cyan }
function Write-Step { param([string] $Text) Write-Host "   $Text" -ForegroundColor DarkGray }

# ---------------------------------------------------------------------------
# Artifact + environment identity
# ---------------------------------------------------------------------------

function Get-ReleaseArtifactFingerprint {
    <#
    .SYNOPSIS
        Binds the campaign to one set of bytes.
    .DESCRIPTION
        No fallback resolution on purpose. A release PASS says "these bytes behaved
        correctly"; a runner that helpfully found some other exosnap.exe would make
        that sentence false without saying so.
    #>
    param([Parameter(Mandatory)] [string] $Path, [string] $ReleaseTag)

    if (-not (Test-Path -LiteralPath $Path)) {
        throw "No artifact at '$Path'. Release gates bind to an explicit binary; there is no default."
    }
    $item = Get-Item -LiteralPath $Path
    $facts = @{
        kind           = 'release'
        tag            = $ReleaseTag
        exePath        = $item.FullName
        exeSha256      = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        exeBytes       = $item.Length
        productVersion = $item.VersionInfo.ProductVersion
        fileVersion    = $item.VersionInfo.FileVersion
        builtUtc       = $item.LastWriteTimeUtc.ToString('o')
        # Whether this artifact sits in an installed tree decides which scenarios can
        # run at all: the updater and handoff paths resolve applicationDirPath()-
        # relative files that only exist after `cmake --install`.
        installTree    = (Test-Path -LiteralPath (Join-Path $item.DirectoryName 'exosnap-updater.exe'))
    }
    $facts['fingerprint'] = Get-LiveVerifyFingerprint -Properties $facts
    return $facts
}

function Get-ReleaseEnvironmentFacts {
    <#
    .SYNOPSIS
        The environment properties a scenario's verdict is bound to.
    .DESCRIPTION
        Where the Live Verify runner asks WMI for a coarse machine description, this
        prefers exosnap-envctl, because a release verdict has to be bound to the
        state a scenario actually depended on -- the HDR state of one display keyed
        by its stable id, not "there are two monitors". The WMI-shaped facts stay as
        a floor so a machine without the tool still fingerprints something real.

        Nothing here identifies the person at the machine: no hostname, no user name,
        no paths outside the repository. A run directory is evidence other people
        read.
    #>
    param($Orchestrator)

    $facts = [ordered]@{}
    $facts['osVersion'] = [System.Environment]::OSVersion.Version.ToString()
    $facts['architecture'] = "$([System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture)"
    $facts['processorCount'] = [System.Environment]::ProcessorCount

    try {
        $gpus = @(Get-CimInstance Win32_VideoController -ErrorAction Stop |
                ForEach-Object { "$($_.Name)@$($_.DriverVersion)" })
        $facts['gpus'] = ($gpus -join '|')
    }
    catch { $facts['gpus'] = 'unavailable' }

    Add-Type -AssemblyName System.Windows.Forms -ErrorAction SilentlyContinue
    try {
        $screens = @([System.Windows.Forms.Screen]::AllScreens |
                ForEach-Object { "$($_.DeviceName)=$($_.Bounds.Width)x$($_.Bounds.Height)" } | Sort-Object)
        $facts['monitorTopology'] = ($screens -join '|')
        $facts['monitorCount'] = ([System.Windows.Forms.Screen]::AllScreens).Count
    }
    catch {
        $facts['monitorTopology'] = 'unavailable'
        $facts['monitorCount'] = 'unavailable'
    }

    $facts['elevated'] = ([Security.Principal.WindowsPrincipal]::new(
            [Security.Principal.WindowsIdentity]::GetCurrent())).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)

    $probe = Get-Command ffprobe -ErrorAction SilentlyContinue
    $facts['ffprobeVersion'] = if ($null -ne $probe) {
        (& $probe.Source -version 2>&1 | Select-Object -First 1) -replace '\s+', ' '
    }
    else { 'absent' }

    # The precise half. Each controllable property becomes its own environment key so
    # a scenario can declare exactly what it depended on -- and changing the refresh
    # rate of one display does not invalidate the audio scenarios.
    $facts['envctl'] = if ($null -ne $Orchestrator -and $Orchestrator.Available) { 'available' } else { 'absent' }
    if ($null -ne $Orchestrator -and $Orchestrator.Available) {
        try {
            $snapshot = Get-EnvironmentSnapshot -Orchestrator $Orchestrator
            if ($null -ne $snapshot -and $snapshot.PSObject.Properties.Name -contains 'properties') {
                foreach ($property in $snapshot.properties) {
                    $facts["env.$($property.id)"] = "$($property.value)"
                }
            }
        }
        catch { $facts['envctlError'] = $_.Exception.Message }
    }

    $stringified = @{}
    foreach ($key in $facts.Keys) { $stringified[$key] = "$($facts[$key])" }
    return $stringified
}

# ---------------------------------------------------------------------------
# Human gates
# ---------------------------------------------------------------------------

function Invoke-ReleaseHumanGate {
    <#
    .SYNOPSIS
        Asks the operator for one action, then verifies the consequence itself.
    .DESCRIPTION
        The contract, in order, and no step may be skipped:

            state the check id and WHY this cannot be automated
            state the exact action
            state the expected observable consequence
            state HOW THE RUNNER WILL VERIFY IT
            wait
            verify independently
            only then decide

        A gate whose Verify block returns false is a FAIL even when the operator said
        "done" -- an operator can be mistaken about what they just did, and a gate
        that trusts the answer instead of the machine is a checkbox with extra steps.

        A gate nobody could answer (redirected stdin, -NonInteractive, an operator who
        declined) is DEFERRED. It is never a FAIL: a question nobody was asked has no
        wrong answer. This is checked BEFORE the instructions are printed, so nobody
        performs a two-minute physical action that cannot be confirmed afterwards.
    #>
    param(
        [Parameter(Mandatory)] $Gate,
        [Parameter(Mandatory)] $Context
    )

    if ($NonInteractive) {
        return @{ Result = 'DEFERRED'; Message = "Human gate not offered (-NonInteractive): $($Gate.Title)" }
    }
    if ([Console]::IsInputRedirected) {
        return @{ Result = 'DEFERRED'
            Message      = 'stdin is redirected, so this gate cannot be answered. Run from a real terminal.'
        }
    }

    Write-Host ''
    Write-Host "MANUAL ACTION REQUIRED - $($Gate.Id)  $($Gate.Title)" -ForegroundColor Yellow
    Write-Host ''
    Write-Host 'Why this is manual:' -ForegroundColor Yellow
    Write-Host "  $($Gate.Why)"
    Write-Host ''
    Write-Host 'Exact action:' -ForegroundColor Yellow
    $index = 1
    foreach ($step in @($Gate.Do)) { Write-Host "  $index. $step"; $index++ }
    Write-Host ''
    Write-Host 'Expected observable consequence:' -ForegroundColor Yellow
    Write-Host "  $($Gate.Expected)"
    Write-Host ''
    Write-Host 'How this runner will verify it:' -ForegroundColor Yellow
    Write-Host "  $($Gate.VerifyDescription)"
    Write-Host ''
    $answer = Read-Host "Type 'done' when performed, 'skip' to defer, anything else to abort"
    if ($null -eq $answer) { $answer = '' }
    switch ($answer.Trim().ToLowerInvariant()) {
        'done' { }
        'skip' { return @{ Result = 'DEFERRED'; Message = 'The operator deferred this gate' } }
        default { return @{ Result = 'DEFERRED'; Message = 'The operator aborted this gate' } }
    }

    if ($null -eq $Gate.Verify) {
        # Refused rather than trusted. A gate with no verification is a gate that
        # cannot distinguish "done" from "typed done", and this runner does not
        # record that as a pass.
        return @{ Result = 'UNVERIFIED'
            Message      = "Gate '$($Gate.Id)' declares no Verify block; the operator's answer alone is not evidence"
        }
    }

    Write-Step 'verifying the consequence independently...'
    $verdict = & $Gate.Verify $Context
    if ($null -eq $verdict) {
        return @{ Result = 'UNVERIFIED'; Message = 'The gate verification returned nothing' }
    }
    if ($verdict.Ok) {
        return @{ Result = 'PASS'; Message = $verdict.Detail; Evidence = $verdict.Evidence }
    }
    return @{ Result = 'FAIL'
        Message      = "The operator reported the action as performed, but the runner could not observe it: $($verdict.Detail)"
        Evidence     = $verdict.Evidence
    }
}

# ---------------------------------------------------------------------------
# Sessions
# ---------------------------------------------------------------------------

$script:Session = $null

function Start-ReleaseSession {
    param([Parameter(Mandatory)] $Run)

    if ($null -ne $script:Session) {
        $script:Session.Process.Refresh()
        if (-not $script:Session.Process.HasExited) { return $script:Session }
        try { $script:Session.Connection.Close() } catch { }
        $script:Session = $null
    }

    $exe = $Run.Artifact.exePath
    $sessionRunId = New-LiveVerifyRunId
    Write-Step "launching $([IO.Path]::GetFileName($exe)) with the control channel armed"

    # Recordings land in the run directory, never in the operator's video library.
    # EXOSNAP_OUTPUT_DIR is a runtime override of the configured folder, not a
    # harness switch -- this stays a normal launch, which is what is being accepted.
    $recordings = Join-Path $Run.Directory 'media/recordings'
    New-Item -ItemType Directory -Path $recordings -Force | Out-Null
    $previous = $env:EXOSNAP_OUTPUT_DIR
    $env:EXOSNAP_OUTPUT_DIR = $recordings
    try { $process = Start-Process -FilePath $exe -ArgumentList @('--live-verify-control', $sessionRunId) -PassThru }
    finally { $env:EXOSNAP_OUTPUT_DIR = $previous }

    try { $connection = Connect-LiveVerify -RunId $sessionRunId -ConnectTimeoutMs 30000 }
    catch {
        if (-not $process.HasExited) { $process | Stop-Process -Force -ErrorAction SilentlyContinue }
        throw
    }

    $script:Session = [pscustomobject]@{
        Process       = $process
        Connection    = $connection
        RunId         = $sessionRunId
        RecordingsDir = $recordings
    }
    return $script:Session
}

function Stop-ReleaseSession {
    if ($null -eq $script:Session) { return }
    $session = $script:Session
    $script:Session = $null
    # The channel closes first and the event queue is deliberately NOT drained: a
    # runner waiting for the process to exit is, by definition, not reading events.
    # That is the shape of the regression this invariant guards (ADR 0067), and
    # REL-SHUTDOWN-001 asserts it explicitly rather than relying on this teardown.
    try { $session.Connection.Close() } catch { }
    $session.Process.Refresh()
    if (-not $session.Process.HasExited) {
        # CloseMainWindow rather than a kill: the close-to-tray refusal and the close
        # guards are product behaviour, and killing would step straight over them.
        [void]$session.Process.CloseMainWindow()
        if (-not $session.Process.WaitForExit(15000)) {
            Write-Host '   WARNING: the application did not exit within 15 s of being asked to close.' -ForegroundColor Red
            $session.Process | Stop-Process -Force -ErrorAction SilentlyContinue
        }
    }
    $session.Process.WaitForExit(5000) | Out-Null
}

# ---------------------------------------------------------------------------
# Run selection
# ---------------------------------------------------------------------------

function Get-LatestRunDirectory {
    if (-not (Test-Path -LiteralPath $runsRoot)) { return $null }
    $directories = @(Get-ChildItem -LiteralPath $runsRoot -Directory |
            Where-Object { Test-Path (Join-Path $_.FullName 'state.json') } |
            Sort-Object LastWriteTimeUtc -Descending)
    if ($directories.Count -eq 0) { return $null }
    return $directories[0].FullName
}

function Resolve-RunDirectory {
    if (-not [string]::IsNullOrWhiteSpace($RunId)) {
        $path = Join-Path $runsRoot $RunId
        if (-not (Test-Path -LiteralPath $path)) { throw "No release campaign '$RunId' under $runsRoot" }
        return $path
    }
    $latest = Get-LatestRunDirectory
    if ($null -eq $latest) { throw "No release campaign yet. Start one with: prepare -ExePath <exosnap.exe>" }
    return $latest
}

function ConvertTo-Hashtable {
    param($Object)
    $table = @{}
    if ($null -eq $Object) { return $table }
    foreach ($property in $Object.PSObject.Properties) { $table[$property.Name] = $property.Value }
    return $table
}

# ---------------------------------------------------------------------------
# Execution
# ---------------------------------------------------------------------------

function New-ReleaseContext {
    param([Parameter(Mandatory)] $Run, [Parameter(Mandatory)] $Orchestrator)
    return [pscustomobject]@{
        RunDirectory  = $Run.Directory
        Artifact      = $Run.Artifact
        Environment   = $Run.Environment
        Orchestrator  = $Orchestrator
        RepositoryRoot = $repositoryRoot
        State         = @{}
        EnsureSession = { Start-ReleaseSession -Run $Run }.GetNewClosure()
        EndSession    = { Stop-ReleaseSession }
        HumanGate     = $null   # assigned below; needs the context itself
    }
}

function Invoke-Scenarios {
    param(
        [Parameter(Mandatory)] $Run,
        [Parameter(Mandatory)] $Orchestrator,
        [Parameter(Mandatory)] [object[]] $Catalog,
        [Parameter(Mandatory)] [object[]] $Entries
    )

    $context = New-ReleaseContext -Run $Run -Orchestrator $Orchestrator
    $context.HumanGate = { param($gate) Invoke-ReleaseHumanGate -Gate $gate -Context $context }.GetNewClosure()

    $environmentTable = ConvertTo-Hashtable $Run.Environment
    $artifactFingerprint = $Run.Artifact.fingerprint
    $aliases = $null
    if ($Orchestrator.Available) {
        try { $aliases = Resolve-EnvironmentAliases -Orchestrator $Orchestrator } catch { $aliases = $null }
    }

    try {
        foreach ($entry in $Entries) {
            Write-Heading "$($entry.Id)  $($entry.Title)"
            Write-Step "layer $($entry.Layer)  class $($entry.Class)"

            $environmentFingerprint = Get-LiveVerifyEnvironmentFingerprint -Entry $entry -Environment $environmentTable
            Set-LiveVerifyCheckRunning -Run $Run -Id $entry.Id `
                -ArtifactFingerprint $artifactFingerprint `
                -EnvironmentFingerprint $environmentFingerprint | Out-Null

            $outcome = Invoke-OneScenario -Entry $entry -Context $context -Orchestrator $Orchestrator -Aliases $aliases

            $evidence = @()
            if ($outcome.ContainsKey('Evidence') -and $null -ne $outcome.Evidence) { $evidence = @($outcome.Evidence) }
            Complete-LiveVerifyCheck -Run $Run -Id $entry.Id -Result $outcome.Result `
                -Message $outcome.Message -Evidence $evidence `
                -RestoreResult $outcome.RestoreResult -EnvironmentEvidence $outcome.EnvironmentEvidence | Out-Null

            $colour = switch ($outcome.Result) {
                'PASS' { 'Green' }
                'FAIL' { 'Red' }
                default { 'Yellow' }
            }
            Write-Host "  -> $($outcome.Result)  $($outcome.Message)" -ForegroundColor $colour
            if ($outcome.RestoreResult -ne 'NOT_APPLICABLE' -and $outcome.RestoreResult -ne 'RESTORED') {
                Write-Host "  -> ENVIRONMENT RESTORE: $($outcome.RestoreResult)" -ForegroundColor Red
            }
        }
    }
    finally {
        Stop-ReleaseSession
    }
}

function Invoke-OneScenario {
    <#
    .SYNOPSIS
        Requirement gate, then the environment transaction, then the scenario body.
    .DESCRIPTION
        The order is the design. A requirement that this machine cannot meet is
        answered before anything is mutated or launched, so an UNAVAILABLE scenario
        costs nothing and disturbs nothing. And the transaction wraps the body rather
        than the body managing its own environment, so the restore is structurally
        unavoidable -- including when the body throws, times out, or the operator
        walks away from a gate inside it.
    #>
    param(
        [Parameter(Mandatory)] $Entry,
        [Parameter(Mandatory)] $Context,
        [Parameter(Mandatory)] $Orchestrator,
        $Aliases
    )

    $result = @{
        Result              = 'UNVERIFIED'
        Message             = $null
        Evidence            = @()
        RestoreResult       = 'NOT_APPLICABLE'
        EnvironmentEvidence = $null
    }

    $requires = if ($Entry.PSObject.Properties.Name -contains 'Requires' -and $null -ne $Entry.Requires) {
        $Entry.Requires
    }
    else { @{} }

    if ($requires.Count -gt 0) {
        $verdict = Test-EnvironmentRequirement -Orchestrator $Orchestrator -Requirement $requires -Aliases $Aliases
        if (-not $verdict.Satisfied) {
            $result.Result = 'UNAVAILABLE'
            $result.Message = $verdict.Reason
            return $result
        }
    }

    if ($Entry.PSObject.Properties.Name -contains 'RequiresInstallTree' -and $Entry.RequiresInstallTree) {
        if (-not $Context.Artifact.installTree) {
            $result.Result = 'UNAVAILABLE'
            $result.Message = 'This scenario needs an installed tree (cmake --install): the updater and its ' +
            'applicationDirPath()-relative files do not exist beside a build-tree binary.'
            return $result
        }
    }

    $desired = if ($Entry.PSObject.Properties.Name -contains 'Desired' -and $null -ne $Entry.Desired) {
        $Entry.Desired
    }
    else { @{} }

    if ($desired.Count -gt 0 -and -not $Orchestrator.Available) {
        $result.Result = 'UNAVAILABLE'
        $result.Message = 'exosnap-envctl is not built; this scenario needs a real environment mutation and ' +
        'will not fake one'
        return $result
    }

    try {
        $transaction = Invoke-EnvironmentTransaction -Orchestrator $Orchestrator -Scenario $Entry.Id `
            -Desired $desired -Body { param($begun) & $Entry.Run $Context $begun }.GetNewClosure()
    }
    catch {
        $result.Result = 'FAIL'
        $result.Message = "Scenario setup failed: $($_.Exception.Message)"
        return $result
    }

    $result.RestoreResult = $transaction.RestoreResult
    $result.EnvironmentEvidence = $transaction.Evidence

    if ($null -ne $transaction.Error) {
        $result.Result = 'FAIL'
        $result.Message = "Scenario threw: $($transaction.Error)"
        return $result
    }
    $product = $transaction.Product
    if ($null -eq $product) {
        $result.Result = 'UNVERIFIED'
        $result.Message = 'The scenario returned nothing'
        return $result
    }
    $result.Result = $product.Result
    if ($product.ContainsKey('Message')) { $result.Message = $product.Message }
    if ($product.ContainsKey('Evidence') -and $null -ne $product.Evidence) { $result.Evidence = @($product.Evidence) }
    return $result
}

# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------

function Select-Entries {
    param([Parameter(Mandatory)] [object[]] $Catalog, [object[]] $Runnable)
    $entries = if ($null -eq $Runnable) { $Catalog } else { $Runnable }
    if ($null -ne $Only -and $Only.Count -gt 0) {
        $entries = @($entries | Where-Object { $Only -contains $_.Id })
    }
    # Opt-in classes stay out of a default sweep. A 45-minute mixed-clock recording
    # and a scenario that asks the operator to unplug an audio interface are not
    # things a runner should start because somebody typed `run`.
    $included = @($IncludeClass)
    $entries = @($entries | Where-Object {
            $optIn = $_.PSObject.Properties.Name -contains 'OptIn' -and $_.OptIn
            (-not $optIn) -or ($included -contains $_.Class) -or ($null -ne $Only -and $Only -contains $_.Id)
        })
    return [object[]]$entries
}

$catalog = Get-ReleaseScenarioCatalog

switch ($Command) {
    'list' {
        Write-Heading 'Release scenario catalog'
        foreach ($entry in $catalog) {
            $requires = if ($entry.PSObject.Properties.Name -contains 'Requires' -and $entry.Requires.Count -gt 0) {
                ' requires:' + (($entry.Requires.Values) -join ',')
            }
            else { '' }
            $optIn = if ($entry.PSObject.Properties.Name -contains 'OptIn' -and $entry.OptIn) { ' [opt-in]' } else { '' }
            Write-Host ("  {0,-26} {1,-16} {2}{3}{4}" -f $entry.Id, $entry.Layer, $entry.Title, $requires, $optIn)
        }
        Write-Host ''
        Write-Host "  $($catalog.Count) scenarios."
        return
    }

    'prepare' {
        if ([string]::IsNullOrWhiteSpace($ExePath)) {
            throw 'prepare needs -ExePath. A release gate binds to explicit bytes; there is no default artifact.'
        }
        $artifact = Get-ReleaseArtifactFingerprint -Path $ExePath -ReleaseTag $Tag
        $campaignId = if ([string]::IsNullOrWhiteSpace($RunId)) {
            "rel-$([DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss'))"
        }
        else { $RunId }
        $directory = Join-Path $runsRoot $campaignId
        $journalDirectory = Join-Path $directory 'environment'

        $orchestrator = New-EnvironmentOrchestrator -RunId $campaignId -JournalDirectory $journalDirectory `
            -EnvctlPath $EnvctlPath -AliasProfile $AliasProfile
        $environment = Get-ReleaseEnvironmentFacts -Orchestrator $orchestrator

        New-LiveVerifyRun -RunId $campaignId -RunDirectory $directory -Catalog $catalog `
            -Artifact $artifact -Environment $environment | Out-Null

        Write-Heading "Release campaign $campaignId"
        Write-Host "  artifact : $($artifact.exePath)"
        Write-Host "  version  : $($artifact.productVersion)  tag $($artifact.tag)"
        Write-Host "  sha256   : $($artifact.exeSha256)"
        Write-Host "  install  : $($artifact.installTree)"
        Write-Host "  envctl   : $(if ($orchestrator.Available) { $orchestrator.EnvctlPath } else { 'NOT BUILT - mutating scenarios will report UNAVAILABLE' })"
        if ($orchestrator.Dirty) {
            Write-Host "  WARNING  : environment is dirty - $($orchestrator.DirtyDetail)" -ForegroundColor Red
        }
        Write-Host ''
        Write-Host "  $($catalog.Count) scenarios pending. Run them with: release-verify.ps1 run"
        return
    }

    'recover' {
        $directory = Resolve-RunDirectory
        $run = Get-LiveVerifyRun -RunDirectory $directory
        $orchestrator = New-EnvironmentOrchestrator -RunId $run.Run.runId `
            -JournalDirectory (Join-Path $directory 'environment') -EnvctlPath $EnvctlPath -AliasProfile $AliasProfile
        Write-Heading 'Environment recovery'
        if (-not $orchestrator.Available) { Write-Host '  exosnap-envctl is not built; nothing to recover with.'; return }
        foreach ($item in @($orchestrator.Recovered)) { Write-Host "  restored: $item" }
        if ($orchestrator.Dirty) {
            Write-Host "  STILL DIRTY: $($orchestrator.DirtyDetail)" -ForegroundColor Red
            exit 1
        }
        Write-Host '  clean.' -ForegroundColor Green
        return
    }

    'run' {
        $directory = Resolve-RunDirectory
        $run = Get-LiveVerifyRun -RunDirectory $directory
        $orchestrator = New-EnvironmentOrchestrator -RunId $run.Run.runId `
            -JournalDirectory (Join-Path $directory 'environment') -EnvctlPath $EnvctlPath -AliasProfile $AliasProfile
        Resolve-LiveVerifyInterrupted -Run $run | Out-Null
        $runnable = @(Get-LiveVerifyRunnableChecks -Run $run -Catalog $catalog)
        $entries = Select-Entries -Catalog $catalog -Runnable $runnable
        if ($entries.Count -eq 0) { Write-Host 'Nothing runnable.'; return }
        Invoke-Scenarios -Run $run -Orchestrator $orchestrator -Catalog $catalog -Entries $entries
        Write-LiveVerifyReport -Run $run
        return
    }

    'resume' {
        $directory = Resolve-RunDirectory
        $run = Get-LiveVerifyRun -RunDirectory $directory
        $orchestrator = New-EnvironmentOrchestrator -RunId $run.Run.runId `
            -JournalDirectory (Join-Path $directory 'environment') -EnvctlPath $EnvctlPath -AliasProfile $AliasProfile
        $artifact = Get-ReleaseArtifactFingerprint -Path $run.Artifact.exePath -ReleaseTag $run.Artifact.tag
        $environment = Get-ReleaseEnvironmentFacts -Orchestrator $orchestrator
        Write-JsonAtomic -Path (Join-Path $directory 'artifact-fingerprint.json') -Value $artifact
        Write-JsonAtomic -Path (Join-Path $directory 'environment.json') -Value $environment
        $run = Get-LiveVerifyRun -RunDirectory $directory
        Resolve-LiveVerifyInterrupted -Run $run | Out-Null
        $stale = Update-LiveVerifyStaleness -Run $run -Catalog $catalog `
            -ArtifactFingerprint $artifact.fingerprint -Environment $environment
        Write-Heading "Resumed $($run.Run.runId)"
        if ($stale.Count -gt 0) { Write-Host "  $($stale.Count) result(s) became STALE: $($stale -join ', ')" }
        $runnable = @(Get-LiveVerifyRunnableChecks -Run $run -Catalog $catalog)
        $entries = Select-Entries -Catalog $catalog -Runnable $runnable
        if ($entries.Count -eq 0) { Write-Host 'Nothing runnable.'; return }
        Invoke-Scenarios -Run $run -Orchestrator $orchestrator -Catalog $catalog -Entries $entries
        Write-LiveVerifyReport -Run $run
        return
    }

    'status' {
        $directory = Resolve-RunDirectory
        $run = Get-LiveVerifyRun -RunDirectory $directory
        $summary = Get-LiveVerifySummary -Run $run
        Write-Heading "Release campaign $($run.Run.runId)"
        Write-Host "  artifact : $($run.Artifact.productVersion)  $($run.Artifact.exePath)"
        Write-Host "  sha256   : $($run.Artifact.exeSha256)"
        Write-Host ''
        foreach ($property in $run.State.checks.PSObject.Properties) {
            $check = $property.Value
            $colour = switch ($check.state) {
                'PASS' { 'Green' }
                'FAIL' { 'Red' }
                'PENDING' { 'DarkGray' }
                default { 'Yellow' }
            }
            $restore = if ($check.PSObject.Properties.Name -contains 'restoreResult' -and
                $check.restoreResult -notin @($null, 'NOT_APPLICABLE')) { "  [restore $($check.restoreResult)]" }
            else { '' }
            Write-Host ("  {0,-26} {1,-14}{2} {3}" -f $check.id, $check.state, $restore, $check.message) -ForegroundColor $colour
        }
        Write-Host ''
        foreach ($state in ($summary.Keys | Where-Object { $summary[$_] -gt 0 })) {
            Write-Host "  $state = $($summary[$state])"
        }
        return
    }

    'report' {
        $directory = Resolve-RunDirectory
        $run = Get-LiveVerifyRun -RunDirectory $directory
        Write-LiveVerifyReport -Run $run
        # The release-facing name. Same content as report.json -- one writer, so the
        # machine-readable release verdict and the Live Verify report cannot disagree.
        Copy-Item -LiteralPath (Join-Path $directory 'report.json') `
            -Destination (Join-Path $directory 'release-verification.json') -Force
        Write-Heading 'Report written'
        Write-Host "  $(Join-Path $directory 'release-verification.json')"
        Write-Host "  $(Join-Path $directory 'report.md')"
        Write-Host "  $(Join-Path $directory 'junit.xml')"
        return
    }
}
