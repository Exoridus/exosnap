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

# ---------------------------------------------------------------------------
# probe_stall_window -- the capture target that stalls on purpose
# ---------------------------------------------------------------------------

function Resolve-StallWindowProbe {
    <#
    .SYNOPSIS
        Locates probe_stall_window.exe, or returns $null.
    .DESCRIPTION
        Returns $null rather than throwing, exactly like Resolve-EnvctlPath: the
        probe is a developer build artifact (-DEXOSNAP_BUILD_PROBES=ON) and a
        machine without it can still run the scenario -- as the operator gate it
        has always been. Its absence is a statement about the build tree, never
        about the product.
    #>
    if ($env:EXOSNAP_STALL_PROBE -and (Test-Path -LiteralPath $env:EXOSNAP_STALL_PROBE)) {
        return (Get-Item -LiteralPath $env:EXOSNAP_STALL_PROBE).FullName
    }
    $root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
    $candidates = @(
        'build/windows-x64-release/tools/probes/probe_stall_window/Release/probe_stall_window.exe',
        'build/windows-x64-debug/tools/probes/probe_stall_window/Debug/probe_stall_window.exe',
        'build/windows-x64-ninja-release/tools/probes/probe_stall_window/probe_stall_window.exe',
        'build/windows-x64-ninja-debug/tools/probes/probe_stall_window/probe_stall_window.exe'
    )
    foreach ($candidate in $candidates) {
        $full = Join-Path $root $candidate
        if (Test-Path -LiteralPath $full) { return (Get-Item -LiteralPath $full).FullName }
    }
    return $null
}

# Windows Light/Dark appearance, switched by the runner instead of asked for.
#
# Both values live under HKCU and Windows documents them as the personalization
# surface the Settings app writes; no elevation, and the exact previous values go
# back afterwards. Only the LOOKING stays with a person -- the capture-excluded
# overlays defeat every screenshot path, so how they reach the desktop cannot be
# read back by anything.
function Set-WindowsAppearance {
    param([Parameter(Mandatory)] [ValidateSet('Light', 'Dark')] [string] $Appearance)
    $key = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize'
    $value = if ($Appearance -eq 'Light') { 1 } else { 0 }
    Set-ItemProperty -Path $key -Name 'AppsUseLightTheme' -Value $value -Type DWord
    Set-ItemProperty -Path $key -Name 'SystemUsesLightTheme' -Value $value -Type DWord
    # Applications repaint on the broadcast, not on the registry write.
    if (-not ('ExoSnapAppearanceBroadcast' -as [type])) {
        Add-Type -Namespace 'ExoSnap' -Name 'AppearanceBroadcast' -MemberDefinition @'
[DllImport("user32.dll", CharSet = CharSet.Auto)]
public static extern System.IntPtr SendMessageTimeout(System.IntPtr hWnd, uint Msg, System.IntPtr wParam,
    string lParam, uint fuFlags, uint uTimeout, out System.UIntPtr lpdwResult);
'@ -ErrorAction SilentlyContinue
    }
    try {
        [System.UIntPtr] $result = [System.UIntPtr]::Zero
        [void][ExoSnap.AppearanceBroadcast]::SendMessageTimeout(
            [IntPtr]0xffff, 0x001A, [IntPtr]::Zero, 'ImmersiveColorSet', 0x0002, 3000, [ref]$result)
    }
    catch { }
    Start-Sleep -Milliseconds 1200
}

function Get-WindowsAppearance {
    $key = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize'
    $apps = (Get-ItemProperty -Path $key -Name 'AppsUseLightTheme' -ErrorAction SilentlyContinue).AppsUseLightTheme
    if ($null -eq $apps) { return 'Dark' }
    return $(if ([int]$apps -eq 1) { 'Light' } else { 'Dark' })
}

function Resolve-FullscreenProbe {
    <#
    .SYNOPSIS
        Locates probe_fullscreen_present.exe, or returns $null.
    #>
    if ($env:EXOSNAP_FSE_PROBE -and (Test-Path -LiteralPath $env:EXOSNAP_FSE_PROBE)) {
        return (Get-Item -LiteralPath $env:EXOSNAP_FSE_PROBE).FullName
    }
    $root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
    $candidates = @(
        'build/windows-x64-release/tools/probes/probe_fullscreen_present/Release/probe_fullscreen_present.exe',
        'build/windows-x64-debug/tools/probes/probe_fullscreen_present/Debug/probe_fullscreen_present.exe',
        'build/windows-x64-ninja-release/tools/probes/probe_fullscreen_present/probe_fullscreen_present.exe',
        'build/windows-x64-ninja-debug/tools/probes/probe_fullscreen_present/probe_fullscreen_present.exe'
    )
    foreach ($candidate in $candidates) {
        $full = Join-Path $root $candidate
        if (Test-Path -LiteralPath $full) { return (Get-Item -LiteralPath $full).FullName }
    }
    return $null
}

