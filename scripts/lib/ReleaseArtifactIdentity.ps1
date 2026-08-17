#Requires -Version 7.0
<#
.SYNOPSIS
    What a release campaign is allowed to claim about the bytes it tested.

.DESCRIPTION
    Its own file for one reason: `release-verify.ps1` is a script with a mandatory
    command parameter, so nothing in it can be dot-sourced by a test. Artifact
    identity is exactly the kind of thing that must be tested -- every assertion in a
    campaign report rests on it -- so it lives here, where the test file can reach it.

    Everything in here is derived from the artifact itself. Nothing consults the
    repository the runner happens to sit in: a campaign says "these bytes behaved this
    way", and the working tree can have moved on since they were built.
#>

function Get-ReleaseArtifactSourceCommit {
    <#
    .SYNOPSIS
        The commit the artifact under test was built from, or $null.
    .DESCRIPTION
        scripts/build-release-artifacts.ps1 writes `artifact-manifest.json` beside the
        staging directory it packages, carrying `sourceCommit` and `version`. That file
        is the only provenance the artifact itself can offer, so it is read from the
        artifact's own directory tree -- searched upwards a bounded number of levels,
        never from the repository root, which would answer with whatever the runner is
        checked out at.

        The version is compared before the commit is believed: a manifest left behind
        by an earlier build describes different bytes, and attributing its commit to
        these would be worse than reporting nothing.
    #>
    param([Parameter(Mandatory)] $ExeItem)

    $directory = $ExeItem.Directory
    for ($level = 0; $level -lt 4 -and $null -ne $directory; $level++) {
        $manifestPath = Join-Path $directory.FullName 'artifact-manifest.json'
        if (Test-Path -LiteralPath $manifestPath) {
            try {
                $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
            }
            catch {
                # A corrupt manifest is unknown provenance, not a runner failure: the
                # campaign's job is to test the binary, not to parse its paperwork.
                return $null
            }
            $manifestVersion = "$($manifest.version)"
            $exeVersion = "$($ExeItem.VersionInfo.ProductVersion)"
            if ($manifestVersion -ne $exeVersion) { return $null }
            if ([string]::IsNullOrWhiteSpace("$($manifest.sourceCommit)")) { return $null }
            return "$($manifest.sourceCommit)"
        }
        $directory = $directory.Parent
    }
    return $null
}

function Get-ReleaseArtifactQtRuntimeVersion {
    <#
    .SYNOPSIS
        The Qt runtime version the artifact SHIPS, or $null.
    .DESCRIPTION
        Read from `Qt6Core.dll` beside the executable -- not from the Qt the machine
        has installed, which is a different fact and not the one under test. A build
        tree and a portable package can disagree, and it is the package that ships.

        $null when there is no Qt6Core.dll to ask, which is a real state: a partially
        staged package is exactly the kind of artifact a release gate must not
        describe as complete.
    #>
    param([Parameter(Mandatory)] $ExeItem)

    $qtCore = Join-Path $ExeItem.DirectoryName 'Qt6Core.dll'
    if (-not (Test-Path -LiteralPath $qtCore)) { return $null }
    return (Get-Item -LiteralPath $qtCore).VersionInfo.FileVersion
}

function Get-ReleaseArtifactFingerprint {
    <#
    .SYNOPSIS
        Binds the campaign to one set of bytes.
    .DESCRIPTION
        No fallback resolution on purpose. A release PASS says "these bytes behaved
        correctly"; a runner that helpfully found some other exosnap.exe would make
        that sentence false without saying so.

        The Qt runtime version is part of the fingerprint, not a note beside it: a
        framework uplift changes what was tested, so a result recorded against one Qt
        runtime has to go STALE against another rather than silently carry over.
    #>
    param([Parameter(Mandatory)] [string] $Path, [string] $ReleaseTag)

    if (-not (Test-Path -LiteralPath $Path)) {
        throw "No artifact at '$Path'. Release gates bind to an explicit binary; there is no default."
    }
    $item = Get-Item -LiteralPath $Path
    $facts = @{
        kind             = 'release'
        tag              = $ReleaseTag
        exePath          = $item.FullName
        exeSha256        = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        exeBytes         = $item.Length
        productVersion   = $item.VersionInfo.ProductVersion
        fileVersion      = $item.VersionInfo.FileVersion
        qtRuntimeVersion = (Get-ReleaseArtifactQtRuntimeVersion -ExeItem $item)
        sourceCommit     = (Get-ReleaseArtifactSourceCommit -ExeItem $item)
        builtUtc         = $item.LastWriteTimeUtc.ToString('o')
        # Whether this artifact sits in an installed tree decides which scenarios can
        # run at all: the updater and handoff paths resolve applicationDirPath()-
        # relative files that only exist after `cmake --install`.
        installTree      = (Test-Path -LiteralPath (Join-Path $item.DirectoryName 'exosnap-updater.exe'))
    }
    $facts['fingerprint'] = Get-LiveVerifyFingerprint -Properties $facts
    return $facts
}
