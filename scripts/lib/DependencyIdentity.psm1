<#
.SYNOPSIS
    Reads the identity of every third-party component that reaches a shipped
    ExoSnap binary.

.DESCRIPTION
    The release manifest already names every file in the package with its
    SHA-256. That answers "which bytes shipped" but not "which upstream sources
    those bytes were built from", and a statically linked library leaves no file
    behind to hash at all.

    This module answers the second question from the files that already pin the
    dependencies, so it adds no second authority: `third_party/CMakeLists.txt`
    for the FetchContent pins, `cmake/VendorFFmpeg.cmake` for the prebuilt
    FFmpeg archive, `.qt-version` for Qt, and the vendored source trees
    themselves for the amalgamations that carry no version string.

    A component that disappears from its pin file is an error, not an omission:
    the expected inventory is declared here, and a missing entry throws.
#>

Set-StrictMode -Version Latest

# The components that reach a shipped binary, with how each one gets there.
# googletest is deliberately absent: it links into test executables only and
# never into the package.
$script:ExpectedFetchContent = @(
    @{ Name = 'spdlog';       Linkage = 'static' }
    @{ Name = 'nlohmann_json'; Linkage = 'header-only' }
    @{ Name = 'tomlplusplus'; Linkage = 'header-only' }
    @{ Name = 'libopus';      Linkage = 'static' }
    @{ Name = 'flac';         Linkage = 'static' }
    @{ Name = 'rnnoise';      Linkage = 'static' }
    @{ Name = 'EBML';         Linkage = 'static' }
    @{ Name = 'libmatroska';  Linkage = 'static' }
    @{ Name = 'presentmon';   Linkage = 'static' }
)

# Amalgamations vendored into the tree. They have no upstream pin to read, so
# their identity is the hash of the sources actually compiled, plus the release
# THIRD_PARTY_NOTICES.md names them against.
$script:ExpectedVendored = @(
    @{ Name = 'miniz';      Path = 'libs/update/third_party/miniz'; Linkage = 'static (vendored amalgamation)' }
    @{ Name = 'monocypher'; Path = 'libs/update/third_party/monocypher'; Linkage = 'static (vendored amalgamation)' }
    @{ Name = 'nvEncodeAPI'; Path = 'third_party/nvidia'; Linkage = 'header only (NVENC entry points, resolved at runtime)' }
    @{ Name = 'fonts';      Path = 'app/assets/fonts'; Linkage = 'compiled into the Qt resource' }
)

function Get-FetchContentPin {
    param(
        [Parameter(Mandatory)] [string] $Text,
        [Parameter(Mandatory)] [string] $Name
    )

    # FetchContent_Declare(<name> ... GIT_REPOSITORY <url> ... GIT_TAG <rev> [# tag <v>])
    # The declaration is matched as one block so a GIT_TAG belonging to the next
    # component can never be read as this one's.
    $pattern = "(?is)FetchContent_Declare\(\s*$([regex]::Escape($Name))\s(.*?)\)"
    $block = [regex]::Match($Text, $pattern)
    if (-not $block.Success) { return $null }

    $body = $block.Groups[1].Value
    $repository = [regex]::Match($body, '(?i)GIT_REPOSITORY\s+(\S+)')
    $revision = [regex]::Match($body, '(?i)GIT_TAG\s+([0-9a-f]{7,40})\s*(?:#\s*(?:==\s*)?(?:tag\s+)?(\S+))?')
    if (-not $repository.Success -or -not $revision.Success) { return $null }

    $version = if ($revision.Groups[2].Success) { $revision.Groups[2].Value } else { $null }
    return [ordered]@{
        source   = $repository.Groups[1].Value
        revision = $revision.Groups[1].Value
        version  = $version
    }
}

function Get-VendoredSourceHash {
    param([Parameter(Mandatory)] [string] $Directory)

    # One hash over the sorted relative paths and their contents: a renamed file
    # has to change the digest, or the digest would not identify the tree.
    $files = Get-ChildItem -LiteralPath $Directory -Recurse -File | Sort-Object FullName
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $buffer = [System.IO.MemoryStream]::new()
        foreach ($file in $files) {
            $relative = $file.FullName.Substring($Directory.Length + 1).Replace('\', '/')
            $nameBytes = [Text.Encoding]::UTF8.GetBytes("$relative`n")
            $buffer.Write($nameBytes, 0, $nameBytes.Length)
            $contentBytes = [IO.File]::ReadAllBytes($file.FullName)
            $buffer.Write($contentBytes, 0, $contentBytes.Length)
        }
        return [BitConverter]::ToString($sha.ComputeHash($buffer.ToArray())).Replace('-', '').ToLowerInvariant()
    }
    finally {
        $sha.Dispose()
    }
}

