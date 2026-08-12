#requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory, Position = 0)]
    [ValidateSet('prepare', 'run', 'status', 'resume', 'retry', 'skip', 'note', 'report', 'list')]
    [string] $Command,

    [Parameter(Position = 1)]
    [string] $Argument,

    # prepare: which artifact the run is bound to.
    [ValidateSet('local', 'rc')]
    [string] $Artifact = 'local',
    [string] $ExePath,
    [string] $Tag,

    # run/resume/retry: restrict to these check ids.
    [string[]] $Only,

    # skip: mandatory reason. note: the note text (also -Argument).
    [string] $Reason,
    [string] $Text,

    # Reuse an existing run instead of the newest one.
    [string] $RunId,

    # Never prompt: manual gates resolve to MANUAL_REQUIRED instead of blocking.
    [switch] $NonInteractive
)

<#
.SYNOPSIS
    Resumable Live Verify acceptance orchestrator for ExoSnap.

.DESCRIPTION
    One runner, not several. It owns process lifecycle, check state, evidence and
    the bounded human gates, and it delegates every actual verification to the
    strongest mechanism that already exists (--hwnd-audit, --auto-record/
    --auto-edit, ffprobe, the Live Verify control channel, UI Automation).

    A run can be interrupted at any point without losing verified progress:
    `resume` re-reads the persisted state, converts anything stranded mid-check
    into UNVERIFIED (never PASS), re-checks artifact and environment binding, and
    continues.

.EXAMPLE
    pwsh scripts/live-verify.ps1 prepare -Artifact local
    pwsh scripts/live-verify.ps1 run
    pwsh scripts/live-verify.ps1 status
    pwsh scripts/live-verify.ps1 resume
    pwsh scripts/live-verify.ps1 retry LV-REC-001
    pwsh scripts/live-verify.ps1 skip LV-WIN-003 -Reason "single-monitor machine"
    pwsh scripts/live-verify.ps1 note LV-PREV-001 -Text "ran with the browser on display 2"
    pwsh scripts/live-verify.ps1 report
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$runsRoot = Join-Path $repositoryRoot '.workspace/live-verify'

Import-Module (Join-Path $PSScriptRoot 'lib/LiveVerifyClient.psm1') -Force
Import-Module (Join-Path $PSScriptRoot 'lib/LiveVerifyState.psm1') -Force
. (Join-Path $PSScriptRoot 'lib/LiveVerifyChecks.ps1')

function Write-Step([string] $message) { Write-Host "  $message" }
function Write-Heading([string] $message) { Write-Host "`n$message" -ForegroundColor Cyan }

# ---------------------------------------------------------------------------
# Artifact + environment
# ---------------------------------------------------------------------------

function Resolve-DefaultExe {
    # Release first: acceptance is about what users receive. The Debug tree is
    # the fallback so the infrastructure itself can be exercised without a
    # Release build sitting around.
    $candidates = @(
        'build/windows-x64-release/app/Release/exosnap.exe',
        'build/windows-x64-release/app/exosnap.exe',
        'build/windows-x64-debug/app/Debug/exosnap.exe',
        'build/windows-x64-debug/app/exosnap.exe'
    )
    foreach ($candidate in $candidates) {
        $path = Join-Path $repositoryRoot $candidate
        if (Test-Path -LiteralPath $path) { return $path }
    }
    return $null
}

function Get-ArtifactFingerprint {
    param([string] $Kind, [string] $Path, [string] $ReleaseTag)

    if ([string]::IsNullOrWhiteSpace($Path)) { $Path = Resolve-DefaultExe }
    if ([string]::IsNullOrWhiteSpace($Path) -or -not (Test-Path -LiteralPath $Path)) {
        throw 'No ExoSnap executable found. Build one, or pass -ExePath.'
    }
    $item = Get-Item -LiteralPath $Path
    $facts = @{
        kind           = $Kind
        tag            = $ReleaseTag
        exePath        = $item.FullName
        exeSha256      = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        exeBytes       = $item.Length
        productVersion = $item.VersionInfo.ProductVersion
        fileVersion    = $item.VersionInfo.FileVersion
        builtUtc       = $item.LastWriteTimeUtc.ToString('o')
    }
    # Computed once, here, and carried in the run directory. Recomputing it from
    # the reloaded JSON would not survive the round trip: ConvertFrom-Json turns
    # `builtUtc` back into a [DateTime] whose string form is locale-dependent, so
    # every reload looked like a rebuild and invalidated the whole run.
    $facts['fingerprint'] = Get-LiveVerifyFingerprint -Properties $facts
    return $facts
}

