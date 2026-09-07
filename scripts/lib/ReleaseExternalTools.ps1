#Requires -Version 7.0
<#
.SYNOPSIS
    The external oracles and actuators the human layer replaced people with.

.DESCRIPTION
    Every gate that stopped asking a person now asks a tool instead, and the tools
    are not ours. That is deliberate: an oracle we wrote would answer with the same
    ETW session, the same WASAPI call and the same window handle the product used,
    and agreeing with itself is not evidence.

    Three rules hold for all of them:

    1. A MISSING TOOL IS NEVER GREEN. It produces `precondition missing: ...` with
       the exact way to install it, and the scenario reports UNAVAILABLE -- an
       unmet requirement is not a failure (rule 3 of the runner), and it is not a
       pass either.
    2. NOTHING IS DISCOVERED BY GUESSING. Each tool has one environment variable
       that names it explicitly and one documented default location. A tool found
       by neither is absent, however many similarly named binaries are on PATH.
    3. EVERY INVOCATION GOES THROUGH ONE SEAM. `Invoke-ReleaseTool` is the only
       place this file starts a process, so a dry run can observe the whole human
       layer -- every gate RED once and GREEN once -- without a real machine
       action.
#>

Set-StrictMode -Version Latest

# The invocation seam. A dry run replaces it; nothing else here starts a process.
$script:ReleaseToolInvoker = $null

function Set-ReleaseToolInvoker {
    <#
    .SYNOPSIS
        Installs a simulated tool layer. Passing $null restores real execution.
    .DESCRIPTION
        The block receives the resolved tool path and its arguments and returns
        @{ ExitCode; Output }.
    #>
    param([scriptblock] $Invoker)
    $script:ReleaseToolInvoker = $Invoker
}

# The resolver seam, separate from the invoker: a dry run has to be able to say
# "this machine does NOT have PresentMon" as easily as "it does", and that is a
# different question from what the tool answers when it runs.
$script:ReleaseToolOverride = @{}

function Set-ReleaseToolAvailability {
    <#
    .SYNOPSIS
        Declares a tool present (at a fake path) or absent, for a dry run.
    #>
    param(
        [Parameter(Mandatory)] [string] $Name,
        [string] $Path
    )
    if ([string]::IsNullOrWhiteSpace($Path)) { $script:ReleaseToolOverride[$Name] = $null }
    else { $script:ReleaseToolOverride[$Name] = $Path }
}

function Clear-ReleaseToolAvailability {
    param()
    $script:ReleaseToolOverride = @{}
}

function Get-ReleaseToolCatalog {
    <#
    .SYNOPSIS
        Every external tool the release gates use, what it is for, and how to get it.
    .DESCRIPTION
        `Install` is printed verbatim into an UNAVAILABLE message, so it has to be
        the actual command or the actual page -- a reader of the report must be able
        to act on it without going looking.
    #>
    return [ordered]@{
        sandbox         = @{
            Purpose  = 'runs the install, update and packaging gates on a clean machine'
            Variable = 'EXOSNAP_SANDBOX_EXE'
            Default  = { Join-Path $env:WINDIR 'System32/WindowsSandbox.exe' }
            Install  = 'enable the Windows Sandbox feature (elevated, needs a reboot): ' +
            'Enable-WindowsOptionalFeature -Online -FeatureName Containers-DisposableClientVM'
        }
        presentmon      = @{
            Purpose  = 'the independent present-mode oracle beside our own ETW diagnostics'
            Variable = 'EXOSNAP_PRESENTMON'
            Default  = { 'PresentMon.exe' }
            Install  = 'download the Intel PresentMon CLI from https://github.com/GameTechDev/PresentMon/releases ' +
            'and either put PresentMon.exe on PATH or set EXOSNAP_PRESENTMON to it'
        }
        soundvolumeview = @{
            Purpose  = 'sets the default playback endpoint and its shared-mode format'
            Variable = 'EXOSNAP_SOUNDVOLUMEVIEW'
            Default  = { 'SoundVolumeView.exe' }
            Install  = 'download NirSoft SoundVolumeView from https://www.nirsoft.net/utils/sound_volume_view.html ' +
            'and either put SoundVolumeView.exe on PATH or set EXOSNAP_SOUNDVOLUMEVIEW to it'
        }
        pnputil         = @{
            Purpose  = 'disables and re-enables the audio device for the degradation gate'
            Variable = 'EXOSNAP_PNPUTIL'
            Default  = { Join-Path $env:WINDIR 'System32/pnputil.exe' }
            Install  = 'pnputil ships with Windows; if it is missing, the machine is not one a release may be cut from'
        }
        choco           = @{
            Purpose  = 'packs and installs the Chocolatey package in the rehearsal'
            Variable = 'EXOSNAP_CHOCO'
            Default  = { 'choco.exe' }
            Install  = 'install Chocolatey from https://chocolatey.org/install (the sandbox worker installs it itself)'
        }
    }
}

