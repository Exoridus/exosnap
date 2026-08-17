#requires -Version 7.0
[CmdletBinding()]
param(
    # The exosnap.exe under test.
    [Parameter(Mandatory)]
    [string] $AppPath,

    # How long the recording runs. Long enough for the pipeline to leave warm-up
    # and publish real numbers; short enough that the check is not a soak.
    [int] $RecordSeconds = 6,

    # Also run the export. Off by default because a remux is minutes on a long
    # clip and this journey is about coverage, not throughput.
    [switch] $RequireExport,

    [string] $EvidencePath,

    [int] $TimeoutSeconds = 90
)

<#
.SYNOPSIS
    One coherent product journey across every surface Wave C made observable and
    controllable, driven entirely through the semantic control channel.

.DESCRIPTION
        identity            -> the exact artifact, by SHA-256
        settings.snapshot   -> requested / effective / running
        environment.snapshot-> what the machine is
        diagnostics.results -> the structured checklist, tiers intact
        windows.snapshot    -> every native window with its ROLE
        record.selectTarget -> a monitor, by product semantics
        record.start        -> recording
        pipeline.snapshot   -> the live pipeline, valid and measured
        record.addMarker
        record.pause        -> pipeline lifecycle follows
        record.resume
        record.stop         -> completed
        record.result       -> the file the product says it wrote
        session.latest      -> the canonical report for that recording
        events.recent       -> the recording correlated by its own session id
        edit.open / seek / trim / close
        export.start        -> optional, with -RequireExport

    This drives the product through its own edges only: every command below is
    bound to the same intent a click reaches. There is no window automation and
    no synthetic input anywhere in it.

    It RECORDS. Output goes to an isolated EXOSNAP_OUTPUT_DIR under the system
    temp directory and is removed with the run; the configuration directory is
    isolated the same way, so the developer's own settings are never read or
    written.

    No Start-Sleep as a synchronisation mechanism. Waits are blocking pipe
    connects, stateRevision advances delivered as events, or process handle
    waits. The one bounded timer is the recording DURATION itself, which is a
    product parameter rather than a guess about how long something takes.

.EXAMPLE
    ./live-verify-product-journey.ps1 -AppPath build/windows-x64-debug/app/exosnap.exe
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Import-Module (Join-Path $PSScriptRoot 'lib/LiveVerifyClient.psm1') -Force

$steps = [System.Collections.Generic.List[object]]::new()
function Add-Step([string] $Name, [bool] $Pass, [object] $Detail) {
    $steps.Add([ordered]@{ step = $Name; pass = $Pass; detail = $Detail })
    $status = if ($Pass) { 'PASS' } else { 'FAIL' }
    Write-Host ("{0,-4} {1}" -f $status, $Name)
    if (-not $Pass) { Write-Host ("     {0}" -f ($Detail | ConvertTo-Json -Compress -Depth 6)) }
}

function Invoke-Query($Connection, [string] $Command, [hashtable] $Parameters) {
    $response = Invoke-LiveVerifyCommand -Connection $Connection -Command $Command -Parameters $Parameters
    if (-not $response.ok) { throw "$Command refused: $($response.error.code) - $($response.error.message)" }
    return $response.result
}

# Waits for the recording state to become one of `Wanted`, using stateRevision
# advances rather than a clock. A revision only moves when the OBSERVABLE product
# state differs, so this cannot be satisfied by an elapsed-time tick.
function Wait-RecordingState($Connection, [string[]] $Wanted, [int] $TimeoutMs) {
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    $state = Get-LiveVerifyState -Connection $Connection
    while ($Wanted -notcontains $state.recordingState) {
        $remaining = [int]([Math]::Max(1, ($deadline - [DateTime]::UtcNow).TotalMilliseconds))
        if ($remaining -le 1) { throw "Timed out waiting for $($Wanted -join '|'); still $($state.recordingState)" }
        [void](Wait-LiveVerifyRevision -Connection $Connection -After $state.stateRevision -TimeoutMs $remaining)
        $state = Get-LiveVerifyState -Connection $Connection
    }
    return $state
}

