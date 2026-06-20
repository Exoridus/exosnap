<#
.SYNOPSIS
    Builds the canonical ExoSnap portable Windows x64 release artifact.

.DESCRIPTION
    Produces a deterministic, validated portable ZIP for the current canonical
    project version (parsed from the root CMakeLists.txt project(VERSION)).

    The CMake install tree is the sole authority for the package contents — this
    script never hand-copies runtime files. Steps:

      1. configure + build the Release exosnap target (skippable)
      2. cmake --install into an isolated staging tree under .workspace/
      3. validate the install tree (presence / absence / leak / exe metadata)
      4. create the canonical ZIP with one top-level package directory
      5. write a SHA-256 sidecar
      6. write a machine-readable manifest and a human-readable report
      7. launch the extracted ZIP in an isolated environment (smoke; skippable)

    All generated outputs live under .workspace/release/<version>/ and are never
    committed. The script is idempotent: rerunning cleans its own staging.

    This script does NOT create a git tag, GitHub Release, or change repository
    visibility. Publication is a separate, manual, approved step.

.PARAMETER SkipConfigure
    Skip the CMake configure step (assumes the Release build tree is configured).

.PARAMETER SkipBuild
    Skip configure and build; package whatever Release exosnap.exe already exists.

.PARAMETER SkipSmoke
    Skip the extracted-ZIP launch smoke test.

.PARAMETER SkipMsi
    Skip MSI package build (requires WiX Toolset v4).

.PARAMETER KeepStaging
    Do not delete a previous staging tree before installing (debugging aid).