function Resolve-ReleaseTool {
    <#
    .SYNOPSIS
        Finds one external tool, or says truthfully that it is not here.
    .DESCRIPTION
        Returns @{ Name; Available; Path; Detail; Install }. `Detail` is written for
        a report reader: it names the variable that would have pointed at the tool
        and the way to install it, never just "not found".
    #>
    param([Parameter(Mandatory)] [string] $Name)

    $catalog = Get-ReleaseToolCatalog
    if (-not $catalog.Contains($Name)) { throw "No such release tool: $Name" }
    $entry = $catalog[$Name]

    if ($script:ReleaseToolOverride.ContainsKey($Name)) {
        $override = $script:ReleaseToolOverride[$Name]
        if ([string]::IsNullOrWhiteSpace($override)) {
            return @{ Name = $Name; Available = $false; Path = $null; Install = $entry.Install
                Detail     = "precondition missing: $Name ($($entry.Purpose)). $($entry.Install)"
            }
        }
        return @{ Name = $Name; Available = $true; Path = $override; Install = $entry.Install; Detail = 'simulated' }
    }

    $named = [Environment]::GetEnvironmentVariable($entry.Variable)
    if (-not [string]::IsNullOrWhiteSpace($named)) {
        if (Test-Path -LiteralPath $named) {
            return @{ Name = $Name; Available = $true; Path = $named; Install = $entry.Install
                Detail     = "named by $($entry.Variable)"
            }
        }
        return @{ Name = $Name; Available = $false; Path = $null; Install = $entry.Install
            Detail     = "precondition missing: $($entry.Variable) points at '$named', which does not exist"
        }
    }

    $default = & $entry.Default
    $resolved = $null
    if ($default -match '[\\/]') {
        if (Test-Path -LiteralPath $default) { $resolved = $default }
    }
    else {
        $command = Get-Command -Name $default -CommandType Application -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($null -ne $command) { $resolved = $command.Source }
    }
    if ($null -ne $resolved) {
        return @{ Name = $Name; Available = $true; Path = $resolved; Install = $entry.Install; Detail = 'found' }
    }
    return @{ Name = $Name; Available = $false; Path = $null; Install = $entry.Install
        Detail     = "precondition missing: $Name ($($entry.Purpose)). $($entry.Install)"
    }
}

function Invoke-ReleaseTool {
    <#
    .SYNOPSIS
        Runs one resolved external tool and returns @{ ExitCode; Output }.
    .DESCRIPTION
        The single process-starting seam of the human layer. A dry run replaces it
        and observes every gate without touching the machine; the real one captures
        stdout and stderr together, because a tool that explains its refusal on
        stderr is exactly the case a verdict has to be able to quote.
    #>
    param(
        [Parameter(Mandatory)] $Tool,
        [string[]] $Arguments = @(),
        [int] $TimeoutSeconds = 120
    )
    if (-not $Tool.Available) { throw "Invoke-ReleaseTool called for an absent tool: $($Tool.Name)" }
    if ($null -ne $script:ReleaseToolInvoker) {
        $simulated = & $script:ReleaseToolInvoker $Tool $Arguments
        if ($null -eq $simulated) { return @{ ExitCode = 0; Output = '' } }
        return $simulated
    }
    $output = & $Tool.Path @Arguments 2>&1 | Out-String
    return @{ ExitCode = $LASTEXITCODE; Output = $output }
}

