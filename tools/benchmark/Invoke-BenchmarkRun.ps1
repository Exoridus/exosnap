<#
.SYNOPSIS
    One accepted (or calibration) run of the Widgets-vs-Quick frontend benchmark.

.DESCRIPTION
    Development tooling. Nothing here belongs in the shipped application: the
    application knows how to record and how to report on itself, and knows
    nothing about Superposition, run identities or artifact layouts. That split
    is deliberate — QuickApplication must never learn to launch a benchmark.

    The run is:

        verify display topology
        -> start the external workload (when the scenario has one)
        -> let the workload warm up
        -> start the ExoSnap frontend with --auto-record
        -> the app's own warm-up/measure/stop sequence runs inside that
        -> stop the workload
        -> collect artifacts into one run directory

    The ExoSnap measurement window is owned entirely by the application
    (--benchmark-warmup + --duration), so nothing here has to guess when the
    measured interval opened.

.NOTES
    Requires a Release build configured with EXOSNAP_BUILD_BENCHMARK_HARNESS=ON
    for an ACCEPTED run. A Debug binary is allowed only with -Calibration, and
    the run directory is then marked as such.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('widgets', 'quick')][string]$Frontend,
    [Parameter(Mandatory)][string]$Scenario,
    [int]$RunIndex = 1,
    [string]$OutputRoot = (Join-Path $PSScriptRoot '..\..\.workspace\benchmark-results'),
    [string]$WidgetsExe,
    [string]$QuickExe,
    [string]$SuperpositionCli = 'C:\Program Files\Unigine\Superposition Benchmark\bin\superposition_cli.exe',
    # A disposable validation run: allowed to use a Debug binary, never counted
    # in the campaign statistics, and written into a separate tree.
    [switch]$Calibration,
    # Escape hatch for working on the tooling on a different machine. An accepted
    # run must never use it; the run manifest records that it was used.
    [switch]$SkipTopologyCheck
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Import-Module (Join-Path $PSScriptRoot 'BenchmarkTopology.psm1') -Force

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$scenarioPath = Join-Path $PSScriptRoot "scenarios\$Scenario.json"
if (-not (Test-Path $scenarioPath)) {
    throw "Unknown scenario '$Scenario'. Expected a definition at $scenarioPath."
}
$definition = Get-Content -Raw -Path $scenarioPath | ConvertFrom-Json

# `exosnap` is the shipping Qt Quick application (ADR 0064) and owns
# app/<config>/exosnap.exe. There is no Widgets executable any more: the frontend
# A/B campaign it existed for is complete, its results are archived under
# .workspace/benchmark-results*/, and the frontend itself was removed with the
# cutover. -Frontend widgets therefore fails with an explanation rather than
# silently measuring the Quick binary and labelling the report "widgets".
if (-not $QuickExe) {
    $QuickExe = Join-Path $repoRoot 'build\windows-x64-release-bench\app\Release\exosnap.exe'
}
if ($Frontend -eq 'widgets' -and -not $WidgetsExe) {
    throw ("The Qt Widgets frontend was removed with the Qt Quick cutover (ADR 0064), so there is no " +
           "executable to measure. The archived A/B results under .workspace/benchmark-results*/ are the " +
           "record of that comparison. To re-run it, check out the pre-cutover checkpoint and build there, " +
           "or pass -WidgetsExe with a binary you built yourself.")
}
$exe = if ($Frontend -eq 'widgets') { $WidgetsExe } else { $QuickExe }
if (-not (Test-Path $exe)) {
    throw "$Frontend executable not found at $exe. Build it with EXOSNAP_BUILD_BENCHMARK_HARNESS=ON first."
}

