#Requires -Version 7.0
<#
.SYNOPSIS
    Moves every version literal in the tree to a new product version, in one step.
.DESCRIPTION
    `project(exosnap VERSION x.y.z)` in the root CMakeLists.txt declares the product
    version, and roughly sixteen packaging literals repeat it by hand: the Chocolatey
    nuspec and install script, the three WinGet manifests and the directory they live
    in, and the Scoop manifest. `check-packaging-version.ps1` fails a pull request in
    which they disagree; this is the other half, so that agreeing is not a matter of
    finding all of them.

    Every occurrence of the current version in the files listed below is replaced.
    The values that a release produces and a bump cannot know are reset to their
    placeholders rather than left naming the previous release's bytes: the
    Chocolatey `checksum64`, the WinGet `InstallerSha256` and `ProductCode`, and the
    Scoop `hash`. The full validators refuse those placeholders at submission time
    (docs/release-checklist.md section 8), which is the point: a bumped tree cannot be
    submitted by accident.

    Refuses to run on a dirty tree unless -Force, so the bump is the whole diff and
    can be reviewed as one. Finishes by running check-packaging-version.ps1, and its
    verdict is this script's exit code.
.PARAMETER Version
    The new product version, x.y.z. Prerelease suffixes are a release identity, not
    a product version, and are refused.
.PARAMETER RepoRoot
    The tree to bump. Defaults to the repository this script is in; a test passes a
    copy.
.PARAMETER Force
    Bump a tree with uncommitted changes.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $Version,
    [string] $RepoRoot,
    [switch] $Force
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($Version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$') {
    throw "Version '$Version' is not x.y.z. A prerelease suffix is a release identity (EXOSNAP_RELEASE_VERSION), not a product version."
}

if (-not $RepoRoot) { $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path }
$RepoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path

$cmakeLists = Join-Path $RepoRoot 'CMakeLists.txt'
$cmakeText = Get-Content -LiteralPath $cmakeLists -Raw
if ($cmakeText -notmatch 'project\(\s*exosnap\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') {
    throw "Could not parse project(exosnap VERSION x.y.z) from $cmakeLists."
}
$current = $Matches[1]

if ($current -eq $Version) {
    Write-Host "The tree already declares $Version; nothing to bump."
    exit 0
}

if (-not $Force -and (Test-Path -LiteralPath (Join-Path $RepoRoot '.git'))) {
    $dirty = & git -C $RepoRoot status --porcelain --untracked-files=no
    if ($LASTEXITCODE -eq 0 -and $dirty) {
        throw "The tree has uncommitted changes; a bump should be the whole diff. Commit or stash first, or pass -Force."
    }
}

Write-Host "Bumping $current -> $Version in $RepoRoot"

# Version literals: the exact current version, not preceded or followed by another
# version character, so 0.9.0 does not match inside 10.9.0 or 0.9.01. Every file
# below must contain at least one; a file that no longer does has changed shape
# and the list here is stale.
$literal = "(?<![0-9.])$([regex]::Escape($current))(?![0-9.])"
$wingetDirectory = Join-Path $RepoRoot 'packaging/winget/manifests/c/Codexo/ExoSnap'
$versionFiles = @(
    'packaging/chocolatey/exosnap.nuspec',
    'packaging/chocolatey/tools/chocolateyinstall.ps1',
    'packaging/scoop/exosnap.json',
    "packaging/winget/manifests/c/Codexo/ExoSnap/$current/Codexo.ExoSnap.yaml",
    "packaging/winget/manifests/c/Codexo/ExoSnap/$current/Codexo.ExoSnap.installer.yaml",
    "packaging/winget/manifests/c/Codexo/ExoSnap/$current/Codexo.ExoSnap.locale.en-US.yaml"
)

# The placeholders the validators recognise (and refuse at submission time).
$placeholders = @(
    @{ File = 'packaging/chocolatey/tools/chocolateyinstall.ps1'
       Pattern = "(?m)^(\s*checksum64\s*=\s*')[0-9a-f]{64}(')"
       Value = '0' * 64; Label = 'checksum64' },
    @{ File = 'packaging/scoop/exosnap.json'
       Pattern = '("hash":\s*")[0-9a-f]{64}(")'
       Value = '0' * 64; Label = 'hash' },
    @{ File = "packaging/winget/manifests/c/Codexo/ExoSnap/$current/Codexo.ExoSnap.installer.yaml"
       Pattern = "(?m)^(\s*InstallerSha256:\s*')[0-9A-F]{64}(')"
       Value = '0' * 64; Label = 'InstallerSha256' },
    @{ File = "packaging/winget/manifests/c/Codexo/ExoSnap/$current/Codexo.ExoSnap.installer.yaml"
       Pattern = "(?m)^(\s*ProductCode:\s*')\{[0-9A-Fa-f-]{36}\}(')"
       Value = '{00000000-0000-0000-0000-000000000000}'; Label = 'ProductCode' }
)

function Set-FileText {
    param([string] $Path, [string] $Text)
    # Bytes, not Set-Content: the packaging files are LF and must stay so, and
    # Set-Content would add a trailing newline the file may not have.
    [IO.File]::WriteAllBytes($Path, [Text.UTF8Encoding]::new($false).GetBytes($Text))
}

$edits = [System.Collections.Generic.List[string]]::new()

$cmakeUpdated = [regex]::Replace($cmakeText, "(project\(\s*exosnap\s+VERSION\s+)$([regex]::Escape($current))", "`${1}$Version", 1)
Set-FileText -Path $cmakeLists -Text $cmakeUpdated
$edits.Add("CMakeLists.txt: project(exosnap VERSION $Version)")

foreach ($relative in $versionFiles) {
    $path = Join-Path $RepoRoot $relative
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "$relative is not there; the file list in this script is stale." }
    $text = Get-Content -LiteralPath $path -Raw
    $count = [regex]::Matches($text, $literal).Count
    if ($count -eq 0) { throw "$relative names no $current; the file list in this script is stale." }
    $text = [regex]::Replace($text, $literal, $Version)
    foreach ($placeholder in ($placeholders | Where-Object { $_.File -eq $relative })) {
        if ($text -notmatch $placeholder.Pattern) { throw "$relative has no $($placeholder.Label) to reset; the pattern in this script is stale." }
        $text = [regex]::Replace($text, $placeholder.Pattern, "`${1}$($placeholder.Value)`${2}")
        $edits.Add("$relative`: $($placeholder.Label) reset to its placeholder")
    }
    Set-FileText -Path $path -Text $text
    $edits.Add("$relative`: $count literal(s)")
}

# WinGet keeps one directory per version, and the validator insists on exactly one.
# Move-Item rather than git mv: git records the rename from the result, and a tree
# under test need not be a repository.
Move-Item -LiteralPath (Join-Path $wingetDirectory $current) -Destination (Join-Path $wingetDirectory $Version)
$edits.Add("packaging/winget/manifests/c/Codexo/ExoSnap/$current -> $Version")

foreach ($edit in $edits) { Write-Host "  $edit" }
Write-Host ''
Write-Host 'Left for the release that produces the bytes (docs/release-checklist.md section 8):'
Write-Host '  checksum64, InstallerSha256, ProductCode (from the freshly built MSI), hash, ReleaseDate.'
Write-Host ''

& pwsh -NoProfile -NonInteractive -File (Join-Path $RepoRoot 'scripts/check-packaging-version.ps1')
exit $LASTEXITCODE