function Get-DependencyIdentity {
    <#
    .SYNOPSIS
        Returns one ordered entry per third-party component that reaches a
        shipped binary.

    .DESCRIPTION
        Each entry carries `name`, `linkage`, and whichever identity fields the
        component's pin provides: `version`, `source`, `revision`,
        `archiveSha256` for a prebuilt archive, `sourceSha256` for a vendored
        tree.

        Throws when an expected component has no readable pin. A release
        manifest that silently dropped a dependency would be worse than no
        dependency section at all.
    #>
    param([Parameter(Mandatory)] [string] $RepoRoot)

    $thirdPartyPath = Join-Path $RepoRoot 'third_party/CMakeLists.txt'
    $ffmpegPath = Join-Path $RepoRoot 'cmake/VendorFFmpeg.cmake'
    $qtVersionPath = Join-Path $RepoRoot '.qt-version'
    foreach ($required in @($thirdPartyPath, $ffmpegPath, $qtVersionPath)) {
        if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
            throw "Dependency identity: '$required' is missing."
        }
    }

    $entries = [System.Collections.Generic.List[object]]::new()

    $qtVersion = (Get-Content -LiteralPath $qtVersionPath -Raw).Trim()
    if ($qtVersion -notmatch '^\d+\.\d+\.\d+$') {
        throw "Dependency identity: .qt-version contains '$qtVersion', which is not a three-part version."
    }
    $entries.Add([ordered]@{
            name    = 'Qt'
            linkage = 'dynamic'
            version = $qtVersion
            source  = 'https://download.qt.io/official_releases/qt/'
        })

    $ffmpegText = Get-Content -LiteralPath $ffmpegPath -Raw

    # The pin is expressed through CMake variables, so the URL literal in the
    # file still contains `${...}` references. Reading the literal would record a
    # placeholder as the shipped identity, which is worse than recording nothing:
    # it looks like an answer. Resolve the two variables the URL is built from,
    # then substitute them.
    $ffmpegTag = [regex]::Match($ffmpegText, '(?i)set\(\s*EXOSNAP_FFMPEG_PACKAGE_TAG\s+"([^"$]+)"')
    $ffmpegUpstream = [regex]::Match($ffmpegText, '(?i)set\(\s*EXOSNAP_FFMPEG_UPSTREAM_TAG\s+"([^"$]+)"')
    $ffmpegUrl = [regex]::Match($ffmpegText, '(?im)^\s*URL\s+"([^"]+)"')
    $ffmpegHash = [regex]::Match($ffmpegText, '(?i)URL_HASH\s+"?SHA256=([0-9a-fA-F]{64})"?')
    if (-not ($ffmpegTag.Success -and $ffmpegUpstream.Success -and $ffmpegUrl.Success -and $ffmpegHash.Success)) {
        throw 'Dependency identity: cannot read the FFmpeg package tag, upstream tag, URL and SHA-256 from cmake/VendorFFmpeg.cmake.'
    }

    $resolvedUrl = $ffmpegUrl.Groups[1].Value.
        Replace('${EXOSNAP_FFMPEG_PACKAGE_TAG}', $ffmpegTag.Groups[1].Value).
        Replace('${EXOSNAP_FFMPEG_UPSTREAM_TAG}', $ffmpegUpstream.Groups[1].Value)
    if ($resolvedUrl -match '\$\{') {
        throw "Dependency identity: the FFmpeg URL still contains an unresolved variable after substitution: $resolvedUrl"
    }

    $ffmpegMajors = [ordered]@{}
    foreach ($lib in @('AVFORMAT', 'AVCODEC', 'AVUTIL', 'SWRESAMPLE')) {
        $major = [regex]::Match($ffmpegText, "(?i)set\(\s*EXOSNAP_FFMPEG_${lib}_MAJOR\s+(\d+)\s*\)")
        if (-not $major.Success) {
            throw "Dependency identity: cmake/VendorFFmpeg.cmake declares no pinned major for $lib."
        }
        $ffmpegMajors[$lib.ToLowerInvariant()] = [int]$major.Groups[1].Value
    }

    $entries.Add([ordered]@{
            name             = 'FFmpeg'
            linkage          = 'dynamic'
            version          = $ffmpegTag.Groups[1].Value
            upstreamVersion  = $ffmpegUpstream.Groups[1].Value
            source           = $resolvedUrl
            archiveSha256    = $ffmpegHash.Groups[1].Value.ToLowerInvariant()
            libraryMajors    = $ffmpegMajors
        })

    $thirdPartyText = Get-Content -LiteralPath $thirdPartyPath -Raw
    foreach ($expected in $script:ExpectedFetchContent) {
        $pin = Get-FetchContentPin -Text $thirdPartyText -Name $expected.Name
        if (-not $pin) {
            throw "Dependency identity: no readable FetchContent pin for '$($expected.Name)' in third_party/CMakeLists.txt."
        }
        $entry = [ordered]@{
            name    = $expected.Name
            linkage = $expected.Linkage
        }
        if ($pin.version) { $entry['version'] = $pin.version }
        $entry['source'] = $pin.source
        $entry['revision'] = $pin.revision
        $entries.Add($entry)
    }

    foreach ($expected in $script:ExpectedVendored) {
        $directory = Join-Path $RepoRoot $expected.Path
        if (-not (Test-Path -LiteralPath $directory -PathType Container)) {
            throw "Dependency identity: vendored source tree '$($expected.Path)' is missing."
        }
        $entries.Add([ordered]@{
                name         = $expected.Name
                linkage      = $expected.Linkage
                source       = $expected.Path
                sourceSha256 = Get-VendoredSourceHash -Directory (Resolve-Path -LiteralPath $directory).Path
            })
    }

    return $entries.ToArray()
}

Export-ModuleMember -Function Get-DependencyIdentity, Get-FetchContentPin, Get-VendoredSourceHash