# Refuse a binary that does not contain the harness.
#
# A shipping Release build does not know --auto-record. Handed one it does not
# fail: it ignores the unknown switch, starts as the ORDINARY application,
# persists into the user's real configuration directory (the isolation is itself
# inside the harness guard), records nothing, and never exits. Two campaign
# attempts died exactly that way, and from the outside it looked like a
# mysterious hang rather than the wrong executable. Fail loudly instead.
#
# The marker is the option string itself, in UTF-16 as Qt stores literals.
$harnessMarker = [System.Text.Encoding]::Unicode.GetBytes('--benchmark-scenario')
$exeBytes = [System.IO.File]::ReadAllBytes($exe)
$markerFound = $false
$limit = $exeBytes.Length - $harnessMarker.Length
for ($i = 0; $i -le $limit -and -not $markerFound; $i++) {
    if ($exeBytes[$i] -ne $harnessMarker[0]) { continue }
    $match = $true
    for ($j = 1; $j -lt $harnessMarker.Length; $j++) {
        if ($exeBytes[$i + $j] -ne $harnessMarker[$j]) { $match = $false; break }
    }
    $markerFound = $match
}
$exeBytes = $null
if (-not $markerFound) {
    throw ("$exe was built WITHOUT the benchmark harness. Reconfigure with " +
           '-DEXOSNAP_BUILD_BENCHMARK_HARNESS=ON and rebuild. A shipping build silently ' +
           'starts as the normal application and writes to the real user configuration.')
}

# ---------------------------------------------------------------------------
# Topology
# ---------------------------------------------------------------------------
$topologyResult = $null
if ($SkipTopologyCheck) {
    Write-Warning 'Topology verification skipped. This run cannot be accepted into a campaign.'
} else {
    $topologyResult = Test-BenchmarkTopology -Expected $definition.topology
    foreach ($problem in $topologyResult.Problems) { Write-Warning $problem }
    if (-not $topologyResult.Ok) {
        throw 'Display topology does not match the scenario. Refusing to benchmark the wrong monitor.'
    }
    # Model names are reported for the machine as a whole, never per screen: see
    # the ordering note in BenchmarkTopology.psm1.
    Write-Host ("Attached panels: {0}" -f ($topologyResult.AttachedPanels -join ', '))
    Write-Host ("Capture display: {0} {1}x{2}@{3}Hz (primary)" -f `
        $topologyResult.CaptureDisplay.DeviceName, $topologyResult.CaptureDisplay.Width, `
        $topologyResult.CaptureDisplay.Height, $topologyResult.CaptureDisplay.RefreshHz)
    Write-Host ("UI display:      {0} {1}x{2}@{3}Hz" -f `
        $topologyResult.UiDisplay.DeviceName, $topologyResult.UiDisplay.Width, `
        $topologyResult.UiDisplay.Height, $topologyResult.UiDisplay.RefreshHz)
}

# ---------------------------------------------------------------------------
# Run directory
# ---------------------------------------------------------------------------
$runId = '{0}-run{1:d2}' -f $Frontend, $RunIndex
$scenarioRoot = Join-Path $OutputRoot $(if ($Calibration) { "calibration\$($definition.id)" } else { $definition.id })
$runDir = Join-Path $scenarioRoot $runId
New-Item -ItemType Directory -Force -Path $runDir | Out-Null
Write-Host "Run directory: $runDir"

# ---------------------------------------------------------------------------
# External workload
# ---------------------------------------------------------------------------
$workloadProcess = $null
$workloadArgs = @()
if ($definition.workload.kind -eq 'superposition') {
    if (-not (Test-Path $SuperpositionCli)) {
        throw "Superposition CLI not found at $SuperpositionCli."
    }
    $cli = $definition.workload.cli
    $workloadArgs = @(
        '-api', $cli.api,
        '-resolution', $cli.resolution,
        '-fullscreen', $cli.fullscreen,
        '-quality', $cli.quality,
        '-textures', $cli.textures,
        '-dof', $cli.dof,
        '-motion_blur', $cli.motion_blur,
        '-sound', $cli.sound,
        '-iterations', 1
    )
    if ($cli.mode -eq 'scene') {
        $workloadArgs += @('-mode', 'scene', $cli.scene, '-mode_duration', $cli.mode_duration_minutes)
    } elseif ($cli.mode -eq 'frame') {
        $workloadArgs += @('-mode', 'frame', $cli.frame, '-mode_duration', $cli.mode_duration_minutes)
    } else {
        $workloadArgs += @('-mode', 'default')
    }
    # Never the default report directory: the reports have to land beside the
    # ExoSnap artifacts of the same run, or the archive stops being self-describing.
    $workloadArgs += @(
        '-log_csv', (Join-Path $runDir 'superposition.csv'),
        '-log_csv_step', 0,
        '-log_txt', (Join-Path $runDir 'superposition.txt')
    )

    Write-Host 'Starting Superposition...'
    $workloadProcess = Start-Process -FilePath $SuperpositionCli -ArgumentList $workloadArgs -PassThru
    Start-Sleep -Seconds $definition.workload.warmup_seconds
    if ($workloadProcess.HasExited) {
        throw "Superposition exited during warm-up with code $($workloadProcess.ExitCode)."
    }
}