function Get-EnvironmentFacts {
    <#
    .SYNOPSIS
        The environment properties checks declare dependencies on.
    .DESCRIPTION
        Only what a check actually keys off, and nothing that identifies the
        person at the machine: no user name, no hostname, no file paths outside
        the repository. A run directory is evidence that gets read by others.
    #>
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

    try {
        $monitors = @(Get-CimInstance Win32_DesktopMonitor -ErrorAction Stop).Count
        $facts['monitorCount'] = $monitors
    }
    catch { $facts['monitorCount'] = 'unavailable' }

    Add-Type -AssemblyName System.Windows.Forms -ErrorAction SilentlyContinue
    try {
        $screens = @([System.Windows.Forms.Screen]::AllScreens |
                ForEach-Object { "$($_.DeviceName)=$($_.Bounds.Width)x$($_.Bounds.Height)@$($_.Bounds.X),$($_.Bounds.Y)" } |
                Sort-Object)
        $facts['monitorTopology'] = ($screens -join '|')
        $facts['primaryScreen'] = ([System.Windows.Forms.Screen]::PrimaryScreen).DeviceName
    }
    catch {
        $facts['monitorTopology'] = 'unavailable'
        $facts['primaryScreen'] = 'unavailable'
    }

    try {
        $facts['defaultAudioEndpoint'] = (Get-CimInstance Win32_SoundDevice -ErrorAction Stop |
                Select-Object -First 1 -ExpandProperty Name)
    }
    catch { $facts['defaultAudioEndpoint'] = 'unavailable' }

    try {
        $drive = Get-PSDrive -Name ((Get-Item $repositoryRoot).PSDrive.Name)
        $facts['freeGigabytes'] = [math]::Round($drive.Free / 1GB, 1)
    }
    catch { $facts['freeGigabytes'] = 'unavailable' }

    $probe = Get-Command ffprobe -ErrorAction SilentlyContinue
    $facts['ffprobeVersion'] = if ($null -ne $probe) {
        (& $probe.Source -version 2>&1 | Select-Object -First 1) -replace '\s+', ' '
    }
    else { 'absent' }

    # Everything as a string, so the value a check's fingerprint was computed
    # against survives the JSON round trip byte for byte.
    $stringified = [ordered]@{}
    foreach ($key in $facts.Keys) { $stringified[$key] = "$($facts[$key])" }
    $result = @{}
    foreach ($key in $stringified.Keys) { $result[$key] = $stringified[$key] }
    return $result
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
        if (-not (Test-Path -LiteralPath $path)) { throw "No run '$RunId' under $runsRoot" }
        return $path
    }
    $latest = Get-LatestRunDirectory
    if ($null -eq $latest) { throw "No Live Verify run yet. Start one with: live-verify.ps1 prepare" }
    return $latest
}

# ---------------------------------------------------------------------------
# Process lifecycle
# ---------------------------------------------------------------------------

$script:Session = $null

