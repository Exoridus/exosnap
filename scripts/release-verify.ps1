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
    retry     re-attempt named scenarios (a FAIL is otherwise left alone as a finding)
    status    print the current state
    report    write release-verification.json + .md + junit.xml
    qualify   evaluate the campaign for promotion, and (with -Publish) attach the
              qualification record to the RC release
    recover   restore a dirty environment left by a killed runner, and nothing else
    list      print the scenario catalog with its layers and requirements

.PARAMETER ExePath
    The exosnap.exe under test. MANDATORY for `prepare`: a release gate binds its
    verdict to a specific set of bytes, so there is deliberately no default
    resolution here. `live-verify.ps1` may guess at a local build; this may not.

.EXAMPLE
    pwsh scripts/release-verify.ps1 prepare -ExePath ./rc/portable/exosnap.exe -Tag v0.9.1-rc1 `
        -SourceCommit <the commit v0.9.1-rc1 points at> `
        -PortableZip ./rc/ExoSnap-0.9.1-rc1-windows-x64-portable.zip `
        -Msi ./rc/ExoSnap-0.9.1-rc1-windows-x64.msi
    pwsh scripts/release-verify.ps1 run
    pwsh scripts/release-verify.ps1 report
    pwsh scripts/release-verify.ps1 qualify
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet('prepare', 'run', 'resume', 'retry', 'status', 'report', 'qualify', 'recover', 'list')]
    [string] $Command = 'status',

    [string] $ExePath,
    [string] $Tag,
    # The commit the RC was built from. A published portable ZIP carries no build
    # manifest, so a campaign against downloaded release assets cannot read its own
    # provenance -- and a qualification record without a source commit promotes
    # nothing.
    [string] $SourceCommit,
    # The RC's published downloadables. Hashing them is what binds the campaign's
    # verdict to the bytes the release page actually serves; the publish gate compares
    # these against the release's own .sha256 sidecars.
    [string] $PortableZip,
    [string] $Msi,
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
    [string[]] $IncludeClass,
    # Scenario ids whose operator action the CALLER has already performed. The gate
    # prints its instructions as usual and then goes straight to verification
    # instead of asking. This is not a way to pass a gate: the Verify block still
    # decides, and a gate that declares none is still UNVERIFIED. What it changes
    # is who is allowed to have acted -- an automation that really did unplug,
    # minimise or reconfigure something, in a session with no terminal to answer
    # from. Every attested result says so in the report, so a reader can tell an
    # attested run from one a person stood in front of.
    [string[]] $Attest,
    # Opt-in scenario ids this release must also have answered. Opt-in classes stay
    # out of a default sweep, so treating them as silently required would block every
    # promotion and treating them as permanently optional would mean a soak or an
    # unplug gate never has to be answered. Naming them per release is the honest form.
    [string[]] $Required,
    # `qualify` only: uploads the qualification record to the RC's GitHub release.
    # Promotion is the developer's explicit act and nobody else's.
    [switch] $Publish,
    # `qualify` only: the base64-encoded 32-byte ed25519 seed the release is signed
    # with, the same key the update manifest is signed with. The record is signed
    # because every other field in it is publicly readable from the RC release and can
    # therefore be retyped by hand; the publish gate refuses an unsigned one. Defaults
    # to EXOSNAP_UPDATE_SIGNING_KEY so the key never has to appear in a command line.
    [string] $SigningKeyBase64 = $env:EXOSNAP_UPDATE_SIGNING_KEY,

    # Which harness carries out the command. PowerShell is the default until every
    # gate has been migrated: a default that silently ran a harness with fewer gates
    # than the checklist names would produce a record about a smaller bar than the one
    # a reader assumes. `DotNet` builds and publishes ExoSnap.Verify if it has to and
    # forwards prepare / run / list / qualify / report to it.
    [ValidateSet('PowerShell', 'DotNet')]
    [string] $Engine = 'PowerShell'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$runsRoot = Join-Path $repositoryRoot '.workspace/release-verify'

Import-Module (Join-Path $PSScriptRoot 'lib/LiveVerifyState.psm1') -Force -DisableNameChecking
Import-Module (Join-Path $PSScriptRoot 'lib/LiveVerifyClient.psm1') -Force -DisableNameChecking
Import-Module (Join-Path $PSScriptRoot 'lib/EnvironmentOrchestrator.psm1') -Force -DisableNameChecking
. (Join-Path $PSScriptRoot 'lib/LiveVerifyChecks.ps1')
. (Join-Path $PSScriptRoot 'lib/ReleaseArtifactIdentity.ps1')
. (Join-Path $PSScriptRoot 'lib/ReleaseOperator.ps1')
. (Join-Path $PSScriptRoot 'lib/ReleaseExternalTools.ps1')
. (Join-Path $PSScriptRoot 'lib/ReleaseSandbox.ps1')
. (Join-Path $PSScriptRoot 'lib/ReleaseScenarios.ps1')
. (Join-Path $PSScriptRoot 'lib/ReleaseQualification.ps1')

# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------

function Write-Heading { param([string] $Text) Write-Host ''; Write-Host "== $Text" -ForegroundColor Cyan }
function Write-Step { param([string] $Text) Write-Host "   $Text" -ForegroundColor DarkGray }

# ---------------------------------------------------------------------------
# Artifact + environment identity
# ---------------------------------------------------------------------------

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
                    # `key` is "<alias>:<property>" -- the stable-id-backed identity a
                    # scenario declares. The friendly name is deliberately not part of
                    # the fingerprint: a monitor firmware update that renames the panel
                    # must not invalidate every display verdict.
                    $facts["env.$($property.key)"] = "$($property.value)"
                }
            }
        }
        catch { $facts['envctlError'] = $_.Exception.Message }

        # Which aliases are bound is itself an environment fact. Without it, binding a
        # display for the first time would leave every alias-dependent scenario stuck
        # at the UNAVAILABLE it earned while nothing was bound -- the staleness pass
        # would see an unchanged fingerprint and never offer them again.
        try {
            $aliases = Resolve-EnvironmentAliases -Orchestrator $Orchestrator
            if ($null -ne $aliases -and $aliases.PSObject.Properties.Name -contains 'bindings') {
                $bound = @($aliases.bindings | ForEach-Object { "$($_.alias)=$($_.status)" } | Sort-Object)
                $facts['aliasProfile'] = ($bound -join '|')
            }
            else { $facts['aliasProfile'] = 'none' }
        }
        catch { $facts['aliasProfile'] = 'unresolved' }
    }

    $stringified = @{}
    foreach ($key in $facts.Keys) { $stringified[$key] = "$($facts[$key])" }
    return $stringified
}

# ---------------------------------------------------------------------------
# Human gates
# ---------------------------------------------------------------------------

function Get-ReleaseGateLine {
    <#
    .SYNOPSIS
        The one line an operator is shown for a gate.
    .DESCRIPTION
        `Line` when the gate declares one. A gate that still carries only the old
        numbered `Do` block falls back to its first step, and the whole block stays
        available behind `?` -- so a catalog entry that has not been through the
        rewrite is shown in the new protocol rather than in neither.
    #>
    param([Parameter(Mandatory)] $Gate)
    if ($Gate.ContainsKey('Line') -and -not [string]::IsNullOrWhiteSpace("$($Gate.Line)")) { return "$($Gate.Line)" }
    $steps = @($Gate.Do)
    if ($steps.Count -gt 0) { return "$($steps[0])" }
    return "$($Gate.Title)"
}

function New-ReleaseOperatorStep {
    <#
    .SYNOPSIS
        The gate, as the operator protocol wants it.
    #>
    param([Parameter(Mandatory)] $Gate)
    $detail = @{}
    foreach ($name in 'Why', 'Expected', 'VerifyDescription') {
        if ($Gate.ContainsKey($name)) { $detail[$name] = $Gate[$name] }
    }
    if ($Gate.ContainsKey('Do')) {
        $steps = @($Gate.Do)
        if ($steps.Count -gt 1) {
            $detail['Expected'] = "$($detail['Expected'])  [in full: $(($steps | Select-Object -Skip 1) -join ' ')]"
        }
    }
    $detail['Id'] = $Gate.Id
    $detail['Line'] = Get-ReleaseGateLine -Gate $Gate
    $detail['Kind'] = if ($Gate.ContainsKey('Kind')) { "$($Gate.Kind)" } else { 'Done' }
    return $detail
}

function Invoke-ReleaseHumanGate {
    <#
    .SYNOPSIS
        Shows the operator ONE line, waits for Enter, then verifies the consequence.
    .DESCRIPTION
        The prompt says who acts next and nothing else. `[Enter] startet jetzt` means
        the runner acts on Enter and nothing has happened yet; `[Enter] wenn erledigt`
        means the operator has already acted and the runner is about to look. The
        line those two replaced -- `[y] yes [n] no` -- meant the first for the MSI and
        Chocolatey gates and the second for the present, audio and visual gates, and
        an rc19 campaign lost three gates to that.

        Everything the gate used to print before the prompt (why a person is needed,
        what to expect, how this will be verified) is behind `?`. It is the same
        text; what changed is that it no longer separates the question from its
        meaning by a screenful.

        A gate whose Verify block returns false is a FAIL even when the operator
        pressed Enter -- an operator can be mistaken about what they just did, and a
        gate that trusts the keystroke instead of the machine is a checkbox with
        extra steps.

        A gate nobody could answer (redirected stdin, -NonInteractive, an operator
        who skipped) is DEFERRED. It is never a FAIL: a question nobody was asked has
        no wrong answer. This is checked BEFORE anything is printed, so nobody
        performs a two-minute physical action that cannot be confirmed afterwards.
    #>
    param(
        [Parameter(Mandatory)] $Gate,
        [Parameter(Mandatory)] $Context
    )

    # Attest is checked BEFORE NonInteractive, not after. An attested gate does not
    # need a human at all -- the caller states that the action was performed and the
    # Verify block still decides -- so returning DEFERRED for it made the two
    # switches contradict each other: -NonInteractive -Attest <id> deferred the very
    # gate the attest was there to run. Observed on REL-AUD-SILENCE-001.
    $attested = @(Expand-ListArgument -Values $Attest) -contains $Gate.Id
    if ($NonInteractive -and -not $attested) {
        return @{ Result = 'DEFERRED'; Message = "Human gate not offered (-NonInteractive): $($Gate.Title)" }
    }
    if (-not $attested -and -not (Test-ReleaseOperatorPresent)) {
        return @{ Result = 'DEFERRED'
            Message      = 'stdin is redirected, so this gate cannot be answered. Run from a real terminal.'
        }
    }

    if ($attested) {
        Write-Step "attested by the caller (-Attest): $(Get-ReleaseGateLine -Gate $Gate); verifying it anyway"
    }
    else {
        $step = New-ReleaseOperatorStep -Gate $Gate
        switch (Invoke-ReleaseOperatorStep -Step $step) {
            'go' { }
            'skip' { return @{ Result = 'DEFERRED'; Message = 'The operator skipped this step' } }
            'abort' { return @{ Result = 'DEFERRED'; Message = 'The operator stopped at this step' } }
        }
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
    # The gate is passed back to its own Verify block so a scenario can hand values
    # forward in $Gate.State instead of capturing them in a closure -- see
    # New-ReleaseContext for why closures cannot resolve this script's functions.
    # Normalised, because a Verify block may omit Detail or Evidence -- and under
    # Set-StrictMode reading an absent key throws rather than yielding null. See
    # Resolve-ReleaseVerdict for what that cost.
    $verdict = Resolve-ReleaseVerdict (& $Gate.Verify $Context $Gate)
    if ($null -eq $verdict) {
        return @{ Result = 'UNVERIFIED'; Message = 'The gate verification returned nothing' }
    }
    # Who acted belongs in the record: an attested PASS was verified the same way,
    # but nobody stood at the machine to be asked, and a reader of the report is
    # entitled to know which of the two they are looking at.
    $actor = if ($attested) { '[attested] ' } else { '' }
    # A Verify block may name its own terminal state when neither PASS nor FAIL is
    # the truth. `Ok = $false` alone collapses "the evidence was never produced"
    # into "the product is broken", which is exactly the distinction rules 1 and 3
    # exist to keep: a gate whose worker stopped before it could measure anything
    # has found no defect, and a page of red for it reads like a product collapse.
    # PASS and FAIL keep travelling through Ok, so a block that names nothing is
    # unaffected.
    $named = if ($verdict -is [System.Collections.IDictionary] -and $verdict.ContainsKey('Result')) {
        "$($verdict['Result'])"
    }
    else { '' }
    if ($named -in @('UNVERIFIED', 'UNAVAILABLE', 'DEFERRED')) {
        return @{ Result = $named; Message = "$actor$($verdict.Detail)"; Evidence = $verdict.Evidence }
    }
    if ($verdict.Ok) {
        return @{ Result = 'PASS'; Message = "$actor$($verdict.Detail)"; Evidence = $verdict.Evidence }
    }
    $who = if ($attested) { 'The caller attested the action' } else { 'The operator reported the action as performed' }
    return @{ Result = 'FAIL'
        Message      = "$who, but the runner could not observe it: $($verdict.Detail)"
        Evidence     = $verdict.Evidence
    }
}

# ---------------------------------------------------------------------------
# Sessions
# ---------------------------------------------------------------------------

$script:Session = $null
# Script-scoped so the plain (non-closure) blocks in New-ReleaseContext can reach
# them. See the note there for why closures are not usable here.
$script:CurrentRun = $null
$script:CurrentContext = $null

$script:ElevatedSession = $null

function Start-ReleaseElevatedSession {
    <#
    .SYNOPSIS
        The ONE elevated instance the present-diagnostics gates share.
    .DESCRIPTION
        Present statistics need a real-time ETW session, which Windows grants only
        to an elevated process, and the single-instance guard is a machine-wide
        mutex. Those two facts together are why REL-CAP-FSE-001 failed in the rc19
        campaign: it launched its own unelevated instance while REL-PRESENT-002's
        elevated one was still up, the launch handed focus to that instance and
        exited without opening a pipe, and the gate reported "could not connect to
        the Live Verify endpoint" for a product that was working.

        So there is one instance, established by whichever gate needs it first and
        reused by the next, and the ordered sequence keeps those gates adjacent.
        Stop-ReleaseElevatedSession ends it as soon as the following scenario does
        not want it, because an instance left running is the same mutex problem
        pointed the other way.

        Returns $null when this runner is not elevated. An unelevated parent cannot
        launch an elevated child without a Secure Desktop prompt, and raising one
        here would be the runner clicking UAC by proxy.
    #>
    param([Parameter(Mandatory)] $Run)
    if ($null -ne $script:ElevatedSession) {
        $script:ElevatedSession.Process.Refresh()
        if (-not $script:ElevatedSession.Process.HasExited) { return $script:ElevatedSession }
        try { $script:ElevatedSession.Connection.Close() } catch { }
        $script:ElevatedSession = $null
    }
    if (-not (Test-RunnerElevated)) { return $null }

    # The campaign's own unelevated instance holds the same machine-wide mutex.
    Stop-ReleaseSession

    $exe = $Run.Artifact.exePath
    $sessionRunId = New-LiveVerifyRunId
    Write-Step 'launching the shared ELEVATED instance for the present-diagnostics gates'
    $process = Start-Process -FilePath $exe -PassThru -ArgumentList @('--live-verify-control', $sessionRunId)
    try { $connection = Connect-LiveVerify -RunId $sessionRunId -ConnectTimeoutMs 30000 }
    catch {
        if (-not $process.HasExited) { $process | Stop-Process -Force -ErrorAction SilentlyContinue }
        throw
    }
    $script:ElevatedSession = [pscustomobject]@{
        Process    = $process
        Connection = $connection
        RunId      = $sessionRunId
    }
    return $script:ElevatedSession
}

function Stop-ReleaseElevatedSession {
    if ($null -eq $script:ElevatedSession) { return }
    $session = $script:ElevatedSession
    $script:ElevatedSession = $null
    try { $session.Connection.Close() } catch { }
    try {
        $session.Process.Refresh()
        if (-not $session.Process.HasExited) {
            [void]$session.Process.CloseMainWindow()
            if (-not $session.Process.WaitForExit(10000)) { $session.Process.Kill() }
        }
    }
    catch { }
    Write-Step 'the shared elevated instance was closed'
}

function Start-ReleaseSession {
    param([Parameter(Mandatory)] $Run)

    if ($null -ne $script:Session) {
        $script:Session.Process.Refresh()
        if (-not $script:Session.Process.HasExited) { return $script:Session }
        try { $script:Session.Connection.Close() } catch { }
        $script:Session = $null
    }

    # The shared elevated instance holds the same machine-wide single-instance
    # mutex: launching over it would hand focus to it and exit without ever opening
    # a pipe, which is the exact failure REL-CAP-FSE-001 reported in rc19.
    Stop-ReleaseElevatedSession

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
    # A leftover recovery manifest (see Backup-ReleaseRecoveryManifest) is moved
    # aside once at `prepare`, but a scenario that genuinely leaves a recording
    # unfinalized -- or a crash mid-campaign -- can write a fresh one at any later
    # launch. Dismissed here, at connect, before the scenario sends its first
    # command: a recovery surface refuses everything else on the control channel,
    # so any later point would be too late for the scenario about to run.
    Clear-ReleaseBlockingSurface -Session $script:Session -When 'at launch'
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
        # A close that does not exit is not a defect here: closing to the tray is
        # product behaviour and the setting that governs it is a user choice. The
        # process is ended after the grace period so the next scenario starts from a
        # known state, and REL-SHUTDOWN-001 is where the exit invariant is actually
        # asserted -- not in a teardown that cannot tell the two apart.
        if (-not $session.Process.WaitForExit(10000)) {
            # Says what was observed, not why. A close that has not finished in ten
            # seconds may be hiding to the tray, finalizing a recording, or stuck --
            # and a teardown that names one of those is guessing. The app's own
            # `close requested -> ...` log line is what distinguishes them.
            Write-Step 'the process had not exited 10 s after the close; ending it for the next scenario'
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

function Write-ReleaseQualificationRecord {
    <#
    .SYNOPSIS
        Regenerates `release-verification.json` from the current campaign state and
        returns it.
    .DESCRIPTION
        Always regenerated, never patched: the record is the release verdict, and one
        that could drift from the state it claims to describe would be worse than
        none at all.
    #>
    param(
        [Parameter(Mandatory)] $Run,
        [Parameter(Mandatory)] [object[]] $Catalog,
        [string[]] $RequiredOptIn = @()
    )

    $packages = @()
    $packagesPath = Join-Path $Run.Directory 'packages.json'
    if (Test-Path -LiteralPath $packagesPath) {
        $stored = Read-JsonFile -Path $packagesPath
        if ($null -ne $stored) { $packages = @($stored) }
    }
    $record = New-ReleaseQualificationRecord -Run $Run -Catalog $Catalog `
        -CatalogVersion (Get-ReleaseScenarioCatalogVersion) -RequiredOptIn $RequiredOptIn `
        -RepositoryRoot $repositoryRoot -Packages $packages
    Write-JsonAtomic -Path (Join-Path $Run.Directory 'release-verification.json') -Value $record
    return $record
}

function Show-ReleaseQualification {
    <#
    .SYNOPSIS
        Prints the promotion verdict a developer acts on.
    #>
    param([Parameter(Mandatory)] $Record)

    if ($Record.qualification.overall -eq 'QUALIFIED') {
        Write-Heading 'QUALIFIED FOR PROMOTION'
        Write-Host "  commit : $($Record.sourceCommit)" -ForegroundColor Green
        Write-Host "  rc tag : $($Record.rcTag)" -ForegroundColor Green
        foreach ($package in @($Record.packages)) {
            Write-Host "  package: $($package.fileName)  $($package.sha256)"
        }
        return
    }
    Write-Heading 'NOT QUALIFIED FOR PROMOTION'
    foreach ($reason in @($Record.qualification.reasons)) {
        Write-Host "  - $reason" -ForegroundColor Red
    }
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

function Read-ReleaseOperatorJudgement {
    <#
    .SYNOPSIS
        The sight check: one judgement, subject to the same rules as a human gate.
    .DESCRIPTION
        Two of the sixteen scenarios end in a person's eyes rather than a
        measurement. Colour on a capture-excluded overlay has no other reader:
        WDA_EXCLUDEFROMCAPTURE defeats screenshots, screen recording and
        PrintWindow by design, and the visual harness only ever grabs the scene
        graph, which shows correct alpha even when the window composes wrongly on
        screen. UI Automation can say the surface is really there with really that
        text -- and that is asserted separately -- but not what colour it is.

        Returned as a hashtable so the reason for a `wrong` answer travels with it
        into the report. `skip` rather than an answer for an attested scenario, and
        that is the point: -Attest says the CALLER performed an action, and a Verify
        block still decides. Here the question IS the verdict, and no caller can
        perform someone else's looking. Attesting it would manufacture a pass for a
        surface nobody saw; what -Attest legitimately buys is the run up to this
        point.
    #>
    param(
        [Parameter(Mandatory)] [string] $ScenarioId,
        [Parameter(Mandatory)] [string] $Line,
        [hashtable] $Detail = @{}
    )
    if ($NonInteractive) {
        Write-Step "not asked (-NonInteractive): $Line"
        return @{ Answer = 'skip' }
    }
    if (@(Expand-ListArgument -Values $Attest) -contains $ScenarioId) {
        Write-Step "not asked (-Attest names this scenario; a visual judgement cannot be attested): $Line"
        return @{ Answer = 'skip' }
    }
    if (-not (Test-ReleaseOperatorPresent)) {
        Write-Step "not asked (no interactive terminal): $Line"
        return @{ Answer = 'skip' }
    }
    $step = $Detail.Clone()
    $step['Id'] = $ScenarioId
    $step['Line'] = $Line
    return Invoke-ReleaseOperatorJudgement -Step $step
}

function New-ReleaseContext {
    <#
    .SYNOPSIS
        The object every scenario body receives.
    .DESCRIPTION
        EnsureSession and HumanGate are PLAIN script blocks over script-scoped state,
        deliberately not `.GetNewClosure()` ones. GetNewClosure binds the captured
        VARIABLES into a fresh synthetic module, and that module does not inherit this
        script's functions -- so a closure invoked from inside EnvironmentOrchestrator
        (a real module) failed with "Start-ReleaseSession is not recognized" while the
        function was plainly defined a few lines above. A plain block keeps the
        script's session state and resolves both.
    #>
    param([Parameter(Mandatory)] $Run, [Parameter(Mandatory)] $Orchestrator)
    $script:CurrentRun = $Run
    return [pscustomobject]@{
        RunDirectory   = $Run.Directory
        Artifact       = $Run.Artifact
        Environment    = $Run.Environment
        Orchestrator   = $Orchestrator
        RepositoryRoot = $repositoryRoot
        State          = @{}
        EnsureSession  = { Start-ReleaseSession -Run $script:CurrentRun }
        EndSession     = { Stop-ReleaseSession }
        # The shared elevated instance the present gates reuse. $null when this
        # runner is not elevated, which is the caller's cue to fall back to the
        # operator gate rather than to raise a prompt of its own.
        ElevatedSession = { Start-ReleaseElevatedSession -Run $script:CurrentRun }
        HumanGate      = { param($gate) Invoke-ReleaseHumanGate -Gate $gate -Context $script:CurrentContext }
        # One question at the moment it can be answered, for a scenario that has
        # several observable states rather than one verdict at the end.
        Judge          = { param($scenarioId, $line, $detail) Read-ReleaseOperatorJudgement -ScenarioId $scenarioId -Line $line -Detail $detail }
    }
}

function Clear-ReleaseBlockingSurface {
    <#
    .SYNOPSIS
        Dismisses a modal surface wherever it showed up, and says that it did.
    .DESCRIPTION
        The product refuses to be driven while a blocking surface is up -- a
        recording error, a recovery offer, a crash-report prompt -- and that refusal
        is correct: those surfaces exist to be answered. What is not correct is
        carrying one into the NEXT scenario, or into a fresh launch, which then
        meets "A recordingError surface is open; answer it before driving the
        shell" at its own setup and reports a failure describing something else
        entirely. Four gates failed that way in one sweep, a 30 min soak died 13 s
        in on a recovery offer an earlier scenario had left behind, and a leftover
        recovery-manifest.json from a PREVIOUS campaign failed two gates before
        this one's first scenario ever ran -- the app opened the recovery surface
        at launch, before any scenario or cleanup pass had a chance to run at all.

        Called from two places for that reason: Close-ReleaseBlockingSurface
        (after every scenario, the sibling of Stop-ReleaseLeakedRecording) and
        Start-ReleaseSession (at every launch, before the first command of the
        scenario about to run). A scenario that leaves a surface open, or a
        campaign that inherits one, is a fact about the catalog worth seeing even
        though it is repaired here.

        Dismissed, never answered: `recovery.dismiss` puts the offer away without
        deciding it and `recordingError.dismiss` closes without sending a report.
        Deciding FOR the operator would be a different kind of wrong.
    #>
    param($Session, [string] $When = 'after the scenario')
    if ($null -eq $Session) { return }
    $commandFor = @{
        recordingError = 'recordingError.dismiss'
        recovery       = 'recovery.dismiss'
        crashReport    = 'crashReport.decline'
    }
    try {
        # Bounded: one surface can reveal another (a recovery offer behind a crash
        # prompt), but a surface that keeps coming back is a product fact to report
        # rather than something to loop on.
        for ($attempt = 0; $attempt -lt 4; $attempt++) {
            $state = Get-LiveVerifyState -Connection $Session.Connection
            $surface = "$($state.blockingSurface)"
            if ([string]::IsNullOrWhiteSpace($surface) -or $surface -eq 'none') { return }
            if (-not $commandFor.ContainsKey($surface)) {
                Write-Step "a '$surface' surface was open $When and this runner has no command to close it"
                return
            }
            Write-Step "a $surface surface was open $When; dismissing it"
            $answer = Invoke-LiveVerifyCommand -Connection $Session.Connection -Command $commandFor[$surface]
            if (-not $answer.ok) {
                Write-Step "  it refused to close: $($answer.error.message)"
                return
            }
        }
    }
    catch {
        # The session may already be gone -- a scenario is allowed to end it.
    }
}

function Close-ReleaseBlockingSurface {
    param($Session)
    Clear-ReleaseBlockingSurface -Session $Session -When 'after the scenario'
}

function Backup-ReleaseRecoveryManifest {
    <#
    .SYNOPSIS
        Moves a leftover recovery manifest out of the way before the campaign
        launches anything, and says that it did.
    .DESCRIPTION
        The runner launches the real, unconfigured ExoSnap -- no EXOSNAP_CONFIG_DIR
        override, deliberately, because a release gate has to bind its verdict to
        the same persistence path a shipped install actually uses. That means a
        recovery-manifest.json a PREVIOUS campaign (or a real crash) left behind is
        still sitting under %LOCALAPPDATA%\ExoSnap when this one starts, and the
        app opens the recovery surface at the very first launch -- before any
        scenario has run, and before the control channel has offered a single
        command to answer it with. Clear-ReleaseBlockingSurface, called from every
        session start, closes a surface that reappears mid-campaign; it cannot
        reach this one, because it needs a connection that a blocked launch never
        finishes making.

        Moved into the campaign directory, never deleted: the manifest may
        describe a real, recoverable recording, and this runner has no basis for
        deciding that it does not.
    #>
    param([Parameter(Mandatory)] [string] $CampaignDirectory)
    if ([string]::IsNullOrWhiteSpace($env:LOCALAPPDATA)) { return }
    $manifestPath = Join-Path $env:LOCALAPPDATA 'ExoSnap/recovery-manifest.json'
    if (-not (Test-Path -LiteralPath $manifestPath)) { return }
    $backupPath = Join-Path $CampaignDirectory 'leftover-recovery-manifest.json'
    Move-Item -LiteralPath $manifestPath -Destination $backupPath -Force
    Write-Step "a leftover recovery manifest from a previous run was moved to $backupPath so it cannot open the recovery surface at launch"
}

function Stop-ReleaseLeakedRecording {
    <#
    .SYNOPSIS
        Ends a recording a scenario left running, and says that it did.
    .DESCRIPTION
        The product analogue of the environment restore, and it exists for the same
        reason: a scenario must not be able to hand the next one a machine it did not
        expect.

        Several human-gated scenarios start a recording in their body and stop it in
        their Verify block, which is correct while a human answers -- but a DEFERRED
        gate never runs Verify, so the recording simply kept going. The next scenario
        then met `update.check is refused while recording` and reported a FAIL that
        described the runner, not the product. Cleaning up inside every one of those
        five bodies would have been five chances to forget; this is one.

        Reported rather than silent: a scenario that leaks a recording is a fact about
        the catalog worth seeing, even though it is repaired here.
    #>
    param($Session)
    if ($null -eq $Session) { return }
    try {
        $state = Get-LiveVerifyState -Connection $Session.Connection
        if ($state.recordingState -notin @('Recording', 'Paused', 'Countdown')) { return }
        Write-Step "a recording was still $($state.recordingState) after the scenario; stopping it"
        [void](Invoke-LiveVerifyCommand -Connection $Session.Connection -Command 'record.stop')
        [void](Wait-ReleaseRecordingState -Connection $Session.Connection -States @('Completed', 'Failed', 'Ready') -TimeoutMs 60000)
    }
    catch {
        # The session may already be gone -- a scenario is allowed to end it. Nothing
        # is owed then, and turning that into an error would report a teardown as a
        # product failure.
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
    $script:CurrentContext = $context

    $environmentTable = ConvertTo-Hashtable $Run.Environment
    $artifactFingerprint = $Run.Artifact.fingerprint
    $aliases = $null
    if ($Orchestrator.Available) {
        try { $aliases = Resolve-EnvironmentAliases -Orchestrator $Orchestrator } catch { $aliases = $null }
    }

    # One window, one ordered sequence: the dependencies between the gates are
    # declared in the catalog and resolved here, so an operator sees the whole run
    # before the first prompt instead of discovering halfway through that the gate
    # they just passed removed the starting point of the next one.
    $Entries = @(Get-ReleaseHumanPlanOrder -Entries $Entries)
    Show-ReleaseHumanPlan -Entries $Entries

    try {
        for ($position = 0; $position -lt $Entries.Count; $position++) {
            $entry = $Entries[$position]
            Write-Heading "$($entry.Id)  $($entry.Title)"
            Write-Step "layer $($entry.Layer)  class $($entry.Class)"

            $environmentFingerprint = Get-LiveVerifyEnvironmentFingerprint -Entry $entry -Environment $environmentTable
            Set-LiveVerifyCheckRunning -Run $Run -Id $entry.Id `
                -ArtifactFingerprint $artifactFingerprint `
                -EnvironmentFingerprint $environmentFingerprint | Out-Null

            $outcome = Invoke-OneScenario -Entry $entry -Context $context -Orchestrator $Orchestrator -Aliases $aliases
            # The shared elevated instance is released the moment the NEXT scenario
            # does not want it. It holds the machine-wide single-instance mutex, so
            # one left running makes every following scenario wait for a control
            # channel that was never created.
            $next = if ($position + 1 -lt $Entries.Count) { $Entries[$position + 1] } else { $null }
            $nextWantsElevated = $null -ne $next -and
                $next.PSObject.Properties.Name -contains 'UsesElevatedSession' -and $next.UsesElevatedSession
            if (-not $nextWantsElevated) { Stop-ReleaseElevatedSession }
            # Before the verdict is written, so the next scenario cannot inherit a
            # recording this one started. See Stop-ReleaseLeakedRecording.
            Stop-ReleaseLeakedRecording -Session $script:Session
            # ...nor a modal surface, which refuses every command the next scenario
            # sends. See Close-ReleaseBlockingSurface.
            Close-ReleaseBlockingSurface -Session $script:Session

            $evidence = @()
            if ($outcome.ContainsKey('Evidence') -and $null -ne $outcome.Evidence) { $evidence = @($outcome.Evidence) }
            Complete-LiveVerifyCheck -Run $Run -Id $entry.Id -Result $outcome.Result `
                -Message $outcome.Message -Evidence $evidence `
                -RestoreResult $outcome.RestoreResult -EnvironmentEvidence $outcome.EnvironmentEvidence | Out-Null

            $colour = switch ($outcome.Result) {
                'PASS' { 'Green' }
                'FAIL' { 'Red' }
                'INFRA_ERROR' { 'Red' }
                default { 'Yellow' }
            }
            Write-Host "  -> $($outcome.Result)  $($outcome.Message)" -ForegroundColor $colour
            if ($outcome.RestoreResult -ne 'NOT_APPLICABLE' -and $outcome.RestoreResult -ne 'RESTORED') {
                Write-Host "  -> ENVIRONMENT RESTORE: $($outcome.RestoreResult)" -ForegroundColor Red
            }
        }
    }
    finally {
        Stop-ReleaseElevatedSession
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

    # `Desired` may be a hashtable or a script block. The block form exists because
    # some desired values can only be chosen against the machine in front of you --
    # a refresh rate has to come from what the display actually enumerates, and a
    # catalogue that hardcoded one would only ever be correct at one desk. A block
    # that returns nothing is saying "this machine offers no usable value", which is
    # UNAVAILABLE rather than a failure.
    $desired = @{}
    if ($Entry.PSObject.Properties.Name -contains 'Desired' -and $null -ne $Entry.Desired) {
        if ($Entry.Desired -is [scriptblock]) {
            $resolved = & $Entry.Desired $Context
            if ($null -eq $resolved -or $resolved -isnot [hashtable]) {
                $result.Result = 'UNAVAILABLE'
                $result.Message = if ($resolved -is [string]) { $resolved }
                else { 'this machine offers no value the scenario can target' }
                return $result
            }
            $desired = $resolved
        }
        else { $desired = $Entry.Desired }
    }

    if ($desired.Count -gt 0 -and -not $Orchestrator.Available) {
        $result.Result = 'UNAVAILABLE'
        $result.Message = 'exosnap-envctl is not built; this scenario needs a real environment mutation and ' +
        'will not fake one'
        return $result
    }

    # A dirty environment is answered HERE rather than by letting
    # Assert-EnvironmentClean throw into the generic catch below. Both refuse to run
    # the scenario, but the catch would record every subsequent mutating scenario as
    # FAIL -- turning one unrestored display into a page of red that reads like a
    # product collapse. Nothing was tested, so nothing failed: UNAVAILABLE, with the
    # reason and the way out.
    if ($desired.Count -gt 0 -and $Orchestrator.Dirty) {
        $result.Result = 'UNAVAILABLE'
        $result.Message = "The environment is dirty from an earlier transaction and was not restored: " +
        "$($Orchestrator.DirtyDetail). Mutating scenarios stay blocked until " +
        "'release-verify.ps1 recover' reports it clean."
        return $result
    }

    try {
        $transaction = Invoke-EnvironmentTransaction -Orchestrator $Orchestrator -Scenario $Entry.Id `
            -Desired $desired -Body { param($begun) & $Entry.Run $Context $begun }.GetNewClosure()
    }
    catch {
        # The transaction never opened, so the product was never exercised. That is an
        # infrastructure failure, and calling it FAIL would put a defect ExoSnap does
        # not have into a release report.
        $result.Result = 'INFRA_ERROR'
        $result.Message = "Scenario setup failed: $($_.Exception.Message)"
        return $result
    }

    $result.RestoreResult = $transaction.RestoreResult
    $result.EnvironmentEvidence = $transaction.Evidence

    # Resolve-ReleaseScenarioOutcome owns the FAIL / INFRA_ERROR / UNAVAILABLE split
    # and lives in a library, so the rule is testable without running a campaign.
    $outcome = Resolve-ReleaseScenarioOutcome -Transaction $transaction
    $result.Result = $outcome.Result
    $result.Message = $outcome.Message
    if ($outcome.Evidence.Count -gt 0) { $result.Evidence = @($outcome.Evidence) }
    return $result
}

# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------

function Expand-ListArgument {
    <#
    .SYNOPSIS
        Normalises a list parameter that may have arrived as one comma-joined string.
    .DESCRIPTION
        `pwsh -File script.ps1 -Only A,B` does NOT parse PowerShell array syntax: the
        whole thing arrives as the single string "A,B". Without this, such a call
        matched no scenario and the runner printed "Nothing runnable" -- a silently
        empty selection that reads exactly like "everything is already done".
    #>
    param([string[]] $Values)
    if ($null -eq $Values) { return @() }
    return @($Values | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } |
            Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
}

function Select-Entries {
    param([Parameter(Mandatory)] [object[]] $Catalog, [object[]] $Runnable)
    $entries = if ($null -eq $Runnable) { $Catalog } else { $Runnable }
    $selected = @(Expand-ListArgument -Values $Only)
    if ($selected.Count -gt 0) {
        $unknown = @($selected | Where-Object { $_ -notin @($Catalog | ForEach-Object { $_.Id }) })
        if ($unknown.Count -gt 0) {
            $names = $unknown -join ', '
            throw "No such scenario: $names. Run 'release-verify.ps1 list' for the catalog."
        }
        $entries = @($entries | Where-Object { $selected -contains $_.Id })
    }
    # Opt-in classes stay out of a default sweep. A 45-minute mixed-clock recording
    # and a scenario that asks the operator to unplug an audio interface are not
    # things a runner should start because somebody typed `run`.
    $included = @(Expand-ListArgument -Values $IncludeClass)
    $entries = @($entries | Where-Object {
            $optIn = $_.PSObject.Properties.Name -contains 'OptIn' -and $_.OptIn
            (-not $optIn) -or ($included -contains $_.Class) -or ($selected -contains $_.Id)
        })
    return [object[]]$entries
}