# ---------------------------------------------------------------------------
# ExoSnap
# ---------------------------------------------------------------------------
$settings = $definition.exosnap
$exoArgs = @(
    '--auto-record',
    '--enable-preview',
    '--target', $settings.target,
    '--duration', $settings.duration_seconds,
    '--frame-rate', $settings.frame_rate,
    '--container', $settings.container,
    '--video-codec', $settings.video_codec,
    '--audio-codec', $settings.audio_codec,
    '--chroma', $settings.chroma,
    '--bit-depth', $settings.bit_depth,
    '--hdr', $settings.hdr_mode,
    '--benchmark-scenario', $definition.id,
    '--benchmark-output', $runDir,
    '--benchmark-warmup', $settings.warmup_seconds
)
if ($settings.audio_rows -and $settings.audio_rows.Count -gt 0) {
    $exoArgs += @('--audio-rows', ($settings.audio_rows -join ','))
}
if ($settings.target -eq 'window' -and $settings.target_window_title) {
    $exoArgs += @('--target-window-title', $settings.target_window_title)
}
if ($definition.source_notes) {
    $exoArgs += @('--benchmark-notes', $definition.source_notes)
}

# Recordings never land in the user's real output folder.
$env:EXOSNAP_OUTPUT_DIR = $runDir

$stdoutPath = Join-Path $runDir 'exosnap.stdout.log'
$stderrPath = Join-Path $runDir 'exosnap.stderr.log'
Write-Host "Starting ExoSnap ($Frontend)..."
$exoProcess = Start-Process -FilePath $exe -ArgumentList $exoArgs -PassThru `
    -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath

# Where the frontend window actually landed, sampled once while the run is live.
#
# The placement rule lives in the binaries (benchmark::ResolveHarnessWindowPlacement),
# so both frontends are placed identically by construction — but "identically" and
# "on the secondary display" are different claims, and the campaign gate asserts the
# second one. Read-only: the window handle is resolved from our own child process id
# and only its rectangle is queried. No input is synthesised and no focus is taken.
$windowPlacement = $null
$deadline = (Get-Date).AddSeconds(30)
while ((Get-Date) -lt $deadline -and -not $exoProcess.HasExited) {
    $exoProcess.Refresh()
    if ($exoProcess.MainWindowHandle -ne [IntPtr]::Zero) {
        $bounds = [System.Windows.Forms.Screen]::FromHandle($exoProcess.MainWindowHandle)
        $windowPlacement = [pscustomobject]@{
            ScreenDeviceName = $bounds.DeviceName
            ScreenPrimary    = $bounds.Primary
            ScreenBounds     = $bounds.Bounds.ToString()
        }
        break
    }
    Start-Sleep -Milliseconds 500
}
if ($windowPlacement) {
    Write-Host ("ExoSnap window is on {0} (primary: {1})" -f `
        $windowPlacement.ScreenDeviceName, $windowPlacement.ScreenPrimary)
    if ($windowPlacement.ScreenPrimary -and -not $SkipTopologyCheck) {
        Write-Warning 'The frontend window is on the CAPTURE display. It is inside the image being recorded.'
    }
} elseif (-not $exoProcess.HasExited) {
    Write-Warning 'Could not resolve the ExoSnap window; its display cannot be confirmed for this run.'
}

# Bounded wait. The application already owns a grace timer around its own drive
# loop, so a run that outlives warm-up + duration + this margin is wedged, not
# slow. One such hang has been observed (a process with a window that never
# reached logging init and never exited); it did not reproduce, but an
# unreproducible hang must still not be able to stall a campaign overnight.
$exoBudget = [int]$settings.warmup_seconds + [int]$settings.duration_seconds + 180
$exoTimedOut = $false
if (-not $exoProcess.WaitForExit($exoBudget * 1000)) {
    $exoTimedOut = $true
    Write-Warning "ExoSnap did not exit within ${exoBudget}s. Killing it; this run cannot be accepted."
    try { $exoProcess.Kill() } catch { }
    $exoProcess.WaitForExit(15000) | Out-Null
}
$exoExitCode = if ($exoTimedOut) { -1 } else { $exoProcess.ExitCode }
Write-Host "ExoSnap exited with $exoExitCode."