function New-ReleaseMissingToolResult {
    <#
    .SYNOPSIS
        The scenario result for a tool this machine does not have.
    .DESCRIPTION
        UNAVAILABLE, never FAIL and never PASS: nothing was measured, and the
        absence of an oracle is a fact about the desk. The message begins with
        `precondition missing` so a report can be searched for exactly the set of
        things somebody has to install before the next campaign.
    #>
    param([Parameter(Mandatory)] $Tool)
    return @{ Result = 'UNAVAILABLE'; Message = $Tool.Detail }
}

# ---------------------------------------------------------------------------
# PresentMon: the independent present-mode oracle
# ---------------------------------------------------------------------------

function Get-ReleasePresentMonObservation {
    <#
    .SYNOPSIS
        What PresentMon saw for one process over a short window.
    .DESCRIPTION
        Returns @{ Ok; Detail; Frames; PresentModes }. The CLI writes a CSV whose
        `PresentMode` column carries Intel's vocabulary ("Hardware: Independent
        Flip", "Hardware: Legacy Flip", "Composed: Flip", ...), which is the whole
        reason this exists: our own classification is derived from the same ETW
        events, so only a second decoder of those events can contradict it.

        A short capture with no rows is not a failure of the oracle -- a process
        that presented nothing produces no rows -- so `Frames = 0` is reported as
        such and left to the caller to judge.
    #>
    param(
        [Parameter(Mandatory)] $Tool,
        # Omitted deliberately by the gates that do not own the presenting process.
        # REL-PRESENT-002 asserts that a real-time ETW session works on this machine
        # at all, and the desktop-wide capture is the corroboration for that claim;
        # only REL-CAP-FSE-001 starts the process it is measuring and can name one.
        [int] $ProcessId = 0,
        [Parameter(Mandatory)] [string] $CsvPath,
        [int] $Seconds = 5
    )
    $arguments = @()
    if ($ProcessId -gt 0) { $arguments += @('--process_id', "$ProcessId") }
    $arguments += @(
        '--output_file', $CsvPath,
        '--timed', "$Seconds",
        '--terminate_after_timed',
        '--stop_existing_session',
        '--no_top'
    )
    $run = Invoke-ReleaseTool -Tool $Tool -Arguments $arguments -TimeoutSeconds ($Seconds + 60)
    if ($run.ExitCode -ne 0) {
        return @{ Ok = $false; Detail = "PresentMon exited $($run.ExitCode): $($run.Output)"; Frames = 0; PresentModes = @() }
    }
    if (-not (Test-Path -LiteralPath $CsvPath)) {
        return @{ Ok = $false; Detail = 'PresentMon wrote no CSV'; Frames = 0; PresentModes = @() }
    }
    $rows = @(Import-Csv -LiteralPath $CsvPath)
    $modes = @($rows | ForEach-Object { "$($_.PresentMode)" } | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
            Sort-Object -Unique)
    return @{ Ok = $true; Frames = $rows.Count; PresentModes = $modes
        Detail          = "$($rows.Count) present(s), mode(s): $(if ($modes.Count -gt 0) { $modes -join ', ' } else { 'none' })"
    }
}

function Test-ReleasePresentModeAgreement {
    <#
    .SYNOPSIS
        Whether PresentMon's vocabulary agrees with ours for the same window.
    .DESCRIPTION
        Our `exclusiveFullscreen` is Intel's "Hardware: Legacy Flip" or "Hardware:
        Legacy Copy to front buffer"; our `independentFlip` is "Hardware:
        Independent Flip"; everything composed is "Composed: *". The mapping is
        one-way on purpose: this answers "does the oracle contradict us", not "what
        would the oracle have called it", so an unmapped mode is reported as
        undecidable rather than as a disagreement.
    #>
    param(
        [Parameter(Mandatory)] [string] $OurMode,
        [Parameter(Mandatory)] [AllowEmptyCollection()] [string[]] $PresentMonModes
    )
    $expected = switch ($OurMode) {
        'exclusiveFullscreen' { @('Hardware: Legacy Flip', 'Hardware: Legacy Copy to front buffer') }
        'independentFlip' { @('Hardware: Independent Flip', 'Hardware Composed: Independent Flip') }
        'composed' { @('Composed: Flip', 'Composed: Copy with GPU GDI', 'Composed: Copy with CPU GDI') }
        default { @() }
    }
    if ($expected.Count -eq 0) {
        return @{ Decidable = $false; Agrees = $false
            Detail          = "no PresentMon equivalent is defined for present mode '$OurMode'"
        }
    }
    if ($PresentMonModes.Count -eq 0) {
        return @{ Decidable = $false; Agrees = $false; Detail = 'PresentMon observed no presents to compare against' }
    }
    $agreeing = @($PresentMonModes | Where-Object { $_ -in $expected })
    if ($agreeing.Count -gt 0) {
        return @{ Decidable = $true; Agrees = $true
            Detail          = "PresentMon agrees: '$($agreeing -join ', ')' for our '$OurMode'"
        }
    }
    return @{ Decidable = $true; Agrees = $false
        Detail          = "PresentMon saw '$($PresentMonModes -join ', ')' where we classified '$OurMode'"
    }
}