# ---------------------------------------------------------------------------
# The typed harness (ADR 0070)
# ---------------------------------------------------------------------------

function Invoke-DotNetVerifyHarness {
    <#
    .SYNOPSIS
        Forwards one command to ExoSnap.Verify, building it first when needed.
    .DESCRIPTION
        The bootstrap's whole job. It resolves the SDK, builds the solution if the
        executable is missing or older than its sources, translates this script's
        parameters into the harness's own vocabulary, and returns the harness's exit
        code unchanged -- a wrapper that swallowed a non-zero exit would turn a
        refusal to qualify into a success.

        `recover`, `resume`, `retry` and `status` are not forwarded: they have no
        counterpart in the typed harness yet, and answering them with something that
        merely looks similar would be worse than saying so.
    #>
    param(
        [Parameter(Mandatory)] [string] $Verb
    )

    $solution = Join-Path $repositoryRoot 'tools/release-verify/ExoSnap.Verify.slnx'
    if (-not (Test-Path -LiteralPath $solution -PathType Leaf)) {
        throw "The typed harness is not in this checkout: $solution is missing."
    }
    if ($null -eq (Get-Command dotnet -ErrorAction SilentlyContinue)) {
        throw 'dotnet is not on PATH; -Engine DotNet needs the .NET SDK pinned by tools/release-verify/global.json.'
    }

    $forwardable = @('prepare', 'run', 'list', 'qualify', 'report')
    if ($Verb -notin $forwardable) {
        throw "-Engine DotNet does not carry '$Verb' yet. It forwards $($forwardable -join ', '); " +
        "run this one with -Engine PowerShell."
    }

    $project = Join-Path $repositoryRoot 'tools/release-verify/ExoSnap.Verify/ExoSnap.Verify.csproj'
    $exe = Join-Path $repositoryRoot 'tools/release-verify/ExoSnap.Verify/bin/Release/net10.0-windows/ExoSnap.Verify.exe'

    # Rebuilt when anything under the harness is newer than the executable, so a
    # campaign never runs gates that have been edited since they were compiled.
    $newestSource = (Get-ChildItem -LiteralPath (Join-Path $repositoryRoot 'tools/release-verify') -Recurse -File |
            Where-Object { $_.FullName -notmatch '\\(bin|obj)\\' } |
            Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1)
    $stale = -not (Test-Path -LiteralPath $exe -PathType Leaf)
    if (-not $stale -and $null -ne $newestSource) {
        $stale = $newestSource.LastWriteTimeUtc -gt (Get-Item -LiteralPath $exe).LastWriteTimeUtc
    }

    if ($stale) {
        Write-Step 'building tools/release-verify (Release)'
        $priorDotNetPlatform = $env:Platform
        Push-Location (Split-Path -Parent $solution)
        try {
            # MSVC exports Platform=x64, but this solution uses Any CPU. The working
            # directory also selects the harness's pinned SDK through global.json.
            $env:Platform = $null
            & dotnet build $solution -c Release --nologo | Out-Host
            if ($LASTEXITCODE -ne 0) {
                throw "The typed harness did not build (exit $LASTEXITCODE); nothing was run."
            }
        }
        finally {
            $env:Platform = $priorDotNetPlatform
            Pop-Location
        }
    }

    $arguments = @($Verb, '--repo', $repositoryRoot)
    if ($RunId) { $arguments += @('--run-dir', (Join-Path $runsRoot $RunId)) }
    switch ($Verb) {
        'prepare' {
            if ($ExePath) { $arguments += @('--exe', $ExePath) }
            if ($Tag) { $arguments += @('--rc', $Tag) }
            if ($SourceCommit) { $arguments += @('--commit', $SourceCommit) }
            if ($RunId) { $arguments += @('--run-id', $RunId) }
            foreach ($package in @($PortableZip, $Msi)) {
                if ($package) { $arguments += @('--package', $package) }
            }
        }
        'run' {
            foreach ($id in @(Expand-ListArgument -Values $Only)) { $arguments += @('--id', $id) }
            $classes = @(Expand-ListArgument -Values $IncludeClass)
            foreach ($class in $classes) { $arguments += @('--class', $class) }
            if ($classes.Count -gt 0) { $arguments += '--include-opt-in' }
            if ($AliasProfile) { $arguments += @('--alias-profile', $AliasProfile) }
        }
        'qualify' {
            foreach ($id in @(Expand-ListArgument -Values $Required)) { $arguments += @('--required', $id) }
        }
    }

    Write-Step "ExoSnap.Verify $($arguments -join ' ')"
    # Build and command output must stay out of the success stream: the caller passes
    # this function's result to exit, which needs exactly one integer.
    $output = & $exe @arguments
    $exitCode = $LASTEXITCODE
    $output | Out-Host
    return $exitCode
}

