#requires -Version 7.0
[CmdletBinding()]
param(
    # The install-equivalent tree under test. The updater is taken from BESIDE
    # this executable, not from a build tree: the cross-process update contract is
    # only ever asserted against an installed artifact.
    [Parameter(Mandatory)]
    [string] $AppPath,

    [string] $FeedUrl = 'https://api.github.com/repos/Exoridus/exosnap/releases',

    [ValidateSet('Stable', 'Preview')]
    [string] $Channel = 'Preview',

    # Set when the build under test embeds the public key that signs this feed.
    # It changes ONE expectation: a target-version mismatch can then be reached
    # at all. Without a matching key the signature gate fires first -- which is
    # itself the ordering this suite asserts, so the case is not skipped, its
    # expected outcome simply differs.
    [switch] $KeyMatchesFeed,

    [string] $EvidencePath,

    [int] $TimeoutSeconds = 120
)

<#
.SYNOPSIS
    The negative half of the update-handoff contract: every way a handoff can be
    wrong, driven against the REAL updater and REAL signed release bytes.

.DESCRIPTION
    The handoff document is untrusted input. This suite tampers with it -- one
    field at a time -- and asserts that the updater refuses, names the refusal as
    data, and leaves the installation provably intact.

    Nothing here is mocked: the manifest and its detached signature are the
    actual release assets, the updater is the shipped executable from the install
    tree, and the refusals are read off its automation endpoint.

    No case in this suite is a VALID handoff, so no case can install anything.
    That is a property of the inputs, not of timing.

    NOT COVERED HERE, and why: a wrong package hash and a missing package would
    need a manifest that names them AND carries a valid signature, which requires
    the release signing key. Those two gates are covered where they can be
    covered honestly -- package_verifier's hash comparison and the updater's
    lock-and-verify path in the unit suites -- rather than by weakening the
    signature check to make a live case reachable.

.EXAMPLE
    ./live-verify-update-handoff-trust.ps1 -AppPath ./scratch-install/bin/exosnap.exe
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
$appDir = Split-Path -Parent $appFull
$updaterExe = Join-Path $appDir 'exosnap-updater.exe'
if (-not (Test-Path -LiteralPath $updaterExe)) {
    throw "$appDir is not an install-equivalent tree: exosnap-updater.exe is not beside exosnap.exe."
}
$installedVersion = (Get-Item -LiteralPath $appFull).VersionInfo.ProductVersion