# ---------------------------------------------------------------------------
# Audio endpoints
# ---------------------------------------------------------------------------

function Set-ReleaseDefaultAudioEndpoint {
    <#
    .SYNOPSIS
        Makes one render endpoint the default for every role.
    .DESCRIPTION
        Windows exposes no documented setter for this -- `default-roles` is
        ENV_HUMAN in the envctl catalogue for exactly that reason, and envctl
        refuses to write it by policy. So the mechanism stays OUTSIDE the release
        path in a named third-party tool, the same arrangement
        REL-AUD-DEGRADE-001 already uses for endpoint visibility: the runner knows
        what it wants, not how, and a machine without the tool reports the gate
        unavailable rather than passing it.
    #>
    param(
        [Parameter(Mandatory)] $Tool,
        [Parameter(Mandatory)] [string] $EndpointName
    )
    $run = Invoke-ReleaseTool -Tool $Tool -Arguments @('/SetDefault', $EndpointName, 'all')
    if ($run.ExitCode -ne 0) {
        return @{ Ok = $false; Detail = "SoundVolumeView /SetDefault '$EndpointName' exited $($run.ExitCode): $($run.Output)" }
    }
    return @{ Ok = $true; Detail = "'$EndpointName' is the default render endpoint for every role" }
}

function Set-ReleaseAudioEndpointFormat {
    <#
    .SYNOPSIS
        Sets one render endpoint's shared-mode format.
    .DESCRIPTION
        The `Default Format` drop-down in Sound settings, driven by the only tool
        that offers it. Same boundary as the default role: `device-format` is
        ENV_HUMAN, so this cannot be an envctl transaction and does not pretend to
        be one -- the caller restores the previous value itself.
    #>
    param(
        [Parameter(Mandatory)] $Tool,
        [Parameter(Mandatory)] [string] $EndpointName,
        [Parameter(Mandatory)] [int] $SampleRate,
        [int] $BitDepth = 24,
        [int] $Channels = 2
    )
    $format = "$Channels Channels, $BitDepth Bit, $SampleRate Hz"
    $run = Invoke-ReleaseTool -Tool $Tool -Arguments @('/SetDefaultFormat', $EndpointName, $format)
    if ($run.ExitCode -ne 0) {
        return @{ Ok = $false; Detail = "SoundVolumeView /SetDefaultFormat '$EndpointName' exited $($run.ExitCode): $($run.Output)" }
    }
    return @{ Ok = $true; Detail = "'$EndpointName' shared-mode format set to $format" }
}

function Set-ReleaseDeviceEnabled {
    <#
    .SYNOPSIS
        Disables or re-enables one PnP device by instance id.
    .DESCRIPTION
        The documented, reversible way to make an audio endpoint disappear without
        anybody unplugging a cable. It needs an elevated token; an unelevated call
        is refused by pnputil itself and reported as such rather than retried.
    #>
    param(
        [Parameter(Mandatory)] $Tool,
        [Parameter(Mandatory)] [string] $InstanceId,
        [Parameter(Mandatory)] [bool] $Enabled
    )
    $verb = if ($Enabled) { '/enable-device' } else { '/disable-device' }
    $run = Invoke-ReleaseTool -Tool $Tool -Arguments @($verb, $InstanceId)
    if ($run.ExitCode -ne 0) {
        return @{ Ok = $false; Detail = "pnputil $verb '$InstanceId' exited $($run.ExitCode): $($run.Output)" }
    }
    return @{ Ok = $true; Detail = "$InstanceId $(if ($Enabled) { 'enabled' } else { 'disabled' })" }
}

