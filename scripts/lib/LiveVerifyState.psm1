#requires -Version 7.0
Set-StrictMode -Version Latest

<#
.SYNOPSIS
    Persistent, resumable state for a Live Verify acceptance run.

.DESCRIPTION
    The whole reason this exists instead of a Markdown checklist: a checkbox
    records that somebody ticked it, not what was tested, on which binary, in
    which environment, or whether the run died halfway through the check it was
    ticking.

    Three properties this module is responsible for, and they are the only
    reasons it is not a hashtable:

      1. ATOMIC. A check is persisted as RUNNING before it executes, and its
         terminal state is written in one Move-Item over a temporary file. A
         runner killed mid-check therefore leaves evidence that it was mid-check.
      2. INTERRUPTION IS NOT SUCCESS. Resume converts a stranded RUNNING into
         UNVERIFIED, never into PASS, and remembers that it was interrupted.
      3. ARTIFACT-BOUND. A PASS carries the fingerprint of the artifact and the
         environment properties it depended on. When either changes, the PASS
         becomes STALE rather than being inherited by a binary nobody tested.
#>

$script:CheckStates = @(
    'PENDING',          # never attempted in this run
    'RUNNING',          # persisted before execution; a leftover means interruption
    'PASS',
    'FAIL',
    'BLOCKED',          # environment cannot satisfy the check (no second monitor, no HDR display)
    'MANUAL_REQUIRED',  # waiting for a human gate
    'SKIPPED',          # deliberately not run, with a recorded reason
    'UNVERIFIED',       # attempted, outcome unknown (interrupted, or evidence unusable)
    'STALE'             # passed once, against an artifact/environment that has since changed
)

function Get-LiveVerifyCheckStates {
    return $script:CheckStates
}

function Write-JsonAtomic {
    <#
    .SYNOPSIS
        Whole-file replace via a temporary sibling, so a kill during the write
        cannot leave a half-written state file behind.
    #>
    param(
        [Parameter(Mandatory)] [string] $Path,
        [Parameter(Mandatory)] $Value
    )
    $directory = Split-Path -Parent $Path
    if (-not (Test-Path -LiteralPath $directory)) {
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
    }
    $temporary = "$Path.tmp"
    $json = $Value | ConvertTo-Json -Depth 30
    Set-Content -LiteralPath $temporary -Value $json -Encoding utf8NoBOM
    Move-Item -LiteralPath $temporary -Destination $Path -Force
}

function Read-JsonFile {
    param([Parameter(Mandatory)] [string] $Path)
    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    $raw = Get-Content -LiteralPath $Path -Raw
    return $raw | ConvertFrom-Json
}

