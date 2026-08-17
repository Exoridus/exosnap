#requires -Version 7.0
[CmdletBinding()]
param(
    # The exosnap.exe under test. Bound explicitly: an acceptance run that picked
    # "whatever build was on PATH" is evidence about an unknown artifact.
    [Parameter(Mandatory)]
    [string] $AppPath,

    # The feed the APPLICATION reads. The real GitHub releases API by default,
    # which is a read-only GET. The updater no longer reads any feed at all --
    # that is the point of this cut.
    [string] $FeedUrl = 'https://api.github.com/repos/Exoridus/exosnap/releases',

    # The update channel the isolated config is seeded with. Preview by default,
    # and that is a property of the feed rather than a preference: a development
    # build carries <base>-dev, the Stable channel only ever names non-prerelease
    # releases, and until <base> itself ships there is nothing on Stable that
    # ranks above it. With no offer there is no handoff to follow, so a Stable
    # run would end at "the feed offered nothing to apply" without ever reaching
    # the assertions this script exists for.
    [ValidateSet('Stable', 'Preview')]
    [string] $Channel = 'Preview',

    # Require the operation to reach a COMPLETED apply and a relaunched
    # application. Only meaningful against a build whose embedded update public
    # key matches the feed's signing key -- a development build pins an all-zero
    # key and stops truthfully at the signature gate, which is its own evidence.
    [switch] $RequireApply,

    [string] $EvidencePath,

    [int] $TimeoutSeconds = 120,

    # The apply downloads and swaps a real release package. Generous, and still
    # bounded: a timeout is a FAIL, never a silent retry.
    [int] $ApplyTimeoutSeconds = 900
)

<#
.SYNOPSIS
    Follows the App -> Updater -> App transition end to end over the control
    channel, and proves the operation is ONE correlated, version-pinned
    transaction across all three processes.

.DESCRIPTION
        app: update.check    -> updateAvailable X
        app: update.apply    -> handoff U written, updater process starts
        the handoff document  -> schema 1, target X, transaction U
        updater endpoint      -> mode appHandoff, target X, transaction U
        updater               -> manifest re-verified in the child
        parent exits          -> observed, not assumed
        updater               -> terminal state + truthful exit code
        new ExoSnap           -> identity + version X            (with -RequireApply)

    No Start-Sleep anywhere. Every wait is either a blocking connect (the pipe
    does not exist until the child's server has started, so connecting IS the
    readiness observation), a stateRevision advance delivered as an event, or a
    real process handle wait.

    SAFETY. Against a DEVELOPMENT build nothing can be installed, and that is a
    property of the build rather than of this script's timing: it pins an
    all-zero update public key, so the manifest signature check fails in the
    updater before a single package byte is fetched. The run then ends in a
    truthful verifyDownloadFailed with the installation untouched. Against a
    build whose key matches the feed, this DOES install -- run it only against an
    isolated scratch install tree.

.EXAMPLE
    ./live-verify-update-handoff.ps1 -AppPath .workspace/install-scratch/bin/exosnap.exe
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

