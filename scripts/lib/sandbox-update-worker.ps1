#Requires -Version 7.0
<#
.SYNOPSIS
    The update gates, executed inside a clean Windows Sandbox.

.DESCRIPTION
    Runs REL-UPD-MSI-DECLINE-001 and REL-UPD-MSI-001 back to back on a machine that
    has never seen ExoSnap: install an older release from its MSI, decline an update,
    then accept one. On the developer's own machine those two could only ever run in
    that order and only once -- the accept installs the new version over the older
    starting point both of them need -- and a sandbox makes the order a convenience
    rather than a constraint, because the next run starts from nothing again.

    The elevation prompt is gone twice over. The sandbox logon command already holds
    an administrative token, so msiexec raises none; and the DECLINE is produced by
    the updater's own fault-injection seam (EXOSNAP_UPDATER_FAULT=uacDeclined), which
    returns ERROR_CANCELLED from the elevation call without a Secure Desktop ever
    being involved. The product assertion is unchanged: failureCase uacDeclined with
    the installation intact.

    Every step appends to `steps` with a name, an ok flag and a detail, and the whole
    document is written even when a step throws -- a step that was never reached must
    read as never reached, not as absent. The marker file is written last, in a
    finally block, because the host waits on it to know the sandbox is done at all.
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $StagingDirectory,
    # The older release to update FROM, as an installer. It is installed here rather
    # than assumed present: the whole point of the sandbox is that nothing is.
    [Parameter(Mandatory)] [string] $BaseMsiPath,
    [Parameter(Mandatory)] [string] $ResultPath,
    [Parameter(Mandatory)] [string] $MarkerPath,
    [string] $UpdateChannel = 'Preview',
    [int] $OfferTimeoutSeconds = 90
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:Steps = [System.Collections.Generic.List[object]]::new()
$script:LogDirectory = Join-Path $StagingDirectory 'logs'
New-Item -ItemType Directory -Path $script:LogDirectory -Force | Out-Null

function Add-Step {
    param([Parameter(Mandatory)] [string] $Name, [Parameter(Mandatory)] [bool] $Ok, [string] $Detail = '')
    $script:Steps.Add([pscustomobject]@{ name = $Name; ok = $Ok; detail = $Detail })
    Write-Host "  $(if ($Ok) { 'ok  ' } else { 'FAIL' })  $Name  $Detail"
}

function Write-Result {
    param([string] $Fatal = '')
    $document = [pscustomobject]@{
        finishedUtc = [DateTime]::UtcNow.ToString('o')
        fatal       = $Fatal
        steps       = @($script:Steps)
    }
    Set-Content -LiteralPath $ResultPath -Value ($document | ConvertTo-Json -Depth 12) -Encoding utf8NoBOM
}