function Get-LiveVerifyFingerprint {
    <#
    .SYNOPSIS
        Stable digest of a property bag, order-independent.
    .DESCRIPTION
        Used for both the artifact identity and the per-check environment
        dependency, so "did anything this check relies on change" is one string
        comparison rather than a hand-written field-by-field diff that quietly
        forgets a field.
    #>
    param([Parameter(Mandatory)] [hashtable] $Properties)
    $normalized = [ordered]@{}
    foreach ($key in ($Properties.Keys | Sort-Object)) {
        # `fingerprint` is the digest of the rest and must never be part of it.
        if ($key -eq 'fingerprint') { continue }
        $normalized[$key] = "$($Properties[$key])"
    }
    $text = ($normalized.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join ';'
    $stream = [System.IO.MemoryStream]::new([System.Text.Encoding]::UTF8.GetBytes($text))
    try {
        return (Get-FileHash -InputStream $stream -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    finally { $stream.Dispose() }
}

function New-LiveVerifyRun {
    <#
    .SYNOPSIS
        Creates the run directory and the initial state for a check catalog.
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] [string] $RunDirectory,
        [Parameter(Mandatory)] [string] $RunId,
        [Parameter(Mandatory)] [object[]] $Catalog,
        [hashtable] $Artifact = @{},
        [hashtable] $Environment = @{}
    )

    foreach ($sub in @('', 'checks', 'logs', 'media', 'analysis', 'screenshots', 'updater')) {
        $path = if ($sub) { Join-Path $RunDirectory $sub } else { $RunDirectory }
        New-Item -ItemType Directory -Path $path -Force | Out-Null
    }

    $checks = [ordered]@{}
    foreach ($entry in $Catalog) {
        $checks[$entry.Id] = [ordered]@{
            id                     = $entry.Id
            title                  = $entry.Title
            layer                  = $entry.Layer
            state                  = 'PENDING'
            attempts               = 0
            startedUtc             = $null
            finishedUtc            = $null
            message                = $null
            note                   = $null
            skipReason             = $null
            interrupted            = $false
            evidence               = @()
            artifactFingerprint    = $null
            environmentFingerprint = $null
        }
    }

    $run = [ordered]@{
        runId        = $RunId
        createdUtc   = [DateTime]::UtcNow.ToString('o')
        runnerFormat = 1
        directory    = $RunDirectory
    }

    Write-JsonAtomic -Path (Join-Path $RunDirectory 'run.json') -Value $run
    Write-JsonAtomic -Path (Join-Path $RunDirectory 'artifact-fingerprint.json') -Value $Artifact
    Write-JsonAtomic -Path (Join-Path $RunDirectory 'environment.json') -Value $Environment
    Write-JsonAtomic -Path (Join-Path $RunDirectory 'state.json') -Value ([ordered]@{ checks = $checks })

    return Get-LiveVerifyRun -RunDirectory $RunDirectory
}

function Get-LiveVerifyRun {
    <#
    .SYNOPSIS
        Loads a run. Throws on a corrupt state file rather than starting over --
        silently reinitialising would erase verified progress, which is the exact
        thing this module exists to protect.
    #>
    [CmdletBinding()]
    param([Parameter(Mandatory)] [string] $RunDirectory)

    $statePath = Join-Path $RunDirectory 'state.json'
    if (-not (Test-Path -LiteralPath $statePath)) {
        throw "No Live Verify run at '$RunDirectory' (state.json is missing)"
    }
    try {
        $state = Read-JsonFile -Path $statePath
    }
    catch {
        throw "Live Verify state at '$statePath' is corrupt and was NOT reset: $($_.Exception.Message)"
    }
    if ($null -eq $state -or $null -eq $state.checks) {
        throw "Live Verify state at '$statePath' is corrupt and was NOT reset: no check table"
    }

    return [pscustomobject]@{
        Directory   = $RunDirectory
        Run         = Read-JsonFile -Path (Join-Path $RunDirectory 'run.json')
        State       = $state
        Artifact    = Read-JsonFile -Path (Join-Path $RunDirectory 'artifact-fingerprint.json')
        Environment = Read-JsonFile -Path (Join-Path $RunDirectory 'environment.json')
    }
}

function Save-LiveVerifyRun {
    param([Parameter(Mandatory)] $Run)
    Write-JsonAtomic -Path (Join-Path $Run.Directory 'state.json') -Value $Run.State
}

function Get-LiveVerifyCheck {
    param(
        [Parameter(Mandatory)] $Run,
        [Parameter(Mandatory)] [string] $Id
    )
    if ($Run.State.checks.PSObject.Properties.Name -notcontains $Id) {
        throw "Unknown check id '$Id'"
    }
    return $Run.State.checks.$Id
}

function Set-LiveVerifyCheckRunning {
    <#
    .SYNOPSIS
        Persists RUNNING plus the identity the result will be bound to, BEFORE
        the check executes.
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] $Run,
        [Parameter(Mandatory)] [string] $Id,
        [string] $ArtifactFingerprint,
        [string] $EnvironmentFingerprint
    )
    $check = Get-LiveVerifyCheck -Run $Run -Id $Id
    $check.state = 'RUNNING'
    $check.startedUtc = [DateTime]::UtcNow.ToString('o')
    $check.finishedUtc = $null
    $check.message = $null
    $check.interrupted = $false
    $check.attempts = [int]$check.attempts + 1
    $check.artifactFingerprint = $ArtifactFingerprint
    $check.environmentFingerprint = $EnvironmentFingerprint
    Save-LiveVerifyRun -Run $Run
    return $check
}

function Complete-LiveVerifyCheck {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] $Run,
        [Parameter(Mandatory)] [string] $Id,
        [Parameter(Mandatory)] [ValidateSet('PASS', 'FAIL', 'BLOCKED', 'MANUAL_REQUIRED', 'UNVERIFIED')]
        [string] $Result,
        [string] $Message,
        [string[]] $Evidence = @()
    )
    $check = Get-LiveVerifyCheck -Run $Run -Id $Id
    $check.state = $Result
    $check.finishedUtc = [DateTime]::UtcNow.ToString('o')
    $check.message = $Message
    if ($Evidence.Count -gt 0) {
        $check.evidence = @($check.evidence) + $Evidence | Select-Object -Unique
    }
    Save-LiveVerifyRun -Run $Run
    return $check
}