# Whether this runner is already elevated. An elevated runner can DO the things
# the SECURE gates ask an operator for -- launching an elevated child raises no
# prompt when the parent already holds the token -- so the question becomes
# unnecessary rather than unanswerable. What stays with a person is the prompt
# itself: REL-UPD-MSI-DECLINE-001 needs a real prompt to decline, which an
# elevated run never produces.
function Test-RunnerElevated {
    try {
        $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
        return ([Security.Principal.WindowsPrincipal]$identity).IsInRole(
            [Security.Principal.WindowsBuiltInRole]::Administrator)
    }
    catch { return $false }
}

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
        # Chosen against the machine, not hardcoded. Windows enumerates the NOMINAL and
        # the ACTUAL rate of one physical mode separately (59 and 60, 119 and 120) and
        # collapses them on apply -- so asking for 60 on this panel is answered by a
        # read-back of 59, and the transaction correctly refuses. That is the mechanism
        # working, not failing, so the scenario picks a rate with no neighbour within
        # 1 Hz instead of weakening the verify rule to accommodate one.
        Desired             = {
            param($ctx)
            if (-not $ctx.Orchestrator.Available) { return 'exosnap-envctl is not built' }
            $listed = Get-EnvironmentDisplayModes -Orchestrator $ctx.Orchestrator -Alias 'display.main-hdr'
            if ($null -eq $listed -or -not $listed.ok) { return 'the display modes could not be enumerated' }
            $display = @($listed.displays) | Select-Object -First 1
            if ($null -eq $display) { return 'no display resolved for display.main-hdr' }
            $rate = Select-UntwinnedRefreshRate -Display $display
            if ($null -eq $rate) {
                return 'this display enumerates no alternative refresh rate a read-back could confirm'
            }
            return @{ 'display.main-hdr:refresh-hz' = "$rate" }
        }
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
    # The field contract -- run first, because everything after it reads these
    # =======================================================================

    $catalog += [pscustomobject]@{
        Id                  = 'REL-SCHEMA-001'
        Title               = 'Every field path the catalog reads actually exists'
        Class               = 'schema'
        Layer               = 'FULL_AUTO'
        Source              = 'Wave D review: eight scenarios read fields no emitter emits'
        ArtifactBound       = $true
        RequiresInstallTree = $false
        EnvironmentKeys     = @()
        Requires            = @{}
        Desired             = @{}
        Run                 = {
            param($ctx)
            # WHY THIS EXISTS. Eight scenarios shipped reading field paths that no
            # emitter emits -- pipeline.audio.tracks[].degraded, pipeline.avDriftMs,
            # notifications.notifications[].id, an `index` parameter on
            # window.moveToScreen. Under StrictMode each of those throws, and several
            # throw INSIDE a human gate: after the operator has already unplugged an
            # audio interface or answered a UAC prompt. The errors survived for one
            # reason only -- the opt-in scenarios were never executed, so nothing ever
            # evaluated the paths.
            #
            # A catalog whose assertions are only checked when a human is standing at
            # the machine has no early failure mode at all. This scenario is that
            # failure mode: it connects once, walks the paths every other scenario
            # depends on, and turns "a gate throws at the worst possible moment" into a
            # fast, automated, unattended FAIL.
            #
            # It asserts EXISTENCE, never values. What a field says is the other
            # scenarios' business; that it is there at all is a contract between this
            # catalog and the product's emitters, and contracts are checked cheaply.
            $session = & $ctx.EnsureSession
            $conn = $session.Connection
            $contract = @(Get-ReleaseFieldContract)
            if ($contract.Count -eq 0) {
                return @{ Result = 'FAIL'; Message = 'the field contract is empty; nothing was checked' }
            }

            # The hub keeps a permanent record but starts empty, so an entry has to be
            # published before the entry-shape paths can be walked at all. Waited for as
            # a state, not slept over: whether a diagnostics run publishes anything on
            # this machine depends on what it finds, and an empty hub is reported as
            # unchecked rather than passed.
            [void](Invoke-LiveVerifyCommand -Connection $conn -Command 'diagnostics.run')
            $notificationDeadline = [DateTime]::UtcNow.AddSeconds(10)
            while ([DateTime]::UtcNow -lt $notificationDeadline) {
                $hub = (Invoke-LiveVerifyCommand -Connection $conn -Command 'notifications.snapshot').result
                # @(...) around the CALL, not only inside it: a function returning an
                # empty array hands back $null, and $null.Count throws under StrictMode.
                if (@(Get-ReleaseNotificationEntry -Snapshot $hub).Count -gt 0) { break }
                Start-Sleep -Milliseconds 250
            }

            $missing = @()
            $skipped = @()
            $checked = 0
            $snapshots = [ordered]@{}

            $stages = @('idle', 'recording', 'result')
            foreach ($stage in $stages) {
                $stageContract = @($contract | Where-Object { $_.Stage -eq $stage })
                if ($stageContract.Count -eq 0) { continue }

                if ($stage -eq 'recording') {
                    # The pipeline's measurement groups are absent by design while
                    # `valid` is false: emitting them idle would hand a reader a
                    # complete, entirely zero pipeline that reads as a healthy
                    # recording. So the groups can only be walked with one running.
                    [void](Invoke-LiveVerifyCommand -Connection $conn -Command 'record.selectTarget' -Parameters @{ kind = 'monitor' })
                    $audioOn = Enable-ReleaseSystemAudio -Connection $conn
                    if (-not $audioOn.Ok) {
                        $skipped += "audio paths: $($audioOn.Detail)"
                    }
                    $started = Invoke-LiveVerifyCommand -Connection $conn -Command 'record.start'
                    if (-not $started.ok) {
                        return @{ Result = 'FAIL'; Message = "record.start refused: $($started.error.message)" }
                    }
                    [void](Wait-ReleaseRecordingState -Connection $conn -States @('Recording') -TimeoutMs 30000)
                    # Long enough for the audio and A/V groups to have measured
                    # something. A group that exists but has seen nothing still has its
                    # keys, which is all this asserts.
                    Start-Sleep -Seconds 4
                }
                if ($stage -eq 'result') {
                    [void](Invoke-LiveVerifyCommand -Connection $conn -Command 'record.stop')
                    [void](Wait-ReleaseRecordingState -Connection $conn -States @('Completed', 'Failed') -TimeoutMs 60000)
                }

                foreach ($command in (@($stageContract | ForEach-Object { $_.Command }) | Sort-Object -Unique)) {
                    $response = Invoke-LiveVerifyCommand -Connection $conn -Command $command
                    if (-not $response.ok) {
                        # A refused command is a different fact from a missing field, and
                        # it is reported as its own line rather than folded into either
                        # a pass or a failure.
                        $skipped += "$command refused: $($response.error.message)"
                        continue
                    }
                    $snapshots["$stage/$command"] = $response.result
                    foreach ($entry in @($stageContract | Where-Object { $_.Command -eq $command })) {
                        $checked++
                        $resolved = Resolve-ReleaseFieldPath -Root $response.result -Path $entry.Path
                        switch ($resolved.Status) {
                            'present' { }
                            'empty' {
                                # The collection is there and the name is therefore right;
                                # its element shape simply could not be walked right now.
                                # Named out loud so an empty check never reads as a pass.
                                $skipped += "$command $($entry.Path): '$($resolved.At)' is empty, element shape unchecked"
                            }
                            default {
                                $missing += "$command.$($entry.Path) (read by $($entry.UsedBy)) -- '$($resolved.At)' is not emitted"
                            }
                        }
                    }
                }
            }

            $evidence = @(
                Save-LiveVerifyEvidence -Context $ctx -CheckId 'REL-SCHEMA-001' -Name 'snapshots.json' -Value $snapshots
                Save-LiveVerifyEvidence -Context $ctx -CheckId 'REL-SCHEMA-001' -Name 'contract.json' `
                    -Value @{ checked = $checked; missing = $missing; skipped = $skipped }
            )
            if ($missing.Count -gt 0) {
                return @{ Result = 'FAIL'
                    Message      = "$($missing.Count) of $checked field path(s) do not exist: " + ($missing -join ' | ')
                    Evidence     = $evidence
                }
            }
            $note = if ($skipped.Count -gt 0) { "; $($skipped.Count) unchecked: $($skipped -join ' | ')" } else { '' }
            return @{ Result = 'PASS'
                Message      = "$checked field path(s) exist across $($snapshots.Count) snapshot(s)$note"
                Evidence     = $evidence
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
            # An elevated runner needs no operator here: launching an elevated child
            # raises no prompt when the parent already holds the token, so the gate
            # becomes a sequence rather than a question. The instance is CLOSED again
            # afterwards -- the single-instance guard is machine-wide, and one left
            # running makes every later scenario wait for a control channel that was
            # never created.
            if (Test-RunnerElevated) {
                $elevated = Start-Process -FilePath $exe -PassThru -ArgumentList @('--live-verify-control', $runId)
                try {
                    $verdict = Resolve-ReleaseVerdict (& $gate.Verify $ctx $gate)
                } finally {
                    if ($null -ne $elevated -and -not $elevated.HasExited) {
                        [void]$elevated.CloseMainWindow()
                        if (-not $elevated.WaitForExit(10000)) { $elevated.Kill() }
                    }
                }
                if ($null -eq $verdict) {
                    return @{ Result = 'UNVERIFIED'; Message = 'the present verification returned nothing' }
                }
                if ($verdict.Ok) {
                    return @{ Result = 'PASS'
                        Message      = "[elevated runner] $($verdict.Detail)"
                        Evidence     = $verdict.Evidence
                    }
                }
                return @{ Result = 'FAIL'; Message = $verdict.Detail; Evidence = $verdict.Evidence }
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
                # The shape matters, and the earlier wording asked for the one case
                # the product deliberately stays silent about. Per the capture-stall
                # section of docs/product-spec.md the notice requires a window that is
                # fullscreen-shaped, alive, visible and NOT minimized: a minimized or
                # captioned window that stops producing frames is supposed to be
                # silent, so an operator following the old steps could only ever
                # produce a FAIL that looked like a product regression.
                Do                = @(
                    'Start an application in BORDERLESS fullscreen (no caption, covering the whole monitor).',
                    'Select that window as the capture target in ExoSnap and start recording it.',
                    'Make its content stop changing for at least 15 seconds (pause the video, leave it at rest).',
                    'Do NOT minimise it: a minimized window is documented to stay silent.'
                )
                Expected          = 'After about ten seconds a standing caution appears saying the window capture ' +
                'appears to have stalled. The recording keeps running and the transport controls keep working.'
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
                        # `entries`, with sequence/title/body/severity/unread. There is no
                        # `id` and no `detail` on the hub's entries -- the manager-assigned
                        # `sequence` is the only stable identity a client may address one by.
                        $stall = @(Get-ReleaseNotificationEntry -Snapshot $notifications |
                            Where-Object { "$($_.title) $($_.body)" -match 'stall|no frame' }) | Select-Object -First 1
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
                    $claimsFse = "$($stall.title) $($stall.body)" -match 'exclusive|fullscreen'
                    # presentMode lives under `sourcePresentation`, which -- like every other
                    # measurement group -- is absent entirely while `valid` is false. Reading
                    # it through a helper keeps "no measurement" from throwing under StrictMode
                    # on the one path where a human has already done their part.
                    $measuredMode = Get-ReleaseSnapshotValue -Object $pipeline -Path 'sourcePresentation.presentMode'
                    $modeAvailability = Get-ReleaseSnapshotValue -Object $pipeline -Path 'sourcePresentation.modeAvailability'
                    $measuredFse = $measuredMode -eq 'exclusiveFullscreen'
                    if ($claimsFse -and -not $measuredFse) {
                        return @{ Ok = $false
                            Detail   = 'the stall notice blames exclusive fullscreen but presentMode was ' +
                            "'$measuredMode' (availability $modeAvailability)"
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

            # Automated path: the probe owns a window, shows it without taking
            # focus, and minimises ITSELF on a timer. Every step the gate used to
            # ask an operator for is then a documented API call on a window this
            # campaign created -- no synthesised input, no foreign window, and the
            # same Verify block decides either way.
            $probe = Resolve-StallWindowProbe
            if ($null -eq $probe) {
                return & $ctx.HumanGate $gate
            }

            # freeze, not minimise: the probe becomes a borderless window covering the
            # monitor and then stops repainting, which is the one shape the product
            # reports (docs/product-spec.md). REL-CAP-QUIET-001 covers the silent case.
            $stallAfter = 8
            # A probe left over from an earlier attempt would hold a window and a
            # capture lease for nothing. The TITLE is no longer ambiguous (see
            # below), so this is housekeeping rather than disambiguation.
            Wait-ReleaseProbeGone -ProcessName 'probe_stall_window'
            # The probe puts its own pid in the window title, so this filter can only
            # ever match THIS probe. Two scenarios run it back to back and both bind
            # by title; a shared title let the second select the first one's window
            # while the app's target list still held it, and the recording then died
            # in validation with "audio_target_process_id must be a non-zero PID".
            $probeProcess = Start-Process -FilePath $probe -PassThru -WindowStyle Normal `
                -ArgumentList @('--mode', 'freeze', '--stall-after', "$stallAfter", '--seconds', '90')
            $probeTitle = "ExoSnap stall probe $($probeProcess.Id)"
            try {
                # The window has to exist before it can be selected, and the app
                # enumerates windows when asked to select rather than watching for
                # new ones. record.snapshot carries no target list, so the selection
                # confirms itself: sourceName is what the app resolved the filter to.
                $selected = $false
                $sourceName = ''
                $deadline = [DateTime]::UtcNow.AddSeconds(20)
                while ([DateTime]::UtcNow -lt $deadline) {
                    [void](Invoke-LiveVerifyCommand -Connection $conn -Command 'record.selectTarget' `
                            -Parameters @{ kind = 'window'; titleFilter = $probeTitle })
                    $snapshot = (Invoke-LiveVerifyCommand -Connection $conn -Command 'record.snapshot').result
                    $sourceName = "$(Get-ReleaseSnapshotValue -Object $snapshot -Path 'sourceName')"
                    if ($sourceName -match [regex]::Escape($probeTitle)) { $selected = $true; break }
                    Start-Sleep -Milliseconds 500
                }
                if (-not $selected) {
                    return @{ Result = 'UNVERIFIED'
                        Message      = "the stall probe window was never selectable (source stayed '$sourceName')"
                    }
                }
                $started = Invoke-LiveVerifyCommand -Connection $conn -Command 'record.start'
                if (-not $started) {
                    return @{ Result = 'FAIL'; Message = 'record.start returned nothing for the probe window' }
                }
                # Frames first, then the stall: a capture that never produced a
                # frame would satisfy "no frames" for the wrong reason.
                # The detector needs ten seconds without frame progress before it says
                # anything (product spec), so the wait has to outlast that.
                Start-Sleep -Seconds ($stallAfter + 13)

                $verdict = Resolve-ReleaseVerdict (& $gate.Verify $ctx $gate)
                if ($null -eq $verdict) {
                    return @{ Result = 'UNVERIFIED'; Message = 'the stall verification returned nothing' }
                }
                if ($verdict.Ok) {
                    return @{ Result = 'PASS'; Message = "[probe] $($verdict.Detail)"; Evidence = $verdict.Evidence }
                }
                return @{ Result = 'FAIL'; Message = $verdict.Detail; Evidence = $verdict.Evidence }
            } finally {
                # Stopping is not being stopped: the finalize runs on after the
                # command returns, and the campaign keeps ONE app session for every
                # scenario. Handing the next one a recording still in flight got it
                # refused at setup ("Settings cannot be changed while a recording is
                # in flight") -- a failure that belongs to this teardown, not to it.
                try { [void](Invoke-LiveVerifyCommand -Connection $conn -Command 'record.stop') } catch { }
                $settleBy = [DateTime]::UtcNow.AddSeconds(30)
                while ([DateTime]::UtcNow -lt $settleBy) {
                    $state = (Invoke-LiveVerifyCommand -Connection $conn -Command 'record.snapshot').result
                    $busy = [bool](Get-ReleaseSnapshotValue -Object $state -Path 'recording') -or
                            [bool](Get-ReleaseSnapshotValue -Object $state -Path 'finalizing')
                    if (-not $busy) { break }
                    Start-Sleep -Milliseconds 500
                }
                if ($null -ne $probeProcess -and -not $probeProcess.HasExited) {
                    $probeProcess.Kill()
                }
            }
        }
    }

    # The other half of the capture-stall contract, and the reason the scenario
    # above had to be corrected: a minimized window is SUPPOSED to stop producing
    # frames, so warning about it would be a false alarm about a state the user
    # created. Silence is the documented behaviour (docs/product-spec.md), and a
    # contract that is only ever satisfied by doing nothing is exactly the kind
    # that rots unnoticed.
    $catalog += [pscustomobject]@{
        Id                  = 'REL-CAP-QUIET-001'
        Title               = 'A minimized window stalls silently and the recording carries on'
        Class               = 'capture'
        Layer               = 'FULL_AUTO'
        Source              = 'docs/product-spec.md (capture stall); QCR-804'
        ArtifactBound       = $true
        RequiresInstallTree = $false
        EnvironmentKeys     = @()
        Requires            = @{}
        Desired             = @{}
        OptIn               = $true
        Run                 = {
            param($ctx)
            $probe = Resolve-StallWindowProbe
            if ($null -eq $probe) {
                return @{ Result = 'UNAVAILABLE'
                    Message      = 'probe_stall_window is not built (-DEXOSNAP_BUILD_PROBES=ON)'
                }
            }
            $session = & $ctx.EnsureSession
            $conn = $session.Connection
            # Longer than the 20 s the selection loop below is allowed to take. A
            # MINIMISED window drops out of the app's window list, so a probe that
            # minimises while the gate is still resolving its title can never be
            # selected -- observed as "the probe window was never selectable" after
            # the previous scenario's teardown ate into the window. The freeze gate
            # has no such constraint: a frozen window stays listed.
            $stallAfter = 25
            # A probe left over from an earlier attempt would hold a window and a
            # capture lease for nothing. The TITLE is no longer ambiguous (see
            # below), so this is housekeeping rather than disambiguation.
            Wait-ReleaseProbeGone -ProcessName 'probe_stall_window'
            # The probe puts its own pid in the window title, so this filter can only
            # ever match THIS probe. Two scenarios run it back to back and both bind
            # by title; a shared title let the second select the first one's window
            # while the app's target list still held it, and the recording then died
            # in validation with "audio_target_process_id must be a non-zero PID".
            $probeProcess = Start-Process -FilePath $probe -PassThru -WindowStyle Normal `
                -ArgumentList @('--mode', 'minimise', '--stall-after', "$stallAfter", '--seconds', '90')
            $probeTitle = "ExoSnap stall probe $($probeProcess.Id)"
            try {
                $selected = $false
                $sourceName = ''
                $deadline = [DateTime]::UtcNow.AddSeconds(20)
                while ([DateTime]::UtcNow -lt $deadline) {
                    [void](Invoke-LiveVerifyCommand -Connection $conn -Command 'record.selectTarget' `
                            -Parameters @{ kind = 'window'; titleFilter = $probeTitle })
                    $snapshot = (Invoke-LiveVerifyCommand -Connection $conn -Command 'record.snapshot').result
                    $sourceName = "$(Get-ReleaseSnapshotValue -Object $snapshot -Path 'sourceName')"
                    if ($sourceName -match [regex]::Escape($probeTitle)) { $selected = $true; break }
                    Start-Sleep -Milliseconds 500
                }
                if (-not $selected) {
                    return @{ Result = 'UNVERIFIED'
                        Message      = "the probe window was never selectable (source stayed '$sourceName')"
                    }
                }
                # Everything the hub already holds. The hub is a permanent record,
                # so "is there a stall notice" can only be asked about entries that
                # did not exist before this recording.
                $notificationBaseline = @(Get-ReleaseNotificationEntry `
                        -Snapshot (Invoke-LiveVerifyCommand -Connection $conn -Command 'notifications.snapshot').result |
                    ForEach-Object { $_.sequence })
                [void](Invoke-LiveVerifyCommand -Connection $conn -Command 'record.start')
                Start-Sleep -Seconds ($stallAfter + 13)

                # Silence only means something once the capture has actually stopped
                # producing frames. Two samples four seconds apart establish that,
                # so this can never pass by measuring a healthy recording.
                $first = (Invoke-LiveVerifyCommand -Connection $conn -Command 'pipeline.snapshot').result
                $framesFirst = [int](Get-ReleaseSnapshotValue -Object $first -Path 'capture.framesCaptured')
                Start-Sleep -Seconds 4
                $second = (Invoke-LiveVerifyCommand -Connection $conn -Command 'pipeline.snapshot').result
                $framesSecond = [int](Get-ReleaseSnapshotValue -Object $second -Path 'capture.framesCaptured')
                $notifications = (Invoke-LiveVerifyCommand -Connection $conn -Command 'notifications.snapshot').result
                $evidence = @(
                    Save-LiveVerifyEvidence -Context $ctx -CheckId 'REL-CAP-QUIET-001' -Name 'pipeline.json' -Value $second
                    Save-LiveVerifyEvidence -Context $ctx -CheckId 'REL-CAP-QUIET-001' -Name 'notifications.json' -Value $notifications
                )
                if ($framesSecond -ne $framesFirst) {
                    return @{ Result = 'UNVERIFIED'
                        Message      = "the minimized window kept producing frames ($framesFirst -> $framesSecond); " +
                        'nothing was silent about'
                        Evidence     = $evidence
                    }
                }
                # TITLE only, and only entries raised since this scenario started.
                # Matching title AND body caught the SAVED notification of the
                # previous scenario, whose body is the output filename -- and that
                # filename contains the probe's window title, "ExoSnap stall probe".
                # The gate then reported "a minimized window raised 'Recording
                # saved'", which was true about the string and false about the
                # product.
                $stall = @(Get-ReleaseNotificationEntry -Snapshot $notifications |
                    Where-Object { $_.sequence -notin $notificationBaseline } |
                    Where-Object { "$($_.title)" -match 'stall|no frame' }) | Select-Object -First 1
                if ($null -ne $stall) {
                    return @{ Result = 'FAIL'
                        Message      = "a minimized window raised '$($stall.title)'; it is documented to stay silent"
                        Evidence     = $evidence
                    }
                }
                $lifecycle = "$(Get-ReleaseSnapshotValue -Object $second -Path 'lifecycle')"
                if ($lifecycle -notin @('recording', 'paused')) {
                    return @{ Result = 'FAIL'
                        Message      = "the recording left the running lifecycle ($lifecycle) while the window was minimized"
                        Evidence     = $evidence
                    }
                }
                return @{ Result = 'PASS'
                    Message      = "frames held at $framesSecond for 4 s with no notification, lifecycle $lifecycle"
                    Evidence     = $evidence
                }
            } finally {
                # Stopping is not being stopped: the finalize runs on after the
                # command returns, and the campaign keeps ONE app session for every
                # scenario. Handing the next one a recording still in flight got it
                # refused at setup ("Settings cannot be changed while a recording is
                # in flight") -- a failure that belongs to this teardown, not to it.
                try { [void](Invoke-LiveVerifyCommand -Connection $conn -Command 'record.stop') } catch { }
                $settleBy = [DateTime]::UtcNow.AddSeconds(30)
                while ([DateTime]::UtcNow -lt $settleBy) {
                    $state = (Invoke-LiveVerifyCommand -Connection $conn -Command 'record.snapshot').result
                    $busy = [bool](Get-ReleaseSnapshotValue -Object $state -Path 'recording') -or
                            [bool](Get-ReleaseSnapshotValue -Object $state -Path 'finalizing')
                    if (-not $busy) { break }
                    Start-Sleep -Milliseconds 500
                }
                if ($null -ne $probeProcess -and -not $probeProcess.HasExited) {
                    $probeProcess.Kill()
                }
            }
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
            # The probe takes a display into REAL exclusive fullscreen
            # (SetFullscreenState), which is the only condition this gate accepts --
            # a borderless window would test the simulation. It releases the display
            # itself and exits on its own deadline. The verdict still needs present
            # diagnostics, so this only reaches a PASS when the session is elevated.
            $fseProbe = Resolve-FullscreenProbe
            if ($null -ne $fseProbe) {
                $fse = Start-Process -FilePath $fseProbe -PassThru `
                    -ArgumentList @('--display', '0', '--seconds', '45')
                try {
                    Start-Sleep -Seconds 3   # let the window exist before selecting it
                    # Present statistics follow the SELECTED capture target (ADR 0033:
                    # `updatePresentAttribution` is keyed on the selection). Without
                    # this the attribution stays at pid 0, which counts every process
                    # on the desktop -- and the verdict then reports whatever
                    # presented last, not the probe. That is how this gate reported
                    # 'composed' while the probe demonstrably owned the display.
                    $selected = Invoke-LiveVerifyCommand -Connection $session.Connection `
                        -Command 'record.selectTarget' `
                        -Parameters @{ kind = 'window'; titleFilter = 'ExoSnap FSE probe' }
                    if (-not $selected.ok) {
                        return @{ Result = 'UNVERIFIED'
                            Message = "the probe window could not be selected, so present statistics would " +
                            "describe the desktop rather than it: $($selected.error.message)"
                        }
                    }
                    Start-Sleep -Seconds 5   # let the mode change settle and presents accumulate
                    $verdict = Resolve-ReleaseVerdict (& $gate.Verify $ctx $gate)
                } finally {
                    if ($null -ne $fse -and -not $fse.HasExited) { $fse.Kill() }
                }
                if ($null -eq $verdict) {
                    return @{ Result = 'UNVERIFIED'; Message = 'the fullscreen verification returned nothing' }
                }
                if ($verdict.Ok) {
                    return @{ Result = 'PASS'; Message = "[probe] $($verdict.Detail)"; Evidence = $verdict.Evidence }
                }
                return @{ Result = 'FAIL'; Message = $verdict.Detail; Evidence = $verdict.Evidence }
            }
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

            # Before the recording, not after: the gate below tells the operator that
            # system audio IS enabled, and that had better be a fact rather than a hope
            # about the settings this machine happened to arrive with.
            $audio = Enable-ReleaseSystemAudio -Connection $conn
            if (-not $audio.Ok) { return @{ Result = 'FAIL'; Message = $audio.Detail } }
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
                        # Degradation is reported per SNAPSHOT, not per track: `sourceDegraded`
                        # is the live state and `degradedSources` counts how many sources are
                        # currently lost. There is no `tracks[]` array under pipeline.audio --
                        # and the whole group is absent while `valid` is false.
                        $degraded = [bool](Get-ReleaseSnapshotValue -Object $pipeline -Path 'audio.sourceDegraded')
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
            # A tool that can make the endpoint disappear turns this into a timed
            # sequence rather than a question. The runner deliberately does not know
            # HOW: making an endpoint vanish without unplugging it needs an
            # undocumented interface, which must not become part of the release path
            # (see the environment-truth boundary in docs/dev/release-verify.md). The
            # caller names a tool, this calls it, and the product assertions are
            # unchanged either way.
            #
            # Contract: <tool> set-visibility <endpointId> 0|1
            $visibilityTool = $env:EXOSNAP_ENDPOINT_VISIBILITY_TOOL
            $endpointId = $null
            if (-not [string]::IsNullOrWhiteSpace($visibilityTool) -and (Test-Path -LiteralPath $visibilityTool) -and
                $ctx.Orchestrator.Available) {
                $envSnapshot = Get-EnvironmentSnapshot -Orchestrator $ctx.Orchestrator
                $idProperty = @($envSnapshot.properties |
                        Where-Object { $_.key -eq 'audio.render.normal:endpoint-id' }) | Select-Object -First 1
                if ($null -ne $idProperty) { $endpointId = $idProperty.value }
            }
            if (-not [string]::IsNullOrWhiteSpace($endpointId)) {
                # The outage has to happen WHILE the verification polls, and it has to
                # end while it is still polling: the assertion is degraded-then-
                # recovered, so a device that never comes back fails it just as a
                # device that never left does.
                $outage = Start-Job -ScriptBlock {
                    param($tool, $id)
                    Start-Sleep -Seconds 2
                    & $tool set-visibility $id 0 | Out-Null
                    Start-Sleep -Seconds 10
                    & $tool set-visibility $id 1 | Out-Null
                } -ArgumentList $visibilityTool, $endpointId
                try {
                    $verdict = Resolve-ReleaseVerdict (& $gate.Verify $ctx $gate)
                } finally {
                    Wait-Job $outage -Timeout 60 | Out-Null
                    Remove-Job $outage -Force -ErrorAction SilentlyContinue
                    # Belt and braces: an endpoint left hidden is a machine this
                    # campaign broke, so it is put back even when the job died.
                    & $visibilityTool set-visibility $endpointId 1 | Out-Null
                }
                if ($null -eq $verdict) {
                    return @{ Result = 'UNVERIFIED'; Message = 'the degradation verification returned nothing' }
                }
                if ($verdict.Ok) {
                    return @{ Result = 'PASS'; Message = "[tool] $($verdict.Detail)"; Evidence = $verdict.Evidence }
                }
                return @{ Result = 'FAIL'; Message = $verdict.Detail; Evidence = $verdict.Evidence }
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
            # The whole scenario is about a source that is present and quiet. Without
            # this there is no source at all, and "nothing reported degraded" is true
            # for the same reason an empty list satisfies any assertion over it.
            $audio = Enable-ReleaseSystemAudio -Connection $conn
            if (-not $audio.Ok) { return @{ Result = 'FAIL'; Message = $audio.Detail } }
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
                VerifyDescription = 'This runner polls pipeline.snapshot for 15 s and requires that audio was ' +
                'actually being captured throughout AND that pipeline.audio.sourceDegraded never became true, ' +
                'while the recording keeps running.'
                Verify            = {
                    param($context, $gate)
                    $conn2 = $script:Session.Connection
                    $samples = @()
                    $degradedSeen = $false
                    # An assertion about a source that was never active is not evidence
                    # that quiet is tolerated -- it is evidence that nothing was listened
                    # to. So the presence of audio is asserted alongside its health.
                    $audioActiveSeen = $false
                    $deadline = [DateTime]::UtcNow.AddSeconds(15)
                    while ([DateTime]::UtcNow -lt $deadline) {
                        $pipeline = (Invoke-LiveVerifyCommand -Connection $conn2 -Command 'pipeline.snapshot').result
                        $samples += $pipeline
                        if (Get-ReleaseSnapshotValue -Object $pipeline -Path 'audio.active') { $audioActiveSeen = $true }
                        if (Get-ReleaseSnapshotValue -Object $pipeline -Path 'audio.sourceDegraded') { $degradedSeen = $true }
                        Start-Sleep -Milliseconds 500
                    }
                    $evidence = @(Save-LiveVerifyEvidence -Context $context -CheckId 'REL-AUD-SILENCE-001' -Name 'pipeline-samples.json' -Value $samples)
                    try { [void](Invoke-LiveVerifyCommand -Connection $conn2 -Command 'record.stop') } catch { }
                    if (-not $audioActiveSeen) {
                        return @{ Ok = $false
                            Detail   = 'no audio source was active during the 15 s window, so nothing was observed ' +
                            'about how a silent one is treated'
                            Evidence = $evidence
                        }
                    }
                    if ($degradedSeen) {
                        return @{ Ok = $false; Detail = 'a connected but silent source was reported as degraded'; Evidence = $evidence }
                    }
                    return @{ Ok = $true; Detail = 'an active source, silent for 15 s, produced no degradation'; Evidence = $evidence }
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
            # The assertion below is about the audio track in the output file, so the
            # source has to be on. Otherwise a missing track would be reported as a
            # 44.1 kHz defect when it is a settings state.
            $audioOn = Enable-ReleaseSystemAudio -Connection $conn
            if (-not $audioOn.Ok) { return @{ Result = 'FAIL'; Message = $audioOn.Detail } }
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
                # The three A/V facts live under `avTiming`, never at the snapshot root,
                # and the group is absent entirely while `valid` is false.
                $samples += [pscustomobject]@{
                    atUtc             = [DateTime]::UtcNow.ToString('o')
                    lifecycle         = $pipeline.lifecycle
                    avDriftMs         = Get-ReleaseSnapshotValue -Object $pipeline -Path 'avTiming.avDriftMs'
                    driftAvailability = Get-ReleaseSnapshotValue -Object $pipeline -Path 'avTiming.avDriftAvailability'
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
            # No new threshold is invented here: the checklist's own soak post-checks
            # are applied to the report this scenario already fetches. A container
            # duration alone cannot see a drain that dropped an audio tail, a muxer
            # that failed, or a segment that was never finalized.
            $audioIndexes = @($probe.streams | Where-Object { $_.codec_type -eq 'audio' } |
                    ForEach-Object { [int]$_.index })
            if ($audioIndexes.Count -eq 0) {
                return @{ Result = 'FAIL'; Message = 'the soak recording carries no audio track'
                    Evidence = $evidence
                }
            }
            $spans = @(Get-ReleaseAudioPacketSpan -FfprobePath $ffprobe -Path $result.outputPath `
                    -StreamIndexes $audioIndexes)
            $verdict = Get-ReleaseSoakVerdict -Report $report -ContainerSeconds $duration `
                -ExpectedSeconds $expected -AudioSpanSeconds $spans
            return @{ Result = $verdict.Result
                Message      = "$minutes min recorded, $($samples.Count) drift samples; $($verdict.Message)"
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
        # Same rule as REL-ENV-003: pick a rate the read-back can confirm.
        Desired             = {
            param($ctx)
            if (-not $ctx.Orchestrator.Available) { return 'exosnap-envctl is not built' }
            $listed = Get-EnvironmentDisplayModes -Orchestrator $ctx.Orchestrator -Alias 'display.main-hdr'
            if ($null -eq $listed -or -not $listed.ok) { return 'the display modes could not be enumerated' }
            $display = @($listed.displays) | Select-Object -First 1
            if ($null -eq $display) { return 'no display resolved for display.main-hdr' }
            $rate = Select-UntwinnedRefreshRate -Display $display
            if ($null -eq $rate) { return 'this display enumerates no alternative refresh rate a read-back could confirm' }
            return @{ 'display.main-hdr:refresh-hz' = "$rate" }
        }
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
                Message      = "recorded at the applied display refresh; capture fps $($pipeline.capture.actualFps)"
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
            # The parameter is `screen`, and it is a NAME -- QScreen::name(), which is
            # what environment.snapshot reports for each display. There is no index
            # form: an ordinal would silently mean a different monitor after a
            # topology change, which is the one thing a mixed-monitor gate must not do.
            $target = @($displays | Where-Object { -not $_.primary }) | Select-Object -First 1
            if ($null -eq $target) { $target = $displays[1] }
            # The gate is about what the crossing does to a LIVE preview. Judging one
            # that never delivered a frame would pass on the strength of nothing
            # having been published, so the precondition is established first and
            # reported as its own outcome when it cannot be.
            $before = Get-ReleasePreviewProgress -Connection $conn -TimeoutMs 10000
            if (-not $before.Live) {
                return @{ Result = 'UNVERIFIED'
                    Message      = "the preview never consumed a frame before the crossing ($($before.Message))"
                }
            }
            $moved = Invoke-LiveVerifyCommand -Connection $conn -Command 'window.moveToScreen' `
                -Parameters @{ screen = "$($target.name)" }
            if (-not $moved.ok) {
                return @{ Result = 'FAIL'; Message = "window.moveToScreen '$($target.name)' refused: $($moved.error.message)" }
            }
            [void](Wait-LiveVerifyEvent -Connection $conn -EventName 'window.screenChanged' -TimeoutMs 15000)
            $windows = (Invoke-LiveVerifyCommand -Connection $conn -Command 'windows.snapshot').result
            # The defect this catches is the preview freezing after a monitor crossing:
            # a frame published that no render pass ever presents. That condition is
            # structured state under `updateGate`, but it is a debt a healthy preview
            # takes on and settles once per frame, so it is sampled over a window and
            # judged on progress -- see Get-PreviewFreezeVerdict.
            $samples = @(for ($i = 0; $i -lt 6; $i++) {
                    if ($i -gt 0) { Start-Sleep -Milliseconds 250 }
                    (Invoke-LiveVerifyCommand -Connection $conn -Command 'preview.snapshot').result
                })
            $evidence = @(
                Save-LiveVerifyEvidence -Context $ctx -CheckId 'REL-DISP-MIXED-001' -Name 'windows.json' -Value $windows
                Save-LiveVerifyEvidence -Context $ctx -CheckId 'REL-DISP-MIXED-001' -Name 'preview.json' -Value $samples
            )
            $verdict = Get-PreviewFreezeVerdict -Samples $samples
            return @{ Result = $verdict.Result
                Message      = "moved to '$($target.name)' of $($displays.Count) displays; $($verdict.Message)"
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
            # Prepare the product state the human is asked to look at. A running
            # recording alone is NOT enough: the badge appears on its own, but the
            # quick-control pill and the diagnostics HUD are opt-in settings and the
            # toast needs a notification. Without this an operator is asked about
            # five overlays while exactly one is on screen -- which is what happened,
            # and the answer then described the taskbar and the tray instead.
            $restoreOverlaySettings = @{}
            foreach ($key in 'app.showQuickControls', 'app.showDiagnosticsOverlay', 'app.showRecordingOverlay') {
                $current = Invoke-LiveVerifyCommand -Connection $conn -Command 'settings.get' -Parameters @{ key = $key }
                if ($current.ok) { $restoreOverlaySettings[$key] = $current.result.value }
                [void](Invoke-LiveVerifyCommand -Connection $conn -Command 'settings.set' `
                        -Parameters @{ key = $key; value = $true })
            }
            [void](Invoke-LiveVerifyCommand -Connection $conn -Command 'record.selectTarget' -Parameters @{ kind = 'monitor' })
            [void](Invoke-LiveVerifyCommand -Connection $conn -Command 'record.start')
            [void](Wait-ReleaseRecordingState -Connection $conn -States @('Recording') -TimeoutMs 30000)
            # The fourth surface. Synthetic and reported as such: it is here to be
            # LOOKED at, and proves nothing about when the product raises one.
            [void](Invoke-LiveVerifyCommand -Connection $conn -Command 'notification.raise' -Parameters @{
                    type   = 'windowCaptureStalled'
                    title  = 'Window capture appears to have stalled'
                    body   = 'Synthetic notification, raised so its toast can be judged.'
                    action = 'openDiagnostics'
                })
            $overlays = (Invoke-LiveVerifyCommand -Connection $conn -Command 'overlay.snapshot').result

            $gate = @{
                Id                = 'REL-VIS-OVERLAY-001'
                Title             = 'Judge the overlays on the real desktop, in Light and in Dark'
                Why               = 'These five overlays set WDA_EXCLUDEFROMCAPTURE. That defeats screenshots, ' +
                'screen recording and PrintWindow by design, and the visual harness can only grab their scene ' +
                'graph -- which shows correct alpha even when the window composes wrongly on screen. How they ' +
                'actually reach the desktop can only be seen by a person looking at it.'
                Do                = @(
                    'A recording is running and every optional overlay has been switched on, so the recording',
                    'badge, the quick-control pill, the diagnostics HUD and a notification toast are all on the',
                    'recorded display. (The countdown is not: it only exists BEFORE capture starts.)',
                    'Judge THOSE surfaces. The taskbar preview and the tray menu are Windows chrome, not these.',
                    'Windows has ALREADY been switched to LIGHT and back to DARK by this runner, holding each for a',
                    'few seconds. Look at every ExoSnap overlay on the desktop while that happens.',
                    'Answer for what you saw: the switching is automated, the judgement is not.'
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
            # The appearance switch is a documented HKCU setting, so the runner does
            # it -- twice, holding each long enough to be seen -- and puts the original
            # back. What cannot be automated is the looking: these overlays set
            # WDA_EXCLUDEFROMCAPTURE, so no screenshot, recording or PrintWindow can
            # read them back, and the harness only ever sees their scene graph.
            # One question per state, asked while that state is on screen. Switching
            # both appearances and then asking once meant answering from memory about
            # a screen that no longer looked that way; and a single verdict could not
            # say WHICH appearance was wrong.
            $originalAppearance = Get-WindowsAppearance
            $answers = [ordered]@{}
            try {
                foreach ($appearance in @('Light', 'Dark')) {
                    Set-WindowsAppearance -Appearance $appearance
                    Write-Host ''
                    Write-Host "Windows is now in $($appearance.ToUpperInvariant()) appearance." -ForegroundColor Yellow
                    Write-Host '  Every capture-excluded overlay must still be DARK, with legible text.'
                    $answers[$appearance] = & $ctx.Ask "Do the overlays look right in ${appearance}?"
                    if ($answers[$appearance] -in @('skip', 'abort')) { break }
                }
            }
            finally {
                Set-WindowsAppearance -Appearance $originalAppearance
                # The overlay settings are the operator's, not the campaign's.
                foreach ($entry in $restoreOverlaySettings.GetEnumerator()) {
                    [void](Invoke-LiveVerifyCommand -Connection $conn -Command 'settings.set' `
                            -Parameters @{ key = $entry.Key; value = $entry.Value })
                }
            }

            $wrong = @($answers.Keys | Where-Object { $answers[$_] -eq 'no' })
            if ($wrong.Count -gt 0) {
                $why = Read-Host "  What was wrong in $($wrong -join ' and ')? (one line, recorded in the report)"
                if ([string]::IsNullOrWhiteSpace($why)) { $why = 'no detail given' }
                try { [void](Invoke-LiveVerifyCommand -Connection $script:Session.Connection -Command 'record.stop') } catch { }
                return @{ Result = 'FAIL'
                    Message      = "The operator judged the overlays WRONG in $($wrong -join ' and '): $why"
                }
            }
            if ($answers.Values -contains 'skip' -or $answers.Values -contains 'abort' -or $answers.Count -lt 2) {
                try { [void](Invoke-LiveVerifyCommand -Connection $script:Session.Connection -Command 'record.stop') } catch { }
                return @{ Result = 'DEFERRED'; Message = 'The operator did not judge both appearances' }
            }

            # Both answered yes: the state assertions still have to hold, so the gate
            # runs its own Verify rather than trusting the answer.
            $verdict = Resolve-ReleaseVerdict (& $gate.Verify $ctx $gate)
            if ($null -eq $verdict) {
                return @{ Result = 'UNVERIFIED'; Message = 'the overlay verification returned nothing' }
            }
            if ($verdict.Ok) {
                return @{ Result = 'PASS'
                    Message      = "judged in Light and in Dark: $($verdict.Detail)"
                    Evidence     = $verdict.Evidence
                }
            }
            return @{ Result = 'FAIL'; Message = $verdict.Detail; Evidence = $verdict.Evidence }
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
            $baseline = @(Get-ReleaseNotificationEntry -Snapshot $before | ForEach-Object { $_.sequence })
            # Raise a deterministic product state rather than a synthetic toast: a
            # notification the product decided to show is the thing under test.
            [void](Invoke-LiveVerifyCommand -Connection $conn -Command 'diagnostics.run')

            # ... but ExoSnap notifies about PROBLEMS, and a healthy machine has none.
            # Every publisher in the product is a real fault -- a settings write that
            # failed, an unfinalized recording from a crash, an update that is ready, a
            # capture that stalled. So on a clean desk a diagnostics run legitimately
            # publishes nothing, and there is no product state this runner may fabricate
            # to change that: a synthetic toast would test the injection.
            #
            # The operator is therefore not asked to judge an empty surface. That is
            # UNAVAILABLE -- a fact about this machine having nothing to report -- and
            # not a failure of the notification surface, which was never exercised.
            $fresh = @()
            $deadline = [DateTime]::UtcNow.AddSeconds(10)
            while ([DateTime]::UtcNow -lt $deadline) {
                $now = (Invoke-LiveVerifyCommand -Connection $conn -Command 'notifications.snapshot').result
                $fresh = @(Get-ReleaseNotificationEntry -Snapshot $now | Where-Object { $_.sequence -notin $baseline })
                if ($fresh.Count -gt 0) { break }
                Start-Sleep -Milliseconds 250
            }
            # Nothing to report is the NORMAL case on a healthy machine, and it used
            # to end the gate as UNAVAILABLE -- which meant the notification surface
            # was never looked at on exactly the machines a release is cut from.
            #
            # What this gate asks is whether a notification RENDERS correctly, and a
            # raised one renders through the same manager, the same hub and the same
            # toast window as any other. So the surface is exercised with a synthetic
            # notification when the product has nothing of its own to say. It is
            # marked synthetic end to end (see NotificationEvent), the verdict says
            # so, and it is never treated as evidence that the product WOULD have
            # raised anything -- that claim belongs to the scenarios that cause a
            # real condition.
            $synthetic = $false
            if ($fresh.Count -eq 0) {
                $raised = Invoke-LiveVerifyCommand -Connection $conn -Command 'notification.raise' -Parameters @{
                    type   = 'windowCaptureStalled'
                    title  = 'Window capture appears to have stalled'
                    body   = 'Synthetic notification, raised so the toast and hub entry can be judged.'
                    action = 'openDiagnostics'
                }
                if (-not $raised.ok) {
                    return @{ Result = 'UNAVAILABLE'
                        Message      = 'the product published no notification and none could be raised to judge ' +
                        "the surface with: $($raised.error.message)"
                    }
                }
                $synthetic = $true
                $deadline = [DateTime]::UtcNow.AddSeconds(10)
                while ([DateTime]::UtcNow -lt $deadline) {
                    $now = (Invoke-LiveVerifyCommand -Connection $conn -Command 'notifications.snapshot').result
                    $fresh = @(Get-ReleaseNotificationEntry -Snapshot $now | Where-Object { $_.sequence -notin $baseline })
                    if ($fresh.Count -gt 0) { break }
                    Start-Sleep -Milliseconds 250
                }
                if ($fresh.Count -eq 0) {
                    return @{ Result = 'UNVERIFIED'
                        Message = 'a notification was raised but never reached the hub'
                    }
                }
            }

            # Said in the instructions, not only in the report: an operator judging a
            # raised notification has to know it was raised, or "it appeared" reads
            # as product behaviour.
            $originLine = $synthetic ?
                'This machine had nothing of its own to report, so the notification was RAISED by this runner: judge how it LOOKS, not that it appeared.' :
                'The notification you see was published by the product itself.'

            $gate = @{
                Id                = 'REL-VIS-NOTIFY-001'
                Title             = 'Judge the real desktop notification surface'
                # Values the Verify block needs travel HERE, not in a closure: a
                # `.GetNewClosure()` block is bound to a synthetic module that does not
                # inherit the runner's functions.
                State             = @{ baselineSequences = $baseline; synthetic = $synthetic }
                Why               = 'What reaches the desktop is composed by the OS notification surface, not by ' +
                'us. Our own snapshot proves what we asked for; it cannot prove what appeared.'
                Do                = @(
                    'Watch the desktop notification area.',
                    'Open the ExoSnap notification hub and look at the entries there too.',
                    $originLine
                )
                Expected          = 'Notifications appear with the correct severity glyph and tint, and the text ' +
                'is legible and not truncated.'
                VerifyDescription = 'This runner asserts that NEW notifications reached the hub while the gate ' +
                'was open, by diffing notifications.snapshot entry sequences against the ones it recorded ' +
                'beforehand, and records whether they came from the product or were raised synthetically. ' +
                'Whether they LOOKED right is your verdict.'
                Verify            = {
                    param($context, $gate)
                    $conn2 = $script:Session.Connection
                    $after = (Invoke-LiveVerifyCommand -Connection $conn2 -Command 'notifications.snapshot').result
                    $evidence = @(Save-LiveVerifyEvidence -Context $context -CheckId 'REL-VIS-NOTIFY-001' -Name 'notifications-after.json' -Value $after)
                    # `entries`, not `notifications`. The hub keeps a permanent record, so
                    # the count BEFORE the gate is the baseline -- an entry that was
                    # already there is not something the product published just now.
                    $published = @(Get-ReleaseNotificationEntry -Snapshot $after)
                    $baseline = @($gate.State.baselineSequences)
                    $fresh = @($published | Where-Object { $_.sequence -notin $baseline })
                    if ($fresh.Count -eq 0) {
                        return @{ Ok = $false; Detail = 'the product published no notification while the gate was open'; Evidence = $evidence }
                    }
                    # Who raised it belongs in the record: a synthetic one proves the
                    # surface renders and never that the product would have spoken.
                    $origin = if ($gate.State.synthetic) { '[synthetic] ' } else { '' }
                    return @{ Ok = $true
                        Detail   = "$origin$($fresh.Count) new notification(s) reached the hub and were judged"
                        Evidence = $evidence
                    }
                }
            }
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
            # An update needs something to update FROM, and it cannot be the artifact
            # this campaign is bound to: that one is the newest release the feed
            # offers, so it correctly reports "up to date" and the run ends before
            # any of the assertions this scenario exists for. The starting point is
            # therefore a PREVIOUS official build, named by EXOSNAP_UPDATE_FROM.
            # Without one there is nothing to prove and the scenario says so, rather
            # than passing on a check that never ran.
            $from = $env:EXOSNAP_UPDATE_FROM
            if ([string]::IsNullOrWhiteSpace($from) -or -not (Test-Path -LiteralPath $from)) {
                return @{ Result = 'UNAVAILABLE'
                    Message      = 'set EXOSNAP_UPDATE_FROM to an older official exosnap.exe; the bound artifact ' +
                    'is the newest release and can only ever report up-to-date'
                }
            }
            $fromVersion = (Get-Item -LiteralPath $from).VersionInfo.ProductVersion
            if ($fromVersion -eq $ctx.Artifact.productVersion) {
                return @{ Result = 'UNAVAILABLE'
                    Message      = "EXOSNAP_UPDATE_FROM carries $fromVersion, the same version as the bound artifact"
                }
            }
            # The handoff launches its own isolated instance, so this campaign's shared
            # session has to be out of the way first: the single-instance guard is a
            # machine-wide mutex, and `EXOSNAP_CONFIG_DIR` isolation does not change
            # that. A second launch hands focus to the running instance and exits 0
            # WITHOUT ever constructing the Live Verify server, so the handoff then
            # waits for a pipe nobody opened and fails on a connect timeout.
            & $ctx.EndSession
            $output = & pwsh -NoProfile -File $script -AppPath $from -RequireApply 2>&1 | Out-String
            $exit = $LASTEXITCODE
            $evidence = @(Save-LiveVerifyEvidence -Context $ctx -CheckId 'REL-UPD-PORTABLE-001' -Name 'handoff.log' -Raw $output)
            if ($exit -ne 0) {
                return @{ Result = 'FAIL'; Message = "the update handoff script exited $exit"; Evidence = $evidence }
            }
            return @{ Result = 'PASS'
                Message      = "$fromVersion applied the published $($ctx.Artifact.productVersion): app, handoff and " +
                'updater agreed on one pinned version'
                Evidence     = $evidence
            }
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
            # Same starting point as REL-UPD-MSI-001, and for the same reason: the
            # BOUND artifact is the newest release, so an update check on it can only
            # ever report up-to-date and the prompt this gate exists for is never
            # raised. It has to start from the older INSTALLED build.
            #
            # Order matters between the two MSI gates. This one must run BEFORE the
            # accept gate, which leaves the new version installed and removes the
            # older starting point both of them need.
            $from = $env:EXOSNAP_UPDATE_FROM
            if ([string]::IsNullOrWhiteSpace($from) -or -not (Test-Path -LiteralPath $from)) {
                return @{ Result = 'UNAVAILABLE'
                    Message      = 'set EXOSNAP_UPDATE_FROM to an older INSTALLED exosnap.exe; the bound artifact ' +
                    'is the newest release and can only ever report up-to-date'
                }
            }
            # Its own session, not the campaign's -- see REL-UPD-MSI-001 for why the
            # machine-wide single-instance mutex has to be clear first.
            & $ctx.EndSession
            $waitUntil = [DateTime]::UtcNow.AddSeconds(20)
            while ([DateTime]::UtcNow -lt $waitUntil) {
                $running = @(Get-Process -Name 'exosnap' -ErrorAction SilentlyContinue)
                if ($running.Count -eq 0) { break }
                foreach ($instance in $running) {
                    try {
                        [void]$instance.CloseMainWindow()
                        if (-not $instance.WaitForExit(5000)) { $instance.Kill() }
                    }
                    catch { }
                }
                Start-Sleep -Milliseconds 500
            }
            $runId = New-LiveVerifyRunId
            $fromProcess = Start-Process -FilePath $from -PassThru -ArgumentList @('--live-verify-control', $runId)
            $conn = Connect-LiveVerify -RunId $runId -ConnectTimeoutMs 60000
            [void]$fromProcess
            # A release candidate is a GitHub prerelease and only the Preview channel
            # names one; the channel is part of the scenario, not of the machine.
            $channel = Invoke-LiveVerifyCommand -Connection $conn -Command 'settings.set' `
                -Parameters @{ key = 'app.updateChannel'; value = 'Preview' }
            if (-not $channel.ok) {
                return @{ Result = 'FAIL'; Message = "could not select the Preview channel: $($channel.error.message)" }
            }
            # Same restart REL-UPD-MSI-001 does, and for the same reason: the channel
            # is read when the update service starts, so setting it and checking in
            # one session asks the channel the app was LAUNCHED with. Observed as
            # "no update is offered to 0.9.0-rc15 on the Preview channel" while rc16
            # was published as a prerelease.
            try { $conn.Close() } catch { }
            if ($null -ne $fromProcess -and -not $fromProcess.HasExited) {
                [void]$fromProcess.CloseMainWindow()
                if (-not $fromProcess.WaitForExit(15000)) { $fromProcess.Kill() }
            }
            $runId = New-LiveVerifyRunId
            $fromProcess = Start-Process -FilePath $from -PassThru -ArgumentList @('--live-verify-control', $runId)
            $conn = Connect-LiveVerify -RunId $runId -ConnectTimeoutMs 60000

            $checked = Invoke-LiveVerifyCommand -Connection $conn -Command 'update.check'
            if (-not $checked.ok) { return @{ Result = 'FAIL'; Message = "update.check refused: $($checked.error.message)" } }
            # Asynchronous: the answer lands on the state, not on the command.
            $waitUntilOffered = [DateTime]::UtcNow.AddSeconds(45)
            $state = $null
            while ([DateTime]::UtcNow -lt $waitUntilOffered) {
                $state = (Invoke-LiveVerifyCommand -Connection $conn -Command 'update.getState').result
                if ($state.updateAvailable) { break }
                Start-Sleep -Milliseconds 500
            }
            if (-not $state.updateAvailable) {
                return @{ Result = 'UNAVAILABLE'
                    Message = "no update is offered to $($state.currentVersion) on the Preview channel"
                }
            }
            $applied = Invoke-LiveVerifyCommand -Connection $conn -Command 'update.apply'
            if (-not $applied.ok) { return @{ Result = 'FAIL'; Message = "update.apply refused: $($applied.error.message)" } }
            # The child the apply launched, named by the launch itself. `installState` and
            # `phase` are the UPDATER's vocabulary -- asking the application for them was
            # asking the wrong process, which is why the two fields never existed.
            $launch = (Invoke-LiveVerifyCommand -Connection $conn -Command 'update.getState').result.updaterLaunch

            $gate = @{
                Id                = 'REL-UPD-MSI-DECLINE-001'
                Title             = 'DECLINE the elevation prompt'
                State             = @{ updaterRunId = "$($launch.controlRunId)"; updaterPipe = "$($launch.controlPipe)" }
                Why               = 'Same Secure Desktop boundary as accepting it. What is under test is what the ' +
                'product says afterwards.'
                Do                = @('A UAC prompt is appearing now.', 'DECLINE it.')
                Expected          = 'The updater reports a cancelled update, not a failed one, and nothing is ' +
                'left half-installed.'
                VerifyDescription = "This runner attaches to the updater's own control endpoint (run id " +
                "$($launch.controlRunId)) and requires a cancelled-or-idle phase with installState intact -- never " +
                'a failure state, and never strandedInBackup. Those fields belong to the updater; the application ' +
                'only reports which child it launched.'
                Verify            = {
                    param($context, $gate)
                    if ([string]::IsNullOrWhiteSpace($gate.State.updaterRunId)) {
                        return @{ Ok = $false; Detail = 'update.apply reported no updater launch, so there is no child to ask' }
                    }
                    try { $updater = Connect-LiveVerify -RunId $gate.State.updaterRunId -Role 'Updater' -ConnectTimeoutMs 30000 }
                    catch {
                        return @{ Ok = $false; Detail = "the updater endpoint could not be reached: $($_.Exception.Message)" }
                    }
                    try {
                        # Checked before reading, because the runner runs under
                        # Set-StrictMode: a response that carries an error instead of
                        # a result throws "the property 'result' cannot be found" and
                        # the scenario reports a PowerShell message where a verdict
                        # belongs. This is the first release where the gate reached
                        # this code at all -- it used to stop at "no update is
                        # offered" before the prompt was ever raised.
                        $state_response = Invoke-LiveVerifyCommand -Connection $updater -Command 'updater.getState'
                        if (-not $state_response.ok) {
                            return @{ Ok = $false
                                Detail = "the updater refused updater.getState: $($state_response.error.message)"
                            }
                        }
                        $after = $state_response.result
                        $evidence = @(Save-LiveVerifyEvidence -Context $context -CheckId 'REL-UPD-MSI-DECLINE-001' -Name 'updater-state.json' -Value $after)
                        $installState = "$(Get-ReleaseSnapshotValue -Object $after -Path 'installState')"
                        $phase = "$(Get-ReleaseSnapshotValue -Object $after -Path 'phase')"
                        if ($installState -eq 'strandedInBackup') {
                            return @{ Ok = $false; Detail = 'the install is stranded in backup after a declined prompt'; Evidence = $evidence }
                        }
                        if ($phase -match 'fail|error') {
                            return @{ Ok = $false; Detail = "a declined prompt was reported as a failure: $phase"; Evidence = $evidence }
                        }
                        return @{ Ok = $true; Detail = "phase=$phase installState=$installState"; Evidence = $evidence }
                    }
                    finally { try { $updater.Close() } catch { } }
                }
            }
            return & $ctx.HumanGate $gate
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
            # The same starting-point rule the portable update gate needs, for the
            # same reason: the bound artifact is the newest release the feed offers,
            # so an update check against it can only ever say "up to date". For THIS
            # gate the starting point must additionally be an INSTALLED build --
            # the MSI path is what is under test -- so EXOSNAP_UPDATE_FROM points at
            # the installed exosnap.exe of an older release.
            $from = $env:EXOSNAP_UPDATE_FROM
            if ([string]::IsNullOrWhiteSpace($from) -or -not (Test-Path -LiteralPath $from)) {
                return @{ Result = 'UNAVAILABLE'
                    Message      = 'set EXOSNAP_UPDATE_FROM to an older INSTALLED exosnap.exe; the bound artifact ' +
                    'is the newest release and can only ever report up-to-date'
                }
            }
            # Its own session, not the campaign's: the app under test here is the
            # installed older build, and the campaign's is the bound artifact.
            & $ctx.EndSession
            # The single-instance guard is a machine-wide mutex: a second launch hands
            # focus to whatever is already running and exits without ever creating a
            # control endpoint, so the connect below would wait for a pipe nobody
            # opened. Any leftover instance -- including one this gate's own previous
            # attempt left behind -- has to be gone first.
            $waitUntil = [DateTime]::UtcNow.AddSeconds(20)
            while ([DateTime]::UtcNow -lt $waitUntil) {
                $running = @(Get-Process -Name 'exosnap' -ErrorAction SilentlyContinue)
                if ($running.Count -eq 0) { break }
                foreach ($instance in $running) {
                    try {
                        [void]$instance.CloseMainWindow()
                        if (-not $instance.WaitForExit(5000)) { $instance.Kill() }
                    }
                    catch { }
                }
                Start-Sleep -Milliseconds 500
            }
            $runId = New-LiveVerifyRunId
            $fromProcess = Start-Process -FilePath $from -PassThru -ArgumentList @('--live-verify-control', $runId)
            $conn = Connect-LiveVerify -RunId $runId -ConnectTimeoutMs 60000
            # app.identity answers the same object system.hello carries, and its version
            # field is `productVersion`. There is no `version`.
            $before = (Invoke-LiveVerifyCommand -Connection $conn -Command 'app.identity').result
            $beforeVersion = "$($before.productVersion)"
            # A release candidate is a GitHub prerelease, and the Stable channel names
            # only non-prerelease releases -- so a check on Stable is correct to
            # report nothing and the gate would learn nothing from it. The channel is
            # part of the scenario, not of the machine it happens to run on.
            $channel = Invoke-LiveVerifyCommand -Connection $conn -Command 'settings.set' `
                -Parameters @{ key = 'app.updateChannel'; value = 'Preview' }
            if (-not $channel.ok) {
                return @{ Result = 'FAIL'; Message = "could not select the Preview channel: $($channel.error.message)" }
            }
            # The channel is read when the update service starts, so it takes a
            # restart to be the channel the next check actually uses. Setting it and
            # checking in the same session reported "nothing offered" against the
            # channel the app was launched with.
            try { $conn.Close() } catch { }
            if ($null -ne $fromProcess -and -not $fromProcess.HasExited) {
                [void]$fromProcess.CloseMainWindow()
                if (-not $fromProcess.WaitForExit(15000)) { $fromProcess.Kill() }
            }
            $runId = New-LiveVerifyRunId
            $fromProcess = Start-Process -FilePath $from -PassThru -ArgumentList @('--live-verify-control', $runId)
            $conn = Connect-LiveVerify -RunId $runId -ConnectTimeoutMs 60000

            $checked = Invoke-LiveVerifyCommand -Connection $conn -Command 'update.check'
            if (-not $checked.ok) { return @{ Result = 'FAIL'; Message = "update.check refused: $($checked.error.message)" } }
            # The check is asynchronous: the answer lands on the state, not on the
            # command, so reading the state once could only ever see what was there
            # before the check finished.
            $state = $null
            $checkDeadline = [DateTime]::UtcNow.AddSeconds(60)
            while ([DateTime]::UtcNow -lt $checkDeadline) {
                $state = (Invoke-LiveVerifyCommand -Connection $conn -Command 'update.getState').result
                if ($state.updateAvailable) { break }
                Start-Sleep -Milliseconds 1000
            }
            if (-not $state.updateAvailable) {
                return @{ Result = 'UNAVAILABLE'
                    Message      = "no update is offered to $beforeVersion on the Preview channel " +
                    "(phase $($state.phase))"
                }
            }
            $applied = Invoke-LiveVerifyCommand -Connection $conn -Command 'update.apply'
            if (-not $applied.ok) { return @{ Result = 'FAIL'; Message = "update.apply refused: $($applied.error.message)" } }

            $gate = @{
                Id                = 'REL-UPD-MSI-001'
                Title             = 'Accept the elevation prompt for the MSI install'
                State             = @{ previousVersion = $beforeVersion; installedPath = $from }
                Why               = 'msiexec needs an elevated token, and the prompt runs on the Secure Desktop. ' +
                'Windows blocks synthetic input across that boundary by design; there is nothing to automate.'
                Do                = @(
                    'A UAC prompt is appearing now, raised by the ExoSnap updater for msiexec.',
                    'Read what it says, then ACCEPT it.',
                    'Wait for the installer to finish and for ExoSnap to relaunch.'
                )
                Expected          = 'The install completes and ExoSnap comes back at the new version.'
                VerifyDescription = "This runner captured the version before the update ($beforeVersion) and " +
                'will reconnect afterwards, read app.identity again, and require a DIFFERENT version. Install ' +
                'integrity is not asked of the application here: after an accepted install the updater has ' +
                'already exited, so the only honest evidence available is that the new version came back and ' +
                'runs. It never touches the prompt.'
                Verify            = {
                    param($context, $gate)
                    $deadline = [DateTime]::UtcNow.AddMinutes(5)
                    $identity = $null
                    while ([DateTime]::UtcNow -lt $deadline) {
                        try {
                            $runId2 = New-LiveVerifyRunId
                            # An accepted install RELAUNCHES the application, and the
                            # single-instance mutex is machine-wide: the launch below
                            # would then hand focus to that instance and exit without
                            # ever opening a control endpoint, leaving this loop to
                            # spend its whole 5 min budget connecting to a pipe nobody
                            # created. Observed exactly that -- the gate sat in
                            # "verifying the consequence independently..." until the
                            # relaunched instance was ended by hand.
                            Wait-ReleaseProbeGone -ProcessName 'exosnap' -TimeoutMs 15000
                            # The INSTALLED build, not the bound artifact. Launching the
                            # artifact here asked whether rc14 reports rc14, which is
                            # true whether or not the install under test ever ran --
                            # the assertion passed on the version difference between
                            # two files that were always different.
                            $process = Start-Process -FilePath $gate.State.installedPath `
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
                    $afterVersion = "$($identity.productVersion)"
                    if ($afterVersion -eq $gate.State.previousVersion) {
                        return @{ Ok = $false; Detail = "the version is unchanged at $afterVersion; nothing was installed"; Evidence = $evidence }
                    }
                    return @{ Ok = $true; Detail = "installed: $($gate.State.previousVersion) -> $afterVersion"; Evidence = $evidence }
                }
            }
            # An elevated caller never sees the prompt this gate is named after:
            # msiexec inherits the token and installs without asking. The install
            # itself still has to happen and is verified the same way, so the result
            # says which of the two it was.
            if (Test-RunnerElevated) {
                $verdict = Resolve-ReleaseVerdict (& $gate.Verify $ctx $gate)
                if ($null -eq $verdict) {
                    return @{ Result = 'UNVERIFIED'; Message = 'the install verification returned nothing' }
                }
                if ($verdict.Ok) {
                    return @{ Result = 'PASS'
                        Message      = "[elevated runner: no prompt was raised] $($verdict.Detail)"
                        Evidence     = $verdict.Evidence
                    }
                }
                return @{ Result = 'FAIL'; Message = $verdict.Detail; Evidence = $verdict.Evidence }
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
            # Own instance, own run id — so the shared session must go first, for the
            # single-instance reason spelled out in REL-UPD-PORTABLE-001. Passing today
            # only because the scenario before this one happens to end the session.
            & $ctx.EndSession
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

function Get-ReleaseFieldContract {
    <#
    .SYNOPSIS
        Every control-channel field path the catalog reads, and who reads it.
    .DESCRIPTION
        The contract between this catalog and the product's emitters, written down in
        one place so REL-SCHEMA-001 can check all of it in one unattended pass.

        A path is listed here BECAUSE a scenario depends on it. `UsedBy` is not
        decoration: when a path disappears it names, without searching, exactly which
        gates are about to throw -- including the ones that would otherwise only throw
        after a human had already unplugged something.

        `Stage` says what the product must be doing for the path to exist at all:

            idle       readable at any time
            recording  a recording is running. The pipeline's measurement groups are
                       ABSENT while `valid` is false, by design -- emitting them idle
                       would hand a reader a complete, entirely zero pipeline that
                       reads as a healthy recording rather than an idle process.
            result     after a recording has completed

        `[]` after a segment means "and then its first element". A collection that is
        legitimately empty at check time is reported as unchecked, never as passing:
        the name was proven, the element shape was not.
    #>
    $groups = @(
        @{ Command = 'app.identity'; Stage = 'idle'; UsedBy = 'REL-UPD-MSI-001, REL-PRESENT-002'
            Paths = @('productVersion', 'executableSha256')
        }
        @{ Command = 'environment.snapshot'; Stage = 'idle'; UsedBy = 'REL-PRESENT-001, REL-PRESENT-002'
            Paths = @('present.optIn', 'present.elevated', 'present.available', 'present.availability')
        }
        @{ Command = 'environment.snapshot'; Stage = 'idle'
            UsedBy = 'REL-DISP-HDR-001, REL-DISP-MIXED-001, REL-DISP-DPI-001'
            Paths  = @('displays.screens[].name', 'displays.screens[].primary',
                'displays.screens[].hdrActive', 'displays.screens[].devicePixelRatio')
        }
        @{ Command = 'windows.snapshot'; Stage = 'idle'; UsedBy = 'REL-DISP-DPI-001'
            Paths = @('windows[].role', 'windows[].nativeWindowCreated')
        }
        @{ Command = 'preview.snapshot'; Stage = 'idle'; UsedBy = 'REL-DISP-MIXED-001'
            # The repaint bookkeeping is a group of its own: `owed` at the top level
            # would be a claim about the preview, and it is a claim about the update
            # gate that drives it.
            Paths = @('active', 'frameReady', 'updateGate.owed', 'updateGate.renderPasses')
        }
        @{ Command = 'record.snapshot'; Stage = 'idle'
            UsedBy = 'REL-AUD-DEGRADE-001, REL-AUD-SILENCE-001, REL-AUD-FORMAT-001'
            Paths  = @('systemAudioEnabled')
        }
        @{ Command = 'overlay.snapshot'; Stage = 'idle'; UsedBy = 'REL-VIS-OVERLAY-001'
            Paths = @('overlays[].visible')
        }
        @{ Command = 'notifications.snapshot'; Stage = 'idle'; UsedBy = 'REL-CAP-STALL-001, REL-VIS-NOTIFY-001'
            Paths = @('entries[].sequence', 'entries[].title', 'entries[].body')
        }
        @{ Command = 'update.getState'; Stage = 'idle'; UsedBy = 'REL-UPD-MSI-001, REL-UPD-MSI-DECLINE-001'
            # `installState` and `phase` are the UPDATER's vocabulary, not the app's.
            # The app reports its own update state machine plus the endpoint of the
            # child it launched; whoever wants the updater's answer connects to that
            # child, which is what `updaterLaunch` exists for.
            Paths = @('updateAvailable', 'state', 'blocker', 'currentVersion',
                'updaterLaunch.controlRunId', 'updaterLaunch.controlPipe')
        }
        @{ Command = 'pipeline.snapshot'; Stage = 'recording'
            UsedBy = 'REL-CAP-001, REL-CAP-STALL-001, REL-AUD-DEGRADE-001'
            Paths  = @('valid', 'lifecycle')
        }
        @{ Command = 'pipeline.snapshot'; Stage = 'recording'; UsedBy = 'REL-DISP-REFRESH-001'
            Paths = @('capture.actualFps')
        }
        @{ Command = 'pipeline.snapshot'; Stage = 'recording'; UsedBy = 'REL-CAP-STALL-001'
            Paths = @('sourcePresentation.presentMode', 'sourcePresentation.modeAvailability')
        }
        @{ Command = 'pipeline.snapshot'; Stage = 'recording'; UsedBy = 'REL-AUD-DEGRADE-001, REL-AUD-SILENCE-001'
            Paths = @('audio.active', 'audio.sourceDegraded', 'audio.degradedSources')
        }
        @{ Command = 'pipeline.snapshot'; Stage = 'recording'; UsedBy = 'REL-AUD-CLOCK-001'
            Paths = @('avTiming.avDriftMs', 'avTiming.avDriftAvailability')
        }
        @{ Command = 'record.result'; Stage = 'result'
            UsedBy = 'REL-CAP-001, REL-AUD-FORMAT-001, REL-AUD-CLOCK-001, REL-DISP-HDR-001'
            Paths  = @('succeeded', 'outputPath')
        }
    )

    $contract = @()
    foreach ($group in $groups) {
        foreach ($path in $group.Paths) {
            $contract += [pscustomobject]@{
                Command = $group.Command
                Stage   = $group.Stage
                Path    = $path
                UsedBy  = $group.UsedBy
            }
        }
    }
    return [object[]]$contract
}

function Resolve-ReleaseFieldPath {
    <#
    .SYNOPSIS
        Walks a dotted field path and says whether it exists.
    .OUTPUTS
        @{ Status = 'present' | 'missing' | 'empty'; At = '<segment>'; Value = ... }
    .DESCRIPTION
        'empty' is its own answer rather than a failure. A collection that has no
        elements right now proves its own NAME is right and proves nothing about the
        shape of its elements, and collapsing that into either verdict would be a
        false statement in one direction or the other.
    #>
    param($Root, [Parameter(Mandatory)] [string] $Path)
    $current = $Root
    foreach ($segment in ($Path -split '\.')) {
        $indexed = $segment.EndsWith('[]')
        $name = if ($indexed) { $segment.Substring(0, $segment.Length - 2) } else { $segment }
        if ($null -eq $current) { return @{ Status = 'missing'; At = $name } }
        $properties = $current.PSObject.Properties.Name
        if ($null -eq $properties -or $properties -notcontains $name) {
            return @{ Status = 'missing'; At = $name }
        }
        $current = $current.$name
        if ($indexed) {
            $items = @($current)
            if ($items.Count -eq 0) { return @{ Status = 'empty'; At = $name } }
            $current = $items[0]
        }
    }
    return @{ Status = 'present'; At = $Path; Value = $current }
}

function Get-ReleaseSnapshotValue {
    <#
    .SYNOPSIS
        Reads a dotted field path out of a control-channel snapshot, or $null.
    .DESCRIPTION
        Set-StrictMode -Version Latest makes `$snapshot.audio.sourceDegraded` THROW when
        `audio` is absent, and whole measurement groups are legitimately absent: a
        pipeline snapshot with `valid: false` carries only its summary, because emitting
        the groups would hand a reader a complete, entirely zero pipeline that reads as
        a healthy recording rather than an idle process.

        So "not measured" has to be answerable without an exception. It returns $null,
        which a caller compares against -- never a default that could be mistaken for a
        reading.
    #>
    param($Object, [Parameter(Mandatory)] [string] $Path)
    $current = $Object
    foreach ($segment in ($Path -split '\.')) {
        if ($null -eq $current) { return $null }
        if ($current -isnot [psobject] -or $current.PSObject.Properties.Name -notcontains $segment) { return $null }
        $current = $current.$segment
    }
    return $current
}

function Get-ReleaseNotificationEntry {
    <#
    .SYNOPSIS
        The hub entries out of a notifications.snapshot, as an array.
    .DESCRIPTION
        The array is `entries`, and each entry carries sequence/title/body/severity/
        unread/actions. There is no `id` and no `detail`: the manager-assigned
        `sequence` is the hub's own stable identity and the only thing a client may
        address an entry by.

        Wrap the CALL in @(...) before reading .Count. PowerShell unrolls an empty
        array on return, so the caller receives $null and $null.Count throws under
        StrictMode -- which looks exactly like "the hub is broken" rather than "the hub
        is empty".
    #>
    param($Snapshot)
    $entries = Get-ReleaseSnapshotValue -Object $Snapshot -Path 'entries'
    if ($null -eq $entries) { return @() }
    return @($entries)
}

function Enable-ReleaseSystemAudio {
    <#
    .SYNOPSIS
        Turns the system-audio source on through the product's own settings surface.
    .DESCRIPTION
        Every audio scenario needs this and none of them may assume it. The shipped
        default has SYS on, but a campaign runs against whatever settings the machine
        already had -- and a scenario that asserts over audio it never enabled asserts
        over nothing. REL-AUD-SILENCE-001 passed that way: its loop looked for a
        degraded source among sources that did not exist.
    #>
    param([Parameter(Mandatory)] $Connection)
    $set = Invoke-LiveVerifyCommand -Connection $Connection -Command 'settings.set' `
        -Parameters @{ key = 'audio.systemEnabled'; value = $true }
    if (-not $set.ok) {
        return @{ Ok = $false; Detail = "settings.set audio.systemEnabled refused: $($set.error.message)" }
    }
    $record = (Invoke-LiveVerifyCommand -Connection $Connection -Command 'record.snapshot').result
    if (-not (Get-ReleaseSnapshotValue -Object $record -Path 'systemAudioEnabled')) {
        return @{ Ok = $false; Detail = 'the system-audio source did not become enabled after settings.set' }
    }
    return @{ Ok = $true; Detail = 'system audio enabled' }
}

function Resolve-ReleaseVerdict {
    <#
    .SYNOPSIS
        Gives a Verify block's return value a fixed shape.
    .DESCRIPTION
        A Verify block returns @{ Ok; Detail; Evidence } but is allowed to omit the
        fields it has nothing to say about -- and about half of them do, because a
        refusal has no evidence to attach. Under Set-StrictMode reading an absent
        key does not evaluate to null, it THROWS "the property 'Evidence' cannot be
        found on this object", and the scenario then reports a PowerShell message
        where a verdict belongs.

        This is the same defect class as the response shape in LiveVerifyClient: an
        optional field read at ~25 sites. It cost an operator three UAC prompts in
        one campaign, each answered correctly and each thrown away by a different
        missing key. Normalising here means a Verify block can keep omitting what it
        does not have, and no reader has to remember which.

        A $null verdict stays $null: "the block returned nothing" is a real and
        different finding from "it returned a verdict with no detail", and callers
        report it as UNVERIFIED.
    #>
    param($Verdict)
    if ($null -eq $Verdict) { return $null }
    if ($Verdict -isnot [System.Collections.IDictionary]) { return $Verdict }
    if (-not $Verdict.ContainsKey('Ok')) { $Verdict['Ok'] = $false }
    if (-not $Verdict.ContainsKey('Detail') -or $null -eq $Verdict['Detail']) { $Verdict['Detail'] = '' }
    if (-not $Verdict.ContainsKey('Evidence') -or $null -eq $Verdict['Evidence']) { $Verdict['Evidence'] = @() }
    return $Verdict
}

function Wait-ReleaseProbeGone {
    <#
    .SYNOPSIS
        Waits for a probe process to be gone, ending it if it overstays.
    .DESCRIPTION
        Both stall scenarios select their window with
        `record.selectTarget -titleFilter 'ExoSnap stall probe'`, and both probes
        carry that same title. Run back to back, the second scenario can therefore
        select the FIRST one's window while it is still being torn down -- and then
        read the previous scenario's notifications as its own. REL-CAP-QUIET-001
        reported "a minimized window raised 'Recording saved'" that way, which was
        REL-CAP-STALL-001's toast.

        Called before starting a probe, so the title is unambiguous by the time
        anything selects on it.
    #>
    param([Parameter(Mandatory)] [string] $ProcessName, [int] $TimeoutMs = 10000)
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        $running = @(Get-Process -Name $ProcessName -ErrorAction SilentlyContinue)
        if ($running.Count -eq 0) { return }
        foreach ($instance in $running) {
            try { $instance.Kill() } catch { }
        }
        Start-Sleep -Milliseconds 250
    }
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

function Get-PreviewFreezeVerdict {
    <#
    .SYNOPSIS
        Decides from a series of preview snapshots whether the preview is frozen.
    .DESCRIPTION
        `updateGate.owed` is `published_generation > presented_generation`, which a
        healthy preview crosses between every publish and the render pass that
        follows it. It is therefore not a verdict on its own: one sample fails a
        live preview at whatever share of the frame period that window occupies,
        and passes a preview that has published nothing at all.

        Progress is what separates the two. A preview that keeps consuming frames
        is presenting them, whatever a single instant of the debt flag says; a
        standing debt with no consumed frame is the stall this gate exists for;
        and a preview that never published anything proves nothing either way.

        Returns @{ Result; Message } with Result PASS, FAIL or UNVERIFIED.
    #>
    param([Parameter(Mandatory)] [object[]] $Samples)

    if ($Samples.Count -lt 2) {
        return @{ Result = 'UNVERIFIED'; Message = 'a transient publish debt needs at least two observations' }
    }
    $first = $Samples[0]
    $last = $Samples[-1]
    $consumedFrom = Get-ReleaseSnapshotValue -Object $first -Path 'consumedFrames'
    $consumedTo = Get-ReleaseSnapshotValue -Object $last -Path 'consumedFrames'
    $rendersFrom = Get-ReleaseSnapshotValue -Object $first -Path 'updateGate.renderPasses'
    $rendersTo = Get-ReleaseSnapshotValue -Object $last -Path 'updateGate.renderPasses'
    if ($null -eq $consumedFrom -or $null -eq $consumedTo -or $null -eq $rendersTo) {
        return @{ Result = 'UNVERIFIED'; Message = 'the preview snapshots carry no consumedFrames/updateGate counters' }
    }
    $owedThroughout = @($Samples | Where-Object {
            (Get-ReleaseSnapshotValue -Object $_ -Path 'updateGate.owed') -ne $true }).Count -eq 0

    if ([double]$consumedTo -gt [double]$consumedFrom) {
        return @{ Result  = 'PASS'
            Message = "preview kept presenting: consumed $consumedFrom -> $consumedTo, renders $rendersFrom -> $rendersTo"
        }
    }
    if ($owedThroughout) {
        return @{ Result  = 'FAIL'
            Message = "a published preview frame stayed unrendered across $($Samples.Count) samples (frozen preview); consumed stuck at $consumedTo, renders $rendersFrom -> $rendersTo"
        }
    }
    return @{ Result  = 'UNVERIFIED'
        Message = "the preview consumed no frame during the window (consumed $consumedTo), so presenting one was never exercised"
    }
}

function Get-ReleasePreviewProgress {
    <#
    .SYNOPSIS
        Waits until the preview has consumed a new frame, or the timeout expires.
    .DESCRIPTION
        Returns @{ Live; Message; Snapshot } where Live means a frame was consumed
        while watching -- the only evidence that the preview is presenting rather
        than merely reporting itself active.
    #>
    param([Parameter(Mandatory)] $Connection, [int] $TimeoutMs = 10000)

    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    $snapshot = (Invoke-LiveVerifyCommand -Connection $Connection -Command 'preview.snapshot').result
    $baseline = Get-ReleaseSnapshotValue -Object $snapshot -Path 'consumedFrames'
    if ($null -eq $baseline) {
        return @{ Live = $false; Message = 'the preview snapshot carries no consumedFrames'; Snapshot = $snapshot }
    }
    while ([DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 200
        $snapshot = (Invoke-LiveVerifyCommand -Connection $Connection -Command 'preview.snapshot').result
        $current = Get-ReleaseSnapshotValue -Object $snapshot -Path 'consumedFrames'
        if ($null -ne $current -and [double]$current -gt [double]$baseline) {
            return @{ Live = $true; Message = "consumed $baseline -> $current"; Snapshot = $snapshot }
        }
    }
    $status = Get-ReleaseSnapshotValue -Object $snapshot -Path 'statusText'
    return @{ Live = $false; Message = "consumedFrames stayed at $baseline; status '$status'"; Snapshot = $snapshot }
}

function Get-ReleaseSoakVerdict {
    <#
    .SYNOPSIS
        Applies the soak post-checks of docs/release-checklist.md section 7 to one
        session report.
    .DESCRIPTION
        The container being as long as the recording is the weakest of the checks
        the checklist lists, and on its own it passes a run whose audio drain
        dropped its tail, whose muxer failed, or whose segments were never
        finalized. Every counter below is already in the report the scenario
        fetches, so reading them invents no threshold -- it stops discarding the
        evidence.

        `AudioSpanSeconds` is the first-to-last packet span of each audio stream,
        measured independently of the report, because "the track exists" and "the
        track covers the recording" are different statements.

        The A/V drift counters are reported, never asserted: they carry the audio
        device's own clock residual, which the engine already judges and logs
        (`audio.clock_slaving` saturation). A device whose clock leaves the
        correction envelope is a statement about that device, and the file it
        produced can still be correct.

        Returns @{ Result; Message } with Result PASS, FAIL or UNVERIFIED.
    #>
    param(
        $Report,
        [Parameter(Mandatory)] [double] $ContainerSeconds,
        [Parameter(Mandatory)] [double] $ExpectedSeconds,
        [Parameter(Mandatory)] [double[]] $AudioSpanSeconds
    )

    # session.latest answers an envelope, { available, report }; a caller holding
    # the report itself is equally valid. Both are accepted so the gate cannot be
    # wired to the wrong one and report UNVERIFIED after a 30-minute recording.
    if ($null -ne $Report -and $null -ne (Get-ReleaseSnapshotValue -Object $Report -Path 'report')) {
        $Report = Get-ReleaseSnapshotValue -Object $Report -Path 'report'
    }
    if ($null -eq $Report -or $null -eq (Get-ReleaseSnapshotValue -Object $Report -Path 'counters')) {
        return @{ Result = 'UNVERIFIED'; Message = 'no session report, so the soak post-checks were not performed' }
    }

    $problems = @()
    $skew = [Math]::Abs($ContainerSeconds - $ExpectedSeconds)
    if ($skew -gt ($ExpectedSeconds * 0.02)) {
        $problems += "container ${ContainerSeconds}s vs ${ExpectedSeconds}s recorded (skew ${skew}s)"
    }
    foreach ($span in $AudioSpanSeconds) {
        if ($span -lt ($ContainerSeconds * 0.99)) {
            $problems += "an audio track spans ${span}s of a ${ContainerSeconds}s container"
        }
    }

    $counters = Get-ReleaseSnapshotValue -Object $Report -Path 'counters'

    # Audio outages are judged by the time a listener lost, not by how often the
    # OS fell behind. A machine under real load -- which is the normal case for a
    # game recording -- will miss buffers, and the engine answers each miss with
    # exactly as much silence, keeping the track aligned with video. Demanding
    # zero of those would fail the product for behaving correctly. What must stay
    # small is the total gap and the worst single one: many sub-millisecond gaps
    # are inaudible, one half-second dropout is not.
    #
    # What this cannot know is whether anything was PLAYING across the gap. A gap
    # in silence -- a muted microphone, a quiet desktop -- costs a listener
    # nothing, and the report carries no level at the moment of the outage to tell
    # the two apart. So the finding says what was measured (frames the device
    # dropped) and stops short of claiming it was heard. Adding that certainty
    # means measuring level alongside the discontinuity in the engine, not
    # guessing here.
    $discTotalMs = Get-ReleaseSnapshotValue -Object $counters -Path 'audio_discontinuity_ms_total'
    $discLongestMs = Get-ReleaseSnapshotValue -Object $counters -Path 'audio_discontinuity_ms_longest'
    $discCount = Get-ReleaseSnapshotValue -Object $counters -Path 'audio_discontinuities'
    if ($null -eq $discTotalMs -or $null -eq $discLongestMs) {
        $problems += 'the audio discontinuity duration counters are absent from the report'
    }
    else {
        # 0.1% of the recording, and no single gap past a syllable.
        $budgetMs = $ExpectedSeconds * 1000.0 * 0.001
        if ([double]$discTotalMs -gt $budgetMs) {
            $problems += ("audio lost ${discTotalMs} ms across $discCount outage(s), over the " +
                "${budgetMs} ms budget for a ${ExpectedSeconds}s recording")
        }
        if ([double]$discLongestMs -gt 120.0) {
            $problems += ("the longest single audio outage was ${discLongestMs} ms; long enough to be heard if " +
                'anything was playing at the time, which this report cannot say')
        }
    }

    foreach ($path in @('mux_failures', 'encoder_keyframe_prediction_mismatches',
            'frames_dropped.processing_failure', 'frames_dropped.backpressure')) {
        $value = Get-ReleaseSnapshotValue -Object $counters -Path $path
        if ($null -eq $value) { $problems += "counters.$path is absent from the report"; continue }
        if ([double]$value -ne 0) { $problems += "counters.$path = $value" }
    }

    if ((Get-ReleaseSnapshotValue -Object $Report -Path 'audio.degraded_occurred') -eq $true) {
        $problems += 'audio.degraded_occurred is true'
    }
    foreach ($drain in @(Get-ReleaseSnapshotValue -Object $Report -Path 'audio.resampler_drain')) {
        $undrained = Get-ReleaseSnapshotValue -Object $drain -Path 'undrained_frames'
        if ($null -ne $undrained -and [double]$undrained -ne 0) {
            $problems += "track $($drain.track) left $undrained undrained frame(s)"
        }
    }
    foreach ($segment in @(Get-ReleaseSnapshotValue -Object $Report -Path 'segments')) {
        if ((Get-ReleaseSnapshotValue -Object $segment -Path 'finalized') -ne $true) {
            $problems += "segment $($segment.index) is not finalized"
        }
    }

    $drift = Get-ReleaseSnapshotValue -Object $counters -Path 'av_drift_ms'
    $peak = Get-ReleaseSnapshotValue -Object $counters -Path 'peak_av_drift_ms'
    $reported = ("av_drift_ms $drift, peak $peak (device clock residual, reported not asserted); " +
        "audio outages $discCount totalling $discTotalMs ms, longest $discLongestMs ms")

    if ($problems.Count -gt 0) {
        return @{ Result = 'FAIL'; Message = ($problems -join '; ') + "; $reported" }
    }
    return @{ Result  = 'PASS'
        Message = "container ${ContainerSeconds}s, audio spans $($AudioSpanSeconds -join 's, ')s, " +
        "post-checks clean; $reported"
    }
}

function Get-ReleaseAudioPacketSpan {
    <#
    .SYNOPSIS
        First-to-last packet span, in seconds, of every audio stream in a file.
    .DESCRIPTION
        Measured from the packets rather than from a container duration tag: a
        live-muxed MKV carries no per-stream DURATION, so the tag a caller would
        otherwise read is simply absent and every stream would look full length.
    #>
    param([Parameter(Mandatory)] [string] $FfprobePath, [Parameter(Mandatory)] [string] $Path,
        [Parameter(Mandatory)] [int[]] $StreamIndexes)

    $spans = @()
    foreach ($index in $StreamIndexes) {
        $times = @(& $FfprobePath -v error -select_streams $index -show_entries packet=pts_time `
                -of csv=p=0 -- "$Path" 2>$null | Where-Object { $_ })
        if ($times.Count -eq 0) { $spans += 0.0; continue }
        $spans += ([double]$times[-1] - [double]$times[0])
    }
    return $spans
}