function Invoke-Msi {
    param([Parameter(Mandatory)] [string] $Arguments, [Parameter(Mandatory)] [string] $LogName)
    $log = Join-Path $script:LogDirectory $LogName
    $process = Start-Process -FilePath 'msiexec.exe' -ArgumentList "$Arguments /qn /l*v `"$log`"" -Wait -PassThru
    return @{ ExitCode = $process.ExitCode; Log = $log }
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

function Get-InstalledExoSnap {
    <#
    .SYNOPSIS
        The installed exosnap.exe, read from the location the MSI declares.
    #>
    $key = 'HKLM:\SOFTWARE\Codexo\ExoSnap'
    if (-not (Test-Path -LiteralPath $key)) { return $null }
    $path = (Get-ItemProperty -LiteralPath $key -ErrorAction SilentlyContinue).InstallPath
    if ([string]::IsNullOrWhiteSpace($path)) { return $null }
    $exe = Join-Path $path 'exosnap.exe'
    if (-not (Test-Path -LiteralPath $exe)) { return $null }
    return $exe
}

function Start-ControlledApp {
    <#
    .SYNOPSIS
        Launches the installed app with a control channel and returns both handles.
    .DESCRIPTION
        The single-instance guard is machine-wide even here: a second launch hands
        focus to whatever is running and exits without ever opening a pipe, so every
        caller stops the previous instance first.
    #>
    param([Parameter(Mandatory)] [string] $ExePath, [int] $ConnectTimeoutMs = 60000)
    Stop-ExoSnapProcesses
    $runId = New-LiveVerifyRunId
    $process = Start-Process -FilePath $ExePath -PassThru -ArgumentList @('--live-verify-control', $runId)
    $connection = Connect-LiveVerify -RunId $runId -ConnectTimeoutMs $ConnectTimeoutMs
    return @{ Process = $process; Connection = $connection; RunId = $runId }
}

function Wait-UpdateOffer {
    param([Parameter(Mandatory)] $Connection, [int] $TimeoutSeconds)
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $state = $null
    while ([DateTime]::UtcNow -lt $deadline) {
        $state = (Invoke-LiveVerifyCommand -Connection $Connection -Command 'update.getState').result
        $available = $state.PSObject.Properties['updateAvailable']
        if ($null -ne $available -and $available.Value) { return $state }
        Start-Sleep -Milliseconds 500
    }
    return $state
}

try {
    Import-Module (Join-Path $StagingDirectory 'LiveVerifyClient.psm1') -Force -DisableNameChecking

    # ---------------------------------------------------------------- base install
    $install = Invoke-Msi -Arguments "/i `"$BaseMsiPath`"" -LogName 'install-base.log'
    if ($install.ExitCode -ne 0) {
        Add-Step -Name 'install-base' -Ok $false -Detail "msiexec exited $($install.ExitCode); see logs/install-base.log"
        Write-Result -Fatal 'the older release could not be installed, so there was nothing to update from'
        return
    }
    $installedExe = Get-InstalledExoSnap
    if ($null -eq $installedExe) {
        Add-Step -Name 'install-base' -Ok $false -Detail 'the MSI reported success but HKLM:\SOFTWARE\Codexo\ExoSnap names no installed exosnap.exe'
        Write-Result -Fatal 'no installed build to update from'
        return
    }
    Add-Step -Name 'install-base' -Ok $true -Detail $installedExe

    # ------------------------------------------------------------- channel + offer
    # The channel is read when the update service starts, so it takes a restart to
    # be the channel the next check actually uses. Setting it and checking in one
    # session asks the channel the app was LAUNCHED with.
    $app = Start-ControlledApp -ExePath $installedExe
    try {
        $set = Invoke-LiveVerifyCommand -Connection $app.Connection -Command 'settings.set' `
            -Parameters @{ key = 'app.updateChannel'; value = $UpdateChannel }
        if (-not $set.ok) {
            Add-Step -Name 'select-channel' -Ok $false -Detail "settings.set refused: $($set.error.message)"
            Write-Result -Fatal 'the update channel could not be selected'
            return
        }
        $before = (Invoke-LiveVerifyCommand -Connection $app.Connection -Command 'app.identity').result
        $beforeVersion = "$($before.productVersion)"
        Add-Step -Name 'select-channel' -Ok $true -Detail "$UpdateChannel, installed version $beforeVersion"
    }
    finally { try { $app.Connection.Close() } catch { } }

    # --------------------------------------------------------------------- decline
    # The fault seam is armed for THIS launch only, in the child's environment, so
    # the accept run below cannot inherit it and report a decline it never asked for.
    $env:EXOSNAP_UPDATER_FAULT = 'uacDeclined'
    $app = Start-ControlledApp -ExePath $installedExe
    $declineOk = $false
    try {
        $checked = Invoke-LiveVerifyCommand -Connection $app.Connection -Command 'update.check'
        if (-not $checked.ok) {
            Add-Step -Name 'decline-offer' -Ok $false -Detail "update.check refused: $($checked.error.message)"
        }
        else {
            $state = Wait-UpdateOffer -Connection $app.Connection -TimeoutSeconds $OfferTimeoutSeconds
            $offered = $null -ne $state -and $null -ne $state.PSObject.Properties['updateAvailable'] -and $state.updateAvailable
            if (-not $offered) {
                Add-Step -Name 'decline-offer' -Ok $false -Detail "no update is offered to $beforeVersion on the $UpdateChannel channel"
            }
            else {
                Add-Step -Name 'decline-offer' -Ok $true -Detail "an update is offered to $beforeVersion"
                $applied = Invoke-LiveVerifyCommand -Connection $app.Connection -Command 'update.apply'
                if (-not $applied.ok) {
                    Add-Step -Name 'decline-apply' -Ok $false -Detail "update.apply refused: $($applied.error.message)"
                }
                else {
                    $launch = (Invoke-LiveVerifyCommand -Connection $app.Connection -Command 'update.getState').result.updaterLaunch
                    Add-Step -Name 'decline-apply' -Ok $true -Detail "updater run id $($launch.controlRunId)"
                    $updater = Connect-LiveVerify -RunId "$($launch.controlRunId)" -Role 'Updater' -ConnectTimeoutMs 30000
                    try {
                        # The failure lands asynchronously: the elevation call is made
                        # on the worker thread, so reading the state once could only
                        # ever see the phase before it was attempted.
                        $deadline = [DateTime]::UtcNow.AddSeconds(60)
                        $after = $null
                        while ([DateTime]::UtcNow -lt $deadline) {
                            $response = Invoke-LiveVerifyCommand -Connection $updater -Command 'updater.getState'
                            if ($response.ok) {
                                $after = $response.result
                                if ("$($after.failureCase)" -eq 'uacDeclined') { break }
                            }
                            Start-Sleep -Milliseconds 500
                        }
                        Set-Content -LiteralPath (Join-Path $script:LogDirectory 'updater-state.json') `
                            -Value ($after | ConvertTo-Json -Depth 12) -Encoding utf8NoBOM
                        $failureCase = if ($null -ne $after) { "$($after.failureCase)" } else { '' }
                        $installState = if ($null -ne $after) { "$($after.installState)" } else { '' }
                        if ($failureCase -ne 'uacDeclined') {
                            Add-Step -Name 'decline-state' -Ok $false -Detail "failureCase is '$failureCase', expected uacDeclined"
                        }
                        elseif ($installState -eq 'strandedInBackup') {
                            Add-Step -Name 'decline-state' -Ok $false -Detail 'the installation was left stranded in the backup directory'
                        }
                        else {
                            $declineOk = $true
                            Add-Step -Name 'decline-state' -Ok $true -Detail "failureCase uacDeclined, installState $installState"
                        }
                        # THE DEFECT THIS STEP EXISTS FOR. A declined update used to
                        # leave the updater running with its failure card open; it
                        # holds exosnap-updater.exe, so the next launch cannot stage
                        # its own updater over the file and the accept gate failed
                        # with "Failed to stage updater file".
                        $closed = Invoke-LiveVerifyCommand -Connection $updater -Command 'updater.close'
                        Add-Step -Name 'decline-updater-closed' -Ok ([bool]$closed.ok) `
                            -Detail $(if ($closed.ok) { 'the updater closed on request' } else { "updater.close refused: $($closed.error.message)" })
                    }
                    finally { try { $updater.Close() } catch { } }
                }
            }
        }
    }
    finally {
        try { $app.Connection.Close() } catch { }
        Remove-Item Env:EXOSNAP_UPDATER_FAULT -ErrorAction SilentlyContinue
        Stop-ExoSnapProcesses
    }
    [void]$declineOk

    # The updater has to be gone before the accept run, not merely asked to go:
    # the accept path stages a fresh exosnap-updater.exe over the same file.
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    while ([DateTime]::UtcNow -lt $deadline -and @(Get-Process -Name 'exosnap-updater' -ErrorAction SilentlyContinue).Count -gt 0) {
        Start-Sleep -Milliseconds 500
    }
    $stillOpen = @(Get-Process -Name 'exosnap-updater' -ErrorAction SilentlyContinue).Count
    Add-Step -Name 'updater-gone-before-accept' -Ok ($stillOpen -eq 0) `
        -Detail $(if ($stillOpen -eq 0) { 'no updater process is holding exosnap-updater.exe' } else { "$stillOpen updater process(es) still running" })

    # ---------------------------------------------------------------------- accept
    $app = Start-ControlledApp -ExePath $installedExe
    try {
        $checked = Invoke-LiveVerifyCommand -Connection $app.Connection -Command 'update.check'
        if (-not $checked.ok) {
            Add-Step -Name 'accept-offer' -Ok $false -Detail "update.check refused: $($checked.error.message)"
            Write-Result
            return
        }
        $state = Wait-UpdateOffer -Connection $app.Connection -TimeoutSeconds $OfferTimeoutSeconds
        $offered = $null -ne $state -and $null -ne $state.PSObject.Properties['updateAvailable'] -and $state.updateAvailable
        if (-not $offered) {
            Add-Step -Name 'accept-offer' -Ok $false -Detail "no update is offered to $beforeVersion after the decline"
            Write-Result
            return
        }
        Add-Step -Name 'accept-offer' -Ok $true -Detail 'an update is offered again after the declined one'
        $applied = Invoke-LiveVerifyCommand -Connection $app.Connection -Command 'update.apply'
        if (-not $applied.ok) {
            Add-Step -Name 'accept-apply' -Ok $false -Detail "update.apply refused: $($applied.error.message)"
            Write-Result
            return
        }
        Add-Step -Name 'accept-apply' -Ok $true -Detail 'the updater was launched without a fault injected'
    }
    finally { try { $app.Connection.Close() } catch { } }

    # The install replaces the running application, so the version is read from a
    # FRESH launch of the installed path rather than from the connection that
    # started it -- that process is the one being replaced.
    $deadline = [DateTime]::UtcNow.AddMinutes(6)
    $afterVersion = ''
    while ([DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Seconds 5
        try {
            $installedExe = Get-InstalledExoSnap
            if ($null -eq $installedExe) { continue }
            $probe = Start-ControlledApp -ExePath $installedExe -ConnectTimeoutMs 30000
            try { $afterVersion = "$((Invoke-LiveVerifyCommand -Connection $probe.Connection -Command 'app.identity').result.productVersion)" }
            finally {
                try { $probe.Connection.Close() } catch { }
                Stop-ExoSnapProcesses
            }
            if (-not [string]::IsNullOrWhiteSpace($afterVersion) -and $afterVersion -ne $beforeVersion) { break }
        }
        catch { }
    }
    if ([string]::IsNullOrWhiteSpace($afterVersion)) {
        Add-Step -Name 'accept-installed' -Ok $false -Detail 'the application could not be reached again after the install'
    }
    elseif ($afterVersion -eq $beforeVersion) {
        Add-Step -Name 'accept-installed' -Ok $false -Detail "the version is unchanged at $afterVersion; nothing was installed"
    }
    else {
        Add-Step -Name 'accept-installed' -Ok $true -Detail "installed: $beforeVersion -> $afterVersion"
    }
    Write-Result
}
catch {
    Add-Step -Name 'worker' -Ok $false -Detail "$($_.Exception.Message)"
    Write-Result -Fatal "$($_.Exception.Message)"
}
finally {
    Stop-ExoSnapProcesses
    # Last, and unconditionally: the host waits on this file to know the sandbox
    # finished at all, and a run that threw before writing it is indistinguishable
    # from one that never started.
    Set-Content -LiteralPath $MarkerPath -Value ([DateTime]::UtcNow.ToString('o')) -Encoding utf8NoBOM
}