function Set-LiveVerifyCheckSkipped {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] $Run,
        [Parameter(Mandatory)] [string] $Id,
        [Parameter(Mandatory)] [string] $Reason
    )
    if ([string]::IsNullOrWhiteSpace($Reason)) {
        throw 'A skip needs a reason; an unexplained skip is indistinguishable from an oversight'
    }
    $check = Get-LiveVerifyCheck -Run $Run -Id $Id
    $check.state = 'SKIPPED'
    $check.skipReason = $Reason
    $check.finishedUtc = [DateTime]::UtcNow.ToString('o')
    Save-LiveVerifyRun -Run $Run
    return $check
}

function Add-LiveVerifyNote {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] $Run,
        [Parameter(Mandatory)] [string] $Id,
        [Parameter(Mandatory)] [string] $Note
    )
    $check = Get-LiveVerifyCheck -Run $Run -Id $Id
    $check.note = if ([string]::IsNullOrWhiteSpace($check.note)) { $Note } else { "$($check.note)`n$Note" }
    Save-LiveVerifyRun -Run $Run
    return $check
}

function Reset-LiveVerifyCheck {
    <#
    .SYNOPSIS
        Puts one check back to PENDING for a retry, keeping its attempt count and
        any notes. Evidence from the previous attempt is retained on disk; the
        state no longer points at it, so a report cannot cite last attempt's
        evidence for this attempt's result.
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] $Run,
        [Parameter(Mandatory)] [string] $Id
    )
    $check = Get-LiveVerifyCheck -Run $Run -Id $Id
    $check.state = 'PENDING'
    $check.startedUtc = $null
    $check.finishedUtc = $null
    $check.message = $null
    $check.interrupted = $false
    $check.skipReason = $null
    $check.evidence = @()
    Save-LiveVerifyRun -Run $Run
    return $check
}

function Resolve-LiveVerifyInterrupted {
    <#
    .SYNOPSIS
        Turns every stranded RUNNING into UNVERIFIED. Never into PASS.
    .OUTPUTS
        The ids that were stranded, so `resume` can report them rather than
        quietly fixing them up.
    #>
    [CmdletBinding()]
    param([Parameter(Mandatory)] $Run)

    $stranded = @()
    foreach ($property in $Run.State.checks.PSObject.Properties) {
        $check = $property.Value
        if ($check.state -eq 'RUNNING') {
            $check.state = 'UNVERIFIED'
            $check.interrupted = $true
            $check.finishedUtc = [DateTime]::UtcNow.ToString('o')
            $check.message = 'Interrupted while running; the outcome is unknown'
            $stranded += $check.id
        }
    }
    if ($stranded.Count -gt 0) { Save-LiveVerifyRun -Run $Run }
    # `,` so a single stranded id stays an array; PowerShell unrolls a one-element
    # result otherwise and every caller's `.Count` then reads a string length.
    return , [string[]]$stranded
}

function Update-LiveVerifyStaleness {
    <#
    .SYNOPSIS
        Marks PASSes STALE when the artifact or the environment they depended on
        has changed.
    .DESCRIPTION
        Per check, not globally: a monitor rearrangement invalidates the
        cross-monitor Preview check and nothing about updater identity. The
        dependency is declared in the catalog (EnvironmentKeys / ArtifactBound),
        so "which checks does this change invalidate" is data, not a judgement
        call made at 2 a.m.
    .OUTPUTS
        The ids that became stale.
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] $Run,
        [Parameter(Mandatory)] [object[]] $Catalog,
        # The artifact's authoritative digest, computed once when the artifact was
        # resolved and carried in artifact-fingerprint.json. Deliberately a string
        # rather than the property bag: ConvertFrom-Json reinterprets an ISO-8601
        # value as a [DateTime], whose string form is locale-dependent, so hashing
        # a reloaded bag produced a different digest than hashing the original and
        # every reload looked like a rebuild.
        [Parameter(Mandatory)] [string] $ArtifactFingerprint,
        [Parameter(Mandatory)] [hashtable] $Environment
    )

    $artifactFingerprint = $ArtifactFingerprint
    # Every machine-produced terminal result, not only PASS. A FAIL against a
    # superseded binary is just as wrong as a PASS against one: leaving it in
    # place means the fix that turned it green is never observed. SKIPPED and
    # MANUAL_REQUIRED are human decisions and are left alone -- a skip carries a
    # reason, and an unperformed gate is re-offered by the runnable set instead.
    $invalidatable = @('PASS', 'FAIL', 'BLOCKED', 'UNVERIFIED')
    $stale = @()
    foreach ($entry in $Catalog) {
        if ($Run.State.checks.PSObject.Properties.Name -notcontains $entry.Id) { continue }
        $check = $Run.State.checks.$($entry.Id)
        if ($check.state -notin $invalidatable) { continue }

        $environmentFingerprint = Get-LiveVerifyEnvironmentFingerprint -Entry $entry -Environment $Environment
        $artifactChanged = $entry.ArtifactBound -and $check.artifactFingerprint -ne $artifactFingerprint
        $environmentChanged = $check.environmentFingerprint -ne $environmentFingerprint
        if ($artifactChanged -or $environmentChanged) {
            $previous = $check.state
            $check.state = 'STALE'
            $check.message = if ($artifactChanged) {
                "The tested artifact changed; the previous $previous does not describe the current binary"
            }
            else {
                "An environment property this check depends on changed; the previous $previous no longer applies"
            }
            $stale += $check.id
        }
    }
    if ($stale.Count -gt 0) { Save-LiveVerifyRun -Run $Run }
    return , [string[]]$stale
}

