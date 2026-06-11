<#
.SYNOPSIS
    Validates the canonical Codexo.ExoSnap WinGet manifest set.

.DESCRIPTION
    Dependency-free regex/line-based checks (no YAML module) over
    packaging/winget/manifests/c/Codexo/ExoSnap/<version>/:

      - all three manifest files exist (version, installer, locale en-US)
      - PackageIdentifier is Codexo.ExoSnap in all three
      - PackageVersion is identical across all three
      - ManifestVersion is 1.10.0 in all three
      - installer: InstallerSha256 is 64 uppercase hex characters
      - installer: ProductCode and UpgradeCode are well-formed braced GUIDs
      - installer: Microsoft.VCRedist.2015+.x64 is declared under
        Dependencies/PackageDependencies (regression guard — see below)
      - installer: InstallerUrl matches the expected GitHub Release MSI asset URL

.PARAMETER Version
    Manifest version directory to validate (e.g. "0.1.0"). Defaults to the
    single version directory present under packaging/winget/manifests/c/Codexo/ExoSnap/.
#>
param(
    [string]$Version
)

$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$manifestRoot = Join-Path $repoRoot 'packaging/winget/manifests/c/Codexo/ExoSnap'

if (-not (Test-Path -LiteralPath $manifestRoot -PathType Container)) {
    throw "WinGet manifest root not found: $manifestRoot"
}

if (-not $Version) {
    $versionDirs = @(Get-ChildItem -LiteralPath $manifestRoot -Directory)
    if ($versionDirs.Count -ne 1) {
        $names = ($versionDirs | ForEach-Object { $_.Name }) -join ', '
        throw "Expected exactly one version directory under $manifestRoot, found: $names. Pass -Version explicitly."
    }
    $Version = $versionDirs[0].Name
}

$versionDir = Join-Path $manifestRoot $Version
if (-not (Test-Path -LiteralPath $versionDir -PathType Container)) {
    throw "WinGet manifest version directory not found: $versionDir"
}

$versionPath = Join-Path $versionDir 'Codexo.ExoSnap.yaml'
$installerPath = Join-Path $versionDir 'Codexo.ExoSnap.installer.yaml'
$localePath = Join-Path $versionDir 'Codexo.ExoSnap.locale.en-US.yaml'

$script:Errors = [System.Collections.Generic.List[string]]::new()
function Add-Error { param([string]$Message) $script:Errors.Add($Message) | Out-Null }

# ---------------------------------------------------------------------------
# Existence
# ---------------------------------------------------------------------------
foreach ($entry in @(
        @{ Path = $versionPath; Label = 'version manifest' },
        @{ Path = $installerPath; Label = 'installer manifest' },
        @{ Path = $localePath; Label = 'locale en-US manifest' }
    )) {
    if (-not (Test-Path -LiteralPath $entry.Path -PathType Leaf)) {
        Add-Error "Missing $($entry.Label): $($entry.Path)"
    }
}
if ($script:Errors.Count -gt 0) {
    foreach ($e in $script:Errors) { Write-Host "  [FAIL] $e" }
    exit 1
}

