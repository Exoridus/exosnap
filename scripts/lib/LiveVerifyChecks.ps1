#requires -Version 7.0
Set-StrictMode -Version Latest

<#
.SYNOPSIS
    The Live Verify check catalog.

.DESCRIPTION
    Each entry REFERENCES its canonical acceptance criteria (docs/release-checklist.md,
    docs/privacy-review.md, the frontend hardening handoff) and does not restate
    them. A runner catalog that re-specified the product would become a second
    source of truth, and the two would drift.

    `Layer` records the strongest verifier that can actually prove the check.
    The ladder, strongest first:

        FULL_AUTO        a deterministic test or script, no running application
        EXTERNAL_TOOL    an existing harness or analyzer (--hwnd-audit, ffprobe, --auto-record)
        CONTROL_CHANNEL  observation/intent against the running application
        UI_AUTOMATION    the real visible control, via UIA
        SEMI_AUTO        machine work plus one bounded human confirmation
        MANUAL_VISUAL    a human judging composition or appearance
        MANUAL_PHYSICAL  a human moving real hardware or a real pointer

    Nothing here may use a higher (weaker) layer than the requirement needs, and
    nothing may claim a lower one than it can actually deliver.
#>

function Get-LiveVerifyCatalog {
    $catalog = @()

    # -----------------------------------------------------------------------
    # Artifact identity
    # -----------------------------------------------------------------------
    $catalog += [pscustomobject]@{
        Id              = 'LV-ART-001'
        Title           = 'Artifact identity is resolved and recorded'
        Layer           = 'FULL_AUTO'
        Source          = 'docs/release-checklist.md §4 (spot-check), §7a (Identity)'
        ArtifactBound   = $true
        EnvironmentKeys = @()
        Run             = {
            param($ctx)
            $exe = $ctx.Artifact.exePath
            if (-not (Test-Path -LiteralPath $exe)) {
                return @{ Result = 'FAIL'; Message = "Artifact executable not found: $exe" }
            }
            $evidence = Save-LiveVerifyEvidence -Context $ctx -CheckId 'LV-ART-001' -Name 'artifact.json' `
                -Value $ctx.Artifact
            # The whole point of the fingerprint: every later PASS is bound to
            # these bytes, and a rebuild invalidates them.
            if ([string]::IsNullOrWhiteSpace($ctx.Artifact.exeSha256)) {
                return @{ Result = 'FAIL'; Message = 'No SHA-256 for the artifact; nothing could be bound to it' }
            }
            return @{
                Result   = 'PASS'
                Message  = "$($ctx.Artifact.productVersion) / $($ctx.Artifact.exeSha256.Substring(0,16))…"
                Evidence = @($evidence)
            }
        }
    }

    # -----------------------------------------------------------------------
    # Production isolation
    # -----------------------------------------------------------------------
    $catalog += [pscustomobject]@{
        Id              = 'LV-APP-001'
        Title           = 'A normal launch exposes no Live Verify endpoint'
        Layer           = 'FULL_AUTO'
        Source          = 'ADR 0066 (security boundary)'
        ArtifactBound   = $true
        EnvironmentKeys = @()
        Run             = {
            param($ctx)
            # --smoke-test is the cheapest launch of the REAL executable that
            # loads the QML engine and exits, so this observes the shipping
            # startup path rather than a stripped one.
            $runId = New-LiveVerifyRunId
            $probe = Start-Process -FilePath $ctx.Artifact.exePath -ArgumentList '--smoke-test' -PassThru
            $observed = @()
            try {
                for ($i = 0; $i -lt 40 -and -not $probe.HasExited; $i++) {
                    $observed += Test-Path -LiteralPath (New-LiveVerifyPipeName -RunId $runId)
                    # Any ExoSnap Live Verify endpoint at all, not only this run's.
                    $observed += @(Get-ChildItem '\\.\pipe\' -ErrorAction SilentlyContinue |
                            Where-Object { $_.Name -like 'ExoSnap.LiveVerify.*' }).Count -gt 0
                    Start-Sleep -Milliseconds 50
                }
            }
            finally {
                if (-not $probe.HasExited) { $probe | Stop-Process -Force -ErrorAction SilentlyContinue }
            }
            $anyEndpoint = @($observed | Where-Object { $_ }).Count -gt 0
            $evidence = Save-LiveVerifyEvidence -Context $ctx -CheckId 'LV-APP-001' -Name 'endpoint-probe.json' `
                -Value @{ samples = $observed.Count; anyEndpointSeen = $anyEndpoint; probedRunId = $runId }
            if ($anyEndpoint) {
                return @{ Result = 'FAIL'; Message = 'A Live Verify endpoint existed during a normal launch'
                    Evidence = @($evidence) }
            }
            return @{ Result = 'PASS'; Message = "No endpoint across $($observed.Count) samples"
                Evidence = @($evidence) }
        }
    }

    $catalog += [pscustomobject]@{
        Id              = 'LV-APP-002'
        Title           = 'Explicit Live Verify launch handshakes with the expected artifact'
        Layer           = 'CONTROL_CHANNEL'
        Source          = 'ADR 0066 (handshake)'
        ArtifactBound   = $true
        EnvironmentKeys = @()
        Run             = {
            param($ctx)
            $session = & $ctx.EnsureSession
            $identity = $session.Connection.Identity
            $evidence = Save-LiveVerifyEvidence -Context $ctx -CheckId 'LV-APP-002' -Name 'identity.json' `
                -Value $identity

            $problems = @()
            if ($identity.executableSha256 -ne $ctx.Artifact.exeSha256) {
                $problems += "executable SHA-256 mismatch (handshake $($identity.executableSha256), artifact $($ctx.Artifact.exeSha256))"
            }
            if ($identity.pid -ne $session.Process.Id) {
                $problems += "PID mismatch (handshake $($identity.pid), launched $($session.Process.Id))"
            }
            if ($identity.runId -ne $session.RunId) {
                $problems += 'run id mismatch'
            }
            if ($problems.Count -gt 0) {
                return @{ Result = 'FAIL'; Message = ($problems -join '; '); Evidence = @($evidence) }
            }
            return @{
                Result   = 'PASS'
                Message  = "$($identity.productVersion) @ $($identity.commit) pid $($identity.pid)"
                Evidence = @($evidence)
            }
        }
    }

    $catalog += [pscustomobject]@{
        Id              = 'LV-APP-003'
        Title           = 'The endpoint is gone once the application exits'
        Layer           = 'FULL_AUTO'
        Source          = 'ADR 0066 (lifetime)'
        ArtifactBound   = $true
        EnvironmentKeys = @()
        Run             = {
            param($ctx)
            $session = & $ctx.EnsureSession
            $runId = $session.RunId
            & $ctx.EndSession
            $pipe = New-LiveVerifyPipeName -RunId $runId
            $stillThere = Test-Path -LiteralPath $pipe
            $evidence = Save-LiveVerifyEvidence -Context $ctx -CheckId 'LV-APP-003' -Name 'endpoint-after-exit.json' `
                -Value @{ pipe = $pipe; present = $stillThere }
            if ($stillThere) {
                return @{ Result = 'FAIL'; Message = 'The endpoint outlived the process'; Evidence = @($evidence) }
            }
            return @{ Result = 'PASS'; Message = 'No endpoint after exit'; Evidence = @($evidence) }
        }
    }

    # -----------------------------------------------------------------------
    # Native window / chrome
    # -----------------------------------------------------------------------
    $catalog += [pscustomobject]@{
        Id              = 'LV-WIN-001'
        Title           = 'Window ownership and chrome audit (--hwnd-audit)'
        Layer           = 'EXTERNAL_TOOL'
        Source          = 'AGENTS.md, "Window-ownership and chrome auditing"'
        ArtifactBound   = $true
        EnvironmentKeys = @()
        Run             = {
            param($ctx)
            # The existing gate, invoked rather than reimplemented. It never
            # activates the window, so it is safe to run while the developer is
            # doing something else.
            $log = Join-Path $ctx.RunDirectory 'checks/LV-WIN-001/hwnd-audit.txt'
            New-Item -ItemType Directory -Path (Split-Path -Parent $log) -Force | Out-Null
            $process = Start-Process -FilePath $ctx.Artifact.exePath -ArgumentList '--hwnd-audit' `
                -RedirectStandardOutput $log -RedirectStandardError "$log.err" -PassThru -Wait
            $text = if (Test-Path $log) { Get-Content -LiteralPath $log -Raw } else { '' }
            $errorText = if (Test-Path "$log.err") { Get-Content -LiteralPath "$log.err" -Raw } else { '' }
            Set-Content -LiteralPath $log -Value "$text`n$errorText" -Encoding utf8NoBOM
            $relative = 'checks/LV-WIN-001/hwnd-audit.txt'
            if ($process.ExitCode -ne 0) {
                return @{ Result = 'FAIL'; Message = "--hwnd-audit exited $($process.ExitCode)"
                    Evidence = @($relative) }
            }
            return @{ Result = 'PASS'; Message = 'child_hwnds=0, no non-client area, WS_THICKFRAME present'
                Evidence = @($relative) }
        }
    }

    # -----------------------------------------------------------------------
    # Preview
    # -----------------------------------------------------------------------
    $catalog += [pscustomobject]@{
        Id              = 'LV-PREV-001'
        Title           = 'Live Preview presents frames'
        Layer           = 'CONTROL_CHANNEL'
        Source          = '.workspace/visual-reference/main-app/quick-hardening/LIVE-VERIFY.md §1'
        ArtifactBound   = $true
        EnvironmentKeys = @('primaryScreen')
        Run             = {
            param($ctx)
            $session = & $ctx.EnsureSession
            $null = Invoke-LiveVerifyCommand -Connection $session.Connection -Command 'record.selectTarget' `
                -Parameters @{ kind = 'monitor' }
            $ready = Wait-LiveVerifyState -Connection $session.Connection -Command 'preview.snapshot' `
                -Field 'frameReady' -Value 'True' -TimeoutMs 20000
            if ($null -eq $ready) {
                $last = (Invoke-LiveVerifyCommand -Connection $session.Connection -Command 'preview.snapshot').result
                $evidence = Save-LiveVerifyEvidence -Context $ctx -CheckId 'LV-PREV-001' -Name 'preview.json' -Value $last
                return @{ Result = 'FAIL'; Message = 'Preview never reported a ready frame'; Evidence = @($evidence) }
            }
            $first = (Invoke-LiveVerifyCommand -Connection $session.Connection -Command 'preview.snapshot').result
            Start-Sleep -Seconds 2
            $second = (Invoke-LiveVerifyCommand -Connection $session.Connection -Command 'preview.snapshot').result
            $evidence = Save-LiveVerifyEvidence -Context $ctx -CheckId 'LV-PREV-001' -Name 'preview.json' `
                -Value @{ first = $first; second = $second }
            # A rate alone is a number the adapter computes; the render-pass
            # counter moving is the scene graph actually drawing.
            $advanced = $second.updateGate.renderPasses -gt $first.updateGate.renderPasses
            if (-not $advanced) {
                return @{ Result = 'FAIL'; Message = 'No render pass in 2 s while the preview was live'
                    Evidence = @($evidence) }
            }
            return @{
                Result   = 'PASS'
                Message  = "renderPasses $($first.updateGate.renderPasses) -> $($second.updateGate.renderPasses), rate $([math]::Round($second.presentationRate,1)) Hz"
                Evidence = @($evidence)
            }
        }
    }

    $catalog += [pscustomobject]@{
        Id              = 'LV-WIN-002'
        Title           = 'Preview keeps presenting across a programmatic monitor change'
        Layer           = 'CONTROL_CHANNEL'
        Source          = '.workspace/visual-reference/main-app/quick-hardening/LIVE-VERIFY.md §1'
        ArtifactBound   = $true
        EnvironmentKeys = @('monitorTopology')
        Run             = {
            param($ctx)
            $session = & $ctx.EnsureSession
            $system = (Invoke-LiveVerifyCommand -Connection $session.Connection -Command 'system.snapshot').result
            if ($system.screenCount -lt 2) {
                return @{ Result = 'BLOCKED'
                    Message = "Needs two active monitors; this machine reports $($system.screenCount)" }
            }
            $null = Invoke-LiveVerifyCommand -Connection $session.Connection -Command 'record.selectTarget' `
                -Parameters @{ kind = 'monitor' }
            $null = Wait-LiveVerifyState -Connection $session.Connection -Command 'preview.snapshot' `
                -Field 'frameReady' -Value 'True' -TimeoutMs 20000

            $window = (Invoke-LiveVerifyCommand -Connection $session.Connection -Command 'window.snapshot').result
            # Not $home: that is a read-only PowerShell automatic variable.
            $homeScreen = $window.screen.name
            $other = @($system.screens | Where-Object { $_.name -ne $homeScreen })[0].name

            $samples = @()
            foreach ($target in @($other, $homeScreen)) {
                $before = (Invoke-LiveVerifyCommand -Connection $session.Connection -Command 'preview.snapshot').result
                $null = Invoke-LiveVerifyCommand -Connection $session.Connection -Command 'window.moveToScreen' `
                    -Parameters @{ screen = $target }
                $observed = Wait-LiveVerifyEvent -Connection $session.Connection -EventName 'window.screenChanged' `
                    -TimeoutMs 10000
                Start-Sleep -Seconds 3
                $settled = (Invoke-LiveVerifyCommand -Connection $session.Connection -Command 'preview.snapshot').result
                Start-Sleep -Seconds 2
                $after = (Invoke-LiveVerifyCommand -Connection $session.Connection -Command 'preview.snapshot').result
                $samples += @{
                    target             = $target
                    screenChangedSeen  = ($null -ne $observed)
                    renderPassesBefore = $before.updateGate.renderPasses
                    renderPassesSettled = $settled.updateGate.renderPasses
                    renderPassesAfter  = $after.updateGate.renderPasses
                    owedSettled        = $settled.updateGate.owed
                    owedAfter          = $after.updateGate.owed
                    presentationRate   = $after.presentationRate
                }
            }
            $evidence = Save-LiveVerifyEvidence -Context $ctx -CheckId 'LV-WIN-002' -Name 'transitions.json' `
                -Value @{ home = $homeScreen; other = $other; samples = $samples }

            # The defect this replaces: after a boundary crossing the preview
            # stopped until the mouse moved. Its signature is render passes not
            # advancing at all in a settled window after the transition.
            #
            # `owed` on its own is deliberately NOT an assertion. It is a
            # momentary flag -- true whenever a publish landed after the last
            # render pass -- so at 45 Hz on a live desktop it is true a large
            # fraction of the time on a perfectly healthy preview. It is recorded
            # as evidence, and only "owed with nothing rendering" is a freeze,
            # which the render-pass window below already covers.
            $stalledAfterMove = @($samples | Where-Object { $_.renderPassesSettled -le $_.renderPassesBefore })
            if ($stalledAfterMove.Count -gt 0) {
                return @{ Result = 'FAIL'
                    Message = "Preview stopped presenting after moving to $($stalledAfterMove[0].target)"
                    Evidence = @($evidence) }
            }
            $stalledSince = @($samples | Where-Object { $_.renderPassesAfter -le $_.renderPassesSettled })
            if ($stalledSince.Count -gt 0) {
                return @{ Result = 'FAIL'
                    Message = "Preview presented once after moving to $($stalledSince[0].target) and then froze with the pointer stationary"
                    Evidence = @($evidence) }
            }
            return @{ Result = 'PASS'; Message = "Both crossings kept presenting ($homeScreen <-> $other)"
                Evidence = @($evidence) }
        }
    }

    $catalog += [pscustomobject]@{
        Id              = 'LV-WIN-003'
        Title           = 'Preview survives a real interactive drag across a monitor boundary'
        Layer           = 'MANUAL_PHYSICAL'
        Source          = '.workspace/visual-reference/main-app/quick-hardening/LIVE-VERIFY.md §1'
        ArtifactBound   = $true
        EnvironmentKeys = @('monitorTopology')
        Run             = {
            param($ctx)
            $session = & $ctx.EnsureSession
            $system = (Invoke-LiveVerifyCommand -Connection $session.Connection -Command 'system.snapshot').result
            if ($system.screenCount -lt 2) {
                return @{ Result = 'BLOCKED'
                    Message = "Needs two active monitors; this machine reports $($system.screenCount)" }
            }
            $null = Invoke-LiveVerifyCommand -Connection $session.Connection -Command 'record.selectTarget' `
                -Parameters @{ kind = 'monitor' }
            $null = Wait-LiveVerifyState -Connection $session.Connection -Command 'preview.snapshot' `
                -Field 'frameReady' -Value 'True' -TimeoutMs 20000
            $before = (Invoke-LiveVerifyCommand -Connection $session.Connection -Command 'preview.snapshot').result

            $answered = & $ctx.ManualGate @{
                Title    = 'Interactive cross-monitor drag'
                Why      = @'
Programmatic window placement does not reproduce an interactive drag: the modal
move loop, the per-pixel WM_MOVING sequence and the moment the pointer stops are
all part of the failure this check exists for. The machine-observable half of it
is verified automatically before and after your drag.
'@
                Do       = @(
                    'Drag the ExoSnap window with the mouse from one monitor to the other.',
                    'Stop moving the mouse the moment the window lands, and do not touch anything for ~10 seconds.',
                    'Drag it back the same way and again leave the mouse still for ~10 seconds.'
                )
                Expected = 'The live Preview keeps updating during and after both crossings, without needing a mouse move, a hover or any other interaction.'
            }
            if (-not $answered) {
                return @{ Result = 'MANUAL_REQUIRED'; Message = 'Human drag gate not performed in this run' }
            }

            $settled = (Invoke-LiveVerifyCommand -Connection $session.Connection -Command 'preview.snapshot').result
            Start-Sleep -Seconds 2
            $after = (Invoke-LiveVerifyCommand -Connection $session.Connection -Command 'preview.snapshot').result
            $window = (Invoke-LiveVerifyCommand -Connection $session.Connection -Command 'window.snapshot').result
            $evidence = Save-LiveVerifyEvidence -Context $ctx -CheckId 'LV-WIN-003' -Name 'drag.json' `
                -Value @{ before = $before; settled = $settled; after = $after; window = $window }
            if ($settled.updateGate.renderPasses -le $before.updateGate.renderPasses) {
                return @{ Result = 'FAIL'
                    Message = 'No render pass happened across the whole manual drag window'
                    Evidence = @($evidence) }
            }
            # The freeze under test only shows with the pointer stationary, which
            # is what the gate asked the operator to leave it. `owed` alone is a
            # momentary flag and is evidence, not an assertion -- see LV-WIN-002.
            if ($after.updateGate.renderPasses -le $settled.updateGate.renderPasses) {
                return @{ Result = 'FAIL'
                    Message = 'The preview stopped presenting after the drag, with the pointer stationary'
                    Evidence = @($evidence) }
            }
            return @{ Result = 'PASS'
                Message = "renderPasses advanced by $($after.updateGate.renderPasses - $before.updateGate.renderPasses) across the manual drag and kept advancing afterwards"
                Evidence = @($evidence) }
        }
    }

    # -----------------------------------------------------------------------
    # Recording
    # -----------------------------------------------------------------------
    $catalog += [pscustomobject]@{
        Id              = 'LV-REC-001'
        Title           = 'Record -> Pause -> Resume -> Stop through the application intents'
        Layer           = 'CONTROL_CHANNEL'
        Source          = 'docs/product-spec.md (transport), docs/release-checklist.md §7'
        ArtifactBound   = $true
        EnvironmentKeys = @('primaryScreen')
        Run             = {
            param($ctx)
            $session = & $ctx.EnsureSession
            $connection = $session.Connection
            $null = Invoke-LiveVerifyCommand -Connection $connection -Command 'record.selectTarget' `
                -Parameters @{ kind = 'monitor' }
            $null = Wait-LiveVerifyState -Connection $connection -Command 'preview.snapshot' `
                -Field 'frameReady' -Value 'True' -TimeoutMs 20000

            $timeline = @()
            $step = {
                param($command, $field, $expected, $timeoutMs)
                $response = Invoke-LiveVerifyCommand -Connection $connection -Command $command
                if (-not $response.ok) { throw "$command refused: $($response.error.code) - $($response.error.message)" }
                # Waits for the authoritative state, never a sleep.
                $state = Wait-LiveVerifyState -Connection $connection -Command 'record.snapshot' `
                    -Field $field -Value $expected -TimeoutMs $timeoutMs
                if ($null -eq $state) { throw "$command did not reach $field=$expected within ${timeoutMs} ms" }
                return $state
            }

            try {
                $timeline += @{ step = 'start'; state = (& $step 'record.start' 'recording' 'True' 45000) }
                Start-Sleep -Seconds 3
                $timeline += @{ step = 'pause'; state = (& $step 'record.pause' 'paused' 'True' 20000) }
                Start-Sleep -Seconds 1
                $timeline += @{ step = 'resume'; state = (& $step 'record.resume' 'recording' 'True' 20000) }
                Start-Sleep -Seconds 3
                $stopResponse = Invoke-LiveVerifyCommand -Connection $connection -Command 'record.stop'
                if (-not $stopResponse.ok) { throw "record.stop refused: $($stopResponse.error.message)" }
                $ready = Wait-LiveVerifyEvent -Connection $connection -EventName 'record.resultReady' -TimeoutMs 120000
                if ($null -eq $ready) { throw 'No record.resultReady event within 120 s' }
                $timeline += @{ step = 'stop'; state = $ready.data }
            }
            catch {
                $evidence = Save-LiveVerifyEvidence -Context $ctx -CheckId 'LV-REC-001' -Name 'timeline.json' `
                    -Value $timeline
                return @{ Result = 'FAIL'; Message = $_.Exception.Message; Evidence = @($evidence) }
            }

            $result = (Invoke-LiveVerifyCommand -Connection $connection -Command 'record.result').result
            $transcript = Save-LiveVerifyEvidence -Context $ctx -CheckId 'LV-REC-001' -Name 'transcript.json' `
                -Value (Get-LiveVerifyTranscript -Connection $connection)
            $evidence = Save-LiveVerifyEvidence -Context $ctx -CheckId 'LV-REC-001' -Name 'result.json' -Value $result
            $ctx.State['lastRecordingPath'] = $result.outputPath

            if (-not $result.succeeded) {
                return @{ Result = 'FAIL'; Message = "Recording failed: $($result.errorDetail)"
                    Evidence = @($evidence, $transcript) }
            }
            return @{
                Result   = 'PASS'
                Message  = "$([math]::Round($result.durationSeconds,1)) s -> $($result.outputPath)"
                Evidence = @($evidence, $transcript)
            }
        }
    }

    $catalog += [pscustomobject]@{
        Id              = 'LV-MEDIA-001'
        Title           = 'The produced recording exists and decodes'
        Layer           = 'EXTERNAL_TOOL'
        Source          = 'docs/release-checklist.md §7 (post-checks)'
        ArtifactBound   = $true
        EnvironmentKeys = @('ffprobeVersion')
        DependsOn       = 'LV-REC-001'
        Run             = {
            param($ctx)
            $path = $ctx.State['lastRecordingPath']
            if ([string]::IsNullOrWhiteSpace($path)) {
                return @{ Result = 'UNVERIFIED'; Message = 'LV-REC-001 produced no output path to analyse' }
            }
            if (-not (Test-Path -LiteralPath $path)) {
                return @{ Result = 'FAIL'; Message = "Recording is missing on disk: $path" }
            }
            $probe = Get-LiveVerifyFfprobe
            if ($null -eq $probe) {
                return @{ Result = 'BLOCKED'; Message = 'ffprobe is not on PATH; media cannot be analysed' }
            }
            $json = & $probe -v error -print_format json -show_format -show_streams -- "$path" 2>&1 | Out-String
            $evidenceProbe = Save-LiveVerifyEvidence -Context $ctx -CheckId 'LV-MEDIA-001' -Name 'ffprobe.json' `
                -Raw $json
            $parsed = $null
            try { $parsed = $json | ConvertFrom-Json } catch { $parsed = $null }
            if ($null -eq $parsed) {
                return @{ Result = 'FAIL'; Message = 'ffprobe produced no parseable output'
                    Evidence = @($evidenceProbe) }
            }
            $video = @($parsed.streams | Where-Object { $_.codec_type -eq 'video' })
            if ($video.Count -lt 1) {
                return @{ Result = 'FAIL'; Message = 'No video stream in the recording'; Evidence = @($evidenceProbe) }
            }
            # Filename and container header are not proof that the bytes decode.
            $decodeLog = & $probe -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames `
                -of default=nokey=1:noprint_wrappers=1 -read_intervals '%+#60' -- "$path" 2>&1 | Out-String
            $evidenceDecode = Save-LiveVerifyEvidence -Context $ctx -CheckId 'LV-MEDIA-001' -Name 'decode.txt' `
                -Raw $decodeLog
            $frames = 0
            [void][int]::TryParse($decodeLog.Trim(), [ref]$frames)
            if ($frames -lt 1) {
                return @{ Result = 'FAIL'; Message = 'No video frame decoded from the recording'
                    Evidence = @($evidenceProbe, $evidenceDecode) }
            }
            return @{
                Result   = 'PASS'
                Message  = "$($video[0].codec_name) $($video[0].width)x$($video[0].height), $frames frames decoded, duration $($parsed.format.duration) s"
                Evidence = @($evidenceProbe, $evidenceDecode)
            }
        }
    }

    $catalog += [pscustomobject]@{
        Id              = 'LV-EDIT-001'
        Title           = 'Chained Record -> Edit -> Export harness'
        Layer           = 'EXTERNAL_TOOL'
        Source          = 'docs/superpowers/specs/2026-07-14-auto-record-harness-design.md; ADR 0022'
        ArtifactBound   = $true
        EnvironmentKeys = @('primaryScreen')
        Run             = {
            param($ctx)
            # The existing deterministic media E2E, invoked as-is. It exercises
            # the real decode path, which no fixture reaches -- a fixture-only
            # suite once hid a defect that aborted the process on the first real
            # clip. Nothing here is reimplemented over the control channel.
            # The harness launches its own ExoSnap. Any control-channel session
            # still open would own the single-instance guard, and the harness
            # process would then be activated-and-exited by it -- exit 0, no
            # file, and a FAIL that describes the runner rather than the product.
            & $ctx.EndSession
            $outputDirectory = Join-Path $ctx.RunDirectory 'media/auto-edit'
            New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
            $log = Join-Path $ctx.RunDirectory 'checks/LV-EDIT-001/auto-edit.log'
            New-Item -ItemType Directory -Path (Split-Path -Parent $log) -Force | Out-Null

            $previous = $env:EXOSNAP_OUTPUT_DIR
            $env:EXOSNAP_OUTPUT_DIR = $outputDirectory
            $timedOut = $false
            $noHarness = $false
            try {
                # The report is the only thing that says WHY the chain failed.
                # A GUI subsystem binary writes nothing to a redirected stdout on
                # Windows, so the .log files next to this are always empty --
                # without the report a non-zero exit is just a number.
                $reportPath = Join-Path $ctx.RunDirectory 'checks/LV-EDIT-001/auto-edit-report.json'
                $process = Start-Process -FilePath $ctx.Artifact.exePath -PassThru `
                    -RedirectStandardOutput $log -RedirectStandardError "$log.err" `
                    -ArgumentList @('--auto-record', '--target', 'monitor', '--duration', '6',
                    # Without --audio-rows the harness records no audio at all,
                    # and the chain's decode.audioTracks stage then fails on
                    # every run by construction -- it asserts a track this
                    # invocation never asked to be captured. SYS is the shipped
                    # default for screen capture, so it is what acceptance should
                    # be exercising anyway.
                    '--audio-rows', 'sys',
                    '--auto-edit', '--export-container', 'mkv', '--auto-edit-report', $reportPath)
                # Establish whether the artifact even HAS the harness, because a
                # Release build without EXOSNAP_BUILD_BENCHMARK_HARNESS=ON does
                # not know these options: it ignores them and opens a normal
                # window instead, which never returns. Waiting the full timeout
                # for that left a real ExoSnap window -- recovery prompt and all
                # -- on the operator's desktop for three minutes, looking like a
                # product state under test.
                #
                # The signal is PROGRESS, not the presence of a window: --auto-
                # edit legitimately opens one off-screen to reuse the preview and
                # editor machinery, so "a window exists" says nothing. It was
                # tried, and it reported BLOCKED for a harness that was working
                # and had already written both files. A harness run instead makes
                # the recording appear in the output directory within a bounded
                # time; a build without one never writes anything there.
                $recordingDeadline = [DateTime]::UtcNow.AddSeconds(75)
                while ([DateTime]::UtcNow -lt $recordingDeadline) {
                    if ($process.HasExited) { break }
                    if (@(Get-ChildItem -LiteralPath $outputDirectory -File -ErrorAction SilentlyContinue).Count -gt 0) {
                        break
                    }
                    Start-Sleep -Milliseconds 500
                }
                if (-not $process.HasExited -and
                    @(Get-ChildItem -LiteralPath $outputDirectory -File -ErrorAction SilentlyContinue).Count -eq 0) {
                    $noHarness = $true
                }
                if ($noHarness) {
                    $process | Stop-Process -Force -ErrorAction SilentlyContinue
                    [void]$process.WaitForExit(15000)
                }
                elseif (-not $process.WaitForExit(180000)) {
                    $timedOut = $true
                    $process | Stop-Process -Force -ErrorAction SilentlyContinue
                    # Wait for it to actually be gone. A process still dying is
                    # indistinguishable from a foreign instance to the next
                    # check's single-instance guard, and that turned a BLOCKED
                    # here into three unrelated FAILs after it.
                    [void]$process.WaitForExit(15000)
                }
            }
            finally { $env:EXOSNAP_OUTPUT_DIR = $previous }

            $relative = 'checks/LV-EDIT-001/auto-edit.log'
            $produced = @(Get-ChildItem -LiteralPath $outputDirectory -File -ErrorAction SilentlyContinue)
            if ($noHarness) {
                return @{ Result = 'BLOCKED'
                    Message = 'The artifact wrote no recording within 75 s and kept running; --auto-record/--auto-edit are not compiled into it (a Release build needs EXOSNAP_BUILD_BENCHMARK_HARNESS=ON)'
                    Evidence = @($relative) }
            }
            if ($timedOut) {
                return @{ Result = 'BLOCKED'
                    Message = 'The artifact ran headless but never exited within 180 s'
                    Evidence = @($relative) }
            }
            if ($process.ExitCode -ne 0) {
                $failedStages = @()
                if (Test-Path -LiteralPath $reportPath) {
                    try {
                        $report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
                        $failedStages = @($report.stages | Where-Object { -not $_.passed } | ForEach-Object {
                                if ($_.PSObject.Properties.Name -contains 'detail' -and $_.detail) {
                                    "$($_.stage) ($($_.detail))"
                                } else { $_.stage }
                            })
                    }
                    catch { $failedStages = @() }
                }
                $why = if ($failedStages.Count -gt 0) { ": $($failedStages -join '; ')" } else { '' }
                return @{ Result = 'FAIL'
                    Message = "--auto-record/--auto-edit exited $($process.ExitCode)$why"
                    Evidence = @($relative, 'checks/LV-EDIT-001/auto-edit-report.json') }
            }
            if ($produced.Count -lt 1) {
                return @{ Result = 'FAIL'; Message = 'The chained harness exited 0 but produced no file'
                    Evidence = @($relative) }
            }
            return @{ Result = 'PASS'; Message = "$($produced.Count) file(s) produced by the chained harness"
                Evidence = @($relative) }
        }
    }

    # -----------------------------------------------------------------------
    # Capture-excluded overlays
    # -----------------------------------------------------------------------
    $catalog += [pscustomobject]@{
        Id              = 'LV-OVL-001'
        Title           = 'Overlay native composition invariants'
        Layer           = 'CONTROL_CHANNEL'
        Source          = '.workspace/visual-reference/main-app/quick-hardening/LIVE-VERIFY.md §2; ADR 0016'
        ArtifactBound   = $true
        EnvironmentKeys = @()
        Run             = {
            param($ctx)
            $session = & $ctx.EnsureSession
            $connection = $session.Connection

            # The invariants only hold for a window that has been SHOWN: the
            # WS_EX_LAYERED strip and the display-affinity call run on show, so a
            # never-shown overlay legitimately still carries the bit. Drive a
            # recording so the pill is genuinely on screen, then assert.
            $null = Invoke-LiveVerifyCommand -Connection $connection -Command 'record.selectTarget' `
                -Parameters @{ kind = 'monitor' }
            $null = Wait-LiveVerifyState -Connection $connection -Command 'preview.snapshot' `
                -Field 'frameReady' -Value 'True' -TimeoutMs 20000
            $started = Invoke-LiveVerifyCommand -Connection $connection -Command 'record.start'
            if (-not $started.ok) {
                return @{ Result = 'UNVERIFIED'
                    Message = "Could not start a recording to raise the overlays: $($started.error.message)" }
            }
            $null = Wait-LiveVerifyState -Connection $connection -Command 'record.snapshot' `
                -Field 'recording' -Value 'True' -TimeoutMs 45000
            Start-Sleep -Seconds 2
            $snapshot = (Invoke-LiveVerifyCommand -Connection $connection -Command 'overlay.snapshot').result
            $null = Invoke-LiveVerifyCommand -Connection $connection -Command 'record.stop'
            $null = Wait-LiveVerifyEvent -Connection $connection -EventName 'record.resultReady' -TimeoutMs 120000

            # A second look AFTER the stop, because the toast stack is the one
            # operable overlay that does not exist during a recording -- it says
            # the recording was saved. Snapshotting only mid-recording is how a
            # toast that could not be clicked at all passed this check.
            Start-Sleep -Seconds 2
            $afterStop = (Invoke-LiveVerifyCommand -Connection $connection -Command 'overlay.snapshot').result

            $evidence = Save-LiveVerifyEvidence -Context $ctx -CheckId 'LV-OVL-001' -Name 'overlays.json' `
                -Value @{ duringRecording = $snapshot; afterStop = $afterStop }

            $realized = @(@($snapshot.overlays) + @($afterStop.overlays) |
                    Where-Object { $_.visible -and $_.nativeWindowCreated } |
                    Group-Object -Property objectName |
                    ForEach-Object { $_.Group[0] })
            if ($realized.Count -eq 0) {
                $names = @($snapshot.overlays | ForEach-Object { $_.objectName }) -join ', '
                return @{ Result = 'UNVERIFIED'
                    Message = "No overlay was on screen during the recording (present but hidden: $names). Enable the recording overlay in Settings, Overlays."
                    Evidence = @($evidence) }
            }
            # The overlays a user is meant to operate. The other three are
            # informational and are click-through on purpose.
            $operableOverlays = @('quickOverlayNotificationToast', 'quickOverlayQuickControls')

            $problems = @()
            foreach ($overlay in $realized) {
                # WS_EX_LAYERED and DirectComposition are mutually exclusive for
                # per-pixel alpha; the bit being set is the defect that made the
                # countdown compose as an opaque rectangle.
                if ($overlay.native.layered) { $problems += "$($overlay.objectName): WS_EX_LAYERED is set" }
                if (-not $overlay.native.captureExcluded) {
                    $problems += "$($overlay.objectName): display affinity is $($overlay.native.displayAffinity), not WDA_EXCLUDEFROMCAPTURE"
                }
                # Two of the five overlays carry controls. WS_EX_TRANSPARENT on
                # those is invisible everywhere else -- the scene graph renders
                # them, the harness screenshots them, UI Automation finds them --
                # and yet nothing on the window can be clicked. The toast shipped
                # exactly like that: dismiss, Edit and Show in folder all dead,
                # and a notification the user could not get rid of.
                if ($operableOverlays -contains $overlay.objectName -and $overlay.native.transparentForInput) {
                    $problems += "$($overlay.objectName): WS_EX_TRANSPARENT is set, so its controls cannot be clicked"
                }
            }
            if ($problems.Count -gt 0) {
                return @{ Result = 'FAIL'; Message = ($problems -join '; '); Evidence = @($evidence) }
            }
            # Name what was actually covered. An operable overlay only exists
            # while the condition that raises it holds -- the toast needs a
            # notification, and "Open editor when finished" suppresses the saved
            # one entirely -- so a run can legitimately never see it. Saying so
            # beats a PASS that reads as if every overlay had been inspected.
            $seen = @($realized | ForEach-Object { $_.objectName })
            $unseenOperable = @($operableOverlays | Where-Object { $seen -notcontains $_ })
            $coverage = if ($unseenOperable.Count -gt 0) {
                "; not raised in this run, so unchecked: $($unseenOperable -join ', ')"
            } else { '' }
            return @{
                Result   = 'PASS'
                Message  = "$($realized.Count) visible overlay window(s): no WS_EX_LAYERED, WDA_EXCLUDEFROMCAPTURE held, operable ones take input$coverage"
                Evidence = @($evidence)
            }
        }
    }

    $catalog += [pscustomobject]@{
        Id              = 'LV-OVL-002'
        Title           = 'Overlay appearance on the real desktop'
        Layer           = 'MANUAL_VISUAL'
        Source          = '.workspace/visual-reference/main-app/quick-hardening/LIVE-VERIFY.md §2a/§2b'
        ArtifactBound   = $true
        EnvironmentKeys = @()
        Run             = {
            param($ctx)
            $session = & $ctx.EnsureSession
            $answered = & $ctx.ManualGate @{
                Title    = 'Capture-excluded overlay composition'
                Why      = @'
WDA_EXCLUDEFROMCAPTURE defeats screenshots, screen recording and PrintWindow by
design, and the scene-graph grab the harness uses shows correct alpha even when
the window composes wrongly on the desktop. No instrument in this repository can
see how these windows actually reach your screen. The native state that decides
composition is already asserted by LV-OVL-001.
'@
                Do       = @(
                    'In ExoSnap set a 3 second countdown (Record, chevron) and start a recording with the recording overlay enabled (Settings, Overlays).',
                    'Watch the countdown circle and then the recording pill on the desktop.',
                    'Stop the recording.'
                )
                Expected = 'Both are drawn on a truly transparent background - the desktop shows through, there is no rectangular grey or black backing panel, and the countdown drop shadow is soft.'
            }
            if (-not $answered) {
                return @{ Result = 'MANUAL_REQUIRED'; Message = 'Human composition gate not performed in this run' }
            }
            # The machine half is re-read afterwards rather than trusted from
            # before: the human just made the windows exist.
            $snapshot = (Invoke-LiveVerifyCommand -Connection $session.Connection -Command 'overlay.snapshot').result
            $evidence = Save-LiveVerifyEvidence -Context $ctx -CheckId 'LV-OVL-002' -Name 'overlays-after.json' `
                -Value $snapshot
            return @{ Result = 'PASS'; Message = 'Confirmed on the real desktop by the operator'
                Evidence = @($evidence) }
        }
    }

    # -----------------------------------------------------------------------
    # UI Automation
    # -----------------------------------------------------------------------
    $catalog += [pscustomobject]@{
        Id              = 'LV-UIA-001'
        Title           = 'The visible transport control is wired to the recording state'
        Layer           = 'UI_AUTOMATION'
        Source          = 'ADR 0066 (dual-path verification)'
        ArtifactBound   = $true
        EnvironmentKeys = @()
        Run             = {
            param($ctx)
            # Dual path on purpose: UIA activates the control a user clicks, and
            # the control channel observes the authoritative state that follows.
            # An IPC call alone would not be a UI test, and a UIA click alone
            # would not prove the recording state changed.
            if (-not (Test-LiveVerifyUiaAvailable)) {
                return @{ Result = 'BLOCKED'
                    Message = 'UI Automation client assemblies are unavailable in this PowerShell host' }
            }
            $session = & $ctx.EnsureSession
            $connection = $session.Connection
            $null = Invoke-LiveVerifyCommand -Connection $connection -Command 'record.selectTarget' `
                -Parameters @{ kind = 'monitor' }
            $null = Wait-LiveVerifyState -Connection $connection -Command 'preview.snapshot' `
                -Field 'frameReady' -Value 'True' -TimeoutMs 20000

            # 1. UIA invokes the real visible Record button.
            if (-not (Invoke-LiveVerifyUiaElement -ProcessId $session.Process.Id -NamePattern '(?i)^start recording$')) {
                $tree = Get-LiveVerifyUiaTree -ProcessId $session.Process.Id
                $evidenceTree = Save-LiveVerifyEvidence -Context $ctx -CheckId 'LV-UIA-001' -Name 'uia-tree.json' `
                    -Value $tree
                return @{ Result = 'FAIL'
                    Message = 'The accessibility tree exposes no invokable "Start recording"; uia-tree.json records what it does expose'
                    Evidence = @($evidenceTree) }
            }
            # 2. The control channel observes the authoritative state that follows.
            #    Neither half alone is the proof: an IPC call is not a UI test, and
            #    a UIA click is not evidence that recording actually started.
            $recording = Wait-LiveVerifyState -Connection $connection -Command 'record.snapshot' `
                -Field 'recording' -Value 'True' -TimeoutMs 45000
            if ($null -eq $recording) {
                return @{ Result = 'FAIL'; Message = 'UIA invoked Start recording but the state never became recording' }
            }

            $paused = $null
            $pauseInvoked = Invoke-LiveVerifyUiaElement -ProcessId $session.Process.Id -NamePattern '(?i)^pause( recording)?$'
            if ($pauseInvoked) {
                $paused = Wait-LiveVerifyState -Connection $connection -Command 'record.snapshot' `
                    -Field 'paused' -Value 'True' -TimeoutMs 20000
            }

            $tree = Get-LiveVerifyUiaTree -ProcessId $session.Process.Id
            $evidenceTree = Save-LiveVerifyEvidence -Context $ctx -CheckId 'LV-UIA-001' -Name 'uia-tree.json' -Value $tree

            # Always leave the application at rest, whatever the outcome above.
            $null = Invoke-LiveVerifyCommand -Connection $connection -Command 'record.stop'
            $null = Wait-LiveVerifyEvent -Connection $connection -EventName 'record.resultReady' -TimeoutMs 120000

            if (-not $pauseInvoked) {
                return @{ Result = 'FAIL'
                    Message = 'While recording, the accessibility tree exposes no invokable Pause control'
                    Evidence = @($evidenceTree) }
            }
            if ($null -eq $paused) {
                return @{ Result = 'FAIL'
                    Message = 'UIA invoked Pause but the recording state never became paused'
                    Evidence = @($evidenceTree) }
            }
            return @{ Result = 'PASS'
                Message = "UIA invoked Start recording and Pause on the real controls; the control channel observed recording then paused ($($tree.Count) elements exposed)"
                Evidence = @($evidenceTree) }
        }
    }

    # -----------------------------------------------------------------------
    # Appearance
    # -----------------------------------------------------------------------
    $catalog += [pscustomobject]@{
        Id              = 'LV-THEME-001'
        Title           = 'Light appearance reads as four distinguishable steps'
        Layer           = 'MANUAL_VISUAL'
        Source          = '.workspace/visual-reference/main-app/quick-hardening/LIVE-VERIFY.md §4'
        ArtifactBound   = $true
        EnvironmentKeys = @()
        Run             = {
            param($ctx)
            $session = & $ctx.EnsureSession
            $app = (Invoke-LiveVerifyCommand -Connection $session.Connection -Command 'app.snapshot').result
            $answered = & $ctx.ManualGate @{
                Title    = 'Light appearance walkthrough'
                Why      = @'
"Washed out" and "enough depth" are judgements. The numeric contrast floor is
already a unit test (theme_contrast_tests) against the CURRENT token values -
judge what you see, not an older Light screenshot.
'@
                Do       = @(
                    'Switch Settings, Appearance to Light.',
                    'Walk Record, Settings, Diagnostics, Logs, About.',
                    'Hover a control on a card on each page.'
                )
                Expected = 'Page background, card, raised control and hover are four distinguishable steps; hovering visibly changes a control; no large sterile pure-white regions; recording coral, caution amber and ready green stay recognisable as states and none is confusable with the accent.'
            }
            if (-not $answered) {
                return @{ Result = 'MANUAL_REQUIRED'; Message = 'Light appearance gate not performed in this run' }
            }
            $after = (Invoke-LiveVerifyCommand -Connection $session.Connection -Command 'app.snapshot').result
            $evidence = Save-LiveVerifyEvidence -Context $ctx -CheckId 'LV-THEME-001' -Name 'appearance.json' `
                -Value @{ before = $app; after = $after }
            return @{ Result = 'PASS'; Message = "Confirmed by the operator (appearance $($after.appearanceId)/$($after.accentId))"
                Evidence = @($evidence) }
        }
    }

    # -----------------------------------------------------------------------
    # RC-only
    # -----------------------------------------------------------------------
    $catalog += [pscustomobject]@{
        Id              = 'LV-UPD-001'
        Title           = 'Updater round-trip on a published RC'
        Layer           = 'SEMI_AUTO'
        Source          = 'docs/release-checklist.md §5, §7a'
        ArtifactBound   = $true
        EnvironmentKeys = @()
        Run             = {
            param($ctx)
            if ($ctx.Artifact.kind -ne 'rc') {
                return @{ Result = 'BLOCKED'
                    Message = 'Needs a published, immutable RC; a local Release build cannot prove the updater path' }
            }
            return @{ Result = 'MANUAL_REQUIRED'
                Message = 'Run docs/release-checklist.md §5 and §7a against this RC; process observation is wired, the swap itself is not yet driven from here' }
        }
    }

    return $catalog
}

# ---------------------------------------------------------------------------
# Helpers shared by the checks
# ---------------------------------------------------------------------------

function Save-LiveVerifyEvidence {
    <#
    .SYNOPSIS
        Writes one evidence file under checks/<id>/ and returns its run-relative
        path, which is what a report cites.
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] $Context,
        [Parameter(Mandatory)] [string] $CheckId,
        [Parameter(Mandatory)] [string] $Name,
        $Value,
        [string] $Raw
    )
    $directory = Join-Path $Context.RunDirectory "checks/$CheckId"
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
    $path = Join-Path $directory $Name
    if ($PSBoundParameters.ContainsKey('Raw')) {
        Set-Content -LiteralPath $path -Value $Raw -Encoding utf8NoBOM
    }
    else {
        Set-Content -LiteralPath $path -Value ($Value | ConvertTo-Json -Depth 20) -Encoding utf8NoBOM
    }
    return "checks/$CheckId/$Name"
}