function Get-LiveVerifyEnvironmentFingerprint {
    <#
    .SYNOPSIS
        Fingerprint over only the environment keys a check declared.
    #>
    param(
        [Parameter(Mandatory)] $Entry,
        [Parameter(Mandatory)] [hashtable] $Environment
    )
    $keys = @()
    if ($Entry.PSObject.Properties.Name -contains 'EnvironmentKeys' -and $null -ne $Entry.EnvironmentKeys) {
        $keys = @($Entry.EnvironmentKeys)
    }
    if ($keys.Count -eq 0) { return 'none' }
    $subset = @{}
    foreach ($key in $keys) {
        $subset[$key] = if ($Environment.ContainsKey($key)) { $Environment[$key] } else { '<absent>' }
    }
    return Get-LiveVerifyFingerprint -Properties $subset
}

function Get-LiveVerifyRunnableChecks {
    <#
    .SYNOPSIS
        Checks a `run`/`resume` should attempt: never-run, stale, interrupted, or
        an unperformed human gate. A PASS bound to the current artifact is not
        rerun -- that is what makes resume worth having. A FAIL bound to the
        current artifact is not rerun either: it is a finding, and `retry` is the
        explicit way to re-attempt it after a change.
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] $Run,
        [Parameter(Mandatory)] [object[]] $Catalog
    )
    $runnable = @()
    foreach ($entry in $Catalog) {
        if ($Run.State.checks.PSObject.Properties.Name -notcontains $entry.Id) { continue }
        $state = $Run.State.checks.$($entry.Id).state
        if ($state -in @('PENDING', 'STALE', 'UNVERIFIED', 'RUNNING', 'MANUAL_REQUIRED')) { $runnable += $entry }
    }
    return , [object[]]$runnable
}

function Get-LiveVerifySummary {
    param([Parameter(Mandatory)] $Run)
    $summary = [ordered]@{}
    foreach ($state in $script:CheckStates) { $summary[$state] = 0 }
    foreach ($property in $Run.State.checks.PSObject.Properties) {
        $summary[$property.Value.state] = [int]$summary[$property.Value.state] + 1
    }
    return $summary
}