function Assert-NoForeignInstance {
    <#
    .SYNOPSIS
        Refuses to launch while another ExoSnap is running.
    .DESCRIPTION
        A verification launch is a NORMAL launch -- the single-instance guard is
        part of what is being accepted -- so a second instance would exit
        immediately and, worse, activate the running window, taking focus off
        whatever the developer is doing. Fail with an instruction instead.
    #>
    # Poll rather than sample once: a process the previous check just terminated
    # can still be visible for a moment, and "on its way out" is not a foreign
    # instance. Sampling once turned that into unrelated failures for every
    # check that followed.
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    do {
        $running = @(Get-Process -Name 'exosnap' -ErrorAction SilentlyContinue)
        if ($running.Count -eq 0) { return }
        Start-Sleep -Milliseconds 500
    } while ([DateTime]::UtcNow -lt $deadline)

    throw "ExoSnap is already running (PID $($running.Id -join ', ')). Close it first: a verification launch is a normal launch and would be refused by the single-instance guard."
}

function Start-LiveVerifySession {
    param([Parameter(Mandatory)] $Run)
    if ($null -ne $script:Session) { return $script:Session }

    Assert-NoForeignInstance
    $runId = New-LiveVerifyRunId
    $exe = $Run.Artifact.exePath
    Write-Step "launching $([IO.Path]::GetFileName($exe)) with the control channel armed"
    $process = Start-Process -FilePath $exe -ArgumentList @('--live-verify-control', $runId) -PassThru

    $connection = $null
    try {
        $connection = Connect-LiveVerify -RunId $runId -ConnectTimeoutMs 30000
    }
    catch {
        if (-not $process.HasExited) { $process | Stop-Process -Force -ErrorAction SilentlyContinue }
        throw
    }

    $script:Session = [pscustomobject]@{
        RunId      = $runId
        Process    = $process
        Connection = $connection
        StartedUtc = [DateTime]::UtcNow.ToString('o')
    }
    # Process identity is recorded, not assumed: "some exosnap.exe" is never the
    # right answer, especially once the updater starts replacing the binary.
    Write-JsonAtomic -Path (Join-Path $Run.Directory 'logs/process.json') -Value @{
        pid              = $process.Id
        exePath          = $exe
        exeSha256        = $Run.Artifact.exeSha256
        controlRunId     = $runId
        startedUtc       = $script:Session.StartedUtc
        handshakeIdentity = $connection.Identity
    }
    return $script:Session
}

function Stop-LiveVerifySession {
    if ($null -eq $script:Session) { return }
    $session = $script:Session
    $script:Session = $null
    try { $session.Connection.Close() } catch { }
    if (-not $session.Process.HasExited) {
        # CloseMainWindow first: the close-to-tray refusal and the close guards
        # are product behaviour, and killing would step over them.
        [void]$session.Process.CloseMainWindow()
        if (-not $session.Process.WaitForExit(8000)) {
            $session.Process | Stop-Process -Force -ErrorAction SilentlyContinue
        }
    }
    $session.Process.WaitForExit(5000) | Out-Null
}

# ---------------------------------------------------------------------------
# Manual gates
# ---------------------------------------------------------------------------

function Invoke-ManualGate {
    param([Parameter(Mandatory)] [hashtable] $Gate)
    if ($NonInteractive) { return $false }

    Write-Host ''
    Write-Host "MANUAL ACTION REQUIRED - $($Gate.Title)" -ForegroundColor Yellow
    Write-Host ''
    Write-Host 'Why:'
    Write-Host $Gate.Why
    Write-Host ''
    Write-Host 'Please do:'
    $index = 1
    foreach ($step in $Gate.Do) {
        Write-Host "$index. $step"
        $index++
    }
    Write-Host ''
    Write-Host 'Expected visible result:'
    Write-Host $Gate.Expected
    Write-Host ''
    $answer = Read-Host "Reply only with 'done' when complete (anything else defers this gate)"
    return ($answer.Trim().ToLowerInvariant() -eq 'done')
}

# ---------------------------------------------------------------------------
# Execution
# ---------------------------------------------------------------------------

function New-CheckContext {
    param([Parameter(Mandatory)] $Run)
    $context = [pscustomobject]@{
        RunDirectory  = $Run.Directory
        Artifact      = $Run.Artifact
        Environment   = $Run.Environment
        State         = @{}
        EnsureSession = { Start-LiveVerifySession -Run $Run }.GetNewClosure()
        EndSession    = { Stop-LiveVerifySession }
        ManualGate    = { param($gate) Invoke-ManualGate -Gate $gate }
    }
    return $context
}

