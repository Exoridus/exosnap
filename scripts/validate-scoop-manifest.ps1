#Requires -Version 7.0
<#
.SYNOPSIS
    Validates packaging/scoop/exosnap.json against the canonical CMake project
    version.

.DESCRIPTION
    Scoop was the one packaging surface with no validator at all, so its four
    version-bearing literals -- `version`, the download URL's tag and file name,
    `extract_dir`, and the version named in `notes` -- could each drift on their own.
    The published bucket entry refreshes itself through `autoupdate`, but this
    in-repo template is what a first-time bucket copy starts from, so a stale value
    here becomes a bucket that installs the wrong release.

    Checked here:

      - version equals the target version
      - architecture.64bit.url is the GitHub Release portable ZIP for that version,
        tag segment and file name both
      - architecture.64bit.hash is 64 lowercase hex characters, or the placeholder a
        bumped-but-unpublished release legitimately carries
      - extract_dir is the directory that ZIP unpacks to
      - no other X.Y.Z anywhere in the file names a different version
      - the autoupdate block still templates on $version rather than hard-coding one,
        which is what makes the published bucket self-updating at all

    The installer hash is deliberately not cross-checked against a published
    release: that needs the network, and this runs on every pull request.

.PARAMETER Version
    Target version (e.g. "0.9.1"). Defaults to the canonical
    project(exosnap VERSION x.y.z) from the root CMakeLists.txt.
#>
param(
    [string] $Version
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$manifestPath = Join-Path $repoRoot 'packaging/scoop/exosnap.json'

if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw "Scoop manifest not found: $manifestPath"
}

if (-not $Version) {
    $cmakeText = Get-Content -LiteralPath (Join-Path $repoRoot 'CMakeLists.txt') -Raw
    if ($cmakeText -notmatch 'project\(\s*exosnap\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') {
        throw 'Could not parse project(exosnap VERSION x.y.z) from root CMakeLists.txt.'
    }
    $Version = $Matches[1]
}

$errors = [System.Collections.Generic.List[string]]::new()
function Add-Error { param([string] $Message) $errors.Add($Message) | Out-Null }

$text = Get-Content -LiteralPath $manifestPath -Raw
try { $manifest = $text | ConvertFrom-Json }
catch { throw "packaging/scoop/exosnap.json is not valid JSON: $($_.Exception.Message)" }

if ("$($manifest.version)" -ne $Version) {
    Add-Error "version '$($manifest.version)' != target version '$Version'"
}

$architecture = $manifest.architecture.'64bit'
$expectedUrl = "https://github.com/Exoridus/exosnap/releases/download/v$Version/ExoSnap-$Version-windows-x64-portable.zip"
if ("$($architecture.url)" -ne $expectedUrl) {
    Add-Error "architecture.64bit.url '$($architecture.url)' != expected '$expectedUrl'"
}

$expectedExtractDir = "ExoSnap-$Version-windows-x64-portable"
if ("$($architecture.extract_dir)" -ne $expectedExtractDir) {
    Add-Error "architecture.64bit.extract_dir '$($architecture.extract_dir)' != expected '$expectedExtractDir'"
}

$hash = "$($architecture.hash)"
if ($hash -notmatch '^[0-9a-f]{64}$') {
    Add-Error "architecture.64bit.hash '$hash' is not 64 lowercase hex characters"
}

# The published bucket entry rewrites url, hash and extract_dir from these templates
# on every new release. A literal version here would freeze the bucket on one
# release without any error ever being raised.
$autoupdate = $manifest.autoupdate.architecture.'64bit'
foreach ($field in @('url', 'extract_dir')) {
    if ("$($autoupdate.$field)" -notmatch '\$version') {
        Add-Error "autoupdate.architecture.64bit.$field does not template on `$version: '$($autoupdate.$field)'"
    }
}
if ("$($autoupdate.hash.url)" -notmatch '\$version') {
    Add-Error "autoupdate.architecture.64bit.hash.url does not template on `$version: '$($autoupdate.hash.url)'"
}

# Everything the dedicated checks above did not reach: the version named in the
# notes, and any literal left behind by a partial bump.
$lines = $text -split "`r?`n"
for ($i = 0; $i -lt $lines.Count; $i++) {
    $line = $lines[$i]
    if ($line -match '\$version') { continue }
    foreach ($match in [regex]::Matches($line, '(?<![\d.])\d+\.\d+\.\d+(?![\d.])')) {
        if ($match.Value -ne $Version) {
            Add-Error "line $($i + 1): stale version reference '$($match.Value)' in: $($line.Trim())"
        }
    }
}

if ($errors.Count -gt 0) {
    foreach ($message in $errors) { Write-Host "  [FAIL] $message" }
    Write-Host "Scoop manifest validation FAILED ($($errors.Count) error(s)) for version $Version." -ForegroundColor Red
    exit 1
}

Write-Host "Scoop manifest validation PASSED for ExoSnap $Version." -ForegroundColor Green
exit 0
