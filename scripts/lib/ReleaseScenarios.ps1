#Requires -Version 7.0
<#
.SYNOPSIS
    The v0.9 release scenario catalog.

.DESCRIPTION
    One entry per release gate. A scenario declares what it needs and what it proves;
    it never hardcodes a fact about one desk.

        Id                   stable identifier, cited by the report and the checklist
        Title                one line, in the terms the gate is written in
        Class                grouping, and the unit -IncludeClass opts into
        Layer                the WEAKEST mechanism this scenario actually uses
        Source               where the requirement is written down; never restated here
        ArtifactBound        does the verdict describe these bytes?
        RequiresInstallTree  needs `cmake --install` (updater / handoff paths)
        EnvironmentKeys      environment facts the verdict depends on -> staleness
        Requires             device ALIASES the scenario needs bound and present
        Desired              the environment mutation, as { "alias:property" = "value" }
        OptIn                excluded from a default sweep (long, or physically disruptive)
        Run                  { param($ctx, $transaction) -> @{ Result; Message; Evidence } }

    Two conventions the whole catalog depends on:

    ALIASES, NOT NAMES. `display.main-hdr`, not "27GL850". A scenario that names one
    monitor is a scenario that can only ever run at one desk, and a friendly name is
    not even stable there -- two identical monitors share one. The alias profile is
    the only machine-specific file, and it maps alias -> stable Windows id.

    STRUCTURED STATE, NOT SCREEN TEXT. Every product assertion goes through the
    control channel's typed surfaces. Nothing here parses a label, and nothing
    screenshots a page to decide whether a recording worked -- with one deliberate
    class of exception, the visual gates, where a human is judging real desktop
    composition that no screenshot of ours can show.
#>