$appFull = (Resolve-Path -LiteralPath $AppPath).Path
$appSha = (Get-FileHash -LiteralPath $appFull -Algorithm SHA256).Hash.ToLowerInvariant()

$scratch = Join-Path ([System.IO.Path]::GetTempPath()) ("exosnap-journey-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $scratch -Force | Out-Null
$env:EXOSNAP_CONFIG_DIR = Join-Path $scratch 'config'
$env:EXOSNAP_OUTPUT_DIR = Join-Path $scratch 'output'
New-Item -ItemType Directory -Path $env:EXOSNAP_CONFIG_DIR -Force | Out-Null
New-Item -ItemType Directory -Path $env:EXOSNAP_OUTPUT_DIR -Force | Out-Null

$evidence = [ordered]@{ application = @{ path = $appFull; sha256 = $appSha } }
$exitCode = 1
$app = $null
$conn = $null
try {
    $runId = New-LiveVerifyRunId
    $app = Start-Process -FilePath $appFull -PassThru -ArgumentList @('--live-verify-control', $runId)
    $conn = Connect-LiveVerify -RunId $runId -ConnectTimeoutMs ($TimeoutSeconds * 1000)

    # -- identity --------------------------------------------------------------
    $identity = Invoke-Query $conn 'app.identity'
    $evidence.identity = $identity
    Add-Step 'app.identity binds the run to this exact artifact' `
        ($identity.executableSha256 -eq $appSha) `
        @{ reported = $identity.executableSha256; measured = $appSha; version = $identity.productVersion }

    # -- windows ----------------------------------------------------------------
    $windows = Invoke-Query $conn 'windows.snapshot'
    $evidence.windows = $windows
    $roles = @($windows.windows | ForEach-Object { $_.role })
    Add-Step 'every native top-level window has a unique semantic role' `
        ($windows.rolesUnique -and $roles -contains 'main' -and $roles -notcontains 'unknown') `
        @{ roles = $roles; rolesUnique = $windows.rolesUnique; titlesUnique = $windows.titlesUnique }
    Add-Step 'the automation identity is the role plus this process, not the title' `
        ($windows.identity -eq 'role+processId' -and $windows.processId -eq $app.Id) `
        @{ identity = $windows.identity; processId = $windows.processId; pid = $app.Id }

    # -- configuration and environment ------------------------------------------
    $settings = Invoke-Query $conn 'settings.snapshot'
    $evidence.settings = $settings
    Add-Step 'settings.snapshot reports requested, effective and running' `
        ($null -ne $settings.requested -and $null -ne $settings.effective -and $null -ne $settings.running) `
        @{ differences = $settings.differences.Count; runningValid = $settings.running.valid }
    Add-Step 'no encoder is claimed to be running before one exists' `
        (-not $settings.running.valid) $settings.running

    $environment = Invoke-Query $conn 'environment.snapshot'
    $evidence.environment = $environment
    Add-Step 'environment.snapshot answers with truth classes, not bare booleans' `
        ($environment.present.availability -in @('available', 'unavailable', 'requiresOptIn', 'requiresElevation') -and
         $environment.gpu.adapterAvailability -in @('available', 'unavailable')) `
        @{ present = $environment.present.availability; gpu = $environment.gpu.adapterAvailability }

    $diagnostics = Invoke-Query $conn 'diagnostics.results'
    $evidence.diagnostics = @{ checked = $diagnostics.checked; results = $diagnostics.results.Count }
    Add-Step 'diagnostics.results distinguishes "nothing wrong" from "not checked"' `
        ($diagnostics.PSObject.Properties.Name -contains 'checked') `
        @{ checked = $diagnostics.checked; hasBlocker = $diagnostics.hasBlocker }

    # An idle process has no live pipeline, and must not answer as if it had one.
    $idlePipeline = Invoke-Query $conn 'pipeline.snapshot'
    Add-Step 'pipeline.snapshot is honest about an idle pipeline' `
        (-not $idlePipeline.valid -and $null -eq $idlePipeline.capture) `
        @{ valid = $idlePipeline.valid; lifecycle = $idlePipeline.lifecycle }

    # -- record -------------------------------------------------------------------
    $selected = Invoke-LiveVerifyCommand -Connection $conn -Command 'record.selectTarget' `
        -Parameters @{ kind = 'monitor' }
    Add-Step 'a capture target is selected through the product path' ([bool]$selected.ok) $selected.result

    $beforeStart = (Get-LiveVerifyState -Connection $conn).stateRevision
    $started = Invoke-LiveVerifyCommand -Connection $conn -Command 'record.start'
    Add-Step 'record.start is accepted without claiming the recording is running' `
        ($started.ok -and -not $started.settled) $started
    if (-not $started.ok) { throw "record.start refused: $($started.error.message)" }
    [void]$beforeStart

    $recording = Wait-RecordingState $conn @('Recording') ($TimeoutSeconds * 1000)
    $evidence.recordingState = $recording.recordingState

    $live = Invoke-Query $conn 'pipeline.snapshot'
    $evidence.pipelineWhileRecording = $live
    Add-Step 'pipeline.snapshot reports a real, measured pipeline while recording' `
        ($live.valid -and $live.lifecycle -eq 'recording' -and $live.capture.targetFps -gt 0) `
        @{ health = $live.health; bottleneck = $live.bottleneck; targetFps = $live.capture.targetFps }

    $runningSettings = Invoke-Query $conn 'settings.snapshot'
    Add-Step 'the RUNNING encoder configuration appears once an encoder exists' `
        ($runningSettings.running.valid -and $runningSettings.running.live) `
        $runningSettings.running
    $evidence.runningSettings = $runningSettings.running

    $marker = Invoke-LiveVerifyCommand -Connection $conn -Command 'record.addMarker'
    Add-Step 'record.addMarker is available while recording' ([bool]$marker.ok) $marker

    $paused = Invoke-LiveVerifyCommand -Connection $conn -Command 'record.pause'
    if (-not $paused.ok) { throw "record.pause refused: $($paused.error.message)" }
    [void](Wait-RecordingState $conn @('Paused') ($TimeoutSeconds * 1000))
    $pausedPipeline = Invoke-Query $conn 'pipeline.snapshot'
    Add-Step 'the pipeline lifecycle follows the transport into paused' `
        ($pausedPipeline.lifecycle -eq 'paused') @{ lifecycle = $pausedPipeline.lifecycle }

    $resumed = Invoke-LiveVerifyCommand -Connection $conn -Command 'record.resume'
    if (-not $resumed.ok) { throw "record.resume refused: $($resumed.error.message)" }
    [void](Wait-RecordingState $conn @('Recording') ($TimeoutSeconds * 1000))

    # The one bounded wait in the run, and it is a product parameter: how long to
    # record. Everything else is an observed transition.
    Start-Sleep -Seconds $RecordSeconds

    $stopped = Invoke-LiveVerifyCommand -Connection $conn -Command 'record.stop'
    if (-not $stopped.ok) { throw "record.stop refused: $($stopped.error.message)" }
    [void](Wait-RecordingState $conn @('Completed', 'Failed') ($TimeoutSeconds * 1000))

    $result = Invoke-Query $conn 'record.result'
    $evidence.result = $result
    Add-Step 'the recording completed and the product names its output' `
        ($result.hasResult -and $result.succeeded -and $result.outputFileBytes -gt 0) `
        @{ succeeded = $result.succeeded; bytes = $result.outputFileBytes; markers = $result.markerCount }

    # -- the session, and the events that belong to it ---------------------------
    $session = Invoke-Query $conn 'session.latest'
    $evidence.session = $session
    Add-Step 'session.latest is the canonical report for the recording that just ran' `
        ($session.available -and $null -ne $session.report.recording_session_id) `
        @{ available = $session.available; id = $session.report.recording_session_id }

    if ($session.available) {
        $recordingId = $session.report.recording_session_id
        $events = Invoke-Query $conn 'events.recent' @{ recordingSessionId = $recordingId; max = 20 }
        $codes = @($events.events | ForEach-Object { $_.eventCode })
        Add-Step 'the recording is addressable in the event stream by its own id' `
            ($codes -contains 'record.sessionStarted' -and $codes -contains 'record.sessionEnded') `
            @{ id = $recordingId; codes = $codes }
        $evidence.events = $codes
    }

    # -- edit ----------------------------------------------------------------------
    $opened = Invoke-LiveVerifyCommand -Connection $conn -Command 'edit.open'
    Add-Step 'the completed recording opens in the editor' ([bool]$opened.ok) $opened.result
    if ($opened.ok) {
        $editor = $opened.result
        $middle = [int]($editor.durationMs / 2)
        $sought = Invoke-LiveVerifyCommand -Connection $conn -Command 'edit.seek' -Parameters @{ positionMs = $middle }
        Add-Step 'the playhead moves and the adapter clamps it' `
            ($sought.ok -and $sought.result.positionMs -le $editor.durationMs) $sought.result

        $trimIn = Invoke-LiveVerifyCommand -Connection $conn -Command 'edit.setTrimIn' `
            -Parameters @{ positionMs = [int]($editor.durationMs / 4) }
        $trimOut = Invoke-LiveVerifyCommand -Connection $conn -Command 'edit.setTrimOut' `
            -Parameters @{ positionMs = [int]($editor.durationMs * 3 / 4) }
        Add-Step 'the trim range is set through the product path' `
            ($trimIn.ok -and $trimOut.ok -and $trimOut.result.trimStartMs -lt $trimOut.result.trimEndMs) `
            @{ start = $trimOut.result.trimStartMs; end = $trimOut.result.trimEndMs }

        if ($RequireExport) {
            $exportStarted = Invoke-LiveVerifyCommand -Connection $conn -Command 'export.start'
            Add-Step 'export.start is accepted without claiming completion' `
                ($exportStarted.ok -and -not $exportStarted.settled) $exportStarted
            $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds * 4)
            $state = Get-LiveVerifyState -Connection $conn
            while ($state.exportState -notin @('completed', 'failed')) {
                $remaining = [int]([Math]::Max(1, ($deadline - [DateTime]::UtcNow).TotalMilliseconds))
                if ($remaining -le 1) { break }
                [void](Wait-LiveVerifyRevision -Connection $conn -After $state.stateRevision -TimeoutMs $remaining)
                $state = Get-LiveVerifyState -Connection $conn
            }
            Add-Step 'the export reached a terminal state' ($state.exportState -eq 'completed') `
                @{ exportState = $state.exportState }
            $evidence.exportState = $state.exportState
        }

        $closed = Invoke-LiveVerifyCommand -Connection $conn -Command 'edit.close'
        Add-Step 'the edit session closes' ([bool]$closed.ok) $closed.result
    }

    $final = Get-LiveVerifyState -Connection $conn
    $evidence.finalState = $final
    Add-Step 'the shell ends where it started, with nothing blocking' `
        ($null -eq $final.blockingSurface -and $final.editSession -eq 'closed') `
        @{ page = $final.page; blockingSurface = $final.blockingSurface; editSession = $final.editSession }

    $exitCode = if (($steps | Where-Object { -not $_.pass }).Count -eq 0) { 0 } else { 1 }
}
finally {
    if ($null -ne $conn) { try { $conn.Close() } catch { } }
    if ($null -ne $app -and -not $app.HasExited) {
        try { [void]$app.CloseMainWindow(); [void]$app.WaitForExit(10000) } catch { }
        if (-not $app.HasExited) { try { $app.Kill() } catch { } }
    }
    # The recording this run made is evidence of the run, not an artifact to
    # keep. It never leaves the temp directory and it is never committed.
    Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue

    $evidence.steps = $steps
    $evidence.pass = ($steps | Where-Object { -not $_.pass }).Count -eq 0
    if ($EvidencePath) {
        $evidence | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $EvidencePath -Encoding utf8
        Write-Host "evidence: $EvidencePath"
    }
}

$verdict = if ($exitCode -eq 0) { 'PASS' } else { 'FAIL' }
Write-Host ("`n{0}: {1}/{2} steps passed" -f $verdict, ($steps | Where-Object { $_.pass }).Count, $steps.Count)
exit $exitCode