function Get-Sha256([string] $Path) {
    if ([string]::IsNullOrEmpty($Path) -or -not (Test-Path -LiteralPath $Path)) { return $null }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function ConvertTo-ComparablePath([string] $Path) {
    if ([string]::IsNullOrEmpty($Path)) { return '' }
    return $Path.Replace('/', '\').TrimEnd('\').ToLowerInvariant()
}

# The image path of a running process, straight from the OS. Get-Process reads it
# via MainModule, which is an open-and-read of the target and comes back null for
# a child this process is not entitled to inspect that way; the CIM view is a
# query against the OS and answers for any process on the machine.
function Get-ProcessImagePath([int] $ProcessId) {
    return (Get-CimInstance -ClassName Win32_Process -Filter "ProcessId = $ProcessId" `
            -ErrorAction SilentlyContinue).ExecutablePath
}

if (-not (Test-Path -LiteralPath $AppPath)) { throw "No such application: $AppPath" }
$appFull = (Resolve-Path -LiteralPath $AppPath).Path
$appSha = Get-Sha256 $appFull
$appDir = Split-Path -Parent $appFull
$appVersion = (Get-Item -LiteralPath $appFull).VersionInfo.ProductVersion

# Artifact-class precondition. LaunchUpdater() stages its runtime from paths
# relative to applicationDirPath(), and only an installed tree is flat that way
# -- in a build tree the updater lives one directory over, so update.apply
# settles as operation_failed and the run proves nothing about the handoff. One
# marker file, deliberately not a second copy of UpdaterStagingFileList(): that
# list stays the product's own source of truth.
$updaterBeside = Join-Path $appDir 'exosnap-updater.exe'
if (-not (Test-Path -LiteralPath $updaterBeside)) {
    throw ("$appDir is not an install-equivalent tree: exosnap-updater.exe is not beside exosnap.exe. " +
        'Run cmake --install into a scratch prefix and point -AppPath at that tree.')
}

$runId = New-LiveVerifyRunId
$scratch = Join-Path ([System.IO.Path]::GetTempPath()) "exosnap-handoff-$runId"
New-Item -ItemType Directory -Path $scratch -Force | Out-Null

$env:EXOSNAP_CONFIG_DIR = Join-Path $scratch 'config'
$env:EXOSNAP_OUTPUT_DIR = Join-Path $scratch 'output'

# Seed the isolated config with the channel to check. This is the settings key
# the Updates section writes, read back by AppSettingsStore on startup -- so the
# run still takes the product's own path; only the starting preference is given
# rather than clicked. Nothing else is seeded: every other setting stays at its
# first-launch default.
New-Item -ItemType Directory -Path $env:EXOSNAP_CONFIG_DIR -Force | Out-Null
Set-Content -LiteralPath (Join-Path $env:EXOSNAP_CONFIG_DIR 'settings.ini') -Encoding utf8 -Value @(
    '[update]'
    "channel=$Channel"
)

$app = $null
$appConn = $null
$updaterConn = $null
$newApp = $null
$evidence = [ordered]@{
    runId       = $runId
    feed        = $FeedUrl
    channel     = $Channel
    requireApply = [bool]$RequireApply
    application = @{ path = $appFull; sha256 = $appSha; productVersion = $appVersion }
}
$exitCode = 1
try {
    # Which build this is decides how it is allowed to reach a feed at all, and
    # the artifact says so itself: a `-dev` ProductVersion is the marker for an
    # UNOFFICIAL build (root CMakeLists.txt), whose update check is gated off
    # entirely. For it, `--update-base-url` is the documented — and only — way to
    # look at any feed, production one included. An official build refuses the
    # flag outright and reads the production feed on its own, so passing it there
    # would turn a valid run into a refused launch.
    $appArgs = @('--live-verify-control', $runId)
    if ($appVersion -like '*-dev') {
        $appArgs += @('--update-base-url', $FeedUrl)
    }
    $app = Start-Process -FilePath $appFull -PassThru -ArgumentList $appArgs
    Add-Step 'application started' $true @{ pid = $app.Id; path = $appFull; sha256 = $appSha }

    # Connecting is the readiness wait: the endpoint is created during startup
    # and a normal launch creates none at all.
    $appConn = Connect-LiveVerify -RunId $runId -ConnectTimeoutMs ($TimeoutSeconds * 1000)
    $identity = $appConn.Identity
    $evidence.parentIdentity = $identity
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
    $evidence.offeredVersion = $offered

    # -- apply ---------------------------------------------------------------
    $apply = Invoke-LiveVerifyCommand -Connection $appConn -Command 'update.apply'
    Add-Step 'update.apply accepted, not completed' `
        ($apply.ok -and -not $apply.settled) $apply
    if (-not $apply.ok) { throw "update.apply refused: $($apply.error.code) - $($apply.error.message)" }

    $launch = $apply.result.updaterLaunch
    $evidence.updaterLaunch = $launch
    Add-Step 'the response names the child it started' `
        ($launch.pid -gt 0 -and -not [string]::IsNullOrEmpty($launch.controlPipe)) $launch
    Add-Step 'the child is pinned to the offered version' `
        ($launch.targetVersion -eq $offered) @{ offered = $offered; pinned = $launch.targetVersion }
    Add-Step 'the launch names an update transaction' `
        (-not [string]::IsNullOrEmpty($launch.updateTransactionId)) `
        @{ updateTransactionId = $launch.updateTransactionId }
    $transactionId = $launch.updateTransactionId

    # The launch snapshot claims a staged executable. Both halves of that claim
    # are checked against the operating system rather than believed: the running
    # child's own image path, and the bytes on disk.
    # Opened WHILE the child is alive, and its native handle materialised right
    # away. Get-Process hands back a Process object that opens its handle lazily;
    # without this touch the object has none by the time the process is gone, and
    # ExitCode then reads as $null -- which passes every "-ne 0" test while
    # measuring nothing. Touching .Handle makes .NET cache it, so the exit code
    # is still readable afterwards.
    $child = Get-Process -Id $launch.pid -ErrorAction SilentlyContinue
    if ($null -ne $child) { $null = $child.Handle }
    $childPath = Get-ProcessImagePath $launch.pid
    Add-Step 'the running child is the staged executable the event named' `
        ((ConvertTo-ComparablePath $childPath) -eq (ConvertTo-ComparablePath $launch.stagedExecutable)) `
        @{ reported = $launch.stagedExecutable; running = $childPath }

    $stagedSha = Get-Sha256 $launch.stagedExecutable
    Add-Step 'the staged executable hashes to what the event reported' `
        ($null -ne $stagedSha -and $stagedSha -eq $launch.stagedExecutableSha256) `
        @{ reported = $launch.stagedExecutableSha256; measured = $stagedSha }

    # -- the handoff document ------------------------------------------------
    # Read off disk, not taken from the response: the document IS the contract,
    # and what the updater will act on is the file, not what the parent said
    # about it.
    Add-Step 'the handoff document exists where the launch said' `
        (-not [string]::IsNullOrEmpty($launch.handoffPath) -and (Test-Path -LiteralPath $launch.handoffPath)) `
        @{ handoffPath = $launch.handoffPath }
    $handoff = Get-Content -LiteralPath $launch.handoffPath -Raw | ConvertFrom-Json
    $evidence.handoff = $handoff
    Add-Step 'the handoff is the schema version this build writes' `
        ($handoff.handoffVersion -eq 1 -and $launch.handoffVersion -eq 1) `
        @{ document = $handoff.handoffVersion; reported = $launch.handoffVersion }
    Add-Step 'the handoff pins the offered version and the same transaction' `
        ($handoff.targetVersion -eq $offered -and $handoff.updateTransactionId -eq $transactionId) `
        @{ target = $handoff.targetVersion; transaction = $handoff.updateTransactionId }
    Add-Step 'the handoff names the parent process and its installation' `
        ($handoff.appPid -eq $app.Id -and (ConvertTo-ComparablePath $handoff.installDir) -eq (ConvertTo-ComparablePath $appDir)) `
        @{ appPid = $handoff.appPid; parent = $app.Id; installDir = $handoff.installDir }
    Add-Step 'the handoff references the release trust anchor rather than carrying it' `
        ((Test-Path -LiteralPath $handoff.manifestPath) -and (Test-Path -LiteralPath $handoff.manifestSignaturePath) -and
         $null -eq ($handoff.PSObject.Properties.Name | Where-Object { $_ -in @('manifest', 'signature', 'packages') })) `
        @{ manifest = $handoff.manifestPath; signature = $handoff.manifestSignaturePath }

    # The manifest the child will verify is the one the parent downloaded, byte
    # for byte. Recorded so the evidence can be compared against the release.
    $evidence.manifestSha256 = Get-Sha256 $handoff.manifestPath

    # -- attach to the child -------------------------------------------------
    # Same run id, different role. Connect blocks until the child's endpoint
    # exists, which is the readiness observation -- no polling, no sleep.
    $updaterConn = Connect-LiveVerify -RunId $runId -Role 'Updater' -ConnectTimeoutMs ($TimeoutSeconds * 1000)
    $updaterIdentity = $updaterConn.Identity
    $evidence.childIdentity = $updaterIdentity
    Add-Step 'updater endpoint attached' `
        ($updaterIdentity.product -eq 'exosnap-updater') $updaterIdentity
    Add-Step 'the attached endpoint is the one the launch advertised' `
        ($updaterConn.PipeName -eq $launch.controlPipe) `
        @{ advertised = $launch.controlPipe; attached = $updaterConn.PipeName }

    # The run id IS the credential: it is part of the endpoint name, so an id the
    # runner did not mint reaches no endpoint at all. A blocking connect against a
    # name that does not exist is the observation -- not a wait for anything.
    $foreignId = New-LiveVerifyRunId
    $rejected = $false
    try {
        $foreign = Connect-LiveVerify -RunId $foreignId -Role 'Updater' -ConnectTimeoutMs 2000
        $foreign.Close()
    } catch {
        $rejected = $true
    }
    Add-Step 'a run id the updater was not given reaches no endpoint' $rejected `
        @{ acceptedRunId = $runId; refusedRunId = $foreignId }

    # THE cross-process assertions: what the app offered is what the updater is
    # allowed to install, and both processes name the same operation. Observed in
    # two processes rather than inferred from timing.
    Add-Step 'app offeredVersion == updater targetVersion' `
        ($updaterIdentity.targetVersion -eq $offered) `
        @{ appOffered = $offered; updaterTarget = $updaterIdentity.targetVersion }
    Add-Step 'app transaction == handoff transaction == updater transaction' `
        ($updaterIdentity.updateTransactionId -eq $transactionId) `
        @{ parent = $transactionId; handoff = $handoff.updateTransactionId; child = $updaterIdentity.updateTransactionId }
    Add-Step 'the updater read the handoff under the schema the app wrote' `
        ($updaterIdentity.handoffVersion -eq 1) @{ childHandoffVersion = $updaterIdentity.handoffVersion }
    # A handoff run resolves no feed at all -- so it reports no channel, rather
    # than the default one it would never read.
    Add-Step 'the handoff run names no channel to resolve' `
        ($null -eq $updaterIdentity.channel) @{ channel = $updaterIdentity.channel }

    $updaterState = (Invoke-LiveVerifyCommand -Connection $updaterConn -Command 'updater.getState').result
    Add-Step 'updater reports the app-handoff mode, the same target and the same transaction' `
        ($updaterState.mode -eq 'appHandoff' -and $updaterState.targetVersion -eq $offered -and
         $updaterState.updateTransactionId -eq $transactionId) $updaterState

    # -- follow the child to a terminal state --------------------------------
    $terminal = @('completed', 'failed', 'cancelled', 'rebootRequired', 'restartPending', 'upToDate')
    $rev = $updaterConn.StateRevision
    $deadline = [DateTime]::UtcNow.AddSeconds($ApplyTimeoutSeconds)
    while ($updaterState.phase -notin $terminal -and [DateTime]::UtcNow -lt $deadline) {
        $remaining = [int]([Math]::Max(1, ($deadline - [DateTime]::UtcNow).TotalMilliseconds))
        $observed = Wait-LiveVerifyRevision -Connection $updaterConn -After $rev -TimeoutMs $remaining `
            -EventName 'updater.stateChanged' -StateCommand 'updater.getState'
        if ($null -eq $observed) { break }
        $rev = $updaterConn.StateRevision
        $updaterState = $observed
    }
    $evidence.updaterTerminalState = $updaterState
    Add-Step 'updater reached a terminal phase' ($updaterState.phase -in $terminal) $updaterState
    Add-Step 'the transaction id survives to the terminal state' `
        ($updaterState.updateTransactionId -eq $transactionId) `
        @{ expected = $transactionId; actual = $updaterState.updateTransactionId }

    $applied = $updaterState.phase -eq 'completed'
    if ($RequireApply -and -not $applied) {
        Add-Step 'the operation completed the apply' $false $updaterState
    }

    if (-not $applied) {
        # Nothing was installed: the state has to say so rather than merely
        # report a failure. This is the assertion that distinguishes a truthful
        # refusal from a broken installation.
        Add-Step 'the existing installation is untouched' `
            ($updaterState.installState -eq 'intact') @{ installState = $updaterState.installState }
    }

    # -- the parent transition -----------------------------------------------
    # Only an apply asks the parent to close. When it does, the exit is OBSERVED
    # on the process handle -- never assumed from the child's phase.
    if ($applied) {
        $parentGone = $app.WaitForExit(60000)
        Add-Step 'the parent process exited for the handoff' $parentGone `
            @{ pid = $app.Id; exited = $parentGone }
        $appConn.Close(); $appConn = $null
    }

    # -- the relaunched application ------------------------------------------
    if ($applied) {
        $installedExe = Join-Path $appDir 'exosnap.exe'
        $newSha = Get-Sha256 $installedExe
        $newVersion = (Get-Item -LiteralPath $installedExe).VersionInfo.ProductVersion
        Add-Step 'the installed executable is a different artifact than the one that started' `
            ($null -ne $newSha -and $newSha -ne $appSha) `
            @{ before = $appSha; after = $newSha }
        Add-Step 'the installed version is the version that was offered' `
            ($newVersion -eq $offered) @{ offered = $offered; installed = $newVersion }

        # The updater only reports completed after its Launch step observed the
        # new instance's single-instance mutex, so the process exists by now:
        # this identifies it, it does not wait for it.
        $candidates = @(Get-CimInstance -ClassName Win32_Process -Filter "Name = 'exosnap.exe'" `
                -ErrorAction SilentlyContinue |
            Where-Object { (ConvertTo-ComparablePath $_.ExecutablePath) -eq (ConvertTo-ComparablePath $installedExe) -and
                           $_.ProcessId -ne $app.Id })
        Add-Step 'a new ExoSnap process is running from the installation' `
            ($candidates.Count -ge 1) `
            @{ found = $candidates.Count; path = $installedExe }
        if ($candidates.Count -ge 1) {
            $newApp = Get-Process -Id $candidates[0].ProcessId -ErrorAction SilentlyContinue
            $evidence.newApplication = @{
                pid     = $candidates[0].ProcessId
                path    = $candidates[0].ExecutablePath
                sha256  = $newSha
                version = $newVersion
            }
            Add-Step 'the new process is a different process than the one that handed off' `
                ($candidates[0].ProcessId -ne $app.Id) `
                @{ old = $app.Id; new = $candidates[0].ProcessId }
        }
    }

    # -- close the child and read its outcome --------------------------------
    if ($null -ne $child -and -not $child.HasExited) {
        $close = Invoke-LiveVerifyCommand -Connection $updaterConn -Command 'updater.close'
        Add-Step 'updater.close accepted' $close.ok $close
    }
    if ($null -ne $child) {
        $exited = $child.WaitForExit(60000)
        $observedExit = $null
        if ($exited) { $observedExit = $child.ExitCode }
        $evidence.updaterExitCode = $observedExit
        $expectedSuccess = $updaterState.phase -in @('completed', 'restartPending')
        # A measured code, not merely "not zero": an unreadable exit code is null,
        # and null passes every -ne 0 test while proving nothing.
        Add-Step 'the updater exit code matches its reported outcome' `
            ($null -ne $observedExit -and
             (($expectedSuccess -and $observedExit -eq 0) -or (-not $expectedSuccess -and $observedExit -ne 0))) `
            @{ exitCode = $observedExit; exited = $exited; phase = $updaterState.phase }
    } else {
        Add-Step 'the updater process could be observed to its exit' $false @{ pid = $launch.pid }
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
    if ($null -ne $newApp -and -not $newApp.HasExited) { Stop-Process -Id $newApp.Id -Force -ErrorAction SilentlyContinue }

    if (-not [string]::IsNullOrWhiteSpace($EvidencePath)) {
        $evidence.steps = $steps
        $evidence | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $EvidencePath -Encoding utf8
        Write-Host "evidence: $EvidencePath"
    }
}

exit $exitCode
