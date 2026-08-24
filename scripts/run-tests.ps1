#!/usr/bin/env pwsh
#Requires -Version 7.0
<#
.SYNOPSIS
    The canonical way to run the ExoSnap test suite.

.DESCRIPTION
    Wraps `ctest` with the environment every ExoSnap test binary needs and a
    quiet, summarised output:

      * EXOSNAP_CONFIG_DIR  -> a fresh throwaway temp dir (never touch real
                               user config), removed again on exit.
      * QT_QPA_PLATFORM      -> offscreen (no windows pop up; headless-safe).
      * QT_PLUGIN_PATH       -> the Qt install's plugins dir.
      * PATH                 -> Qt bin prepended so Qt/FFmpeg DLLs resolve.

    The full ctest output (including every failing gtest `Suite.Case`) is written
    to <BuildDir>/Testing/last-run.log. Only a compact summary — pass/fail
    counts, wall-clock, and the failing binaries + their gtest cases — goes to
    stdout. The script's exit code is ctest's exit code.

    Each CTest entry is one test BINARY (gtest_main runs all its cases in-process
    and prints the exact failing case), so -R / -Filter matches binary names,
    e.g. "recorder_core." or "capability.".

.PARAMETER BuildDir
    CMake build tree to test. Default: build/windows-x64-debug.

.PARAMETER Config
    Multi-config configuration to run (ctest -C). Default: Debug.

.PARAMETER Filter
    Regex passed to `ctest -R` to select test binaries by name.

.PARAMETER ExcludeLabel
    Label passed to `ctest -LE` to exclude a category. Use "live" to skip the
    binaries that issue real hardware queries (DXGI adapter enumeration, GPU
    capability probes) so the suite runs cleanly with no GPU present.

.PARAMETER Jobs
    Parallel test jobs (ctest -j). Default: the processor count.

.PARAMETER Build
    Do a full build of the tree (all targets) before running tests.

.EXAMPLE
    pwsh scripts/run-tests.ps1

.EXAMPLE
    pwsh scripts/run-tests.ps1 -Filter recorder_core. -Build

.EXAMPLE
    pwsh scripts/run-tests.ps1 -ExcludeLabel live
#>
[CmdletBinding()]
param(
    [string]$BuildDir = 'build/windows-x64-debug',
    [string]$Config = 'Debug',
    [string]$Filter = '',
    [string]$ExcludeLabel = '',
    [int]$Jobs = 0,
    [switch]$Build
)

$ErrorActionPreference = 'Stop'

# Resolve paths relative to the repo root (this script lives in scripts/)
# without Set-Location, so an in-session caller keeps its working directory.
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not [System.IO.Path]::IsPathRooted($BuildDir)) {
    $BuildDir = Join-Path $repoRoot $BuildDir
}

if ($Jobs -le 0) {
    $Jobs = [Environment]::ProcessorCount
    if ($Jobs -le 0) { $Jobs = 4 }
}

# Write-Host, not Write-Error: with ErrorActionPreference=Stop a Write-Error
# throws immediately, which would skip the intended exit code (and further down
# the config-dir cleanup).
if (-not (Test-Path $BuildDir -PathType Container)) {
    Write-Host "Build dir '$BuildDir' does not exist. Configure it first (cmake --preset ...) or pass -BuildDir." -ForegroundColor Red
    exit 2
}

# --- Qt / DLL resolution -----------------------------------------------------
# Resolved from .qt-version, never spelled out here: a hard-coded path keeps
# working after a Qt uplift, against the previous Qt.
Import-Module (Join-Path $PSScriptRoot 'lib/QtEnvironment.psm1') -Force
Add-QtToPath -RepoRoot $repoRoot -IncludePlugins | Out-Null
$env:QT_QPA_PLATFORM = 'offscreen'

# --- Isolated, throwaway config dir -----------------------------------------
$configDir = Join-Path ([System.IO.Path]::GetTempPath()) ("exosnap_runtests_" + [System.Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $configDir -Force | Out-Null
$env:EXOSNAP_CONFIG_DIR = $configDir

# --- Optional full build -----------------------------------------------------
if ($Build) {
    Write-Host "Building all targets in $BuildDir ($Config)..." -ForegroundColor Cyan
    & cmake --build $BuildDir --config $Config
    if ($LASTEXITCODE -ne 0) {
        $buildExit = $LASTEXITCODE
        Write-Host "Build failed (exit $buildExit)." -ForegroundColor Red
        Remove-Item -Recurse -Force $configDir -ErrorAction SilentlyContinue
        exit $buildExit
    }
}

# --- ctest invocation --------------------------------------------------------
$logDir = Join-Path $BuildDir 'Testing'
New-Item -ItemType Directory -Path $logDir -Force | Out-Null
$logFile = Join-Path $logDir 'last-run.log'

$ctestArgs = @(
    '--test-dir', $BuildDir,
    '-C', $Config,
    '-j', "$Jobs",
    '--output-on-failure'
)
if ($Filter)       { $ctestArgs += @('-R', $Filter) }
if ($ExcludeLabel) { $ctestArgs += @('-LE', $ExcludeLabel) }

$argLine = ($ctestArgs -join ' ')
Write-Host "ctest $argLine" -ForegroundColor DarkGray
Write-Host "Full log: $logFile" -ForegroundColor DarkGray
Write-Host ''

# Stream ctest output to the log file while capturing it for summarisation.
$ctestExit = 0
& ctest @ctestArgs 2>&1 | Tee-Object -FilePath $logFile | Out-Null
$ctestExit = $LASTEXITCODE

# --- Cleanup -----------------------------------------------------------------
Remove-Item -Recurse -Force $configDir -ErrorAction SilentlyContinue

# --- Summary -----------------------------------------------------------------
$log = Get-Content -LiteralPath $logFile

$summaryLine = ($log | Select-String -Pattern 'tests passed,.*failed out of' | Select-Object -Last 1).Line
$timeLine    = ($log | Select-String -Pattern 'Total Test time' | Select-Object -Last 1).Line

Write-Host ('-' * 60)
if ($summaryLine) { Write-Host $summaryLine.Trim() -ForegroundColor ($ctestExit -eq 0 ? 'Green' : 'Red') }
if ($timeLine)    { Write-Host $timeLine.Trim() }

if ($ctestExit -ne 0) {
    # Failed binaries, as ctest lists them under "The following tests FAILED:".
    $failedBinaries = $log |
        Select-String -Pattern '^\s*\d+\s+-\s+(.+?)\s+\(.*(Failed|Timeout).*\)$' |
        ForEach-Object { $_.Matches[0].Groups[1].Value }

    if ($failedBinaries) {
        Write-Host ''
        Write-Host 'Failed test binaries:' -ForegroundColor Red
        $failedBinaries | Select-Object -Unique | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    }

    # Exact failing gtest cases (gtest_main prints "[  FAILED  ] Suite.Case").
    $failedCases = $log |
        Select-String -Pattern '\[\s*FAILED\s*\]\s+([A-Za-z0-9_./]+\.[A-Za-z0-9_/]+)' |
        ForEach-Object { $_.Matches[0].Groups[1].Value }

    if ($failedCases) {
        Write-Host ''
        Write-Host 'Failing gtest cases:' -ForegroundColor Red
        $failedCases | Select-Object -Unique | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    }
    Write-Host ''
    Write-Host "See $logFile for full output." -ForegroundColor Yellow
}
Write-Host ('-' * 60)

exit $ctestExit