function Invoke-Checks {
    param(
        [Parameter(Mandatory)] $Run,
        [Parameter(Mandatory)] [object[]] $Catalog,
        [Parameter(Mandatory)] [object[]] $Entries
    )

    $context = New-CheckContext -Run $Run
    $environmentHashtable = ConvertTo-Hashtable $Run.Environment
    $artifactFingerprint = $Run.Artifact.fingerprint

    try {
        foreach ($entry in $Entries) {
            $environmentFingerprint = Get-LiveVerifyEnvironmentFingerprint -Entry $entry `
                -Environment $environmentHashtable
            Write-Heading "$($entry.Id)  $($entry.Title)"
            Write-Step "layer $($entry.Layer)"

            # Persisted BEFORE execution. A kill between here and the terminal
            # write leaves RUNNING on disk, which resume turns into UNVERIFIED.
            Set-LiveVerifyCheckRunning -Run $Run -Id $entry.Id `
                -ArtifactFingerprint $artifactFingerprint `
                -EnvironmentFingerprint $environmentFingerprint | Out-Null

            $outcome = $null
            try {
                $outcome = & $entry.Run $context
            }
            catch {
                $outcome = @{ Result = 'FAIL'; Message = "Check threw: $($_.Exception.Message)" }
            }
            if ($null -eq $outcome) {
                $outcome = @{ Result = 'UNVERIFIED'; Message = 'The check returned nothing' }
            }

            $evidence = @()
            if ($outcome.ContainsKey('Evidence') -and $null -ne $outcome.Evidence) { $evidence = @($outcome.Evidence) }
            $message = if ($outcome.ContainsKey('Message')) { $outcome.Message } else { $null }
            Complete-LiveVerifyCheck -Run $Run -Id $entry.Id -Result $outcome.Result -Message $message `
                -Evidence $evidence | Out-Null

            $colour = switch ($outcome.Result) {
                'PASS' { 'Green' }
                'FAIL' { 'Red' }
                default { 'Yellow' }
            }
            Write-Host "  -> $($outcome.Result)  $message" -ForegroundColor $colour
        }
    }
    finally {
        Stop-LiveVerifySession
    }
}

function ConvertTo-Hashtable {
    param($Object)
    $table = @{}
    if ($null -eq $Object) { return $table }
    foreach ($property in $Object.PSObject.Properties) { $table[$property.Name] = $property.Value }
    return $table
}

function Show-Status {
    param([Parameter(Mandatory)] $Run)
    $summary = Get-LiveVerifySummary -Run $Run
    Write-Heading "Run $($Run.Run.runId)"
    Write-Host "  artifact : $($Run.Artifact.kind) $($Run.Artifact.productVersion)"
    Write-Host "  exe      : $($Run.Artifact.exePath)"
    Write-Host "  sha256   : $($Run.Artifact.exeSha256)"
    Write-Host ''
    foreach ($property in $Run.State.checks.PSObject.Properties) {
        $check = $property.Value
        $colour = switch ($check.state) {
            'PASS' { 'Green' }
            'FAIL' { 'Red' }
            'PENDING' { 'DarkGray' }
            default { 'Yellow' }
        }
        $detail = if ($check.message) { " - $($check.message)" } elseif ($check.skipReason) { " - $($check.skipReason)" } else { '' }
        Write-Host ("  {0,-14} {1,-16} {2}{3}" -f $check.id, $check.state, $check.title, $detail) -ForegroundColor $colour
    }
    Write-Host ''
    Write-Host ('  ' + (($summary.GetEnumerator() | Where-Object { $_.Value -gt 0 } |
                ForEach-Object { "$($_.Key)=$($_.Value)" }) -join '  '))
}

# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------

$catalog = Get-LiveVerifyCatalog