#>
[CmdletBinding()]
param(
    [switch]$SkipConfigure,
    [switch]$SkipBuild,
    [switch]$SkipSmoke,
    [switch]$SkipMsi,
    [switch]$KeepStaging
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------------------
# Paths (resolved from the script location, independent of the caller's CWD)
# ---------------------------------------------------------------------------
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$Platform = 'windows-x64'
$Preset = 'windows-x64-release'
$BuildDir = Join-Path $RepoRoot "build/$Preset"
$ReleaseExe = Join-Path $BuildDir 'app/Release/exosnap.exe'

# ---------------------------------------------------------------------------
# Canonical version — single source of truth is the root project(... VERSION ...)
# ---------------------------------------------------------------------------
$cmakeText = Get-Content -LiteralPath (Join-Path $RepoRoot 'CMakeLists.txt') -Raw
if ($cmakeText -notmatch 'project\(\s*exosnap\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') {
    throw "Could not parse project(exosnap VERSION x.y.z) from root CMakeLists.txt."
}
$Version = $Matches[1]
$PortablePackageName = "ExoSnap-$Version-$Platform-portable"
$MsiPackageName = "ExoSnap-$Version-$Platform"

$ReleaseRoot = Join-Path $RepoRoot '.workspace/release'
$ReleaseDir = Join-Path $ReleaseRoot $Version
$StagingDir = Join-Path $ReleaseDir 'staging'
$PackageRoot = Join-Path $StagingDir $PortablePackageName
$ZipPath = Join-Path $ReleaseDir "$PortablePackageName.zip"
$ShaPath = Join-Path $ReleaseDir "$PortablePackageName.sha256"
$MsiPath = Join-Path $ReleaseDir "$MsiPackageName.msi"
$MsiShaPath = Join-Path $ReleaseDir "$MsiPackageName.msi.sha256"
$ManifestPath = Join-Path $ReleaseDir 'artifact-manifest.json'
$ReportPath = Join-Path $ReleaseDir 'validation-report.md'
$SmokeDir = Join-Path $ReleaseDir 'smoke'

$msiBuilt = $false
$msiSha = ''

$script:Errors = [System.Collections.Generic.List[string]]::new()
$script:ReportLines = [System.Collections.Generic.List[string]]::new()

function Add-ReportLine { param([string]$Line) $script:ReportLines.Add($Line) | Out-Null }
function Add-Error { param([string]$Message) $script:Errors.Add($Message) | Out-Null; Write-Host "  [FAIL] $Message" }
function Write-Step { param([string]$Message) Write-Host "==> $Message" }

# Guard: only ever recursively delete inside .workspace/release/.
function Remove-SafeDir {
    param([string]$Path)
    $full = [System.IO.Path]::GetFullPath($Path)
    $guard = [System.IO.Path]::GetFullPath($ReleaseRoot)
    if (-not $full.StartsWith($guard, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to delete a path outside the release workspace: $full"
    }
    if (Test-Path -LiteralPath $full) {
        Remove-Item -LiteralPath $full -Recurse -Force
    }
}

# ---------------------------------------------------------------------------
# Static runtime-dependency audit
#
# exosnap.exe and the shipped Qt6 DLLs link the dynamic MSVC runtime (/MD) and
# statically import VCRUNTIME140.dll, MSVCP140.dll, etc. Neither the MSI nor
# the portable ZIP bundle these — WinGet satisfies them via the declared
# Microsoft.VCRedist.2015+.x64 package dependency, and direct MSI/portable
# users are documented to install the VC++ 2015-2022 redistributable.
#
# This audit walks every PE binary in the staging tree with `dumpbin
# /dependents` and classifies each statically-imported DLL name so that any
# import that is neither shipped, a documented Windows system component, nor
# the known MSVC runtime set fails the build instead of shipping silently.
# ---------------------------------------------------------------------------

# Windows system DLLs known to be present on a stock Windows 10/11 x64
# install, grouped by subsystem. Anything not in this list (and not shipped
# in the staging tree, and not a known MSVC runtime DLL) is UNRESOLVED.
$WindowsSystemDllAllowlist = @(
    # Core / kernel / process
    'kernel32.dll', 'advapi32.dll', 'ntdll.dll', 'rpcrt4.dll', 'secur32.dll', 'sspicli.dll',
    'userenv.dll', 'powrprof.dll', 'cfgmgr32.dll', 'setupapi.dll', 'normaliz.dll',
    # MSVC / C runtime (system-provided, not part of the redistributable)
    'ucrtbase.dll', 'msvcrt.dll',
    # Shell / UI / windowing
    'user32.dll', 'gdi32.dll', 'shell32.dll', 'shlwapi.dll', 'comdlg32.dll', 'comctl32.dll',
    'uxtheme.dll', 'imm32.dll', 'uiautomationcore.dll', 'dwmapi.dll',
    # COM / OLE
    'ole32.dll', 'oleaut32.dll',
    # Crypto / security
    'crypt32.dll', 'bcrypt.dll', 'ncrypt.dll', 'authz.dll', 'dbghelp.dll',
    # Networking
    'ws2_32.dll', 'netapi32.dll', 'wtsapi32.dll', 'dnsapi.dll', 'iphlpapi.dll',
    'winhttp.dll', 'wininet.dll', 'urlmon.dll', 'wldap32.dll', 'mpr.dll',
    # Graphics / media (Direct3D, DXGI, Media Foundation, audio)
    'd3d11.dll', 'd3d9.dll', 'd3d12.dll', 'dxgi.dll', 'dxva2.dll', 'dcomp.dll',
    'dwrite.dll', 'd2d1.dll', 'windowscodecs.dll',
    'mf.dll', 'mfplat.dll', 'mfreadwrite.dll', 'mfcore.dll',
    'propsys.dll', 'avrt.dll', 'ksuser.dll', 'audioses.dll', 'mmdevapi.dll',
    # Misc
    'winmm.dll', 'version.dll'
)

# The dynamic MSVC runtime (vcruntime/msvcp/concrt/vcomp). Satisfied via the
# WinGet package dependency Microsoft.VCRedist.2015+.x64 (installer manifest)
# or a user-installed Microsoft Visual C++ 2015-2022 Redistributable (x64).
$ExternalPrerequisiteDlls = @(
    'vcruntime140.dll', 'vcruntime140_1.dll',
    'msvcp140.dll', 'msvcp140_1.dll', 'msvcp140_2.dll',
    'msvcp140_atomic_wait.dll', 'msvcp140_codecvt_ids.dll',
    'concrt140.dll', 'vcomp140.dll'
)

# Locate dumpbin.exe via vswhere (part of the required MSVC toolchain).
function Find-Dumpbin {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        throw "vswhere.exe not found at '$vswhere'. The MSVC toolchain (Visual Studio Build Tools/Community) is required."
    }
    $dumpbin = & $vswhere -latest -find 'VC\Tools\MSVC\**\bin\Hostx64\x64\dumpbin.exe' | Select-Object -First 1
    if (-not $dumpbin -or -not (Test-Path -LiteralPath $dumpbin -PathType Leaf)) {
        throw "dumpbin.exe not found via vswhere. Install the MSVC v143 build tools (Desktop development with C++)."
    }
    return $dumpbin
}

# Parse the "Image has the following dependencies:" block from `dumpbin
# /dependents` output into a list of imported DLL names.
function Get-DumpbinImports {
    param([string]$Dumpbin, [string]$BinaryPath)

    $out = & $Dumpbin /dependents $BinaryPath
    if ($LASTEXITCODE -ne 0) { throw "dumpbin /dependents failed for '$BinaryPath' with exit code $LASTEXITCODE." }

    $imports = [System.Collections.Generic.List[string]]::new()
    $inDeps = $false
    foreach ($line in $out) {
        $trimmed = $line.Trim()
        if (-not $inDeps) {
            if ($trimmed -eq 'Image has the following dependencies:') { $inDeps = $true }
            continue
        }
        if ($trimmed -eq '') { continue }
        if ($trimmed -eq 'Summary') { break }
        # Delay-loaded DLLs do not fail the loader at process start; only
        # static imports are audited here.
        if ($trimmed -like 'Image has the following delay load dependencies*') { break }
        $imports.Add($trimmed)
    }
    return $imports
}

# Audit every .exe/.dll in $Root against $WindowsSystemDllAllowlist,
# $ExternalPrerequisiteDlls, and the files present in $Root itself. Adds a
# build error (via Add-Error) for every UNRESOLVED import. On success, prints
# a one-line summary of classification counts.
function Test-RuntimeDependencies {
    param([string]$Root)

    Write-Step "Auditing static runtime dependencies (dumpbin /dependents)"

    $dumpbin = Find-Dumpbin
    $binaries = Get-ChildItem -LiteralPath $Root -Recurse -Include '*.exe', '*.dll'

    $shippedNames = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($b in $binaries) { [void]$shippedNames.Add($b.Name) }

    $externalSet = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($d in $ExternalPrerequisiteDlls) { [void]$externalSet.Add($d) }

    $systemSet = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($d in $WindowsSystemDllAllowlist) { [void]$systemSet.Add($d) }

    $countPresent = 0
    $countSystem = 0
    $countExternal = 0
    $unresolved = [System.Collections.Generic.List[string]]::new()
    $externalSeen = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)

    foreach ($binary in $binaries) {
        $imports = Get-DumpbinImports -Dumpbin $dumpbin -BinaryPath $binary.FullName
        foreach ($import in $imports) {
            if ($shippedNames.Contains($import)) { $countPresent++; continue }
            if ($externalSet.Contains($import)) { $countExternal++; [void]$externalSeen.Add($import.ToLowerInvariant()); continue }
            if ($systemSet.Contains($import)) { $countSystem++; continue }
            if ($import -match '^(api-ms-win-|ext-ms-)') { $countSystem++; continue }
            $unresolved.Add("$($binary.FullName.Substring($Root.Length + 1)) -> $import")
        }
    }

    foreach ($entry in $unresolved) {
        Add-Error "Unresolved runtime dependency: $entry"
    }

    if ($unresolved.Count -eq 0) {
        Write-Host "  Runtime dependency audit: $($binaries.Count) binaries, $countPresent present-in-tree, $countSystem Windows-system, $countExternal MSVC-runtime (redistributable) imports, 0 unresolved."
    }
}

