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

function Get-ReleasePackageIdentity {
    <#
    .SYNOPSIS
        One published release package, by the name and the hash the release page
        carries for it.
    .DESCRIPTION
        The bridge between "these bytes behaved correctly" and "these bytes are what
        the RC published". The exe fingerprint identifies what was driven; this
        identifies the downloadable the user gets, which is what the publish gate can
        compare against the release's own `.sha256` sidecars.

        Kept out of the artifact fingerprint on purpose: a package that has been
        deleted from disk after the campaign started must not turn every verified
        result STALE.
    #>
    param(
        [Parameter(Mandatory)] [string] $Path,
        [Parameter(Mandatory)] [ValidateSet('portable', 'installer')] [string] $Kind
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "No release package at '$Path'. Point this at the file downloaded from the RC release."
    }
    $item = Get-Item -LiteralPath $Path
    return [ordered]@{
        kind     = $Kind
        fileName = $item.Name
        path     = $item.FullName
        bytes    = $item.Length
        sha256   = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}

function Get-ReleasePeSectionHash {
    <#
    .SYNOPSIS
        Lowercase SHA-256 of every section of a PE image, by section name.
    .DESCRIPTION
        A whole-file hash cannot compare a release build against its qualified
        candidate: the release identity is compiled in, so the two executables differ
        by construction. Per section, they need not. With the identity held in
        fixed-width fields, everything the linker places in .text, .data, .pdata and
        .reloc is byte-identical between the two builds of one commit, and only the
        sections that hold the identity (.rdata) and the VERSIONINFO resource (.rsrc)
        move. The headers are excluded: they carry the link timestamp.

        Raw section data as laid out in the file, not the virtual image, so the hash
        is of bytes that exist on disk and can be re-read by anyone with the file.
    .OUTPUTS
        [ordered] section name -> sha256, in file order.
    #>
    param([Parameter(Mandatory)] [string] $Path)

    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 0x40 -or $bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) {
        throw "'$Path' is not a PE image (no MZ header)."
    }
    $peOffset = [BitConverter]::ToInt32($bytes, 0x3C)
    if ($peOffset + 24 -gt $bytes.Length -or [BitConverter]::ToUInt32($bytes, $peOffset) -ne 0x00004550) {
        throw "'$Path' is not a PE image (no PE signature)."
    }
    $sectionCount = [BitConverter]::ToUInt16($bytes, $peOffset + 6)
    $optionalHeaderSize = [BitConverter]::ToUInt16($bytes, $peOffset + 20)
    $tableOffset = $peOffset + 24 + $optionalHeaderSize

    $sha = [Security.Cryptography.SHA256]::Create()
    $sections = [ordered]@{}
    try {
        for ($index = 0; $index -lt $sectionCount; $index++) {
            $entry = $tableOffset + $index * 40
            $name = [Text.Encoding]::ASCII.GetString($bytes, $entry, 8).TrimEnd([char]0)
            $rawSize = [BitConverter]::ToUInt32($bytes, $entry + 16)
            $rawPointer = [BitConverter]::ToUInt32($bytes, $entry + 20)
            if ($rawPointer + $rawSize -gt $bytes.Length) {
                throw "'$Path' section '$name' points past the end of the file."
            }
            $digest = $sha.ComputeHash($bytes, [int]$rawPointer, [int]$rawSize)
            $sections[$name] = ([BitConverter]::ToString($digest) -replace '-', '').ToLowerInvariant()
        }
    }
    finally { $sha.Dispose() }
    return $sections
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
    .PARAMETER SourceCommit
        The commit the RC was built from, when the artifact cannot say so itself. A
        published portable ZIP ships no `artifact-manifest.json`, so a campaign against
        downloaded release assets has no provenance to read; a qualification record
        without a source commit cannot promote anything, which makes naming it here
        the difference between a verifiable release and an unverifiable one.
    #>
    param(
        [Parameter(Mandatory)] [string] $Path,
        [string] $ReleaseTag,
        [string] $SourceCommit
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        throw "No artifact at '$Path'. Release gates bind to an explicit binary; there is no default."
    }
    $item = Get-Item -LiteralPath $Path
    $manifestCommit = Get-ReleaseArtifactSourceCommit -ExeItem $item
    if (-not [string]::IsNullOrWhiteSpace($SourceCommit) -and
        -not [string]::IsNullOrWhiteSpace($manifestCommit) -and
        -not [string]::Equals($SourceCommit.Trim(), $manifestCommit, [StringComparison]::OrdinalIgnoreCase)) {
        throw "SourceCommit '$($SourceCommit.Trim())' conflicts with the artifact manifest commit '$manifestCommit'."
    }
    $facts = @{
        kind             = 'release'
        tag              = $ReleaseTag
        exePath          = $item.FullName
        exeSha256        = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        exeBytes         = $item.Length
        productVersion   = $item.VersionInfo.ProductVersion
        fileVersion      = $item.VersionInfo.FileVersion
        qtRuntimeVersion = (Get-ReleaseArtifactQtRuntimeVersion -ExeItem $item)
        sourceCommit     = if (-not [string]::IsNullOrWhiteSpace($SourceCommit)) { $SourceCommit.Trim() }
        else { $manifestCommit }
        builtUtc         = $item.LastWriteTimeUtc.ToString('o')
        # Whether this artifact sits in an installed tree decides which scenarios can
        # run at all: the updater and handoff paths resolve applicationDirPath()-
        # relative files that only exist after `cmake --install`.
        installTree      = (Test-Path -LiteralPath (Join-Path $item.DirectoryName 'exosnap-updater.exe'))
    }
    $facts['fingerprint'] = Get-LiveVerifyFingerprint -Properties $facts
    return $facts
}