function Write-LiveVerifyReport {
    <#
    .SYNOPSIS
        Writes report.md, report.json and junit.xml.
    .DESCRIPTION
        Three formats because three readers: a person reviewing the run, a script
        gating on it, and CI. All three are generated from the same state, so
        they cannot disagree.
    #>
    [CmdletBinding()]
    param([Parameter(Mandatory)] $Run)

    $summary = Get-LiveVerifySummary -Run $Run
    $checks = @($Run.State.checks.PSObject.Properties | ForEach-Object { $_.Value })

    # --- JSON -------------------------------------------------------------
    $json = [ordered]@{
        runId       = $Run.Run.runId
        createdUtc  = $Run.Run.createdUtc
        reportedUtc = [DateTime]::UtcNow.ToString('o')
        artifact    = $Run.Artifact
        environment = $Run.Environment
        summary     = $summary
        checks      = $checks
    }
    Write-JsonAtomic -Path (Join-Path $Run.Directory 'report.json') -Value $json

    # --- Markdown ---------------------------------------------------------
    $lines = [System.Collections.Generic.List[string]]::new()
    $lines.Add("# Live Verify report - $($Run.Run.runId)")
    $lines.Add('')
    $lines.Add("Created: $($Run.Run.createdUtc)  ")
    $lines.Add("Reported: $([DateTime]::UtcNow.ToString('o'))")
    $lines.Add('')
    $lines.Add('## Result')
    $lines.Add('')
    $lines.Add('| State | Count |')
    $lines.Add('|---|---:|')
    foreach ($state in $script:CheckStates) {
        if ($summary[$state] -gt 0) { $lines.Add("| $state | $($summary[$state]) |") }
    }
    $lines.Add('')
    $lines.Add('## Artifact under test')
    $lines.Add('')
    if ($null -ne $Run.Artifact) {
        foreach ($property in $Run.Artifact.PSObject.Properties) {
            $lines.Add("- **$($property.Name)**: ``$($property.Value)``")
        }
    }
    $lines.Add('')
    foreach ($state in $script:CheckStates) {
        $group = @($checks | Where-Object { $_.state -eq $state })
        if ($group.Count -eq 0) { continue }
        $lines.Add("## $state")
        $lines.Add('')
        foreach ($check in $group) {
            $lines.Add("### $($check.id) - $($check.title)")
            $lines.Add('')
            $lines.Add("- Layer: $($check.layer)")
            $lines.Add("- Attempts: $($check.attempts)")
            if ($check.message) { $lines.Add("- Result: $($check.message)") }
            if ($check.skipReason) { $lines.Add("- Skip reason: $($check.skipReason)") }
            if ($check.interrupted) { $lines.Add('- Interrupted: yes') }
            if ($check.note) { $lines.Add("- Note: $($check.note)") }
            $evidence = @($check.evidence)
            if ($evidence.Count -gt 0) {
                $lines.Add('- Evidence:')
                foreach ($item in $evidence) { $lines.Add("  - ``$item``") }
            }
            elseif ($check.state -eq 'PASS') {
                $lines.Add('- Evidence: **none recorded** (an automated PASS without evidence is a defect in the check)')
            }
            $lines.Add('')
        }
    }
    Set-Content -LiteralPath (Join-Path $Run.Directory 'report.md') -Value ($lines -join "`n") -Encoding utf8NoBOM

    # --- JUnit ------------------------------------------------------------
    $failures = [int]$summary['FAIL']
    $skipped = [int]$summary['SKIPPED'] + [int]$summary['BLOCKED'] + [int]$summary['MANUAL_REQUIRED'] +
        [int]$summary['UNVERIFIED'] + [int]$summary['STALE'] + [int]$summary['PENDING']
    $xml = [System.Collections.Generic.List[string]]::new()
    $xml.Add('<?xml version="1.0" encoding="UTF-8"?>')
    $xml.Add("<testsuites name=`"live-verify`" tests=`"$($checks.Count)`" failures=`"$failures`" skipped=`"$skipped`">")
    $xml.Add("  <testsuite name=`"$([System.Security.SecurityElement]::Escape($Run.Run.runId))`" tests=`"$($checks.Count)`" failures=`"$failures`" skipped=`"$skipped`">")
    foreach ($check in $checks) {
        $name = [System.Security.SecurityElement]::Escape("$($check.id) $($check.title)")
        $message = [System.Security.SecurityElement]::Escape("$($check.message)$($check.skipReason)")
        $xml.Add("    <testcase classname=`"live-verify`" name=`"$name`">")
        switch ($check.state) {
            'PASS' { }
            'FAIL' { $xml.Add("      <failure message=`"$message`" />") }
            default { $xml.Add("      <skipped message=`"$($check.state): $message`" />") }
        }
        $xml.Add('    </testcase>')
    }
    $xml.Add('  </testsuite>')
    $xml.Add('</testsuites>')
    Set-Content -LiteralPath (Join-Path $Run.Directory 'junit.xml') -Value ($xml -join "`n") -Encoding utf8NoBOM

    return $summary
}

Export-ModuleMember -Function Get-LiveVerifyCheckStates, Write-JsonAtomic, Read-JsonFile,
    Get-LiveVerifyFingerprint, New-LiveVerifyRun, Get-LiveVerifyRun, Save-LiveVerifyRun,
    Get-LiveVerifyCheck, Set-LiveVerifyCheckRunning, Complete-LiveVerifyCheck,
    Set-LiveVerifyCheckSkipped, Add-LiveVerifyNote, Reset-LiveVerifyCheck,
    Resolve-LiveVerifyInterrupted, Update-LiveVerifyStaleness, Get-LiveVerifyEnvironmentFingerprint,
    Get-LiveVerifyRunnableChecks, Get-LiveVerifySummary, Write-LiveVerifyReport