function Get-LiveVerifyFfprobe {
    $command = Get-Command ffprobe -ErrorAction SilentlyContinue
    if ($null -ne $command) { return $command.Source }
    return $null
}

function Test-LiveVerifyUiaAvailable {
    try {
        Add-Type -AssemblyName UIAutomationClient -ErrorAction Stop
        Add-Type -AssemblyName UIAutomationTypes -ErrorAction Stop
        return $true
    }
    catch { return $false }
}

function Get-LiveVerifyUiaTree {
    <#
    .SYNOPSIS
        Flattens the accessibility tree of a process into role/name/state rows.
    .DESCRIPTION
        Structural, never coordinate-based: the rows carry control type,
        accessible name, enabled state and whether an Invoke/Toggle pattern is
        available. If the tree turns out not to expose what a check needs, the
        answer is to record the gap -- not to start clicking at pixel positions.
    #>
    [CmdletBinding()]
    param([Parameter(Mandatory)] [int] $ProcessId, [int] $MaxElements = 400)

    if (-not (Test-LiveVerifyUiaAvailable)) { return @() }
    $root = [System.Windows.Automation.AutomationElement]::RootElement
    $condition = [System.Windows.Automation.PropertyCondition]::new(
        [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $ProcessId)
    $windows = $root.FindAll([System.Windows.Automation.TreeScope]::Children, $condition)

    $rows = @()
    foreach ($window in $windows) {
        $descendants = $window.FindAll([System.Windows.Automation.TreeScope]::Descendants,
            [System.Windows.Automation.Condition]::TrueCondition)
        foreach ($element in $descendants) {
            if ($rows.Count -ge $MaxElements) { break }
            # Asked the same way the invoker asks, so the tree never advertises
            # something Invoke-LiveVerifyUiaElement would then fail to reach.
            $invokePattern = $null
            $togglePattern = $null
            $rows += [pscustomobject]@{
                controlType  = $element.Current.ControlType.ProgrammaticName
                name         = $element.Current.Name
                automationId = $element.Current.AutomationId
                enabled      = $element.Current.IsEnabled
                invokable    = $element.TryGetCurrentPattern(
                    [System.Windows.Automation.InvokePattern]::Pattern, [ref]$invokePattern)
                togglable    = $element.TryGetCurrentPattern(
                    [System.Windows.Automation.TogglePattern]::Pattern, [ref]$togglePattern)
            }
        }
    }
    return $rows
}

function Invoke-LiveVerifyUiaElement {
    <#
    .SYNOPSIS
        Invokes one element by accessible name. No screen coordinates, no
        synthesized pointer: the OS cursor never moves, so this cannot collide
        with the developer's own pointer.
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] [int] $ProcessId,
        [Parameter(Mandatory)] [string] $NamePattern
    )
    if (-not (Test-LiveVerifyUiaAvailable)) { return $false }
    $root = [System.Windows.Automation.AutomationElement]::RootElement
    $condition = [System.Windows.Automation.PropertyCondition]::new(
        [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $ProcessId)
    foreach ($window in $root.FindAll([System.Windows.Automation.TreeScope]::Children, $condition)) {
        foreach ($element in $window.FindAll([System.Windows.Automation.TreeScope]::Descendants,
                [System.Windows.Automation.Condition]::TrueCondition)) {
            if ($element.Current.Name -notmatch $NamePattern) { continue }
            $pattern = $null
            if ($element.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$pattern)) {
                $pattern.Invoke()
                return $true
            }
        }
    }
    return $false
}
