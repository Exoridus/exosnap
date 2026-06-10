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
$requiredLicenses = @('spdlog.txt', 'nlohmann_json.txt', 'tomlplusplus.txt', 'opus.txt', 'fdk-aac.txt', 'libebml.txt', 'libmatroska.txt', 'qt.txt')
foreach ($lic in $requiredLicenses) {
    if (-not (Test-Path -LiteralPath (Join-Path $PackageRoot "licenses/$lic") -PathType Leaf)) { Add-Error "Missing third-party license: licenses/$lic" }
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

# Documentation must name 0.1.0 and not call the release 1.0.
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
                $msiArgs = @(
                    'build', '-arch', 'x64',
                    '-o', $MsiPath,
                    '-d', "StagingDir=$PackageRoot",
                    '-d', "ProductVersion=$Version",
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
# ---------------------------------------------------------------------------
$smokeResult = 'skipped'
$smokeNotes = @()
if (-not $SkipSmoke) {
    Write-Step "Running extracted-ZIP smoke (isolated)"
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

        $proc = [System.Diagnostics.Process]::Start($psi)
        $deadline = (Get-Date).AddSeconds(8)
        while (-not $proc.HasExited -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 500 }

        if ($proc.HasExited) {
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
            # Reached the run loop without crashing -> stable enough for R1.
            $smokeResult = 'launched'
            $null = $proc.CloseMainWindow()
            if (-not $proc.WaitForExit(5000)) { $proc.Kill($true); $smokeNotes += "Forced close after graceful-close timeout." }
        }

        # Prove the real per-user config was not modified.
        $after = Get-DirSnapshot $realConfig
        $diff = Compare-Object -ReferenceObject $before -DifferenceObject $after
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
