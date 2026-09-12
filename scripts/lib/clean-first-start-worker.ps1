#Requires -Version 7.0
<#
.SYNOPSIS
    The clean-first-start gate, executed inside a machine that has never seen ExoSnap.

.DESCRIPTION
    Clean is a state, not a screen. What a first start has to be measured against is a
    machine with nothing of ExoSnap on it: no install, no per-user configuration, no
    recovery manifest, no update state, no per-user registry key. Each of those
    changes what the first start does -- a recovery manifest in particular opens the
    recovery surface, and defaults asserted behind it are asserted against the wrong
    window -- so this establishes the state before it measures anything, and a machine
    that was not clean is the harness failing to set the test up rather than a defect.

    What it then measures is state, not appearance. The application starts, reports
    its defaults over the control channel, enters no migration or recovery path,
    shuts down cleanly, and starts a second time still healthy. The visual first-run
    surfaces belong to the visual scenarios; pressing them into this gate would make a
    state test fail for a pixel.

    The upgrade direction is not repeated here. REL-UPD-MSI-001 already installs an
    older release and updates it to the bound candidate.

    Every step appends to `steps` with a name, an ok flag, a detail and a kind, and
    the whole document is written even when a step throws: a step that was never
    reached must read as never reached, not as absent. The marker file is written
    last, in a finally block, because the host waits on it to know the machine is
    done at all.
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $StagingDirectory,
    # The candidate, as an installer. Installed here rather than assumed present:
    # the whole point of a disposable machine is that nothing is.
    [Parameter(Mandatory)] [string] $MsiPath,
    [Parameter(Mandatory)] [string] $ResultPath,
    [Parameter(Mandatory)] [string] $MarkerPath,
    [Parameter(Mandatory)] [string] $EvidenceDirectory,
    # The candidate the campaign bound. The started application is compared against
    # this, not against "something is installed now".
    [Parameter(Mandatory)] [string] $ExpectedVersion,
    [int] $StartTimeoutSeconds = 90
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Import-Module (Join-Path $StagingDirectory 'LiveVerifyClient.psm1') -Force

$script:Steps = [System.Collections.Generic.List[object]]::new()
New-Item -ItemType Directory -Path $EvidenceDirectory -Force | Out-Null
$script:LogDirectory = Join-Path $EvidenceDirectory 'logs'
New-Item -ItemType Directory -Path $script:LogDirectory -Force | Out-Null

function Add-Step {
    <#
    .SYNOPSIS
        Record one step, saying whether it is a product assertion or the test
        environment being built.
    .DESCRIPTION
        bootstrap: establishing or checking the state the assertions need. A machine
        that already had ExoSnap on it, or an install that would not run, measured
        nothing about the product.

        product: what the gate is asserting about the started application.

        There is no default. An unclassified step is indistinguishable from either
        kind, so reading it as a product assertion can accuse ExoSnap of the harness's
        own setup problem; the host leaves such a run unverified instead.
    #>
    param(
        [Parameter(Mandatory)] [string] $Name,
        [Parameter(Mandatory)] [bool] $Ok,
        [string] $Detail = '',
        [Parameter(Mandatory)] [ValidateSet('product', 'bootstrap')] [string] $Kind
    )
    $script:Steps.Add([pscustomobject]@{ name = $Name; ok = $Ok; detail = $Detail; kind = $Kind })
    Write-Host "  $(if ($Ok) { 'ok  ' } else { 'FAIL' })  $Name  [$Kind]  $Detail"
}

function Write-Result {
    param([string] $Fatal = '')
    $document = [pscustomobject]@{
        finishedUtc = [DateTime]::UtcNow.ToString('o')
        fatal       = $Fatal
        steps       = @($script:Steps)
    }
    $json = $document | ConvertTo-Json -Depth 12
    Set-Content -LiteralPath $ResultPath -Value $json -Encoding utf8NoBOM
    Set-Content -LiteralPath (Join-Path $EvidenceDirectory 'result.json') -Value $json -Encoding utf8NoBOM
}