$scratch = Join-Path ([System.IO.Path]::GetTempPath()) ("exosnap-handoff-trust-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $scratch -Force | Out-Null

# -- the real release trust anchor -------------------------------------------
$releases = Invoke-RestMethod -Uri "$FeedUrl`?per_page=30" -Headers @{ 'User-Agent' = 'ExoSnap-LiveVerify' }
$wantPrerelease = $Channel -eq 'Preview'
$release = $releases |
    Where-Object { -not $_.draft -and ($wantPrerelease -or -not $_.prerelease) } |
    Where-Object { ($_.assets.name -contains 'update-manifest.json') -and ($_.assets.name -contains 'update-manifest.json.sig') } |
    Select-Object -First 1
if ($null -eq $release) { throw "no release on the $Channel channel carries a signed update manifest" }
$releaseVersion = $release.tag_name -replace '^v', ''

$assetDir = Join-Path $scratch 'release'
New-Item -ItemType Directory -Path $assetDir -Force | Out-Null
$manifestPath = Join-Path $assetDir 'update-manifest.json'
$signaturePath = Join-Path $assetDir 'update-manifest.json.sig'
foreach ($pair in @(@{ n = 'update-manifest.json'; p = $manifestPath }, @{ n = 'update-manifest.json.sig'; p = $signaturePath })) {
    $url = ($release.assets | Where-Object { $_.name -eq $pair.n }).browser_download_url
    Invoke-WebRequest -Uri $url -OutFile $pair.p -Headers @{ 'User-Agent' = 'ExoSnap-LiveVerify' } | Out-Null
}
Add-Step 'fetched the real signed manifest for the newest release on the channel' `
    ((Test-Path -LiteralPath $manifestPath) -and (Test-Path -LiteralPath $signaturePath)) `
    @{ release = $releaseVersion; manifest = $manifestPath }

# The baseline document. Every case below is this, with exactly one thing wrong.
function New-BaselineHandoff {
    return [ordered]@{
        handoffVersion        = 1
        updateTransactionId   = 'u-' + [guid]::NewGuid().ToString('N').Substring(0, 16)
        targetVersion         = $releaseVersion
        currentVersion        = $installedVersion
        manifestPath          = $manifestPath
        manifestSignaturePath = $signaturePath
        installMode           = 'portable'
        installDir            = $appDir
        # A real, running process: the schema refuses 0, and a handoff that
        # cannot name its parent cannot sequence a swap against it. No case here
        # reaches the close step, so nothing is ever asked of it.
        appPid                = $PID
        verifyReinstall       = $false
    }
}

# Run one case: write the document, start the shipped updater on it, read the
# terminal state off its endpoint, and check the exit code.
function Invoke-HandoffCase {
    param(
        [Parameter(Mandatory)] [string] $Name,
        # The document as raw text (so a malformed-JSON case is expressible).
        [Parameter(Mandatory)] [string] $DocumentText,
        [Parameter(Mandatory)] [string[]] $ExpectedFailureCases
    )

    $caseDir = Join-Path $scratch $Name
    New-Item -ItemType Directory -Path $caseDir -Force | Out-Null
    $path = Join-Path $caseDir 'update-handoff.json'
    Set-Content -LiteralPath $path -Value $DocumentText -Encoding utf8

    $runId = New-LiveVerifyRunId
    $process = $null
    $connection = $null
    try {
        $process = Start-Process -FilePath $updaterExe -PassThru -WorkingDirectory $appDir -ArgumentList @(
            '--apply-handoff', $path,
            '--automation-control', $runId
        )
        # Connecting is the readiness observation: the pipe does not exist until
        # the child's server has started.
        $connection = Connect-LiveVerify -RunId $runId -Role 'Updater' -ConnectTimeoutMs ($TimeoutSeconds * 1000)

        $terminal = @('completed', 'failed', 'cancelled', 'rebootRequired', 'restartPending', 'upToDate')
        $state = (Invoke-LiveVerifyCommand -Connection $connection -Command 'updater.getState').result
        $rev = $connection.StateRevision
        $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
        while ($state.phase -notin $terminal -and [DateTime]::UtcNow -lt $deadline) {
            $remaining = [int]([Math]::Max(1, ($deadline - [DateTime]::UtcNow).TotalMilliseconds))
            $observed = Wait-LiveVerifyRevision -Connection $connection -After $rev -TimeoutMs $remaining `
                -EventName 'updater.stateChanged' -StateCommand 'updater.getState'
            if ($null -eq $observed) { break }
            $rev = $connection.StateRevision
            $state = $observed
        }

        Add-Step "$Name -> refused as $($ExpectedFailureCases -join '|')" `
            ($state.phase -eq 'failed' -and $state.failureCase -in $ExpectedFailureCases) $state
        # The assertion that matters most after a refusal: nothing was touched.
        Add-Step "$Name -> the installation is provably intact" `
            ($state.installState -eq 'intact') @{ installState = $state.installState }
        # A refused handoff offers no retry: re-reading the same document would be
        # refused for the same reason.
        Add-Step "$Name -> no retry is offered for an input this process cannot repair" `
            ($null -eq $state.retryEntryStep -or $state.availableActions -notcontains 'updater.retry') `
            @{ retryEntryStep = $state.retryEntryStep; availableActions = $state.availableActions }

        $close = Invoke-LiveVerifyCommand -Connection $connection -Command 'updater.close'
        if (-not $close.ok) { Add-Step "$Name -> updater.close accepted" $false $close }
        return $state
    }
    finally {
        if ($null -ne $connection) { $connection.Close() }
        if ($null -ne $process) {
            $process.WaitForExit(30000) | Out-Null
            if (-not $process.HasExited) {
                Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
                Add-Step "$Name -> the updater exited on its own" $false @{ pid = $process.Id }
            } else {
                # Truthful exit code: a refused handoff is a failure, and a
                # failure must never leave a zero behind.
                Add-Step "$Name -> exit code is non-zero" ($process.ExitCode -ne 0) @{ exitCode = $process.ExitCode }
            }
        }
    }
}

$exitCode = 1
$results = [ordered]@{}
try {
    # -- schema ---------------------------------------------------------------
    $doc = New-BaselineHandoff; $doc.handoffVersion = 2
    $results.unsupportedVersion = Invoke-HandoffCase -Name 'unsupported-handoff-version' `
        -DocumentText ($doc | ConvertTo-Json -Depth 5) -ExpectedFailureCases @('handoffRejected')

    $results.malformedJson = Invoke-HandoffCase -Name 'malformed-json' `
        -DocumentText '{ "handoffVersion": 1, ' -ExpectedFailureCases @('handoffRejected')

    $doc = New-BaselineHandoff; $doc.Remove('targetVersion')
    $results.missingField = Invoke-HandoffCase -Name 'missing-required-field' `
        -DocumentText ($doc | ConvertTo-Json -Depth 5) -ExpectedFailureCases @('handoffRejected')

    $doc = New-BaselineHandoff; $doc.installMode = 'msix'
    $results.unsupportedInstallMode = Invoke-HandoffCase -Name 'unsupported-install-mode' `
        -DocumentText ($doc | ConvertTo-Json -Depth 5) -ExpectedFailureCases @('handoffRejected')

    # -- installation context -------------------------------------------------
    $foreignDir = Join-Path $scratch 'not-an-installation'
    New-Item -ItemType Directory -Path $foreignDir -Force | Out-Null
    $doc = New-BaselineHandoff; $doc.installDir = $foreignDir
    $results.foreignInstallDir = Invoke-HandoffCase -Name 'install-dir-is-not-an-exosnap-tree' `
        -DocumentText ($doc | ConvertTo-Json -Depth 5) -ExpectedFailureCases @('handoffRejected')

    $doc = New-BaselineHandoff; $doc.currentVersion = '9.9.9'
    $results.installDirVersionMismatch = Invoke-HandoffCase -Name 'install-dir-runs-another-version' `
        -DocumentText ($doc | ConvertTo-Json -Depth 5) -ExpectedFailureCases @('handoffRejected')

    $doc = New-BaselineHandoff; $doc.manifestPath = Join-Path $scratch 'absent-manifest.json'
    $results.missingManifestAsset = Invoke-HandoffCase -Name 'handoff-names-a-manifest-that-is-not-there' `
        -DocumentText ($doc | ConvertTo-Json -Depth 5) -ExpectedFailureCases @('handoffRejected')

    # -- the trust chain, re-established in the updater ------------------------
    # The manifest is re-verified in the child even though the parent fetched it:
    # the updater owns the apply trust boundary, so it does not inherit trust.
    $tamperedManifest = Join-Path $scratch 'tampered-manifest.json'
    $bytes = [System.IO.File]::ReadAllBytes($manifestPath)
    $bytes[[int]($bytes.Length / 2)] = $bytes[[int]($bytes.Length / 2)] -bxor 0x01
    [System.IO.File]::WriteAllBytes($tamperedManifest, $bytes)
    $doc = New-BaselineHandoff; $doc.manifestPath = $tamperedManifest
    $results.modifiedManifest = Invoke-HandoffCase -Name 'modified-manifest-bytes' `
        -DocumentText ($doc | ConvertTo-Json -Depth 5) -ExpectedFailureCases @('verifyDownloadFailed')

    $tamperedSignature = Join-Path $scratch 'tampered-manifest.json.sig'
    $sigText = (Get-Content -LiteralPath $signaturePath -Raw).Trim()
    $flipped = if ($sigText[0] -eq '0') { '1' } else { '0' }
    Set-Content -LiteralPath $tamperedSignature -Value ($flipped + $sigText.Substring(1)) -NoNewline -Encoding ascii
    $doc = New-BaselineHandoff; $doc.manifestSignaturePath = $tamperedSignature
    $results.modifiedSignature = Invoke-HandoffCase -Name 'modified-manifest-signature' `
        -DocumentText ($doc | ConvertTo-Json -Depth 5) -ExpectedFailureCases @('verifyDownloadFailed')

    # -- the pinned target ----------------------------------------------------
    # With a matching key the signature passes and the version gate is what
    # refuses. Without one the SIGNATURE refuses first -- which is the ordering
    # this case proves either way: no manifest field is read before the bytes are
    # authenticated.
    $doc = New-BaselineHandoff; $doc.targetVersion = '0.0.1'
    $expected = if ($KeyMatchesFeed) { @('targetVersionMismatch') } else { @('verifyDownloadFailed') }
    $results.targetMismatch = Invoke-HandoffCase -Name 'handoff-target-is-not-the-manifest-version' `
        -DocumentText ($doc | ConvertTo-Json -Depth 5) -ExpectedFailureCases $expected

    $exitCode = if ($steps.Where({ -not $_.pass }).Count -eq 0) { 0 } else { 1 }
}
finally {
    if (-not [string]::IsNullOrWhiteSpace($EvidencePath)) {
        $evidence = [ordered]@{
            appPath          = $appFull
            installedVersion = $installedVersion
            feed             = $FeedUrl
            channel          = $Channel
            releaseVersion   = $releaseVersion
            keyMatchesFeed   = [bool]$KeyMatchesFeed
            cases            = $results
            steps            = $steps
        }
        $evidence | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $EvidencePath -Encoding utf8
        Write-Host "evidence: $EvidencePath"
    }
}

exit $exitCode