# ---------------------------------------------------------------------------
# UI Automation: the only reader the capture-excluded overlays have
# ---------------------------------------------------------------------------

function Get-ReleaseAutomationElements {
    <#
    .SYNOPSIS
        The automation tree of one process, as name/class/type triples.
    .DESCRIPTION
        WDA_EXCLUDEFROMCAPTURE defeats screenshots, screen recording and
        PrintWindow, and it defeats them by design -- but it does not touch the
        automation tree, so UI Automation is the one reader that can say a toast
        or an overlay is really on the desktop with really that text.

        What it CANNOT say is what colour anything is. That is why the sight check
        survives: presence, text and the severity glyph's accessible name are
        asserted here, and colour is judged by a person.

        Returns @{ Ok; Detail; Elements }. A machine whose runtime cannot load
        UIAutomationClient reports Ok = $false rather than an empty tree, because
        an empty tree and an unreadable one are the opposite verdicts.
    #>
    param(
        [Parameter(Mandatory)] [int] $ProcessId,
        [int] $TimeoutSeconds = 10
    )
    if ($null -ne $script:ReleaseToolInvoker) {
        # The dry run answers this the same way it answers a process: through the
        # one seam, so a simulated toast is as observable as a real one.
        $simulated = & $script:ReleaseToolInvoker @{ Name = 'uia'; Available = $true; Path = 'uia' } @("$ProcessId")
        if ($null -eq $simulated) { return @{ Ok = $false; Detail = 'no simulated automation tree'; Elements = @() } }
        return $simulated
    }
    try { Add-Type -AssemblyName UIAutomationClient -ErrorAction Stop }
    catch {
        return @{ Ok = $false; Elements = @()
            Detail   = "precondition missing: UIAutomationClient could not be loaded ($($_.Exception.Message)); " +
            'the capture-excluded overlays have no other reader'
        }
    }
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $elements = @()
    while ([DateTime]::UtcNow -lt $deadline) {
        $condition = [System.Windows.Automation.PropertyCondition]::new(
            [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $ProcessId)
        $windows = [System.Windows.Automation.AutomationElement]::RootElement.FindAll(
            [System.Windows.Automation.TreeScope]::Children, $condition)
        $found = @()
        foreach ($window in $windows) {
            $found += [pscustomobject]@{
                Name        = "$($window.Current.Name)"
                ClassName   = "$($window.Current.ClassName)"
                ControlType = "$($window.Current.ControlType.ProgrammaticName)"
            }
            $descendants = $window.FindAll([System.Windows.Automation.TreeScope]::Descendants,
                [System.Windows.Automation.Condition]::TrueCondition)
            foreach ($element in $descendants) {
                $found += [pscustomobject]@{
                    Name        = "$($element.Current.Name)"
                    ClassName   = "$($element.Current.ClassName)"
                    ControlType = "$($element.Current.ControlType.ProgrammaticName)"
                }
            }
        }
        if ($found.Count -gt 0) { $elements = $found; break }
        Start-Sleep -Milliseconds 250
    }
    return @{ Ok = $true; Elements = $elements; Detail = "$($elements.Count) automation element(s)" }
}

function Test-ReleaseAutomationText {
    <#
    .SYNOPSIS
        Whether the automation tree carries every required piece of text.
    .DESCRIPTION
        Substring matching, case-insensitively, against every element name: the
        assertion is that the words reached the desktop, not that a particular
        control owns them. Reports which ones were missing, because "the toast is
        not there" and "the toast is there with the wrong severity word" are two
        different findings.
    #>
    param(
        [Parameter(Mandatory)] [AllowEmptyCollection()] [object[]] $Elements,
        [Parameter(Mandatory)] [string[]] $Required
    )
    $names = @($Elements | ForEach-Object { "$($_.Name)" })
    $missing = @($Required | Where-Object {
            $needle = $_
            -not @($names | Where-Object { $_ -like "*$needle*" }).Count
        })
    if ($missing.Count -gt 0) {
        return @{ Ok = $false; Detail = "not on the desktop: $($missing -join ' | ')" }
    }
    return @{ Ok = $true; Detail = "all $($Required.Count) required string(s) present in the automation tree" }
}
