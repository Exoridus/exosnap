#requires -Version 7.0
[CmdletBinding()]
param(
    # The exosnap.exe under test. Bound explicitly: an acceptance run that picked
    # "whatever build was on PATH" is evidence about an unknown artifact.
    [Parameter(Mandatory)]
    [string] $AppPath,

    # The feed both processes read. The real GitHub releases API by default,
    # which is a read-only GET -- see the safety note below for why that cannot
    # install anything from a development build.
    [string] $FeedUrl = 'https://api.github.com/repos/Exoridus/exosnap/releases',

    [string] $EvidencePath,

    [int] $TimeoutSeconds = 120
)

<#
.SYNOPSIS
    Follows the CURRENT app-to-updater handoff end to end over the control
    channel, and proves the pinned target version across the process boundary.

.DESCRIPTION
    This is the measurement point taken BEFORE the handoff is rewritten. It
    drives the shipping path -- the update card's own check and its own primary
    action -- and then attaches to the updater process that action started:

        app: update.check   -> updateAvailable X
        app: update.apply   -> updater process starts
        event update.updaterLaunched
        updater endpoint    -> targetVersion X          <-- the cross-process proof
        updater             -> terminal state + exit code

    No Start-Sleep anywhere. Every wait is either a blocking connect (the pipe
    does not exist until the child's server has started, so connecting IS the
    readiness observation) or a stateRevision advance delivered as an event.

    SAFETY. Nothing here can install anything, and that is a property of the
    build rather than of this script's timing: a development build pins an
    all-zero update public key, so the manifest signature check fails before a
    single package byte is fetched. The run therefore ends in a truthful
    verifyDownloadFailed with the installation untouched -- which is exactly the
    cross-process failure evidence worth having. Run this against an OFFICIAL
    build only with a feed you control.

.EXAMPLE
    ./live-verify-update-handoff.ps1 -AppPath build/windows-x64-debug/app/quick/ExoSnap/Quick/Debug/exosnap.exe
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

if (-not (Test-Path -LiteralPath $AppPath)) { throw "No such application: $AppPath" }
$appFull = (Resolve-Path -LiteralPath $AppPath).Path
$appSha = (Get-FileHash -LiteralPath $appFull -Algorithm SHA256).Hash.ToLowerInvariant()

$runId = New-LiveVerifyRunId
$scratch = Join-Path ([System.IO.Path]::GetTempPath()) "exosnap-handoff-$runId"
New-Item -ItemType Directory -Path $scratch -Force | Out-Null

$env:EXOSNAP_CONFIG_DIR = Join-Path $scratch 'config'
$env:EXOSNAP_OUTPUT_DIR = Join-Path $scratch 'output'