$versionText = Get-Content -LiteralPath $versionPath -Raw
$installerText = Get-Content -LiteralPath $installerPath -Raw
$localeText = Get-Content -LiteralPath $localePath -Raw

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
function Get-YamlValue {
    param([string]$Text, [string]$Key, [string]$FileLabel)
    $m = [Regex]::Match($Text, "(?m)^\s*$([Regex]::Escape($Key)):\s*(\S+)\s*$")
    if (-not $m.Success) {
        Add-Error "$FileLabel`: missing key '$Key'"
        return $null
    }
    return $m.Groups[1].Value.Trim("'`"")
}

# ---------------------------------------------------------------------------
# PackageIdentifier — must be Codexo.ExoSnap in all three
# ---------------------------------------------------------------------------
foreach ($entry in @(
        @{ Text = $versionText; Label = 'version manifest' },
        @{ Text = $installerText; Label = 'installer manifest' },
        @{ Text = $localeText; Label = 'locale manifest' }
    )) {
    $id = Get-YamlValue -Text $entry.Text -Key 'PackageIdentifier' -FileLabel $entry.Label
    if ($id -and $id -ne 'Codexo.ExoSnap') {
        Add-Error "$($entry.Label): PackageIdentifier '$id' != 'Codexo.ExoSnap'"
    }
}

# ---------------------------------------------------------------------------
# PackageVersion — identical across all three
# ---------------------------------------------------------------------------
$versionVersion = Get-YamlValue -Text $versionText -Key 'PackageVersion' -FileLabel 'version manifest'
$installerVersion = Get-YamlValue -Text $installerText -Key 'PackageVersion' -FileLabel 'installer manifest'
$localeVersion = Get-YamlValue -Text $localeText -Key 'PackageVersion' -FileLabel 'locale manifest'
if ($versionVersion -and $installerVersion -and $versionVersion -ne $installerVersion) {
    Add-Error "PackageVersion mismatch: version manifest '$versionVersion' != installer manifest '$installerVersion'"
}
if ($versionVersion -and $localeVersion -and $versionVersion -ne $localeVersion) {
    Add-Error "PackageVersion mismatch: version manifest '$versionVersion' != locale manifest '$localeVersion'"
}
if ($versionVersion -and $versionVersion -ne $Version) {
    Add-Error "PackageVersion '$versionVersion' does not match manifest directory name '$Version'"
}

# ---------------------------------------------------------------------------
# ManifestVersion — 1.10.0 in all three
# ---------------------------------------------------------------------------
foreach ($entry in @(
        @{ Text = $versionText; Label = 'version manifest' },
        @{ Text = $installerText; Label = 'installer manifest' },
        @{ Text = $localeText; Label = 'locale manifest' }
    )) {
    $mv = Get-YamlValue -Text $entry.Text -Key 'ManifestVersion' -FileLabel $entry.Label
    if ($mv -and $mv -ne '1.10.0') {
        Add-Error "$($entry.Label): ManifestVersion '$mv' != '1.10.0'"
    }
}

# ---------------------------------------------------------------------------
# InstallerSha256 — 64 uppercase hex characters
# ---------------------------------------------------------------------------
$sha = Get-YamlValue -Text $installerText -Key 'InstallerSha256' -FileLabel 'installer manifest'
if ($sha -and $sha -notmatch '^[0-9A-F]{64}$') {
    Add-Error "installer manifest: InstallerSha256 '$sha' is not 64 uppercase hex characters"
}

# ---------------------------------------------------------------------------
# ProductCode / UpgradeCode — well-formed braced GUIDs
# ---------------------------------------------------------------------------
$guidPattern = '^\{[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}\}$'
$productCodeMatches = [Regex]::Matches($installerText, "(?m)^\s*ProductCode:\s*'([^']+)'")
if ($productCodeMatches.Count -eq 0) {
    Add-Error "installer manifest: no ProductCode entries found"
}
foreach ($m in $productCodeMatches) {
    $guid = $m.Groups[1].Value
    if ($guid -notmatch $guidPattern) {
        Add-Error "installer manifest: ProductCode '$guid' is not a well-formed braced GUID"
    }
}
$upgradeCodeMatches = [Regex]::Matches($installerText, "(?m)^\s*UpgradeCode:\s*'([^']+)'")
if ($upgradeCodeMatches.Count -eq 0) {
    Add-Error "installer manifest: no UpgradeCode entries found"
}
foreach ($m in $upgradeCodeMatches) {
    $guid = $m.Groups[1].Value
    if ($guid -notmatch $guidPattern) {
        Add-Error "installer manifest: UpgradeCode '$guid' is not a well-formed braced GUID"
    }
}

# ---------------------------------------------------------------------------
# Dependencies — Microsoft.VCRedist.2015+.x64 (REGRESSION GUARD)
#
# ExoSnap (exosnap.exe and the shipped Qt6 DLLs) links the dynamic MSVC
# runtime (/MD) and is not bundled with VCRUNTIME140.dll, MSVCP140.dll, etc.
# The MSI does not install the redistributable. If this dependency is removed
# from the installer manifest, WinGet installs ExoSnap on a clean machine
# without the MSVC runtime present, and exosnap.exe fails to start with
# STATUS_DLL_NOT_FOUND (0xC0000135) — exactly the 0.1.0 WinGet validation
# failure this manifest fixes.
# ---------------------------------------------------------------------------
if ($installerText -notmatch '(?m)^\s*PackageDependencies:\s*$') {
    Add-Error "installer manifest: missing Dependencies/PackageDependencies block (regression: removing this reintroduces STATUS_DLL_NOT_FOUND on clean machines)"
}
elseif ($installerText -notmatch "(?m)^\s*-\s*PackageIdentifier:\s*Microsoft\.VCRedist\.2015\+\.x64\s*$") {
    Add-Error "installer manifest: Dependencies/PackageDependencies does not declare Microsoft.VCRedist.2015+.x64 (regression: removing this reintroduces STATUS_DLL_NOT_FOUND on clean machines)"
}

# ---------------------------------------------------------------------------
# InstallerUrl — must match the expected GitHub Release MSI asset
# ---------------------------------------------------------------------------
$installerUrl = Get-YamlValue -Text $installerText -Key 'InstallerUrl' -FileLabel 'installer manifest'
$expectedUrl = "https://github.com/Exoridus/exosnap/releases/download/v$Version/ExoSnap-$Version-windows-x64.msi"
if ($installerUrl -and $installerUrl -ne $expectedUrl) {
    Add-Error "installer manifest: InstallerUrl '$installerUrl' != expected '$expectedUrl'"
}

# ---------------------------------------------------------------------------
# Result
# ---------------------------------------------------------------------------
if ($script:Errors.Count -gt 0) {
    foreach ($e in $script:Errors) { Write-Host "  [FAIL] $e" }
    Write-Host "WinGet manifest validation FAILED ($($script:Errors.Count) error(s)) for version $Version." -ForegroundColor Red
    exit 1
}

Write-Host "WinGet manifest validation PASSED for Codexo.ExoSnap $Version (3 files, dependency on Microsoft.VCRedist.2015+.x64 confirmed)." -ForegroundColor Green
exit 0