# Run a native command with a lightweight elapsed-seconds heartbeat so long
# build/install steps visibly progress. Streams output; throws on nonzero exit.
function Invoke-Heartbeat {
    param([string]$Name, [string]$FilePath, [string[]]$Arguments)

    Write-Host "$Name..." -NoNewline
    $logPath = Join-Path ([System.IO.Path]::GetTempPath()) ("exosnap-rel-{0}-{1}.log" -f ($Name -replace '[^A-Za-z0-9]', '-'), $PID)
    $errPath = "$logPath.err"
    Remove-Item -LiteralPath $logPath, $errPath -Force -ErrorAction SilentlyContinue
    $started = Get-Date

    $proc = Start-Process -FilePath $FilePath -ArgumentList $Arguments -WorkingDirectory $RepoRoot `
        -NoNewWindow -PassThru -RedirectStandardOutput $logPath -RedirectStandardError $errPath

    $nextBeat = 10
    while (-not $proc.HasExited) {
        Start-Sleep -Milliseconds 1000
        $elapsed = ((Get-Date) - $started).TotalSeconds
        if ($elapsed -ge $nextBeat) { Write-Host (" {0}s" -f [int]$elapsed) -NoNewline; $nextBeat += 15 }
    }
    $proc.WaitForExit()
    $total = [int]((Get-Date) - $started).TotalSeconds

    if ($proc.ExitCode -ne 0) {
        Write-Host ""
        if (Test-Path -LiteralPath $logPath) { Get-Content -LiteralPath $logPath -Tail 60 | ForEach-Object { Write-Host $_ } }
        if (Test-Path -LiteralPath $errPath) { Get-Content -LiteralPath $errPath -Tail 40 | ForEach-Object { Write-Host $_ } }
        throw "$Name failed with exit code $($proc.ExitCode)."
    }
    Remove-Item -LiteralPath $logPath, $errPath -Force -ErrorAction SilentlyContinue
    Write-Host " OK (${total}s)"
}

Write-Host ""
Write-Host "ExoSnap release artifact builder"
Write-Host "  Version  : $Version"
Write-Host "  Portable : $PortablePackageName"
Write-Host "  MSI      : $MsiPackageName"
Write-Host "  Output   : $ReleaseDir"
Write-Host ""

# ---------------------------------------------------------------------------
# 1. Configure + build (Release)
# ---------------------------------------------------------------------------
if (-not $SkipBuild) {
    if (-not $SkipConfigure) {
        Invoke-Heartbeat -Name 'cmake configure' -FilePath 'cmake' -Arguments @('--preset', $Preset)
    }
    Invoke-Heartbeat -Name 'cmake build (Release exosnap)' -FilePath 'cmake' -Arguments @('--build', '--preset', "$Preset-exosnap")
}
else {
    Write-Step "Skipping configure/build (-SkipBuild)."
}

if (-not (Test-Path -LiteralPath $ReleaseExe -PathType Leaf)) {
    throw "Release executable not found at '$ReleaseExe'. Build first (omit -SkipBuild)."
}

# ---------------------------------------------------------------------------
# 2. Install into clean staging (CMake install is the sole authority)
# ---------------------------------------------------------------------------
New-Item -ItemType Directory -Path $ReleaseDir -Force | Out-Null
if (-not $KeepStaging) { Remove-SafeDir $StagingDir }
New-Item -ItemType Directory -Path $PackageRoot -Force | Out-Null

Write-Step "Installing into staging tree: $PackageRoot"
Invoke-Heartbeat -Name 'cmake install' -FilePath 'cmake' `
    -Arguments @('--install', $BuildDir, '--config', 'Release', '--prefix', $PackageRoot)

# ---------------------------------------------------------------------------
# 2b. Prune leaked dependency development trees
#
# Several vendored static dependencies (libmatroska, libebml, fdk-aac, opus,
# Crashpad/mini_chromium) carry install(TARGETS)/install(FILES) rules we never
# consume: they emit import libraries, headers, CMake package configs, and
# pkg-config files into lib/ and include/. Unlike gtest/spdlog/nlohmann/sentry —
# all disabled via their own INSTALL=OFF switches in third_party/CMakeLists.txt
# and VendorSentry.cmake — these projects expose no such switch (or install via a
# git submodule we do not control), so `cmake --install` unavoidably stages their
# dev trees. The portable package ships runtime only: exosnap.exe, the Qt/FFmpeg
# DLLs, crashpad_handler.exe, plugins/, and licenses/ — all flat or under
# plugins/ per Qt convention. Nothing the app loads at runtime lives in lib/ or
# include/, so remove these dev trees before the absence audit asserts on them.
foreach ($devDir in @('lib', 'include')) {
    $devPath = Join-Path $PackageRoot $devDir
    if (Test-Path -LiteralPath $devPath -PathType Container) {
        Remove-Item -LiteralPath $devPath -Recurse -Force
        Write-Host "  Pruned leaked dependency dev tree: $devDir/"
    }
}

# ---------------------------------------------------------------------------
# 3. Validate the install tree
# ---------------------------------------------------------------------------
Write-Step "Validating install tree"

$allFiles = Get-ChildItem -LiteralPath $PackageRoot -Recurse -File
$relPaths = $allFiles | ForEach-Object { $_.FullName.Substring($PackageRoot.Length + 1) }

# Presence — required runtime files and docs.
$requiredFiles = @(
    'exosnap.exe', 'qt.conf',
    'Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll', 'Qt6Svg.dll',
    'LICENSE', 'THIRD_PARTY_NOTICES.md', 'KNOWN_LIMITATIONS.md', 'README-PORTABLE.md'
)
foreach ($f in $requiredFiles) {
    if (-not (Test-Path -LiteralPath (Join-Path $PackageRoot $f) -PathType Leaf)) { Add-Error "Missing required file: $f" }
}
foreach ($d in @('plugins/platforms', 'licenses')) {
    if (-not (Test-Path -LiteralPath (Join-Path $PackageRoot $d) -PathType Container)) { Add-Error "Missing required directory: $d" }
}
$requiredLicenses = @('spdlog.txt', 'nlohmann_json.txt', 'tomlplusplus.txt', 'opus.txt', 'fdk-aac.txt', 'libebml.txt', 'libmatroska.txt', 'qt.txt', 'ibm-plex-mono.txt')
foreach ($lic in $requiredLicenses) {
    if (-not (Test-Path -LiteralPath (Join-Path $PackageRoot "licenses/$lic") -PathType Leaf)) { Add-Error "Missing third-party license: licenses/$lic" }
}

# Crash-capture (ADR 0017) is an optional build: crashpad_handler.exe is only in
# the install tree when EXOSNAP_ENABLE_CRASH_CAPTURE=ON. When present, the Sentry/
# Crashpad/mini_chromium license texts MUST ship alongside it (VendorSentry stages
# them into licenses/). When absent (OFF build), neither is required, so packaging
# does not break for self-builds.
$CrashpadHandlerName = 'crashpad_handler.exe'
$IncludeCrashpad = Test-Path -LiteralPath (Join-Path $PackageRoot $CrashpadHandlerName) -PathType Leaf
if ($IncludeCrashpad) {
    Write-Host "  Crash capture: ON ($CrashpadHandlerName present — bundling handler + sentry licenses)"
    $crashLicenses = @('sentry-native.txt', 'crashpad.txt', 'mini_chromium.txt')
    foreach ($lic in $crashLicenses) {
        if (-not (Test-Path -LiteralPath (Join-Path $PackageRoot "licenses/$lic") -PathType Leaf)) { Add-Error "Missing crash-capture license: licenses/$lic" }
    }
}
else {
    Write-Host "  Crash capture: OFF ($CrashpadHandlerName absent — handler/sentry licenses not bundled)"
}

# Absence — development files, user data, and workspace artifacts must not leak.
$forbiddenExt = @('.pdb', '.ilk', '.exp', '.lib', '.obj', '.pch', '.h', '.hpp', '.cpp', '.c', '.cmake', '.pc',
    '.suo', '.user', '.vcxproj', '.sln', '.log', '.tmp')
foreach ($file in $allFiles) {
    if ($forbiddenExt -contains $file.Extension.ToLowerInvariant()) {
        Add-Error "Forbidden development file leaked: $($file.FullName.Substring($PackageRoot.Length + 1))"
    }
}
$forbiddenNames = @('recording-history.json', 'settings.ini', 'presets.ini')
foreach ($file in $allFiles) {
    if ($forbiddenNames -contains $file.Name.ToLowerInvariant()) {
        Add-Error "User-data file leaked: $($file.Name)"
    }
}
foreach ($dir in @('.git', '.github', '.workspace', '.claude', 'include', 'lib', 'src', 'tests', 'CMakeFiles', 'Testing')) {
    if (Test-Path -LiteralPath (Join-Path $PackageRoot $dir) -PathType Container) { Add-Error "Forbidden directory leaked: $dir/" }
}
# No Debug Qt DLLs (Qt6Cored.dll etc.).
$debugQt = $allFiles | Where-Object { $_.Name -match '^Qt6.*d\.dll$' }
foreach ($d in $debugQt) { Add-Error "Debug Qt DLL leaked: $($d.Name)" }

# Path / secret leak scan over text-like files only.
$textExt = @('.md', '.txt', '.json', '.ini', '.conf')
$leakPatterns = @('C:\\Users\\', $RepoRoot, '.workspace', '.claude', 'dima@codexo.de')
foreach ($file in ($allFiles | Where-Object { $textExt -contains $_.Extension.ToLowerInvariant() })) {
    $content = Get-Content -LiteralPath $file.FullName -Raw -ErrorAction SilentlyContinue
    if (-not $content) { continue }
    foreach ($pat in $leakPatterns) {
        if ($content -match [Regex]::Escape($pat)) {
            Add-Error "Path/secret leak '$pat' in $($file.FullName.Substring($PackageRoot.Length + 1))"
        }
    }
}

# Executable version metadata.
$vi = (Get-Item -LiteralPath (Join-Path $PackageRoot 'exosnap.exe')).VersionInfo
$expectedFileVersion = "$Version.0"
if ($vi.ProductVersion -ne $Version) { Add-Error "exe ProductVersion '$($vi.ProductVersion)' != '$Version'" }
if ($vi.FileVersion -ne $expectedFileVersion) { Add-Error "exe FileVersion '$($vi.FileVersion)' != '$expectedFileVersion'" }
if ($vi.ProductName -ne 'ExoSnap') { Add-Error "exe ProductName '$($vi.ProductName)' != 'ExoSnap'" }

# Documentation must name the canonical version and not call the release 1.0.
$knownLimits = Get-Content -LiteralPath (Join-Path $PackageRoot 'KNOWN_LIMITATIONS.md') -Raw
if ($knownLimits -notmatch [Regex]::Escape($Version)) { Add-Error "KNOWN_LIMITATIONS.md does not name version $Version" }

Write-Host "  Files in package : $($allFiles.Count)"

# ---------------------------------------------------------------------------
# 3a. Runtime pruning — remove safely known-unnecessary files
# ---------------------------------------------------------------------------
$pruneCandidates = @(
    'opengl32sw.dll'           # Software OpenGL fallback — ExoSnap uses D3D11 exclusively
)
$removedFiles = @()
foreach ($candidate in $pruneCandidates) {
    $candidatePath = Join-Path $PackageRoot $candidate
    if (Test-Path -LiteralPath $candidatePath) {
        Remove-Item -LiteralPath $candidatePath -Force
        $removedFiles += $candidate
        Write-Host "  Pruned: $candidate"
    }
}
# Remove translations directory if present (no i18n in MVP)
$translationsDir = Join-Path $PackageRoot 'translations'
if (Test-Path -LiteralPath $translationsDir) {
    Remove-Item -LiteralPath $translationsDir -Recurse -Force
    $removedFiles += 'translations/'
    Write-Host "  Pruned: translations/"
}
# Remove bearer/TLS plugins (no network features in MVP)
foreach ($dir in @('bearer', 'tls')) {
    $pluginDir = Join-Path $PackageRoot "plugins/$dir"
    if (Test-Path -LiteralPath $pluginDir) {
        Remove-Item -LiteralPath $pluginDir -Recurse -Force
        $removedFiles += "plugins/$dir/"
        Write-Host "  Pruned: plugins/$dir/"
    }
}
$allFiles = Get-ChildItem -LiteralPath $PackageRoot -Recurse -File
Write-Host "  Files after pruning: $($allFiles.Count)"

# ---------------------------------------------------------------------------
# 3b. Static runtime-dependency audit (final pruned tree)
# ---------------------------------------------------------------------------
Test-RuntimeDependencies -Root $PackageRoot

# ---------------------------------------------------------------------------
# 4. Create the canonical ZIP (one top-level package directory)
# ---------------------------------------------------------------------------
Write-Step "Creating ZIP: $PortablePackageName.zip"
Add-Type -AssemblyName System.IO.Compression.FileSystem
Remove-Item -LiteralPath $ZipPath -Force -ErrorAction SilentlyContinue
# includeBaseDirectory = $true -> archive entries are prefixed with the package
# directory name, giving exactly one top-level folder inside the ZIP.
[System.IO.Compression.ZipFile]::CreateFromDirectory(
    $PackageRoot, $ZipPath, [System.IO.Compression.CompressionLevel]::Optimal, $true)

$zipInfo = Get-Item -LiteralPath $ZipPath
$uncompressed = ($allFiles | Measure-Object -Property Length -Sum).Sum

# ---------------------------------------------------------------------------
# 5. SHA-256 sidecar (generated AFTER the final ZIP)
# ---------------------------------------------------------------------------
$sha = (Get-FileHash -LiteralPath $ZipPath -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content -LiteralPath $ShaPath -Value "$sha  $PortablePackageName.zip" -NoNewline -Encoding ascii
Write-Host "  SHA-256 : $sha"

# ---------------------------------------------------------------------------
# 6. MSI package (WiX Toolset v4)
# ---------------------------------------------------------------------------
if (-not $SkipMsi) {
    Write-Step "Building MSI: $MsiPackageName.msi"

    $wix = Get-Command wix -ErrorAction SilentlyContinue
    if (-not $wix) {
        Write-Host "  WiX Toolset v4 not found on PATH. Skipping MSI build."
        Write-Host "  Install: dotnet tool install --global wix"
    }
    else {
        $wxsPath = Join-Path $RepoRoot 'packaging/msi/Package.wxs'
        if (-not (Test-Path -LiteralPath $wxsPath)) {
            Add-Error "MSI: packaging/msi/Package.wxs not found"
        }
        else {
            try {
                # IncludeCrashpad mirrors the staging tree: only emit the
                # crashpad_handler.exe component when the ON build produced it,
                # so `wix build` never references a missing source file.
                $includeCrashpadValue = if ($IncludeCrashpad) { 'yes' } else { 'no' }
                $msiArgs = @(
                    'build', '-arch', 'x64',
                    '-o', $MsiPath,
                    '-d', "StagingDir=$PackageRoot",
                    '-d', "ProductVersion=$Version",
                    '-d', "IncludeCrashpad=$includeCrashpadValue",
                    $wxsPath
                )
                Invoke-Heartbeat -Name 'wix build' -FilePath 'wix' -Arguments $msiArgs

                if (Test-Path -LiteralPath $MsiPath) {
                    $msiSha = (Get-FileHash -LiteralPath $MsiPath -Algorithm SHA256).Hash.ToLowerInvariant()
                    Set-Content -LiteralPath $MsiShaPath -Value "$msiSha  $MsiPackageName.msi" -NoNewline -Encoding ascii
                    Write-Host "  MSI SHA-256: $msiSha"
                    $msiBuilt = $true
                }
                else {
                    Add-Error "MSI: build did not produce output file"
                    $msiBuilt = $false
                }
            }
            catch {
                Add-Error "MSI: build failed: $_"
                $msiBuilt = $false
            }
        }
    }
}
else {
    Write-Step "Skipping MSI build (-SkipMsi)."
    $msiBuilt = $false
    $msiSha = ''
}

# ---------------------------------------------------------------------------
# 7. Manifest + report
# ---------------------------------------------------------------------------
$sourceCommit = (& git -C $RepoRoot rev-parse HEAD).Trim()
$fileEntries = foreach ($file in ($allFiles | Sort-Object FullName)) {
    [ordered]@{
        path   = "$PortablePackageName/" + $file.FullName.Substring($PackageRoot.Length + 1).Replace('\', '/')
        size   = $file.Length
        sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}
$manifest = [ordered]@{
    product         = 'ExoSnap'
    version         = $Version
    platform        = $Platform
    sourceCommit    = $sourceCommit
    portableArchive = "$PortablePackageName.zip"
    portableSha256  = $sha
    fileCount       = $allFiles.Count
    files           = $fileEntries
}
if ($msiBuilt) {
    $manifest['msiPackage'] = "$MsiPackageName.msi"
    $manifest['msiSha256'] = $msiSha
}
$manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $ManifestPath -Encoding utf8

# ---------------------------------------------------------------------------
# 8. Extracted-ZIP smoke (isolated environment; cannot touch real user data)
#
# Missing-DLL robustness: by default Windows shows a modal "System Error" /
# "Systemfehler" dialog when a required DLL cannot be found, and the loader
# hangs waiting for the user to dismiss it.  The smoke would then see a live
# process at timeout and report 'launched' — a false positive.
#
# Fix (two layers):
#   Layer 1 — SetErrorMode: before launching the child, we suppress the
#     Windows hard-error UI with SetErrorMode(SEM_FAILCRITICALERRORS |
#     SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX).  Child processes
#     inherit the error mode, so the loader converts a missing-DLL condition
#     into an immediate non-zero exit instead of a dialog.  The parent's
#     prior error mode is restored after launch.
#   Layer 2 — dialog sentinel: during the poll loop we enumerate top-level
#     windows on the smoke process and treat any window whose title matches
#     the Windows hard-error / application-error dialog pattern as a failure
#     (catches any residual dialog from a runtime that resets its own error
#     mode before the loader dialog fires).
# ---------------------------------------------------------------------------
$smokeResult = 'skipped'
$smokeNotes = @()
if (-not $SkipSmoke) {
    Write-Step "Running extracted-ZIP smoke (isolated)"

    # Load Win32 helpers needed for the two robustness layers.
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Collections.Generic;
using System.Text;

public static class SmokeNative {
    // Error-mode flags
    public const uint SEM_FAILCRITICALERRORS  = 0x0001;
    public const uint SEM_NOGPFAULTERRORBOX   = 0x0002;
    public const uint SEM_NOOPENFILEERRORBOX  = 0x8000;

    [DllImport("kernel32.dll")]
    public static extern uint SetErrorMode(uint uMode);

    // Window enumeration for dialog sentinel
    public delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint lpdwProcessId);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetWindowText(IntPtr hwnd, StringBuilder lpString, int nMaxCount);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool IsWindowVisible(IntPtr hwnd);

    /// <summary>
    /// Returns all visible top-level window titles belonging to the given PID.
    /// </summary>
    public static List<string> GetVisibleWindowTitles(int pid) {
        var titles = new List<string>();
        EnumWindows((hwnd, _) => {
            uint wpid;
            GetWindowThreadProcessId(hwnd, out wpid);
            if ((int)wpid == pid && IsWindowVisible(hwnd)) {
                var sb = new StringBuilder(512);
                GetWindowText(hwnd, sb, sb.Capacity);
                var t = sb.ToString();
                if (t.Length > 0) titles.Add(t);
            }
            return true;
        }, IntPtr.Zero);
        return titles;
    }
}
'@ -Language CSharp -ErrorAction Stop

    Remove-SafeDir $SmokeDir
    $extractRoot = Join-Path $SmokeDir 'extracted'
    $isoConfig = Join-Path $SmokeDir 'config'
    $isoTemp = Join-Path $SmokeDir 'temp'
    New-Item -ItemType Directory -Path $extractRoot, $isoConfig, $isoTemp -Force | Out-Null
    [System.IO.Compression.ZipFile]::ExtractToDirectory($ZipPath, $extractRoot)

    $extractedPkg = Join-Path $extractRoot $PortablePackageName
    if (-not (Test-Path -LiteralPath $extractedPkg -PathType Container)) {
        Add-Error "Smoke: extracted ZIP does not contain a single top-level '$PortablePackageName' directory"
        $smokeResult = 'failed'
    }
    else {
        $smokeExe = Join-Path $extractedPkg 'exosnap.exe'

        # Snapshot the real per-user config dir so we can prove it is untouched.
        $realConfig = Join-Path $env:LOCALAPPDATA 'ExoSnap'
        function Get-DirSnapshot { param([string]$Dir)
            if (-not (Test-Path -LiteralPath $Dir)) { return @() }
            Get-ChildItem -LiteralPath $Dir -Recurse -File -ErrorAction SilentlyContinue |
            ForEach-Object { "{0}|{1}|{2}" -f $_.FullName, $_.Length, $_.LastWriteTimeUtc.Ticks }
        }
        $before = Get-DirSnapshot $realConfig

        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = $smokeExe
        $psi.WorkingDirectory = [System.IO.Path]::GetTempPath()  # CWD outside the repo
        $psi.UseShellExecute = $false
        $psi.EnvironmentVariables['EXOSNAP_CONFIG_DIR'] = $isoConfig
        $psi.EnvironmentVariables['TEMP'] = $isoTemp
        $psi.EnvironmentVariables['TMP'] = $isoTemp

        # Layer 1: suppress Windows hard-error dialogs in the child.
        # SetErrorMode is inherited by child processes; the loader turns a
        # missing-DLL condition into a non-zero exit instead of a modal dialog.
        $suppressFlags = [SmokeNative]::SEM_FAILCRITICALERRORS `
                       -bor [SmokeNative]::SEM_NOGPFAULTERRORBOX `
                       -bor [SmokeNative]::SEM_NOOPENFILEERRORBOX
        $prevErrorMode = [SmokeNative]::SetErrorMode($suppressFlags)
        try {
            $proc = [System.Diagnostics.Process]::Start($psi)
        } finally {
            # Restore the parent's error mode immediately after fork.
            [SmokeNative]::SetErrorMode($prevErrorMode) | Out-Null
        }

        # Layer 2 — dialog sentinel title patterns (belt-and-suspenders).
        # Matches the Windows "System Error" / "Systemfehler" hard-error box
        # and the WER "exosnap.exe has stopped working" dialog.
        $dialogPatterns = @(
            [regex]::new('System(fehler| Error)', [System.Text.RegularExpressions.RegexOptions]::IgnoreCase),
            [regex]::new([regex]::Escape('exosnap.exe'), [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
        )

        $deadline = (Get-Date).AddSeconds(8)
        $dialogDetected = $false
        while (-not $proc.HasExited -and (Get-Date) -lt $deadline) {
            Start-Sleep -Milliseconds 500
            # Check for error dialog windows owned by the smoke process.
            try {
                $titles = [SmokeNative]::GetVisibleWindowTitles($proc.Id)
                foreach ($title in $titles) {
                    if ($dialogPatterns | Where-Object { $_.IsMatch($title) }) {
                        $smokeNotes += "Dialog sentinel matched window title: '$title'"
                        $dialogDetected = $true
                        break
                    }
                }
            } catch { <# EnumWindows can race with process exit; ignore. #> }
            if ($dialogDetected) { break }
        }

        if ($dialogDetected) {
            # Kill the hung dialog process and report failure.
            try { $proc.Kill($true) } catch { }
            Add-Error "Smoke: error dialog detected (title matched sentinel pattern) — likely a missing DLL or loader failure"
            $smokeResult = 'failed'
        }
        elseif ($proc.HasExited) {
            if ($proc.ExitCode -eq 0) {
                $smokeResult = 'inconclusive'
                $smokeNotes += "Process exited cleanly (exit 0) within the wait window; may be the single-instance guard. UI stability not confirmed."
            }
            else {
                Add-Error "Smoke: extracted exe exited with code $($proc.ExitCode) (likely a startup/dependency failure)"
                $smokeResult = 'failed'
            }
        }
        else {
            # Reached the run loop without crashing -> stable enough.
            $smokeResult = 'launched'
            $null = $proc.CloseMainWindow()
            if (-not $proc.WaitForExit(5000)) { $proc.Kill($true); $smokeNotes += "Forced close after graceful-close timeout." }
        }

        # Prove the real per-user config was not modified.
        $after = Get-DirSnapshot $realConfig
        # @(...) guards against PowerShell unwrapping an empty snapshot to $null
        # (happens on a clean runner where %LOCALAPPDATA%\ExoSnap does not exist),
        # which would make Compare-Object throw "ReferenceObject is null".
        $diff = Compare-Object -ReferenceObject @($before) -DifferenceObject @($after)
        if ($diff) {
            Add-Error "Smoke: real per-user config under '$realConfig' was modified — isolation breach"
            $smokeResult = 'failed'
        }
        else {
            $smokeNotes += "Real per-user config under %LOCALAPPDATA%\ExoSnap unchanged (isolated via EXOSNAP_CONFIG_DIR)."
        }
        $isoWrote = (Test-Path -LiteralPath $isoConfig) -and ((Get-ChildItem -LiteralPath $isoConfig -Recurse -File -ErrorAction SilentlyContinue).Count -gt 0)
        $smokeNotes += "Isolated config dir received writes: $isoWrote."
    }
}
else {
    Write-Step "Skipping smoke (-SkipSmoke)."
}

# ---------------------------------------------------------------------------
# 9. Report
# ---------------------------------------------------------------------------
Add-ReportLine "# ExoSnap $Version — Release Artifact Validation Report"
Add-ReportLine ""
Add-ReportLine "- Generated (local time): $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
Add-ReportLine "- Source commit: $sourceCommit"
Add-ReportLine "- Version: $Version"
Add-ReportLine "- Build preset: $Preset (Release)"
Add-ReportLine "- Portable archive: $PortablePackageName.zip"
Add-ReportLine "- Portable SHA-256: $sha"
Add-ReportLine "- Portable checksum file: $PortablePackageName.sha256"
Add-ReportLine "- File count: $($allFiles.Count)"
Add-ReportLine "- Compressed size: $([math]::Round($zipInfo.Length / 1MB, 2)) MB ($($zipInfo.Length) bytes)"
Add-ReportLine "- Uncompressed size: $([math]::Round($uncompressed / 1MB, 2)) MB ($uncompressed bytes)"
if ($msiBuilt) {
    Add-ReportLine "- MSI package: $MsiPackageName.msi"
    Add-ReportLine "- MSI SHA-256: $msiSha"
    Add-ReportLine "- MSI checksum file: $MsiPackageName.msi.sha256"
}
Add-ReportLine ""
Add-ReportLine "## Executable metadata"
Add-ReportLine "- ProductName: $($vi.ProductName)"
Add-ReportLine "- FileDescription: $($vi.FileDescription)"
Add-ReportLine "- CompanyName: $($vi.CompanyName)"
Add-ReportLine "- ProductVersion: $($vi.ProductVersion)"
Add-ReportLine "- FileVersion: $($vi.FileVersion)"
Add-ReportLine "- InternalName: $($vi.InternalName)"
Add-ReportLine "- OriginalFilename: $($vi.OriginalFilename)"
Add-ReportLine "- LegalCopyright: $($vi.LegalCopyright)"
Add-ReportLine ""
Add-ReportLine "## Smoke test"
Add-ReportLine "- Result: $smokeResult"
foreach ($n in $smokeNotes) { Add-ReportLine "- $n" }
Add-ReportLine ""
Add-ReportLine "## Validation result"
if ($script:Errors.Count -eq 0) {
    Add-ReportLine "- PASSED — no validation errors."
}
else {
    Add-ReportLine "- FAILED — $($script:Errors.Count) error(s):"
    foreach ($e in $script:Errors) { Add-ReportLine "  - $e" }
}
$script:ReportLines -join "`n" | Set-Content -LiteralPath $ReportPath -Encoding utf8

Write-Host ""
Write-Host "Portable   : $ZipPath"
Write-Host "Portable SHA: $ShaPath"
if ($msiBuilt) {
    Write-Host "MSI        : $MsiPath"
    Write-Host "MSI SHA    : $MsiShaPath"
}
Write-Host "Manifest   : $ManifestPath"
Write-Host "Report     : $ReportPath"
Write-Host "Smoke      : $smokeResult"
Write-Host ""

if ($script:Errors.Count -gt 0) {
    Write-Host "RELEASE ARTIFACT VALIDATION FAILED ($($script:Errors.Count) error(s))." -ForegroundColor Red
    exit 1
}
Write-Host "RELEASE ARTIFACT VALIDATION PASSED." -ForegroundColor Green
exit 0