function Get-ExoSnapResidue {
    <#
    .SYNOPSIS
        Everything of an earlier ExoSnap still on this machine.
    .DESCRIPTION
        All of it at once. Each of these is cleared by hand or by starting from a
        fresh image, so learning about them one campaign at a time is one machine
        rebuild per residue.
    #>
    [OutputType([string[]])]
    param()
    $found = @()

    $installed = @(Get-ChildItem -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall' `
            -ErrorAction SilentlyContinue |
        ForEach-Object { Get-ItemProperty -LiteralPath $_.PSPath -ErrorAction SilentlyContinue } |
        Where-Object { $_.DisplayName -eq 'ExoSnap' })
    if ($installed.Count -gt 0) {
        $found += "an installed product (ExoSnap $($installed[0].DisplayVersion))"
    }

    $userConfig = Join-Path $env:LOCALAPPDATA 'ExoSnap'
    if (Test-Path -LiteralPath $userConfig) { $found += "a per-user configuration directory ($userConfig)" }

    # Named separately from the configuration directory even though it lives inside
    # it: this is the residue that changes what the first start LOOKS like, and a run
    # that hit it has to say so rather than reporting a generic leftover.
    $recovery = Join-Path $userConfig 'recovery'
    if (Test-Path -LiteralPath $recovery) { $found += "recovery state from a previous recording run ($recovery)" }

    $update = Join-Path $userConfig 'update'
    if (Test-Path -LiteralPath $update) { $found += "update state from an earlier install ($update)" }

    if (Test-Path -LiteralPath 'HKCU:\SOFTWARE\Codexo\ExoSnap') {
        $found += 'a per-user registry key (HKCU:\SOFTWARE\Codexo\ExoSnap)'
    }

    return $found
}