if ($Engine -eq 'DotNet') {
    exit (Invoke-DotNetVerifyHarness -Verb $Command)
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
        $artifact = Get-ReleaseArtifactFingerprint -Path $ExePath -ReleaseTag $Tag -SourceCommit $SourceCommit
        $packages = @()
        if (-not [string]::IsNullOrWhiteSpace($PortableZip)) {
            $packages += Get-ReleasePackageIdentity -Path $PortableZip -Kind 'portable'
        }
        if (-not [string]::IsNullOrWhiteSpace($Msi)) {
            $packages += Get-ReleasePackageIdentity -Path $Msi -Kind 'installer'
        }
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
        # Kept out of artifact-fingerprint.json deliberately: a package deleted from
        # disk after the campaign started must not turn every verified result STALE.
        if ($packages.Count -gt 0) {
            Write-JsonAtomic -Path (Join-Path $directory 'packages.json') -Value $packages
        }
        Backup-ReleaseRecoveryManifest -CampaignDirectory $directory

        Write-Heading "Release campaign $campaignId"
        Write-Host "  artifact : $($artifact.exePath)"
        Write-Host "  version  : $($artifact.productVersion)  tag $($artifact.tag)"
        Write-Host "  sha256   : $($artifact.exeSha256)"
        Write-Host "  commit   : $(if ($artifact.sourceCommit) { $artifact.sourceCommit } else { 'UNKNOWN - pass -SourceCommit, or this campaign can never qualify a release' })"
        Write-Host "  packages : $(if ($packages.Count -gt 0) { ($packages | ForEach-Object { $_.fileName }) -join ', ' } else { 'NONE - pass -PortableZip/-Msi, or this campaign can never qualify a release' })"
        Write-Host "  install  : $($artifact.installTree)"
        Write-Host "  envctl   : $(if ($orchestrator.Available) { $orchestrator.EnvctlPath } else { 'NOT BUILT - mutating scenarios will report UNAVAILABLE' })"
        if ($orchestrator.Dirty) {
            Write-Host "  WARNING  : environment is dirty - $($orchestrator.DirtyDetail)" -ForegroundColor Red
        }
        Write-Host ''
        Write-Host "  $($catalog.Count) scenarios pending. Run them with: release-verify.ps1 run"
        return
    }

    'retry' {
        # A FAIL is a finding, not a to-do: `run` deliberately leaves it alone so a
        # rerun cannot quietly erase it. Re-attempting one is therefore an explicit
        # act, and it drops the old evidence link so the report never pairs a new
        # verdict with an old artefact.
        $directory = Resolve-RunDirectory
        $run = Get-LiveVerifyRun -RunDirectory $directory
        $selected = @(Expand-ListArgument -Values $Only)
        if ($selected.Count -eq 0) { throw 'retry needs -Only <scenario id>' }
        foreach ($id in $selected) { Reset-LiveVerifyCheck -Run $run -Id $id | Out-Null }
        Write-Heading "Reset for retry: $($selected -join ', ')"
        $orchestrator = New-EnvironmentOrchestrator -RunId $run.Run.runId `
            -JournalDirectory (Join-Path $directory 'environment') -EnvctlPath $EnvctlPath -AliasProfile $AliasProfile
        $entries = @(Select-Entries -Catalog $catalog -Runnable $null)
        if ($entries.Count -eq 0) { Write-Host 'Nothing runnable.'; return }
        Invoke-Scenarios -Run $run -Orchestrator $orchestrator -Catalog $catalog -Entries $entries
        Write-LiveVerifyReport -Run $run | Out-Null
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
        # @(...) around the call, not only inside it: a function that returns an
        # EMPTY [object[]] hands back $null, and $null.Count throws under
        # StrictMode -- which is what "nothing left to run" looked like.
        $entries = @(Select-Entries -Catalog $catalog -Runnable $runnable)
        if ($entries.Count -eq 0) { Write-Host 'Nothing runnable.'; return }
        Invoke-Scenarios -Run $run -Orchestrator $orchestrator -Catalog $catalog -Entries $entries
        Write-LiveVerifyReport -Run $run | Out-Null
        return
    }

    'resume' {
        $directory = Resolve-RunDirectory
        $run = Get-LiveVerifyRun -RunDirectory $directory
        $orchestrator = New-EnvironmentOrchestrator -RunId $run.Run.runId `
            -JournalDirectory (Join-Path $directory 'environment') -EnvctlPath $EnvctlPath -AliasProfile $AliasProfile
        # The source commit is re-supplied rather than re-derived: it is part of the
        # artifact fingerprint, and a resume that silently lost it would mark every
        # verified result STALE against a binary that did not change.
        $artifact = Get-ReleaseArtifactFingerprint -Path $run.Artifact.exePath -ReleaseTag $run.Artifact.tag `
            -SourceCommit $run.Artifact.sourceCommit
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
        # @(...) around the call, not only inside it: a function that returns an
        # EMPTY [object[]] hands back $null, and $null.Count throws under
        # StrictMode -- which is what "nothing left to run" looked like.
        $entries = @(Select-Entries -Catalog $catalog -Runnable $runnable)
        if ($entries.Count -eq 0) { Write-Host 'Nothing runnable.'; return }
        Invoke-Scenarios -Run $run -Orchestrator $orchestrator -Catalog $catalog -Entries $entries
        Write-LiveVerifyReport -Run $run | Out-Null
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
                'INFRA_ERROR' { 'Red' }
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
        Write-LiveVerifyReport -Run $run | Out-Null
        $record = Write-ReleaseQualificationRecord -Run $run -Catalog $catalog `
            -RequiredOptIn (Expand-ListArgument -Values $Required)
        Write-Heading 'Report written'
        Write-Host "  $(Join-Path $directory 'release-verification.json')"
        Write-Host "  $(Join-Path $directory 'report.md')"
        Write-Host "  $(Join-Path $directory 'junit.xml')"
        Show-ReleaseQualification -Record $record
        return
    }

    'qualify' {
        # The promotion step. It decides nothing a report did not already decide --
        # it states the decision in the one sentence a release is cut on, and offers
        # the single act that makes the record reachable by the publish gate.
        $directory = Resolve-RunDirectory
        $run = Get-LiveVerifyRun -RunDirectory $directory
        Write-LiveVerifyReport -Run $run | Out-Null
        $record = Write-ReleaseQualificationRecord -Run $run -Catalog $catalog `
            -RequiredOptIn (Expand-ListArgument -Values $Required)
        $recordPath = Join-Path $directory 'release-verification.json'
        Show-ReleaseQualification -Record $record
        Write-Host ''
        Write-Host "  record : $recordPath"

        if ($record.qualification.overall -ne 'QUALIFIED') {
            Write-Host ''
            Write-Host '  Nothing is uploaded and no tag may be pushed until every reason above is gone.'
            exit 1
        }

        if ([string]::IsNullOrWhiteSpace($SigningKeyBase64)) {
            Write-Host ''
            Write-Host '  This record is UNSIGNED and the publish gate refuses an unsigned record.' -ForegroundColor Yellow
            Write-Host '  Set EXOSNAP_UPDATE_SIGNING_KEY (the base64 ed25519 seed, the same key the update'
            Write-Host '  manifest is signed with) or pass -SigningKeyBase64, then run qualify again.'
            if ($Publish) {
                throw 'Refusing to attach an unsigned qualification record to the RC release.'
            }
            return
        }

        $signaturePath = New-ReleaseQualificationSignatureFile -RecordPath $recordPath `
            -SigningKeyBase64 $SigningKeyBase64 -ExpectedPublicKeyHex $env:EXOSNAP_UPDATE_PUBLIC_KEY_HEX
        Write-Host "  signed : $signaturePath"

        if (-not $Publish) {
            Write-Host ''
            Write-Host '  The publish gate reads this record from the RC release, so it has to be attached to it:'
            Write-Host "    pwsh scripts/release-verify.ps1 qualify -RunId $($run.Run.runId) -Publish"
            Write-Host "  Then push the final tag from $($record.sourceCommit)."
            return
        }

        if ($null -eq (Get-Command gh -ErrorAction SilentlyContinue)) {
            throw 'gh is not on PATH; the qualification record cannot be attached to the RC release.'
        }
        Write-Heading "Attaching the record to $($record.rcTag)"
        # Both files or neither: a record whose signature did not make it to the
        # release reads as unsigned at the gate, which is the one failure that would
        # look like tampering rather than like an interrupted upload.
        & gh release upload $record.rcTag $recordPath $signaturePath --clobber
        if ($LASTEXITCODE -ne 0) {
            throw "gh release upload failed with exit code $LASTEXITCODE; the record is NOT attached."
        }
        Write-Host "  uploaded release-verification.json and its detached signature to $($record.rcTag)" -ForegroundColor Green
        Write-Host "  the final tag may now be pushed from $($record.sourceCommit)."
        return
    }
}