# PowerShell splits `-Only A,B` typed on a command line, but NOT a single string
# that happens to contain commas — which is what any caller passing the value
# through a variable ends up with. Unsplit, nothing matched and the run reported
# "Nothing to run." while looking like it had done its job.
if ($Only) { $Only = @($Only | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ }) }

switch ($Command) {
    'prepare' {
        # NOTE: never assign to $artifact/$environment here — PowerShell variable
        # names are case-insensitive, so that would overwrite the validated
        # -Artifact parameter with a hashtable.
        $artifactFacts = Get-ArtifactFingerprint -Kind $Artifact -Path $ExePath -ReleaseTag $Tag
        $environmentFacts = Get-EnvironmentFacts
        $newRunId = "{0:yyyyMMdd-HHmmss}-{1}" -f [DateTime]::Now, ($artifactFacts.exeSha256.Substring(0, 8))
        $runDirectory = Join-Path $runsRoot $newRunId
        New-LiveVerifyRun -RunDirectory $runDirectory -RunId $newRunId -Catalog $catalog `
            -Artifact $artifactFacts -Environment $environmentFacts | Out-Null
        Write-Heading "Prepared run $newRunId"
        Write-Step "artifact  $($artifactFacts.kind) $($artifactFacts.productVersion) ($($artifactFacts.exeSha256.Substring(0,16))…)"
        Write-Step "exe       $($artifactFacts.exePath)"
        Write-Step "checks    $($catalog.Count)"
        Write-Step "directory $runDirectory"
        if ($artifactFacts.kind -eq 'local') {
            Write-Host ''
            Write-Host '  NOTE: a local build validates the infrastructure. Official acceptance needs a published, immutable RC.' -ForegroundColor Yellow
        }
    }

    'run' {
        $run = Get-LiveVerifyRun -RunDirectory (Resolve-RunDirectory)
        # @(...) around the whole conditional: an `if` whose branch produces
        # nothing yields $null, and $null.Count is a hard error under StrictMode —
        # so a -Only naming an id that does not exist crashed instead of saying
        # there was nothing to run.
        $entries = @(if ($Only) { $catalog | Where-Object { $Only -contains $_.Id } }
            else { Get-LiveVerifyRunnableChecks -Run $run -Catalog $catalog })
        if ($entries.Count -eq 0) { Write-Host 'Nothing to run.'; break }
        Invoke-Checks -Run $run -Catalog $catalog -Entries $entries
        Write-LiveVerifyReport -Run (Get-LiveVerifyRun -RunDirectory $run.Directory) | Out-Null
        Show-Status -Run (Get-LiveVerifyRun -RunDirectory $run.Directory)
    }

    'resume' {
        $run = Get-LiveVerifyRun -RunDirectory (Resolve-RunDirectory)
        $stranded = Resolve-LiveVerifyInterrupted -Run $run
        if ($stranded.Count -gt 0) {
            Write-Host "Interrupted while running, now UNVERIFIED (not PASS): $($stranded -join ', ')" -ForegroundColor Yellow
        }
        # Re-fingerprint: the binary may have been rebuilt and the desk may have
        # been rearranged since the run started.
        $artifactFacts = Get-ArtifactFingerprint -Kind $run.Artifact.kind -Path $run.Artifact.exePath -ReleaseTag $run.Artifact.tag
        $environmentFacts = Get-EnvironmentFacts
        $stale = Update-LiveVerifyStaleness -Run $run -Catalog $catalog `
            -ArtifactFingerprint $artifactFacts.fingerprint -Environment $environmentFacts
        if ($stale.Count -gt 0) {
            Write-Host "Artifact or environment changed; previously passing checks are now STALE: $($stale -join ', ')" -ForegroundColor Yellow
            Write-JsonAtomic -Path (Join-Path $run.Directory 'artifact-fingerprint.json') -Value $artifactFacts
            Write-JsonAtomic -Path (Join-Path $run.Directory 'environment.json') -Value $environmentFacts
            $run = Get-LiveVerifyRun -RunDirectory $run.Directory
        }
        $entries = @(if ($Only) { $catalog | Where-Object { $Only -contains $_.Id } }
            else { Get-LiveVerifyRunnableChecks -Run $run -Catalog $catalog })
        if ($entries.Count -eq 0) { Write-Host 'Nothing left to run.'; Show-Status -Run $run; break }
        if ($Only) {
            # -Only is an explicit selection and deliberately reruns whatever it
            # names, PASS included. Saying "already passed and will not be rerun"
            # here would be contradicted by the very next lines.
            Write-Host "Resuming with $($entries.Count) explicitly selected check(s); every one of them is rerun."
        }
        else {
            Write-Host "Resuming with $($entries.Count) check(s); $(@($run.State.checks.PSObject.Properties | Where-Object { $_.Value.state -eq 'PASS' }).Count) already passed and will not be rerun."
        }
        Invoke-Checks -Run $run -Catalog $catalog -Entries $entries
        Write-LiveVerifyReport -Run (Get-LiveVerifyRun -RunDirectory $run.Directory) | Out-Null
        Show-Status -Run (Get-LiveVerifyRun -RunDirectory $run.Directory)
    }

    'retry' {
        if ([string]::IsNullOrWhiteSpace($Argument)) { throw 'retry needs a check id' }
        $run = Get-LiveVerifyRun -RunDirectory (Resolve-RunDirectory)
        Reset-LiveVerifyCheck -Run $run -Id $Argument | Out-Null
        $entries = @($catalog | Where-Object { $_.Id -eq $Argument })
        if ($entries.Count -eq 0) { throw "Unknown check id '$Argument'" }
        Invoke-Checks -Run $run -Catalog $catalog -Entries $entries
        Write-LiveVerifyReport -Run (Get-LiveVerifyRun -RunDirectory $run.Directory) | Out-Null
        Show-Status -Run (Get-LiveVerifyRun -RunDirectory $run.Directory)
    }

    'skip' {
        if ([string]::IsNullOrWhiteSpace($Argument)) { throw 'skip needs a check id' }
        if ([string]::IsNullOrWhiteSpace($Reason)) { throw 'skip needs -Reason' }
        $run = Get-LiveVerifyRun -RunDirectory (Resolve-RunDirectory)
        Set-LiveVerifyCheckSkipped -Run $run -Id $Argument -Reason $Reason | Out-Null
        Write-Host "$Argument SKIPPED: $Reason"
    }

    'note' {
        if ([string]::IsNullOrWhiteSpace($Argument)) { throw 'note needs a check id' }
        if ([string]::IsNullOrWhiteSpace($Text)) { throw 'note needs -Text' }
        $run = Get-LiveVerifyRun -RunDirectory (Resolve-RunDirectory)
        Add-LiveVerifyNote -Run $run -Id $Argument -Note $Text | Out-Null
        Write-Host "$Argument noted."
    }

    'status' {
        Show-Status -Run (Get-LiveVerifyRun -RunDirectory (Resolve-RunDirectory))
    }

    'report' {
        $run = Get-LiveVerifyRun -RunDirectory (Resolve-RunDirectory)
        $summary = Write-LiveVerifyReport -Run $run
        Write-Heading "Report written to $($run.Directory)"
        Write-Step 'report.md, report.json, junit.xml'
        Write-Host ''
        Write-Host ('  ' + (($summary.GetEnumerator() | Where-Object { $_.Value -gt 0 } |
                    ForEach-Object { "$($_.Key)=$($_.Value)" }) -join '  '))
        # A run with a FAIL is a failed acceptance; the exit code says so for CI.
        if ($summary['FAIL'] -gt 0) { exit 1 }
    }

    'list' {
        foreach ($entry in $catalog) {
            Write-Host ("{0,-14} {1,-16} {2}" -f $entry.Id, $entry.Layer, $entry.Title)
            Write-Host ("{0,-14} {1}" -f '', "source: $($entry.Source)") -ForegroundColor DarkGray
        }
    }
}
