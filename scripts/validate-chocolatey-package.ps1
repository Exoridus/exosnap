<#
.SYNOPSIS
    Validates that the Chocolatey package under packaging/chocolatey/ is
    consistent with the canonical CMake project version (and, when available,
    the published release artifact manifest).

.DESCRIPTION
    Dependency-free regex/line-based and XML checks (no external module) over
    packaging/chocolatey/exosnap.nuspec,
    packaging/chocolatey/tools/chocolateyinstall.ps1 and
    packaging/chocolatey/tools/chocolateyuninstall.ps1:

      - exosnap.nuspec: <version> equals the target version
      - exosnap.nuspec: <iconUrl> references the exact @v<version> jsDelivr tag
      - exosnap.nuspec: <releaseNotes> references the exact GitHub Release tag URL
      - chocolateyinstall.ps1: url64bit is exactly the expected GitHub Release
        MSI asset URL for the target version
      - chocolateyinstall.ps1: checksum64 is 64 lowercase hex characters, and
        checksumType64 is 'sha256'
      - chocolateyinstall.ps1: checksum64 is not a degenerate placeholder (a
        single repeated character, e.g. 64 zeros). Between a version bump and
        the published release the MSI does not exist yet and no real hash can
        be written, so the placeholder is the honest value to carry in the
        tree, but it must never reach `choco pack` or a submission. This check
        is unconditional; -RequireManifest does not relax it.
      - chocolateyinstall.ps1: checksum64 matches the MSI SHA-256 recorded in
        the release artifact manifest (.workspace/release/<version>/artifact-
        manifest.json), when a manifest is available — skipped with a clear
        message otherwise, never silently passed
      - no other stale/leftover version-looking string (X.Y.Z) anywhere else
        in packaging/chocolatey/, e.g. a forgotten version-specific line in
        the nuspec <description> (historical "**X.Y.Z:**" changelog bullets
        and the vcredist140 dependency's own version are intentionally exempt)

    It additionally enforces the mechanically checkable subset of the
    Chocolatey community moderation requirements and guidelines, so a
    submission is not rejected for something a regex could have caught:

      - nuspec: every field the community feed requires or strongly expects is
        present and non-empty, all its URLs are HTTPS, the description is
        between 30 and 4000 characters with no Markdown heading missing its
        space after the '#', the copyright is more than three characters, no
        e-mail address or leftover template placeholder is present, tags are
        lowercase and space-separated (no commas) and do not include
        "chocolatey", the id is lowercase, and every <dependency> pins a
        version
      - nuspec: <iconUrl> is served from a CDN (a raw github.com or
        raw.githubusercontent.com link is rejected outright) and ends in a
        permitted image extension
      - tools/: the automation scripts are named exactly chocolateyinstall.ps1
        and chocolateyuninstall.ps1, each opens with
        $ErrorActionPreference = 'Stop', and none of them uses Write-Host, a
        `choco` command, an Import-Module of a Chocolatey module, a private
        Chocolatey environment variable, a deprecated helper, or a raw msiexec
        call
      - tools/: an uninstall script exists at all. Without one the package
        verifier's uninstall pass leaves the software installed, which is a
        moderation requirement, not a nicety
      - the package ships no binaries and no source-control or OS index files,
        which is what makes LICENSE.txt / VERIFICATION.txt unnecessary here

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

.PARAMETER RequireManifest
    Turn the "no manifest found" skip into a hard failure. The default (skip)
    suits local/dev use where no release build has happened yet; a real
    Chocolatey submission must never proceed on a checksum64 that was never
    cross-checked against a built MSI's actual hash. Pass this for the release
    invocation, per docs/release-checklist.md §8:

        scripts/validate-chocolatey-package.ps1 -Version 0.9.0 `
            -ManifestPath .workspace/release/0.9.0/artifact-manifest.json -RequireManifest
#>
param(
    [string]$Version,
    [string]$ManifestPath,
    [switch]$RequireManifest,
    # Checks only what a version bump has to get right, and stops before everything
    # that cannot be true until the release is published: the real MSI checksum and
    # the moderation subset that reads it. This is the form CI runs on every pull
    # request, so a partial bump fails within seconds of being pushed rather than at
    # submission time.
    [switch]$VersionOnly
)

$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$chocoRoot = Join-Path $repoRoot 'packaging/chocolatey'
$nuspecPath = Join-Path $chocoRoot 'exosnap.nuspec'
$toolsRoot = Join-Path $chocoRoot 'tools'
$installPath = Join-Path $toolsRoot 'chocolateyinstall.ps1'
$uninstallPath = Join-Path $toolsRoot 'chocolateyuninstall.ps1'

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
        @{ Path = $installPath; Label = 'tools/chocolateyinstall.ps1' },
        # Without an uninstall script the package verifier's uninstall pass
        # leaves the MSI installed, which moderation treats as a requirement
        # failure rather than a missing nicety.
        @{ Path = $uninstallPath; Label = 'tools/chocolateyuninstall.ps1' }
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
$uninstallText = Get-Content -LiteralPath $uninstallPath -Raw
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
if ($VersionOnly) {
    # Between a version bump and the published release there is no MSI to hash, so
    # the tree legitimately carries the placeholder. Saying nothing about it here is
    # what lets the version axis be checked on every commit.
    Write-Skip 'checksum64 not examined (-VersionOnly).'
}
elseif (-not $checksum64Match.Success) {
    Add-Error "chocolateyinstall.ps1: could not find 'checksum64 = ...'"
}
else {
    $checksum64 = $checksum64Match.Groups[1].Value
    if ($checksum64 -notmatch '^[0-9a-f]{64}$') {
        Add-Error "chocolateyinstall.ps1: checksum64 '$checksum64' is not 64 lowercase hex characters"
    }
    elseif ($checksum64 -match '^(.)\1+$') {
        # A single repeated character is well-formed enough to satisfy the hex
        # check and the Chocolatey "a checksum is present" rules, so nothing but
        # an explicit test stops the placeholder from being packed and pushed.
        # Unconditional on purpose: -RequireManifest governs whether a real hash
        # is cross-checked, never whether a fake one is tolerated.
        Add-Error ("chocolateyinstall.ps1: checksum64 is the placeholder for an unpublished release " +
            "('$($checksum64.Substring(0, 1))' x 64), not a real hash. The package cannot be packed or " +
            "submitted until the v$Version release exists. Take the lowercase value from " +
            "ExoSnap-$Version-windows-x64.msi.sha256 (the sidecar published next to the MSI on the " +
            "GitHub Release), or from the artifact manifest's msiSha256 for a local release build.")
    }
}

$checksumType64Match = [Regex]::Match($installText, "(?m)^\s*checksumType64\s*=\s*'([^']+)'")
if ($VersionOnly) {
    # Nothing to say: the type matters only alongside a real checksum.
}
elseif (-not $checksumType64Match.Success) {
    Add-Error "chocolateyinstall.ps1: could not find 'checksumType64 = ...'"
}
elseif ($checksumType64Match.Groups[1].Value -ne 'sha256') {
    Add-Error "chocolateyinstall.ps1: checksumType64 '$($checksumType64Match.Groups[1].Value)' != 'sha256'"
}

# ---------------------------------------------------------------------------
# checksum64 vs. the release artifact manifest's MSI SHA-256 (if available)
# ---------------------------------------------------------------------------
$manifestExplicit = [bool]$ManifestPath
if ($VersionOnly) {
    $ManifestPath = $null
}
elseif (-not $ManifestPath) {
    $ManifestPath = Join-Path $repoRoot ".workspace/release/$Version/artifact-manifest.json"
}

if ($VersionOnly) {
    Write-Skip 'checksum64-vs-manifest check skipped (-VersionOnly).'
}
elseif (-not (Test-Path -LiteralPath $ManifestPath -PathType Leaf)) {
    if ($manifestExplicit) {
        Add-Error "Manifest path specified but not found: $ManifestPath"
    }
    elseif ($RequireManifest) {
        Add-Error "No release artifact manifest at '$ManifestPath' and -RequireManifest was set — refusing to validate checksum64 without a real built-MSI hash to check it against."
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
        if ($RequireManifest) {
            Add-Error "Artifact manifest '$ManifestPath' has no msiSha256 (MSI build was skipped) and -RequireManifest was set."
        }
        else {
            Write-Skip "Artifact manifest '$ManifestPath' has no msiSha256 (MSI build was skipped) — checksum64-vs-manifest check skipped."
        }
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
Find-StaleVersionReferences -Text $uninstallText -Label 'tools/chocolateyuninstall.ps1' -TargetVersion $Version `
    -ExcludeLineSubstrings @()

# The moderation subset below describes a submission, and a submission cannot happen
# before the release exists. -VersionOnly stops here so the version axis can be a
# pull-request check without dragging the rest of the submission bar into it.
if ($VersionOnly) {
    if ($script:Errors.Count -gt 0) {
        foreach ($e in $script:Errors) { Write-Host "  [FAIL] $e" }
        Write-Host "Chocolatey version validation FAILED ($($script:Errors.Count) error(s)) for version $Version." -ForegroundColor Red
        exit 1
    }
    Write-Host "Chocolatey version validation PASSED for ExoSnap $Version (nuspec + chocolateyinstall.ps1 name one version)." -ForegroundColor Green
    exit 0
}

# ---------------------------------------------------------------------------
# Chocolatey community moderation rules, mechanical subset
#
# The community feed rejects a submission for any of these, and every one of
# them is decidable from the package files alone. Catching them here costs a
# second; catching them in moderation costs a review round trip.
# ---------------------------------------------------------------------------
$meta = $nuspecXml.package.metadata

function Get-MetaValue {
    param([string]$Name)
    $node = $meta.SelectSingleNode("*[local-name()='$Name']")
    if ($null -eq $node) { return $null }
    return $node.InnerText
}

foreach ($field in @(
        'id', 'version', 'title', 'authors', 'owners', 'summary', 'description',
        'copyright', 'projectUrl', 'licenseUrl', 'iconUrl', 'releaseNotes', 'tags',
        # Not strictly required, but the feed's "nuspec enhancements" rule asks
        # for these and a maintainer only ever forgets them once.
        'docsUrl', 'bugTrackerUrl', 'packageSourceUrl', 'projectSourceUrl'
    )) {
    $value = Get-MetaValue $field
    if ([string]::IsNullOrWhiteSpace($value)) {
        Add-Error "exosnap.nuspec: <$field> is missing or empty (required or strongly expected by Chocolatey moderation)"
    }
}

$nuspecId = Get-MetaValue 'id'
if ($nuspecId -and $nuspecId -cne $nuspecId.ToLowerInvariant()) {
    Add-Error "exosnap.nuspec: <id>$nuspecId</id> must be all lowercase"
}

$nuspecTitle = Get-MetaValue 'title'
if ($nuspecTitle -and $nuspecId -and $nuspecTitle -ceq $nuspecId) {
    Add-Error "exosnap.nuspec: <title> is character-identical to <id> ('$nuspecId'); the id is lowercase, the title is title-cased"
}

$nuspecCopyright = Get-MetaValue 'copyright'
if ($nuspecCopyright -and $nuspecCopyright.Trim().Length -lt 4) {
    Add-Error "exosnap.nuspec: <copyright> '$nuspecCopyright' is shorter than 4 characters"
}

if ((Get-MetaValue 'requireLicenseAcceptance') -eq 'true' -and -not (Get-MetaValue 'licenseUrl')) {
    Add-Error "exosnap.nuspec: <requireLicenseAcceptance>true</requireLicenseAcceptance> without a <licenseUrl>"
}

# Every URL the feed publishes has to be reachable over HTTPS; a plain-http
# metadata link is a flat rejection.
foreach ($urlField in @('projectUrl', 'licenseUrl', 'iconUrl', 'releaseNotes', 'docsUrl',
        'bugTrackerUrl', 'packageSourceUrl', 'projectSourceUrl', 'mailingListUrl')) {
    $value = Get-MetaValue $urlField
    if ($value -and $value -notmatch '^https://') {
        Add-Error "exosnap.nuspec: <$urlField> '$value' is not an https:// URL"
    }
}

$nuspecIcon = Get-MetaValue 'iconUrl'
if ($nuspecIcon) {
    if ($nuspecIcon -match 'raw\.githubusercontent\.com|github\.com/.*/raw/') {
        Add-Error "exosnap.nuspec: <iconUrl> must go through a CDN (jsDelivr, Statically, Githack); raw GitHub links are rejected"
    }
    if ($nuspecIcon -notmatch '\.(png|ico|gif|jpg|jpeg|bmp|webp|svg)$') {
        Add-Error "exosnap.nuspec: <iconUrl> '$nuspecIcon' does not end in a permitted image extension"
    }
}

$nuspecTags = Get-MetaValue 'tags'
if ($nuspecTags) {
    if ($nuspecTags -match ',') {
        Add-Error "exosnap.nuspec: <tags> must be space-separated, not comma-separated"
    }
    if ($nuspecTags -cne $nuspecTags.ToLowerInvariant()) {
        Add-Error "exosnap.nuspec: <tags> must be lowercase: '$nuspecTags'"
    }
    if (($nuspecTags -split '\s+') -contains 'chocolatey') {
        Add-Error "exosnap.nuspec: <tags> must not contain 'chocolatey'"
    }
}

$nuspecDescription = Get-MetaValue 'description'
if ($nuspecDescription) {
    $descLength = $nuspecDescription.Trim().Length
    if ($descLength -lt 30) {
        Add-Error "exosnap.nuspec: <description> is $descLength characters, the minimum is 30"
    }
    if ($descLength -gt 4000) {
        Add-Error "exosnap.nuspec: <description> is $descLength characters, the maximum is 4000"
    }
    # The feed renders the description with Markdig, which needs the space after
    # the '#'; without it the heading ships as literal '###Text'.
    $descLines = $nuspecDescription -split "`r?`n"
    for ($i = 0; $i -lt $descLines.Count; $i++) {
        if ($descLines[$i] -match '^\s*#{1,6}[^#\s]') {
            Add-Error "exosnap.nuspec: <description> line $($i + 1) is a Markdown heading with no space after the '#': $($descLines[$i].Trim())"
        }
    }
}

foreach ($dep in $meta.SelectNodes("*[local-name()='dependencies']/*[local-name()='dependency']")) {
    if ([string]::IsNullOrWhiteSpace($dep.GetAttribute('version'))) {
        Add-Error "exosnap.nuspec: <dependency id=`"$($dep.GetAttribute('id'))`"> has no version range"
    }
}

if ($nuspecText -match '[\w.%+-]+@[\w.-]+\.[A-Za-z]{2,}') {
    Add-Error "exosnap.nuspec: contains an e-mail address ('$($Matches[0])'); the feed does not accept one"
}

foreach ($placeholder in @('__REPLACE', 'PACKAGE_NAME', 'space separated', 'Software Name',
        'REPLACE_ME', 'YOUR_', 'The software LICENSE ACCEPTANCE')) {
    if ($nuspecText -like "*$placeholder*") {
        Add-Error "exosnap.nuspec: leftover template placeholder '$placeholder'"
    }
}

# ---------------------------------------------------------------------------
# Automation scripts
# ---------------------------------------------------------------------------
$scriptChecks = @(
    @{ Pattern = 'Write-Host'; Message = "uses Write-Host; Chocolatey's own output helpers or Write-Warning are expected" },
    @{ Pattern = '(?m)^\s*choco(latey)?(\.exe)?\s+(install|upgrade|uninstall|push|pack)\b'; Message = 'calls a choco command from an automation script' },
    @{ Pattern = 'Import-Module\s+.*chocolatey'; Message = 'imports a Chocolatey module; the helpers are already in scope' },
    @{ Pattern = '\$env:(chocolateyPackageFolder|packageFolder|chocolateyToolsLocation|chocolateyBinRoot|chocolatey_bin_root|chocolateyChecksum(32|64)|chocolateyChecksumType(32|64)|downloadCacheAvailable)\b'; Message = 'reads a private Chocolatey environment variable' },
    @{ Pattern = '\$(nugetChocolateyPath|nugetPath|nugetExePath|nugetLibPath|chocInstallVariableName|nugetExe)\b'; Message = 'uses an internal Chocolatey variable' },
    @{ Pattern = 'Get-BinRoot'; Message = 'uses the deprecated Get-BinRoot; use Get-ToolsLocation' },
    @{ Pattern = 'Get-WmiObject'; Message = 'uses Get-WmiObject to find installed software; use Get-UninstallRegistryKey' },
    @{ Pattern = '(?m)^\s*[^#]*\bmsiexec\b'; Message = 'shells out to msiexec; Install-/Uninstall-ChocolateyPackage does that correctly' },
    @{ Pattern = 'http://'; Message = 'contains a plain-http URL' }
)

foreach ($script in @(
        @{ Label = 'tools/chocolateyinstall.ps1'; Text = $installText },
        @{ Label = 'tools/chocolateyuninstall.ps1'; Text = $uninstallText }
    )) {
    if ($script.Text -notmatch "(?m)^\s*\`$ErrorActionPreference\s*=\s*'Stop'") {
        Add-Error "$($script.Label): must start with `$ErrorActionPreference = 'Stop'"
    }
    foreach ($check in $scriptChecks) {
        if ($script.Text -match $check.Pattern) {
            Add-Error "$($script.Label): $($check.Message)"
        }
    }
}

# ---------------------------------------------------------------------------
# Package contents
#
# LICENSE.txt and VERIFICATION.txt are only required when a package ships
# binaries. This package ships none, so the check that keeps that true is the
# one worth having: anything in tools/ that is not a .ps1 changes the rules.
# ---------------------------------------------------------------------------
foreach ($file in Get-ChildItem -LiteralPath $toolsRoot -Recurse -Force -File) {
    $relative = $file.FullName.Substring($chocoRoot.Length + 1) -replace '\\', '/'
    if ($file.Extension -ne '.ps1') {
        Add-Error ("$relative is packaged but is not a PowerShell script. A package that ships binaries " +
            "additionally needs tools/LICENSE.txt and tools/VERIFICATION.txt, and distribution rights " +
            "for the binary.")
    }
    if ($file.Name -match '^\.git|^Thumbs\.db$|^\.DS_Store$|^desktop\.ini$') {
        Add-Error "$relative is a source-control or operating-system index file and must not be packaged"
    }
}

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