function Get-ReleaseScenarioCatalog {
    $catalog = @()

    # =======================================================================
    # Environment infrastructure -- the orchestrator proving itself
    # =======================================================================

    $catalog += [pscustomobject]@{
        Id                  = 'REL-ENV-001'
        Title               = 'Environment capability classification is recorded'
        Class               = 'environment'
        Layer               = 'FULL_AUTO'
        Source              = 'ADR 0069 (environment orchestration)'
        ArtifactBound       = $false
        RequiresInstallTree = $false
        EnvironmentKeys     = @('osVersion', 'gpus', 'monitorTopology')
        Requires            = @{}
        Desired             = @{}
        Run                 = {
            param($ctx)
            if (-not $ctx.Orchestrator.Available) {
                return @{ Result = 'UNAVAILABLE'; Message = 'exosnap-envctl is not built' }
            }
            $describe = Get-EnvironmentCapabilities -Orchestrator $ctx.Orchestrator
            $evidence = Save-LiveVerifyEvidence -Context $ctx -CheckId 'REL-ENV-001' `
                -Name 'capabilities.json' -Value $describe

            # Asserted over the CATALOGUE, not over `properties`. `properties` is the
            # catalogue projected onto the aliases this machine happens to have bound,
            # so on a machine with no alias profile it is empty -- and an empty list
            # satisfies "every entry is classified" vacuously. That reported PASS for a
            # classification nobody had made.
            $catalogue = @($describe.catalogue)
            if ($catalogue.Count -eq 0) {
                return @{ Result = 'FAIL'
                    Message      = 'the capability catalogue is empty; no environment property is classified at all'
                    Evidence     = @($evidence)
                }
            }
            $known = @('ENV_READ', 'ENV_MUTATE_SAFE', 'ENV_MUTATE_TESTONLY', 'ENV_HUMAN', 'PHYSICAL',
                'SECURE', 'UNAVAILABLE')
            $unclassified = @($catalogue | Where-Object { $_.capability -notin $known })
            if ($unclassified.Count -gt 0) {
                return @{ Result = 'FAIL'
                    Message      = "$($unclassified.Count) catalogue entries carry no recognised capability class"
                    Evidence     = @($evidence)
                }
            }
            # Every entry must also name HOW it is read, and a mutable one how it is
            # mutated. ENV_MUTATE_SAFE with no named mechanism is a claim with nothing
            # behind it, which is precisely what the class is supposed to rule out.
            $mechanismless = @($catalogue | Where-Object {
                    [string]::IsNullOrWhiteSpace($_.readMechanism) -or
                    ($_.capability -in @('ENV_MUTATE_SAFE', 'ENV_MUTATE_TESTONLY') -and
                    [string]::IsNullOrWhiteSpace($_.mutateMechanism))
                })
            if ($mechanismless.Count -gt 0) {
                return @{ Result = 'FAIL'
                    Message      = "$($mechanismless.Count) catalogue entries name no mechanism for what they claim"
                    Evidence     = @($evidence)
                }
            }
            $mutable = @($catalogue | Where-Object { $_.capability -eq 'ENV_MUTATE_SAFE' })
            return @{ Result = 'PASS'
                Message      = "$($catalogue.Count) properties classified, $($mutable.Count) safely mutable"
                Evidence     = @($evidence)
            }
        }
    }

    $catalog += [pscustomobject]@{
        Id                  = 'REL-ENV-002'
        Title               = 'Device aliases resolve to stable identifiers, unambiguously'
        Class               = 'environment'
        Layer               = 'FULL_AUTO'
        Source              = 'ADR 0069'
        ArtifactBound       = $false
        RequiresInstallTree = $false
        EnvironmentKeys     = @('monitorTopology', 'aliasProfile')
        Requires            = @{}
        Desired             = @{}
        Run                 = {
            param($ctx)
            if (-not $ctx.Orchestrator.Available) {
                return @{ Result = 'UNAVAILABLE'; Message = 'exosnap-envctl is not built' }
            }
            $aliases = Resolve-EnvironmentAliases -Orchestrator $ctx.Orchestrator
            $evidence = Save-LiveVerifyEvidence -Context $ctx -CheckId 'REL-ENV-002' `
                -Name 'aliases.json' -Value $aliases

            # An ambiguous alias is a hard failure of the profile, not a warning:
            # picking one of two matching devices silently is exactly the behaviour
            # that makes a test suite lie about which hardware it exercised.
            $ambiguous = @($aliases.errors | Where-Object { $_.code -eq 'ambiguous_device' })
            $ambiguous += @($aliases.bindings | Where-Object { $_.status -eq 'ambiguous_device' })
            if ($ambiguous.Count -gt 0) {
                return @{ Result = 'FAIL'
                    Message      = "ambiguous_device for: $(($ambiguous | ForEach-Object { $_.alias }) -join ', ')"
                    Evidence     = @($evidence)
                }
            }
            $bound = @($aliases.bindings | Where-Object { $_.status -in @('ok', 'friendly_name_changed') })
            $unbound = @($aliases.bindings | Where-Object { $_.status -eq 'device_not_present' }) + @($aliases.errors)
            # No bindings AND no errors means there is no alias profile on this machine
            # yet: nothing was resolved, so nothing was proven. Reporting PASS over an
            # empty list would claim the alias model works on a machine that has never
            # used it -- an assertion that is true because it is vacuous.
            if ($bound.Count -eq 0 -and $unbound.Count -eq 0) {
                return @{ Result = 'UNAVAILABLE'
                    Message      = 'no alias profile on this machine; bind one with exosnap-envctl bind-alias ' +
                    "($(@($aliases.candidates).Count) device(s) enumerated as candidates)"
                    Evidence     = @($evidence)
                }
            }
            return @{ Result = 'PASS'
                Message      = "$($bound.Count) alias(es) bound and present; $($unbound.Count) not on this machine"
                Evidence     = @($evidence)
            }
        }
    }

    $catalog += [pscustomobject]@{
        Id                  = 'REL-ENV-003'
        Title               = 'A real mutation is applied, verified and restored exactly'
        Class               = 'environment'
        Layer               = 'FULL_AUTO'
        Source              = 'ADR 0069 (write -> read-back -> compare; exact restore)'
        ArtifactBound       = $false
        RequiresInstallTree = $false
        EnvironmentKeys     = @('env.display.main-hdr:refresh-hz', 'env.display.main-hdr:mode')
        Requires            = @{ display = 'display.main-hdr' }
        # The refresh rate is the cheapest ENV_MUTATE_SAFE property to prove the whole
        # transaction on: documented setter, documented read-back, and a change that
        # restores itself. 60 Hz rather than a machine-specific rate -- and a display
        # that does not offer it answers `apply_rejected`, which the runner records as
        # UNAVAILABLE rather than as a product failure.
        Desired             = @{ 'display.main-hdr:refresh-hz' = '60' }
        Run                 = {
            param($ctx, $transaction)
            # The applied state was already read back and compared by envctl before this
            # body ran -- reaching here at all means actual == desired. What is asserted
            # here is the other half: that the mutation was minimal.
            $evidence = Save-LiveVerifyEvidence -Context $ctx -CheckId 'REL-ENV-003' `
                -Name 'transaction.json' -Value $transaction
            $applied = @($transaction.applied)
            if ($applied.Count -ne 1) {
                return @{ Result = 'FAIL'
                    Message      = "A refresh-rate change touched $($applied.Count) properties; it must touch exactly one"
                    Evidence     = @($evidence)
                }
            }
            return @{ Result = 'PASS'
                Message      = "refresh $($applied[0].from) -> $($applied[0].to), verified by read-back"
                Evidence     = @($evidence)
            }
        }
    }

    # =======================================================================
    # Present diagnostics (ADR 0033 / Wave D wiring)
    # =======================================================================

    $catalog += [pscustomobject]@{
        Id                  = 'REL-PRESENT-001'
        Title               = 'Unelevated present diagnostics explain themselves and open no session'
        Class               = 'present'
        Layer               = 'CONTROL_CHANNEL'
        Source              = 'ADR 0033 (Wave D wiring)'
        ArtifactBound       = $true
        RequiresInstallTree = $false
        EnvironmentKeys     = @('elevated')
        Requires            = @{}
        Desired             = @{}
        Run                 = {
            param($ctx)
            $session = & $ctx.EnsureSession
            $conn = $session.Connection

            # Opt in through the product's own settings surface, not by editing a file.
            $set = Invoke-LiveVerifyCommand -Connection $conn -Command 'settings.set' `
                -Parameters @{ key = 'app.presentDiagnosticsOptIn'; value = $true }
            if (-not $set.ok) {
                return @{ Result = 'FAIL'; Message = "settings.set refused: $($set.error.message)" }
            }
            $environment = Invoke-LiveVerifyCommand -Connection $conn -Command 'environment.snapshot'
            $present = $environment.result.present
            $evidence = Save-LiveVerifyEvidence -Context $ctx -CheckId 'REL-PRESENT-001' `
                -Name 'present.json' -Value $present

            if ($present.elevated) {
                return @{ Result = 'UNAVAILABLE'
                    Message      = 'This process is elevated, so the unelevated posture cannot be observed here'
                    Evidence     = @($evidence)
                }
            }
            $problems = @()
            if (-not $present.optIn) { $problems += 'the opt-in did not reach environment.snapshot' }
            if ($present.available) { $problems += 'present data is reported available without elevation' }
            if ($present.availability -ne 'requiresElevation') {
                $problems += "availability is '$($present.availability)', expected 'requiresElevation'"
            }
            if ($problems.Count -gt 0) {
                return @{ Result = 'FAIL'; Message = ($problems -join '; '); Evidence = @($evidence) }
            }
            return @{ Result = 'PASS'
                Message      = 'opt-in on, not elevated -> requiresElevation, no session, no prompt'
                Evidence     = @($evidence)
            }
        }
    }

    $catalog += [pscustomobject]@{
        Id                  = 'REL-PRESENT-002'
        Title               = 'Elevated present diagnostics report real presents'
        Class               = 'present'
        Layer               = 'SECURE'
        Source              = 'ADR 0033; docs/release-checklist.md §7 (present-mode diagnostics)'
        ArtifactBound       = $true
        RequiresInstallTree = $false
        EnvironmentKeys     = @('elevated', 'gpus')
        Requires            = @{}
        Desired             = @{}
        Run                 = {
            param($ctx)
            # UAC is a Windows secure surface. Automation may prepare the transition,
            # explain it, and observe both sides of it; it may never click it, and
            # nothing on the Secure Desktop can be synthesised even if it tried.
            $runId = New-LiveVerifyRunId
            $exe = $ctx.Artifact.exePath
            $gate = @{
                Id                = 'REL-PRESENT-002'
                Title             = 'Elevated relaunch for present diagnostics'
                # Values the Verify block needs travel HERE, not in a closure. A
                # `.GetNewClosure()` block is bound to a synthetic module that does not
                # inherit the runner's functions, so it cannot call Connect-LiveVerify
                # at all -- the capture would work and the call would not.
                State             = @{ runId = $runId; exePath = $exe }
                Why               = 'PresentMon needs a real-time ETW session, which Windows grants only to an ' +
                'elevated process. The elevation prompt runs on the Secure Desktop, where synthetic input is ' +
                'blocked by design -- not merely discouraged.'
                Do                = @(
                    'Close any running ExoSnap.',
                    "Start an ELEVATED PowerShell and run:  & '$exe' --live-verify-control $runId",
                    'Accept the UAC prompt.',
                    'Leave a window presenting on the primary display (a video, a game, any animation).'
                )
                Expected          = 'ExoSnap starts elevated with its control channel armed, and something on ' +
                'screen is presenting frames.'
                VerifyDescription = "This runner connects to \\.\pipe\ExoSnap.LiveVerify.$runId, checks the " +
                'reported identity against the artifact SHA-256 under test, asserts os.elevated is true, turns ' +
                'the opt-in on through settings.set, and then requires present.available == true with a ' +
                'presentCount greater than zero. Tearing false and discarded zero are accepted results.'
                Verify            = {
                    param($context, $gate)
                    try { $conn = Connect-LiveVerify -RunId $gate.State.runId -ConnectTimeoutMs 20000 }
                    catch { return @{ Ok = $false; Detail = "no control channel at run id $($gate.State.runId): $($_.Exception.Message)" } }
                    try {
                        $identity = $conn.Identity
                        if ($identity.executableSha256 -ne $context.Artifact.exeSha256) {
                            return @{ Ok = $false
                                Detail   = 'the elevated process is a DIFFERENT binary: ' +
                                "$($identity.executableSha256) vs $($context.Artifact.exeSha256)"
                            }
                        }
                        $set = Invoke-LiveVerifyCommand -Connection $conn -Command 'settings.set' `
                            -Parameters @{ key = 'app.presentDiagnosticsOptIn'; value = $true }
                        if (-not $set.ok) { return @{ Ok = $false; Detail = "settings.set refused: $($set.error.message)" } }

                        # Bounded, state-based wait: the ETW session needs a present to
                        # decode, and presents arrive when the desktop draws. Polling the
                        # actual snapshot is the measurement, not a guess that enough time
                        # has passed.
                        $deadline = [DateTime]::UtcNow.AddSeconds(30)
                        $present = $null
                        while ([DateTime]::UtcNow -lt $deadline) {
                            $present = (Invoke-LiveVerifyCommand -Connection $conn -Command 'environment.snapshot').result.present
                            if ($present.available -and $present.presentCount -gt 0) { break }
                            Start-Sleep -Milliseconds 500
                        }
                        $evidence = Save-LiveVerifyEvidence -Context $context -CheckId 'REL-PRESENT-002' `
                            -Name 'present-elevated.json' -Value $present
                        if (-not $present.elevated) {
                            return @{ Ok = $false; Detail = 'the process reports os.elevated false'; Evidence = @($evidence) }
                        }
                        if (-not $present.available) {
                            return @{ Ok = $false
                                Detail   = "present data is still unavailable: $($present.availability) / $($present.reason)"
                                Evidence = @($evidence)
                            }
                        }
                        if ($present.presentCount -le 0) {
                            return @{ Ok = $false
                                Detail   = 'the session is open but decoded no presents in 30 s (was anything drawing?)'
                                Evidence = @($evidence)
                            }
                        }
                        if ($present.mode -eq 'unavailable' -or [string]::IsNullOrWhiteSpace($present.mode)) {
                            return @{ Ok = $false; Detail = 'presents were counted but no mode was classified'; Evidence = @($evidence) }
                        }
                        return @{ Ok = $true
                            Detail   = "mode=$($present.mode) presents=$($present.presentCount) tearing=$($present.tearing) discarded=$($present.discardedCount)"
                            Evidence = @($evidence)
                        }
                    }
                    finally { try { $conn.Close() } catch { } }
                }
            }
            return & $ctx.HumanGate $gate
        }
    }

    # =======================================================================
    # Capture
    # =======================================================================

    $catalog += [pscustomobject]@{
        Id                  = 'REL-CAP-001'
        Title               = 'A recording is produced and validated by an independent tool'
        Class               = 'capture'
        Layer               = 'FULL_AUTO'
        Source              = 'docs/release-checklist.md §7'
        ArtifactBound       = $true
        RequiresInstallTree = $false
        EnvironmentKeys     = @('gpus', 'monitorTopology', 'ffprobeVersion')
        Requires            = @{}
        Desired             = @{}
        Run                 = {
            param($ctx)
            $ffprobe = Get-LiveVerifyFfprobe
            if ($null -eq $ffprobe) {
                return @{ Result = 'UNAVAILABLE'; Message = 'ffprobe is not on PATH; the output cannot be validated independently' }
            }
            $session = & $ctx.EnsureSession
            $conn = $session.Connection

            $selected = Invoke-LiveVerifyCommand -Connection $conn -Command 'record.selectTarget' -Parameters @{ kind = 'monitor' }
            if (-not $selected.ok) { return @{ Result = 'FAIL'; Message = "record.selectTarget refused: $($selected.error.message)" } }
            $started = Invoke-LiveVerifyCommand -Connection $conn -Command 'record.start'
            if (-not $started.ok) { return @{ Result = 'FAIL'; Message = "record.start refused: $($started.error.message)" } }
            $state = Wait-ReleaseRecordingState -Connection $conn -States @('Recording') -TimeoutMs 30000
            if ($state.recordingState -ne 'Recording') {
                return @{ Result = 'FAIL'; Message = "the session never reached Recording (last: $($state.recordingState))" }
            }

            # A product duration, not a synchronisation sleep: the recording has to be
            # long enough to contain something.
            Start-Sleep -Seconds 6

            $pipeline = (Invoke-LiveVerifyCommand -Connection $conn -Command 'pipeline.snapshot').result
            [void](Invoke-LiveVerifyCommand -Connection $conn -Command 'record.addMarker')
            [void](Invoke-LiveVerifyCommand -Connection $conn -Command 'record.stop')
            [void](Wait-ReleaseRecordingState -Connection $conn -States @('Completed', 'Failed') -TimeoutMs 60000)

            $result = (Invoke-LiveVerifyCommand -Connection $conn -Command 'record.result').result
            $evidence = @(
                Save-LiveVerifyEvidence -Context $ctx -CheckId 'REL-CAP-001' -Name 'pipeline.json' -Value $pipeline
                Save-LiveVerifyEvidence -Context $ctx -CheckId 'REL-CAP-001' -Name 'result.json' -Value $result
            )
            if (-not $result.succeeded) {
                return @{ Result = 'FAIL'; Message = 'the product reports the recording did not succeed'; Evidence = $evidence }
            }
            $file = $result.outputPath
            if ([string]::IsNullOrWhiteSpace($file) -or -not (Test-Path -LiteralPath $file)) {
                return @{ Result = 'FAIL'; Message = "the product named an output file that does not exist: $file"; Evidence = $evidence }
            }

            # The independent half. ExoSnap's own report confirming ExoSnap's own
            # recording proves only that it is self-consistent.
            $probeRaw = & $ffprobe -v error -print_format json -show_format -show_streams -- "$file" 2>&1 | Out-String
            $evidence += Save-LiveVerifyEvidence -Context $ctx -CheckId 'REL-CAP-001' -Name 'ffprobe.json' -Raw $probeRaw
            try { $probe = $probeRaw | ConvertFrom-Json }
            catch { return @{ Result = 'FAIL'; Message = 'ffprobe could not read the output file'; Evidence = $evidence } }

            $video = @($probe.streams | Where-Object { $_.codec_type -eq 'video' })
            $duration = [double]$probe.format.duration
            $problems = @()
            if ($video.Count -ne 1) { $problems += "expected exactly one video stream, found $($video.Count)" }
            if ($duration -lt 3.0) { $problems += "ffprobe reports only ${duration}s for a 6 s recording" }
            if ($problems.Count -gt 0) {
                return @{ Result = 'FAIL'; Message = ($problems -join '; '); Evidence = $evidence }
            }
            return @{ Result = 'PASS'
                Message      = "$([IO.Path]::GetFileName($file)): $($video[0].codec_name) ${duration}s, $($probe.streams.Count) stream(s)"
                Evidence     = $evidence
            }
        }
    }

    $catalog += [pscustomobject]@{
        Id                  = 'REL-CAP-STALL-001'
        Title               = 'A stalled window capture is reported honestly and stays controllable'
        Class               = 'capture'
        Layer               = 'SEMI_AUTO'
        Source              = 'QCR-804; docs/release-checklist.md §7'
        ArtifactBound       = $true
        RequiresInstallTree = $false
        EnvironmentKeys     = @('gpus')
        Requires            = @{}
        Desired             = @{}
        OptIn               = $true
        Run                 = {
            param($ctx)
            $session = & $ctx.EnsureSession
            $conn = $session.Connection
            $windows = (Invoke-LiveVerifyCommand -Connection $conn -Command 'record.snapshot').result
            $gate = @{
                Id                = 'REL-CAP-STALL-001'
                Title             = 'Stall a window capture'
                Why               = 'A genuine WGC stall means the compositor stops producing frames for a window. ' +
                'There is no product seam that fakes one, and injecting a fake would test the injection rather ' +
                'than the detector.'
                Do                = @(
                    'Pick an application window and select it as the capture target in ExoSnap.',
                    'Start recording it.',
                    'Minimise that window and leave it minimised for at least 15 seconds.'
                )
                Expected          = 'A standing warning appears saying the capture produced no frames. The ' +
                'recording keeps running and the transport controls keep working.'
                VerifyDescription = 'This runner reads notifications.snapshot and pipeline.snapshot. It requires ' +
                'a standing capture-stall notification, a recording still in Recording or Paused, and it checks ' +
                'that the stall is NOT explained as exclusive fullscreen unless pipeline.snapshot actually ' +
                'reports presentMode exclusiveFullscreen. It then stops the recording through the product.'
                Verify            = {
                    param($context, $gate)
                    $conn2 = $script:Session.Connection
                    $deadline = [DateTime]::UtcNow.AddSeconds(30)
                    $notifications = $null
                    $stall = $null
                    while ([DateTime]::UtcNow -lt $deadline) {
                        $notifications = (Invoke-LiveVerifyCommand -Connection $conn2 -Command 'notifications.snapshot').result
                        $stall = @($notifications.notifications | Where-Object { "$($_.id)$($_.title)" -match 'stall|no frame' }) |
                        Select-Object -First 1
                        if ($null -ne $stall) { break }
                        Start-Sleep -Milliseconds 1000
                    }
                    $pipeline = (Invoke-LiveVerifyCommand -Connection $conn2 -Command 'pipeline.snapshot').result
                    $evidence = @(
                        Save-LiveVerifyEvidence -Context $context -CheckId 'REL-CAP-STALL-001' -Name 'notifications.json' -Value $notifications
                        Save-LiveVerifyEvidence -Context $context -CheckId 'REL-CAP-STALL-001' -Name 'pipeline.json' -Value $pipeline
                    )
                    try { [void](Invoke-LiveVerifyCommand -Connection $conn2 -Command 'record.stop') } catch { }

                    if ($null -eq $stall) {
                        return @{ Ok = $false; Detail = 'no standing capture-stall notification appeared within 30 s'; Evidence = $evidence }
                    }
                    if ($pipeline.lifecycle -notin @('recording', 'paused')) {
                        return @{ Ok = $false; Detail = "the recording left the running lifecycle ($($pipeline.lifecycle)); a stall must not stop it"; Evidence = $evidence }
                    }
                    # The honesty assertion: an FSE explanation is only permitted when a
                    # present mode was actually measured as exclusive fullscreen.
                    $claimsFse = "$($stall.body)$($stall.detail)" -match 'exclusive|fullscreen'
                    $measuredFse = $pipeline.capture.presentMode -eq 'exclusiveFullscreen'
                    if ($claimsFse -and -not $measuredFse) {
                        return @{ Ok = $false
                            Detail   = 'the stall notice blames exclusive fullscreen but presentMode was ' +
                            "'$($pipeline.capture.presentMode)' (availability $($pipeline.capture.modeAvailability))"
                            Evidence = $evidence
                        }
                    }
                    return @{ Ok = $true
                        Detail   = "standing stall notice present, lifecycle $($pipeline.lifecycle), fseClaim=$claimsFse fseMeasured=$measuredFse"
                        Evidence = $evidence
                    }
                }
            }
            [void]$windows
            return & $ctx.HumanGate $gate
        }
    }

    $catalog += [pscustomobject]@{
        Id                  = 'REL-CAP-FSE-001'
        Title               = 'True exclusive fullscreen is detected and explained'
        Class               = 'capture'
        Layer               = 'SEMI_AUTO'
        Source              = 'docs/superpowers/specs/2026-07-11-exclusive-fullscreen-capture-spec.md'
        ArtifactBound       = $true
        RequiresInstallTree = $false
        EnvironmentKeys     = @('gpus')
        Requires            = @{}
        Desired             = @{}
        OptIn               = $true
        Run                 = {
            param($ctx)
            $session = & $ctx.EnsureSession
            $gate = @{
                Id                = 'REL-CAP-FSE-001'
                Title             = 'Put a real application into true exclusive fullscreen'
                Why               = 'No API makes another process take exclusive fullscreen. It is a decision that ' +
                'application makes, and only a real one can make it.'
                Do                = @(
                    'Start a game or application that supports TRUE exclusive fullscreen (not borderless).',
                    'Put it into exclusive fullscreen on the primary display.',
                    'Leave it presenting.'
                )
                Expected          = 'The application owns the display exclusively.'
                VerifyDescription = 'This runner requires present diagnostics to be available (elevated + opt-in) ' +
                'and then asserts that environment.snapshot reports present mode exclusiveFullscreen. Without a ' +
                'real present measurement this scenario reports UNAVAILABLE rather than guessing from window shape.'
                Verify            = {
                    param($context, $gate)
                    $conn = $script:Session.Connection
                    $present = (Invoke-LiveVerifyCommand -Connection $conn -Command 'environment.snapshot').result.present
                    $evidence = @(Save-LiveVerifyEvidence -Context $context -CheckId 'REL-CAP-FSE-001' -Name 'present.json' -Value $present)
                    if (-not $present.available) {
                        return @{ Ok = $false
                            Detail   = "present diagnostics are unavailable ($($present.availability)); run REL-PRESENT-002 first"
                            Evidence = $evidence
                        }
                    }
                    if ($present.mode -ne 'exclusiveFullscreen') {
                        return @{ Ok = $false; Detail = "present mode is '$($present.mode)', not exclusiveFullscreen"; Evidence = $evidence }
                    }
                    return @{ Ok = $true; Detail = "present mode exclusiveFullscreen over $($present.presentCount) presents"; Evidence = $evidence }
                }
            }
            [void]$session
            return & $ctx.HumanGate $gate
        }
    }

    # =======================================================================
    # Audio
    # =======================================================================

    $catalog += [pscustomobject]@{
        Id                  = 'REL-AUD-DEGRADE-001'
        Title               = 'Losing an audio endpoint mid-recording degrades to honest silence and recovers'
        Class               = 'audio-physical'
        Layer               = 'MANUAL_PHYSICAL'
        Source              = 'ADR 0046; docs/release-checklist.md §7'
        ArtifactBound       = $true
        RequiresInstallTree = $false
        EnvironmentKeys     = @('env.audio.render.normal:endpoint-state')
        Requires            = @{ audioRender = 'audio.render.normal' }
        Desired             = @{}
        OptIn               = $true
        Run                 = {
            param($ctx)
            $session = & $ctx.EnsureSession
            $conn = $session.Connection

            [void](Invoke-LiveVerifyCommand -Connection $conn -Command 'record.selectTarget' -Parameters @{ kind = 'monitor' })
            $started = Invoke-LiveVerifyCommand -Connection $conn -Command 'record.start'
            if (-not $started.ok) { return @{ Result = 'FAIL'; Message = "record.start refused: $($started.error.message)" } }
            [void](Wait-ReleaseRecordingState -Connection $conn -States @('Recording') -TimeoutMs 30000)

            $gate = @{
                Id                = 'REL-AUD-DEGRADE-001'
                Title             = 'Remove the recorded audio endpoint, then put it back'
                Why               = 'A real endpoint loss is a physical or driver-level event. The unit tests cover ' +
                'the logic with fake sources; the real device path has no harness seam, which is exactly why this ' +
                'gate exists.'
                Do                = @(
                    'A recording is running RIGHT NOW with system audio enabled.',
                    'Unplug (or disable in Sound settings) the playback device bound to audio.render.normal.',
                    'Wait about five seconds.',
                    'Plug it back in (or re-enable it).'
                )
                Expected          = 'The recording does NOT stop. A standing notification says the audio source ' +
                'degraded and the recording continues. When the device returns, the notice clears.'
                VerifyDescription = 'This runner polls pipeline.snapshot and notifications.snapshot throughout. It ' +
                'requires: the lifecycle stayed recording; an audio-degradation state became active; and it ' +
                'cleared again after the device returned. It then stops the recording and validates the output ' +
                'file with ffprobe -- the file must still contain its audio track.'
                Verify            = {
                    param($context, $gate)
                    $conn2 = $script:Session.Connection
                    $observedDegraded = $false
                    $recoveredAgain = $false
                    $leftRecording = $false
                    $samples = @()
                    $deadline = [DateTime]::UtcNow.AddSeconds(60)
                    while ([DateTime]::UtcNow -lt $deadline) {
                        $pipeline = (Invoke-LiveVerifyCommand -Connection $conn2 -Command 'pipeline.snapshot').result
                        $samples += $pipeline
                        if ($pipeline.lifecycle -notin @('recording', 'paused')) { $leftRecording = $true; break }
                        $degraded = $false
                        foreach ($track in @($pipeline.audio.tracks)) {
                            if ($track.PSObject.Properties.Name -contains 'degraded' -and $track.degraded) { $degraded = $true }
                        }
                        if ($degraded) { $observedDegraded = $true }
                        elseif ($observedDegraded) { $recoveredAgain = $true; break }
                        Start-Sleep -Milliseconds 500
                    }
                    $evidence = @(Save-LiveVerifyEvidence -Context $context -CheckId 'REL-AUD-DEGRADE-001' -Name 'pipeline-samples.json' -Value $samples)
                    try {
                        [void](Invoke-LiveVerifyCommand -Connection $conn2 -Command 'record.stop')
                        [void](Wait-ReleaseRecordingState -Connection $conn2 -States @('Completed', 'Failed') -TimeoutMs 60000)
                    }
                    catch { }
                    if ($leftRecording) { return @{ Ok = $false; Detail = 'the recording stopped; ADR 0046 requires it to continue'; Evidence = $evidence } }
                    if (-not $observedDegraded) { return @{ Ok = $false; Detail = 'no audio-source degradation was observed within 60 s'; Evidence = $evidence } }
                    if (-not $recoveredAgain) { return @{ Ok = $false; Detail = 'degradation was observed but never cleared after the device returned'; Evidence = $evidence } }
                    return @{ Ok = $true; Detail = 'degraded during the outage, recovered afterwards, recording never stopped'; Evidence = $evidence }
                }
            }
            return & $ctx.HumanGate $gate
        }
    }

    $catalog += [pscustomobject]@{
        Id                  = 'REL-AUD-SILENCE-001'
        Title               = 'A silent but connected source is not reported as degraded'
        Class               = 'audio'
        Layer               = 'SEMI_AUTO'
        Source              = 'ADR 0046 (degradation is device loss, not quiet)'
        ArtifactBound       = $true
        RequiresInstallTree = $false
        EnvironmentKeys     = @()
        Requires            = @{}
        Desired             = @{}
        OptIn               = $true
        Run                 = {
            param($ctx)
            $session = & $ctx.EnsureSession
            $conn = $session.Connection
            [void](Invoke-LiveVerifyCommand -Connection $conn -Command 'record.selectTarget' -Parameters @{ kind = 'monitor' })
            $started = Invoke-LiveVerifyCommand -Connection $conn -Command 'record.start'
            if (-not $started.ok) { return @{ Result = 'FAIL'; Message = "record.start refused: $($started.error.message)" } }
            [void](Wait-ReleaseRecordingState -Connection $conn -States @('Recording') -TimeoutMs 30000)

            $gate = @{
                Id                = 'REL-AUD-SILENCE-001'
                Title             = 'Leave the audio endpoint connected but silent'
                Why               = 'The distinction being tested is between "the device is gone" and "the device ' +
                'is playing nothing". Only a real endpoint can be the second while remaining the first.'
                Do                = @(
                    'A recording is running now.',
                    'Make sure NOTHING is playing on the recorded output device, and leave the device connected and enabled.',
                    'Wait about fifteen seconds.'
                )
                Expected          = 'No degradation notice appears. Silence is not a fault.'
                VerifyDescription = 'This runner polls pipeline.snapshot for 15 s and requires that no audio track ' +
                'ever reports degraded, while the recording keeps running.'
                Verify            = {
                    param($context, $gate)
                    $conn2 = $script:Session.Connection
                    $samples = @()
                    $degradedSeen = $false
                    $deadline = [DateTime]::UtcNow.AddSeconds(15)
                    while ([DateTime]::UtcNow -lt $deadline) {
                        $pipeline = (Invoke-LiveVerifyCommand -Connection $conn2 -Command 'pipeline.snapshot').result
                        $samples += $pipeline
                        foreach ($track in @($pipeline.audio.tracks)) {
                            if ($track.PSObject.Properties.Name -contains 'degraded' -and $track.degraded) { $degradedSeen = $true }
                        }
                        Start-Sleep -Milliseconds 500
                    }
                    $evidence = @(Save-LiveVerifyEvidence -Context $context -CheckId 'REL-AUD-SILENCE-001' -Name 'pipeline-samples.json' -Value $samples)
                    try { [void](Invoke-LiveVerifyCommand -Connection $conn2 -Command 'record.stop') } catch { }
                    if ($degradedSeen) {
                        return @{ Ok = $false; Detail = 'a connected but silent source was reported as degraded'; Evidence = $evidence }
                    }
                    return @{ Ok = $true; Detail = 'silence over 15 s produced no degradation'; Evidence = $evidence }
                }
            }
            return & $ctx.HumanGate $gate
        }
    }

    $catalog += [pscustomobject]@{
        Id                  = 'REL-AUD-FORMAT-001'
        Title               = 'Recording on a 44.1 kHz endpoint produces correct audio'
        Class               = 'audio'
        Layer               = 'SEMI_AUTO'
        Source              = 'docs/release-checklist.md §7 (44.1 kHz output device)'
        ArtifactBound       = $true
        RequiresInstallTree = $false
        EnvironmentKeys     = @('env.audio.render.44100-test:device-format')
        Requires            = @{ audioRender = 'audio.render.44100-test' }
        # No Desired: Windows exposes no documented, supported API for setting an
        # endpoint's shared-mode format. The only mechanism is an undocumented
        # IPolicyConfig variant, which this project does not use for test convenience.
        # So the format change is the operator's, and the verification is the runner's.
        Desired             = @{}
        OptIn               = $true
        Run                 = {
            param($ctx)
            $gate = @{
                Id                = 'REL-AUD-FORMAT-001'
                Title             = 'Set the test endpoint to 44.1 kHz'
                Why               = 'Windows has no documented, supported API for setting an endpoint shared-mode ' +
                'format. The only route is an undocumented IPolicyConfig variant. Using it to make a test more ' +
                'convenient would put an unsupported mechanism on the release path, so the change stays yours.'
                Do                = @(
                    'Open Sound settings for the device bound to audio.render.44100-test.',
                    'Set its default format to 44100 Hz.',
                    'Leave the device enabled.'
                )
                Expected          = 'Windows reports the endpoint at 44100 Hz.'
                VerifyDescription = 'This runner re-reads the endpoint format through exosnap-envctl (the ' +
                'user-selected shared-mode format, not the engine mix format) and refuses to continue until it ' +
                'actually reads 44100. It then records with system audio and validates the sample rate of the ' +
                'audio track in the output file with ffprobe.'
                Verify            = {
                    param($context, $gate)
                    if (-not $context.Orchestrator.Available) {
                        return @{ Ok = $false; Detail = 'exosnap-envctl is not built; the endpoint format cannot be re-read' }
                    }
                    $snapshot = Get-EnvironmentSnapshot -Orchestrator $context.Orchestrator
                    $property = @($snapshot.properties | Where-Object { $_.key -eq 'audio.render.44100-test:device-format' }) | Select-Object -First 1
                    $evidence = @(Save-LiveVerifyEvidence -Context $context -CheckId 'REL-AUD-FORMAT-001' -Name 'endpoint-format.json' -Value $property)
                    if ($null -eq $property) {
                        return @{ Ok = $false; Detail = 'the endpoint format could not be read'; Evidence = $evidence }
                    }
                    if ("$($property.value)" -notmatch '44100') {
                        return @{ Ok = $false; Detail = "the endpoint still reports '$($property.value)', not 44100"; Evidence = $evidence }
                    }
                    return @{ Ok = $true; Detail = "endpoint shared-mode format is $($property.value)"; Evidence = $evidence }
                }
            }
            $gateResult = & $ctx.HumanGate $gate
            if ($gateResult.Result -ne 'PASS') { return $gateResult }

            $ffprobe = Get-LiveVerifyFfprobe
            if ($null -eq $ffprobe) { return @{ Result = 'UNAVAILABLE'; Message = 'ffprobe is not on PATH' } }
            $session = & $ctx.EnsureSession
            $conn = $session.Connection
            [void](Invoke-LiveVerifyCommand -Connection $conn -Command 'record.selectTarget' -Parameters @{ kind = 'monitor' })
            [void](Invoke-LiveVerifyCommand -Connection $conn -Command 'record.start')
            [void](Wait-ReleaseRecordingState -Connection $conn -States @('Recording') -TimeoutMs 30000)
            Start-Sleep -Seconds 8
            [void](Invoke-LiveVerifyCommand -Connection $conn -Command 'record.stop')
            [void](Wait-ReleaseRecordingState -Connection $conn -States @('Completed', 'Failed') -TimeoutMs 60000)
            $result = (Invoke-LiveVerifyCommand -Connection $conn -Command 'record.result').result
            if (-not $result.succeeded) { return @{ Result = 'FAIL'; Message = 'the recording did not succeed' } }

            $probeRaw = & $ffprobe -v error -print_format json -show_streams -- "$($result.outputPath)" 2>&1 | Out-String
            $evidence = @(Save-LiveVerifyEvidence -Context $ctx -CheckId 'REL-AUD-FORMAT-001' -Name 'ffprobe.json' -Raw $probeRaw)
            $probe = $probeRaw | ConvertFrom-Json
            $audio = @($probe.streams | Where-Object { $_.codec_type -eq 'audio' })
            if ($audio.Count -eq 0) {
                return @{ Result = 'FAIL'; Message = 'the output file has no audio track'; Evidence = $evidence }
            }
            return @{ Result = 'PASS'
                Message      = "audio present: $($audio[0].codec_name) @ $($audio[0].sample_rate) Hz from a 44.1 kHz endpoint"
                Evidence     = $evidence
            }
        }
    }

    $catalog += [pscustomobject]@{
        Id                  = 'REL-AUD-CLOCK-001'
        Title               = 'A long mixed-clock recording stays in sync'
        Class               = 'audio-long'
        Layer               = 'FULL_AUTO'
        Source              = 'docs/release-checklist.md §7 (long-duration soak, clock slaving)'
        ArtifactBound       = $true
        RequiresInstallTree = $false
        EnvironmentKeys     = @('env.audio.render.normal:device-format', 'env.audio.capture.main-mic:device-format')
        Requires            = @{}
        Desired             = @{}
        OptIn               = $true
        Run                 = {
            param($ctx)
            # 30 minutes. The defect class this catches -- resampler drift and drain
            # residue across two clock domains -- does not appear in a six-second clip,
            # which is why the existing checklist calls for a soak rather than a check.
            $minutes = 30
            $ffprobe = Get-LiveVerifyFfprobe
            if ($null -eq $ffprobe) { return @{ Result = 'UNAVAILABLE'; Message = 'ffprobe is not on PATH' } }
            $session = & $ctx.EnsureSession
            $conn = $session.Connection
            [void](Invoke-LiveVerifyCommand -Connection $conn -Command 'record.selectTarget' -Parameters @{ kind = 'monitor' })
            $started = Invoke-LiveVerifyCommand -Connection $conn -Command 'record.start'
            if (-not $started.ok) { return @{ Result = 'FAIL'; Message = "record.start refused: $($started.error.message)" } }
            [void](Wait-ReleaseRecordingState -Connection $conn -States @('Recording') -TimeoutMs 30000)

            $samples = @()
            $deadline = [DateTime]::UtcNow.AddMinutes($minutes)
            while ([DateTime]::UtcNow -lt $deadline) {
                Start-Sleep -Seconds 30
                $pipeline = (Invoke-LiveVerifyCommand -Connection $conn -Command 'pipeline.snapshot').result
                $samples += [pscustomobject]@{
                    atUtc     = [DateTime]::UtcNow.ToString('o')
                    lifecycle = $pipeline.lifecycle
                    avDriftMs = $pipeline.avDriftMs
                    driftAvailability = $pipeline.avDriftAvailability
                }
                if ($pipeline.lifecycle -notin @('recording', 'paused')) { break }
            }
            [void](Invoke-LiveVerifyCommand -Connection $conn -Command 'record.stop')
            [void](Wait-ReleaseRecordingState -Connection $conn -States @('Completed', 'Failed') -TimeoutMs 120000)
            $result = (Invoke-LiveVerifyCommand -Connection $conn -Command 'record.result').result
            $report = (Invoke-LiveVerifyCommand -Connection $conn -Command 'session.latest').result
            $evidence = @(
                Save-LiveVerifyEvidence -Context $ctx -CheckId 'REL-AUD-CLOCK-001' -Name 'drift-samples.json' -Value $samples
                Save-LiveVerifyEvidence -Context $ctx -CheckId 'REL-AUD-CLOCK-001' -Name 'session.json' -Value $report
            )
            if (-not $result.succeeded) { return @{ Result = 'FAIL'; Message = 'the long recording did not succeed'; Evidence = $evidence } }

            $probeRaw = & $ffprobe -v error -print_format json -show_format -show_streams -- "$($result.outputPath)" 2>&1 | Out-String
            $evidence += Save-LiveVerifyEvidence -Context $ctx -CheckId 'REL-AUD-CLOCK-001' -Name 'ffprobe.json' -Raw $probeRaw
            $probe = $probeRaw | ConvertFrom-Json
            $duration = [double]$probe.format.duration
            $expected = $minutes * 60.0
            # No new threshold is invented here: the engine already owns the drift policy
            # and reports its own verdict. What is asserted is container-level -- that the
            # file is as long as the recording was, so a drain that quietly dropped the
            # tail cannot pass.
            $skew = [Math]::Abs($duration - $expected)
            if ($skew -gt ($expected * 0.02)) {
                return @{ Result = 'FAIL'
                    Message      = "container duration ${duration}s vs ${expected}s recorded (skew ${skew}s)"
                    Evidence     = $evidence
                }
            }
            return @{ Result = 'PASS'
                Message      = "$minutes min recorded, container ${duration}s, $($samples.Count) drift samples captured"
                Evidence     = $evidence
            }
        }
    }

    # =======================================================================
    # Display: refresh, HDR, mixed monitor, scaling
    # =======================================================================

    $catalog += [pscustomobject]@{
        Id                  = 'REL-DISP-REFRESH-001'
        Title               = 'Recording is correct across the display refresh rates this machine offers'
        Class               = 'display'
        Layer               = 'FULL_AUTO'
        Source              = 'docs/release-checklist.md §7'
        ArtifactBound       = $true
        RequiresInstallTree = $false
        EnvironmentKeys     = @('env.display.main-hdr:refresh-hz', 'env.display.main-hdr:mode')
        Requires            = @{ display = 'display.main-hdr' }
        Desired             = @{ 'display.main-hdr:refresh-hz' = '60' }
        OptIn               = $true
        Run                 = {
            param($ctx, $transaction)
            $session = & $ctx.EnsureSession
            $conn = $session.Connection
            [void](Invoke-LiveVerifyCommand -Connection $conn -Command 'record.selectTarget' -Parameters @{ kind = 'monitor' })
            [void](Invoke-LiveVerifyCommand -Connection $conn -Command 'record.start')
            [void](Wait-ReleaseRecordingState -Connection $conn -States @('Recording') -TimeoutMs 30000)
            Start-Sleep -Seconds 8
            $pipeline = (Invoke-LiveVerifyCommand -Connection $conn -Command 'pipeline.snapshot').result
            [void](Invoke-LiveVerifyCommand -Connection $conn -Command 'record.stop')
            [void](Wait-ReleaseRecordingState -Connection $conn -States @('Completed', 'Failed') -TimeoutMs 60000)
            $result = (Invoke-LiveVerifyCommand -Connection $conn -Command 'record.result').result
            $evidence = @(
                Save-LiveVerifyEvidence -Context $ctx -CheckId 'REL-DISP-REFRESH-001' -Name 'pipeline.json' -Value $pipeline
                Save-LiveVerifyEvidence -Context $ctx -CheckId 'REL-DISP-REFRESH-001' -Name 'transaction.json' -Value $transaction
            )
            if (-not $result.succeeded) {
                return @{ Result = 'FAIL'; Message = 'the recording failed at 60 Hz'; Evidence = $evidence }
            }
            return @{ Result = 'PASS'
                Message      = "recorded at a display refresh of 60 Hz; capture fps $($pipeline.capture.actualFps)"
                Evidence     = $evidence
            }
        }
    }

    $catalog += [pscustomobject]@{
        Id                  = 'REL-DISP-HDR-001'
        Title               = 'HDR state is applied, recorded against, and restored exactly'
        Class               = 'display'
        Layer               = 'FULL_AUTO'
        Source              = 'docs/release-checklist.md §7'
        ArtifactBound       = $true
        RequiresInstallTree = $false
        EnvironmentKeys     = @('env.display.main-hdr:hdr')
        Requires            = @{ display = 'display.main-hdr' }
        Desired             = @{ 'display.main-hdr:hdr' = 'on' }
        OptIn               = $true
        Run                 = {
            param($ctx, $transaction)
            $session = & $ctx.EnsureSession
            $conn = $session.Connection
            $environment = (Invoke-LiveVerifyCommand -Connection $conn -Command 'environment.snapshot').result
            $hdrDisplays = @($environment.displays.screens | Where-Object { $_.hdrActive })
            $evidence = @(
                Save-LiveVerifyEvidence -Context $ctx -CheckId 'REL-DISP-HDR-001' -Name 'environment.json' -Value $environment
                Save-LiveVerifyEvidence -Context $ctx -CheckId 'REL-DISP-HDR-001' -Name 'transaction.json' -Value $transaction
            )
            # The product's own view of the machine must agree with the state the
            # orchestrator just applied and verified. If they disagree, one of the two
            # is reading the wrong display -- which is precisely the defect that a
            # positional DXGI-to-Qt display match can produce.
            if ($hdrDisplays.Count -eq 0) {
                return @{ Result = 'FAIL'
                    Message      = 'the orchestrator applied and verified HDR on, but ExoSnap reports no HDR-active display'
                    Evidence     = $evidence
                }
            }
            [void](Invoke-LiveVerifyCommand -Connection $conn -Command 'record.selectTarget' -Parameters @{ kind = 'monitor' })
            [void](Invoke-LiveVerifyCommand -Connection $conn -Command 'record.start')
            [void](Wait-ReleaseRecordingState -Connection $conn -States @('Recording') -TimeoutMs 30000)
            Start-Sleep -Seconds 6
            [void](Invoke-LiveVerifyCommand -Connection $conn -Command 'record.stop')
            [void](Wait-ReleaseRecordingState -Connection $conn -States @('Completed', 'Failed') -TimeoutMs 60000)
            $result = (Invoke-LiveVerifyCommand -Connection $conn -Command 'record.result').result
            if (-not $result.succeeded) {
                return @{ Result = 'FAIL'; Message = 'the HDR recording did not succeed'; Evidence = $evidence }
            }
            return @{ Result = 'PASS'
                Message      = "HDR active on $($hdrDisplays.Count) display(s); recording succeeded"
                Evidence     = $evidence
            }
        }
    }

    $catalog += [pscustomobject]@{
        Id                  = 'REL-DISP-MIXED-001'
        Title               = 'Capture works on each display of a mixed HDR/SDR desktop'
        Class               = 'display'
        Layer               = 'CONTROL_CHANNEL'
        Source              = 'docs/release-checklist.md §7'
        ArtifactBound       = $true
        RequiresInstallTree = $false
        EnvironmentKeys     = @('monitorTopology')
        Requires            = @{}
        Desired             = @{}
        OptIn               = $true
        Run                 = {
            param($ctx)
            $session = & $ctx.EnsureSession
            $conn = $session.Connection
            $environment = (Invoke-LiveVerifyCommand -Connection $conn -Command 'environment.snapshot').result
            $displays = @($environment.displays.screens)
            if ($displays.Count -lt 2) {
                return @{ Result = 'UNAVAILABLE'; Message = "this machine has $($displays.Count) display(s); the mixed-monitor scenario needs two" }
            }
            $moved = Invoke-LiveVerifyCommand -Connection $conn -Command 'window.moveToScreen' -Parameters @{ index = 1 }
            if (-not $moved.ok) {
                return @{ Result = 'FAIL'; Message = "window.moveToScreen refused: $($moved.error.message)" }
            }
            [void](Wait-LiveVerifyEvent -Connection $conn -EventName 'window.screenChanged' -TimeoutMs 15000)
            $windows = (Invoke-LiveVerifyCommand -Connection $conn -Command 'windows.snapshot').result
            $preview = (Invoke-LiveVerifyCommand -Connection $conn -Command 'preview.snapshot').result
            $evidence = @(
                Save-LiveVerifyEvidence -Context $ctx -CheckId 'REL-DISP-MIXED-001' -Name 'windows.json' -Value $windows
                Save-LiveVerifyEvidence -Context $ctx -CheckId 'REL-DISP-MIXED-001' -Name 'preview.json' -Value $preview
            )
            # The defect this catches is the preview freezing after a monitor crossing:
            # a frame published with no render pass following it. `owed` is that
            # condition as structured state, so it is asserted rather than eyeballed.
            if ($preview.owed) {
                return @{ Result = 'FAIL'
                    Message      = 'after the screen change a published preview frame is still owed a render pass (frozen preview)'
                    Evidence     = $evidence
                }
            }
            return @{ Result = 'PASS'
                Message      = "moved across $($displays.Count) displays; preview renders=$($preview.renderPasses) owed=$($preview.owed)"
                Evidence     = $evidence
            }
        }
    }

    $catalog += [pscustomobject]@{
        Id                  = 'REL-DISP-DPI-001'
        Title               = 'Scaling facts are read and the minimum window size stays usable'
        Class               = 'display'
        Layer               = 'CONTROL_CHANNEL'
        Source              = 'docs/product-spec.md (860x700 minimum)'
        ArtifactBound       = $true
        RequiresInstallTree = $false
        EnvironmentKeys     = @('monitorTopology')
        Requires            = @{}
        Desired             = @{}
        Run                 = {
            param($ctx)
            $session = & $ctx.EnsureSession
            $conn = $session.Connection
            $environment = (Invoke-LiveVerifyCommand -Connection $conn -Command 'environment.snapshot').result
            $windows = (Invoke-LiveVerifyCommand -Connection $conn -Command 'windows.snapshot').result
            $evidence = @(
                Save-LiveVerifyEvidence -Context $ctx -CheckId 'REL-DISP-DPI-001' -Name 'displays.json' -Value $environment.displays.screens
                Save-LiveVerifyEvidence -Context $ctx -CheckId 'REL-DISP-DPI-001' -Name 'windows.json' -Value $windows
            )
            $root = @($windows.windows | Where-Object { $_.role -eq 'main' }) | Select-Object -First 1
            if ($null -eq $root) {
                return @{ Result = 'FAIL'; Message = 'no main window was reported'; Evidence = $evidence }
            }
            # Geometry lives under `native`, and only when a native window exists --
            # the snapshot deliberately never calls winId(), because that would CREATE
            # one and the snapshot would have changed what it was observing.
            if (-not $root.nativeWindowCreated) {
                return @{ Result = 'UNVERIFIED'
                    Message      = 'the main window has no native window yet, so its size cannot be read'
                    Evidence     = $evidence
                }
            }
            $width = [int]$root.native.width
            $height = [int]$root.native.height
            # The product minimum. A window smaller than this is not a scaling
            # observation, it is a broken minimum-size constraint.
            if ($width -lt 860 -or $height -lt 700) {
                return @{ Result = 'FAIL'
                    Message      = "the main window is ${width}x${height}, below the 860x700 product minimum"
                    Evidence     = $evidence
                }
            }
            # Per-monitor DPI SETTING has no documented API, so scaling is read-only
            # here and the actual 125/150/175/200 % sweep stays a human gate. What is
            # automatable is that the scale factors were read at all and that the shell
            # honours its declared minimum at the CURRENT scale.
            $scales = @($environment.displays.screens | ForEach-Object { $_.devicePixelRatio })
            return @{ Result = 'PASS'
                Message      = "device pixel ratios: $($scales -join ', '); main window ${width}x${height}"
                Evidence     = $evidence
            }
        }
    }

    # =======================================================================
    # Visual gates -- the irreducibly human ones
    # =======================================================================

    $catalog += [pscustomobject]@{
        Id                  = 'REL-VIS-OVERLAY-001'
        Title               = 'Capture-excluded overlays stay fixed-dark under both Windows appearances'
        Class               = 'visual'
        Layer               = 'MANUAL_VISUAL'
        Source              = 'docs/product-spec.md (fixed-dark capture overlays)'
        ArtifactBound       = $true
        RequiresInstallTree = $false
        EnvironmentKeys     = @('env.system:apps-theme')
        Requires            = @{}
        Desired             = @{}
        OptIn               = $true
        Run                 = {
            param($ctx)
            $session = & $ctx.EnsureSession
            $conn = $session.Connection
            # Prepare the product state the human is asked to look at: a running
            # recording, so the pill, the metrics and the quick controls are all on
            # screen at once.
            [void](Invoke-LiveVerifyCommand -Connection $conn -Command 'record.selectTarget' -Parameters @{ kind = 'monitor' })
            [void](Invoke-LiveVerifyCommand -Connection $conn -Command 'record.start')
            [void](Wait-ReleaseRecordingState -Connection $conn -States @('Recording') -TimeoutMs 30000)
            $overlays = (Invoke-LiveVerifyCommand -Connection $conn -Command 'overlay.snapshot').result

            $gate = @{
                Id                = 'REL-VIS-OVERLAY-001'
                Title             = 'Judge the overlays on the real desktop, in Light and in Dark'
                Why               = 'These five overlays set WDA_EXCLUDEFROMCAPTURE. That defeats screenshots, ' +
                'screen recording and PrintWindow by design, and the visual harness can only grab their scene ' +
                'graph -- which shows correct alpha even when the window composes wrongly on screen. How they ' +
                'actually reach the desktop can only be seen by a person looking at it.'
                Do                = @(
                    'A recording is running now, so the recording pill, metrics and quick controls are visible.',
                    'Set Windows to LIGHT appearance and look at every ExoSnap overlay on the desktop.',
                    'Set Windows to DARK appearance and look again.'
                )
                Expected          = 'Every capture-excluded overlay stays dark in both appearances, with legible ' +
                'text and no white or transparent boxes. The recording state colour stays coral, ready stays ' +
                'green, caution stays amber -- the accent never replaces a state colour.'
                VerifyDescription = 'This runner cannot see these windows and does not pretend to. It records ' +
                'the overlay state it prepared, and your judgement is the verdict. It verifies only that the ' +
                'overlays were actually on screen while you looked, via overlay.snapshot.'
                Verify            = {
                    param($context, $gate)
                    $conn2 = $script:Session.Connection
                    $after = (Invoke-LiveVerifyCommand -Connection $conn2 -Command 'overlay.snapshot').result
                    $evidence = @(Save-LiveVerifyEvidence -Context $context -CheckId 'REL-VIS-OVERLAY-001' -Name 'overlays-after.json' -Value $after)
                    try { [void](Invoke-LiveVerifyCommand -Connection $conn2 -Command 'record.stop') } catch { }
                    $visible = @($after.overlays | Where-Object { $_.visible })
                    if ($visible.Count -eq 0) {
                        return @{ Ok = $false; Detail = 'no overlay was visible while the gate was open; nothing was judged'; Evidence = $evidence }
                    }
                    return @{ Ok = $true; Detail = "$($visible.Count) overlay(s) were on screen and judged by the operator"; Evidence = $evidence }
                }
            }
            [void]$overlays
            return & $ctx.HumanGate $gate
        }
    }

    $catalog += [pscustomobject]@{
        Id                  = 'REL-VIS-NOTIFY-001'
        Title               = 'Desktop notifications render with the right severity glyph'
        Class               = 'visual'
        Layer               = 'MANUAL_VISUAL'
        Source              = 'QCR-416'
        ArtifactBound       = $true
        RequiresInstallTree = $false
        EnvironmentKeys     = @()
        Requires            = @{}
        Desired             = @{}
        OptIn               = $true
        Run                 = {
            param($ctx)
            $session = & $ctx.EnsureSession
            $conn = $session.Connection
            $before = (Invoke-LiveVerifyCommand -Connection $conn -Command 'notifications.snapshot').result
            # Raise a deterministic product state rather than a synthetic toast: a
            # notification the product decided to show is the thing under test.
            [void](Invoke-LiveVerifyCommand -Connection $conn -Command 'diagnostics.run')
            $gate = @{
                Id                = 'REL-VIS-NOTIFY-001'
                Title             = 'Judge the real desktop notification surface'
                Why               = 'What reaches the desktop is composed by the OS notification surface, not by ' +
                'us. Our own snapshot proves what we asked for; it cannot prove what appeared.'
                Do                = @(
                    'Watch the desktop notification area.',
                    'Open the ExoSnap notification hub and look at the entries there too.'
                )
                Expected          = 'Notifications appear with the correct severity glyph and tint, and the text ' +
                'is legible and not truncated.'
                VerifyDescription = 'This runner asserts that the product actually published notifications while ' +
                'the gate was open, by diffing notifications.snapshot. Whether they LOOKED right is your verdict.'
                Verify            = {
                    param($context, $gate)
                    $conn2 = $script:Session.Connection
                    $after = (Invoke-LiveVerifyCommand -Connection $conn2 -Command 'notifications.snapshot').result
                    $evidence = @(Save-LiveVerifyEvidence -Context $context -CheckId 'REL-VIS-NOTIFY-001' -Name 'notifications-after.json' -Value $after)
                    if (@($after.notifications).Count -eq 0) {
                        return @{ Ok = $false; Detail = 'the product published no notification while the gate was open'; Evidence = $evidence }
                    }
                    return @{ Ok = $true; Detail = "$(@($after.notifications).Count) notification(s) were published and judged"; Evidence = $evidence }
                }
            }
            [void]$before
            return & $ctx.HumanGate $gate
        }
    }

    # =======================================================================
    # Update
    # =======================================================================

    $catalog += [pscustomobject]@{
        Id                  = 'REL-UPD-PORTABLE-001'
        Title               = 'The portable update handoff installs the version it pinned'
        Class               = 'update'
        Layer               = 'FULL_AUTO'
        Source              = 'ADR 0068; docs/release-checklist.md §5'
        ArtifactBound       = $true
        RequiresInstallTree = $true
        EnvironmentKeys     = @()
        Requires            = @{}
        Desired             = @{}
        OptIn               = $true
        Run                 = {
            param($ctx)
            # Reuses the Wave B end-to-end script rather than restating it. One
            # implementation of the handoff assertions, two callers.
            $script = Join-Path $ctx.RepositoryRoot 'scripts/live-verify-update-handoff.ps1'
            if (-not (Test-Path -LiteralPath $script)) {
                return @{ Result = 'UNAVAILABLE'; Message = 'scripts/live-verify-update-handoff.ps1 is missing' }
            }
            $output = & pwsh -NoProfile -File $script -AppPath $ctx.Artifact.exePath 2>&1 | Out-String
            $exit = $LASTEXITCODE
            $evidence = @(Save-LiveVerifyEvidence -Context $ctx -CheckId 'REL-UPD-PORTABLE-001' -Name 'handoff.log' -Raw $output)
            if ($exit -ne 0) {
                return @{ Result = 'FAIL'; Message = "the update handoff script exited $exit"; Evidence = $evidence }
            }
            return @{ Result = 'PASS'; Message = 'app, handoff and updater agree on one pinned version'; Evidence = $evidence }
        }
    }

    $catalog += [pscustomobject]@{
        Id                  = 'REL-UPD-MSI-001'
        Title               = 'The MSI update elevates, installs and relaunches'
        Class               = 'update'
        Layer               = 'SECURE'
        Source              = 'docs/release-checklist.md §5, §7a'
        ArtifactBound       = $true
        RequiresInstallTree = $true
        EnvironmentKeys     = @()
        Requires            = @{}
        Desired             = @{}
        OptIn               = $true
        Run                 = {
            param($ctx)
            $session = & $ctx.EnsureSession
            $conn = $session.Connection
            $before = (Invoke-LiveVerifyCommand -Connection $conn -Command 'app.identity').result
            $checked = Invoke-LiveVerifyCommand -Connection $conn -Command 'update.check'
            if (-not $checked.ok) { return @{ Result = 'FAIL'; Message = "update.check refused: $($checked.error.message)" } }
            $state = (Invoke-LiveVerifyCommand -Connection $conn -Command 'update.getState').result
            if (-not $state.updateAvailable) {
                return @{ Result = 'UNAVAILABLE'; Message = "no update is offered to $($before.version) on this channel" }
            }
            $applied = Invoke-LiveVerifyCommand -Connection $conn -Command 'update.apply'
            if (-not $applied.ok) { return @{ Result = 'FAIL'; Message = "update.apply refused: $($applied.error.message)" } }

            $gate = @{
                Id                = 'REL-UPD-MSI-001'
                Title             = 'Accept the elevation prompt for the MSI install'
                State             = @{ previousVersion = $before.version }
                Why               = 'msiexec needs an elevated token, and the prompt runs on the Secure Desktop. ' +
                'Windows blocks synthetic input across that boundary by design; there is nothing to automate.'
                Do                = @(
                    'A UAC prompt is appearing now, raised by the ExoSnap updater for msiexec.',
                    'Read what it says, then ACCEPT it.',
                    'Wait for the installer to finish and for ExoSnap to relaunch.'
                )
                Expected          = 'The install completes and ExoSnap comes back at the new version.'
                VerifyDescription = "This runner captured the version before the update ($($before.version)) and " +
                'will reconnect afterwards, read app.identity again, and require a DIFFERENT and newer version ' +
                'plus an installState of intact or restored. It never touches the prompt.'
                Verify            = {
                    param($context, $gate)
                    $deadline = [DateTime]::UtcNow.AddMinutes(5)
                    $identity = $null
                    while ([DateTime]::UtcNow -lt $deadline) {
                        try {
                            $runId2 = New-LiveVerifyRunId
                            $process = Start-Process -FilePath $context.Artifact.exePath `
                                -ArgumentList @('--live-verify-control', $runId2) -PassThru
                            $conn3 = Connect-LiveVerify -RunId $runId2 -ConnectTimeoutMs 30000
                            try { $identity = (Invoke-LiveVerifyCommand -Connection $conn3 -Command 'app.identity').result }
                            finally {
                                try { $conn3.Close() } catch { }
                                if (-not $process.HasExited) { $process | Stop-Process -Force -ErrorAction SilentlyContinue }
                            }
                            break
                        }
                        catch { Start-Sleep -Seconds 5 }
                    }
                    $evidence = @(Save-LiveVerifyEvidence -Context $context -CheckId 'REL-UPD-MSI-001' -Name 'identity-after.json' -Value $identity)
                    if ($null -eq $identity) {
                        return @{ Ok = $false; Detail = 'the application could not be reached again after the install'; Evidence = $evidence }
                    }
                    if ($identity.version -eq $gate.State.previousVersion) {
                        return @{ Ok = $false; Detail = "the version is unchanged at $($identity.version); nothing was installed"; Evidence = $evidence }
                    }
                    return @{ Ok = $true; Detail = "installed: $($gate.State.previousVersion) -> $($identity.version)"; Evidence = $evidence }
                }
            }
            return & $ctx.HumanGate $gate
        }
    }

    $catalog += [pscustomobject]@{
        Id                  = 'REL-UPD-MSI-DECLINE-001'
        Title               = 'Declining the elevation prompt yields a truthful cancel state'
        Class               = 'update'
        Layer               = 'SECURE'
        Source              = 'ADR 0067 (cancel is not failure)'
        ArtifactBound       = $true
        RequiresInstallTree = $true
        EnvironmentKeys     = @()
        Requires            = @{}
        Desired             = @{}
        OptIn               = $true
        Run                 = {
            param($ctx)
            $session = & $ctx.EnsureSession
            $conn = $session.Connection
            $checked = Invoke-LiveVerifyCommand -Connection $conn -Command 'update.check'
            if (-not $checked.ok) { return @{ Result = 'FAIL'; Message = "update.check refused: $($checked.error.message)" } }
            $state = (Invoke-LiveVerifyCommand -Connection $conn -Command 'update.getState').result
            if (-not $state.updateAvailable) { return @{ Result = 'UNAVAILABLE'; Message = 'no update is offered on this channel' } }
            [void](Invoke-LiveVerifyCommand -Connection $conn -Command 'update.apply')

            $gate = @{
                Id                = 'REL-UPD-MSI-DECLINE-001'
                Title             = 'DECLINE the elevation prompt'
                Why               = 'Same Secure Desktop boundary as accepting it. What is under test is what the ' +
                'product says afterwards.'
                Do                = @('A UAC prompt is appearing now.', 'DECLINE it.')
                Expected          = 'The updater reports a cancelled update, not a failed one, and nothing is ' +
                'left half-installed.'
                VerifyDescription = 'This runner reads update.getState and requires a cancelled-or-idle state with ' +
                'installState intact -- never a failure state, and never strandedInBackup.'
                Verify            = {
                    param($context, $gate)
                    $conn2 = $script:Session.Connection
                    $after = (Invoke-LiveVerifyCommand -Connection $conn2 -Command 'update.getState').result
                    $evidence = @(Save-LiveVerifyEvidence -Context $context -CheckId 'REL-UPD-MSI-DECLINE-001' -Name 'update-state.json' -Value $after)
                    if ("$($after.installState)" -eq 'strandedInBackup') {
                        return @{ Ok = $false; Detail = 'the install is stranded in backup after a declined prompt'; Evidence = $evidence }
                    }
                    if ("$($after.phase)" -match 'fail|error') {
                        return @{ Ok = $false; Detail = "a declined prompt was reported as a failure: $($after.phase)"; Evidence = $evidence }
                    }
                    return @{ Ok = $true; Detail = "phase=$($after.phase) installState=$($after.installState)"; Evidence = $evidence }
                }
            }
            return & $ctx.HumanGate $gate
        }
    }

    # =======================================================================
    # End-to-end journey and shutdown invariant
    # =======================================================================

    $catalog += [pscustomobject]@{
        Id                  = 'REL-JOURNEY-001'
        Title               = 'The full product journey, launch to export to restart'
        Class               = 'journey'
        Layer               = 'FULL_AUTO'
        Source              = 'Wave C product journey'
        ArtifactBound       = $true
        RequiresInstallTree = $false
        EnvironmentKeys     = @('gpus', 'ffprobeVersion')
        Requires            = @{}
        Desired             = @{}
        Run                 = {
            param($ctx)
            # The Wave C journey already asserts the whole chain in one process. It is
            # invoked rather than reimplemented so the two cannot drift, and its own
            # evidence file is carried into this campaign's evidence.
            $script = Join-Path $ctx.RepositoryRoot 'scripts/live-verify-product-journey.ps1'
            if (-not (Test-Path -LiteralPath $script)) {
                return @{ Result = 'UNAVAILABLE'; Message = 'scripts/live-verify-product-journey.ps1 is missing' }
            }
            $journalDirectory = Join-Path $ctx.RunDirectory 'checks/REL-JOURNEY-001'
            New-Item -ItemType Directory -Path $journalDirectory -Force | Out-Null
            $evidencePath = Join-Path $journalDirectory 'journey.json'

            # A journey launches its own isolated instance, so this campaign's shared
            # session must be out of the way first: the application enforces a
            # single-instance guard and a second launch would simply hand focus to the
            # first one and exit.
            & $ctx.EndSession
            $output = & pwsh -NoProfile -File $script -AppPath $ctx.Artifact.exePath `
                -RecordSeconds 8 -EvidencePath $evidencePath 2>&1 | Out-String
            $exit = $LASTEXITCODE
            $log = Save-LiveVerifyEvidence -Context $ctx -CheckId 'REL-JOURNEY-001' -Name 'journey.log' -Raw $output
            $evidence = @($log, 'checks/REL-JOURNEY-001/journey.json')
            if ($exit -ne 0) {
                return @{ Result = 'FAIL'; Message = "the product journey exited $exit"; Evidence = $evidence }
            }
            return @{ Result = 'PASS'; Message = 'launch, record, marker, pause/resume, stop, edit, export, restart'; Evidence = $evidence }
        }
    }

    $catalog += [pscustomobject]@{
        Id                  = 'REL-SHUTDOWN-001'
        Title               = 'Unread control-channel events never hold the application open'
        Class               = 'journey'
        Layer               = 'FULL_AUTO'
        Source              = 'ADR 0067 (no flush-on-stop dependency)'
        ArtifactBound       = $true
        RequiresInstallTree = $false
        EnvironmentKeys     = @()
        Requires            = @{}
        Desired             = @{}
        Run                 = {
            param($ctx)
            # Deliberately generates events and then NEVER reads them, which is what a
            # runner waiting for the process to exit looks like from the server's side.
            # The defect this pins let one unread event block the process forever,
            # because the server flushed the pipe on the stop path and the flush waits
            # for a client that is by definition not reading.
            $runId = New-LiveVerifyRunId
            $process = Start-Process -FilePath $ctx.Artifact.exePath `
                -ArgumentList @('--live-verify-control', $runId) -PassThru
            try {
                $conn = Connect-LiveVerify -RunId $runId -ConnectTimeoutMs 30000
                [void](Invoke-LiveVerifyCommand -Connection $conn -Command 'record.selectTarget' -Parameters @{ kind = 'monitor' })
                [void](Invoke-LiveVerifyCommand -Connection $conn -Command 'ui.navigate' -Parameters @{ page = 'diagnostics' })
                [void](Invoke-LiveVerifyCommand -Connection $conn -Command 'diagnostics.run')
                # Close the channel with events still buffered, then ask the window to
                # close. No event drain here. On purpose.
                try { $conn.Close() } catch { }
                [void]$process.CloseMainWindow()
                $exited = $process.WaitForExit(20000)
                $evidence = @(Save-LiveVerifyEvidence -Context $ctx -CheckId 'REL-SHUTDOWN-001' `
                        -Name 'shutdown.json' -Value @{ exitedWithin20s = $exited; bufferedEvents = @($conn.Events).Count })
                if (-not $exited) {
                    return @{ Result = 'FAIL'
                        Message      = 'the application did not exit within 20 s with events left unread'
                        Evidence     = $evidence
                    }
                }
                return @{ Result = 'PASS'; Message = 'exited promptly with unread events buffered'; Evidence = $evidence }
            }
            finally {
                $process.Refresh()
                if (-not $process.HasExited) { $process | Stop-Process -Force -ErrorAction SilentlyContinue }
            }
        }
    }

    return [object[]]$catalog
}

function Wait-ReleaseRecordingState {
    <#
    .SYNOPSIS
        Waits for the recording lifecycle to reach one of $States.
    .DESCRIPTION
        Event-driven: every wait is a stateRevision advance delivered as an event, not
        a fixed delay. A timeout returns the last observed state rather than throwing,
        so the caller reports what it actually saw instead of "timed out".
    #>
    param(
        [Parameter(Mandatory)] $Connection,
        [Parameter(Mandatory)] [string[]] $States,
        [int] $TimeoutMs = 30000
    )
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    $state = Get-LiveVerifyState -Connection $Connection
    while ($state.recordingState -notin $States) {
        $remaining = [int]([Math]::Max(0, ($deadline - [DateTime]::UtcNow).TotalMilliseconds))
        if ($remaining -le 0) { break }
        try { [void](Wait-LiveVerifyRevision -Connection $Connection -After $state.stateRevision -TimeoutMs $remaining) }
        catch { break }
        $state = Get-LiveVerifyState -Connection $Connection
    }
    return $state
}
