<#
.SYNOPSIS
    Validates that the Chocolatey package under packaging/chocolatey/ is
    consistent with the canonical CMake project version (and, when available,
    the published release artifact manifest).

.DESCRIPTION
    Dependency-free regex/line-based and XML checks (no external module) over
    packaging/chocolatey/exosnap.nuspec and
    packaging/chocolatey/tools/chocolateyinstall.ps1:

      - exosnap.nuspec: <version> equals the target version
      - exosnap.nuspec: <iconUrl> references the exact @v<version> jsDelivr tag
      - exosnap.nuspec: <releaseNotes> references the exact GitHub Release tag URL
      - chocolateyinstall.ps1: url64bit is exactly the expected GitHub Release
        MSI asset URL for the target version
      - chocolateyinstall.ps1: checksum64 is 64 lowercase hex characters, and
        checksumType64 is 'sha256'
      - chocolateyinstall.ps1: checksum64 matches the MSI SHA-256 recorded in
        the release artifact manifest (.workspace/release/<version>/artifact-
        manifest.json), when a manifest is available — skipped with a clear
        message otherwise, never silently passed
      - no other stale/leftover version-looking string (X.Y.Z) anywhere else
        in packaging/chocolatey/, e.g. a forgotten version-specific line in
        the nuspec <description> (historical "**X.Y.Z:**" changelog bullets
        and the vcredist140 dependency's own version are intentionally exempt)

    This proves the relationship chain the release-readiness audit
    (.workspace/audit.md, "Chocolatey-Readiness") asked for:

        CMake version = nuspec version = GitHub Release tag = MSI filename
                       = icon tag = release-notes URL
        chocolateyinstall.ps1 checksum64 = MSI SHA-256 in the artifact manifest

    This script performs STATIC consistency checks only. It does not run
    `choco pack`, does not install or uninstall the package, and does not
    touch Program Files / Add-Remove-Programs on this machine. See
    .workspace/audit.md for a recommended (not implemented here) sandboxed
    install/uninstall smoke test as a follow-up.

.PARAMETER Version
    Target version to validate against (e.g. "0.9.0"). Defaults to the
    canonical project(exosnap VERSION x.y.z) parsed from the root
    CMakeLists.txt.

.PARAMETER ManifestPath
    Path to a release artifact manifest (artifact-manifest.json, written by
    scripts/build-release-artifacts.ps1) to check checksum64 against. Defaults
    to .workspace/release/<version>/artifact-manifest.json if present. When no
    manifest is found (default location absent, or none was built locally),
    the checksum64-vs-manifest check is skipped with a clear message rather
    than failing — this script cannot invent a MSI hash that was never built.
#>
param(
    [string]$Version,
    [string]$ManifestPath
)

$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$chocoRoot = Join-Path $repoRoot 'packaging/chocolatey'
$nuspecPath = Join-Path $chocoRoot 'exosnap.nuspec'
$installPath = Join-Path $chocoRoot 'tools/chocolateyinstall.ps1'

# ---------------------------------------------------------------------------
# Target version — default from the canonical CMake project() declaration
# ---------------------------------------------------------------------------
if (-not $Version) {
    $cmakeListsPath = Join-Path $repoRoot 'CMakeLists.txt'
    $cmakeText = Get-Content -LiteralPath $cmakeListsPath -Raw
    if ($cmakeText -notmatch 'project\(\s*exosnap\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') {
        throw "Could not parse project(exosnap VERSION x.y.z) from root CMakeLists.txt."
    }
    $Version = $Matches[1]
}

$script:Errors = [System.Collections.Generic.List[string]]::new()
function Add-Error { param([string]$Message) $script:Errors.Add($Message) | Out-Null }
function Write-Skip { param([string]$Message) Write-Host "  [SKIP] $Message" -ForegroundColor Yellow }

# ---------------------------------------------------------------------------
# Existence
# ---------------------------------------------------------------------------
foreach ($entry in @(
        @{ Path = $nuspecPath; Label = 'exosnap.nuspec' },
        @{ Path = $installPath; Label = 'tools/chocolateyinstall.ps1' }
    )) {
    if (-not (Test-Path -LiteralPath $entry.Path -PathType Leaf)) {
        Add-Error "Missing $($entry.Label): $($entry.Path)"
    }
}
if ($script:Errors.Count -gt 0) {
    foreach ($e in $script:Errors) { Write-Host "  [FAIL] $e" }
    exit 1
}

$nuspecText = Get-Content -LiteralPath $nuspecPath -Raw
$installText = Get-Content -LiteralPath $installPath -Raw
[xml]$nuspecXml = $nuspecText

# ---------------------------------------------------------------------------
# nuspec <version>
# ---------------------------------------------------------------------------
$nuspecVersion = $nuspecXml.package.metadata.version
if ($nuspecVersion -ne $Version) {
    Add-Error "exosnap.nuspec: <version>$nuspecVersion</version> != target version '$Version'"
}

# ---------------------------------------------------------------------------
# nuspec <iconUrl> — exact @v<version> jsDelivr tag
# ---------------------------------------------------------------------------
$nuspecIconUrl = $nuspecXml.package.metadata.iconUrl
$expectedIconUrl = "https://cdn.jsdelivr.net/gh/Exoridus/exosnap@v$Version/app/assets/brand/exosnap-logo.svg"
if ($nuspecIconUrl -ne $expectedIconUrl) {
    Add-Error "exosnap.nuspec: <iconUrl>$nuspecIconUrl</iconUrl> != expected '$expectedIconUrl'"
}

# ---------------------------------------------------------------------------
# nuspec <releaseNotes> — exact GitHub Release tag URL
# ---------------------------------------------------------------------------
$nuspecReleaseNotes = $nuspecXml.package.metadata.releaseNotes
$expectedReleaseNotes = "https://github.com/Exoridus/exosnap/releases/tag/v$Version"
if ($nuspecReleaseNotes -ne $expectedReleaseNotes) {
    Add-Error "exosnap.nuspec: <releaseNotes>$nuspecReleaseNotes</releaseNotes> != expected '$expectedReleaseNotes'"
}

# ---------------------------------------------------------------------------
# chocolateyinstall.ps1 — url64bit
# ---------------------------------------------------------------------------
$url64bitMatch = [Regex]::Match($installText, "(?m)^\s*url64bit\s*=\s*'([^']+)'")
if (-not $url64bitMatch.Success) {
    Add-Error "chocolateyinstall.ps1: could not find 'url64bit = ...'"
    $url64bit = $null
}
else {
    $url64bit = $url64bitMatch.Groups[1].Value
    $expectedUrl64bit = "https://github.com/Exoridus/exosnap/releases/download/v$Version/ExoSnap-$Version-windows-x64.msi"
    if ($url64bit -ne $expectedUrl64bit) {
        Add-Error "chocolateyinstall.ps1: url64bit '$url64bit' != expected '$expectedUrl64bit'"
    }
}

# ---------------------------------------------------------------------------
# chocolateyinstall.ps1 — checksum64 / checksumType64
# ---------------------------------------------------------------------------
$checksum64Match = [Regex]::Match($installText, "(?m)^\s*checksum64\s*=\s*'([^']+)'")
$checksum64 = $null
if (-not $checksum64Match.Success) {
    Add-Error "chocolateyinstall.ps1: could not find 'checksum64 = ...'"
}
else {
    $checksum64 = $checksum64Match.Groups[1].Value
    if ($checksum64 -notmatch '^[0-9a-f]{64}$') {
        Add-Error "chocolateyinstall.ps1: checksum64 '$checksum64' is not 64 lowercase hex characters"
    }
}

$checksumType64Match = [Regex]::Match($installText, "(?m)^\s*checksumType64\s*=\s*'([^']+)'")
if (-not $checksumType64Match.Success) {
    Add-Error "chocolateyinstall.ps1: could not find 'checksumType64 = ...'"
}
elseif ($checksumType64Match.Groups[1].Value -ne 'sha256') {
    Add-Error "chocolateyinstall.ps1: checksumType64 '$($checksumType64Match.Groups[1].Value)' != 'sha256'"
}

# ---------------------------------------------------------------------------
# checksum64 vs. the release artifact manifest's MSI SHA-256 (if available)
# ---------------------------------------------------------------------------
$manifestExplicit = [bool]$ManifestPath
if (-not $ManifestPath) {
    $ManifestPath = Join-Path $repoRoot ".workspace/release/$Version/artifact-manifest.json"
}

if (-not (Test-Path -LiteralPath $ManifestPath -PathType Leaf)) {
    if ($manifestExplicit) {
        Add-Error "Manifest path specified but not found: $ManifestPath"
    }
    else {
        Write-Skip "No release artifact manifest at '$ManifestPath' — checksum64-vs-manifest check skipped."
    }
}
else {
    $manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
    if ($manifest.version -and $manifest.version -ne $Version) {
        Add-Error "Artifact manifest '$ManifestPath': version '$($manifest.version)' != target version '$Version'"
    }
    elseif (-not $manifest.PSObject.Properties['msiSha256'] -or -not $manifest.msiSha256) {
        Write-Skip "Artifact manifest '$ManifestPath' has no msiSha256 (MSI build was skipped) — checksum64-vs-manifest check skipped."
    }
    elseif ($checksum64) {
        $manifestSha = $manifest.msiSha256.ToLowerInvariant()
        if ($checksum64.ToLowerInvariant() -ne $manifestSha) {
            Add-Error "chocolateyinstall.ps1: checksum64 '$checksum64' != artifact manifest msiSha256 '$manifestSha' ($ManifestPath)"
        }
    }
}

# ---------------------------------------------------------------------------
# No stale/leftover version references anywhere else in packaging/chocolatey/
#
# Scans both files line-by-line for X.Y.Z-looking substrings and flags any
# that do not equal the target version, except:
#   - lines already covered by a dedicated check above (<version>, <iconUrl>,
#     <releaseNotes>, url64bit)
#   - historical "**X.Y.Z:**" changelog bullets in the nuspec <description> —
#     these intentionally document a past release and must never be rewritten
#   - the vcredist140 <dependency> version, which is a VC++ redistributable
#     minimum version, not an ExoSnap release version
# ---------------------------------------------------------------------------
$versionPattern = '(?<![\d.])\d+\.\d+\.\d+(?![\d.])'
$changelogBulletPattern = '^\s*\*\s+\*\*\d+\.\d+\.\d+:\*\*'

function Find-StaleVersionReferences {
    param(
        [string]$Text,
        [string]$Label,
        [string]$TargetVersion,
        [string[]]$ExcludeLineSubstrings
    )
    $lines = $Text -split "`r?`n"
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $line = $lines[$i]
        if ($line -match $changelogBulletPattern) { continue }
        if ($line -match 'vcredist140') { continue }
        $excluded = $false
        foreach ($sub in $ExcludeLineSubstrings) {
            if ($line -like "*$sub*") { $excluded = $true; break }
        }
        if ($excluded) { continue }
        foreach ($m in [Regex]::Matches($line, $versionPattern)) {
            if ($m.Value -ne $TargetVersion) {
                Add-Error "$Label`:$($i + 1): stale version reference '$($m.Value)' (target is '$TargetVersion') in: $($line.Trim())"
            }
        }
    }
}

Find-StaleVersionReferences -Text $nuspecText -Label 'exosnap.nuspec' -TargetVersion $Version `
    -ExcludeLineSubstrings @('<version>', '<iconUrl>', '<releaseNotes>')
Find-StaleVersionReferences -Text $installText -Label 'tools/chocolateyinstall.ps1' -TargetVersion $Version `
    -ExcludeLineSubstrings @('url64bit')

# ---------------------------------------------------------------------------
# Result
# ---------------------------------------------------------------------------
if ($script:Errors.Count -gt 0) {
    foreach ($e in $script:Errors) { Write-Host "  [FAIL] $e" }
    Write-Host "Chocolatey package validation FAILED ($($script:Errors.Count) error(s)) for version $Version." -ForegroundColor Red
    exit 1
}

Write-Host "Chocolatey package validation PASSED for ExoSnap $Version (nuspec + chocolateyinstall.ps1 consistent)." -ForegroundColor Green
exit 0