$app = $null
$appConn = $null
$updaterConn = $null
$exitCode = 1
try {
    $app = Start-Process -FilePath $appFull -PassThru -ArgumentList @(
        '--live-verify-control', $runId,
        '--update-base-url', $FeedUrl
    )
    Add-Step 'application started' $true @{ pid = $app.Id; path = $appFull; sha256 = $appSha }

    # Connecting is the readiness wait: the endpoint is created during startup
    # and a normal launch creates none at all.
    $appConn = Connect-LiveVerify -RunId $runId -ConnectTimeoutMs ($TimeoutSeconds * 1000)
    $identity = $appConn.Identity
    Add-Step 'application endpoint attached (artifact bound)' `
        ($identity.executableSha256 -eq $appSha) `
        @{ reported = $identity.executableSha256; measured = $appSha; version = $identity.productVersion }

    # -- check ---------------------------------------------------------------
    $before = $appConn.StateRevision
    $check = Invoke-LiveVerifyCommand -Connection $appConn -Command 'update.check'
    Add-Step 'update.check accepted without claiming an answer' `
        ($check.ok -and $check.PSObject.Properties.Name -contains 'settled' -and -not $check.settled) `
        $check

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $updateState = $null
    while ([DateTime]::UtcNow -lt $deadline) {
        $remaining = [int]([Math]::Max(1, ($deadline - [DateTime]::UtcNow).TotalMilliseconds))
        $observed = Wait-LiveVerifyRevision -Connection $appConn -After $before -TimeoutMs $remaining
        if ($null -eq $observed) { break }
        $before = $appConn.StateRevision
        $updateState = (Invoke-LiveVerifyCommand -Connection $appConn -Command 'update.getState').result
        if ($updateState.state -ne 'checking') { break }
    }
    if ($null -eq $updateState) { throw 'the update check never produced a state change' }

    $offered = $updateState.availableVersion
    Add-Step 'check resolved to an offered update' `
        ($updateState.state -eq 'available' -and -not [string]::IsNullOrEmpty($offered)) `
        $updateState
    if ($updateState.state -ne 'available') {
        throw "the feed offered nothing to apply (card state '$($updateState.state)')"
    }

    # -- apply ---------------------------------------------------------------
    $apply = Invoke-LiveVerifyCommand -Connection $appConn -Command 'update.apply'
    Add-Step 'update.apply accepted, not completed' `
        ($apply.ok -and -not $apply.settled) $apply
    if (-not $apply.ok) { throw "update.apply refused: $($apply.error.code)" }

    $launch = $apply.result.updaterLaunch
    Add-Step 'the response names the child it started' `
        ($launch.pid -gt 0 -and -not [string]::IsNullOrEmpty($launch.controlPipe)) $launch
    Add-Step 'the child is pinned to the offered version' `
        ($launch.targetVersion -eq $offered) @{ offered = $offered; pinned = $launch.targetVersion }

    # -- attach to the child -------------------------------------------------
    # Same run id, different role. Connect blocks until the child's endpoint
    # exists, which is the readiness observation -- no polling, no sleep.
    $updaterConn = Connect-LiveVerify -RunId $runId -Role 'Updater' -ConnectTimeoutMs ($TimeoutSeconds * 1000)
    $updaterIdentity = $updaterConn.Identity
    Add-Step 'updater endpoint attached' `
        ($updaterIdentity.product -eq 'exosnap-updater') $updaterIdentity

    # THE cross-process assertion: what the app offered is what the updater is
    # allowed to install, observed in two processes rather than inferred.
    Add-Step 'app offeredVersion == updater targetVersion' `
        ($updaterIdentity.targetVersion -eq $offered) `
        @{ appOffered = $offered; updaterTarget = $updaterIdentity.targetVersion }

    $updaterState = (Invoke-LiveVerifyCommand -Connection $updaterConn -Command 'updater.getState').result
    Add-Step 'updater reports the handoff mode and the same target' `
        ($updaterState.mode -eq 'legacyHandoff' -and $updaterState.targetVersion -eq $offered) $updaterState

    # -- follow the child to a terminal state --------------------------------
    $terminal = @('completed', 'failed', 'cancelled', 'rebootRequired', 'restartPending', 'upToDate')
    $rev = $updaterConn.StateRevision
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ($updaterState.phase -notin $terminal -and [DateTime]::UtcNow -lt $deadline) {
        $remaining = [int]([Math]::Max(1, ($deadline - [DateTime]::UtcNow).TotalMilliseconds))
        $observed = Wait-LiveVerifyRevision -Connection $updaterConn -After $rev -TimeoutMs $remaining `
            -EventName 'updater.stateChanged' -StateCommand 'updater.getState'
        if ($null -eq $observed) { break }
        $rev = $updaterConn.StateRevision
        $updaterState = $observed
    }
    Add-Step 'updater reached a terminal phase' ($updaterState.phase -in $terminal) $updaterState
    Add-Step 'the existing installation is untouched' `
        ($updaterState.installState -eq 'intact') @{ installState = $updaterState.installState }

    # -- close the child and read its outcome --------------------------------
    $close = Invoke-LiveVerifyCommand -Connection $updaterConn -Command 'updater.close'
    Add-Step 'updater.close accepted' $close.ok $close
    $child = Get-Process -Id $launch.pid -ErrorAction SilentlyContinue
    if ($null -ne $child) {
        $child.WaitForExit(30000) | Out-Null
        Add-Step 'the updater exit code matches its reported outcome' `
            ($child.ExitCode -ne 0 -or $updaterState.phase -eq 'completed') `
            @{ exitCode = $child.ExitCode; phase = $updaterState.phase }
    }

    $exitCode = if ($steps.Where({ -not $_.pass }).Count -eq 0) { 0 } else { 1 }
}
finally {
    if ($null -ne $updaterConn) { $updaterConn.Close() }
    if ($null -ne $appConn) { $appConn.Close() }
    # The application has no product action that ends it, and inventing one for a
    # test would be inventing product surface. Stopping the harness's own child
    # process is cleanup, not driving.
    if ($null -ne $app -and -not $app.HasExited) { Stop-Process -Id $app.Id -Force -ErrorAction SilentlyContinue }

    if (-not [string]::IsNullOrWhiteSpace($EvidencePath)) {
        $evidence = [ordered]@{
            runId       = $runId
            feed        = $FeedUrl
            application = @{ path = $appFull; sha256 = $appSha }
            steps       = $steps
        }
        $evidence | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $EvidencePath -Encoding utf8
        Write-Host "evidence: $EvidencePath"
    }
}

exit $exitCode