function Invoke-Msi {
    param([Parameter(Mandatory)] [string] $Arguments, [Parameter(Mandatory)] [string] $LogName)
    $log = Join-Path $script:LogDirectory $LogName
    $process = Start-Process -FilePath 'msiexec.exe' -ArgumentList "$Arguments /qn /l*v `"$log`"" -Wait -PassThru
    return @{ ExitCode = $process.ExitCode; Log = $log }
}

function Get-InstalledExoSnap {
    $candidates = @(
        (Join-Path $env:ProgramFiles 'Codexo\ExoSnap\exosnap.exe'),
        (Join-Path ${env:ProgramFiles(x86)} 'Codexo\ExoSnap\exosnap.exe'))
    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate)) { return $candidate }
    }
    return $null
}

function Start-ExoSnapSession {
    <#
    .SYNOPSIS
        Starts ExoSnap under a control channel and connects to it.
    .DESCRIPTION
        Connecting IS the readiness observation: the pipe does not exist until the
        application's control server has started, so a connection that succeeded is a
        started application and no sleep is needed to believe it.
    #>
    param(
        [Parameter(Mandatory)] [string] $ExePath,
        [Parameter(Mandatory)] [string] $RunId
    )
    $process = Start-Process -FilePath $ExePath -PassThru -ArgumentList @('--live-verify-control', $RunId)
    $connection = Connect-LiveVerify -RunId $RunId -ConnectTimeoutMs ($StartTimeoutSeconds * 1000)
    return @{ Process = $process; Connection = $connection; RunId = $RunId }
}

function Close-ExoSnapSession {
    <#
    .SYNOPSIS
        Closes the application the way a person does, and says whether it went cleanly.
    .DESCRIPTION
        There is no quit command on the control channel. Adding one for this gate
        would be a test-only route around the shutdown the product actually has, so
        this asks the window to close and reports what happened -- a process that had
        to be killed did not shut down cleanly, and that is the finding.
    #>
    param([Parameter(Mandatory)] $Process)
    try {
        [void]$Process.CloseMainWindow()
        if (-not $Process.WaitForExit($StartTimeoutSeconds * 1000)) {
            $Process.Kill()
            return @{ Ok = $false; Detail = "the window closed but the process did not exit within $StartTimeoutSeconds s" }
        }
    }
    catch {
        return @{ Ok = $false; Detail = "closing the main window threw: $($_.Exception.Message)" }
    }
    return @{ Ok = ($Process.ExitCode -eq 0); Detail = "the session exited $($Process.ExitCode)" }
}

function Stop-ExoSnapProcesses {
    foreach ($name in 'exosnap', 'exosnap-updater') {
        foreach ($process in @(Get-Process -Name $name -ErrorAction SilentlyContinue)) {
            try {
                [void]$process.CloseMainWindow()
                if (-not $process.WaitForExit(8000)) { $process.Kill() }
            }
            catch { }
        }
    }
}

try {
    # ---- the state the measurement needs ---------------------------------------
    $residue = Get-ExoSnapResidue
    if ($residue.Count -gt 0) {
        Add-Step -Name 'clean-precondition' -Ok $false -Kind 'bootstrap' `
            -Detail ('this machine is not clean, so a first start cannot be measured on it: ' +
                ($residue -join ', '))
        Write-Result
        return
    }
    Add-Step -Name 'clean-precondition' -Ok $true -Kind 'bootstrap' `
        -Detail 'no install, configuration, recovery state, update state or per-user registry key'

    $install = Invoke-Msi -Arguments "/i `"$MsiPath`"" -LogName 'install.log'
    if ($install.ExitCode -ne 0) {
        Add-Step -Name 'install-candidate' -Ok $false -Kind 'bootstrap' `
            -Detail "msiexec exited $($install.ExitCode); see $($install.Log)"
        Write-Result
        return
    }
    $exe = Get-InstalledExoSnap
    if (-not $exe) {
        Add-Step -Name 'install-candidate' -Ok $false -Kind 'bootstrap' `
            -Detail 'msiexec succeeded but no exosnap.exe is where the MSI declares it'
        Write-Result
        return
    }
    Add-Step -Name 'install-candidate' -Ok $true -Kind 'bootstrap' -Detail $exe

    # ---- what the started application does -------------------------------------
    #
    # Read over the control channel the product already exposes. Nothing here is a
    # command invented for this gate: app.identity is what the handshake carries,
    # ui.getState is what every other live check reads, and settings.snapshot is the
    # settings the application actually initialised.
    $first = Start-ExoSnapSession -ExePath $exe -RunId ('clean-first-' + [guid]::NewGuid().ToString('N').Substring(0, 8))

    $identity = Invoke-LiveVerifyCommand -Connection $first.Connection -Command 'app.identity'
    $version = "$($identity.result.version)"
    Add-Step -Name 'first-start-identity' -Ok ($version -eq $ExpectedVersion) -Kind 'product' `
        -Detail "the first start reports $version; the campaign bound $ExpectedVersion"

    # A machine on which ExoSnap has never recorded has nothing to recover and
    # from, so entering either path is the product inventing state that is not there.
    $state = Get-LiveVerifyState -Connection $first.Connection
    $surface = "$($state.blockingSurface)"
    $quiet = [string]::IsNullOrEmpty($surface) -or $surface -eq 'none'
    Add-Step -Name 'first-start-no-recovery' -Ok $quiet -Kind 'product' `
        -Detail "blockingSurface on a machine ExoSnap has never run on: '$surface'"

    $snapshot = Invoke-LiveVerifyCommand -Connection $first.Connection -Command 'settings.snapshot'
    $snapshot.result | ConvertTo-Json -Depth 8 |
        Set-Content -LiteralPath (Join-Path $EvidenceDirectory 'first-start-settings.json') -Encoding utf8NoBOM

    $empty = @($snapshot.result.PSObject.Properties |
            Where-Object { $null -eq $_.Value -or "$($_.Value)" -eq '' } |
            ForEach-Object { "$($_.Name) is empty on a first start" })
    $settingCount = @($snapshot.result.PSObject.Properties).Count
    $detail = if ($empty.Count -eq 0) { "$settingCount setting(s) initialised" } else { $empty -join '; ' }
    Add-Step -Name 'first-start-defaults' -Ok ($empty.Count -eq 0 -and $settingCount -gt 0) -Kind 'product' `
        -Detail $detail

    # No quit command exists on the control channel, and inventing one for this gate
    # would be a test-only route around the shutdown the product actually has.
    $first.Connection.Close()
    $closed = Close-ExoSnapSession -Process $first.Process
    Add-Step -Name 'first-start-shutdown' -Ok $closed.Ok -Kind 'product' -Detail $closed.Detail

    # ---- and again, on the state the first start left behind --------------------
    $second = Start-ExoSnapSession -ExePath $exe -RunId ('clean-second-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
    $secondSurface = "$((Get-LiveVerifyState -Connection $second.Connection).blockingSurface)"
    Add-Step -Name 'second-start-healthy' `
        -Ok ([string]::IsNullOrEmpty($secondSurface) -or $secondSurface -eq 'none') -Kind 'product' `
        -Detail ('a clean shutdown must not leave state that opens a surface on the next start; ' +
            "blockingSurface: '$secondSurface'")
    $second.Connection.Close()
    Close-ExoSnapSession -Process $second.Process | Out-Null

    Write-Result
}
catch {
    # The worker itself threw, so the environment is what failed -- nothing below the
    # throw measured anything.
    Add-Step -Name 'worker' -Ok $false -Kind 'bootstrap' -Detail "$($_.Exception.Message)"
    Write-Result -Fatal "$($_.Exception.Message)"
}
finally {
    Stop-ExoSnapProcesses
    Set-Content -LiteralPath $MarkerPath -Value ([DateTime]::UtcNow.ToString('o')) -Encoding utf8NoBOM
}