# ---------------------------------------------------------------------------
# Stop the workload
# ---------------------------------------------------------------------------
if ($workloadProcess -and -not $workloadProcess.HasExited) {
    # Let the run reach its own -mode_duration end rather than closing it early.
    #
    # Superposition writes both reports during shutdown, and the calibration run
    # showed CloseMainWindow needs well over 20 s to get there — the previous
    # 20 s budget expired and killed a process that was still flushing. The
    # reports survived that time; relying on it would be relying on luck.
    # mode_duration bounds the wait, so this cannot hang the campaign.
    $remaining = [int]$definition.workload.cli.mode_duration_minutes * 60 + 90
    Write-Host "Waiting up to ${remaining}s for Superposition to finish and write its reports..."
    if (-not $workloadProcess.WaitForExit($remaining * 1000)) {
        [void]$workloadProcess.CloseMainWindow()
        if (-not $workloadProcess.WaitForExit(60000)) {
            Write-Warning 'Superposition would not exit; killing it. Its reports may be truncated.'
            $workloadProcess.Kill()
            $workloadProcess.WaitForExit(10000) | Out-Null
        }
    }
}

# ---------------------------------------------------------------------------
# Manifest
# ---------------------------------------------------------------------------
$reportFile = Get-ChildItem -Path $runDir -Filter '*.json' -File |
    Where-Object { $_.Name -ne 'run.json' } | Select-Object -First 1
$recording = Get-ChildItem -Path $runDir -Include '*.mkv', '*.mp4', '*.webm' -File -Recurse -ErrorAction SilentlyContinue |
    Select-Object -First 1

$manifest = [ordered]@{
    run_id                = $runId
    frontend              = $Frontend
    scenario              = $definition.id
    run_index             = $RunIndex
    calibration           = [bool]$Calibration
    topology_verified     = (-not $SkipTopologyCheck)
    executable            = $exe
    exosnap_args          = $exoArgs
    exosnap_exit_code     = $exoExitCode
    workload_kind         = $definition.workload.kind
    workload_args         = $workloadArgs
    exosnap_report        = if ($reportFile) { $reportFile.Name } else { $null }
    recording             = if ($recording) { $recording.Name } else { $null }
    superposition_csv     = if (Test-Path (Join-Path $runDir 'superposition.csv')) { 'superposition.csv' } else { $null }
    superposition_txt     = if (Test-Path (Join-Path $runDir 'superposition.txt')) { 'superposition.txt' } else { $null }
    display_topology      = if ($topologyResult) { $topologyResult.All } else { $null }
    attached_panels       = if ($topologyResult) { $topologyResult.AttachedPanels } else { $null }
    frontend_window       = $windowPlacement
}
$manifest | ConvertTo-Json -Depth 8 | Set-Content -Path (Join-Path $runDir 'run.json') -Encoding UTF8
Copy-Item -Path $scenarioPath -Destination (Join-Path $runDir 'scenario.json') -Force

# ---------------------------------------------------------------------------
# Acceptance
# ---------------------------------------------------------------------------
$missing = @()
if (-not $reportFile) { $missing += 'ExoSnap benchmark JSON' }
if ($definition.workload.kind -eq 'superposition') {
    if (-not (Test-Path (Join-Path $runDir 'superposition.csv'))) { $missing += 'Superposition CSV' }
    if (-not (Test-Path (Join-Path $runDir 'superposition.txt'))) { $missing += 'Superposition TXT' }
}
if ($exoExitCode -ne 0) { $missing += "ExoSnap exit code $exoExitCode" }
if (-not $SkipTopologyCheck) {
    if (-not $windowPlacement) {
        $missing += 'frontend window display could not be confirmed'
    } elseif ($windowPlacement.ScreenPrimary) {
        $missing += 'frontend window was on the capture display'
    }
}

if ($missing.Count -gt 0) {
    # Throw rather than `exit 1`: a caller that dot-invokes this script cannot see
    # an exit code reliably ($LASTEXITCODE is only set by native commands), and a
    # silently-ignored failure is how an unbalanced campaign gets built.
    throw ("Run is NOT acceptable: " + ($missing -join '; '))
}
Write-Host "Run complete: $runDir"
